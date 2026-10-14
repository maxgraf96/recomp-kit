// msvcrt_format.cpp - printf and scanf formatting, strtod/atof, strtol and
// the other number conversions.
//
// Floating-point output follows msvcrt.dll's observable model, which is not
// the C standard's:
//   1. A double becomes 17 significant decimal digits (correctly rounded
//      here) and a decimal exponent. Digits past the 17th print as zeros.
//   2. The digit string is then rounded to the precision asked for by looking
//      at the next character only: '5' or above rounds up. That is a second
//      rounding, half away from zero on the digit string, so printf("%.0f",
//      2.5) gives "3" and printf("%.2f", 0.125) gives "0.13".
//   3. Exponents have at least three digits ("1.000000e+002").
//   4. Infinities and NaNs are the digit strings "1#INF", "1#QNAN", "1#SNAN"
//      and "1#IND", which go through the same rounding: printf("%.2f", inf)
//      prints "1.#J" and "%.1f" prints "1.$", exactly as on Windows.
// Step 1's digit count is the one constant to revisit if the Windows vectors
// (tests/crt in the game repository) disagree.
//
// Decimal input (strtod, atof, scanf's %f) is converted correctly rounded,
// directly to float for a float target. msvcrt's grammar differs from C99:
// no "inf"/"nan" and no hexadecimal floats, and strtod/atof also take 'd' or
// 'D' as the exponent marker.
#include "msvcrt_internal.h"
#include <cerrno>
#include <clocale>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <string>

