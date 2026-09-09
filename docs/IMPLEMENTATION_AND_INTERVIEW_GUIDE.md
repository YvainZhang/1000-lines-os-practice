# 四项能力实现、验证与面试讲解指南

本文以当前源码为准，说明项目已经补齐的设备中断、定时器抢占、进程间管道和多处理器支持，并给出可复现的验证步骤与面试讲法。

第一次学习建议从[分章教程](tutorial/README.md)开始。本文作为完整实现的查阅资料；课程会拆解前置知识、安排实验并给出验收题。

## 1. 项目基线与完成范围

这是一个运行在 QEMU `virt` 机器上的 RV32 教学内核。OpenSBI 提供 M-mode 固件服务，内核运行在 S-mode，应用运行在 U-mode。项目原有 Sv32 页表、系统调用、进程切换、VirtIO block、内存 ustar 文件系统和交互式 shell；本次完成了四项扩展：

1. 统一 trap 入口，通过 PLIC 接入 UART RX 与 VirtIO block 中断。
2. 通过 SBI TIME 产生 10 ms tick，仅抢占正在用户态运行的进程。
3. 实现 8 个具名、256 字节有界管道，支持阻塞、唤醒、short read/write、EOF 和 `-EPIPE`。
4. 通过 SBI HSM 启动 1～4 个 hart，使用 per-CPU 调度器、每 PCB 锁和 IPI 唤醒。

边界需要准确表述：普通内核路径不可抢占；启动阶段的 VirtIO I/O 也由中断唤醒 `wfi`；UART TX 保留短轮询；系统不是完整 UNIX，不提供完整 POSIX、`fork/exec`、虚拟内存回收或完整负载均衡。

## 2. 源码入口与总体架构

主要实现集中在以下文件：

| 文件 | 职责 |
|---|---|
| `kernel.c` / `kernel.h` | trap、PLIC、UART、VirtIO、timer、进程、调度、锁、pipe、SMP 启动 |
| `user.c` / `user.h` | pipe、spawn、hart/tick/IRQ 查询等用户态 syscall 封装 |
| `shell.c` | shell 命令及 `selftest` |
| `kernel.ld` | 内核布局、`__global_pointer$`、四个 32 KiB 启动/调度栈 |
| `run.sh` | 交叉编译、磁盘打包及 `NCPU` 参数化 QEMU 启动 |

关键调用关系如下：

```text
OpenSBI TIME/HSM/IPI                 PLIC external interrupt
          |                          UART RX / VirtIO block
          +-------------------+-------------------+
                              v
                    kernel_entry -> handle_trap
                              |
        +---------------------+----------------------+
        |                     |                      |
      syscall          wakeup(wait_channel)     timer rearm
        |                     |                      |
   pipe / file I/O     BLOCKED -> RUNNABLE     user yield
        +---------------------+----------------------+
                              v
              per-CPU scheduler + per-PCB spinlock
```

源码中的对应入口是 `kernel_entry`、`handle_trap`、`timer_handle_irq`、`external_handle_irq`、`uart_handle_irq`、`virtio_handle_irq`、`sleep_on`、`wakeup`、`scheduler`、`sched`、`yield`、`start_secondary_harts`、`secondary_boot` 和 `secondary_main`。

## 3. 统一 trap 与上下文切换

### 3.1 `sscratch`、可信栈和可信 `tp`

当前实现没有单独的 trap scratch 结构，而是使用一条严格约定：

- U-mode 运行时，`sscratch` 保存当前进程可用内核栈顶 `kernel_sp`。
- S-mode 运行时，`sscratch` 为 0。
- 每个 8 KiB 进程内核栈的顶部预留 16 字节；调度器在 `kernel_sp + 12` 写入可信的 `struct cpu *`。
- 内核态 `tp` 始终指向当前 hart 的 `struct cpu`，用户态传回的 `tp` 绝不用于定位内核状态。

U→S trap 的第一条 `csrrw sp, sscratch, sp` 同时取得 `kernel_sp` 并暂存用户 `sp`。入口随后在内核栈上分配 144 字节 trap frame，先保存用户 `gp/tp`，再显式从链接器符号 `__global_pointer$` 装载内核 `gp`，并从 `kernel_sp + 12` 恢复可信 CPU 指针到 `tp`。进入 C 代码前把 `sscratch` 清零，所以 C 处理期间若出现 S-mode trap，不会错误地再次切换到用户栈。

