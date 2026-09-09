#include "kernel.h"

#ifndef CPU_COUNT
#define CPU_COUNT 2
#endif

#define CPUS_MAX          4
#define PROCS_MAX         16
#define PROC_UNUSED       0
#define PROC_EMBRYO       1
#define PROC_RUNNABLE     2
#define PROC_RUNNING      3
#define PROC_BLOCKED      4
#define PROC_EXITED       5

#define PROCESS_STACK_SIZE 8192
#define PROCESS_STACK_RESERVED 16
#define PROCESS_FDS_MAX    8
#define PIPES_MAX          8
#define PIPE_CAPACITY      256

#define UART0_PADDR        0x10000000
#define UART_RHR           0
#define UART_THR           0
#define UART_IER           1
#define UART_FCR           2
#define UART_LSR           5
#define UART_LSR_RX_READY  (1 << 0)
#define UART_LSR_TX_IDLE   (1 << 5)
#define UART_IRQ           10
#define UART_RX_CAPACITY   128

#define PLIC_PADDR         0x0c000000
#define PLIC_ENABLE_BASE   (PLIC_PADDR + 0x2000)
#define PLIC_CONTEXT_BASE  (PLIC_PADDR + 0x200000)
#define VIRTIO_BLK_IRQ     1

#define SBI_EXT_TIME       0x54494d45
#define SBI_EXT_HSM        0x48534d
#define SBI_EXT_IPI        0x735049
#define SBI_HSM_HART_START 0
#define TIMER_INTERVAL     100000

#if CPU_COUNT < 1 || CPU_COUNT > CPUS_MAX
#error "CPU_COUNT must be between 1 and 4"
#endif

_Static_assert(sizeof(struct trap_frame) == 144,
               "trap frame must preserve 16-byte stack alignment");

extern char _binary_shell_bin_start[], _binary_shell_bin_size[];
extern char __kernel_base[], __free_ram[], __free_ram_end[];
extern char __bss[], __bss_end[], __stack_top[];

struct spinlock {
    volatile uint32_t locked;
};

struct mutex {
    struct spinlock guard;
    bool locked;
};

struct pipe_fd {
    int pipe_index;
    int mode;
};

struct process {
    struct spinlock lock;
    int pid;
    int state;
    int affinity;
    int running_on;
    vaddr_t sp;
    vaddr_t entry;
    vaddr_t user_sp;
    size_t image_size;
    uint32_t initial_arg;
    uint32_t *page_table;
    void *wait_channel;
    struct pipe_fd fds[PROCESS_FDS_MAX];
    uint8_t stack[PROCESS_STACK_SIZE] __attribute__((aligned(16)));
};

struct cpu {
    uint32_t hartid;
    struct process *proc;
    vaddr_t scheduler_sp;
    uint64_t next_timer;
    int noff;
    bool intena;
    bool online;
    uint32_t ticks;
    uint32_t schedules;
};

struct pipe {
    struct spinlock lock;
    bool in_use;
    int key;
    uint8_t data[PIPE_CAPACITY];
    unsigned read_pos;
    unsigned write_pos;
    unsigned count;
    unsigned readers;
    unsigned writers;
    bool had_reader;
    bool had_writer;
};

static struct cpu cpus[CPUS_MAX];
static struct process procs[PROCS_MAX];
static struct pipe pipes[PIPES_MAX];
static struct spinlock pipe_table_lock;
static volatile uint32_t online_count;
static uint32_t boot_hartid;
static volatile uint32_t next_pid = 1;
static uint32_t *kernel_page_table;

static volatile uint32_t timer_irq_count;
static volatile uint32_t uart_irq_count;
static volatile uint32_t block_irq_count;
static volatile uint32_t context_switch_count;
static volatile uint32_t spurious_irq_count;

void yield(void);
void wakeup(void *channel);
void read_write_disk(void *buf, unsigned sector, int is_write);
struct file *fs_lookup(const char *filename);
void fs_flush(void);
static bool fs_can_resize(struct file *target, size_t new_size);
void virtio_handle_irq(void);
void kernel_entry(void);
void secondary_boot(void);

static struct cpu *mycpu(void) {
    struct cpu *cpu;
    __asm__ __volatile__("mv %0, tp" : "=r"(cpu));
    return cpu;
}

static struct process *myproc(void) {
    return mycpu()->proc;
}

static bool intr_get(void) {
    return (READ_CSR(sstatus) & SSTATUS_SIE) != 0;
}

static void intr_on(void) {
    SET_CSR(sstatus, SSTATUS_SIE);
}

static void intr_off(void) {
    CLEAR_CSR(sstatus, SSTATUS_SIE);
}

static void push_off(void) {
    bool old = intr_get();
    intr_off();
    struct cpu *cpu = mycpu();
    if (cpu->noff == 0)
        cpu->intena = old;
    cpu->noff++;
}

static void pop_off(void) {
    struct cpu *cpu = mycpu();
    if (intr_get() || cpu->noff < 1)
        PANIC("invalid interrupt nesting");
    cpu->noff--;
    if (cpu->noff == 0 && cpu->intena)
        intr_on();
}

static void acquire(struct spinlock *lock) {
    push_off();
    while (__sync_lock_test_and_set(&lock->locked, 1)) {
        while (lock->locked)
            __asm__ __volatile__("nop");
    }
    __sync_synchronize();
}

static void release(struct spinlock *lock) {
    __sync_synchronize();
    __sync_lock_release(&lock->locked);
    pop_off();
}

static volatile uint8_t *uart_reg(unsigned offset) {
    return (volatile uint8_t *) (UART0_PADDR + offset);
}

static struct spinlock uart_tx_lock;
static struct spinlock uart_rx_lock;
static uint8_t uart_rx_buf[UART_RX_CAPACITY];
static unsigned uart_rx_read;
static unsigned uart_rx_write;
static unsigned uart_rx_count;
static uint32_t uart_rx_dropped;

static void uart_init(void) {
    *uart_reg(UART_IER) = 0;
    *uart_reg(UART_FCR) = 1;
    *uart_reg(UART_IER) = 1;
}

void putchar(char ch) {
    acquire(&uart_tx_lock);
    while ((*uart_reg(UART_LSR) & UART_LSR_TX_IDLE) == 0)
        ;
    *uart_reg(UART_THR) = ch;
    release(&uart_tx_lock);
}

static void uart_handle_irq(void) {
    acquire(&uart_rx_lock);
    while ((*uart_reg(UART_LSR) & UART_LSR_RX_READY) != 0) {
        uint8_t ch = *uart_reg(UART_RHR);
        if (uart_rx_count == UART_RX_CAPACITY) {
            uart_rx_read = (uart_rx_read + 1) % UART_RX_CAPACITY;
            uart_rx_count--;
            uart_rx_dropped++;
        }
        uart_rx_buf[uart_rx_write] = ch;
        uart_rx_write = (uart_rx_write + 1) % UART_RX_CAPACITY;
        uart_rx_count++;
    }
    wakeup(&uart_rx_count);
    release(&uart_rx_lock);
    __sync_fetch_and_add(&uart_irq_count, 1);
}

