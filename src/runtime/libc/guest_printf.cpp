// SecondSonPC — printf formatting over the guest's System V va_list (R10 sysrt). The conversions follow FreeBSD's
// vfprintf (lib/libc/stdio/vfprintf.c), the console's libc family [A: the PS4 libc is a FreeBSD/Dinkumware mix; the
// %p form and the zero padding of %s/%c are FreeBSD's, H for Sony's own]. Integers, characters, strings and pointers
// are formatted here (the guest's long is 64-bit, the host's 32); finite floating-point values by the host CRT with
// the same flags, width and precision (UCRT is C99-exact for f e g a with a precision), inf/nan here.
#include "runtime/libc/guest_printf.h"

#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string_view>

#include "base/log.h"

namespace SecondSon::Runtime::Libc {

namespace {

constexpr int kMaxField = 1 << 24; // a width or precision above this is refused (the call fails)

class Args {
public:
    explicit Args(SysvVaList* l) : l_(l) {}

    u64 Gp() {
        u64 v;
        if (l_->gp_offset < 48) {
            std::memcpy(&v, l_->reg_save_area + l_->gp_offset, 8);
            l_->gp_offset += 8;
        } else {
            std::memcpy(&v, l_->overflow_arg_area, 8);
            l_->overflow_arg_area += 8;
        }
        return v;
    }

    double Fp() {
        double v;
        if (l_->fp_offset < 176) {
            std::memcpy(&v, l_->reg_save_area + l_->fp_offset, 8);
            l_->fp_offset += 16;
        } else {
            std::memcpy(&v, l_->overflow_arg_area, 8);
            l_->overflow_arg_area += 8;
        }
        return v;
    }

