#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#define OUT_MAX 16384
#define CMD_MAX 1024

static const char *METHODS[] = {
    "mmap-anon", "mmap-anon-spin", "mmap-file", "mmap-file-spin",
    "shm", "file", "pipe", "fifo", "socket", "mq"
};
static const char *METHOD_LABELS[] = {
    "mmap anonymous + semaphore",
    "mmap anonymous + atomic spin",
    "mmap file-backed + semaphore",
    "mmap file-backed + atomic spin",
    "POSIX shared memory",
    "file pread/pwrite",
    "anonymous pipe",
    "named pipe (FIFO)",
    "Unix domain socket",
    "POSIX message queue"
};
static const size_t METHOD_COUNT = sizeof(METHODS) / sizeof(METHODS[0]);
static const unsigned long SIZES[] = {8, 64, 1024, 4096, 65536, 1048576};
static const char *SIZE_LABELS[] = {"8 B", "64 B", "1 KiB", "4 KiB", "64 KiB", "1 MiB"};
static const size_t SIZE_COUNT = sizeof(SIZES) / sizeof(SIZES[0]);

static struct termios old_termios;
static int raw_enabled = 0;
static int method_idx = 1;
static int mode_idx = 0;
static int size_idx = 1;
static unsigned long long iterations = 20000;
static char output[OUT_MAX] = "Ready. Choose parameters and run a benchmark.";

static void restore_terminal(void) {
    if (raw_enabled) tcsetattr(STDIN_FILENO, TCSAFLUSH, &old_termios);
    raw_enabled = 0;
    printf("\033[?25h\033[0m\n");
    fflush(stdout);
}

static void on_signal(int sig) {
    restore_terminal();
    _exit(128 + sig);
}

static int enable_raw(void) {
    if (tcgetattr(STDIN_FILENO, &old_termios) != 0) return -1;
    struct termios t = old_termios;
    t.c_lflag &= (tcflag_t)~(ECHO | ICANON);
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &t) != 0) return -1;
    raw_enabled = 1;
    atexit(restore_terminal);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    printf("\033[?25l");
    return 0;
}

static void disable_raw_temporarily(void) {
    if (raw_enabled) tcsetattr(STDIN_FILENO, TCSAFLUSH, &old_termios);
}

static void reenable_raw(void) {
    struct termios t = old_termios;
    t.c_lflag &= (tcflag_t)~(ECHO | ICANON);
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &t);
}

static void safe_append(char *dst, size_t cap, const char *src) {
    size_t used = strlen(dst);
    if (used + 1 >= cap) return;
    strncat(dst, src, cap - used - 1);
}

static int capture_command(const char *cmd, char *buf, size_t cap) {
    char full[CMD_MAX];
    snprintf(full, sizeof(full), "%s 2>&1", cmd);
    FILE *p = popen(full, "r");
    if (!p) {
        snprintf(buf, cap, "popen failed: %s", strerror(errno));
        return -1;
    }
    buf[0] = '\0';
    char line[1024];
    while (fgets(line, sizeof(line), p)) safe_append(buf, cap, line);
    int status = pclose(p);
    if (buf[0] == '\0') snprintf(buf, cap, "Command produced no output.");
    if (status == -1) return -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -1;
}

static void auto_iterations(void) {
    unsigned long s = SIZES[size_idx];
    if (mode_idx == 0) iterations = (s <= 4096) ? 20000ULL : 2000ULL;
    else iterations = (s <= 4096) ? 50000ULL : (s <= 65536 ? 10000ULL : 1000ULL);
}

static void print_line(const char *s, int width) {
    int n = (int)strlen(s);
    if (n > width) n = width;
    fwrite(s, 1, (size_t)n, stdout);
    for (int i = n; i < width; ++i) putchar(' ');
}

