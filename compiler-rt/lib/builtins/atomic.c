//===-- atomic.c - Implement support functions for atomic operations.------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//  atomic.c defines a set of functions for performing atomic accesses on
//  arbitrary-sized memory locations.  This design uses locks that should
//  be fast in the uncontended case, for two reasons:
//
//  1) This code must work with C programs that do not link to anything
//     (including pthreads) and so it should not depend on any pthread
//     functions.
//  2) Atomic operations, rather than explicit mutexes, are most commonly used
//     on code where contended operations are rate.
//
//  To avoid needing a per-object lock, this code allocates an array of
//  locks and hashes the object pointers to find the one that it should use.
//  For operations that must be atomic on two locations, the lower lock is
//  always acquired first, to avoid deadlock.
//
//===----------------------------------------------------------------------===//

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "assembly.h"

// Clang objects if you redefine a builtin.  This little hack allows us to
// define a function with the same name as an intrinsic.
#pragma redefine_extname __atomic_load_c SYMBOL_NAME(__atomic_load)
#pragma redefine_extname __atomic_store_c SYMBOL_NAME(__atomic_store)
#pragma redefine_extname __atomic_exchange_c SYMBOL_NAME(__atomic_exchange)
#pragma redefine_extname __atomic_compare_exchange_c SYMBOL_NAME(              \
    __atomic_compare_exchange)

/// Number of locks.  This allocates one page on 32-bit platforms, two on
/// 64-bit.  This can be specified externally if a different trade between
/// memory usage and contention probability is required for a given platform.
#ifndef SPINLOCK_COUNT
#define SPINLOCK_COUNT (1 << 10)
#endif
static const long SPINLOCK_MASK = SPINLOCK_COUNT - 1;

////////////////////////////////////////////////////////////////////////////////
// Platform-specific lock implementation.  Falls back to spinlocks if none is
// defined.  Each platform should define the Lock type, and corresponding
// lock() and unlock() functions.
////////////////////////////////////////////////////////////////////////////////
#ifdef __FreeBSD__
#include <errno.h>
// clang-format off
#include <sys/types.h>
#include <machine/atomic.h>
#include <sys/umtx.h>
// clang-format on
typedef struct _usem Lock;
__inline static void unlock(Lock *l) {
  __c11_atomic_store((_Atomic(uint32_t) *)&l->_count, 1, __ATOMIC_RELEASE);
  __c11_atomic_thread_fence(__ATOMIC_SEQ_CST);
  if (l->_has_waiters)
    _umtx_op(l, UMTX_OP_SEM_WAKE, 1, 0, 0);
}
__inline static void lock(Lock *l) {
  uint32_t old = 1;
  while (!__c11_atomic_compare_exchange_weak((_Atomic(uint32_t) *)&l->_count,
                                             &old, 0, __ATOMIC_ACQUIRE,
                                             __ATOMIC_RELAXED)) {
    _umtx_op(l, UMTX_OP_SEM_WAIT, 0, 0, 0);
    old = 1;
  }
}
/// locks for atomic operations
static Lock locks[SPINLOCK_COUNT] = {[0 ... SPINLOCK_COUNT - 1] = {0, 1, 0}};

#elif defined(__APPLE__)
#include <libkern/OSAtomic.h>
typedef OSSpinLock Lock;
__inline static void unlock(Lock *l) { OSSpinLockUnlock(l); }
/// Locks a lock.  In the current implementation, this is potentially
/// unbounded in the contended case.
__inline static void lock(Lock *l) { OSSpinLockLock(l); }
static Lock locks[SPINLOCK_COUNT]; // initialized to OS_SPINLOCK_INIT which is 0

