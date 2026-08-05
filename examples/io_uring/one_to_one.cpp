#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <functional>
#include <iostream>
#include <liburing.h>
#include <optional>
#include <string>
#include <vector>
#include <unistd.h>

enum Role {
    ROLE_UNSET = -1,
    ROLE_PRODUCER,
    ROLE_CONSUMER
};

enum Engine {
    ENGINE_UNSET = -1,
    ENGINE_POSIX,
    ENGINE_URING
};

struct Config {
    Role role            = ROLE_UNSET;
    int n_files          = 0;
    long long file_size  = 0;
    long long chunk_size = 0;
    int queue_depth      = 0;
    Engine engine        = ENGINE_UNSET;
    const char *dir      = ".";
};

static const uint64_t OFFSET_BASIS = 0xcbf29ce484222325ULL;
static const uint64_t FNV_PRIME    = 0x100000001b3ULL;

static const char *prog_name = "one_to_one";

[[noreturn]] static void usage(int status)
{
    std::cerr
        << "Usage: " << prog_name << " [options]\n"
        << "Options:\n"
        << "  -r ROLE         Role: producer or consumer\n"
        << "  -n N_FILES      Number of files to read/write\n"
        << "  -f FILE_SIZE    Bytes per file\n"
        << "  -c CHUNK_SIZE   Bytes per read/write\n"
        << "  -q QUEUE_DEPTH  SQEs in flight (io_uring only)\n"
        << "  -e ENGINE       Engine: posix or io_uring\n"
        << "  -d DIR          Working directory (default: .)\n"
        << "  -h              Show this help\n";
    exit(status);
}

[[noreturn]] static void die(const std::string &message)
{
    std::cerr << message << "\n";
    usage(EXIT_FAILURE);
}

static uint64_t fnv1a(uint64_t hash, const void *data, size_t len)
{
    const unsigned char *bytes = static_cast<const unsigned char *>(data);

    for (size_t i = 0; i < len; ++i) {
        hash ^= bytes[i];
        hash *= FNV_PRIME;
    }

    return hash;
}

// Content depends on the absolute position, so a byte reads the same however the
// file is split into chunks, and on file_idx, so no two files share content.
static void fill_pattern(unsigned char *buf, size_t len, int file_idx, long long offset)
{
    const long long base = file_idx * 131 + 7;

    for (size_t i = 0; i < len; ++i)
        buf[i] = static_cast<unsigned char>((offset + static_cast<long long>(i)) * 31 + base);
}

static long long parse_positive(const char *value, const char *flag)
{
    const char *end = value + strlen(value);
    long long result;

    const auto [stop, ec] = std::from_chars(value, end, result);

    if (ec != std::errc{} || stop != end || result <= 0)
        die(std::string("Invalid value for ") + flag + ": " + value);

    return result;
}

static void parse_args(int argc, char **argv, Config *cfg)
{
    int opt;

    while ((opt = getopt(argc, argv, "r:n:f:c:q:e:d:h")) != -1) {
        switch (opt) {
        case 'r':
            if (strcmp(optarg, "producer") == 0) {
                cfg->role = ROLE_PRODUCER;
            } else if (strcmp(optarg, "consumer") == 0) {
                cfg->role = ROLE_CONSUMER;
            } else {
                die(std::string("Invalid role: ") + optarg);
            }
            break;

        case 'n':
            cfg->n_files = static_cast<int>(parse_positive(optarg, "-n"));
            break;

        case 'f':
            cfg->file_size = parse_positive(optarg, "-f");
            break;

        case 'c':
            cfg->chunk_size = parse_positive(optarg, "-c");
            break;

        case 'q':
            cfg->queue_depth = static_cast<int>(parse_positive(optarg, "-q"));
            break;

        case 'e':
            if (strcmp(optarg, "posix") == 0) {
                cfg->engine = ENGINE_POSIX;
            } else if (strcmp(optarg, "io_uring") == 0) {
                cfg->engine = ENGINE_URING;
            } else {
                die(std::string("Invalid engine: ") + optarg);
            }
            break;

        case 'd':
            cfg->dir = optarg;
            break;

        case 'h':
            usage(EXIT_SUCCESS);

        default:
            usage(EXIT_FAILURE);
        }
    }

    if (cfg->role == ROLE_UNSET)
        die("Missing -r ROLE");

    if (cfg->engine == ENGINE_UNSET)
        die("Missing -e ENGINE");

    if (cfg->n_files == 0)
        die("Missing -n N_FILES");

    if (cfg->file_size == 0)
        die("Missing -f FILE_SIZE");

    if (cfg->chunk_size == 0)
        die("Missing -c CHUNK_SIZE");

    if (cfg->engine == ENGINE_URING && cfg->queue_depth == 0)
        die("Missing -q QUEUE_DEPTH (required for io_uring)");

    if (cfg->chunk_size > cfg->file_size)
        die("CHUNK_SIZE (" + std::to_string(cfg->chunk_size) +
            ") cannot be greater than FILE_SIZE (" + std::to_string(cfg->file_size) + ")");
}

