// msvcrt_internal.h - shared pieces of the msvcrt.dll / msvcp60.dll HLE.
//
// The C runtime is split by area (heap, locale and ctype, strings, number
// formatting and parsing, stdio, maths, time, start-up); each file registers
// its own shim table from msvcrt_register(). Everything here works on guest
// addresses: a guest pointer is a uint32_t, and the host never hands one of
// its own pointers to the guest.
//
// Behaviour follows Windows' msvcrt.dll as observed from the outside (its
// documentation and black-box test vectors), not the C standard where the two
// differ: three-digit printf exponents, round-half-up of printf's decimal
// digits, the per-thread rand() state, 'd' as an exponent marker in strtod.
// Each such rule is commented where it is implemented.
#pragma once
#include "imports.h"
#include "memory.h"
#include <stdint.h>
#include <string>
#include <vector>

namespace crt {

// ---------------------------------------------------------------------------
// Heap (msvcrt_heap.cpp). malloc's contract: 16-byte aligned, the requested
// size remembered for _msize, and a free of anything that is not the start of
// a live block logged and ignored rather than allowed to corrupt the heap.
// ---------------------------------------------------------------------------
uint32_t mem_alloc(uint32_t size);
uint32_t mem_calloc(uint32_t count, uint32_t size);
// `who` names the caller in the log line when `addr` is not a live block;
// `c`, when given, supplies the guest return address for it.
void mem_free(uint32_t addr, const char *who, const X86 *c = nullptr);
uint32_t mem_realloc(uint32_t addr, uint32_t size);
// Requested size of a live block, or 0xffffffff when `addr` is not one.
uint32_t mem_size(uint32_t addr);
// A NUL-terminated copy of `s` on the CRT heap, or 0.
uint32_t mem_strdup(const std::string &s);
// Counts for the tests: live blocks, rejected frees.
struct HeapCounters {
    uint64_t live_blocks;
    uint64_t bad_frees;
    uint64_t slab_chunks;
};
HeapCounters heap_counters();

// ---------------------------------------------------------------------------
// Per-thread state. msvcrt keeps errno, the rand() seed, strtok's position
// and the static buffers of localtime/asctime per thread; so does this. Each
// guest thread runs on its own host thread, so host thread-local storage is
// per guest thread. Guest buffers are allocated on first use.
// ---------------------------------------------------------------------------
struct ThreadData {
    uint32_t epoch = 0;    // heap generation the guest addresses below belong to
    uint32_t holdrand = 1; // rand(): Windows starts every thread at seed 1
    uint32_t errno_addr = 0;
    uint32_t doserrno_addr = 0;
    uint32_t strtok_next = 0;
    uint32_t tm_buf = 0;      // struct tm for gmtime/localtime (36 bytes)
    uint32_t asctime_buf = 0; // 26 bytes
};
ThreadData &thread_data();
uint32_t heap_epoch();
void set_errno(int e);
enum {
    E_PERM = 1,
    E_NOENT = 2,
    E_BADF = 9,
    E_NOMEM = 12,
    E_ACCES = 13,
    E_EXIST = 17,
    E_INVAL = 22,
    E_MFILE = 24,
    E_DOM = 33,
    E_RANGE = 34,
};

// ---------------------------------------------------------------------------
// Locale (msvcrt_locale.cpp). Two locales exist: "C" and the user default,
// English_United States.1252, which every name setlocale accepts maps to
// (with the decimal separator of the language asked for). The multibyte code
// page of the _mbs* functions is the ANSI code page, 1252, from start-up, as
// it is on Windows regardless of setlocale.
// ---------------------------------------------------------------------------
enum { CAT_ALL = 0, CAT_COLLATE, CAT_CTYPE, CAT_MONETARY, CAT_NUMERIC, CAT_TIME, CAT_COUNT };
// msvcrt's _ctype bits.
enum : uint16_t {
    CT_UPPER = 0x1,
    CT_LOWER = 0x2,
    CT_DIGIT = 0x4,
    CT_SPACE = 0x8,
    CT_PUNCT = 0x10,
    CT_CONTROL = 0x20,
    CT_BLANK = 0x40,
    CT_HEX = 0x80,
    CT_ALPHA = 0x100,
    CT_LEADBYTE = 0x8000,
};
// _ctype bits of `c` (-128..255; EOF and anything else answer 0) under the
// current LC_CTYPE. Negative values other than EOF are the signed-char view
// of 128..255, as msvcrt's table extends below zero for them.
uint16_t ctype_bits(int c);
int ctype_tolower(int c);
int ctype_toupper(int c);
bool ctype_is_c_locale();
// The ANSI code page's case map used by _mbs* (cp1252, locale independent).
uint8_t mbc_lower(uint8_t b);
uint8_t mbc_upper(uint8_t b);
bool mbc_is_space(uint32_t c);
bool mbc_is_digit(uint32_t c);
// The decimal separator of LC_NUMERIC.
char numeric_decimal_point();
// Byte <-> UTF-16 under LC_CTYPE: the C locale widens bytes unchanged,
// cp1252 maps 0x80..0x9f through its table. -1 when unmappable.
int mb_to_wide(uint8_t b);
int wide_to_mb(uint32_t w);
// Unicode simple case mapping and C1 classification for wide characters.
uint32_t wide_toupper(uint32_t w);
uint32_t wide_tolower(uint32_t w);
uint16_t wide_ctype_bits(uint32_t w);
// cp1252 <-> UTF-16 independent of the locale (the msvcp60 conversions).
uint16_t cp1252_to_unicode(uint8_t b);
int unicode_to_cp1252(uint32_t w);
void locale_reset();
uint32_t localeconv_addr();

// ---------------------------------------------------------------------------
// Guest string access (msvcrt_string.cpp).
// ---------------------------------------------------------------------------
// Length of the NUL-terminated guest string at `a`, bounded by the arena.
uint32_t gstrlen(uint32_t a);
uint32_t gwcslen(uint32_t a);
static inline const char *gchars(uint32_t a) {
    return (const char *)(g_mem + a);
}

// ---------------------------------------------------------------------------
// Numbers (msvcrt_format.cpp).
// ---------------------------------------------------------------------------
// The digits of a finite double in msvcrt's printf model: the decimal
// expansion correctly rounded to `sig` significant digits (17), as a digit
// string with no leading zero, and the decimal exponent such that the value
// is 0.DIGITS x 10^decpt. Zero gives "0" and decpt 1.
struct Digits {
    std::string digits;
    int decpt;
    bool negative;
};
Digits double_digits(double v, int sig);

// A characters source for the scanf engine and the decimal scanner.
struct CharSource {
    virtual int get() = 0;         // next byte, or -1 at the end
    virtual void unget(int c) = 0; // push back what get() returned
    virtual ~CharSource() = default;
};

// Scans a decimal floating-point number in msvcrt's strtod grammar from
// `src` (leading white space skipped by the caller as the function requires),
// consuming at most `width` characters (0 = unlimited). On success returns
// true with the canonical text in `out` ("[-]digits[.digits][e[-]digits]").
// `consumed` counts the characters taken.
bool scan_decimal(CharSource &src, uint32_t width, bool d_exponent, std::string &out,
                  uint32_t &consumed);
// Correctly rounded conversions of a canonical decimal text. `range` reports
// overflow to infinity or underflow to zero from a non-zero value.
double decimal_to_double(const std::string &canon, bool *range);
float decimal_to_float(const std::string &canon, bool *range);

// printf-family formatting from a guest argument pointer (cdecl varargs:
// dword slots, doubles and __int64 taking two). Returns the formatted bytes.
std::string format_guest(const char *fmt, uint32_t ap);
// scanf-family conversion. Returns the count of assigned fields or -1 (EOF)
// when the input ended before the first conversion.
int scan_guest(CharSource &src, const char *fmt, uint32_t ap);

// ---------------------------------------------------------------------------
// stdio (msvcrt_stdio.cpp): the guest FILE table and output to it.
// ---------------------------------------------------------------------------
void stdio_reset();
void stdio_flush_all();
// Writes bytes to a guest FILE; returns bytes written or -1.
int64_t stdio_write(uint32_t file, const char *data, size_t n);

// ---------------------------------------------------------------------------
// Process-level state (msvcrt.cpp).
// ---------------------------------------------------------------------------
// Runs the process onexit table and flushes the streams: exit() without the
// ExitProcess. Used by exit() and by the tests.
void run_exit_handlers(X86 *c);
// The value of _fmode (0 for text, 0x8000 for _O_BINARY).
uint32_t fmode_value();
// For the tests: type_info::name's undecoration and __getmainargs' split.
std::string undecorated_type_name(const char *decorated);
std::vector<std::string> command_line_arguments(const std::string &line);

// Registration of each area's table.
void register_heap();
void register_locale();
void register_string();
void register_format();
void register_stdio();
void register_math();
void register_float();
void register_time();
void register_process();

} // namespace crt
