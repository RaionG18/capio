#ifndef CAPIO_POSIX_HANDLERS_MMAP_HPP
#define CAPIO_POSIX_HANDLERS_MMAP_HPP

#if defined(SYS_mmap) && defined(SYS_io_uring_setup)

#include <linux/io_uring.h>

#include "utils/common.hpp"
#include "utils/uring.hpp"

/*
 * mmap/munmap handlers, active ONLY for CAPIO ring fds.
 *
 * liburing mmaps the ring fd twice after setup: offset IORING_OFF_SQ_RING for
 * the ring metadata (SQ + CQ, coalesced by SINGLE_MMAP) and offset
 * IORING_OFF_SQES for the SQE array. CAPIO answers each with the region it
 * already allocated in CapioRing, so liburing operates on CAPIO memory. Any
 * mmap whose fd is not a CAPIO ring is passed straight to the kernel -- this is
 * a surgical addition, not a general mmap interceptor.
 */
int mmap_handler(long arg0, long arg1, long arg2, long arg3, long arg4, long arg5, long *result) {
    auto fd     = static_cast<int>(arg4);
    auto offset = static_cast<off64_t>(arg5);
    long tid    = syscall_no_intercept(SYS_gettid);
    START_LOG(tid, "call(fd=%d, offset=0x%lx)", fd, offset);

    CapioRing *ring = get_capio_ring(fd);
    if (ring == nullptr) {
        return CAPIO_POSIX_SYSCALL_SKIP; // not a ring fd: kernel handles it
    }

    if (offset == IORING_OFF_SQ_RING) {
        LOG("mmap ring fd %d SQ_RING -> %p", fd, ring->sq_ring);
        *result = reinterpret_cast<long>(ring->sq_ring);
        return CAPIO_POSIX_SYSCALL_SUCCESS;
    }
    if (offset == IORING_OFF_SQES) {
        LOG("mmap ring fd %d SQES -> %p", fd, ring->sqes);
        *result = reinterpret_cast<long>(ring->sqes);
        return CAPIO_POSIX_SYSCALL_SUCCESS;
    }

    // A ring fd mmap'd at an unexpected offset means an assumption broke.
    LOG("mmap ring fd %d at unexpected offset 0x%lx", fd, offset);
    errno   = EINVAL;
    *result = -errno;
    return CAPIO_POSIX_SYSCALL_SUCCESS;
}

int munmap_handler(long arg0, long arg1, long arg2, long arg3, long arg4, long arg5, long *result) {
    auto *addr = reinterpret_cast<void *>(arg0);
    long tid   = syscall_no_intercept(SYS_gettid);
    START_LOG(tid, "call(addr=%p)", addr);

    // The ring's regions are owned by CapioRing and freed when the ring is torn
    // down, not on the app's munmap. If the address belongs to a ring, absorb
    // the munmap; otherwise let the kernel handle it.
    if (capio_rings != nullptr) {
        for (auto &entry : *capio_rings) {
            CapioRing &r = entry.second;
            if (addr == r.sq_ring || addr == r.sqes) {
                LOG("munmap of ring region %p absorbed", addr);
                *result = 0;
                return CAPIO_POSIX_SYSCALL_SUCCESS;
            }
        }
    }
    return CAPIO_POSIX_SYSCALL_SKIP;
}

#endif // SYS_mmap && SYS_io_uring_setup
#endif // CAPIO_POSIX_HANDLERS_MMAP_HPP
