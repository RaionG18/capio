# io_uring example

A producer/consumer pair that writes a set of files and reads them back,
using either plain POSIX calls or io_uring. It is a small benchmark and a
correctness test in one binary, and it runs the same way with or without
CAPIO.

## What the program does

The producer fills each file with a fixed, position-based pattern and prints
an FNV-1a checksum per file. The consumer reads the files back, rebuilds the
expected pattern on its own, and checks that every byte matches. It prints
`verified N/N files OK` and exits non-zero if any file is wrong. Because the
consumer rebuilds the pattern itself, the two sides share no data — a match
proves the bytes came through intact.

Both engines are in one binary, so you can compare them by changing one
flag.

## Build

The example is self-contained and needs `liburing`. Build it on its own:

```
cmake -S . -B build
cmake --build build
```

## Run without CAPIO

Pick a directory, write the files, then read them back:

```
./build/one_to_one -r producer -e posix    -n 100 -f 1048576 -c 65536 -d /tmp/data
./build/one_to_one -r consumer -e posix    -n 100 -f 1048576 -c 65536 -d /tmp/data
```

Swap `-e posix` for `-e io_uring` to use the ring instead. Add `-q` to set
how many operations io_uring keeps in flight:

```
./build/one_to_one -r producer -e io_uring -q 32 -n 100 -f 1048576 -c 65536 -d /tmp/data
```

### Flags

| Flag | Meaning |
|------|---------|
| `-r` | `producer` or `consumer` |
| `-n` | number of files |
| `-f` | bytes per file |
| `-c` | bytes per read or write |
| `-q` | operations in flight (io_uring only) |
| `-e` | `posix` or `io_uring` |
| `-d` | working directory (default `.`) |

## Run under CAPIO

`capio_config.json` describes the workflow: the producer writes the files,
the consumer reads them, and CAPIO commits each file when it is closed. Start
the server, then run the two steps with the CAPIO library preloaded:

```
CAPIO_DIR=/tmp/mnt capio_server -c capio_config.json &

CAPIO_DIR=/tmp/mnt CAPIO_WORKFLOW_NAME=io_uring_example CAPIO_APP_NAME=producer \
  LD_PRELOAD=libcapio_posix.so ./build/one_to_one -r producer -e posix -n 100 -f 1048576 -c 65536 -d /tmp/mnt

CAPIO_DIR=/tmp/mnt CAPIO_WORKFLOW_NAME=io_uring_example CAPIO_APP_NAME=consumer \
  LD_PRELOAD=libcapio_posix.so ./build/one_to_one -r consumer -e posix -n 100 -f 1048576 -c 65536 -d /tmp/mnt
```

With the POSIX engine the files pass through CAPIO, and the consumer verifies
them. The files do not appear on the real filesystem; drop `LD_PRELOAD` from
the consumer and it fails, which shows CAPIO carried the data.

The io_uring engine does **not** work under CAPIO yet. CAPIO intercepts
`open` and hands back a file descriptor of its own, but io_uring passes that
descriptor straight to the kernel, which does not know it, so the write fails
with `Bad file descriptor`. Teaching CAPIO to serve io_uring is the next step
of this work.

## Tests and benchmark

`engine_test.cpp` runs a set of checks against both engines — round trips,
partial last chunks, and detection of corrupted, swapped, or truncated files
— plus a cross-check that both engines produce the same bytes. Build and run
it:

```
g++ -std=c++17 engine_test.cpp -luring -o engine_test && ./engine_test
```

`bench.sh` runs all four setups — POSIX and io_uring, on the plain
filesystem and under CAPIO — and prints median throughput and a CSV:

```
./bench.sh build/one_to_one
```

## Limits

The example uses no fixed buffers and no SQPOLL. It is meant to be read and
run, not to run as fast as it could.
