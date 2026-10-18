/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The game's menu (runtime/platform/overlay.c): where it goes, its pages, what a tap, a move or a step does, its look;
   and ui.c's atlas and text */
#include "nx/runtime/platform/overlay.h"
#include "nx/runtime/platform/fatal_screen.h"
#include "nx/runtime/platform/backend.h"
#include "nx/runtime/platform/gamepad.h"
#include "nx/runtime/platform/touch.h"
#include "nx/runtime/platform/vpad.h"
#include "nx/runtime/platform/controls_atlas.h"
#include "nx/runtime/platform/plat.h"
#include "common/utilities/font.h"
#include "gui_theme_colors.h"
/* the menu's colour of a held control, as R G B A bytes */
#define FAILURE                                                                                                        \
    (0xFFu << 24 | (SNX_GUI_THEME_YORHA_FAILURE & 0xFFu) << 16 | (SNX_GUI_THEME_YORHA_FAILURE & 0xFF00u) |             \
     (SNX_GUI_THEME_YORHA_FAILURE >> 16 & 0xFFu))
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the platform's lock and the backend's touch screen, without either */
void pm_lock(PMutex* m) { (void)m; }
void pm_unlock(PMutex* m) { (void)m; }
static int touch = 1;
void backend_touch_set(int mode) { touch = mode; }
/* and the gamepads' settings and names, as backend/gamepad.c keeps them; their rows and touch's name them */
static SnxGamepadConfig pads = {.swap_ab = 0, .deadzone = 20, .rumble = 100};
static const int* pad_stored;
static const char* pad_names[3];
SnxGamepadConfig* gamepad_config(void) { return &pads; }
void gamepad_store(const int* setting) { pad_stored = setting; }
static int maps_stored;
static unsigned pads_held;
void gamepad_store_map(void) { ++maps_stored; }
int gamepad_layout(int i, int labels[4], int* symmetric) {
    (void)i;
    (void)labels;
    (void)symmetric;
    return 0; /* no labels known: an Xbox pad's */
}
unsigned gamepad_held(void) { return pads_held; }
const char* gamepad_name(int i) { return i >= 0 && i < 3 ? pad_names[i] : NULL; }
const IniOpt g_backend_ini[] = {
    {"touch", "2", NULL, INI_INT, 0, &touch, 0, 2, NULL, "Touch screen", "Off|Fingers|Mouse too", 0, 0},
    {NULL, NULL, NULL, 0, 0, NULL, 0, 0, NULL, NULL, NULL, 0, 0}};
/* each player's own, two of them here: swap and vibration, every player's until picked, and a map */
static SnxPlayerConfig players[2] = {{"", 1, 0, -1, -10, 0, {{0}}}, {"", 1, 0, -1, -10, 0, {{0}}}};
static int player_maps_stored;
SnxPlayerConfig* gamepad_player(int p) { return p >= 0 && p < 2 ? &players[p] : NULL; }
void gamepad_store_player_map(int p) {
    (void)p;
    ++player_maps_stored;
}
const IniOpt g_gamepad_players_ini[] = {
    {"gamepad_p1_swap_ab", "-1", NULL, INI_INT, 0, &players[0].swap_ab, -1, 1, NULL, "Swap A/B", "As All|Off|On", 0, 0},
    {"gamepad_p1_rumble", "-10", NULL, INI_INT, 0, &players[0].rumble, -10, 100, NULL, "Vibration", "As All|Off|#%", 10,
     0},
    {NULL, NULL, NULL, 0, 0, NULL, 0, 0, NULL, NULL, NULL, 0, 0}};
const IniOpt g_gamepad_ini[] = {
    {"gamepad_swap_ab", "0", NULL, INI_INT, 0, &pads.swap_ab, 0, 1, NULL, "Swap A/B", "Off|On", 0, 0},
    {"gamepad_deadzone", "20", NULL, INI_INT, 0, &pads.deadzone, 0, 90, NULL, "Stick dead zone", "#%", 5, 0},
    {"gamepad_rumble", "100", NULL, INI_INT, 0, &pads.rumble, 0, 100, NULL, "Vibration", "Off|#%", 10, 0},
    {"gamepad_background", "0", NULL, INI_INT, 0, &pads.background, 0, 1, NULL, "Background input", "Off|On", 0, 0},
    {NULL, NULL, NULL, 0, 0, NULL, 0, 0, NULL, NULL, NULL, 0, 0}};
static char stored[64], log_[512]; /* the last key kept, and every key=value since log_ was emptied */
static void store(const char* key, const char* value) {
    size_t n = strlen(log_);
    snprintf(stored, sizeof stored, "%s", key);
    snprintf(log_ + n, sizeof log_ - n, "%s=%s;", key, value);
}

