#ifndef CAPIO_POSIX_UTILS_URING_HPP
#define CAPIO_POSIX_UTILS_URING_HPP

#if defined(SYS_io_uring_setup)

#include <cstdint>
#include <linux/io_uring.h>
#include <mutex>
#include <sys/mman.h>
#include <unordered_map>

#include "calf/SyscallLogger.h"
#include "common/syscall.hpp"

// Setup flags newer than some kernel headers; define them (stable ABI values).
#ifndef IORING_SETUP_SUBMIT_ALL
#define IORING_SETUP_SUBMIT_ALL (1U << 7)
#endif
#ifndef IORING_SETUP_COOP_TASKRUN
#define IORING_SETUP_COOP_TASKRUN (1U << 8)
#endif
#ifndef IORING_SETUP_TASKRUN_FLAG
#define IORING_SETUP_TASKRUN_FLAG (1U << 9)
#endif
#ifndef IORING_SETUP_SINGLE_ISSUER
#define IORING_SETUP_SINGLE_ISSUER (1U << 12)
#endif
#ifndef IORING_SETUP_DEFER_TASKRUN
#define IORING_SETUP_DEFER_TASKRUN (1U << 13)
#endif
#ifndef IORING_SETUP_NO_SQARRAY
#define IORING_SETUP_NO_SQARRAY (1U << 16)
#endif
#ifndef IOSQE_CQE_SKIP_SUCCESS
#define IOSQE_CQE_SKIP_SUCCESS (1U << 6)
#endif
#ifndef IORING_FEAT_CQE_SKIP
#define IORING_FEAT_CQE_SKIP (1U << 11)
#endif

// CAPIO's own io_uring ring: CAPIO owns and lays out the two mmap regions, and
// the fabricated sq_off/cq_off tell liburing where each field lives. Layout of
// the SQ_RING region (holds the CQ too, via SINGLE_MMAP); SQES holds the sqes:
//   [ sq: head tail ring_mask ring_entries flags dropped ][ sq array ]
//   [ cq: head tail ring_mask ring_entries overflow flags ][ cqes[] ]
struct CapioRing {
    uint32_t sq_entries;
    uint32_t cq_entries;
    bool no_sqarray = false; // SQEs in ring order; otherwise sq_array holds their indices

    // The two mmap regions, owned here.
    void *sq_ring       = nullptr; // IORING_OFF_SQ_RING: rings + sq array + cqes
    size_t sq_ring_size = 0;
    io_uring_sqe *sqes  = nullptr; // IORING_OFF_SQES
    size_t sqes_size    = 0;

    // The fields of sq_ring that CAPIO reads or writes while serving the ring.
    uint32_t *sq_head = nullptr, *sq_tail = nullptr, *sq_mask = nullptr, *sq_dropped = nullptr;
    uint32_t *sq_array = nullptr;
    uint32_t *cq_head = nullptr, *cq_tail = nullptr, *cq_mask = nullptr, *cq_ring_entries = nullptr;
    io_uring_cqe *cqes = nullptr;
};

// Round up to the next multiple of alignment (a power of two).
static inline size_t uring_align_up(size_t n, size_t alignment) {
    return (n + alignment - 1) & ~(alignment - 1);
}

/*
 * Lay out the two regions for ring.sq_entries SQEs and ring.cq_entries CQEs,
 * report the offsets in params, and allocate page-rounded backing memory.
 * Returns false on allocation failure.
 */
