/* capture_win32.c - Windows' code page conversions, as the runtime's
 * MultiByteToWideChar/WideCharToMultiByte must reproduce them.
 *
 * Writes TSV to stdout:
 *   mb  <cp> <flags> <byte>  <ret> <err> <units...>     every byte, alone
 *   wc  <cp> <flags> <unit>  <ret> <err> <bytes> <used> every UTF-16 unit,
 *       alone, when it maps to something other than the default character
 *   cpinfo <cp> <ret> <MaxCharSize> <DefaultChar hex> <LeadByte hex>
 *   valid <cp> <IsValidCodePage>
 *   vec <line of vectors.txt> => <ret> <err> <output> [<used>]
 * Code units and bytes are hex; <err> is GetLastError after the call, which
 * the program sets to 0xdead first, so "dead" means untouched.
 * tools/codepages/gen_tables.py turns the mb/wc lines into
 * runtime/codepage_tables.h; the vec, cpinfo and valid lines are the
 * reference crt_tests replays (runtime/tests/data/codepages_windows.tsv).
 *
 * Build and run on Windows, 32-bit (from an x86 Native Tools prompt):
 *   cl /nologo /O1 capture_win32.c
 *   capture_win32.exe vectors.txt > windows.tsv
 * Record the Windows version and build; the system locale must be one whose
 * ANSI code page is 1252 and OEM code page 437 (English (United States)).
 */
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const UINT kTableCps[] = {1252, 437, 850, 10000, 20127, 28591};

static void tables(void) {
    static const DWORD mb_flags[] = {0, MB_ERR_INVALID_CHARS, MB_USEGLYPHCHARS, MB_COMPOSITE};
    static const DWORD wc_flags[] = {0, WC_NO_BEST_FIT_CHARS};
    for (int t = 0; t < (int)(sizeof kTableCps / sizeof kTableCps[0]); ++t) {
        UINT cp = kTableCps[t];
        for (int f = 0; f < 4; ++f) {
            for (int b = 0; b < 256; ++b) {
                char in = (char)b;
                WCHAR out[8];
                SetLastError(0xdead);
                int r = MultiByteToWideChar(cp, mb_flags[f], &in, 1, out, 8);
                DWORD err = GetLastError();
                printf("mb\t%u\t%lx\t%02x\t%d\t%lx\t", cp, mb_flags[f], b, r, err);
                for (int i = 0; i < r; ++i)
                    printf("%s%04x", i ? " " : "", out[i]);
                printf("\n");
            }
        }
        for (int f = 0; f < 2; ++f) {
            for (int u = 0; u < 0x10000; ++u) {
                WCHAR in = (WCHAR)u;
                char out[8];
                BOOL used = 7;
                SetLastError(0xdead);
                int r = WideCharToMultiByte(cp, wc_flags[f], &in, 1, out, 8, NULL, &used);
                DWORD err = GetLastError();
                if (r == 1 && (unsigned char)out[0] == '?' && used == TRUE && u != '?')
                    continue; /* the default character: implied */
                printf("wc\t%u\t%lx\t%04x\t%d\t%lx\t", cp, wc_flags[f], u, r, err);
                for (int i = 0; i < r; ++i)
                    printf("%02x", (unsigned char)out[i]);
                printf("\t%d\n", used);
            }
        }
    }
}

static void cpinfo(void) {
    static const UINT cps[] = {0, 1, 2, 3, 42, 437, 850, 1200, 1252, 10000, 20127, 28591, 65000, 65001, 12345};
    for (size_t i = 0; i < sizeof cps / sizeof cps[0]; ++i) {
        CPINFO info;
        memset(&info, 0xcc, sizeof info);
        SetLastError(0xdead);
        BOOL r = GetCPInfo(cps[i], &info);
        DWORD err = GetLastError();
        printf("cpinfo\t%u\t%d\t%lx\t%u\t%02x%02x\t", cps[i], r, err, r ? info.MaxCharSize : 0,
               r ? info.DefaultChar[0] : 0, r ? info.DefaultChar[1] : 0);
        for (int k = 0; r && k < MAX_LEADBYTES; ++k)
            printf("%02x", info.LeadByte[k]);
        printf("\n");
        printf("valid\t%u\t%d\n", cps[i], IsValidCodePage(cps[i]));
    }
    printf("acp\t%u\toemcp\t%u\n", GetACP(), GetOEMCP());
}