S→S trap 时，入口看到交换得到的值为 0，立即反向交换，继续使用原内核栈。trap 来源依据保存的 `sstatus.SPP` 判断，不能依据用户 `sp` 是否为 0。返回 U-mode 前把 `sscratch` 重新设为该进程的 `kernel_sp`；返回 S-mode 前保持为 0。

### 3.2 保存集合与返回规则

`struct trap_frame` 保存 31 个非零通用寄存器、`sepc`、`sstatus`、`scause`、`stval` 和一个保留字，共 36 个 RV32 word，即 144 字节；`_Static_assert` 固定了汇编与 C 布局。`switch_context` 另用 64 字节上下文保存 `ra` 和 `s0`～`s11`，保持 16 字节 ABI 栈对齐。

同步 syscall 只接受来自 U-mode 的 `ecall`。`handle_trap` 在调用可能阻塞的 syscall 前执行 `f->sepc += 4`，否则恢复后会重复执行同一条 `ecall`。异步 timer、external 和 software interrupt 不修改 `sepc`。

### 3.3 抢占边界

硬件进入 S-mode trap 时会清除 `sstatus.SIE`，普通 syscall、文件系统和驱动路径不会主动重新打开它。因此 timer 只有在来源是 U-mode 且 `myproc()` 非空时才调用 `yield()`；S-mode timer 只重设时钟并记账。

这可以准确概括为“用户进程抢占式、普通内核非抢占式”。调度器空闲、等待次级核上线以及启动阶段等待磁盘完成时，会显式 `intr_on(); wfi; intr_off()`，它们是有意允许中断唤醒的路径。

## 4. 阻塞、唤醒与进程状态

进程状态为：

```text
UNUSED -> EMBRYO -> RUNNABLE -> RUNNING
                         ^         |
                         |         +-> BLOCKED
                         +-------------+
RUNNING -----------------------------> EXITED -> UNUSED
```

`sleep_on(channel, condition_lock)` 解决 lost wakeup：调用者先持有条件锁；函数在条件锁尚未释放时取得当前 PCB 锁，然后释放条件锁、登记 `wait_channel`、设置 `BLOCKED` 并切回 scheduler。恢复后清除 channel、释放 PCB 锁，再重新取得条件锁。调用方始终用 `while` 重新检查条件，因为被唤醒只表示条件“可能”成立。

`wakeup(channel)` 逐个取得 PCB 自己的锁，把匹配的 `BLOCKED` 进程改为 `RUNNABLE`，随后用 SBI IPI 通知其他在线 scheduler。这里没有全局 `proc_lock`；状态选择和转换由每个 `struct process.lock` 保护。

## 5. 功能一：PLIC、UART 与 VirtIO 中断

### 5.1 PLIC 分发

QEMU `virt` 中使用 PLIC `0x0c000000`、UART `0x10000000`/IRQ 10、首个 VirtIO MMIO `0x10001000`/IRQ 1。S-mode context 按 `2 * hartid + 1` 计算。`plic_global_init` 设置 IRQ 1 和 10 的 priority；`plic_hart_init` 只在运行时记录的 `boot_hartid` 上 enable 两个设备 IRQ，其他 hart 只接收自己的 timer/software interrupt。

`external_handle_irq` 循环 claim，直到返回 0。每个 IRQ 先由设备 ISR 清除设备侧原因，再执行 `fence iorw, iorw` 并向 PLIC complete 写回原 IRQ。设备 ACK 和 PLIC complete 属于两层确认，缺少任意一层都可能造成重复投递或中断无法再次进入。

### 5.2 UART RX 中断

`uart_handle_irq` 在 IRQ 中排空 RX FIFO，将字符写入 128 字节环形缓冲区，并唤醒等待 `uart_rx_count` 的 `SYS_GETCHAR`。缓冲区满时明确丢弃最旧字符，再保存最新字符，同时递增 `uart_rx_dropped`；当前 `irqstat` 通过 `uart-rx-dropped` 打印该计数，读取时持有 RX 锁。

TX 路径 `putchar` 仍在锁内短暂轮询 LSR 的 TX idle 位。这样早期启动和 panic 输出不依赖调度器或 TX 中断，代价是大批量输出会占用 CPU。

