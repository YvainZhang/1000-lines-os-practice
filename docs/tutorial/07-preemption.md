# 07 扩展二：从协作调度到定时器抢占

[上一课](06-interrupts.md) · [课程目录](README.md) · [下一课](08-pipes.md)

## 要解决的问题

协作调度依赖进程主动 `yield`。如果用户程序一直计算，其他可运行进程可能没有机会执行。目标是用周期性 timer interrupt 打断用户计算，再通过现有调度路径运行另一个进程。

阅读 [kernel.c](../../kernel.c) 的 `timer_now`、`sbi_set_timer`、`timer_init`、`timer_handle_irq`、`handle_trap`、`yield`；测试入口在 [shell.c](../../shell.c) 的 `test_timer` 和 `test_preemption`。

## 第一步：安排下一次时钟事件

内核通过 SBI TIME 请求下一次绝对 deadline，接口参考 [SBI 官方规范](https://github.com/riscv-non-isa/riscv-sbi-doc)。本项目的 RV32 封装把 64 位值拆成两半传递；读取时间使用 high/low/high 重读，避免低位回绕造成拼接错误。

当前配置把 timebase 设为 10 MHz，`TIMER_INTERVAL=100000` 对应 10 ms。每个 hart 有独立 `next_timer`。中断时沿旧 deadline 增加 interval，直到落在当前时间之后，再重新安排下一次事件。

## 第二步：把 tick 接到调度

`timer_handle_irq` 负责重设 timer 和计数；`handle_trap` 仅在来源为 U-mode 且有当前进程时调用 `yield`。`yield` 取得 PCB 锁，把状态改回 `RUNNABLE`，然后 `sched` 切回 scheduler。

```text
用户计算 -> timer trap -> 保存现场 -> 设置未来 deadline
        -> yield -> RUNNABLE -> scheduler -> 恢复某个进程
```

异步中断不推进 `sepc`。普通 syscall 路径不主动重新开启中断，也不提供任意内核位置的抢占安全性，因此不能简单移除 U-mode 判断来“升级为内核抢占”。

## 第三步：设计能证明抢占的实验

只看到 tick 增长不够，它只能证明计时事件被处理。当前测试把 quick 和 hog 绑定到同一个 hart：hog 报告已开始后进入无 `yield` 的纯计算区间；父任务收到开始消息后打开 gate，让 quick 返回 `Q`；hog 计算完成后返回 `H`。测试要求 `Q` 先于 `H`。

管道在这里是测试同步工具。两个任务若分属两核，同时执行也可能得到 `QH`，所以同核绑定是实验关键。

## 动手实验

1. 运行 `NCPU=1 bash run.sh`，执行 `selftest`，区分 timer 和 preemption 两条 `[PASS]` 各证明什么。
2. 退出后运行 `NCPU=4 bash run.sh`，再次自测，阅读测试源码确认 worker 仍绑定同一 hart。
3. 记录两种配置输出，不用 `get_ticks()` 差值直接估算墙钟时间：当前查询返回所有 hart timer 处理次数的总和。

## 反例练习：有 tick，但没有抢占

仅在自己的实验副本中，暂时取消 timer 分支的 `yield()` 调用，保留 `timer_handle_irq()`，并运行：

```sh
python3 scripts/smoke.py --cpus 1 --timeout 30
```

预期 timer 子测试可能通过，抢占子测试失败或超时。具体失败输出取决于执行时序；这个练习尚未包含在正常回归里。恢复原调用后必须重新通过自测，不能提交这个故障版本。

## 验收与思考

<details><summary>为什么必须先设置未来 deadline，再切换进程？</summary>

避免仍处于到期状态的 timer 立即重复进入；调度前先恢复事件源的下一次触发条件，再交出当前执行权。10 ms 是配置的 timer 周期，不保证所有任务严格每 10 ms 获得 CPU，也不构成实时响应上界。

</details>
