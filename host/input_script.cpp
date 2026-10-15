// input_script.cpp - see input_script.h.
#include "input_script.h"

#include "../platform/os.h"
#include "../third_party/miniz/miniz.h"

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <sstream>

namespace iscript {
namespace {

struct Named {
    const char *name;
    uint32_t value;
};

// DirectInput set-1 scan codes by their DIK_ names, without the prefix.
const Named kKeys[] = {
    {"ESCAPE", 0x01},      {"1", 0x02},          {"2", 0x03},         {"3", 0x04},
    {"4", 0x05},           {"5", 0x06},          {"6", 0x07},         {"7", 0x08},
    {"8", 0x09},           {"9", 0x0a},          {"0", 0x0b},         {"MINUS", 0x0c},
    {"EQUALS", 0x0d},      {"BACK", 0x0e},       {"BACKSPACE", 0x0e}, {"TAB", 0x0f},
    {"Q", 0x10},           {"W", 0x11},          {"E", 0x12},         {"R", 0x13},
    {"T", 0x14},           {"Y", 0x15},          {"U", 0x16},         {"I", 0x17},
    {"O", 0x18},           {"P", 0x19},          {"LBRACKET", 0x1a},  {"RBRACKET", 0x1b},
    {"RETURN", 0x1c},      {"ENTER", 0x1c},      {"LCONTROL", 0x1d},  {"A", 0x1e},
    {"S", 0x1f},           {"D", 0x20},          {"F", 0x21},         {"G", 0x22},
    {"H", 0x23},           {"J", 0x24},          {"K", 0x25},         {"L", 0x26},
    {"SEMICOLON", 0x27},   {"APOSTROPHE", 0x28}, {"GRAVE", 0x29},     {"LSHIFT", 0x2a},
    {"BACKSLASH", 0x2b},   {"Z", 0x2c},          {"X", 0x2d},         {"C", 0x2e},
    {"V", 0x2f},           {"B", 0x30},          {"N", 0x31},         {"M", 0x32},
    {"COMMA", 0x33},       {"PERIOD", 0x34},     {"SLASH", 0x35},     {"RSHIFT", 0x36},
    {"MULTIPLY", 0x37},    {"LMENU", 0x38},      {"LALT", 0x38},      {"SPACE", 0x39},
    {"CAPITAL", 0x3a},     {"F1", 0x3b},         {"F2", 0x3c},        {"F3", 0x3d},
    {"F4", 0x3e},          {"F5", 0x3f},         {"F6", 0x40},        {"F7", 0x41},
    {"F8", 0x42},          {"F9", 0x43},         {"F10", 0x44},       {"NUMLOCK", 0x45},
    {"SCROLL", 0x46},      {"NUMPAD7", 0x47},    {"NUMPAD8", 0x48},   {"NUMPAD9", 0x49},
    {"SUBTRACT", 0x4a},    {"NUMPAD4", 0x4b},    {"NUMPAD5", 0x4c},   {"NUMPAD6", 0x4d},
    {"ADD", 0x4e},         {"NUMPAD1", 0x4f},    {"NUMPAD2", 0x50},   {"NUMPAD3", 0x51},
    {"NUMPAD0", 0x52},     {"DECIMAL", 0x53},    {"F11", 0x57},       {"F12", 0x58},
    {"NUMPADENTER", 0x9c}, {"RCONTROL", 0x9d},   {"DIVIDE", 0xb5},    {"RMENU", 0xb8},
    {"RALT", 0xb8},        {"PAUSE", 0xc5},      {"HOME", 0xc7},      {"UP", 0xc8},
    {"PRIOR", 0xc9},       {"PGUP", 0xc9},       {"LEFT", 0xcb},      {"RIGHT", 0xcd},
    {"END", 0xcf},         {"DOWN", 0xd0},       {"NEXT", 0xd1},      {"PGDN", 0xd1},
    {"INSERT", 0xd2},      {"DELETE", 0xd3},
};

const Named kMessages[] = {
    {"WM_CREATE", 0x0001},        {"WM_DESTROY", 0x0002},
    {"WM_MOVE", 0x0003},          {"WM_SIZE", 0x0005},
    {"WM_ACTIVATE", 0x0006},      {"WM_SETFOCUS", 0x0007},
    {"WM_KILLFOCUS", 0x0008},     {"WM_PAINT", 0x000f},
    {"WM_CLOSE", 0x0010},         {"WM_QUIT", 0x0012},
    {"WM_SHOWWINDOW", 0x0018},    {"WM_ACTIVATEAPP", 0x001c},
    {"WM_SETCURSOR", 0x0020},     {"WM_WINDOWPOSCHANGED", 0x0047},
    {"WM_DISPLAYCHANGE", 0x007e}, {"WM_NCDESTROY", 0x0082},
    {"WM_KEYDOWN", 0x0100},       {"WM_KEYUP", 0x0101},
    {"WM_CHAR", 0x0102},          {"WM_SYSKEYDOWN", 0x0104},
    {"WM_SYSKEYUP", 0x0105},      {"WM_COMMAND", 0x0111},
    {"WM_SYSCOMMAND", 0x0112},    {"WM_TIMER", 0x0113},
    {"WM_MOUSEMOVE", 0x0200},     {"WM_LBUTTONDOWN", 0x0201},
    {"WM_LBUTTONUP", 0x0202},     {"WM_RBUTTONDOWN", 0x0204},
    {"WM_RBUTTONUP", 0x0205},     {"WM_MBUTTONDOWN", 0x0207},
    {"WM_MBUTTONUP", 0x0208},     {"WM_MOUSEWHEEL", 0x020a},
    {"WM_USER", 0x0400},
};

bool equal_nocase(const char *a, const char *b) {
    for (; *a && *b; ++a, ++b)
        if (toupper((unsigned char)*a) != toupper((unsigned char)*b))
            return false;
    return !*a && !*b;
}

std::vector<std::string> words(const std::string &line) {
    std::vector<std::string> out;
    std::istringstream in(line);
    std::string w;
    while (in >> w)
        out.push_back(w);
    return out;
}

// A whole number in any base strtol reads, and nothing after it.
bool integer(const std::string &s, long *out) {
    if (s.empty())
        return false;
    char *end = nullptr;
    long v = strtol(s.c_str(), &end, 0);
    if (!end || *end)
        return false;
    *out = v;
    return true;
}
bool real(const std::string &s, double *out) {
    if (s.empty())
        return false;
    char *end = nullptr;
    double v = strtod(s.c_str(), &end);
    if (!end || *end)
        return false;
    *out = v;
    return true;
}

// [base]+offset, base+offset, [base]-offset or base.
bool address(const std::string &s, Address *out) {
    Address a;
    std::string rest = s;
    if (!rest.empty() && rest[0] == '[') {
        const size_t close = rest.find(']');
        if (close == std::string::npos)
            return false;
        long base = 0;
        if (!integer(rest.substr(1, close - 1), &base))
            return false;
        a.base = uint32_t(base);
        a.deref = true;
        rest = rest.substr(close + 1);
    } else {
        const size_t sign = rest.find_first_of("+-", 1);
        long base = 0;
        if (!integer(rest.substr(0, sign), &base))
            return false;
        a.base = uint32_t(base);
        rest = sign == std::string::npos ? std::string() : rest.substr(sign);
    }
    if (!rest.empty()) {
        if (rest[0] != '+' && rest[0] != '-')
            return false;
        long off = 0;
        if (!integer(rest.substr(1), &off))
            return false;
        a.offset = int32_t(rest[0] == '-' ? -off : off);
    }
    *out = a;
    return true;
}

bool dump_name_ok(const std::string &s) {
    if (s.empty() || s.size() > 96 || s[0] == '.')
        return false;
    for (char c : s)
        if (!isalnum((unsigned char)c) && c != '_' && c != '-' && c != '.')
            return false;
    return true;
}

int button_for(const std::string &s) {
    if (equal_nocase(s.c_str(), "left"))
        return 0;
    if (equal_nocase(s.c_str(), "right"))
        return 1;
    if (equal_nocase(s.c_str(), "middle"))
        return 2;
    return -1;
}

// `within MS` among the trailing words, removed from `w` when found.
bool take_within(std::vector<std::string> &w, size_t from, uint32_t *ms, std::string *error,
                 bool *found) {
    *found = false;
    for (size_t i = from; i < w.size(); ++i) {
        if (!equal_nocase(w[i].c_str(), "within"))
            continue;
        long v = 0;
        if (i + 1 >= w.size() || !integer(w[i + 1], &v) || v <= 0) {
            *error = "within needs a positive number of ms";
            return false;
        }
        *ms = uint32_t(v);
        *found = true;
        w.erase(w.begin() + long(i), w.begin() + long(i) + 2);
        return true;
    }
    return true;
}

const uint32_t kDefaultWithin = 30000;

} // namespace

uint8_t dik_for_name(const char *name) {
    if (!name)
        return 0;
    if (toupper((unsigned char)name[0]) == 'D' && toupper((unsigned char)name[1]) == 'I' &&
        toupper((unsigned char)name[2]) == 'K' && name[3] == '_')
        name += 4;
    for (const Named &k : kKeys)
        if (equal_nocase(k.name, name))
            return uint8_t(k.value);
    return 0;
}

uint32_t message_for_name(const char *name) {
    if (!name)
        return 0;
    for (const Named &m : kMessages)
        if (equal_nocase(m.name, name))
            return m.value;
    long v = 0;
    if (integer(name, &v) && v > 0 && v <= 0xffff)
        return uint32_t(v);
    return 0;
}

bool parse(const std::string &text, Script *out, std::string *error) {
    Script s;
    std::istringstream in(text);
    std::string raw;
    int number = 0;
    auto fail = [&](const std::string &why) {
        if (error)
            *error = "line " + std::to_string(number) + ": " + why;
        return false;
    };
    while (std::getline(in, raw)) {
        ++number;
        const size_t hash = raw.find('#');
        std::string line = hash == std::string::npos ? raw : raw.substr(0, hash);
        std::vector<std::string> w = words(line);
        if (w.empty())
            continue;
        const std::string verb = w[0];
        auto is = [&](const char *v) { return equal_nocase(verb.c_str(), v); };
        Step st;
        st.line = number;
        std::string why;
        long a = 0, b = 0;
        if (is("timeout")) {
            if (w.size() != 2 || !integer(w[1], &a) || a <= 0)
                return fail("timeout needs a positive number of ms");
            s.timeout_ms = uint32_t(a);
            continue;
        } else if (is("pointer")) {
            if (w.size() != 4 ||
                (!equal_nocase(w[1].c_str(), "i32") && !equal_nocase(w[1].c_str(), "f32")))
                return fail("pointer needs i32|f32 XADDR YADDR");
            if (!address(w[2], &s.pointer_x) || !address(w[3], &s.pointer_y))
                return fail("pointer addresses are 0xADDR, [0xADDR]+OFFSET or 0xADDR+OFFSET");
            s.pointer_float = equal_nocase(w[1].c_str(), "f32");
            s.has_pointer = true;
            continue;
        } else if (is("pointer_tolerance")) {
            double t = 0;
            if (w.size() != 2 || !real(w[1], &t) || t < 0.5)
                return fail("pointer_tolerance needs a number of pixels (at least 0.5)");
            s.pointer_tolerance = t;
            continue;
        } else if (is("hold")) {
            if (w.size() != 2 || !integer(w[1], &a) || a < 0)
                return fail("hold needs a number of ms");
            s.hold_ms = uint32_t(a);
            continue;
        } else if (is("wait")) {
            if (w.size() < 2)
                return fail("wait needs MS, frames, message, change, still or pointer");
            const std::string kind = w[1];
            auto kind_is = [&](const char *k) { return equal_nocase(kind.c_str(), k); };
            st.ms = kDefaultWithin;
            bool within = false;
            if (!take_within(w, 2, &st.ms, &why, &within))
                return fail(why);
            if (integer(kind, &a)) {
                if (w.size() != 2 || a < 0 || within)
                    return fail("wait MS takes nothing else");
                st.op = Op::Wait;
                st.ms = uint32_t(a);
            } else if (kind_is("frames")) {
                if (w.size() != 3 || !integer(w[2], &a) || a <= 0)
                    return fail("wait frames needs a positive count");
                st.op = Op::WaitFrames;
                st.count = uint32_t(a);
            } else if (kind_is("message")) {
                if (w.size() != 3 || !(st.msg = message_for_name(w[2].c_str())))
                    return fail("wait message needs a WM_ name or a message number");
                st.op = Op::WaitMessage;
                st.text = w[2];
            } else if (kind_is("change")) {
                st.op = Op::WaitChange;
                if (w.size() == 3 &&
                    (!real(w[2], &st.fraction) || st.fraction <= 0 || st.fraction >= 1))
                    return fail("wait change takes a fraction between 0 and 1");
                if (w.size() > 3)
                    return fail("wait change [FRACTION] [within MS]");
            } else if (kind_is("still")) {
                if (w.size() != 3 || !integer(w[2], &a) || a < 2)
                    return fail("wait still needs a count of at least 2 frames");
                st.op = Op::WaitStill;
                st.count = uint32_t(a);
            } else if (kind_is("pointer")) {
                if (w.size() != 2)
                    return fail("wait pointer [within MS]");
                st.op = Op::WaitPointer;
            } else if (kind_is("pixel")) {
                // wait pixel X Y RRGGBB [tol N]
                long tol = 16;
                for (size_t i = 5; i + 1 < w.size(); ++i)
                    if (equal_nocase(w[i].c_str(), "tol")) {
                        if (!integer(w[i + 1], &tol) || tol < 0 || tol > 255)
                            return fail("tol is 0 to 255");
                        w.erase(w.begin() + long(i), w.begin() + long(i) + 2);
                        break;
                    }
                const std::string hex = w.size() == 5 ? w[4] : std::string();
                char *end = nullptr;
                const unsigned long color = hex.size() == 6 ? strtoul(hex.c_str(), &end, 16) : 0;
                if (w.size() != 5 || !integer(w[2], &a) || !integer(w[3], &b) || a < 0 || b < 0 ||
                    hex.size() != 6 || !end || *end)
                    return fail("wait pixel X Y RRGGBB [tol N] [within MS]");
                st.op = Op::WaitPixel;
                st.x = int32_t(a);
                st.y = int32_t(b);
                st.color = uint32_t(color);
                st.tol = int(tol);
            } else if (kind_is("size")) {
                if (w.size() != 4 || !integer(w[2], &a) || !integer(w[3], &b) || a <= 0 || b <= 0)
                    return fail("wait size W H [within MS]");
                st.op = Op::WaitSize;
                st.x = int32_t(a);
                st.y = int32_t(b);
            } else {
                return fail(
                    "wait needs MS, frames, message, change, still, pointer, size or pixel");
            }
        } else if (is("move") || is("moveby")) {
            if (w.size() != 3 || !integer(w[1], &a) || !integer(w[2], &b))
                return fail(verb + " needs two whole numbers");
            st.op = is("move") ? Op::Move : Op::MoveBy;
            st.x = int32_t(a);
            st.y = int32_t(b);
        } else if (is("click")) {
            size_t i = 1;
            if (w.size() == 4) {
                if ((st.button = button_for(w[1])) < 0)
                    return fail("click button is left, right or middle");
                i = 2;
            } else if (w.size() != 3) {
                return fail("click [left|right|middle] X Y");
            }
            if (!integer(w[i], &a) || !integer(w[i + 1], &b))
                return fail("click needs two whole numbers");
            st.op = Op::Click;
            st.x = int32_t(a);
            st.y = int32_t(b);
        } else if (is("button")) {
            if (w.size() != 3 || (st.button = button_for(w[1])) < 0 ||
                !(equal_nocase(w[2].c_str(), "down") || equal_nocase(w[2].c_str(), "up")))
                return fail("button left|right|middle down|up");
            st.op = Op::Button;
            st.down = equal_nocase(w[2].c_str(), "down") ? 1 : 0;
        } else if (is("key")) {
            if (w.size() < 2 || w.size() > 3 || !(st.dik = dik_for_name(w[1].c_str())))
                return fail("key needs a DirectInput key name (ESCAPE, RETURN, A, F1, ...)");
            if (w.size() == 3) {
                if (equal_nocase(w[2].c_str(), "down"))
                    st.down = 1;
                else if (equal_nocase(w[2].c_str(), "up"))
                    st.down = 0;
                else
                    return fail("key NAME [down|up]");
            }
            st.op = Op::Key;
            st.text = w[1];
        } else if (is("dump")) {
            if (w.size() != 2 || !dump_name_ok(w[1]))
                return fail("dump needs a name of letters, digits, _ - and .");
            st.op = Op::Dump;
            st.text = w[1];
        } else if (is("mark")) {
            if (w.size() != 1)
                return fail("mark takes nothing");
            st.op = Op::Mark;
        } else if (is("peek")) {
            if (w.size() < 2 || w.size() > 3 || !address(w[1], &st.addr))
                return fail("peek ADDR [COUNT]");
            st.count = 1;
            if (w.size() == 3) {
                if (!integer(w[2], &a) || a <= 0 || a > 64)
                    return fail("peek COUNT is 1 to 64 dwords");
                st.count = uint32_t(a);
            }
            st.op = Op::Peek;
        } else if (is("log")) {
            st.op = Op::Log;
            const size_t at = line.find_first_not_of(" \t", line.find(verb) + verb.size());
            st.text = at == std::string::npos ? std::string() : line.substr(at);
            while (!st.text.empty() && isspace((unsigned char)st.text.back()))
                st.text.pop_back();
        } else if (is("quit")) {
            if (w.size() != 1)
                return fail("quit takes nothing");
            st.op = Op::Quit;
        } else {
            return fail("unknown step '" + verb + "'");
        }
        s.steps.push_back(st);
    }
    *out = std::move(s);
    return true;
}

bool write_png(const std::string &path, const uint32_t *argb, int w, int h) {
    if (!argb || w <= 0 || h <= 0)
        return false;
    std::vector<uint8_t> rgb(size_t(w) * size_t(h) * 3);
    for (size_t i = 0, n = size_t(w) * size_t(h); i < n; ++i) {
        rgb[3 * i] = uint8_t(argb[i] >> 16);
        rgb[3 * i + 1] = uint8_t(argb[i] >> 8);
        rgb[3 * i + 2] = uint8_t(argb[i]);
    }
    size_t len = 0;
    void *png = tdefl_write_image_to_png_file_in_memory_ex(rgb.data(), w, h, 3, &len, 3, MZ_FALSE);
    if (!png)
        return false;
    FILE *f = fopen(path.c_str(), "wb");
    bool ok = f && fwrite(png, 1, len, f) == len;
    if (f)
        ok = fclose(f) == 0 && ok;
    mz_free(png);
    return ok;
}

// ===========================================================================
// The runner
// ===========================================================================
namespace {

// Every other pixel of every other row: enough to tell a screen from the next
// one and a settled screen from a moving one, at a quarter of the cost.
struct Thumb {
    std::vector<uint32_t> px;
    int w = 0, h = 0;
    uint64_t hash = 0;
    bool empty() const {
        return px.empty();
    }
};
Thumb thumbnail(const uint32_t *argb, int w, int h) {
    Thumb t;
    t.w = (w + 1) / 2;
    t.h = (h + 1) / 2;
    t.px.resize(size_t(t.w) * size_t(t.h));
    uint64_t hash = 1469598103934665603ull;
    for (int y = 0; y < t.h; ++y)
        for (int x = 0; x < t.w; ++x) {
            const uint32_t p = argb[size_t(2 * y) * size_t(w) + size_t(2 * x)] & 0x00ffffffu;
            t.px[size_t(y) * size_t(t.w) + size_t(x)] = p;
            hash = (hash ^ p) * 1099511628211ull;
        }
    t.hash = hash;
    return t;
}
// The fraction of pixels that differ by more than a small step in any
// channel; 1 when the sizes differ.
double difference(const Thumb &a, const Thumb &b) {
    if (a.w != b.w || a.h != b.h || a.empty())
        return 1.0;
    size_t changed = 0;
    for (size_t i = 0; i < a.px.size(); ++i) {
        const uint32_t p = a.px[i], q = b.px[i];
        if (p == q)
            continue;
        const int dr = abs(int((p >> 16) & 255) - int((q >> 16) & 255));
        const int dg = abs(int((p >> 8) & 255) - int((q >> 8) & 255));
        const int db = abs(int(p & 255) - int(q & 255));
        if (dr > 24 || dg > 24 || db > 24)
            ++changed;
    }
    return double(changed) / double(a.px.size());
}

const char *op_name(Op op) {
    switch (op) {
    case Op::Wait:
        return "wait";
    case Op::WaitFrames:
        return "wait frames";
    case Op::WaitMessage:
        return "wait message";
    case Op::WaitChange:
        return "wait change";
    case Op::WaitStill:
        return "wait still";
    case Op::WaitPointer:
        return "wait pointer";
    case Op::WaitSize:
        return "wait size";
    case Op::WaitPixel:
        return "wait pixel";
    case Op::Move:
        return "move";
    case Op::MoveBy:
        return "moveby";
    case Op::Click:
        return "click";
    case Op::Button:
        return "button";
    case Op::Key:
        return "key";
    case Op::Dump:
        return "dump";
    case Op::Mark:
        return "mark";
    case Op::Peek:
        return "peek";
    case Op::Log:
        return "log";
    case Op::Quit:
        return "quit";
    }
    return "?";
}

bool is_action(Op op) {
    return op == Op::Move || op == Op::MoveBy || op == Op::Click || op == Op::Button ||
           op == Op::Key;
}

} // namespace

struct Runner::Impl {
    Script script;
    bool loaded = false;
    std::string out_dir;
    uint32_t max_dumps = 64;
    MemoryReader reader = nullptr;
    FILE *log = stdout;

