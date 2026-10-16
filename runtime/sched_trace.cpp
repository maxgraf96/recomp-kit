// sched_trace.cpp - RECOMP_SCHED_TRACE: see sched_trace.h.
#include "sched_trace.h"

#include "imports.h"
#include "../platform/os.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <map>
#include <mutex>
#include <string>
#include <vector>

bool g_sched_trace = [] {
    const char *v = recomp_env("SCHED_TRACE");
    return v && *v && strcmp(v, "0") != 0;
}();

namespace {

// Durations in microseconds, by powers of two: bucket k holds [2^(k-1), 2^k)
// microseconds, bucket 0 everything under one. 2^25 us is half a minute.
const int BUCKETS = 26;

struct Hist {
    uint64_t n = 0;
    double total = 0.0, max = 0.0;
    uint64_t bucket[BUCKETS] = {};
    void add(double seconds) {
        if (seconds < 0.0)
            seconds = 0.0;
        ++n;
        total += seconds;
        if (seconds > max)
            max = seconds;
        double us = seconds * 1e6;
        int k = us < 1.0 ? 0 : 1 + (int)floor(log2(us));
        if (k >= BUCKETS)
            k = BUCKETS - 1;
        ++bucket[k];
    }
    // The upper bound of the bucket the q-th quantile falls in, in seconds,
    // capped at the largest value seen.
    double quantile(double q) const {
        if (!n)
            return 0.0;
        uint64_t want = (uint64_t)ceil(q * (double)n), seen = 0;
        for (int k = 0; k < BUCKETS; ++k) {
            seen += bucket[k];
            if (seen >= want)
                return std::min(ldexp(1.0, k) * 1e-6, max);
        }
        return max;
    }
    void merge(const Hist &o) {
        n += o.n;
        total += o.total;
        max = std::max(max, o.max);
        for (int k = 0; k < BUCKETS; ++k)
            bucket[k] += o.bucket[k];
    }
};

struct Gap {
    double seconds = 0.0;
    uint32_t from = SCHED_SITE_NONE, to = SCHED_SITE_NONE;
};

struct Window {
    Hist hold, ready, handoff;
    double blocked = 0.0;
    Gap gap;
    std::map<std::string, uint64_t> left; // why it gave the baton up
    uint64_t preempted = 0;
};

struct ThreadStats {
    bool known = false, is_main = false, finished = false;
    uint32_t id = 0, start = 0, param = 0;
    int32_t priority = 0;
    uint32_t priority_sets = 0;
    std::string name;
    bool named = false;
    // Where it is now. At most one of these is set.
    double hold_since = 0.0, ready_since = 0.0, blocked_since = 0.0;
    double assigned_at = 0.0;
    double last_point = 0.0; // the last scheduling point it passed while holding
    uint32_t last_site = SCHED_SITE_NONE;
    Gap hold_gap; // the longest stretch without a point in this hold
    uint64_t hold_points = 0;
    Window win, all;
};

// Never destroyed: the final report runs from atexit, after static
// destructors that were registered later have already run.
std::vector<ThreadStats> &stats() {
    static std::vector<ThreadStats> *v = new std::vector<ThreadStats>;
    return *v;
}
ThreadStats &st(size_t idx) {
    if (idx >= stats().size())
        stats().resize(idx + 1);
    return stats()[idx];
}

const char *(*g_namer)(uint32_t, uint32_t) = nullptr;
void (*g_reporter)(FILE *, double) = nullptr;

FILE *g_out = nullptr;
double g_every = 10.0;
// RECOMP_SCHED_TRACE_LONG_MS: every hold and every wait for the baton longer
// than this is written out as it happens, a wait with the holds that filled it.
double g_long = 0.0;
struct HoldRec {
    size_t idx;
    double start, end;
    const char *why;
};
const int RING = 256;
HoldRec g_ring[RING];
uint64_t g_ring_n = 0;
double g_t0 = 0.0, g_last_report = 0.0;
double g_idle_win = 0.0, g_idle_all = 0.0;
uint64_t g_assign_win = 0, g_assign_all = 0;

// Input latency: the presses the host has seen and the guest has not read yet.
std::mutex g_input_m;
std::vector<uint64_t> g_input_pending;
Hist g_input_win, g_input_all;
std::map<std::string, uint64_t> g_input_how;
uint64_t g_input_dropped = 0;

double now_s() {
    return (double)os_monotonic_ns() * 1e-9;
}

void open_out() {
    if (g_out)
        return;
    const char *v = recomp_env("SCHED_TRACE");
    if (v && strcmp(v, "1") != 0 && strcmp(v, "stderr") != 0)
        g_out = fopen(v, "w");
    if (!g_out)
        g_out = stderr;
    const char *e = recomp_env("SCHED_TRACE_EVERY");
    if (e && atof(e) > 0.0)
        g_every = atof(e);
    const char *l = recomp_env("SCHED_TRACE_LONG_MS");
    if (l && atof(l) > 0.0)
        g_long = atof(l) / 1000.0;
    g_t0 = g_last_report = now_s();
    atexit(sched_trace_final);
    fprintf(g_out, "# sched trace: window %.1fs; times in ms unless marked\n", g_every);
    fflush(g_out);
}

const char *site_name(uint32_t site) {
    switch (site) {
    case SCHED_SITE_NONE:
        return "-";
    case SCHED_SITE_PREEMPT:
        return "(preempted in translated code)";
    case SCHED_SITE_USER32:
        return "(message pump)";
    default:
        break;
    }
    const char *d = imports_describe(site);
    return d ? d : "?";
}

const std::string &name_of(size_t idx) {
    ThreadStats &t = st(idx);
    if (!t.named) {
        t.named = true;
        const char *n = g_namer && !t.is_main ? g_namer(t.start, t.param) : nullptr;
        char buf[96];
        if (t.is_main)
            snprintf(buf, sizeof buf, "main");
        else if (n && *n)
            snprintf(buf, sizeof buf, "%s", n);
        else
            snprintf(buf, sizeof buf, "start %08x", t.start);
        t.name = buf;
    }
    return t.name;
}

void print_threads(FILE *out, double span, bool lifetime) {
    fprintf(out, "#  thr  id  name                     hold%%   holds  avg   p99   max  | ready "
                 "n   avg   p99   max  | blk%%  | longest without a point, between\n");
    for (size_t i = 0; i < stats().size(); ++i) {
        ThreadStats &t = stats()[i];
        if (!t.known)
            continue;
        const Window &w = lifetime ? t.all : t.win;
        if (!w.hold.n && !w.ready.n && w.blocked <= 0.0)
            continue;
        fprintf(out,
                "  %3zu %4u  %-24.24s %5.1f %7llu %5.2f %5.1f %5.1f | %6llu %5.2f %5.1f %6.1f | "
                "%5.1f | %.1f ms, %s -> %s\n",
                i, t.id, name_of(i).c_str(), span > 0 ? 100.0 * w.hold.total / span : 0.0,
                (unsigned long long)w.hold.n, w.hold.n ? 1e3 * w.hold.total / w.hold.n : 0.0,
                1e3 * w.hold.quantile(0.99), 1e3 * w.hold.max, (unsigned long long)w.ready.n,
                w.ready.n ? 1e3 * w.ready.total / w.ready.n : 0.0, 1e3 * w.ready.quantile(0.99),
                1e3 * w.ready.max, span > 0 ? 100.0 * w.blocked / span : 0.0, 1e3 * w.gap.seconds,
                site_name(w.gap.from), site_name(w.gap.to));
        if (!w.left.empty() || t.priority_sets) {
            fprintf(out, "         priority %d (set %u times); gave up:", t.priority,
                    t.priority_sets);
            for (auto &kv : w.left)
                fprintf(out, " %s %llu", kv.first.c_str(), (unsigned long long)kv.second);
            if (w.preempted)
                fprintf(out, "; %llu decisions at points in translated code",
                        (unsigned long long)w.preempted);
            fprintf(out, "\n");
        }
    }
}

void print_input(FILE *out, const Hist &h) {
    if (!h.n)
        return;
    fprintf(out,
            "# input: %llu presses read; host event -> guest read avg %.1f p50 %.1f p99 %.1f "
            "max %.1f ms;",
            (unsigned long long)h.n, 1e3 * h.total / h.n, 1e3 * h.quantile(0.5),
            1e3 * h.quantile(0.99), 1e3 * h.max);
    for (auto &kv : g_input_how)
        fprintf(out, " %s %llu", kv.first.c_str(), (unsigned long long)kv.second);
    fprintf(out, "\n");
}

void report(double now, bool final) {
    open_out();
    FILE *out = g_out;
    const double span = now - g_last_report;
    Hist handoff;
    for (ThreadStats &t : stats())
        handoff.merge(t.win.handoff);
    double load[1] = {0.0};
#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
    if (getloadavg(load, 1) < 1)
        load[0] = 0.0;
#endif
    fprintf(out,
            "[sched %.1fs] window %.1fs: %llu handoffs (%.0f/s), cost avg %.1f p99 %.1f max "
            "%.1f us; nothing runnable %.1f%%; host load %.1f\n",
            now - g_t0, span, (unsigned long long)g_assign_win,
            span > 0 ? g_assign_win / span : 0.0, handoff.n ? 1e6 * handoff.total / handoff.n : 0.0,
            1e6 * handoff.quantile(0.99), 1e6 * handoff.max,
            span > 0 ? 100.0 * g_idle_win / span : 0.0, load[0]);
    print_threads(out, span, false);
    {
        std::lock_guard<std::mutex> held(g_input_m);
        print_input(out, g_input_win);
        g_input_win = Hist();
    }
    if (g_reporter)
        g_reporter(out, now);
    for (ThreadStats &t : stats()) {
        // A thread inside a hold or a wait carries it into the next window as
        // it is; only the closed intervals belong to this one.
        t.win = Window();
    }
    g_idle_win = 0.0;
    g_assign_win = 0;
    g_last_report = now;
    if (final) {
        const double life = now - g_t0;
        Hist all;
        for (ThreadStats &t : stats())
            all.merge(t.all.handoff);
        fprintf(out,
                "[sched total %.1fs] %llu handoffs (%.0f/s), cost avg %.1f p99 %.1f max %.1f "
                "us; nothing runnable %.1f%%\n",
                life, (unsigned long long)g_assign_all, life > 0 ? g_assign_all / life : 0.0,
                all.n ? 1e6 * all.total / all.n : 0.0, 1e6 * all.quantile(0.99), 1e6 * all.max,
                life > 0 ? 100.0 * g_idle_all / life : 0.0);
        print_threads(out, life, true);
        std::lock_guard<std::mutex> held(g_input_m);
        print_input(out, g_input_all);
        if (g_input_dropped)
            fprintf(out, "# input: %llu presses never read\n", (unsigned long long)g_input_dropped);
    }
    fflush(out);
}

size_t g_holder = (size_t)-1; // the thread in a hold, as this trace saw it

void end_hold(size_t idx, double now, const char *why, bool runnable) {
    ThreadStats &t = st(idx);
    if (t.hold_since > 0.0) {
        const double held = now - t.hold_since;
        t.win.hold.add(held);
        t.all.hold.add(held);
        g_ring[g_ring_n++ % RING] = HoldRec{idx, t.hold_since, now, why};
        if (g_long > 0.0 && held >= g_long) {
            fprintf(g_out,
                    "[sched %.3fs] long hold: %s %.1f ms, given up by %s; %llu points, the "
                    "longest stretch without one %.1f ms (%s -> %s)\n",
                    now - g_t0, name_of(idx).c_str(), 1e3 * held, why,
                    (unsigned long long)t.hold_points, 1e3 * t.hold_gap.seconds,
                    site_name(t.hold_gap.from), site_name(t.hold_gap.to));
            fflush(g_out);
        }
        t.hold_since = 0.0;
    }
    ++t.win.left[why];
    ++t.all.left[why];
    if (runnable)
        t.ready_since = now;
    else
        t.blocked_since = now;
    if (g_holder == idx)
        g_holder = (size_t)-1;
}

} // namespace

