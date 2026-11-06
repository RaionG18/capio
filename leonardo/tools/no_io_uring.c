// no_io_uring: run a command on a kernel that "has no io_uring".
//
// Installs a seccomp filter that makes io_uring_setup/enter/register fail with
// ENOSYS, as on Leonardo (RHEL 8, kernel 4.18), then execs the command. No root
// needed. Used to test CAPIO's fallback (a CAPIO ring with no kernel ring) on a
// machine whose kernel does support io_uring.
//
//   cc -O2 -o no_io_uring no_io_uring.c
//   ./no_io_uring <command> [args...]
#include <errno.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#define DENY(nr)                                                                                   \
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, (nr), 0, 1),                                               \
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | (ENOSYS & SECCOMP_RET_DATA))

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <command> [args...]\n", argv[0]);
        return 2;
    }
    struct sock_filter filter[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, arch)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_X86_64, 1, 0),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
        DENY(425), // io_uring_setup
        DENY(426), // io_uring_enter
        DENY(427), // io_uring_register
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    struct sock_fprog prog = {sizeof(filter) / sizeof(filter[0]), filter};
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0 ||
        prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog) != 0) {
        perror("seccomp");
        return 1;
    }
    execvp(argv[1], argv + 1);
    perror(argv[1]);
    return 127;
}
