#include "seh.h"
#define RECOMP_GUEST_MEMORY_OWNER 1 /* defines and maps g_mem */
#include "memory.h"
#include "win32.h"
#include "loader.h"

#include "../platform/os.h"
#if defined(__APPLE__) || (defined(__linux__) && !defined(__ANDROID__))
#include <execinfo.h>
#endif
#include <algorithm>
#include <map>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <set>
#include <vector>
#include <string>

uint8_t *g_mem = nullptr;

// ---------------------------------------------------------------------------
// The guest-memory watchpoint declared in x86.h. A stray write is only ever
// seen through the damage it does, somewhere else, later; this reports the
// write itself. The host backtrace names the generated function, which is the
// guest routine doing the writing - the one thing the corrupted value cannot
// say. Armed from RECOMP_WATCH=<hex address>[:<length>], default length 4.
// ---------------------------------------------------------------------------
uint32_t recomp_frame_watch = 0;

uint32_t g_watch_base = 0;
uint32_t g_watch_len = 0;
RecompDirty g_dirty[RECOMP_DIRTY_SLOTS];
uint32_t g_dirty_count = 0;

namespace {
struct WatchArm {
    WatchArm() {
        recomp_frame_watch = recomp_env("WATCH_FRAME") ? 1u : 0u;
        const char *s = recomp_env("WATCH");
        if (!s || !*s)
            return;
        char *end = nullptr;
        const unsigned long long base = strtoull(s, &end, 16);
        unsigned long long len = 4;
        if (end && *end == ':') {
            const unsigned long long given = strtoull(end + 1, nullptr, 16);
            if (given)
                len = given;
        }
        if (base >= GUEST_SIZE)
            return;
        if (len == 0 || len > GUEST_SIZE - base)
            len = GUEST_SIZE - base;
        g_watch_base = (uint32_t)base;
        g_watch_len = (uint32_t)len;
    }
};
WatchArm g_watch_arm;

// The host call stack names the generated function - fn_00xxxxxx - and that
// name IS the guest address of the routine doing the writing, which is the
// whole point. One line per write: a watched address in a reused stack slot is
// written thousands of times, and a report long enough to read is a report too
// slow to reach the moment worth reading.
const char *watch_writer() {
#if defined(__APPLE__) || (defined(__linux__) && !defined(__ANDROID__))
    static char out[128];
    void *frames[32];
    const int n = backtrace(frames, 32);
    char **names = backtrace_symbols(frames, n);
    if (!names)
        return "?";
    out[0] = 0;
    for (int i = 1; i < n; ++i) {
        const char *fn = strstr(names[i], "fn_00");
        if (!fn)
            continue;
        size_t k = 0;
        while (k + 1 < sizeof out && fn[k] && fn[k] != ' ')
            ++k;
        snprintf(out, sizeof out, "%.*s", (int)k, fn);
        break;
    }
    free(names);
    return out[0] ? out : "?";
#else
    return "(no backtrace)";
#endif
}
} // namespace

extern "C" void recomp_frame_changed(X86 *c, uint32_t target, uint32_t before, uint32_t after) {
    // Rate-limited: a routine that does this does it on every call, and the
    // first few name it as well as thousands would. EIP is where the callee
    // left off: a routine that escapes through an indirect jump its own body
    // does not cover returns from there without running its epilogue, and that
    // address is the whole diagnosis.
    static uint32_t said = 0;
    if (said++ < 64)
        LOGW("frame: %08x returned with EBP %08x, was %08x (left at eip=%08x)", target, after,
             before, c ? c->eip : 0u);
}

extern "C" void recomp_watch_hit(uint32_t addr, uint32_t n, uint64_t value) {
    // Report every write, not just the first: what matters is which of them
    // was the last one before the damage was read back, and a legitimate
    // writer and a stray one look identical one at a time.
    static uint32_t hits = 0;
    LOGW("watch: %08x %u = %llx by %s (hit %u)", addr, n, (unsigned long long)value, watch_writer(),
         ++hits);
}

// ---------------------------------------------------------------------------
// Logging helpers (declared in guest.h; kept here so every translation unit in
// the runtime gets them without a separate object file).
// ---------------------------------------------------------------------------
int log_level() {
    static int lvl = -1;
    if (lvl < 0) {
        const char *e = recomp_env("LOG");
        lvl = e ? atoi(e) : 1;
    }
    return lvl;
}

