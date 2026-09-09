# 06 扩展一：从轮询到设备中断

[上一课](05-traps-and-sleep.md) · [课程目录](README.md) · [下一课](07-preemption.md)

## 要解决的问题

轮询不断询问设备“完成了吗”。设备速度较慢时，进程会占着 CPU 等待。目标是让设备发出通知，让等待者阻塞，并在完成后恢复执行。

前置知识：第 04 课的块请求和第 05 课的 trap / sleep。阅读 [kernel.c](../../kernel.c) 的 `plic_global_init`、`plic_hart_init`、`external_handle_irq`、`uart_handle_irq`、`virtio_handle_irq`、`getchar_blocking`、`read_write_disk`。

## 第一步：把设备接到 CPU

当前 QEMU 配置中 UART 是 IRQ 10，VirtIO block 是 IRQ 1。PLIC 负责汇集和分发设备中断；初始化设置优先级、使能位、阈值和 hart context，CPU 侧还需使能 external interrupt。硬件地址和 context 计算是当前平台配置，不能直接推广到其他机器。

设备 IRQ 由 boot hart 处理。`handle_trap` 区分同步异常和异步中断，再把 external interrupt 交给 `external_handle_irq`。不能把所有非 ecall 的 trap 都当成程序错误。

## 第二步：完成两层确认

`external_handle_irq` 循环向 PLIC claim IRQ；取到设备号后运行对应 ISR，随后 complete 同一个 IRQ。UART ISR 排空接收 FIFO；VirtIO ISR ACK 设备中断状态，并消费 used ring 中的新完成项。

```text
设备产生事件 -> PLIC -> external trap -> claim
           -> 设备处理 / ACK -> 更新条件 -> wakeup
           -> PLIC complete -> 返回被打断的位置
```

设备确认和 PLIC complete 各自处理一层状态，不能互相替代。VirtIO 还需核对完成描述符 head，不能仅看到通知就把任意在途请求当成完成。描述符和队列索引之间使用 I/O fence，避免“通知可见但数据尚未可见”。

## 第三步：让进程等待条件

UART ISR 将字符放进 128 字节 RX 环形缓冲区，并唤醒 `getchar_blocking` 的等待者。缓冲区满时丢最旧字符，`irqstat` 的 `uart-rx-dropped` 可观察这一情况。用户进程读的是缓冲区，不在用户态访问 UART 寄存器。

块请求在 `blk_irq_lock` 下发布，运行期用 `while (!blk_done) sleep_on(...)` 等待。启动期没有可睡眠进程，使用开中断配合 `wfi` 等待 ISR 更新条件。UART TX 仍短轮询，不能将本项目描述为所有 I/O 都完全没有轮询。

## 实验：证明中断路径发生

1. 运行 `NCPU=1 bash run.sh`，输入 `irqstat` 并记录 UART、block 计数。
2. 输入 `hello`，再查询统计。正常输入会经过 UART 中断，但一次 IRQ 可以处理多个字符，不要求“一字符一中断”。
3. 输入 `selftest`，观察 `[PASS] VirtIO block interrupt`。
4. 对照 `test_block_irq`：它读取内存文件后原样写回，比较写回前后的 block 计数。
5. 保存自测与统计输出，说明为什么只输入 `readfile` 不足以证明磁盘中断处理。

## 加做练习：可观察的接收缓冲

在个人实验中把 `UART_RX_CAPACITY` 暂改为 16，逐条输入命令与粘贴一串较长字符，比较丢弃计数。终端传输速度和 QEMU 时序可能让丢弃不出现；记录实际观察，不把“没有丢弃”当成失败，也不要声称这已是确定性溢出测试。恢复为 128 后再运行多核回归。

## 验收与故障定位

| 现象 | 优先阅读 |
| --- | --- |
| 输入无响应 | UART IER、PLIC enable、CPU SIE、RX wakeup |
| 磁盘一直等完成 | used ring、描述符 head、`blk_done` 与等待锁 |
| IRQ 不断重复 | 设备 ACK、PLIC complete 的顺序与值 |

<details><summary>思考题：为什么 ISR 不直接运行等待进程？</summary>

ISR 更新条件并让进程成为 `RUNNABLE`。选择何时、在哪个 hart 执行由 scheduler 决定；直接跳进用户进程会绕开地址空间、上下文和 PCB 状态管理。

</details>
