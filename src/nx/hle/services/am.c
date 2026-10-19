/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Eden (src/core/hle/service/am/lifecycle_manager.cpp, applet_manager.cpp, am_types.h, am_results.h,
   service/common_state_getter.cpp, service/lock_accessor.cpp, service/application_functions.cpp,
   service/storage_accessor.cpp, frontend/applet_controller.h, frontend/applet_profile_select.h,
   frontend/applet_software_keyboard.cpp, frontend/applet_software_keyboard.h,
   frontend/applet_software_keyboard_types.h):
   SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
   SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project (GPL-2.0-or-later)
   SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project (GPL-2.0-or-later) */
/* appletOE and appletAE: the applet manager as the one application sees it. It is in focus from the start, with the
   focus message queued. Library applets answer what the game asked with the console's one user, the connected
   players, or the host's text dialog: at once, but for the inline software keyboard, which lives until the game
   finalizes it and answers its requests through the interactive channels */
#include "nx/hle/services/services_int.h"
#include "nx/hle/hle.h"
#include "nx/hle/services/hid.h"
#include "nx/hle/filesystem/fs.h"

#include "hle/services/am.swipc.inc"

#define AM_NO_DATA         RC(128, 2)   /* ResultNoDataInChannel */
#define AM_NO_MESSAGES     RC(128, 3)   /* ResultNoMessages */
#define AM_INVALID_OFFSET  RC(128, 503) /* ResultInvalidOffset */
#define AM_INVALID_STORAGE RC(128, 511) /* ResultInvalidStorageType */
#define AM_OUT_OF_MEMORY   RC(128, 600)

/* nn::am::AppletMessage */
#define AM_MSG_FOCUS_STATE_CHANGED      15
#define AM_MSG_PERFORMANCE_MODE_CHANGED 31
#define AM_MSG_STARTUP_LOGO_GONE        95 /* StartupLogoDisappeared, after EnableStartupLogoDisappearedMessage */
#define AM_FOCUS_IN                1 /* nn::am::service::FocusState::InFocus */

/* the AppletIds of the library applets that answer with data */
#define AM_APPLET_CONTROLLER     0x0c
#define AM_APPLET_PROFILE_SELECT 0x10
#define AM_APPLET_SWKBD          0x11
#define AM_APPLET_MII_EDIT       0x12
#define AM_APPLET_WEB            0x13
#define AM_APPLET_SHOP           0x14
#define AM_APPLET_OFFLINE_WEB    0x17
#define AM_APPLET_LOGIN_SHARE    0x18
#define AM_APPLET_WEB_AUTH       0x19

#define AM_PRESELECTED_USER_MAGIC 0xc79497cau /* LaunchParameterAccountPreselectedUser */

/* the console's first user (users.c). Fixed: a game may keep it in its saves, and the saves of existing installs
   belong to this one */
const u64 g_user_id[2] = {SNX_USERS_FIRST_UID0, SNX_USERS_FIRST_UID1};

/* the application's message queue, the user channel and the rest of am's state, and the pools of its objects' data:
   g_hle.am (services_int.h) */

void am_init(void) {
    pm_init(&g_hle.am.lock);
    g_hle.am.msg_event = owned_event("am:message");
    g_hle.am.sleep_lock_event = owned_event("am:sleep lock");
    g_hle.am.launchable_event = owned_event("am:library applet launchable");
    g_hle.am.resolution_event = owned_event("am:display resolution change");
    g_hle.am.next_layer = 1;
    /* the application starts in focus, and is told so, then that the performance mode is set (the SDK reads it on
       that message) */
    g_hle.am.msgs[0] = AM_MSG_FOCUS_STATE_CHANGED;
    g_hle.am.msgs[1] = AM_MSG_PERFORMANCE_MODE_CHANGED;
    g_hle.am.count = 2;
    kevent_signal(g_hle.am.msg_event);
    kevent_signal(g_hle.am.launchable_event);
}

/* ============================================================ storages */
/* IStorage's data is a StorageData, the accessors' an AmAccessor (services_int.h) */
static void am_storage_destroy(SvcObj* o) {
    StorageData* s = (StorageData*)o->data;
    if (s->tmem) kobj_unref(s->tmem);
    free(s->d);
}
static void am_accessor_destroy(SvcObj* o) { svcobj_unref(((AmAccessor*)o->data)->storage); }

static u8* am_storage_bytes(SvcObj* o) {
    StorageData* s = (StorageData*)o->data;
    return s->guest ? GP(s->guest) : s->d;
}
/* a storage of size bytes, a copy of data's or zeros; NULL if they can't be had */
static SvcObj* am_storage_new(u64 size, const void* data) {
    SvcObj* o;
    StorageData* s;
    u8* d;
    if (size > 0x10000000u) return NULL;
    /* the bytes the game sized, zeroed: at least one, so a storage over a transfer memory (size 0 here) has some */
    if (!(d = calloc(1, size ? (size_t)size : 1)))
        fatal("Out of memory for an am storage of %llu bytes", (unsigned long long)size);
    o = nn_am_service_IStorage_new_data(g_hle.am.storages, g_hle.am.storage_next, "am storages");
    s = (StorageData*)o->data;
    s->size = size;
    s->d = d;
    if (data && size) memcpy(d, data, (size_t)size);
    o->destroy = am_storage_destroy;
    return o;
}
/* a storage over a transfer memory's pages: the client's size, within the memory */
static int am_storage_tmem(i64 size, u32 handle, int handle_only, SvcObj** storage) {
    KTmem* t = (KTmem*)h_get_ref(handle, KO_TMEM);
    StorageData* s;
    R_UNLESS(t, RC(1, 114)); /* the kernel's InvalidHandle */
    if (size < 0 || (u64)size > t->size) {
        kobj_unref(&t->o);
        R_THROW(AM_INVALID_OFFSET);
    }
    *storage = am_storage_new(0, NULL);
    s = (StorageData*)(*storage)->data;
    s->size = (u64)size;
    s->guest = t->addr;
    s->tmem = &t->o;
    s->handle_only = handle_only;
    R_SUCCEED();
}

static int nn_am_service_IStorage_Open(SvcObj* self, SvcObj** accessor) {
    R_UNLESS(!((StorageData*)self->data)->handle_only, AM_INVALID_STORAGE);
    *accessor = nn_am_service_IStorageAccessor_new_data(g_hle.am.accessors, g_hle.am.accessor_next, "am storage accessors");
    svcobj_ref(self);
    ((AmAccessor*)(*accessor)->data)->storage = self;
    (*accessor)->destroy = am_accessor_destroy;
    R_SUCCEED();
}
static int nn_am_service_IStorage_OpenTransferStorage(SvcObj* self, SvcObj** accessor) {
    R_UNLESS(((StorageData*)self->data)->guest, AM_INVALID_STORAGE);
    *accessor = nn_am_service_ITransferStorageAccessor_new_data(g_hle.am.accessors, g_hle.am.accessor_next,
                                                                  "am storage accessors");
    svcobj_ref(self);
    ((AmAccessor*)(*accessor)->data)->storage = self;
    (*accessor)->destroy = am_accessor_destroy;
    R_SUCCEED();
}

static int nn_am_service_IStorageAccessor_GetSize(SvcObj* self, i64* size) {
    *size = (i64)((StorageData*)((AmAccessor*)self->data)->storage->data)->size;
    R_SUCCEED();
}
static int nn_am_service_IStorageAccessor_Write(SvcObj* self, i64 offset, IpcBuf data) {
    SvcObj* st = ((AmAccessor*)self->data)->storage;
    u64 size = ((StorageData*)st->data)->size;
    R_UNLESS(offset >= 0 && (u64)offset <= size && data.size <= size - (u64)offset, AM_INVALID_OFFSET);
    memcpy(am_storage_bytes(st) + offset, GP(data.addr), (size_t)data.size);
    R_SUCCEED();
}
/* the whole buffer, as Eden checks a read: one that runs past the end is refused, not cut */
static int nn_am_service_IStorageAccessor_Read(SvcObj* self, i64 offset, IpcBuf data) {
    SvcObj* st = ((AmAccessor*)self->data)->storage;
    u64 size = ((StorageData*)st->data)->size;
    R_UNLESS(offset >= 0 && (u64)offset <= size && data.size <= size - (u64)offset, AM_INVALID_OFFSET);
    memcpy(GP(data.addr), am_storage_bytes(st) + offset, (size_t)data.size);
    R_SUCCEED();
}

