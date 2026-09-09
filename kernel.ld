ENTRY(boot)

SECTIONS {
    . = 0x80200000;
    __kernel_base = .;

    .text :{
        KEEP(*(.text.boot));
        *(.text .text.*);
    }

    .rodata : ALIGN(4) {
        *(.rodata .rodata.*);
    }

    .data : ALIGN(4) {
        *(.data .data.*);
        PROVIDE(__global_pointer$ = . + 0x800);
        *(.sdata .sdata.*);
    }

    .bss : ALIGN(4) {
        __bss = .;
        *(.bss .bss.* .sbss .sbss.*);
        __bss_end = .;
    }

    /* Four independent 32KiB boot/scheduler stacks, indexed by hart ID. */
    . = ALIGN(16);
    .stack (NOLOAD) : ALIGN(16) {
        __stack_bottom = .;
        . += 4 * 32 * 1024;
        __stack_top = .;
    }

    . = ALIGN(4096);
    __free_ram = .;
    . += 64 * 1024 * 1024; /* 64MB */
    __free_ram_end = .;
}
