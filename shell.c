#include "user.h"

#define SELFTEST_STREAM_KEY  0x5101
#define SELFTEST_RESULT_KEY  0x5102
#define SELFTEST_STARTED_KEY 0x5103
#define SELFTEST_GATE_KEY    0x5104
#define SELFTEST_HART_KEY    0x5105

#define SELFTEST_STREAM_BYTES (3 * 1024)
#define SELFTEST_HOG_SPINS    30000000u
#define SELFTEST_MAX_HARTS    16

struct hart_result {
    int expected;
    int actual;
};

static bool write_all(int fd, const void *buf, int len) {
    const uint8_t *p = (const uint8_t *) buf;
    int written = 0;
    while (written < len) {
        int n = pipe_write(fd, p + written, len - written);
        if (n <= 0)
            return false;
        written += n;
    }
    return true;
}

static bool read_exact(int fd, void *buf, int len) {
    uint8_t *p = (uint8_t *) buf;
    int received = 0;
    while (received < len) {
        int n = pipe_read(fd, p + received, len - received);
        if (n <= 0)
            return false;
        received += n;
    }
    return true;
}

static bool selftest_fail(const char *stage) {
    printf("SELFTEST FAIL: %s\n", stage);
    return false;
}

static void worker_fail(const char *stage) {
    printf("SELFTEST FAIL: %s\n", stage);
    exit();
}

static uint8_t stream_byte(int offset) {
    return (uint8_t) ((offset * 37 + 11) & 0xff);
}

static void stream_writer(int key) {
    int fd = pipe_open(key, PIPE_WRITE);
    if (fd < 0)
        worker_fail("pipe-writer-open");

    int offset = 0;
    while (offset < SELFTEST_STREAM_BYTES) {
        uint8_t buf[127];
        int len = SELFTEST_STREAM_BYTES - offset;
        if (len > (int) sizeof(buf))
            len = sizeof(buf);

        for (int i = 0; i < len; i++)
            buf[i] = stream_byte(offset + i);

        if (!write_all(fd, buf, len)) {
            pipe_close(fd);
            worker_fail("pipe-writer-write");
        }
        offset += len;
    }

    if (pipe_close(fd) < 0)
        worker_fail("pipe-writer-close");
    exit();
}

static void quick_worker(int unused) {
    (void) unused;
    int result_fd = pipe_open(SELFTEST_RESULT_KEY, PIPE_WRITE);
    if (result_fd < 0)
        worker_fail("quick-result-open");

    int gate_fd = pipe_open(SELFTEST_GATE_KEY, PIPE_READ);
    if (gate_fd < 0) {
        pipe_close(result_fd);
        worker_fail("quick-gate-open");
    }

    char token;
    if (!read_exact(gate_fd, &token, 1)) {
        pipe_close(gate_fd);
        pipe_close(result_fd);
        worker_fail("quick-gate-read");
    }
    pipe_close(gate_fd);

    token = 'Q';
    if (!write_all(result_fd, &token, 1)) {
        pipe_close(result_fd);
        worker_fail("quick-result-write");
    }
    pipe_close(result_fd);
    exit();
}

static void hog_worker(int unused) {
    (void) unused;
    int result_fd = pipe_open(SELFTEST_RESULT_KEY, PIPE_WRITE);
    if (result_fd < 0)
        worker_fail("hog-result-open");

    int started_fd = pipe_open(SELFTEST_STARTED_KEY, PIPE_WRITE);
    if (started_fd < 0) {
        pipe_close(result_fd);
        worker_fail("hog-started-open");
    }

    char token = 'S';
    if (!write_all(started_fd, &token, 1)) {
        pipe_close(started_fd);
        pipe_close(result_fd);
        worker_fail("hog-started-write");
    }
    pipe_close(started_fd);

    volatile uint32_t spin;
    for (spin = 0; spin < SELFTEST_HOG_SPINS; spin++)
        __asm__ __volatile__("nop");

    token = 'H';
    if (!write_all(result_fd, &token, 1)) {
        pipe_close(result_fd);
        worker_fail("hog-result-write");
    }
    pipe_close(result_fd);
    exit();
}

static void hart_worker(int expected_hart) {
    int fd = pipe_open(SELFTEST_HART_KEY, PIPE_WRITE);
    if (fd < 0)
        worker_fail("hart-worker-open");

    struct hart_result result = {
        .expected = expected_hart,
        .actual = get_hartid(),
    };
    if (!write_all(fd, &result, sizeof(result))) {
        pipe_close(fd);
        worker_fail("hart-worker-write");
    }
    pipe_close(fd);
    exit();
}

static bool test_timer(void) {
    int before = get_ticks();
    if (before < 0)
        return selftest_fail("timer-query");

    int after = before;
    volatile uint32_t attempts;
    for (attempts = 0; attempts < 200000u; attempts++) {
        after = get_ticks();
        if (after != before)
            break;
    }

    if (after == before)
        return selftest_fail("timer-not-advancing");
    return true;
}