inline bool uring_layout(CapioRing &ring, io_uring_params *params) {
    START_LOG(capio_syscall(SYS_gettid), "call(sq_entries=%u, cq_entries=%u)", ring.sq_entries,
              ring.cq_entries);

    io_sqring_offsets &sq = params->sq_off;
    io_cqring_offsets &cq = params->cq_off;
    sq                    = {};
    cq                    = {};
    uint32_t off          = 0;
    auto next             = [&off](size_t bytes) {
        const uint32_t at = off;
        off += static_cast<uint32_t>(bytes);
        return at;
    };

    sq.head         = next(sizeof(uint32_t));
    sq.tail         = next(sizeof(uint32_t));
    sq.ring_mask    = next(sizeof(uint32_t));
    sq.ring_entries = next(sizeof(uint32_t));
    sq.flags        = next(sizeof(uint32_t));
    sq.dropped      = next(sizeof(uint32_t));
    sq.array        = next(sizeof(uint32_t) * ring.sq_entries);
    cq.head         = next(sizeof(uint32_t));
    cq.tail         = next(sizeof(uint32_t));
    cq.ring_mask    = next(sizeof(uint32_t));
    cq.ring_entries = next(sizeof(uint32_t));
    cq.overflow     = next(sizeof(uint32_t));
    cq.flags        = next(sizeof(uint32_t));
    off             = static_cast<uint32_t>(uring_align_up(off, alignof(io_uring_cqe)));
    cq.cqes         = next(sizeof(io_uring_cqe) * ring.cq_entries);

    ring.sq_ring_size = uring_align_up(off, 4096);
    ring.sqes_size    = uring_align_up(sizeof(io_uring_sqe) * ring.sq_entries, 4096);

    // Raw syscalls, as in destroy_capio_ring: libc's would re-enter the mmap hooks.
    // Anonymous memory arrives zeroed, so heads and tails start at 0.
    const long sq_mem = capio_syscall(SYS_mmap, nullptr, ring.sq_ring_size, PROT_READ | PROT_WRITE,
                                      MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    const long sqes_mem = capio_syscall(SYS_mmap, nullptr, ring.sqes_size, PROT_READ | PROT_WRITE,
                                        MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (sq_mem < 0 || sqes_mem < 0) {
        LOG("ring mmap failed: sq_ring=%ld sqes=%ld", sq_mem, sqes_mem);
        if (sq_mem >= 0) {
            capio_syscall(SYS_munmap, sq_mem, ring.sq_ring_size);
        }
        if (sqes_mem >= 0) {
            capio_syscall(SYS_munmap, sqes_mem, ring.sqes_size);
        }
        return false;
    }

    char *base   = reinterpret_cast<char *>(sq_mem);
    auto field   = [base](uint32_t offset) { return reinterpret_cast<uint32_t *>(base + offset); };
    ring.sq_ring = base;
    ring.sqes    = reinterpret_cast<io_uring_sqe *>(sqes_mem);
    ring.sq_head = field(sq.head);
    ring.sq_tail = field(sq.tail);
    ring.sq_mask = field(sq.ring_mask);
    ring.sq_dropped      = field(sq.dropped);
    ring.sq_array        = field(sq.array);
    ring.cq_head         = field(cq.head);
    ring.cq_tail         = field(cq.tail);
    ring.cq_mask         = field(cq.ring_mask);
    ring.cq_ring_entries = field(cq.ring_entries);
    ring.cqes            = reinterpret_cast<io_uring_cqe *>(base + cq.cqes);

    // The ring metadata the app reads.
    *ring.sq_mask           = ring.sq_entries - 1;
    *field(sq.ring_entries) = ring.sq_entries;
    *ring.cq_mask           = ring.cq_entries - 1;
    *ring.cq_ring_entries   = ring.cq_entries;

    LOG("uring_layout: sq_ring_size=%zu sqes_size=%zu sq_array=%u cqes=%u", ring.sq_ring_size,
        ring.sqes_size, sq.array, cq.cqes);
    return true;
}

// Per-process table of rings, keyed by the fake fd returned from setup. Every
// mmap/munmap in the process consults it, so it is locked; recursive because the
// handlers' own libc calls (mmap in uring_layout) re-enter the hook. Entries are
// stable across inserts, so a looked-up ring stays valid until it is closed.
inline std::unordered_map<int, CapioRing> *capio_rings;
inline std::recursive_mutex capio_rings_mutex;

inline CapioRing *get_capio_ring(int fd) {
    const std::lock_guard<std::recursive_mutex> lock(capio_rings_mutex);
    if (capio_rings == nullptr) {
        return nullptr;
    }
    auto it = capio_rings->find(fd);
    return it == capio_rings->end() ? nullptr : &it->second;
}

inline void destroy_capio_ring(CapioRing &ring) {
    // Bypass the hook that keeps application munmap calls from freeing owned rings.
    if (ring.sq_ring != nullptr) {
        capio_syscall(SYS_munmap, ring.sq_ring, ring.sq_ring_size);
        ring.sq_ring = nullptr;
    }
    if (ring.sqes != nullptr) {
        capio_syscall(SYS_munmap, ring.sqes, ring.sqes_size);
        ring.sqes = nullptr;
    }
}

inline bool destroy_capio_ring(int fd) {
    const std::lock_guard<std::recursive_mutex> lock(capio_rings_mutex);
    if (capio_rings == nullptr) {
        return false;
    }
    auto it = capio_rings->find(fd);
    if (it == capio_rings->end()) {
        return false;
    }

    destroy_capio_ring(it->second);
    capio_rings->erase(it);
    if (capio_rings->empty()) {
        delete capio_rings;
        capio_rings = nullptr;
    }
    return true;
}

#endif // SYS_io_uring_setup
#endif // CAPIO_POSIX_UTILS_URING_HPP