    /// A long double argument: the x87 80-bit format, always in memory, 16-byte aligned and sized. The host has no
    /// such type (long double is double for MSVC): it is rounded to double.
    double X87() {
        const auto a = (reinterpret_cast<u64>(l_->overflow_arg_area) + 15) & ~u64{15};
        const auto* p = reinterpret_cast<const u8*>(a);
        l_->overflow_arg_area = reinterpret_cast<u8*>(a + 16);
        u64 mantissa;
        u16 se;
        std::memcpy(&mantissa, p, 8);
        std::memcpy(&se, p + 8, 2);
        const bool negative = (se & 0x8000) != 0;
        const int exponent = se & 0x7fff;
        double v;
        if (exponent == 0x7fff) {
            v = (mantissa << 1) == 0 ? INFINITY : NAN;
        } else if (mantissa == 0) {
            v = 0.0;
        } else {
            v = std::ldexp(static_cast<double>(mantissa), (exponent == 0 ? 1 : exponent) - 16383 - 63);
        }
        return negative ? -v : v;
    }

private:
    SysvVaList* l_;
};

enum class Len { None, HH, H, L, LL, J, Z, T, BigL };

struct Spec {
    bool left = false, plus = false, space = false, alt = false, zero = false;
    int width = 0;
    int prec = -1;
    Len len = Len::None;
    char conv = 0;
};

void Field(std::string& out, const Spec& s, std::string_view prefix, std::string_view body, bool zero_pad_ok) {
    const size_t total = prefix.size() + body.size();
    const size_t pad = static_cast<size_t>(s.width) > total ? static_cast<size_t>(s.width) - total : 0;
    if (s.left) {
        out.append(prefix).append(body).append(pad, ' ');
    } else if (s.zero && zero_pad_ok) {
        out.append(prefix).append(pad, '0').append(body);
    } else {
        out.append(pad, ' ').append(prefix).append(body);
    }
}

std::atomic<u32> g_warned{0}; // unsupported directives logged (a few, not every call)

void Unsupported(std::string& out, std::string_view directive, const char* why) {
    out.append(directive);
    if (g_warned.fetch_add(1, std::memory_order_relaxed) < 16) {
        SSPC_LOG(App, Warning, "guest printf: \"{}\" not supported ({}), copied as text", directive, why);
    }
}

s64 SignedArg(Args& a, Len len) {
    const u64 v = a.Gp();
    switch (len) {
    case Len::HH:
        return static_cast<s8>(v);
    case Len::H:
        return static_cast<s16>(v);
    case Len::None:
        return static_cast<s32>(v);
    default: // l ll q j z t: 64-bit (LP64)
        return static_cast<s64>(v);
    }
}

u64 UnsignedArg(Args& a, Len len) {
    const u64 v = a.Gp();
    switch (len) {
    case Len::HH:
        return static_cast<u8>(v);
    case Len::H:
        return static_cast<u16>(v);
    case Len::None:
        return static_cast<u32>(v);
    default:
        return v;
    }
}

void Integer(std::string& out, const Spec& s, u64 v, bool negative, bool is_signed) {
    int base = 10;
    const char* digits = "0123456789abcdef";
    switch (s.conv) {
    case 'o':
        base = 8;
        break;
    case 'x':
    case 'p':
        base = 16;
        break;
    case 'X':
        base = 16;
        digits = "0123456789ABCDEF";
        break;
    default:
        break;
    }
    char buf[64];
    int n = 0;
    if (!(s.prec == 0 && v == 0)) {
        u64 x = v;
        do {
            buf[sizeof buf - 1 - n++] = digits[x % static_cast<u64>(base)];
            x /= static_cast<u64>(base);
        } while (x != 0);
    }
    std::string body;
    if (s.prec > n) {
        body.append(static_cast<size_t>(s.prec - n), '0');
    }
    body.append(buf + sizeof buf - n, static_cast<size_t>(n));
    if (s.conv == 'o' && s.alt && (body.empty() || body[0] != '0')) {
        body.insert(body.begin(), '0');
    }
    std::string_view prefix;
    if (is_signed) {
        prefix = negative ? "-" : s.plus ? "+" : s.space ? " " : "";
    } else if (s.conv == 'p') {
        prefix = "0x";
    } else if (s.alt && v != 0 && (s.conv == 'x' || s.conv == 'X')) {
        prefix = s.conv == 'x' ? "0x" : "0X";
    }
    Field(out, s, prefix, body, s.prec < 0);
}

void Float(std::string& out, const Spec& s, double v) {
    const bool upper = s.conv == 'F' || s.conv == 'E' || s.conv == 'G' || s.conv == 'A';
    if (std::isnan(v) || std::isinf(v)) {
        std::string_view prefix;
        if (std::isinf(v)) {
            prefix = std::signbit(v) ? "-" : s.plus ? "+" : s.space ? " " : "";
        }
        const char* body = std::isnan(v) ? (upper ? "NAN" : "nan") : (upper ? "INF" : "inf");
        Field(out, s, prefix, body, false);
        return;
    }
    int prec = s.prec;
    if ((s.conv == 'a' || s.conv == 'A') && prec < 0) {
        // FreeBSD prints the shortest exact hex fraction; UCRT always 13 digits without a precision
        u64 frac = std::bit_cast<u64>(v) & ((u64{1} << 52) - 1);
        prec = 13;
        while (prec > 0 && (frac & 0xf) == 0) {
            frac >>= 4;
            --prec;
        }
    }
    char spec[32];
    int k = 0;
    spec[k++] = '%';
    if (s.left) {
        spec[k++] = '-';
    }
    if (s.plus) {
        spec[k++] = '+';
    }
    if (s.space) {
        spec[k++] = ' ';
    }
    if (s.alt) {
        spec[k++] = '#';
    }
    if (s.zero) {
        spec[k++] = '0';
    }
    spec[k++] = '*';
    if (prec >= 0) {
        spec[k++] = '.';
        spec[k++] = '*';
    }
    spec[k++] = s.conv;
    spec[k] = '\0';
    const int need = prec >= 0 ? std::snprintf(nullptr, 0, spec, s.width, prec, v)
                               : std::snprintf(nullptr, 0, spec, s.width, v);
    if (need <= 0) {
        return;
    }
    const size_t at = out.size();
    out.resize(at + static_cast<size_t>(need) + 1);
    if (prec >= 0) {
        std::snprintf(out.data() + at, static_cast<size_t>(need) + 1, spec, s.width, prec, v);
    } else {
        std::snprintf(out.data() + at, static_cast<size_t>(need) + 1, spec, s.width, v);
    }
    out.resize(at + static_cast<size_t>(need));
}

/// Reads a decimal number (width / precision); false if it is above kMaxField.
bool Number(const char*& p, int& value) {
    s64 v = 0;
    while (*p >= '0' && *p <= '9') {
        v = v * 10 + (*p++ - '0');
        if (v > kMaxField) {
            return false;
        }
    }
    value = static_cast<int>(v);
    return true;
}

} // namespace

s32 FormatGuest(std::string& out, const char* format, SysvVaList* ap) {
    const size_t start = out.size();
    if (format == nullptr) {
        return 0;
    }
    Args args(ap);
    const char* p = format;
    while (*p != '\0') {
        if (*p != '%') {
            const char* q = std::strchr(p, '%');
            const size_t n = q != nullptr ? static_cast<size_t>(q - p) : std::strlen(p);
            out.append(p, n);
            p += n;
            continue;
        }
        const char* directive = p++;
        Spec s;
        for (;; ++p) {
            if (*p == '-') {
                s.left = true;
            } else if (*p == '+') {
                s.plus = true;
            } else if (*p == ' ') {
                s.space = true;
            } else if (*p == '#') {
                s.alt = true;
            } else if (*p == '0') {
                s.zero = true;
            } else if (*p == '\'') {
                // thousands grouping: none in the C locale
            } else {
                break;
            }
        }
        if (*p == '*') {
            ++p;
            const s32 w = static_cast<s32>(args.Gp());
            if (w < 0) {
                s.left = true;
                s.width = w == INT32_MIN ? kMaxField + 1 : -w;
            } else {
                s.width = w;
            }
        } else {
            const char* digits = p;
            if (!Number(p, s.width)) {
                SSPC_LOG(App, Error, "guest printf: field width too large in \"{}\"", format);
                return -1;
            }
            if (*p == '$' && p != digits) {
                // the whole directive, to its conversion letter (no argument is taken: their order is unknown)
                ++p;
                while (*p != '\0' && std::strchr("-+ #0'*.$0123456789hlqjztL", *p) != nullptr) {
                    ++p;
                }
                if (*p != '\0') {
                    ++p;
                }
                Unsupported(out, std::string_view(directive, static_cast<size_t>(p - directive)),
                            "positional argument");
                continue;
            }
        }
        if (*p == '.') {
            ++p;
            if (*p == '*') {
                ++p;
                const s32 v = static_cast<s32>(args.Gp());
                s.prec = v < 0 ? -1 : v;
            } else if (!Number(p, s.prec)) {
                SSPC_LOG(App, Error, "guest printf: precision too large in \"{}\"", format);
                return -1;
            }
        }
        if (s.width > kMaxField || s.prec > kMaxField) {
            SSPC_LOG(App, Error, "guest printf: field too large in \"{}\"", format);
            return -1;
        }
        switch (*p) {
        case 'h':
            s.len = *++p == 'h' ? (++p, Len::HH) : Len::H;
            break;
        case 'l':
            s.len = *++p == 'l' ? (++p, Len::LL) : Len::L;
            break;
        case 'q':
            ++p;
            s.len = Len::LL;
            break;
        case 'j':
            ++p;
            s.len = Len::J;
            break;
        case 'z':
            ++p;
            s.len = Len::Z;
            break;
        case 't':
            ++p;
            s.len = Len::T;
            break;
        case 'L':
            ++p;
            s.len = Len::BigL;
            break;
        default:
            break;
        }
        s.conv = *p;
        if (s.conv == '\0') {
            Unsupported(out, std::string_view(directive), "incomplete directive");
            break;
        }
        ++p;
        const std::string_view text(directive, static_cast<size_t>(p - directive));
        switch (s.conv) {
        case 'd':
        case 'i': {
            const s64 v = SignedArg(args, s.len);
            Integer(out, s, v < 0 ? 0 - static_cast<u64>(v) : static_cast<u64>(v), v < 0, true);
            break;
        }
        case 'D':
            s.conv = 'd';
            {
                const s64 v = SignedArg(args, Len::L);
                Integer(out, s, v < 0 ? 0 - static_cast<u64>(v) : static_cast<u64>(v), v < 0, true);
            }
            break;
        case 'u':
        case 'o':
        case 'x':
        case 'X':
            Integer(out, s, UnsignedArg(args, s.len), false, false);
            break;
        case 'U':
        case 'O':
            s.conv = s.conv == 'U' ? 'u' : 'o';
            Integer(out, s, UnsignedArg(args, Len::L), false, false);
            break;
        case 'p':
            Integer(out, s, args.Gp(), false, false);
            break;
        case 'c':
            if (s.len == Len::L) {
                args.Gp();
                Unsupported(out, text, "wide character");
                break;
            }
            {
                const char c = static_cast<char>(args.Gp());
                Field(out, s, "", std::string_view(&c, 1), true);
            }
            break;
        case 's': {
            if (s.len == Len::L) {
                args.Gp();
                Unsupported(out, text, "wide string");
                break;
            }
            const auto* str = reinterpret_cast<const char*>(args.Gp());
            if (str == nullptr) {
                str = "(null)";
            }
            const size_t n = s.prec >= 0 ? strnlen(str, static_cast<size_t>(s.prec)) : std::strlen(str);
            Field(out, s, "", std::string_view(str, n), true);
            break;
        }
        case 'n': {
            auto* dst = reinterpret_cast<u8*>(args.Gp());
            const s64 count = static_cast<s64>(out.size() - start);
            if (dst == nullptr) {
                break;
            }
            switch (s.len) {
            case Len::HH: {
                const s8 v = static_cast<s8>(count);
                std::memcpy(dst, &v, 1);
                break;
            }
            case Len::H: {
                const s16 v = static_cast<s16>(count);
                std::memcpy(dst, &v, 2);
                break;
            }
            case Len::None: {
                const s32 v = static_cast<s32>(count);
                std::memcpy(dst, &v, 4);
                break;
            }
            default:
                std::memcpy(dst, &count, 8);
                break;
            }
            break;
        }
        case '%':
            out.push_back('%');
            break;
        case 'f':
        case 'F':
        case 'e':
        case 'E':
        case 'g':
        case 'G':
        case 'a':
        case 'A':
            Float(out, s, s.len == Len::BigL ? args.X87() : args.Fp());
            break;
        default:
            Unsupported(out, text, "unknown conversion");
            break;
        }
    }
    return static_cast<s32>(out.size() - start);
}

} // namespace SecondSon::Runtime::Libc
