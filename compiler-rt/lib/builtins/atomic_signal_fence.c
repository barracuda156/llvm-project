//===-- atomic_signal_fence.c ---------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements atomic_signal_fence from C11's stdatomic.h.
//
//===----------------------------------------------------------------------===//

#ifndef __has_include
#define __has_include(inc) 0
#endif

#if __has_include(<stdatomic.h>)

#include <stdatomic.h>
#undef atomic_signal_fence
void atomic_signal_fence(memory_order order) {
#if defined(__GNUC__) && !defined(__clang__)
  __atomic_signal_fence(order);
#else
  __c11_atomic_signal_fence(order);
#endif
}

#endif
