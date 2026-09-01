#ifndef CAPIO_POSIX_UTILS_URING_HPP
#define CAPIO_POSIX_UTILS_URING_HPP

#if defined(SYS_io_uring_setup)

#include <cstdint>
#include <cstring>
#include <linux/io_uring.h>
#include <sys/mman.h>
#include <unordered_map>

#include "common/logger.hpp"

// CAPIO's own io_uring ring: CAPIO owns and lays out the two mmap regions, and
// the fabricated sq_off/cq_off tell liburing where each field lives. Layout of
// the SQ_RING region (holds the CQ too, via SINGLE_MMAP); SQES holds the sqes:
//   [ sq: head tail ring_mask ring_entries flags dropped ][ sq array ]
//   [ cq: head tail ring_mask ring_entries overflow cqes flags ][ cqes[] ]
struct CapioRing {
    int fake_fd;
    uint32_t sq_entries;
    uint32_t cq_entries;

    // The two mmap regions, owned here.
    void *sq_ring       = nullptr; // IORING_OFF_SQ_RING: rings + sq array + cqes
    size_t sq_ring_size = 0;
    io_uring_sqe *sqes  = nullptr; // IORING_OFF_SQES
    size_t sqes_size    = 0;

    // Pointers into sq_ring, set by layout(). Named to match the ring fields
    // liburing reads through the reported offsets.
    uint32_t *sq_head = nullptr, *sq_tail = nullptr, *sq_mask = nullptr, *sq_ring_entries = nullptr;
    uint32_t *sq_flags = nullptr, *sq_dropped = nullptr, *sq_array = nullptr;
    uint32_t *cq_head = nullptr, *cq_tail = nullptr, *cq_mask = nullptr, *cq_ring_entries = nullptr;
    uint32_t *cq_overflow = nullptr, *cq_flags = nullptr;
    io_uring_cqe *cqes = nullptr;

    // Head CAPIO consumes SQEs from / tail CAPIO has processed, kept separately
    // from the app-visible sq_head so draining logic is unambiguous.
    uint32_t sq_processed = 0;
};

// Round up to the next multiple of alignment (a power of two).
static inline size_t uring_align_up(size_t n, size_t alignment) {
    return (n + alignment - 1) & ~(alignment - 1);
}

/*
 * Lay out the two regions for `entries` SQEs and 2*entries CQEs, fill `params`
 * with the chosen offsets, and allocate page-rounded backing memory. Returns
 * false on allocation failure.
 */