### 5.3 VirtIO block 中断

驱动使用 legacy VirtIO MMIO：复位并等待 status 归零，累加 ACK/DRIVER 状态，读取 device features、写入当前支持的 guest features 0，设置并回读 `FEATURES_OK`，配置 16 项队列的 `QueueAlign`/`QueuePFN`，最后置 `DRIVER_OK`。

请求固定使用描述符 0→1→2：header、512 字节 data、1 字节 status。当前只有一个全局请求缓冲区和一个在途请求；运行期由 `disk_mutex` 串行化。发布 avail ring、增加 avail index、通知设备，以及读取 used ring/请求 status 的边界均使用 `fence iorw, iorw`。

`virtio_handle_irq` 先读取并 ACK VirtIO interrupt status，再读取 `used.index`，通过 I/O fence 后消费新 used entry。只有观察到完成 head id 为 0 才设置 `blk_done`、增加 block IRQ 计数并唤醒等待者；没有对应 used entry 的通知记为 spurious，而不会误报完成。

运行期 `read_write_disk` 在 `blk_irq_lock` 保护下提交请求，进程通过 `sleep_on(&blk_done, ...)` 阻塞。启动期还没有可睡眠的进程，因此 `fs_init` 发起的读请求通过“检查完成标志—打开中断—`wfi`—再次检查”等待；它同样不是轮询 used ring。

## 6. 功能二：SBI TIME 10 ms 用户态抢占

timer 使用 SBI v0.2 TIME 扩展，EID 为 `0x54494d45`、FID 为 0。RV32 把 64 位绝对 deadline 拆成 `a0=low32`、`a1=high32`；`timer_now` 用 `timeh/time/timeh` 重读，避免低 32 位回绕时读到撕裂值。

当前 QEMU `virt` 的 timebase 按 10 MHz 使用，因此 `TIMER_INTERVAL=100000` 对应 10 ms。每个 `struct cpu` 保存独立的 `next_timer`。中断处理先按旧 deadline 反复增加 interval，直到严格晚于当前时间，再调用 `sbi_set_timer`，最后增加 per-CPU/global 计数。这样先清除 pending 并重设下一次时钟，再决定是否调度。

首次进入用户态时，`enter_user` 设置 `sstatus.SPIE`，使 `sret` 后 S-mode interrupt 可投递。timer 数据流为：

```text
STIP -> kernel_entry 保存现场 -> timer_handle_irq 重设 deadline
     -> SPP==0 且存在当前进程 -> yield
     -> RUNNING→RUNNABLE -> per-CPU scheduler -> 恢复另一个进程
```

普通 S-mode 路径不被 timer 强制切换。因此长 syscall 可能推迟设备和 timer 处理，这是一项明确的教学型权衡，而非实时调度保证。

## 7. 功能三：具名有界管道

系统没有 `fork()`，匿名 pipe 创建后的 fd 无法通过继承交给子进程，所以用户 API 使用正整数 key 连接同一个具名 pipe：

```c
int pipe_open(int key, int mode);
int pipe_read(int fd, void *buf, int len);
int pipe_write(int fd, const void *buf, int len);
int pipe_close(int fd);
```

当前配置有 8 个全局 pipe、每进程 8 个 pipe fd，每个 pipe 是 256 字节环形缓冲区。`pipe_open` 要求访问模式恰好是 `PIPE_READ` 或 `PIPE_WRITE`，可额外带 `PIPE_CREATE`。`pipe_table_lock` 保护 key 查找和对象分配，每个 pipe 自己的锁保护缓冲区、端点计数与历史标志。

具体语义如下：

- read 最多返回当前已有字节数，因此允许 short read。
- write 最多写入当前剩余空间并立即返回实际字节数，因此允许 partial write；内核不保证 `PIPE_BUF` 记录原子性。`shell.c` 的 `write_all` 通过循环处理 short write。
- 空管道且 writer 从未出现时，reader 阻塞；writer 曾出现且全部关闭后，reader 先排空剩余数据，再返回 0 表示 EOF。
- 缓冲区满时 writer 阻塞。reader 从未出现时，writer 的首次 write 也会等待；reader 曾出现但全部关闭后，write 返回 `-32`，即 `-EPIPE`，没有 `SIGPIPE`。
- `pipe_close` 更新 reader/writer 引用并唤醒对端。最后一个 reader 和最后一个 writer 都关闭时，清空缓冲区、key 和历史状态，将对象标记为可复用。
- `proc_exit` 调用 `pipe_close_all`，避免正常退出路径留下管道引用；未处理的用户异常当前会触发内核 panic，不属于可恢复的进程退出路径。

