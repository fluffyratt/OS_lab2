#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <mqueue.h>
#include <semaphore.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif

#define NS_PER_S 1000000000ULL
#define DEFAULT_ITERS 100000ULL
#define DEFAULT_SIZE 64UL
#define MQ_MAX_MSG_FALLBACK 8192UL

typedef enum { MODE_LATENCY, MODE_THROUGHPUT } bench_mode_t;


typedef struct {
    _Atomic uint32_t state; /* 0=writer may write, 1=reader may read, 2=latency ack */
    size_t msg_size;
    unsigned char data[];
} spin_channel_t;

typedef struct {
    sem_t can_write;
    sem_t can_read;
    sem_t ack;
    size_t msg_size;
    unsigned char data[];
} shared_channel_t;

static uint64_t now_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC_RAW, &ts) != 0) {
        perror("clock_gettime");
        exit(2);
    }
    return (uint64_t)ts.tv_sec * NS_PER_S + (uint64_t)ts.tv_nsec;
}

static void die(const char *msg) {
    perror(msg);
    exit(2);
}

static void fill_pattern(unsigned char *buf, size_t n) {
    for (size_t i = 0; i < n; i++) buf[i] = (unsigned char)(i * 131u + 17u);
}

static int write_all(int fd, const void *buf, size_t n) {
    const unsigned char *p = buf;
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += (size_t)w;
        n -= (size_t)w;
    }
    return 0;
}

static int read_all(int fd, void *buf, size_t n) {
    unsigned char *p = buf;
    while (n) {
        ssize_t r = read(fd, p, n);
        if (r == 0) return -1;
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += (size_t)r;
        n -= (size_t)r;
    }
    return 0;
}

static void print_result(const char *method, bench_mode_t mode, size_t size, uint64_t iters,
                         uint64_t elapsed_ns) {
    double seconds = (double)elapsed_ns / 1e9;
    if (mode == MODE_LATENCY) {
        double one_way_ns = (double)elapsed_ns / (2.0 * (double)iters);
        printf("method=%s,mode=latency,message_size=%zu,iterations=%llu,elapsed_s=%.9f,latency_ns=%.2f\n",
               method, size, (unsigned long long)iters, seconds, one_way_ns);
    } else {
        double mib = ((double)size * (double)iters) / (1024.0 * 1024.0);
        double mib_s = seconds > 0.0 ? mib / seconds : 0.0;
        printf("method=%s,mode=throughput,message_size=%zu,iterations=%llu,elapsed_s=%.9f,throughput_MiB_s=%.2f\n",
               method, size, (unsigned long long)iters, seconds, mib_s);
    }
}

static void shared_init(shared_channel_t *ch, size_t msg_size) {
    if (sem_init(&ch->can_write, 1, 1) != 0) die("sem_init can_write");
    if (sem_init(&ch->can_read, 1, 0) != 0) die("sem_init can_read");
    if (sem_init(&ch->ack, 1, 0) != 0) die("sem_init ack");
    ch->msg_size = msg_size;
}

static void shared_destroy(shared_channel_t *ch) {
    sem_destroy(&ch->can_write);
    sem_destroy(&ch->can_read);
    sem_destroy(&ch->ack);
}

static int sem_wait_intr(sem_t *s) {
    int rc;
    while ((rc = sem_wait(s)) != 0 && errno == EINTR) {}
    return rc;
}

static void run_shared(const char *method, shared_channel_t *ch, bench_mode_t mode,
                       size_t size, uint64_t iters) {
    unsigned char *tmp = malloc(size ? size : 1);
    if (!tmp) die("malloc");
    fill_pattern(tmp, size);

    pid_t pid = fork();
    if (pid < 0) die("fork");
    if (pid == 0) {
        for (uint64_t i = 0; i < iters; i++) {
            if (sem_wait_intr(&ch->can_read) != 0) _exit(3);
            if (size && ch->data[0] != tmp[0]) _exit(4);
            if (mode == MODE_LATENCY) {
                if (sem_post(&ch->ack) != 0) _exit(5);
            } else {
                if (sem_post(&ch->can_write) != 0) _exit(6);
            }
        }
        _exit(0);
    }

    uint64_t t0 = now_ns();
    for (uint64_t i = 0; i < iters; i++) {
        if (sem_wait_intr(&ch->can_write) != 0) die("sem_wait can_write");
        memcpy(ch->data, tmp, size);
        if (sem_post(&ch->can_read) != 0) die("sem_post can_read");
        if (mode == MODE_LATENCY) {
            if (sem_wait_intr(&ch->ack) != 0) die("sem_wait ack");
            if (sem_post(&ch->can_write) != 0) die("sem_post can_write");
        }
    }
    int st = 0;
    if (waitpid(pid, &st, 0) < 0) die("waitpid");
    uint64_t t1 = now_ns();
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        fprintf(stderr, "child failed: %d\n", st);
        exit(3);
    }
    print_result(method, mode, size, iters, t1 - t0);
    free(tmp);
}