/* ============================================================ proxies */
static SvcObj* am_proxy(void) { return nn_am_service_IApplicationProxy_new(); }
static int nn_am_service_IApplicationProxyService_OpenApplicationProxy(SvcObj* self, u64 pid, u32 process,
                                                                      SvcObj** proxy) {
    (void)self, (void)pid, (void)process;
    *proxy = am_proxy();
    R_SUCCEED();
}
static int nn_am_service_IAllSystemAppletProxiesService_OpenSystemApplicationProxy(SvcObj* self, u64 pid,
                                                                                  u32 process, SvcObj** proxy) {
    (void)self, (void)pid, (void)process;
    *proxy = am_proxy();
    R_SUCCEED();
}
SvcObj* new_appletoe(void) { return nn_am_service_IApplicationProxyService_new(); }
SvcObj* new_appletae(void) { return nn_am_service_IAllSystemAppletProxiesService_new(); }

static int nn_am_service_IApplicationProxy_GetCommonStateGetter(SvcObj* self, SvcObj** getter) {
    (void)self;
    *getter = nn_am_service_ICommonStateGetter_new();
    R_SUCCEED();
}
static int nn_am_service_IApplicationProxy_GetSelfController(SvcObj* self, SvcObj** controller) {
    (void)self;
    *controller = nn_am_service_ISelfController_new();
    R_SUCCEED();
}
static int nn_am_service_IApplicationProxy_GetWindowController(SvcObj* self, SvcObj** controller) {
    (void)self;
    *controller = nn_am_service_IWindowController_new();
    R_SUCCEED();
}
/* IAudioController's volumes, kept on each controller as Eden keeps them (AmVolumes, services_int.h) */
static int nn_am_service_IApplicationProxy_GetAudioController(SvcObj* self, SvcObj** controller) {
    AmVolumes* v;
    (void)self;
    *controller = nn_am_service_IAudioController_new_data(g_hle.am.volumes, g_hle.am.volume_next, "am audio controllers");
    v = (AmVolumes*)(*controller)->data;
    v->main_volume = 1.0f;
    v->library_volume = 1.0f;
    R_SUCCEED();
}
static int nn_am_service_IApplicationProxy_GetDisplayController(SvcObj* self, SvcObj** controller) {
    (void)self;
    *controller = nn_am_service_IDisplayController_new();
    R_SUCCEED();
}
static int nn_am_service_IApplicationProxy_GetProcessWindingController(SvcObj* self, SvcObj** controller) {
    (void)self;
    *controller = nn_am_service_IProcessWindingController_new();
    R_SUCCEED();
}
static int nn_am_service_IApplicationProxy_GetLibraryAppletCreator(SvcObj* self, SvcObj** creator) {
    (void)self;
    *creator = nn_am_service_ILibraryAppletCreator_new();
    R_SUCCEED();
}
static int nn_am_service_IApplicationProxy_GetApplicationFunctions(SvcObj* self, SvcObj** functions) {
    (void)self;
    *functions = nn_am_service_IApplicationFunctions_new();
    R_SUCCEED();
}
static int nn_am_service_IApplicationProxy_GetDebugFunctions(SvcObj* self, SvcObj** functions) {
    (void)self;
    *functions = nn_am_service_IDebugFunctions_new();
    R_SUCCEED();
}

/* ============================================================ ICommonStateGetter */
static int nn_am_service_ICommonStateGetter_GetEventHandle(SvcObj* self, u32* event) {
    (void)self;
    *event = ev_handle(g_hle.am.msg_event);
    R_SUCCEED();
}
static int nn_am_service_ICommonStateGetter_ReceiveMessage(SvcObj* self, nn_am_AppletMessage* message) {
    int rc = 0;
    (void)self;
    pm_lock(&g_hle.am.lock);
    if (!g_hle.am.count)
        rc = AM_NO_MESSAGES;
    else {
        *message = g_hle.am.msgs[g_hle.am.head];
        g_hle.am.head = (g_hle.am.head + 1) % AM_MESSAGES;
        g_hle.am.count--;
    }
    if (!g_hle.am.count) kevent_clear(g_hle.am.msg_event);
    pm_unlock(&g_hle.am.lock);
    return rc;
}
static int nn_am_service_ICommonStateGetter_GetOperationMode(SvcObj* self, u8* mode) {
    (void)self;
    *mode = g_opt_docked ? 1 : 0; /* nn::oe::OperationMode: Handheld, Console */
    R_SUCCEED();
}
static int nn_am_service_ICommonStateGetter_GetPerformanceMode(SvcObj* self, u32* mode) {
    (void)self;
    *mode = g_opt_docked ? 1 : 0; /* Boost when docked, as apm says */
    R_SUCCEED();
}
static int nn_am_service_ICommonStateGetter_GetCurrentFocusState(SvcObj* self, u8* state) {
    (void)self;
    *state = AM_FOCUS_IN;
    R_SUCCEED();
}
static int nn_am_service_ICommonStateGetter_RequestToAcquireSleepLock(SvcObj* self) {
    (void)self;
    kevent_signal(g_hle.am.sleep_lock_event);
    R_SUCCEED();
}
static int nn_am_service_ICommonStateGetter_GetAcquiredSleepLockEvent(SvcObj* self, u32* event) {
    (void)self;
    *event = ev_handle(g_hle.am.sleep_lock_event);
    R_SUCCEED();
}

/* ILockAccessor: granted whenever it is free, with an event signaled when it is released (AmLock, services_int.h) */
static void am_lock_destroy(SvcObj* o) { kobj_unref(&((AmLock*)o->data)->event->o); }
static SvcObj* am_lock_new(void) {
    SvcObj* o = nn_am_service_ILockAccessor_new_data(g_hle.am.lock_objs, g_hle.am.lock_obj_next, "am lock accessors");
    ((AmLock*)o->data)->event = owned_event("am:lock accessor");
    /* the lock is free: libnx's TryLock and Lock wait on the event before they ask for it */
    kevent_signal(((AmLock*)o->data)->event);
    o->destroy = am_lock_destroy;
    return o;
}
static int nn_am_service_ICommonStateGetter_GetHomeButtonReaderLockAccessor(SvcObj* self, SvcObj** accessor) {
    (void)self;
    *accessor = am_lock_new();
    R_SUCCEED();
}
static int nn_am_service_ICommonStateGetter_GetReaderLockAccessorEx(SvcObj* self, u32 button_type,
                                                                   SvcObj** accessor) {
    (void)self, (void)button_type;
    *accessor = am_lock_new();
    R_SUCCEED();
}
static int nn_am_service_ICommonStateGetter_GetWriterLockAccessorEx(SvcObj* self, u32 button_type,
                                                                   SvcObj** accessor) {
    (void)self, (void)button_type;
    *accessor = am_lock_new();
    R_SUCCEED();
}
static int nn_am_service_ILockAccessor_TryLock(SvcObj* self, u8 return_handle, u8* is_locked, u32* event) {
    AmLock* l = (AmLock*)self->data;
    pm_lock(&g_hle.am.lock);
    *is_locked = (u8)!l->locked;
    l->locked = 1;
    pm_unlock(&g_hle.am.lock);
    if (return_handle) *event = ev_handle(l->event);
    R_SUCCEED();
}
static int nn_am_service_ILockAccessor_Unlock(SvcObj* self) {
    AmLock* l = (AmLock*)self->data;
    pm_lock(&g_hle.am.lock);
    l->locked = 0;
    pm_unlock(&g_hle.am.lock);
    kevent_signal(l->event);
    R_SUCCEED();
}
static int nn_am_service_ILockAccessor_GetEvent(SvcObj* self, u32* event) {
    *event = ev_handle(((AmLock*)self->data)->event);
    R_SUCCEED();
}
static int nn_am_service_ILockAccessor_IsLocked(SvcObj* self, u8* is_locked) {
    AmLock* l = (AmLock*)self->data;
    pm_lock(&g_hle.am.lock);
    *is_locked = (u8)l->locked;
    pm_unlock(&g_hle.am.lock);
    R_SUCCEED();
}

