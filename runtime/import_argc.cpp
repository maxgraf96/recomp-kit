// import_argc.cpp - what a stdcall import pops, for imports no shim describes.
//
// A logging-only import still has to return the way its callee would: a
// stdcall function pops its arguments, and a stub that does not leaves the
// caller's stack displaced by that much from then on. The registered shims say
// for themselves; this file says for everything else, from facts about the
// Win32 ABI rather than about any game:
//
//   1. the game's own table (game.toml [imports.argc], RECOMP_IMPORT_ARGC),
//   2. the Win32 API signatures below,
//   3. the C runtimes' convention (cdecl, with their few stdcall exceptions),
//   4. an MSVC-decorated C++ name, whose calling convention and parameter
//      types are spelled out in it,
//   5. a decorated C name, _name@bytes (imports.cpp).
//
// It also names the imports Winsock DLLs export by ordinal, whose ordinals are
// part of their ABI and differ between ws2_32 and wsock32.
#include "imports.h"
#include "game_config.h"

#include <algorithm>
#include <cctype>
#include <string.h>
#include <string>
#include <vector>

namespace {

struct KnownArgc {
    const char *dll; // lower case, without ".dll"
    const char *name;
    uint8_t argc;
};

// Stdcall argument counts in dwords (a POINT or an in_addr passed by value is
// two or one). Sorted by DLL then name; tests check the order and that every
// registered shim agrees.
const KnownArgc kWin32[] = {
    {"advapi32", "RegCloseKey", 1},
    {"advapi32", "RegCreateKeyExA", 9},
    {"advapi32", "RegDeleteKeyA", 2},
    {"advapi32", "RegDeleteValueA", 2},
    {"advapi32", "RegOpenKeyExA", 5},
    {"advapi32", "RegQueryValueExA", 6},
    {"advapi32", "RegSetValueExA", 6},
    {"ddraw", "DirectDrawCreate", 3},
    {"ddraw", "DirectDrawCreateEx", 4},
    {"ddraw", "DirectDrawEnumerateA", 2},
    {"ddraw", "DirectDrawEnumerateExA", 3},
    {"dinput", "DirectInputCreateA", 4},
    {"gdi32", "Arc", 9},
    {"gdi32", "BitBlt", 9},
    {"gdi32", "CreateBrushIndirect", 1},
    {"gdi32", "CreateCompatibleBitmap", 3},
    {"gdi32", "CreateCompatibleDC", 1},
    {"gdi32", "CreateDIBSection", 6},
    {"gdi32", "CreateDIBitmap", 6},
    {"gdi32", "CreateFontA", 14},
    {"gdi32", "CreatePen", 3},
    {"gdi32", "CreateSolidBrush", 1},
    {"gdi32", "DPtoLP", 3},
    {"gdi32", "DeleteDC", 1},
    {"gdi32", "DeleteObject", 1},
    {"gdi32", "Ellipse", 5},
    {"gdi32", "ExtTextOutA", 8},
    {"gdi32", "GetCharWidthW", 4},
    {"gdi32", "GetCurrentObject", 2},
    {"gdi32", "GetDIBits", 7},
    {"gdi32", "GetDeviceCaps", 2},
    {"gdi32", "GetObjectA", 3},
    {"gdi32", "GetPixel", 3},
    {"gdi32", "GetStockObject", 1},
    {"gdi32", "GetTextExtentPoint32A", 4},
    {"gdi32", "GetTextExtentPoint32W", 4},
    {"gdi32", "GetTextMetricsA", 2},
    {"gdi32", "IntersectClipRect", 5},
    {"gdi32", "Polygon", 3},
    {"gdi32", "Polyline", 3},
    {"gdi32", "Rectangle", 5},
    {"gdi32", "SelectClipRgn", 2},
    {"gdi32", "SelectObject", 2},
    {"gdi32", "SetBkColor", 2},
    {"gdi32", "SetBkMode", 2},
    {"gdi32", "SetPixel", 4},
    {"gdi32", "SetTextColor", 2},
    {"gdi32", "TextOutW", 5},
    {"imm32", "ImmGetCandidateListA", 4},
    {"imm32", "ImmGetCompositionStringA", 4},
    {"imm32", "ImmGetCompositionStringW", 4},
    {"imm32", "ImmGetContext", 1},
    {"imm32", "ImmNotifyIME", 4},
    {"imm32", "ImmReleaseContext", 2},
    {"imm32", "ImmSetOpenStatus", 2},
    {"iphlpapi", "GetAdaptersInfo", 2},
    {"kernel32", "CloseHandle", 1},
    {"kernel32", "CompareFileTime", 2},
    {"kernel32", "CopyFileA", 3},
    {"kernel32", "CreateDirectoryA", 2},
    {"kernel32", "CreateEventA", 4},
    {"kernel32", "CreateFileA", 7},
    {"kernel32", "CreateFileMappingA", 6},
    {"kernel32", "CreateMutexA", 3},
    {"kernel32", "CreateProcessA", 10},
    {"kernel32", "CreateThread", 6},
    {"kernel32", "DeleteCriticalSection", 1},
    {"kernel32", "DeleteFileA", 1},
    {"kernel32", "DisableThreadLibraryCalls", 1},
    {"kernel32", "DuplicateHandle", 7},
    {"kernel32", "EnterCriticalSection", 1},
    {"kernel32", "EnumResourceLanguagesA", 5},
    {"kernel32", "EnumResourceNamesA", 4},
    {"kernel32", "ExitThread", 1},
    {"kernel32", "FindClose", 1},
    {"kernel32", "FindFirstFileA", 2},
    {"kernel32", "FindNextFileA", 2},
    {"kernel32", "FreeLibrary", 1},
    {"kernel32", "GetCurrentDirectoryA", 2},
    {"kernel32", "GetCurrentProcess", 0},
    {"kernel32", "GetCurrentThread", 0},
    {"kernel32", "GetCurrentThreadId", 0},
    {"kernel32", "GetDiskFreeSpaceA", 5},
    {"kernel32", "GetExitCodeThread", 2},
    {"kernel32", "GetFileAttributesA", 1},
    {"kernel32", "GetFileAttributesExA", 3},
    {"kernel32", "GetFileSize", 2},
    {"kernel32", "GetFileTime", 4},
    {"kernel32", "GetLastError", 0},
    {"kernel32", "GetLocalTime", 1},
    {"kernel32", "GetLocaleInfoA", 4},
    {"kernel32", "GetModuleFileNameA", 3},
    {"kernel32", "GetModuleHandleA", 1},
    {"kernel32", "GetProcAddress", 2},
    {"kernel32", "GetShortPathNameA", 3},
    {"kernel32", "GetStartupInfoA", 1},
    {"kernel32", "GetSystemTimeAsFileTime", 1},
    {"kernel32", "GetTempFileNameA", 4},
    {"kernel32", "GetTempPathA", 2},
    {"kernel32", "GetThreadContext", 2},
    {"kernel32", "GetThreadPriority", 1},
    {"kernel32", "GetTickCount", 0},
    {"kernel32", "GetUserDefaultLangID", 0},
    {"kernel32", "GetVersion", 0},
    {"kernel32", "GetVolumeInformationA", 8},
    {"kernel32", "GetWindowsDirectoryA", 2},
    {"kernel32", "GlobalAlloc", 2},
    {"kernel32", "GlobalLock", 1},
    {"kernel32", "GlobalSize", 1},
    {"kernel32", "GlobalUnlock", 1},
    {"kernel32", "InitializeCriticalSection", 1},
    {"kernel32", "InterlockedDecrement", 1},
    {"kernel32", "InterlockedIncrement", 1},
    {"kernel32", "LeaveCriticalSection", 1},
    {"kernel32", "LoadLibraryA", 1},
    {"kernel32", "LoadLibraryExA", 3},
    {"kernel32", "MapViewOfFile", 5},
    {"kernel32", "MulDiv", 3},
    {"kernel32", "MultiByteToWideChar", 6},
    {"kernel32", "OutputDebugStringA", 1},
    {"kernel32", "QueryPerformanceCounter", 1},
    {"kernel32", "QueryPerformanceFrequency", 1},
    {"kernel32", "ReadFile", 5},
    {"kernel32", "RemoveDirectoryA", 1},
    {"kernel32", "ResetEvent", 1},
    {"kernel32", "ResumeThread", 1},
    {"kernel32", "SetEndOfFile", 1},
    {"kernel32", "SetEvent", 1},
    {"kernel32", "SetFileAttributesA", 2},
    {"kernel32", "SetFilePointer", 4},
    {"kernel32", "SetThreadPriority", 2},
    {"kernel32", "Sleep", 1},
    {"kernel32", "SuspendThread", 1},
    {"kernel32", "TerminateThread", 2},
    {"kernel32", "UnmapViewOfFile", 1},
    {"kernel32", "VirtualQueryEx", 4},
    {"kernel32", "WaitForMultipleObjects", 4},
    {"kernel32", "WaitForSingleObject", 2},
    {"kernel32", "WaitForSingleObjectEx", 3},
    {"kernel32", "WideCharToMultiByte", 8},
    {"kernel32", "WriteFile", 5},
    {"kernel32", "lstrcatA", 2},
    {"kernel32", "lstrcpyA", 2},
    {"kernel32", "lstrcpynA", 3},
    {"ole32", "CoCreateInstance", 5},
    {"ole32", "CoInitialize", 1},
    {"ole32", "CoUninitialize", 0},
    {"shell32", "ShellExecuteA", 6},
    {"user32", "AdjustWindowRect", 3},
    {"user32", "BeginPaint", 2},
    {"user32", "CallNextHookEx", 4},
    {"user32", "ChangeDisplaySettingsA", 2},
    {"user32", "ClientToScreen", 2},
    {"user32", "CloseClipboard", 0},
    {"user32", "CloseWindow", 1},
    {"user32", "CreateCursor", 7},
    {"user32", "CreateIconIndirect", 1},
    {"user32", "CreateWindowExA", 12},
    {"user32", "DefWindowProcA", 4},
    {"user32", "DestroyCursor", 1},
    {"user32", "DestroyIcon", 1},
    {"user32", "DestroyWindow", 1},
    {"user32", "DispatchMessageA", 1},
    {"user32", "EmptyClipboard", 0},
    {"user32", "EndPaint", 2},
    {"user32", "EnumDisplaySettingsA", 3},
    {"user32", "GetClassInfoA", 3},
    {"user32", "GetClassInfoExA", 3},
    {"user32", "GetClassNameA", 3},
    {"user32", "GetClientRect", 2},
    {"user32", "GetClipboardData", 1},
    {"user32", "GetCursorPos", 1},
    {"user32", "GetDC", 1},
    {"user32", "GetDesktopWindow", 0},
    {"user32", "GetKeyState", 1},
    {"user32", "GetKeyboardLayout", 1},
    {"user32", "GetMessageA", 4},
    {"user32", "GetParent", 1},
    {"user32", "GetSysColor", 1},
    {"user32", "GetSystemMetrics", 1},
    {"user32", "GetWindowLongA", 2},
    {"user32", "GetWindowPlacement", 2},
    {"user32", "GetWindowRect", 2},
    {"user32", "GetWindowTextA", 3},
    {"user32", "InvalidateRect", 3},
    {"user32", "IsClipboardFormatAvailable", 1},
    {"user32", "IsWindowVisible", 1},
    {"user32", "KillTimer", 2},
    {"user32", "LoadBitmapA", 2},
    {"user32", "LoadCursorA", 2},
    {"user32", "LoadIconA", 2},
    {"user32", "LoadStringA", 4},
    {"user32", "MoveWindow", 6},
    {"user32", "OpenClipboard", 1},
    {"user32", "PeekMessageA", 5},
    {"user32", "PostMessageA", 4},
    {"user32", "PostQuitMessage", 1},
    {"user32", "PostThreadMessageA", 4},
    {"user32", "RedrawWindow", 4},
    {"user32", "RegisterClassA", 1},
    {"user32", "RegisterClassExA", 1},
    {"user32", "ReleaseCapture", 0},
    {"user32", "ReleaseDC", 2},
    {"user32", "ScreenToClient", 2},
    {"user32", "SendMessageA", 4},
    {"user32", "SetCapture", 1},
    {"user32", "SetClipboardData", 2},
    {"user32", "SetCursor", 1},
    {"user32", "SetCursorPos", 2},
    {"user32", "SetFocus", 1},
    {"user32", "SetTimer", 4},
    {"user32", "SetWindowLongA", 3},
    {"user32", "SetWindowPos", 7},
    {"user32", "SetWindowTextA", 2},
    {"user32", "SetWindowsHookExA", 4},
    {"user32", "ShowCursor", 1},
    {"user32", "ShowWindow", 2},
    {"user32", "TranslateMessage", 1},
    {"user32", "UnhookWindowsHookEx", 1},
    {"user32", "UnregisterClassA", 2},
    {"user32", "WindowFromPoint", 2},
    {"user32", "wsprintfA", ARGC_CDECL},
    {"version", "GetFileVersionInfoA", 4},
    {"version", "GetFileVersionInfoSizeA", 2},
    {"version", "VerQueryValueA", 4},
    {"winmm", "auxGetDevCapsA", 3},
    {"winmm", "auxGetNumDevs", 0},
    {"winmm", "auxGetVolume", 2},
    {"winmm", "auxSetVolume", 2},
    {"winmm", "mciSendCommandA", 4},
    {"winmm", "mixerGetControlDetailsA", 3},
    {"winmm", "mixerGetDevCapsA", 3},
    {"winmm", "mixerGetLineControlsA", 3},
    {"winmm", "mixerGetLineInfoA", 3},
    {"winmm", "mixerGetNumDevs", 0},
    {"winmm", "mixerSetControlDetails", 3},
    {"winmm", "timeBeginPeriod", 1},
    {"winmm", "timeEndPeriod", 1},
    {"winmm", "timeGetTime", 0},
    {"winmm", "timeKillEvent", 1},
    {"winmm", "timeSetEvent", 5},
};

// Winsock: the same signatures in ws2_32 and wsock32.
const KnownArgc kWinsock[] = {
    {"", "WSAAsyncSelect", 4},
    {"", "WSACleanup", 0},
    {"", "WSAEventSelect", 3},
    {"", "WSAGetLastError", 0},
    {"", "WSASetLastError", 1},
    {"", "WSAStartup", 2},
    {"", "__WSAFDIsSet", 2},
    {"", "accept", 3},
    {"", "bind", 3},
    {"", "closesocket", 1},
    {"", "connect", 3},
    {"", "gethostbyaddr", 3},
    {"", "gethostbyname", 1},
    {"", "gethostname", 2},
    {"", "getpeername", 3},
    {"", "getsockname", 3},
    {"", "getsockopt", 5},
    {"", "htonl", 1},
    {"", "htons", 1},
    {"", "inet_addr", 1},
    {"", "inet_ntoa", 1},
    {"", "ioctlsocket", 3},
    {"", "listen", 2},
    {"", "ntohl", 1},
    {"", "ntohs", 1},
    {"", "recv", 4},
    {"", "recvfrom", 6},
    {"", "select", 5},
    {"", "send", 4},
    {"", "sendto", 6},
    {"", "setsockopt", 5},
    {"", "shutdown", 2},
    {"", "socket", 3},
};

// Ordinals 1-23 and 51-57 of Winsock 1.1; ws2_32 kept them except that it
// moved ioctlsocket to 10 and inet_addr/inet_ntoa to 11/12.
const char *const kWsock32Ordinals[24] = {
    nullptr,       "accept",     "bind",   "closesocket", "connect",   "getpeername",
    "getsockname", "getsockopt", "htonl",  "htons",       "inet_addr", "inet_ntoa",
    "ioctlsocket", "listen",     "ntohl",  "ntohs",       "recv",      "recvfrom",
    "select",      "send",       "sendto", "setsockopt",  "shutdown",  "socket"};
const char *const kDatabaseOrdinals[7] = {"gethostbyaddr",    "gethostbyname", "getprotobyname",
                                          "getprotobynumber", "getservbyname", "getservbyport",
                                          "gethostname"};
const char *const kWsaOrdinals[16] = {"WSAAsyncSelect",
                                      "WSAAsyncGetHostByAddr",
                                      "WSAAsyncGetHostByName",
                                      "WSAAsyncGetProtoByNumber",
                                      "WSAAsyncGetProtoByName",
                                      "WSAAsyncGetServByPort",
                                      "WSAAsyncGetServByName",
                                      "WSACancelAsyncRequest",
                                      "WSASetBlockingHook",
                                      "WSAUnhookBlockingHook",
                                      "WSAGetLastError",
                                      "WSASetLastError",
                                      "WSACancelBlockingCall",
                                      "WSAIsBlocking",
                                      "WSAStartup",
                                      "WSACleanup"};

// "WSOCK32.dll" -> "wsock32".
std::string dll_key(const char *dll) {
    std::string k(dll ? dll : "");
    std::transform(k.begin(), k.end(), k.begin(),
                   [](unsigned char ch) { return (char)tolower(ch); });
    if (k.size() > 4 && k.compare(k.size() - 4, 4, ".dll") == 0)
        k.resize(k.size() - 4);
    return k;
}

bool is_winsock(const std::string &k) {
    return k == "ws2_32" || k == "wsock32";
}

// The C runtimes export cdecl functions, except the two C++ exception entry
// points the compiler calls as stdcall.
bool is_crt(const std::string &k) {
    return k == "msvcrt" || k == "msvcrtd" || k == "crtdll" || k.compare(0, 5, "msvcr") == 0;
}

// --------------------------------------------------------------------------
// MSVC C++ names. Enough of the grammar to measure the parameters of the
// functions a C++ runtime exports: qualified and template names, back
// references, pointers, references and the built-in types. A class passed by
// value has a size the name does not carry, so it, and anything this parser
// does not know, gives up.
// --------------------------------------------------------------------------
struct Demangle {
    const char *p;
    bool ok = true;
    std::vector<int> param_sizes; // memoised multi-character parameter types