    // Progress, on the tick's thread.
    bool started = false, done = false, fail = false;
    std::string why;
    uint32_t start_ms = 0;
    size_t pc = 0;
    bool step_begun = false;
    uint32_t step_ms = 0;
    uint64_t step_frames = 0;
    int phase = 0;
    uint32_t phase_ms = 0;
    uint64_t phase_frames = 0;
    std::atomic<uint64_t> frames_now{0}; // read by pixels() for its log
    // The closed-loop move.
    struct Move {
        double gain_x = 0, gain_y = 0; // pixels per count, measured; 0 = not yet
        int iterations = 0;
        bool sent = false;
        uint32_t sent_ms = 0;
        uint64_t sent_frames = 0;
        double from_x = 0, from_y = 0;
        int32_t counts_x = 0, counts_y = 0;
    } move;
    double last_px = 0, last_py = 0; // the last pointer read, for the log
    // After a release, the next action waits a moment (40 ms and two frames)
    // so the game reads the release on its own before anything else
    // arrives. The step itself is done at the release: a click that ends the
    // game (an Exit button) completes its script.
    bool settling = false;
    uint32_t settle_ms = 0;
    uint64_t settle_frames = 0;
    void settle(uint32_t now) {
        settling = true;
        settle_ms = now;
        settle_frames = frames_now;
    }
    uint32_t steps_run = 0;

