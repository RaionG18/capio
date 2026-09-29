#ifndef CAPIO_POSIX_HANDLERS_IO_URING_HPP
#define CAPIO_POSIX_HANDLERS_IO_URING_HPP

#if defined(SYS_io_uring_setup) || defined(SYS_io_uring_enter) || defined(SYS_io_uring_register)

#include <algorithm>
#include <linux/io_uring.h>
#include <time.h>

#include "utils/common.hpp"
#include "utils/filesystem.hpp"
#include "utils/uring.hpp"

#include "lseek.hpp"
#include "read.hpp"
#include "write.hpp"

/*
 * io_uring handlers (425-427). setup builds a CAPIO-owned ring (fake fd +
 * fabricated params) so SQEs never reach the kernel; enter drains and serves
 * them; the ring's emulated mmap lives in mmap.hpp, keyed off the fake fd.
 */

// DRAIN and ASYNC hold trivially when SQEs run in order, synchronously; links
// and CQE_SKIP_SUCCESS are honoured in the drain loop.
static constexpr uint8_t kUringSupportedSqeFlags =
    IOSQE_IO_DRAIN | IOSQE_IO_LINK | IOSQE_IO_HARDLINK | IOSQE_ASYNC | IOSQE_CQE_SKIP_SUCCESS;

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

// Kernel limits (IORING_MAX_ENTRIES / IORING_MAX_CQ_ENTRIES).
static constexpr uint32_t kUringMaxSqEntries = 32768;
static constexpr uint32_t kUringMaxCqEntries = 2 * kUringMaxSqEntries;

// Next power of two >= n, as the kernel requires: above max it clamps with
// IORING_SETUP_CLAMP and fails otherwise.
static bool uring_entries(uint32_t n, uint32_t max, bool clamp, uint32_t &out) {
    if (n == 0 || (n > max && !clamp)) {
        return false;
    }
    n          = std::min(n, max);
    uint32_t p = 1;
    while (p < n) {
        p <<= 1;
    }
    out = p;
    return true;
}

