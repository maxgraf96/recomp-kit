// user32_ansi.cpp - the ANSI entry points that have no wide twin elsewhere
// in the runtime yet, and the odd ones out: class queries, window text,
// strings and bitmaps from any loaded module's resources, MoveWindow,
// ChangeDisplaySettingsA, wsprintfA and SetWindowsHookExA.
//
// ANSI text is code page 1252, the code page the runtime reports (GetACP).
#include "user32_internal.h"
#include "gdi_image.h"
#include "memory.h"
#include "msvcrt_internal.h"
#include "resources.h"
#include "win32.h"

#include <algorithm>
#include <string>

namespace {
using namespace user32;

// WNDCLASS[EX]A for a registered class. The class name pointer handed back is
// the caller's own, as Windows does; the menu name is a resource id or empty.
// `ex` is the WNDCLASSEX form: cbSize first, hIconSm last.
void class_info(X86 *c, bool ex, bool wide) {
    ensure_system_classes();
    const uint32_t name = arg(c, 1), p = arg(c, 2);
    auto it = classes().find(class_key(name, wide));
    const uint32_t need = ex ? 48 : 40;
    if (it == classes().end() || !p || !gm_valid(p, need)) {
        set_last_error(1411); // ERROR_CLASS_DOES_NOT_EXIST
        set_eax(c, 0);
        return;
    }
    const WndClass &w = it->second;
    const uint32_t values[] = {w.style, w.wndproc, w.cls_extra, w.wnd_extra, w.hinstance,
                               w.hicon, w.hcursor, w.hbrush,    w.menu_id,   name};
    const uint32_t at = ex ? 4 : 0;
    for (uint32_t i = 0; i < 10; ++i)
        wr32(p + at + 4 * i, values[i]);
    if (ex)
        wr32(p + 44, w.hicon); // hIconSm: the class icon stands in for it
    set_eax(c, w.atom);
}
void get_class_info_a(X86 *c) {
    class_info(c, false, false);
}
void get_class_info_ex_a(X86 *c) {
    class_info(c, true, false);
}
void get_class_info_ex_w(X86 *c) {
    class_info(c, true, true);
}

// The name the class was registered under, truncated to the buffer.
void get_class_name_a(X86 *c) {
    Window *w = find_window(arg(c, 0));
    if (!w) {
        set_last_error(1400); // ERROR_INVALID_WINDOW_HANDLE
        set_eax(c, 0);
        return;
    }
    auto it = classes().find(w->cls);
    set_eax(c, put_text(arg(c, 1), arg(c, 2), it == classes().end() ? "" : it->second.name, false));
}

// GetWindowTextA asks the window, as Windows does for a window of the calling
// process: WM_GETTEXT, which DefWindowProc answers from the title.
void get_window_text_a(X86 *c) {
    const uint32_t hwnd = arg(c, 0), out = arg(c, 1), cap = arg(c, 2);
    if (!find_window(hwnd)) {
        set_last_error(1400);
        if (out && cap && gm_valid(out, 1))
            wr8(out, 0);
        set_eax(c, 0);
        return;
    }
    if (!out || !cap) {
        set_eax(c, 0);
        return;
    }
    set_eax(c, deliver_message(c, hwnd, 0x000d /* WM_GETTEXT */, cap, out, false));
}

// LoadStringA: the RT_STRING block's UTF-16 string in code page 1252, from
// the module hInstance names - a resource-only language DLL as much as the
// executable. Truncated to the buffer, always terminated; the length copied.
void load_string_a(X86 *c) {
    const uint32_t module = arg(c, 0), id = arg(c, 1) & 0xffff, out = arg(c, 2), cap = arg(c, 3);
    if (!out || !cap || !gm_valid(out, cap)) {
        set_eax(c, 0);
        return;
    }
    wr8(out, 0);
    uint32_t bytes = 0;
    uint32_t p = resource_data_in(module, resource_find_in(module, 6, (id >> 4) + 1), &bytes);
    const uint32_t end = p + bytes;
    for (uint32_t i = 0; p && i < 16; ++i) {
        if (end - p < 2)
            break;
        const uint32_t n = rd16(p);
        p += 2;
        if (n > (end - p) / 2)
            break;
        if (i == (id & 15)) {
            const uint32_t copy = std::min(n, cap - 1);
            for (uint32_t k = 0; k < copy; ++k) {
                const int b = crt::unicode_to_cp1252(rd16(p + 2 * k));
                wr8(out + k, uint8_t(b < 0 ? '?' : b));
            }
            wr8(out + copy, 0);
            set_eax(c, copy);
            return;
        }
        p += n * 2;
    }
    set_eax(c, 0);
}

void load_bitmap_a(X86 *c) {
    uint32_t bytes = 0;
    const uint32_t module = arg(c, 0);
    const uint32_t p = resource_data_in(module, resource_find_in(module, 2, arg(c, 1)), &bytes);
    GdiImage image;
    set_eax(c, p && gdi_decode_image(p, bytes, &image) ? gdi_image_bitmap(image) : 0);
}

// MoveWindow is SetWindowPos with a position and a size and nothing else.
void move_window(X86 *c) {
    Window *w = find_window(arg(c, 0));
    if (!w) {
        set_last_error(1400);
        set_eax(c, 0);
        return;
    }
    const uint32_t flags = 0x0014u | (arg(c, 5) ? 0u : 0x0008u); // NOZORDER|NOACTIVATE[|NOREDRAW]
    set_window_pos(c, w, 0, int32_t(arg(c, 1)), int32_t(arg(c, 2)), int32_t(arg(c, 3)),
                   int32_t(arg(c, 4)), flags, false);
    set_eax(c, 1);
}

// ChangeDisplaySettingsA(DEVMODEA*, flags). The real display is never
// touched: a game's mode belongs to its presentation, which the host scales
// onto whatever the screen is. A mode the display enumeration offers is
// accepted (DISP_CHANGE_SUCCESSFUL), anything else is DISP_CHANGE_BADMODE,
// CDS_TEST only asks, and NULL - going back to the registry mode - always
// succeeds.
void change_display_settings_a(X86 *c) {
    const uint32_t dm = arg(c, 0), flags = arg(c, 1);
    if (!dm) {
        set_eax(c, 0);
        return;
    }
    // DEVMODEA: dmDeviceName[32], then dmSize at 36, dmFields at 40, and
    // dmBitsPerPel/dmPelsWidth/dmPelsHeight at 104/108/112.
    if (!gm_valid(dm, 124) || rd16(dm + 36) < 124) {
        set_eax(c, uint32_t(-5)); // DISP_CHANGE_BADPARAM
        return;
    }
    const uint32_t fields = rd32(dm + 40);
    uint32_t cur_w = 0, cur_h = 0, cur_bpp = 0;
    win32_display_mode(&cur_w, &cur_h, &cur_bpp);
    const uint32_t bpp = fields & 0x40000 ? rd32(dm + 104) : cur_bpp;
    const uint32_t width = fields & 0x80000 ? rd32(dm + 108) : cur_w;
    const uint32_t height = fields & 0x100000 ? rd32(dm + 112) : cur_h;
    bool offered = width == cur_w && height == cur_h && bpp == cur_bpp;
    uint32_t w = 0, h = 0, b = 0;
    for (uint32_t i = 0; !offered && i < 4096 && ddraw_enum_display_mode(i, &w, &h, &b); ++i)
        offered = w == width && h == height && b == bpp;
    LOGV("ChangeDisplaySettingsA(%ux%ux%u, flags=%x): %s", width, height, bpp, flags,
         offered ? "accepted, the display is left alone" : "not an offered mode");
    set_eax(c, offered ? 0 : uint32_t(-2)); // DISP_CHANGE_BADMODE
}

// wsprintfA is cdecl with the arguments after the format on the stack.
void wsprintf_a(X86 *c) {
    set_eax(c, format_wsprintf(arg(c, 0), arg(c, 1), c->r[R_ESP] + 12));
}

// SetWindowsHookExA. A hook for every thread (dwThreadId 0) needs the module
// that holds it, except the low-level ones: Windows refuses it with
// ERROR_HOOK_NEEDS_HMOD, and a game that asks for one with hMod NULL gets
// NULL there too. Thread hooks are recorded and never called: nothing in the
// runtime generates the events they would see.
void set_windows_hook_a(X86 *c) {
    const int32_t kind = int32_t(arg(c, 0));
    const uint32_t proc = arg(c, 1), module = arg(c, 2), thread = arg(c, 3);
    const bool low_level = kind == 13 || kind == 14; // WH_KEYBOARD_LL, WH_MOUSE_LL
    if (!proc || kind < -1 || kind > 14) {
        set_last_error(1426); // ERROR_INVALID_HOOK_FILTER
        set_eax(c, 0);
        return;
    }
    if (!thread && !module && !low_level) {
        set_last_error(1428); // ERROR_HOOK_NEEDS_HMOD
        LOGV("SetWindowsHookExA(%d): a global hook without a module is refused", kind);
        set_eax(c, 0);
        return;
    }
    log_once(("hook:" + std::to_string(kind)).c_str(),
             "SetWindowsHookExA(%d, %08x): recorded, never called", kind, proc);
    set_eax(c, record_hook(uint32_t(kind), proc, module, thread));
}

const ImportShim shims[] = {
    {"USER32.dll", "GetClassInfoA", 3, get_class_info_a},
    {"USER32.dll", "GetClassInfoExA", 3, get_class_info_ex_a},
    {"USER32.dll", "GetClassInfoExW", 3, get_class_info_ex_w},
    {"USER32.dll", "GetClassNameA", 3, get_class_name_a},
    {"USER32.dll", "GetWindowTextA", 3, get_window_text_a},
    {"USER32.dll", "LoadStringA", 4, load_string_a},
    {"USER32.dll", "LoadBitmapA", 2, load_bitmap_a},
    {"USER32.dll", "MoveWindow", 6, move_window},
    {"USER32.dll", "ChangeDisplaySettingsA", 2, change_display_settings_a},
    {"USER32.dll", "wsprintfA", ARGC_CDECL, wsprintf_a},
    {"USER32.dll", "SetWindowsHookExA", 4, set_windows_hook_a},
};
} // namespace

void user32_ansi_register() {
    imports_register(shims, sizeof(shims) / sizeof(shims[0]));
}