#else
typedef _Atomic(uintptr_t) Lock;
/// Unlock a lock.  This is a release operation.
__inline static void unlock(Lock *l) {
  __c11_atomic_store(l, 0, __ATOMIC_RELEASE);
}
/// Locks a lock.  In the current implementation, this is potentially
/// unbounded in the contended case.
__inline static void lock(Lock *l) {
  uintptr_t old = 0;
  while (!__c11_atomic_compare_exchange_weak(l, &old, 1, __ATOMIC_ACQUIRE,
                                             __ATOMIC_RELAXED))
    old = 0;
}
/// locks for atomic operations
static Lock locks[SPINLOCK_COUNT];
#endif

/// Returns a lock to use for a given pointer.
static __inline Lock *lock_for_pointer(void *ptr) {
  intptr_t hash = (intptr_t)ptr;
  // Disregard the lowest 4 bits.  We want all values that may be part of the
  // same memory operation to hash to the same value and therefore use the same
  // lock.
  hash >>= 4;
  // Use the next bits as the basis for the hash
  intptr_t low = hash & SPINLOCK_MASK;
  // Now use the high(er) set of bits to perturb the hash, so that we don't
  // get collisions from atomic fields in a single object
  hash >>= 16;
  hash ^= low;
  // Return a pointer to the word to use
  return locks + (hash & SPINLOCK_MASK);
}

/// Macros for determining whether a size is lock free.  Clang can not yet
/// codegen __atomic_is_lock_free(16), so for now we assume 16-byte values are
/// not lock free.
#if defined(__GNUC__) && !defined(__clang__)
#define IS_LOCK_FREE_1 __atomic_is_lock_free(1, NULL)
#define IS_LOCK_FREE_2 __atomic_is_lock_free(2, NULL)
#define IS_LOCK_FREE_4 __atomic_is_lock_free(4, NULL)
#define IS_LOCK_FREE_8 __atomic_is_lock_free(8, NULL)
#else
#define IS_LOCK_FREE_1 __c11_atomic_is_lock_free(1)
#define IS_LOCK_FREE_2 __c11_atomic_is_lock_free(2)
#define IS_LOCK_FREE_4 __c11_atomic_is_lock_free(4)
#define IS_LOCK_FREE_8 __c11_atomic_is_lock_free(8)
#endif
#define IS_LOCK_FREE_16 0

/// Macro that calls the compiler-generated lock-free versions of functions
/// when they exist.
#define LOCK_FREE_CASES()                                                      \
  do {                                                                         \
    switch (size) {                                                            \
    case 1:                                                                    \
      if (IS_LOCK_FREE_1) {                                                    \
        LOCK_FREE_ACTION(uint8_t);                                             \
      }                                                                        \
      break;                                                                   \
    case 2:                                                                    \
      if (IS_LOCK_FREE_2) {                                                    \
        LOCK_FREE_ACTION(uint16_t);                                            \
      }                                                                        \
      break;                                                                   \
    case 4:                                                                    \
      if (IS_LOCK_FREE_4) {                                                    \
        LOCK_FREE_ACTION(uint32_t);                                            \
      }                                                                        \
      break;                                                                   \
    case 8:                                                                    \
      if (IS_LOCK_FREE_8) {                                                    \
        LOCK_FREE_ACTION(uint64_t);                                            \
      }                                                                        \
      break;                                                                   \
    case 16:                                                                   \
      if (IS_LOCK_FREE_16) {                                                   \
        /* FIXME: __uint128_t isn't available on 32 bit platforms.             \
        LOCK_FREE_ACTION(__uint128_t);*/                                       \
      }                                                                        \
      break;                                                                   \
    }                                                                          \
  } while (0)

/// An atomic load operation.  This is atomic with respect to the source
/// pointer only.
void __atomic_load_c(int size, void *src, void *dest, int model) {
#if defined(__GNUC__) && !defined(__clang__)
#define LOCK_FREE_ACTION(type)                                                 \
  *((type *)dest) = __atomic_load_n((_Atomic(type) *)src, model);              \
  return;
#else
#define LOCK_FREE_ACTION(type)                                                 \
  *((type *)dest) = __c11_atomic_load((_Atomic(type) *)src, model);            \
  return;
#endif
  LOCK_FREE_CASES();
#undef LOCK_FREE_ACTION
  Lock *l = lock_for_pointer(src);
  lock(l);
  memcpy(dest, src, size);
  unlock(l);
}

