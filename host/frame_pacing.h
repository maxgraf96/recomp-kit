#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <vector>

// Monotonic seconds; no display-link tick or GPU submission is counted as a
// displayed frame. Offscreen/fallback completions are deliberately excluded.
struct FramePacingSnapshot {
    double new_fps = 0, display_fps = 0, median_ms = 0, p95_ms = 0, gpu_ms = 0, age_ms = 0;
    double repeat_percent = 0;
    uint64_t new_frames = 0, repeats = 0, drops = 0;
    std::vector<double> intervals_ms;
};
class FramePacing {
    struct Event {
        double time, gpu_ms, age_ms;
        bool repeat;
    };
    std::deque<Event> events;
    std::deque<double> intervals;
    double first = -1, last_new = -1;
    uint64_t unique = 0, repeated = 0;

  public:
    void displayed(double ts, bool repeat, double gpu_ms, double age_ms) {
        if (!std::isfinite(ts) || ts < 0 || (!events.empty() && ts < events.back().time))
            return;
        if (first < 0)
            first = ts;
        if (repeat)
            ++repeated;
        else {
            ++unique;
            if (last_new >= 0 && ts > last_new) {
                intervals.push_back((ts - last_new) * 1000);
                if (intervals.size() > 120)
                    intervals.pop_front();
            }
            last_new = ts;
        }
        events.push_back({ts, std::max(0.0, gpu_ms), std::max(0.0, age_ms), repeat});
        while (events.size() > 1024 || (!events.empty() && events.front().time < ts - 2))
            events.pop_front();
    }
    FramePacingSnapshot snapshot(double now, uint64_t drops) const {
        FramePacingSnapshot s;
        s.new_frames = unique;
        s.repeats = repeated;
        s.drops = drops;
        int n = 0, r = 0;
        double gpu = 0, age = 0;
        for (const auto &e : events)
            if (e.time > now - 2 && e.time <= now) {
                ++n;
                r += e.repeat;
                gpu += e.gpu_ms;
                age += e.age_ms;
            }
        const double span = first >= 0 ? std::min(2.0, std::max(0.0, now - first)) : 0;
        if (span >= 0.25) {
            s.new_fps = (n - r) / span;
            s.display_fps = n / span;
        }
        if (n) {
            s.gpu_ms = gpu / n;
            s.age_ms = age / n;
            s.repeat_percent = 100.0 * r / n;
        }
        if (last_new >= 0 && now - last_new < 2 && !intervals.empty()) {
            s.intervals_ms.assign(intervals.begin(), intervals.end());
            auto sorted = s.intervals_ms;
            std::sort(sorted.begin(), sorted.end());
            s.median_ms = sorted[sorted.size() / 2];
            s.p95_ms = sorted[size_t(std::ceil(sorted.size() * .95)) - 1];
        }
        return s;
    }
};

// How long a present that waits for the vertical blank (a DirectDraw 7 Flip
// without DDFLIP_NOVSYNC, a DXGI Present with a sync interval) waits, `now`
// and `period` in seconds on the presenter's device clock. Without a frame
// limit it is the time to the refresh boundary `intervals` refreshes on, as
// Windows' vertical blank. With one (`limit` frames a second, the settings
// page's Frame limit) the frames also keep to a deadline that advances by
// 1/limit each frame, and the wait ends at the first boundary no earlier than
// half a refresh before it: 60 on a 120 Hz display is every other refresh, and
// a rate the display does not divide still averages the limit. A frame more
// than half a refresh past its deadline starts a new phase at the next
// boundary rather than catching up in a burst. `deadline` is the caller's
// state across frames (0 to start). The limit only ever adds refreshes to the
// wait the game asked for.
// How long an unsynced present (a DirectDraw 7 Flip with DDFLIP_NOVSYNC, or a
// vsync-paced one with the pacing turned off) waits under the Frame limit,
// `now` in seconds on the presenter's device clock. There is no vertical blank
// to wait for, so the frames keep to the limit's deadline alone: 1/limit apart
// on average, and a frame more than half a frame past its deadline starts a new
// phase rather than catching up in a burst. Without a limit it does not wait.
// `deadline` is the caller's state across frames (0 to start), shared with
// paced_refresh_delay.
inline double paced_limit_delay(double now, int limit, double *deadline) {
    if (limit <= 0 || !deadline) {
        if (deadline)
            *deadline = 0;
        return 0.0;
    }
    const double frame = 1.0 / limit;
    double target = *deadline + frame;
    if (*deadline <= 0 || target < now - frame / 2)
        target = now;
    *deadline = target;
    return target > now ? target - now : 0.0;
}

inline double paced_refresh_delay(double now, double period, int intervals, int limit,
                                  double *deadline) {
    if (!(period > 0) || intervals <= 0)
        return 0.0;
    double next = (std::floor(now / period) + intervals) * period;
    if (limit > 0 && deadline) {
        const double target = *deadline + 1.0 / limit;
        if (*deadline <= 0 || target < now - period / 2) {
            *deadline = next;
        } else {
            const double earliest = target - period / 2;
            if (next < earliest)
                next = std::ceil(earliest / period) * period;
            *deadline = target;
        }
    } else if (deadline) {
        *deadline = 0;
    }
    return next > now ? next - now : 0.0;
}
