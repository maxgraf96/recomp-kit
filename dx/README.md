# `dx` — DirectDraw, Direct3D, DirectSound, DirectInput, QMixer, weanetr

The graphics, audio and input side of the static recompilation. Every file
here implements a published API contract and nothing about the game: a shim
answers the way the real DLL would, and where it cannot, it says so once in
the log rather than inventing an answer.

Nothing in this directory draws, plays or opens anything. Rendering and audio
leave through the callbacks in `host_api.h`, which the native host implements with
Metal and AVAudioEngine. That keeps every test here headless.

## Files

| file | contents |
| --- | --- |
| `dxtypes.h` | DirectX 6 constants and structure field offsets |
| `com.h/.cpp` | guest COM objects, vtables, the three IUnknown slots |
| `host_api.h/.cpp` | the callbacks into the host app, with weak no-op defaults |
| `dx.h/.cpp` | `dx_register_shims()`, `dx_reset()`, the audio channel allocator |
| `ddraw.cpp` | IDirectDraw/2/4, IDirectDrawSurface/2/3/4, palette, clipper |
| `d3d.cpp` | IDirect3D/2, device, viewport, material, light, texture |
| `ddraw7.cpp`, `d3d7.cpp` | IDirectDraw7, IDirectDrawSurface7, IDirect3D7, IDirect3DDevice7, IDirect3DVertexBuffer7 |
| `d3d7_host.h` | the seam between those and a renderer: surfaces, Blt, Flip and draws |
| `d3d7_soft.cpp` | the software reference implementation of that seam |
| `d3d7_types.h`, `d3d7_internal.h`, `d3d7_prim.h` | DirectX 7 layouts and constants; the surface model; topologies and vertex formats |
| `dsound.cpp` | IDirectSound, buffer, 3D buffer, 3D listener, notify |
| `dmusic.cpp`, `dmusic.h` | DirectMusic (DirectX 6.1/7) over GothicKit/dmusic: loader, performance, IDirectMusic, the synthesizer port and its IKsControl, segment, segment state, style, composer |
| `dmusic_reverb.cpp` | the synthesizer's Waves reverb, approximated |
| `mss32.cpp` | Miles digital drivers, preferences, PCM/MP3 samples (MP3 decoded once per image), EOS callbacks and streams |
| `redbook.cpp` | Miles CD audio backed by configured media files |
| `waveout.cpp` | WinMM PCM/IMA ADPCM queues and device-clock WAVEHDR completion |
| `avi.cpp` | Read-only AVIFile streams and Indeo 5 Video for Windows decoding |
| `dshow.cpp` | DirectShow multimedia streaming, the reading side: IAMMultiMediaStream over an MP3 file, IAudioMediaStream, AMAudioData, IAudioStreamSample (minimp3) |
| `dinput.cpp` | IDirectInputA, mouse and keyboard devices |
| `dinput_joystick.cpp` | the virtual pad as a DirectInput joystick, when a game asks for native pad input |
| `qmixer.cpp` | the 28 QMixer `QSWaveMix*` exports |
| `weanetr.cpp` | the 17 MLDPlay methods, reporting no networking |
| `tests/dx_tests.cpp` | the headless tests |
| `tests/pad_tests.cpp` | the virtual pad through DirectInput, with its own host pad callbacks |
| `tests/d3d7_tests.cpp`, `tests/d3d7_scenes.h`, `tests/d3d7_client.h` | DirectDraw 7 / Direct3D 7, headless; the scenes are shared with `host/tests/d3d7_gpu_tests.cpp` |
| `tests/dmusic_tests.cpp`, `tests/dmusic_client.h`, `tests/make_dmusic_fixtures.py` | DirectMusic on synthetic segments, styles, DLS and SoundFont files, rendered offline |
| `tests/dmusic_render.cpp` | a game's segment rendered offline through the same calls, its level printed as JSON |

## Wiring it up

`imports_init()` runs from `loader_load()` and installs logging-only entries
for these DLLs. `dx_register_shims()` replaces them, so the host must call it
**after** `loader_load()`:

