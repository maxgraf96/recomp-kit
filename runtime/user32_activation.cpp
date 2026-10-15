// user32_activation.cpp - application activation and keyboard focus.
//
// Windows tells an application it has been switched to or away from with a
// fixed sequence of messages, and games hang their device handling on it: a
// DirectInput device in the foreground cooperative level is unacquired when
// the application loses the foreground and is acquired again from the game's
// WM_ACTIVATE or WM_ACTIVATEAPP handler, never automatically. So the sequence,
// its order and its timing are part of the contract, not decoration.
//
// The host reports what the platform says - the window has the focus or not,
// is minimised or not - through host_app_state(). This file turns changes of
// that state into the messages Windows sends:
//
//   activation    WM_ACTIVATEAPP(TRUE) to the thread's top-level windows,
//                 WM_NCACTIVATE(TRUE), WM_ACTIVATE(WA_ACTIVE | minimised << 16),
//                 and DefWindowProc's WM_ACTIVATE gives the window the focus
//                 (WM_SETFOCUS) unless it is minimised.
//   deactivation  WM_NCACTIVATE(FALSE), WM_ACTIVATE(WA_INACTIVE | min << 16),
//                 WM_ACTIVATEAPP(FALSE), WM_KILLFOCUS to the focus window.
//   minimise      WM_SIZE(SIZE_MINIMIZED, 0) before the deactivation.
//   restore       the activation (still minimised), then WM_SIZE(SIZE_RESTORED)
//                 and the focus.
//
// Messages caused by the host are SENT the way the system sends them: queued
// for the window's thread and delivered inside its next PeekMessage or
// GetMessage, never posted where the game's loop would see them. Messages
// caused by the guest itself - showing its first window while the application
// has the focus, which Windows activates inside ShowWindow - are delivered
// synchronously from that call.
//
// A host that never reports focus (the headless hosts) gets none of this and
// keeps the old answers: the application counts as permanently in the
// foreground, so foreground-only devices still acquire.
#include "user32_internal.h"
#include "win32.h"

#include <algorithm>
#include <vector>

