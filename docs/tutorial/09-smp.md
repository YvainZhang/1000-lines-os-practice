# 09 扩展四：让多个 hart 安全运行

[上一课](08-pipes.md) · [课程目录](README.md) · [下一课](10-capstone.md)

## 要解决的问题

启动更多 CPU 只是第一步。两个 hart 若同时选择同一个进程，就可能同时使用同一内核栈，破坏执行现场。目标是建立每核状态、受锁保护的调度选择和跨核唤醒。

阅读 [run.sh](../../run.sh)、[kernel.ld](../../kernel.ld)，以及 [kernel.c](../../kernel.c) 的 `struct cpu`、`start_secondary_harts`、`secondary_boot`、`secondary_main`、`scheduler`、`switch_address_space`、`notify_schedulers`。

## 第一步：先分清全局与每核初始化

`NCPU` 同时生成编译期 `CPU_COUNT` 和 QEMU `-smp` 参数。当前支持 1～4 个连续 hart ID，boot hart 来自固件传入值，不应假设总为 0。

boot hart 独占 BSS、分配器、全局页表、设备和文件系统初始化，通过 SBI HSM 启动其他 hart。每个次级 hart 使用独立的 32 KiB 启动/调度栈，安装自己的 `tp`、页表、trap 和 timer，再登记 online。boot hart 等待所有核上线后创建 shell。

固件启动接口参考 [SBI HSM](https://github.com/riscv-non-isa/riscv-sbi-doc/blob/master/src/ext-hsm.adoc)。启动调用返回与应用层初始化完成是不同事件，因此仍需本项目的 online 协议。

## 第二步：共享进程表，逐 PCB 互斥

各核 scheduler 扫描同一进程数组，取得 PCB 锁后检查 `RUNNABLE` 和 affinity，再改为 `RUNNING`。检查和状态转换必须处于同一临界区，不能先无锁选中再加锁运行。

锁跨上下文切换交接：scheduler 持锁切入进程，首次由 `first_user_entry` 释放；后续 `yield`、sleep 或 exit 持锁切回 scheduler，由 scheduler 完成收尾。这个协议让状态与现场切换不能被其他核看成两个独立操作。

```text
hart A: lock PCB -> RUNNABLE? -> RUNNING -> switch_context
hart B: 等待同一 PCB 锁 -------> 获锁后重新检查状态
```

`running_on` 断言用于捕捉同一进程被双核运行的错误。它是保护措施，不表示回归穷尽了所有竞态。

## 第三步：恢复本核环境并唤醒其他核

`mycpu()` 从可信 `tp` 获得当前核状态，`myproc()` 再取当前进程。切换地址空间时逐核设置需要的 SUM 位；另一个 hart 上先前设过并不意味着本核已设。trap 返回也必须使用当前 scheduler 写入的可信 CPU 指针。

`wakeup` 或创建进程后，IPI 通知其他在线 scheduler 重新检查任务。每个 hart 有自己的 timer，设备 IRQ 则集中在 boot hart。共享 PCB 扫描支持基本并行执行，但没有每核队列、工作窃取或完整负载均衡。

## 动手实验

依次运行，等待每条命令结束：

```sh
python3 scripts/smoke.py --cpus 1
python3 scripts/smoke.py --cpus 2
python3 scripts/smoke.py --cpus 4
```

比较 online 核数和 `worker affinity on all N hart(s)`。阅读 `test_all_harts`：每个 worker 返回期望 hart 与实际 hart，并检查重复和缺失。只看 QEMU 启动参数不能证明每个核都在执行用户任务。

加做：运行 `python3 scripts/smoke.py --cpus 3`，保存实际结果，并与验证记录中的推送前四配置回归比较。既有结果不能代替你修改代码后的重新验证。

## 验收与思考

<details><summary>去掉 affinity 检查能否称为自动负载均衡？</summary>

不能。它只改变哪些核可以选择一个任务；没有衡量负载、调整队列或保证公平性的策略。取消测试的绑定还会破坏抢占实验的同核前提，得到的通过结果不再证明原命题。

</details>

空闲核的检查与 `wfi` 之间仍存在唤醒时序窗口，周期 timer 提供后续唤醒；动态映射修改也尚无跨核 TLB shootdown。这些是进一步实验的起点。