static int am_set_vr_mode(u8 on) {
    pm_lock(&g_hle.am.lock);
    g_hle.am.vr_mode = on;
    pm_unlock(&g_hle.am.lock);
    R_SUCCEED();
}
static int nn_am_service_ICommonStateGetter_IsVrModeEnabled(SvcObj* self, u8* enabled) {
    (void)self;
    pm_lock(&g_hle.am.lock);
    *enabled = g_hle.am.vr_mode;
    pm_unlock(&g_hle.am.lock);
    R_SUCCEED();
}
static int nn_am_service_ICommonStateGetter_SetVrModeEnabled(SvcObj* self, u8 enabled) {
    (void)self;
    return am_set_vr_mode(enabled != 0);
}
static int nn_am_service_ICommonStateGetter_BeginVrModeEx(SvcObj* self) {
    (void)self;
    return am_set_vr_mode(1);
}
static int nn_am_service_ICommonStateGetter_EndVrModeEx(SvcObj* self) {
    (void)self;
    return am_set_vr_mode(0);
}

static int nn_am_service_ICommonStateGetter_GetDefaultDisplayResolution(SvcObj* self, i32* width, i32* height) {
    (void)self;
    *width = g_opt_docked ? 1920 : 1280;
    *height = g_opt_docked ? 1080 : 720;
    R_SUCCEED();
}
static int nn_am_service_ICommonStateGetter_GetDefaultDisplayResolutionChangeEvent(SvcObj* self, u32* event) {
    (void)self;
    *event = ev_handle(g_hle.am.resolution_event);
    R_SUCCEED();
}
static int nn_am_service_ICommonStateGetter_GetHdcpAuthenticationStateChangeEvent(SvcObj* self, u32* event) {
    (void)self;
    *event = ev_handle(g_hle.am.resolution_event);
    R_SUCCEED();
}
/* apm's configuration of the mode the console runs in */
static int nn_am_service_ICommonStateGetter_GetCurrentPerformanceConfiguration(SvcObj* self, u32* configuration) {
    (void)self;
    *configuration = apm_configuration(g_opt_docked ? 1 : 0);
    R_SUCCEED();
}
/* the clocks a game asks for around its loading screens, which apm keeps as Boost mode's configuration */
static int nn_am_service_ICommonStateGetter_SetCpuBoostMode(SvcObj* self, u32 mode) {
    (void)self;
    apm_cpu_boost(mode);
    R_SUCCEED();
}
/* there is no start-up logo here, so it is gone as soon as a game asks to be told */
static int nn_am_service_ICommonStateGetter_EnableStartupLogoDisappearedMessage(SvcObj* self) {
    (void)self;
    pm_lock(&g_hle.am.lock);
    if (g_hle.am.count < AM_MESSAGES) g_hle.am.msgs[(g_hle.am.head + g_hle.am.count++) % AM_MESSAGES] = AM_MSG_STARTUP_LOGO_GONE;
    kevent_signal(g_hle.am.msg_event);
    pm_unlock(&g_hle.am.lock);
    R_SUCCEED();
}
static int nn_am_service_ICommonStateGetter_GetSettingsPlatformRegion(SvcObj* self, u32* region) {
    (void)self;
    *region = 1; /* Global */
    R_SUCCEED();
}
static void am_set_vr_mode_3d(u8 on) {
    pm_lock(&g_hle.am.lock);
    g_hle.am.vr_mode_3d = on;
    pm_unlock(&g_hle.am.lock);
}
static int nn_am_service_ICommonStateGetter_BeginVrMode3d(SvcObj* self) {
    (void)self;
    am_set_vr_mode_3d(1);
    R_SUCCEED();
}
static int nn_am_service_ICommonStateGetter_EndVrMode3d(SvcObj* self) {
    (void)self;
    am_set_vr_mode_3d(0);
    R_SUCCEED();
}
static int nn_am_service_ICommonStateGetter_IsVrModeEnabled3d(SvcObj* self, u8* enabled) {
    (void)self;
    pm_lock(&g_hle.am.lock);
    *enabled = g_hle.am.vr_mode_3d;
    pm_unlock(&g_hle.am.lock);
    R_SUCCEED();
}

/* ============================================================ ISelfController */
static int nn_am_service_ISelfController_Exit(SvcObj* self) {
    (void)self;
    /* the frontend's loop ends the run, saving what outlives it, as when the window closes */
    LOG("am: the application exited\n");
    g_quit = 1;
    R_SUCCEED();
}
static int nn_am_service_ISelfController_GetLibraryAppletLaunchableEvent(SvcObj* self, u32* event) {
    (void)self;
    *event = ev_handle(g_hle.am.launchable_event);
    R_SUCCEED();
}
static int nn_am_service_ISelfController_CreateManagedDisplayLayer(SvcObj* self, u64* layer_id) {
    (void)self;
    pm_lock(&g_hle.am.lock);
    *layer_id = g_hle.am.next_layer++;
    pm_unlock(&g_hle.am.lock);
    R_SUCCEED();
}
static int nn_am_service_ISelfController_CreateManagedDisplaySeparableLayer(SvcObj* self, u64* layer_id,
                                                                           u64* recording_layer_id) {
    (void)self;
    pm_lock(&g_hle.am.lock);
    *layer_id = g_hle.am.next_layer++;
    *recording_layer_id = g_hle.am.next_layer++;
    pm_unlock(&g_hle.am.lock);
    R_SUCCEED();
}
static int nn_am_service_ISelfController_GetAccumulatedSuspendedTickChangedEvent(SvcObj* self, u32* event) {
    (void)self;
    return new_event(event, "am:suspended tick changed", 1);
}
static int nn_am_service_ISelfController_SetIdleTimeDetectionExtension(SvcObj* self, u32 extension) {
    (void)self;
    pm_lock(&g_hle.am.lock);
    g_hle.am.idle_extension = extension;
    pm_unlock(&g_hle.am.lock);
    R_SUCCEED();
}
static int nn_am_service_ISelfController_GetIdleTimeDetectionExtension(SvcObj* self, u32* extension) {
    (void)self;
    pm_lock(&g_hle.am.lock);
    *extension = g_hle.am.idle_extension;
    pm_unlock(&g_hle.am.lock);
    R_SUCCEED();
}
static int nn_am_service_ISelfController_SetAutoSleepDisabled(SvcObj* self, u8 disabled) {
    (void)self;
    pm_lock(&g_hle.am.lock);
    g_hle.am.auto_sleep_disabled = disabled;
    pm_unlock(&g_hle.am.lock);
    R_SUCCEED();
}
static int nn_am_service_ISelfController_IsAutoSleepDisabled(SvcObj* self, u8* disabled) {
    (void)self;
    pm_lock(&g_hle.am.lock);
    *disabled = g_hle.am.auto_sleep_disabled;
    pm_unlock(&g_hle.am.lock);
    R_SUCCEED();
}

static int nn_am_service_IWindowController_GetAppletResourceUserId(SvcObj* self,
                                                                   nn_applet_AppletResourceUserId* aruid) {
    (void)self;
    *aruid = K_PROCESS_ID;
    R_SUCCEED();
}