struct sbiret sbi_call(long arg0, long arg1, long arg2, long arg3, long arg4,
                       long arg5, long fid, long eid) {
    register long a0 __asm__("a0") = arg0;
    register long a1 __asm__("a1") = arg1;
    register long a2 __asm__("a2") = arg2;
    register long a3 __asm__("a3") = arg3;
    register long a4 __asm__("a4") = arg4;
    register long a5 __asm__("a5") = arg5;
    register long a6 __asm__("a6") = fid;
    register long a7 __asm__("a7") = eid;

    __asm__ __volatile__("ecall"
                         : "+r"(a0), "+r"(a1)
                         : "r"(a2), "r"(a3), "r"(a4), "r"(a5), "r"(a6),
                           "r"(a7)
                         : "memory");
    return (struct sbiret){.error = a0, .value = a1};
}

static uint64_t timer_now(void) {
    uint32_t hi0;
    uint32_t lo;
    uint32_t hi1;
    do {
        hi0 = READ_CSR(timeh);
        lo = READ_CSR(time);
        hi1 = READ_CSR(timeh);
    } while (hi0 != hi1);
    return ((uint64_t) hi0 << 32) | lo;
}

static void sbi_set_timer(uint64_t deadline) {
    struct sbiret ret = sbi_call((uint32_t) deadline, (uint32_t) (deadline >> 32),
                                 0, 0, 0, 0, 0, SBI_EXT_TIME);
    if (ret.error)
        PANIC("SBI TIME failed: %d", ret.error);
}

static void timer_init(void) {
    struct cpu *cpu = mycpu();
    cpu->next_timer = timer_now() + TIMER_INTERVAL;
    sbi_set_timer(cpu->next_timer);
}

static void timer_handle_irq(void) {
    struct cpu *cpu = mycpu();
    uint64_t now = timer_now();
    do {
        cpu->next_timer += TIMER_INTERVAL;
    } while (cpu->next_timer <= now);
    sbi_set_timer(cpu->next_timer);
    cpu->ticks++;
    __sync_fetch_and_add(&timer_irq_count, 1);
}

static void send_ipi(uint32_t hart_mask) {
    if (hart_mask == 0)
        return;
    sbi_call(hart_mask, 0, 0, 0, 0, 0, 0, SBI_EXT_IPI);
}

static void notify_schedulers(void) {
    uint32_t mask = 0;
    for (int i = 0; i < CPU_COUNT; i++) {
        if (cpus[i].online && &cpus[i] != mycpu())
            mask |= 1u << i;
    }
    send_ipi(mask);
}

static struct spinlock alloc_lock;
static paddr_t next_paddr;

static void alloc_init(void) {
    next_paddr = (paddr_t) __free_ram;
}

paddr_t alloc_pages(uint32_t n) {
    acquire(&alloc_lock);
    paddr_t paddr = next_paddr;
    paddr_t next = paddr + n * PAGE_SIZE;
    if (next < paddr || next > (paddr_t) __free_ram_end) {
        release(&alloc_lock);
        PANIC("out of memory");
    }
    next_paddr = next;
    release(&alloc_lock);

    memset((void *) paddr, 0, n * PAGE_SIZE);
    return paddr;
}

void map_page(uint32_t *table1, uint32_t vaddr, paddr_t paddr, uint32_t flags) {
    if (!is_aligned(vaddr, PAGE_SIZE) || !is_aligned(paddr, PAGE_SIZE))
        PANIC("unaligned mapping va=%x pa=%x", vaddr, paddr);

    uint32_t vpn1 = (vaddr >> 22) & 0x3ff;
    if ((table1[vpn1] & PAGE_V) == 0) {
        uint32_t pt_paddr = alloc_pages(1);
        table1[vpn1] = ((pt_paddr / PAGE_SIZE) << 10) | PAGE_V;
    }

    uint32_t vpn0 = (vaddr >> 12) & 0x3ff;
    uint32_t *table0 = (uint32_t *) ((table1[vpn1] >> 10) * PAGE_SIZE);
    table0[vpn0] = ((paddr / PAGE_SIZE) << 10) | flags | PAGE_V;
}

static void map_mmio_page(uint32_t *page_table, paddr_t paddr) {
    map_page(page_table, paddr, paddr, PAGE_R | PAGE_W);
}

static void map_kernel_space(uint32_t *page_table) {
    for (paddr_t paddr = (paddr_t) __kernel_base;
         paddr < (paddr_t) __free_ram_end; paddr += PAGE_SIZE)
        map_page(page_table, paddr, paddr, PAGE_R | PAGE_W | PAGE_X);

    map_mmio_page(page_table, UART0_PADDR);
    map_mmio_page(page_table, VIRTIO_BLK_PADDR);
    map_mmio_page(page_table, PLIC_PADDR);
    map_mmio_page(page_table, PLIC_ENABLE_BASE);
    for (int hart = 0; hart < CPUS_MAX; hart++) {
        uint32_t context = 2 * hart + 1;
        map_mmio_page(page_table, PLIC_CONTEXT_BASE + context * PAGE_SIZE);
    }
}

static uint32_t *create_page_table(void) {
    uint32_t *page_table = (uint32_t *) alloc_pages(1);
    map_kernel_space(page_table);
    return page_table;
}

static uint32_t *walk_pte(uint32_t *table1, vaddr_t vaddr) {
    uint32_t pte1 = table1[(vaddr >> 22) & 0x3ff];
    if ((pte1 & PAGE_V) == 0)
        return NULL;
    uint32_t *table0 = (uint32_t *) ((pte1 >> 10) * PAGE_SIZE);
    return &table0[(vaddr >> 12) & 0x3ff];
}

static bool user_access_ok(struct process *proc, vaddr_t addr, int len,
                           bool write) {
    if (len < 0)
        return false;
    if (len == 0)
        return true;
    uint32_t last = addr + (uint32_t) len - 1;
    if (addr < USER_BASE || last < addr)
        return false;

    for (uint32_t page = addr & ~(PAGE_SIZE - 1);;
         page += PAGE_SIZE) {
        uint32_t *pte = walk_pte(proc->page_table, page);
        uint32_t required = PAGE_V | PAGE_U | (write ? PAGE_W : PAGE_R);
        if (!pte || (*pte & required) != required)
            return false;
        if (page == (last & ~(PAGE_SIZE - 1)))
            break;
    }
    return true;
}

static int copy_user_string(char *dst, int dst_size, const char *src) {
    for (int i = 0; i < dst_size; i++) {
        if (!user_access_ok(myproc(), (vaddr_t) &src[i], 1, false))
            return -1;
        dst[i] = src[i];
        if (dst[i] == '\0')
            return 0;
    }
    dst[dst_size - 1] = '\0';
    return -1;
}

static vaddr_t proc_kernel_stack_top(struct process *proc) {
    return (vaddr_t) &proc->stack[PROCESS_STACK_SIZE - PROCESS_STACK_RESERVED];
}

