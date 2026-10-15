// input_script_host.h - scripted input (input_script.h) in a host that boots a
// game: the guest-memory reader, the window-message observer, and the input
// path of a host with no window.
//
// A host owns one script for the run:
//
//   iscript::host_start(path, out_dir, &error)   once, before boot_run
//   iscript::host_tick(sink, frames)             from the host's tick
//   iscript::host_pixels(argb, w, h)             with every presented frame
//   iscript::host_runner()->report(out)          in the host's report
//
// RECOMP_INPUT_SCRIPT names a script and RECOMP_INPUT_SCRIPT_OUT the directory
// its dumps go to (default: beside the script, in <script>-frames/), for a
// host that takes its options from the environment (the windowed app).
#pragma once
#include "input_script.h"

#include <string>

namespace iscript {

// Reads the guest arena; false outside it.
bool guest_memory_read(uint32_t address, uint32_t *value);

// Loads the script and installs the message observer. False with `error` when
// the script does not load. Once per process.
bool host_start(const std::string &path, const std::string &out_dir, std::string *error);
// The same from RECOMP_INPUT_SCRIPT / RECOMP_INPUT_SCRIPT_OUT. False when none
// is named; a script that is named and does not load ends the process with a
// message, because a run that silently ignores its script proves nothing.
bool host_start_from_env();
// The run's script, or null.
Runner *host_runner();
// Advances it on the host's clock (os_monotonic) and the frames presented.
void host_tick(Sink &sink, uint64_t frames);
// A presented frame, for the script's dumps and picture waits.
void host_pixels(const uint32_t *argb, int w, int h);

// The input path of a host without a window: what the windowed host's event
// handlers do with a real device's events, done directly. DirectInput gets the
// motion, buttons and keys (host/input.cpp's state, which wakes the guest's
// input threads); the message queue gets the window messages Windows would
// post, which for a mouse DirectInput holds exclusively is none. Call under
// the guest baton (from a boot tick).
class HeadlessSink : public Sink {
  public:
    HeadlessSink();
    void motion(int32_t dx, int32_t dy) override;
    void place(int32_t x, int32_t y) override;
    void button(int button, bool down) override;
    void key(uint8_t dik, bool down) override;
    void close() override;

  private:
    int32_t x_ = 0, y_ = 0;
    uint8_t buttons_ = 0;
};

} // namespace iscript