void sched_set_thread_namer(const char *(*namer)(uint32_t, uint32_t)) {
    g_namer = namer;
}
void sched_set_trace_reporter(void (*reporter)(FILE *, double)) {
    g_reporter = reporter;
}

void sched_trace_thread(size_t idx, uint32_t id, uint32_t start, uint32_t param, bool is_main) {
    if (!g_sched_trace)
        return;
    open_out();
    ThreadStats &t = st(idx);
    if (t.known)
        return;
    t.known = true;
    t.id = id;
    t.start = start;
    t.param = param;
    t.is_main = is_main;
    // Named now, while the creator's parameter block is certainly what it
    // was: a thread that has ended may have had it freed and reused.
    name_of(idx);
    if (!is_main)
        t.ready_since = now_s(); // runnable from its creation
}

void sched_trace_resume(size_t idx, double now) {
    if (g_holder == idx)
        return; // the hold never ended: a wait satisfied at once
    if (g_holder != (size_t)-1)
        end_hold(g_holder, now, "displaced", true); // not seen to end; never expected
    ThreadStats &t = st(idx);
    if (t.blocked_since > 0.0) {
        // Released and resumed in one step, with no turn spent waiting.
        t.win.blocked += now - t.blocked_since;
        t.all.blocked += now - t.blocked_since;
        t.blocked_since = 0.0;
    }
    if (t.ready_since > 0.0) {
        const double waited = now - t.ready_since;
        t.win.ready.add(waited);
        t.all.ready.add(waited);
        if (g_long > 0.0 && waited >= g_long) {
            fprintf(g_out, "[sched %.3fs] long wait: %s ready for %.1f ms; meanwhile:", now - g_t0,
                    name_of(idx).c_str(), 1e3 * waited);
            const uint64_t n = g_ring_n < RING ? g_ring_n : RING;
            for (uint64_t k = g_ring_n - n; k < g_ring_n; ++k) {
                const HoldRec &h = g_ring[k % RING];
                if (h.end <= t.ready_since)
                    continue;
                const double from = h.start > t.ready_since ? h.start : t.ready_since;
                if (h.end - from >= 0.001)
                    fprintf(g_out, " %s %.1f (%s)", name_of(h.idx).c_str(), 1e3 * (h.end - from),
                            h.why);
            }
            fprintf(g_out, "\n");
            fflush(g_out);
        }
        t.ready_since = 0.0;
    }
    if (t.assigned_at > 0.0) {
        t.win.handoff.add(now - t.assigned_at);
        t.all.handoff.add(now - t.assigned_at);
        t.assigned_at = 0.0;
    }
    t.hold_since = now;
    t.last_point = now;
    t.last_site = SCHED_SITE_NONE;
    t.hold_gap = Gap();
    t.hold_points = 0;
    g_holder = idx;
}

