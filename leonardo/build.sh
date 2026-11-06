#!/usr/bin/env bash
# Builds everything the Leonardo tests need. Run it on a LOGIN node: CMake
# downloads dependencies (FetchContent), and compute nodes have no internet.
#
#   ./leonardo/build.sh            build what is missing, rebuild CAPIO
#   ./leonardo/build.sh --clean    remove the CAPIO build first
#
# Leonardo runs RHEL 8.8 (kernel 4.18): no io_uring in the kernel, and its
# <linux/io_uring.h> is too old for CAPIO's io_uring code. liburing ships a
# current copy of that header; CAPIO is compiled against it.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/env.sh"

jobs="${BUILD_JOBS:-8}"
mkdir -p "$LEO_DEPS/src" "$LEO_DEPS/bin"

if [ "${1:-}" = "--clean" ]; then
    rm -rf "$CAPIO_BUILD"
fi

# --- liburing ------------------------------------------------------------------
if [ ! -f "$LIBURING_PREFIX/lib/pkgconfig/liburing.pc" ]; then
    echo "== liburing 2.14"
    rm -rf "$LEO_DEPS/src/liburing"
    git clone --depth 1 -b liburing-2.14 https://github.com/axboe/liburing.git "$LEO_DEPS/src/liburing"
    (cd "$LEO_DEPS/src/liburing" && ./configure --prefix="$LIBURING_PREFIX" --cc=gcc --cxx=g++ &&
        make -j"$jobs" -C src && make install)
fi

# A current <linux/io_uring.h> for CAPIO (and nothing else in that directory).
compat="$LEO_DEPS/compat/include"
mkdir -p "$compat/linux"
cp "$LIBURING_PREFIX/include/liburing/io_uring.h" "$compat/linux/io_uring.h"

# --- CAPIO ---------------------------------------------------------------------
echo "== CAPIO ($(git -C "$CAPIO_REPO" rev-parse --short HEAD) on $(git -C "$CAPIO_REPO" rev-parse --abbrev-ref HEAD))"
cmake -S "$CAPIO_REPO" -B "$CAPIO_BUILD" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCAPIO_LOG=OFF \
    -DCAPIO_BUILD_TESTS=OFF \
    -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ \
    -DCMAKE_CXX_FLAGS="-isystem $compat"
cmake --build "$CAPIO_BUILD" -j"$jobs"

# --- benchmark programs --------------------------------------------------------
echo "== one_to_one, quickstart, no_io_uring"
inc="-isystem $compat -I$LIBURING_PREFIX/include"
lib="-L$LIBURING_PREFIX/lib -Wl,-rpath,$LIBURING_PREFIX/lib -luring"
g++ -std=c++17 -O2 $inc -o "$ONE_TO_ONE" "$CAPIO_REPO/examples/io_uring/one_to_one.cpp" $lib
g++ -std=c++17 -O2 $inc -o "$LEO_DEPS/bin/qs_producer" "$CAPIO_REPO/examples/io_uring_quickstart/producer.cpp" $lib
g++ -std=c++17 -O2 $inc -o "$LEO_DEPS/bin/qs_consumer" "$CAPIO_REPO/examples/io_uring_quickstart/consumer.cpp" $lib
gcc -O2 -o "$LEO_DEPS/bin/no_io_uring" "$LEO_DIR/tools/no_io_uring.c"

echo
echo "Build complete:"
ls -la "$CAPIO_SERVER" "$CAPIO_LIB" "$ONE_TO_ONE"
