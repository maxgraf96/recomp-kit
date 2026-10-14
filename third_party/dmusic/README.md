# GothicKit/dmusic, vendored

A DirectMusic implementation in C11: it reads DirectMusic Producer files
(segments `.sgt`, styles `.sty`, bands, DLS collections `.dls`), composes the
music from styles, chords and grooves, and renders it with a modified
TinySoundFont. `dx/dmusic.cpp` puts the DirectMusic COM interfaces a game
calls in front of it.

| | |
| --- | --- |
| upstream | https://github.com/GothicKit/dmusic |
| commit | `b73b270b94714b8ec8d9d0b2a9f3a5c445da52d1` (2026-03-02, `main`) |
| licence | MIT (Modern Variant), see `LICENSE.md`; `vendor/TinySoundFont` is GothicKit's modified copy of TinySoundFont (MIT, Bernhard Schelling) under the same terms |

## What is here

From the upstream tree at that commit, byte for byte:

- `LICENSE.md`;
- `README.md`, renamed `UPSTREAM-README` (its links point into parts of the
  tree that are not copied here);
- `include/dmusic.h`;
- every file under `src/` (`io/`, `thread/`, `util/` included);
- `vendor/TinySoundFont/tsf.h` and `tsf.c`.

Left out: the CMake build (the kit builds the library itself, below), `docs/`,
`examples/` (a player built on miniaudio), `support/` (build helpers, a logo
and an MP3 the examples play) and the CI configuration.

## How the kit builds it

`dx/CMakeLists.txt` compiles `src/` and the TinySoundFont copy into the DX
object libraries (`recomp_dx`, `recomp_dx_null`), so every binary that links the
shims has them:

- C11, the C11-threads shim in `src/thread/` (Apple's C library has no
  `<threads.h>`), `DM_STATIC=1`, warnings off for this directory only.
- `recomp/dm_prefix.h` is force-included in every file. It renames each
  external symbol the library defines but does not export as `Dm*`, chiefly
  TinySoundFont's `tsf_*`: the host links the kit's own, unmodified
  TinySoundFont (`third_party/tsf`, for MIDI) into the same executable, and
  the two copies have different structures behind the same names.
- `recomp/` holds only kit files; nothing under it is upstream's.

## Local patches

Each is its own commit on the kit's history touching only files above, with
the reason in its message, so that it can go upstream as it is:

```sh
git log --oneline -- third_party/dmusic/src third_party/dmusic/include third_party/dmusic/vendor
git format-patch --relative=third_party/dmusic <commit>^!   # one upstream-ready patch
```

| Patch | Why |
| --- | --- |
| Select an instrument by bank MSB, LSB and drum flag | A band patch is `drum<<31 \| MSB<<16 \| LSB<<8 \| program`; upstream passed the LSB as the bank and ignored the drum flag, so most GM/GS instruments and every drum kit played the collection's first preset. A missing GS variation plays its capital tone (MSB 0), as GS synthesizers do. |
| Master groove level (`DmPerformance_setMasterGroove`) | DirectMusic's `GUID_PerfMasterGrooveLevel`, added to every command's groove level: how a game sets the intensity of style-based music. Without it a segment whose command track says groove 1 only ever plays its groove-1 patterns. |
| Segment repeats setter (`DmSegment_setRepeats`) | `IDirectMusicSegment::SetRepeats`: a game sets how often a segment loops, whatever its file says. |
| Read the playing state under the lock | `playSegment` computed its boundary and `playTransition` composed from the playing style, band and chord without the performance lock, racing the render thread that changes them; a transition from a segment without a style dereferenced a null style. |
| A segment ends after its last repeat; `DmPerformance_isPlaying` | A DirectMusic segment state ends after its length times (repeats + 1); upstream let the style choose patterns for ever. **Changes behaviour** for a player that relied on endless playback: it should set the repeats (DirectMusic's `DMUS_SEG_REPEAT_INFINITE` is `0xFFFFFFFF`). `isPlaying` tells an API layer when the segment has ended. |
| A missing DLS collection leaves its instruments silent | A band whose collection is missing (a system GM bank that is not installed) failed the whole download, and with automatic download the segment would not load; the first failure also stopped the remaining bands and the style downloading. Now each missing collection is a warning, every band and style is still downloaded, and a style that could not be loaded is skipped at play time instead of dereferenced. |
| Accept a 16-byte `fmt ` chunk | A wave's format may be a `PCMWAVEFORMAT`, without `cbSize`; reading past it logged an error per wave (hundreds for Apple's GS bank). |
| A SoundFont 2 bank in place of a DLS collection | A resolver may answer `gm.dls` with an SF2 (a General MIDI bank where the system has no `gm.dls`); TinySoundFont loads it as it is and the synthesizer plays its presets, banks numbered by MSB as SoundFonts number them. |

To move to a newer upstream commit, replace the files listed under *What is
here*, re-apply the patches that upstream has not taken, and run
`tools/test.py --native` (`dmusic_tests`).
