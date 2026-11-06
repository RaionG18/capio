#ifndef CAPIO_POSIX_UTILS_URING_KERNEL_HPP
#define CAPIO_POSIX_UTILS_URING_KERNEL_HPP

#if defined(SYS_io_uring_setup)

#include <chrono>
#include <mutex>

#include "utils/uring.hpp"

// The hidden kernel ring at run time: SQEs in, CQEs out to the app's CQ, and
// every wait of a CAPIO ring.

// user_data of CAPIO's wake-up NOPs: their CQEs never reach the app.
constexpr uint64_t CAPIO_URING_WAKE_USER_DATA = 0xCA910C0FFEE00002ULL;

// IORING_SQ_CQ_OVERFLOW while kernel work is in flight: liburing then calls
// io_uring_enter when its CQ looks empty, and enter collects the kernel CQEs.
inline void uring_update_enter_hint(CapioRing &ring) {
    const KernelRing &k = ring.kernel;
    const bool kernel_busy =
        uring_has_kernel(ring) &&
        (k.inflight > 0 || *k.cq_head != __atomic_load_n(k.cq_tail, __ATOMIC_ACQUIRE));
    if (kernel_busy || !ring.overflow.empty()) {
        __atomic_fetch_or(ring.sq_flags, IORING_SQ_CQ_OVERFLOW, __ATOMIC_RELEASE);
    } else {
        __atomic_fetch_and(ring.sq_flags, ~IORING_SQ_CQ_OVERFLOW, __ATOMIC_RELEASE);
    }
}

inline void uring_collect(CapioRing &ring) {
    while (!ring.overflow.empty() && uring_cq_has_space(ring)) {
        const io_uring_cqe cqe = ring.overflow.front();
        ring.overflow.pop_front();
        uring_write_cqe(ring, cqe.user_data, cqe.res, cqe.flags);
    }
    if (!uring_has_kernel(ring)) {
        uring_update_enter_hint(ring);
        return;
    }
    KernelRing &k       = ring.kernel;
    uint32_t head       = *k.cq_head;
    const uint32_t tail = __atomic_load_n(k.cq_tail, __ATOMIC_ACQUIRE);
    while (head != tail) {
        const io_uring_cqe &cqe = k.cqes[head & *k.cq_mask];
        if (cqe.user_data != CAPIO_URING_WAKE_USER_DATA) {
            uring_post_cqe(ring, cqe.user_data, cqe.res, cqe.flags);
        }
        if (!(cqe.flags & IORING_CQE_F_MORE) && k.inflight > 0) {
            --k.inflight;
        }
        ++head;
    }
    __atomic_store_n(k.cq_head, head, __ATOMIC_RELEASE);
    uring_update_enter_hint(ring);
}

inline void uring_kernel_flush(CapioRing &ring) {
    KernelRing &k = ring.kernel;
    for (int tries = 0; k.pending > 0 && tries < 1000; ++tries) {
        const long r = capio_syscall(SYS_io_uring_enter, k.fd, k.pending, 0, 0, nullptr, 0);
        if (r > 0) {
            k.pending -= static_cast<uint32_t>(r);
        } else if (r == -EINTR) {
            continue;
        } else if (r == -EAGAIN || r == -EBUSY) {
            uring_collect(ring);
            capio_syscall(SYS_io_uring_enter, k.fd, 0, 1, IORING_ENTER_GETEVENTS, nullptr, 0);
        } else {
            const int32_t err   = r == 0 ? -EAGAIN : static_cast<int32_t>(r);
            const uint32_t head = __atomic_load_n(k.sq_head, __ATOMIC_ACQUIRE);
            for (uint32_t h = head; h != *k.sq_tail; ++h) {
                const io_uring_sqe &sqe = k.sqes[k.sq_array[h & *k.sq_mask]];
                if (!(sqe.flags & IOSQE_CQE_SKIP_SUCCESS) && k.inflight > 0) {
                    --k.inflight;
                }
                if (sqe.user_data != CAPIO_URING_WAKE_USER_DATA) {
                    uring_post_cqe(ring, sqe.user_data, err);
                }
            }
            __atomic_store_n(k.sq_tail, head, __ATOMIC_RELEASE);
            k.pending = 0;
        }
    }
}

