#!/bin/bash
set -xue

NCPU=${NCPU:-2}
case "$NCPU" in
    1|2|3|4) ;;
    *)
        echo "NCPU must be an integer between 1 and 4 (got: $NCPU)" >&2
        exit 2
        ;;
esac

LLVM_BIN=/opt/homebrew/opt/llvm/bin

if [ -z "${CC:-}" ]; then
    if [ -x "$LLVM_BIN/clang" ]; then
        CC="$LLVM_BIN/clang"
    else
        CC=clang
    fi
fi

if [ -z "${OBJCOPY:-}" ]; then
    if [ -x "$LLVM_BIN/llvm-objcopy" ]; then
        OBJCOPY="$LLVM_BIN/llvm-objcopy"
    else
        OBJCOPY=llvm-objcopy
    fi
fi

QEMU=${QEMU:-qemu-system-riscv32}

# clang 路径和编译器标志
CFLAGS=(
    -std=c11
    -O2
    -g3
    -Wall
    -Wextra
    --target=riscv32-unknown-elf
    -march=rv32imac_zicsr_zifencei
    -mabi=ilp32
    -fno-stack-protector
    -ffreestanding
    -nostdlib
    -fuse-ld=lld
    "-DCPU_COUNT=$NCPU"
)

# Build the shell (application)
"$CC" "${CFLAGS[@]}" -Wl,-Tuser.ld -Wl,-Map=shell.map \
    -o shell.elf shell.c user.c common.c
"$OBJCOPY" --set-section-flags .bss=alloc,contents -O binary shell.elf shell.bin
"$OBJCOPY" -Ibinary -Oelf32-littleriscv shell.bin shell.bin.o

(cd disk && tar cf ../disk.tar --format=ustar *.txt)

# 构建内核
"$CC" "${CFLAGS[@]}" -Wl,-Tkernel.ld -Wl,-Map=kernel.map -o kernel.elf \
    kernel.c common.c shell.bin.o

# 启动 QEMU
"$QEMU" -machine virt -smp "$NCPU" -bios default \
    -nographic -serial mon:stdio --no-reboot \
    -d unimp,guest_errors,int,cpu_reset -D qemu.log \
    -drive id=drive0,file=disk.tar,format=raw,if=none \
    -device virtio-blk-device,drive=drive0,bus=virtio-mmio-bus.0 \
    -kernel kernel.elf