static float am_volume(float v) { return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v; }
static int nn_am_service_IAudioController_SetExpectedMasterVolume(SvcObj* self, float main_volume,
                                                                  float library_volume) {
    AmVolumes* v = (AmVolumes*)self->data;
    pm_lock(&g_hle.am.lock);
    v->main_volume = am_volume(main_volume);
    v->library_volume = am_volume(library_volume);
    pm_unlock(&g_hle.am.lock);
    R_SUCCEED();
}
static int nn_am_service_IAudioController_GetMainAppletExpectedMasterVolume(SvcObj* self, float* volume) {
    pm_lock(&g_hle.am.lock);
    *volume = ((AmVolumes*)self->data)->main_volume;
    pm_unlock(&g_hle.am.lock);
    R_SUCCEED();
}
static int nn_am_service_IAudioController_GetLibraryAppletExpectedMasterVolume(SvcObj* self, float* volume) {
    pm_lock(&g_hle.am.lock);
    *volume = ((AmVolumes*)self->data)->library_volume;
    pm_unlock(&g_hle.am.lock);
    R_SUCCEED();
}
/* the fade is not played: nothing here plays the game at its expected volume */
static int nn_am_service_IAudioController_ChangeMainAppletMasterVolume(SvcObj* self, float volume, i64 fade_time_ns) {
    (void)fade_time_ns;
    pm_lock(&g_hle.am.lock);
    ((AmVolumes*)self->data)->main_volume = am_volume(volume);
    pm_unlock(&g_hle.am.lock);
    R_SUCCEED();
}

/* ============================================================ IApplicationFunctions */
static int nn_am_service_IApplicationFunctions_PopLaunchParameter(SvcObj* self, u32 kind, SvcObj** storage) {
    u8* d;
    u32 v;
    (void)self;
    if (kind == 1) { /* UserChannel: the last storage unpopped, which the channel's reference goes with */
        pm_lock(&g_hle.am.lock);
        *storage = g_hle.am.user_count ? g_hle.am.user_channel[--g_hle.am.user_count] : NULL;
        pm_unlock(&g_hle.am.lock);
        R_UNLESS(*storage, AM_NO_DATA);
        R_SUCCEED();
    }
    R_UNLESS(kind == 2, AM_NO_DATA); /* PreselectedUser (2) is the only other kind with data */
    pm_lock(&g_hle.am.lock);
    v = (u32)g_hle.am.preselected_popped;
    g_hle.am.preselected_popped = 1;
    pm_unlock(&g_hle.am.lock);
    R_UNLESS(!v, AM_NO_DATA);
    /* AccountPreselectedUser: the user the game plays as, selected */
    *storage = am_storage_new(0x88, NULL);
    d = am_storage_bytes(*storage);
    v = AM_PRESELECTED_USER_MAGIC;
    memcpy(d, &v, 4);
    v = 1;
    memcpy(d + 4, &v, 4);
    memcpy(d + 8, snx_users_selected()->uid, 16);
    R_SUCCEED();
}

static int nn_am_service_IApplicationFunctions_ClearUserChannel(SvcObj* self) {
    SvcObj* drop[AM_USER_CHANNEL];
    u32 i, n;
    (void)self;
    pm_lock(&g_hle.am.lock);
    n = g_hle.am.user_count;
    memcpy(drop, g_hle.am.user_channel, n * sizeof drop[0]);
    g_hle.am.user_count = 0;
    pm_unlock(&g_hle.am.lock);
    for (i = 0; i < n; i++) svcobj_unref(drop[i]);
    R_SUCCEED();
}
/* a copy, as Eden keeps one: the game may write its storage again after */
static int nn_am_service_IApplicationFunctions_UnpopToUserChannel(SvcObj* self, SvcObj* storage) {
    SvcObj* copy;
    (void)self;
    if (!storage) R_SUCCEED();
    copy = am_storage_new(((StorageData*)storage->data)->size, am_storage_bytes(storage));
    if (!copy) R_THROW(AM_OUT_OF_MEMORY);
    pm_lock(&g_hle.am.lock);
    if (g_hle.am.user_count < AM_USER_CHANNEL) {
        g_hle.am.user_channel[g_hle.am.user_count++] = copy;
        copy = NULL;
    }
    pm_unlock(&g_hle.am.lock);
    if (!copy) R_SUCCEED();
    svcobj_unref(copy);
    WARN_LIMIT(4, "am: the user channel holds %d storages, no more\n", AM_USER_CHANNEL);
    R_THROW(AM_OUT_OF_MEMORY);
}

/* -1: the game was launched by the system, not by another of its programs (ExecuteProgram is not followed here) */
static int nn_am_service_IApplicationFunctions_GetPreviousProgramIndex(SvcObj* self, i32* index) {
    (void)self;
    *index = -1;
    R_SUCCEED();
}

/* the game's control.nacp (nn::ns::ApplicationControlProperty), as the recompiler staged it beside the game's data:
   n bytes at off, NULL past its end or without one */
static const u8* am_nacp(u64 off, u64 n) {
    pm_lock(&g_hle.am.lock);
    if (!g_hle.am.nacp_read) {
        g_hle.am.nacp = fs_read_file("control.nacp", &g_hle.am.nacp_size);
        g_hle.am.nacp_read = 1;
    }
    pm_unlock(&g_hle.am.lock);
    return g_hle.am.nacp && off + n <= g_hle.am.nacp_size ? g_hle.am.nacp + off : NULL;
}
static u64 am_nacp_u64(u64 off) {
    const u8* p = am_nacp(off, 8);
    u64 v = 0;
    if (p) memcpy(&v, p, 8);
    return v;
}
#define NACP_SUPPORTED_LANGUAGES 0x302c
#define NACP_DISPLAY_VERSION     0x3060
#define NACP_ACCOUNT_SAVE_SIZE   0x3080 /* and its journal's, then the device save's and its journal's */

/* nn::ns::ApplicationLanguage of each nn::settings::Language, and back (-1: no settings language has it) */
static const signed char am_app_language[18] = {2, 0, 3, 4, 7, 6, 14, 12, 8, 10, 11, 13, 1, 9, 5, 14, 13, 15};
static const signed char am_settings_language[18] = {1, 12, 0, 2, 3, 14, 5, 4, 8, 13, 9, 10, 7, 16, 15, 17, -1, -1};
/* each ApplicationLanguage's fallbacks, in the order ns tries them */
static const signed char am_priority[16][18] = {
    {0, 1, 5, 9, 3, 4, 6, 7, 8, 10, 11, 2, 14, 13, 12, 16, 17, -1},
    {1, 0, 3, 4, 6, 7, 8, 10, 11, 5, 9, 2, 14, 13, 12, 16, 17, -1},
    {2, 0, 1, 5, 9, 3, 4, 6, 7, 8, 10, 11, 14, 13, 12, 16, 17, -1},
    {3, 9, 1, 0, 4, 6, 7, 8, 10, 11, 5, 2, 14, 13, 12, 16, 17, -1},
    {4, 1, 0, 3, 6, 7, 8, 10, 11, 5, 9, 2, 14, 13, 12, 16, 17, -1},
    {5, 6, 0, 1, 10, 9, 3, 7, 4, 8, 11, 2, 14, 13, 12, 16, 17, -1},
    {6, 5, 1, 0, 3, 4, 7, 8, 10, 11, 9, 2, 14, 13, 12, 16, 17, -1},
    {7, 1, 0, 3, 4, 6, 8, 10, 11, 5, 9, 2, 14, 13, 12, 16, 17, -1},
    {8, 1, 0, 3, 4, 6, 7, 10, 11, 5, 9, 2, 14, 13, 12, 16, 17, -1},
    {9, 3, 0, 1, 5, 6, 4, 7, 8, 10, 11, 2, 14, 13, 12, 16, 17, -1},
    {10, 1, 0, 3, 4, 6, 7, 8, 11, 5, 9, 2, 14, 13, 12, 16, 17, -1},
    {11, 1, 0, 3, 4, 6, 7, 8, 10, 5, 9, 2, 14, 13, 12, 16, 17, -1},
    {12, 0, 1, 5, 9, 3, 4, 6, 7, 8, 10, 11, 2, 14, 13, 16, 17, -1},
    {13, 14, 0, 1, 2, 5, 9, 3, 4, 6, 7, 8, 10, 11, 12, 16, 17, -1},
    {14, 13, 0, 1, 2, 5, 9, 3, 4, 6, 7, 8, 10, 11, 12, 16, 17, -1},
    {15, 10, 5, 0, 1, 2, 3, 4, 6, 7, 8, 9, 11, 12, 14, 13, 16, 17}};
/* the settings language a game with these SupportedLanguageFlag bits is shown in: the system's when the game has it,
   else the first of its fallbacks the game has (all of them when the flags are 0), as ns picks it */