```c
loader_load(...);        /* runs imports_init(), patches the IAT */
dx_register_shims();     /* upgrades the DirectX entries in place */
```

`imports_register` upgrades an already-allocated trampoline in place, so the
addresses the loader wrote into the IAT stay valid and no runtime file needs
to change. Registering twice is a no-op.

After `mem_init()` — which discards the guest heap the COM objects and
vtables live in — call `dx_reset()` before using any of this again.

## How a COM object works

A guest interface pointer is a 16-byte header on the runtime heap:

| offset | field |
| --- | --- |
| 0 | `lpVtbl`, the guest address of the interface's vtable |
| 4 | `COM_MAGIC`, so a stray pointer is caught rather than followed |
| 8 | index of the host-side `ComObj` |
| 12 | which `ComIface` this view is |

The vtable is a second guest allocation holding one import trampoline per
slot, from `imports_alloc_trampoline`. A guest `call [vtbl + 4*n]` therefore
lands in `imports_dispatch`, which runs the slot's shim and then pops the
return address plus `4*argc` exactly as the real `ret n` would. **A shim never
touches ESP**, matching the contract in `runtime/imports.h`.

One host object can be reached through several interface views. An
`IDirectDraw2` and the `IDirectDraw4` a `QueryInterface` returns are two guest
pointers with two vtables and one shared refcount, as COM requires. Views are
allocated on first use and cached, so asking twice returns the same pointer.

Three relationships cross object boundaries and are handled by a per-kind
`QueryInterface` hook: a DirectDraw object asked for `IID_IDirect3D2` returns
the Direct3D object it owns; a surface asked for `IID_IDirect3DTexture2`
returns itself; a sound buffer asked for the 3D or notify interfaces returns
itself.

**Slot order is the entire contract.** Each vtable is written out in full,
including the slots that are stubs, because an interface that stopped early
would send the guest to the wrong method rather than to a missing one. The
tests assert the slot count and that no two slots share a trampoline.

## What the game actually uses

Cross-referencing the EXE's `.text` against the DirectX IID table (all 42 IIDs
are present in `.rdata` because `dxguid.lib` is linked, so presence proves
nothing; only a code reference does) gives the real inventory:

| requested from code | where |
| --- | --- |
| `IID_IDirectDraw2`, `IID_IDirectDraw4` | `0x4b03a7`, `0x520e54` |
| `IID_IDirectDrawSurface2`, `IID_IDirectDrawSurface4` | 8 and 6 sites |
| `IID_IDirect3D2`, `IID_IDirect3DHALDevice` | `0x42f020`, `0x42f03d` |
| `IID_IDirect3DMaterial2` | 3 sites |
| `GUID_SysMouse`, `GUID_SysKeyboard` | `0x52cdc7`, `0x52c907` |
| `IID_IDirectSound3DBuffer`, `IID_IDirectSound3DListener`, `IID_IDirectSoundNotify` | 4 sites |

So this build renders through Direct3D with a HAL device; there is no
software-rasterizer path to fall back on. `IID_IDirectDraw`,
`IID_IDirectDrawSurface`, `IID_IDirectDrawPalette` and `IID_IDirectDrawClipper`
are never requested by IID, but the version 1 interfaces are still implemented
in full because `DirectDrawCreate` returns one and `CreateSurface` hands one
back.

Vtable slot offsets were checked against real call sites in the Ghidra
decompilation rather than taken on trust: `IDirect3D2::FindDevice` at `+0x1c`
and `CreateDevice` at `+0x20` in `init_d3d`, `IDirectDraw2::CreatePalette` at
`+0x14` and `IDirect3DDevice2::EnumTextureFormats` at `+0x24` in
`init_d3d_hw_card`, `SetRenderState` at `+0x5c` in `set_render_states`,
`IDirect3DViewport2::SetViewport2` at `+0x44`, and
`IDirectDraw::SetCooperativeLevel` at `+0x50`.

## Behaviour worth knowing before you debug something

- **Surface pixels are guest memory.** A `Lock` hands back a real guest
  address, the guest writes through it, and the memory comparison fixture can compare
  the result. Pitch is the row width rounded up to 16 bytes, as a real driver
  aligns it.
