#ifndef CAPIO_POSIX_HANDLERS_IO_URING_HPP
#define CAPIO_POSIX_HANDLERS_IO_URING_HPP

#if defined(SYS_io_uring_setup) || defined(SYS_io_uring_enter) ||                                  \
    defined(SYS_io_uring_register)

#include <linux/io_uring.h>

#include "utils/common.hpp"

/*
 * Tracing-only handlers for the io_uring syscalls (425-427).
 *
 * Every handler logs and then returns CAPIO_POSIX_SYSCALL_SKIP, so the call
 * proceeds to the kernel untouched: CAPIO's behaviour is unchanged while these
 * are in place. Their job is to reveal what liburing actually asks for --
 * especially the mmap offsets and the params it gets back -- before any of it
 * is emulated.
 *
 * The setup handler logs io_uring_params on the way out, which is why it is the
 * one place that needs the kernel's answer. syscall_no_intercept re-issues the
 * call without re-entering the hook, and the result is handed back through
 * *result with a return of 0, matching what the other handlers do when they
 * supply their own return value.
 */

// Logged so the emulated setup knows which flags real workloads request.
// Only the ones that change what liburing does with the ring are named here.
static const char *uring_setup_flags_str(unsigned flags) {
    static thread_local char buf[256];
    buf[0] = '\0';

    struct {
        unsigned bit;
        const char *name;
    } static constexpr kFlags[] = {
        {IORING_SETUP_IOPOLL, "IOPOLL"},
        {IORING_SETUP_SQPOLL, "SQPOLL"},
        {IORING_SETUP_SQ_AFF, "SQ_AFF"},
        {IORING_SETUP_CQSIZE, "CQSIZE"},
        {IORING_SETUP_CLAMP, "CLAMP"},
        {IORING_SETUP_ATTACH_WQ, "ATTACH_WQ"},
    };

    for (const auto &f : kFlags) {
        if (flags & f.bit) {
            if (buf[0] != '\0') {
                strncat(buf, "|", sizeof buf - strlen(buf) - 1);
            }
            strncat(buf, f.name, sizeof buf - strlen(buf) - 1);
        }
    }
    if (buf[0] == '\0') {
        strncpy(buf, "none", sizeof buf - 1);
    }
    return buf;
}

#ifdef SYS_io_uring_setup
int io_uring_setup_handler(long arg0, long arg1, long arg2, long arg3, long arg4, long arg5,
                           long *result) {
    auto entries = static_cast<unsigned>(arg0);
    auto *params = reinterpret_cast<io_uring_params *>(arg1);
    long tid     = syscall_no_intercept(SYS_gettid);
    START_LOG(tid, "call(entries=%u, params=0x%08x)", entries, params);

    // Requested flags, before the kernel fills in what it granted.
    if (params != nullptr) {
        LOG("io_uring_setup requested: entries=%u flags=0x%x [%s] sq_thread_cpu=%u "
            "sq_thread_idle=%u",
            entries, params->flags, uring_setup_flags_str(params->flags), params->sq_thread_cpu,
            params->sq_thread_idle);
    }

    // Let the kernel actually create the ring, then report what it returned.
    // The layout below is exactly what an emulated setup will have to fabricate.
    long res = syscall_no_intercept(SYS_io_uring_setup, arg0, arg1);

    if (res >= 0 && params != nullptr) {
        LOG("io_uring_setup returned: ring_fd=%ld sq_entries=%u cq_entries=%u features=0x%x",
            res, params->sq_entries, params->cq_entries, params->features);
        LOG("  sq_off:  head=%u tail=%u ring_mask=%u ring_entries=%u flags=%u dropped=%u "
            "array=%u",
            params->sq_off.head, params->sq_off.tail, params->sq_off.ring_mask,
            params->sq_off.ring_entries, params->sq_off.flags, params->sq_off.dropped,
            params->sq_off.array);
        LOG("  cq_off:  head=%u tail=%u ring_mask=%u ring_entries=%u overflow=%u cqes=%u "
            "flags=%u",
            params->cq_off.head, params->cq_off.tail, params->cq_off.ring_mask,
            params->cq_off.ring_entries, params->cq_off.overflow, params->cq_off.cqes,
            params->cq_off.flags);
        // SINGLE_MMAP decides whether liburing issues 2 mmaps or 3.
        LOG("  features: SINGLE_MMAP=%s NODROP=%s SUBMIT_STABLE=%s",
            (params->features & IORING_FEAT_SINGLE_MMAP) ? "yes" : "no",
            (params->features & IORING_FEAT_NODROP) ? "yes" : "no",
            (params->features & IORING_FEAT_SUBMIT_STABLE) ? "yes" : "no");
    } else if (res < 0) {
        LOG("io_uring_setup failed: %ld", res);
    }

    *result = res;
    return CAPIO_POSIX_SYSCALL_SUCCESS;
}
#endif // SYS_io_uring_setup

#ifdef SYS_io_uring_enter
int io_uring_enter_handler(long arg0, long arg1, long arg2, long arg3, long arg4, long arg5,
                           long *result) {
    auto ring_fd      = static_cast<int>(arg0);
    auto to_submit    = static_cast<unsigned>(arg1);
    auto min_complete = static_cast<unsigned>(arg2);
    auto flags        = static_cast<unsigned>(arg3);
    long tid          = syscall_no_intercept(SYS_gettid);
    START_LOG(tid, "call(ring_fd=%d, to_submit=%u, min_complete=%u, flags=0x%x)", ring_fd,
              to_submit, min_complete, flags);

    // to_submit/min_complete are the contract the emulated enter must honour.
    LOG("io_uring_enter: ring_fd=%d to_submit=%u min_complete=%u flags=0x%x getevents=%s",
        ring_fd, to_submit, min_complete, flags,
        (flags & IORING_ENTER_GETEVENTS) ? "yes" : "no");

    return CAPIO_POSIX_SYSCALL_SKIP;
}
#endif // SYS_io_uring_enter

#ifdef SYS_io_uring_register
int io_uring_register_handler(long arg0, long arg1, long arg2, long arg3, long arg4, long arg5,
                              long *result) {
    auto ring_fd  = static_cast<int>(arg0);
    auto opcode   = static_cast<unsigned>(arg1);
    auto nr_args  = static_cast<unsigned>(arg3);
    long tid      = syscall_no_intercept(SYS_gettid);
    START_LOG(tid, "call(ring_fd=%d, opcode=%u, nr_args=%u)", ring_fd, opcode, nr_args);

    // Registration is out of scope for the MVP; logging it shows whether real
    // workloads depend on it (fixed buffers/files) before it is refused.
    LOG("io_uring_register: ring_fd=%d opcode=%u nr_args=%u", ring_fd, opcode, nr_args);

    return CAPIO_POSIX_SYSCALL_SKIP;
}
#endif // SYS_io_uring_register

#endif // SYS_io_uring_setup || SYS_io_uring_enter || SYS_io_uring_register
#endif // CAPIO_POSIX_HANDLERS_IO_URING_HPP
