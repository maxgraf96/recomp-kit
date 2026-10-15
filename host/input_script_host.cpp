// input_script_host.cpp - see input_script_host.h.
#include "input_script_host.h"

#include "boot.h"
#include "input.h"
#include "../dx/dx.h"
#include "../platform/os.h"
#include "../runtime/guest.h"
#include "../runtime/win32.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace iscript {
namespace {

Runner *g_runner = nullptr;

void observe_message(uint32_t hwnd, uint32_t msg, uint32_t, uint32_t) {
    if (g_runner)
        g_runner->message(hwnd, msg);
}

uint32_t now_ms() {
    return uint32_t(os_monotonic_ns() / 1000000ull);
}

enum : uint32_t {
    kMouseMove = 0x0200,
    kKeyDown = 0x0100,
    kKeyUp = 0x0101,
};

} // namespace

bool guest_memory_read(uint32_t address, uint32_t *value) {
    if (!g_mem || !gm_valid(address, 4))
        return false;
    *value = rd32(address);
    return true;
}

bool host_start(const std::string &path, const std::string &out_dir, std::string *error) {
    if (g_runner) {
        if (error)
            *error = "a script is already running";
        return false;
    }
    auto *runner = new Runner();
    if (!runner->load_file(path, error)) {
        delete runner;
        return false;
    }
    std::string out = out_dir;
    if (out.empty()) {
        out = path;
        const size_t dot = out.rfind('.');
        const size_t slash = out.rfind('/');
        if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
            out.resize(dot);
        out += "-frames";
    }
    runner->set_output(out);
    runner->set_memory_reader(guest_memory_read);
    g_runner = runner;
    win32_set_message_observer(observe_message);
    printf("[script] %s; frames to %s\n", path.c_str(), out.c_str());
    return true;
}

bool host_start_from_env() {
    const char *path = getenv("RECOMP_INPUT_SCRIPT");
    if (!path || !*path)
        return false;
    const char *out = getenv("RECOMP_INPUT_SCRIPT_OUT");
    std::string error;
    if (!host_start(path, out ? out : "", &error)) {
        fprintf(stderr, "[script] %s\n", error.c_str());
        exit(2);
    }
    return true;
}

Runner *host_runner() {
    return g_runner;
}

void host_tick(Sink &sink, uint64_t frames) {
    if (g_runner)
        g_runner->tick(sink, now_ms(), frames);
}

void host_pixels(const uint32_t *argb, int w, int h) {
    if (g_runner && g_runner->wants_pixels())
        g_runner->pixels(argb, w, h);
}

// ---------------------------------------------------------------------------
// The headless input path
// ---------------------------------------------------------------------------
HeadlessSink::HeadlessSink() {
    host_input_set_notify(dinput_host_input_changed);
}

void HeadlessSink::motion(int32_t dx, int32_t dy) {
    // An exclusive DirectInput mouse reports raw motion and the system
    // pointer does not move: no message, only the device (as the windowed
    // host's relative mode does).
    if (dinput_mouse_exclusive()) {
        host_input_motion(x_, y_, dx, dy);
        return;
    }
    place(x_ + dx, y_ + dy);
}

void HeadlessSink::place(int32_t x, int32_t y) {
    const int32_t dx = x - x_, dy = y - y_;
    x_ = x;
    y_ = y;
    host_input_motion(x, y, dx, dy);
    if (!dinput_mouse_exclusive())
        host_post_client_mouse_message(host_main_window(), kMouseMove,
                                       host_mouse_wparam(buttons_, 0), x, y);
}

void HeadlessSink::button(int button, bool down) {
    if (button < 0 || button > 2)
        return;
    if (down)
        buttons_ |= uint8_t(1u << button);
    else
        buttons_ &= uint8_t(~(1u << button));
    host_input_button(button, down);
    if (dinput_mouse_exclusive())
        return;
    static const uint32_t kMsgs[3][2] = {{0x0202, 0x0201}, {0x0205, 0x0204}, {0x0208, 0x0207}};
    host_post_client_mouse_message(host_main_window(), kMsgs[button][down ? 1 : 0],
                                   host_mouse_wparam(buttons_, 0), x_, y_);
}

void HeadlessSink::key(uint8_t dik, bool down) {
    const HostKeyMapping m = host_key_mapping_for_dik(dik);
    if (!m.dik) {
        fprintf(stderr, "[script] no key has DirectInput code %02x\n", dik);
        return;
    }
    host_input_key(m.mac, down);
    if (m.vk)
        host_post_key_message(down ? kKeyDown : kKeyUp, m.vk,
                              host_key_lparam(m, down, false, !down));
}

void HeadlessSink::character(uint32_t codepoint, bool down) {
    uint8_t dik = 0;
    bool shift = false;
    key_for_character(codepoint, &dik, &shift);
    const HostKeyMapping m = dik ? host_key_mapping_for_dik(dik) : HostKeyMapping{0, 0, 0};
    if (shift && down)
        key(0x2a /* LSHIFT */, true);
    if (m.dik)
        host_input_key(m.mac, down);
    const uint32_t vk = m.vk ? m.vk : 0xe7 /* VK_PACKET */;
    const uint32_t lparam =
        m.dik ? host_key_lparam(m, down, false, !down) : (down ? 1u : 0xc0000001u);
    host_post_key_text(down ? kKeyDown : kKeyUp, vk, lparam, down ? codepoint : 0);
    if (shift && !down)
        key(0x2a, false);
}

void HeadlessSink::close() {
    boot_request_close("input script");
}

} // namespace iscript