// Sizes the ring as the kernel does: CQ is 2x SQ unless CQSIZE asks for more.
static bool uring_ring_sizes(uint32_t entries, const io_uring_params *params, CapioRing &ring) {
    const bool clamp = params->flags & IORING_SETUP_CLAMP;
    if (!uring_entries(entries, kUringMaxSqEntries, clamp, ring.sq_entries)) {
        return false;
    }
    if (!(params->flags & IORING_SETUP_CQSIZE)) {
        ring.cq_entries = ring.sq_entries * 2;
        return true;
    }
    return uring_entries(params->cq_entries, kUringMaxCqEntries, clamp, ring.cq_entries) &&
           ring.cq_entries >= ring.sq_entries;
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

    // Allowlist: these flags only tune kernel task_work or completion batching,
    // which synchronous execution already satisfies. The rest change geometry
    // or the mmap mechanism, or need real async, so they fail with -EINVAL.
    constexpr unsigned kSupportedFlags = IORING_SETUP_NO_SQARRAY | IORING_SETUP_CQSIZE |
                                         IORING_SETUP_CLAMP | IORING_SETUP_SUBMIT_ALL |
                                         IORING_SETUP_COOP_TASKRUN | IORING_SETUP_TASKRUN_FLAG |
                                         IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN;
    const unsigned flags = params->flags;
    const bool bad_combination =
        ((flags & IORING_SETUP_DEFER_TASKRUN) && !(flags & IORING_SETUP_SINGLE_ISSUER)) ||
        ((flags & IORING_SETUP_TASKRUN_FLAG) &&
         !(flags & (IORING_SETUP_COOP_TASKRUN | IORING_SETUP_DEFER_TASKRUN)));
    if ((flags & ~kSupportedFlags) || bad_combination) {
        LOG("rejecting setup flags: 0x%x", flags);
        errno   = EINVAL;
        *result = -errno;
        return CAPIO_POSIX_SYSCALL_SUCCESS;
    }

    // Fake fd the CAPIO way: a real kernel fd (so close/poll on it behave), but
    // it names /dev/null, never a real ring. The ring lives in CapioRing.
    int fake_fd =
        static_cast<int>(syscall_no_intercept(SYS_openat, AT_FDCWD, "/dev/null", O_RDONLY, 0));
    if (fake_fd < 0) {
        ERR_EXIT("io_uring_setup: unable to open /dev/null for fake ring fd");
    }

    const std::lock_guard<std::recursive_mutex> lock(capio_rings_mutex);
    if (capio_rings == nullptr) {
        capio_rings = new std::unordered_map<int, CapioRing>();
    }
    CapioRing &ring = (*capio_rings)[fake_fd];
    ring.fake_fd    = fake_fd;
    ring.no_sqarray = flags & IORING_SETUP_NO_SQARRAY;
    if (!uring_ring_sizes(entries, params, ring)) {
        capio_rings->erase(fake_fd);
        syscall_no_intercept(SYS_close, fake_fd);
        errno   = EINVAL;
        *result = -errno;
        return CAPIO_POSIX_SYSCALL_SUCCESS;
    }

    params->sq_entries = ring.sq_entries;
    params->cq_entries = ring.cq_entries;
    // Only what synchronous execution makes true: SQE data is consumed at submit,
    // off == -1 uses the file position, and CQE_SKIP_SUCCESS is honoured.
    params->features   = IORING_FEAT_SINGLE_MMAP | IORING_FEAT_SUBMIT_STABLE |
                       IORING_FEAT_RW_CUR_POS | IORING_FEAT_CQE_SKIP;

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

// A non-CAPIO fd sharing the ring runs synchronously against the kernel and
// reports its real result. preadv2/pwritev2 take off == -1 as "current
// position" and honour the SQE's RWF_* flags, so one call covers every case.
static int32_t uring_passthrough_rw(const io_uring_sqe *sqe, const iovec *iov, int iovcnt,
                                    bool is_write) {
    long r = syscall_no_intercept(is_write ? SYS_pwritev2 : SYS_preadv2, sqe->fd, iov, iovcnt,
                                  static_cast<long>(sqe->off), 0L, sqe->rw_flags);
    return static_cast<int32_t>(r); // raw result: already -errno on failure
}

static int32_t uring_result(off64_t res) {
    return static_cast<int32_t>(res == CAPIO_POSIX_SYSCALL_ERRNO ? -errno : res);
}

// capio_readv/capio_writev start at the descriptor's position, so an explicit
// offset seeks there first (which also flushes the caches and syncs the server)
// and seeks back afterwards, leaving the position as io_uring does.
static int32_t uring_capio_rw(const io_uring_sqe *sqe, const iovec *iov, int iovcnt, bool is_write,
                              long tid) {
    START_LOG(tid, "call(fd=%d, off=%lld, iovcnt=%d, is_write=%d)", sqe->fd,
              static_cast<long long>(sqe->off), iovcnt, is_write);
    auto transfer = [&]() {
        return uring_result(is_write ? capio_writev(sqe->fd, iov, iovcnt, tid)
                                     : capio_readv(sqe->fd, iov, iovcnt, tid));
    };
    if (sqe->off == static_cast<uint64_t>(-1)) {
        return transfer();
    }

    off64_t saved = get_capio_fd_offset(sqe->fd);
    if (capio_lseek(sqe->fd, static_cast<off64_t>(sqe->off), SEEK_SET, tid) < 0) {
        return -errno;
    }
    int32_t res = transfer();
    if (capio_lseek(sqe->fd, saved, SEEK_SET, tid) < 0) {
        LOG("could not restore offset %ld on fd %d", saved, sqe->fd); // res still stands
    }
    return res;
}

static int32_t uring_rw(const io_uring_sqe *sqe, bool vectored, bool is_write, long tid) {
    iovec single{reinterpret_cast<void *>(sqe->addr), sqe->len};
    const auto *iov  = vectored ? reinterpret_cast<const iovec *>(sqe->addr) : &single;
    const int iovcnt = vectored ? static_cast<int>(sqe->len) : 1;
    return exists_capio_fd(sqe->fd) ? uring_capio_rw(sqe, iov, iovcnt, is_write, tid)
                                    : uring_passthrough_rw(sqe, iov, iovcnt, is_write);
}

// Serve one SQE and produce its completion result (bytes transferred, or -errno
// as the io_uring convention). CAPIO fds delegate to the existing capio_*
// handlers so the data path, cache and CAPIO-CL semantics are reused, not
// reimplemented; non-CAPIO fds pass through to the kernel synchronously.
static int32_t uring_dispatch_sqe(const io_uring_sqe *sqe, long tid) {
    START_LOG(tid, "call(opcode=%u, fd=%d, user_data=%llu)", sqe->opcode, sqe->fd,
              (unsigned long long) sqe->user_data);

    if (sqe->flags & IOSQE_FIXED_FILE) {
        return -EBADF; // io_uring_register is refused, so no file is ever registered
    }
    if (sqe->flags & ~kUringSupportedSqeFlags) {
        return -EINVAL;
    }

    switch (sqe->opcode) {
    case IORING_OP_NOP:
        return 0;

    case IORING_OP_FSYNC:
        if (exists_capio_fd(sqe->fd)) {
            // Durability for CAPIO-owned fds is handled by CAPIO commit rules.
            return 0;
        }
        return static_cast<int32_t>(std::min(
            syscall_no_intercept(
                (sqe->fsync_flags & IORING_FSYNC_DATASYNC) ? SYS_fdatasync : SYS_fsync, sqe->fd),
            0L));

    case IORING_OP_WRITE:
        return uring_rw(sqe, false, true, tid);
    case IORING_OP_READ:
        return uring_rw(sqe, false, false, tid);
    case IORING_OP_WRITEV:
        return uring_rw(sqe, true, true, tid);
    case IORING_OP_READV:
        return uring_rw(sqe, true, false, tid);

    default:
        LOG("opcode %u not implemented yet", sqe->opcode);
        return -EINVAL;
    }
}

// A linked SQE fails its chain on an error or, for reads/writes, a short
// transfer (as the kernel does); later members complete with -ECANCELED.
static bool uring_breaks_link(const io_uring_sqe *sqe, int32_t res) {
    if (res < 0) {
        return true;
    }
    if (sqe->opcode == IORING_OP_READ || sqe->opcode == IORING_OP_WRITE) {
        return static_cast<uint32_t>(res) < sqe->len;
    }
    if (sqe->opcode == IORING_OP_READV || sqe->opcode == IORING_OP_WRITEV) {
        const auto *iov = reinterpret_cast<const iovec *>(sqe->addr);
        size_t total    = 0;
        for (uint32_t i = 0; i < sqe->len; ++i) {
            total += iov[i].iov_len;
        }
        return static_cast<size_t>(res) < total;
    }
    return false;
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

    // Drain to_submit SQEs. The app already advanced the SQ tail in CAPIO memory.
    // With NO_SQARRAY the SQEs sit in ring order; otherwise sq_array names each
    // one (liburing fills it 1:1, other apps such as fio do not). Synchronous
    // processing is valid -- io_uring does not guarantee async.
    uint32_t head      = *ring->sq_head;
    uint32_t tail      = __atomic_load_n(ring->sq_tail, __ATOMIC_ACQUIRE);
    uint32_t available = tail - head;
    uint32_t wanted    = std::min<uint32_t>(to_submit, available);
    uint32_t submitted = 0;
    bool chain_failed  = false; // a chain never spans two io_uring_enter calls
    while (submitted < wanted && uring_cq_has_space(*ring)) {
        const uint32_t slot = head & *ring->sq_mask;
        const uint32_t idx  = ring->no_sqarray ? slot : ring->sq_array[slot];
        if (idx >= ring->sq_entries) { // invalid index: dropped, as the kernel does
            ++*ring->sq_dropped;
            ++head;
            break;
        }
        const io_uring_sqe *sqe = &ring->sqes[idx];
        const uint8_t sqe_flags = sqe->flags;
        int32_t res             = chain_failed ? -ECANCELED : uring_dispatch_sqe(sqe, tid);
        if (!chain_failed && (sqe_flags & IOSQE_IO_LINK) && uring_breaks_link(sqe, res)) {
            chain_failed = true;
        }
        if (!(sqe_flags & (IOSQE_IO_LINK | IOSQE_IO_HARDLINK))) {
            chain_failed = false;
        }
        if (!((sqe_flags & IOSQE_CQE_SKIP_SUCCESS) && res >= 0)) {
            uring_post_cqe(*ring, sqe->user_data, res);
        }
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