namespace crt {

// Significant digits of msvcrt's double-to-decimal step.
static const int kPrintfDigits = 17;

// ---------------------------------------------------------------------------
// Double -> decimal digits.
// ---------------------------------------------------------------------------
Digits double_digits(double v, int sig) {
    Digits d;
    d.negative = std::signbit(v);
    if (v == 0) {
        d.digits = "0";
        d.decpt = 1;
        return d;
    }
    // The host's printf is exact; %.*e gives `sig` digits correctly rounded.
    char buf[64];
    snprintf(buf, sizeof buf, "%.*e", sig - 1, std::fabs(v));
    const char *p = buf;
    d.digits.push_back(*p++);
    if (*p && *p != 'e')
        ++p; // the host's decimal separator, whatever the host locale says
    while (*p >= '0' && *p <= '9')
        d.digits.push_back(*p++);
    int exp10 = (*p == 'e') ? atoi(p + 1) : 0;
    d.decpt = exp10 + 1;
    return d;
}

namespace {

// The digit string msvcrt formats from: 17 significant digits for a finite
// value, or the special strings for an infinity or NaN.
struct FloatText {
    std::string digits;
    int decpt;
    bool negative;
    bool special;
};

FloatText float_text(double v) {
    uint64_t bits;
    memcpy(&bits, &v, 8);
    FloatText t;
    t.negative = (bits >> 63) != 0;
    t.special = false;
    uint64_t exp = (bits >> 52) & 0x7ff, mant = bits & 0xfffffffffffffull;
    if (exp == 0x7ff) {
        t.special = true;
        t.decpt = 1;
        if (!mant)
            t.digits = "1#INF";
        else if (t.negative && mant == 0x8000000000000ull)
            t.digits = "1#IND"; // the x87 "indefinite" NaN
        else if (mant & 0x8000000000000ull)
            t.digits = "1#QNAN";
        else
            t.digits = "1#SNAN";
        return t;
    }
    Digits d = double_digits(v, kPrintfDigits);
    t.digits = d.digits;
    t.decpt = d.decpt;
    return t;
}

// Takes `count` characters of the digit string, padding with '0', and when
// count >= 0 rounds half up on the next character alone, as msvcrt does. A
// carry out of the first digit yields a leading '1', one character more, and
// moves the decimal exponent; nothing is dropped, so %f keeps its digit count
// relative to the new exponent.
std::string round_digits(const std::string &mant, int count, int &decpt) {
    std::string out = "0"; // room for a carry
    size_t i = 0;
    for (int k = 0; k < count; ++k)
        out.push_back(i < mant.size() ? mant[i++] : '0');
    if (count >= 0 && i < mant.size() && mant[i] >= '5') {
        size_t at = out.size() - 1;
        while (out[at] == '9')
            out[at--] = '0';
        out[at] = char(out[at] + 1);
    }
    if (out[0] == '1') {
        ++decpt;
        return out;
    }
    return out.substr(1);
}

void crop_zeros(std::string &s, char point) {
    if (s.find(point) == std::string::npos)
        return;
    while (s.back() == '0')
        s.pop_back();
    if (s.back() == point)
        s.pop_back();
}

std::string exponent_text(int e, char letter) {
    char buf[16];
    snprintf(buf, sizeof buf, "%c%c%03d", letter, e < 0 ? '-' : '+', e < 0 ? -e : e);
    return buf;
}

struct Spec {
    bool left = false, plus = false, space = false, alt = false, zero = false;
    int width = 0;
    int prec = -1;
    int size = 0; // 'h' = 1, 'l' = 2, 'L' = 3, I64 = 4
    char type = 0;
};

// %e, %f and %g of a double, without the sign or the padding.
std::string format_float_body(double v, const Spec &s, bool &negative) {
    FloatText t = float_text(v);
    negative = t.negative;
    // The separator is LC_NUMERIC's, as msvcrt prints it.
    const char point = numeric_decimal_point();
    int prec = s.prec < 0 ? 6 : s.prec;
    char type = s.type;
    bool upper = type == 'E' || type == 'G';
    bool strip = false;
    if (type == 'g' || type == 'G') {
        // Round to the significant digits first: the exponent that decides
        // between the two styles is the rounded one.
        int p = prec == 0 ? 1 : prec;
        t.digits = round_digits(t.digits, p, t.decpt);
        int mag = t.decpt - 1;
        strip = !s.alt;
        if (mag < -4 || mag >= p) {
            type = 'e';
            prec = p - 1;
        } else {
            type = 'f';
            prec = p - 1 - mag;
        }
    }
    std::string out;
    if (type == 'e' || type == 'E') {
        int decpt = t.decpt;
        std::string digits = round_digits(t.digits, prec + 1, decpt);
        out.push_back(digits[0]);
        if (prec > 0 || s.alt)
            out.push_back(point);
        out.append(digits, 1, (size_t)prec);
        if (strip)
            crop_zeros(out, point);
        out += exponent_text(t.special ? 0 : decpt - 1, upper ? 'E' : 'e');
        return out;
    }
    int decpt = t.decpt;
    std::string digits = round_digits(t.digits, decpt + prec, decpt);
    if (decpt > 0) {
        for (int i = 0; i < decpt; ++i)
            out.push_back(i < (int)digits.size() ? digits[i] : '0');
    } else {
        out.push_back('0');
    }
    if (prec > 0 || s.alt)
        out.push_back(point);
    if (prec > 0) {
        int lead = decpt < 0 ? std::min(-decpt, prec) : 0;
        out.append(lead, '0');
        size_t from = decpt > 0 ? (size_t)decpt : 0;
        for (int i = lead; i < prec; ++i, ++from)
            out.push_back(from < digits.size() ? digits[from] : '0');
    }
    if (strip)
        crop_zeros(out, point);
    return out;
}

// Applies the sign, prefix, width and zero padding.
std::string pad(const std::string &prefix, const std::string &body, const Spec &s, bool zero_ok) {
    int len = int(prefix.size() + body.size());
    if (len >= s.width)
        return prefix + body;
    int fill = s.width - len;
    if (s.left)
        return prefix + body + std::string(fill, ' ');
    if (s.zero && zero_ok)
        return prefix + std::string(fill, '0') + body;
    return std::string(fill, ' ') + prefix + body;
}

std::string format_integer(uint64_t v, bool negative, const Spec &s) {
    int base = 10;
    bool upper = false;
    switch (s.type) {
    case 'o':
        base = 8;
        break;
    case 'x':
        base = 16;
        break;
    case 'X':
    case 'p':
        base = 16;
        upper = true;
        break;
    default:
        break;
    }
    std::string digits;
    const char *set = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    for (uint64_t x = v; x; x /= base)
        digits.insert(digits.begin(), set[x % base]);
    int prec = s.prec;
    if (s.type == 'p')
        prec = 8;
    if (prec < 0)
        prec = 1;
    if ((int)digits.size() < prec)
        digits.insert(0, prec - digits.size(), '0');
    std::string prefix;
    if (s.type == 'd' || s.type == 'i') {
        if (negative)
            prefix = "-";
        else if (s.plus)
            prefix = "+";
        else if (s.space)
            prefix = " ";
    }
    if (s.alt && base == 8 && (digits.empty() || digits[0] != '0'))
        digits.insert(digits.begin(), '0');
    if (s.alt && base == 16 && v && s.type != 'p')
        prefix = upper ? "0X" : "0x";
    return pad(prefix, digits, s, s.prec < 0);
}

// Reads the varargs as the callee would: dwords, with doubles and __int64
// taking two slots.
struct ArgReader {
    uint32_t ap;
    uint32_t u32() {
        uint32_t v = rd32(ap);
        ap += 4;
        return v;
    }
    uint64_t u64() {
        uint64_t v = rd64(ap);
        ap += 8;
        return v;
    }
    double f64() {
        double v = rdf64(ap);
        ap += 8;
        return v;
    }
};

std::string wide_to_text(uint32_t p, int limit) {
    std::string out;
    for (uint32_t i = 0;; ++i) {
        if (limit >= 0 && (int)out.size() >= limit)
            break;
        uint16_t w = rd16(p + 2 * i);
        if (!w)
            break;
        int b = wide_to_mb(w);
        if (b < 0)
            break; // msvcrt stops at a character the locale cannot convert
        out.push_back((char)b);
    }
    return out;
}

} // namespace

std::string format_guest(const char *fmt, uint32_t ap_start) {
    ArgReader ap{ap_start};
    std::string out;
    for (const char *p = fmt; *p; ++p) {
        if (*p != '%') {
            out.push_back(*p);
            continue;
        }
        const char *start = p++;
        Spec s;
        for (;; ++p) {
            if (*p == '-')
                s.left = true;
            else if (*p == '+')
                s.plus = true;
            else if (*p == ' ')
                s.space = true;
            else if (*p == '#')
                s.alt = true;
            else if (*p == '0')
                s.zero = true;
            else
                break;
        }
        if (*p == '*') {
            int w = (int)ap.u32();
            if (w < 0) {
                s.left = true;
                w = -w;
            }
            s.width = w;
            ++p;
        } else {
            while (*p >= '0' && *p <= '9')
                s.width = s.width * 10 + (*p++ - '0');
        }
        if (*p == '.') {
            ++p;
            s.prec = 0;
            if (*p == '*') {
                int pr = (int)ap.u32();
                s.prec = pr < 0 ? -1 : pr;
                ++p;
            } else {
                while (*p >= '0' && *p <= '9')
                    s.prec = s.prec * 10 + (*p++ - '0');
            }
        }
        bool wide = false;
        for (;;) {
            if (*p == 'h') {
                s.size = 1;
                ++p;
            } else if (*p == 'l') {
                s.size = 2;
                wide = true;
                ++p;
            } else if (*p == 'L') {
                s.size = 3;
                ++p;
            } else if (*p == 'w') {
                wide = true;
                ++p;
            } else if (p[0] == 'I' && p[1] == '6' && p[2] == '4') {
                s.size = 4;
                p += 3;
            } else if (p[0] == 'I' && p[1] == '3' && p[2] == '2') {
                s.size = 0;
                p += 3;
            } else if (*p == 'I') {
                s.size = 0; // pointer-sized: 32 bits
                ++p;
            } else {
                break;
            }
        }
        s.type = *p;
        switch (s.type) {
        case 0:
            --p; // a format ending in '%': nothing more to print
            break;
        case '%':
            out.push_back('%');
            break;
        case 'c':
        case 'C': {
            uint32_t v = ap.u32();
            std::string body;
            if (s.type == 'C' || (wide && s.size == 2)) {
                int b = wide_to_mb(v & 0xffff);
                body.push_back((char)(b < 0 ? '?' : b));
            } else {
                body.push_back((char)v);
            }
            Spec t = s;
            t.zero = false;
            out += pad("", body, t, false);
            break;
        }
        case 's':
        case 'S': {
            uint32_t v = ap.u32();
            bool w = s.type == 'S' ? s.size != 1 : (s.size == 2 || wide);
            std::string body;
            if (!v) {
                body = "(null)";
                if (s.prec >= 0 && (int)body.size() > s.prec)
                    body.resize(s.prec);
            } else if (w) {
                body = wide_to_text(v, s.prec);
            } else {
                uint32_t len = gstrlen(v);
                if (s.prec >= 0 && (uint32_t)s.prec < len)
                    len = (uint32_t)s.prec;
                body.assign(gchars(v), len);
            }
            Spec t = s;
            t.zero = false;
            out += pad("", body, t, false);
            break;
        }
        case 'd':
        case 'i': {
            int64_t v;
            if (s.size == 4)
                v = (int64_t)ap.u64();
            else if (s.size == 1)
                v = (int16_t)ap.u32();
            else
                v = (int32_t)ap.u32();
            uint64_t mag = v < 0 ? 0 - (uint64_t)v : (uint64_t)v;
            out += format_integer(mag, v < 0, s);
            break;
        }
        case 'u':
        case 'o':
        case 'x':
        case 'X': {
            uint64_t v;
            if (s.size == 4)
                v = ap.u64();
            else if (s.size == 1)
                v = (uint16_t)ap.u32();
            else
                v = ap.u32();
            out += format_integer(v, false, s);
            break;
        }
        case 'p':
            out += format_integer(ap.u32(), false, s);
            break;
        case 'n': {
            uint32_t dst = ap.u32();
            if (dst) {
                if (s.size == 1)
                    wr16(dst, (uint16_t)out.size());
                else
                    wr32(dst, (uint32_t)out.size());
            }
            break;
        }
        case 'e':
        case 'E':
        case 'f':
        case 'g':
        case 'G': {
            double v = ap.f64();
            bool negative = false;
            std::string body = format_float_body(v, s, negative);
            std::string prefix = negative ? "-" : s.plus ? "+" : s.space ? " " : "";
            out += pad(prefix, body, s, true);
            break;
        }
        default:
            // Not a conversion msvcrt knows: the character itself is printed.
            (void)start;
            out.push_back(s.type);
            break;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Decimal -> binary.
// ---------------------------------------------------------------------------
namespace {

// Builds "0.DIGITSe<exp>" in the host's own decimal separator, so the host
// conversion (correctly rounded on every platform the kit supports) reads it
// whatever locale the host process is in.
std::string host_text(const std::string &canon, bool *negative, bool *nonzero) {
    const char *p = canon.c_str();
    *negative = false;
    if (*p == '-' || *p == '+')
        *negative = *p++ == '-';
    std::string digits;
    long exp10 = 0;
    bool seen_point = false;
    for (; *p && *p != 'e'; ++p) {
        if (*p == '.') {
            seen_point = true;
            continue;
        }
        if (digits.empty() && *p == '0') {
            if (seen_point)
                --exp10;
            continue;
        }
        digits.push_back(*p);
        if (!seen_point)
            ++exp10;
    }
    if (*p == 'e') {
        long e = strtol(p + 1, nullptr, 10);
        if (e > 100000)
            e = 100000;
        if (e < -100000)
            e = -100000;
        exp10 += e;
    }
    *nonzero = !digits.empty();
    if (digits.empty())
        return "0";
    const char *point = localeconv()->decimal_point;
    return std::string("0") + (point && *point ? point : ".") + digits + "e" +
           std::to_string(exp10);
}

} // namespace

double decimal_to_double(const std::string &canon, bool *range) {
    bool negative, nonzero;
    std::string text = host_text(canon, &negative, &nonzero);
    double v = nonzero ? strtod(text.c_str(), nullptr) : 0.0;
    if (range)
        *range = nonzero && (std::isinf(v) || v == 0);
    return negative ? -v : v;
}

float decimal_to_float(const std::string &canon, bool *range) {
    bool negative, nonzero;
    std::string text = host_text(canon, &negative, &nonzero);
    float v = nonzero ? strtof(text.c_str(), nullptr) : 0.0f;
    if (range)
        *range = nonzero && (std::isinf(v) || v == 0);
    return negative ? -v : v;
}

namespace {

// strtod's grammar over a guest string: [space][sign]digits[.digits]
// [(e|E|d|D)[sign]digits]. An exponent marker without digits is not part of
// the number. Returns the characters consumed, 0 for no number.
uint32_t parse_strtod(uint32_t s, std::string &canon) {
    uint32_t p = s;
    while (ctype_bits(rd8(p)) & CT_SPACE)
        ++p;
    canon.clear();
    if (rd8(p) == '+' || rd8(p) == '-')
        canon.push_back((char)rd8(p++));
    bool any = false;
    while (rd8(p) >= '0' && rd8(p) <= '9') {
        canon.push_back((char)rd8(p++));
        any = true;
    }
    if (rd8(p) == (uint8_t)numeric_decimal_point()) {
        ++p;
        canon.push_back('.');
        while (rd8(p) >= '0' && rd8(p) <= '9') {
            canon.push_back((char)rd8(p++));
            any = true;
        }
    }
    if (!any)
        return 0;
    uint8_t e = rd8(p);
    if (e == 'e' || e == 'E' || e == 'd' || e == 'D') {
        uint32_t q = p + 1;
        std::string ex = "e";
        if (rd8(q) == '+' || rd8(q) == '-')
            ex.push_back((char)rd8(q++));
        if (rd8(q) >= '0' && rd8(q) <= '9') {
            while (rd8(q) >= '0' && rd8(q) <= '9')
                ex.push_back((char)rd8(q++));
            canon += ex;
            p = q;
        }
    }
    return p - s;
}

double guest_strtod(uint32_t s, uint32_t *end) {
    std::string canon;
    uint32_t n = parse_strtod(s, canon);
    if (end)
        *end = s + n;
    if (!n)
        return 0.0;
    bool range = false;
    double v = decimal_to_double(canon, &range);
    if (range)
        set_errno(E_RANGE);
    return v;
}

void crt_atof(X86 *c) {
    fpush(c, guest_strtod(arg(c, 0), nullptr));
}
void crt_strtod(X86 *c) {
    uint32_t end = 0;
    double v = guest_strtod(arg(c, 0), &end);
    if (arg(c, 1))
        wr32(arg(c, 1), end);
    fpush(c, v);
}

// ---------------------------------------------------------------------------
// Integers.
// ---------------------------------------------------------------------------
int digit_value(uint8_t ch) {
    if (ch >= '0' && ch <= '9')
        return ch - '0';
    if (ch >= 'a' && ch <= 'z')
        return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'Z')
        return ch - 'A' + 10;
    return 99;
}

// strtol/strtoul: saturating, with errno ERANGE, and an end pointer at the
// start of the string when there are no digits.
uint32_t guest_strtoul(uint32_t s, uint32_t endp, int base, bool is_signed) {
    uint32_t p = s;
    while (ctype_bits(rd8(p)) & CT_SPACE)
        ++p;
    bool neg = false;
    if (rd8(p) == '+' || rd8(p) == '-')
        neg = rd8(p++) == '-';
    if (base < 0 || base == 1 || base > 36) {
        if (endp)
            wr32(endp, s);
        return 0;
    }
    if (base == 0) {
        if (rd8(p) != '0')
            base = 10;
        else if (rd8(p + 1) == 'x' || rd8(p + 1) == 'X')
            base = 16;
        else
            base = 8;
    }
    if (base == 16 && rd8(p) == '0' && (rd8(p + 1) == 'x' || rd8(p + 1) == 'X'))
        p += 2;
    uint64_t v = 0;
    bool any = false, overflow = false;
    for (;; ++p) {
        int d = digit_value(rd8(p));
        if (d >= base)
            break;
        any = true;
        v = v * base + d;
        if (v > 0xffffffffull) {
            overflow = true;
            v = 0xffffffffull;
        }
    }
    if (!any) {
        if (endp)
            wr32(endp, s);
        return 0;
    }
    if (endp)
        wr32(endp, p);
    if (is_signed) {
        if (overflow || (!neg && v > 0x7fffffffull) || (neg && v > 0x80000000ull)) {
            set_errno(E_RANGE);
            return neg ? 0x80000000u : 0x7fffffffu;
        }
        return neg ? (uint32_t)(0 - (uint32_t)v) : (uint32_t)v;
    }
    if (overflow) {
        set_errno(E_RANGE);
        return 0xffffffffu;
    }
    return neg ? (uint32_t)(0 - (uint32_t)v) : (uint32_t)v;
}
void crt_strtol(X86 *c) {
    set_eax(c, guest_strtoul(arg(c, 0), arg(c, 1), (int)arg(c, 2), true));
}
void crt_strtoul(X86 *c) {
    set_eax(c, guest_strtoul(arg(c, 0), arg(c, 1), (int)arg(c, 2), false));
}
// atoi/atol/_atoi64: no overflow detection, the value wraps.
uint64_t guest_atoi64(uint32_t s) {
    uint32_t p = s;
    while (ctype_bits(rd8(p)) & CT_SPACE)
        ++p;
    bool neg = false;
    if (rd8(p) == '+' || rd8(p) == '-')
        neg = rd8(p++) == '-';
    uint64_t v = 0;
    while (rd8(p) >= '0' && rd8(p) <= '9')
        v = v * 10 + (rd8(p++) - '0');
    return neg ? 0 - v : v;
}
void crt_atoi(X86 *c) {
    set_eax(c, (uint32_t)guest_atoi64(arg(c, 0)));
}
void crt_atoi64(X86 *c) {
    set_eax64(c, guest_atoi64(arg(c, 0)));
}
// char *_itoa(int value, char *buf, int radix)
void crt_itoa(X86 *c) {
    int32_t value = (int32_t)arg(c, 0);
    uint32_t buf = arg(c, 1);
    int radix = (int)arg(c, 2);
    std::string out;
    if (radix >= 2 && radix <= 36) {
        bool neg = radix == 10 && value < 0;
        uint32_t v = neg ? 0u - (uint32_t)value : (uint32_t)value;
        do {
            int d = int(v % radix);
            out.insert(out.begin(), (char)(d < 10 ? '0' + d : 'a' + d - 10));
            v /= radix;
        } while (v);
        if (neg)
            out.insert(out.begin(), '-');
    }
    gm_put_str(buf, out.c_str(), 34);
    set_eax(c, buf);
}
void crt_abs(X86 *c) {
    int32_t v = (int32_t)arg(c, 0);
    set_eax(c, v < 0 ? 0u - (uint32_t)v : (uint32_t)v);
}

// ---------------------------------------------------------------------------
// printf family.
// ---------------------------------------------------------------------------
void crt_sprintf(X86 *c) {
    uint32_t buf = arg(c, 0);
    std::string s = format_guest(gchars(arg(c, 1)), c->r[R_ESP] + 12);
    gm_put_str(buf, s.c_str(), (uint32_t)s.size() + 1);
    set_eax(c, (uint32_t)s.size());
}
void crt_vsprintf(X86 *c) {
    uint32_t buf = arg(c, 0);
    std::string s = format_guest(gchars(arg(c, 1)), arg(c, 2));
    gm_put_str(buf, s.c_str(), (uint32_t)s.size() + 1);
    set_eax(c, (uint32_t)s.size());
}
// _snprintf(buf, count, fmt, ...): writes at most count bytes; when the text
// does not fit, no terminator is written and the result is -1. A text of
// exactly count bytes is written whole, unterminated, and returns count.
void crt_snprintf(X86 *c) {
    uint32_t buf = arg(c, 0), count = arg(c, 1);
    std::string s = format_guest(gchars(arg(c, 2)), c->r[R_ESP] + 16);
    uint32_t n = (uint32_t)s.size();
    if (n < count) {
        gm_put_str(buf, s.c_str(), n + 1);
        set_eax(c, n);
    } else {
        if (count && gm_valid(buf, count))
            memcpy(g_mem + buf, s.data(), count);
        set_eax(c, n == count ? n : 0xffffffffu);
    }
}
void crt_printf(X86 *c) {
    std::string s = format_guest(gchars(arg(c, 0)), c->r[R_ESP] + 8);
    uint32_t stdout_file = imports_data_address("msvcrt.dll", "_iob");
    stdio_write(stdout_file ? stdout_file + 32 : 0, s.data(), s.size());
    set_eax(c, (uint32_t)s.size());
}
void crt_fprintf(X86 *c) {
    std::string s = format_guest(gchars(arg(c, 1)), c->r[R_ESP] + 12);
    int64_t n = stdio_write(arg(c, 0), s.data(), s.size());
    set_eax(c, n < 0 ? 0xffffffffu : (uint32_t)s.size());
}

// ---------------------------------------------------------------------------
// scanf family over a guest string.
// ---------------------------------------------------------------------------
struct StringSource : CharSource {
    uint32_t p;
    explicit StringSource(uint32_t s) : p(s) {}
    int get() override {
        uint8_t ch = rd8(p);
        if (!ch)
            return -1;
        ++p;
        return ch;
    }
    void unget(int ch) override {
        if (ch >= 0)
            --p;
    }
};
void crt_sscanf(X86 *c) {
    StringSource src(arg(c, 0));
    set_eax(c, (uint32_t)scan_guest(src, gchars(arg(c, 1)), c->r[R_ESP] + 12));
}

const ImportShim shims[] = {
    {"msvcrt.dll", "atof", ARGC_CDECL, crt_atof},
    {"msvcrt.dll", "strtod", ARGC_CDECL, crt_strtod},
    {"msvcrt.dll", "strtol", ARGC_CDECL, crt_strtol},
    {"msvcrt.dll", "strtoul", ARGC_CDECL, crt_strtoul},
    {"msvcrt.dll", "atoi", ARGC_CDECL, crt_atoi},
    {"msvcrt.dll", "_atoi64", ARGC_CDECL, crt_atoi64},
    {"msvcrt.dll", "_itoa", ARGC_CDECL, crt_itoa},
    {"msvcrt.dll", "abs", ARGC_CDECL, crt_abs},
    {"msvcrt.dll", "sprintf", ARGC_CDECL, crt_sprintf},
    {"msvcrt.dll", "vsprintf", ARGC_CDECL, crt_vsprintf},
    {"msvcrt.dll", "_snprintf", ARGC_CDECL, crt_snprintf},
    {"msvcrt.dll", "printf", ARGC_CDECL, crt_printf},
    {"msvcrt.dll", "fprintf", ARGC_CDECL, crt_fprintf},
    {"msvcrt.dll", "sscanf", ARGC_CDECL, crt_sscanf},
};

} // namespace

// ---------------------------------------------------------------------------
// The scanf engine.
// ---------------------------------------------------------------------------
namespace {

struct Counted {
    CharSource &src;
    uint32_t count = 0;
    int get() {
        int ch = src.get();
        if (ch >= 0)
            ++count;
        return ch;
    }
    void unget(int ch) {
        if (ch >= 0) {
            src.unget(ch);
            --count;
        }
    }
};

bool is_space(int ch) {
    return ch >= 0 && (ctype_bits(ch) & CT_SPACE);
}

void store_int(uint32_t dst, uint64_t v, int size) {
    if (!dst)
        return;
    if (size == 1)
        wr16(dst, (uint16_t)v);
    else if (size == 4)
        wr64(dst, v);
    else
        wr32(dst, (uint32_t)v);
}

} // namespace

// scanf's float scanner: one character of look-ahead, so an exponent marker
// that is not followed by digits is consumed and ignored, as msvcrt does.
bool scan_decimal(CharSource &src, uint32_t width, bool d_exponent, std::string &out,
                  uint32_t &consumed) {
    out.clear();
    consumed = 0;
    auto room = [&]() { return width == 0 || consumed < width; };
    int ch = room() ? src.get() : -1;
    auto take = [&]() {
        out.push_back((char)ch);
        ++consumed;
        ch = room() ? src.get() : -1;
    };
    if (ch == '+' || ch == '-')
        take();
    bool any = false;
    while (ch >= '0' && ch <= '9') {
        take();
        any = true;
    }
    if (ch == (uint8_t)numeric_decimal_point()) {
        out.push_back('.');
        ++consumed;
        ch = room() ? src.get() : -1;
        while (ch >= '0' && ch <= '9') {
            take();
            any = true;
        }
    }
    if (any && (ch == 'e' || ch == 'E' || (d_exponent && (ch == 'd' || ch == 'D')))) {
        std::string ex = "e";
        ++consumed;
        ch = room() ? src.get() : -1;
        if (ch == '+' || ch == '-') {
            ex.push_back((char)ch);
            ++consumed;
            ch = room() ? src.get() : -1;
        }
        bool digits = false;
        while (ch >= '0' && ch <= '9') {
            ex.push_back((char)ch);
            ++consumed;
            digits = true;
            ch = room() ? src.get() : -1;
        }
        if (digits)
            out += ex;
    }
    src.unget(ch);
    return any;
}

int scan_guest(CharSource &raw, const char *fmt, uint32_t ap_start) {
    Counted in{raw};
    ArgReader ap{ap_start};
    int assigned = 0;
    bool any_conversion = false;
    for (const char *f = fmt; *f; ++f) {
        if (is_space((uint8_t)*f)) {
            int ch;
            while (is_space(ch = in.get()))
                ;
            in.unget(ch);
            continue;
        }
        if (*f != '%' || f[1] == '%') {
            if (*f == '%')
                ++f;
            int ch = in.get();
            if (ch != (uint8_t)*f) {
                in.unget(ch);
                return (ch < 0 && !any_conversion) ? -1 : assigned;
            }
            continue;
        }
        ++f;
        bool suppress = false;
        if (*f == '*') {
            suppress = true;
            ++f;
        }
        uint32_t width = 0;
        while (*f >= '0' && *f <= '9')
            width = width * 10 + uint32_t(*f++ - '0');
        int size = 0;
        bool wide = false;
        for (;;) {
            if (*f == 'h') {
                size = 1;
                ++f;
            } else if (*f == 'l') {
                size = 2;
                wide = true;
                ++f;
            } else if (*f == 'L') {
                size = 3;
                ++f;
            } else if (*f == 'w') {
                wide = true;
                ++f;
            } else if (f[0] == 'I' && f[1] == '6' && f[2] == '4') {
                size = 4;
                f += 3;
            } else if (*f == 'F' || *f == 'N') {
                ++f;
            } else {
                break;
            }
        }
        char type = *f;
        if (!type)
            break;
        uint32_t dst = 0;
        if (type == 'n') {
            if (!suppress)
                store_int(ap.u32(), in.count, size);
            continue;
        }
        if (type != 'c' && type != '[') {
            int ch;
            while (is_space(ch = in.get()))
                ;
            if (ch < 0)
                return any_conversion ? assigned : -1;
            in.unget(ch);
        }
        if (!suppress)
            dst = ap.u32();
        switch (type) {
        case 'd':
        case 'i':
        case 'u':
        case 'o':
        case 'x':
        case 'X':
        case 'p': {
            int base = type == 'o' ? 8 : (type == 'x' || type == 'X' || type == 'p') ? 16 : 10;
            uint32_t used = 0;
            auto room = [&]() { return width == 0 || used < width; };
            int ch = in.get();
            ++used;
            bool neg = false;
            if (ch == '+' || ch == '-') {
                neg = ch == '-';
                ch = room() ? in.get() : -1;
                ++used;
            }
            bool any = false;
            if (type == 'i' && ch == '0') {
                any = true;
                ch = room() ? in.get() : -1;
                ++used;
                if (ch == 'x' || ch == 'X') {
                    base = 16;
                    any = false;
                    ch = room() ? in.get() : -1;
                    ++used;
                } else {
                    base = 8;
                }
            } else if (base == 16 && ch == '0') {
                any = true;
                ch = room() ? in.get() : -1;
                ++used;
                if (ch == 'x' || ch == 'X') {
                    any = false;
                    ch = room() ? in.get() : -1;
                    ++used;
                }
            }
            uint64_t v = 0;
            while (ch >= 0 && digit_value((uint8_t)ch) < base) {
                v = v * base + digit_value((uint8_t)ch);
                any = true;
                ch = room() ? in.get() : -1;
                ++used;
            }
            in.unget(ch);
            if (!any)
                return assigned;
            any_conversion = true;
            if (neg)
                v = 0 - v;
            if (!suppress) {
                store_int(dst, v, type == 'p' ? 0 : size);
                ++assigned;
            }
            break;
        }
        case 'e':
        case 'E':
        case 'f':
        case 'g':
        case 'G': {
            std::string canon;
            uint32_t used = 0;
            if (!scan_decimal(in.src, width, false, canon, used)) {
                in.count += used;
                return assigned;
            }
            in.count += used;
            any_conversion = true;
            if (!suppress) {
                if (size == 2 || size == 3)
                    wrf64(dst, decimal_to_double(canon, nullptr));
                else
                    wrf32(dst, decimal_to_float(canon, nullptr));
                ++assigned;
            }
            break;
        }
        case 's':
        case 'c':
        case '[': {
            bool set[256] = {};
            bool negate = false;
            if (type == '[') {
                const char *q = f + 1;
                if (*q == '^') {
                    negate = true;
                    ++q;
                }
                if (*q == ']') {
                    set[(uint8_t)']'] = true;
                    ++q;
                }
                for (; *q && *q != ']'; ++q) {
                    if (q[1] == '-' && q[2] && q[2] != ']') {
                        uint8_t a = (uint8_t)q[0], b = (uint8_t)q[2];
                        if (a > b)
                            std::swap(a, b);
                        for (int k = a; k <= b; ++k)
                            set[k] = true;
                        q += 2;
                    } else {
                        set[(uint8_t)*q] = true;
                    }
                }
                f = *q ? q : q - 1;
            }
            uint32_t limit = width ? width : (type == 'c' ? 1 : 0xffffffffu);
            uint32_t n = 0;
            int ch = 0;
            while (n < limit) {
                ch = in.get();
                if (ch < 0)
                    break;
                bool ok = type == 'c' ? true : type == 's' ? !is_space(ch) : (set[ch] != negate);
                if (!ok) {
                    in.unget(ch);
                    break;
                }
                if (dst) {
                    if (wide)
                        wr16(dst + 2 * n, (uint16_t)mb_to_wide((uint8_t)ch));
                    else
                        wr8(dst + n, (uint8_t)ch);
                }
                ++n;
            }
            if (!n)
                return (ch < 0 && !any_conversion) ? -1 : assigned;
            any_conversion = true;
            if (dst) {
                if (type != 'c') {
                    if (wide)
                        wr16(dst + 2 * n, 0);
                    else
                        wr8(dst + n, 0);
                }
                ++assigned;
            }
            break;
        }
        default:
            return assigned;
        }
    }
    return assigned;
}

void register_format() {
    imports_register(shims, sizeof shims / sizeof shims[0]);
}

} // namespace crt