enum { SW = 2424, SH = 1080 };
static int px, py, s, C; /* C: the panel's columns, overlay_cols' */
/* a tap on a row of items, at a column, where the panel is for its page */
static int tap(int row, int col) {
    overlay_place(SW, SH, &px, &py, &s);
    C = overlay_cols(SW, s);
    return overlay_tap((float)(px + col * 8 * s + 2), (float)(py + (3 + row) * 16 * s + 5), SW, SH);
}
static SnxUiQuad quads[2048];
static SnxUiList list;
static void draw(void) {
    overlay_place(SW, SH, &px, &py, &s);
    C = overlay_cols(SW, s);
    list.q = quads;
    list.n = 0;
    list.max = 2048;
    overlay_draw(&list, SW, SH);
}
/* whether text shows anywhere on the picture, its letters a cell apart on one row (spaces draw nothing) */
static int shows(const char* text) {
    int i, k;
    for (i = 0; i < list.n; ++i) {
        float x = list.q[i].x, y = list.q[i].y, step = 8.0f * (float)s;
        for (k = 0; text[k]; ++k) {
            unsigned char c = (unsigned char)text[k];
            int j, found = c == ' ';
            for (j = 0; j < list.n && !found; ++j)
                found = list.q[j].x == x + (float)k * step && list.q[j].y == y &&
                        list.q[j].u == SNX_CONTROLS_FONT_X + c % 16 * 8 &&
                        list.q[j].v == SNX_CONTROLS_FONT_Y + c / 16 * 16;
            if (!found) break;
        }
        if (!text[k]) return 1;
    }
    return 0;
}
/* the quads filled with rgba */
static int quads_of(u32 rgba) {
    int i, n = 0;
    for (i = 0; i < list.n; ++i) n += list.q[i].rgba == rgba && list.q[i].w > 0;
    return n;
}
/* the quad of a glyph at a cell, or -1 */
static int glyph_at(int col, int row, u8 c) {
    int i;
    for (i = 0; i < list.n; ++i)
        if (list.q[i].x == (float)(px + col * 8 * s) && list.q[i].y == (float)(py + row * 16 * s) &&
            list.q[i].u == SNX_CONTROLS_FONT_X + c % 16 * 8 && list.q[i].v == SNX_CONTROLS_FONT_Y + c / 16 * 16)
            return i;
    return -1;
}
/* whether a line of text is drawn from that cell on, in the font's glyphs (a space draws none) */
static int text_at(int col, int row, const char* text) {
    int k;
    for (k = 0; text[k]; ++k)
        if (text[k] != ' ' && glyph_at(col + k, row, (u8)text[k]) < 0) return 0;
    return 1;
}