static bool test_block_irq(void) {
    int before = get_irq_count(IRQ_BLOCK);
    if (before < 0)
        return selftest_fail("block-irq-query-before");

    char buf[128];
    int len = readfile("hello.txt", buf, sizeof(buf) - 1);
    if (len < 0)
        return selftest_fail("block-irq-readfile");
    if (writefile("hello.txt", buf, len) != len)
        return selftest_fail("block-irq-writefile");

    int after = get_irq_count(IRQ_BLOCK);
    if (after < 0)
        return selftest_fail("block-irq-query-after");
    if (after == before)
        return selftest_fail("block-irq-not-incremented");
    return true;
}

static bool test_file_capacity(void) {
    char original_hello[1024];
    char original_meow[1024];
    char payload[1024];
    char actual[1024];
    int hello_len = readfile("hello.txt", original_hello, sizeof(original_hello));
    int meow_len = readfile("meow.txt", original_meow, sizeof(original_meow));
    if (hello_len < 0 || meow_len < 0)
        return selftest_fail("file-capacity-backup");

    memset(payload, 'C', sizeof(payload));
    bool ok = writefile("meow.txt", payload, 0) == 0;
    if (ok)
        ok = writefile("hello.txt", payload, sizeof(payload)) == sizeof(payload);
    if (ok)
        ok = writefile("meow.txt", payload, sizeof(payload)) == -1;
    if (ok)
        ok = readfile("meow.txt", actual, sizeof(actual)) == 0;
    if (ok)
        ok = readfile("hello.txt", actual, sizeof(actual)) == sizeof(payload);
    for (int i = 0; ok && i < (int) sizeof(payload); i++) {
        if (actual[i] != payload[i])
            ok = false;
    }

    // Shrink both files before restoring their original combined footprint.
    bool restored = writefile("meow.txt", payload, 0) == 0;
    if (writefile("hello.txt", payload, 0) != 0)
        restored = false;
    if (writefile("hello.txt", original_hello, hello_len) != hello_len)
        restored = false;
    if (writefile("meow.txt", original_meow, meow_len) != meow_len)
        restored = false;
    if (!restored)
        return selftest_fail("file-capacity-restore");
    return ok ? true : selftest_fail("file-capacity-boundary");
}

static bool test_pipe_stream(void) {
    int fd = pipe_open(SELFTEST_STREAM_KEY, PIPE_READ | PIPE_CREATE);
    if (fd < 0)
        return selftest_fail("pipe-reader-open");

    if (spawn(stream_writer, SELFTEST_STREAM_KEY, -1) < 0) {
        pipe_close(fd);
        return selftest_fail("pipe-writer-spawn");
    }

    int offset = 0;
    while (1) {
        uint8_t buf[83];
        int n = pipe_read(fd, buf, sizeof(buf));
        if (n < 0) {
            pipe_close(fd);
            return selftest_fail("pipe-read");
        }
        if (n == 0)
            break;
        if (offset + n > SELFTEST_STREAM_BYTES) {
            pipe_close(fd);
            return selftest_fail("pipe-length-overflow");
        }

        for (int i = 0; i < n; i++) {
            if (buf[i] != stream_byte(offset + i)) {
                pipe_close(fd);
                return selftest_fail("pipe-data-mismatch");
            }
        }
        offset += n;
    }

    if (pipe_close(fd) < 0)
        return selftest_fail("pipe-reader-close");
    if (offset != SELFTEST_STREAM_BYTES)
        return selftest_fail("pipe-short-stream");
    return true;
}

static bool test_preemption(void) {
    int hart = get_hartid();
    if (hart < 0)
        return selftest_fail("preemption-hart-query");

    int result_fd = pipe_open(SELFTEST_RESULT_KEY, PIPE_READ | PIPE_CREATE);
    int result_keep = pipe_open(SELFTEST_RESULT_KEY, PIPE_WRITE);
    int started_fd = pipe_open(SELFTEST_STARTED_KEY, PIPE_READ | PIPE_CREATE);
    int started_keep = pipe_open(SELFTEST_STARTED_KEY, PIPE_WRITE);
    int gate_fd = pipe_open(SELFTEST_GATE_KEY, PIPE_WRITE | PIPE_CREATE);
    if (result_fd < 0 || result_keep < 0 || started_fd < 0 ||
        started_keep < 0 || gate_fd < 0)
        return selftest_fail("preemption-pipe-open");

    if (spawn(quick_worker, 0, hart) < 0)
        return selftest_fail("preemption-quick-spawn");
    if (spawn(hog_worker, 0, hart) < 0)
        return selftest_fail("preemption-hog-spawn");

    char started;
    if (!read_exact(started_fd, &started, 1) || started != 'S')
        return selftest_fail("preemption-hog-start");
    pipe_close(started_keep);
    pipe_close(started_fd);

    char token = 'G';
    if (!write_all(gate_fd, &token, 1))
        return selftest_fail("preemption-gate-write");
    pipe_close(gate_fd);

    char results[2];
    if (!read_exact(result_fd, results, sizeof(results)))
        return selftest_fail("preemption-results");
    pipe_close(result_keep);

    char extra;
    int eof = pipe_read(result_fd, &extra, 1);
    pipe_close(result_fd);
    if (eof != 0)
        return selftest_fail("preemption-eof");
    if (results[0] != 'Q')
        return selftest_fail("preemption-first-result-not-Q");
    return true;
}

