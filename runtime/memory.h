// memory.h - guest arena and the guest heap allocator.
#pragma once
#include "guest.h"

// Maps (or re-maps) the guest arena at g_mem, zero filled, and resets the heap
// free list. Safe to call repeatedly; each call discards all guest state.
// Aborts on failure.
//
// On a 64-bit host the arena is guarded: the whole 4 GB a 32-bit guest can
// name is reserved, so g_mem + any guest address stays inside the reservation.
// [0, GUEST_SIZE) - image, modules, heap, stacks, TEB, trampolines - is
// readable and writable (backed only where touched); everything from
// GUEST_SIZE up faults, and so does the null page when mem_null_guard() is
// armed. Such a fault is reported with the guest address and the guest
// registers instead of as a host crash. Where the reservation cannot be had
// (WebAssembly, a small address space) the arena is the plain GUEST_SIZE
// mapping it always was, and mem_guarded() says so.
void mem_init();
void mem_shutdown();
bool mem_guarded();
// True when [addr, addr+size) lies in the arena: a module's place must be
// inside [game] guest_size. Every such byte is already readable and writable.
bool mem_commit(uint32_t addr, uint32_t size);
// True when every byte of [addr, addr+size) can be read and written.
bool mem_committed(uint32_t addr, uint32_t size);
// Keeps heap growth out of [addr, addr+size): a module image's place, which
// must stay free whether or not the module is loaded yet. Call before guest
// code runs.
void mem_reserve_range(uint32_t addr, uint32_t size);
// The first 64 KB fault, as on Windows, instead of reading as zeros. Armed by
// mem_init under strict mode (recomp_strict()) or RECOMP_NULL_GUARD=1, and off
// with RECOMP_NULL_GUARD=0; only a guarded arena can do it.
bool mem_null_guard();
// Registers a callback mem_init runs after it has reset the arena, so a layer
// that keeps guest addresses on the host side (the C runtime's allocator and
// its streams) forgets them with the memory they named. Idempotent per fn.
void mem_add_reset_hook(void (*fn)());

// First-fit free-list allocator over HEAP_BASE..HEAP_LIMIT, which grows, once
// that is full, into [GUEST_SHIM_END, GUEST_SIZE) around the reserved module
// ranges - so a game whose [game] guest_size is larger than the 256 MB default
// gets a heap of most of the difference.
// Returns a guest address, or 0 on failure. Sizes are rounded up to a multiple
// of 16 and every block is at least 16-byte aligned; `align` may request more
// (VirtualAlloc uses 4096). `zero` clears the usable bytes.
uint32_t heap_alloc(uint32_t size, bool zero = false, uint32_t align = 16);
// Grows or shrinks in place when possible, otherwise allocates and copies.
// Returns the new address or 0 (in which case the old block is untouched).
uint32_t heap_realloc(uint32_t addr, uint32_t new_size, bool zero = false);
bool heap_free(uint32_t addr);
// Requested (not rounded) size of a live block, or 0xffffffff if `addr` is not
// the start of a live allocation.
uint32_t heap_size(uint32_t addr);
bool heap_owns(uint32_t addr);

struct HeapStats {
    uint32_t used_blocks;
    uint32_t free_blocks;
    uint64_t used_bytes; // rounded block sizes
    uint64_t req_bytes;  // bytes actually requested by the guest
    uint64_t free_bytes;
    uint32_t largest_free;
    uint64_t total_allocs;
    uint64_t total_frees;
};
HeapStats heap_stats();
// Bytes the heap has claimed: the base arena plus every segment it grew.
uint64_t heap_span();
// Walks the block list and returns an error string, or "" when consistent.
std::string heap_check();