static std::string file_path(const char *dir, int idx)
{
    char name[32];
    snprintf(name, sizeof(name), "/file_%04d.dat", idx);
    return std::string(dir) + name;
}

static bool write_all(int fd, const unsigned char *buf, size_t len)
{
    size_t done = 0;

    while (done < len) {
        ssize_t n = write(fd, buf + done, len - done);

        if (n < 0) {
            if (errno == EINTR)   // interrupted by a signal: retry
                continue;
            return false;
        }

        done += static_cast<size_t>(n);
    }

    return true;
}

static bool read_all(int fd, unsigned char *buf, size_t len)
{
    size_t done = 0;

    while (done < len) {
        ssize_t n = read(fd, buf + done, len - done);

        if (n < 0) {
            if (errno == EINTR)   // interrupted by a signal: retry
                continue;
            return false;
        }

        if (n == 0)   // unexpected EOF
            return false;

        done += static_cast<size_t>(n);
    }

    return true;
}

// Writes file `idx` with the deterministic pattern; returns its FNV-1a checksum.
static uint64_t posix_write_file(const Config &cfg, int idx)
{
    const std::string path = file_path(cfg.dir, idx);

    int fd = open(path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0)
        die("cannot create " + path + ": " + strerror(errno));

    std::vector<unsigned char> buf(cfg.chunk_size);
    uint64_t checksum = OFFSET_BASIS;

    for (long long offset = 0; offset < cfg.file_size; offset += cfg.chunk_size) {
        const size_t n = std::min(cfg.chunk_size, cfg.file_size - offset);

        fill_pattern(buf.data(), n, idx, offset);
        checksum = fnv1a(checksum, buf.data(), n);

        if (!write_all(fd, buf.data(), n)) {
            close(fd);
            die("write failed on " + path + ": " + strerror(errno));
        }
    }

    close(fd);
    return checksum;
}

// Verifies file `idx` against the pattern, regenerated here rather than shared
// with the producer, so a match proves the bytes survived the round trip.
// Cannot open -> die() (broken pipeline); wrong or short content -> false.
static bool posix_read_file(const Config &cfg, int idx)
{
    const std::string path = file_path(cfg.dir, idx);

    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0)
        die("cannot open " + path + ": " + strerror(errno));

    std::vector<unsigned char> got(cfg.chunk_size);
    std::vector<unsigned char> want(cfg.chunk_size);
    bool ok = true;

    for (long long offset = 0; offset < cfg.file_size; offset += cfg.chunk_size) {
        const size_t n = std::min(cfg.chunk_size, cfg.file_size - offset);

        if (!read_all(fd, got.data(), n)) {   // short file: verification fails
            ok = false;
            break;
        }

        fill_pattern(want.data(), n, idx, offset);
        if (memcmp(got.data(), want.data(), n) != 0) {
            ok = false;
            break;
        }
    }

    close(fd);
    return ok;
}

// --- io_uring engine --------------------------------------------------------
// Same work as the POSIX engine, but batched: up to queue_depth chunks per
// submit instead of one write()/read() per chunk.

