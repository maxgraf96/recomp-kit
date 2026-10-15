// input_script.h - scripted input: driving a running game the way a person at
// the keyboard and mouse would, with nobody there.
//
// A script is a list of steps run in order. Each step either acts (moves the
// pointer, presses a button or a key, writes a frame) or waits for something
// the game does (time passing, frames being presented, a window message, the
// picture changing or settling). Nothing reaches the guest by a side door: the
// host turns every action into the events its own input path already handles
// (input_script_host.cpp for a host without a window, SDL events in the
// windowed host), so DirectInput, the message queue and GetAsyncKeyState see
// what they would see from real hardware.
//
// THE GAME'S OWN POINTER
//
// A game that holds its DirectInput mouse exclusively integrates relative
// motion into a cursor of its own; an absolute position means nothing to it.
// A script can name where the game keeps that cursor:
//
//   pointer f32 [0x00123450]+4 [0x00123450]+8
//
// (`[a]` reads the 32-bit pointer at a; `+n` adds to it; i32 or f32 values).
// `move` and `click` then walk the game's cursor to the target with relative
// motion, reading it back after each step, so the gain the game applies (and
// any acceleration or history in it) does not have to be known. Without a
// `pointer`, `move` places the pointer absolutely, which is what a game that
// reads window messages sees.
//
// THE LANGUAGE (one step per line; `#` starts a comment)
//
//   timeout MS                    the whole script fails after MS (default 600000)
//   pointer i32|f32 XADDR YADDR   the game's cursor, as above
//   pointer_tolerance PX          how close `move` must get (default 2; a game
//                                 whose gain is coarser is accepted within half
//                                 its measured step)
//   hold MS                       how long `click` and `key NAME` hold (default 80)
//   wait MS                       host time
//   wait frames N                 N more presented frames
//   wait message MSG [within MS]  a window procedure received MSG (a number or
//                                 a WM_ name)
//   wait change [FRACTION] [within MS]
//                                 the presented picture differs from the one
//                                 before the last input step in more than
//                                 FRACTION of its pixels (default 0.05)
//   wait still N [within MS]      N presented frames in a row are identical
//   wait pointer [within MS]      the `pointer` reads a position
//   wait size W H [within MS]     the presented frame is W x H (a display mode
//                                 the game has switched to)
//   wait pixel X Y RRGGBB [tol N] [within MS]
//                                 the presented frame's pixel at (X, Y) is this
//                                 colour, each channel within N (default 16):
//                                 a screen known by one of its own pixels, when
//                                 a loading screen comes and goes before it
//   move X Y                      the pointer to client pixel (X, Y)
//   moveby DX DY                  relative motion, in mouse counts
//   click [left|right|middle] X Y move, press, hold, release
//   button left|right|middle down|up
//   key NAME [down|up]            a DirectInput key name (ESCAPE, RETURN, A, F1,
//                                 ...); without down/up: press and release
//   dump NAME                     the next presented frame as OUT/NAME.png
//   mark                          the picture `wait change` compares against
//                                 becomes the latest one now
//   peek ADDR [COUNT]             print COUNT dwords of guest memory at ADDR
//   log TEXT                      print TEXT
//   quit                          ask the game to close, as closing its window does
//
// Every wait that takes `within` fails the script when it runs out; the
// default is 30000 ms. A failed script stops where it failed and says why.
#pragma once
#include <stdint.h>
#include <stdio.h>

#include <memory>
#include <string>
#include <vector>

