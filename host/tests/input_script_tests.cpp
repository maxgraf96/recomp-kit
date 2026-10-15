// input_script_tests.cpp - scripted input (host/input_script.h) against a
// simulated game: the parser, the closed-loop pointer, the timed actions, the
// waits and the frame dumps. No guest, no window.
#include "../input_script.h"
#include "../../platform/os.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

static int g_failures = 0, g_checks = 0;
#define CHECK(c)                                                                                   \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(c)) {                                                                                \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                           \
            ++g_failures;                                                                          \
        }                                                                                          \
    } while (0)

using namespace iscript;

// ---------------------------------------------------------------------------
// A game that integrates relative motion into a cursor of its own, as an
// exclusive-DirectInput game does: it reads its mouse every 25 ms, scales the
// counts by a gain that depends on the smallest motion it has seen (one real
// game does exactly that), clamps to its screen, and keeps the result as two
// floats behind a pointer.
// ---------------------------------------------------------------------------
struct FakeGame : Sink {
    float x = 512, y = 384;
    int32_t pend_x = 0, pend_y = 0;
    uint32_t last_poll = 0;
    int min_x = 100, min_y = 100; // the smallest |count| seen, per axis
    double k = 2.234;             // pixels for a count when the smallest is 1
    std::vector<std::string> events;
    std::vector<uint32_t> downs, ups; // when buttons went down and up
    uint32_t now = 0;
    bool closed = false;
    bool readable = true;

    void motion(int32_t dx, int32_t dy) override {
        pend_x += dx;
        pend_y += dy;
    }
    void place(int32_t px, int32_t py) override {
        events.push_back("place " + std::to_string(px) + " " + std::to_string(py));
    }
    void button(int b, bool down) override {
        events.push_back("button " + std::to_string(b) + (down ? " down" : " up"));
        (down ? downs : ups).push_back(now);
    }
    void key(uint8_t dik, bool down) override {
        char buf[32];
        snprintf(buf, sizeof buf, "key %02x %s", dik, down ? "down" : "up");
        events.push_back(buf);
    }
    void close() override {
        closed = true;
    }

    void poll(uint32_t now) {
        if (now - last_poll < 25)
            return;
        last_poll = now;
        if (pend_x) {
            min_x = std::min(min_x, abs(pend_x));
            x += float(k * pend_x / min_x);
        }
        if (pend_y) {
            min_y = std::min(min_y, abs(pend_y));
            y += float(k * pend_y / min_y);
        }
        pend_x = pend_y = 0;
        x = std::max(0.0f, std::min(1023.9f, x));
        y = std::max(0.0f, std::min(767.9f, y));
    }
};
static FakeGame *g_game = nullptr;

// The fake guest memory: [0x1000] = 0x2000, the cursor at 0x2004 / 0x2008.
static bool read_fake(uint32_t a, uint32_t *out) {
    if (!g_game || !g_game->readable)
        return false;
    if (a == 0x1000) {
        *out = 0x2000;
        return true;
    }
    float f;
    if (a == 0x2004)
        f = g_game->x;
    else if (a == 0x2008)
        f = g_game->y;
    else
        return false;
    memcpy(out, &f, 4);
    return true;
}

// Runs the script to its end (or `limit` ms) at a 16 ms frame; `frame`
// makes the pixels of each frame from (frame number, the game).
struct Sim {
    Runner r;
    FakeGame game;
    uint32_t now = 1000;
    uint64_t frames = 0;
    std::vector<uint32_t> pixels = std::vector<uint32_t>(64 * 48, 0xff102030u);
    void (*paint)(Sim &) = nullptr;
    uint32_t messages_at = 0, message = 0; // a message at this time

    explicit Sim(const char *text, MemoryReader reader = read_fake) {
        std::string error;
        CHECK(r.load_text(text, &error));
        if (!error.empty())
            fprintf(stderr, "  parse: %s\n", error.c_str());
        r.set_memory_reader(reader);
        r.set_log(stderr);
        g_game = &game;
    }
    void run(uint32_t limit) {
        const uint32_t end = now + limit;
        while (r.running() && now < end) {
            now += 4;
            game.now = now;
            game.poll(now);
            if (message && now >= messages_at) {
                r.message(0x10, message);
                message = 0;
            }
            if (now % 16 == 0) {
                ++frames;
                if (paint)
                    paint(*this);
                r.pixels(pixels.data(), 64, 48);
            }
            r.tick(game, now, frames);
        }
    }
};