/// An atomic store operation.  This is atomic with respect to the destination
/// pointer only.
void __atomic_store_c(int size, void *dest, void *src, int model) {
#if defined(__GNUC__) && !defined(__clang__)
#define LOCK_FREE_ACTION(type)                                                 \
  __atomic_store_n((_Atomic(type) *)dest, *(type *)src, model);                \
  return;
#else
#define LOCK_FREE_ACTION(type)                                                 \
  __c11_atomic_store((_Atomic(type) *)dest, *(type *)src, model);              \
  return;
#endif
  LOCK_FREE_CASES();
#undef LOCK_FREE_ACTION
  Lock *l = lock_for_pointer(dest);
  lock(l);
  memcpy(dest, src, size);
  unlock(l);
}

/// Atomic compare and exchange operation.  If the value at *ptr is identical
/// to the value at *expected, then this copies value at *desired to *ptr.  If
/// they  are not, then this stores the current value from *ptr in *expected.
///
/// This function returns 1 if the exchange takes place or 0 if it fails.
int __atomic_compare_exchange_c(int size, void *ptr, void *expected,
                                void *desired, int success, int failure) {
#if defined(__GNUC__) && !defined(__clang__)
#define LOCK_FREE_ACTION(type)                                                 \
  return __atomic_compare_exchange_n(                                          \
      (_Atomic(type) *)ptr, (type *)expected, *(type *)desired, true,          \
      success, failure)
#else
#define LOCK_FREE_ACTION(type)                                                 \
  return __c11_atomic_compare_exchange_strong(                                 \
      (_Atomic(type) *)ptr, (type *)expected, *(type *)desired, success,       \
      failure)
#endif
  LOCK_FREE_CASES();
#undef LOCK_FREE_ACTION
  Lock *l = lock_for_pointer(ptr);
  lock(l);
  if (memcmp(ptr, expected, size) == 0) {
    memcpy(ptr, desired, size);
    unlock(l);
    return 1;
  }
  memcpy(expected, ptr, size);
  unlock(l);
  return 0;
}

/// Performs an atomic exchange operation between two pointers.  This is atomic
/// with respect to the target address.
void __atomic_exchange_c(int size, void *ptr, void *val, void *old, int model) {
#if defined(__GNUC__) && !defined(__clang__)
#define LOCK_FREE_ACTION(type)                                                 \
  *(type *)old =                                                               \
      __atomic_exchange_n((_Atomic(type) *)ptr, *(type *)val, model);          \
  return;
#else
#define LOCK_FREE_ACTION(type)                                                 \
  *(type *)old =                                                               \
      __c11_atomic_exchange((_Atomic(type) *)ptr, *(type *)val, model);        \
  return;
#endif
  LOCK_FREE_CASES();
#undef LOCK_FREE_ACTION
  Lock *l = lock_for_pointer(ptr);
  lock(l);
  memcpy(old, ptr, size);
  memcpy(ptr, val, size);
  unlock(l);
}

////////////////////////////////////////////////////////////////////////////////
// Where the size is known at compile time, the compiler may emit calls to
// specialised versions of the above functions.
////////////////////////////////////////////////////////////////////////////////
#ifdef __SIZEOF_INT128__
#define OPTIMISED_CASES                                                        \
  OPTIMISED_CASE(1, IS_LOCK_FREE_1, uint8_t)                                   \
  OPTIMISED_CASE(2, IS_LOCK_FREE_2, uint16_t)                                  \
  OPTIMISED_CASE(4, IS_LOCK_FREE_4, uint32_t)                                  \
  OPTIMISED_CASE(8, IS_LOCK_FREE_8, uint64_t)                                  \
  OPTIMISED_CASE(16, IS_LOCK_FREE_16, __uint128_t)