/* a stopped panel's texts, Close in focus */
static SnxOverlayError panel_of(const char* name, const char* msg, const char* how, const char* log, const char* said) {
    SnxOverlayError e;
    memset(&e, 0, sizeof e);
    e.name = name;
    e.msg = msg;
    e.how = how;
    e.log = log;
    e.said = said;
    e.focus = SNX_OVERLAY_ERROR_CLOSE;
    return e;
}
int main(void) {
    const u32 ink = 0xFF3A4145u;
    int x, y, sc;
    vpad_set_store(store);
    snx_pad_map_unset(&pads.own); /* as gamepad.c starts it: every control the button in its place */
    snx_pad_map_unset(&players[0].own);
    assert(!overlay_shown());
    overlay_toggle();
    assert(overlay_shown());
    /* letters half the screen's height at most, in whole steps, in the middle: a phone held wide, a small window. The
       page is as tall as its items (three here, under three rows of head, over a blank one), at the scale of the
       tallest page, and as wide as three quarters of the picture hold, between 40 and 72 columns */
    overlay_place(SW, SH, &px, &py, &s);
    C = overlay_cols(SW, s);
    assert(s == 2 && C == OVERLAY_COLS_MAX && px == (SW - C * 16) / 2 && py == (SH - 7 * 32) / 2);
    assert(overlay_cols(1280, 2) == 60 && overlay_cols(500, 1) == 46 && overlay_cols(400, 1) == OVERLAY_COLS &&
           overlay_cols(3840, 4) == OVERLAY_COLS_MAX);
    overlay_place(500, 120, &x, &y, &sc);
    assert(sc == 1 && x == (500 - 46 * 8) / 2 && y == 4);
    /* a phone's display scale: the Poco F8 (408 dpi, 2510x1156) and the Pixel 9 (420 dpi) at 3x, where half the
       picture gave 2x; a 4K picture at 100% keeps the 4x half of it gives */
    overlay_set_display_scale(408.0f / 160);
    overlay_place(2510, 1156, &x, &y, &sc);
    assert(sc == 3);
    overlay_set_display_scale(420.0f / 160);
    overlay_place(SW, SH, &x, &y, &sc);
    assert(sc == 3);
    overlay_set_display_scale(1.0f);
    overlay_place(3840, 2160, &x, &y, &sc);
    assert(sc == 4);

    /* the first page: the title on a band of ink, the items under it, the focused one on a band; the page hides the
       game, whose text would show through it */
    draw();
    assert(list.q[0].w == (float)(C * 16) && list.q[0].h == 7 * 32 && list.q[1].rgba == ink && list.q[1].h == 32);
    assert(list.q[0].rgba >> 24 == 0xFFu);
    assert(text_at(C - 2, 3, ">") && text_at(C - 2, 4, ">")); /* a page under each */
    assert(text_at(1, 0, "SCARLETNX  /  MENU") && text_at(1, 3, "\xFE On-screen controls") &&
           text_at(1, 4, "\xFE Controllers") && text_at(1, 5, "\xFE Exit"));
    /* a tap on Exit exits; on the title, nothing; outside the panel, it closes */
    assert(tap(2, 3) == OVERLAY_EXIT && tap(-3, 3) == OVERLAY_NONE);
    assert(overlay_tap(5.0f, 5.0f, SW, SH) == OVERLAY_CLOSE);
    /* the focus goes round, from Exit to the first item and back over the top; Back from here closes */
    overlay_move(1);
    overlay_move(-4);
    assert(overlay_select() == OVERLAY_EXIT && overlay_back() == OVERLAY_CLOSE);

    /* the controls' page: each setting shown with its value, changed and kept */
    assert(tap(0, 3) == OVERLAY_NONE);
    draw();
    assert(text_at(1, 0, "SCARLETNX  /  CONTROLS") && text_at(1, 3, "\xFE Show controls"));
    assert(list.q[0].h == 16 * 32 && text_at(C - 2, 12, ">")); /* twelve items: the tallest; Buttons opens a page */
    /* named as the .ini's rows name them, in the launcher's order, the game's own actions among them */
    assert(text_at(1, 4, "\xFE Edit layout") && text_at(1, 5, "\xFE Snap to grid") &&
           text_at(1, 8, "\xFE Relative stick center"));
    assert(text_at(1, 11, "\xFE Touch screen") && text_at(1, 12, "\xFE Buttons") &&
           text_at(1, 13, "\xFE Reset layout"));
    vpad_config()->on = 0;
    draw();
    assert(text_at(C - 4, 3, "Off"));
    assert(tap(0, 3) == OVERLAY_NONE && vpad_config()->on && !strcmp(stored, "controls"));
    draw();
    assert(text_at(C - 3, 3, "On"));
    /* Scale's arrows: the left one down, the right one up, by tens, between 50% and 150% */
    assert(vpad_config()->scale == 50);
    draw();
    assert(text_at(C - 9, 6, "< 100% >"));
    tap(3, C - 2);
    assert(vpad_config()->scale == 60 && !strcmp(stored, "controls_scale"));
    tap(3, C - 9);
    tap(3, C - 9);
    assert(vpad_config()->scale == 40);
    for (x = 0; x < 9; ++x) tap(3, C - 9);
    assert(vpad_config()->scale == 0);
    vpad_config()->scale = 50;
    /* the keys: down from Scale to Opacity, left steps it */
    overlay_move(1);
    overlay_side(-1);
    assert(vpad_config()->opacity == 90 && !strcmp(stored, "controls_opacity"));
    overlay_side(1);
    /* the toggles; the touch screen goes round its three */
    tap(2, 3);
    assert(vpad_config()->snap && !strcmp(stored, "controls_snap"));
    tap(2, 3);
    tap(5, 3);
    assert(!vpad_config()->rel_center && !strcmp(stored, "controls_rel_center"));
    tap(5, 3);
    tap(8, 3);
    assert(touch == 2);
    draw();
    assert(text_at(C - 10, 11, "Mouse too"));
    tap(8, 3);
    assert(touch == 0);
    draw();
    assert(text_at(C - 4, 11, "Off"));
    tap(8, 3);
    /* Edit layout is the caller's to do */
    assert(tap(1, 3) == OVERLAY_EDIT);

    /* the buttons' page: one hidden and kept, All shows them again; it scrolls to its last item */
    assert(tap(9, 3) == OVERLAY_NONE);
    draw();
    assert(text_at(1, 0, "SCARLETNX  /  BUTTONS") && text_at(1, 4, "\xFE L"));
    log_[0] = 0;
    tap(1, 3);
    assert(!vpad_config()->at[SNX_VPAD_L].shown && !strcmp(log_, "controls_l_shown=0;")); /* its place stays */
    draw();
    assert(text_at(C - 7, 4, "Hidden") && text_at(C - 12, 3, "Some hidden"));
    tap(0, 3);
    assert(vpad_config()->at[SNX_VPAD_L].shown);
    tap(0, 3); /* all shown: All hides them */
    assert(!vpad_config()->at[SNX_VPAD_A].shown && !vpad_config()->at[SNX_VPAD_L].shown);
    tap(0, 3);
    overlay_move(-1); /* round to Back, at the bottom */
    draw();
    assert(text_at(1, 3 + 11, "\xFE Back") && text_at(C - 1, 3, "\x18") && !text_at(C - 1, 14, "\x19"));
    /* the wheel stops at the ends: up past All, and down past Back, where Back still goes up a page */
    overlay_scroll(-20);
    draw();
    assert(text_at(1, 3, "\xFE All") && !text_at(C - 1, 3, "\x18") && text_at(C - 1, 14, "\x19"));
    overlay_scroll(20);
    overlay_scroll(1);
    /* Back on it, and the key Back, go up a page each */
    assert(overlay_select() == OVERLAY_NONE);
    draw();
    assert(text_at(1, 0, "SCARLETNX  /  CONTROLS"));
    assert(overlay_back() == OVERLAY_NONE);
    draw();
    assert(text_at(1, 0, "SCARLETNX  /  MENU"));
    /* a pad's buttons, as the game reads them: the D-pad and the left stick move, A selects, B goes back, the rest do
       nothing */
    {
        static const i32 still[4], down[4] = {0, -20000, 0, 0}, left[4] = {-20000, 0, 0, 0};
        assert(overlay_pad(1ull << 15) == OVERLAY_NONE && overlay_pad(1ull << 19) == OVERLAY_NONE);
        assert(overlay_pad(1ull << 13) == OVERLAY_NONE && overlay_pad(1ull << 2) == OVERLAY_NONE); /* X */
        assert(overlay_pad(1ull << 0) == OVERLAY_NONE);                                            /* Controllers */
        draw();
        assert(text_at(1, 0, "SCARLETNX  /  CONTROLLERS"));
        assert(overlay_pad(1ull << 1) == OVERLAY_NONE);
        draw();
        assert(text_at(1, 0, "SCARLETNX  /  MENU"));
        /* the pads each poll (the on-screen controls and the connected ones): a press acts once; a direction held, the
           left stick past half way as one, goes on after a while, then on and on; with the menu closed, nothing, and
           a press held as it opens does not act */
        assert(overlay_controls(0, still, 1000) == OVERLAY_NONE); /* the focus back on Controllers */
        assert(overlay_controls(0, down, 1000) == OVERLAY_NONE);  /* to Exit */
        assert(overlay_controls(0, down, 1000 + SNX_OVERLAY_REPEAT_DELAY - 1) == OVERLAY_NONE);
        assert(overlay_controls(1ull << 0, down, 1000 + SNX_OVERLAY_REPEAT_DELAY - 1) == OVERLAY_EXIT); /* not yet */
        assert(overlay_controls(0, down, 1000 + SNX_OVERLAY_REPEAT_DELAY) == OVERLAY_NONE); /* round to the top */
        assert(overlay_controls(1ull << 0, down, 1001 + SNX_OVERLAY_REPEAT_DELAY) == OVERLAY_NONE); /* a page */
        draw();
        assert(text_at(1, 0, "SCARLETNX  /  CONTROLS"));
        assert(overlay_controls(1ull << 1, still, 2000) == OVERLAY_NONE);  /* B: back to the menu, on its first */
        assert(overlay_controls(1ull << 15, still, 2001) == OVERLAY_NONE); /* the D-pad down: Controllers */
        assert(overlay_controls(1ull << 15, still, 2001 + SNX_OVERLAY_REPEAT_DELAY) == OVERLAY_NONE); /* Exit */
        assert(overlay_controls(1ull << 15, still, 2001 + SNX_OVERLAY_REPEAT_DELAY + SNX_OVERLAY_REPEAT_MS - 1) ==
               OVERLAY_NONE);
        assert(overlay_controls(1ull << 0, still, 3000) == OVERLAY_EXIT);
        assert(overlay_controls(1ull << 13, still, 3001) == OVERLAY_NONE &&
               overlay_controls(0, left, 3002) == OVERLAY_NONE);
        assert(overlay_controls(1ull << 1, still, 3003) == OVERLAY_CLOSE);
        overlay_toggle(); /* closed: the pads are the game's */
        assert(!overlay_shown() && overlay_controls(1ull << 1, still, 3004) == OVERLAY_NONE);
        overlay_toggle(); /* B still held as it opens */
        assert(overlay_controls(1ull << 1, still, 3005) == OVERLAY_NONE);
        assert(overlay_controls(0, still, 3006) == OVERLAY_NONE);
        /* a finger or the mouse on the open menu: down on an on-screen control it is the controls', whose D-pad then
           moves the menu and whose lift elsewhere taps nothing; off them its lift taps the menu, outside the panel
           closing it; one the menu forgot (it opened, the window lost the focus) or the system cancelled taps nothing */
        {
            const int area[4] = {0, 0, SW, SH};
            const SnxVpadConfig* v = vpad_config();
            float dx = v->at[SNX_VPAD_DPAD].x * SW, dy = v->at[SNX_VPAD_DPAD].y * SH + 0.6f * 0.25f * SH / 2;
            u64 b;
            i32 st[4];
            assert(vpad_ini("controls", "1"));
            overlay_pointer_down(SNX_TOUCH_MOUSE, dx, dy, area);
            vpad_state(&b, st);
            assert(b == 1ull << 15 && overlay_controls(b, still, 4000) == OVERLAY_NONE); /* to Controllers */
            assert(overlay_pointer_up(SNX_TOUCH_MOUSE, 5, 5, SW, SH) == OVERLAY_NONE && overlay_shown());
            vpad_state(&b, st);
            assert(!b && overlay_controls(1ull << 0, still, 4001) == OVERLAY_NONE);
            draw();
            assert(text_at(1, 0, "SCARLETNX  /  CONTROLLERS"));
            assert(overlay_controls(1ull << 1, still, 4002) == OVERLAY_NONE); /* B: the menu */
            overlay_pointer_down(8, 5, 5, area);
            assert(overlay_pointer_up(8, 5, 5, SW, SH) == OVERLAY_CLOSE);
            overlay_pointer_down(9, 5, 5, area);
            overlay_pointers_reset();
            assert(overlay_pointer_up(9, 5, 5, SW, SH) == OVERLAY_NONE);
            overlay_pointer_down(10, 5, 5, area);
            overlay_pointer_cancel(10);
            assert(overlay_pointer_up(10, 5, 5, SW, SH) == OVERLAY_NONE);
            assert(vpad_ini("controls", "0"));
        }
    }

    /* the controllers' page: the ones connected, the Player row (every player's, or one's own), Swap A/B, the dead
       zone by fives, the vibration by tens down to Off and the background input, each kept */
    assert(tap(1, 3) == OVERLAY_NONE);
    draw();
    assert(text_at(1, 0, "SCARLETNX  /  CONTROLLERS") && text_at(1, 1, "None connected."));
    assert(list.q[0].h == 11 * 32 && text_at(1, 3, "\xFE Player") && shows("< All >"));
    assert(text_at(1, 4, "\xFE Swap A/B") && text_at(C - 4, 4, "Off"));
    tap(1, 3);
    assert(pads.swap_ab && pad_stored == &pads.swap_ab);
    draw();
    assert(text_at(C - 3, 4, "On"));
    tap(1, 3);
    assert(!pads.swap_ab);
    overlay_side(1);
    assert(pads.swap_ab);
    pads.swap_ab = 0;
    draw();
    assert(text_at(C - 8, 5, "< 20% >"));
    tap(2, C - 2);
    assert(pads.deadzone == 25 && pad_stored == &pads.deadzone);
    tap(2, C - 9);
    tap(2, C - 9);
    assert(pads.deadzone == 15);
    for (x = 0; x < 20; ++x) tap(2, C - 2);
    assert(pads.deadzone == 90);
    pads.deadzone = 20;
    for (x = 0; x < 10; ++x) tap(3, C - 9);
    assert(pads.rumble == 0 && pad_stored == &pads.rumble);
    draw();
    assert(text_at(C - 8, 6, "< Off >"));
    assert(overlay_select() == OVERLAY_NONE && pads.rumble == 10); /* the focus is on it: a select steps up */
    pads.rumble = 100;
    draw();
    assert(text_at(1, 7, "\xFE Background input") && text_at(C - 4, 7, "Off"));
    tap(4, 3);
    assert(pads.background && pad_stored == &pads.background);
    draw();
    assert(text_at(C - 3, 7, "On"));
    tap(4, 3);
    assert(!pads.background);
    pad_names[0] = "Xbox Wireless Controller";
    draw();
    assert(text_at(1, 1, "Xbox Wireless Controller") && !text_at(26, 1, "and"));
    pad_names[1] = "Pro Controller";
    pad_names[2] = "A controller with a very long name indeed";
    draw();
    assert(text_at(1, 1, "Xbox Wireless Controller and 2 more"));
    pad_names[0] = pad_names[2];
    draw();
    assert(text_at(1, 1, "A controller with a very lo and 2 more"));
    /* Button map: NieR's CONTROLS over the picture, no presets, the first control focused; its button changed (the map
       the player's from then on), a press on the pad while waiting swapping two, that press no menu key; Back goes up
       to its item */
    pad_names[0] = "Xbox Wireless Controller";
    pad_names[1] = pad_names[2] = NULL;
    draw();
    assert(text_at(1, 8, "\xFE Button map") && text_at(C - 2, 8, ">"));
    assert(tap(5, 3) == OVERLAY_NONE);
    pads_held = 1u << SNX_PAD_RT;
    draw();
    assert(shows("SCARLETNX  /  BUTTON MAP") && !shows("By label") && shows("Xbox Wireless Controller"));
    assert(shows("LT sends ZL") && quads_of(FAILURE) > 0); /* RT held: lit */
    overlay_side(1);                                       /* LT's button, ZL, up one: ZR */
    assert(pads.own.bit[SNX_PAD_LT] == 9 && maps_stored == 1);
    draw();
    assert(shows("LT sends ZR"));
    assert(overlay_select() == OVERLAY_NONE && overlay_listening());
    overlay_press(SNX_PAD_SOUTH); /* the bottom one, B: LT and it swap */
    assert(!overlay_listening() && pads.own.bit[SNX_PAD_LT] == 1 && pads.own.bit[SNX_PAD_SOUTH] == 9 &&
           maps_stored == 2);
    {
        static const i32 rest[4];
        assert(overlay_controls(1ull << 0, rest, 5000) == OVERLAY_NONE && !overlay_listening()); /* taken: no A */
        assert(overlay_controls(1ull << 0, rest, 5001) == OVERLAY_NONE && !overlay_listening());
        assert(overlay_controls(0, rest, 5002) == OVERLAY_NONE);
    }
    assert(overlay_back() == OVERLAY_NONE); /* no presets to go back to: up to its item at once */
    draw();
    assert(text_at(1, 0, "SCARLETNX  /  CONTROLLERS") && text_at(1, 8, "\xFE Button map"));
    pads_held = 0;
    /* the Player row steps to player 1: its Swap A/B and Vibration its own, As All until picked; its Button map
       its own map, kept as its key; round the eight back to All */
    tap(0, 3);
    draw();
    assert(shows("< 1 >") && text_at(1, 4, "\xFE Swap A/B") && shows("As All"));
    tap(1, 3);
    assert(players[0].swap_ab == 0 && pad_stored == &players[0].swap_ab && !pads.swap_ab);
    tap(5, 3);
    draw();
    assert(shows("SCARLETNX  /  PLAYER 1  /  BUTTON MAP"));
    overlay_side(1); /* from every player's map, where LT sends B now: up one, X; every player's stays */
    assert(players[0].has_map && players[0].own.bit[SNX_PAD_LT] == 2 && pads.own.bit[SNX_PAD_LT] == 1 &&
           player_maps_stored == 1 && maps_stored == 2);
    assert(overlay_back() == OVERLAY_NONE);
    for (x = 0; x < 8; ++x) tap(0, 3);
    draw();
    assert(shows("< All >"));
    /* Back, on its item or the key, comes up to the controllers' item */
    assert(tap(6, 3) == OVERLAY_NONE);
    draw();
    assert(text_at(1, 0, "SCARLETNX  /  MENU") && overlay_select() == OVERLAY_NONE);
    draw();
    assert(text_at(1, 0, "SCARLETNX  /  CONTROLLERS") && overlay_back() == OVERLAY_NONE);
    draw();
    assert(text_at(1, 0, "SCARLETNX  /  MENU"));
    /* closed, it draws nothing; opened again, its first page */
    overlay_toggle();
    draw();
    assert(!overlay_shown() && !list.n);
    overlay_toggle();
    draw();
    assert(text_at(1, 0, "SCARLETNX  /  MENU"));

    /* ui.c: the atlas with the font's glyphs, white where a glyph's bit is, and sprites' alpha premultiplied */
    {
        u32* sprites = calloc((size_t)SNX_CONTROLS_ATLAS_WIDTH * SNX_CONTROLS_ATLAS_HEIGHT, 4);
        const u32* a;
        int w, h;
        u32 version;
        assert(sprites);
        sprites[0] = 0x80FF8040u; /* half alpha */
        assert(!ui_atlas(&w, &h, &version) && !version);
        ui_atlas_set(sprites, 4, 4); /* not the atlas the header is for */
        assert(!ui_atlas(&w, &h, &version));
        ui_atlas_set(sprites, SNX_CONTROLS_ATLAS_WIDTH, SNX_CONTROLS_ATLAS_HEIGHT);
        a = ui_atlas(&w, &h, &version);
        assert(a && version == 1 && w == SNX_CONTROLS_ATLAS_WIDTH);
        assert(a[0] == 0x80804020u);
        /* every glyph, white where its bit is: 'A' has both */
        for (sc = 0; sc < 256; ++sc)
            for (y = 0; y < 16; ++y)
                for (x = 0; x < 8; ++x)
                    assert(a[(SNX_CONTROLS_FONT_Y + sc / 16 * 16 + y) * w + SNX_CONTROLS_FONT_X + sc % 16 * 8 + x] ==
                           (gui_vga_font[sc * 16 + y] & (0x80 >> x) ? 0xFFFFFFFFu : 0));
        assert(gui_vga_font['A' * 16 + 5] && gui_vga_font['A' * 16 + 5] != 0xFF);
        assert(a[(SNX_CONTROLS_WHITE_Y + 1) * w + SNX_CONTROLS_WHITE_X + 1] == 0xFFFFFFFFu);
        ui_atlas_done();
        assert(!ui_atlas(&w, &h, &version) && version == 1);
        free(sprites);
    }
    /* a fatal error's panel: the menu's, centred on its own rows; the head, the game's name, the error in the
       failure's colour, its hint, then Share log and Close at its foot, Close in focus. UTF-8 comes out in the font's
       bytes, and on a short picture the last paragraphs give up rows so the panel stays on it, what Share log did last */
    {
        float top, row_h;
        int total, i, close_band = -1, share_band = -1;
        unsigned plain, focused;
        SnxOverlayError panel = panel_of("ENDER MAGNOLIA", "vulkan: the GPU device was lost (driver reset or crash)",
                                         "Share log sends the game's log, which has what ran last.", NULL, NULL);
        list.n = 0;
        overlay_error_draw(&list, SW, SH, &panel);
        overlay_place(SW, SH, &px, &py, &s);
        row_h = 16.0f * (float)s;
        top = list.q[0].y;
        total = (int)(list.q[0].h / row_h);
        C = overlay_cols(SW, s);
        assert(list.n > 4 && list.q[0].x == (float)px && list.q[0].w == (float)(C * 8 * s) &&
               list.q[0].rgba >> 24 == 0xFFu);
        assert(top >= 0 && top + list.q[0].h <= (float)SH && list.q[1].y == top && list.q[1].h == row_h);
        py = (int)top;
        assert(text_at(1, 0, "SCARLETNX  /  STOPPED") && text_at(1, 1, "ENDER MAGNOLIA"));
        assert(
            text_at(1, 3, "vulkan: the GPU device was lost (driver reset or crash)")); /* one row, as wide as it is */
        i = glyph_at(1, 3, 'v');
        assert(i >= 0 && list.q[i].rgba == 0xFF3C47A5u);
        assert(text_at(1, 5, "The GPU stopped answering and its"));
        for (i = 0; i < list.n; ++i)
            if (list.q[i].y == top + row_h * (float)(total - 2) && list.q[i].h == row_h && list.q[i].w == list.q[0].w)
                close_band = i;
        assert(close_band >= 0 && text_at(1, total - 2, "\xFE Close") && text_at(1, total - 3, "\xFE Copy path") &&
               text_at(1, total - 4, "\xFE Share log"));
        i = glyph_at(3, total - 4, 'S');
        assert(i >= 0);
        plain = list.q[i].rgba;
        i = glyph_at(3, total - 2, 'C');
        assert(i >= 0 && list.q[i].rgba != plain);
        focused = list.q[i].rgba;
        for (i = 0; i < SNX_OVERLAY_ERROR_ITEMS; ++i)
            assert(panel.items[i].y == top + row_h * (float)(total - 1 - SNX_OVERLAY_ERROR_ITEMS + i) &&
                   panel.items[i].h == row_h && panel.items[i].x == list.q[0].x && panel.items[i].w == list.q[0].w);
        assert(overlay_error_hit(&panel.items[SNX_OVERLAY_ERROR_SHARE], panel.items[0].x + 1, panel.items[0].y + 1));
        assert(!overlay_error_hit(&panel.items[SNX_OVERLAY_ERROR_SHARE], panel.items[1].x + 1, panel.items[1].y + 1));
        /* the band follows the focus */
        panel.focus = SNX_OVERLAY_ERROR_SHARE;
        list.n = 0;
        overlay_error_draw(&list, SW, SH, &panel);
        for (i = 0; i < list.n; ++i)
            if (list.q[i].y == top + row_h * (float)(total - 4) && list.q[i].h == row_h && list.q[i].w == list.q[0].w)
                share_band = i;
        assert(share_band >= 0);
        i = glyph_at(3, total - 2, 'C');
        assert(i >= 0 && list.q[i].rgba == plain);
        i = glyph_at(3, total - 4, 'S');
        assert(i >= 0 && list.q[i].rgba == focused);
        /* what Share log did, over the items, and kept when the picture is short */
        panel.said = "Log saved to /home/player/.local/state/scarletnx/games/0100DA801624E000/scarletnx.log.";
        list.n = 0;
        overlay_error_draw(&list, SW, 260, &panel);
        overlay_place(SW, 260, &px, &py, &s);
        py = (int)list.q[0].y;
        assert(list.q[0].y >= 0 && list.q[0].y + list.q[0].h <= 260.0f);
        total = (int)(list.q[0].h / (16.0f * (float)s));
        for (i = 1, share_band = 0; i < total - 5; ++i) share_band |= text_at(1, i, "Log saved to");
        assert(share_band);
        list.n = 0;
        panel = panel_of("CAF\xC3\x89", "x", "y", NULL, NULL);
        overlay_error_draw(&list, SW, SH, &panel);
        overlay_place(SW, SH, &px, &py, &s);
        py = (int)list.q[0].y;
        assert(text_at(1, 1, "CAF\x90"));
        list.n = 0;
        panel =
            panel_of("N", "message",
                     "a b c d e f g h i j k l m n o p q r s t u v w x y z a b c d e f g h i j k l m n o p q r s t u "
                     "v w x y z a b c d e f g h i j k l m n o p q r s t u v w x y z",
                     NULL, NULL);
        overlay_error_draw(&list, SW, 200, &panel);
        assert(list.q[0].y >= 0 && list.q[0].y + list.q[0].h <= 200.0f);
    }
    /* Only visible log rows are a link. Wrapped paths remain one target, and a
       short screen that hides the path leaves no invisible click target. */
    {
        int i, underlines = 0;
        const char* path = "/home/player/.local/state/scarletnx/A game with a long name.log";
        SnxOverlayError panel = panel_of("GAME", "message", "The game's log:", path, NULL);
        SnxOverlayRect link;
        list.n = 0;
        overlay_error_draw(&list, SW, SH, &panel);
        link = panel.link;
        assert(link.w > 0 && link.h >= 16.0f * (float)s);
        /* right under its label, no blank row between */
        overlay_place(SW, SH, &px, &py, &s);
        py = (int)list.q[0].y;
        assert(text_at(1, (int)((link.y - (float)py) / (16.0f * (float)s)) - 1, "The game's log:"));
        assert(overlay_error_hit(&link, link.x, link.y));
        assert(overlay_error_hit(&link, link.x + link.w - 1, link.y + link.h - 1));
        assert(!overlay_error_hit(&link, link.x - 1, link.y));
        assert(!overlay_error_hit(&link, link.x + link.w, link.y));
        assert(!overlay_error_hit(&link, link.x, link.y + link.h));
        for (i = 0; i < list.n; ++i)
            if (list.q[i].x == link.x && list.q[i].h == (float)s && list.q[i].w > 0 && list.q[i].y >= link.y &&
                list.q[i].y < link.y + link.h)
                ++underlines;
        assert(underlines == (int)(link.h / (16.0f * (float)s)));
        list.n = 0;
        overlay_error_draw(&list, SW, 160, &panel);
        assert(!overlay_error_hit(&panel.link, panel.link.x, panel.link.y) && !panel.link.h);
        list.n = 0;
        panel = panel_of("GAME", "message", "Share log", NULL, NULL);
        overlay_error_draw(&list, SW, SH, &panel);
        assert(!panel.link.w && !panel.link.h && !overlay_error_hit(NULL, 0, 0));
        /* Share log's line takes room from the log's label and the hint first: on a picture the panel just fills with
           both, the whole path is still there and still a link */
        {
            const char* said =
                "Log saved to /home/player/.local/state/scarletnx/games/0100DA801624E000/scarletnx.log (path copied).";
            int whole;
            panel = panel_of("GAME", "Unsupported instruction ce62c820 at 801b67c",
                             "The game's log has what ran last:", path, NULL);
            list.n = 0;
            overlay_error_draw(&list, 1280, 720, &panel);
            overlay_place(1280, 720, &px, &py, &s);
            whole = (int)(panel.link.h / (16.0f * (float)s));
            for (int height = 720, cut = 0; height >= 200; height -= 4) {
                int label = 0, rows;
                panel.said = NULL; /* the log whole at this size, its columns and scale */
                list.n = 0;
                overlay_error_draw(&list, 1280, height, &panel);
                overlay_place(1280, height, &px, &py, &s);
                whole = (int)(panel.link.h / (16.0f * (float)s));
                panel.said = said;
                list.n = 0;
                overlay_error_draw(&list, 1280, height, &panel);
                py = (int)list.q[0].y;
                assert(list.q[0].y >= 0 && list.q[0].y + list.q[0].h <= (float)height);
                rows = (int)(panel.link.h / (16.0f * (float)s));
                for (int r = 0; r < 64; ++r) label |= text_at(1, r, "The game's log has");
                assert(rows == whole || !label);
                cut |= !label;
                if (height == 200) assert(cut); /* the sweep got to where rows are cut */
            }
        }
    }
    {
        char url[256];
        assert(fatal_log_url("/tmp/a b#c%?.log", url, sizeof url));
        assert(!strcmp(url, "file:///tmp/a%20b%23c%25%3F.log"));
        assert(fatal_log_url("C:\\games\\a b.log", url, sizeof url));
        assert(!strcmp(url, "file:///C:/games/a%20b.log"));
        assert(fatal_log_url("\\\\server\\share\\a.log", url, sizeof url));
        assert(!strcmp(url, "file://server/share/a.log"));
        assert(fatal_log_url("/tmp/caf\xC3\xA9\\x.log", url, sizeof url));
        assert(!strcmp(url, "file:///tmp/caf%C3%A9%5Cx.log"));
        assert(!fatal_log_url("relative.log", url, sizeof url) && !*url);
        assert(!fatal_log_url("https://example.org", url, sizeof url) && !*url);
        assert(!fatal_log_url("/tmp/a.log", url, 8) && !*url);
        assert(!fatal_log_url(NULL, url, sizeof url) && !*url);
        assert(!fatal_log_url("", url, sizeof url) && !*url);
        assert(!fatal_log_url("/tmp/a.log", NULL, 0));
    }
    puts("overlay: placing, pages, taps, keys, values, scrolling, look, the error's panel and the atlas passed");
    return 0;
}
