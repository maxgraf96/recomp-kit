# `runtime` — guest runtime, image loader and Win32 shims

This directory is the host side of the static recompilation: it owns the guest
address space, loads `D3DPopTB.exe` into it, and implements the Win32 imports
the game calls. It knows nothing about game logic; every shim implements a
Win32 contract and nothing else.

The CPU state (`struct X86`), the memory accessors (`rd32`/`wr32` and friends)
and `recomp_call` come from `runtime/x86.h`, which the translator
owns. Compile the runtime with `-I<repo root>` so `guest.h` can find it.

## Files

| file | contents |
| --- | --- |
| `guest.h` | address-space constants, guest string helpers, logging, `guest_call` |
| `memory.h/.cpp` | the 256 MB arena and the first-fit heap allocator |
| `loader.h/.cpp` | SHA-256 check, PE mapping, IAT patching, TEB/stack setup, `run_entry` |
| `imports.h/.cpp` | shim table, trampoline allocation, dispatch, coverage reporting |
| `cpu.cpp` | the call-outs `x86.h` declares, plus the `_setjmp`/`_longjmp` intrinsics |
| `intrinsics.h` | the intrinsic contract generated code uses for `_setjmp`/`_longjmp` |
| `dispatch.cpp` | weak `recomp_call`/`recomp_jump` through the module registry when no image table is linked (freestanding bundles), and the `recomp_libm_*` functions `freestanding.h` maps to |
| `freestanding.h` | the C library surface generated code uses, as builtins and runtime entry points, for `RECOMP_FREESTANDING` |
| `kernel32.cpp` | heap, files, modules, TLS, sync objects, time, strings and locale |
| `user32.cpp` | window classes and windows, the per-thread message queues and sent messages, paint/DC/input |
| `user32_activation.cpp` | activation and focus: what the host reports through `host_app_state` becomes WM_ACTIVATEAPP / WM_ACTIVATE / WM_SETFOCUS / WM_KILLFOCUS / WM_SIZE, as Windows sends them |
| `user32_ansi.cpp` | the ANSI odds and ends: class queries, window text, LoadStringA/LoadBitmapA from any loaded module, MoveWindow, ChangeDisplaySettingsA, wsprintfA, SetWindowsHookExA |
| `gdi32_text.cpp`, `gdi32_truetype.cpp`, `gdi32_fontdb.cpp` | GDI text: fonts realized and measured as GDI measures them (VDMX, hdmx, OS/2), the installed copies of Windows faces with `[runtime] system_fonts`, the A and W text calls |
| `gdi_surface.h` | a DC over pixels the caller owns, for a DirectDraw surface's GetDC/ReleaseDC |
| `misc.cpp` | GDI32, ADVAPI32 registry, SHELL32, ole32, WINMM (clock, mmio, MIDI out) |
| `codepages.cpp`, `codepage_tables.h` | MultiByteToWideChar/WideCharToMultiByte as Windows converts: code pages 1252, 437, 850, 10000, 20127, 28591 (tables measured on Windows, `tools/codepages`), UTF-8, CP_SYMBOL |
| `user32_cursor.cpp` | cursors: stock IDC_* cursors, CreateCursor's planes and CreateIconIndirect's bitmaps kept with their hot spots, SetCursor, ShowCursor, SetCursorPos, and what a host reads to show them |
| `imm32.cpp` | IMM32, and text from the platform's input method (`host_post_text`) as WM_CHAR or an IME result |
| `winsock.cpp`, `winsock.h` | WSOCK32 and WS2_32 on the host's sockets ("Networking" below) |
| `iphlpapi.cpp` | the IP helper API (GetAdaptersInfo and friends) and NETAPI32's Netbios |
| `net_adapters.h/.cpp` | the network adapters a guest is shown, one list for Winsock and the IP helper API |
| `x87.c` | the exact x87's out-of-line half: SoftFloat-backed arithmetic, stores, FPREM, FXTRACT, FSCALE, FNSAVE/FRSTOR, transcendentals, and the msvcrt float-control mapping (the inline half is `x86.h`'s x87 section) |
| `msvcrt*.cpp` | msvcrt.dll: start-up and exit, heap, locale and ctype, strings, printf/scanf/strtod, stdio, maths, float control (`msvcrt_float.cpp`), time, RTTI (`msvcrt_internal.h` says which file holds what) |
| `msvcp60.cpp` | msvcp60.dll: `std::_Lockit`, `std::bad_cast`, the locale vectors and conversions |
| `win32.h` | what the host app and the DirectX shims call into |
| `tests/` | `runtime_tests.cpp` plus the test-only `recomp_call` |