syscall 在复制数据前通过 `user_access_ok` 逐页检查地址范围、溢出、`PAGE_U` 及读写权限。pipe 内只保存复制后的字节，不跨进程保存用户指针。

## 8. 功能四：1～4 hart SMP

### 8.1 启动协议

`run.sh` 用 `NCPU` 同时生成 `-DCPU_COUNT=N` 和 QEMU `-smp N`；编译期限制为 1～4。内核支持连续 hart ID 集合 `[0, NCPU)`，并不把 boot hart 写死为 0：`kernel_main(a0)` 记录 OpenSBI 传入的 `boot_hartid`，再通过 SBI HSM `hart_start` 启动集合中除 boot hart 外的每个 ID。

boot hart 独占 BSS 清零、allocator、页表、PLIC 全局配置、VirtIO 和文件系统初始化。`secondary_boot` 收到 hart ID 后选择自己的 32 KiB 启动/调度栈，显式建立内核 `gp`；`secondary_main` 安装 `tp`、内核页表、trap、PLIC context 和本地 timer。

每个次级 hart 设置 `online=true`，原子增加 `online_count`，并向 boot hart 发送 IPI。boot hart 在 `online_count == CPU_COUNT` 前通过 `wfi` 等待，之后才创建 shell 并进入调度。源码和链接脚本为最多四个 hart 预留了四份独立 32 KiB scheduler 栈。

### 8.2 Per-CPU 调度和每 PCB 锁

`struct cpu` 保存 `hartid`、当前进程、scheduler SP、下一 timer deadline、中断嵌套状态、online 状态和计数；内核通过可信 `tp` 实现 `mycpu()`，再由 `mycpu()->proc` 实现 `myproc()`。每个进程仍有自己的页表和 8 KiB 内核栈。

每个 scheduler 扫描共享 PCB 数组，并逐个取得 `proc->lock`。只有持有该 PCB 锁且观察到 `RUNNABLE` 的 hart 才能执行 `RUNNABLE -> RUNNING`，所以两个 hart 不会同时选中同一进程。锁跨过 process/scheduler context switch：首次运行由 `first_user_entry` 释放；之后 `yield`、`sleep_on` 或 `proc_exit` 持锁切回 scheduler，scheduler 完成状态收尾后释放。

调度器还检查 `running_on == -1`，违反时立即 panic，作为“同一 PCB 不得双核运行”的运行时不变量。切入进程页表时，`switch_address_space` 在当前 hart 设置 `SSTATUS_SUM`，允许已校验的 syscall 访问用户页；切回内核页表时清除它。这一点必须逐 hart 设置，否则未绑定进程换到另一 hart 后会在复制用户缓冲区时触发访问异常。

进程可设置 `affinity=-1` 表示任意 hart，也可绑定具体 hart；shell 固定在 boot hart，pipe stream writer 使用 `-1`，all-harts 和抢占 worker 则显式绑定。没有 per-CPU run queue、优先级、工作窃取或周期性迁移；未绑定任务只是被空闲 hart 从共享表取走，因此这是最小多核并行调度，不是完整负载均衡。

### 8.3 锁、中断嵌套与 IPI

自旋锁用原子 test-and-set 和全内存屏障实现。`acquire/release` 配合 `push_off/pop_off` 保存每 hart 的原始 SIE 与嵌套深度，防止本 hart 持锁时被 ISR 打断并再次获取同一把锁。

进程创建和 `wakeup` 会调用 `notify_schedulers`，通过 SBI IPI 向其他在线 hart 发送 software interrupt；handler 清除 `sip.SSIP` 后返回 scheduler 检查。设备 external IRQ 集中在 boot hart，但每个 hart 都有自己的 timer，因此所有 CPU 都能被时间片唤醒和抢占。

## 9. 关键不变量与数据流