__attribute__((naked))
void switch_context(uint32_t *prev_sp, uint32_t *next_sp) {
    __asm__ __volatile__(
        "addi sp, sp, -16 * 4\n"
        "sw ra,  0  * 4(sp)\n"
        "sw s0,  1  * 4(sp)\n"
        "sw s1,  2  * 4(sp)\n"
        "sw s2,  3  * 4(sp)\n"
        "sw s3,  4  * 4(sp)\n"
        "sw s4,  5  * 4(sp)\n"
        "sw s5,  6  * 4(sp)\n"
        "sw s6,  7  * 4(sp)\n"
        "sw s7,  8  * 4(sp)\n"
        "sw s8,  9  * 4(sp)\n"
        "sw s9,  10 * 4(sp)\n"
        "sw s10, 11 * 4(sp)\n"
        "sw s11, 12 * 4(sp)\n"
        "sw sp, (a0)\n"
        "lw sp, (a1)\n"
        "lw ra,  0  * 4(sp)\n"
        "lw s0,  1  * 4(sp)\n"
        "lw s1,  2  * 4(sp)\n"
        "lw s2,  3  * 4(sp)\n"
        "lw s3,  4  * 4(sp)\n"
        "lw s4,  5  * 4(sp)\n"
        "lw s5,  6  * 4(sp)\n"
        "lw s6,  7  * 4(sp)\n"
        "lw s7,  8  * 4(sp)\n"
        "lw s8,  9  * 4(sp)\n"
        "lw s9,  10 * 4(sp)\n"
        "lw s10, 11 * 4(sp)\n"
        "lw s11, 12 * 4(sp)\n"
        "addi sp, sp, 16 * 4\n"
        "ret\n"
    );
}

__attribute__((naked, noreturn))
static void enter_user(vaddr_t entry, uint32_t arg, vaddr_t user_sp,
                       vaddr_t kernel_sp) {
    __asm__ __volatile__(
        "csrw sepc, a0\n"
        "csrw sscratch, a3\n"
        "li t0, %[sstatus]\n"
        "csrw sstatus, t0\n"
        "mv sp, a2\n"
        "mv a0, a1\n"
        "sret\n"
        :
        : [sstatus] "i" (SSTATUS_SPIE | SSTATUS_SUM)
    );
}

static void first_user_entry(void) {
    struct process *proc = myproc();
    vaddr_t entry = proc->entry;
    uint32_t arg = proc->initial_arg;
    vaddr_t user_sp = proc->user_sp;
    vaddr_t kernel_sp = proc_kernel_stack_top(proc);
    release(&proc->lock);
    enter_user(entry, arg, user_sp, kernel_sp);
}

static int create_process(const void *image, size_t image_size,
                          vaddr_t entry, uint32_t arg, int affinity) {
    struct process *proc = NULL;
    for (int i = 0; i < PROCS_MAX; i++) {
        acquire(&procs[i].lock);
        if (procs[i].state == PROC_UNUSED) {
            proc = &procs[i];
            proc->state = PROC_EMBRYO;
            break;
        }
        release(&procs[i].lock);
    }
    if (!proc)
        return -1;

    proc->pid = __sync_fetch_and_add(&next_pid, 1);
    proc->affinity = affinity;
    proc->running_on = -1;
    proc->entry = entry;
    proc->initial_arg = arg;
    proc->user_sp = (USER_BASE + image_size) & ~15u;
    proc->wait_channel = NULL;
    for (int i = 0; i < PROCESS_FDS_MAX; i++) {
        proc->fds[i].pipe_index = -1;
        proc->fds[i].mode = 0;
    }
    const uint8_t *src = (const uint8_t *) image;
    bool reuse_image = proc->page_table && proc->image_size == image_size;
    if (!reuse_image)
        proc->page_table = create_page_table();
    for (uint32_t off = 0; off < image_size; off += PAGE_SIZE) {
        paddr_t page;
        if (reuse_image) {
            uint32_t *pte = walk_pte(proc->page_table, USER_BASE + off);
            if (!pte || (*pte & (PAGE_V | PAGE_U)) != (PAGE_V | PAGE_U))
                PANIC("reused process has invalid user mapping");
            page = (*pte >> 10) * PAGE_SIZE;
            memset((void *) page, 0, PAGE_SIZE);
        } else {
            page = alloc_pages(1);
            map_page(proc->page_table, USER_BASE + off, page,
                     PAGE_U | PAGE_R | PAGE_W | PAGE_X);
        }
        size_t remaining = image_size - off;
        size_t copy_size = PAGE_SIZE <= remaining ? PAGE_SIZE : remaining;
        memcpy((void *) page, src + off, copy_size);
    }
    proc->image_size = image_size;

    memset(proc->stack, 0, sizeof(proc->stack));
    uint32_t *sp = (uint32_t *) proc_kernel_stack_top(proc);
    sp -= 16;
    memset(sp, 0, 16 * sizeof(uint32_t));
    sp[0] = (uint32_t) first_user_entry;
    proc->sp = (vaddr_t) sp;
    proc->state = PROC_RUNNABLE;
    int pid = proc->pid;
    release(&proc->lock);
    notify_schedulers();
    return pid;
}

static void switch_address_space(struct process *proc) {
    uint32_t *page_table = proc ? proc->page_table : kernel_page_table;
    if (proc)
        SET_CSR(sstatus, SSTATUS_SUM);
    else
        CLEAR_CSR(sstatus, SSTATUS_SUM);
    __asm__ __volatile__(
        "sfence.vma\n"
        "csrw satp, %0\n"
        "sfence.vma\n"
        :
        : "r"(SATP_SV32 | ((uint32_t) page_table / PAGE_SIZE))
        : "memory"
    );
    WRITE_CSR(sscratch, 0);
}

static void sched(void) {
    struct cpu *cpu = mycpu();
    struct process *proc = cpu->proc;
    switch_context(&proc->sp, &cpu->scheduler_sp);
}

void yield(void) {
    struct process *proc = myproc();
    if (!proc)
        return;
    acquire(&proc->lock);
    proc->state = PROC_RUNNABLE;
    sched();
    release(&proc->lock);
}

static void sleep_on(void *channel, struct spinlock *condition_lock) {
    struct process *proc = myproc();
    if (!proc)
        PANIC("boot code tried to sleep");

    acquire(&proc->lock);
    release(condition_lock);
    proc->wait_channel = channel;
    proc->state = PROC_BLOCKED;
    sched();
    proc->wait_channel = NULL;
    release(&proc->lock);
    acquire(condition_lock);
}

void wakeup(void *channel) {
    for (int i = 0; i < PROCS_MAX; i++) {
        struct process *proc = &procs[i];
        acquire(&proc->lock);
        if (proc->state == PROC_BLOCKED && proc->wait_channel == channel)
            proc->state = PROC_RUNNABLE;
        release(&proc->lock);
    }
    notify_schedulers();
}

static void mutex_lock(struct mutex *mutex) {
    acquire(&mutex->guard);
    while (mutex->locked)
        sleep_on(mutex, &mutex->guard);
    mutex->locked = true;
    release(&mutex->guard);
}

static void mutex_unlock(struct mutex *mutex) {
    acquire(&mutex->guard);
    mutex->locked = false;
    wakeup(mutex);
    release(&mutex->guard);
}

static void pipe_close_all(struct process *proc);

__attribute__((noreturn))
static void proc_exit(void) {
    struct process *proc = myproc();
    pipe_close_all(proc);
    acquire(&proc->lock);
    proc->state = PROC_EXITED;
    sched();
    PANIC("exited process resumed");
}

