#ifndef _I386_TYPES_H
#define _I386_TYPES_H

#ifndef __ASSEMBLY__

typedef unsigned short umode_t;

/*
 * __xx is ok: it doesn't pollute the POSIX namespace. Use these in the
 * header files exported to user space
 */

typedef __signed__ char __s8;
typedef unsigned char __u8;

typedef __signed__ short __s16;
typedef unsigned short __u16;

typedef __signed__ int __s32;
typedef unsigned int __u32;

#if defined(__GNUC__) && !defined(__STRICT_ANSI__)
typedef __signed__ long long __s64;

// 明白u开头的意思了。 无关cpu架构。内核定义大小，自动转换成对应cpu的位数大小。
// 比如在i386  ,32位机上，想定义64位，就需要2个long。 而假如是ia64，64位机上，就只需要一个long。
// 所以u开头的系列是内核帮你自动适配cpu的位数，让你想用多少位就多少位，不要去在乎cpu的位数。
typedef unsigned long long __u64;		
#endif

#endif /* __ASSEMBLY__ */

/*
 * These aren't exported outside the kernel to avoid name space clashes
 */
#ifdef __KERNEL__

#define BITS_PER_LONG 32

#ifndef __ASSEMBLY__

#include <linux/config.h>

typedef signed char s8;
typedef unsigned char u8;

typedef signed short s16;
typedef unsigned short u16;

typedef signed int s32;
typedef unsigned int u32;

typedef signed long long s64;
typedef unsigned long long u64;

/* DMA addresses come in generic and 64-bit flavours.  */

#ifdef CONFIG_HIGHMEM64G
typedef u64 dma_addr_t;
#else
typedef u32 dma_addr_t;
#endif
typedef u64 dma64_addr_t;

#ifdef CONFIG_LBD
typedef u64 sector_t;
#define HAVE_SECTOR_T
#endif

#endif /* __ASSEMBLY__ */

#endif /* __KERNEL__ */

#endif
