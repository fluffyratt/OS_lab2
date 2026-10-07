CC ?= gcc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra -Wpedantic
BENCH_LDLIBS ?= -pthread -lrt

BENCH_TARGET := ipc_bench
UI_TARGET := ipc_ui
BENCH_SRC := src/ipc_bench.c
UI_SRC := src/ipc_ui.c

.PHONY: all bench ui clean smoke
all: $(BENCH_TARGET) $(UI_TARGET)

bench: $(BENCH_TARGET)
ui: $(UI_TARGET)

$(BENCH_TARGET): $(BENCH_SRC)
	$(CC) $(CFLAGS) -o $@ $< $(BENCH_LDLIBS)

$(UI_TARGET): $(UI_SRC) $(BENCH_TARGET)
	$(CC) $(CFLAGS) -o $@ $(UI_SRC)

smoke: $(BENCH_TARGET)
	./$(BENCH_TARGET) mmap-private-demo
	./$(BENCH_TARGET) capacity
	./$(BENCH_TARGET) mmap-anon latency 64 1000
	./$(BENCH_TARGET) mmap-file throughput 4096 1000
	./$(BENCH_TARGET) shm latency 64 1000
	./$(BENCH_TARGET) pipe latency 64 1000
	./$(BENCH_TARGET) fifo throughput 4096 1000
	./$(BENCH_TARGET) socket latency 64 1000
	./$(BENCH_TARGET) file throughput 4096 1000
	-./$(BENCH_TARGET) mq latency 64 1000

clean:
	rm -f $(BENCH_TARGET) $(UI_TARGET)