static void draw_wrapped_text(const char *text, int width, int max_lines) {
    int col = 0, lines = 1;
    for (const char *p = text; *p && lines <= max_lines; ++p) {
        if (*p == '\n') {
            putchar('\n');
            col = 0;
            lines++;
            continue;
        }
        if (col >= width) {
            putchar('\n');
            col = 0;
            lines++;
            if (lines > max_lines) break;
        }
        putchar(*p);
        col++;
    }
    putchar('\n');
}

static void draw_screen(int selected) {
    struct winsize ws = {0};
    ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws);
    int cols = ws.ws_col ? ws.ws_col : 100;
    int rows = ws.ws_row ? ws.ws_row : 30;
    if (cols < 76) cols = 76;

    printf("\033[2J\033[H");
    printf("\033[1mIPC BENCHMARK — interactive TUI\033[0m\n");
    for (int i = 0; i < cols - 1; ++i) putchar('-');
    putchar('\n');

    const char *labels[] = {
        "Method", "Mode", "Message size", "Iterations",
        "Run benchmark", "Run all -> results/results.csv", "View result summary",
        "MAP_PRIVATE demo", "Channel capacity", "Exit"
    };

    for (int i = 0; i < 10; ++i) {
        char left[40];
        snprintf(left, sizeof(left), "%c %-31s", i == selected ? '>' : ' ', labels[i]);
        if (i == selected) printf("\033[7m");
        print_line(left, 35);
        if (i == selected) printf("\033[0m");
        printf("  ");
        if (i == 0) printf("%s", METHOD_LABELS[method_idx]);
        else if (i == 1) printf("%s", mode_idx ? "throughput" : "latency");
        else if (i == 2) printf("%s", SIZE_LABELS[size_idx]);
        else if (i == 3) printf("%llu", iterations);
        putchar('\n');
    }

    for (int i = 0; i < cols - 1; ++i) putchar('-');
    printf("\n\033[1mOutput\033[0m\n");
    int max_lines = rows - 18;
    if (max_lines < 4) max_lines = 4;
    draw_wrapped_text(output, cols - 2, max_lines);
    for (int i = 0; i < cols - 1; ++i) putchar('-');
    printf("\nArrows: navigate/change | Enter: select | A: auto iterations | Q: quit\n");
    fflush(stdout);
}

enum { K_OTHER, K_UP, K_DOWN, K_LEFT, K_RIGHT, K_ENTER, K_QUIT, K_AUTO };

static int read_key(void) {
    unsigned char c;
    if (read(STDIN_FILENO, &c, 1) != 1) return K_OTHER;
    if (c == 'q' || c == 'Q') return K_QUIT;
    if (c == 'a' || c == 'A') return K_AUTO;
    if (c == '\r' || c == '\n') return K_ENTER;
    if (c == 27) {
        unsigned char seq[2];
        if (read(STDIN_FILENO, &seq[0], 1) != 1) return K_OTHER;
        if (read(STDIN_FILENO, &seq[1], 1) != 1) return K_OTHER;
        if (seq[0] == '[') {
            if (seq[1] == 'A') return K_UP;
            if (seq[1] == 'B') return K_DOWN;
            if (seq[1] == 'C') return K_RIGHT;
            if (seq[1] == 'D') return K_LEFT;
        }
    }
    return K_OTHER;
}

static void edit_iterations(void) {
    char buf[64];
    disable_raw_temporarily();
    printf("\033[2J\033[HIterations (>0): ");
    fflush(stdout);
    if (!fgets(buf, sizeof(buf), stdin)) {
        reenable_raw();
        return;
    }
    buf[strcspn(buf, "\r\n")] = '\0';
    char *end = NULL;
    unsigned long long v = strtoull(buf, &end, 10);
    if (end && *end == '\0' && v > 0) iterations = v;
    else snprintf(output, sizeof(output), "Invalid iterations value: '%s'", buf);
    reenable_raw();
}