## Messages, activation and text

**Every guest thread has its own message queue**, as on Windows: a window's
messages go to the thread that created it, PeekMessage and GetMessage read only
the caller's queue (a window filter of -1 reads thread messages only), and
PostThreadMessage reaches any live guest thread. SendMessage to another
thread's window queues the message ahead of that thread's posted ones and the
sender waits, delivering messages sent to itself meanwhile; the receiver runs
the window procedure inside its next PeekMessage, GetMessage, WaitMessage or
MsgWaitForMultipleObjects and never sees the message come out of its queue.

**Activation is the host's report, the runtime's messages.** The host calls
`host_app_state(focused, minimized)` whenever the platform's focus or
minimised state may have changed; the runtime sends what Windows sends, as
sent messages on the window's thread in order with the input queued around
them: WM_ACTIVATEAPP, WM_NCACTIVATE, WM_ACTIVATE (with fMinimized), then
DefWindowProc's WM_ACTIVATE gives the focus (WM_SETFOCUS); the reverse with
WM_KILLFOCUS; WM_SIZE(SIZE_MINIMIZED / SIZE_RESTORED) around a minimise. A
window shown while the application has the focus is activated from inside
ShowWindow. DirectInput's foreground devices are unacquired at the moment of
deactivation and are acquired again only by the game, from its WM_ACTIVATE
handling. A host that never reports the focus keeps the old behaviour: the
application counts as always in the foreground.

**Keyboard text.** `host_post_key_text` queues a key-down with the character
the host's layout made of it (WM_CHAR, or WM_SYSCHAR after Alt, in the window's
encoding: code page 1252 for an ANSI window) and marks the key translated, so
the guest's TranslateMessage makes no second one. TranslateMessage on a key the
guest posted itself uses the US layout and the keyboard state. A desktop host
keeps the platform's input method on and sends its text on its own
(`host_post_text`, after a key posted with `HOST_KEY_TEXT_FOLLOWS`), so dead
keys, Option characters, every layout and an input method's committed text
arrive as Windows delivers them: WM_CHAR per UTF-16 unit for a Unicode window;
for an ANSI window the 1252 bytes, or, when 1252 cannot spell the text, an IME
result (WM_IME_STARTCOMPOSITION, WM_IME_COMPOSITION with GCS_RESULTSTR,
WM_IME_ENDCOMPOSITION) whose text ImmGetCompositionStringW reads from the
non-NULL context ImmGetContext gives; DefWindowProc makes WM_IME_CHAR and then
WM_CHAR ('?' for what 1252 lacks) of it for a window that does not.

**Text metrics are GDI's.** `truetype_realize` turns a LOGFONT into the
whole-pixel em GDI hints at, the ascent and descent from the font's VDMX table
(else its OS/2 Windows ascent and descent), and advances from its hdmx device
widths (else the rounded linear advance); TEXTMETRIC, GetCharWidth and text
extents come from there. `[runtime] system_fonts` lets GDI use the platform's
installed copies of Windows faces (macOS has Arial, Tahoma, Times New Roman,
Courier New, Verdana and more, with the same tables), chosen as GDI's mapper
chooses; without it a Windows interface face is drawn with the bundled Open
Sans. `tools/gdi_metrics` holds the comparison with a Windows screen DC.

## Address space

Guest address `a` is `g_mem + a`.

| range | contents |
| --- | --- |
| `0x00400000`–`loader_image_limit()` | image at `[game] image_base` (`0xd4c000` for this EXE, from SizeOfImage) |
| `0x01000000`–`0x0e000000` | heap arena (HeapAlloc/GlobalAlloc/LocalAlloc/VirtualAlloc) |
| `0x0ef00000`–`0x0f000000` | stack, growing down |
| `0x0fe00000` | TEB: FS:[0] SEH head, FS:[4]/[8] stack bounds, FS:[0x18] self, FS:[0x2c] TLS |
| `0x0fe01000` | 64 TLS slots |
| `0x0ff00000 + 16*i` | trampoline for import `i` |
| `0x10000000`–`GUEST_SIZE` | `[modules.aux.*]` DLLs at their configured bases, then heap growth around them |
| `GUEST_SIZE`–4 GB | reserved, never mapped: any access traps (guarded arena) |

