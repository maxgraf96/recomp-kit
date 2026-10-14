// msvcrt_heap.cpp - malloc, free, operator new/delete and friends.
//
// Small blocks (up to 2 KB) come from size-class slabs: 64 KB chunks carved
// from the guest heap (heap_alloc), one size class per chunk, with a LIFO free
// list per class. malloc and free are then a vector pop and push, which is
// what a program that news and deletes tens of thousands of objects a second
// needs, where the general first-fit heap would walk its free list. Larger
// blocks go to heap_alloc directly. Chunks are never returned.
//
// The free-list state lives on the host, outside guest memory: a guest that
// writes past the end of a block corrupts its neighbour's contents, as it
// would on Windows, but not the allocator.
//
// free() of something that is not the start of a live block - an interior
// pointer, a block freed twice, a stack address - is logged and ignored. On
// Windows it corrupts the heap and the process dies later somewhere
// unrelated; a program that does it on a path its users reach has survived
// only where that path was rare. Ignoring it leaks the block, which is the
// harmless half of the outcome.
//
// Addresses are deterministic: the same sequence of calls returns the same
// addresses on every run.
#include "msvcrt_internal.h"
#include "win32.h"
#include <algorithm>
#include <map>
#include <vector>

namespace crt {
namespace {

constexpr uint32_t CHUNK_SIZE = 0x10000; // 64 KB, and 64 KB aligned
constexpr uint32_t SLAB_MAX = 2048;
constexpr uint32_t CLASS_SIZES[] = {16,  32,  48,  64,   80,   96,   112,  128,
                                    160, 192, 224, 256,  320,  384,  448,  512,
                                    640, 768, 896, 1024, 1280, 1536, 1792, 2048};
constexpr int NCLASS = int(sizeof CLASS_SIZES / sizeof CLASS_SIZES[0]);

struct Chunk {
    uint32_t base = 0;
    uint32_t slot = 0;
    uint32_t nslots = 0;
    int cls = 0;
    // Requested size + 1 per slot; 0 marks a free slot.
    std::vector<uint32_t> req;
};

struct Heap {
    // Chunk for each 64 KB of the 32-bit address space (addr >> 16).
    Chunk *chunk_of[0x10000] = {};
    std::vector<Chunk *> chunks;
    std::vector<uint32_t> free_slots[NCLASS];
    // Blocks above SLAB_MAX: start -> requested size.
    std::map<uint32_t, uint32_t> large;
    uint8_t class_of[SLAB_MAX / 16 + 1] = {};
    uint64_t live = 0;
    uint64_t bad_frees = 0;
    uint32_t epoch = 1;
    uint32_t new_handler = 0; // _set_new_handler: int (__cdecl *)(size_t)