#else
#define OPTIMISED_CASES                                                        \
  OPTIMISED_CASE(1, IS_LOCK_FREE_1, uint8_t)                                   \
  OPTIMISED_CASE(2, IS_LOCK_FREE_2, uint16_t)                                  \
  OPTIMISED_CASE(4, IS_LOCK_FREE_4, uint32_t)                                  \
  OPTIMISED_CASE(8, IS_LOCK_FREE_8, uint64_t)
#endif

#if defined(__GNUC__) && !defined(__clang__)
#define OPTIMISED_CASE(n, lockfree, type)                                      \
  type __atomic_load_##n(type *src, int model) {                               \
    if (lockfree)                                                              \
      return __atomic_load_n((_Atomic(type) *)src, model);                     \
    Lock *l = lock_for_pointer(src);                                           \
    lock(l);                                                                   \
    type val = *src;                                                           \
    unlock(l);                                                                 \
    return val;                                                                \
  }
#else
#define OPTIMISED_CASE(n, lockfree, type)                                      \
  type __atomic_load_##n(type *src, int model) {                               \
    if (lockfree)                                                              \
      return __c11_atomic_load((_Atomic(type) *)src, model);                   \
    Lock *l = lock_for_pointer(src);                                           \
    lock(l);                                                                   \
    type val = *src;                                                           \
    unlock(l);                                                                 \
    return val;                                                                \
  }
#endif
OPTIMISED_CASES
#undef OPTIMISED_CASE

#if defined(__GNUC__) && !defined(__clang__)
#define OPTIMISED_CASE(n, lockfree, type)                                      \
  void __atomic_store_##n(type *dest, type val, int model) {                   \
    if (lockfree) {                                                            \
      __atomic_store_n((_Atomic(type) *)dest, val, model);                     \
      return;                                                                  \
    }                                                                          \
    Lock *l = lock_for_pointer(dest);                                          \
    lock(l);                                                                   \
    *dest = val;                                                               \
    unlock(l);                                                                 \
    return;                                                                    \
  }
#else
#define OPTIMISED_CASE(n, lockfree, type)                                      \
  void __atomic_store_##n(type *dest, type val, int model) {                   \
    if (lockfree) {                                                            \
      __c11_atomic_store((_Atomic(type) *)dest, val, model);                   \
      return;                                                                  \
    }                                                                          \
    Lock *l = lock_for_pointer(dest);                                          \
    lock(l);                                                                   \
    *dest = val;                                                               \
    unlock(l);                                                                 \
    return;                                                                    \
  }
#endif
OPTIMISED_CASES
#undef OPTIMISED_CASE

#if defined(__GNUC__) && !defined(__clang__)
#define OPTIMISED_CASE(n, lockfree, type)                                      \
  type __atomic_exchange_##n(type *dest, type val, int model) {                \
    if (lockfree)                                                              \
      return __atomic_exchange_n((_Atomic(type) *)dest, val, model);           \
    Lock *l = lock_for_pointer(dest);                                          \
    lock(l);                                                                   \
    type tmp = *dest;                                                          \
    *dest = val;                                                               \
    unlock(l);                                                                 \
    return tmp;                                                                \
  }
#else
#define OPTIMISED_CASE(n, lockfree, type)                                      \
  type __atomic_exchange_##n(type *dest, type val, int model) {                \
    if (lockfree)                                                              \
      return __c11_atomic_exchange((_Atomic(type) *)dest, val, model);         \
    Lock *l = lock_for_pointer(dest);                                          \
    lock(l);                                                                   \
    type tmp = *dest;                                                          \
    *dest = val;                                                               \
    unlock(l);                                                                 \
    return tmp;                                                                \
  }
#endif
OPTIMISED_CASES
#undef OPTIMISED_CASE