On a 64-bit host the arena is **guarded**: `mem_init` reserves the whole 4 GB a
32-bit guest can name and opens `[0, GUEST_SIZE)`, so `g_mem + a` never leaves
the reservation. An access from `GUEST_SIZE` up - and to the first 64 KB when
the null guard is armed (strict mode, or `RECOMP_NULL_GUARD=1`) - is reported
as `guest memory fault` with the guest address and registers, and the process
ends (exit code 5), instead of reading host memory or crashing without a word.
WebAssembly and other hosts that cannot reserve 4 GB keep the plain mapping.

**The heap grows.** Once `[heap_base, 0x0e000000)` cannot satisfy a request,
`heap_alloc` claims 16 MB stretches of `[0x10000000, GUEST_SIZE)` that no
configured module occupies (`mem_reserve_range`), so `[game] guest_size =
0x40000000` gives a game about a gigabyte more heap.

**Memory at exact addresses.** A tool that puts recorded guest memory back
where the original program had it - a replay of calls captured on Windows,
whose heap can lie anywhere in the 4 GB - uses two calls. `mem_open(addr,
size)` opens a range at exactly those addresses, above `GUEST_SIZE` included
(whole 64 KB grains, guarded arena only): generated code reaches it like any
guest memory, and `gm_valid`, `gm_str` and the C runtime's string scans
(`mem_readable_end`) accept it as they accept the arena. `heap_exclude(addr,
size)` takes a range away from the heap, base arena and growth alike, before
anything is allocated there - typically from a `mem_add_reset_hook` callback,
so the DllMains and the runtime allocate around it. `mem_init` undoes both.

**Modules sit at configured bases, relocated.** A DLL's `[modules.aux.*] base`
need not be its preferred ImageBase: the loader applies the PE's base
relocations as it maps it, exactly as the translator does to the bytes and
listings it reads (`translate.py` `Image`/`Rebase`), so several DLLs that all
prefer `0x10000000` load side by side from the original files. Every image is
mapped before any import table is bound, so the executable's imports from a
DLL - and DLLs' imports from each other - bind to the exports' guest addresses
in any configuration order; calls through them dispatch through the module
registry like any other call.

**DllMain runs in the Windows loader's order.** `run_entry` first attaches
every module the image imports (directly or through another module),
dependencies first, with `lpReserved` set; such a static import is never
unloaded. `LoadLibrary` of a module loads what it imports (one reference each)
before attaching it; `FreeLibrary` of the last reference detaches it and then
releases those. A DllMain that refuses the attach is detached again and its
dependencies released (`ERROR_DLL_INIT_FAILED`). Guest threads send
`DLL_THREAD_ATTACH`/`DETACH` (unless `DisableThreadLibraryCalls`), modules with
a TLS directory get a slot, a block in every thread and their callbacks, and
`ExitProcess` detaches everything in reverse. A DLL can also be the main image
(a harness without its executable): `run_entry` then runs its DllMain instead
of a process entry.

Handles are small integers outside the arena's mapped regions and are never
dereferenced: kernel objects from `0x00010004`, windows from `0x00020004`,
GDI objects from `0x00028004`, registry keys from `0x00030004`, mmio files
from `0x00040004`, pseudo modules from `0x60000000`.

`loader_init_context` sets the process-start CPU state: x87 control word
`0x027f` (`GUEST_INITIAL_FPU_CW`, what Windows gives every new thread; guest
threads start with it too), clear status word, all-empty tag word `0xffff`, `fpu_top` 0, and
`eflags_misc` `0x202` so `PUSHFD` reads `0x202`. The CRT's `__ftol`,
`__ctrlfp` and `__statfp` depend on those x87 values.

## The shim contract

```c
struct ImportShim {
    const char* dll;
    const char* name;
    uint8_t     argc_stdcall;   // dwords the shim pops; ARGC_CDECL, ARGC_UNKNOWN
    void      (*fn)(X86*);      // nullptr => logging-only, returns EAX = 0
};
```