    explicit Demangle(const char *s) : p(s) {}

    void fail() {
        ok = false;
    }
    bool eat(char ch) {
        if (*p != ch)
            return false;
        ++p;
        return true;
    }
    // A name fragment: identifier@, a back reference digit, or a template.
    void fragment() {
        if (!ok || !*p) {
            fail();
            return;
        }
        if (isdigit((unsigned char)*p)) {
            ++p;
            return;
        }
        if (p[0] == '?' && p[1] == '$') {
            p += 2;
            Demangle inner(p); // template arguments have their own back references
            inner.ident();
            while (inner.ok && *inner.p && *inner.p != '@')
                inner.template_arg();
            if (!inner.ok || !inner.eat('@')) {
                fail();
                return;
            }
            p = inner.p;
            return;
        }
        if (p[0] == '?') {
            // A special name inside a scope (a nested operator): rare in an
            // export table; not measured.
            fail();
            return;
        }
        ident();
    }
    void ident() {
        const char *at = strchr(p, '@');
        if (!at || at == p) {
            fail();
            return;
        }
        p = at + 1;
    }
    // Scope fragments up to the terminating '@'.
    void qualified_tail() {
        while (ok && *p && *p != '@')
            fragment();
        if (!eat('@'))
            fail();
    }
    void template_arg() {
        if (p[0] == '$' && p[1] == '0') { // an integer constant
            p += 2;
            number();
            return;
        }
        type(nullptr);
    }
    void number() {
        if (*p == '?')
            ++p;
        if (isdigit((unsigned char)*p)) {
            ++p;
            return;
        }
        while (*p && *p != '@')
            ++p;
        if (!eat('@'))
            fail();
    }
    // A data type; *size receives its size on the stack in bytes, or -1.
    void type(int *size) {
        int dummy;
        int &out = size ? *size : dummy;
        out = -1;
        if (!ok || !*p) {
            fail();
            return;
        }
        const char c = *p++;
        switch (c) {
        case 'C':
        case 'D':
        case 'E':
        case 'F':
        case 'G':
        case 'H':
        case 'I':
        case 'J':
        case 'K':
        case 'M':
            out = 4;
            return;
        case 'N':
        case 'O':
            out = 8;
            return;
        case 'X':
            out = 0;
            return;
        case '_':
            switch (*p++) {
            case 'J':
            case 'K':
                out = 8;
                return;
            case 'N':
            case 'W':
            case 'D':
            case 'E':
            case 'F':
            case 'G':
            case 'H':
            case 'I':
                out = 4;
                return;
            default:
                fail();
                return;
            }
        case 'P':
        case 'Q':
        case 'R':
        case 'S':
        case 'A':
        case 'B': // pointer, reference
            if (*p == 'E')
                ++p;         // __ptr64
            if (*p == '6') { // pointer to function
                ++p;
                function_type(nullptr, nullptr);
            } else {
                if (*p < 'A' || *p > 'D') { // cv qualifier of the pointee
                    fail();
                    return;
                }
                ++p;
                type(nullptr);
            }
            out = 4;
            return;
        case 'W': // enum: W<underlying digit><name>
            if (!isdigit((unsigned char)*p)) {
                fail();
                return;
            }
            ++p;
            qualified_tail();
            out = 4;
            return;
        case 'V':
        case 'U':
        case 'T': // class, struct, union by value: size unknown
            qualified_tail();
            out = -1;
            return;
        case '?': // a cv-qualified type used by value
            if (*p < 'A' || *p > 'D') {
                fail();
                return;
            }
            ++p;
            type(&out);
            return;
        default:
            fail();
            return;
        }
    }
    // Return type then parameters; *bytes the parameters' stack size (-1 when
    // unknown), *varargs when it ends in "...".
    void function_type(int *bytes, bool *varargs) {
        if (!ok || !*p) {
            fail();
            return;
        }
        ++p; // calling convention of a pointed-to function
        returns_and_params(bytes, varargs);
    }
    void returns_and_params(int *bytes, bool *varargs) {
        if (!eat('@')) // '@' = no return type (a constructor or destructor)
            type(nullptr);
        params(bytes, varargs);
    }
    void params(int *bytes, bool *varargs) {
        int total = 0;
        bool known = true, dots = false;
        if (eat('X')) {
            // (void)
        } else {
            while (ok && *p && *p != '@' && *p != 'Z') {
                int size = -1;
                if (isdigit((unsigned char)*p)) {
                    const size_t i = (size_t)(*p++ - '0');
                    size = i < param_sizes.size() ? param_sizes[i] : -1;
                } else {
                    const char *start = p;
                    type(&size);
                    if (p - start > 1 && param_sizes.size() < 10)
                        param_sizes.push_back(size);
                }
                if (size < 0)
                    known = false;
                else
                    total += (size + 3) & ~3;
            }
            if (eat('Z'))
                dots = true;
            else if (!eat('@'))
                fail();
        }
        if (!eat('Z')) // the function type's own terminator
            fail();
        if (bytes)
            *bytes = known ? total : -1;
        if (varargs)
            *varargs = dots;
    }
};

uint8_t pops(int bytes) {
    return bytes < 0 || bytes / 4 >= ARGC_UNKNOWN ? ARGC_UNKNOWN : uint8_t(bytes / 4);
}

#ifdef RECOMP_IMPORT_ARGC
struct GameArgc {
    const char *key; // "dll!name", the DLL as the game spells it
    uint8_t argc;
};
const GameArgc kGame[] = RECOMP_IMPORT_ARGC;
#endif

} // namespace