- **`Flip` swaps the pixel pointers** of the front and back surface rather
  than copying. That is what retargeting the scan-out does, and it is what the
  guest's next `Lock` of either surface has to see.
- **Presentation happens on `Flip`, on `Unlock` of the primary, on a `Blt` or
  `BltFast` whose destination is the primary, on `ReleaseDC`, and on
  `SetPalette` or `SetEntries` affecting the visible surface.** The last one
  matters: attaching or editing a palette changes what is on screen without
  moving a pixel.
- **A back buffer never presents.** Only the visible surface does; a back
  buffer becomes visible by being flipped, not by being drawn into.
- **`GetDeviceIdentifier` reports vendor and device 0 on purpose.** The game
  matches these against a list of 1998 chipsets (Riva 128, Riva TNT, Rage Pro,
  Voodoo2, G200, Permedia2, Banshee) and takes a per-card workaround path on a
  hit. Zero is "unrecognised", which is the generic path and the only one this
  shim can honestly claim to implement.
- **The Direct3D device advertises the full documented blend, compare and
  texture-blend cap sets.** This is not padding: `init_d3d` rejects a device
  outright unless specific `dpcTriCaps` blend and alpha-compare bits are set.
  Advertising a superset is a promise the native renderer has to keep.
- **Direct3D records, it does not rasterize.** Every draw becomes one
  `host_d3d_draw` carrying the vertices, the render-state snapshot, the active
  transforms, the viewport and the texture handle. The immediate-mode
  `Begin`/`Vertex`/`Index`/`End` path accumulates into guest memory and emits
  the same single command, so the host sees one shape of input.
- **`GetDC` hands out a unique HDC but GDI cannot draw into surface memory**
  here. Anything rendered through that DC is lost. It logs once.
- **DirectInput deltas are consumed on read.** `host_input_state` clears them,
  so `poll_device` is the only reader; a second read in the same call would
  lose that frame's motion.
- **Nothing in the game drives the mixer, so the shims are driven from the
  message loop.** `QSWaveMixPump` is called zero times in a whole run: real
  QMixer runs its own mixing thread and that export is for an application that
  wants to pump it by hand. Everything that refilled a streamed wave used to
  hang off it, so nothing ever refilled one, and a streamed wave played the
  chunks it was given at the start and then stopped - which is a menu that
  plays a second of music and goes quiet. `dx_register_shims` now registers
  `qmixer_frame_pump` with `host_set_frame_pump`, and the guest's own message
  loop runs it between frames. The loop is `PeekMessageA` and only that; the
  game never calls `GetMessageA`, so anything hung off that one never runs
  either. DirectSound's notification service rides along on the same tick.
- **QMixer's `PauseChannel` stops rather than pauses**, because there is no
  host pause; `RestartChannel` replays from the start. It logs once.
