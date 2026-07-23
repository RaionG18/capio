#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
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

int main(int argc, char **argv)
{
    Config cfg;

    prog_name = argv[0];
    parse_args(argc, argv, &cfg);

    std::cout << "role=" << (cfg.role == ROLE_PRODUCER ? "producer" : "consumer")
              << " engine=" << (cfg.engine == ENGINE_POSIX ? "posix" : "io_uring")
              << " n_files=" << cfg.n_files
              << " file_size=" << cfg.file_size
              << " chunk=" << cfg.chunk_size
              << " queue_depth=" << cfg.queue_depth
              << " dir=" << cfg.dir << "\n";

    return EXIT_SUCCESS;
}
