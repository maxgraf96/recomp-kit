// win32_ui_tests.cpp - a game's window system as it sees it: per-thread
// message queues, sent messages, activation and focus, keyboard translation,
// and the DirectInput keyboard and mouse that hang off activation.
//
// Every call goes through the guest path: arguments pushed on a guest stack
// and the shim reached through imports_dispatch, so a wrong stdcall count
// shows up as a stack mismatch. Window procedures and thread bodies are
// trampolines into native functions here, entered through guest_call exactly
// as translated code would be. No game image is needed.
#include "../com.h"
#include "../dinput_joystick.h"
#include "../dx.h"
#include "../host_api.h"
#include "../../runtime/loader.h"
#include "../../runtime/memory.h"
#include "../../runtime/win32.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int g_checks = 0, g_failures = 0;
static bool check(bool ok, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static bool check(bool ok, const char *fmt, ...) {
    ++g_checks;
    if (ok)
        return true;
    ++g_failures;
    va_list ap;
    va_start(ap, fmt);
    char msg[512];
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    fprintf(stderr, "  FAIL %s\n", msg);
    return false;
}

// ---------------------------------------------------------------------------
// Guest calls
// ---------------------------------------------------------------------------
static uint32_t call_target(X86 *c, uint32_t target, const std::vector<uint32_t> &args) {
    uint32_t esp = c->r[R_ESP];
    const uint32_t before = esp;
    for (size_t i = args.size(); i-- > 0;) {
        esp -= 4;
        wr32(esp, args[i]);
    }
    esp -= 4;
    wr32(esp, 0x00401000u);
    c->r[R_ESP] = esp;
    if (!imports_dispatch(c, target)) {
        check(false, "%08x is not a shim trampoline", target);
        c->r[R_ESP] = before;
        return 0;
    }
    const uint32_t expected =
        imports_argc(target) == ARGC_CDECL ? before - uint32_t(args.size()) * 4 : before;
    check(c->r[R_ESP] == expected, "%s left ESP at %08x, expected %08x",
          imports_describe(target) ? imports_describe(target) : "shim", c->r[R_ESP], expected);
    c->r[R_ESP] = before;
    return c->r[R_EAX];
}
static uint32_t call(X86 *c, const char *dll, const char *name, const std::vector<uint32_t> &args) {
    const uint32_t target = imports_resolve(dll, name);
    if (!target) {
        check(false, "no trampoline for %s!%s", dll, name);
        return 0;
    }
    return call_target(c, target, args);
}
static uint32_t user(X86 *c, const char *name, const std::vector<uint32_t> &args) {
    return call(c, "USER32.dll", name, args);
}
static uint32_t method(X86 *c, uint32_t iface, uint32_t slot, const std::vector<uint32_t> &rest) {
    std::vector<uint32_t> args{iface};
    args.insert(args.end(), rest.begin(), rest.end());
    return call_target(c, rd32(rd32(iface) + slot * 4), args);
}

// A scratch block per caller, so a thread body does not trample the main
// thread's buffers.
static uint32_t g_scratch = 0;
static uint32_t scratch(uint32_t offset) {
    return g_scratch + offset;
}

// ---------------------------------------------------------------------------
// The host's input, as host_input_state reports it to DirectInput.
// ---------------------------------------------------------------------------
static HostInputState g_input;
extern "C" void host_input_state(HostInputState *out) {
    *out = g_input;
    g_input.mouse_dx = g_input.mouse_dy = g_input.mouse_dz = 0;
}

// ---------------------------------------------------------------------------
// A recording window procedure: every message, the thread it ran on, then
// DefWindowProcA - except for WM_USER+1, which answers 0x1234 itself.
// ---------------------------------------------------------------------------
struct Seen {
    uint32_t hwnd, msg, wp, lp, thread;
};
static std::vector<Seen> g_seen;
static void (*g_on_message)(X86 *c, const Seen &s) = nullptr;
static void recording_proc(X86 *c) {
    Seen s{arg(c, 0), arg(c, 1), arg(c, 2), arg(c, 3), guest_current_thread_id()};
    g_seen.push_back(s);
    if (g_on_message)
        g_on_message(c, s);
    if (s.msg == 0x0401) {
        set_eax(c, 0x1234);
        return;
    }
    const uint32_t def = imports_resolve("USER32.dll", "DefWindowProcA");
    set_eax(c, guest_call(c, def, s.hwnd, s.msg, s.wp, s.lp));
}

// The messages the activation tests look at, in the order they ran.
static std::vector<std::pair<uint32_t, uint32_t>> activation_seen(uint32_t hwnd) {
    std::vector<std::pair<uint32_t, uint32_t>> out;
    for (const Seen &s : g_seen)
        if (s.hwnd == hwnd && (s.msg == 0x0005 || s.msg == 0x0006 || s.msg == 0x0007 ||
                               s.msg == 0x0008 || s.msg == 0x001c || s.msg == 0x0086))
            out.push_back({s.msg, s.wp});
    return out;
}
static std::string describe(const std::vector<std::pair<uint32_t, uint32_t>> &v) {
    std::string s;
    char buf[32];
    for (const auto &p : v) {
        snprintf(buf, sizeof buf, "%s%x(%x)", s.empty() ? "" : " ", p.first, p.second);
        s += buf;
    }
    return s.empty() ? "nothing" : s;
}
static bool sequence_is(uint32_t hwnd, const std::vector<std::pair<uint32_t, uint32_t>> &want,
                        const char *what) {
    const auto got = activation_seen(hwnd);
    return check(got == want, "%s: got %s, want %s", what, describe(got).c_str(),
                 describe(want).c_str());
}

static uint32_t g_class_atom = 0;
static uint32_t make_window(X86 *c, bool visible) {
    if (!g_class_atom) {
        const uint32_t wc = scratch(0x100);
        memset(g_mem + wc, 0, 40);
        wr32(wc + 4, imports_alloc_trampoline("test", "recording_proc", recording_proc, 4));
        gm_put_str(wc + 0x40, "WinUiTest", 32);
        wr32(wc + 36, wc + 0x40);
        g_class_atom = user(c, "RegisterClassA", {wc});
        check(g_class_atom != 0, "RegisterClassA");
    }
    return user(c, "CreateWindowExA",
                {0, g_class_atom, 0, visible ? 0x10000000u : 0u, 0, 0, 640, 480, 0, 0, 0, 0});
}

// A message loop's pass over the calling thread's queue: every message is
// retrieved and dispatched (WM_PAINT stays until something validates it), and
// sent messages are delivered on the way.
static std::vector<uint32_t> drain(X86 *c, uint32_t filter = 0) {
    std::vector<uint32_t> got;
    const uint32_t msg = scratch(0x300 + guest_current_thread_id() * 0x40);
    for (int i = 0; i < 1000 && user(c, "PeekMessageA", {msg, filter, 0, 0, 1}); ++i) {
        got.push_back(rd32(msg + 4));
        user(c, "DispatchMessageA", {msg});
    }
    check(got.size() < 1000, "the queue drains");
    return got;
}

static X86 g_main;

// ---------------------------------------------------------------------------
// Activation and focus
// ---------------------------------------------------------------------------
static void test_activation() {
    X86 *c = &g_main;
    win32_reset_activation_for_test();
    const uint32_t hwnd = make_window(c, false);
    check(hwnd != 0, "a hidden window");
    drain(c);
    g_seen.clear();

    // The platform says the application has the focus before it has shown a
    // window: nothing to tell yet.
    host_app_state(true, false);
    drain(c);
    sequence_is(hwnd, {}, "focus before any window is shown");

    // Showing the window activates it from inside ShowWindow, synchronously,
    // and DefWindowProc's WM_ACTIVATE gives it the focus.
    user(c, "ShowWindow", {hwnd, 5 /* SW_SHOW */});
    sequence_is(hwnd, {{0x1c, 1}, {0x86, 1}, {0x06, 1}, {0x07, 0}},
                "ShowWindow activates: WM_ACTIVATEAPP, WM_NCACTIVATE, WM_ACTIVATE, WM_SETFOCUS");
    check(user(c, "GetActiveWindow", {}) == hwnd, "the shown window is active");
    check(user(c, "GetFocus", {}) == hwnd, "and has the focus");
    check(win32_app_active(), "the application is active");
    drain(c);

    // Command-Tab away: the host's report queues the deactivation, which the
    // window's thread receives inside its next message retrieval.
    g_seen.clear();
    host_app_state(false, false);
    check(!win32_app_active(), "the application is in the background at once");
    sequence_is(hwnd, {}, "nothing is delivered before the thread reads its queue");
    const auto posted = drain(c);
    sequence_is(hwnd, {{0x86, 0}, {0x06, 0}, {0x1c, 0}, {0x08, 0}},
                "deactivation: WM_NCACTIVATE, WM_ACTIVATE(WA_INACTIVE), WM_ACTIVATEAPP, "
                "WM_KILLFOCUS");
    bool leaked = false;
    for (uint32_t m : posted)
        leaked |= m == 0x06 || m == 0x1c || m == 0x86 || m == 0x07 || m == 0x08;
    check(!leaked, "sent messages never come out of PeekMessage");
    check(user(c, "GetActiveWindow", {}) == 0, "no active window in the background");
    check(user(c, "GetFocus", {}) == 0, "and no focus");

    // And back.
    g_seen.clear();
    host_app_state(true, false);
    drain(c);
    sequence_is(hwnd, {{0x1c, 1}, {0x86, 1}, {0x06, 1}, {0x07, 0}}, "reactivation");
    g_seen.clear();
    host_app_state(true, false);
    drain(c);
    sequence_is(hwnd, {}, "reporting an unchanged state sends nothing");

    // Command-M while active: the minimise, then the deactivation, flagged.
    g_seen.clear();
    host_app_state(false, true);
    drain(c);
    sequence_is(hwnd, {{0x05, 1}, {0x86, 0}, {0x06, 0x10000}, {0x1c, 0}, {0x08, 0}},
                "minimise: WM_SIZE(SIZE_MINIMIZED), then deactivation with fMinimized");
    check(user(c, "IsIconic", {hwnd}) == 1, "IsIconic while minimised");
    // Restore from the Dock: activated while still minimised (so DefWindowProc
    // gives no focus), then restored, then focused.
    g_seen.clear();
    host_app_state(true, false);
    drain(c);
    sequence_is(hwnd, {{0x1c, 1}, {0x86, 1}, {0x06, 0x10001}, {0x05, 0}, {0x07, 0}},
                "restore: activation with fMinimized, WM_SIZE(SIZE_RESTORED), WM_SETFOCUS");
    check(user(c, "IsIconic", {hwnd}) == 0, "not iconic once restored");

    // Activation messages are sent in order with the input around them: a key
    // queued before the focus loss is read before the deactivation arrives.
    g_seen.clear();
    host_post_key_text(0x0100, 'A', 0x001e0001u, 'a');
    host_app_state(false, false);
    const uint32_t msg = scratch(0x280);
    check(user(c, "PeekMessageA", {msg, 0, 0, 0, 1}) == 1 && rd32(msg + 4) == 0x0100,
          "the key queued before the focus loss comes first");
    sequence_is(hwnd, {}, "and the deactivation has not been delivered yet");
    check(user(c, "PeekMessageA", {msg, 0, 0, 0, 1}) == 1 && rd32(msg + 4) == 0x0102,
          "then its character");
    check(user(c, "PeekMessageA", {msg, 0, 0, 0, 1}) == 0,
          "then nothing to return: the deactivation was delivered on the way");
    sequence_is(hwnd, {{0x86, 0}, {0x06, 0}, {0x1c, 0}, {0x08, 0}}, "the deactivation");
    host_app_state(true, false);
    drain(c);

    // A second top-level window shown while the application is active takes
    // the activation from the first.
    g_seen.clear();
    const uint32_t second = make_window(c, true);
    sequence_is(hwnd, {{0x86, 0}, {0x06, 0}, {0x08, second}},
                "the first window is deactivated (and loses the focus to the second)");
    sequence_is(second, {{0x86, 1}, {0x06, 1}, {0x07, hwnd}},
                "a WS_VISIBLE window is activated by CreateWindowEx, without WM_ACTIVATEAPP");
    check(user(c, "GetActiveWindow", {}) == second, "the new window is active");
    // Destroying it hands the activation back.
    g_seen.clear();
    user(c, "DestroyWindow", {second});
    drain(c);
    sequence_is(hwnd, {{0x86, 1}, {0x06, 1}, {0x07, 0}},
                "destroying the active window activates the other");
    user(c, "DestroyWindow", {hwnd});
    drain(c);
    win32_reset_activation_for_test();
}

// SetFocus sends WM_KILLFOCUS and WM_SETFOCUS, and only on a change.
static void test_set_focus() {
    X86 *c = &g_main;
    const uint32_t a = make_window(c, true), b = make_window(c, true);
    user(c, "SetFocus", {a});
    g_seen.clear();
    check(user(c, "SetFocus", {b}) == a, "SetFocus returns the previous focus");
    std::vector<std::pair<uint32_t, uint32_t>> both;
    for (const Seen &s : g_seen)
        if (s.msg == 0x07 || s.msg == 0x08)
            both.push_back({s.msg, s.hwnd == a ? 0xa : 0xb});
    check(both == std::vector<std::pair<uint32_t, uint32_t>>{{0x08, 0xa}, {0x07, 0xb}},
          "WM_KILLFOCUS to the old window, then WM_SETFOCUS to the new: %s",
          describe(both).c_str());
    g_seen.clear();
    user(c, "SetFocus", {b});
    check(g_seen.empty(), "setting the same focus again sends nothing");
    user(c, "DestroyWindow", {a});
    user(c, "DestroyWindow", {b});
    drain(c);
}

// ---------------------------------------------------------------------------
// Keyboard translation
// ---------------------------------------------------------------------------
static void test_translate() {
    X86 *c = &g_main;
    const uint32_t hwnd = make_window(c, true);
    user(c, "SetFocus", {hwnd});
    drain(c);
    const uint32_t msg = scratch(0x280);

    // A key the host translated: its character is already queued behind it,
    // so TranslateMessage reports the key translated and makes no second one.
    host_post_key_text(0x0100, 'A', 0x001e0001u, 'A');
    host_post_key_message(0x0101, 'A', 0xc01e0001u);
    check(user(c, "PeekMessageA", {msg, 0, 0, 0, 1}) && rd32(msg + 4) == 0x0100, "WM_KEYDOWN");
    check(user(c, "TranslateMessage", {msg}) == 1, "TranslateMessage reports it translated");
    check(user(c, "PeekMessageA", {msg, 0, 0, 0, 1}) && rd32(msg + 4) == 0x0102 &&
              rd32(msg + 8) == 'A',
          "one WM_CHAR, with the host's character");
    check(user(c, "PeekMessageA", {msg, 0, 0, 0, 1}) && rd32(msg + 4) == 0x0101,
          "then WM_KEYUP, and no second WM_CHAR before it");
    check(user(c, "PeekMessageA", {msg, 0, 0, 0, 1}) == 0, "nothing else");

    // An ANSI window gets code page 1252: e-acute is 0xe9, the euro 0x80, and
    // a character 1252 cannot spell makes none at all.
    host_post_key_text(0x0100, 0xde, 0x00280001u, 0xe9);
    host_post_key_text(0x0100, 'E', 0x00120001u, 0x20ac);
    host_post_key_text(0x0100, 'Z', 0x002c0001u, 0x4e2d);
    std::vector<uint32_t> chars;
    for (int i = 0; i < 100 && user(c, "PeekMessageA", {msg, 0, 0, 0, 1}); ++i)
        if (rd32(msg + 4) == 0x0102)
            chars.push_back(rd32(msg + 8));
    check(chars == std::vector<uint32_t>{0xe9, 0x80}, "WM_CHAR in code page 1252 (%zu)",
          chars.size());

    // A key the guest posts itself is translated from the US layout, with the
    // keyboard state, and its character comes before the key-up queued after it.
    user(c, "PostMessageA", {hwnd, 0x0100, 'B', 0x00300001u});
    user(c, "PostMessageA", {hwnd, 0x0101, 'B', 0xc0300001u});
    check(user(c, "PeekMessageA", {msg, 0, 0, 0, 1}) && rd32(msg + 4) == 0x0100, "WM_KEYDOWN B");
    check(user(c, "TranslateMessage", {msg}) == 1, "translated");
    check(user(c, "PeekMessageA", {msg, 0, 0, 0, 1}) && rd32(msg + 4) == 0x0102 &&
              rd32(msg + 8) == 'b',
          "WM_CHAR 'b' comes next");
    check(user(c, "PeekMessageA", {msg, 0, 0, 0, 1}) && rd32(msg + 4) == 0x0101,
          "and only then WM_KEYUP");
    host_set_key_state(0x10, true);
    user(c, "PostMessageA", {hwnd, 0x0100, '1', 0x00020001u});
    user(c, "PeekMessageA", {msg, 0, 0, 0, 1});
    user(c, "TranslateMessage", {msg});
    check(user(c, "PeekMessageA", {msg, 0, 0, 0, 1}) && rd32(msg + 8) == '!',
          "Shift+1 is '!' on the US layout");
    host_set_key_state(0x10, false);
    host_set_key_state(0x11, true);
    user(c, "PostMessageA", {hwnd, 0x0100, 'C', 0x002e0001u});
    user(c, "PeekMessageA", {msg, 0, 0, 0, 1});
    user(c, "TranslateMessage", {msg});
    check(user(c, "PeekMessageA", {msg, 0, 0, 0, 1}) && rd32(msg + 8) == 0x03, "Ctrl+C is 0x03");
    host_set_key_state(0x11, false);
    user(c, "PostMessageA", {hwnd, 0x0100, 0x25 /* VK_LEFT */, 0x014b0001u});
    user(c, "PeekMessageA", {msg, 0, 0, 0, 1});
    check(user(c, "TranslateMessage", {msg}) == 0, "an arrow key makes no character");
    user(c, "PostMessageA", {hwnd, 0x0104, 'F', 0x20210001u});
    user(c, "PeekMessageA", {msg, 0, 0, 0, 1});
    user(c, "TranslateMessage", {msg});
    check(user(c, "PeekMessageA", {msg, 0, 0, 0, 1}) && rd32(msg + 4) == 0x0106 &&
              rd32(msg + 8) == 'f',
          "Alt+F makes WM_SYSCHAR");
    drain(c);

    // Alt+F4: DefWindowProc turns it into WM_SYSCOMMAND(SC_CLOSE), and that
    // into WM_CLOSE.
    g_seen.clear();
    wr32(msg, hwnd);
    wr32(msg + 4, 0x0104);
    wr32(msg + 8, 0x73);
    wr32(msg + 12, 0x203e0001u);
    user(c, "DispatchMessageA", {msg});
    std::vector<uint32_t> order;
    for (const Seen &s : g_seen)
        order.push_back(s.msg);
    check(order.size() >= 3 && order[0] == 0x0104 && order[1] == 0x0112 && order[2] == 0x0010,
          "Alt+F4 -> WM_SYSCOMMAND -> WM_CLOSE");
    check(user(c, "IsWindow", {hwnd}) == 0, "and DefWindowProc's WM_CLOSE destroyed it");
    drain(c);
}

// ---------------------------------------------------------------------------
// Threads: one queue each, thread messages, and SendMessage across them
// ---------------------------------------------------------------------------
static uint32_t g_main_window = 0, g_worker_window = 0, g_worker_id = 0;
static uint32_t g_send_result = 0;
static int g_stage = 0;
static std::vector<uint32_t> g_worker_got;

static void worker_body(X86 *c) {
    g_worker_id = guest_current_thread_id();
    g_worker_window = make_window(c, false);
    // Post to the main thread's window and to the main thread itself.
    user(c, "PostMessageA", {g_main_window, 0x8001, 1, 2});
    user(c, "PostThreadMessageA", {1, 0x8002, 3, 4});
    g_stage = 1;
    // A message SENT to the main thread's window: this waits until the main
    // thread retrieves messages, and runs the window procedure there.
    g_send_result = user(c, "SendMessageA", {g_main_window, 0x0401, 5, 6});
    g_stage = 2;
    // Wait for what the main thread posts here, then read it with filters.
    while (g_stage < 3)
        call(c, "KERNEL32.dll", "Sleep", {1});
    const uint32_t msg = scratch(0x600);
    if (user(c, "PeekMessageA", {msg, 0xffffffffu, 0, 0, 1}))
        g_worker_got.push_back(rd32(msg + 4)); // the thread message only
    for (int i = 0; i < 100 && user(c, "PeekMessageA", {msg, 0, 0, 0, 1}); ++i)
        g_worker_got.push_back(rd32(msg + 4));
    user(c, "DestroyWindow", {g_worker_window});
    g_stage = 4;
    set_eax(c, 0);
}

static void test_threads() {
    X86 *c = &g_main;
    g_main_window = make_window(c, false);
    drain(c);
    g_seen.clear();
    const uint32_t body = imports_alloc_trampoline("test", "worker_body", worker_body, 1);
    const uint32_t th = call(c, "KERNEL32.dll", "CreateThread", {0, 0, body, 0, 0, 0});
    check(th != 0, "CreateThread");
    for (int i = 0; i < 200 && g_stage < 1; ++i)
        call(c, "KERNEL32.dll", "Sleep", {1});
    check(g_stage == 1, "the worker posted and is now sending");
    check(g_worker_id != 1 && g_worker_window != 0, "the worker has its own window");

    // The main thread sees only its own messages: the worker's window and its
    // thread messages are not here.
    std::vector<uint32_t> got;
    const uint32_t msg = scratch(0x280);
    for (int i = 0; i < 200 && g_stage < 2; ++i) {
        for (int n = 0; n < 100 && user(c, "PeekMessageA", {msg, 0, 0, 0, 1}); ++n) {
            got.push_back(rd32(msg + 4));
            check(rd32(msg) == g_main_window || rd32(msg) == 0,
                  "a message for another thread's window came out of this queue");
        }
        call(c, "KERNEL32.dll", "Sleep", {1});
    }
    check(g_stage == 2, "the sent message was delivered and the worker resumed");
    check(g_send_result == 0x1234, "SendMessage returned the window procedure's answer (%x)",
          g_send_result);
    bool on_main = false;
    for (const Seen &s : g_seen)
        if (s.msg == 0x0401)
            on_main = s.thread == 1 && s.wp == 5 && s.lp == 6;
    check(on_main, "the window procedure ran on the window's own thread");
    bool posted = false, thread_msg = false, sent_leaked = false;
    for (uint32_t m : got) {
        posted |= m == 0x8001;
        thread_msg |= m == 0x8002;
        sent_leaked |= m == 0x0401;
    }
    check(posted && thread_msg, "PostMessage and PostThreadMessage reached the main thread");
    check(!sent_leaked, "the sent message was never returned by PeekMessage");

    // Now the other way round.
    user(c, "PostMessageA", {g_worker_window, 0x8003, 0, 0});
    check(user(c, "PostThreadMessageA", {g_worker_id, 0x8004, 0, 0}) == 1,
          "PostThreadMessage to a live thread");
    g_stage = 3;
    for (int i = 0; i < 200 && g_stage < 4; ++i)
        call(c, "KERNEL32.dll", "Sleep", {1});
    check(g_stage == 4, "the worker read its queue");
    // The worker's own window's WM_MOVE and WM_SIZE from its creation are
    // queued there too, ahead of what the main thread posted.
    check(!g_worker_got.empty() && g_worker_got[0] == 0x8004,
          "a filter of -1 took the thread message first, past the window messages");
    check(g_worker_got.size() >= 2 && g_worker_got.back() == 0x8003,
          "then the window message the main thread posted");
    check(call(c, "KERNEL32.dll", "WaitForSingleObject", {th, 2000}) == 0, "the worker finished");
    check(user(c, "PostThreadMessageA", {g_worker_id, 0x8005, 0, 0}) == 0,
          "PostThreadMessage to a finished thread fails");
    check(call(c, "KERNEL32.dll", "GetLastError", {}) == 1444, "with ERROR_INVALID_THREAD_ID");
    check(drain(c).empty(), "nothing for the worker leaked into the main queue");
    user(c, "DestroyWindow", {g_main_window});
    drain(c);
}

// Timers fire on the queue of the window's thread, and SetCursor(NULL) from
// WM_SETCURSOR is what a game drawing its own cursor does.
static void test_timer_and_cursor() {
    X86 *c = &g_main;
    const uint32_t hwnd = make_window(c, true);
    drain(c);
    check(user(c, "SetTimer", {hwnd, 7, 10, 0}) == 7, "SetTimer");
    bool fired = false;
    for (int i = 0; i < 100 && !fired; ++i) {
        for (uint32_t m : drain(c))
            fired |= m == 0x0113;
        call(c, "KERNEL32.dll", "Sleep", {2});
    }
    check(fired, "WM_TIMER arrived");
    user(c, "KillTimer", {hwnd, 7});

    g_seen.clear();
    host_post_mouse_message(0x0200, 0, 10, 10);
    drain(c);
    bool setcursor_first = false;
    for (size_t i = 0; i + 1 < g_seen.size(); ++i)
        if (g_seen[i].msg == 0x0020 && g_seen[i].wp == hwnd &&
            g_seen[i].lp == ((0x0200u << 16) | 1) && g_seen[i + 1].msg == 0x0200)
            setcursor_first = true;
    check(setcursor_first, "WM_SETCURSOR(HTCLIENT, WM_MOUSEMOVE) precedes WM_MOUSEMOVE");
    user(c, "DestroyWindow", {hwnd});
    drain(c);
}

// ---------------------------------------------------------------------------
// The ANSI odds and ends a Windows game of the time imports
// ---------------------------------------------------------------------------
static void test_ansi_entry_points() {
    X86 *c = &g_main;
    const uint32_t hwnd = make_window(c, false);
    drain(c);
    const uint32_t buf = scratch(0xc00), name = scratch(0xd00);

    gm_put_str(name, "winuitest", 32); // class names match without case
    gm_zero(buf, 48);
    wr32(buf, 48);
    check(user(c, "GetClassInfoExA", {0, name, buf}) != 0, "GetClassInfoExA finds the class");
    check(rd32(buf + 8) != 0 && rd32(buf + 40) == name,
          "WNDCLASSEXA: the window procedure, and the caller's name pointer");
    gm_put_str(name, "NoSuchClass", 32);
    check(user(c, "GetClassInfoExA", {0, name, buf}) == 0 &&
              call(c, "KERNEL32.dll", "GetLastError", {}) == 1411,
          "an unregistered class: FALSE and ERROR_CLASS_DOES_NOT_EXIST");
    gm_zero(buf, 40);
    gm_put_str(name, "WinUiTest", 32);
    check(user(c, "GetClassInfoA", {0, name, buf}) != 0 && rd32(buf + 4) != 0,
          "GetClassInfoA: WNDCLASSA with the window procedure");
    check(user(c, "GetClassNameA", {hwnd, buf, 64}) == 9 && gm_str(buf) == "WinUiTest",
          "GetClassNameA");

    gm_put_str(name, "Empire", 32);
    user(c, "SetWindowTextA", {hwnd, name});
    check(user(c, "GetWindowTextA", {hwnd, buf, 64}) == 6 && gm_str(buf) == "Empire",
          "GetWindowTextA reads the title back");
    check(user(c, "GetWindowTextA", {hwnd, buf, 4}) == 3 && gm_str(buf) == "Emp",
          "and truncates to the buffer");

    g_seen.clear();
    check(user(c, "MoveWindow", {hwnd, 10, 20, 300, 200, 1}) == 1, "MoveWindow");
    user(c, "GetWindowRect", {hwnd, buf});
    check(rd32(buf) == 10 && rd32(buf + 4) == 20 && rd32(buf + 8) == 310 && rd32(buf + 12) == 220,
          "the window moved and resized");
    bool moved = false, sized = false;
    for (const Seen &s : g_seen) {
        moved |= s.msg == 0x0003 && s.lp == ((20u << 16) | 10u);
        sized |= s.msg == 0x0005 && s.lp == ((200u << 16) | 300u);
    }
    check(moved && sized, "WM_MOVE and WM_SIZE from DefWindowProc's WM_WINDOWPOSCHANGED");

    gm_put_str(name, "%d-%s-%04x|%c", 32);
    gm_put_str(name + 0x40, "ok", 8);
    check(user(c, "wsprintfA", {buf, name, uint32_t(-7), name + 0x40, 0xab, 'Z'}) == 12 &&
              gm_str(buf) == "-7-ok-00ab|Z",
          "wsprintfA (cdecl, varargs): %s", gm_str(buf).c_str());

    const uint32_t hook = imports_alloc_trampoline("test", "hook_proc", recording_proc, 3);
    check(user(c, "SetWindowsHookExA", {4 /* WH_CALLWNDPROC */, hook, 0, 0}) == 0 &&
              call(c, "KERNEL32.dll", "GetLastError", {}) == 1428,
          "a global hook without a module fails with ERROR_HOOK_NEEDS_HMOD, as on Windows");
    const uint32_t h = user(c, "SetWindowsHookExA", {4, hook, 0, 1});
    check(h != 0, "a thread hook is accepted");
    check(user(c, "UnhookWindowsHookEx", {h}) == 1, "and can be removed");

    check(user(c, "ChangeDisplaySettingsA", {0, 0}) == 0, "ChangeDisplaySettings(NULL) restores");
    gm_zero(buf, 156);
    wr16(buf + 36, 156);
    wr32(buf + 40, 0x1c0000); // BITSPERPEL | PELSWIDTH | PELSHEIGHT
    uint32_t w = 0, hh = 0, bpp = 0;
    win32_display_mode(&w, &hh, &bpp);
    wr32(buf + 104, bpp);
    wr32(buf + 108, w);
    wr32(buf + 112, hh);
    check(user(c, "ChangeDisplaySettingsA", {buf, 4 /* CDS_FULLSCREEN */}) == 0,
          "the current mode is accepted");
    wr32(buf + 108, 12345);
    check(user(c, "ChangeDisplaySettingsA", {buf, 4}) == 0xfffffffeu,
          "a mode nobody offers is DISP_CHANGE_BADMODE");
    check(user(c, "CreateCursor", {0, 0, 0, 32, 32, buf, buf}) != 0, "CreateCursor");
    check(user(c, "LoadStringA", {0, 1, buf, 16}) == 0 && rd8(buf) == 0,
          "LoadStringA without a string resource: 0 and an empty buffer");
    user(c, "DestroyWindow", {hwnd});
    drain(c);
}

// ---------------------------------------------------------------------------
// DirectInput: foreground, exclusive, buffers, objects
// ---------------------------------------------------------------------------
enum {
    DID_GetProperty = 5,
    DID_SetProperty = 6,
    DID_Acquire = 7,
    DID_Unacquire = 8,
    DID_GetDeviceState = 9,
    DID_GetDeviceData = 10,
    DID_SetDataFormat = 11,
    DID_SetCooperativeLevel = 13,
    DID_GetObjectInfo = 14,
};
static uint32_t g_kb = 0, g_mouse = 0;

static uint32_t make_device(X86 *c, uint32_t di, bool mouse, uint32_t buffer) {
    static const uint8_t KBD[16] = {0x61, 0x2B, 0x1D, 0x6F, 0xA0, 0xD5, 0xCF, 0x11,
                                    0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00};
    const uint32_t guid = scratch(0x800);
    for (uint32_t i = 0; i < 16; ++i)
        wr8(guid + i, KBD[i] - (mouse && i == 0 ? 1 : 0));
    check(method(c, di, 3 /* CreateDevice */, {guid, scratch(0x810), 0}) == DI_OK, "CreateDevice");
    const uint32_t dev = rd32(scratch(0x810));
    const uint32_t df = scratch(0x820);
    gm_zero(df, 24);
    wr32(df, 24);
    wr32(df + 4, 16);
    wr32(df + 8, 1);
    wr32(df + 12, mouse ? 16 : 256);
    check(method(c, dev, DID_SetDataFormat, {df}) == DI_OK, "SetDataFormat");
    const uint32_t prop = scratch(0x840);
    gm_zero(prop, 20);
    wr32(prop, 20);
    wr32(prop + 4, 16);
    wr32(prop + 16, buffer);
    check(method(c, dev, DID_SetProperty, {1 /* DIPROP_BUFFERSIZE */, prop}) == DI_OK,
          "DIPROP_BUFFERSIZE");
    return dev;
}

// The game's WM_ACTIVATE handler: re-acquire on activation, as EE's does.
static uint32_t g_reacquired = 0xffffffffu;
static void reacquire_on_activate(X86 *c, const Seen &s) {
    if (s.msg == 0x0006 && (s.wp & 0xffff) != 0)
        g_reacquired = method(c, g_mouse, DID_Acquire, {}) | method(c, g_kb, DID_Acquire, {});
}

static void test_dinput_foreground() {
    X86 *c = &g_main;
    dinput_reset();
    win32_reset_activation_for_test();
    memset(&g_input, 0, sizeof g_input);
    const uint32_t create = imports_resolve("DINPUT.dll", "DirectInputCreateA");
    check(call_target(c, create, {0x400000, 0x0700, scratch(0x880), 0}) == DI_OK,
          "DirectInputCreateA(0x700)");
    const uint32_t di = rd32(scratch(0x880));
    g_kb = make_device(c, di, false, 256);
    g_mouse = make_device(c, di, true, 256);
    const uint32_t hwnd = make_window(c, false);

    check(method(c, g_kb, DID_SetCooperativeLevel, {hwnd, 3}) == DIERR_INVALIDPARAM,
          "exclusive and non-exclusive at once is invalid");
    check(method(c, g_kb, DID_SetCooperativeLevel, {hwnd, 9}) == DIERR_UNSUPPORTED,
          "the keyboard cannot be exclusive in the background");
    check(method(c, g_kb, DID_SetCooperativeLevel, {hwnd, 6}) == DI_OK,
          "keyboard NONEXCLUSIVE | FOREGROUND");
    check(method(c, g_mouse, DID_SetCooperativeLevel, {hwnd, 5}) == DI_OK,
          "mouse EXCLUSIVE | FOREGROUND");

    // No host has reported the focus yet: the application counts as in front.
    check(method(c, g_mouse, DID_Acquire, {}) == DI_OK, "acquire before any focus report");
    check(method(c, g_mouse, DID_SetCooperativeLevel, {hwnd, 6}) == DIERR_ACQUIRED,
          "the level cannot change while acquired");
    check(dinput_mouse_exclusive(), "an exclusive mouse is acquired");
    method(c, g_mouse, DID_Unacquire, {});
    check(!dinput_mouse_exclusive(), "and is not once unacquired");

    // The host reports the focus; the window is shown and activated.
    host_app_state(true, false);
    user(c, "ShowWindow", {hwnd, 5});
    drain(c);
    check(method(c, g_mouse, DID_Acquire, {}) == DI_OK && method(c, g_kb, DID_Acquire, {}) == DI_OK,
          "both acquire in the foreground");

    // With the mouse exclusive, Windows sends the window no mouse messages.
    host_post_mouse_message(0x0201, 1, 5, 5);
    bool mouse_msg = false;
    for (uint32_t m : drain(c))
        mouse_msg |= m >= 0x0200 && m <= 0x0209;
    check(!mouse_msg, "no WM_LBUTTONDOWN while the mouse is exclusive");

    // Focus lost: both foreground devices are unacquired on the spot.
    host_app_state(false, false);
    const uint32_t state = scratch(0x900);
    check(method(c, g_mouse, DID_GetDeviceState, {16, state}) == DIERR_NOTACQUIRED,
          "the mouse reports DIERR_NOTACQUIRED after the focus loss");
    wr32(scratch(0x940), 256);
    check(method(c, g_kb, DID_GetDeviceData, {16, scratch(0x1000), scratch(0x940), 0}) ==
              DIERR_NOTACQUIRED,
          "and so does the keyboard");
    check(method(c, g_kb, DID_Acquire, {}) == DIERR_OTHERAPPHASPRIO,
          "acquiring in the background is refused");
    drain(c);

    // Focus back: nothing re-acquires by itself; the game does it from its
    // WM_ACTIVATE handler, and there it succeeds.
    host_app_state(true, false);
    check(method(c, g_mouse, DID_GetDeviceState, {16, state}) == DIERR_NOTACQUIRED,
          "still unacquired until the game acts");
    g_on_message = reacquire_on_activate;
    g_reacquired = 0xffffffffu;
    drain(c);
    g_on_message = nullptr;
    check(g_reacquired == DI_OK, "re-acquired from WM_ACTIVATE (%x)", g_reacquired);
    check(method(c, g_mouse, DID_GetDeviceState, {16, state}) == DI_OK, "and readable again");

    user(c, "DestroyWindow", {hwnd});
    drain(c);
    win32_reset_activation_for_test();
}

static void test_dinput_buffers() {
    X86 *c = &g_main;
    // Leaves g_kb and g_mouse acquired from the previous test.
    const uint32_t count = scratch(0x940), buf = scratch(0x1000);
    wr32(count, 0xffffffffu);
    method(c, g_kb, DID_GetDeviceData, {16, 0, count, 0}); // flush
    // Every key goes down, then every key comes up, with no read in between:
    // 510 transitions for a 256-entry buffer.
    for (int k = 1; k < 256; ++k)
        g_input.keys[k] = 0x80;
    wr32(count, 0);
    method(c, g_kb, DID_GetDeviceData, {16, buf, count, 0}); // samples, reads nothing
    for (int k = 1; k < 256; ++k)
        g_input.keys[k] = 0;
    wr32(count, 256);
    const uint32_t hr = method(c, g_kb, DID_GetDeviceData, {16, buf, count, 0});
    check(rd32(count) == 255, "a 256-entry buffer holds 255 records, never a full 256 (%u)",
          rd32(count));
    check(hr == DI_BUFFEROVERFLOW, "and says the rest were lost (%x)", hr);
    check(rd32(buf) == 1 && rd32(buf + 4) == 0x80, "the oldest record survives");
    wr32(count, 256);
    check(method(c, g_kb, DID_GetDeviceData, {16, buf, count, 0}) == DI_OK && rd32(count) == 0,
          "the overflow is reported once");

    // The mouse: granularity per axis, and the wheel in notches.
    const uint32_t prop = scratch(0x980);
    auto granularity = [&](uint32_t ofs, uint32_t *out) {
        gm_zero(prop, 20);
        wr32(prop, 20);
        wr32(prop + 4, 16);
        wr32(prop + 8, ofs);
        wr32(prop + 12, 1 /* DIPH_BYOFFSET */);
        uint32_t r = method(c, g_mouse, DID_GetProperty, {3 /* DIPROP_GRANULARITY */, prop});
        *out = rd32(prop + 16);
        return r;
    };
    uint32_t g = 0;
    check(granularity(0, &g) == DI_OK && g == 1, "X granularity 1");
    check(granularity(4, &g) == DI_OK && g == 1, "Y granularity 1");
    check(granularity(8, &g) == DI_OK && g == 120, "wheel granularity 120 (%u)", g);
    check(granularity(0xf, &g) == DIERR_OBJECTNOTFOUND, "no fourth button");

    const uint32_t info = scratch(0xa00);
    struct {
        uint32_t ofs, type;
        const char *name;
    } objects[] = {{0, 0x001, "X-axis"},     {4, 0x101, "Y-axis"},     {8, 0x201, "Wheel"},
                   {0xc, 0x004, "Button 0"}, {0xd, 0x104, "Button 1"}, {0xe, 0x204, "Button 2"}};
    for (const auto &o : objects) {
        gm_zero(info, 0x13c);
        wr32(info, 0x13c);
        check(method(c, g_mouse, DID_GetObjectInfo, {info, o.ofs, 1}) == DI_OK &&
                  rd32(info + 20) == o.ofs && rd32(info + 24) == o.type &&
                  gm_str(info + 32) == o.name,
              "GetObjectInfo(%x): %s", o.ofs, o.name);
    }
    wr32(info, 0x13c);
    check(method(c, g_mouse, DID_GetObjectInfo, {info, 0xf, 1}) == DIERR_OBJECTNOTFOUND,
          "GetObjectInfo of the fourth button: DIERR_OBJECTNOTFOUND");
    wr32(info, 0x13c);
    check(method(c, g_kb, DID_GetObjectInfo, {info, 0x1e, 1}) == DI_OK && rd32(info + 20) == 0x1e,
          "a keyboard key by offset");

    // Motion and the wheel reach both the buffer and the immediate state.
    wr32(count, 0xffffffffu);
    method(c, g_mouse, DID_GetDeviceData, {16, 0, count, 0});
    const uint32_t state = scratch(0x900);
    method(c, g_mouse, DID_GetDeviceState, {16, state});
    g_input.mouse_dx = 3;
    g_input.mouse_dz = -120;
    wr32(count, 16);
    check(method(c, g_mouse, DID_GetDeviceData, {16, buf, count, 0}) == DI_OK && rd32(count) == 2,
          "one record per axis that moved");
    check(rd32(buf) == 0 && rd32(buf + 4) == 3 && rd32(buf + 16) == 8 &&
              (int32_t)rd32(buf + 20) == -120,
          "X +3 and one wheel notch down");
    check(method(c, g_mouse, DID_GetDeviceState, {16, state}) == DI_OK && rd32(state) == 3 &&
              (int32_t)rd32(state + 8) == -120,
          "the immediate state saw the same motion");
    method(c, g_mouse, DID_Unacquire, {});
    method(c, g_kb, DID_Unacquire, {});
    dinput_reset();
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    mem_init();
    imports_init();
    dx_register_shims();
    loader_init_context(&g_main);
    sched_set_guest_thread(true);
    g_scratch = heap_alloc(0x4000, true, 16);
    if (!g_scratch) {
        fprintf(stderr, "cannot allocate scratch\n");
        return 1;
    }
    struct {
        const char *name;
        void (*fn)();
    } tests[] = {
        {"activation and focus", test_activation},
        {"SetFocus", test_set_focus},
        {"keyboard translation", test_translate},
        {"per-thread queues and cross-thread SendMessage", test_threads},
        {"timers and WM_SETCURSOR", test_timer_and_cursor},
        {"ANSI entry points", test_ansi_entry_points},
        {"DirectInput foreground and exclusive mouse", test_dinput_foreground},
        {"DirectInput buffers and objects", test_dinput_buffers},
    };
    for (const auto &t : tests) {
        const int before = g_failures;
        t.fn();
        printf("%s %s\n", g_failures == before ? "ok  " : "FAIL", t.name);
    }
    printf("win32_ui_tests: %d checks, %d failures\n", g_checks, g_failures);
    sched_run_thread_finished();
    return g_failures ? 1 : 0;
}