On entry to `fn`, ESP points at the guest return address and argument `i` is
`arg(c, i)` (the dword at `ESP + 4 + 4*i`). A shim writes its result with
`set_eax` (or `set_eax64` for a 64-bit result in EDX:EAX) and returns. It never
touches ESP: `imports_dispatch` pops the return address plus `4*argc_stdcall`
bytes and sets EIP, exactly as the real `ret n` would.

`ARGC_CDECL` pops only the return address. `ARGC_UNKNOWN` pops only the return
address too, but logs loudly the first time, because leaving a stdcall callee's
arguments on the stack makes the guest stack drift.

To call back into the guest — a WNDPROC, a thread body, a multimedia timer
callback — use `guest_call(c, fn_addr, a0, a1, ...)`. It pushes the arguments
right to left plus a sentinel return address, dispatches through `recomp_call`
and restores ESP.

Adding a shim means adding one row to the table at the bottom of the relevant
`.cpp`. Registration happens once in `imports_init()`. The DirectX adapter adds
COM vtable slots to the same trampoline range with:

```c
uint32_t imports_alloc_trampoline("ddraw.dll", "IDirectDraw::Blt", fn, argc);
```

Anything the loader finds in the IAT that has no registered shim still gets a
trampoline, so a call logs `DLL!name` once and returns 0 rather than jumping
into nothing. A `thiscall` method reads `this` from `c->r[R_ECX]` and its
stack arguments with `arg` as usual.

Four weanetr imports are variables rather than functions
(`BFAID_INet`, `BFAID_MODEM`, `BFSPGUID_MODEM`, `options_to_parity_table`).
Those IAT slots hold the address of zeroed guest storage instead of a
trampoline; `imports_register_data(dll, name, size)` adds more.

## `_setjmp` / `_longjmp`

The translator substitutes these by address (`_longjmp` is `0x0055db78`).
`intrinsics.h` documents the contract: prefer the two-call form, which puts the
host `setjmp` in the generated function's own frame:

```c
{ jmp_buf *b = recomp_setjmp_prepare(c); recomp_setjmp_return(c, setjmp(*b)); }
```

`recomp_setjmp(c)` exists only to catch a call site that was not translated
into that form. It cannot work, because the host `setjmp` would belong to a
frame that has returned by the time `_longjmp` fires, so it logs what the call
site should emit and aborts.

`RaiseException` and `RtlUnwind` log their arguments and abort: there is no SEH
here, so continuing would run the guest past a point Windows never reaches.

## Behaviour worth knowing before you debug something

- **The image hash is a hard gate.** `loader_load` refuses any EXE whose
  SHA-256 is not the expected one, with no override, because everything after
  it trusts that exact layout. PE parsing bounds-checks the headers, the
  section table and every import RVA against the image.
- **`GetMessageA` blocks, and owns the wait.** It drives the multimedia
  timers, calls the host's pump, and yields the scheduler baton, round and
  round until a message matching the filter arrives. The host's pump services
  its event loop once and reports whether anything is queued; it must not wait
  itself, because only `GetMessageA` knows the filter and only the runtime can
  yield the baton. With no host installed at all it returns -1, the documented
  error, rather than blocking forever or inventing a `WM_NULL`.
- **`LoadLibraryA` only succeeds for DLLs the runtime has shims for** or maps
  as translated modules (any path; only the file name counts). Anything else
  reports `ERROR_MOD_NOT_FOUND`, so the guest takes its own "feature
  unavailable" path instead of calling into nothing.
- **`CreateWindowExA` sends `WM_NCCREATE` then `WM_CREATE`** and honours their
  failure returns by cancelling creation, as Windows does.
- **Painting follows the window's update region, not the clock.** Showing a
  hidden window invalidates it and calls the host's window-shown hook, which is
  the transition a window manager reacts to. `UpdateWindow` sends `WM_PAINT`
  synchronously, and only when the region is dirty; `BeginPaint` validates it.
  A posted `WM_PAINT` would arrive whenever the guest next pumped, and a guest
  that never pumps would never paint.
- **A wait yields to other guest threads.** Finite waits expire only after their
  deadline, and infinite waits remain pending until satisfied or shutdown. See
  **Threads** below for ownership, suspension and deadlock diagnostics.
- **`VirtualAlloc` works in whole pages**, base and size, and `MEM_DECOMMIT`
  discards the contents so a recommit sees zeroes.
