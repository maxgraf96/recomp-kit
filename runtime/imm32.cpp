// imm32.cpp - text from the platform's input method, as a Windows program
// receives it: the IMM32 input context, the WM_IME_* messages and the
// characters a keyboard or an input method delivers to the focus window.
//
// Text reaches the focus window in the window's encoding. A Unicode window
// gets one WM_CHAR per UTF-16 unit. An ANSI window gets WM_CHAR with the code
// page 1252 byte when 1252 spells every character; text with any other
// character arrives the way an IME commits it on Windows: WM_IME_STARTCOMPOSITION,
// WM_IME_COMPOSITION with GCS_RESULTSTR (the program reads the text with
// ImmGetCompositionStringW, or the ANSI form with ImmGetCompositionStringA)
// and WM_IME_ENDCOMPOSITION. A program that leaves WM_IME_COMPOSITION to
// DefWindowProc gets WM_IME_CHAR for each character, and DefWindowProc turns
// WM_IME_CHAR into WM_CHAR, so a window that only reads WM_CHAR still gets
// '?' (or the best-fit byte) for what 1252 cannot spell, as on an English
// Windows with an East Asian IME.
//
// The IME messages are posted, so the result string travels with its
// WM_IME_COMPOSITION: the queued message carries a serial (MSGF_IME_RESULT),
// and retrieving it makes that text the thread's current result, which stays
// until the next one, as an input context keeps its last result.
#include "codepages.h"
#include "imports.h"
#include "user32_internal.h"
#include "win32.h"

#include <map>
#include <mutex>
#include <string>

namespace {

// The input context every window of a thread shares (Windows gives each
// thread a default one, and ImmGetContext of any of its windows returns it).
const uint32_t kDefaultContext = 0x00494d43;

const uint32_t WM_IME_STARTCOMPOSITION = 0x010d, WM_IME_ENDCOMPOSITION = 0x010e,
               WM_IME_COMPOSITION = 0x010f, WM_IME_SETCONTEXT = 0x0281, WM_IME_NOTIFY = 0x0282,
               WM_IME_CHAR = 0x0286, WM_CHAR = 0x0102;
const uint32_t GCS_RESULTSTR = 0x0800, GCS_RESULTCLAUSE = 0x1000;

struct ImeState {
    std::mutex lock;
    uint32_t next_serial = 1;
    std::map<uint32_t, std::u16string> posted;  // serial -> text, not yet retrieved
    std::map<uint32_t, std::u16string> current; // thread -> its context's result
    bool open = false;
};
ImeState &ime() {
    static ImeState s;
    return s;
}

std::u16string current_result() {
    ImeState &s = ime();
    std::lock_guard<std::mutex> guard(s.lock);
    auto it = s.current.find(guest_current_thread_id());
    return it == s.current.end() ? std::u16string() : it->second;
}

// The byte an ANSI window gets for one UTF-16 unit: 1252, best fit, else '?'.
uint8_t ansi_byte(char16_t unit) {
    std::string b = codepages::utf16_to_ansi(std::u16string(1, unit));
    return b.empty() ? uint8_t('?') : uint8_t(b[0]);
}

void push(uint32_t hwnd, uint32_t msg, uint32_t wparam, uint32_t lparam, uint32_t flags = 0) {
    using namespace user32;
    Msg m{hwnd, msg, wparam, lparam, host_millis(), (uint32_t)g_cursor_x, (uint32_t)g_cursor_y};
    m.flags = flags;
    queue_for_thread(window_thread(hwnd)).push_back(m);
}

// -------------------------------------------------------------------------
// IMM32.
// -------------------------------------------------------------------------
void i_ImmGetContext(X86 *c) {
    set_eax(c, user32::find_window(arg(c, 0)) ? kDefaultContext : 0);
}
void i_ImmReleaseContext(X86 *c) {
    set_eax(c, 1);
}
void i_ImmGetOpenStatus(X86 *c) {
    std::lock_guard<std::mutex> guard(ime().lock);
    set_eax(c, arg(c, 0) == kDefaultContext && ime().open ? 1 : 0);
}
void i_ImmSetOpenStatus(X86 *c) {
    if (arg(c, 0) != kDefaultContext) {
        set_eax(c, 0);
        return;
    }
    std::lock_guard<std::mutex> guard(ime().lock);
    ime().open = arg(c, 1) != 0;
    set_eax(c, 1);
}
void i_ImmGetCandidateListA(X86 *c) {
    set_eax(c, 0);
}
void i_ImmSetCompositionWindow(X86 *c) {
    set_eax(c, 1);
}
void i_ImmNotifyIME(X86 *c) {
    set_eax(c, 0);
}

// ImmGetCompositionString(himc, index, buffer, bytes): the size in bytes
// when `bytes` is 0, else what fits, copied, and its size. Only the result
// string exists: the platform's input method composes on its own, so no
// composition string is ever pending.
void get_composition_string(X86 *c, bool wide) {
    const uint32_t himc = arg(c, 0), index = arg(c, 1), buf = arg(c, 2);
    const int32_t len = (int32_t)arg(c, 3);
    if (himc != kDefaultContext) { // no context: nothing to read
        set_eax(c, 0);
        return;
    }
    if (len < 0) {
        set_eax(c, uint32_t(-2)); // IMM_ERROR_GENERAL
        return;
    }
    if (index != GCS_RESULTSTR) {
        set_eax(c, 0);
        return;
    }
    std::u16string text = current_result();
    std::string bytes;
    if (wide)
        bytes.assign(reinterpret_cast<const char *>(text.data()), text.size() * 2);
    else
        bytes = codepages::utf16_to_ansi(text);
    if (len == 0 || !buf) {
        set_eax(c, uint32_t(bytes.size()));
        return;
    }
    uint32_t n = std::min<uint32_t>(uint32_t(len), uint32_t(bytes.size()));
    if (wide)
        n &= ~1u;
    if (!gm_valid(buf, n)) {
        set_eax(c, uint32_t(-2));
        return;
    }
    memcpy(g_mem + buf, bytes.data(), n);
    set_eax(c, n);
}
void i_ImmGetCompositionStringA(X86 *c) {
    get_composition_string(c, false);
}
void i_ImmGetCompositionStringW(X86 *c) {
    get_composition_string(c, true);
}

const ImportShim kImm32Shims[] = {
    {"IMM32.dll", "ImmGetContext", 1, i_ImmGetContext},
    {"IMM32.dll", "ImmReleaseContext", 2, i_ImmReleaseContext},
    {"IMM32.dll", "ImmGetOpenStatus", 1, i_ImmGetOpenStatus},
    {"IMM32.dll", "ImmSetOpenStatus", 2, i_ImmSetOpenStatus},
    {"IMM32.dll", "ImmGetCompositionStringA", 4, i_ImmGetCompositionStringA},
    {"IMM32.dll", "ImmGetCompositionStringW", 4, i_ImmGetCompositionStringW},
    {"IMM32.dll", "ImmGetCandidateListA", 4, i_ImmGetCandidateListA},
    {"IMM32.dll", "ImmSetCompositionWindow", 2, i_ImmSetCompositionWindow},
    {"IMM32.dll", "ImmNotifyIME", 4, i_ImmNotifyIME},
};

} // namespace