__attribute__((noreturn))
static void scheduler(void) {
    struct cpu *cpu = mycpu();
    cpu->proc = NULL;
    switch_address_space(NULL);

    for (;;) {
        bool ran_process = false;
        for (int i = 0; i < PROCS_MAX; i++) {
            struct process *proc = &procs[i];
            acquire(&proc->lock);
            if (proc->state != PROC_RUNNABLE ||
                (proc->affinity >= 0 && proc->affinity != (int) cpu->hartid)) {
                release(&proc->lock);
                continue;
            }

            ran_process = true;
            if (proc->running_on != -1)
                PANIC("process %d is already running on hart %d", proc->pid,
                      proc->running_on);
            proc->state = PROC_RUNNING;
            proc->running_on = cpu->hartid;
            cpu->proc = proc;
            cpu->schedules++;
            __sync_fetch_and_add(&context_switch_count, 1);

            vaddr_t kernel_sp = proc_kernel_stack_top(proc);
            *((uint32_t *) (kernel_sp + 12)) = (uint32_t) cpu;
            switch_address_space(proc);
            switch_context(&cpu->scheduler_sp, &proc->sp);

            cpu->proc = NULL;
            proc->running_on = -1;
            switch_address_space(NULL);
            if (proc->state == PROC_EXITED) {
                proc->pid = 0;
                proc->state = PROC_UNUSED;
            }
            release(&proc->lock);
        }

        if (!ran_process) {
            intr_on();
            __asm__ __volatile__("wfi");
            intr_off();
        }
    }
}

static int pipe_fd_alloc(struct process *proc) {
    for (int fd = 0; fd < PROCESS_FDS_MAX; fd++) {
        if (proc->fds[fd].pipe_index < 0)
            return fd;
    }
    return -1;
}

static int pipe_open_sys(int key, int mode) {
    int access = mode & (PIPE_READ | PIPE_WRITE);
    if (key <= 0 || (access != PIPE_READ && access != PIPE_WRITE) ||
        (mode & ~(PIPE_READ | PIPE_WRITE | PIPE_CREATE)) != 0)
        return -1;

    struct process *proc = myproc();
    int fd = pipe_fd_alloc(proc);
    if (fd < 0)
        return -1;

    acquire(&pipe_table_lock);
    int index = -1;
    int unused = -1;
    for (int i = 0; i < PIPES_MAX; i++) {
        if (pipes[i].in_use && pipes[i].key == key) {
            index = i;
            break;
        }
        if (!pipes[i].in_use && unused < 0)
            unused = i;
    }
    bool create = index < 0 && (mode & PIPE_CREATE) && unused >= 0;
    if (create)
        index = unused;
    if (index < 0) {
        release(&pipe_table_lock);
        return -1;
    }

    struct pipe *pipe = &pipes[index];
    acquire(&pipe->lock);
    if (create) {
        pipe->key = key;
        pipe->read_pos = 0;
        pipe->write_pos = 0;
        pipe->count = 0;
        pipe->readers = 0;
        pipe->writers = 0;
        pipe->had_reader = false;
        pipe->had_writer = false;
        __sync_synchronize();
        pipe->in_use = true;
    }
    if (access == PIPE_READ) {
        pipe->readers++;
        pipe->had_reader = true;
    } else {
        pipe->writers++;
        pipe->had_writer = true;
    }
    proc->fds[fd].pipe_index = index;
    proc->fds[fd].mode = access;
    wakeup(pipe);
    release(&pipe->lock);
    release(&pipe_table_lock);
    return fd;
}

static int pipe_read_sys(int fd, void *buf, int len) {
    struct process *proc = myproc();
    if (fd < 0 || fd >= PROCESS_FDS_MAX || len < 0 ||
        proc->fds[fd].pipe_index < 0 ||
        (proc->fds[fd].mode & PIPE_READ) == 0 ||
        !user_access_ok(proc, (vaddr_t) buf, len, true))
        return -1;
    if (len == 0)
        return 0;

    struct pipe *pipe = &pipes[proc->fds[fd].pipe_index];
    acquire(&pipe->lock);
    while (pipe->count == 0) {
        if (pipe->had_writer && pipe->writers == 0) {
            release(&pipe->lock);
            return 0;
        }
        sleep_on(pipe, &pipe->lock);
    }

    int count = len < (int) pipe->count ? len : (int) pipe->count;
    uint8_t *dst = (uint8_t *) buf;
    for (int i = 0; i < count; i++) {
        dst[i] = pipe->data[pipe->read_pos];
        pipe->read_pos = (pipe->read_pos + 1) % PIPE_CAPACITY;
    }
    pipe->count -= count;
    wakeup(pipe);
    release(&pipe->lock);
    return count;
}

static int pipe_write_sys(int fd, const void *buf, int len) {
    struct process *proc = myproc();
    if (fd < 0 || fd >= PROCESS_FDS_MAX || len < 0 ||
        proc->fds[fd].pipe_index < 0 ||
        (proc->fds[fd].mode & PIPE_WRITE) == 0 ||
        !user_access_ok(proc, (vaddr_t) buf, len, false))
        return -1;
    if (len == 0)
        return 0;

    struct pipe *pipe = &pipes[proc->fds[fd].pipe_index];
    acquire(&pipe->lock);
    while (!pipe->had_reader && pipe->readers == 0)
        sleep_on(pipe, &pipe->lock);
    while (pipe->count == PIPE_CAPACITY) {
        if (pipe->had_reader && pipe->readers == 0) {
            release(&pipe->lock);
            return -32;
        }
        sleep_on(pipe, &pipe->lock);
    }
    if (pipe->had_reader && pipe->readers == 0) {
        release(&pipe->lock);
        return -32;
    }

    int available = PIPE_CAPACITY - pipe->count;
    int count = len < available ? len : available;
    const uint8_t *src = (const uint8_t *) buf;
    for (int i = 0; i < count; i++) {
        pipe->data[pipe->write_pos] = src[i];
        pipe->write_pos = (pipe->write_pos + 1) % PIPE_CAPACITY;
    }
    pipe->count += count;
    wakeup(pipe);
    release(&pipe->lock);
    return count;
}

static int pipe_close_fd(struct process *proc, int fd) {
    if (fd < 0 || fd >= PROCESS_FDS_MAX || proc->fds[fd].pipe_index < 0)
        return -1;

    int index = proc->fds[fd].pipe_index;
    int mode = proc->fds[fd].mode;
    proc->fds[fd].pipe_index = -1;
    proc->fds[fd].mode = 0;

    acquire(&pipe_table_lock);
    struct pipe *pipe = &pipes[index];
    acquire(&pipe->lock);
    if (mode & PIPE_READ)
        pipe->readers--;
    if (mode & PIPE_WRITE)
        pipe->writers--;
    if (pipe->readers == 0 && pipe->writers == 0) {
        pipe->count = 0;
        pipe->read_pos = 0;
        pipe->write_pos = 0;
        pipe->had_reader = false;
        pipe->had_writer = false;
        pipe->key = 0;
        __sync_synchronize();
        pipe->in_use = false;
    } else {
        wakeup(pipe);
    }
    release(&pipe->lock);
    release(&pipe_table_lock);
    return 0;
}