- **A looping buffer the guest writes into is a stream, not a loop.** This is
  how the game plays music and speech, and the Wine trace of the original
  settles every detail of it. `0057e1e0` creates a 32768-byte buffer at
  22050 Hz stereo 16-bit with `GETCURRENTPOSITION2 | CTRLVOLUME | CTRLPAN |
  CTRLFREQUENCY`, reads its length back out of `GetCaps`, and calls
  `Play(0, 0, DSBPLAY_LOOPING)` while it is still empty. A thread then polls
  `GetCurrentPosition` every ten milliseconds and, every forty, locks three or
  four kilobytes at its own running offset - 0, 3584, 7056, 10640, wrapping at
  32768 - staying about a lap ahead of the play cursor. There are no
  notification positions anywhere on that path.

  So the first lap really is silence, in the original as much as here. What
  must not happen is re-submitting the ring on each `Unlock`, which restarts
  the voice twenty-five times a second. Ring order from the play cursor is
  play order, so the first write converts the buffer: the rest of the current
  lap is re-issued as a one-shot and everything written afterwards is appended
  with `host_audio_queue`. The cursor the guest polls is then derived from what
  the host has left to play, which is what paces the guest's own writer. A
  looping buffer nobody writes into stays a loop.

  `host_audio_stream` does the conversion at the cursor: it stops the voice
  once and reschedules immediately from where it had reached, so nothing is
  repeated and nothing is skipped, but there is a scheduling seam of a
  millisecond or two. That is deliberate. Handing over at the loop point
  instead would avoid the seam and cost up to a whole extra lap of the ring
  first - 370 ms at 32 KB and 22050 Hz stereo - on a buffer holding silence at
  that instant. It happens once per sound; everything after it is appended and
  gapless. It reports the
  offset it resumed from, and `host_audio_played_bytes` counts on from there at
  the sample rate and never goes backwards, across a re-schedule, across a
  refill and across an underrun. Reducing that modulo the ring is the whole of
  the play cursor.

  It has to be a count of what has been played and not of what is in flight.
  The guest keeps about a lap of audio in flight on purpose, so a cursor
  derived from the outstanding bytes sits a fixed distance ahead of the guest's
  own write offset - and that distance is exactly what the refill gate measures
  (`0057df10` returns the bytes from the write offset forward to the cursor,
  `0057dc80` compares that to the next chunk's length). Freeze it and the guest
  stops writing, which stops the stream, which freezes it further: the movie
  waits on a number that will not move, with nothing to show that anything is
  wrong.

  A host that answers -1 to `host_audio_stream` cannot continue a sound, and
  the ring stays a loop that is re-submitted at every refill. It says so once.
- **DirectSound notification positions are signalled.** The game has a second
  streaming path that uses them - `0056ec50` builds a worker thread and two
  events, `0056efe0` registers them at offset 0 and the half-way point of a
  `0x7800` buffer and calls `Play`, `0056eb70` parks the worker in
  `WaitForMultipleObjects` on them - and it does not appear in the traced run,
  but recording positions without signalling them would hang that worker for
  good. `dsound_pump()` compares the host play cursor against the registered
  offsets and signals what it has passed; `QSWaveMixPump` calls it because the
  game calls that every frame, and every DirectSound entry point calls it too.
- **A streamed wave is refilled through `host_audio_queue` where the host has
  it, and re-submitted where it does not.** Re-submitting restarts the voice,
  so the join is audible; it logs once when it falls back. The chunk the guest
  callback returns zero on is still played, because zero means "this was the
  last one", not "this one is bad" (`0056f090`).
- **weanetr reports failure so networking is unavailable.** The polarity is
  not a guess: at `0x413f90` the game frees its MLDPlay object when
  `StartupNetwork` returns zero, so zero is failure across MLDPlay.
  `GetCurrentMs` is the exception and returns the real clock, because the game
  calls it from dozens of places that have nothing to do with networking.
- **Miles answers as Miles 6.1a does, not as its manual reads.** The rules
  below were read from the library itself. `AIL_startup` installs the whole
  preference table, and a mixer that divides by `DIG_DS_FRAGMENT_SIZE` (8 ms)
  before setting it gets a divisor. `AIL_start_sample` always begins at the
  top: a position set before it is lost, so a "random start offset" a game
  sets ahead of the start plays from 0 on Windows too. The loop count counts
  down per pass. Pan is a balance: the centre is full on both sides and the
  far side fades linearly, so it converts to DirectSound's one-sided
  attenuation rather than being swept in millibels. A compressed sample's
  positions are its image's bytes and its granularity is 1.
- **Every audio library shares one IDirectSound.** `dsound_shared_device()`
  is what `AIL_get_DirectSound_info` reports, and so what a game passes on to
  DirectMusic and Bink. The runtime holds its creation reference; the
  borrowers' AddRef/Release pairs balance around it. Nothing plays through
  it: each library reaches the host mixer directly.
- **Miles' end-of-sample callback runs on a guest thread holding the baton**:
  the frame pump or `AIL_serve` for a natural end, and `AIL_end_sample`
  itself, synchronously. Never from `AIL_sample_status`.

## DirectDraw 7 and Direct3D 7

A separate object model from the version 1-6 shims, for a game written against
DirectX 7 (and so for its T&L HAL device as well as its HAL device). A game
opts in with `[runtime] ddraw7 = true` in its game.toml (`RECOMP_DDRAW7=0/1`
overrides): `DirectDrawCreateEx(IID_IDirectDraw7)` is otherwise refused, which
is what a game that only probes for version 7 and falls back expects.

**Surfaces live on the renderer first.** `ddraw7.cpp` and `d3d7.cpp` own the
guest objects and every rule the API states; pixels go through `d3d7_host.h`
to a backend - the GPU renderer (`host/d3d7_render.cpp`) when the host has
one, else `d3d7_soft.cpp` when `RECOMP_D3D7_SOFTWARE=1`, else none. Each
surface (each mip level) records whether its guest memory or its backend
copy is current, and pixels move only when the other side asks:

| what the guest does | what moves |
| --- | --- |
| `Lock`, `GetDC` | the backend copy is read back if it is the newer one |
| `Unlock`, `ReleaseDC` | nothing; the backend copy is now stale |
| draw with a texture, `Blt` from a surface | the CPU copy is uploaded if it is the newer one |
| draw, `Clear`, `Blt` into a surface | nothing; the CPU copy is now stale |
| `Blt` between two video-memory surfaces | nothing: the copy runs on the backend |

Without a backend every operation is done on the CPU and draws are dropped,
which is how the headless tests drive DirectDraw.

**Flip copies.** `Flip` copies the back buffer into the front buffer and
presents the front, so the back buffer keeps its contents: a menu drawn once
and then only patched by a software cursor stays on screen. It waits for the
display's refresh unless `DDFLIP_NOVSYNC` (`RECOMP_D3D7_VSYNC=0` turns the
wait off). A `Blt` to the primary presents its destination rectangle, which
is the windowed present.