// Completions may arrive out of order, so user_data carries the slot index.
static void uring_reap(io_uring &ring, int count, std::vector<int> &res)
{
    for (int r = 0; r < count; ++r) {
        io_uring_cqe *cqe;
        int ret = io_uring_wait_cqe(&ring, &cqe);
        if (ret < 0)
            die(std::string("io_uring_wait_cqe: ") + strerror(-ret));
        res[io_uring_cqe_get_data64(cqe)] = cqe->res;
        io_uring_cqe_seen(&ring, cqe);
    }
}

// Ring and buffers live here so they are set up once, not per file: otherwise
// that cost dominates a many-small-files run. Memory is queue_depth * chunk_size.
struct UringCtx {
    io_uring ring;
    std::vector<std::vector<unsigned char>> bufs;
    std::vector<long long> off;
    std::vector<size_t> len;
    std::vector<int> res;
    std::vector<unsigned char> want;

    explicit UringCtx(const Config &cfg)
        : bufs(cfg.queue_depth, std::vector<unsigned char>(cfg.chunk_size)),
          off(cfg.queue_depth), len(cfg.queue_depth), res(cfg.queue_depth),
          want(cfg.chunk_size)
    {
        if (int r = io_uring_queue_init(cfg.queue_depth, &ring, 0); r < 0)
            die(std::string("io_uring_queue_init: ") + strerror(-r));
    }
    ~UringCtx() { io_uring_queue_exit(&ring); }
};

// The checksum is folded while filling the buffers, in offset order, so it does
// not depend on the order completions arrive in.
static uint64_t uring_write_file(UringCtx &ctx, const Config &cfg, int idx)
{
    const std::string path = file_path(cfg.dir, idx);

    int fd = open(path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0)
        die("cannot create " + path + ": " + strerror(errno));

    const int depth = cfg.queue_depth;
    uint64_t checksum = OFFSET_BASIS;

    for (long long base = 0; base < cfg.file_size; base += static_cast<long long>(depth) * cfg.chunk_size) {
        int count = 0;
        for (int k = 0; k < depth; ++k) {
            const long long offset = base + static_cast<long long>(k) * cfg.chunk_size;
            if (offset >= cfg.file_size)
                break;
            ctx.off[k] = offset;
            ctx.len[k] = std::min(cfg.chunk_size, cfg.file_size - offset);

            fill_pattern(ctx.bufs[k].data(), ctx.len[k], idx, offset);
            checksum = fnv1a(checksum, ctx.bufs[k].data(), ctx.len[k]);

            io_uring_sqe *sqe = io_uring_get_sqe(&ctx.ring);
            if (!sqe)
                die("io_uring: submission queue full");
            io_uring_prep_write(sqe, fd, ctx.bufs[k].data(), ctx.len[k], offset);
            io_uring_sqe_set_data64(sqe, k);
            ++count;
        }

        if (int r = io_uring_submit_and_wait(&ctx.ring, count); r < 0)
            die(std::string("io_uring_submit_and_wait: ") + strerror(-r));
        uring_reap(ctx.ring, count, ctx.res);

        for (int k = 0; k < count; ++k) {
            if (ctx.res[k] < 0)
                die("io_uring write failed on " + path + ": " + strerror(-ctx.res[k]));

            // Not retried: a partial write means an assumption broke, so say so.
            if (static_cast<size_t>(ctx.res[k]) != ctx.len[k])
                die("io_uring short write on " + path + ": " + std::to_string(ctx.res[k]) +
                    " of " + std::to_string(ctx.len[k]) + " bytes");
        }
    }

    close(fd);
    return checksum;
}