static void pipe_close_all(struct process *proc) {
    for (int fd = 0; fd < PROCESS_FDS_MAX; fd++) {
        if (proc->fds[fd].pipe_index >= 0)
            pipe_close_fd(proc, fd);
    }
}

static struct mutex fs_mutex;

static int getchar_blocking(void) {
    acquire(&uart_rx_lock);
    while (uart_rx_count == 0)
        sleep_on(&uart_rx_count, &uart_rx_lock);
    int ch = uart_rx_buf[uart_rx_read];
    uart_rx_read = (uart_rx_read + 1) % UART_RX_CAPACITY;
    uart_rx_count--;
    release(&uart_rx_lock);
    return ch;
}

static void handle_syscall(struct trap_frame *f) {
    struct process *proc = myproc();
    switch (f->a3) {
        case SYS_GETCHAR:
            f->a0 = getchar_blocking();
            break;
        case SYS_EXIT:
            proc_exit();
        case SYS_PUTCHAR:
            putchar(f->a0);
            f->a0 = 0;
            break;
        case SYS_READFILE:
        case SYS_WRITEFILE: {
            char filename[100];
            int requested = (int) f->a2;
            if (requested < 0 ||
                copy_user_string(filename, sizeof(filename),
                                 (const char *) f->a0) < 0) {
                f->a0 = -1;
                break;
            }

            mutex_lock(&fs_mutex);
            struct file *file = fs_lookup(filename);
            if (!file) {
                mutex_unlock(&fs_mutex);
                f->a0 = -1;
                break;
            }

            int len = requested;
            if (f->a3 == SYS_WRITEFILE) {
                if (len > (int) sizeof(file->data))
                    len = sizeof(file->data);
                if (!fs_can_resize(file, (size_t) len)) {
                    mutex_unlock(&fs_mutex);
                    f->a0 = -1;
                    break;
                }
                if (!user_access_ok(proc, f->a1, len, false)) {
                    mutex_unlock(&fs_mutex);
                    f->a0 = -1;
                    break;
                }
                memcpy(file->data, (const void *) f->a1, len);
                file->size = len;
                fs_flush();
            } else {
                if (len > (int) file->size)
                    len = file->size;
                if (!user_access_ok(proc, f->a1, len, true)) {
                    mutex_unlock(&fs_mutex);
                    f->a0 = -1;
                    break;
                }
                memcpy((void *) f->a1, file->data, len);
            }
            mutex_unlock(&fs_mutex);
            f->a0 = len;
            break;
        }
        case SYS_PIPE_OPEN:
            f->a0 = pipe_open_sys((int) f->a0, (int) f->a1);
            break;
        case SYS_PIPE_READ:
            f->a0 = pipe_read_sys((int) f->a0, (void *) f->a1,
                                  (int) f->a2);
            break;
        case SYS_PIPE_WRITE:
            f->a0 = pipe_write_sys((int) f->a0, (const void *) f->a1,
                                   (int) f->a2);
            break;
        case SYS_PIPE_CLOSE:
            f->a0 = pipe_close_fd(proc, (int) f->a0);
            break;
        case SYS_SPAWN: {
            vaddr_t entry = f->a0;
            int affinity = (int) f->a2;
            uint32_t *pte = walk_pte(proc->page_table, entry);
            if (affinity < -1 || affinity >= CPU_COUNT || entry < USER_BASE ||
                !pte || (*pte & (PAGE_V | PAGE_U | PAGE_X)) !=
                            (PAGE_V | PAGE_U | PAGE_X)) {
                f->a0 = -1;
                break;
            }
            int child_pid = create_process(
                _binary_shell_bin_start, (size_t) _binary_shell_bin_size,
                entry, f->a1, affinity);
            f->a0 = child_pid;
            break;
        }
        case SYS_GET_HARTID:
            f->a0 = mycpu()->hartid;
            break;
        case SYS_GET_NCPU:
            f->a0 = CPU_COUNT;
            break;
        case SYS_GET_TICKS:
            f->a0 = timer_irq_count;
            break;
        case SYS_GET_IRQ_COUNT:
            switch (f->a0) {
                case IRQ_TIMER: f->a0 = timer_irq_count; break;
                case IRQ_UART: f->a0 = uart_irq_count; break;
                case IRQ_BLOCK: f->a0 = block_irq_count; break;
                case IRQ_CONTEXT_SWITCH: f->a0 = context_switch_count; break;
                case IRQ_UART_RX_DROPPED:
                    acquire(&uart_rx_lock);
                    f->a0 = uart_rx_dropped;
                    release(&uart_rx_lock);
                    break;
                default: f->a0 = -1; break;
            }
            break;
        case SYS_YIELD:
            yield();
            f->a0 = 0;
            break;
        default:
            f->a0 = -1;
            break;
    }
}

static uint32_t plic_context(uint32_t hartid) {
    return 2 * hartid + 1;
}

static volatile uint32_t *plic_priority(int irq) {
    return (volatile uint32_t *) (PLIC_PADDR + irq * 4);
}

static volatile uint32_t *plic_enable(uint32_t hartid) {
    return (volatile uint32_t *)
        (PLIC_ENABLE_BASE + plic_context(hartid) * 0x80);
}

static volatile uint32_t *plic_threshold(uint32_t hartid) {
    return (volatile uint32_t *)
        (PLIC_CONTEXT_BASE + plic_context(hartid) * 0x1000);
}

static volatile uint32_t *plic_claim(uint32_t hartid) {
    return plic_threshold(hartid) + 1;
}

static void plic_global_init(void) {
    *plic_priority(VIRTIO_BLK_IRQ) = 1;
    *plic_priority(UART_IRQ) = 1;
}

static void plic_hart_init(void) {
    uint32_t hartid = mycpu()->hartid;
    *plic_enable(hartid) = hartid == boot_hartid
        ? (1u << VIRTIO_BLK_IRQ) | (1u << UART_IRQ)
        : 0;
    *plic_threshold(hartid) = 0;
}

static void external_handle_irq(void) {
    volatile uint32_t *claim = plic_claim(mycpu()->hartid);
    for (;;) {
        uint32_t irq = *claim;
        if (irq == 0)
            break;
        if (irq == UART_IRQ)
            uart_handle_irq();
        else if (irq == VIRTIO_BLK_IRQ)
            virtio_handle_irq();
        else
            __sync_fetch_and_add(&spurious_irq_count, 1);
        __asm__ __volatile__("fence iorw, iorw" : : : "memory");
        *claim = irq;
    }
}

void handle_trap(struct trap_frame *f) {
    uint32_t cause = f->scause & ~SCAUSE_INTERRUPT;
    if (f->scause & SCAUSE_INTERRUPT) {
        if (cause == IRQ_S_TIMER) {
            timer_handle_irq();
            if ((f->sstatus & SSTATUS_SPP) == 0 && myproc())
                yield();
        } else if (cause == IRQ_S_EXTERNAL) {
            external_handle_irq();
        } else if (cause == IRQ_S_SOFTWARE) {
            CLEAR_CSR(sip, 1 << IRQ_S_SOFTWARE);
        } else {
            PANIC("unexpected interrupt scause=%x sepc=%x", f->scause,
                  f->sepc);
        }
        return;
    }

    if (f->scause == SCAUSE_ECALL && (f->sstatus & SSTATUS_SPP) == 0) {
        f->sepc += 4;
        handle_syscall(f);
        return;
    }

    PANIC("unexpected trap scause=%x stval=%x sepc=%x", f->scause,
          f->stval, f->sepc);
}

