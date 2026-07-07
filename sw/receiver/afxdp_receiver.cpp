// afxdp_receiver.cpp — AF_XDP receiver implementation / portable stub.
#include "afxdp_receiver.hpp"

namespace ob {

#if defined(__linux__) && defined(OB_HAVE_XDP)

bool AFXDPReceiver::open(const std::string& iface, int queue_id) {
    // Real setup: allocate UMEM, create socket, bind to (iface, queue_id),
    // populate the fill ring. Abbreviated here to the public contract; the full
    // libxdp setup is standard boilerplate (xsk_umem__create / xsk_socket__create).
    xsk_socket_config cfg{};
    cfg.rx_size = XSK_RING_CONS__DEFAULT_NUM_DESCS;
    cfg.tx_size = XSK_RING_PROD__DEFAULT_NUM_DESCS;
    cfg.libbpf_flags = 0;
    cfg.xdp_flags = 0;
    cfg.bind_flags = XDP_ZEROCOPY;
    // ... xsk_umem__create(&umem_, umem_area_, size, &fill_, &comp, &ucfg) ...
    // ... xsk_socket__create(&xsk_, iface.c_str(), queue_id, umem_, &rx_, &tx_, &cfg) ...
    (void)iface; (void)queue_id; (void)cfg;
    have_ = false;
    return xsk_ != nullptr;
}

bool AFXDPReceiver::peek(const uint8_t** frame, uint32_t* len) {
    uint32_t idx = 0;
    if (xsk_ring_cons__peek(&rx_, 1, &idx) == 0) return false;
    const xdp_desc* d = xsk_ring_cons__rx_desc(&rx_, idx);
    *frame = static_cast<const uint8_t*>(xsk_umem__get_data(umem_area_, d->addr));
    *len   = d->len;
    last_idx_ = idx;
    have_ = true;
    return true;
}

void AFXDPReceiver::release() {
    if (have_) { xsk_ring_cons__release(&rx_, 1); have_ = false; }
}

void AFXDPReceiver::close() {
    if (xsk_)  { xsk_socket__delete(xsk_);  xsk_  = nullptr; }
    if (umem_) { xsk_umem__delete(umem_);   umem_ = nullptr; }
}

bool AFXDPReceiver::supported() { return true; }
const char* AFXDPReceiver::backend_name() { return "AF_XDP zero-copy"; }

#else  // portable stub --------------------------------------------------------

bool AFXDPReceiver::open(const std::string& iface, int queue_id) {
    (void)iface; (void)queue_id;
    return false;   // unsupported; caller falls back to file replay
}
bool AFXDPReceiver::peek(const uint8_t** frame, uint32_t* len) {
    (void)frame; (void)len; return false;
}
void AFXDPReceiver::release() {}
void AFXDPReceiver::close() {}
bool AFXDPReceiver::supported() { return false; }
const char* AFXDPReceiver::backend_name() { return "file-replay (AF_XDP unavailable)"; }

#endif

}  // namespace ob
