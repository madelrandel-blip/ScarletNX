<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
<!-- Copyright 2026 the ScarletNX contributors -->
# Usage

## Index

* [Extract → AOT → run](#extract--aot--run)
  * [NRO applications](#nro-applications)
* [GDB (UNIX only)](#gdb-unix-only)
* [Headless Vulkan / lavapipe](#headless-vulkan--lavapipe)
* [AppImage / display](#appimage--display)
* [Folders](#folders)
* [Users](#users)
* [Settings / input](#settings--input)
  * [On-screen controls](#on-screen-controls)

Setup: [BUILD](BUILD.md). Checkout CWD; own dumps/keys.
For the keyboard-controlled SDL3 menu and file drops, see [GUI](GUI.md).

## Extract → AOT → run

Pass a single XCI, NSP, DXCI or DNSP to extract, recompile and play:

```sh
./build/scarletnx "Game.xci"
```

The driver runs each step as a child process and starts the game with its folder, `games/<id>/`, as its working
directory. The game is named by its title ID, read from the dump's NCA headers before anything is extracted, an NRO by
its name (a dump without a title ID of its own by its stem); `games/` is the data folder's, from a build tree too
([Folders](#folders)). The dump is extracted straight into the game's folder, `games/<id>/<id>_data/` (`exefs/`,
`romfs/`, `control.nacp`), and the game made from it there: there is no second copy. `<id>.extracting` stays in the
game's folder while an extraction is unfinished, and a stopped one is redone on the next run, the game's saves kept.
`--repo DIR` and `--build-dir DIR` may precede the dump. Keys use the usual discovery paths below.
`--outdir DIR` puts the game's folder in DIR in place of the data folder's `games/`, as `-r` takes it. `--out DIR`
keeps the dump's extraction at DIR instead, as `-x` takes it, the game made from a copy of it; an NRO has no
extraction to name.
`--tmpfs` extracts to tmpfs instead: `<temp>/scarletnx/work/<title ID>` (`$TMPDIR`, else `/tmp`; the temporary
folder on Windows), where the game reads its RomFS from while its ExeFS and saves stay in `games/<id>/<id>_data/`.
The game's folder gets `<id>.source`, a link to the dump, in either mode, and `<id>.tmpfs` while its RomFS is in
tmpfs. A run extracts the RomFS again from the link when the system emptied tmpfs, and moves it between tmpfs and
the game's folder when `--tmpfs` is given or left out. Without the link (a game made before, or Windows without the right to
make one) it says *Can't be extracted to tmpfs due to missing symlink*. `-r --tmpfs` on an extraction leaves its
RomFS where it is.
`-H` (`--headless`) can precede or follow the dump and is passed to the game only.
Omitting it starts the game with a window.
`-x` means `--extract`, and `-r` means `--recomp` (the old `--recompile` spelling was removed).
On UNIX, short flags can be combined. Windows accepts separate short flags only.

To prepare a dump without playing, use `./build/scarletnx -r "Game.xci"`.
This extracts when necessary and forces recompilation, even if a cached executable exists.
`-rx` is equivalent on UNIX (`-r -x` on Windows). `-x` alone only extracts.

A game made already plays the same way, named by its folder or its program: `./build/scarletnx games/<id>` (or
`games/<id>/<id>`) runs it, recompiled first where it is when another build made it, from what its `<id>_data/`
keeps (the ExeFS or the NRO: no dump, keys or extraction). `-r` recompiles it without playing. `--outdir` and `--out`
name a dump's folders and are refused for it. `--game=games/<id>` runs it as it is, whichever build made it.
Pass arguments to the game executable after `--`, for example:

```sh
./build/scarletnx "Game.xci" -- some arg
```

The argument boundary is retained when launching the deployed game, so game arguments
cannot trigger launcher actions or overwrite runtime settings. This does not add a
homebrew-loader guest argv block.
`-f` (or `--fullscreen`, `--fullscreen=1`) can also precede or follow the dump and is forwarded to the game,
which then starts fullscreen (not in timed test runs). `--fullscreen=0` starts it windowed. Without either, the
game's `.ini` decides (`fullscreen=1` starts fullscreen).
`--touch=0|1|2` is forwarded to the game too: its touch screen takes nothing, the fingers, or the fingers and the
mouse's left button (the default). The game's menu sets it too; the game's `.ini` keeps its own `touch`.
`--controls=0|1` likewise hides or shows the [on-screen controls](#on-screen-controls) (on by default on Android);
the game's `.ini` keeps `controls`. Any other `--key=value` of the game's
`.ini` is forwarded as well, for that run (`scarletnx --settings` lists them all), and so are the timed-run ones
(`--seconds=`, `--shot=`...); any other key stops the run with `unknown option`.
The game's own executable also takes `-v`, the same as `--log=2`.
LLVM is the only backend. `--backend=llvm`, which older scripts give, is accepted and changes nothing, and any
other `--backend` stops with an error.
Use the separate commands for updates, custom keys or other compiler options.

DXCI and DNSP with decrypted content need no keys. Containers are identified by their headers,
including renamed files. A matching bundled update is applied automatically. An explicit `--update`
replaces the bundled update and must match the base title. DLC-only and update-only dumps cannot
be used as a base game.

```powershell
.\build\scarletnx.exe "C:\Games\Game.dxci"
.\build\scarletnx.exe --extract "C:\Games\Game.dnsp" --update "C:\Games\Update.dnsp"
```

Run the same command again to retry after a failure or interruption:

- `work/Game/` skips extraction. Automatic extraction publishes this directory only after success,
  using `work/Game.extracting/` while running. An incomplete staging directory is discarded on retry.
- A complete `games/<id>/<id>` (`<id>.exe` on Windows), code module and `<id>.abi` stamp naming this build
  skip recompilation. The stamp holds `SNX_ABI` and the build's commit (the window title's), so a game another
  build made, of another version or branch, is recompiled automatically, as is one with a missing or invalid
  stamp. Failed builds never publish a valid ABI checkpoint. Builds of a tree with uncommitted changes
  (`-dirty`) or from outside git (`unknown`) cannot tell each other apart: there `-r` recompiles.
- Changing directory and running the game happen on every launch. A game failure never invalidates the build.

Failures stop the workflow and print the failed stage and exit status or signal. On POSIX, stopping a
child also aborts that stage so it can be retried, except for intentional debugger stops. Uncatchable termination of the driver itself, such as
`SIGKILL`, cannot print a diagnostic. Do not run concurrent workflows for the same output name.
Existing manually extracted directories are assumed complete. Remove stale checkpoints or use the
explicit commands when changing a dump or rebuilding game code.

### NRO applications

Pass an uncompressed NRO directly to recompile and play, without keys or extraction:

```sh
./build/scarletnx "app.nro" -H
```

The `.nro` extension is case-insensitive. NRO launches always recompile and restage the input,
so a same-named cached executable or extraction directory cannot silently select an older program.
Embedded RomFS assets are staged by the recompiler. Compressed NROs are not supported.
This uses ScarletNX's existing guest runtime, not a complete homebrew-loader environment.
Compatibility still depends on the application's runtime and service requirements.
See [NRO implementation notes](NRO.md) for the reference and validation policy.

To run the steps separately, here in folders of the current one (`--out` and `--outdir`; without them, the data
folder's `work/` and `games/`):

```sh
./build/scarletnx -x "Game.xci" --out work/Game
./build/scarletnx -r work/Game Game --outdir games
games/Game/Game
```

```powershell
.\build\scarletnx.exe -x "Game.xci" --out work/Game
.\build\scarletnx.exe -r work/Game Game --outdir games
.\games\Game\Game.exe
```

Remaining examples: POSIX.

```sh
# NSP + same-title update
./build/scarletnx -x Game.nsp --update update.nsp --name Game \
  --keys /path/prod.keys --title-keys /path/title.keys

./build/scarletnx -r work/Game Game --outdir games --jobs 8
./build/scarletnx -r app.nro -o App --outdir games
./build/scarletnx -r -h
```

- **Keys:** encrypted content uses flags → the data folder ([Folders](#folders)) → `~/.switch`
  (`%USERPROFILE%\.switch` on Windows).
  Common NSP tickets supplement title keys. Fully decrypted content needs neither key file.
- **Extraction:** defaults to the data folder's `work/<dump stem>`; `--name`/`--out DIR` override. Refuses existing
  `exefs`. hactool runs inside the driver and reads each NCA where it lies in the XCI or NSP, so the ExeFS and RomFS
  are written straight into the output and nothing is unpacked beside it
  ([key filtering and cleanup](../src/nx/runtime/driver/extract.c)).
  Sparse and compressed NCAs, updates with large relocation tables and gamecard dumps that start with the card's key
  area all extract, through what [`storage.c`](../src/nx/runtime/driver/hactool/storage.c) ports from LibHac.
- **Recompile:** AOT → module (LLVM) →
  final runtime deployment, for this machine's CPU. Shaders compile directly to SPIR-V at runtime.
  `-O 0..3,s,z` sets the optimization level.
  `--target auto|linux|win64` takes only this host's own system: modules are built for the machine that builds
  them. `--runtime FILE` deploys that runtime instead of the running one, or of the one a tools-only build builds in
  its checkout's `build/native-recomp` (`build/win64-recomp` on Windows). `-q` (`--quiet`) prints no progress.
  `--keep-c` is the deprecated spelling of `--keep-ir`.
- **Codegen:** `--no-build` stages data/INI only; `--interp-only` omits AOT functions.
  `--chunk N` sets instructions per code unit. Nothing is cached between runs.
  `--keep-ir` saves the generated code (LLVM IR) in `<outdir>/<NAME>/<NAME>_build/`.
  `--hook-direct-calls` sends calls and branches between guest functions through the dispatch table too, so
  `SNX_NVN_TRACE` also counts calls made by direct branch. The game runs slower, so use it only for investigating.
- **Data updates:** ExeFS refreshes; `<id>_data/romfs` is reused.
  Replace cached RomFS after asset changes; preserve `<id>_data/save/`.

With `--out work/Game` and `--outdir games` as above (without them, see [Folders](#folders)):

| Output | Contents |
| --- | --- |
| `work/Game/{exefs,romfs}` | Extracted input |
| `games/<id>/<id>[.exe]`, `<id>_code.so/.dll` | Runtime + AOT module |
| `games/<id>/<id>_data/`, `<id>.ini` | Data/saves (and `control.nacp`) + settings |
| `games/<id>/<id>_build/` | Generated code, only with `--keep-ir` |
| `games/<id>/shader_cache/vk_pipelines.bin` | Vulkan pipeline cache |
| `games/<id>/shader_cache/<id>.vkshaders` | Translated shaders and the pipelines made, made again at the next start |

## GDB (UNIX only)

Install `gdbserver` on PATH. `-S` enables the native-code stub on loopback port **1234**
(the QEMU default port). The game runs until a debugger connects.
`-s` enables the stub and waits before runtime initialization. It implies `-S`.

```sh
./build/scarletnx -s "Game.xci"
# Or: games/<id>/<id> -Hs
```

In GDB, load the deployed executable's symbols and connect:

```gdb
file games/Game/Game
tar rem :1234
continue
```

The launcher prints a connection hint before waiting. A separate helper keeps accepting
connections while the game is stopped, then delegates the remote protocol to `gdbserver`.
The stub debugs host/native registers and code, not an emulated ARM64 CPU. It supports one
connection per run. Local ptrace permissions must allow attachment. Binding failures and
missing `gdbserver` are reported instead of silently running without a debugger.
No debugger flags or combined short flags are supported on Windows.

## Headless Vulkan / lavapipe

The normal SDL3 build can run without initializing SDL video or opening a window:

```sh
./build/scarletnx --game=games/Game -H --seconds=30 --shot=/tmp/frame
# Or extract, recompile and launch a dump without a window:
./build/scarletnx "Game.xci" -H
```

`-H` enables it. Omit the flag for the normal window. `--headless` is the long spelling
and accepts no value (`--headless=0` and `--headless=1` are errors).
This is a startup option, not a live toggle or an INI setting. SDL3 is required by every NX runtime
build, but headless runs do not initialize its video subsystem or require a display server.

Rendering still uses Vulkan 1.3, including uploads, draws, compute and screenshot readback. Headless runs
create no window, `VkSurfaceKHR` or swapchain and request no WSI extensions, including `VK_EXT_headless_surface`.
Frames still submit and recycle their resources. `--seconds`, `--frames`, `--script` and `--shot` work as before.
FFmpeg decoding is always included in the NX runtime, including headless builds, for video and hwopus audio.

To use software rendering, select Mesa's **lavapipe Vulkan ICD**, rather than the hardware driver. Mesa may
report this device as `llvmpipe` in the Vulkan log. Selecting a CPU ICD is independent of `-H`:

```sh
# Linux example. Substitute the actual lvp ICD manifest installed on your machine.
VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json \
  ./build/scarletnx --game=games/Game -H --seconds=30 --shot=/tmp/frame
```

Manifest names vary (for example `lvp_icd.json`). FreeBSD packages normally install manifests under
`/usr/local/share/vulkan/icd.d/`. Older Vulkan loaders use `VK_ICD_FILENAMES` instead of `VK_DRIVER_FILES`.
`LIBGL_ALWAYS_SOFTWARE` is an OpenGL setting and is not how this Vulkan renderer selects lavapipe.
No display server is required. Check the `vulkan:` device line in the log to confirm the chosen ICD.

The NX runtime includes SDL3 even when running headless; headless mode skips video initialization.

## AppImage / display

```sh
chmod +x ScarletNX-linux-x86_64.AppImage
./ScarletNX-linux-x86_64.AppImage --game=games/Game
SDL_VIDEO_DRIVER=x11 games/Game/Game
```

On an ARM64 machine the image is `ScarletNX-linux-aarch64.AppImage`.
`--game` selects the game's folder, its `Game_code.so` and `Game_data/`; or name the image `games/Game/Game.AppImage`
to load the files beside it.
The AppImage also prepares dumps: `./ScarletNX-linux-x86_64.AppImage Game.nsp`, in the folder [Folders](#folders) names,
and each game gets a copy of the AppImage (on Windows, of `scarletnx.exe` and the DLLs next to it).
X11 bypasses Wayland. [Licenses](CREDITS.md).

## Folders

The driver works in one folder, its data folder, the first of these that applies:

1. the one `--repo DIR` names (the menu names its own);
2. a portable copy's: `.scarletnx/` beside the AppImage, or beside the Windows zip's programs, when it is there;
3. the user's data folder: `~/.local/share/scarletnx` on Linux, `%APPDATA%\scarletnx` on Windows,
   `~/Library/Application Support/scarletnx` on macOS. On Android the app names its own, `.scarletnx/` in its own
   storage ([ANDROID](ANDROID.md)).

A driver run from its build tree works in the same folder as an installed one: the checkout it was built from is
not a data folder. `--repo DIR`, or `--outdir DIR` for a recompile, keeps a run's files in one folder of your choice.

Each game is a folder of `games/` named by its title ID, `games/0100DA801624E000/` and its files
`0100DA801624E000*` in it; a program without an ID of its own (an NRO, an ExeFS without NPDM) by its name, and
`--name` names any. The game is shown by the name in its `control.nacp` (the window, the recompile, the menu's
list). The user's data folder and a portable copy's keep each kind of file apart: on Linux the user's in the
[XDG](https://specifications.freedesktop.org/basedir/latest/) folder of its kind, elsewhere in its own `config/`,
`state/` and `cache/`. Any other folder `--repo` or `--outdir` names keeps everything in its games' folders.

| What | Another `--repo`, an `--outdir` | Linux, the user's data folder | A portable copy's, the user's elsewhere |
| --- | --- | --- | --- |
| a game: its program, `<id>_code.so`, `<id>_data/` (saves, `control.nacp`), `captures/` | `games/<id>/` | `~/.local/share/scarletnx/games/<id>/` | `games/<id>/` |
| its settings, `<id>.ini` | `games/<id>/` | `~/.config/scarletnx/games/<id>/` | `config/games/<id>/` |
| its `<id>.log` and `threads.txt` | `games/<id>/` | `~/.local/state/scarletnx/games/<id>/` | `state/games/<id>/` |
| its pipeline cache | `games/<id>/shader_cache/` | `~/.cache/scarletnx/games/<id>/shader_cache/` | `cache/games/<id>/shader_cache/` |
| extractions (`3ds/` for the 3DS driver) | `work/` | `~/.cache/scarletnx/work/` | `cache/work/` |
| the menu's themes, `<name>/theme.ini` | - | `~/.local/share/scarletnx/themes/` | `themes/` |
| the menu's `settings.ini`, `app.ini`, `folders.txt`, `gamecontrollerdb.txt` | - | `~/.config/scarletnx/` | `config/` |
| the menu's `recent.txt`, `last-run.log` | - | `~/.local/state/scarletnx/` | `state/` |
| the menu's Share log, `scarletnx.log` | - | `~/.local/state/scarletnx/` | `state/` |
| `prod.keys`, read before `~/.switch` | the folder | `~/.local/share/scarletnx/` | the folder |
| the console's users, `users.ini` and `<uid>.jpg` ([Users](#users)), and `online.ini`, an Eden room's password (`room`) | `users/` of the user's data folder | `~/.local/share/scarletnx/users/` | `users/` |

The menu works in the user's data folder or a portable copy's, unless its Settings > App > Folders picks a data
folder, a games folder or an extractions folder ([GUI](GUI.md)): it passes them to the driver as `--repo`, `--outdir`
and `--out` (an extractions folder picked keeps a Switch dump's extraction there, the game made from a copy; to the
3DS driver as `--repo` and `--output`), and its own files stay where they are. On Android they
are in its data folder's `config/` and `state/`, in the app's own storage, which needs no permission. `~/.local/share`, `~/.config`, `~/.local/state` and `~/.cache` are the
defaults of `$XDG_DATA_HOME`, `$XDG_CONFIG_HOME`, `$XDG_STATE_HOME` and `$XDG_CACHE_HOME` (a relative one is ignored).

An older version kept a game's files side by side in `games/` (`games/Game`, `Game_code.so`, `Game_data/`,
`Game.ini`), the menu's in the data folder's `gui/`, outside Linux the rest in the data folder itself, a portable
copy's in a `games/` beside the program, and a build tree's `games/`, `work/` and keys in its checkout. This one moves
none of it: as long as it is there, the menu lists each folder and where it goes now, the driver says so on each run,
and an older game runs where it is, its run saying where its folder goes.
[DEPRECATIONS](DEPRECATIONS.md) says how to move them.

## Users

The console's users are in `users/users.ini` of the data folder ([Folders](#folders)), every game's: up to 8, each a
section in the console's order. Without the file there is one, `Player`. User 1 is always that first user, by the uid it
has always had, so its saves stay its own; each other one needs a uid of 32 hex digits of its own (not zero) and a
nickname. A nickname is 4 to 20 letters, digits, spaces, `.`, `_` or `-`, the names Eden's rooms take; any other
stops the game and says why:

```ini
[user1]
nickname=Aogo
[user2]
uid=5f3c0d9e8a7b41c2963e0f1a2b3c4d5e
nickname=Xuji
```

`uid` (user 1's, if written, is `0123456789abcdeffedcba9876543210`) and `nickname` are the keys; any
other key, a section out of order or twice, or a line that is no key stops the game and says why. A user's picture is
`users/<uid>.jpg`, a JPEG of at most 128 KiB (256x256 as the console's), read when the game starts; without one it is
Wimsy. The game plays as the user its `.ini`'s `user` names (`--user=N` for one run): with one user, the game picks it
on its own; with more, the game's user selector gets that user. Each user's Account saves are `save/user.<uid>` in the
game's `<id>_data/`; the first user's that an older version left in `save/user` are played there until you move them
([DEPRECATIONS](DEPRECATIONS.md)). The game's menu (Home, F2) shows the user playing in a box at its head's right, their
nickname and picture, and the launcher the one its Settings' `user` names, where a click picks another
([GUI](GUI.md)); a users.ini that lists no users is said in the launcher's foot.

## Settings / input

`Game.ini`: help rewritten at startup, most settings hot-reload. `--key=value` overrides one run.
Restart for `docked`, `fullscreen`, `language`, `resolution`, `anisotropy`, `memory_gb`, `heap_max_mb`, `airplane_mode`,
`ldn`, `room`, `profile`, `jit_threads`.
`scarletnx --settings` prints each setting the launcher's Settings offer, one a line: its section, key, default,
type, range and help, tab-separated.

| Key | Default | Effect |
| --- | --- | --- |
| `docked` | `1` | `1` is TV mode, `0` handheld mode (1280x720) |
| `fullscreen` | `0` | `1` starts fullscreen, except in timed test runs (`--seconds`, `--frames`). `-f` or `--fullscreen=0` sets it for one run. F11 or Alt+Enter switches without changing it |
| `language` | `1` | The game's language, English (US). The `.ini` lists the others, `0` to `17`. One the runtime doesn't know is English (US) |
| `resolution` | `1` | Internal 3D scale, `0.25` to `4`: `1` native, `2` double, `0.75` and `0.5` faster |
| `shadows` | `1` | `0` skips shadow maps |
| `vsync` | `1` | Images are shown only on the display's refresh, so nothing tears. `0` doesn't wait for it: the newest image is shown on the next refresh where the display can do that, else at once (which may tear). A display that can do neither waits for the refresh as `1` does |
| `anisotropy` | `0` | Texture filtering as the game asks. `2` to `16` sharpens surfaces seen at an angle |
| `fps_limit` | `game` | The pace the game asks for, as on the console: 60 FPS, or 30 for a game made for 30. `0` is unlimited, and `10` to `1000` is that many FPS |
| `memory_gb` | `0` | Memory the game sees. Below `6` it is the console's 3.2 GB, `6` gives 4.8 GB and `8` gives 6.8 GB. Games that run out of memory on 3.2 GB, like Tears of the Kingdom, need `6` |
| `heap_max_mb` | `0` | The most heap the game can allocate, in MB. `0` is as much as the memory allows |
| `jit_threads` | `0` | Threads compiling the game code the recompiler missed, which a thread waits for when it reaches it. `0` uses every CPU the game may run on but two, at most 8. They run below the game's threads |
| `network` | `0` | `0` reports offline and lets the game reach only loopback, which is not a network sandbox. `1` uses the computer's connection, and the resolver blocks known console service domains. Local wireless works either way (`ldn`) |
| `airplane_mode` | `0` | `1` is the console in airplane mode: offline whatever `network` says, no local wireless, as the game hears it, and in no Eden room |
| `ldn` | `lan` | Local wireless. `lan` is over the LAN with ldn_mitm's protocol, with another ScarletNX, a Ryujinx or a console running ldn_mitm: with `network=1` the console is the computer's address, with `0` it is `127.0.0.N`, one per ScarletNX on the machine, so two of them play together through loopback. `room` is through the Eden room `room` names, with the Edens and ScarletNXs in it. `off` is local wireless disabled, as the game hears it. Another word stops the game |
| `room` | empty | The Eden room `ldn=room` joins at start: `host` or `host:port` (`24872` without one); empty with `ldn=room`, or with a space in it, stops the game. The console is there as the user playing, by its nickname ([Users](#users)), at the address the room gives it (`192.168.x.y`), which nifm gives the game too, and the game's datagrams go through the room. A room with a password takes it from `users/online.ini` ([Folders](#folders)), a file only you may read (`chmod 600`, not checked on Windows) that holds `room_password=...` alone: another key or a section stops the game. A room that refuses stops the game and says why |
| `user` | `1` | The console's user the game plays as, `1` to `8`, as [Users](#users) lists them. A number with no user stops the game |
| `touch` | `2` | The game's touch screen: `2` fingers and the mouse's left button, `1` fingers, `0` off |
| `controls` | `1` on Android, else `0` | The [on-screen controls](#on-screen-controls) shown |
| `controls_snap`, `controls_scale`, `controls_opacity`, `controls_rel_center`, `controls_dpad_slide`, `controls_haptics` | `0`, `50`, `100`, `1`, `1`, `1` | As [On-screen controls](#on-screen-controls) describes them. `controls_scale` runs from `0` to `100` for 50% to 150% |
| `gamepad_swap_ab` | `0` | `1` swaps a gamepad's A and B. `0` sends A, B, X and Y where the console's are |
| `gamepad_map` | none | The player's map over that, below |
| `gamepad_deadzone` | `20` | The sticks' dead zone, in percent of their travel (`0` to `90`) |
| `gamepad_rumble` | `100` | How strongly gamepads vibrate when the game asks, in percent. `0` is never |
| `gamepad_background` | `0` | `1` keeps gamepads playing while the window is not focused |
| `gamepad_joycon_pair` | `1` | `1` plays a left and right Joy-Con together, `0` separately. Restart the game after changing it |
| `gamepad_p<N>_device` (N from 1 to 8) | empty | Player N's controller: empty takes the next to connect, else its SDL GUID as the log names it, `/2` after it for the second of two alike |
| `gamepad_p<N>_connected` | `1` | `0`: the game never sees player N. It always sees player 1, which the keyboard drives |
| `gamepad_p<N>_type` | `0` | The controller the game sees for player N: `0` as it is, `1` Pro Controller, `2` two Joy-Cons, `3` a left Joy-Con, `4` a right one |
| `gamepad_p<N>_swap_ab`, `gamepad_p<N>_rumble`, `gamepad_p<N>_map` | `-1`, `-10`, empty | Player N's own `gamepad_swap_ab`, `gamepad_rumble` and `gamepad_map`; at these values, every player's |
| `log` | `1` | Log detail. `2` logs everything, which adds runtime overhead |
| `auto_stub_ipc` | `1` | A service command the runtime doesn't know answers success, logged as `[undeclared]`, and a service it doesn't have answers every command that way. `0` answers them with the console's UnknownCommandId instead, which shows the gap where the game makes the call |
| `diag` | `0` | `1` saves the data of each new compute shader to `captures/compute`, and each new shader's snx-shaderc corpus entry to `captures/shaders`, for debugging |
| `profile` | `0` | `1` writes `profile.txt` every 10 seconds. The CPU profiler is Windows only |

A gamepad's motion sensors, a Joy-Con's among them, go to the game when SDL exposes them. No setting turns them off.

`audout` PCM playback and the audio renderer's device sink (`audren`) use SDL3's system-default output on
Windows, Linux and supported Unix systems. Choose the output in your operating system's sound settings. The
renderer does not render splitters, voice biquad filters, or the delay, reverb and biquad effects yet.

SDL gamepads send A, B, X and Y where the console's are (A right, B bottom, X top, Y left), as emulators do;
`gamepad_swap_ab=1` swaps A and B. `gamepad_map` sets what any control sends, a control by where it sits and the
button it sends, as `south:B east:A lt:ZL`; the ones it leaves out send the button in their place. The sticks'
dead zone in `gamepad_deadzone` and the game's vibration as strong as `gamepad_rumble` (both in percent), and with
`gamepad_background=1` still play while the window is not focused; the game's menu (Home, F2) sets them under
*Controllers*, which names the pads connected; its *Player* row shows every player's settings (*All*) or one
player's own Swap A/B, Vibration and Button map. Each connected gamepad uses its own player slot, up to eight.
Keyboard and on-screen controls also drive player one.
`gamepad_joycon_pair=1` makes SDL combine a left and right Joy-Con. Set it to `0` to expose them separately and
restart the game after changing it.

For a joystick that SDL detects but does not recognize as a gamepad, put SDL mapping lines in
`gamecontrollerdb.txt` beside the launcher's `settings.ini` ([Folders](#folders)). A game in the launcher's
`games/` reads the same file; any other game, the user's (on Android every game, the launcher's). To use a file
elsewhere, set
`SDL_GAMECONTROLLERCONFIG_FILE` to its path before starting the launcher or game. A mapping line starts with the
joystick GUID shown in the game log or the launcher's Controllers page, which names the file too, followed by a name
and SDL button and axis bindings. For example:

```text
GUID,My Controller,a:b0,b:b1,x:b2,y:b3,leftx:a0,lefty:a1,rightx:a2,righty:a3,platform:Linux,
```

Replace `GUID` and each binding with values for the device, and `Linux` with `Windows`, `FreeBSD` or `Android`
where it runs: SDL takes a line of the file only with its own platform. This SDL mapping selects physical joystick
controls; `gamepad_map` instead selects which Switch buttons the mapped controls send. The game log reports every
detected joystick, whether it has an SDL mapping, and any gamepad open error. On Linux, installing
`dist/72-scarletnx-input.rules` in `/etc/udev/rules.d/` grants desktop seat access to joystick event nodes and to
the hidraw nodes of the console's controllers, for Joy-Con features. Reload udev rules and reconnect the controller
after installation. Other HIDAPI controllers may need their vendor's own hidraw rule.

| Keyboard | Switch |
| --- | --- |
| Arrows; WASD / IJKL | D-pad; L/R sticks |
| X or Space / Z / C / V | A / B / X / Y |
| Q / E / 1 / 3 | L / R / ZL / ZR |
| Enter / Backspace | Plus / Minus |

F1 help; F2 the game's menu; F3 screenshot; F4 FPS limit off and on; F5 thread dump; F11/Alt+Enter fullscreen;
Escape quit. A window shows at most one image per refresh and drops the rest unless images tear, and the game keeps
the pace `fps_limit` sets.

In the game's menu the arrows, Enter and Escape move, select and go back; a gamepad's D-pad or left stick, A and
B do the same, the on-screen controls' too, and a direction held goes on after a moment.

A game that stops on an error shows a panel with **Share log**, **Copy path** and **Close**. Share log saves
`scarletnx.log` (the build, the error and the game's log) beside the game's log, as the menu's does, and on Android
sends it to the app you pick; Copy path copies that file's path once it is saved, else the game's log's. The panel says
what each did. Up and Down move between the items, Enter or A does the one in focus, and any other key or button
closes it. On a desktop the game's log path is
underlined once the log exists: a click opens it in the system's viewer, the panel staying open.

### On-screen controls

A pad drawn over the game, with rEden's buttons and sizes, laid out as the console's two controllers hold them:
sticks, D-pad, A B X Y, L R ZL ZR, plus and minus, the stick buttons, Home (the game's menu) and Capture (a
screenshot). Fingers press them, the mouse's left button too; a finger that lands off them goes to the game's
touch screen. They are on by default on Android and off elsewhere.

The game's menu (Back on Android, Home on a gamepad, F2) has their page: show them, *Edit layout* (drag a control;
tap one and size it with the bar on top; *Done* or Back ends it), snap to a grid, scale and opacity, the buttons
shown, a stick's middle where the finger lands, a D-pad a finger slides on, a buzz on each press (Android), the touch
screen, and *Reset layout*. The game's `.ini` keeps them: `controls`, `controls_scale`, `controls_opacity`,
`controls_snap`, `controls_rel_center`, `controls_dpad_slide`, `controls_haptics`, a control's place once it
was moved (`controls_a=0.930 0.470 100`: its middle across the screen and its size in percent), and whether it is
shown once that changed (`controls_a_shown=0`). The launcher's Settings have the same, but the layout.
Beside the executable: `Game.log`, `threads.txt` (15 s refresh), `captures/` (screenshots); an installed copy on Linux
keeps the first two in its state folder ([Folders](#folders)).
Scripts/disasm/shaders: [DEVELOPMENT](DEVELOPMENT.md).
