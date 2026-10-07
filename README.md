# IPC Benchmark Lab (C / Linux)

Демонстраційний проєкт для порівняння IPC-механізмів:

- `mmap()` у режимах `MAP_SHARED | MAP_ANONYMOUS` та file-backed `MAP_SHARED`; для обох є semaphore і atomic spin synchronization;
- `MAP_PRIVATE` як окрема демонстрація copy-on-write (не повноцінний канал обміну змінами);
- POSIX shared memory: `shm_open()` + `mmap()`;
- file I/O: `pwrite()` / `pread()` з міжпроцесною синхронізацією;
- anonymous pipes;
- FIFO (`mkfifo()`);
- Unix domain sockets (`socketpair(AF_UNIX, SOCK_STREAM, ...)`);
- POSIX message queues (`mq_open`, `mq_send`, `mq_receive`).

## Збірка

```bash
make
```

Потрібен Linux, GCC/Clang, POSIX semaphores і POSIX message queues. На сучасному glibc `-lrt` може бути не обов'язковим, але залишений для сумісності.

## CLI

```bash
./ipc_bench <method> <latency|throughput> <message_size> <iterations>
```

Методи:

```text
mmap-anon
mmap-anon-spin
mmap-file
mmap-file-spin
shm
file
pipe
fifo
socket
mq
```

Приклади:

```bash
./ipc_bench mmap-anon latency 64 100000
./ipc_bench mmap-file throughput 65536 10000
./ipc_bench socket latency 64 100000
./ipc_bench pipe throughput 1048576 1000
./ipc_bench mmap-private-demo
./ipc_bench capacity
```

## Автоматичний benchmark

```bash
REPEATS=5 ./scripts/run_bench.sh
```

CSV буде записаний у `results/results.csv`.

Рекомендація для звіту: зробити 5–10 повторів, а в таблиці показувати медіану. Важливо запускати всі методи на одній машині, з однаковим навантаженням і без паралельних важких задач.

## Що саме вимірюється

### Latency

Ping-pong: процес A передає повідомлення процесу B, B повертає acknowledgement/відповідь. Одностороння оцінка:

`latency ≈ elapsed / (2 * iterations)`.

### Throughput

Процес A послідовно передає N повідомлень, процес B їх приймає. Результат:

`MiB/s = message_size * iterations / elapsed`.

### Важлива методологічна примітка

Shared-memory реалізації тут використовують process-shared POSIX semaphores. Отже вимірюється не тільки копіювання даних у спільну пам'ять, а повний практичний канал "shared memory + synchronization". Це чесніше для прикладного IPC, але semaphore wakeups можуть домінувати для малих повідомлень.

File benchmark використовує `pwrite/pread` і shared semaphores лише для узгодження producer/consumer. Дані між процесами при цьому проходять саме через file I/O path/page cache. `fsync()` навмисно не викликається: це окремий режим durability/storage latency, а не чистий IPC.

POSIX MQ має системний ліміт розміру повідомлення (`/proc/sys/fs/mqueue/msgsize_max`), тому великі message sizes можуть бути пропущені скриптом.

## Додатковий режим mmap-spin

`mmap-anon-spin` та `mmap-file-spin` використовують C11 atomics із acquire/release memory ordering і busy-wait замість POSIX semaphore. Це навмисно показує trade-off: нижча latency без sleep/wakeup syscalls проти постійного використання CPU під час очікування.


## Інтерактивний TUI (`ipc_ui`)

Проєкт містить термінальний інтерфейс, тому немає потреби вручну вводити кожну команду для запуску benchmark-тестів. Інтерфейс використовує ANSI-керування терміналом і `termios`, тому **жодних додаткових бібліотек для UI не потрібно** — достатньо стандартних засобів для компіляції C.

Збірка і запуск:

```bash
make clean && make
./ipc_ui
```

Керування:

- `Up/Down` — переміщення між пунктами меню
- `Left/Right` — зміна методу, режиму або розміру повідомлення
- `Enter` — вибір або редагування поточного пункту
- `A` — встановити рекомендовану кількість ітерацій для вибраного режиму та розміру повідомлення
- `Q` — вихід

Інтерфейс дозволяє запускати окремий benchmark, запускати повний набір тестів із записом результатів у `results/results.csv`, переглядати коротке зведення результатів, демонструвати роботу `MAP_PRIVATE` та виводити інформацію про місткість IPC-каналів. `ipc_ui` є лише frontend-інтерфейсом: усі вимірювання, як і раніше, виконуються тим самим C-виконуваним файлом `ipc_bench`.