**Direct3D records state and submits.** Each draw becomes one `HostD7Draw`:
the guest's vertices (any FVF, `XYZRHW` included), its indices, and a
snapshot of the device state. Transform, lighting (8 point, spot or
directional lights, material sources, `NORMALIZENORMALS`, local viewer),
vertex and table fog, the stage-0 combiner, alpha test, blending, depth and
culling are the backend's. `d3d7_soft.cpp` implements them from the DirectX 7
equations, in double precision, at Direct3D 7's integer pixel centres with the
top-left rule: it is the reference the GPU renderer is tested against.

| switch | effect |
| --- | --- |
| `RECOMP_D3D7_MODES=WxHxB,...` | the modes `EnumDisplayModes` offers (16 and 32 bpp) |
| `RECOMP_D3D7_SOFTWARE=1` | render with the reference renderer, no GPU |
| `RECOMP_D3D7_VSYNC=0` | `Flip` does not wait for the refresh |
| `RECOMP_D3D7_DUMP_DIR=dir`, `RECOMP_D3D7_DUMP_EVERY=N` | every Nth presented frame as `dir/frame-NNNNNN.ppm` |

Not implemented, each logged once when a game asks: texture stages above 0
(stage 1 may only be disabled), stencil, state blocks, `ProcessVertices`,
strided draws, user clip planes, vertex blending, texture transforms, fill
modes other than solid, `LASTPIXEL` (lines omit their last pixel), border
colours other than transparent black, destination colour keys, palettes,
clippers, overlays and FOURCC surfaces.

## DirectMusic

For a game that plays DirectMusic segments: `CoCreateInstance` makes the
loader, the composer and the performance (and `IDirectMusic`) when the game
opts in with `[runtime] dmusic = true` (`RECOMP_DMUSIC=0/1` overrides);
otherwise they are not registered and DirectMusic looks absent. The music
itself - segments, styles, bands and DLS collections read, patterns chosen by
groove level, transitions, the synthesizer - is GothicKit/dmusic, vendored
with its patches in `third_party/dmusic` (its README lists them).

**The calls.** Every DirectX 6.1/7 slot is in place; what DirectMusic does
that dmusic cannot is logged once when a game asks for it. What works:

