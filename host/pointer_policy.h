// pointer_policy.h - who owns the system pointer while a game has an
// exclusive DirectInput mouse.
//
// The SDL host turns SDL's relative mouse mode on while the guest holds its
// DirectInput mouse exclusively. That hides the system cursor and
// disassociates it from the mouse, so the window's own controls, the menu bar
// and the macOS fullscreen button are unreachable. Holding Escape must give
// the pointer back to the player even though the guest still owns the device:
// relative mode is suspended, motion is not fed to the guest meanwhile, and it
// resumes when Escape is released. These decisions are pure so the headless
// host tests can check every release condition without a window.
#pragma once

namespace host {

// The window and guest state the decisions read. `guest_pointer_hidden` (the
// second argument below) is the guest's own SetCursor(NULL) state, not the
// host's cursor visibility.
struct PointerPolicy {
    bool relative_mouse;        // [runtime] relative_mouse is configured
    bool guest_mouse_exclusive; // the guest's DirectInput mouse is acquired exclusively
    bool window;                // the SDL window exists
    bool focused;               // the host's focus state
    bool window_focused;        // SDL says the window has input focus
    bool minimized_or_hidden;
    bool settings_page;
    bool controls_editor;
    bool close_requested;
    bool escape_held;
};

// Relative mode is for a focused game that owns the pointer. Escape suspends
// it: the player gets the system cursor until the key comes up.
inline bool relative_mouse_wanted(const PointerPolicy &p) {
    return p.relative_mouse && p.guest_mouse_exclusive && p.window && p.focused &&
           p.window_focused && !p.minimized_or_hidden && !p.settings_page && !p.controls_editor &&
           !p.close_requested && !p.escape_held;
}

// The guest's hidden pointer hides the system one too, except while Escape
// gives the pointer back to the player.
inline bool guest_hides_pointer(const PointerPolicy &p, bool guest_pointer_hidden) {
    return p.relative_mouse && guest_pointer_hidden && p.window && p.focused && p.window_focused &&
           !p.settings_page && !p.controls_editor && !p.escape_held;
}

} // namespace host