| 不变量 | 破坏后的结果 |
|---|---|
| U-mode 时 `sscratch=kernel_sp`，S-mode 时为 0 | trap 切到错误栈或覆盖现场 |
| `kernel_sp+12` 的 CPU 指针由 scheduler 写入 | 用户可伪造 `tp` 并控制内核寻址 |
| 先保存用户 `gp`，再装载内核 `gp`，返回时恢复用户值 | C 代码访问错误的小数据区 |
| trap 来源只看保存的 `sstatus.SPP` | 合法用户 `sp` 值可能被误判为 S-mode |
| trap frame/context frame 保持 16 字节对齐 | 违反编译器 ABI，调用行为不可靠 |
| `RUNNABLE -> RUNNING` 必须持有该 PCB 锁 | 同一内核栈可能在两核同时执行 |
| 条件检查、登记 channel 和 BLOCKED 转换原子衔接 | 中断先唤醒、进程后睡眠，永久 lost wakeup |
| timer 先重设未来 deadline 再 `yield` | STIP 持续 pending 或时间片漂移 |
| VirtIO 发布/消费队列使用 I/O fence | 设备或 CPU 看到 index，却看不到描述符/used 数据 |
| VirtIO 先 ACK、PLIC 后 complete | 电平中断重复进入或后续 IRQ 无法投递 |
| 最后管道端点关闭时回收对象 | key 泄漏，8 个槽很快耗尽 |

四条核心数据流可以这样记：

```text
timer: STIP -> 保存 frame -> rearm -> U态进程 yield -> scheduler -> sret
UART:  SEIP -> claim 10 -> drain RX -> wakeup -> complete -> getchar 返回
disk:  submit -> BLOCKED/WFI -> claim 1 -> consume used id=0 -> wakeup -> resume
pipe:  empty/full -> sleep_on -> 对端 copy/close -> wakeup/IPI -> while 复查
```

## 10. 验证方法与实际证据

### 10.1 构建和 1/2/4 核验证

分别启动三次：

```sh
NCPU=1 bash run.sh
NCPU=2 bash run.sh
NCPU=4 bash run.sh
```

每次启动应出现对应的：

```text
smp: 1 hart(s) online
smp: 2 hart(s) online
smp: 4 hart(s) online
```

进入 shell 后执行：

```text
irqstat
selftest
irqstat
```

当前版本已经在 `NCPU=1/2/4` 下通过 `selftest`，成功标记为：

```text
SELFTEST PASS
```

`irqstat` 输出当前 hart、配置 CPU 数、全局 tick，以及 timer、UART、block 和 context-switch 计数。输入命令会使 UART 计数增长；`selftest` 后 block、timer 和 context-switch 应继续增长。退出 QEMU 后可检查：

```sh
rg "PANIC|unexpected trap|guest_errors" qemu.log
git status --short
```

预期没有 panic、未分类 trap 或 QEMU guest error；同时不要提交 `*.elf`、`*.bin`、`*.map`、`*.o`、`disk.tar` 和 `qemu.log` 等构建产物。

### 10.2 `selftest` 实际覆盖内容

| 子测试 | 当前操作 | 能证明什么 |
|---|---|---|
| timer | 循环查询 tick，要求发生变化 | SBI timer 已投递且计数前进 |
| block IRQ | 读取 `hello.txt`，把同样字节原样 `writefile` 回去 | `fs_flush` 触发 VirtIO 写且 block IRQ 计数增长；不证明重启持久化 |
| pipe stream | 未绑定 writer 以 127 字节块发送 3 KiB，reader 以 83 字节块读取 | short read/write 循环、环形回绕、阻塞唤醒、字节顺序、总长度、EOF，以及未绑定任务的调度路径 |
| preemption | quick 与无 yield 的 CPU hog 固定在同一 hart，要求 `Q` 先于 `H` | hog 的纯计算区间能被 timer 抢占 |
| all harts | 为每个配置 hart 启动一个 affinity worker | 1/2/4 hart 均上线，任务实际运行 hart 与绑定值一致且无重复结果 |

不要扩大测试结论：内核有 `running_on` 双运行断言，但自测没有专门注入冲突，也没有记录未绑定 writer 每次恢复所在的 hart，因此不能声称确定观察到进程迁移。当前自测也没有栈 canary/完整寄存器压力测试或关机重启后的持久化校验。

### 10.3 手工边界检查