static int am_desired_language(int lang, u32 supported) {
    const signed char* list;
    int i, app;
    if (lang < 0 || lang >= 18) lang = 1;
    list = am_priority[(int)am_app_language[lang]];
    for (i = 0; i < 18 && list[i] >= 0; i++) {
        app = list[i];
        if (am_settings_language[app] < 0) continue; /* Polish and Thai: no language code here */
        if (!supported || ((supported >> app) & 1)) return i ? am_settings_language[app] : lang;
    }
    return lang;
}
static int nn_am_service_IApplicationFunctions_GetDesiredLanguage(SvcObj* self, nn_settings_LanguageCode* code) {
    const u8* p = am_nacp(NACP_SUPPORTED_LANGUAGES, 4);
    u32 supported = 0;
    (void)self;
    if (p) memcpy(&supported, p, 4);
    set_language_code(am_desired_language(g_opt_lang, supported), code->v);
    R_SUCCEED();
}
/* the NACP's display version, "1.0.0" for a game without one */
static int nn_am_service_IApplicationFunctions_GetDisplayVersion(SvcObj* self, nn_oe_DisplayVersion* version) {
    const u8* p = am_nacp(NACP_DISPLAY_VERSION, 16);
    (void)self;
    if (p)
        memcpy(version->v, p, 16);
    else
        memcpy(version->v, "1.0.0", 6);
    version->v[15] = 0;
    R_SUCCEED();
}
/* the account's (type 1) and the device's (3) save data: the size ExtendSaveData gave it, else the NACP's, 64 MiB each
   without a NACP */
#define AM_DEFAULT_SAVE_SIZE 0x4000000
static int nn_am_service_IApplicationFunctions_GetSaveDataSize(SvcObj* self, u8 type, const nn_account_Uid* uid,
                                                              i64* normal_size, i64* journal_size) {
    int device = type == 3, extended = 0;
    (void)self, (void)uid;
    if (type == 1 || type == 3) {
        pm_lock(&g_hle.am.lock);
        extended = g_hle.am.save_extended[device];
        if (extended) {
            *normal_size = g_hle.am.save_size[device][0];
            *journal_size = g_hle.am.save_size[device][1];
        }
        pm_unlock(&g_hle.am.lock);
    }
    if (extended) R_SUCCEED();
    if (!am_nacp(0, 0)) {
        *normal_size = AM_DEFAULT_SAVE_SIZE;
        *journal_size = AM_DEFAULT_SAVE_SIZE;
    } else if (type == 1 || type == 3) {
        *normal_size = (i64)am_nacp_u64(NACP_ACCOUNT_SAVE_SIZE + (device ? 16 : 0));
        *journal_size = (i64)am_nacp_u64(NACP_ACCOUNT_SAVE_SIZE + 8 + (device ? 16 : 0));
    }
    R_SUCCEED();
}
static int nn_am_service_IApplicationFunctions_ExtendSaveData(SvcObj* self, u8 type, const nn_account_Uid* uid,
                                                              i64 normal_size, i64 journal_size, i64* required_size) {
    (void)self, (void)uid;
    if (type == 1 || type == 3) {
        pm_lock(&g_hle.am.lock);
        g_hle.am.save_size[type == 3][0] = normal_size;
        g_hle.am.save_size[type == 3][1] = journal_size;
        g_hle.am.save_extended[type == 3] = 1;
        pm_unlock(&g_hle.am.lock);
    }
    *required_size = 0; /* the space still missing, when there isn't enough: there always is */
    R_SUCCEED();
}
static int nn_am_service_IApplicationFunctions_GetLimitedApplicationLicenseUpgradableEvent(SvcObj* self,
                                                                                          u32* event) {
    (void)self;
    return new_event(event, "am:license upgradable", 0);
}
/* the same for every run of the game on this machine */
static int nn_am_service_IApplicationFunctions_GetPseudoDeviceId(SvcObj* self, nn_util_Uuid* id) {
    (void)self;
    memcpy(id->v, &g_hle.kernel.title_id, 8);
    memcpy(id->v + 8, g_user_id, 8);
    R_SUCCEED();
}
static int nn_am_service_IApplicationFunctions_GetGpuErrorDetectedSystemEvent(SvcObj* self, u32* event) {
    (void)self;
    return new_event(event, "am:gpu error", 0);
}
static int nn_am_service_IApplicationFunctions_GetFriendInvitationStorageChannelEvent(SvcObj* self, u32* event) {
    (void)self;
    return new_event(event, "am:friend invitation", 0);
}
static int nn_am_service_IApplicationFunctions_TryPopFromFriendInvitationStorageChannel(SvcObj* self,
                                                                                       SvcObj** storage) {
    (void)self, (void)storage;
    R_THROW(AM_NO_DATA);
}
static int nn_am_service_IApplicationFunctions_GetNotificationStorageChannelEvent(SvcObj* self, u32* event) {
    (void)self;
    return new_event(event, "am:notification", 0);
}
static int nn_am_service_IApplicationFunctions_TryPopFromNotificationStorageChannel(SvcObj* self, SvcObj** storage) {
    (void)self, (void)storage;
    R_THROW(AM_NO_DATA);
}
static int nn_am_service_IApplicationFunctions_GetHealthWarningDisappearedSystemEvent(SvcObj* self, u32* event) {
    (void)self;
    return new_event(event, "am:health warning", 0);
}
static int nn_am_service_IApplicationFunctions_GetUnknownEvent210(SvcObj* self, u32* event) {
    (void)self;
    return new_event(event, "am:unknown 210", 0);
}

/* ============================================================ library applets */
/* AM_IN_DATA and AM_OUT_DATA, the storages an applet holds, and the LibApplet: services_int.h */

/* the inline software keyboard (SwkbdState, SwkbdRequestCommand, SwkbdReplyType) */
#define SWKBD_NOT_INITIALIZED 0
#define SWKBD_HIDDEN          1
#define SWKBD_SHOWN           3
#define SWKBD_REQ_FINALIZE    4
#define SWKBD_REQ_CALC        0xa
#define SWKBD_REQ_UNSET_DICTS 0xc
#define SWKBD_REQ_USER_WORDS  6
#define SWKBD_FINISHED_INIT   0
#define SWKBD_DEFAULT         1
#define SWKBD_CHANGED         2
#define SWKBD_ENTER           5
#define SWKBD_CANCEL          6
#define SWKBD_CHANGED_UTF8    7
#define SWKBD_ENTER_UTF8      9
#define SWKBD_RELEASED_WORDS  0xb
#define SWKBD_UNSET_DICTS     0xc
/* SwkbdCalcArgFlags */
#define SWKBD_SET_INIT   (1u << 0)
#define SWKBD_APPEAR     (1u << 2)
#define SWKBD_SET_TEXT   (1u << 3)
#define SWKBD_SET_CURSOR (1u << 4)
#define SWKBD_SET_UTF8   (1u << 5)
#define SWKBD_DISAPPEAR  (1u << 7)
/* SWKBD_TEXT_CHARS, the calc argument's input text (UTF-16): services_int.h */
#define SWKBD_UTF16_SIZE 0x3ec /* a reply's text, in either encoding */
#define SWKBD_UTF8_SIZE  0x7d4
/* where the two SwkbdCalcArg layouts (the request's whole size tells them apart) keep what is read here, from the
   SwkbdCalcArgCommon at the start: SwkbdCalcArgOld puts the appear argument after the volume and cursor, New first */
#define SWKBD_CALC_OLD_SIZE 0x4a0
#define SWKBD_CALC_NEW_SIZE 0x4e8


static int am_is_inline(const LibApplet* a) {
    return a->applet_id == AM_APPLET_SWKBD && (a->mode == 1 || a->mode == 3); /* PartialForeground(IndirectDisplay) */
}
static void am_applet_destroy(SvcObj* o) {
    LibApplet* a = (LibApplet*)o->data;
    int i;
    for (i = 0; i < a->nin; i++) svcobj_unref(a->in[i]);
    for (i = 0; i < a->nout; i++) svcobj_unref(a->out[i]);
    kobj_unref(&a->state_ev->o);
    kobj_unref(&a->out_ev->o);
    kobj_unref(&a->interactive_ev->o);
}
static void am_applet_finish(LibApplet* a) {
    a->completed = 1;
    kevent_signal(a->state_ev);
}
/* the applet has finished by the time the game looks, its state changed and its output there; an inline keyboard
   runs until it is finalized */
