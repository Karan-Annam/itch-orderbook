// AF_XDP zero-copy kernel-bypass receiver.
//
// AF_XDP (eXpress Data Path) lets userspace pull frames straight out of the NIC
// RX ring with no kernel processing on the hot path. The real implementation
// (compiled only when built against libxdp with -DOB_HAVE_XDP on Linux 5.x+):
//   1. create an AF_XDP socket and mmap a UMEM region shared with the kernel,
//   2. register UMEM, bind the socket to a specific NIC queue,
//   3. steer the ITCH multicast flow to that queue with ethtool,
//   4. in the hot path peek the RX ring, get a zero-copy pointer to the frame,
//      process it, release the descriptor, and replenish the fill ring —
//      with no syscalls per packet.
//
// On hosts without libxdp the class compiles to an unsupported stub so the rest
// of the pipeline builds and the FileReplayReceiver is used instead.
#pragma once

#include <cstdint>
#include <cstddef>
#include <string>

#if defined(__linux__) && defined(OB_HAVE_XDP)
#  include <xdp/xsk.h>
#  include <linux/if_xdp.h>
#endif

namespace ob {

class AFXDPReceiver {
public:
    // Set up the AF_XDP socket on (iface, queue). Returns false if unsupported.
    bool open(const std::string& iface, int queue_id);

    // Peek one frame from the RX ring (zero copy). Sets *frame/*len to a pointer
    // into the UMEM. Returns true if a frame was available. Call release() after
    // processing. Returns false (no frame / unsupported) otherwise.
    bool peek(const uint8_t** frame, uint32_t* len);
    void release();

    void close();
    static bool supported();
    static const char* backend_name();

private:
#if defined(__linux__) && defined(OB_HAVE_XDP)
    xsk_socket*       xsk_      = nullptr;
    xsk_umem*         umem_     = nullptr;
    xsk_ring_cons     rx_{};
    xsk_ring_prod     fill_{};
    void*             umem_area_ = nullptr;
    uint32_t          last_idx_  = 0;
    bool              have_      = false;
#endif
};

}  // namespace ob