    // Shared with pixels() and message(), under `m`.
    mutable std::mutex m;
    std::vector<std::string> dumps_written;
    Thumb latest, reference, last_dump;
    int latest_w = 0, latest_h = 0; // the full size of the latest frame
    bool reference_wanted = false;  // no frame yet: the next one is the reference
    uint64_t pixel_frames = 0;      // frames whose pixels arrived
    double change_since_reference = 0;
    uint32_t still_run = 0;
    std::vector<std::pair<std::string, std::string>> pending_dumps; // path, name
    uint32_t dumps_armed = 0, dumps_done = 0;
    std::map<uint32_t, uint64_t> messages, message_baseline;

    void say(const char *fmt, ...) __attribute__((format(printf, 2, 3))) {
        va_list ap;
        va_start(ap, fmt);
        fprintf(log, "[script] ");
        vfprintf(log, fmt, ap);
        fputc('\n', log);
        fflush(log);
        va_end(ap);
    }
    void failed(const Step &st, const std::string &what) {
        fail = true;
        why = "line " + std::to_string(st.line) + ": " + op_name(st.op) + ": " + what;
        say("FAILED %s", why.c_str());
    }

    bool read_u32(const Address &a, uint32_t *out) const {
        if (!reader)
            return false;
        uint32_t base = a.base;
        if (a.deref) {
            if (!reader(a.base, &base) || !base)
                return false;
        }
        return reader(base + uint32_t(a.offset), out);
    }
    bool read_pointer(double *x, double *y) const {
        if (!script.has_pointer)
            return false;
        uint32_t rx = 0, ry = 0;
        if (!read_u32(script.pointer_x, &rx) || !read_u32(script.pointer_y, &ry))
            return false;
        if (script.pointer_float) {
            float fx, fy;
            memcpy(&fx, &rx, 4);
            memcpy(&fy, &ry, 4);
            if (!std::isfinite(fx) || !std::isfinite(fy))
                return false;
            *x = fx;
            *y = fy;
        } else {
            *x = int32_t(rx);
            *y = int32_t(ry);
        }
        return true;
    }

