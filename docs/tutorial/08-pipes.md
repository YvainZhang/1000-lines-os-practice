# 08 扩展三：用具名管道连接进程

[上一课](07-preemption.md) · [课程目录](README.md) · [下一课](09-smp.md)

## 要解决的问题

进程有独立地址空间，传一个指针并不会让另一个进程获得对应数据。目标是通过内核维护的有限字节缓冲通信：无数据时 reader 睡眠，无空间时 writer 睡眠，对端读写或关闭后唤醒。

阅读 [kernel.c](../../kernel.c) 的 `struct pipe`、`pipe_open_sys`、`pipe_read_sys`、`pipe_write_sys`、`pipe_close_fd`；再读 [user.c](../../user.c) 的封装和 [shell.c](../../shell.c) 的 `write_all`、`read_exact`、`stream_writer`、`test_pipe_stream`。

## 第一步：定义对象和连接方式

当前没有 `fork` 和 fd 继承，因而使用正整数 key 查找同一个内核管道。reader 与 writer 分别打开同一个 key，得到各自进程内的 fd。fd 数值不必相同，也不能把一个进程的 fd 直接当作另一个进程的句柄。

有 8 个全局管道对象、每进程 8 个 fd，每个管道容量 256 字节。`read_pos`、`write_pos` 取模回绕，`count` 区分满与空；不变量为 `0 <= count <= PIPE_CAPACITY`。

## 第二步：实现有限缓冲的读写

reader 持有 pipe 锁检查 count，无数据时结合 writer 状态决定睡眠或 EOF。有数据时最多复制当前已有字节，然后唤醒等待空间的 writer。writer 对称地等待 reader 或空闲空间，最多写入当前可用容量。

一次成功调用可以只传输部分数据。发送 3072 字节不能假设一次 `pipe_write` 返回 3072；`write_all` 必须前移指针、减少剩余长度并处理错误。内核在复制前逐页验证用户缓冲区，管道只保存字节，不保存用户指针。

## 第三步：关闭也是协议的一部分

| 条件 | 行为 |
| --- | --- |
| 空缓冲，writer 还从未出现 | reader 等待连接 |
| writer 曾出现、现在全关，仍有数据 | 先读完剩余数据 |
| writer 曾出现、现在全关，已空 | read 返回 0，即 EOF |
| reader 曾出现、现在全关 | write 返回 -32，即断管错误 |
| reader、writer 全部关闭 | 对象清空，key 与槽位可复用 |

`had_reader/had_writer` 区分“还没连接”和“连接后已断开”。`proc_exit` 关闭该进程的所有 pipe fd。当前不提供 socket、共享内存、SIGPIPE 或 POSIX 记录原子性。

## 实验一：跨多次回绕传输

运行 `bash run.sh` 后输入 `selftest`，要求出现 `[PASS] pipe stream: 3072 bytes and EOF`。定位 writer 的 127 字节发送块和 reader 的 83 字节接收块，手算为什么数据边界不对齐；校验按整个流的偏移比较，不要求一次读恰好对应一次写。

加做：把 `PIPE_CAPACITY` 暂改为 64，保持总数据量不变，再运行自测。预期数据仍完整，读写循环要适应更多次阻塞与回绕。不要预设上下文切换次数必然增加固定倍数。恢复 256 后再回归。

## 实验二：补一个断管测试

在 `shell.c` 中自行增加测试函数，并从 `run_selftest` 调用。下面是步骤草图，不是已接入的函数：

```text
r = pipe_open(新的正整数 key, PIPE_READ | PIPE_CREATE)
w = pipe_open(同一个 key, PIPE_WRITE)
检查 r、w 成功
pipe_close(r)
尝试向 w 写 1 字节，要求返回 -32
pipe_close(w)
```

reader 必须先存在再关闭，否则 writer 会等待首次连接。所有失败路径都要关闭已打开的 fd。验收需要实际输出与重复运行结果；当前已有 `SELFTEST PASS` 不替代这项新增测试。

## 验收与思考

<details><summary>为什么空管道不能永远返回 0？</summary>

0 在这里表示流已结束。如果 writer 只是尚未写入数据，返回 0 会让 reader 误认为 EOF，提前丢弃后续流。应等待数据或明确的端点关闭事件，再决定结果。

</details>
