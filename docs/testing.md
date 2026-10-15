# Testing

Run checks appropriate to your change. Every suite's output belongs under ignored
`build/`; requested tests must report failure rather than silently skip prerequisites.

| Command | What it checks | Needs game files? |
| --- | --- | --- |
| `tools/test.py` | Setup failures, synthetic texture processing and display-mode tooling | No |
| `tools/format.py` | Consistent formatting of handwritten native code | No |
| `tools/test.py --compile-only` | Every native test binary this platform has compiles (macOS, Linux, Windows) | No |
| `tools/test.py --native` | Portable suites everywhere; runtime, adapters, offscreen Metal and UI tests on macOS | Partly: `game`-labeled suites need the image |
| `tools/test.py --mods` | Real loader/hooks/settings/native Options and replay contracts | Yes, plus translated archive |
| `tools/test.py --gameplay` | Menu navigation, mode cycling, selection, movement and clean exit | Yes, plus translated archive |

Native suites are CTest entries with labels: `nogame` runs everywhere and in CI,
`game` needs your installation, `gpu` needs a Metal device, `device` needs a
real Metal and audio device (the offline audio render is not what a hosted CI
runner produces), `mods` needs the translated archive and the entity snapshot `tools/test.py --mods` captures. Run
one directly with `.venv/bin/ctest --test-dir build/cmake/macos -L nogame` or `-R dx_tests`.
Two suites cover the on-screen controls, both `nogame`: `controls_tests` for the
layout model, geometry, routing, virtual pad, mapped binding, raster primitives
and editor (including the old keypad geometry as a regression oracle), and
`pad_tests` for the DirectInput joystick and the three XInput DLLs, which lives
beside `dx_tests` because it defines the `host_pad_*` callbacks strongly.
`dmusic_tests` (`nogame`) drives DirectMusic through its COM vtables on
synthetic segments, styles, DLS and SoundFont files that
`dx/tests/make_dmusic_fixtures.py` writes (`dmusic_fixtures_current` checks
the committed header is its output), and renders the synthesizer offline:
loading, playing, repeats, tempo to the sample, groove transitions at bar
lines, volume and fades, the reverb, the General MIDI bank and a concurrent
render thread. `dmusic_render` measures a game's own segments the same way.
`input_script_tests` (`nogame`) runs scripted input (`host/input_script.h`)
against a simulated game: the language and its errors, the closed-loop
pointer, the waits and the capped PNG dumps.
`win32_ui_tests` (`nogame`, beside `dx_tests`) drives the window system
through guest calls and guest threads: activation and focus sequences, sent
messages in order with queued input, per-thread queues, PostThreadMessage and
cross-thread SendMessage, keyboard translation, timers and WM_SETCURSOR, and
DirectInput's foreground and exclusive semantics, its 256-record buffer,
granularity and object info. `gdi_tests` pins GDI's text-measuring rules on a
generated font that carries VDMX and hdmx tables (`make_gdi_font.py`), and the
installed Windows faces where the machine has them.
`crt_tests` also replays a Windows 11 capture of the code page conversions
(`runtime/tests/data/codepages_windows.tsv`, written by
`tools/codepages/capture_win32.c` from `tools/codepages/vectors.txt`): every
byte and UTF-16 unit of the six single-byte tables and each edge case, return
value, last error and buffer contents included. `codepage_tables_current`
checks that `runtime/codepage_tables.h` is `tools/codepages/gen_tables.py`'s
output for that capture.
`gdi_metrics_reference` compares the runtime's text metrics with a Windows
96-dpi screen DC's once those are committed as
`runtime/tests/data/gdi_metrics_windows.jsonl`: capture them with
`tools/gdi_metrics/capture_win32.c` on Windows, and see
`tools/gdi_metrics/compare.py` for the tolerances.
Every `tools/test.py` mode takes `--game-dir /abs/path/to/<game>`; without it
the kit's stub game is used and the `game`-labelled suites report a skip.

Invoke these with `.venv/bin/python`. The native tests need a macOS Metal device;
CI compiles them but does not claim GPU or original-game execution. The mod suite
first builds and runs a deterministic 32-frame entity capture from your own game
in an isolated directory; no saved capture from another checkout is needed. The gameplay
runner uses an isolated profile and original textures unless a local pack exists.

For a Windows cross-build, set `LLVM_MINGW_ROOT`, then run
`tools/build.py --preset windows-cross --stub --target app` and
`tools/test.py --preset windows-cross-stub --compile-only`. CI verifies the
Indeo 5/AVI and Vorbis/Ogg components and uses Wine to load `dx_tests.exe` with
only its three copied FFmpeg DLLs. `RECOMP_TEST_AUDIO=/path/to/track.ogg` exercises
the CD-music decoder and requires nonzero PCM; CI generates its own sine tone.
`RECOMP_TEST_AVI=/path/to/movie.avi` exercises the guest AVIFile/Indeo imports,
requiring changing nonblack frames. `RECOMP_TEST_MILES=/dir` loads every `.wav` and
`.mp3` in a directory through the Miles calls a mixer plugin makes (load,
granularity, rate, ms length, play), checks each WAVE against its own header
and each MP3 against a second, cached load, and prints the census by format.
`RECOMP_TEST_BINK_AV=/path/to/movie.bik` plays a whole Bink movie through the
imports with the device consuming audio at the picture's rate, and fails on an
underrun or an audio track much shorter than the picture. Movie and sound
probes use private local inputs, never uploaded. These headless probes do not open an audio device or game window.