还可手工执行 `hello`、`readfile`、`writefile` 验证基本 syscall。若继续补测试，优先覆盖 UART 超过 128 字节突发输入、reader 提前关闭后的 `-EPIPE`、8 个管道槽的关闭回收，以及大量空闲/唤醒循环；这些是当前实现的重要边界，但不属于现有 `selftest` 的成功标记。

## 11. 面试三分钟版本

可以直接按下面的话术展开：

> 这是一个运行在 QEMU virt 和 OpenSBI 上的 RV32 教学操作系统。基线已经有 Sv32、用户态、系统调用、进程切换和 VirtIO 文件系统，但设备依赖轮询，调度依赖进程主动让出，没有 IPC，也只跑单核。
>
> 我先重做统一 trap 基础，因为设备 IRQ、timer 和 SMP 都依赖正确的现场保存。入口保存 31 个通用寄存器和 4 个关键 CSR，总 frame 144 字节。U 态时 sscratch 保存进程内核栈顶，S 态时为 0；栈顶保留槽的加 12 位置存可信 CPU 指针。入口保存用户 gp/tp 后显式装载内核 gp，并依据 sstatus.SPP 判断 trap 来源，所以不会信任用户 tp 或用户 sp。
>
> 设备侧通过 PLIC 接入 UART RX 和 VirtIO block。UART ISR 排空 FIFO、写 128 字节 ring 并唤醒 getchar，溢出丢最旧字符；TX 为早期日志保留短轮询。VirtIO 使用设备 ACK 加 PLIC complete 两层确认，提交和 used ring 消费用 I/O fence。运行期进程睡眠，启动期还没有进程，所以用中断加 wfi 等待，而不是轮询 used ring。
>
> 调度侧通过 SBI TIME 给每个 hart 设置 10 ms 的绝对 deadline，只在 trap 来自用户态时把当前进程变回 RUNNABLE。因此用户任务可抢占，普通内核路径不可抢占。IPC 实现为 8 个具名、256 字节有界管道，支持 short read/write、阻塞唤醒、排空后 EOF；首次 reader 尚未出现时 write 等待，reader 已全部关闭时返回 -EPIPE，最后端点关闭后回收对象。
>
> 多核通过 SBI HSM 启动 1 到 4 个连续 hart，online_count 做启动汇合，每核有独立 32 KiB scheduler 栈、CPU 状态和 timer。调度器扫描共享 PCB 表，但每个 PCB 有自己的锁；只有持锁的 hart 能完成 RUNNABLE 到 RUNNING，所以同一进程不会被两个核同时选中。唤醒时用 SBI IPI 叫醒 wfi 的其他核。
>
> 验证上，我在 NCPU=1、2、4 下运行 selftest。它验证 tick 前进、原样 write/flush 触发 block IRQ、3 KiB 管道传输、同核无 yield hog 被抢占，以及每个 hart 的 affinity worker，最终都输出 SELFTEST PASS。边界是内核不抢占、单在途块请求、UART TX 轮询、没有 fork/完整 POSIX、虚拟内存回收和完整负载均衡。

## 12. 面试十分钟展开

### 12.1 第 0～1 分钟：先讲问题而非代码量

说明四项缺失并非孤立功能：中断是 timer 和异步 I/O 的基础；阻塞 I/O 需要睡眠/唤醒；SMP 会把原来隐含的单核原子性全部打破。因此实现顺序是 trap/锁基础 → PLIC 与设备 → timer 抢占 → 通用阻塞原语与 pipe → HSM/SMP → 1/2/4 核组合验证。

### 12.2 第 1～3 分钟：讲最难的 trap 信任边界

画出 U→S 的栈切换：`sscratch=kernel_sp`，交换后用户 `sp` 暂存在 CSR，frame 在内核栈向下生长。强调用户可修改 `gp/tp/sp`，所以必须先保存用户值，再建立内核 `gp`，并从 scheduler 写入的保留槽恢复可信 `tp`。解释为何用 `SPP` 判源、为何 S-mode 要令 `sscratch=0`、为何返回用户前重新安装 kernel stack top。

再区分 trap frame 和 context frame：前者面对任意异步点，必须保存所有可见寄存器；后者发生在 C 调用边界，只需保存 ABI callee-saved 集合。两者都保持 16 字节对齐。

