# 04 从文件名到磁盘请求

[上一课](03-processes.md) · [课程目录](README.md) · [下一课](05-traps-and-sleep.md)

## 本课目标

区分文件接口、内存文件对象和块设备请求。阅读 [kernel.c](../../kernel.c) 的 `fs_init`、`fs_lookup`、`fs_flush`、`read_write_disk`，以及 [kernel.h](../../kernel.h) 的 `tar_header`、`file`、`virtio_blk_req`。

## 三层数据模型

用户程序使用文件名；文件系统在内存里保存文件内容；VirtIO 处理以扇区为单位的请求。启动时 `run.sh` 把 `disk/*.txt` 打成 ustar 镜像，`fs_init` 从块设备读入并解析为内存文件对象。

当前 `readfile` 从内存对象复制数据，不意味着每次都读取块设备。`writefile` 更新内存对象后调用 `fs_flush`，重新组织 tar 数据并写出扇区。实现限制为最多两个文件、每个文件的数据数组 1024 字节，没有通用目录树、动态文件创建或日志恢复。

VirtIO 请求使用三个串联描述符：请求头、512 字节数据、一个状态字节。读写方向以设备视角描述；磁盘读操作中，设备会写入客体的数据缓冲区。队列通知不等于请求已经完成。

## 动手练习

启动 `NCPU=1 bash run.sh`，按顺序输入：

```text
readfile
irqstat
writefile
readfile
irqstat
selftest
```

预期写后读能看到示例文本，写回使 block 完成计数增加。不要要求每条 shell 命令恰好增加一个中断：文件写回可能包含多个扇区请求。

退出 QEMU，重新运行 `run.sh` 后再次读取。解释为什么会重新得到 `disk/hello.txt` 的输入内容：脚本每次重建镜像。因此这一操作不是检验磁盘持久化的方法。

## 验收与思考

把 `writefile` 路径画成“syscall → 内存对象 → tar 打包 → 描述符 → 完成等待”。为什么 `test_block_irq` 会把读到的数据原样写回？

<details><summary>参考要点</summary>

只读内存文件不能证明运行期设备完成中断发生；原样写回触发真实块请求，同时不改变文件的逻辑内容。它仍不能证明掉电恢复、崩溃一致性或重启持久化。

</details>

本课先把“等待完成”作为驱动步骤理解。第 05～06 课再拆解它如何通过锁、中断与唤醒完成。
