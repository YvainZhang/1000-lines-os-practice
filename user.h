#pragma once
#include "common.h"

__attribute__((noreturn)) void exit(void);
void putchar(char ch);
int getchar(void);
int readfile(const char *filename, char *buf, int len);
int writefile(const char *filename, const char *buf, int len);
int pipe_open(int key, int mode);
int pipe_read(int fd, void *buf, int len);
int pipe_write(int fd, const void *buf, int len);
int pipe_close(int fd);
int spawn(void (*entry)(int), int arg, int hart);
int get_hartid(void);
int get_ncpu(void);
int get_ticks(void);
int get_irq_count(int which);
void yield(void);
