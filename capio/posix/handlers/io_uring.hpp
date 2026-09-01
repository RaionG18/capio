#ifndef CAPIO_POSIX_HANDLERS_IO_URING_HPP
#define CAPIO_POSIX_HANDLERS_IO_URING_HPP

#if defined(SYS_io_uring_setup) || defined(SYS_io_uring_enter) || defined(SYS_io_uring_register)

#include <algorithm>
#include <linux/io_uring.h>
#include <time.h>

#include "utils/common.hpp"
#include "utils/filesystem.hpp"
#include "utils/uring.hpp"

#include "read.hpp"
#include "write.hpp"

/*
 * io_uring handlers (425-427). setup builds a CAPIO-owned ring (fake fd +
 * fabricated params) so SQEs never reach the kernel; enter drains and serves
 * them; the ring's emulated mmap lives in mmap.hpp, keyed off the fake fd.
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
        {IORING_SETUP_IOPOLL, "IOPOLL"}, {IORING_SETUP_SQPOLL, "SQPOLL"},
        {IORING_SETUP_SQ_AFF, "SQ_AFF"}, {IORING_SETUP_CQSIZE, "CQSIZE"},
        {IORING_SETUP_CLAMP, "CLAMP"},   {IORING_SETUP_ATTACH_WQ, "ATTACH_WQ"},
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
// Return false when n is unsupported to avoid shift overflow.
static bool uring_roundup_pow2(uint32_t n, uint32_t &out) {
    static constexpr uint32_t kMaxSqEntries = 1u << 30;
    if (n == 0 || n > kMaxSqEntries) {
        return false;
    }

    uint32_t p = 1;
    while (p < n) {
        p <<= 1;
    }
    out = p;
    return true;
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

    // Allowlist, not denylist: NO_SQARRAY is a no-op for us (the drain reads
    // sqes[head & mask] directly), so accept it; reject every other flag with
    // -EINVAL (they change geometry, the mmap mechanism, or require real async).
    constexpr unsigned kSupportedFlags = IORING_SETUP_NO_SQARRAY;
    if (params->flags & ~kSupportedFlags) {
        LOG("rejecting unsupported setup flags: 0x%x", params->flags & ~kSupportedFlags);
        errno   = EINVAL;
        *result = -errno;
        return CAPIO_POSIX_SYSCALL_SUCCESS;
    }

    // Fake fd the CAPIO way: a real kernel fd (so close/poll on it behave), but
    // it names /dev/null, never a real ring. The ring lives in CapioRing.
    int fake_fd =
        static_cast<int>(syscall_no_intercept(SYS_openat, AT_FDCWD, "/dev/null", O_RDONLY, 0));
    if (fake_fd == -1) {
        ERR_EXIT("io_uring_setup: unable to open /dev/null for fake ring fd");
    }

    if (capio_rings == nullptr) {
        capio_rings = new std::unordered_map<int, CapioRing>();
    }
    CapioRing &ring = (*capio_rings)[fake_fd];
    ring.fake_fd    = fake_fd;
    if (!uring_roundup_pow2(entries, ring.sq_entries)) {
        capio_rings->erase(fake_fd);
        syscall_no_intercept(SYS_close, fake_fd);
        errno   = EINVAL;
        *result = -errno;
        return CAPIO_POSIX_SYSCALL_SUCCESS;
    }
    ring.cq_entries = ring.sq_entries * 2; // 2x so CQ overflow never needs handling

    params->sq_entries = ring.sq_entries;
    params->cq_entries = ring.cq_entries;
    params->features   = IORING_FEAT_SINGLE_MMAP;

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

// A non-CAPIO fd sharing the ring: run it synchronously against the real kernel
// and report its real result. This preserves correctness for mixed rings; the
// MVP does not overlap these with async, which is a documented performance
// limitation, not a correctness one. Uses the p{read,write} form so the SQE's
// explicit offset is honoured without a separate seek.
static int32_t uring_passthrough_rw(const io_uring_sqe *sqe, bool is_write) {
    long r = -1;
    if (sqe->off == static_cast<uint64_t>(-1)) {
        long syscall_no = is_write ? SYS_write : SYS_read;
        r               = syscall_no_intercept(syscall_no, sqe->fd, sqe->addr, sqe->len);
    } else {
        long syscall_no = is_write ? SYS_pwrite64 : SYS_pread64;
        off64_t off     = static_cast<off64_t>(sqe->off);
        r               = syscall_no_intercept(syscall_no, sqe->fd, sqe->addr, sqe->len, off);
    }
    return static_cast<int32_t>(r < 0 ? -errno : r);
}

static int32_t uring_capio_rw(const io_uring_sqe *sqe, bool is_write, long tid) {
    if (sqe->off == static_cast<uint64_t>(-1)) {
        return static_cast<int32_t>(
            is_write
                ? capio_write(sqe->fd, reinterpret_cast<const void *>(sqe->addr), sqe->len, tid)
                : capio_read(sqe->fd, reinterpret_cast<void *>(sqe->addr), sqe->len, tid));
    }

    off64_t saved = get_capio_fd_offset(sqe->fd);
    set_capio_fd_offset(sqe->fd, static_cast<off64_t>(sqe->off));
    auto res = static_cast<int32_t>(
        is_write ? capio_write(sqe->fd, reinterpret_cast<const void *>(sqe->addr), sqe->len, tid)
                 : capio_read(sqe->fd, reinterpret_cast<void *>(sqe->addr), sqe->len, tid));
    set_capio_fd_offset(sqe->fd, saved);
    return res;
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
        return 0;

    case IORING_OP_FSYNC:
        if (exists_capio_fd(sqe->fd)) {
            // Durability for CAPIO-owned fds is handled by CAPIO commit rules.
            return 0;
        }
        if (syscall_no_intercept((sqe->fsync_flags & IORING_FSYNC_DATASYNC) ? SYS_fdatasync
                                                                            : SYS_fsync,
                                 sqe->fd) < 0) {
            return -errno;
        }
        return 0;

    case IORING_OP_WRITE:
        if (!exists_capio_fd(sqe->fd)) {
            return uring_passthrough_rw(sqe, true);
        }
        return uring_capio_rw(sqe, true, tid);

    case IORING_OP_READ:
        if (!exists_capio_fd(sqe->fd)) {
            return uring_passthrough_rw(sqe, false);
        }
        return uring_capio_rw(sqe, false, tid);

    default:
        LOG("opcode %u not implemented yet", sqe->opcode);
        return -EINVAL;
    }
}

static uint32_t uring_cq_ready(const CapioRing &ring) {
    uint32_t head = __atomic_load_n(ring.cq_head, __ATOMIC_ACQUIRE);
    uint32_t tail = __atomic_load_n(ring.cq_tail, __ATOMIC_ACQUIRE);
    return tail - head;
}

static bool uring_cq_has_space(const CapioRing &ring) {
    return uring_cq_ready(ring) < *ring.cq_ring_entries;
}

// Post one completion into the CQ, preserving user_data (io_uring convention).
static void uring_post_cqe(CapioRing &ring, uint64_t user_data, int32_t res) {
    uint32_t tail     = *ring.cq_tail;
    io_uring_cqe &cqe = ring.cqes[tail & *ring.cq_mask];
    cqe.user_data     = user_data;
    cqe.res           = res;
    cqe.flags         = 0;
    // Release so the app sees the CQE fields before the advanced tail.
    __atomic_store_n(ring.cq_tail, tail + 1, __ATOMIC_RELEASE);
}

// min_complete is already met: the synchronous drain posted every completion
// before this runs, so there is nothing to wait for. Cap the requirement at CQ
// capacity so an over-large min_complete is satisfiable, not a spin.
// PONYTAIL (synchronous only): F5's async poster must make this a real blocking
// wait on a semaphore it signals (like Queue's _sem_num_elems), never a sleep.
static bool uring_min_complete_satisfied(const CapioRing &ring, unsigned min_complete) {
    if (min_complete == 0) {
        return true;
    }
    unsigned reachable = std::min<unsigned>(min_complete, *ring.cq_ring_entries);
    return uring_cq_ready(ring) >= reachable;
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

    // Drain to_submit SQEs. io_uring_submit already advanced the SQ tail in CAPIO
    // memory; with NO_SQARRAY the SQEs sit in ring order, so sqes[i & mask] is the
    // i-th. Synchronous processing is valid -- io_uring does not guarantee async.
    uint32_t head      = *ring->sq_head;
    uint32_t tail      = __atomic_load_n(ring->sq_tail, __ATOMIC_ACQUIRE);
    uint32_t available = tail - head;
    uint32_t wanted    = std::min<uint32_t>(to_submit, available);
    uint32_t submitted = 0;
    while (submitted < wanted && uring_cq_has_space(*ring)) {
        const io_uring_sqe *sqe = &ring->sqes[head & *ring->sq_mask];
        int32_t res             = uring_dispatch_sqe(sqe, tid);
        uring_post_cqe(*ring, sqe->user_data, res);
        ++head;
        ++submitted;
    }
    // Publish the consumed head so the app's next get_sqe sees the free slots.
    __atomic_store_n(ring->sq_head, head, __ATOMIC_RELEASE);

    if (to_submit > 0 && submitted == 0 && !uring_cq_has_space(*ring)) {
        errno   = EBUSY;
        *result = -errno;
        return CAPIO_POSIX_SYSCALL_SUCCESS;
    }

    // Synchronous processing already posted every completion this call can
    // produce, so the min_complete contract holds without any wait. If it does
    // not, an assumption broke (the drain and the contract disagree) -- surface
    // it instead of masking it with a spin.
    if (!uring_min_complete_satisfied(*ring, min_complete)) {
        LOG("io_uring_enter: min_complete=%u unmet after synchronous drain (ready=%u)",
            min_complete, uring_cq_ready(*ring));
    }
    LOG("io_uring_enter: served %u SQEs synchronously (min_complete=%u, requested_submit=%u)",
        submitted, min_complete, to_submit);

    *result = submitted;
    return CAPIO_POSIX_SYSCALL_SUCCESS;
}
#endif // SYS_io_uring_enter

#ifdef SYS_io_uring_register
int io_uring_register_handler(long arg0, long arg1, long arg2, long arg3, long arg4, long arg5,
                              long *result) {
    auto ring_fd = static_cast<int>(arg0);
    auto opcode  = static_cast<unsigned>(arg1);
    auto nr_args = static_cast<unsigned>(arg3);
    long tid     = syscall_no_intercept(SYS_gettid);
    START_LOG(tid, "call(ring_fd=%d, opcode=%u, nr_args=%u)", ring_fd, opcode, nr_args);

    // Registration is out of scope for the MVP; logging it shows whether real
    // workloads depend on it (fixed buffers/files) before it is refused.
    LOG("io_uring_register: ring_fd=%d opcode=%u nr_args=%u", ring_fd, opcode, nr_args);

    if (get_capio_ring(ring_fd) == nullptr) {
        return CAPIO_POSIX_SYSCALL_SKIP;
    }

    errno   = EOPNOTSUPP;
    *result = -errno;
    return CAPIO_POSIX_SYSCALL_SUCCESS;
}
#endif // SYS_io_uring_register

#endif // SYS_io_uring_setup || SYS_io_uring_enter || SYS_io_uring_register
#endif // CAPIO_POSIX_HANDLERS_IO_URING_HPP
