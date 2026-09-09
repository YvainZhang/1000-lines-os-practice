# 三分钟演示

本文用于完成学习后的成果展示。系统学习请先阅读[课程目录](tutorial/README.md)，并完成[综合实验](tutorial/10-capstone.md)。

## 0:00–0:30：说明来源和范围

“这是基于 Operating System in 1,000 Lines 扩展的 RISC-V 教学内核。我重点研究的是设备中断、用户态抢占、阻塞管道和多核同步。基础页表、用户态和文件系统来自教程实践。”

## 0:30–1:00：启动并观察

提前安装工具链，在仓库根目录执行 `NCPU=4 bash run.sh`。指出 `smp: 4 hart(s) online`，输入 `help`、`hello`、`irqstat`。说明 shell 在 U-mode，输出经过 syscall；统计值来自内核，不是宿主机指标。

## 1:00–2:00：证明扩展可运行

执行 `selftest`，逐项解释输出：

- Timer：观察 tick 确实前进。
- Block IRQ：文件写回触发磁盘完成中断。
- Filesystem capacity：超过总缓冲区容量的写入被拒绝，已有数据保持不变。
- Pipe：通过 256 字节缓冲传送 3072 字节，核对顺序、总长度和 EOF。
- Preemption：把两个 worker 绑定同核，纯计算任务不主动 yield，短任务仍先返回。
- SMP：每个 hart 启动绑定任务，检查实际执行核。

再次输入 `irqstat`，观察中断和切换计数。计数增长证明活动发生，不能用作吞吐或延迟结论。

## 2:00–3:00：讲一个技术难点

打开 `kernel.c` 的 `sleep_on`、`wakeup` 和 `sched`，解释条件锁与 PCB 锁的交接如何防止检查条件后错过唤醒。也可选择 `kernel_entry`，解释为什么不能信任用户态的栈和 `tp`。

最后说明边界：内核路径不可抢占，缺少完整内存回收，文件自测没有验证重启持久化。展示 `python3 scripts/smoke.py` 的日志作为复现入口。

## 项目描述范例

> 基于 OS in 1,000 Lines 构建 RV32 教学内核扩展，实现 PLIC/UART/VirtIO 中断、10 ms 用户态抢占、阻塞具名管道与最多 4 hart SMP；使用串口回归验证 1/2/4 核启动、管道数据完整性、同核抢占和任务亲和性。

只有在能够解释对应实现、且回归实际通过时使用这段描述；不填写未经测量的性能提升比例。