static SvcObj* am_applet_new(u32 applet_id, u32 mode) {
    SvcObj* o = nn_am_service_ILibraryAppletAccessor_new_data(g_hle.am.applets, g_hle.am.applet_next, "library applets");
    LibApplet* a = (LibApplet*)o->data;
    a->applet_id = applet_id;
    a->mode = mode;
    a->state_ev = owned_event("am:applet state changed");
    a->out_ev = owned_event("am:applet out data");
    a->interactive_ev = owned_event("am:applet interactive out data");
    a->kb_max = SWKBD_TEXT_CHARS - 1;
    o->destroy = am_applet_destroy;
    if (!am_is_inline(a)) {
        am_applet_finish(a);
        kevent_signal(a->out_ev);
        if (applet_id != AM_APPLET_SWKBD)
            LOG_LIMIT(4, "am: library applet %#x (mode %u) closed right away\n", applet_id, mode);
    } else
        LOG("am: library applet %#x (mode %u)\n", applet_id, mode);
    return o;
}
static int nn_am_service_ILibraryAppletCreator_CreateLibraryApplet(SvcObj* self, u32 applet_id,
                                                                   u32 library_applet_mode, SvcObj** accessor) {
    (void)self;
    *accessor = am_applet_new(applet_id, library_applet_mode);
    R_SUCCEED();
}
static int nn_am_service_ILibraryAppletCreator_CreateLibraryAppletEx(SvcObj* self, u32 applet_id,
                                                                     u32 library_applet_mode, u64 thread_id,
                                                                     SvcObj** accessor) {
    (void)self, (void)thread_id;
    *accessor = am_applet_new(applet_id, library_applet_mode);
    R_SUCCEED();
}
static int nn_am_service_ILibraryAppletCreator_CreateStorage(SvcObj* self, i64 size, SvcObj** storage) {
    (void)self;
    R_UNLESS(size >= 0, AM_INVALID_OFFSET);
    *storage = am_storage_new((u64)size, NULL);
    return *storage ? 0 : AM_OUT_OF_MEMORY;
}
static int nn_am_service_ILibraryAppletCreator_CreateTransferMemoryStorage(SvcObj* self, u8 is_writable, i64 size,
                                                                           u32 transfer_memory, SvcObj** storage) {
    (void)self, (void)is_writable;
    return am_storage_tmem(size, transfer_memory, 0, storage);
}
static int nn_am_service_ILibraryAppletCreator_CreateHandleStorage(SvcObj* self, i64 size, u32 transfer_memory,
                                                                   SvcObj** storage) {
    (void)self;
    return am_storage_tmem(size, transfer_memory, 1, storage);
}

static int nn_am_service_ILibraryAppletAccessor_GetAppletStateChangedEvent(SvcObj* self, u32* event) {
    *event = ev_handle(((LibApplet*)self->data)->state_ev);
    R_SUCCEED();
}
static int nn_am_service_ILibraryAppletAccessor_IsCompleted(SvcObj* self, u8* completed) {
    LibApplet* a = (LibApplet*)self->data;
    pm_lock(&g_hle.am.lock);
    *completed = (u8)a->completed;
    pm_unlock(&g_hle.am.lock);
    R_SUCCEED();
}
static int nn_am_service_ILibraryAppletAccessor_RequestExit(SvcObj* self) {
    LibApplet* a = (LibApplet*)self->data;
    pm_lock(&g_hle.am.lock);
    am_applet_finish(a);
    pm_unlock(&g_hle.am.lock);
    R_SUCCEED();
}
static int nn_am_service_ILibraryAppletAccessor_Terminate(SvcObj* self) {
    return nn_am_service_ILibraryAppletAccessor_RequestExit(self);
}
static int nn_am_service_ILibraryAppletAccessor_PushInData(SvcObj* self, SvcObj* storage) {
    LibApplet* a = (LibApplet*)self->data;
    if (!storage) R_SUCCEED();
    pm_lock(&g_hle.am.lock);
    if (a->nin < AM_IN_DATA) {
        svcobj_ref(storage);
        a->in[a->nin++] = storage;
    }
    pm_unlock(&g_hle.am.lock);
    R_SUCCEED();
}

/* UTF-16 (n units at in, stopping at a 0) to UTF-8, at most cap - 1 bytes and a terminator */
static void am_utf16_to_8(const u16* in, u64 n, char* out, size_t cap) {
    size_t k = 0;
    u64 i;
    for (i = 0; i < n && in[i]; i++) {
        u32 c = in[i];
        if (c >= 0xd800 && c < 0xdc00 && i + 1 < n && in[i + 1] >= 0xdc00 && in[i + 1] < 0xe000) {
            c = 0x10000 + ((c - 0xd800) << 10) + (in[i + 1] - 0xdc00u);
            i++;
        }
        if (c < 0x80) {
            if (k + 1 >= cap) break;
            out[k++] = (char)c;
        } else if (c < 0x800) {
            if (k + 2 >= cap) break;
            out[k++] = (char)(0xc0 | c >> 6);
            out[k++] = (char)(0x80 | (c & 0x3f));
        } else if (c < 0x10000) {
            if (k + 3 >= cap) break;
            out[k++] = (char)(0xe0 | c >> 12);
            out[k++] = (char)(0x80 | (c >> 6 & 0x3f));
            out[k++] = (char)(0x80 | (c & 0x3f));
        } else {
            if (k + 4 >= cap) break;
            out[k++] = (char)(0xf0 | c >> 18);
            out[k++] = (char)(0x80 | (c >> 12 & 0x3f));
            out[k++] = (char)(0x80 | (c >> 6 & 0x3f));
            out[k++] = (char)(0x80 | (c & 0x3f));
        }
    }
    out[k] = 0;
}
/* UTF-8 to UTF-16, at most max units (a surrogate pair is never split); returns the units written. A byte that
   can't start a character, a sequence cut short, an overlong form, a surrogate or a value past U+10FFFF each become
   one U+FFFD, as the Unicode standard's maximal subparts */
static u32 am_utf8_to_16(const char* in, u16* out, u32 max) {
    const unsigned char* s = (const unsigned char*)in;
    u32 n = 0, c, need, i;
    while (*s) {
        unsigned char b = *s++, lo = 0x80, hi = 0xbf; /* the range of the second byte */
        if (b < 0x80) {
            c = b;
            need = 0;
        } else if (b >= 0xc2 && b <= 0xdf) {
            c = b & 0x1fu;
            need = 1;
        } else if (b >= 0xe0 && b <= 0xef) {
            c = b & 0x0fu;
            need = 2;
            if (b == 0xe0) lo = 0xa0; /* overlong */
            if (b == 0xed) hi = 0x9f; /* surrogates */
        } else if (b >= 0xf0 && b <= 0xf4) {
            c = b & 0x07u;
            need = 3;
            if (b == 0xf0) lo = 0x90; /* overlong */
            if (b == 0xf4) hi = 0x8f; /* past U+10FFFF */
        } else {
            c = 0xfffd;
            need = 0;
        }
        for (i = 0; i < need; i++, lo = 0x80, hi = 0xbf) {
            if (*s < lo || *s > hi) { /* the NUL at the end too: it is never consumed */
                c = 0xfffd;
                break;
            }
            c = c << 6 | (*s++ & 0x3fu);
        }
        if (c >= 0x10000) {
            if (n + 2 > max) break;
            c -= 0x10000;
            out[n++] = (u16)(0xd800 | c >> 10);
            out[n++] = (u16)(0xdc00 | (c & 0x3ff));
        } else {
            if (n + 1 > max) break;
            out[n++] = (u16)c;
        }
    }
    return n;
}
static void am_put32(u8* p, u32 v) { memcpy(p, &v, 4); }