namespace user32 {
namespace {

enum : uint32_t {
    WM_SIZE_ = 0x0005,
    WM_ACTIVATE_ = 0x0006,
    WM_SETFOCUS_ = 0x0007,
    WM_KILLFOCUS_ = 0x0008,
    WM_ACTIVATEAPP_ = 0x001c,
    WM_NCACTIVATE_ = 0x0086,
    WS_CHILD_ = 0x40000000u,
    WS_MINIMIZE_ = 0x20000000u,
};

bool g_reported = false;   // a host has reported the focus at least once
bool g_host_focus = false; // the platform says the window has the focus
bool g_minimized = false;  // the platform says the window is minimised
bool g_active = false;     // what the guest has been told
std::vector<void (*)(bool)> &observers() {
    static std::vector<void (*)(bool)> list;
    return list;
}

bool top_level(const Window *w) {
    return w && w->hwnd != desktop_handle && !(w->style & WS_CHILD_) && !w->parent;
}

// Delivers a message the way the activation needs it delivered: straight to
// the window procedure when the caller is the window's own thread inside a
// guest call, otherwise sent through that thread's queue.
struct Sender {
    X86 *c = nullptr;
    void send(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp, uint32_t effect = 0) const {
        Window *w = find_window(hwnd);
        if (!w)
            return;
        if (c && window_thread(hwnd) == guest_current_thread_id()) {
            if (effect)
                apply_window_effect(hwnd, effect);
            host_dispatch_to_wndproc(c, hwnd, msg, wp, lp);
        } else
            notify_message(hwnd, msg, wp, lp, effect);
    }
};

// The window activation goes to: the active window if it can still take it,
// else the topmost visible top-level window.
uint32_t activation_target() {
    Window *w = find_window(active_window());
    if (top_level(w) && w->visible)
        return w->hwnd;
    std::vector<uint32_t> order = window_z_order(0);
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
        Window *candidate = find_window(*it);
        if (top_level(candidate) && candidate->visible)
            return *it;
    }
    return 0;
}

void tell_observers(bool active) {
    for (auto fn : std::vector<void (*)(bool)>(observers()))
        fn(active);
}

// WM_ACTIVATEAPP goes to every top-level window of the thread being switched,
// hidden ones included.
void send_activate_app(const Sender &s, uint32_t target, bool active) {
    const uint32_t thread = window_thread(target);
    std::vector<uint32_t> tops;
    for (const auto &kv : windows())
        if (top_level(&kv.second) && window_thread(kv.first) == thread)
            tops.push_back(kv.first);
    for (uint32_t hwnd : tops)
        s.send(hwnd, WM_ACTIVATEAPP_, active ? 1 : 0, 0);
}

void activate(const Sender &s, uint32_t target, bool minimized) {
    g_active = true;
    set_active_window(target);
    // An application in the background has no focus window; DefWindowProc's
    // WM_ACTIVATE gives the activated window the focus, and WM_SETFOCUS with it.
    set_focus_window(0);
    reorder_window(target, 0);
    tell_observers(true);
    send_activate_app(s, target, true);
    s.send(target, WM_NCACTIVATE_, 1, 0);
    s.send(target, WM_ACTIVATE_, 1u /* WA_ACTIVE */ | (minimized ? 0x10000u : 0u), 0);
}

void deactivate(const Sender &s, bool minimized) {
    const uint32_t target = active_window();
    g_active = false;
    // Foreground-only devices lose their acquisition the moment the platform
    // takes the focus away, before the game has heard about it.
    tell_observers(false);
    if (find_window(target)) {
        s.send(target, WM_NCACTIVATE_, 0, 0);
        s.send(target, WM_ACTIVATE_, 0u /* WA_INACTIVE */ | (minimized ? 0x10000u : 0u), 0);
        send_activate_app(s, target, false);
    }
    const uint32_t focus = focus_window();
    set_focus_window(0);
    if (find_window(focus))
        s.send(focus, WM_KILLFOCUS_, 0, 0);
}

// The window's state changes when the WM_SIZE announcing it is delivered, so
// a procedure handling the messages before it still sees the old state.
void size_message(const Sender &s, uint32_t hwnd, bool minimized) {
    Window *w = find_window(hwnd);
    if (!w)
        return;
    if (minimized)
        s.send(hwnd, WM_SIZE_, 1 /* SIZE_MINIMIZED */, 0, NOTIFY_MINIMIZE);
    else
        s.send(hwnd, WM_SIZE_, 0 /* SIZE_RESTORED */,
               ((uint32_t)(uint16_t)w->h << 16) | (uint16_t)w->w, NOTIFY_RESTORE);
}

// Brings what the guest has been told in line with what the host reported.
// A window the guest minimised itself (CloseWindow, SW_MINIMIZE) is restored
// when the application is activated again, which is what clicking its
// taskbar button does on Windows.
void reconcile(const Sender &s, bool was_minimized) {
    const uint32_t target = activation_target();
    Window *tw = find_window(target);
    const bool guest_minimized = tw && (tw->style & WS_MINIMIZE_) && !was_minimized;
    if (guest_minimized && g_host_focus && !g_active && !g_minimized)
        was_minimized = true;
    const bool restore = was_minimized && !g_minimized;
    const bool minimize = !was_minimized && g_minimized;
    if (minimize && target)
        size_message(s, target, true);
    const bool want = g_host_focus && target != 0;
    if (g_active && !want)
        deactivate(s, g_minimized || minimize);
    else if (!g_active && want)
        activate(s, target, was_minimized);
    if (restore && target) {
        size_message(s, target, false);
        // DefWindowProc gave no focus to a minimised window it activated.
        if (g_active && !find_window(focus_window())) {
            set_focus_window(target);
            s.send(target, WM_SETFOCUS_, 0, 0);
        }
    }
}

} // namespace