- **A message box is answered with its default button.** There is no display,
  so nobody can click it, but replying `IDOK` to an `MB_YESNO` box would hand
  the caller an answer that box never offered. `MessageBoxA`/`W` return the
  default button of the button set the caller asked for, honouring
  `MB_DEFBUTTON1/2/3`, which is what pressing Return would have given.

## Threads

`CreateThread` starts a **cooperative** thread and returns, the way Windows
does. The game needs that: both DirectInput service threads (`0052c880`
keyboard, `0052cd40` mouse) tell their creator they are ready by setting a
field it polls and then loop on `WaitForSingleObject` for the rest of the
process, so a body run to completion inside `CreateThread` never comes back.

Each guest thread owns a register file, a 256 KB guest stack, a TEB and a TLS
array, and runs on its own host thread so guest code can nest host C frames
freely. **Exactly one of them executes at a time**, named by a baton held
under a mutex. Nothing in the guest arena, the shims or the generated code
needs a lock, because there is never more than one guest instruction in
flight — the same invariant the single-threaded runtime had.

**Every call into the runtime is a scheduling checkpoint.** `imports_dispatch`
yields if the running thread has held the baton for a millisecond. Scheduling
must not depend on which API the guest happens to poll: the frame limiter spins
on `GetTickCount`, and when only blocking calls yielded it could starve the
DirectInput workers indefinitely with their 200 ms waits already expired. The
millisecond rate limit keeps the cost to one clock read.

**A thread that cannot proceed blocks.** It leaves the runnable set with a
deadline, the scheduler runs everything else, and when nothing is runnable it
sleeps until the earliest deadline instead of spinning. `Sleep(ms)` suspends
the caller for the interval; `Sleep(0)` yields, as documented.

**Nothing is released by a timeout that did not happen.** A zero timeout
answers from the current state. A finite timeout expires only once the time has
passed. An `INFINITE` wait with no runnable peer and no pending deadline is a
deadlock, and it is reported as one — with the last sixteen baton hand-offs and
every thread's state — rather than answered with `WAIT_TIMEOUT`. The host's own
run deadline ends the process in that case.

**A wait is taken when it is granted.** The scheduler decrements the semaphore,
clears the auto-reset event or takes the mutex at the moment it releases a
waiter, so one signal releases exactly one of them.

**Suspension is a count.** `SuspendThread` and `ResumeThread` both return the
previous count and a thread runs again only at zero, so two suspends need two
resumes. `CREATE_SUSPENDED` starts it at one. Suspending yourself deschedules
before the call returns.

**Locks have owners.** A `CRITICAL_SECTION` keeps `LockCount`,
`RecursionCount` and `OwningThread` where Windows keeps them; its owner
recurses and anybody else blocks. A mutex is owned recursively by one thread,
and `ReleaseMutex` from a non-owner fails with `ERROR_NOT_OWNER`. This is not
bookkeeping for its own sake: a yield inside a protected section now really can
let another thread reach the same section.

A thread that has not finished reports `STILL_ACTIVE` from
`GetExitCodeThread`, which is itself a scheduling point because a caller
polling it is spinning (`0052d580` does exactly this), and leaves a wait on its
handle unsatisfied. `ExitThread` ends its own thread. `ExitProcess` called from
a guest thread cannot longjmp, because the landing pad is on the main thread's
stack: it records the request, hands the baton to the main thread and ends
itself, and the main thread performs the exit as soon as it is running again.

## Networking

WSOCK32 (Winsock 1.1) and WS2_32 (Winsock 2.2) are one implementation
(`winsock.cpp`) over `platform/os.h`'s `os_net_*` layer, as WSOCK32 forwards
to WS2_32 on Windows. They differ where their ABIs do: the IPPROTO_IP option
numbers (Winsock 1.1 has its own) and ordinals 10-12 (`import_argc.cpp`).
IPv4 only; the Windows host has no socket layer yet, and there WSAStartup
reports the network as unavailable.