### 12.3 第 3～5 分钟：讲中断和内存可见性

PLIC 路径按 claim → 设备处理/ACK → I/O fence → complete。UART RX ISR 做最少工作：排空 FIFO、入 ring、唤醒；读取 syscall 阻塞而不忙等。VirtIO 则重点讲 split queue 的发布关系：描述符和 avail entry 必须先于 index/notify 对设备可见；used index 之后必须先建立顺序，再读 used element 和 status。

说明当前把 external IRQ 集中到 boot hart，简化设备并发；timer 是 per-hart。块请求仅允许一个在途，用 `disk_mutex` 串行，因此完成 id 固定检查 0。启动阶段使用 `wfi` 是因为还没有 `struct process` 可以进入 `BLOCKED`。

### 12.4 第 5～7 分钟：讲调度与 lost wakeup

timer 使用绝对 deadline，ISR 先追赶到未来并 rearm，再在 U-mode 来源下 `yield`。普通内核不抢占，减少持锁中途切换的复杂度，但长 syscall 会增加延迟。

然后用 pipe 空读举例讲 lost wakeup：若“看到空”和“登记睡眠”之间释放锁，writer 可能先唤醒，reader 随后永久睡眠。当前 `sleep_on` 用“条件锁 + PCB 锁”的交接保证原子性，`wakeup` 只把状态改为 RUNNABLE，恢复后必须 `while` 复查。

### 12.5 第 7～9 分钟：讲 SMP 锁交接

OpenSBI 只把 boot hart 送入内核，boot hart 用 HSM 启动其余连续 ID，次级核建立自己的栈、`tp`、页表、trap 和 timer，再增加 `online_count`。每核 scheduler 扫描同一 PCB 表，但选择进程时持该 PCB 锁；该锁跨 context switch 交接，覆盖 RUNNABLE→RUNNING 和运行结束后的状态收尾。

明确说明这不是全局 `proc_lock`，也不是完整负载均衡：每 PCB 锁降低无关进程间的锁冲突，affinity 可绑定 hart，但没有 per-CPU run queue、优先级或工作窃取。IPI 只负责让空闲 hart 尽快离开 `wfi`。

### 12.6 第 9～10 分钟：用证据收尾

给出 `NCPU=1/2/4 bash run.sh`、`irqstat` 和 `selftest`。最强证据不是打印交替，而是 quick 与 hog 被固定在同一 hart，hog 的核心循环没有 syscall/yield，结果仍要求 `Q` 先于 `H`。pipe 用 3 KiB、不同读写块大小强制 short I/O 和环形回绕，writer 不绑定 hart；all-harts 测试验证绑定任务确实到达每个配置 hart。最后主动说明自测没有证明重启持久化、确定发生的跨 hart 迁移或完整寄存器 canary，体现对证据边界的控制。

## 13. 常见追问与回答要点

### 为什么 timer 走 SBI，不直接写 CLINT？

内核运行在 S-mode，机器级 timer 由 OpenSBI 管理。SBI TIME 是规范接口，可避免依赖某个 QEMU 版本的 CLINT 地址和 M-mode 细节。

### 为什么 RV32 读取时间要 `high/low/high`？

64 位 `time` 由两次 32 位读取组成。低位可能在两次读取之间回绕；只有前后高位相同，组合值才属于同一时刻。

### 为什么不能信任用户 `tp`，又为什么要显式建立 `gp`？

用户可以自由修改所有通用寄存器。若直接把用户 `tp` 当 CPU 指针，会把用户输入当内核地址；若沿用用户 `gp` 调 C，编译器对小数据区的访问会寻址错误。当前入口从内核栈保留槽恢复 `tp`，从链接器符号恢复 `gp`，返回前再还原用户值。

### 为什么依据 `SPP` 而不是 `sp` 判断 trap 来源？

`SPP` 是硬件记录的上一特权级；用户 `sp` 既不可信，也允许合法为 0。把某个寄存器值当来源标志会造成错误切栈。

### 有 timer 为什么不抢占内核？

内核抢占要求所有共享状态、锁顺序和中断上下文都支持任意点切换。教学实现只在 U-mode 边界抢占，仍能阻止 CPU hog 饿死其他用户进程，同时显著缩小并发状态空间。代价是长 syscall 延迟 IRQ。

