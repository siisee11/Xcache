/*
 * Copyright (c) 1998-2010 Julien Benoist <julien@benoist.name>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY JULIEN BENOIST ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */


/* double-word - as in machine word - primitive
 * used for the double-compare-and-swap operations */
#ifdef __x86_64__
typedef __uint128_t DWORD;
#else
typedef __uint64_t DWORD;
#endif

/* need:
 *   add-and-fetch
 *   compare-and-swap
 *   double-compare-and-swap */

#ifdef __x86_64__
#define SHIFT 	  64
#define XADDx	  "xaddq"
#define CMPXCHGxB "cmpxchg16b"
#else
#define SHIFT 	  32
#define XADDx	  "xaddl"
#define CMPXCHGxB "cmpxchg8b"
#endif
/* add-and-fetch: atomically adds @add to @mem
 *   @mem: pointer to value
 *   @add: value to add
 *
 *   returns: new value */
static inline
unsigned int FAA(volatile unsigned long *mem, unsigned long add)
{
	unsigned long __tmp = add;
	__asm__ __volatile__("lock " XADDx " %0,%1"
			:"+r" (add),
			"+m" (*mem)
			: : "memory");
	return add;
}