inline bool uring_layout(CapioRing &ring, io_uring_params *params) {
    START_LOG(capio_syscall(SYS_gettid), "call(entries=%u)", ring.sq_entries);

    const uint32_t sqe_n = ring.sq_entries;
    const uint32_t cqe_n = ring.cq_entries;

    // Six sq control words, then the sq array (one u32 per sqe slot).
    size_t off = 0;
    auto place = [&](uint32_t *&field) {
        field = reinterpret_cast<uint32_t *>(off);
        off += sizeof(uint32_t);
    };

    place(ring.sq_head);
    place(ring.sq_tail);
    place(ring.sq_mask);
    place(ring.sq_ring_entries);
    place(ring.sq_flags);
    place(ring.sq_dropped);
    const size_t sq_array_off = off;
    off += sizeof(uint32_t) * sqe_n;

    // Seven cq control words, aligned for the cqe array that follows.
    place(ring.cq_head);
    place(ring.cq_tail);
    place(ring.cq_mask);
    place(ring.cq_ring_entries);
    place(ring.cq_overflow);
    place(ring.cq_flags);

    off                   = uring_align_up(off, alignof(io_uring_cqe));
    const size_t cqes_off = off;
    off += sizeof(io_uring_cqe) * cqe_n;

    ring.sq_ring_size = uring_align_up(off, 4096);
    ring.sqes_size    = uring_align_up(sizeof(io_uring_sqe) * sqe_n, 4096);

    void *sq = mmap(nullptr, ring.sq_ring_size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE,
                    -1, 0);
    if (sq == MAP_FAILED) {
        LOG("sq_ring mmap failed");
        return false;
    }
    void *sqes =
        mmap(nullptr, ring.sqes_size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (sqes == MAP_FAILED) {
        munmap(sq, ring.sq_ring_size);
        LOG("sqes mmap failed");
        return false;
    }
    memset(sq, 0, ring.sq_ring_size);

    ring.sq_ring = sq;
    ring.sqes    = reinterpret_cast<io_uring_sqe *>(sqes);

    // Turn the byte offsets accumulated above into real pointers into sq.
    auto rebase = [&](uint32_t *&field) {
        field = reinterpret_cast<uint32_t *>(static_cast<char *>(sq) +
                                             reinterpret_cast<uintptr_t>(field));
    };
    rebase(ring.sq_head);
    rebase(ring.sq_tail);
    rebase(ring.sq_mask);
    rebase(ring.sq_ring_entries);
    rebase(ring.sq_flags);
    rebase(ring.sq_dropped);
    rebase(ring.cq_head);
    rebase(ring.cq_tail);
    rebase(ring.cq_mask);
    rebase(ring.cq_ring_entries);
    rebase(ring.cq_overflow);
    rebase(ring.cq_flags);
    ring.sq_array = reinterpret_cast<uint32_t *>(static_cast<char *>(sq) + sq_array_off);
    ring.cqes     = reinterpret_cast<io_uring_cqe *>(static_cast<char *>(sq) + cqes_off);

    // Initialise the ring metadata the app will read.
    *ring.sq_mask         = sqe_n - 1;
    *ring.sq_ring_entries = sqe_n;
    *ring.cq_mask         = cqe_n - 1;
    *ring.cq_ring_entries = cqe_n;

    // Report the chosen layout to liburing. Offsets are relative to sq_ring.
    auto rel = [&](const void *p) {
        return static_cast<uint32_t>(reinterpret_cast<const char *>(p) -
                                     static_cast<const char *>(sq));
    };
    params->sq_off.head         = rel(ring.sq_head);
    params->sq_off.tail         = rel(ring.sq_tail);
    params->sq_off.ring_mask    = rel(ring.sq_mask);
    params->sq_off.ring_entries = rel(ring.sq_ring_entries);
    params->sq_off.flags        = rel(ring.sq_flags);
    params->sq_off.dropped      = rel(ring.sq_dropped);
    params->sq_off.array        = static_cast<uint32_t>(sq_array_off);

    params->cq_off.head         = rel(ring.cq_head);
    params->cq_off.tail         = rel(ring.cq_tail);
    params->cq_off.ring_mask    = rel(ring.cq_mask);
    params->cq_off.ring_entries = rel(ring.cq_ring_entries);
    params->cq_off.overflow     = rel(ring.cq_overflow);
    params->cq_off.cqes         = static_cast<uint32_t>(cqes_off);
    params->cq_off.flags        = rel(ring.cq_flags);

    LOG("uring_layout: sq_ring_size=%zu sqes_size=%zu sq_array_off=%zu cqes_off=%zu",
        ring.sq_ring_size, ring.sqes_size, sq_array_off, cqes_off);
    return true;
}

// Per-process table of rings, keyed by the fake fd returned from setup.
inline std::unordered_map<int, CapioRing> *capio_rings;

inline CapioRing *get_capio_ring(int fd) {
    if (capio_rings == nullptr) {
        return nullptr;
    }
    auto it = capio_rings->find(fd);
    return it == capio_rings->end() ? nullptr : &it->second;
}

inline void destroy_capio_ring(CapioRing &ring) {
    if (ring.sq_ring != nullptr) {
        munmap(ring.sq_ring, ring.sq_ring_size);
        ring.sq_ring = nullptr;
    }
    if (ring.sqes != nullptr) {
        munmap(ring.sqes, ring.sqes_size);
        ring.sqes = nullptr;
    }
}

inline bool destroy_capio_ring(int fd) {
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