static bool test_all_harts(void) {
    int ncpu = get_ncpu();
    if (ncpu <= 0 || ncpu > SELFTEST_MAX_HARTS)
        return selftest_fail("hart-count");

    int fd = pipe_open(SELFTEST_HART_KEY, PIPE_READ | PIPE_CREATE);
    int keep = pipe_open(SELFTEST_HART_KEY, PIPE_WRITE);
    if (fd < 0 || keep < 0)
        return selftest_fail("hart-pipe-open");

    for (int hart = 0; hart < ncpu; hart++) {
        if (spawn(hart_worker, hart, hart) < 0)
            return selftest_fail("hart-worker-spawn");
    }

    uint32_t seen = 0;
    for (int i = 0; i < ncpu; i++) {
        struct hart_result result;
        if (!read_exact(fd, &result, sizeof(result)))
            return selftest_fail("hart-worker-result");
        if (result.expected < 0 || result.expected >= ncpu ||
            result.actual != result.expected)
            return selftest_fail("hart-worker-affinity");

        uint32_t bit = 1u << result.actual;
        if (seen & bit)
            return selftest_fail("hart-worker-duplicate");
        seen |= bit;
    }

    pipe_close(keep);
    char extra;
    int eof = pipe_read(fd, &extra, 1);
    pipe_close(fd);
    if (eof != 0)
        return selftest_fail("hart-worker-eof");

    for (int hart = 0; hart < ncpu; hart++) {
        if ((seen & (1u << hart)) == 0)
            return selftest_fail("hart-worker-missing");
    }
    return true;
}

static void run_selftest(void) {
    if (!test_timer())
        return;
    printf("[PASS] timer advances\n");
    if (!test_block_irq())
        return;
    printf("[PASS] VirtIO block interrupt\n");
    if (!test_file_capacity())
        return;
    printf("[PASS] filesystem capacity and rejected-write integrity\n");
    if (!test_pipe_stream())
        return;
    printf("[PASS] pipe stream: 3072 bytes and EOF\n");
    if (!test_preemption())
        return;
    printf("[PASS] same-hart user preemption\n");
    if (!test_all_harts())
        return;
    printf("[PASS] worker affinity on all %d hart(s)\n", get_ncpu());
    printf("SELFTEST PASS\n");
}

static void print_irqstat(void) {
    int hart = get_hartid();
    int ncpu = get_ncpu();
    int ticks = get_ticks();
    int timer = get_irq_count(IRQ_TIMER);
    int uart = get_irq_count(IRQ_UART);
    int block = get_irq_count(IRQ_BLOCK);
    int switches = get_irq_count(IRQ_CONTEXT_SWITCH);

    printf("hart=%d ncpu=%d ticks=%d\n", hart, ncpu, ticks);
    printf("irq timer=%d uart=%d block=%d context-switch=%d\n",
           timer, uart, block, switches);
    printf("uart-rx-dropped=%d\n", get_irq_count(IRQ_UART_RX_DROPPED));
}

void main(void) {
    printf("RVKernel Lab | type help for commands\n");
    while (1) {
prompt:
        printf("> ");
        char cmdline[128];
        for (int i = 0;; i++) {
            char ch = getchar();
            putchar(ch);
            if (i == (int) sizeof(cmdline) - 1) {
                printf("command line too long\n");
                goto prompt;
            } else if (ch == '\r') {
                printf("\n");
                cmdline[i] = '\0';
                break;
            } else {
                cmdline[i] = ch;
            }
        }

        if (strcmp(cmdline, "help") == 0)
            printf("help      Show commands\n"
                   "hello     User-space syscall demo\n"
                   "irqstat   Interrupt and scheduling counters\n"
                   "selftest  Timer, disk IRQ, pipe, preemption, SMP\n"
                   "readfile  Read hello.txt\n"
                   "writefile Write sample text to hello.txt\n"
                   "exit      Exit shell (QEMU: Ctrl-a then x)\n");
        else if (strcmp(cmdline, "hello") == 0)
            printf("Hello world from shell!\n");
        else if (strcmp(cmdline, "exit") == 0)
            exit();
        else if (strcmp(cmdline, "readfile") == 0) {
            char buf[128];
            int len = readfile("hello.txt", buf, sizeof(buf) - 1);
            if (len < 0)
                printf("readfile failed: %d\n", len);
            else {
                buf[len] = '\0';
                printf("%s\n", buf);
            }
        } else if (strcmp(cmdline, "writefile") == 0)
            writefile("hello.txt", "Hello from shell!\n", 19);
        else if (strcmp(cmdline, "irqstat") == 0)
            print_irqstat();
        else if (strcmp(cmdline, "selftest") == 0)
            run_selftest();
        else
            printf("unknown command: %s\n", cmdline);
    }
}