static void run_spin(const char *method, spin_channel_t *ch, bench_mode_t mode,
                     size_t size, uint64_t iters) {
    unsigned char *tmp = malloc(size ? size : 1);
    if (!tmp) die("malloc");
    fill_pattern(tmp, size);
    atomic_store_explicit(&ch->state, 0, memory_order_relaxed);
    ch->msg_size = size;

    pid_t pid = fork();
    if (pid < 0) die("fork");
    if (pid == 0) {
        for (uint64_t i = 0; i < iters; i++) {
            // процес буквально Укрутитьс€Ф у цикл≥, поки ≥нший процес не зм≥нить стан
            while (atomic_load_explicit(&ch->state, memory_order_acquire) != 1) { }
            if (size && ch->data[0] != tmp[0]) _exit(4);
            atomic_store_explicit(&ch->state, mode == MODE_LATENCY ? 2u : 0u,
                                  memory_order_release);
        }
        _exit(0);
    }

    uint64_t t0 = now_ns();
    for (uint64_t i = 0; i < iters; i++) {
        while (atomic_load_explicit(&ch->state, memory_order_acquire) != 0) { }
        memcpy(ch->data, tmp, size);
        atomic_store_explicit(&ch->state, 1, memory_order_release);
        if (mode == MODE_LATENCY) {
            while (atomic_load_explicit(&ch->state, memory_order_acquire) != 2) { }
            atomic_store_explicit(&ch->state, 0, memory_order_release);
        }
    }
    int st = 0;
    if (waitpid(pid, &st, 0) < 0) die("waitpid");
    uint64_t t1 = now_ns();
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        fprintf(stderr, "spin child failed: %d\\n", st);
        exit(3);
    }
    print_result(method, mode, size, iters, t1 - t0);
    free(tmp);
}

static void bench_mmap_anon_spin(bench_mode_t mode, size_t size, uint64_t iters) {
    size_t bytes = sizeof(spin_channel_t) + size;
    spin_channel_t *ch = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                              MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (ch == MAP_FAILED) die("mmap anonymous spin");
    run_spin("mmap-anon-spin", ch, mode, size, iters);
    munmap(ch, bytes);
}

static void bench_mmap_file_spin(bench_mode_t mode, size_t size, uint64_t iters) {
    char path[] = "/tmp/ipc_mmap_spin_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) die("mkstemp spin");
    unlink(path);
    size_t bytes = sizeof(spin_channel_t) + size;
    if (ftruncate(fd, (off_t)bytes) != 0) die("ftruncate spin");
    spin_channel_t *ch = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (ch == MAP_FAILED) die("mmap file spin");
    run_spin("mmap-file-spin", ch, mode, size, iters);
    munmap(ch, bytes);
    close(fd);
}

static void bench_mmap_anon(bench_mode_t mode, size_t size, uint64_t iters) {
    size_t bytes = sizeof(shared_channel_t) + size;
    shared_channel_t *ch = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                                MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (ch == MAP_FAILED) die("mmap anonymous");
    shared_init(ch, size);
    run_shared("mmap-anon-shared", ch, mode, size, iters);
    shared_destroy(ch);
    munmap(ch, bytes);
}

static void bench_mmap_file(bench_mode_t mode, size_t size, uint64_t iters) {
    char path[] = "/tmp/ipc_mmap_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) die("mkstemp");
    unlink(path);
    size_t bytes = sizeof(shared_channel_t) + size;
    if (ftruncate(fd, (off_t)bytes) != 0) die("ftruncate");
    shared_channel_t *ch = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (ch == MAP_FAILED) die("mmap file");
    shared_init(ch, size);
    run_shared("mmap-file-shared", ch, mode, size, iters);
    shared_destroy(ch);
    munmap(ch, bytes);
    close(fd);
}