uint8_t imports_msvc_argc(const char *mangled) {
    if (!mangled || mangled[0] != '?')
        return ARGC_UNKNOWN;
    Demangle d(mangled + 1);
    // The unqualified name: an operator or special name (?0 constructor, ?1
    // destructor, ?_7 vftable, ...), or an identifier.
    if (*d.p == '?') {
        ++d.p;
        if (*d.p == '_')
            ++d.p;
        if (!*d.p)
            return ARGC_UNKNOWN;
        ++d.p;
    } else if (*d.p == '$') {
        return ARGC_UNKNOWN; // a template function: not measured
    } else {
        d.ident();
    }
    d.qualified_tail(); // the scopes, ending in "@@" (the last '@' eaten here)
    if (!d.ok || !*d.p)
        return ARGC_UNKNOWN;
    const char access = *d.p++;
    if (isdigit((unsigned char)access))
        return ARGC_UNKNOWN; // data: a static member, a global, a vftable
    bool member = false;
    switch (access) {
    case 'A':
    case 'B':
    case 'E':
    case 'F':
    case 'I':
    case 'J':
    case 'M':
    case 'N':
    case 'Q':
    case 'R':
    case 'U':
    case 'V':
        member = true; // a this-qualifier follows
        break;
    case 'C':
    case 'D':
    case 'K':
    case 'L':
    case 'S':
    case 'T':
    case 'Y':
    case 'Z':
        break;
    default:
        return ARGC_UNKNOWN; // thunks carry an adjustor; not measured
    }
    if (member) {
        if (*d.p == 'E')
            ++d.p;
        if (*d.p < 'A' || *d.p > 'D')
            return ARGC_UNKNOWN;
        ++d.p;
    }
    const char cc = *d.p++;
    int bytes = -1;
    bool varargs = false;
    d.returns_and_params(&bytes, &varargs);
    if (!d.ok || *d.p)
        return ARGC_UNKNOWN;
    switch (cc) {
    case 'A':
    case 'B': // __cdecl
        return ARGC_CDECL;
    case 'E':
    case 'F': // __thiscall: this in ECX, the callee pops the rest
    case 'G':
    case 'H': // __stdcall
        return varargs ? ARGC_CDECL : pops(bytes);
    default: // __pascal, __fastcall: not measured
        return ARGC_UNKNOWN;
    }
}