    // Before every action: what `wait change` and `wait message` measure from.
    void baseline() {
        std::lock_guard<std::mutex> hold(m);
        reference = latest;
        reference_wanted = latest.empty();
        change_since_reference = 0;
        message_baseline = messages;
    }

    // The closed loop: true when the pointer is at (tx, ty).
    // Returns 1 done, 0 pending, -1 failed (reason in `why`).
    int walk_pointer(Sink &sink, const Step &st, uint32_t now) {
        Move &mv = move;
        double px = 0, py = 0;
        if (!read_pointer(&px, &py)) {
            if (now - step_ms > kDefaultWithin) {
                failed(st, "the pointer is not readable");
                return -1;
            }
            return 0;
        }
        last_px = px;
        last_py = py;
        if (mv.sent) {
            const bool moved = px != mv.from_x || py != mv.from_y;
            // The guest reads its mouse on its own schedule: give it time to.
            if (!moved && now - mv.sent_ms < 400)
                return 0;
            if (moved && (frames_now - mv.sent_frames < 2 || now - mv.sent_ms < 50))
                return 0;
            const double gx = mv.counts_x ? (px - mv.from_x) / mv.counts_x : 0;
            const double gy = mv.counts_y ? (py - mv.from_y) / mv.counts_y : 0;
            // A gain is only measured from motion that was not stopped by an
            // edge: one that moved less than a count's worth tells nothing.
            if (gx > 0.01)
                mv.gain_x = gx;
            if (gy > 0.01)
                mv.gain_y = gy;
            mv.sent = false;
        }
        const double ex = st.x - px, ey = st.y - py;
        const double tol_x = std::max(script.pointer_tolerance, 0.6 * mv.gain_x);
        const double tol_y = std::max(script.pointer_tolerance, 0.6 * mv.gain_y);
        if (fabs(ex) <= tol_x && fabs(ey) <= tol_y)
            return 1;
        if (++mv.iterations > 80) {
            char buf[160];
            snprintf(buf, sizeof buf,
                     "the pointer stopped at (%.1f, %.1f), short of (%d, %d) (gain %.3f, %.3f)", px,
                     py, st.x, st.y, mv.gain_x, mv.gain_y);
            failed(st, buf);
            return -1;
        }
        // Counts toward the target at the measured gain; a probe of a few
        // counts while the gain is unknown.
        auto counts = [&](double e, double gain, double tol) -> int32_t {
            if (fabs(e) <= tol)
                return 0;
            double c = gain > 0 ? e / gain : (e > 0 ? 4 : -4);
            if (gain > 0)
                c = std::max(-400.0, std::min(400.0, c));
            int32_t n = int32_t(lround(c));
            if (!n)
                n = e > 0 ? 1 : -1;
            return n;
        };
        mv.counts_x = counts(ex, mv.gain_x, tol_x);
        mv.counts_y = counts(ey, mv.gain_y, tol_y);
        mv.from_x = px;
        mv.from_y = py;
        mv.sent = true;
        mv.sent_ms = now;
        mv.sent_frames = frames_now;
        sink.motion(mv.counts_x, mv.counts_y);
        return 0;
    }

