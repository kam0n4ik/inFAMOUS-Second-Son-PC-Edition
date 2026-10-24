// SecondSonPC — printf formatting for the guest's libc functions (R10 sysrt, docs/R10_SYSRT.md §4).
#pragma once

#include <string>

#include "base/abi.h"

namespace SecondSon::Runtime::Libc {

/// The guest's va_list: System V AMD64 __va_list_tag (a va_list argument is a pointer to one). Not MSVC's va_list.
struct SysvVaList {
    u32 gp_offset;           // next integer register slot in reg_save_area (0..48 step 8; 48: none left)
    u32 fp_offset;           // next vector register slot (48..176 step 16; 176: none left)
    u8* overflow_arg_area;   // arguments passed on the stack
    u8* reg_save_area;       // rdi, rsi, rdx, rcx, r8, r9, then xmm0..xmm7
};
static_assert(sizeof(SysvVaList) == 24);

/// Formats `format` with the arguments of `ap` (advanced as va_arg does) the way the console's libc does — LP64
/// (long is 64-bit), FreeBSD's conversions: d i u o x X D U O c s p n % f F e E g G a A, flags - + space # 0 ',
/// width and precision (digits or *), lengths hh h l ll q j z t L. %p prints like %#lx (0x0 for null); %s of null
/// prints "(null)"; inf/nan as "inf"/"nan" (upper case for F E G A). Unsupported (positional %n$, wide %lc/%ls, an
/// unknown conversion): the directive is copied as text and logged once. Appends to *out; returns the characters
/// produced.
s32 FormatGuest(std::string& out, const char* format, SysvVaList* ap);

} // namespace SecondSon::Runtime::Libc