static void bench_posix_shm(bench_mode_t mode, size_t size, uint64_t iters) {
    char name[64];
    snprintf(name, sizeof(name), "/ipc_bench_%ld_%d", (long)getpid(), rand());
    int fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0) die("shm_open");
    shm_unlink(name);
    size_t bytes = sizeof(shared_channel_t) + size;
    if (ftruncate(fd, (off_t)bytes) != 0) die("ftruncate shm");
    shared_channel_t *ch = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (ch == MAP_FAILED) die("mmap shm");
    shared_init(ch, size);
    run_shared("posix-shm", ch, mode, size, iters);
    shared_destroy(ch);
    munmap(ch, bytes);
    close(fd);
}

static void bench_duplex_fds(const char *method, int p2c_w, int p2c_r, int c2p_w, int c2p_r,
                             bench_mode_t mode, size_t size, uint64_t iters) {
    unsigned char *buf = malloc(size ? size : 1);
    if (!buf) die("malloc");
    fill_pattern(buf, size);
    pid_t pid = fork();
    if (pid < 0) die("fork");
    if (pid == 0) {
        unsigned char *tmp = malloc(size ? size : 1);
        if (!tmp) _exit(2);
        for (uint64_t i = 0; i < iters; i++) {
            if (read_all(p2c_r, tmp, size) != 0) _exit(3);
            if (mode == MODE_LATENCY && write_all(c2p_w, tmp, size) != 0) _exit(4);
        }
        free(tmp);
        _exit(0);
    }
    uint64_t t0 = now_ns();
    for (uint64_t i = 0; i < iters; i++) {
        if (write_all(p2c_w, buf, size) != 0) die("write");
        if (mode == MODE_LATENCY && read_all(c2p_r, buf, size) != 0) die("read ack");
    }
    int st = 0;
    if (waitpid(pid, &st, 0) < 0) die("waitpid");
    uint64_t t1 = now_ns();
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        fprintf(stderr, "child failed: %d\n", st);
        exit(3);
    }
    print_result(method, mode, size, iters, t1 - t0);
    free(buf);
}

static void bench_pipe(bench_mode_t mode, size_t size, uint64_t iters) {
    int a[2], b[2];
    if (pipe(a) || pipe(b)) die("pipe");
    bench_duplex_fds("pipe", a[1], a[0], b[1], b[0], mode, size, iters);
    close(a[0]); close(a[1]); close(b[0]); close(b[1]);
}

static void bench_socket(bench_mode_t mode, size_t size, uint64_t iters) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) die("socketpair");
    bench_duplex_fds("unix-socket", sv[0], sv[1], sv[1], sv[0], mode, size, iters);
    close(sv[0]); close(sv[1]);
}

static void bench_fifo(bench_mode_t mode, size_t size, uint64_t iters) {
    char f1[128], f2[128];
    snprintf(f1, sizeof(f1), "/tmp/ipc_fifo_a_%ld", (long)getpid());
    snprintf(f2, sizeof(f2), "/tmp/ipc_fifo_b_%ld", (long)getpid());
    unlink(f1); unlink(f2);
    if (mkfifo(f1, 0600) != 0 || mkfifo(f2, 0600) != 0) die("mkfifo");
    int a = open(f1, O_RDWR);
    int b = open(f2, O_RDWR);
    if (a < 0 || b < 0) die("open fifo");
    bench_duplex_fds("fifo", a, a, b, b, mode, size, iters);
    close(a); close(b); unlink(f1); unlink(f2);
}