**Sockets and blocking.** A guest SOCKET is a small integer (0x100, 0x104,
...) naming a record that owns a host descriptor. Host descriptors are always
non-blocking; a guest socket blocks until FIONBIO or WSAEventSelect says
otherwise, and a blocking call is a loop of non-blocking attempts with a guest
wait between them (`guest_wait_objects` on a per-thread event, in slices of at
most a second), so other guest threads run while one waits, and SO_RCVTIMEO /
SO_SNDTIMEO bound it. `closesocket` ends a call blocked on the socket with
WSAEINTR. A non-blocking connect reports WSAEWOULDBLOCK, as Windows does; a
refused one lands in select's except set, not its write set. Datagrams too
long for the buffer are truncated with WSAEMSGSIZE, and ICMP errors are never
reported on a datagram socket (Windows' WSAECONNRESET on recvfrom included).
Errors are per guest thread (WSAGetLastError) and also GetLastError's.
WSACleanup counts start-ups but never closes sockets, so a module that cleans
up and starts again mid-session keeps them.

**WSAEventSelect** follows Windows' recording rules: an event is recorded once
and set, and then stays quiet until the call that re-enables it (recv/recvfrom
for FD_READ, accept for FD_ACCEPT, a send that would block for FD_WRITE);
the re-enabling call records it again at once while its condition still holds,
so a reader that takes one datagram per wake-up is woken again while datagrams
are queued. WSAEventSelect records what already holds, a later call replaces
the earlier one, and an accepted socket inherits its listener's selection.
One host thread (the monitor) polls the descriptors with armed events or
blocked callers and signals through `guest_event_signal_from_host`.

**Broadcast.** BSD sends 255.255.255.255 out of the default route's interface
only. A datagram to it from a socket bound to the wildcard address is sent
once per adapter instead, to the adapter's directed broadcast address (or out
of a point-to-point tunnel by interface index), so a LAN game's discovery
reaches the local network and an overlay such as ZeroTier alike; a socket bound
to one adapter's address broadcasts on that adapter only. BSD also delivers
broadcasts only to wildcard sockets, where Windows delivers them to a socket
bound to the receiving interface's address: a datagram socket the guest binds
to an adapter's address is the wildcard on the host, reports the guest's
address from getsockname, and drops what Windows would not have delivered.
TCP sockets get SO_REUSEADDR on the host, which lets a port be bound again
while old connections linger as Windows does, and still refuses a second
socket on the same address and port.