    // One step. True when it is complete and the next may start.
    bool run_step(Sink &sink, const Step &st, uint32_t now) {
        const uint32_t hold = script.hold_ms;
        switch (st.op) {
        case Op::Wait:
            return now - step_ms >= st.ms;
        case Op::WaitFrames:
            return frames_now - step_frames >= st.count;
        case Op::WaitMessage: {
            uint64_t seen = 0, before = 0;
            {
                std::lock_guard<std::mutex> lock(m);
                auto it = messages.find(st.msg);
                seen = it == messages.end() ? 0 : it->second;
                auto jt = message_baseline.find(st.msg);
                before = jt == message_baseline.end() ? 0 : jt->second;
            }
            if (seen > before)
                return true;
            if (now - step_ms > st.ms) {
                char buf[96];
                snprintf(buf, sizeof buf, "%s (0x%04x) not seen within %u ms", st.text.c_str(),
                         st.msg, st.ms);
                failed(st, buf);
            }
            return false;
        }
        case Op::WaitChange: {
            double change;
            bool have_reference;
            {
                std::lock_guard<std::mutex> lock(m);
                change = change_since_reference;
                have_reference = !reference.empty();
            }
            if (have_reference && change > st.fraction) {
                say("line %d: the picture changed in %.1f%% of its pixels", st.line, 100 * change);
                return true;
            }
            if (now - step_ms > st.ms) {
                char buf[128];
                if (have_reference)
                    snprintf(buf, sizeof buf,
                             "the picture changed in at most %.2f%% of its pixels within %u ms "
                             "(wanted %.1f%%)",
                             100 * change, st.ms, 100 * st.fraction);
                else
                    snprintf(buf, sizeof buf, "no picture to compare with (no frame pixels)");
                failed(st, buf);
            }
            return false;
        }
        case Op::WaitStill: {
            uint32_t run;
            {
                std::lock_guard<std::mutex> lock(m);
                run = still_run;
            }
            if (run >= st.count)
                return true;
            if (now - step_ms > st.ms) {
                char buf[96];
                snprintf(buf, sizeof buf, "no %u identical frames in a row within %u ms", st.count,
                         st.ms);
                failed(st, buf);
            }
            return false;
        }
        case Op::WaitPointer: {
            double x, y;
            if (read_pointer(&x, &y)) {
                say("line %d: pointer at (%.1f, %.1f)", st.line, x, y);
                return true;
            }
            if (now - step_ms > st.ms)
                failed(st, script.has_pointer ? "the pointer did not become readable"
                                              : "the script names no pointer");
            return false;
        }
        case Op::WaitSize: {
            int w, h;
            {
                std::lock_guard<std::mutex> lock(m);
                w = latest_w;
                h = latest_h;
            }
            if (w == st.x && h == st.y)
                return true;
            if (now - step_ms > st.ms) {
                char buf[96];
                snprintf(buf, sizeof buf, "no %dx%d frame within %u ms (the last was %dx%d)", st.x,
                         st.y, st.ms, w, h);
                failed(st, buf);
            }
            return false;
        }
        case Op::WaitPixel: {
            bool have = false;
            uint32_t got = 0;
            {
                std::lock_guard<std::mutex> lock(m);
                if (!latest.empty() && st.x < latest_w && st.y < latest_h) {
                    // The thumbnail keeps every other pixel of every other row.
                    const int tx = std::min(st.x / 2, latest.w - 1);
                    const int ty = std::min(st.y / 2, latest.h - 1);
                    got = latest.px[size_t(ty) * size_t(latest.w) + size_t(tx)];
                    have = true;
                }
            }
            auto near = [&](int shift) {
                return abs(int((got >> shift) & 255) - int((st.color >> shift) & 255)) <= st.tol;
            };
            if (have && near(16) && near(8) && near(0)) {
                say("line %d: pixel (%d, %d) is %06x", st.line, st.x, st.y, got);
                return true;
            }
            if (now - step_ms > st.ms) {
                char buf[128];
                snprintf(buf, sizeof buf,
                         "pixel (%d, %d) was %06x, not %06x within %d, after %u ms", st.x, st.y,
                         got, st.color, st.tol, st.ms);
                failed(st, buf);
            }
            return false;
        }
        case Op::Move:
        case Op::Click: {
            if (phase == 0) {
                if (script.has_pointer) {
                    const int r = walk_pointer(sink, st, now);
                    if (r <= 0)
                        return false;
                } else {
                    sink.place(st.x, st.y);
                }
                if (st.op == Op::Move)
                    return true;
                phase = 1;
                phase_ms = now;
                phase_frames = frames_now;
                sink.button(st.button, true);
                return false;
            }
            if (phase == 1) {
                if (now - phase_ms < hold || frames_now - phase_frames < 3)
                    return false;
                sink.button(st.button, false);
                settle(now);
                return true;
            }
            return true;
        }
        case Op::MoveBy:
            sink.motion(st.x, st.y);
            return true;
        case Op::Button:
            sink.button(st.button, st.down == 1);
            return true;
        case Op::Key: {
            if (st.down >= 0) {
                sink.key(st.dik, st.down == 1);
                return true;
            }
            if (phase == 0) {
                sink.key(st.dik, true);
                phase = 1;
                phase_ms = now;
                phase_frames = frames_now;
                return false;
            }
            if (phase == 1) {
                if (now - phase_ms < hold || frames_now - phase_frames < 3)
                    return false;
                sink.key(st.dik, false);
                settle(now);
                return true;
            }
            return true;
        }
        case Op::Dump: {
            if (phase == 0) {
                if (dumps_armed >= max_dumps) {
                    failed(st, "more than " + std::to_string(max_dumps) + " dumps");
                    return false;
                }
                if (out_dir.empty()) {
                    failed(st, "no output directory");
                    return false;
                }
                os_mkdir(out_dir.c_str());
                std::lock_guard<std::mutex> lock(m);
                pending_dumps.push_back({out_dir + "/" + st.text + ".png", st.text});
                ++dumps_armed;
                phase = 1;
                return false;
            }
            uint32_t done_now;
            {
                std::lock_guard<std::mutex> lock(m);
                done_now = dumps_done;
            }
            if (done_now >= dumps_armed)
                return true;
            if (now - step_ms > 10000)
                failed(st, "no frame was presented to write");
            return false;
        }
        case Op::Mark: {
            std::lock_guard<std::mutex> lock(m);
            reference = latest;
            reference_wanted = latest.empty();
            change_since_reference = 0;
            return true;
        }
        case Op::Peek: {
            std::string line;
            char buf[32];
            uint32_t base = st.addr.base;
            if (st.addr.deref && !(reader && reader(st.addr.base, &base))) {
                say("line %d: peek: [%08x] is not readable", st.line, st.addr.base);
                return true;
            }
            base += uint32_t(st.addr.offset);
            for (uint32_t i = 0; i < st.count; ++i) {
                uint32_t v = 0;
                if (reader && reader(base + 4 * i, &v))
                    snprintf(buf, sizeof buf, " %08x", v);
                else
                    snprintf(buf, sizeof buf, " ????????");
                line += buf;
            }
            say("line %d: peek %08x:%s", st.line, base, line.c_str());
            return true;
        }
        case Op::Log:
            say("line %d: %s", st.line, st.text.c_str());
            return true;
        case Op::Quit:
            sink.close();
            return true;
        }
        return true;
    }
};

Runner::Runner() : impl_(std::make_unique<Impl>()) {}
Runner::~Runner() = default;

bool Runner::load_text(const std::string &text, std::string *error) {
    Script s;
    if (!parse(text, &s, error))
        return false;
    impl_->script = std::move(s);
    impl_->loaded = true;
    return true;
}

bool Runner::load_file(const std::string &path, std::string *error) {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) {
        if (error)
            *error = "cannot read " + path;
        return false;
    }
    std::string text;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0)
        text.append(buf, n);
    fclose(f);
    if (!load_text(text, error)) {
        if (error)
            *error = path + ": " + *error;
        return false;
    }
    return true;
}