const char *imports_ordinal_name(const char *dll, uint32_t ordinal) {
    const std::string k = dll_key(dll);
    if (!is_winsock(k))
        return nullptr;
    if (ordinal >= 1 && ordinal <= 23) {
        if (k == "ws2_32" && ordinal >= 10 && ordinal <= 12)
            return ordinal == 10 ? "ioctlsocket" : ordinal == 11 ? "inet_addr" : "inet_ntoa";
        return kWsock32Ordinals[ordinal];
    }
    if (ordinal >= 51 && ordinal <= 57)
        return kDatabaseOrdinals[ordinal - 51];
    if (ordinal >= 101 && ordinal <= 116)
        return kWsaOrdinals[ordinal - 101];
    if (ordinal == 151)
        return "__WSAFDIsSet";
    return nullptr;
}

uint8_t imports_known_win32_argc(const char *dll, const char *name) {
    if (!name)
        return ARGC_UNKNOWN;
    const std::string k = dll_key(dll);
    const KnownArgc *table = is_winsock(k) ? kWinsock : kWin32;
    const size_t n =
        is_winsock(k) ? sizeof kWinsock / sizeof *kWinsock : sizeof kWin32 / sizeof *kWin32;
    const std::string want = is_winsock(k) ? "" : k;
    const KnownArgc *it =
        std::lower_bound(table, table + n, std::make_pair(want, std::string(name)),
                         [](const KnownArgc &e, const auto &key) {
                             int c = strcmp(e.dll, key.first.c_str());
                             return c < 0 || (c == 0 && strcmp(e.name, key.second.c_str()) < 0);
                         });
    return it != table + n && want == it->dll && strcmp(it->name, name) == 0 ? it->argc
                                                                             : ARGC_UNKNOWN;
}