// An SQE with CQE_SKIP_SUCCESS may post no CQE, so it does not count as in flight.
inline void uring_kernel_queue(CapioRing &ring, const io_uring_sqe &sqe) {
    KernelRing &k = ring.kernel;
    if (!uring_has_kernel(ring)) { // not reached: uring_submit keeps such SQEs in CAPIO
        if (sqe.user_data != CAPIO_URING_WAKE_USER_DATA) {
            uring_post_cqe(ring, sqe.user_data, sqe.opcode == IORING_OP_NOP ? 0 : -ENOSYS);
        }
        return;
    }
    if (*k.sq_tail - __atomic_load_n(k.sq_head, __ATOMIC_ACQUIRE) == k.sq_entries) {
        uring_kernel_flush(ring);
    }
    const uint32_t tail = *k.sq_tail;
    const uint32_t slot = tail & *k.sq_mask;
    k.sqes[slot]        = sqe;
    k.sq_array[slot]    = slot;
    __atomic_store_n(k.sq_tail, tail + 1, __ATOMIC_RELEASE);
    ++k.pending;
    if (!(sqe.flags & IOSQE_CQE_SKIP_SUCCESS)) {
        ++k.inflight;
    }
}

// Every wait is in the kernel ring; a thread that posts CAPIO CQEs ends it with a NOP.
inline void uring_wake_waiters(CapioRing &ring) {
    if (ring.waiters > 0 && !uring_has_kernel(ring)) {
        ring.completions.notify_all();
    } else if (ring.waiters > 0) {
        io_uring_sqe nop{};
        nop.opcode    = IORING_OP_NOP;
        nop.user_data = CAPIO_URING_WAKE_USER_DATA;
        uring_kernel_queue(ring, nop);
        uring_kernel_flush(ring);
    }
}

struct UringWaitArg {
    unsigned flags  = 0;
    const void *arg = nullptr;
    size_t size     = 0;
};

// Without a kernel ring only CAPIO posts CQEs, and it wakes the waiters. A
// forked process that shares the ring cannot, so the wait rechecks every 5 ms.
// Returns 0 (recheck the CQ), -ETIME (EXT_ARG timeout passed) or -EINVAL.
inline long uring_local_wait(CapioRing &ring, std::unique_lock<std::mutex> &lock,
                             const UringWaitArg &wait) {
    using clock    = std::chrono::steady_clock;
    auto until     = clock::now() + std::chrono::milliseconds(5);
    bool has_limit = false;
    clock::time_point limit;
    if (wait.flags & IORING_ENTER_EXT_ARG) {
        if (wait.size != sizeof(io_uring_getevents_arg)) {
            return -EINVAL;
        }
        const auto *arg = static_cast<const io_uring_getevents_arg *>(wait.arg);
        if (arg != nullptr && arg->ts != 0) {
            const auto *ts = reinterpret_cast<const __kernel_timespec *>(arg->ts);
            limit          = clock::now() + std::chrono::seconds(ts->tv_sec) +
                    std::chrono::nanoseconds(ts->tv_nsec);
            has_limit = true;
            until     = std::min(until, limit);
        }
    }
    ++ring.waiters;
    ring.completions.wait_until(lock, until);
    --ring.waiters;
    return has_limit && clock::now() >= limit ? -ETIME : 0;
}

inline long uring_kernel_wait(CapioRing &ring, std::unique_lock<std::mutex> &lock,
                              const UringWaitArg &wait = {}) {
    if (!uring_has_kernel(ring)) {
        return uring_local_wait(ring, lock, wait);
    }
    ++ring.waiters;
    lock.unlock();
    const long r = capio_syscall(SYS_io_uring_enter, ring.kernel.fd, 0, 1,
                                 IORING_ENTER_GETEVENTS | wait.flags, wait.arg, wait.size);
    lock.lock();
    --ring.waiters;
    return r;
}

// CAPIO's SQEs are already complete: IOSQE_IO_DRAIN waits for kernel SQEs only.
inline void uring_drain(CapioRing &ring, std::unique_lock<std::mutex> &lock) {
    uring_kernel_flush(ring);
    for (uring_collect(ring); ring.kernel.inflight > 0; uring_collect(ring)) {
        uring_kernel_wait(ring, lock);
    }
}

inline long uring_wait(CapioRing &ring, unsigned min_complete, const UringWaitArg &wait,
                       std::unique_lock<std::mutex> &lock) {
    KernelRing &k = ring.kernel;
    if (k.inflight > 0) {
        capio_syscall(SYS_io_uring_enter, k.fd, 0, 0, IORING_ENTER_GETEVENTS, nullptr, 0);
    }
    uring_collect(ring);
    while (uring_cq_ready(ring) < min_complete && uring_cq_has_space(ring)) {
        const long r = uring_kernel_wait(ring, lock, wait);
        uring_collect(ring);
        if ((r == -EINTR || r == -ETIME || r == -EINVAL) && uring_cq_ready(ring) < min_complete) {
            return r;
        }
    }
    return 0;
}

#endif // SYS_io_uring_setup
#endif // CAPIO_POSIX_UTILS_URING_KERNEL_HPP