/* Hex text to bytes; "-" is NULL. Returns the byte count. */
static int unhex(const char *s, unsigned char *out, int cap, int *is_null) {
    *is_null = strcmp(s, "-") == 0;
    if (*is_null || strcmp(s, "\"\"") == 0)
        return 0;
    int n = 0;
    for (; s[0] && s[1] && n < cap; s += 2) {
        unsigned v;
        sscanf(s, "%2x", &v);
        out[n++] = (unsigned char)v;
    }
    return n;
}

/* One vectors.txt line:
 *   m2w <cp> <flags> <src hex|-> <cb> <cch>
 *   w2m <cp> <flags> <src hex of UTF-16LE|-> <cch> <cb> <default hex|-> <used 0/1>
 * The source is copied into a buffer of its own, followed by guard bytes that
 * are not NUL, so a length of -1 reads the NUL the hex spells and no more.
 * The destination is a 64-unit buffer filled with 0xcc, printed in full up to
 * max(cb or cch, ret), so writes past a short buffer would show. */
static void vector(char *line) {
    char op[8], src[512], defc[16];
    unsigned cp;
    unsigned long flags;
    int a, b, used_req = 0;
    unsigned char srcb[256 + 8];
    int null_src = 0, null_def = 0;
    char echo[1024];
    strncpy(echo, line, sizeof echo - 1);
    echo[sizeof echo - 1] = 0;
    if (sscanf(line, "%7s", op) != 1 || op[0] == '#')
        return;
    if (strcmp(op, "m2w") == 0) {
        if (sscanf(line, "%7s %u %lx %511s %d %d", op, &cp, &flags, src, &a, &b) != 6)
            return;
        int n = unhex(src, srcb, 256, &null_src);
        memset(srcb + n, 0x41, 8);
        WCHAR out[64 + 8];
        memset(out, 0xcc, sizeof out);
        SetLastError(0xdead);
        int r = MultiByteToWideChar(cp, flags, null_src ? NULL : (LPCSTR)srcb, a, b ? out : NULL, b);
        DWORD err = GetLastError();
        int show = b > r ? b : r;
        if (show > 64)
            show = 64;
        printf("vec\t%s\t=>\t%d\t%lx\t", echo, r, err);
        for (int i = 0; b && i < show; ++i)
            printf("%s%04x", i ? " " : "", out[i]);
        printf("\n");
    } else if (strcmp(op, "w2m") == 0) {
        if (sscanf(line, "%7s %u %lx %511s %d %d %15s %d", op, &cp, &flags, src, &a, &b, defc,
                   &used_req) != 8)
            return;
        int n = unhex(src, srcb, 256, &null_src);
        memset(srcb + n, 0x41, 8);
        unsigned char defb[8] = {0};
        unhex(defc, defb, 4, &null_def);
        char out[64 + 8];
        memset(out, 0xcc, sizeof out);
        BOOL used = 7;
        SetLastError(0xdead);
        int r = WideCharToMultiByte(cp, flags, null_src ? NULL : (LPCWSTR)srcb, a, b ? out : NULL,
                                    b, null_def ? NULL : (LPCSTR)defb, used_req ? &used : NULL);
        DWORD err = GetLastError();
        int show = b > r ? b : r;
        if (show > 64)
            show = 64;
        printf("vec\t%s\t=>\t%d\t%lx\t", echo, r, err);
        for (int i = 0; b && i < show; ++i)
            printf("%02x", (unsigned char)out[i]);
        printf("\t%d\n", used_req ? used : -1);
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: capture_win32 vectors.txt > windows.tsv\n");
        return 2;
    }
    OSVERSIONINFOA v;
    v.dwOSVersionInfoSize = sizeof v;
#pragma warning(suppress : 4996)
    GetVersionExA(&v);
    printf("# windows\t%lu.%lu.%lu\n", v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber);
    cpinfo();
    FILE *f = fopen(argv[1], "r");
    if (!f) {
        fprintf(stderr, "cannot open %s\n", argv[1]);
        return 1;
    }
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = 0;
        if (n)
            vector(line);
    }
    fclose(f);
    if (argc < 3 || strcmp(argv[2], "--no-tables") != 0)
        tables();
    return 0;
}
