# 01 从链接地址到第一个字符

[上一课](00-getting-started.md) · [课程目录](README.md) · [下一课](02-memory.md)

## 本课目标

解释为什么内核不能直接从普通 C 函数开始，以及程序布局、栈和输出的关系。先读 [kernel.ld](../../kernel.ld)、[kernel.c](../../kernel.c) 的 `boot` / `kernel_main`，再读 [common.c](../../common.c) 的 `printf`。

## 原理与源码路线

`kernel.ld` 使用 `ENTRY(boot)`，把内核放在 `0x80200000`，收集代码、常量、数据和 BSS，并预留启动栈与物理内存范围。链接脚本决定地址，启动代码负责让 CPU 的寄存器与这些地址匹配。

`boot` 先建立内核 `gp` 和当前 hart 的启动栈，再跳入 `kernel_main`。后者清零 BSS，建立本核状态并初始化设备。C 的局部变量和函数调用需要有效栈；“C 源码里没有手写栈”不意味着运行时不需要栈。

从 `printf` 追到 `putchar`，会发现公共格式化代码由内核和用户程序分别链接。内核版本的 `putchar` 写 UART；用户版本通过 syscall 请求输出。相同函数名在两个不同程序里不必指向同一份实现。

```text
run.sh -> kernel.elf -> OpenSBI -> boot -> kernel_main
                                           -> printf -> kernel putchar -> UART
```

## 动手练习

1. 用 `git show 6c3b96c:kernel.c` 阅读早期引导代码，对比当前 `boot` 多了哪些多核相关准备。
2. 在 `kernel_main` 现有启动信息之后增加一条自己的课程标识输出。不要把输出放在 `uart_init` 之前。
3. 运行 `NCPU=1 bash run.sh`，确认标识先于 shell 出现，执行 `hello` 与 `selftest`。
4. 阅读生成的 `kernel.map`，搜索 `boot`、`__bss`、`__stack_top` 和 `__free_ram`。这些是构建产物，不提交它们。
5. 手动恢复自己添加的输出，再运行构建确认恢复。

## 验收与思考

画出“链接器确定地址 → 汇编建立寄存器 → C 初始化 → UART 输出”的链路。回答：BSS 清零与栈预留为什么是两件事？

<details><summary>参考要点</summary>

BSS 对应需要零初值的数据区域；栈用于运行时保存调用现场和局部状态。本项目把启动栈单独放在 `.stack`，不能用“清零 BSS”代替设置 `sp`。

</details>

本课只需理解启动链，不必先读懂全部多核启动协议；第 09 课会解释每个 hart 为什么需要不同的栈。