| interface | |
| --- | --- |
| `IDirectMusicLoader` | search directories (a missing one is `DMUS_E_LOADER_BADPATH`); `GetObject` of a segment by file name, full path or from memory; the cache, on by default as in DirectMusic. Names go through the runtime's file resolver, so case does not matter. |
| `IDirectMusicPerformance` | `Init` (holding the `IDirectSound` it is given, or given none the one the audio libraries share, `dsound_shared_device`), ports and PChannels, `PlaySegment`/`Stop` at the boundary the flags (or `DMUS_SEGF_DEFAULT` and the segment's resolution) name, `GetSegmentState`/`IsPlaying`, `GetTime`, the global parameters (auto-download, master groove level, master volume; every one kept for `GetGlobalParam`), `AllocPMsg`/`FreePMsg`, `CloseDown`. Notifications are accepted and never signalled. |
| `IDirectMusic`, `IDirectMusicPort`, `IKsControl` | one software synthesizer port: output class, DLS, 1000 channel groups; `GUID_DMUS_PROP_Volume`, `_Effects` and `_WavesReverb` get and set. |
| `IDirectMusicSegment`, `...SegmentState`, `...Style` | repeats, length, resolution, `GUID_Download`/`GUID_Unload`, the style (which has no motifs); the state's segment is the loader's own pointer. |
| `IDirectMusicComposer` | `AutoTransition` (groove, fill, intro, break, end) at the next measure, beat, grid or at once. |

**When a segment ends.** `Stop` ends the primary segment for the state
queries at once, whatever the audio thread has still to render, so a game that
retries an unload while its score "still plays" gets through on the first try.
A segment that runs out of repeats ends when the render reaches its end; with
no audio output nothing renders and a playing segment plays until stopped.

**The sound.** The synthesizer renders on the host's audio thread through
`host_synth_attach`, at the host's rate (`host_synth_rate`, resampled if the
host's rate changes later), and the output stage does what the port does on
Windows: the KS volume and the master volume in hundredths of a dB, ramped
over about 5 ms so a game's stepped fade is a fade, a trim of -3 dB to the
level of Windows' synthesizer (measured on one game's music against a loopback
recording), and the Waves reverb (`dmusic_reverb.h`: a Freeverb-shaped network
whose decay times are the ones asked for; its level against Windows' is still
to be calibrated).

**General MIDI instruments.** A band takes them from `gm.dls`/`gs.dls`, which
Windows supplied. A game folder that has one is used; else the file
`RECOMP_DMUSIC_GM` names (a DLS collection or a SoundFont; a name that is not
there means none); else macOS's own Roland GS collection,
`/System/Library/Components/CoreAudio.component/Contents/Resources/gs_instruments.dls`,
read from the player's system and never copied; else the kit's GeneralUser GS
SoundFont, which dmusic plays in place of a DLS collection. With none, those
instruments are silent and the rest plays.

**Approximations.** `GetTime`'s music time runs at 120 beats a minute from
`Init`; start times passed to `PlaySegment`/`Stop` are not honoured; a queued
segment starts at its boundary rather than after the playing one; secondary
segments, motifs, chord maps, tracks and tools do not play; the synthesizer is
TinySoundFont (no 32-voice limit, its own envelopes) at the host's rate rather
than Microsoft's DLS1 synthesizer at 22.05 kHz. A band's first notes build
that collection's instruments on the audio thread, once per collection.

## Known gaps

- **`QSWAVEMIXOPENWAVEDATA`'s layout is unconfirmed.** The game builds it in a
  stack slot Ghidra could not reconstruct and no dynamic trace existed when
  this was written. `locate_wave` therefore accepts the pointer itself being
  RIFF data, or any of the first 16 fields pointing at RIFF data, and logs
  once when it finds neither. A wave it cannot decode still gets a valid
  handle, so the game proceeds silently rather than failing.
- **Blits between surfaces of different bit depths copy raw pixels** instead
  of converting. It logs once.
- **`DDBLT_KEYDEST` and `DDBLT_ROP` are ignored**, each with one log line.
- **Positional audio is stored, not applied**, in both DirectSound and QMixer,
  for the native host to consume when needed.