static void bench_file(bench_mode_t mode, size_t size, uint64_t iters) {
    char path[] = "/tmp/ipc_file_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) die("mkstemp file");
    unlink(path);
    if (ftruncate(fd, (off_t)(size ? size : 1)) != 0) die("ftruncate file");

    size_t sync_bytes = sizeof(shared_channel_t) + size;
    shared_channel_t *sync = mmap(NULL, sync_bytes, PROT_READ | PROT_WRITE,
                                  MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (sync == MAP_FAILED) die("mmap sync");
    shared_init(sync, size);
    unsigned char *buf = malloc(size ? size : 1);
    if (!buf) die("malloc");
    fill_pattern(buf, size);

    pid_t pid = fork();
    if (pid < 0) die("fork");
    if (pid == 0) {
        unsigned char *tmp = malloc(size ? size : 1);
        if (!tmp) _exit(2);
        for (uint64_t i = 0; i < iters; i++) {
            if (sem_wait_intr(&sync->can_read) != 0) _exit(3);
            ssize_t r = pread(fd, tmp, size, 0);
            if (r != (ssize_t)size) _exit(4);
            if (mode == MODE_LATENCY) {
                if (sem_post(&sync->ack) != 0) _exit(5);
            } else {
                if (sem_post(&sync->can_write) != 0) _exit(6);
            }
        }
        free(tmp);
        _exit(0);
    }

    uint64_t t0 = now_ns();
    for (uint64_t i = 0; i < iters; i++) {
        if (sem_wait_intr(&sync->can_write) != 0) die("sem_wait file");
        ssize_t w = pwrite(fd, buf, size, 0);
        if (w != (ssize_t)size) die("pwrite");
        if (sem_post(&sync->can_read) != 0) die("sem_post file");
        if (mode == MODE_LATENCY) {
            if (sem_wait_intr(&sync->ack) != 0) die("sem_wait file ack");
            if (sem_post(&sync->can_write) != 0) die("sem_post file write");
        }
    }
    int st = 0;
    waitpid(pid, &st, 0);
    uint64_t t1 = now_ns();
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) { fprintf(stderr, "file child failed\n"); exit(3); }
    print_result("file-pread-pwrite", mode, size, iters, t1 - t0);
    shared_destroy(sync);
    munmap(sync, sync_bytes);
    close(fd);
    free(buf);
}

static long read_long_file(const char *path, long fallback) {
    FILE *f = fopen(path, "r");
    if (!f) return fallback;
    long v = fallback;
    if (fscanf(f, "%ld", &v) != 1) v = fallback;
    fclose(f);
    return v;
}

static void bench_mq(bench_mode_t mode, size_t size, uint64_t iters) {
    long sys_msgsize = read_long_file("/proc/sys/fs/mqueue/msgsize_max", (long)MQ_MAX_MSG_FALLBACK);
    if ((long)size > sys_msgsize) {
        fprintf(stderr, "mq: message_size=%zu exceeds system msgsize_max=%ld\n", size, sys_msgsize);
        exit(4);
    }
    char q1[64], q2[64];
    snprintf(q1, sizeof(q1), "/ipcq1_%ld", (long)getpid());
    snprintf(q2, sizeof(q2), "/ipcq2_%ld", (long)getpid());
    struct mq_attr attr = {0};
    attr.mq_maxmsg = 10;
    attr.mq_msgsize = (long)(size ? size : 1);
    mqd_t a = mq_open(q1, O_CREAT | O_RDWR, 0600, &attr);
    mqd_t b = mq_open(q2, O_CREAT | O_RDWR, 0600, &attr);
    if (a == (mqd_t)-1 || b == (mqd_t)-1) die("mq_open");
    mq_unlink(q1); mq_unlink(q2);
    unsigned char *buf = malloc(size ? size : 1);
    if (!buf) die("malloc");
    fill_pattern(buf, size);

    pid_t pid = fork();
    if (pid < 0) die("fork");
    if (pid == 0) {
        unsigned char *tmp = malloc(size ? size : 1);
        if (!tmp) _exit(2);
        for (uint64_t i = 0; i < iters; i++) {
            if (mq_receive(a, (char *)tmp, size ? size : 1, NULL) < 0) _exit(3);
            if (mode == MODE_LATENCY && mq_send(b, (char *)tmp, size ? size : 1, 0) != 0) _exit(4);
        }
        free(tmp); _exit(0);
    }
    uint64_t t0 = now_ns();
    for (uint64_t i = 0; i < iters; i++) {
        if (mq_send(a, (char *)buf, size ? size : 1, 0) != 0) die("mq_send");
        if (mode == MODE_LATENCY && mq_receive(b, (char *)buf, size ? size : 1, NULL) < 0) die("mq_receive");
    }
    int st = 0; waitpid(pid, &st, 0);
    uint64_t t1 = now_ns();
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) { fprintf(stderr, "mq child failed\n"); exit(3); }
    print_result("posix-mq", mode, size, iters, t1 - t0);
    mq_close(a); mq_close(b); free(buf);
}