static void test_parse() {
    Script s;
    std::string e;
    CHECK(parse("# a comment\n\nwait 10\nwait frames 3\nwait message WM_CLOSE within 50\n"
                "wait change 0.1 within 99\nwait still 4\nwait pointer\nmove 1 2\nmoveby -3 4\n"
                "click 5 6\nclick right 7 8\nbutton middle down\nkey ESCAPE\nkey dik_return up\n"
                "dump a-b_c.1\nmark\npeek [0x10]+4 3\nlog hello world  \nquit\n",
                &s, &e));
    CHECK(s.steps.size() == 18);
    CHECK(s.steps[0].op == Op::Wait && s.steps[0].ms == 10 && s.steps[0].line == 3);
    CHECK(s.steps[2].op == Op::WaitMessage && s.steps[2].msg == 0x10 && s.steps[2].ms == 50);
    CHECK(s.steps[3].op == Op::WaitChange && fabs(s.steps[3].fraction - 0.1) < 1e-9 &&
          s.steps[3].ms == 99);
    CHECK(s.steps[4].op == Op::WaitStill && s.steps[4].count == 4 && s.steps[4].ms == 30000);
    CHECK(s.steps[7].op == Op::MoveBy && s.steps[7].x == -3 && s.steps[7].y == 4);
    CHECK(s.steps[8].op == Op::Click && s.steps[8].button == 0);
    CHECK(s.steps[9].op == Op::Click && s.steps[9].button == 1 && s.steps[9].x == 7);
    CHECK(s.steps[10].op == Op::Button && s.steps[10].button == 2 && s.steps[10].down == 1);
    CHECK(s.steps[11].op == Op::Key && s.steps[11].dik == 0x01 && s.steps[11].down == -1);
    CHECK(s.steps[12].op == Op::Key && s.steps[12].dik == 0x1c && s.steps[12].down == 0);
    CHECK(s.steps[14].op == Op::Mark);
    CHECK(s.steps[15].op == Op::Peek && s.steps[15].addr.deref && s.steps[15].addr.base == 0x10 &&
          s.steps[15].addr.offset == 4 && s.steps[15].count == 3);
    CHECK(s.steps[16].text == "hello world");
    CHECK(!s.has_pointer && s.timeout_ms == 600000 && s.hold_ms == 80);

    CHECK(parse("pointer f32 [0x9197a8]+4 [0x9197a8]+8\npointer_tolerance 3\nhold 120\n"
                "timeout 5000\n",
                &s, &e));
    CHECK(s.has_pointer && s.pointer_float && s.pointer_x.deref && s.pointer_x.base == 0x9197a8 &&
          s.pointer_y.offset == 8 && s.pointer_tolerance == 3 && s.hold_ms == 120 &&
          s.timeout_ms == 5000 && s.steps.empty());
    CHECK(parse("pointer i32 0x500 0x500-4\n", &s, &e));
    CHECK(!s.pointer_float && !s.pointer_x.deref && s.pointer_y.offset == -4);

    // Every mistake names its line.
    const char *bad[] = {"wait",
                         "wait soon",
                         "move 1",
                         "move 1 x",
                         "click up 1 2",
                         "key NOPE",
                         "key A sideways",
                         "dump ../x",
                         "wait frames 0",
                         "wait still 1",
                         "wait change 2",
                         "pointer f64 1 2",
                         "pointer f32 [1 2",
                         "frobnicate",
                         "wait message WM_NOPE",
                         "wait 5 within 3",
                         "peek 0x10 99",
                         "timeout -1",
                         "wait size 640",
                         "wait size 0 480",
                         "wait pixel 1 2",
                         "wait pixel 1 2 12345",
                         "wait pixel 1 2 123456 tol 300"};
    for (const char *b : bad) {
        std::string text = std::string("wait 1\n") + b + "\n";
        e.clear();
        const bool ok = parse(text, &s, &e);
        CHECK(!ok);
        CHECK(e.rfind("line 2:", 0) == 0);
        if (ok)
            fprintf(stderr, "  accepted: %s\n", b);
    }
    CHECK(dik_for_name("escape") == 0x01 && dik_for_name("DIK_F12") == 0x58 &&
          dik_for_name("A") == 0x1e && dik_for_name("PGDN") == 0xd1 && dik_for_name("D") == 0x20 &&
          dik_for_name("DIK") == 0 && dik_for_name("") == 0);
    CHECK(message_for_name("WM_LBUTTONDOWN") == 0x201 && message_for_name("0x400") == 0x400 &&
          message_for_name("wm_close") == 0x10 && message_for_name("0") == 0);
}