    Heap() {
        int cls = 0;
        for (uint32_t units = 0; units <= SLAB_MAX / 16; ++units) {
            while (CLASS_SIZES[cls] < units * 16)
                ++cls;
            class_of[units] = (uint8_t)cls;
        }
    }
    void reset() {
        for (Chunk *c : chunks)
            delete c;
        chunks.clear();
        std::fill(std::begin(chunk_of), std::end(chunk_of), nullptr);
        for (auto &v : free_slots)
            v.clear();
        large.clear();
        live = 0;
        bad_frees = 0;
        new_handler = 0;
        ++epoch;
    }
};

Heap &heap() {
    static auto *h = new Heap();
    return *h;
}

void on_mem_reset() {
    heap().reset();
    stdio_reset();
    locale_reset();
}

struct HookInstaller {
    HookInstaller() {
        mem_add_reset_hook(on_mem_reset);
    }
} g_install_hook;

bool new_chunk(Heap &h, int cls) {
    uint32_t base = heap_alloc(CHUNK_SIZE, false, CHUNK_SIZE);
    if (!base)
        return false;
    auto *ch = new Chunk();
    ch->base = base;
    ch->slot = CLASS_SIZES[cls];
    ch->nslots = CHUNK_SIZE / ch->slot;
    ch->cls = cls;
    ch->req.assign(ch->nslots, 0);
    h.chunks.push_back(ch);
    h.chunk_of[base >> 16] = ch;
    // Pushed highest first, so the lowest address is handed out first.
    auto &fl = h.free_slots[cls];
    for (uint32_t i = ch->nslots; i-- > 0;)
        fl.push_back(base + i * ch->slot);
    return true;
}

// The slab chunk holding `addr`, and its slot index, if `addr` is exactly the
// start of a slot. A chunk is found for an interior address too, with
// `exact` false.
Chunk *slab_find(Heap &h, uint32_t addr, uint32_t *slot, bool *exact) {
    Chunk *ch = h.chunk_of[addr >> 16];
    if (!ch)
        return nullptr;
    uint32_t off = addr - ch->base;
    *slot = off / ch->slot;
    *exact = (off % ch->slot) == 0 && *slot < ch->nslots;
    return ch;
}

void report_bad_free(Heap &h, uint32_t addr, const char *who, const char *why,
                     const X86 *c = nullptr) {
    ++h.bad_frees;
    // Every instance up to 16, then one in a thousand: a loop that frees the
    // same bad pointer must not flood the log.
    if (h.bad_frees <= 16 || h.bad_frees % 1000 == 0) {
        if (!c)
            c = guest_current_context();
        LOGW("msvcrt: %s(%08x) ignored: %s (return address %08x, %llu so far)", who, addr, why,
             c ? rd32(c->r[R_ESP]) : 0u, (unsigned long long)h.bad_frees);
    }
}

} // namespace

uint32_t heap_epoch() {
    return heap().epoch;
}

uint32_t mem_alloc(uint32_t size) {
    Heap &h = heap();
    // msvcrt allocates a byte for malloc(0), so the block is unique and
    // _msize reports 1.
    uint32_t want = size ? size : 1;
    if (want <= SLAB_MAX) {
        int cls = h.class_of[(want + 15) >> 4];
        auto &fl = h.free_slots[cls];
        if (fl.empty() && !new_chunk(h, cls))
            return 0;
        uint32_t a = fl.back();
        fl.pop_back();
        Chunk *ch = h.chunk_of[a >> 16];
        ch->req[(a - ch->base) / ch->slot] = want + 1;
        ++h.live;
        return a;
    }
    if (want > 0xffffffe0u) // _HEAP_MAXREQ
        return 0;
    uint32_t a = heap_alloc(want, false);
    if (!a)
        return 0;
    h.large[a] = want;
    ++h.live;
    return a;
}

uint32_t mem_calloc(uint32_t count, uint32_t size) {
    uint64_t total = (uint64_t)count * size;
    if (total > 0xffffffe0u) {
        set_errno(E_NOMEM);
        return 0;
    }
    uint32_t a = mem_alloc((uint32_t)total);
    if (a)
        memset(g_mem + a, 0, total ? (size_t)total : 1);
    return a;
}

uint32_t mem_size(uint32_t addr) {
    Heap &h = heap();
    uint32_t slot;
    bool exact;
    if (Chunk *ch = slab_find(h, addr, &slot, &exact))
        return exact && ch->req[slot] ? ch->req[slot] - 1 : 0xffffffffu;
    auto it = h.large.find(addr);
    if (it != h.large.end())
        return it->second;
    return heap_size(addr); // a kernel32 heap block, as HeapSize would report
}

void mem_free(uint32_t addr, const char *who, const X86 *c) {
    if (!addr)
        return;
    Heap &h = heap();
    uint32_t slot;
    bool exact;
    if (Chunk *ch = slab_find(h, addr, &slot, &exact)) {
        if (!exact) {
            report_bad_free(h, addr, who, "an interior pointer", c);
            return;
        }
        if (!ch->req[slot]) {
            report_bad_free(h, addr, who, "already free", c);
            return;
        }
        ch->req[slot] = 0;
        h.free_slots[ch->cls].push_back(addr);
        --h.live;
        return;
    }
    auto it = h.large.find(addr);
    if (it != h.large.end()) {
        h.large.erase(it);
        heap_free(addr);
        --h.live;
        return;
    }
    if (heap_owns(addr)) {
        // A block from HeapAlloc on the process heap, which msvcrt's free
        // accepts because its heap is the process heap.
        LOGV("msvcrt: %s(%08x) frees a kernel32 heap block", who, addr);
        heap_free(addr);
        return;
    }
    auto above = h.large.upper_bound(addr);
    if (above != h.large.begin()) {
        auto prev = std::prev(above);
        if (addr - prev->first < prev->second) {
            report_bad_free(h, addr, who, "an interior pointer", c);
            return;
        }
    }
    report_bad_free(h, addr, who, "not a heap block", c);
}

uint32_t mem_realloc(uint32_t addr, uint32_t size) {
    if (!addr)
        return mem_alloc(size);
    if (!size) {
        mem_free(addr, "realloc");
        return 0;
    }
    Heap &h = heap();
    uint32_t slot;
    bool exact;
    uint32_t old_size;
    if (Chunk *ch = slab_find(h, addr, &slot, &exact)) {
        if (!exact || !ch->req[slot]) {
            report_bad_free(h, addr, "realloc", exact ? "already free" : "an interior pointer");
            return 0;
        }
        if (size <= ch->slot) {
            ch->req[slot] = size + 1;
            return addr;
        }
        old_size = ch->req[slot] - 1;
    } else {
        auto it = h.large.find(addr);
        if (it == h.large.end()) {
            if (heap_owns(addr))
                return heap_realloc(addr, size);
            report_bad_free(h, addr, "realloc", "not a heap block");
            return 0;
        }
        if (size > SLAB_MAX) {
            uint32_t moved = heap_realloc(addr, size);
            if (!moved)
                return 0;
            h.large.erase(it);
            h.large[moved] = size;
            return moved;
        }
        old_size = it->second;
    }
    uint32_t fresh = mem_alloc(size);
    if (!fresh)
        return 0; // the old block is untouched, as the C standard requires
    memmove(g_mem + fresh, g_mem + addr, std::min(old_size, size));
    mem_free(addr, "realloc");
    return fresh;
}

uint32_t mem_strdup(const std::string &s) {
    uint32_t a = mem_alloc((uint32_t)s.size() + 1);
    if (a)
        memcpy(g_mem + a, s.c_str(), s.size() + 1);
    return a;
}

HeapCounters heap_counters() {
    Heap &h = heap();
    return HeapCounters{h.live, h.bad_frees, (uint64_t)h.chunks.size()};
}

namespace {

void crt_malloc(X86 *c) {
    uint32_t a = mem_alloc(arg(c, 0));
    if (!a)
        set_errno(E_NOMEM);
    set_eax(c, a);
}
void crt_calloc(X86 *c) {
    set_eax(c, mem_calloc(arg(c, 0), arg(c, 1)));
}
void crt_realloc(X86 *c) {
    set_eax(c, mem_realloc(arg(c, 0), arg(c, 1)));
}
void crt_free(X86 *c) {
    mem_free(arg(c, 0), "free", c);
}
void crt_msize(X86 *c) {
    set_eax(c, mem_size(arg(c, 0)));
}
// _callnewh(size): 1 when a new handler exists and asked for a retry.
uint32_t call_new_handler(X86 *c, uint32_t size) {
    uint32_t handler = heap().new_handler;
    if (!handler)
        return 0;
    return guest_call(c, handler, size) ? 1 : 0;
}
void crt_callnewh(X86 *c) {
    set_eax(c, call_new_handler(c, arg(c, 0)));
}
// operator new(size_t): msvcrt's is the non-throwing pre-standard form, which
// retries through the new handler and otherwise returns NULL.
void crt_operator_new(X86 *c) {
    uint32_t size = arg(c, 0);
    for (;;) {
        uint32_t a = mem_alloc(size);
        if (a || !call_new_handler(c, size)) {
            set_eax(c, a);
            return;
        }
    }
}
void crt_operator_delete(X86 *c) {
    mem_free(arg(c, 0), "operator delete", c);
}
// _set_new_handler(handler) returns the previous handler.
void crt_set_new_handler(X86 *c) {
    uint32_t old = heap().new_handler;
    heap().new_handler = arg(c, 0);
    set_eax(c, old);
}

const ImportShim shims[] = {
    {"msvcrt.dll", "malloc", ARGC_CDECL, crt_malloc},
    {"msvcrt.dll", "calloc", ARGC_CDECL, crt_calloc},
    {"msvcrt.dll", "realloc", ARGC_CDECL, crt_realloc},
    {"msvcrt.dll", "free", ARGC_CDECL, crt_free},
    {"msvcrt.dll", "_msize", ARGC_CDECL, crt_msize},
    {"msvcrt.dll", "_callnewh", ARGC_CDECL, crt_callnewh},
    {"msvcrt.dll", "??2@YAPAXI@Z", ARGC_CDECL, crt_operator_new},
    {"msvcrt.dll", "??3@YAXPAX@Z", ARGC_CDECL, crt_operator_delete},
    {"msvcrt.dll", "?_set_new_handler@@YAP6AHI@ZP6AHI@Z@Z", ARGC_CDECL, crt_set_new_handler},
};

} // namespace

void register_heap() {
    imports_register(shims, sizeof shims / sizeof shims[0]);
}

} // namespace crt
