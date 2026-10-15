// codepages.h - Windows' code page conversions on host buffers: the rules of
// MultiByteToWideChar and WideCharToMultiByte, measured on Windows 11
// (tools/codepages: capture_win32.c, vectors.txt; the capture is
// runtime/tests/data/codepages_windows.tsv and its tables codepage_tables.h).
//
// The ANSI code page is 1252 and the OEM code page 437, as on an English
// (United States) Windows. Single-byte code pages 1252, 437, 850, 10000
// (CP_MACCP), 20127 and 28591 have Windows' own tables: the bytes 0x80..0x9f,
// the best-fit mappings, the default character, MB_COMPOSITE, MB_USEGLYPHCHARS,
// WC_COMPOSITECHECK and its companions. UTF-8 decodes as Windows does,
// replacement characters included, and CP_SYMBOL maps to the U+F0xx range.
// Any other code page Windows would accept (932, 1251, ...) is converted as
// 1252, with one log line; one Windows would refuse fails with
// ERROR_INVALID_PARAMETER.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string>

namespace codepages {

enum : uint32_t {
    kAnsiCodePage = 1252,
    kOemCodePage = 437,
    kMacCodePage = 10000,
};

// What one conversion did. `result` is the function's return value; when it
// is 0, `error` is the GetLastError code (0 means none was set). `written` is
// how many units or bytes went to the output buffer, also when the buffer was
// too small (Windows fills what fits before failing with 122). `used_default`
// is the lpUsedDefaultChar answer, `used_default_set` whether Windows writes it.
struct Conversion {
    int result = 0;
    uint32_t error = 0;
    size_t written = 0;
    bool used_default = false;
    bool used_default_set = false;
};

// The code page CP_ACP (0), CP_OEMCP (1), CP_MACCP (2) and CP_THREAD_ACP (3)
// stand for; any other value unchanged.
uint32_t resolve(uint32_t cp);
// IsValidCodePage: the code pages this runtime converts by their own rules.
bool is_valid(uint32_t cp);
// GetCPInfo's MaxCharSize, or 0 when GetCPInfo fails for the code page.
uint32_t max_char_size(uint32_t cp);

// MultiByteToWideChar(cp, flags, src, n, dst, cap) with the source length
// already resolved (a -1 length includes the terminator in `n`). `dst` null
// or `cap` 0 asks for the length. Arguments the shim checks itself (null
// pointers, zero or negative lengths) are not checked here.
Conversion mb_to_wide(uint32_t cp, uint32_t flags, const uint8_t *src, size_t n, uint16_t *dst,
                      size_t cap);
// WideCharToMultiByte likewise. `default_char` is lpDefaultChar (null for the
// code page's own), `want_used` whether lpUsedDefaultChar was given.
Conversion wide_to_mb(uint32_t cp, uint32_t flags, const uint16_t *src, size_t n, uint8_t *dst,
                      size_t cap, const uint8_t *default_char, bool want_used);

// One ANSI (1252) byte as UTF-16, and a UTF-16 unit as its exact 1252 byte
// (-1 when 1252 has none: no best fit, for text that must round-trip).
uint16_t ansi_to_unicode(uint8_t b);
int unicode_to_ansi(uint32_t unit);
// Whole strings, for host code: ANSI bytes to UTF-16 and back (best fit,
// '?' for the rest), and UTF-8 to UTF-16 and back (U+FFFD for bad input).
std::u16string ansi_to_utf16(const std::string &ansi);
std::string utf16_to_ansi(const std::u16string &text);
std::u16string utf8_to_utf16(const std::string &utf8);
std::string utf16_to_utf8(const std::u16string &text);

} // namespace codepages
