// Internal USER32 model. All access is on the guest scheduler baton; guest
// callbacks may erase windows, so never retain a Window pointer across one.
#pragma once
#include "imports.h"
#include "gdi32_internal.h"
#include "win32.h"
#include <deque>
#include <map>
#include <string>
#include <vector>
namespace user32 {
struct WndClass {
    uint32_t style = 0;
    uint32_t wndproc = 0;
    uint32_t cls_extra = 0;
    uint32_t wnd_extra = 0;
    uint32_t hinstance = 0;
    uint32_t hicon = 0;
    uint32_t hcursor = 0;
    uint32_t hbrush = 0;
    std::string menu, name;
    uint32_t menu_id = 0, atom = 0;
    bool unicode = false;
    std::vector<uint32_t> extra;
};

struct Window {
    gdi::Surface surface;
    uint32_t hwnd = 0;
    uint32_t wndproc = 0;
    uint32_t style = 0, exstyle = 0;
    int32_t x = 0, y = 0, w = 0, h = 0;
    std::string cls, title_utf8;
    bool unicode = false;
    std::map<std::string, uint32_t> props;
    uint32_t menu = 0, parent = 0, owner = 0, id = 0;
    uint32_t thread = 0;
    bool enabled = true;
    bool owned_hidden = false;
    uint32_t region = 0;
    uint32_t show_cmd = 0;
    uint32_t userdata = 0;
    uint32_t font = 0; // WM_SETFONT, kept for the system control classes
    uint32_t hinstance = 0;
    std::vector<uint32_t> extra;
    bool visible = false;
    bool shown = false;
    // Windows tracks an update region per window; the runtime only needs to
    // know whether it is empty, which is what UpdateWindow and BeginPaint act
    // on. Showing a window invalidates it, painting it validates it.
    bool update_pending = false;
    // SetLayeredWindowAttributes: LWA_COLORKEY (1) makes every pixel of that
    // colour transparent, LWA_ALPHA (2) applies one opacity to the whole
    // window. Both are honoured where the window surfaces are composited.
    uint32_t layered_flags = 0, layered_key = 0, layered_alpha = 255;
};

struct Msg {
    uint32_t hwnd, message, wparam, lparam, time, ptx, pty;
    // Non-zero for a SENT message waiting in the queue: the id of its
    // SentCall. Message retrieval delivers it straight to the window procedure
    // and never returns it to the caller, which is how Windows hands a thread
    // the messages other threads (and the system) send it.
    uint32_t sent = 0;
    // MSGF_* below: what the runtime knows about a queued message that its
    // guest-visible MSG cannot carry.
    uint32_t flags = 0;
};
// A key message the host posted together with the character it produced, so
// TranslateMessage has nothing left to post for it (host_post_key_text).
constexpr uint32_t MSGF_TRANSLATED = 1;

std::map<std::string, WndClass> &classes();
std::map<uint32_t, Window> &windows();
// The calling guest thread's message queue. Windows gives every thread its
// own: PeekMessage and GetMessage see only the messages for windows the
// calling thread created and the thread messages posted to it.
std::deque<Msg> &queue();
std::deque<Msg> &queue_for_thread(uint32_t thread_id);
// Every thread's queue, for the bookkeeping that spans them (a window's
// messages die with it, whichever thread it was on).
std::map<uint32_t, std::deque<Msg>> &thread_queues();
// The thread a message for this window is queued on: the one that created
// it. The main thread (1) for the desktop and for unknown windows.
uint32_t window_thread(uint32_t hwnd);
// Delivers every sent message waiting in the calling thread's queue, in
// order; true when it delivered any. Message retrieval and every wait a
// thread makes inside USER32 call this, as Windows does in the same places.
bool deliver_sent_messages(X86 *c);
// Queues a message the host SENDS to a window: delivered to its window
// procedure (never returned by PeekMessage) when the window's thread next
// retrieves messages, in order with the input queued around it.
void notify_message(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp);
// The window manager's view of activation and keyboard focus
// (GetActiveWindow, GetFocus, GetCapture), kept in user32_vcl.cpp.
uint32_t active_window();
void set_active_window(uint32_t hwnd);
uint32_t focus_window();
void set_focus_window(uint32_t hwnd);
uint32_t capture_window();
// SendMessage to a window of another guest thread: queued there, and the
// caller waits for the answer (user32.cpp).
uint32_t send_to_thread(X86 *c, uint32_t thread_id, uint32_t hwnd, uint32_t msg, uint32_t wp,
                        uint32_t lp, bool wide);
Window *find_window(uint32_t hwnd);
std::string class_key(uint32_t p, bool wide = false);
uint32_t wide_units(const std::string &text);
uint32_t put_text(uint32_t out, uint32_t cap, const std::string &text, bool wide);
void register_class_named(X86 *c, bool wide, uint32_t shift);
void create_window_named(X86 *c, bool wide);
void def_window_proc(X86 *c, bool wide);
void send_message(X86 *c, bool wide);
void peek_message(X86 *c);
void dispatch_message(X86 *c);
uint32_t deliver_message(X86 *c, uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp, bool wide);
extern uint32_t g_main_hwnd, g_cursor;
extern int32_t g_cursor_x, g_cursor_y;
extern uint8_t g_key_state[256];
extern bool (*g_message_waiter)();
extern Msg last_message;
constexpr uint32_t desktop_handle = 0x00020000;
void pump_window_timers();
// Whether some visible window has an update region a paint would answer.
bool paint_pending();
void window_created(uint32_t hwnd);
// Siblings in back-to-front stacking order, respecting WS_EX_TOPMOST.
std::vector<uint32_t> window_z_order(uint32_t parent);
void reorder_window(uint32_t hwnd, uint32_t after);
void pump_mouse_input(X86 *c);
void forget_window_services(uint32_t hwnd);
bool destroy_window(X86 *c, uint32_t hwnd);
void client_origin(uint32_t hwnd, int32_t *x, int32_t *y);
void display_rect(uint32_t out);
void alias_ansi(X86 *c, const char *name);
// Registers the system control classes (STATIC) on first use.
void ensure_system_classes();
// The activation model (user32_activation.cpp): a top-level window became
// visible (`activate` is false for the show commands that do not activate),
// or is being destroyed.
void activation_window_shown(X86 *c, uint32_t hwnd, bool activate);
void activation_window_destroyed(uint32_t hwnd);
// SetFocus's body: WM_KILLFOCUS to the window losing the focus, WM_SETFOCUS
// to the one gaining it. Returns the window that had it.
uint32_t change_focus(X86 *c, uint32_t hwnd);
// SetWindowPos's body (user32.cpp), for MoveWindow and the display code.
void set_window_pos(X86 *c, Window *w, uint32_t after, int32_t x, int32_t y, int32_t cx, int32_t cy,
                    uint32_t flags, bool refresh_size);
// A window hook SetWindowsHookEx accepted (recorded, never called); its handle.
uint32_t record_hook(uint32_t kind, uint32_t proc, uint32_t module, uint32_t thread);
// wsprintfA's formatting over a guest argument array; the length written.
uint32_t format_wsprintf(uint32_t out, uint32_t format, uint32_t va);
} // namespace user32
void user32_ansi_register();
void user32_wide_register();

void user32_vcl_register();