// The closed loop reaches targets whatever the gain does, and the click is a
// press held for frames and time, then a release.
static void test_pointer_walk_and_click() {
    Sim sim("pointer f32 [0x1000]+4 [0x1000]+8\n"
            "move 100 700\nclick 900 50\nmove 0 0\nmove 1023 767\nquit\n");
    sim.run(60000);
    CHECK(sim.r.finished());
    CHECK(sim.game.closed);
    // Ended at the corner, within a count of it.
    CHECK(sim.game.x > 1020 && sim.game.y > 764);
    CHECK(sim.game.events.size() == 2 && sim.game.events[0] == "button 0 down" &&
          sim.game.events[1] == "button 0 up");
}

// Where the click happened: the pointer was at the target when the button went down.
static float g_press_x = -1, g_press_y = -1;
struct PressSpy : FakeGame {
    void button(int b, bool down) override {
        if (down) {
            g_press_x = x;
            g_press_y = y;
        }
        FakeGame::button(b, down);
    }
};
static void test_click_lands_on_target() {
    std::string e;
    Runner r;
    CHECK(r.load_text("pointer f32 [0x1000]+4 [0x1000]+8\nclick 300 200\n", &e));
    r.set_memory_reader(read_fake);
    r.set_log(stderr);
    PressSpy game;
    g_game = &game;
    uint32_t now = 0;
    uint64_t frames = 0;
    while (r.running() && now < 30000) {
        now += 4;
        game.poll(now);
        if (now % 16 == 0)
            ++frames;
        r.tick(game, now, frames);
    }
    CHECK(r.finished());
    CHECK(fabs(g_press_x - 300) <= 2.5 && fabs(g_press_y - 200) <= 2.5);
}

// Without a pointer to read, `move` places the pointer absolutely.
static void test_place_without_pointer() {
    Sim sim("move 10 20\nclick right 30 40\nkey RETURN\nkey F1 down\nmoveby 5 -5\n");
    sim.run(5000);
    CHECK(sim.r.finished());
    sim.game.poll(sim.now + 100); // the game reads the last motion
    const std::vector<std::string> want = {"place 10 20", "place 30 40", "button 1 down",
                                           "button 1 up", "key 1c down", "key 1c up",
                                           "key 3b down"};
    CHECK(sim.game.events == want);
    CHECK(sim.game.pend_x == 0 && sim.game.x > 512); // the moveby was relative motion
}

// A click is done at its release - a click that ends the game still finishes
// its script - and the next action waits for the game to have read it.
static void test_click_settles_before_the_next_action() {
    Sim sim("click 10 10\nclick 20 20\nkey RETURN\n");
    sim.run(5000);
    CHECK(sim.r.finished());
    CHECK(sim.game.downs.size() == 2 && sim.game.ups.size() == 2);
    if (sim.game.downs.size() == 2 && sim.game.ups.size() == 2)
        CHECK(sim.game.downs[1] >= sim.game.ups[0] + 40);
    Sim last("click 10 10\n");
    last.run(5000);
    CHECK(last.r.finished());
    CHECK(!last.game.ups.empty() && last.now == last.game.ups.back()); // finished at the release
}