void Runner::set_output(const std::string &dir, uint32_t max_dumps) {
    impl_->out_dir = dir;
    impl_->max_dumps = max_dumps;
}
void Runner::set_memory_reader(MemoryReader reader) {
    impl_->reader = reader;
}
void Runner::set_log(FILE *log) {
    impl_->log = log ? log : stdout;
}

void Runner::tick(Sink &sink, uint32_t now, uint64_t frames) {
    Impl &d = *impl_;
    if (!d.loaded || d.done || d.fail)
        return;
    d.frames_now = frames;
    if (!d.started) {
        d.started = true;
        d.start_ms = now;
        d.say("%zu steps; timeout %u ms%s", d.script.steps.size(), d.script.timeout_ms,
              d.script.has_pointer ? "; the game's pointer is read back" : "");
    }
    if (now - d.start_ms > d.script.timeout_ms) {
        d.fail = true;
        d.why = "the script did not finish within " + std::to_string(d.script.timeout_ms) + " ms";
        if (d.pc < d.script.steps.size())
            d.why += " (at line " + std::to_string(d.script.steps[d.pc].line) + ")";
        d.say("FAILED %s", d.why.c_str());
        return;
    }
    // Run every step that completes now; stop at the first that has to wait.
    for (int guard = 0; guard < 64 && d.pc < d.script.steps.size(); ++guard) {
        const Step &st = d.script.steps[d.pc];
        if (!d.step_begun && d.settling && is_action(st.op)) {
            if (now - d.settle_ms < 40 || frames - d.settle_frames < 2)
                return;
            d.settling = false;
        }
        if (!d.step_begun) {
            d.step_begun = true;
            d.step_ms = now;
            d.step_frames = frames;
            d.phase = 0;
            d.move = Impl::Move{d.move.gain_x, d.move.gain_y};
            if (is_action(st.op))
                d.baseline();
        }
        const bool complete = d.run_step(sink, st, now);
        if (d.fail)
            return;
        if (!complete)
            return;
        if (st.op == Op::Move || st.op == Op::Click) {
            if (d.script.has_pointer)
                d.say("line %d: %s (%d, %d): pointer at (%.1f, %.1f) after %d steps", st.line,
                      op_name(st.op), st.x, st.y, d.last_px, d.last_py, d.move.iterations);
            else
                d.say("line %d: %s (%d, %d)", st.line, op_name(st.op), st.x, st.y);
        }
        ++d.steps_run;
        ++d.pc;
        d.step_begun = false;
    }
    if (d.pc >= d.script.steps.size() && !d.done) {
        d.done = true;
        size_t written;
        {
            std::lock_guard<std::mutex> lock(d.m);
            written = d.dumps_written.size();
        }
        d.say("finished: %u steps in %.1f s, %zu frames written", d.steps_run,
              (now - d.start_ms) / 1000.0, written);
    }
}