namespace iscript {

enum class Op {
    Wait,
    WaitFrames,
    WaitMessage,
    WaitChange,
    WaitStill,
    WaitPointer,
    WaitSize,
    WaitPixel,
    Move,
    MoveBy,
    Click,
    Button,
    Key,
    Dump,
    Mark,
    Peek,
    Log,
    Quit,
};

// A guest address, optionally through one pointer: [base]+offset or base+offset.
struct Address {
    uint32_t base = 0;
    int32_t offset = 0;
    bool deref = false;
};

struct Step {
    Op op = Op::Wait;
    int line = 0;
    uint32_t ms = 0;        // wait; within (the timeout of a wait)
    uint32_t count = 0;     // frames, still, peek count
    uint32_t msg = 0;       // wait message
    double fraction = 0.05; // wait change
    int32_t x = 0, y = 0;   // move, click; moveby: the counts
    int button = 0;         // 0 left, 1 right, 2 middle
    int down = -1;          // button/key: 1 down, 0 up, -1 press and release
    uint8_t dik = 0;        // key
    uint32_t color = 0;     // wait pixel: 0xRRGGBB
    int tol = 16;           // wait pixel: per channel
    Address addr;           // peek
    std::string text;       // dump name, log text, key name
};

struct Script {
    std::vector<Step> steps;
    uint32_t timeout_ms = 600000;
    bool has_pointer = false;
    bool pointer_float = false;
    Address pointer_x, pointer_y;
    double pointer_tolerance = 2.0;
    uint32_t hold_ms = 80;
};

// Parses `text`. False with `error` naming the line and the problem.
bool parse(const std::string &text, Script *out, std::string *error);
// The DirectInput scan code a key name stands for, or 0.
uint8_t dik_for_name(const char *name);
// A window message's number from a WM_ name or a number, or 0.
uint32_t message_for_name(const char *name);

// What the runner asks of the host. Every call is made from Runner::tick, on
// the thread and under whatever lock the host calls tick with.
class Sink {
  public:
    virtual ~Sink() = default;
    // Relative motion in mouse counts, as a DirectInput mouse reports it.
    virtual void motion(int32_t dx, int32_t dy) = 0;
    // The pointer at client pixel (x, y), as a desktop pointer arrives.
    virtual void place(int32_t x, int32_t y) = 0;
    virtual void button(int button, bool down) = 0;
    virtual void key(uint8_t dik, bool down) = 0;
    // The game is asked to close (WM_CLOSE to its main window).
    virtual void close() = 0;
};

// Reads 32 guest bits; false when the address is not readable.
using MemoryReader = bool (*)(uint32_t address, uint32_t *value);

// Writes BGRA/ARGB pixels (0xAARRGGBB words, alpha ignored) as an RGB PNG.
bool write_png(const std::string &path, const uint32_t *argb, int w, int h);

class Runner {
  public:
    Runner();
    ~Runner();
    Runner(const Runner &) = delete;
    Runner &operator=(const Runner &) = delete;

    // The script. False with `error` when it does not parse or cannot be read.
    bool load_file(const std::string &path, std::string *error);
    bool load_text(const std::string &text, std::string *error);
    // Where `dump` writes, created on first use. At most `max_dumps` frames are
    // ever written (default 64); a script that asks for more fails.
    void set_output(const std::string &dir, uint32_t max_dumps = 64);
    void set_memory_reader(MemoryReader reader);
    void set_log(FILE *log); // default stdout; lines start with "[script]"

    // Advances the script. `now_ms` is the host's monotonic clock and
    // `frames` the number of frames presented so far.
    void tick(Sink &sink, uint32_t now_ms, uint64_t frames);
    // A presented frame's pixels. Any thread; the host passes every presented
    // frame it can, or at least every one while wants_pixels() says so.
    void pixels(const uint32_t *argb, int w, int h);
    bool wants_pixels() const;
    // A window procedure is about to receive `msg`. Any thread.
    void message(uint32_t hwnd, uint32_t msg);

    bool loaded() const;
    bool running() const;  // loaded, not finished and not failed
    bool finished() const; // every step ran
    bool failed() const;
    std::string failure() const;
    // A summary for the host's report.
    void report(FILE *out) const;

    struct Impl;

  private:
    std::unique_ptr<Impl> impl_;
};

} // namespace iscript