static void paint_after_click(Sim &sim) {
    // The picture changes once the button has gone down and up.
    const bool changed = sim.game.events.size() >= 2;
    std::fill(sim.pixels.begin(), sim.pixels.end(), changed ? 0xff808080u : 0xff102030u);
}
static void test_waits() {
    {
        Sim sim("wait frames 10\nwait 100\n");
        sim.run(5000);
        CHECK(sim.r.finished());
        CHECK(sim.frames >= 10);
    }
    {
        // The picture changes after the click: `wait change` sees it.
        Sim sim("wait frames 2\nclick 1 1\nwait change within 2000\nwait still 3 within 2000\n");
        sim.paint = paint_after_click;
        sim.run(10000);
        CHECK(sim.r.finished());
    }
    {
        // A picture that never changes fails `wait change`, with the line.
        Sim sim("click 1 1\nwait change 0.5 within 300\nlog never\n");
        sim.run(10000);
        CHECK(sim.r.failed());
        CHECK(sim.r.failure().find("line 2: wait change") == 0);
    }
    {
        // A message after the last action counts; one before it does not.
        Sim sim("key SPACE\nwait message WM_COMMAND within 500\n");
        sim.message = 0x111;
        sim.messages_at = sim.now + 200;
        sim.run(5000);
        CHECK(sim.r.finished());
    }
    {
        Sim sim("wait message WM_COMMAND within 300\n");
        sim.run(5000);
        CHECK(sim.r.failed());
        CHECK(sim.r.failure().find("WM_COMMAND (0x0111) not seen") != std::string::npos);
    }
    {
        // An unreadable pointer fails a move after the default patience.
        Sim sim("pointer f32 [0x1000]+4 [0x1000]+8\nwait pointer within 200\n");
        sim.game.readable = false;
        sim.run(5000);
        CHECK(sim.r.failed());
    }
    {
        // The frame's size: met by the 64x48 frames, failed by another.
        Sim sim("wait size 64 48 within 500\n");
        sim.run(5000);
        CHECK(sim.r.finished());
        Sim other("wait size 640 480 within 300\n");
        other.run(5000);
        CHECK(other.r.failed());
        CHECK(other.r.failure().find("no 640x480 frame within 300 ms (the last was 64x48)") !=
              std::string::npos);
    }
    {
        // A pixel: the frames are 0x102030 until a click paints them grey.
        Sim sim("wait frames 2\nwait pixel 10 10 102030 within 500\nclick 1 1\n"
                "wait pixel 11 11 828282 tol 4 within 2000\n");
        sim.paint = paint_after_click;
        sim.run(10000);
        CHECK(sim.r.finished());
        Sim wrong("wait pixel 10 10 ff0000 within 300\n");
        wrong.run(5000);
        CHECK(wrong.r.failed());
        CHECK(wrong.r.failure().find("pixel (10, 10) was 102030, not ff0000") != std::string::npos);
    }
    {
        // The whole-script timeout.
        Sim sim("timeout 500\nwait 10000\n");
        sim.run(5000);
        CHECK(sim.r.failed());
        CHECK(sim.r.failure().find("did not finish within 500 ms") != std::string::npos);
    }
}

static void test_dumps() {
    char dir[256];
    snprintf(dir, sizeof dir, "%s/input_script_tests-%d",
             getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", int(os_monotonic_ns() % 100000));
    {
        Sim sim("dump first\ndump second\n");
        sim.r.set_output(dir, 2);
        sim.run(5000);
        CHECK(sim.r.finished());
        for (const char *name : {"first", "second"}) {
            const std::string path = std::string(dir) + "/" + name + ".png";
            FILE *f = fopen(path.c_str(), "rb");
            CHECK(f != nullptr);
            unsigned char magic[8] = {0};
            if (f) {
                CHECK(fread(magic, 1, 8, f) == 8);
                fclose(f);
                remove(path.c_str());
            }
            CHECK(magic[0] == 0x89 && magic[1] == 'P' && magic[2] == 'N' && magic[3] == 'G');
        }
    }
    {
        // The cap holds: a third dump fails the script rather than filling a disk.
        Sim sim("dump a\ndump b\ndump c\n");
        sim.r.set_output(dir, 2);
        sim.run(5000);
        CHECK(sim.r.failed());
        CHECK(sim.r.failure().find("more than 2 dumps") != std::string::npos);
        remove((std::string(dir) + "/a.png").c_str());
        remove((std::string(dir) + "/b.png").c_str());
    }
    os_rmdir(dir);
}

int main() {
    test_parse();
    test_pointer_walk_and_click();
    test_click_lands_on_target();
    test_place_without_pointer();
    test_click_settles_before_the_next_action();
    test_waits();
    test_dumps();
    printf("input_script_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