/* the software keyboard as a whole applet: the host's text dialog with the game's initial text, or the text of the
   environment's SNX_SWKBD_TEXT (for scripted runs). KeyboardConfig (SwkbdConfigCommon, the second storage) gives the
   length limits, whether the answer is UTF-8, and where the initial text is in the work buffer (the third). The
   dialog is asked again until the text is long enough, or cancelled */
#define AM_SWKBD_OUT   0x7d8                       /* SwkbdResult and the 0x7d4 byte string buffer */
#define AM_SWKBD_UNITS ((AM_SWKBD_OUT - 4) / 2 - 1) /* the UTF-16 text that fits with its terminator */
static SvcObj* am_swkbd(LibApplet* a) {
    char text[SWKBD_UTF8_SIZE], initial[SWKBD_UTF8_SIZE];
    u16 wide[AM_SWKBD_UNITS + 1];
    u32 max = AM_SWKBD_UNITS, min = 0, off = 0, len = 0, n = 0, v;
    int utf8 = 0, r;
    const char* env = getenv("SNX_SWKBD_TEXT");
    SvcObj* out;
    u8* d;
    initial[0] = 0;
    if (a->nin > 1) {
        u64 size = ((StorageData*)a->in[1]->data)->size;
        const u8* cfg = am_storage_bytes(a->in[1]);
        if (size >= 0x3b4) {
            memcpy(&v, cfg + 0x3ac, 4); /* max_text_length: 0 for no limit */
            if (v && v < max) max = v;
            memcpy(&min, cfg + 0x3b0, 4); /* min_text_length */
        }
        if (size >= 0x3d4) {
            utf8 = cfg[0x3bd];
            memcpy(&off, cfg + 0x3c0, 4); /* initial_string_offset and _length, in the work buffer */
            memcpy(&len, cfg + 0x3c4, 4);
        }
    }
    if (min > max) {
        WARN("am: the software keyboard asks for %u to %u characters, %u it is\n", min, max, max);
        min = max;
    }
    if (len && a->nin > 2) {
        u64 size = ((StorageData*)a->in[2]->data)->size;
        if (off < size && len <= (size - off) / 2 && len <= AM_SWKBD_UNITS) {
            memcpy(wide, am_storage_bytes(a->in[2]) + off, (size_t)len * 2);
            am_utf16_to_8(wide, len, initial, sizeof initial);
        }
    }
    if (env) {
        n = am_utf8_to_16(env, wide, max);
        r = n < min; /* too short: cancelled, as a player can't send it */
    } else
        for (;;) {
            r = plat_text_input(text, sizeof text, initial);
            if (r < 0) {
                WARN_LIMIT(1, "am: no text dialog on this host (zenity or kdialog), the software keyboard is "
                              "cancelled\n");
                r = 1;
            }
            if (r) break;
            n = am_utf8_to_16(text, wide, max);
            if (n >= min) break;
            LOG("am: the software keyboard needs %u characters or more, it asks again\n", min);
            am_utf16_to_8(wide, n, initial, sizeof initial);
        }
    out = am_storage_new(AM_SWKBD_OUT, NULL);
    d = am_storage_bytes(out);
    am_put32(d, r ? 1 : 0); /* SwkbdResult: Ok, Cancel */
    if (!r) {
        if (utf8)
            am_utf16_to_8(wide, n, (char*)d + 4, AM_SWKBD_OUT - 4);
        else
            memcpy(d + 4, wide, (size_t)n * 2);
    }
    LOG("am: swkbd %s with %u UTF-16 code units\n", r ? "cancelled" : "accepted", r ? 0 : n);
    return out;
}

/* ------------------------------------------------------------ the inline keyboard */
/* a reply (SwkbdState, SwkbdReplyType, then size - 8 bytes) queued for PopInteractiveOutData; its body to fill */
static u8* kb_reply(LibApplet* a, u32 type, u32 size) {
    SvcObj* s;
    u8* d;
    if (a->nout >= AM_OUT_DATA) return NULL;
    s = am_storage_new(size, NULL);
    d = am_storage_bytes(s);
    am_put32(d, a->kb_state);
    am_put32(d + 4, type);
    a->out[a->nout++] = s;
    kevent_signal(a->interactive_ev);
    return d + 8;
}
static void kb_show(LibApplet* a, u32 state) {
    a->kb_state = state;
    kb_reply(a, SWKBD_DEFAULT, 8);
}
/* the text, in the encoding the game asked for, ahead of its arguments (SwkbdChangedStringArg: the length,
   no dictionary range, the cursor; SwkbdDecidedEnterArg: the length) */
static void kb_text_reply(LibApplet* a, int decided) {
    u32 type = a->kb_utf8 ? (decided ? SWKBD_ENTER_UTF8 : SWKBD_CHANGED_UTF8) : (decided ? SWKBD_ENTER : SWKBD_CHANGED);
    u32 text = a->kb_utf8 ? SWKBD_UTF8_SIZE : SWKBD_UTF16_SIZE;
    u8* d = kb_reply(a, type, 8 + text + (decided ? 4 : 16));
    if (!d) return;
    if (a->kb_utf8)
        am_utf16_to_8(a->kb_text, a->kb_len, (char*)d, SWKBD_UTF8_SIZE);
    else
        memcpy(d, a->kb_text, (size_t)(a->kb_len < SWKBD_UTF16_SIZE / 2 ? a->kb_len : SWKBD_UTF16_SIZE / 2 - 1) * 2);
    am_put32(d + text, a->kb_len);
    if (!decided) {
        am_put32(d + text + 4, ~0u);
        am_put32(d + text + 8, ~0u);
        am_put32(d + text + 12, (u32)a->kb_cursor);
    }
}
/* Appear: shown, then the host's dialog decides at once, and the keyboard hides again */
static void kb_appear(LibApplet* a) {
    char text[SWKBD_UTF8_SIZE], initial[SWKBD_UTF8_SIZE];
    int r;
    kb_show(a, SWKBD_SHOWN);
    am_utf16_to_8(a->kb_text, a->kb_len, initial, sizeof initial);
    r = plat_text_input(text, sizeof text, initial);
    if (r < 0) {
        LOG("am: no text dialog on this host, the inline keyboard answers \"%s\"\n", initial);
        memcpy(text, initial, sizeof text);
        r = 0;
    }
    if (r)
        kb_reply(a, SWKBD_CANCEL, 8);
    else {
        a->kb_len = am_utf8_to_16(text, a->kb_text, a->kb_max);
        a->kb_cursor = (i32)a->kb_len;
        kb_text_reply(a, 0);
        kb_text_reply(a, 1);
    }
    kb_show(a, SWKBD_HIDDEN);
}
/* Calc: the SwkbdCalcArg after the command word, either layout */
static void kb_calc(LibApplet* a, const u8* req, u64 size) {
    const u8* arg = req + 4;
    u64 flags;
    u32 appear, cursor, text, max, i;
    if (size < 4 + 0x18) return;
    memcpy(&flags, arg + 8, 8);
    if (size >= 4 + SWKBD_CALC_NEW_SIZE) {
        appear = 0x18; /* SwkbdCalcArgNew: appear argument, volume, cursor, text, utf8 mode */
        cursor = 0x8c;
        text = 0x90;
    } else if (size >= 4 + SWKBD_CALC_OLD_SIZE) {
        appear = 0x20; /* SwkbdCalcArgOld: volume, cursor, appear argument, text, utf8 mode */
        cursor = 0x1c;
        text = 0x68;
    } else
        return;
    if (flags & SWKBD_SET_TEXT) {
        for (i = 0; i < SWKBD_TEXT_CHARS - 1; i++) {
            memcpy(&a->kb_text[i], arg + text + 2 * i, 2);
            if (!a->kb_text[i]) break;
        }
        a->kb_len = i;
    }
    if (flags & SWKBD_SET_CURSOR) memcpy(&a->kb_cursor, arg + cursor, 4);
    if (flags & SWKBD_SET_UTF8) a->kb_utf8 = arg[text + 2 * SWKBD_TEXT_CHARS] != 0;
    if (a->kb_state == SWKBD_NOT_INITIALIZED && (flags & SWKBD_SET_INIT)) {
        a->kb_state = SWKBD_HIDDEN;
        kb_reply(a, SWKBD_FINISHED_INIT, 9);
    }
    if (!(flags & SWKBD_SET_INIT) && (flags & (SWKBD_SET_TEXT | SWKBD_SET_CURSOR)) && a->kb_state != SWKBD_NOT_INITIALIZED)
        kb_text_reply(a, 0);
    if (a->kb_state == SWKBD_HIDDEN && (flags & SWKBD_APPEAR)) {
        memcpy(&max, arg + appear + 0x20, 4); /* max_text_length */
        a->kb_max = max && max < SWKBD_TEXT_CHARS - 1 ? max : SWKBD_TEXT_CHARS - 1;
        kb_appear(a);
    } else if (a->kb_state == SWKBD_SHOWN && (flags & SWKBD_DISAPPEAR))
        kb_show(a, SWKBD_HIDDEN);
}
static int nn_am_service_ILibraryAppletAccessor_PushInteractiveInData(SvcObj* self, SvcObj* storage) {
    LibApplet* a = (LibApplet*)self->data;
    StorageData* s;
    const u8* req;
    u32 cmd;
    if (!storage || !am_is_inline(a)) R_SUCCEED();
    s = (StorageData*)storage->data;
    if (s->size < 4) R_SUCCEED();
    req = am_storage_bytes(storage);
    memcpy(&cmd, req, 4);
    pm_lock(&g_hle.am.lock);
    switch (cmd) {
    case SWKBD_REQ_FINALIZE:
        a->kb_state = SWKBD_NOT_INITIALIZED;
        am_applet_finish(a);
        break;
    case SWKBD_REQ_CALC: kb_calc(a, req, s->size); break;
    case SWKBD_REQ_USER_WORDS: kb_reply(a, SWKBD_RELEASED_WORDS, 8); break;
    case SWKBD_REQ_UNSET_DICTS: kb_reply(a, SWKBD_UNSET_DICTS, 8); break;
    default: break; /* the dictionaries and the V2 reply flags change nothing here */
    }
    pm_unlock(&g_hle.am.lock);
    R_SUCCEED();
}

