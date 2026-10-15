// user32_clipboard.cpp - the clipboard, and its text bridged to the platform's.
//
// Windows' rules: one window (or, for OpenClipboard(NULL), one thread) has the
// clipboard open at a time, and opening it again from the same one succeeds;
// EmptyClipboard frees what was there; SetClipboardData hands a GlobalAlloc
// block to the clipboard; GetClipboardData returns the clipboard's own block,
// which a program may read without GlobalLock (GlobalAlloc's handle is the
// block's address here, as GMEM_FIXED's is on Windows, so a program that
// reads the handle as a pointer reads the text). CF_UNICODETEXT, CF_TEXT and
// CF_OEMTEXT stand in for each other: asking for one the program did not set
// gets it converted from another, in code page 1252 or 437.
//
// With a host bridge (host_set_clipboard), text is the platform's too: what a
// program copies (CF_UNICODETEXT, else CF_TEXT) is published when it closes
// the clipboard, and text another application copies is read when a program
// asks for a text format - a paste - and not before, so merely having the
// clipboard change reads nothing. Line ends are CR LF on the Windows side and
// LF on the platform's. Without a bridge the clipboard is the process's own.
#include "codepages.h"
#include "imports.h"
#include "memory.h"
#include "user32_internal.h"
#include "win32.h"

#include <map>
#include <mutex>
#include <string>

