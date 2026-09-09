# 验证记录

2026-09-09，在提交前运行 `python3 scripts/smoke.py`。本记录对应本次提交的源码快照，不代表已发布版本。

环境：macOS arm64，Homebrew Clang 23.1.0，QEMU 11.1.1，OpenSBI 1.8.1，Python 3.14.7；目标 RV32 / QEMU `virt`。

| 配置 | 编译、启动 | shell / selftest / irqstat | 串口证据 |
| --- | --- | --- | --- |
| NCPU=1 | 通过，无编译警告 | 通过 | `test-results/smoke-1.log` |
| NCPU=2 | 通过，无编译警告 | 通过 | `test-results/smoke-2.log` |
| NCPU=4 | 通过，无编译警告 | 通过 | `test-results/smoke-4.log` |

三种配置均输出以下结果，最后一项核数随配置变化：

```text
[PASS] timer advances
[PASS] VirtIO block interrupt
[PASS] pipe stream: 3072 bytes and EOF
[PASS] same-hart user preemption
[PASS] worker affinity on all 4 hart(s)
SELFTEST PASS
```

`help`、`hello` 和中断统计输出通过串口匹配检查。正常输入下 `uart-rx-dropped=0`；这个字段读取受 RX 锁保护的丢弃计数，用于诊断接收缓冲溢出，不是中断次数。脚本发送 `exit` 后退出 QEMU，但串口回显不单独证明进程资源已回收。

日志属于本地生成产物，已忽略，不随源码提交。复现命令会重建根目录 ELF 与 `disk.tar`，不要和交互式 QEMU 实例同时运行。每次运行会覆盖同配置日志。

## 结论边界

- 检验的是功能行为，不是吞吐、延迟、公平性或长期稳定性。
- 通过同核 quick/hog 返回次序检验用户态抢占；内核路径仍不可抢占。
- 管道自测覆盖数据、回绕、阻塞和 EOF；未覆盖全部断管、资源耗尽和并发关闭组合。
- SMP 验证各核 affinity，不证明确定发生迁移或实现完整负载均衡。
- 文件测试通过原样写回来触发块设备 IRQ，不证明断电恢复和重启持久化。
- 本轮未测试 3 核、真实硬件或非 macOS 宿主环境。

`git diff --check` 通过。后续修改内核同步、trap 或 syscall 后，应重新运行回归并更新本记录。