void sched_trace_assign(size_t to, double now, const char *why, bool from_runnable) {
    ++g_assign_win;
    ++g_assign_all;
    if (g_holder != (size_t)-1 && g_holder != to)
        end_hold(g_holder, now, why, from_runnable);
    st(to).assigned_at = now;
}

void sched_trace_ready(size_t idx, double at) {
    ThreadStats &t = st(idx);
    if (t.blocked_since > 0.0) {
        if (at < t.blocked_since)
            at = t.blocked_since;
        t.win.blocked += at - t.blocked_since;
        t.all.blocked += at - t.blocked_since;
        t.blocked_since = 0.0;
    }
    if (t.ready_since <= 0.0 && t.hold_since <= 0.0)
        t.ready_since = at;
}

void sched_trace_idle_begin(size_t idx, double now) {
    if (g_holder == idx)
        end_hold(idx, now, "idle", false);
}

void sched_trace_idle(double seconds) {
    if (seconds > 0.0) {
        g_idle_win += seconds;
        g_idle_all += seconds;
    }
}

void sched_trace_finished(size_t idx, double now) {
    ThreadStats &t = st(idx);
    if (g_holder == idx)
        end_hold(idx, now, "ended", false);
    t.hold_since = t.ready_since = t.blocked_since = 0.0;
    t.finished = true;
}

