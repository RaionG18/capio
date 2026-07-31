// Self-check battery for the POSIX engine. Each check exercises one property
// the engine must hold for F1 to be considered done. Positive checks expect
// posix_read_file() to accept a correctly written file; detection checks expect
// it to REJECT a file that was corrupted, swapped, or truncated on disk — a
// verifier that never fails would verify nothing.
//
// Ponytail: includes the .cpp directly because the functions under test are
// `static` in one_to_one.cpp. Zero-refactor, no build system, no framework.
// Upgrade path: extract the helpers into a header if the example grows.
// Compile: g++ -std=c++17 -o /tmp/posix_test posix_test.cpp && /tmp/posix_test
#define main one_to_one_main   // shadow the example's main() while including it
#include "one_to_one.cpp"
#undef main

#include <cassert>
#include <sys/stat.h>

static Config make_cfg(const char *dir, long long file_size, long long chunk_size)
{
    Config cfg;
    cfg.dir        = dir;
    cfg.file_size  = file_size;
    cfg.chunk_size = chunk_size;
    return cfg;
}

static long long on_disk_size(const std::string &path)
{
    struct stat st;
    assert(stat(path.c_str(), &st) == 0);
    return st.st_size;
}

// Base case, already the trickiest: file_size not a multiple of chunk_size, so
// the last chunk is partial. Checks exact on-disk size and round-trip.
static void check_partial_chunk()
{
    Config cfg = make_cfg("/tmp", 10000, 4096);   // 3 chunks, last = 1808 bytes
    posix_write_file(cfg, 0);
    assert(on_disk_size(file_path(cfg.dir, 0)) == cfg.file_size);
    assert(posix_read_file(cfg, 0));
    unlink(file_path(cfg.dir, 0).c_str());
}

// T1: every file in a multi-file run is written and verified.
static void check_multi_file()
{
    Config cfg = make_cfg("/tmp", 8192, 4096);
    for (int i = 0; i < 5; ++i)
        posix_write_file(cfg, i);
    for (int i = 0; i < 5; ++i)
        assert(posix_read_file(cfg, i));
    for (int i = 0; i < 5; ++i)
        unlink(file_path(cfg.dir, i).c_str());
}

// T2: a single flipped byte on disk is detected.
static void check_corruption()
{
    Config cfg = make_cfg("/tmp", 8192, 4096);
    posix_write_file(cfg, 0);

    const std::string path = file_path(cfg.dir, 0);
    int fd = open(path.c_str(), O_RDWR);
    assert(fd >= 0);
    unsigned char b;
    assert(pread(fd, &b, 1, 5000) == 1);
    b ^= 0xFF;
    assert(pwrite(fd, &b, 1, 5000) == 1);
    close(fd);

    assert(!posix_read_file(cfg, 0));   // must reject
    unlink(path.c_str());
}

// T3: reading file_0002's slot when it holds file_0007's content is detected,
// i.e. the pattern is per-file, not just per-position.
static void check_isolation()
{
    Config cfg = make_cfg("/tmp", 8192, 4096);
    posix_write_file(cfg, 2);
    posix_write_file(cfg, 7);
    assert(posix_read_file(cfg, 2));
    assert(posix_read_file(cfg, 7));

    // Move file_0007's bytes into file_0002's slot.
    rename(file_path(cfg.dir, 7).c_str(), file_path(cfg.dir, 2).c_str());
    assert(!posix_read_file(cfg, 2));   // content belongs to idx 7, not 2

    unlink(file_path(cfg.dir, 2).c_str());
}

// T4: a truncated file is detected (EOF before the expected length).
static void check_truncation()
{
    Config cfg = make_cfg("/tmp", 8192, 4096);
    posix_write_file(cfg, 0);

    const std::string path = file_path(cfg.dir, 0);
    assert(truncate(path.c_str(), 5000) == 0);
    assert(!posix_read_file(cfg, 0));   // must reject

    unlink(path.c_str());
}

// T5: chunk_size == file_size (a single, full chunk; no partial path).
static void check_single_chunk()
{
    Config cfg = make_cfg("/tmp", 4096, 4096);
    posix_write_file(cfg, 0);
    assert(posix_read_file(cfg, 0));
    unlink(file_path(cfg.dir, 0).c_str());
}

// T6: file_size an exact multiple of chunk_size (no partial last chunk).
static void check_exact_multiple()
{
    Config cfg = make_cfg("/tmp", 8192, 4096);
    posix_write_file(cfg, 0);
    assert(posix_read_file(cfg, 0));
    unlink(file_path(cfg.dir, 0).c_str());
}

// T7: a realistic size with a partial last chunk, to shake out any size bug at
// scale. Ceiling: does not reach >4 GB, so it does not prove 64-bit overflow
// safety, only that the engine works far past a handful of chunks.
static void check_large()
{
    Config cfg = make_cfg("/tmp", 8LL * 1024 * 1024 + 123, 1024 * 1024);
    posix_write_file(cfg, 0);
    assert(on_disk_size(file_path(cfg.dir, 0)) == cfg.file_size);
    assert(posix_read_file(cfg, 0));
    unlink(file_path(cfg.dir, 0).c_str());
}

int main()
{
    check_partial_chunk();
    check_multi_file();
    check_corruption();
    check_isolation();
    check_truncation();
    check_single_chunk();
    check_exact_multiple();
    check_large();

    std::cout << "POSIX engine self-test PASSED (8 checks)\n";
    return 0;
}
