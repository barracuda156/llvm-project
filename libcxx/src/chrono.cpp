//===------------------------- chrono.cpp ---------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "chrono"
#include "cerrno"        // errno
#include "system_error"  // __throw_system_error
#include <time.h>        // clock_gettime and CLOCK_{MONOTONIC,REALTIME,MONOTONIC_RAW}
#include "include/apple_availability.h"

#if __has_include(<unistd.h>)
#include <unistd.h>
#endif

#if !defined(__APPLE__) && _POSIX_TIMERS > 0
#define _LIBCPP_USE_CLOCK_GETTIME
#endif

#if defined(_LIBCPP_WIN32API)
#  define WIN32_LEAN_AND_MEAN
#  define VC_EXTRA_LEAN
#  include <windows.h>
#  if _WIN32_WINNT >= _WIN32_WINNT_WIN8
#    include <winapifamily.h>
#  endif
#else
#  if !defined(CLOCK_REALTIME)
#    include <sys/time.h>        // for gettimeofday and timeval
#  endif // !defined(CLOCK_REALTIME)
#endif // defined(_LIBCPP_WIN32API)

 // restore previous libcxx fallback
#if __ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__ < 101200
#include <mach/mach_time.h>
#endif

#if defined(__ELF__) && defined(_LIBCPP_LINK_RT_LIB)
#  pragma comment(lib, "rt")
#endif

_LIBCPP_BEGIN_NAMESPACE_STD

namespace chrono
{

// system_clock

const bool system_clock::is_steady;

system_clock::time_point
system_clock::now() _NOEXCEPT
{
#if defined(_LIBCPP_WIN32API)
  // FILETIME is in 100ns units
  using filetime_duration =
      _VSTD::chrono::duration<__int64,
                              _VSTD::ratio_multiply<_VSTD::ratio<100, 1>,
                                                    nanoseconds::period>>;

  // The Windows epoch is Jan 1 1601, the Unix epoch Jan 1 1970.
  static _LIBCPP_CONSTEXPR const seconds nt_to_unix_epoch{11644473600};

  FILETIME ft;
#if _WIN32_WINNT >= _WIN32_WINNT_WIN8
#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
  GetSystemTimePreciseAsFileTime(&ft);
#else
  GetSystemTimeAsFileTime(&ft);
#endif
#else
  GetSystemTimeAsFileTime(&ft);
#endif

  filetime_duration d{(static_cast<__int64>(ft.dwHighDateTime) << 32) |
                       static_cast<__int64>(ft.dwLowDateTime)};
  return time_point(duration_cast<duration>(d - nt_to_unix_epoch));
#else
#if defined(CLOCK_REALTIME)
  struct timespec tp;
  if (0 != clock_gettime(CLOCK_REALTIME, &tp))
    __throw_system_error(errno, "clock_gettime(CLOCK_REALTIME) failed");
  return time_point(seconds(tp.tv_sec) + microseconds(tp.tv_nsec / 1000));
#else
    timeval tv;
    gettimeofday(&tv, 0);
    return time_point(seconds(tv.tv_sec) + microseconds(tv.tv_usec));
#endif // CLOCK_REALTIME
#endif
}

time_t
system_clock::to_time_t(const time_point& t) _NOEXCEPT
{
    return time_t(duration_cast<seconds>(t.time_since_epoch()).count());
}

system_clock::time_point
system_clock::from_time_t(time_t t) _NOEXCEPT
{
    return system_clock::time_point(seconds(t));
}

#ifndef _LIBCPP_HAS_NO_MONOTONIC_CLOCK
// steady_clock
//
// Warning:  If this is not truly steady, then it is non-conforming.  It is
//  better for it to not exist and have the rest of libc++ use system_clock
//  instead.

const bool steady_clock::is_steady;

#if defined(__APPLE__)

#if __ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__ >= 101200

#if !defined(CLOCK_MONOTONIC_RAW)
#  error "Building libc++ on Apple platforms requires CLOCK_MONOTONIC_RAW"
#endif

// On Apple platforms, only CLOCK_UPTIME_RAW, CLOCK_MONOTONIC_RAW or
// mach_absolute_time are able to time functions in the nanosecond range.
// Furthermore, only CLOCK_MONOTONIC_RAW is truly monotonic, because it
// also counts cycles when the system is asleep. Thus, it is the only
// acceptable implementation of steady_clock.
steady_clock::time_point
steady_clock::now() _NOEXCEPT
{
    struct timespec tp;
    if (0 != clock_gettime(CLOCK_MONOTONIC_RAW, &tp))
        __throw_system_error(errno, "clock_gettime(CLOCK_MONOTONIC_RAW) failed");
    return time_point(seconds(tp.tv_sec) + nanoseconds(tp.tv_nsec));
}
#else
 // restore previous libcxx fallback
 //
 //   mach_absolute_time() * MachInfo.numer / MachInfo.denom is the number of
 //   nanoseconds since the computer booted up.  MachInfo.numer and MachInfo.denom
 //   are run time constants supplied by the OS.  This clock has no relationship
 //   to the Gregorian calendar.  It's main use is as a high resolution timer.

