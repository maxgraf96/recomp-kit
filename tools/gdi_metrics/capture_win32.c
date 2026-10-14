/* capture_win32.c - GDI text metrics as Windows reports them, for the
 * runtime's text-metric comparison (tools/gdi_metrics/compare.py).
 *
 * Reads cases.txt (UTF-8) and writes one JSON line per font: the TEXTMETRICA
 * and face name a 96-dpi screen DC gives for CreateFontA(height, 0, 0, 0,
 * weight, italic, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
 * CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH, face), the
 * GetCharWidth32A widths of 0x20..0xFF, and the GetTextExtentPoint32A extent
 * of every string (converted to code page 1252). The runtime's side is
 * `gdi_tests --capture cases.txt out.jsonl`, in the same format.
 *
 * Build and run on Windows (any of):
 *   cl /O2 capture_win32.c gdi32.lib user32.lib
 *   gcc -O2 -o capture_win32.exe capture_win32.c -lgdi32 -luser32
 *   capture_win32.exe cases.txt windows.jsonl
 * Record the machine: Windows version and build, display scaling (must be
 * 100 %: the DC must report 96 dpi, which the program checks), ClearType on
 * or off (it does not change these numbers, but say which).
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_LINE 1024
#define MAX_STRINGS 64

static char g_strings[MAX_STRINGS][MAX_LINE];
static int g_string_count = 0;

/* UTF-8 to code page 1252, as the A entry points expect. */
static int to_ansi(const char *utf8, char *out, int cap) {
    WCHAR wide[MAX_LINE];
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide, MAX_LINE);
    if (n <= 0)
        return 0;
    return WideCharToMultiByte(1252, 0, wide, -1, out, cap, NULL, NULL);
}

static void trim(char *s) {
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r'))
        s[--n] = 0;
}

static void json_string(FILE *out, const char *s) {
    fputc('"', out);
    for (; *s; ++s) {
        if (*s == '"' || *s == '\\')
            fputc('\\', out);
        fputc(*s, out);
    }
    fputc('"', out);
}

static void capture(FILE *out, HDC dc, const char *face, int height, int weight, int italic) {
    char face_ansi[LF_FACESIZE];
    HFONT font, old;
    TEXTMETRICA tm;
    char realized[LF_FACESIZE];
    INT widths[224];
    int i;
    to_ansi(face, face_ansi, sizeof face_ansi);
    font = CreateFontA(height, 0, 0, 0, weight, italic, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH, face_ansi);
    old = (HFONT)SelectObject(dc, font);
    memset(&tm, 0, sizeof tm);
    GetTextMetricsA(dc, &tm);
    realized[0] = 0;
    GetTextFaceA(dc, sizeof realized, realized);
    GetCharWidth32A(dc, 0x20, 0xff, widths);
    fprintf(out, "{\"face\":");
    json_string(out, face);
    fprintf(out, ",\"height\":%d,\"weight\":%d,\"italic\":%d,\"realized\":", height, weight,
            italic);
    json_string(out, realized);
    fprintf(out,
            ",\"tm\":[%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%ld,%u,%u,%u,%u,%u,%u,%u,%u,%u]",
            tm.tmHeight, tm.tmAscent, tm.tmDescent, tm.tmInternalLeading, tm.tmExternalLeading,
            tm.tmAveCharWidth, tm.tmMaxCharWidth, tm.tmWeight, tm.tmOverhang,
            tm.tmDigitizedAspectX, tm.tmDigitizedAspectY, (unsigned char)tm.tmFirstChar,
            (unsigned char)tm.tmLastChar, (unsigned char)tm.tmDefaultChar,
            (unsigned char)tm.tmBreakChar, tm.tmItalic, tm.tmUnderlined, tm.tmStruckOut,
            tm.tmPitchAndFamily, tm.tmCharSet);
    fprintf(out, ",\"widths\":[");
    for (i = 0; i < 224; ++i)
        fprintf(out, "%s%d", i ? "," : "", widths[i]);
    fprintf(out, "],\"extents\":[");
    for (i = 0; i < g_string_count; ++i) {
        char ansi[MAX_LINE];
        SIZE size = {0, 0};
        int n = to_ansi(g_strings[i], ansi, sizeof ansi);
        GetTextExtentPoint32A(dc, ansi, n > 0 ? n - 1 : 0, &size);
        fprintf(out, "%s[%ld,%ld]", i ? "," : "", size.cx, size.cy);
    }
    fprintf(out, "]}\n");
    SelectObject(dc, old);
    DeleteObject(font);
}

/* The numbers of a comma-separated list; how many. */
static int numbers(const char *list, int *out, int cap) {
    int n = 0;
    const char *p = list;
    while (*p && n < cap) {
        out[n++] = atoi(p);
        while (*p && *p != ',')
            ++p;
        if (*p == ',')
            ++p;
    }
    return n;
}

int main(int argc, char **argv) {
    FILE *in, *out;
    char line[MAX_LINE];
    HDC dc;
    if (argc != 3) {
        fprintf(stderr, "usage: capture_win32 cases.txt out.jsonl\n");
        return 2;
    }
    in = fopen(argv[1], "rb");
    out = fopen(argv[2], "wb");
    if (!in || !out) {
        fprintf(stderr, "cannot open the files\n");
        return 2;
    }
    SetProcessDPIAware();
    dc = GetDC(NULL);
    if (GetDeviceCaps(dc, LOGPIXELSY) != 96) {
        fprintf(stderr, "the screen is not at 96 dpi (100%% scaling): %d\n",
                GetDeviceCaps(dc, LOGPIXELSY));
        return 3;
    }
    /* Strings first: every font measures all of them. */
    while (fgets(line, sizeof line, in)) {
        trim(line);
        if (!strncmp(line, "string ", 7) && g_string_count < MAX_STRINGS)
            strcpy(g_strings[g_string_count++], line + 7);
    }
    rewind(in);
    while (fgets(line, sizeof line, in)) {
        char *face, *heights, *weights, *italic;
        int h[64], w[8], nh, nw, i, j;
        trim(line);
        if (strncmp(line, "font ", 5))
            continue;
        face = line + 5;
        heights = strchr(face, ';');
        if (!heights)
            continue;
        *heights++ = 0;
        weights = strchr(heights, ';');
        if (!weights)
            continue;
        *weights++ = 0;
        italic = strchr(weights, ';');
        if (!italic)
            continue;
        *italic++ = 0;
        nh = numbers(heights, h, 64);
        nw = numbers(weights, w, 8);
        for (i = 0; i < nh; ++i)
            for (j = 0; j < nw; ++j)
                capture(out, dc, face, h[i], w[j], atoi(italic));
    }
    ReleaseDC(NULL, dc);
    fclose(out);
    fclose(in);
    return 0;
}
