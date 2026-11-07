// Isolates the CAPIO files_location lock failure without CAPIO.
//
//   fcntl_test <file> cancel        another process holds F_WRLCK for 2 s; thread A of this
//                                   process blocks in F_SETLKW(F_RDLCK); 300 ms later thread B
//                                   (same process = same lock owner) does F_SETLK(F_RDLCK) on
//                                   the same range. Reports what thread A's fcntl returns.
//   fcntl_test <file> serial        same, but thread B waits for thread A (no overlap in time).
//   fcntl_test <file> stress P T N  P processes x T threads, each N x (F_SETLKW lock, F_SETLK
//                                   unlock) on the same file, no mutex: CAPIO's pattern.
//                                   Counts the failures by errno.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}
static int setlk(int fd, int cmd, short type) {
    struct flock l;
    memset(&l, 0, sizeof l);
    l.l_type   = type;
    l.l_whence = SEEK_SET; // whole file, as CAPIO's Flock
    return fcntl(fd, cmd, &l);
}

static const char *path;
static int fd;
static pthread_mutex_t serial_mutex = PTHREAD_MUTEX_INITIALIZER;
static int serial;

static void *thread_a(void *arg) {
    (void) arg;
    if (serial) pthread_mutex_lock(&serial_mutex);
    double s = now();
    int r = setlk(fd, F_SETLKW, F_RDLCK);
    int e = errno;
    printf("thread A: F_SETLKW(F_RDLCK) returned %d%s%s after %.2f s\n", r,
           r < 0 ? ", errno=" : "", r < 0 ? strerror(e) : "", now() - s);
    setlk(fd, F_SETLK, F_UNLCK);
    if (serial) pthread_mutex_unlock(&serial_mutex);
    return NULL;
}

static int holder(double secs) { // another process: the "remote server" writing its file
    pid_t c = fork();
    if (c == 0) {
        int h = open(path, O_RDWR);
        if (setlk(h, F_SETLKW, F_WRLCK) < 0) { perror("holder lock"); _exit(1); }
        usleep((useconds_t) (secs * 1e6));
        setlk(h, F_SETLK, F_UNLCK);
        _exit(0);
    }
    usleep(300000);
    return c;
}

struct counts { long ok, eio, eintr, enolck, edeadlk, ebadf, other; };
static long iters;
static void *stress_thread(void *arg) {
    struct counts *c = arg;
    int my = open(path, O_RDWR | O_APPEND); // one fd per thread does not matter: same owner
    for (long i = 0; i < iters; i++) {
        short type = (i & 1) ? F_WRLCK : F_RDLCK;
        if (setlk(my, F_SETLKW, type) < 0) {
            switch (errno) {
            case EIO: c->eio++; break;
            case EINTR: c->eintr++; break;
            case ENOLCK: c->enolck++; break;
            case EDEADLK: c->edeadlk++; break;
            case EBADF: c->ebadf++; break;
            default: c->other++; break;
            }
            continue;
        }
        if (type == F_WRLCK) { (void) !write(my, "x\n", 2); }
        else { char b[2]; (void) !pread(my, b, 2, 0); }
        if (setlk(my, F_SETLK, F_UNLCK) < 0) c->other++;
        c->ok++;
    }
    close(my);
    return NULL;
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: see source\n"); return 2; }
    path = argv[1];
    fd   = open(path, O_RDWR | O_CREAT | O_APPEND, 0644);
    if (fd < 0) { perror("open"); return 2; }
    const char *mode = argv[2];

    if (!strcmp(mode, "cancel") || !strcmp(mode, "serial")) {
        serial    = !strcmp(mode, "serial");
        pid_t h   = holder(2.0);
        pthread_t a;
        pthread_create(&a, NULL, thread_a, NULL);
        usleep(300000); // thread A is blocked by the holder now
        if (serial) pthread_mutex_lock(&serial_mutex);
        double s = now();
        int r = setlk(fd, F_SETLK, F_RDLCK);
        int e = errno;
        printf("thread B: F_SETLK(F_RDLCK)  returned %d%s%s after %.2f s (%s)\n", r,
               r < 0 ? ", errno=" : "", r < 0 ? strerror(e) : "", now() - s,
               serial ? "after thread A finished" : "while thread A was blocked");
        if (serial) pthread_mutex_unlock(&serial_mutex);
        pthread_join(a, NULL);
        waitpid(h, NULL, 0);
        return 0;
    }

    if (!strcmp(mode, "stress") && argc == 6) {
        int P = atoi(argv[3]), T = atoi(argv[4]);
        iters = atol(argv[5]);
        pid_t kids[64];
        for (int p = 0; p < P; p++) {
            kids[p] = fork();
            if (kids[p] == 0) {
                pthread_t th[64];
                struct counts c[64];
                memset(c, 0, sizeof c);
                double s = now();
                for (int t = 0; t < T; t++) pthread_create(&th[t], NULL, stress_thread, &c[t]);
                struct counts tot = {0};
                for (int t = 0; t < T; t++) {
                    pthread_join(th[t], NULL);
                    tot.ok += c[t].ok; tot.eio += c[t].eio; tot.eintr += c[t].eintr;
                    tot.enolck += c[t].enolck; tot.edeadlk += c[t].edeadlk;
                    tot.ebadf += c[t].ebadf; tot.other += c[t].other;
                }
                printf("process %d: %d threads x %ld: ok=%ld EIO=%ld EINTR=%ld ENOLCK=%ld "
                       "EDEADLK=%ld EBADF=%ld other=%ld in %.2f s\n",
                       p, T, iters, tot.ok, tot.eio, tot.eintr, tot.enolck, tot.edeadlk,
                       tot.ebadf, tot.other, now() - s);
                fflush(stdout);
                fflush(stdout);
                _exit(0);
            }
        }
        for (int p = 0; p < P; p++) waitpid(kids[p], NULL, 0);
        return 0;
    }
    fprintf(stderr, "bad mode\n");
    return 2;
}
