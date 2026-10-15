// platform_clipboard.cpp - see platform_clipboard.h.
#include "platform_clipboard.h"
#include "../../runtime/win32.h"

#include <SDL3/SDL.h>

#include <mutex>
#include <string>

namespace {

std::mutex g_lock;
bool g_pending = false;
std::string g_pending_text;

bool has_text() {
    // Asks for the kind of data only; nothing is read.
    return SDL_IsMainThread() && SDL_HasClipboardText();
}

bool get_text(std::string *out) {
    if (!SDL_IsMainThread()) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            SDL_Log("clipboard: a paste on a guest worker thread cannot read the platform's "
                    "clipboard");
        }
        return false;
    }
    char *text = SDL_GetClipboardText();
    if (!text)
        return false;
    *out = text;
    SDL_free(text);
    return true;
}

void set_text(const std::string &text) {
    if (SDL_IsMainThread()) {
        SDL_SetClipboardText(text.c_str());
        return;
    }
    std::lock_guard<std::mutex> guard(g_lock);
    g_pending = true;
    g_pending_text = text;
}

const HostClipboard kBridge = {has_text, get_text, set_text};

} // namespace

void sdl_clipboard_install() {
    host_set_clipboard(&kBridge);
}

void sdl_clipboard_pump() {
    std::string text;
    {
        std::lock_guard<std::mutex> guard(g_lock);
        if (!g_pending)
            return;
        g_pending = false;
        text.swap(g_pending_text);
    }
    SDL_SetClipboardText(text.c_str());
}
