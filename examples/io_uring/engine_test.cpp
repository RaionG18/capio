// Self-check battery for the I/O engines. Each check exercises one property an
// engine must hold for F1 to be considered done. The whole battery runs against
// BOTH the POSIX and io_uring engines through the WRITE/READ function pointers,
// so passing it is objective evidence the two engines are equivalent. A final
// cross-engine check writes with one engine and reads with the other.
//
// Positive checks expect READ to accept a correctly written file; detection
// checks expect it to REJECT a file corrupted, swapped, or truncated on disk —
// a verifier that never fails would verify nothing.
//
// Ponytail: includes the .cpp directly because the functions under test are
// `static` in one_to_one.cpp. Zero-refactor, no build system, no framework.
// Compile: g++ -std=c++17 engine_test.cpp -luring -o /tmp/engine_test
#define main one_to_one_main   // shadow the example's main() while including it
#include "one_to_one.cpp"
#undef main

#include <cassert>
#include <sys/stat.h>

// The engine under test. main() points these at each engine in turn.
static uint64_t (*WRITE)(const Config &, int) = nullptr;
static bool     (*READ)(const Config &, int)  = nullptr;

// Owning wrappers so the uring engine matches the (Config,idx) test signature.
// A per-call ring is fine here — these tests check correctness, not throughput.
static uint64_t uring_write_owned(const Config &cfg, int idx)
{
    UringCtx ctx(cfg);
    return uring_write_file(ctx, cfg, idx);
}
static bool uring_read_owned(const Config &cfg, int idx)
{
    UringCtx ctx(cfg);
    return uring_read_file(ctx, cfg, idx);
}

static Config make_cfg(const char *dir, long long file_size, long long chunk_size)
{
    Config cfg;
    cfg.dir         = dir;
    cfg.file_size   = file_size;
    cfg.chunk_size  = chunk_size;
    cfg.queue_depth = 32;   // used by the io_uring engine, ignored by POSIX
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
    WRITE(cfg, 0);
    assert(on_disk_size(file_path(cfg.dir, 0)) == cfg.file_size);
    assert(READ(cfg, 0));
    unlink(file_path(cfg.dir, 0).c_str());
}

// T1: every file in a multi-file run is written and verified.
static void check_multi_file()
{
    Config cfg = make_cfg("/tmp", 8192, 4096);
    for (int i = 0; i < 5; ++i)
        WRITE(cfg, i);
    for (int i = 0; i < 5; ++i)
        assert(READ(cfg, i));
    for (int i = 0; i < 5; ++i)
        unlink(file_path(cfg.dir, i).c_str());
}

// T2: a single flipped byte on disk is detected.
static void check_corruption()
{
    Config cfg = make_cfg("/tmp", 8192, 4096);
    WRITE(cfg, 0);

    const std::string path = file_path(cfg.dir, 0);
    int fd = open(path.c_str(), O_RDWR);
    assert(fd >= 0);
    unsigned char b;
    assert(pread(fd, &b, 1, 5000) == 1);
    b ^= 0xFF;
    assert(pwrite(fd, &b, 1, 5000) == 1);
    close(fd);

    assert(!READ(cfg, 0));   // must reject
    unlink(path.c_str());
}

// T3: reading file_0002's slot when it holds file_0007's content is detected,
// i.e. the pattern is per-file, not just per-position.
static void check_isolation()
{
    Config cfg = make_cfg("/tmp", 8192, 4096);
    WRITE(cfg, 2);
    WRITE(cfg, 7);
    assert(READ(cfg, 2));
    assert(READ(cfg, 7));

    // Move file_0007's bytes into file_0002's slot.
    rename(file_path(cfg.dir, 7).c_str(), file_path(cfg.dir, 2).c_str());
    assert(!READ(cfg, 2));   // content belongs to idx 7, not 2

    unlink(file_path(cfg.dir, 2).c_str());
}

// T4: a truncated file is detected (EOF before the expected length).
static void check_truncation()
{
    Config cfg = make_cfg("/tmp", 8192, 4096);
    WRITE(cfg, 0);

    const std::string path = file_path(cfg.dir, 0);
    assert(truncate(path.c_str(), 5000) == 0);
    assert(!READ(cfg, 0));   // must reject

    unlink(path.c_str());
}

// T5: chunk_size == file_size (a single, full chunk; no partial path).
static void check_single_chunk()
{
    Config cfg = make_cfg("/tmp", 4096, 4096);
    WRITE(cfg, 0);
    assert(READ(cfg, 0));
    unlink(file_path(cfg.dir, 0).c_str());
}

// T6: file_size an exact multiple of chunk_size (no partial last chunk).
static void check_exact_multiple()
{
    Config cfg = make_cfg("/tmp", 8192, 4096);
    WRITE(cfg, 0);
    assert(READ(cfg, 0));
    unlink(file_path(cfg.dir, 0).c_str());
}

// T7: a realistic size with a partial last chunk, to shake out any size bug at
// scale. Ceiling: does not reach >4 GB, so it does not prove 64-bit overflow
// safety, only that the engine works far past a handful of chunks.
static void check_large()
{
    Config cfg = make_cfg("/tmp", 8LL * 1024 * 1024 + 123, 1024 * 1024);
    WRITE(cfg, 0);
    assert(on_disk_size(file_path(cfg.dir, 0)) == cfg.file_size);
    assert(READ(cfg, 0));
    unlink(file_path(cfg.dir, 0).c_str());
}

static void run_battery(const char *name,
                        uint64_t (*w)(const Config &, int),
                        bool (*r)(const Config &, int))
{
    WRITE = w;
    READ = r;
    check_partial_chunk();
    check_multi_file();
    check_corruption();
    check_isolation();
    check_truncation();
    check_single_chunk();
    check_exact_multiple();
    check_large();
    std::cout << name << " engine self-test PASSED (8 checks)\n";
}

// T11: the two engines produce byte-identical files. Write with each engine,
// verify with the OTHER, and confirm the write checksums match.
static void check_cross_engine()
{
    Config cfg = make_cfg("/tmp", 10000, 4096);

    const uint64_t sum_posix = posix_write_file(cfg, 0);
    assert(uring_read_owned(cfg, 0));                // POSIX-written, uring-read
    unlink(file_path(cfg.dir, 0).c_str());

    const uint64_t sum_uring = uring_write_owned(cfg, 0);
    assert(posix_read_file(cfg, 0));                 // uring-written, POSIX-read
    unlink(file_path(cfg.dir, 0).c_str());

    assert(sum_posix == sum_uring);
    std::cout << "cross-engine equivalence PASSED\n";
}

int main()
{
    run_battery("POSIX", posix_write_file, posix_read_file);
    run_battery("io_uring", uring_write_owned, uring_read_owned);
    check_cross_engine();
    return 0;
}
