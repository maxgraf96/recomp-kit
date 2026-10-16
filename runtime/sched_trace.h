// sched_trace.h - opt-in instrumentation of the guest-thread scheduler.
//
//   RECOMP_SCHED_TRACE=FILE        write the report to FILE (1: stderr)
//   RECOMP_SCHED_TRACE_EVERY=S     a window report every S seconds (default 10)
//   RECOMP_SCHED_TRACE_LONG_MS=N   every hold and wait longer than N ms, as it happens
//   RECOMP_SCHED_TRACE_TIMELINE=F  every hold as a row of F (start, end, thread,
//                                  why), on the host's monotonic clock
//
// Per guest thread: how long it held the baton each time (a hold runs from
// its return to guest code to the call that gives the baton up), how long it
// waited for the baton while it could have run, how often and why it gave it
// up, and the longest stretch it held the baton without passing a scheduling
// point - an import call, or a preemption check in translated code - with the
// imports on either side. Globally: handoffs, their cost (from the moment the
// baton is assigned to the moment its new holder runs), the time no thread
// could run, and the latency of a mouse press from the host's event to the
// guest's first read of it.
//
// Off, every entry point costs a load of g_sched_trace and a branch, at the
// caller. Nothing here changes what the scheduler decides.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

extern bool g_sched_trace;

// Called with the scheduler's mutex held, so the statistics need no lock of
// their own: every one of them changes only under that mutex or, for the
// checkpoint, on the baton holder, which is the one thread running guest code.
//
// A hold is continuous possession of the baton while running: it begins when
// a thread returns to guest code holding it (resume) and ends when the baton
// is assigned to another thread (assign) or its holder parks with nothing
// runnable (idle_begin). A blocking call that is satisfied at once is no
// break in the hold.
void sched_trace_thread(size_t idx, uint32_t id, uint32_t start, uint32_t param, bool is_main);
void sched_trace_resume(size_t idx, double now);
void sched_trace_assign(size_t to, double now, const char *why, bool from_runnable);
void sched_trace_ready(size_t idx, double at);
void sched_trace_idle_begin(size_t idx, double now);
void sched_trace_idle(double seconds);
void sched_trace_finished(size_t idx, double now);
void sched_trace_priority(size_t idx, int32_t priority);
// On the holder; the mutex need not be held. `site` is an import's trampoline
// address, or SCHED_SITE_* for a point that is not an import.
const uint32_t SCHED_SITE_NONE = 0xffffffffu;
const uint32_t SCHED_SITE_PREEMPT = 0xfffffffeu;
const uint32_t SCHED_SITE_USER32 = 0xfffffffdu;
void sched_trace_checkpoint(size_t idx, double now, uint32_t site);
// On the holder at a scheduling point: whether a window report is due, and,
// with the mutex held, the report itself.
bool sched_trace_due(double now);
void sched_trace_report(double now);
void sched_trace_final(void);

// Input latency, from any thread. `at_ns` is os_monotonic_ns() time.
void sched_trace_input_event(uint64_t at_ns);
void sched_trace_input_consumed(const char *how);

// What a game names its threads and adds to each report. The namer is asked
// once per thread, on the baton holder, at its first report; the reporter is
// called on the baton holder at every window report.
void sched_set_thread_namer(const char *(*namer)(uint32_t start, uint32_t param));
void sched_set_trace_reporter(void (*reporter)(FILE *out, double now));