static void run_one(void) {
    if (strcmp(METHODS[method_idx], "mq") == 0 && SIZES[size_idx] > 8192) {
        snprintf(output, sizeof(output),
                 "POSIX MQ skipped: selected message size %lu B exceeds the typical Linux msgsize_max=8192 B.\n"
                 "Choose 4 KiB or smaller, or inspect 'Channel capacity'.", SIZES[size_idx]);
        return;
    }
    char cmd[CMD_MAX];
    snprintf(cmd, sizeof(cmd), "./ipc_bench %s %s %lu %llu",
             METHODS[method_idx], mode_idx ? "throughput" : "latency",
             SIZES[size_idx], iterations);
    snprintf(output, sizeof(output), "Running: %s ...", cmd);
    draw_screen(4);
    char raw[OUT_MAX];
    int rc = capture_command(cmd, raw, sizeof(raw));
    snprintf(output, sizeof(output), "Command: %s\nexit=%d\n", cmd, rc);
    safe_append(output, sizeof(output), raw);
}

static void run_all(void) {
    snprintf(output, sizeof(output),
             "Running full benchmark (5 repeats). This can take several minutes...\n"
             "Output will be written to results/results.csv");
    draw_screen(5);
    int rc = capture_command("REPEATS=5 ./scripts/run_bench.sh", output, sizeof(output));
    if (rc == 0) safe_append(output, sizeof(output), "\nDone. Open 'View result summary' or results/results.csv.");
    else {
        char suffix[128];
        snprintf(suffix, sizeof(suffix), "\nrun_bench.sh exited with code %d", rc);
        safe_append(output, sizeof(output), suffix);
    }
}

static void view_results(void) {
    const char *cmd =
        "if [ -f results/results.csv ]; then "
        "echo 'Rows:' $(($(wc -l < results/results.csv)-1)); "
        "echo; echo 'Last 12 measurements:'; tail -n 12 results/results.csv; "
        "else echo 'results/results.csv not found. Run all first.'; fi";
    capture_command(cmd, output, sizeof(output));
}

int main(void) {
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        fprintf(stderr, "ipc_ui requires an interactive terminal.\n");
        return 1;
    }
    if (enable_raw() != 0) {
        perror("terminal setup");
        return 1;
    }

    int selected = 0;
    auto_iterations();
    for (;;) {
        draw_screen(selected);
        int key = read_key();
        if (key == K_QUIT) break;
        if (key == K_AUTO) {
            auto_iterations();
            snprintf(output, sizeof(output), "Iterations set automatically for current mode/message size.");
            continue;
        }
        if (key == K_UP) selected = (selected + 9) % 10;
        else if (key == K_DOWN) selected = (selected + 1) % 10;
        else if (key == K_LEFT || key == K_RIGHT) {
            int d = (key == K_RIGHT) ? 1 : -1;
            if (selected == 0) method_idx = (method_idx + d + (int)METHOD_COUNT) % (int)METHOD_COUNT;
            else if (selected == 1) mode_idx = 1 - mode_idx;
            else if (selected == 2) size_idx = (size_idx + d + (int)SIZE_COUNT) % (int)SIZE_COUNT;
            if (selected == 1 || selected == 2) auto_iterations();
        } else if (key == K_ENTER) {
            if (selected == 0) method_idx = (method_idx + 1) % (int)METHOD_COUNT;
            else if (selected == 1) { mode_idx = 1 - mode_idx; auto_iterations(); }
            else if (selected == 2) { size_idx = (size_idx + 1) % (int)SIZE_COUNT; auto_iterations(); }
            else if (selected == 3) edit_iterations();
            else if (selected == 4) run_one();
            else if (selected == 5) run_all();
            else if (selected == 6) view_results();
            else if (selected == 7) capture_command("./ipc_bench mmap-private-demo", output, sizeof(output));
            else if (selected == 8) capture_command("./ipc_bench capacity", output, sizeof(output));
            else if (selected == 9) break;
        }
    }

    restore_terminal();
    return 0;
}
