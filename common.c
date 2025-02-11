#include "common.h"

void putchar(char ch);

void *memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *) dst;
    const uint8_t *s = (const uint8_t *) src;
    while (n--)
        *d++ = *s++;
    return dst;
}

void *memset(void *buf, char c, size_t n) {
    uint8_t *p = (uint8_t *) buf;
    while (n--)
        *p++ = c;
    return buf;
}

char *strcpy(char *dst, const char *src) {
    char *d = dst;
    while (*src)
        *d++ = *src++;
    *d = '\0';
    return dst;
}

int strcmp(const char *s1, const char *s2) {
    while (*s1 && *s2) {
        if (*s1 != *s2)
            break;
        s1++;
        s2++;
    }

    return *(unsigned char *)s1 - *(unsigned char *)s2;
}

void printf(const char *fmt, ...) {
    va_list vargs;
    va_start(vargs, fmt);

    while (*fmt) {
        if (*fmt == '%') {
            fmt++; // 跳过 '%'
            switch (*fmt) { // 读取下一个字符
                case '\0': // 当 '%' 作为格式字符串的末尾
                    putchar('%');
                    goto end;
                case '%': // 打印 '%'
                    putchar('%');
                    break;
                case 's': { // 打印以 NULL 结尾的字符串
                    const char *s = va_arg(vargs, const char *);
                    while (*s) {
                        putchar(*s);
                        s++;
                    }
                    break;
                }
                case 'd': { // 以十进制打印整型
                    int value = va_arg(vargs, int);
                    if (value < 0) {
                        putchar('-');
                        value = -value;
                    }

                    int divisor = 1;
                    while (value / divisor > 9)
                        divisor *= 10;

                    while (divisor > 0) {
                        putchar('0' + value / divisor);
                        value %= divisor;
                        divisor /= 10;
                    }

                    break;
                }
                case 'x': { // 以十六进制打印整型
                    int value = va_arg(vargs, int);
                    for (int i = 7; i >= 0; i--) {
                        int nibble = (value >> (i * 4)) & 0xf;
                        putchar("0123456789abcdef"[nibble]);
                    }
                }
            }
        } else {
            putchar(*fmt);
        }

        fmt++;
    }

end:
    va_end(vargs);
}

// 错误码定义
#define EOK       0  // 成功
#define EINVAL    1  // 无效参数
#define EOVERFLOW 2  // 缓冲区溢出

int strcpy_s(char *dst, size_t dstsize, const char *src) {
    // 参数验证
    if (dst == NULL || dstsize == 0) {
        return EINVAL;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return EINVAL;
    }

    // 计算源字符串长度
    size_t srclen = 0;
    while (src[srclen] && srclen < dstsize) {
        srclen++;
    }

    // 检查是否有足够空间（包括结尾的空字符）
    if (srclen >= dstsize) {
        // 确保目标字符串总是以空字符结尾
        dst[0] = '\0';
        return EOVERFLOW;
    }

    // 执行复制
    while (*src) {
        *dst++ = *src++;
    }
    *dst = '\0';
    
    return EOK;
}