uint8_t imports_infer_argc(const char *dll, const char *name) {
    if (!name)
        return ARGC_UNKNOWN;
#ifdef RECOMP_IMPORT_ARGC
    for (const GameArgc &g : kGame) {
        const char *bang = strchr(g.key, '!');
        if (bang && strcmp(bang + 1, name) == 0 &&
            dll_key(std::string(g.key, bang).c_str()) == dll_key(dll))
            return g.argc;
    }
#endif
    uint8_t argc = imports_known_win32_argc(dll, name);
    if (argc != ARGC_UNKNOWN)
        return argc;
    if (name[0] == '?')
        return imports_msvc_argc(name);
    const std::string k = dll_key(dll);
    if (is_crt(k)) {
        if (strcmp(name, "_CxxThrowException") == 0)
            return 2;
        if (strcmp(name, "__CxxLongjmpUnwind") == 0)
            return 1;
        return ARGC_CDECL;
    }
    // The C++ runtime's undecorated exports (_Getctype, btowc, ...) are C functions.
    if (k.compare(0, 5, "msvcp") == 0)
        return ARGC_CDECL;
    return ARGC_UNKNOWN;
}

size_t imports_known_win32_count() {
    return sizeof kWin32 / sizeof *kWin32 + sizeof kWinsock / sizeof *kWinsock;
}

bool imports_known_win32_sorted() {
    auto sorted = [](const KnownArgc *t, size_t n) {
        for (size_t i = 1; i < n; ++i) {
            int c = strcmp(t[i - 1].dll, t[i].dll);
            if (c > 0 || (c == 0 && strcmp(t[i - 1].name, t[i].name) >= 0))
                return false;
        }
        return true;
    };
    return sorted(kWin32, sizeof kWin32 / sizeof *kWin32) &&
           sorted(kWinsock, sizeof kWinsock / sizeof *kWinsock);
}