__attribute__((naked, aligned(4)))
void kernel_entry(void) {
    __asm__ __volatile__(
        ".equ TF_SIZE, 144\n"
        "csrrw sp, sscratch, sp\n"
        "bnez sp, 1f\n"
        "csrrw sp, sscratch, sp\n"
        "1:\n"
        "addi sp, sp, -TF_SIZE\n"
        "sw ra,   4 * 0(sp)\n"
        "sw gp,   4 * 1(sp)\n"
        "sw tp,   4 * 2(sp)\n"
        "sw t0,   4 * 3(sp)\n"
        "sw t1,   4 * 4(sp)\n"
        "sw t2,   4 * 5(sp)\n"
        "sw t3,   4 * 6(sp)\n"
        "sw t4,   4 * 7(sp)\n"
        "sw t5,   4 * 8(sp)\n"
        "sw t6,   4 * 9(sp)\n"
        "sw a0,   4 * 10(sp)\n"
        "sw a1,   4 * 11(sp)\n"
        "sw a2,   4 * 12(sp)\n"
        "sw a3,   4 * 13(sp)\n"
        "sw a4,   4 * 14(sp)\n"
        "sw a5,   4 * 15(sp)\n"
        "sw a6,   4 * 16(sp)\n"
        "sw a7,   4 * 17(sp)\n"
        "sw s0,   4 * 18(sp)\n"
        "sw s1,   4 * 19(sp)\n"
        "sw s2,   4 * 20(sp)\n"
        "sw s3,   4 * 21(sp)\n"
        "sw s4,   4 * 22(sp)\n"
        "sw s5,   4 * 23(sp)\n"
        "sw s6,   4 * 24(sp)\n"
        "sw s7,   4 * 25(sp)\n"
        "sw s8,   4 * 26(sp)\n"
        "sw s9,   4 * 27(sp)\n"
        "sw s10,  4 * 28(sp)\n"
        "sw s11,  4 * 29(sp)\n"
        ".option push\n"
        ".option norelax\n"
        "la gp, __global_pointer$\n"
        ".option pop\n"
        "csrr t0, sstatus\n"
        "sw t0, 4 * 32(sp)\n"
        "andi t1, t0, %[spp]\n"
        "bnez t1, 2f\n"
        "csrr t0, sscratch\n"
        "sw t0, 4 * 30(sp)\n"
        "lw tp, TF_SIZE + 12(sp)\n"
        "csrw sscratch, zero\n"
        "j 3f\n"
        "2:\n"
        "addi t0, sp, TF_SIZE\n"
        "sw t0, 4 * 30(sp)\n"
        "3:\n"
        "csrr t0, sepc\n"
        "sw t0, 4 * 31(sp)\n"
        "csrr t0, scause\n"
        "sw t0, 4 * 33(sp)\n"
        "csrr t0, stval\n"
        "sw t0, 4 * 34(sp)\n"
        "sw zero, 4 * 35(sp)\n"
        "mv a0, sp\n"
        "call handle_trap\n"
        "lw t0, 4 * 31(sp)\n"
        "csrw sepc, t0\n"
        "lw t0, 4 * 32(sp)\n"
        "csrw sstatus, t0\n"
        "andi t0, t0, %[spp]\n"
        "bnez t0, 4f\n"
        "addi t0, sp, TF_SIZE\n"
        "csrw sscratch, t0\n"
        "j 5f\n"
        "4:\n"
        "csrw sscratch, zero\n"
        "5:\n"
        "lw ra,   4 * 0(sp)\n"
        "lw gp,   4 * 1(sp)\n"
        "lw tp,   4 * 2(sp)\n"
        "lw t1,   4 * 4(sp)\n"
        "lw t2,   4 * 5(sp)\n"
        "lw t3,   4 * 6(sp)\n"
        "lw t4,   4 * 7(sp)\n"
        "lw t5,   4 * 8(sp)\n"
        "lw t6,   4 * 9(sp)\n"
        "lw a0,   4 * 10(sp)\n"
        "lw a1,   4 * 11(sp)\n"
        "lw a2,   4 * 12(sp)\n"
        "lw a3,   4 * 13(sp)\n"
        "lw a4,   4 * 14(sp)\n"
        "lw a5,   4 * 15(sp)\n"
        "lw a6,   4 * 16(sp)\n"
        "lw a7,   4 * 17(sp)\n"
        "lw s0,   4 * 18(sp)\n"
        "lw s1,   4 * 19(sp)\n"
        "lw s2,   4 * 20(sp)\n"
        "lw s3,   4 * 21(sp)\n"
        "lw s4,   4 * 22(sp)\n"
        "lw s5,   4 * 23(sp)\n"
        "lw s6,   4 * 24(sp)\n"
        "lw s7,   4 * 25(sp)\n"
        "lw s8,   4 * 26(sp)\n"
        "lw s9,   4 * 27(sp)\n"
        "lw s10,  4 * 28(sp)\n"
        "lw s11,  4 * 29(sp)\n"
        "lw t0,   4 * 3(sp)\n"
        "lw sp,   4 * 30(sp)\n"
        "sret\n"
        :
        : [spp] "i" (SSTATUS_SPP)
    );
}

uint32_t virtio_reg_read32(unsigned offset) {
    return *((volatile uint32_t *) (VIRTIO_BLK_PADDR + offset));
}

uint64_t virtio_reg_read64(unsigned offset) {
    return *((volatile uint64_t *) (VIRTIO_BLK_PADDR + offset));
}

void virtio_reg_write32(unsigned offset, uint32_t value) {
    *((volatile uint32_t *) (VIRTIO_BLK_PADDR + offset)) = value;
}

void virtio_reg_fetch_and_or32(unsigned offset, uint32_t value) {
    virtio_reg_write32(offset, virtio_reg_read32(offset) | value);
}

static struct virtio_virtq *blk_request_vq;
static struct virtio_blk_req *blk_req;
static paddr_t blk_req_paddr;
static uint64_t blk_capacity;
static struct mutex disk_mutex;
static struct spinlock blk_irq_lock;
static volatile bool blk_done;

static struct virtio_virtq *virtq_init(unsigned index) {
    paddr_t virtq_paddr = alloc_pages(
        align_up(sizeof(struct virtio_virtq), PAGE_SIZE) / PAGE_SIZE);
    struct virtio_virtq *vq = (struct virtio_virtq *) virtq_paddr;
    vq->queue_index = index;
    vq->used_index = (volatile uint16_t *) &vq->used.index;
    virtio_reg_write32(VIRTIO_REG_QUEUE_SEL, index);
    if (virtio_reg_read32(VIRTIO_REG_QUEUE_NUM_MAX) < VIRTQ_ENTRY_NUM)
        PANIC("virtio: queue too small");
    virtio_reg_write32(VIRTIO_REG_QUEUE_NUM, VIRTQ_ENTRY_NUM);
    virtio_reg_write32(VIRTIO_REG_QUEUE_ALIGN, PAGE_SIZE);
    virtio_reg_write32(VIRTIO_REG_QUEUE_PFN, virtq_paddr / PAGE_SIZE);
    return vq;
}