void Runner::pixels(const uint32_t *argb, int w, int h) {
    Impl &d = *impl_;
    if (!argb || w <= 0 || h <= 0 || !d.loaded)
        return;
    Thumb t = thumbnail(argb, w, h);
    std::vector<std::pair<std::string, std::string>> dumps;
    {
        std::lock_guard<std::mutex> lock(d.m);
        ++d.pixel_frames;
        d.still_run = (!d.latest.empty() && d.latest.hash == t.hash) ? d.still_run + 1 : 1;
        if (d.reference_wanted) {
            d.reference = t;
            d.reference_wanted = false;
        } else if (!d.reference.empty()) {
            d.change_since_reference =
                std::max(d.change_since_reference, difference(t, d.reference));
        }
        dumps.swap(d.pending_dumps);
    }
    for (const auto &dump : dumps) {
        const bool ok = write_png(dump.first, argb, w, h);
        double vs_last;
        {
            std::lock_guard<std::mutex> lock(d.m);
            vs_last = d.last_dump.empty() ? 1.0 : difference(t, d.last_dump);
            d.last_dump = t;
            if (ok)
                d.dumps_written.push_back(dump.second);
            ++d.dumps_done;
        }
        if (ok) {
            d.say("dump %s: %dx%d at frame %llu, hash %016llx, differs from the previous dump "
                  "in %.1f%% of its pixels",
                  dump.second.c_str(), w, h, (unsigned long long)d.frames_now.load(),
                  (unsigned long long)t.hash, 100 * vs_last);
        } else {
            d.say("dump %s: could not write %s", dump.second.c_str(), dump.first.c_str());
        }
    }
    std::lock_guard<std::mutex> lock(d.m);
    d.latest = std::move(t);
    d.latest_w = w;
    d.latest_h = h;
}

bool Runner::wants_pixels() const {
    return running();
}

void Runner::message(uint32_t, uint32_t msg) {
    Impl &d = *impl_;
    if (!d.loaded)
        return;
    std::lock_guard<std::mutex> lock(d.m);
    ++d.messages[msg];
}

bool Runner::loaded() const {
    return impl_->loaded;
}
bool Runner::running() const {
    return impl_->loaded && !impl_->done && !impl_->fail;
}
bool Runner::finished() const {
    return impl_->done;
}
bool Runner::failed() const {
    return impl_->fail;
}
std::string Runner::failure() const {
    return impl_->why;
}

void Runner::report(FILE *out) const {
    const Impl &d = *impl_;
    if (!d.loaded)
        return;
    size_t written;
    {
        std::lock_guard<std::mutex> lock(d.m);
        written = d.dumps_written.size();
    }
    fprintf(out, "input script: %s; %u of %zu steps run; %zu frames written%s%s\n",
            d.fail   ? "FAILED"
            : d.done ? "finished"
                     : "unfinished",
            d.steps_run, d.script.steps.size(), written, d.fail ? ": " : "",
            d.fail ? d.why.c_str() : "");
}

} // namespace iscript
