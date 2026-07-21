#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <unistd.h>

enum role {
    ROLE_UNSET = -1,
    ROLE_PRODUCER,
    ROLE_CONSUMER
};

enum engine {
    ENGINE_UNSET = -1,
    ENGINE_POSIX,
    ENGINE_URING
};

struct config {
    enum role role       = ROLE_UNSET;
    int n_files          = 0;
    long long file_size  = 0;
    long long chunk_size = 0;
    int queue_depth      = 0;
    enum engine engine   = ENGINE_UNSET;
    const char *dir      = ".";
};

[[noreturn]] static void usage(const char *prog_name, int status)
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

static long long parse_positive(const char *value, const char *flag,
                                const char *prog_name)
{
    char *end;
    errno = 0;

    long long result = std::strtoll(value, &end, 10);

    if (errno != 0 || end == value || *end != '\0' || result <= 0) {
        std::cerr << "Invalid value for " << flag << ": " << value << "\n";
        usage(prog_name, EXIT_FAILURE);
    }

    return result;
}

static void parse_args(int argc, char **argv, struct config *config)
{
    int opt;

    while ((opt = getopt(argc, argv, "r:n:f:c:q:e:d:h")) != -1) {
        switch (opt) {
        case 'r':
            if (strcmp(optarg, "producer") == 0) {
                config->role = ROLE_PRODUCER;
            } else if (strcmp(optarg, "consumer") == 0) {
                config->role = ROLE_CONSUMER;
            } else {
                std::cerr << "Invalid role: " << optarg << "\n";
                usage(argv[0], EXIT_FAILURE);
            }
            break;

        case 'n':
            config->n_files = (int) parse_positive(optarg, "-n", argv[0]);
            break;

        case 'f':
            config->file_size = parse_positive(optarg, "-f", argv[0]);
            break;

        case 'c':
            config->chunk_size = parse_positive(optarg, "-c", argv[0]);
            break;

        case 'q':
            config->queue_depth = (int) parse_positive(optarg, "-q", argv[0]);
            break;

        case 'e':
            if (strcmp(optarg, "posix") == 0) {
                config->engine = ENGINE_POSIX;
            } else if (strcmp(optarg, "io_uring") == 0) {
                config->engine = ENGINE_URING;
            } else {
                std::cerr << "Invalid engine: " << optarg << "\n";
                usage(argv[0], EXIT_FAILURE);
            }
            break;

        case 'd':
            config->dir = optarg;
            break;

        case 'h':
            usage(argv[0], EXIT_SUCCESS);

        default:
            usage(argv[0], EXIT_FAILURE);
        }
    }

    if (config->role == ROLE_UNSET) {
        std::cerr << "Missing -r ROLE\n";
        usage(argv[0], EXIT_FAILURE);
    }

    if (config->engine == ENGINE_UNSET) {
        std::cerr << "Missing -e ENGINE\n";
        usage(argv[0], EXIT_FAILURE);
    }

    if (config->n_files == 0) {
        std::cerr << "Missing -n N_FILES\n";
        usage(argv[0], EXIT_FAILURE);
    }

    if (config->file_size == 0) {
        std::cerr << "Missing -f FILE_SIZE\n";
        usage(argv[0], EXIT_FAILURE);
    }

    if (config->chunk_size == 0) {
        std::cerr << "Missing -c CHUNK_SIZE\n";
        usage(argv[0], EXIT_FAILURE);
    }

    if (config->engine == ENGINE_URING && config->queue_depth == 0) {
        std::cerr << "Missing -q QUEUE_DEPTH (required for io_uring)\n";
        usage(argv[0], EXIT_FAILURE);
    }

    if (config->chunk_size > config->file_size) {
        std::cerr << "CHUNK_SIZE (" << config->chunk_size
                  << ") cannot be greater than FILE_SIZE ("
                  << config->file_size << ")\n";
        usage(argv[0], EXIT_FAILURE);
    }
}

int main(int argc, char **argv)
{
    struct config config;
    parse_args(argc, argv, &config);

    std::cout << "role=" << (config.role == ROLE_PRODUCER ? "producer" : "consumer")
              << " engine=" << (config.engine == ENGINE_POSIX ? "posix" : "io_uring")
              << " n_files=" << config.n_files
              << " file_size=" << config.file_size
              << " chunk=" << config.chunk_size
              << " queue_depth=" << config.queue_depth
              << " dir=" << config.dir << "\n";

    return EXIT_SUCCESS;
}