static void virtio_blk_init(void) {
    if (virtio_reg_read32(VIRTIO_REG_MAGIC) != 0x74726976)
        PANIC("virtio: invalid magic value");
    if (virtio_reg_read32(VIRTIO_REG_VERSION) != 1)
        PANIC("virtio: expected legacy MMIO device");
    if (virtio_reg_read32(VIRTIO_REG_DEVICE_ID) != VIRTIO_DEVICE_BLK)
        PANIC("virtio: invalid device id");

    virtio_reg_write32(VIRTIO_REG_DEVICE_STATUS, 0);
    while (virtio_reg_read32(VIRTIO_REG_DEVICE_STATUS) != 0)
        ;
    virtio_reg_fetch_and_or32(VIRTIO_REG_DEVICE_STATUS, VIRTIO_STATUS_ACK);
    virtio_reg_fetch_and_or32(VIRTIO_REG_DEVICE_STATUS, VIRTIO_STATUS_DRIVER);
    (void) virtio_reg_read32(VIRTIO_REG_DEVICE_FEATURES);
    virtio_reg_write32(VIRTIO_REG_GUEST_FEATURES, 0);
    virtio_reg_write32(VIRTIO_REG_GUEST_PAGE_SIZE, PAGE_SIZE);
    virtio_reg_fetch_and_or32(VIRTIO_REG_DEVICE_STATUS, VIRTIO_STATUS_FEAT_OK);
    if ((virtio_reg_read32(VIRTIO_REG_DEVICE_STATUS) & VIRTIO_STATUS_FEAT_OK) == 0)
        PANIC("virtio: device rejected negotiated features");
    blk_request_vq = virtq_init(0);
    virtio_reg_fetch_and_or32(VIRTIO_REG_DEVICE_STATUS,
                             VIRTIO_STATUS_DRIVER_OK);

    blk_capacity = virtio_reg_read64(VIRTIO_REG_DEVICE_CONFIG) * SECTOR_SIZE;
    printf("virtio-blk: capacity is %d bytes\n", (int) blk_capacity);
    blk_req_paddr = alloc_pages(
        align_up(sizeof(*blk_req), PAGE_SIZE) / PAGE_SIZE);
    blk_req = (struct virtio_blk_req *) blk_req_paddr;
}

static void virtq_kick(struct virtio_virtq *vq, int desc_index) {
    vq->avail.ring[vq->avail.index % VIRTQ_ENTRY_NUM] = desc_index;
    __asm__ __volatile__("fence iorw, iorw" : : : "memory");
    vq->avail.index++;
    __asm__ __volatile__("fence iorw, iorw" : : : "memory");
    virtio_reg_write32(VIRTIO_REG_QUEUE_NOTIFY, vq->queue_index);
}

void virtio_handle_irq(void) {
    uint32_t status = virtio_reg_read32(VIRTIO_REG_INTERRUPT_STATUS);
    uint32_t handled = status & 3;
    if (handled)
        virtio_reg_write32(VIRTIO_REG_INTERRUPT_ACK, handled);
    if ((status & 1) == 0)
        return;

    acquire(&blk_irq_lock);
    uint16_t used_index = *blk_request_vq->used_index;
    __asm__ __volatile__("fence iorw, iorw" : : : "memory");
    bool completed = false;
    while (blk_request_vq->last_used_index != used_index) {
        struct virtq_used_elem *elem = &blk_request_vq->used.ring[
            blk_request_vq->last_used_index % VIRTQ_ENTRY_NUM];
        if (elem->id == 0)
            completed = true;
        blk_request_vq->last_used_index++;
    }
    if (completed) {
        blk_done = true;
        __sync_fetch_and_add(&block_irq_count, 1);
        wakeup((void *) &blk_done);
    } else {
        __sync_fetch_and_add(&spurious_irq_count, 1);
    }
    release(&blk_irq_lock);
}

void read_write_disk(void *buf, unsigned sector, int is_write) {
    if (sector >= blk_capacity / SECTOR_SIZE)
        PANIC("virtio: sector %d outside capacity", sector);

    struct process *proc = myproc();
    if (proc)
        mutex_lock(&disk_mutex);

    blk_req->sector = sector;
    blk_req->type = is_write ? VIRTIO_BLK_T_OUT : VIRTIO_BLK_T_IN;
    blk_req->status = 0xff;
    if (is_write)
        memcpy(blk_req->data, buf, SECTOR_SIZE);

    struct virtio_virtq *vq = blk_request_vq;
    vq->descs[0].addr = blk_req_paddr;
    vq->descs[0].len = sizeof(uint32_t) * 2 + sizeof(uint64_t);
    vq->descs[0].flags = VIRTQ_DESC_F_NEXT;
    vq->descs[0].next = 1;
    vq->descs[1].addr = blk_req_paddr + offsetof(struct virtio_blk_req, data);
    vq->descs[1].len = SECTOR_SIZE;
    vq->descs[1].flags = VIRTQ_DESC_F_NEXT |
        (is_write ? 0 : VIRTQ_DESC_F_WRITE);
    vq->descs[1].next = 2;
    vq->descs[2].addr = blk_req_paddr + offsetof(struct virtio_blk_req, status);
    vq->descs[2].len = sizeof(uint8_t);
    vq->descs[2].flags = VIRTQ_DESC_F_WRITE;

    acquire(&blk_irq_lock);
    blk_done = false;
    virtq_kick(vq, 0);
    if (proc) {
        while (!blk_done)
            sleep_on((void *) &blk_done, &blk_irq_lock);
        release(&blk_irq_lock);
    } else {
        release(&blk_irq_lock);
        for (;;) {
            acquire(&blk_irq_lock);
            bool done = blk_done;
            release(&blk_irq_lock);
            if (done)
                break;
            intr_on();
            __asm__ __volatile__("wfi");
            intr_off();
        }
    }

    __asm__ __volatile__("fence iorw, iorw" : : : "memory");
    if (blk_req->status != 0)
        PANIC("virtio: request failed sector=%d status=%d", sector,
              blk_req->status);
    if (!is_write)
        memcpy(buf, blk_req->data, SECTOR_SIZE);
    if (proc)
        mutex_unlock(&disk_mutex);
}

struct file files[FILES_MAX];
uint8_t disk[DISK_MAX_SIZE];

static int oct2int(char *oct, int len) {
    int dec = 0;
    for (int i = 0; i < len; i++) {
        if (oct[i] < '0' || oct[i] > '7')
            break;
        dec = dec * 8 + (oct[i] - '0');
    }
    return dec;
}