### 如何避免 lost wakeup？

条件检查在条件锁内完成，`sleep_on` 在释放条件锁前先取得 PCB 锁，再登记 channel 和 BLOCKED 状态。生产者无法在“尚未登记”和“已经睡眠”之间穿过这个交接；恢复后仍以 `while` 复查。

### 为什么使用每 PCB 锁而不是全局 `proc_lock`？

选择和转换某个进程只需要串行该 PCB。两个 hart 可同时检查不同 PCB，减少全局锁竞争；同一 PCB 的 `RUNNABLE -> RUNNING` 仍是互斥的。代价是 `wakeup` 需要逐项加锁扫描，且锁交接更难解释。

### 为什么是具名 pipe？short write 怎么处理？

没有 `fork` 就没有 fd 继承，整数 key 让独立 spawn 的进程重新打开同一对象。pipe 返回实际写入字节数，用户库或应用循环到全部写完；当前不承诺 POSIX `PIPE_BUF` 原子性。

### EOF 与 `-EPIPE` 为什么需要 `had_reader/had_writer`？

引用数为 0 可能表示“对端尚未打开”，也可能表示“对端曾存在但已全部关闭”。历史位区分两者：writer 曾存在且清零后，排空数据得到 EOF；reader 从未出现时 write 等待，reader 曾存在且清零后，writer 得到 `-EPIPE`。

### 为什么 VirtIO 既 ACK 设备又 complete PLIC？

VirtIO ACK 清除设备内部中断原因，PLIC complete 结束中断控制器中的 claim。两者管理不同层级，不能互相替代。

### 为什么需要 `fence iorw, iorw`？

VirtIO 队列同时涉及普通内存中的 descriptor/ring 与 MMIO notify/ACK。仅有编译器 `volatile` 不能保证 CPU、设备观察顺序；I/O fence 明确排序内存和设备访问。

### 为什么设备 IRQ 只给 boot hart？

它避免多个 hart 同时操作单队列 VirtIO 和 UART FIFO，锁模型更容易验证。代价是 external IRQ 集中，boot hart 可能成为瓶颈；这是教学规模下的明确取舍。

### 这算负载均衡吗？

不算。未绑定任务可被任一空闲 hart 从共享 PCB 表取得，具备基本并行利用能力；但没有 per-CPU 队列长度比较、工作窃取、迁移成本、优先级或公平性策略。

## 14. 已知边界与后续改进方向

- 普通内核路径不可抢占；长 syscall 会推迟 timer 和设备 IRQ。
- 启动阶段 VirtIO 通过中断加 `wfi` 等待；运行期只有一个在途 block 请求，没有多队列、超时、重试、缓存或 DMA/IOMMU 隔离。
- UART RX 溢出丢最旧字符；TX 保留短轮询，没有 TX ring/THRE interrupt。
- 管道不是完整 POSIX：没有 `fork` 继承、`dup`、`select/poll`、权限命名空间、信号或 `PIPE_BUF` 原子保证。
- SMP 最多 4 个连续 hart；没有 hotplug、per-CPU run queue、工作窃取、优先级、完整负载均衡或饥饿控制。
- `send_ipi` 当前不检查 SBI 返回错误；“检查条件后打开中断再 `wfi`”存在刚好先处理唤醒的窗口，但周期 timer 会再次唤醒，表现为最多增加一个 tick 级延迟而非永久 lost wakeup。
- 地址空间映射基本静态，没有跨 hart TLB shootdown/SBI RFENCE；若未来动态解除其他 hart 正在使用的映射，必须补齐。
- 没有 `fork/exec`、写时复制、按需分页、swap、物理页和页表完整回收或 OOM 恢复；PCB 复用只在镜像大小相同时复用已有用户页。
- 文件系统仍是小型内存 ustar 模型。`writefile` 会 `fs_flush` 到块设备，但当前自测没有重启读取，因此不能声称已验证崩溃一致性或持久化。
- UART/PLIC/VirtIO 地址和 10 MHz timebase 绑定当前 QEMU `virt` 配置，没有解析 FDT，移植时需要替换这些常量。
- 内核已有 `running_on` 双运行断言，但当前自测没有故障注入、栈 canary、完整寄存器压力或确定性迁移轨迹；这些适合作为进一步强化验证，而不是现有自测的已通过能力。