// A top-level window became visible. `activate` is false for the show
// commands that leave activation alone (SW_SHOWNA, SW_SHOWNOACTIVATE, ...).
// Windows activates the window from inside ShowWindow when the application
// has the foreground; when another of its windows is active, that one is
// deactivated first.
void activation_window_shown(X86 *c, uint32_t hwnd, bool activate_it) {
    Window *w = find_window(hwnd);
    if (!g_reported || !g_host_focus || !activate_it || !top_level(w) || !w->visible)
        return;
    Sender s{c};
    if (!g_active) {
        activate(s, hwnd, false);
        return;
    }
    const uint32_t old = active_window();
    if (old == hwnd)
        return;
    if (find_window(old)) {
        s.send(old, WM_NCACTIVATE_, 0, 0);
        s.send(old, WM_ACTIVATE_, 0, hwnd);
    }
    set_active_window(hwnd);
    reorder_window(hwnd, 0);
    s.send(hwnd, WM_NCACTIVATE_, 1, 0);
    s.send(hwnd, WM_ACTIVATE_, 1, find_window(old) ? old : 0);
}

// The active window was hidden. Windows activates its owner when that can take
// it, else the topmost other visible top-level window, with WM_NCACTIVATE and
// WM_ACTIVATE to both - never WM_ACTIVATEAPP, the application keeps the
// foreground - and DefWindowProc's WM_ACTIVATE moves the focus. Nothing to
// activate leaves the hidden window active, as the program has no other.
void activation_window_hidden(X86 *c, uint32_t hwnd) {
    Window *w = find_window(hwnd);
    if (!g_reported || !g_active || active_window() != hwnd || !top_level(w))
        return;
    uint32_t next = 0;
    if (Window *owner = find_window(w->owner);
        owner && top_level(owner) && owner->visible && owner->enabled)
        next = owner->hwnd;
    if (!next) {
        std::vector<uint32_t> order = window_z_order(0);
        for (auto it = order.rbegin(); it != order.rend(); ++it) {
            Window *candidate = find_window(*it);
            if (*it != hwnd && top_level(candidate) && candidate->visible && candidate->enabled) {
                next = *it;
                break;
            }
        }
    }
    if (!next)
        return;
    Sender s{c};
    s.send(hwnd, WM_NCACTIVATE_, 0, 0);
    s.send(hwnd, WM_ACTIVATE_, 0u /* WA_INACTIVE */, next);
    set_active_window(next);
    if (find_window(next)) {
        s.send(next, WM_NCACTIVATE_, 1, 0);
        s.send(next, WM_ACTIVATE_, 1u /* WA_ACTIVE */, find_window(hwnd) ? hwnd : 0);
    }
}

// The active window is going away: another visible top-level window of the
// application takes the activation, or nothing does. The messages are queued,
// since the destruction is still running its own.
void activation_window_destroyed(uint32_t hwnd) {
    if (!g_reported || !g_active || active_window() != hwnd)
        return;
    set_active_window(0);
    std::vector<uint32_t> order = window_z_order(0);
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
        Window *w = find_window(*it);
        if (*it == hwnd || !top_level(w) || !w->visible)
            continue;
        set_active_window(*it);
        notify_message(*it, WM_NCACTIVATE_, 1, 0);
        notify_message(*it, WM_ACTIVATE_, 1, 0);
        return;
    }
}

} // namespace user32

using namespace user32;

void host_app_state(bool focused, bool minimized) {
    const bool was_minimized = g_minimized;
    g_reported = true;
    g_host_focus = focused;
    g_minimized = minimized;
    reconcile(Sender{}, was_minimized);
}

uint32_t win32_close_target() {
    const uint32_t main = host_main_window();
    Window *a = find_window(active_window());
    if (!main || !a || a->hwnd == main || !top_level(a) || !a->visible ||
        window_thread(a->hwnd) != window_thread(main))
        return main;
    // Only a window the main window owns, at any depth: an unrelated popup
    // (a splash screen still up) is not what the player is closing.
    const Window *w = a;
    for (size_t hops = 0; w && hops <= windows().size(); ++hops) {
        if (w->owner == main)
            return a->hwnd;
        w = find_window(w->owner);
    }
    return main;
}

bool win32_app_active() {
    return !g_reported || g_active;
}

bool win32_app_focus_reported() {
    return g_reported;
}

void win32_add_activation_observer(void (*fn)(bool active)) {
    if (fn && std::find(observers().begin(), observers().end(), fn) == observers().end())
        observers().push_back(fn);
}

void win32_reset_activation_for_test() {
    g_reported = g_host_focus = g_minimized = g_active = false;
}