/* the Mii editor (nn::mii::AppletInput, 0x100 bytes: version, mode, then a v4's special key code and CharInfo), with
   no editor and no database to add to: an edit gives the Mii back unchanged, the modes that only show something
   succeed, and adding or creating a Mii is cancelled, as when the player backs out of the editor. The answer is
   nn::mii::AppletOutput {result, index} or, for create and edit, AppletOutputForCharInfoEditing {result, CharInfo} */
#define MII_EDIT_SHOW         0
#define MII_EDIT_APPEND       1
#define MII_EDIT_CREATE       4
#define MII_EDIT_EDIT         5
#define MII_EDIT_SUCCESS      0
#define MII_EDIT_CANCEL       1
#define MII_EDIT_INPUT_SIZE   0x100
#define MII_EDIT_CHARINFO     0xc /* the v4 input's CharInfo, after the version, the mode and the special key code */
#define MII_EDIT_CHARINFO_LEN 0x58
static SvcObj* am_mii_edit(LibApplet* a) {
    const u8* in = NULL;
    u32 mode = MII_EDIT_APPEND, result;
    SvcObj* out;
    int i;
    for (i = 0; i < a->nin && !in; i++)
        if (((StorageData*)a->in[i]->data)->size >= MII_EDIT_INPUT_SIZE) in = am_storage_bytes(a->in[i]);
    if (in) memcpy(&mode, in + 4, 4);
    if (mode == MII_EDIT_CREATE || mode == MII_EDIT_EDIT) {
        result = mode == MII_EDIT_EDIT && in ? MII_EDIT_SUCCESS : MII_EDIT_CANCEL;
        out = am_storage_new(0x80, NULL);
        memcpy(am_storage_bytes(out), &result, 4);
        if (result == MII_EDIT_SUCCESS)
            memcpy(am_storage_bytes(out) + 4, in + MII_EDIT_CHARINFO, MII_EDIT_CHARINFO_LEN);
        return out;
    }
    result = mode == MII_EDIT_APPEND || mode > MII_EDIT_EDIT ? MII_EDIT_CANCEL : MII_EDIT_SUCCESS;
    out = am_storage_new(0x20, NULL);
    memcpy(am_storage_bytes(out), &result, 4);
    LOG("am: Mii editor mode %u answered %s\n", mode, result == MII_EDIT_SUCCESS ? "success" : "cancel");
    return out;
}
static int nn_am_service_ILibraryAppletAccessor_PopOutData(SvcObj* self, SvcObj** storage) {
    LibApplet* a = (LibApplet*)self->data;
    u8* d;
    u32 first = 0;
    int popped;
    pm_lock(&g_hle.am.lock);
    popped = a->out_popped || am_is_inline(a);
    a->out_popped = 1;
    pm_unlock(&g_hle.am.lock);
    R_UNLESS(!popped, AM_NO_DATA);
    switch (a->applet_id) {
    case AM_APPLET_CONTROLLER: { /* ControllerSupportResultInfo: the players connected and the first of them */
        /* ControllerSupportArg, the last storage pushed, starts with the fewest players the game accepts */
        int players = hid_connected_players(&first);
        int least = a->nin ? (signed char)am_storage_bytes(a->in[a->nin - 1])[0] : 0;
        *storage = am_storage_new(0xc, NULL);
        d = am_storage_bytes(*storage);
        d[0] = (u8)players;
        memcpy(d + 4, &first, 4);
        if (players < least) { /* nobody to pick: the player closed the applet */
            LOG("am: the controller applet wants %d players, %d are connected: cancelled\n", least, players);
            am_put32(d + 8, 2); /* the result: cancelled */
        }
        break;
    }
    case AM_APPLET_PROFILE_SELECT: /* UiReturnArg: success, the user the game plays as */
        *storage = am_storage_new(0x18, NULL);
        memcpy(am_storage_bytes(*storage) + 8, snx_users_selected()->uid, 16);
        break;
    case AM_APPLET_SWKBD: *storage = am_swkbd(a); break;
    case AM_APPLET_MII_EDIT: *storage = am_mii_edit(a); break;
    case AM_APPLET_WEB:
    case AM_APPLET_SHOP:
    case AM_APPLET_OFFLINE_WEB:
    case AM_APPLET_LOGIN_SHARE:
    case AM_APPLET_WEB_AUTH: /* WebCommonReturnValue: the page was closed (0), no last URL */
        *storage = am_storage_new(0x1010, NULL);
        break;
    default: /* the other applets' results start with a zero result code */
        *storage = am_storage_new(0x10, NULL);
        break;
    }
    kevent_clear(a->out_ev);
    R_SUCCEED();
}
static int nn_am_service_ILibraryAppletAccessor_PopInteractiveOutData(SvcObj* self, SvcObj** storage) {
    LibApplet* a = (LibApplet*)self->data;
    int rc = 0;
    pm_lock(&g_hle.am.lock);
    if (!a->nout)
        rc = AM_NO_DATA;
    else {
        *storage = a->out[0];
        a->nout--;
        memmove(a->out, a->out + 1, (size_t)a->nout * sizeof a->out[0]);
    }
    if (!a->nout) kevent_clear(a->interactive_ev);
    pm_unlock(&g_hle.am.lock);
    return rc;
}
static int nn_am_service_ILibraryAppletAccessor_GetPopOutDataEvent(SvcObj* self, u32* event) {
    *event = ev_handle(((LibApplet*)self->data)->out_ev);
    R_SUCCEED();
}
static int nn_am_service_ILibraryAppletAccessor_GetPopInteractiveOutDataEvent(SvcObj* self, u32* event) {
    *event = ev_handle(((LibApplet*)self->data)->interactive_ev);
    R_SUCCEED();
}
static int nn_am_service_ILibraryAppletAccessor_GetLibraryAppletInfo(SvcObj* self,
                                                                    nn_am_service_LibraryAppletInfo* info) {
    LibApplet* a = (LibApplet*)self->data;
    info->applet_id = a->applet_id;
    info->library_applet_mode = a->mode;
    R_SUCCEED();
}
