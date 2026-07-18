/**
 * @file compat/ps3_toolchain_compat.h
 * @brief Force-included first on every PS3 TU (Makefile_PS3 `-include`).
 *
 * ps3dev ships GCC 7.2 + newlib whose libstdc++ was configured with C99 math
 * TR1 DISABLED (newlib lacks the long-double variants log2l/logbl/nexttowardl,
 * so enabling _GLIBCXX_USE_C99_MATH_TR1 makes <cmath> fail instead). As a
 * result <cmath> never puts log2/exp2/round/trunc/asinh/acosh/atanh into
 * namespace std.
 *
 * The engine's bundled glm does `using ::std::round;` (etc.) and then passes
 * the name to its `functor1<T,...>::call(round, v)` helper, which needs a
 * proper float+double OVERLOAD SET (exactly what desktop libstdc++ provides).
 * A bare `using ::round;` (single double function) makes glm see an
 * "unresolved overloaded function type". So we define the float and double
 * overloads explicitly, forwarding to newlib's C functions, BEFORE any glm
 * header is seen. Scoped to the PS3 addon build so nothing else is affected.
 */
#pragma once

// C++ only — this shim adds overloads into namespace std and pulls in
// <type_traits>. The Makefile force-includes it for every TU, including the
// bundled Lua .c files compiled by ppu-gcc, where those constructs are invalid
// (and unneeded — C code calls the global math functions directly).
#if defined(POLYPHASE_PLATFORM_ADDON) && defined(PLATFORM_PS3) && defined(__cplusplus)
#include <math.h>
#include <type_traits>
namespace std
{
    // float + double overloads — glm passes these by name to its
    // functor1<T,...>::call(fn, v) helper, so it needs an exact float match.
    inline float  log2(float x)  { return ::log2f(x); }
    inline double log2(double x) { return ::log2(x); }
    inline float  exp2(float x)  { return ::exp2f(x); }
    inline double exp2(double x) { return ::exp2(x); }
    inline float  round(float x) { return ::roundf(x); }
    inline double round(double x){ return ::round(x); }
    inline float  trunc(float x) { return ::truncf(x); }
    inline double trunc(double x){ return ::trunc(x); }
    inline float  asinh(float x) { return ::asinhf(x); }
    inline double asinh(double x){ return ::asinh(x); }
    inline float  acosh(float x) { return ::acoshf(x); }
    inline double acosh(double x){ return ::acosh(x); }
    inline float  atanh(float x) { return ::atanhf(x); }
    inline double atanh(double x){ return ::atanh(x); }

    // Integral overloads — C++11 <cmath> maps integer args to the double form.
    // Without these, calls like log2(someUnsigned) are ambiguous between the
    // float and double overloads above (both need a user conversion).
    template<class T> inline typename std::enable_if<std::is_integral<T>::value, double>::type log2(T x)  { return ::log2 ((double)x); }
    template<class T> inline typename std::enable_if<std::is_integral<T>::value, double>::type exp2(T x)  { return ::exp2 ((double)x); }
    template<class T> inline typename std::enable_if<std::is_integral<T>::value, double>::type round(T x) { return ::round((double)x); }
    template<class T> inline typename std::enable_if<std::is_integral<T>::value, double>::type trunc(T x) { return ::trunc((double)x); }
    template<class T> inline typename std::enable_if<std::is_integral<T>::value, double>::type asinh(T x) { return ::asinh((double)x); }
    template<class T> inline typename std::enable_if<std::is_integral<T>::value, double>::type acosh(T x) { return ::acosh((double)x); }
    template<class T> inline typename std::enable_if<std::is_integral<T>::value, double>::type atanh(T x) { return ::atanh((double)x); }
}
#endif
