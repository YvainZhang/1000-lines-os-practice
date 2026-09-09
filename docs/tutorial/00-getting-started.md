# 00 环境与第一次启动

[课程目录](README.md) · [下一课：启动与链接](01-boot.md)

## 本课目标

启动当前内核，知道终端里的每段输出由谁产生，并保存第一次自测结果。先不修改内核。

## 先建立运行模型

宿主机运行编译器和 QEMU；编译器输出 RV32 程序，QEMU 模拟 `virt` 机器。OpenSBI 是 M-mode 固件，随后进入 S-mode 内核；shell 在 U-mode 中通过系统调用请求内核服务。宿主机终端承载模拟串口，终端上显示文字不代表程序运行在宿主机上。

## 操作步骤

1. 按[项目首页](../../README.md)安装 LLVM 和 QEMU。查看 [run.sh](../../run.sh)，找出 `CC`、`OBJCOPY`、`QEMU` 和 `NCPU` 的默认值。
2. 在仓库根目录运行 `NCPU=1 bash run.sh`。先用单核减少同时发生的事件。
3. 确认依次出现 OpenSBI 信息、内核启动信息、`smp: 1 hart(s) online` 和 shell 提示符。
4. 依次输入 `help`、`hello`、`irqstat`、`selftest`。预期看到 `Hello world from shell!` 和最终 `SELFTEST PASS`。
5. 按 Ctrl-a，再按 x 退出 QEMU。`exit` 只结束 shell，不关闭模拟器。

可选自动复现：

```sh
python3 scripts/smoke.py --cpus 1
```

日志保存在 `test-results/smoke-1.log`；不要直接在仍占用磁盘镜像的 QEMU 旁运行它。

## 动手练习

记录编译器版本、QEMU 版本、启动核数和完整自测输出。将每段启动输出标成“构建脚本 / 固件 / 内核 / 用户程序”四类。观察 `irqstat` 两次输出，说明为什么不应要求数值固定。

## 常见问题

| 现象 | 从哪里查 |
| --- | --- |
| 找不到 clang / llvm-objcopy | `run.sh` 的工具路径和环境变量覆盖 |
| 找不到 QEMU | `QEMU` 配置以及 PATH |
| 镜像无法锁定 | 是否已有另一个实例使用 `disk.tar` |
| 只有固件输出 | 编译结果、`kernel.ld` 入口和 QEMU 内核加载参数 |
| 自测挂住 | 保留日志，区分停在启动、某项测试还是退出阶段 |

## 验收与思考

能够独立完成启动、自测和退出，并回答：RV32 指令实际由谁执行？为什么 shell 退出后 QEMU 还在？

<details><summary>参考要点</summary>

QEMU 模拟 RV32 CPU；shell 是客体内的一个进程。进程退出让调度器继续运行或空闲，不等于执行模拟机器关机。

</details>