#if defined(__GNUC__) && !defined(__clang__)
#define OPTIMISED_CASE(n, lockfree, type)                                      \
  bool __atomic_compare_exchange_##n(type *ptr, type *expected, type desired,  \
                                     int success, int failure) {               \
    if (lockfree)                                                              \
      return __atomic_compare_exchange_n(                                      \
          (_Atomic(type) *)ptr, expected, desired, true, success, failure);    \
    Lock *l = lock_for_pointer(ptr);                                           \
    lock(l);                                                                   \
    if (*ptr == *expected) {                                                   \
      *ptr = desired;                                                          \
      unlock(l);                                                               \
      return true;                                                             \
    }                                                                          \
    *expected = *ptr;                                                          \
    unlock(l);                                                                 \
    return false;                                                              \
  }
#else
#define OPTIMISED_CASE(n, lockfree, type)                               \
  bool __atomic_compare_exchange_##n(type *ptr, type *expected, type desired,  \
                                     int success, int failure) {               \
    if (lockfree)                                                              \
      return __c11_atomic_compare_exchange_strong(                             \
          (_Atomic(type) *)ptr, expected, desired, success, failure);          \
    Lock *l = lock_for_pointer(ptr);                                           \
    lock(l);                                                                   \
    if (*ptr == *expected) {                                                   \
      *ptr = desired;                                                          \
      unlock(l);                                                               \
      return true;                                                             \
    }                                                                          \
    *expected = *ptr;                                                          \
    unlock(l);                                                                 \
    return false;                                                              \
  }
#endif
OPTIMISED_CASES
#undef OPTIMISED_CASE

////////////////////////////////////////////////////////////////////////////////
// Atomic read-modify-write operations for integers of various sizes.
////////////////////////////////////////////////////////////////////////////////
#if defined(__GNUC__) && !defined(__clang__)
#define ATOMIC_RMW(n, lockfree, type, opname, op)                              \
  type __atomic_fetch_##opname##_##n(type *ptr, type val, int model) {         \
    if (lockfree)                                                              \
      return __atomic_fetch_##opname((_Atomic(type) *)ptr, val, model);        \
    Lock *l = lock_for_pointer(ptr);                                           \
    lock(l);                                                                   \
    type tmp = *ptr;                                                           \
    *ptr = tmp op val;                                                         \
    unlock(l);                                                                 \
    return tmp;                                                                \
  }
#else
#define ATOMIC_RMW(n, lockfree, type, opname, op)                              \
  type __atomic_fetch_##opname##_##n(type *ptr, type val, int model) {         \
    if (lockfree)                                                              \
      return __c11_atomic_fetch_##opname((_Atomic(type) *)ptr, val, model);    \
    Lock *l = lock_for_pointer(ptr);                                           \
    lock(l);                                                                   \
    type tmp = *ptr;                                                           \
    *ptr = tmp op val;                                                         \
    unlock(l);                                                                 \
    return tmp;                                                                \
  }
#endif

#define OPTIMISED_CASE(n, lockfree, type) ATOMIC_RMW(n, lockfree, type, add, +)
OPTIMISED_CASES
#undef OPTIMISED_CASE
#define OPTIMISED_CASE(n, lockfree, type) ATOMIC_RMW(n, lockfree, type, sub, -)
OPTIMISED_CASES
#undef OPTIMISED_CASE
#define OPTIMISED_CASE(n, lockfree, type) ATOMIC_RMW(n, lockfree, type, and, &)
OPTIMISED_CASES
#undef OPTIMISED_CASE
#define OPTIMISED_CASE(n, lockfree, type) ATOMIC_RMW(n, lockfree, type, or, |)
OPTIMISED_CASES
#undef OPTIMISED_CASE
#define OPTIMISED_CASE(n, lockfree, type) ATOMIC_RMW(n, lockfree, type, xor, ^)
OPTIMISED_CASES
#undef OPTIMISED_CASE