namespace {

const uint32_t CF_TEXT = 1, CF_OEMTEXT = 7, CF_UNICODETEXT = 13;
const uint32_t ERROR_ACCESS_DENIED = 5, ERROR_CLIPBOARD_NOT_OPEN = 1418;

struct Clipboard {
    bool open = false;
    uint32_t open_hwnd = 0, open_thread = 0;
    uint32_t owner = 0;                 // the window EmptyClipboard made the owner
    std::map<uint32_t, uint32_t> data;  // format -> GlobalAlloc block (0: delayed)
    std::map<uint32_t, bool> synthetic; // formats converted from another
    bool changed = false;               // set while open; published at CloseClipboard
    // The platform side.
    const HostClipboard *bridge = nullptr;
    uint32_t platform_version = 1, seen_version = 0;
    std::u16string published; // what this process last published (Windows line ends)
};
std::mutex g_lock; // the platform side; the rest is under the baton
Clipboard &cb() {
    static Clipboard c;
    return c;
}

// A block's text: up to its terminator or its end.
std::u16string read_wide(uint32_t block) {
    std::u16string out;
    const uint32_t size = heap_owns(block) ? heap_size(block) : 0;
    for (uint32_t i = 0; i + 1 < size; i += 2) {
        const uint16_t unit = rd16(block + i);
        if (!unit)
            break;
        out.push_back(char16_t(unit));
    }
    return out;
}
std::string read_narrow(uint32_t block) {
    std::string out;
    const uint32_t size = heap_owns(block) ? heap_size(block) : 0;
    for (uint32_t i = 0; i < size; ++i) {
        const uint8_t b = rd8(block + i);
        if (!b)
            break;
        out.push_back(char(b));
    }
    return out;
}
uint32_t new_block(const void *bytes, size_t n, size_t terminator) {
    const uint32_t block = heap_alloc(uint32_t(n + terminator), true);
    if (block && n)
        memcpy(g_mem + block, bytes, n);
    return block;
}
uint32_t wide_block(const std::u16string &text) {
    return new_block(text.data(), text.size() * 2, 2);
}

// The text the clipboard holds in UTF-16, whichever format it came in.
bool clipboard_text(std::u16string *out) {
    Clipboard &c = cb();
    auto it = c.data.find(CF_UNICODETEXT);
    if (it != c.data.end() && it->second && !c.synthetic[CF_UNICODETEXT]) {
        *out = read_wide(it->second);
        return true;
    }
    for (uint32_t fmt : {CF_TEXT, CF_OEMTEXT}) {
        it = c.data.find(fmt);
        if (it == c.data.end() || !it->second || c.synthetic[fmt])
            continue;
        const std::string bytes = read_narrow(it->second);
        std::vector<uint16_t> units(bytes.size());
        codepages::Conversion r = codepages::mb_to_wide(
            fmt == CF_TEXT ? 0 : 1, 0, reinterpret_cast<const uint8_t *>(bytes.data()),
            bytes.size(), units.data(), units.size());
        out->assign(units.begin(), units.begin() + long(r.written));
        return true;
    }
    return false;
}

void free_all() {
    Clipboard &c = cb();
    std::map<uint32_t, bool> freed;
    for (auto &kv : c.data)
        if (kv.second && !freed[kv.second]) {
            freed[kv.second] = true;
            heap_free(kv.second);
        }
    c.data.clear();
    c.synthetic.clear();
}

std::u16string to_windows_lines(const std::u16string &text) {
    std::u16string out;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == u'\n' && (i == 0 || text[i - 1] != u'\r'))
            out.push_back(u'\r');
        out.push_back(text[i]);
    }
    return out;
}
std::string to_platform_lines(const std::string &text) {
    std::string out;
    for (size_t i = 0; i < text.size(); ++i)
        if (!(text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n'))
            out.push_back(text[i]);
    return out;
}

// Before a program reads text formats: when the platform's clipboard changed
// since the last look and holds other text than this process published, that
// text becomes the clipboard's (CF_UNICODETEXT; the rest is synthesised).
void take_platform_text() {
    Clipboard &c = cb();
    const HostClipboard *bridge;
    uint32_t version;
    {
        std::lock_guard<std::mutex> guard(g_lock);
        bridge = c.bridge;
        version = c.platform_version;
    }
    if (!bridge || !bridge->get_text || version == c.seen_version)
        return;
    std::string utf8;
    if (!bridge->get_text(&utf8))
        return; // could not read it now: look again next time
    c.seen_version = version;
    const std::u16string text = to_windows_lines(codepages::utf8_to_utf16(utf8));
    std::u16string current;
    if (text == c.published || (clipboard_text(&current) && current == text))
        return; // what this process put there, or already here
    free_all();
    c.owner = 0;
    if (!text.empty())
        c.data[CF_UNICODETEXT] = wide_block(text);
}

// At CloseClipboard: the text a program copied, to the platform.
void publish() {
    Clipboard &c = cb();
    std::u16string text;
    if (!clipboard_text(&text))
        return;
    c.published = text;
    const HostClipboard *bridge;
    {
        std::lock_guard<std::mutex> guard(g_lock);
        bridge = c.bridge;
    }
    if (bridge && bridge->set_text)
        bridge->set_text(to_platform_lines(codepages::utf16_to_utf8(text)));
}

bool is_text(uint32_t fmt) {
    return fmt == CF_TEXT || fmt == CF_OEMTEXT || fmt == CF_UNICODETEXT;
}

void open_clipboard(X86 *c) {
    Clipboard &k = cb();
    const uint32_t hwnd = arg(c, 0), thread = guest_current_thread_id();
    if (k.open && (k.open_hwnd != hwnd || k.open_thread != thread)) {
        set_last_error(ERROR_ACCESS_DENIED);
        set_eax(c, 0);
        return;
    }
    k.open = true;
    k.open_hwnd = hwnd;
    k.open_thread = thread;
    set_eax(c, 1);
}

void close_clipboard(X86 *c) {
    Clipboard &k = cb();
    if (!k.open || k.open_thread != guest_current_thread_id()) {
        set_last_error(ERROR_CLIPBOARD_NOT_OPEN);
        set_eax(c, 0);
        return;
    }
    k.open = false;
    if (k.changed) {
        k.changed = false;
        publish();
    }
    set_eax(c, 1);
}

void empty_clipboard(X86 *c) {
    Clipboard &k = cb();
    if (!k.open) {
        set_last_error(ERROR_CLIPBOARD_NOT_OPEN);
        set_eax(c, 0);
        return;
    }
    free_all();
    k.owner = k.open_hwnd;
    k.changed = true;
    set_eax(c, 1);
}

// SetClipboardData(format, hMem): the clipboard takes the block (NULL asks
// for delayed rendering, which nothing here renders). A converted stand-in
// for the format gives way to the program's own.
void set_clipboard(X86 *c) {
    Clipboard &k = cb();
    const uint32_t fmt = arg(c, 0), data = arg(c, 1);
    if (!k.open || !fmt) {
        set_last_error(ERROR_CLIPBOARD_NOT_OPEN);
        set_eax(c, 0);
        return;
    }
    auto it = k.data.find(fmt);
    if (it != k.data.end() && it->second && it->second != data)
        heap_free(it->second);
    k.data[fmt] = data;
    k.synthetic[fmt] = false;
    // Converted text formats were made from the old text: drop them.
    if (is_text(fmt))
        for (uint32_t other : {CF_TEXT, CF_OEMTEXT, CF_UNICODETEXT}) {
            auto s = k.data.find(other);
            if (other != fmt && s != k.data.end() && k.synthetic[other]) {
                if (s->second)
                    heap_free(s->second);
                k.data.erase(s);
                k.synthetic.erase(other);
            }
        }
    k.changed = true;
    set_eax(c, data);
}

void get_clipboard(X86 *c) {
    Clipboard &k = cb();
    const uint32_t fmt = arg(c, 0);
    if (!k.open) {
        set_last_error(ERROR_CLIPBOARD_NOT_OPEN);
        set_eax(c, 0);
        return;
    }
    if (is_text(fmt) && !k.changed)
        take_platform_text();
    auto it = k.data.find(fmt);
    if (it != k.data.end()) {
        set_eax(c, it->second);
        return;
    }
    std::u16string text;
    if (!is_text(fmt) || !clipboard_text(&text)) {
        set_eax(c, 0);
        return;
    }
    uint32_t block = 0;
    if (fmt == CF_UNICODETEXT) {
        block = wide_block(text);
    } else {
        std::string bytes(text.size() * 2, '\0');
        codepages::Conversion r = codepages::wide_to_mb(
            fmt == CF_TEXT ? 0 : 1, 0, reinterpret_cast<const uint16_t *>(text.data()), text.size(),
            reinterpret_cast<uint8_t *>(&bytes[0]), bytes.size(), nullptr, false);
        block = new_block(bytes.data(), r.written, 1);
    }
    if (block) {
        k.data[fmt] = block;
        k.synthetic[fmt] = true;
    }
    set_eax(c, block);
}

void clipboard_available(X86 *c) {
    Clipboard &k = cb();
    const uint32_t fmt = arg(c, 0);
    if (k.data.count(fmt)) {
        set_eax(c, 1);
        return;
    }
    if (!is_text(fmt)) {
        set_eax(c, 0);
        return;
    }
    std::u16string text;
    if (clipboard_text(&text)) {
        set_eax(c, 1);
        return;
    }
    // Text another application copied: asked without reading it.
    const HostClipboard *bridge;
    uint32_t version;
    {
        std::lock_guard<std::mutex> guard(g_lock);
        bridge = k.bridge;
        version = k.platform_version;
    }
    set_eax(c, bridge && bridge->has_text && version != k.seen_version && bridge->has_text());
}

void count_clipboard_formats(X86 *c) {
    set_eax(c, uint32_t(cb().data.size()));
}

void get_clipboard_owner(X86 *c) {
    set_eax(c, cb().owner);
}

const ImportShim kClipboardShims[] = {
    {"USER32.dll", "OpenClipboard", 1, open_clipboard},
    {"USER32.dll", "CloseClipboard", 0, close_clipboard},
    {"USER32.dll", "EmptyClipboard", 0, empty_clipboard},
    {"USER32.dll", "SetClipboardData", 2, set_clipboard},
    {"USER32.dll", "GetClipboardData", 1, get_clipboard},
    {"USER32.dll", "IsClipboardFormatAvailable", 1, clipboard_available},
    {"USER32.dll", "CountClipboardFormats", 0, count_clipboard_formats},
    {"USER32.dll", "GetClipboardOwner", 0, get_clipboard_owner},
};

} // namespace

void user32_clipboard_register() {
    imports_register(kClipboardShims, sizeof kClipboardShims / sizeof kClipboardShims[0]);
}

void host_set_clipboard(const HostClipboard *bridge) {
    std::lock_guard<std::mutex> guard(g_lock);
    cb().bridge = bridge;
    ++cb().platform_version;
}

void host_clipboard_changed() {
    std::lock_guard<std::mutex> guard(g_lock);
    ++cb().platform_version;
}
