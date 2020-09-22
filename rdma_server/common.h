#ifndef COMMON_H
#define COMMON_H

#ifndef asm
#define asm __asm
#endif

#define ATOMIC_SET __sync_lock_test_and_set
#define ATOMIC_RELEASE __sync_lock_release

#define ATOMIC_SUB __sync_sub_and_fetch
#define ATOMIC_SUB64 ATOMIC_SUB
#define CAS_ __sync_bool_compare_and_swap
#define XCHG __sync_lock_test_and_set
#define ATOMIC_ADD __sync_add_and_fetch
#define ATOMIC_ADD64 ATOMIC_ADD

#define mb __sync_synchronize
#define lmb() asm volatile("":::"memory")
#define smb() asm volatile("":::"memory")

//#define CAS(_p, _u, _v)  (__atomic_compare_exchange_n (_p, _u, _v, false, __ATOMIC_ACQUIRE, __ATOMIC_ACQUIRE))
#endif