**Adapters** (`net_adapters.h`): the host's up IPv4 interfaces without
loopback; the local network first (`en*`, `eth*`, `wl*`, the default route's
ahead of the rest), then overlay networks (ZeroTier's `feth*`, `zt*`), then
tunnels (`utun*`, `tun*`, `tap*`, `wg*`, `ppp*`, `ipsec*`), then the rest;
at most ten, and no more than fit a 6,400-byte GetAdaptersInfo buffer, which
programs of the period allocated as a fixed size. Winsock's own-host lookup,
its broadcast fan-out, SIO_GET_INTERFACE_LIST, GetAdaptersInfo,
GetAdaptersAddresses, GetIpForwardTable, GetIpAddrTable, GetBestInterface,
GetBestRoute and Netbios (NCBENUM, NCBRESET, NCBASTAT) all answer from this
list in this order.

**Names.** `gethostname` is the host's name up to its first dot.
`gethostbyname` answers a dotted address, `localhost` and the own name (every
adapter address, in adapter order) without DNS; a name in game.toml's
`[network] offline_hosts` or `RECOMP_NET_OFFLINE_HOSTS` fails at once with
WSAHOST_NOT_FOUND, as does every other name under `RECOMP_NET_DNS=0`; the rest
go to the system resolver on a host thread while the guest thread waits (ten
seconds at most, then WSATRY_AGAIN). `gethostbyaddr` does no reverse lookup.
The HOSTENT and `inet_ntoa`'s string are per thread and overwritten by the
thread's next call, as Windows documents them.

Not modelled: overlapped I/O (WSARecv and friends work without an
OVERLAPPED), WSAAsyncSelect, IPv6, raw sockets.

## The guest file system

The guest root `C:\Populous\` maps to the directory holding the loaded EXE
(`original/gog` by default). Path resolution converts `\` to `/`, resolves
`.`/`..`, and matches each component case-insensitively against the real
directory (cached per directory, invalidated on create/delete/rename). Relative
paths resolve against the guest current directory, which starts at
`C:\Populous`.

## Environment variables

| variable | effect |
| --- | --- |
| `RECOMP_LOG` | `0` silent, `1` warnings and one-shot notices (default), `2` traces every import call and returned EAX |
| `RECOMP_REGISTRY` | registry backing file (default `build/recomp/registry.json`) |
| `RECOMP_CREATETHREAD=skip` | record `CreateThread` without running the thread body |
| `RECOMP_CREATETHREAD=sync` | run a thread body to completion inside `CreateThread` (pre-scheduler behaviour, for bisection) |
| `RECOMP_IMPORT_STATS=1` | at exit, classify every import as called / not reached / logging-only |
| `RECOMP_STRICT=1` / `0` | strict mode on or off (default: `[runtime] strict`): an undeliverable call or jump, a call through an import of unknown stdcall argument count, or a divide error stops the run with a diagnostic; the null page faults |
| `RECOMP_NULL_GUARD=1` / `0` | arm or disarm the null-page guard independently of strict mode |
| `RECOMP_NET_TRACE=1` | every socket call and its outcome on stderr, broadcast fan-out included |
| `RECOMP_NET_DNS=0` | gethostbyname answers only what it can without DNS |
| `RECOMP_NET_OFFLINE_HOSTS` | comma-separated host names gethostbyname fails at once, besides `[network] offline_hosts` |
| `RECOMP_NET_ADAPTERS` | comma-separated interface names: the adapters a guest sees, in that order |

## Tests

```
.venv/bin/python tools/test.py --native          # build and run
.venv/bin/python tools/test.py --compile-only    # build only
.venv/bin/ctest --preset macos -R runtime_tests  # one suite
```

Run from the repository root. The tests load the real EXE, cross-check the
section mapping against `pefile`, exercise the allocator, open
`data/VCONFIG0.DAT` through a mixed-case guest path, round-trip the registry
and check that every shim leaves ESP balanced.

## MIDI out

The game's music is MIDI played through a SoundFont, and the device name is the
whole gate. `0x575e40` walks the `midiOut` devices, calls `midiOutGetDevCapsA`
on each, and keeps the first whose `szPname` begins with the nine characters
`SoundFont` (the literal at `0x5eb5b0`); if none matches it returns -1 and there
is no music at all. It tries `SFMAN32.DLL` first - the Creative SoundFont
manager - which this build reports as missing, so the plain `midiOut` path is
the one that runs. It then opens with a null callback, sends a twelve-byte
sysex through `midiOutPrepareHeader` and `midiOutLongMsg`, and plays note by
note with `midiOutShortMsg` (`0x576140` is its all-notes-off: control change
`0x7b` on channel 0).

So `midiOutGetDevCapsA` reports one device called "SoundFont Synth", and the
bank is `Sound/POPFIGHT.SF2` beside the executable, resolved through the file
shim and handed to the host as a path. `host_midi_open/short/sysex/reset/close`
in `win32.h` are the whole contract; the weak defaults in `misc.cpp` accept
every message and play nothing, so a build with no host links and reports a
device that is simply silent. That is deliberate: the game has one MIDI path
and no fallback, so failing the open would lose the music and gain nothing.

Every one of these runs on a guest thread and holds the scheduler baton for the
whole call, so a host that blocks inside one stops every guest thread and not
just the caller. `host_midi_short` is the hot one, called per note, and must
not take a lock an audio thread can hold. `host_midi_open` is the slow one: the
retail bank is 485 KB and parses quickly, but a host that loaded a large one
synchronously would freeze the game at the moment the music starts with nothing
to say why, so the open is timed.

The host now builds the synth and loads the bank before the guest starts, so
`host_midi_open` publishes what already exists and parses nothing. The timing
above therefore measures a call whose expected reading is zero, and the
threshold is two milliseconds to match: against an expectation of zero, fifty
would let a twenty-millisecond parse back in unremarked, and that stutters
every time the music starts. If it fires, the work belongs in
`host_midi_startup` rather than on a host thread - which is what the message
says, because the advice has to move with the meaning.

That is a better answer than moving the load to a host thread would have been,
and the reason is worth keeping. Loading before any guest thread exists gives
up nothing: what the baton provides is exclusion between guest threads, so a
host thread would have surrendered it - no note can arrive in the middle of an
open that builds nothing, whereas one certainly could arrive in the middle of
an open waiting on another thread. And nothing waits while holding the baton,
because nothing waits at all.
