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

// Writes file `idx` filled with the deterministic pattern and returns its
// FNV-1a checksum. Aborts on any I/O error (it knows the path, so it can report
// which file failed and why).
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

// Reads file `idx` back and verifies it independently of the producer: it
// regenerates the expected content from the same pattern and compares
// checksums. A file that cannot be opened is a broken pipeline -> die(); wrong
// content or a short (truncated) file is a verification failure -> false.
static bool posix_read_file(const Config &cfg, int idx)
{
    const std::string path = file_path(cfg.dir, idx);

    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0)
        die("cannot open " + path + ": " + strerror(errno));

    std::vector<unsigned char> got(cfg.chunk_size);
    std::vector<unsigned char> want(cfg.chunk_size);
    uint64_t got_sum = OFFSET_BASIS;
    uint64_t want_sum = OFFSET_BASIS;
    bool complete = true;

    for (long long offset = 0; offset < cfg.file_size; offset += cfg.chunk_size) {
        const size_t n = std::min(cfg.chunk_size, cfg.file_size - offset);

        if (!read_all(fd, got.data(), n)) {   // short file: verification fails
            complete = false;
            break;
        }

        fill_pattern(want.data(), n, idx, offset);
        got_sum  = fnv1a(got_sum,  got.data(),  n);
        want_sum = fnv1a(want_sum, want.data(), n);
    }

    close(fd);
    return complete && got_sum == want_sum;
}

// --- io_uring engine --------------------------------------------------------
// Same work as the POSIX engine, but each chunk is submitted through an
// io_uring ring instead of a direct write()/read(). This first version submits
// one operation at a time and waits for it (queue depth 1 in effect);
// batching up to cfg.queue_depth is a later, measurable optimization.

// Submits a single prep'd SQE and returns the completion's res (bytes done, or
// -errno). Centralizes the get_sqe -> submit_and_wait -> cqe_seen dance so the
// write/read paths don't repeat it.
static int uring_submit_one(io_uring &ring, int fd, void *buf, size_t len,
                            long long offset, bool is_write)
{
    io_uring_sqe *sqe = io_uring_get_sqe(&ring);
    if (!sqe)
        die("io_uring: submission queue full");   // cannot happen at depth 1

    if (is_write)
        io_uring_prep_write(sqe, fd, buf, len, offset);
    else
        io_uring_prep_read(sqe, fd, buf, len, offset);

    int ret = io_uring_submit_and_wait(&ring, 1);
    if (ret < 0)
        die(std::string("io_uring_submit_and_wait: ") + strerror(-ret));

    io_uring_cqe *cqe;
    ret = io_uring_wait_cqe(&ring, &cqe);
    if (ret < 0)
        die(std::string("io_uring_wait_cqe: ") + strerror(-ret));

    int res = cqe->res;
    io_uring_cqe_seen(&ring, cqe);
    return res;
}

// Like write_all()/read_all(), but each pass goes through the ring. res < 0 is
// -errno; res == 0 on a read means EOF; a short res means retry the remainder.
static bool uring_rw_all(io_uring &ring, int fd, unsigned char *buf, size_t len,
                         long long offset, bool is_write)
{
    size_t done = 0;

    while (done < len) {
        int res = uring_submit_one(ring, fd, buf + done, len - done,
                                   offset + static_cast<long long>(done), is_write);

        if (res < 0) {
            if (res == -EINTR)
                continue;
            errno = -res;
            return false;
        }

        if (res == 0)   // unexpected EOF (read side)
            return false;

        done += static_cast<size_t>(res);
    }

    return true;
}

// Finishes a slot that did not fully complete in a batch, using the proven
// depth-1 helper (rare for regular files: only short completions or -EINTR).
static bool uring_finish_slot(io_uring &ring, int fd, unsigned char *buf,
                              size_t len, long long offset, int res, bool is_write)
{
    const size_t done = res > 0 ? static_cast<size_t>(res) : 0;
    if (done >= len)
        return true;
    return uring_rw_all(ring, fd, buf + done, len - done,
                        offset + static_cast<long long>(done), is_write);
}

// Reaps `count` completions, matching each CQE back to its slot via user_data
// (completions may arrive out of order), storing bytes-done into res[].
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

// Owns the ring and the per-batch buffers so they are allocated once and reused
// across every file, instead of per file — otherwise ring setup and queue_depth
// buffer allocations would dominate a many-small-files benchmark. Buffer memory
// is queue_depth * chunk_size.
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

// Batches up to queue_depth chunks per submit. Buffers, checksum folding, and
// remainder handling all proceed in offset order (batches ascend, and slots
// within a batch ascend), so the checksum is well-defined regardless of the
// order completions arrive in.
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

        for (int k = 0; k < count; ++k)
            if (!uring_finish_slot(ctx.ring, fd, ctx.bufs[k].data(), ctx.len[k], ctx.off[k], ctx.res[k], /*write=*/true))
                die("io_uring write failed on " + path + ": " + strerror(errno));
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
    uint64_t got_sum = OFFSET_BASIS, want_sum = OFFSET_BASIS;
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

        // Fold in offset order; a slot that can't be completed means a short file.
        for (int k = 0; k < count; ++k) {
            if (!uring_finish_slot(ctx.ring, fd, ctx.bufs[k].data(), ctx.len[k], ctx.off[k], ctx.res[k], /*write=*/false)) {
                ok = false;
                break;
            }
            fill_pattern(ctx.want.data(), ctx.len[k], idx, ctx.off[k]);
            got_sum  = fnv1a(got_sum,  ctx.bufs[k].data(), ctx.len[k]);
            want_sum = fnv1a(want_sum, ctx.want.data(),    ctx.len[k]);
        }
    }

    close(fd);
    return ok && got_sum == want_sum;
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