void sched_trace_priority(size_t idx, int32_t priority) {
    ThreadStats &t = st(idx);
    t.priority = priority;
    ++t.priority_sets;
}

void sched_trace_checkpoint(size_t idx, double now, uint32_t site) {
    ThreadStats &t = st(idx);
    if (t.hold_since <= 0.0)
        return; // not a hold this trace saw begin (the boot, before any thread)
    double gap = now - t.last_point;
    if (gap > t.win.gap.seconds)
        t.win.gap = Gap{gap, t.last_site, site};
    if (gap > t.all.gap.seconds)
        t.all.gap = Gap{gap, t.last_site, site};
    if (gap > t.hold_gap.seconds)
        t.hold_gap = Gap{gap, t.last_site, site};
    ++t.hold_points;
    if (site == SCHED_SITE_PREEMPT) {
        ++t.win.preempted;
        ++t.all.preempted;
    }
    t.last_point = now;
    t.last_site = site;
}

bool sched_trace_due(double now) {
    return g_sched_trace && g_out && now - g_last_report >= g_every;
}

void sched_trace_report(double now) {
    if (g_sched_trace)
        report(now, false);
}

void sched_trace_final(void) {
    if (!g_sched_trace)
        return;
    report(now_s(), true);
}

void sched_trace_input_event(uint64_t at_ns) {
    if (!g_sched_trace)
        return;
    std::lock_guard<std::mutex> held(g_input_m);
    if (g_input_pending.size() >= 64) {
        g_input_pending.erase(g_input_pending.begin());
        ++g_input_dropped;
    }
    g_input_pending.push_back(at_ns);
}

void sched_trace_input_consumed(const char *how) {
    if (!g_sched_trace)
        return;
    uint64_t now = os_monotonic_ns();
    std::lock_guard<std::mutex> held(g_input_m);
    if (g_input_pending.empty())
        return;
    // Every press the host has seen up to now is in what the guest just read:
    // the device and the queue both deliver in order.
    for (uint64_t at : g_input_pending) {
        double s = now > at ? (double)(now - at) * 1e-9 : 0.0;
        g_input_win.add(s);
        g_input_all.add(s);
    }
    g_input_pending.clear();
    ++g_input_how[how];
}