void imm32_register() {
    imports_register(kImm32Shims, sizeof kImm32Shims / sizeof kImm32Shims[0]);
}

namespace user32 {

void post_text_to_window(uint32_t hwnd, const std::u16string &text) {
    Window *w = find_window(hwnd);
    if (!w || text.empty())
        return;
    if (w->unicode) {
        for (char16_t unit : text)
            push(hwnd, WM_CHAR, unit, 1);
        return;
    }
    bool spelled = true;
    for (char16_t unit : text)
        spelled = spelled && codepages::unicode_to_ansi(unit) >= 0;
    if (spelled) {
        for (char16_t unit : text)
            push(hwnd, WM_CHAR, uint32_t(codepages::unicode_to_ansi(unit)), 1);
        return;
    }
    uint32_t serial;
    {
        ImeState &s = ime();
        std::lock_guard<std::mutex> guard(s.lock);
        serial = s.next_serial++ & 0xffffff;
        if (!serial)
            serial = s.next_serial++ & 0xffffff;
        s.posted[serial] = text;
    }
    push(hwnd, WM_IME_STARTCOMPOSITION, 0, 0);
    push(hwnd, WM_IME_COMPOSITION, ansi_byte(text.back()), GCS_RESULTSTR | GCS_RESULTCLAUSE,
         MSGF_IME_RESULT | (serial << 8));
    push(hwnd, WM_IME_ENDCOMPOSITION, 0, 0);
}

void ime_message_retrieved(const Msg &m, bool removed) {
    if (!(m.flags & MSGF_IME_RESULT))
        return;
    ImeState &s = ime();
    std::lock_guard<std::mutex> guard(s.lock);
    auto it = s.posted.find(m.flags >> 8);
    if (it == s.posted.end())
        return;
    s.current[window_thread(m.hwnd)] = it->second;
    if (removed)
        s.posted.erase(it);
}

// DefWindowProc's part in IME input. A result string becomes WM_IME_CHAR,
// one per character, sent to the window in the encoding of the DefWindowProc
// that was called; WM_IME_CHAR becomes a posted WM_CHAR (two for a double-byte
// character, lead byte first). The rest is the IME's own windows: nothing.
bool ime_def_window_proc(X86 *c, bool wide) {
    const uint32_t hwnd = arg(c, 0), msg = arg(c, 1), wparam = arg(c, 2), lparam = arg(c, 3);
    switch (msg) {
    case WM_IME_COMPOSITION:
        if (lparam & GCS_RESULTSTR) {
            for (char16_t unit : current_result()) {
                if (!find_window(hwnd))
                    break;
                host_dispatch_to_wndproc(c, hwnd, WM_IME_CHAR, wide ? unit : ansi_byte(unit), 1);
            }
        }
        set_eax(c, 0);
        return true;
    case WM_IME_CHAR:
        if (!wide && (wparam & 0xff00))
            push(hwnd, WM_CHAR, (wparam >> 8) & 0xff, lparam);
        push(hwnd, WM_CHAR, wide ? (wparam & 0xffff) : (wparam & 0xff), lparam);
        set_eax(c, 0);
        return true;
    case WM_IME_STARTCOMPOSITION:
    case WM_IME_ENDCOMPOSITION:
    case WM_IME_SETCONTEXT:
    case WM_IME_NOTIFY:
        set_eax(c, 0);
        return true;
    default:
        return false;
    }
}

} // namespace user32

// Text the platform's input method (or keyboard layout) produced, for the
// window with the keyboard focus.
void host_post_text(const uint16_t *units, size_t count) {
    if (!units || !count)
        return;
    if (uint32_t hwnd = user32::keyboard_window())
        user32::post_text_to_window(
            hwnd, std::u16string(reinterpret_cast<const char16_t *>(units), count));
}