static void fs_init(void) {
    for (unsigned sector = 0; sector < sizeof(disk) / SECTOR_SIZE; sector++)
        read_write_disk(&disk[sector * SECTOR_SIZE], sector, false);

    unsigned off = 0;
    for (int i = 0; i < FILES_MAX; i++) {
        if (off + sizeof(struct tar_header) > sizeof(disk))
            break;
        struct tar_header *header = (struct tar_header *) &disk[off];
        if (header->name[0] == '\0')
            break;
        if (strcmp(header->magic, "ustar") != 0)
            PANIC("invalid tar header");

        int filesz = oct2int(header->size, sizeof(header->size));
        if (filesz < 0 || filesz > (int) sizeof(files[i].data))
            PANIC("file too large: %s", header->name);
        if ((unsigned) filesz > sizeof(disk) - off - sizeof(*header))
            PANIC("file data exceeds filesystem buffer");
        struct file *file = &files[i];
        file->in_use = true;
        strcpy(file->name, header->name);
        memcpy(file->data, header->data, filesz);
        file->size = filesz;
        printf("file: %s, size=%d\n", file->name, file->size);
        off += align_up(sizeof(struct tar_header) + filesz, SECTOR_SIZE);
    }
}

// Called with fs_mutex held, before changing file data or size.
static bool fs_can_resize(struct file *target, size_t new_size) {
    size_t used = 0;
    for (int i = 0; i < FILES_MAX; i++) {
        if (!files[i].in_use)
            continue;
        size_t size = &files[i] == target ? new_size : files[i].size;
        size_t span = align_up(sizeof(struct tar_header) + size, SECTOR_SIZE);
        if (span > sizeof(disk) - used)
            return false;
        used += span;
    }
    return true;
}

void fs_flush(void) {
    memset(disk, 0, sizeof(disk));
    unsigned off = 0;
    for (int file_i = 0; file_i < FILES_MAX; file_i++) {
        struct file *file = &files[file_i];
        if (!file->in_use)
            continue;

        struct tar_header *header = (struct tar_header *) &disk[off];
        memset(header, 0, sizeof(*header));
        strcpy(header->name, file->name);
        strcpy(header->mode, "000644");
        strcpy(header->magic, "ustar");
        strcpy(header->version, "00");
        header->type = '0';

        int filesz = file->size;
        for (int i = sizeof(header->size); i > 0; i--) {
            header->size[i - 1] = (filesz % 8) + '0';
            filesz /= 8;
        }

        int checksum = ' ' * sizeof(header->checksum);
        for (unsigned i = 0; i < sizeof(struct tar_header); i++)
            checksum += (unsigned char) disk[off + i];
        for (int i = 5; i >= 0; i--) {
            header->checksum[i] = (checksum % 8) + '0';
            checksum /= 8;
        }

        memcpy(header->data, file->data, file->size);
        off += align_up(sizeof(struct tar_header) + file->size, SECTOR_SIZE);
    }

    for (unsigned sector = 0; sector < sizeof(disk) / SECTOR_SIZE; sector++)
        read_write_disk(&disk[sector * SECTOR_SIZE], sector, true);
}

struct file *fs_lookup(const char *filename) {
    for (int i = 0; i < FILES_MAX; i++) {
        if (files[i].in_use && strcmp(files[i].name, filename) == 0)
            return &files[i];
    }
    return NULL;
}

static void interrupt_init(void) {
    WRITE_CSR(stvec, (uint32_t) kernel_entry);
    WRITE_CSR(sscratch, 0);
    plic_hart_init();
    timer_init();
    __asm__ __volatile__("fence iorw, iorw" : : : "memory");
    SET_CSR(sie, SIE_SSIE | SIE_STIE | SIE_SEIE);
}

static void activate_kernel_page_table(void) {
    WRITE_CSR(satp,
              SATP_SV32 | ((uint32_t) kernel_page_table / PAGE_SIZE));
    __asm__ __volatile__("sfence.vma" : : : "memory");
}

static void start_secondary_harts(void) {
    __sync_synchronize();
    for (int hart = 0; hart < CPU_COUNT; hart++) {
        if ((uint32_t) hart == boot_hartid)
            continue;
        struct sbiret ret = sbi_call(hart, (uint32_t) secondary_boot, 0, 0, 0,
                                     0, SBI_HSM_HART_START, SBI_EXT_HSM);
        if (ret.error)
            PANIC("failed to start hart %d: %d", hart, ret.error);
    }

    while (online_count < CPU_COUNT) {
        intr_on();
        __asm__ __volatile__("wfi");
        intr_off();
    }
}

__attribute__((noreturn))
void secondary_main(uint32_t hartid) {
    if (hartid >= CPU_COUNT)
        for (;;)
            __asm__ __volatile__("wfi");

    struct cpu *cpu = &cpus[hartid];
    __asm__ __volatile__("mv tp, %0" : : "r"(cpu));
    cpu->hartid = hartid;
    cpu->proc = NULL;
    activate_kernel_page_table();
    interrupt_init();
    cpu->online = true;
    __sync_fetch_and_add(&online_count, 1);
    send_ipi(1u << boot_hartid);
    scheduler();
}

__attribute__((noreturn))
void kernel_main(uint32_t hartid) {
    if (hartid >= CPU_COUNT)
        for (;;)
            __asm__ __volatile__("wfi");

    memset(__bss, 0, (size_t) __bss_end - (size_t) __bss);
    boot_hartid = hartid;
    struct cpu *cpu = &cpus[hartid];
    __asm__ __volatile__("mv tp, %0" : : "r"(cpu));
    cpu->hartid = hartid;
    cpu->online = true;
    online_count = 1;

    uart_init();
    printf("\nRISC-V teaching OS: interrupt/preemption/IPC/SMP\n");
    alloc_init();
    kernel_page_table = create_page_table();
    activate_kernel_page_table();
    plic_global_init();
    interrupt_init();
    virtio_blk_init();
    fs_init();

    start_secondary_harts();
    printf("smp: %d hart(s) online\n", CPU_COUNT);

    int shell_pid = create_process(
        _binary_shell_bin_start, (size_t) _binary_shell_bin_size,
        USER_BASE, 0, boot_hartid);
    if (shell_pid < 0)
        PANIC("failed to create shell");
    scheduler();
}

__attribute__((section(".text.boot"), naked, aligned(4)))
void boot(void) {
    __asm__ __volatile__(
        "li t1, 4\n"
        "bgeu a0, t1, 1f\n"
        ".option push\n"
        ".option norelax\n"
        "la gp, __global_pointer$\n"
        ".option pop\n"
        "la sp, __stack_top\n"
        "li t0, 32768\n"
        "mul t0, a0, t0\n"
        "sub sp, sp, t0\n"
        "andi sp, sp, -16\n"
        "tail kernel_main\n"
        "1: wfi\n"
        "j 1b\n"
    );
}

__attribute__((section(".text.boot"), naked, aligned(4)))
void secondary_boot(void) {
    __asm__ __volatile__(
        "li t1, 4\n"
        "bgeu a0, t1, 1f\n"
        ".option push\n"
        ".option norelax\n"
        "la gp, __global_pointer$\n"
        ".option pop\n"
        "la sp, __stack_top\n"
        "li t0, 32768\n"
        "mul t0, a0, t0\n"
        "sub sp, sp, t0\n"
        "andi sp, sp, -16\n"
        "tail secondary_main\n"
        "1: wfi\n"
        "j 1b\n"
    );
}
