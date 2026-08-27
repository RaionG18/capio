#ifndef CAPIO_POSIX_HANDLERS_IO_URING_HPP
#define CAPIO_POSIX_HANDLERS_IO_URING_HPP

#if defined(SYS_io_uring_setup) || defined(SYS_io_uring_enter) ||                                  \
    defined(SYS_io_uring_register)

#include <linux/io_uring.h>

#include "utils/common.hpp"
#include "utils/filesystem.hpp"
#include "utils/uring.hpp"

#include "read.hpp"
#include "write.hpp"

/*
 * io_uring handlers (425-427).
 *
 * setup builds a CAPIO-owned ring (fake fd + fabricated params) instead of the
 * kernel's, so the SQEs the application submits never reach the kernel and can
 * be served by CAPIO. enter and register stay log-only for now (F3.2+ turns
 * enter into the SQ drain). The emulated mmap that backs the ring lives in
 * mmap.hpp and keys off the fake fd registered here.
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

// Next power of two >= n, for at least 1. liburing requires power-of-two entries.
static uint32_t uring_roundup_pow2(uint32_t n) {
    uint32_t p = 1;
    while (p < n) {
        p <<= 1;
    }
    return p;
}

#ifdef SYS_io_uring_setup
int io_uring_setup_handler(long arg0, long arg1, long arg2, long arg3, long arg4, long arg5,
                           long *result) {
    auto entries = static_cast<unsigned>(arg0);
    auto *params = reinterpret_cast<io_uring_params *>(arg1);
    long tid     = syscall_no_intercept(SYS_gettid);
    START_LOG(tid, "call(entries=%u, params=0x%08x)", entries, params);

    if (params == nullptr || entries == 0) {
        errno   = EINVAL;
        *result = -errno;
        return CAPIO_POSIX_SYSCALL_SUCCESS;
    }

    LOG("io_uring_setup requested: entries=%u flags=0x%x [%s]", entries, params->flags,
        uring_setup_flags_str(params->flags));

    // SQPOLL/IOPOLL are out of scope for the MVP: reject cleanly rather than
    // pretend to support them (documented limitation).
    if (params->flags & (IORING_SETUP_SQPOLL | IORING_SETUP_IOPOLL)) {
        LOG("rejecting SQPOLL/IOPOLL setup: -EINVAL");
        errno   = EINVAL;
        *result = -errno;
        return CAPIO_POSIX_SYSCALL_SUCCESS;
    }

    // Fake fd the CAPIO way: a real kernel fd (so close/poll on it behave), but
    // it names /dev/null, never a real ring. The ring lives in CapioRing.
    int fake_fd = static_cast<int>(
        syscall_no_intercept(SYS_openat, AT_FDCWD, "/dev/null", O_RDONLY, 0));
    if (fake_fd == -1) {
        ERR_EXIT("io_uring_setup: unable to open /dev/null for fake ring fd");
    }

    if (capio_rings == nullptr) {
        capio_rings = new std::unordered_map<int, CapioRing>();
    }
    CapioRing &ring = (*capio_rings)[fake_fd];
    ring.fake_fd    = fake_fd;
    ring.sq_entries = uring_roundup_pow2(entries);
    ring.cq_entries = ring.sq_entries * 2; // 2x so CQ overflow never needs handling

    params->sq_entries = ring.sq_entries;
    params->cq_entries = ring.cq_entries;
    params->features   = IORING_FEAT_SINGLE_MMAP | IORING_FEAT_NODROP;

    if (!uring_layout(ring, params)) {
        capio_rings->erase(fake_fd);
        syscall_no_intercept(SYS_close, fake_fd);
        errno   = ENOMEM;
        *result = -errno;
        return CAPIO_POSIX_SYSCALL_SUCCESS;
    }

    LOG("io_uring_setup emulated: fake_fd=%d sq_entries=%u cq_entries=%u", fake_fd, ring.sq_entries,
        ring.cq_entries);
    *result = fake_fd;
    return CAPIO_POSIX_SYSCALL_SUCCESS;
}
#endif // SYS_io_uring_setup

// io_uring uses ~0ULL in off to mean "use the current file position". For an
// explicit offset we point the CAPIO fd there before delegating, so capio_read /
// capio_write (which work from the fd's tracked offset) honour the SQE's offset.
static void uring_seek_if_explicit(int fd, uint64_t off) {
    if (off != static_cast<uint64_t>(-1)) {
        set_capio_fd_offset(fd, static_cast<off64_t>(off));
    }
}

// A non-CAPIO fd sharing the ring: run it synchronously against the real kernel
// and report its real result. This preserves correctness for mixed rings; the
// MVP does not overlap these with async, which is a documented performance
// limitation, not a correctness one. Uses the p{read,write} form so the SQE's
// explicit offset is honoured without a separate seek.
static int32_t uring_passthrough_rw(const io_uring_sqe *sqe, bool is_write) {
    long syscall_no = is_write ? SYS_pwrite64 : SYS_pread64;
    off64_t off     = (sqe->off == static_cast<uint64_t>(-1)) ? 0 : static_cast<off64_t>(sqe->off);
    long r          = syscall_no_intercept(syscall_no, sqe->fd, sqe->addr, sqe->len, off);
    return static_cast<int32_t>(r < 0 ? -errno : r);
}

// Serve one SQE and produce its completion result (bytes transferred, or -errno
// as the io_uring convention). CAPIO fds delegate to the existing capio_*
// handlers so the data path, cache and CAPIO-CL semantics are reused, not
// reimplemented; non-CAPIO fds pass through to the kernel synchronously.
static int32_t uring_dispatch_sqe(const io_uring_sqe *sqe, long tid) {
    START_LOG(tid, "call(opcode=%u, fd=%d, user_data=%llu)", sqe->opcode, sqe->fd,
              (unsigned long long) sqe->user_data);

    switch (sqe->opcode) {
    case IORING_OP_NOP:
    case IORING_OP_FSYNC:
        // Durability is provided by CAPIO's commit rule, not fsync.
        return 0;

    case IORING_OP_WRITE:
        if (!exists_capio_fd(sqe->fd)) {
            return uring_passthrough_rw(sqe, true);
        }
        uring_seek_if_explicit(sqe->fd, sqe->off);
        return static_cast<int32_t>(
            capio_write(sqe->fd, reinterpret_cast<const void *>(sqe->addr), sqe->len, tid));

    case IORING_OP_READ:
        if (!exists_capio_fd(sqe->fd)) {
            return uring_passthrough_rw(sqe, false);
        }
        uring_seek_if_explicit(sqe->fd, sqe->off);
        return static_cast<int32_t>(
            capio_read(sqe->fd, reinterpret_cast<void *>(sqe->addr), sqe->len, tid));

    default:
        LOG("opcode %u not implemented yet", sqe->opcode);
        return -EINVAL;
    }
}

// Post one completion into the CQ, preserving user_data (io_uring convention).
static void uring_post_cqe(CapioRing &ring, uint64_t user_data, int32_t res) {
    uint32_t tail = *ring.cq_tail;
    io_uring_cqe &cqe = ring.cqes[tail & *ring.cq_mask];
    cqe.user_data     = user_data;
    cqe.res           = res;
    cqe.flags         = 0;
    // Release so the app sees the CQE fields before the advanced tail.
    __atomic_store_n(ring.cq_tail, tail + 1, __ATOMIC_RELEASE);
}

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

    CapioRing *ring = get_capio_ring(ring_fd);
    if (ring == nullptr) {
        return CAPIO_POSIX_SYSCALL_SKIP; // not a CAPIO ring: kernel handles it
    }

    // Drain to_submit SQEs. liburing has already advanced the SQ tail (in CAPIO
    // memory) inside io_uring_submit, before this syscall. liburing requests
    // IORING_SETUP_NO_SQARRAY, so SQEs occupy ring slots directly and the sq
    // array indirection is unused -- sqes[i & mask] is the i-th SQE. Processing
    // synchronously here is valid: io_uring does not guarantee async, and the
    // kernel itself often completes inline.
    uint32_t head      = *ring->sq_head;
    uint32_t submitted = 0;
    while (submitted < to_submit) {
        const io_uring_sqe *sqe = &ring->sqes[head & *ring->sq_mask];
        int32_t res             = uring_dispatch_sqe(sqe, tid);
        uring_post_cqe(*ring, sqe->user_data, res);
        ++head;
        ++submitted;
    }
    // Publish the consumed head so the app's next get_sqe sees the free slots.
    __atomic_store_n(ring->sq_head, head, __ATOMIC_RELEASE);

    LOG("io_uring_enter: served %u SQEs synchronously (min_complete=%u)", submitted, min_complete);

    // Synchronous processing means every requested completion is already posted,
    // so enter returns the number of SQEs consumed, as the kernel would.
    *result = submitted;
    return CAPIO_POSIX_SYSCALL_SUCCESS;
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
