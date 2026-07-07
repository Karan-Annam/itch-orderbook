// Raw-socket busy-poll receiver (the simpler kernel-bypass option).
//
// On Linux this opens an AF_PACKET raw socket, joins the ITCH multicast group,
// and busy-polls with MSG_DONTWAIT on an isolated, SCHED_FIFO-pinned core so the
// thread never sleeps — eliminating scheduler jitter at the cost of one core.
// On non-Linux hosts the class still compiles but open()/poll() report
// unsupported, so callers fall back to FileReplayReceiver.
#pragma once

#include <cstdint>
#include <cstddef>
#include <string>

#include "../util/cpu_affinity.hpp"

#if defined(__linux__)
#  include <sys/socket.h>
#  include <sys/types.h>
#  include <netinet/in.h>
#  include <net/if.h>
#  include <arpa/inet.h>
#  include <unistd.h>
#  include <cstring>
#  include <cerrno>
#endif

namespace ob {

class RawSocketReceiver {
public:
    // Returns false if raw sockets are unavailable on this platform.
    bool open(const std::string& iface, const std::string& mcast_group, uint16_t port,
              int pin_core = -1) {
#if defined(__linux__)
        fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (fd_ < 0) return false;
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(port);
        if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) return false;
        ip_mreq mreq{};
        mreq.imr_multiaddr.s_addr = ::inet_addr(mcast_group.c_str());
        mreq.imr_interface.s_addr = htonl(INADDR_ANY);
        ::setsockopt(fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));
        if (pin_core >= 0) { pin_to_core(pin_core); set_realtime_priority(); }
        (void)iface;
        return true;
#else
        (void)iface; (void)mcast_group; (void)port; (void)pin_core;
        return false;   // unsupported; use FileReplayReceiver
#endif
    }

    // Busy-poll one datagram into buf (non-blocking). Returns bytes read, 0 if
    // none available, -1 on error / unsupported. (Portable `long` return so the
    // header compiles on hosts without POSIX ssize_t.)
    long poll_once(uint8_t* buf, size_t cap) {
#if defined(__linux__)
        long n = static_cast<long>(::recv(fd_, buf, cap, MSG_DONTWAIT));
        return (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) ? 0 : n;
#else
        (void)buf; (void)cap;
        return -1;
#endif
    }

    void close() {
#if defined(__linux__)
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
#endif
    }

    bool supported() const {
#if defined(__linux__)
        return true;
#else
        return false;
#endif
    }

private:
#if defined(__linux__)
    int fd_ = -1;
#endif
};

}  // namespace ob