void log_msg(int level, const char *fmt, ...) {
    if (log_level() < level)
        return;
    va_list ap;
    va_start(ap, fmt);
    fputs("[recomp] ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

// A guest that generates code writes a new routine at a new address every
// time, and a diagnostic keyed by that address is a distinct key on every
// call. The set is therefore bounded: past the cap nothing new is remembered
// or reported, so a run that would have filled memory with keys for addresses
// nobody will ever look at stays flat instead. The cap is far above what a
// run that is behaving reports.
static const size_t kLogOnceKeys = 4096;

bool log_once(const char *key, const char *fmt, ...) {
    static std::set<std::string> seen;
    static bool capped = false;
    if (seen.size() >= kLogOnceKeys) {
        if (!capped) {
            capped = true;
            if (log_level() >= 1)
                fprintf(stderr,
                        "[recomp] log_once: %zu distinct diagnostics reported; further "
                        "first-occurrence messages are suppressed\n",
                        seen.size());
        }
        return false;
    }
    if (!seen.insert(key).second)
        return false;
    if (log_level() < 1)
        return true;
    va_list ap;
    va_start(ap, fmt);
    fputs("[recomp] ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    return true;
}

std::string gm_str(uint32_t a, size_t max_len) {
    if (a == 0)
        return std::string();
    std::string out;
    const uint32_t end = mem_readable_end(a);
    for (size_t i = 0; i < max_len && a + i < end; ++i) {
        char ch = (char)g_mem[a + i];
        if (!ch)
            break;
        out.push_back(ch);
    }
    return out;
}

uint32_t gm_put_str(uint32_t a, const char *s, uint32_t cap) {
    if (!a || !cap)
        return 0;
    uint32_t n = 0;
    while (s[n] && n + 1 < cap) {
        g_mem[a + n] = (uint8_t)s[n];
        ++n;
    }
    g_mem[a + n] = 0;
    return n;
}

std::string gm_wstr(uint32_t a, size_t max_chars) {
    std::string out;
    if (!a || !gm_valid(a, 2))
        return out;
    // Clamp before address arithmetic, including the lookahead for a low surrogate.
    max_chars = std::min(max_chars, (size_t)(mem_readable_end(a) - a) / 2);
    for (size_t i = 0; i < max_chars; ++i) {
        uint32_t u = rd16(a + 2 * (uint32_t)i);
        if (!u)
            break;
        if (u >= 0xd800 && u < 0xdc00 && i + 1 < max_chars) {
            uint32_t lo = rd16(a + 2 * (uint32_t)(i + 1));
            if (lo >= 0xdc00 && lo < 0xe000) {
                u = 0x10000 + ((u - 0xd800) << 10) + (lo - 0xdc00);
                ++i;
            }
        }
        if (u >= 0xd800 && u < 0xe000)
            u = 0xfffd;
        if (u < 0x80) {
            out += (char)u;
        } else if (u < 0x800) {
            out += (char)(0xc0 | (u >> 6));
            out += (char)(0x80 | (u & 0x3f));
        } else if (u < 0x10000) {
            out += (char)(0xe0 | (u >> 12));
            out += (char)(0x80 | ((u >> 6) & 0x3f));
            out += (char)(0x80 | (u & 0x3f));
        } else {
            out += (char)(0xf0 | (u >> 18));
            out += (char)(0x80 | ((u >> 12) & 0x3f));
            out += (char)(0x80 | ((u >> 6) & 0x3f));
            out += (char)(0x80 | (u & 0x3f));
        }
    }
    return out;
}

uint32_t gm_put_wstr(uint32_t a, const std::string &s, uint32_t cap) {
    if (!a || !cap || !gm_valid(a, 2))
        return 0;
    cap = std::min(cap, (mem_readable_end(a) - a) / 2);
    uint32_t n = 0;
    size_t i = 0;
    while (i < s.size() && n + 1 < cap) {
        unsigned char b = (unsigned char)s[i];
        if (!b)
            break;
        uint32_t u = 0xfffd, minimum = 0;
        size_t len = 1;
        if (b < 0x80) {
            u = b;
        } else if (b >= 0xc2 && b <= 0xdf) {
            u = b & 0x1f;
            minimum = 0x80;
            len = 2;
        } else if (b >= 0xe0 && b <= 0xef) {
            u = b & 0x0f;
            minimum = 0x800;
            len = 3;
        } else if (b >= 0xf0 && b <= 0xf4) {
            u = b & 0x07;
            minimum = 0x10000;
            len = 4;
        }
        bool valid = len <= s.size() - i;
        for (size_t k = 1; valid && k < len; ++k) {
            unsigned char next = (unsigned char)s[i + k];
            valid = (next & 0xc0) == 0x80;
            u = (u << 6) | (next & 0x3f);
        }
        if (!valid || u < minimum || u > 0x10ffff || (u >= 0xd800 && u < 0xe000)) {
            // Consume one invalid byte at a time; never read beyond the source.
            u = 0xfffd;
            len = 1;
        }
        i += len;
        if (u >= 0x10000) {
            if (n + 2 >= cap)
                break;
            wr16(a + 2 * n++, (uint16_t)(0xd800 + ((u - 0x10000) >> 10)));
            wr16(a + 2 * n++, (uint16_t)(0xdc00 + ((u - 0x10000) & 0x3ff)));
        } else {
            wr16(a + 2 * n++, (uint16_t)u);
        }
    }
    wr16(a + 2 * n, 0);
    return n;
}

// ---------------------------------------------------------------------------
// Arena
// ---------------------------------------------------------------------------
namespace {

// The whole 32-bit guest space plus a page of slack, so an unaligned access
// at 0xffffffff still lands in the reservation. Only a guarded arena is this
// large; the fallback is GUEST_SIZE.
const uint64_t ARENA_RESERVE = 0x100000000ull + 0x10000u;
size_t g_reserved = 0; // bytes mapped at g_mem; 0 when there is no arena
bool g_guarded = false, g_null_guard = false;

const char *arena_region(uint64_t guest) {
    if (guest >= 0x100000000ull)
        return "past the top of the 32-bit address space";
    if (guest < GUEST_NULL_LIMIT)
        return "in the null page (Windows maps nothing below 64 KB)";
    return "outside the guest arena ([game] guest_size)";
}

// What a fault the guest takes no handler for prints: the guest address, what
// is (not) there and the guest registers, instead of a host SIGSEGV with a
// host address. Raw writes only: it runs inside the fault handler as well.
void report_arena_fault(const char *what, uint64_t guest) {
    char line[320];
    int n = snprintf(line, sizeof line,
                     "\n[recomp] guest memory fault (%s): access to guest %08llx, %s\n", what,
                     (unsigned long long)guest, arena_region(guest));
    os_write_stderr_raw(line, (size_t)n);
    if (const X86 *c = guest_current_context()) {
        n = snprintf(line, sizeof line,
                     "[recomp]   guest thread %u: EIP=%08x ESP=%08x EBP=%08x EAX=%08x EBX=%08x "
                     "ECX=%08x EDX=%08x ESI=%08x EDI=%08x (EIP is the last call or branch the "
                     "translation recorded, not the faulting instruction)\n",
                     guest_current_thread_id(), c->eip, c->r[R_ESP], c->r[R_EBP], c->r[R_EAX],
                     c->r[R_EBX], c->r[R_ECX], c->r[R_EDX], c->r[R_ESI], c->r[R_EDI]);
        os_write_stderr_raw(line, (size_t)n);
    }
    n = snprintf(line, sizeof line, "[recomp]   in translated routine %s\n", watch_writer());
    os_write_stderr_raw(line, (size_t)n);
}

// Out of the fault handler, on the faulting thread's own stack (see
// arena_fault): the access violation Windows would raise, for the guest's own
// handlers - an __except, or catch(...) as MSVC 6 compiled it. One that takes
// it resumes elsewhere and never comes back; with none, the run ends with the
// report it always had. `packed` is the guest address, and bit 32 a write.
void arena_fault_raise(uintptr_t packed) {
    const uint32_t guest = (uint32_t)packed;
    X86 *c = guest_current_context();
    if (c && c->r[R_ESP] >= GUEST_NULL_LIMIT + 16 && c->r[R_ESP] - 16 + 8 <= GUEST_SIZE) {
        // EXCEPTION_ACCESS_VIOLATION's parameters below the guest stack
        // pointer, where the dispatcher copies them from before any handler.
        const uint32_t info = c->r[R_ESP] - 16;
        wr32(info, (uint32_t)(((uint64_t)packed >> 32) & 1));
        wr32(info + 4, guest);
        recomp_seh_raise_at(c, 0xc0000005u, 0, 2, info, c->eip);
    }
    report_arena_fault("SIGSEGV, no guest handler took the access violation", guest);
    os_exit_immediately(5);
}

// An access fault inside the reservation is the guest's - generated code or a
// shim following a guest pointer - and is reported as such. A guest thread
// with exception handlers first gets the access violation Windows would give
// it: the thread is moved on to arena_fault_raise, outside this handler. Not
// strictly signal safe (formatting, the backtrace); the process ends here
// otherwise, and that is worth a report a debugger is not needed to read.
int arena_fault(const char *what, uintptr_t address) {
    const uintptr_t base = (uintptr_t)g_mem;
    if (!g_mem || !g_guarded || address < base || address - base >= g_reserved)
        return 0;
    const uint64_t guest = address - base;
    // Not for an address past 4 GB (no guest pointer names it), and not when
    // the faults are recursing: a handler that faults in turn gets a fresh
    // dispatch each time, so a bounded number of them is the loop's limit.
    if (guest < 0x100000000ull && guest_current_context() && recomp_seh_dispatch_depth() < 16) {
        const uint64_t write = os_fault_was_write() == 1 ? 1 : 0;
        if (os_fault_continue_in(arena_fault_raise, (uintptr_t)(guest | (write << 32))))
            return 1;
    }
    report_arena_fault(what, guest);
    os_exit_immediately(5);
    return 1;
}

inline uint32_t page_down(uint32_t a) {
    return a & ~0xfffu;
}

} // namespace

bool mem_guarded() {
    return g_mem && g_guarded;
}
bool mem_null_guard() {
    return g_mem && g_null_guard;
}

namespace {
// Module images the heap must not grow over, in address order.
std::vector<std::pair<uint32_t, uint32_t>> &reserved_ranges() {
    static auto *v = new std::vector<std::pair<uint32_t, uint32_t>>();
    return *v;
}
void add_span(std::vector<std::pair<uint32_t, uint32_t>> &v, uint32_t lo, uint32_t hi) {
    v.push_back({lo, hi});
    std::sort(v.begin(), v.end());
    std::vector<std::pair<uint32_t, uint32_t>> merged;
    for (const auto &r : v) {
        if (!merged.empty() && r.first <= merged.back().second)
            merged.back().second = std::max(merged.back().second, r.second);
        else
            merged.push_back(r);
    }
    v.swap(merged);
}
} // namespace

// The whole arena below GUEST_SIZE is open (the host backs a page only once
// it is touched, so a large guest_size costs nothing until used), which keeps
// the invariant every shim relies on: gm_valid(a, n) means the bytes can be
// read. The guards are the null page and everything from GUEST_SIZE to 4 GB.
bool mem_commit(uint32_t addr, uint32_t size) {
    return g_mem && size && addr < GUEST_SIZE && size <= GUEST_SIZE - addr;
}

bool mem_committed(uint32_t addr, uint32_t size) {
    if (!g_mem || addr >= GUEST_SIZE || size > GUEST_SIZE - addr)
        return mem_opened(addr, size);
    return !(g_null_guard && addr < GUEST_NULL_LIMIT);
}

void mem_reserve_range(uint32_t addr, uint32_t size) {
    if (size)
        add_span(reserved_ranges(), page_down(addr),
                 (uint32_t)std::min<uint64_t>(((uint64_t)addr + size + 0xfffu) & ~0xfffull,
                                              0xfffff000u));
}

namespace {
// Spans mem_open committed above GUEST_SIZE, merged, in address order. Ends
// are at most OPEN_LIMIT, so they fit a uint32_t.
std::vector<std::pair<uint32_t, uint32_t>> &opened_ranges() {
    static auto *v = new std::vector<std::pair<uint32_t, uint32_t>>();
    return *v;
}
const uint64_t OPEN_LIMIT = 0xffff0000ull;
const uint32_t OPEN_GRAIN = 0x10000u; // a multiple of every host page size

// The opened span holding `a`, or end().
std::vector<std::pair<uint32_t, uint32_t>>::const_iterator opened_span(uint32_t a) {
    const auto &v = opened_ranges();
    auto it = std::upper_bound(v.begin(), v.end(), std::make_pair(a, 0xffffffffu));
    if (it == v.begin())
        return v.end();
    --it;
    return a < it->second ? it : v.end();
}
} // namespace

bool mem_opened(uint32_t a, uint32_t n) {
    if (opened_ranges().empty() || !g_mem)
        return false;
    // Spans are merged, so a range is covered only by the one span holding
    // its first byte - or, when it starts in the arena, by the span that
    // continues the arena at GUEST_SIZE.
    const uint64_t end = (uint64_t)a + n;
    if (a < GUEST_SIZE) {
        if (g_null_guard && a < GUEST_NULL_LIMIT)
            return false;
        auto it = opened_span(GUEST_SIZE);
        return it != opened_ranges().end() && end <= it->second;
    }
    auto it = opened_span(a);
    return it != opened_ranges().end() && end <= it->second;
}

uint32_t mem_readable_end(uint32_t a) {
    if (!g_mem)
        return a;
    const uint32_t from = a < GUEST_SIZE ? GUEST_SIZE : a;
    auto it = opened_span(from);
    if (it == opened_ranges().end())
        return from;
    return it->second;
}

bool mem_open(uint32_t addr, uint32_t size) {
    if (!g_mem || !size || (uint64_t)addr + size > OPEN_LIMIT)
        return false;
    if (g_null_guard && addr < GUEST_NULL_LIMIT)
        return false;
    if ((uint64_t)addr + size <= GUEST_SIZE)
        return mem_commit(addr, size);
    if (!g_guarded)
        return false;
    // Whole grains: committing the arena's own last grain again is harmless.
    const uint32_t lo = std::max<uint32_t>(addr, GUEST_SIZE) & ~(OPEN_GRAIN - 1);
    const uint64_t hi = ((uint64_t)addr + size + OPEN_GRAIN - 1) & ~(uint64_t)(OPEN_GRAIN - 1);
    if (os_vm_commit(g_mem + lo, (size_t)(hi - lo)) != 0)
        return false;
    add_span(opened_ranges(), std::max<uint32_t>(lo, GUEST_SIZE), (uint32_t)hi);
    return true;
}

namespace {

struct Blk {
    uint32_t size; // rounded, multiple of 16
    uint32_t req;  // bytes the guest asked for
    bool used;
};

// Every block in address order, plus an index of just the free ones so that
// first fit walks free blocks instead of the whole heap.
std::map<uint32_t, Blk> *g_blocks = nullptr;
std::set<uint32_t> *g_free = nullptr;
uint64_t g_total_allocs = 0, g_total_frees = 0;

inline uint32_t align_up(uint32_t v, uint32_t a) {
    return (v + a - 1) & ~(a - 1);
}

// Largest request the arena could ever satisfy: the base heap, or with a
// guarded arena the space above the shims it can grow into, whichever is
// larger. Anything above it would overflow the rounding in align_up
// (0xffffffff rounds to 0), so it is rejected before any arithmetic on it.
const uint32_t HEAP_ARENA_BYTES = HEAP_LIMIT - HEAP_BASE;
uint32_t heap_capacity() {
    const uint32_t grow = GUEST_SIZE > GUEST_SHIM_END ? GUEST_SIZE - GUEST_SHIM_END : 0;
    return std::max(HEAP_ARENA_BYTES, grow);
}
inline bool size_is_sane(uint32_t size) {
    return size <= heap_capacity();
}

// The heap's address ranges: [HEAP_BASE, HEAP_LIMIT) and every segment it
// grew into, merged where they touch, in address order. Blocks tile each.
std::vector<std::pair<uint32_t, uint32_t>> *g_segments = nullptr;
uint32_t g_grow_next = 0; // the next address growth may claim
const uint32_t GROW_CHUNK = 0x01000000u;

// Records a block, keeping the free index in step.
void put_block(uint32_t addr, uint32_t size, uint32_t req, bool used) {
    (*g_blocks)[addr] = Blk{size, req, used};
    if (used)
        g_free->erase(addr);
    else
        g_free->insert(addr);
}

void drop_block(std::map<uint32_t, Blk>::iterator it) {
    g_free->erase(it->first);
    g_blocks->erase(it);
}

void heap_reset() {
    if (!g_blocks)
        g_blocks = new std::map<uint32_t, Blk>();
    if (!g_free)
        g_free = new std::set<uint32_t>();
    g_blocks->clear();
    g_free->clear();
    put_block(HEAP_BASE, HEAP_LIMIT - HEAP_BASE, 0, false);
    g_total_allocs = g_total_frees = 0;
    if (!g_segments)
        g_segments = new std::vector<std::pair<uint32_t, uint32_t>>();
    g_segments->assign(1, {HEAP_BASE, HEAP_LIMIT});
    g_grow_next = GUEST_SHIM_END;
}

// Claims the next stretch of [GUEST_SHIM_END, GUEST_SIZE) that no reserved
// module range covers, commits it and frees it into the heap - merged with a
// free block that ends where it starts, so coalescing never has to know
// about segments. Keeps going until one stretch can hold `need` bytes at
// `align`; false when the space is used up. A stretch too small for this
// request still joins the heap: smaller requests can use it.
bool heap_grow(uint32_t need, uint32_t align) {
    if (GUEST_SIZE <= GUEST_SHIM_END)
        return false;
    while (g_grow_next < GUEST_SIZE) {
        uint32_t lo = (g_grow_next + 0xffffu) & ~0xffffu;
        bool moved = false;
        for (const auto &r : reserved_ranges())
            if (r.first <= lo && lo < r.second) {
                lo = (r.second + 0xffffu) & ~0xffffu;
                moved = true;
            }
        if (moved || lo >= GUEST_SIZE) {
            g_grow_next = lo;
            continue;
        }
        uint64_t want = std::max<uint64_t>(GROW_CHUNK, (uint64_t)need + align + 0xffffu);
        uint64_t hi = std::min<uint64_t>(((uint64_t)lo + want) & ~0xffffull, GUEST_SIZE);
        for (const auto &r : reserved_ranges())
            if (r.first > lo && r.first < hi)
                hi = r.first;
        g_grow_next = (uint32_t)std::min<uint64_t>(hi, 0xffffffffu);
        if (hi <= lo)
            continue;
        uint32_t start = lo, len = (uint32_t)(hi - lo);
        auto last = g_blocks->empty() ? g_blocks->end() : std::prev(g_blocks->end());
        if (last != g_blocks->end() && !last->second.used &&
            last->first + last->second.size == lo) {
            start = last->first;
            len += last->second.size;
            drop_block(last);
        }
        put_block(start, len, 0, false);
        if (!g_segments->empty() && g_segments->back().second == lo)
            g_segments->back().second = (uint32_t)hi;
        else
            g_segments->push_back({lo, (uint32_t)hi});
        LOGV("heap: grew by %08x bytes at %08x", (uint32_t)(hi - lo), lo);
        const uint32_t user = (start + align - 1) & ~(align - 1);
        if (user >= start && user - start <= len && need <= len - (user - start))
            return true;
    }
    return false;
}

// Deliberately leaked, like the import tables: registration happens from
// static initialisers in other translation units.
std::vector<void (*)()> &reset_hooks() {
    static auto *v = new std::vector<void (*)()>();
    return *v;
}

} // namespace

void mem_init() {
    recomp_seh_reset(nullptr);
    if (g_mem) {
        os_vm_release(g_mem, g_reserved);
        g_mem = nullptr;
        g_reserved = 0;
    }
    reserved_ranges().clear();
    opened_ranges().clear(); // a fresh reservation has none of them open
    // The guarded arena: reserve all 4 GB, open [0, GUEST_SIZE). Without it,
    // the plain mapping the kit always had.
    g_guarded = false;
    g_null_guard = false;
    void *p = sizeof(void *) >= 8 ? os_vm_reserve_guarded((size_t)ARENA_RESERVE) : nullptr;
    if (p && os_vm_commit(p, GUEST_SIZE) == 0) {
        g_guarded = true;
        g_reserved = (size_t)ARENA_RESERVE;
    } else {
        if (p)
            os_vm_release(p, (size_t)ARENA_RESERVE);
        static bool said = false;
        if (!said) {
            said = true;
            LOGW("guest arena: no 4 GB reservation on this host; wild guest accesses are not "
                 "trapped and the heap cannot grow past [game] heap_base..%08x",
                 HEAP_LIMIT);
        }
        p = os_vm_reserve(GUEST_SIZE);
        g_reserved = GUEST_SIZE;
    }
    if (!p) {
        fprintf(stderr, "[recomp] fatal: cannot map %u bytes of guest memory\n", GUEST_SIZE);
        abort();
    }
    g_mem = (uint8_t *)p;
    if (g_guarded) {
        static bool hooked = false;
        if (!hooked) {
            hooked = true;
            os_install_fault_address_hook(arena_fault);
        }
        const char *want = recomp_env("NULL_GUARD");
        if (want ? atoi(want) != 0 : recomp_strict())
            g_null_guard = os_vm_protect_none(g_mem, GUEST_NULL_LIMIT) == 0;
    }
    heap_reset();
    for (void (*fn)() : reset_hooks())
        fn();
}

void mem_add_reset_hook(void (*fn)()) {
    auto &hooks = reset_hooks();
    if (fn && std::find(hooks.begin(), hooks.end(), fn) == hooks.end())
        hooks.push_back(fn);
}

void mem_shutdown() {
    recomp_seh_reset(nullptr);
    if (g_mem) {
        os_vm_release(g_mem, g_reserved);
        g_mem = nullptr;
        g_reserved = 0;
    }
    g_guarded = g_null_guard = false;
    reserved_ranges().clear();
    opened_ranges().clear();
    if (g_blocks) {
        delete g_blocks;
        g_blocks = nullptr;
    }
    if (g_free) {
        delete g_free;
        g_free = nullptr;
    }
}

namespace {
uint32_t heap_fit(uint32_t size, uint32_t need, uint32_t align, bool zero);
} // namespace

uint32_t heap_alloc(uint32_t size, bool zero, uint32_t align) {
    if (!g_blocks)
        return 0;
    if (!size_is_sane(size)) {
        LOGW("heap_alloc: refusing a %u byte request, the arena is %u bytes", size,
             heap_capacity());
        // Oversized requests often originate in a corrupted guest length.
        // Use this thread's register file, including worker-thread callers.
        if (const X86 *c = guest_current_context()) {
            LOGW("heap_alloc: EIP=%08x EAX=%08x ECX=%08x EDX=%08x EBX=%08x "
                 "ESP=%08x EBP=%08x ESI=%08x EDI=%08x",
                 c->eip, c->r[R_EAX], c->r[R_ECX], c->r[R_EDX], c->r[R_EBX], c->r[R_ESP],
                 c->r[R_EBP], c->r[R_ESI], c->r[R_EDI]);
            auto chain = win32_return_chain(c->r[R_EBP], 12);
            for (size_t i = 0; i < chain.size(); ++i)
                LOGW("  frame %zu returns to %08x", i, chain[i]);
            // Frameless RTL helpers do not appear in the EBP chain. Include
            // stack words preceded by a CALL, as the exception diagnostic does.
            auto candidates = win32_stack_return_candidates(c->r[R_ESP], 0x400, loader_image_base(),
                                                            loader_image_limit(), 24);
            std::string line;
            for (uint32_t ret : candidates) {
                char word[16];
                snprintf(word, sizeof word, " %08x", ret);
                line += word;
            }
            if (!line.empty())
                LOGW("  return addresses on the stack, newest first:%s", line.c_str());
        }
        return 0;
    }
    if (align < 16)
        align = 16;
    uint32_t need = align_up(size ? size : 1, 16);

    for (int attempt = 0; attempt < 2; ++attempt) {
        if (uint32_t user = heap_fit(size, need, align, zero))
            return user;
        // Full: claim more of the arena above the shims, then try once more.
        if (attempt || !heap_grow(need, align))
            break;
    }
    LOGW("heap_alloc: out of guest heap (%u bytes requested)", size);
    return 0;
}

namespace {
// First fit of `need` rounded bytes at `align` among the free blocks; 0 when
// none can hold it.
uint32_t heap_fit(uint32_t size, uint32_t need, uint32_t align, bool zero) {
    for (uint32_t start : *g_free) {
        auto it = g_blocks->find(start);
        if (it == g_blocks->end() || it->second.used)
            continue; // index out of step
        uint32_t len = it->second.size;
        uint32_t user = align_up(start, align);
        if (user < start)
            continue; // overflow
        uint32_t pad = user - start;
        if (pad > len || need > len - pad)
            continue; // does not fit

        uint32_t tail = len - pad - need;
        drop_block(it);
        if (pad)
            put_block(start, pad, 0, false);
        put_block(user, need, size, true);
        if (tail)
            put_block(user + need, tail, 0, false);
        if (zero)
            memset(g_mem + user, 0, need);
        ++g_total_allocs;
        return user;
    }
    return 0;
}
} // namespace

bool heap_owns(uint32_t addr) {
    if (!g_blocks)
        return false;
    auto it = g_blocks->find(addr);
    return it != g_blocks->end() && it->second.used;
}

uint32_t heap_size(uint32_t addr) {
    if (!g_blocks)
        return 0xffffffffu;
    auto it = g_blocks->find(addr);
    if (it == g_blocks->end() || !it->second.used)
        return 0xffffffffu;
    return it->second.req;
}

bool heap_free(uint32_t addr) {
    if (!g_blocks || !addr)
        return false;
    auto it = g_blocks->find(addr);
    if (it == g_blocks->end() || !it->second.used) {
        LOGW("heap_free: %08x is not a live allocation", addr);
        return false;
    }
    it->second.used = false;
    it->second.req = 0;
    ++g_total_frees;

    // RECOMP_HEAP_QUARANTINE=1 retires a block instead of returning it: the
    // address is never handed out again, and no neighbour absorbs it either.
    // A guest that goes on using memory it has already freed then reads its
    // own dead object rather than whatever was allocated over the top of it,
    // which is the difference between a fault that names the culprit and one
    // that does not. Off by default, and a run with it on gives up every byte
    // it frees, so it is a diagnostic rather than a way to play.
    static const bool quarantine = recomp_env("HEAP_QUARANTINE") != nullptr;
    if (quarantine)
        return true;
    g_free->insert(addr);

    // Coalesce with the following block.
    auto next = std::next(it);
    if (next != g_blocks->end() && !next->second.used &&
        next->first == it->first + it->second.size) {
        it->second.size += next->second.size;
        drop_block(next);
    }
    // Coalesce with the preceding block.
    if (it != g_blocks->begin()) {
        auto prev = std::prev(it);
        if (!prev->second.used && prev->first + prev->second.size == it->first) {
            prev->second.size += it->second.size;
            drop_block(it);
        }
    }
    return true;
}

// The range leaves the heap as a hole: free blocks across it are cut back to
// its edges and the segments they tile are split there, so the blocks still
// tile every segment (heap_check) and coalescing, which joins only blocks
// that touch, never bridges it. Growth skips it as it skips a module range.
bool heap_exclude(uint32_t addr, uint32_t size) {
    if (!size)
        return true;
    const uint32_t lo = page_down(addr);
    const uint32_t hi =
        (uint32_t)std::min<uint64_t>(((uint64_t)addr + size + 0xfffu) & ~0xfffull, 0xfffff000u);
    if (hi <= lo)
        return false;
    if (g_blocks && g_segments) {
        // The first block that can reach lo: the last one starting at or below it.
        auto first = g_blocks->upper_bound(lo);
        if (first != g_blocks->begin())
            --first;
        for (auto it = first; it != g_blocks->end() && it->first < hi; ++it)
            if (it->second.used && it->first + it->second.size > lo)
                return false;
        for (auto it = first; it != g_blocks->end() && it->first < hi;) {
            const uint32_t b = it->first, e = b + it->second.size;
            auto next = std::next(it);
            if (e > lo) {
                drop_block(it);
                if (b < lo)
                    put_block(b, lo - b, 0, false);
                if (e > hi)
                    put_block(hi, e - hi, 0, false);
            }
            it = next;
        }
        std::vector<std::pair<uint32_t, uint32_t>> kept;
        for (const auto &s : *g_segments) {
            if (s.second <= lo || s.first >= hi) {
                kept.push_back(s);
                continue;
            }
            if (s.first < lo)
                kept.push_back({s.first, lo});
            if (s.second > hi)
                kept.push_back({hi, s.second});
        }
        g_segments->swap(kept);
    }
    mem_reserve_range(lo, hi - lo);
    return true;
}

// Resize a guest heap allocation in place when possible, otherwise copy into a new block.
// Reject arena-sized overflow requests and leave the original allocation live if growth fails.
uint32_t heap_realloc(uint32_t addr, uint32_t new_size, bool zero) {
    if (!addr)
        return heap_alloc(new_size, zero);
    if (!g_blocks)
        return 0;
    if (!size_is_sane(new_size)) {
        LOGW("heap_realloc: refusing a %u byte request, the arena is %u bytes", new_size,
             heap_capacity());
        return 0;
    }
    auto it = g_blocks->find(addr);
    if (it == g_blocks->end() || !it->second.used) {
        LOGW("heap_realloc: %08x is not a live allocation", addr);
        return 0;
    }
    uint32_t old_req = it->second.req;
    uint32_t need = align_up(new_size ? new_size : 1, 16);
    uint32_t have = it->second.size;

    if (need <= have) {
        uint32_t tail = have - need;
        if (tail >= 16) {
            it->second.size = need;
            auto next = std::next(it);
            if (next != g_blocks->end() && !next->second.used && next->first == addr + have) {
                uint32_t merged = tail + next->second.size;
                drop_block(next);
                put_block(addr + need, merged, 0, false);
            } else {
                put_block(addr + need, tail, 0, false);
            }
        }
        it->second.req = new_size;
        if (zero && new_size > old_req)
            memset(g_mem + addr + old_req, 0, new_size - old_req);
        return addr;
    }

    // Try to grow into a free neighbour.
    auto next = std::next(it);
    if (next != g_blocks->end() && !next->second.used && next->first == addr + have &&
        have + next->second.size >= need) {
        uint32_t total = have + next->second.size;
        drop_block(next);
        uint32_t tail = total - need;
        it->second.size = need;
        it->second.req = new_size;
        if (tail)
            put_block(addr + need, tail, 0, false);
        if (zero)
            memset(g_mem + addr + old_req, 0, new_size - old_req);
        return addr;
    }

    uint32_t fresh = heap_alloc(new_size, false);
    if (!fresh)
        return 0;
    uint32_t copy = old_req < new_size ? old_req : new_size;
    memmove(g_mem + fresh, g_mem + addr, copy);
    if (zero && new_size > copy)
        memset(g_mem + fresh + copy, 0, new_size - copy);
    heap_free(addr);
    return fresh;
}

uint64_t heap_span() {
    uint64_t n = 0;
    if (g_segments)
        for (const auto &r : *g_segments)
            n += r.second - r.first;
    return n;
}

HeapStats heap_stats() {
    HeapStats s{};
    s.total_allocs = g_total_allocs;
    s.total_frees = g_total_frees;
    if (!g_blocks)
        return s;
    for (auto &kv : *g_blocks) {
        if (kv.second.used) {
            ++s.used_blocks;
            s.used_bytes += kv.second.size;
            s.req_bytes += kv.second.req;
        } else {
            ++s.free_blocks;
            s.free_bytes += kv.second.size;
            if (kv.second.size > s.largest_free)
                s.largest_free = kv.second.size;
        }
    }
    return s;
}

// Check that the heap block map covers the arena without gaps, overlap or adjacent free blocks.
// Return the first invariant failure, or an empty string when the allocation structure is consistent.
std::string heap_check() {
    if (!g_blocks)
        return "heap not initialised";
    // heap_exclude can take the base arena whole before anything has grown.
    if (g_segments->empty())
        return g_blocks->empty() ? std::string() : std::string("blocks outside every segment");
    // Blocks tile each heap segment exactly, in address order.
    size_t segment = 0;
    uint32_t cursor = (*g_segments)[0].first;
    bool prev_free = false;
    char buf[160];
    for (auto &kv : *g_blocks) {
        if (cursor == (*g_segments)[segment].second && segment + 1 < g_segments->size()) {
            cursor = (*g_segments)[++segment].first;
            prev_free = false;
        }
        if (kv.first != cursor) {
            snprintf(buf, sizeof buf, "gap or overlap at %08x (expected %08x)", kv.first, cursor);
            return buf;
        }
        if (kv.second.size == 0 || (kv.second.size & 15)) {
            snprintf(buf, sizeof buf, "block %08x has bad size %u", kv.first, kv.second.size);
            return buf;
        }
        if (!kv.second.used && prev_free) {
            snprintf(buf, sizeof buf, "adjacent free blocks at %08x", kv.first);
            return buf;
        }
        prev_free = !kv.second.used;
        cursor += kv.second.size;
    }
    if (segment + 1 != g_segments->size() || cursor != g_segments->back().second) {
        snprintf(buf, sizeof buf, "heap ends at %08x, expected %08x", cursor,
                 g_segments->back().second);
        return buf;
    }
    for (uint32_t a : *g_free) {
        auto it = g_blocks->find(a);
        if (it == g_blocks->end() || it->second.used) {
            snprintf(buf, sizeof buf, "free index holds %08x, which is not a free block", a);
            return buf;
        }
    }
    size_t free_count = 0;
    for (auto &kv : *g_blocks)
        if (!kv.second.used)
            ++free_count;
    if (free_count != g_free->size()) {
        snprintf(buf, sizeof buf, "free index has %zu entries, the heap has %zu free blocks",
                 g_free->size(), free_count);
        return buf;
    }
    return std::string();
}