 // MachInfo.numer / MachInfo.denom is often 1 on the latest equipment.  Specialize
 //   for that case as an optimization.

 static
 steady_clock::rep
 steady_simplified()
 {
     return static_cast<steady_clock::rep>(mach_absolute_time());
 }

 static
 double
 compute_steady_factor()
 {
     mach_timebase_info_data_t MachInfo;
     mach_timebase_info(&MachInfo);
     return static_cast<double>(MachInfo.numer) / MachInfo.denom;
 }

 static
 steady_clock::rep
 steady_full()
 {
     static const double factor = compute_steady_factor();
     return static_cast<steady_clock::rep>(mach_absolute_time() * factor);
 }

 typedef steady_clock::rep (*FP)();

 static
 FP
 init_steady_clock()
 {
     mach_timebase_info_data_t MachInfo;
     mach_timebase_info(&MachInfo);
     if (MachInfo.numer == MachInfo.denom)
         return &steady_simplified;
     return &steady_full;
 }

 steady_clock::time_point
 steady_clock::now() _NOEXCEPT
 {
     static FP fp = init_steady_clock();
     return time_point(duration(fp()));
 }
#endif // restore previous libcxx fallback

#elif defined(_LIBCPP_WIN32API)

// https://msdn.microsoft.com/en-us/library/windows/desktop/ms644905(v=vs.85).aspx says:
//    If the function fails, the return value is zero. <snip>
//    On systems that run Windows XP or later, the function will always succeed
//      and will thus never return zero.

static LARGE_INTEGER
__QueryPerformanceFrequency()
{
	LARGE_INTEGER val;
	(void) QueryPerformanceFrequency(&val);
	return val;
}

steady_clock::time_point
steady_clock::now() _NOEXCEPT
{
  static const LARGE_INTEGER freq = __QueryPerformanceFrequency();

  LARGE_INTEGER counter;
  (void) QueryPerformanceCounter(&counter);
  return time_point(duration(counter.QuadPart * nano::den / freq.QuadPart));
}

#elif defined(CLOCK_MONOTONIC)

steady_clock::time_point
steady_clock::now() _NOEXCEPT
{
    struct timespec tp;
    if (0 != clock_gettime(CLOCK_MONOTONIC, &tp))
        __throw_system_error(errno, "clock_gettime(CLOCK_MONOTONIC) failed");
    return time_point(seconds(tp.tv_sec) + nanoseconds(tp.tv_nsec));
}

#else
#  error "Monotonic clock not implemented"
#endif

#endif // !_LIBCPP_HAS_NO_MONOTONIC_CLOCK

}

_LIBCPP_END_NAMESPACE_STD
