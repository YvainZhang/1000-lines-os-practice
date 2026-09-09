# 03 进程如何进入用户态并调用内核

[上一课](02-memory.md) · [课程目录](README.md) · [下一课](04-filesystem.md)

## 本课目标

把“进程”具体化为地址空间、寄存器现场、内核栈和调度状态。阅读 [kernel.c](../../kernel.c) 的 `struct process`、`create_process`、`first_user_entry`、`enter_user`、`switch_context`、`handle_syscall`，以及 [user.c](../../user.c)。

## 从镜像到进程

`run.sh` 先生成用户 ELF，再转换为平坦镜像并链接到内核。`create_process` 将镜像复制进用户页、记录入口和参数、准备内核栈上的初始上下文，最终标为 `RUNNABLE`。它没有从文件系统加载任意 ELF 的 `exec`。

`scheduler` 选择可运行进程后切换地址空间和上下文。首次执行进入 `first_user_entry`，随后 `enter_user` 设置用户入口、用户栈和状态，通过 `sret` 进入 U-mode。再次调度通常恢复先前保存的内核执行位置，而不是重新执行用户 `main`。

## 跟踪一次字符输出

```text
shell printf -> user putchar -> syscall(SYS_PUTCHAR, ...)
             -> ecall -> kernel_entry -> handle_trap
             -> handle_syscall -> kernel putchar -> UART
             -> 恢复 trap frame -> sret -> 用户下一条指令
```

本项目约定 `a3` 放系统调用号，`a0`～`a2` 放参数，`a0` 放返回值。这是本项目 ABI，不要照搬 Linux 的 syscall 寄存器约定。U-mode `ecall` 分支将保存的 `sepc` 加 4；异步中断不能照做。

`switch_context` 保存函数调用约定需要保留的寄存器；trap 可能打断任意用户指令，因此 trap frame 保存更完整的现场。两者都“保存寄存器”，用途与保存集合却不同。

## 动手练习

1. 在 [shell.c](../../shell.c) 增加一个 `about` 命令，打印一句课程说明；放在既有命令比较分支中。
2. 执行 `bash run.sh`，验证 `about`、`hello`、`selftest` 和 `exit`。确认退出 shell 后仍需手动退出 QEMU。
3. 沿 `SYS_PUTCHAR` 从共享宏追到用户封装和内核分支，记录每层参数。
4. 恢复自己新增的分支。这个练习复用了已有 syscall，无需新增调用号。

## 验收与思考

如果 `ecall` 返回时没有推进 `sepc` 会怎样？`spawn(entry, arg, hart)` 为什么不等于 `fork()`？

<details><summary>参考要点</summary>

会再次执行同一条 `ecall`。当前 `spawn` 创建同一嵌入镜像中的指定入口任务，没有复制调用瞬间的整个父进程执行状态，也不提供 `fork` 的双返回语义。

</details>