static void mmap_private_demo(void) {
    size_t n = 4096;
    unsigned char *p = mmap(NULL, n, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) die("mmap private");
    p[0] = 1;
    pid_t pid = fork();
    if (pid < 0) die("fork");
    if (pid == 0) {
        p[0] = 99;
        printf("child sees: %u\n", p[0]);
        fflush(stdout);
        _exit(0);
    }
    waitpid(pid, NULL, 0);
    printf("parent sees after child write: %u (expected 1 due to copy-on-write)\n", p[0]);
    munmap(p, n);
}

static void print_capacity(void) {
    long pipe_max = read_long_file("/proc/sys/fs/pipe-max-size", -1);
    long mq_msgsize = read_long_file("/proc/sys/fs/mqueue/msgsize_max", -1);
    long mq_msgmax = read_long_file("/proc/sys/fs/mqueue/msg_max", -1);
    printf("capacity notes (system-dependent):\n");
    printf("  mmap/shared-memory: configured mapping size; constrained by virtual memory/RAM/backing store.\n");
    printf("  file: filesystem/free-space limits.\n");
    printf("  pipe/fifo: kernel pipe buffer; /proc/sys/fs/pipe-max-size=%ld bytes (maximum tunable cap, not necessarily default).\n", pipe_max);
    printf("  unix socket: SO_SNDBUF/SO_RCVBUF kernel socket buffers; query with getsockopt().\n");
    printf("  POSIX MQ: msgsize_max=%ld bytes, msg_max=%ld messages (system defaults/limits).\n", mq_msgsize, mq_msgmax);
}

static void usage(const char *argv0) {
    fprintf(stderr,
      "Usage:\n"
      "  %s <method> <latency|throughput> [message_size] [iterations]\n"
      "  %s mmap-private-demo\n"
      "  %s capacity\n\n"
      "Methods: mmap-anon, mmap-anon-spin, mmap-file, mmap-file-spin, shm, file, pipe, fifo, socket, mq\n",
      argv0, argv0, argv0);
}

int main(int argc, char **argv) {
    if (argc >= 2 && strcmp(argv[1], "mmap-private-demo") == 0) { mmap_private_demo(); return 0; }
    if (argc >= 2 && strcmp(argv[1], "capacity") == 0) { print_capacity(); return 0; }
    if (argc < 3) { usage(argv[0]); return 1; }

    bench_mode_t mode;
    if (strcmp(argv[2], "latency") == 0) mode = MODE_LATENCY;
    else if (strcmp(argv[2], "throughput") == 0) mode = MODE_THROUGHPUT;
    else { usage(argv[0]); return 1; }

    size_t size = argc >= 4 ? (size_t)strtoull(argv[3], NULL, 10) : DEFAULT_SIZE;
    uint64_t iters = argc >= 5 ? strtoull(argv[4], NULL, 10) : DEFAULT_ITERS;
    if (size == 0 || iters == 0) { fprintf(stderr, "message_size and iterations must be > 0\n"); return 1; }

    const char *m = argv[1];
    if (strcmp(m, "mmap-anon") == 0) bench_mmap_anon(mode, size, iters);
    else if (strcmp(m, "mmap-anon-spin") == 0) bench_mmap_anon_spin(mode, size, iters);
    else if (strcmp(m, "mmap-file") == 0) bench_mmap_file(mode, size, iters);
    else if (strcmp(m, "mmap-file-spin") == 0) bench_mmap_file_spin(mode, size, iters);
    else if (strcmp(m, "shm") == 0) bench_posix_shm(mode, size, iters);
    else if (strcmp(m, "file") == 0) bench_file(mode, size, iters);
    else if (strcmp(m, "pipe") == 0) bench_pipe(mode, size, iters);
    else if (strcmp(m, "fifo") == 0) bench_fifo(mode, size, iters);
    else if (strcmp(m, "socket") == 0) bench_socket(mode, size, iters);
    else if (strcmp(m, "mq") == 0) bench_mq(mode, size, iters);
    else { usage(argv[0]); return 1; }
    return 0;
}