For instruction-translation changes, use the original differential harness:

```sh
.venv/bin/python tools/recomp/tests/test_translate.py --help
.venv/bin/python tools/recomp/tests/test_translate.py
.venv/bin/python -m pytest tools/recomp/tests/test_translate_hooks.py
```

The x87 has its own: `test_x87_conformance.py` translates every x87
operation and runs recorded hardware vectors through it, requiring every
non-transcendental result bit for bit (`x87_conformance.py --vectors V
--results R` takes a larger corpus; `tools/recomp/tests/data/x87/README.md`
has the format); `test_x87_fma.py` checks that no fused multiply-add reaches
x87 code (`tools/recomp/check_fma.py` scans any build's objects); and
`tools/recomp/x87_bench.py` times the arithmetic paths.

Five translator suites need no game and run in `tools/test.py`:
`test_translate_insns.py` runs synthetic listings of individual instruction
forms through the translator, compiles them with the test harness and
compares registers, flags and memory with Unicorn; `test_jumptables.py`
decodes the jump-table shapes on synthetic functions over a fake image;
`test_translate_driver.py` covers driver rules such as what a withdrawn block
leaves behind. `test_function_db.py` covers a function database as the
translator's authority and translating a module at another base;
`test_freestanding.py` compiles the instruction vocabulary with no C library,
links a translation into a bundle with `ld64.lld` and loads it into a stand-in
host (it needs an LLVM clang and lld, and says so when they are missing).
`tools/tests/test_toolchain.py` covers the
bundled first-run toolchain (`tools/toolchain/`) without building it; with
`RECOMP_STAGED_TOOLCHAIN=<dir>` it also runs a staged one from a read-only copy
and builds a bundle with it alone.
`test_translate_msvc_eh.py` covers MSVC exception frames: which frames get a
checkpoint, the funclets and continuations their tables name, and `_setjmp3`
call sites.
Add a case there first when the translator meets an instruction or table
shape it does not handle.

DirectDraw 7 and Direct3D 7 have two suites. `d3d7_tests` (`nogame`) drives the
shims through the guest ABI: the object model, enumeration, locks, Blt and Flip
on the CPU path and on the software reference renderer (which must agree
exactly), GetDC, device state, coherence, and synthetic scenes against pixels
computed by hand. `d3d7_gpu_tests` (`gpu`) renders the same scenes
(`dx/tests/d3d7_scenes.h`) on the GPU and on the reference renderer and compares
them channel by channel, each scene within its stated tolerance and edge
budget; `RECOMP_D3D7_TEST_DUMP=<dir>` writes every compared pair as PPM. It also
checks a software cursor over a flip chain and times a frame at an engine's
call rate. `host_tests` checks that a presented surface reaches the presenter.

`winsock_tests` (`nogame`) drives WSOCK32, WS2_32, the IP helper API and
Netbios through the guest ABI on real sockets on the loopback interface:
datagram and stream echo, non-blocking calls, select, WSAEventSelect's
re-signalling while datagrams are queued, a blocking call in one guest thread
while another runs, per-thread HOSTENTs, an offline host and a resolver that
runs off the baton; on fake interface lists, the adapter order and limits,
GetAdaptersInfo's layout and its agreement with the own host's HOSTENT, and
broadcast fan-out. Nothing leaves the machine unless
`RECOMP_NET_TEST_BROADCAST=1` asks for one real broadcast on the host's own
interfaces. `platform_tests` covers the `os_net_*` layer beneath it.

The native `eh_tests` suite (`nogame`) runs MSVC C++ and structured exception
handling (`runtime/msvc_eh.cpp`) over synthetic guest frames written the way
the translator emits them: catch matching and catch objects, unwinding,
rethrow, a throw from a catch block, a try block inside a catch block, a
throwing loop whose host stack must not grow, `__except`/`__finally`, a
divide error, `_setjmp3`/`longjmp` and `bad_cast`, plus the terminate and
abort paths in child processes. Like `seh_tests`, it spawns itself; where the
macOS crash reporter is unreliable, run native suites with
`DYLD_INSERT_LIBRARIES` naming a universal (arm64 and x86_64) library that
maps SIGABRT to `_exit(134)`.

The differential harness compares translated routines with original instructions
under Unicorn. Unicorn is a development tool, not part of the playable app.

## Manual gameplay checks

- Start a level, select a person, issue a move order and observe the destination.
- Compare Classic/Enhanced and Wide on/off in a window wider than an 800×600 canvas.
- Cycle resolution through 640×480, 800×600 and 3840×2160, then resume play.
- Change graphics/host settings, quit normally, relaunch and check persistence.
- Test Command-Q, focus loss/return and mouse motion at all four edges in each window mode.
- Play the intro long enough to expose streaming stalls; check music and effects in a level.
- Compare animation speed at 60 and 120 FPS. Capture frame-pacing data during real play.

Report macOS/device, selected resolution, window mode, rendering mode and frame
limit. State whether FPS counts submitted, completed or displayed frames. A pinned
smoke clock is deterministic test timing and cannot substantiate real-time performance.

## Current local evidence

The source snapshot includes fixes exercised through native Options, live
640×480 → 800×600 → 4K → 640×480 transitions, unit movement and clean exit.
The initial publication additionally runs the source-only CI checks and local
native suites built through CMake from the standalone checkout, and the Linux and
Windows portable-layer suites in CI. Long campaign completion, multiplayer
and sustained 4K120 remain unverified; publish measurements with their conditions.