static bool uring_read_file(UringCtx &ctx, const Config &cfg, int idx)
{
    const std::string path = file_path(cfg.dir, idx);

    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0)
        die("cannot open " + path + ": " + strerror(errno));

    const int depth = cfg.queue_depth;
    bool ok = true;

    for (long long base = 0; ok && base < cfg.file_size; base += static_cast<long long>(depth) * cfg.chunk_size) {
        int count = 0;
        for (int k = 0; k < depth; ++k) {
            const long long offset = base + static_cast<long long>(k) * cfg.chunk_size;
            if (offset >= cfg.file_size)
                break;
            ctx.off[k] = offset;
            ctx.len[k] = std::min(cfg.chunk_size, cfg.file_size - offset);

            io_uring_sqe *sqe = io_uring_get_sqe(&ctx.ring);
            if (!sqe)
                die("io_uring: submission queue full");
            io_uring_prep_read(sqe, fd, ctx.bufs[k].data(), ctx.len[k], offset);
            io_uring_sqe_set_data64(sqe, k);
            ++count;
        }

        if (int r = io_uring_submit_and_wait(&ctx.ring, count); r < 0)
            die(std::string("io_uring_submit_and_wait: ") + strerror(-r));
        uring_reap(ctx.ring, count, ctx.res);

        for (int k = 0; k < count; ++k) {
            if (ctx.res[k] < 0)
                die("io_uring read failed on " + path + ": " + strerror(-ctx.res[k]));

            if (static_cast<size_t>(ctx.res[k]) != ctx.len[k]) {   // truncated file
                ok = false;   // a verification failure, not a broken pipeline
                break;
            }

            fill_pattern(ctx.want.data(), ctx.len[k], idx, ctx.off[k]);
            if (memcmp(ctx.bufs[k].data(), ctx.want.data(), ctx.len[k]) != 0) {
                ok = false;
                break;
            }
        }
    }

    close(fd);
    return ok;
}

static double now_seconds()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

// One parseable line for the benchmark harness to harvest (key=value pairs).
static void print_metrics(const char *engine, const char *role, int files,
                          long long bytes, double secs)
{
    const double mbps = secs > 0 ? bytes / 1e6 / secs : 0.0;
    std::cout << "engine=" << engine << " role=" << role
              << " files=" << files << " bytes=" << bytes
              << " secs=" << secs << " MBps=" << mbps << "\n";
}

int main(int argc, char **argv)
{
    Config cfg;

    prog_name = argv[0];
    parse_args(argc, argv, &cfg);

    // Select the engine once; both engines share the producer/consumer loops.
    // The io_uring engine keeps a single ring + buffers alive across all files.
    std::optional<UringCtx> uring_ctx;
    std::function<uint64_t(int)> write_file;
    std::function<bool(int)> read_file;

    if (cfg.engine == ENGINE_URING) {
        uring_ctx.emplace(cfg);
        write_file = [&](int idx) { return uring_write_file(*uring_ctx, cfg, idx); };
        read_file  = [&](int idx) { return uring_read_file(*uring_ctx, cfg, idx); };
    } else {
        write_file = [&](int idx) { return posix_write_file(cfg, idx); };
        read_file  = [&](int idx) { return posix_read_file(cfg, idx); };
    }

    const char *engine_name = cfg.engine == ENGINE_URING ? "io_uring" : "posix";
    const long long total_bytes = static_cast<long long>(cfg.n_files) * cfg.file_size;

    if (cfg.role == ROLE_PRODUCER) {
        // Time only the I/O loop; keep checksum printing out of the measured region.
        std::vector<uint64_t> sums(cfg.n_files);
        const double t0 = now_seconds();
        for (int idx = 0; idx < cfg.n_files; ++idx)
            sums[idx] = write_file(idx);
        const double secs = now_seconds() - t0;

        print_metrics(engine_name, "producer", cfg.n_files, total_bytes, secs);
        for (int idx = 0; idx < cfg.n_files; ++idx)
            std::cout << file_path(cfg.dir, idx) << " " << std::hex << sums[idx] << std::dec << "\n";
        return EXIT_SUCCESS;
    }

    int verified = 0;
    const double t0 = now_seconds();
    for (int idx = 0; idx < cfg.n_files; ++idx)
        if (read_file(idx))
            ++verified;
    const double secs = now_seconds() - t0;

    print_metrics(engine_name, "consumer", cfg.n_files, total_bytes, secs);
    std::cout << "verified " << verified << "/" << cfg.n_files << " files OK\n";
    return verified == cfg.n_files ? EXIT_SUCCESS : EXIT_FAILURE;
}
