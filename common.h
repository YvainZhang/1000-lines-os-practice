#pragma once

typedef int bool;
typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef unsigned long long uint64_t;
typedef uint32_t size_t;
typedef uint32_t paddr_t;
typedef uint32_t vaddr_t;

#define PAGE_SIZE 4096

#define SYS_PUTCHAR 1
#define SYS_GETCHAR 2
#define SYS_EXIT    3
#define SYS_READFILE  4
#define SYS_WRITEFILE 5
#define SYS_PIPE_OPEN  6
#define SYS_PIPE_READ  7
#define SYS_PIPE_WRITE 8
#define SYS_PIPE_CLOSE 9
#define SYS_SPAWN      10
#define SYS_GET_HARTID 11
#define SYS_GET_NCPU   12
#define SYS_GET_TICKS  13
#define SYS_GET_IRQ_COUNT 14
#define SYS_YIELD      15

#define PIPE_READ   1
#define PIPE_WRITE  2
#define PIPE_CREATE 4

#define IRQ_TIMER          0
#define IRQ_UART           1
#define IRQ_BLOCK          2
#define IRQ_CONTEXT_SWITCH 3
#define IRQ_UART_RX_DROPPED 4

#define true  1
#define false 0
#define NULL  ((void *) 0)
#define align_up(value, align)   __builtin_align_up(value, align)
#define is_aligned(value, align) __builtin_is_aligned(value, align)
#define offsetof(type, member)   __builtin_offsetof(type, member)
#define va_list  __builtin_va_list
#define va_start __builtin_va_start
#define va_end   __builtin_va_end
#define va_arg   __builtin_va_arg

void *memset(void *buf, char c, size_t n);
void *memcpy(void *dst, const void *src, size_t n);
char *strcpy(char *dst, const char *src);
int strcmp(const char *s1, const char *s2);
void printf(const char *fmt, ...);
