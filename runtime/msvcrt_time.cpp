// msvcrt_time.cpp - time, clock, localtime/gmtime/mktime, asctime, strftime
// and the locale time names msvcp60 asks msvcrt for.
//
// time_t is msvcrt.dll's 32-bit one. localtime and gmtime share one struct tm
// per thread, as they do in msvcrt, and refuse negative times. Local time is
// the host's, which is also what GetLocalTime reports in this runtime.
// clock() is wall time since start-up in milliseconds (CLOCKS_PER_SEC is
// 1000), which is what msvcrt measures, not processor time.
#include "msvcrt_internal.h"
#include "../platform/os.h"
#include <ctime>
#include <string>
#include <vector>

namespace crt {
namespace {

const char *const kDayAbbr[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
const char *const kDay[7] = {"Sunday",   "Monday", "Tuesday", "Wednesday",
                             "Thursday", "Friday", "Saturday"};
const char *const kMonAbbr[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                  "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
const char *const kMon[12] = {"January",   "February", "March",    "April",
                              "May",       "June",     "July",     "August",
                              "September", "October",  "November", "December"};

uint64_t g_start_ns = os_monotonic_ns();

struct Tm {
    int sec, min, hour, mday, mon, year, wday, yday, isdst;
};
Tm read_tm(uint32_t p) {
    Tm t;
    int *f = &t.sec;
    for (int i = 0; i < 9; ++i)
        f[i] = (int32_t)rd32(p + 4 * i);
    return t;
}
void write_tm(uint32_t p, const struct tm &t) {
    const int f[9] = {t.tm_sec,  t.tm_min,  t.tm_hour,
                      t.tm_mday, t.tm_mon,  t.tm_year,
                      t.tm_wday, t.tm_yday, t.tm_isdst > 0 ? 1 : 0};
    for (int i = 0; i < 9; ++i)
        wr32(p + 4 * i, (uint32_t)f[i]);
}

uint32_t thread_buffer(uint32_t ThreadData::*field, uint32_t size) {
    ThreadData &td = thread_data();
    if (!(td.*field))
        td.*field = mem_alloc(size);
    return td.*field;
}

int64_t now_seconds() {
    return (int64_t)(os_wall_time_us() / 1000000);
}

void crt_time(X86 *c) {
    uint32_t t = (uint32_t)now_seconds();
    if (arg(c, 0))
        wr32(arg(c, 0), t);
    set_eax(c, t);
}

void crt_clock(X86 *c) {
    set_eax(c, (uint32_t)((os_monotonic_ns() - g_start_ns) / 1000000));
}

void convert(X86 *c, bool local) {
    uint32_t pt = arg(c, 0);
    int32_t t = pt ? (int32_t)rd32(pt) : -1;
    struct tm out{};
    if (t < 0 || (local ? os_localtime(t, &out) : os_gmtime(t, &out)) != 0) {
        set_errno(E_INVAL);
        set_eax(c, 0);
        return;
    }
    uint32_t buf = thread_buffer(&ThreadData::tm_buf, 36);
    write_tm(buf, out);
    set_eax(c, buf);
}
void crt_localtime(X86 *c) {
    convert(c, true);
}
void crt_gmtime(X86 *c) {
    convert(c, false);
}

// mktime normalises the structure it is given and returns the local time it
// names, or -1 outside the 32-bit range (1970 to 2038-01-18 19:14:07 UTC).
void crt_mktime(X86 *c) {
    uint32_t p = arg(c, 0);
    Tm in = read_tm(p);
    struct tm t{};
    t.tm_sec = in.sec;
    t.tm_min = in.min;
    t.tm_hour = in.hour;
    t.tm_mday = in.mday;
    t.tm_mon = in.mon;
    t.tm_year = in.year;
    t.tm_isdst = in.isdst;
    time_t r = mktime(&t);
    if (r == (time_t)-1 || r < 0 || r > 0x7fffd27f) {
        set_eax(c, 0xffffffffu);
        return;
    }
    write_tm(p, t);
    set_eax(c, (uint32_t)r);
}

// asctime: "Wed Jan 02 02:03:55 1980\n". msvcrt pads the day of the month
// with a zero where the C standard pads with a space.
std::string asctime_text(const Tm &t) {
    char buf[64];
    snprintf(buf, sizeof buf, "%.3s %.3s %02d %02d:%02d:%02d %4d\n",
             (t.wday >= 0 && t.wday < 7) ? kDayAbbr[t.wday] : "???",
             (t.mon >= 0 && t.mon < 12) ? kMonAbbr[t.mon] : "???", t.mday, t.hour, t.min, t.sec,
             t.year + 1900);
    return buf;
}
void crt_asctime(X86 *c) {
    uint32_t buf = thread_buffer(&ThreadData::asctime_buf, 26);
    if (!buf) {
        set_eax(c, 0);
        return;
    }
    gm_put_str(buf, asctime_text(read_tm(arg(c, 0))).c_str(), 26);
    set_eax(c, buf);
}

// The locale's date and time pictures (Windows GetLocaleInfo syntax).
struct TimeFormats {
    const char *short_date, *long_date, *time;
};
TimeFormats time_formats() {
    // The C locale's, and the user default (English, United States).
    if (ctype_is_c_locale())
        return {"MM/dd/yy", "dddd, MMMM dd, yyyy", "HH:mm:ss"};
    return {"M/d/yyyy", "dddd, MMMM d, yyyy", "h:mm:ss tt"};
}

// Expands a Windows date/time picture: d dd ddd dddd, M MM MMM MMMM, y yy
// yyyy, h hh H HH, m mm, s ss, t tt, and 'quoted' text.
std::string expand_picture(const char *pic, const Tm &t) {
    std::string out;
    char buf[16];
    for (const char *p = pic; *p;) {
        char ch = *p;
        int n = 1;
        if (ch == '\'') {
            ++p;
            while (*p && *p != '\'')
                out.push_back(*p++);
            if (*p)
                ++p;
            continue;
        }
        while (p[n] == ch)
            ++n;
        p += n;
        switch (ch) {
        case 'd':
            if (n >= 4)
                out += kDay[(t.wday % 7 + 7) % 7];
            else if (n == 3)
                out += kDayAbbr[(t.wday % 7 + 7) % 7];
            else {
                snprintf(buf, sizeof buf, n == 2 ? "%02d" : "%d", t.mday);
                out += buf;
            }
            break;
        case 'M':
            if (n >= 4)
                out += kMon[(t.mon % 12 + 12) % 12];
            else if (n == 3)
                out += kMonAbbr[(t.mon % 12 + 12) % 12];
            else {
                snprintf(buf, sizeof buf, n == 2 ? "%02d" : "%d", t.mon + 1);
                out += buf;
            }
            break;
        case 'y':
            if (n >= 3)
                snprintf(buf, sizeof buf, "%d", t.year + 1900);
            else
                snprintf(buf, sizeof buf, n == 2 ? "%02d" : "%d", (t.year + 1900) % 100);
            out += buf;
            break;
        case 'h':
        case 'H': {
            int h = ch == 'h' ? (t.hour % 12 == 0 ? 12 : t.hour % 12) : t.hour;
            snprintf(buf, sizeof buf, n >= 2 ? "%02d" : "%d", h);
            out += buf;
            break;
        }
        case 'm':
        case 's':
            snprintf(buf, sizeof buf, n >= 2 ? "%02d" : "%d", ch == 'm' ? t.min : t.sec);
            out += buf;
            break;
        case 't':
            out += n >= 2 ? (t.hour < 12 ? "AM" : "PM") : (t.hour < 12 ? "A" : "P");
            break;
        default:
            out.append((size_t)n, ch);
            break;
        }
    }
    return out;
}

int week_number(const Tm &t, int first_day) {
    int wday = (t.wday - first_day + 7) % 7;
    return (t.yday + 7 - wday) / 7;
}

// strftime's conversions, with msvcrt's '#' flag (no leading zeros; the long
// forms of %c and %x).
std::string format_time(const char *fmt, const Tm &t, bool *ok) {
    std::string out;
    char buf[64];
    *ok = true;
    TimeFormats tf = time_formats();
    for (const char *p = fmt; *p; ++p) {
        if (*p != '%') {
            out.push_back(*p);
            continue;
        }
        ++p;
        bool alt = false;
        if (*p == '#') {
            alt = true;
            ++p;
        }
        const char *two = alt ? "%d" : "%02d";
        switch (*p) {
        case 'a':
            out += kDayAbbr[(t.wday % 7 + 7) % 7];
            break;
        case 'A':
            out += kDay[(t.wday % 7 + 7) % 7];
            break;
        case 'b':
            out += kMonAbbr[(t.mon % 12 + 12) % 12];
            break;
        case 'B':
            out += kMon[(t.mon % 12 + 12) % 12];
            break;
        case 'c':
            out += expand_picture(alt ? tf.long_date : tf.short_date, t);
            out += ' ';
            out += expand_picture(tf.time, t);
            break;
        case 'd':
            snprintf(buf, sizeof buf, two, t.mday);
            out += buf;
            break;
        case 'H':
            snprintf(buf, sizeof buf, two, t.hour);
            out += buf;
            break;
        case 'I':
            snprintf(buf, sizeof buf, two, t.hour % 12 == 0 ? 12 : t.hour % 12);
            out += buf;
            break;
        case 'j':
            snprintf(buf, sizeof buf, alt ? "%d" : "%03d", t.yday + 1);
            out += buf;
            break;
        case 'm':
            snprintf(buf, sizeof buf, two, t.mon + 1);
            out += buf;
            break;
        case 'M':
            snprintf(buf, sizeof buf, two, t.min);
            out += buf;
            break;
        case 'p':
            out += t.hour < 12 ? "AM" : "PM";
            break;
        case 'S':
            snprintf(buf, sizeof buf, two, t.sec);
            out += buf;
            break;
        case 'U':
            snprintf(buf, sizeof buf, two, week_number(t, 0));
            out += buf;
            break;
        case 'w':
            snprintf(buf, sizeof buf, "%d", t.wday);
            out += buf;
            break;
        case 'W':
            snprintf(buf, sizeof buf, two, week_number(t, 1));
            out += buf;
            break;
        case 'x':
            out += expand_picture(alt ? tf.long_date : tf.short_date, t);
            break;
        case 'X':
            out += expand_picture(tf.time, t);
            break;
        case 'y':
            snprintf(buf, sizeof buf, two, (t.year + 1900) % 100);
            out += buf;
            break;
        case 'Y':
            snprintf(buf, sizeof buf, "%d", t.year + 1900);
            out += buf;
            break;
        case 'z':
        case 'Z':
            // Windows prints its time zone's name, which nothing portable
            // reproduces; the zone is left out rather than invented.
            break;
        case '%':
            out.push_back('%');
            break;
        default:
            *ok = false; // an invalid conversion: msvcrt returns 0
            return out;
        }
    }
    return out;
}

void strftime_into(X86 *c, uint32_t buf, uint32_t max, uint32_t fmt, uint32_t tm) {
    bool ok = true;
    std::string s = format_time(gchars(fmt), read_tm(tm), &ok);
    if (!ok || s.size() + 1 > max) {
        if (max)
            wr8(buf, 0);
        set_eax(c, 0);
        return;
    }
    gm_put_str(buf, s.c_str(), max);
    set_eax(c, (uint32_t)s.size());
}
void crt_strftime(X86 *c) {
    strftime_into(c, arg(c, 0), arg(c, 1), arg(c, 2), arg(c, 3));
}
// _Strftime(buf, max, fmt, tm, lc_time): the time names come from the
// current locale either way.
void crt_Strftime(X86 *c) {
    strftime_into(c, arg(c, 0), arg(c, 1), arg(c, 2), arg(c, 3));
}

// _Getdays/_Getmonths: ":Sun:Sunday:Mon:Monday:..." on the heap, which the
// caller frees.
void crt_Getdays(X86 *c) {
    std::string s;
    for (int i = 0; i < 7; ++i)
        s += std::string(":") + kDayAbbr[i] + ":" + kDay[i];
    set_eax(c, mem_strdup(s));
}
void crt_Getmonths(X86 *c) {
    std::string s;
    for (int i = 0; i < 12; ++i)
        s += std::string(":") + kMonAbbr[i] + ":" + kMon[i];
    set_eax(c, mem_strdup(s));
}
// _Gettnames: one heap block holding the time-names structure (7 + 7 + 12 +
// 12 + 2 + 3 string pointers) followed by the strings, so a single free()
// releases it.
void crt_Gettnames(X86 *c) {
    TimeFormats tf = time_formats();
    std::vector<std::string> strings;
    for (auto *s : kDayAbbr)
        strings.push_back(s);
    for (auto *s : kDay)
        strings.push_back(s);
    for (auto *s : kMonAbbr)
        strings.push_back(s);
    for (auto *s : kMon)
        strings.push_back(s);
    strings.push_back("AM");
    strings.push_back("PM");
    strings.push_back(tf.short_date);
    strings.push_back(tf.long_date);
    strings.push_back(tf.time);
    uint32_t header = 4 * (uint32_t)strings.size();
    uint32_t bytes = header;
    for (auto &s : strings)
        bytes += (uint32_t)s.size() + 1;
    uint32_t block = mem_alloc(bytes);
    if (!block) {
        set_eax(c, 0);
        return;
    }
    uint32_t at = block + header;
    for (size_t i = 0; i < strings.size(); ++i) {
        wr32(block + 4 * (uint32_t)i, at);
        at += gm_put_str(at, strings[i].c_str(), (uint32_t)strings[i].size() + 1) + 1;
    }
    set_eax(c, block);
}

// _timezone: seconds west of UTC in standard time.
void init_timezone(uint32_t addr) {
    int64_t now = now_seconds();
    struct tm local{}, utc{};
    os_localtime(now, &local);
    os_gmtime(now, &utc);
    int64_t lsec = (int64_t)local.tm_yday * 86400 + local.tm_hour * 3600 + local.tm_min * 60;
    int64_t usec = (int64_t)utc.tm_yday * 86400 + utc.tm_hour * 3600 + utc.tm_min * 60;
    if (local.tm_year != utc.tm_year)
        lsec += (local.tm_year > utc.tm_year ? 1 : -1) * 365 * 86400;
    int64_t west = usec - lsec;
    if (local.tm_isdst > 0)
        west += 3600;
    wr32(addr, (uint32_t)(int32_t)west);
}

const ImportShim shims[] = {
    {"msvcrt.dll", "time", ARGC_CDECL, crt_time},
    {"msvcrt.dll", "clock", ARGC_CDECL, crt_clock},
    {"msvcrt.dll", "localtime", ARGC_CDECL, crt_localtime},
    {"msvcrt.dll", "gmtime", ARGC_CDECL, crt_gmtime},
    {"msvcrt.dll", "mktime", ARGC_CDECL, crt_mktime},
    {"msvcrt.dll", "asctime", ARGC_CDECL, crt_asctime},
    {"msvcrt.dll", "strftime", ARGC_CDECL, crt_strftime},
    {"msvcrt.dll", "_Strftime", ARGC_CDECL, crt_Strftime},
    {"msvcrt.dll", "_Getdays", ARGC_CDECL, crt_Getdays},
    {"msvcrt.dll", "_Getmonths", ARGC_CDECL, crt_Getmonths},
    {"msvcrt.dll", "_Gettnames", ARGC_CDECL, crt_Gettnames},
};

} // namespace

void register_time() {
    imports_register(shims, sizeof shims / sizeof shims[0]);
    imports_register_data_init("msvcrt.dll", "_timezone", 4, init_timezone);
}

} // namespace crt