## Diagnosing a missing sound

`dx_dump` ends with a QMixer tally: waves opened and refused by reason, plays
asked for against plays delivered, and every drop counted under its cause - no
channel, channel disabled, session inactive, no host voice left, wave empty,
stream dry. The first twenty drops print a line each. "Some sounds are
missing" cannot be answered by a log of successes, and it was two counted
drops that found both of the bugs below.

Both were guesses about an SDK whose header this project does not have, and
both are now settled from evidence rather than from the guess:

- **`QSWaveMixOpenChannel`'s third argument selects what the second means.**
  QMixer.dll switches on it with `cmp eax,3` and a four-entry jump table, so it
  is 0 to 3. The game passes 2 with 15 and then plays on channels 0 upwards, so
  2 is a count. Reading it as an index left every channel the game uses
  unopened.
- **`QSWaveMixEnableChannel`'s fourth argument is not read as a mute.** The
  game calls it with 0 and then plays on the same channel and expects to hear
  it. Reading that as "disable" dropped every sound on the channel. The request
  is recorded and said once; playback is not gated on it.

- **`QSWaveMixSetVolume`'s value is a linear amplitude on 0 to 32767.** Its
  parameter handler in QMixer.dll is three instructions - `fild [edi+4]`,
  `fmul [0x180244d4]` where that constant is 3.051851e-05 or 1/32767, `fstp`
  into the channel's gain - so gain is value/32767. The game passes 14190,
  which is 0.433 or -7.3 dB. Read as hundredths of a decibel, as it was, every
  positive number clamped to unity and every sound played at full volume with
  no mix at all. A channel nobody has set is at full scale, because zero on a
  linear scale is silence rather than unity.

The 28 export arities are confirmed against the real QMixer.dll, so nothing on
that path is corrupting the guest stack.

## Diagnosing a silent run

`RECOMP_AUDIO_TRACE=N` prints the first N audio events: every `Lock`, `Unlock`,
`GetCurrentPosition` and submission on a playing buffer, with the offsets, the
lengths and the peak of what was written. It is off by default and costs
nothing when off.

The point of it is that a stream which goes quiet is one of a small number of
things, and they are told apart by which line stops appearing. No `Lock` at
all means the guest is not asking, and for the video player that means its own
cursor gate never opened, which means `GetCurrentPosition` is not advancing.
`Unlock` with `peak 0.000` means the guest is asking and writing silence.
`Unlock` with a real peak and no submission after it means the write is not
reaching the host. `build/recomp/pop_headless` boots into the same intro, so
all of this can be read off a headless run:

```
RECOMP_AUDIO_TRACE=200 RECOMP_MAX_SECONDS=25 build/recomp/pop_headless
```

## Tests

```
.venv/bin/python tools/test.py --native          # build and run
.venv/bin/python tools/test.py --compile-only    # build only
.venv/bin/ctest --preset macos -R dx_tests       # one suite
build/recomp/dmusic_render --dir DIR --segment NAME.sgt --seconds 60   # a game's music, measured
```

Run from the repository root. 4540 checks: vtable integrity, the
gradient/flip/present path the task specifies, blitting and colour keys,
`QueryInterface` identity, display-mode enumeration, the full Direct3D
pipeline through to a textured indexed draw, the DirectSound data path from a
`Lock` pointer to the bytes the host is handed, DirectSound notification
positions driven across the loop point, DirectInput state and buffered events,
a QMixer session, streamed QMixer waves fed by a synthetic guest callback on
hosts with and without a queue, weanetr's failure codes, and reference
counting against the heap block count. The audio groups are driven from the
Wine trace of the original: the streaming test uses its buffer size, format,
lock offsets and poll positions verbatim.

Every call goes through the real guest path: arguments are pushed onto a guest
stack and the shim is reached through `imports_dispatch`, the same way
recompiled code reaches it. The harness asserts after every call that the
shim popped exactly its own arguments and returned to the pushed address,
which is how a wrong `argc` in a vtable is caught. The suite is clean under
`-fsanitize=address,undefined`.
