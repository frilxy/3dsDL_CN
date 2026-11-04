#include "mdns_responder.hpp"

#include "mdns.hpp"
#include "socket_wait.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <vector>

namespace net {

namespace {

// Another device that already uses the name answers a probe well within
// this; RFC 6762 waits 250 ms between three probes.
constexpr int ProbeGapMs = 250;
constexpr int ProbeCount = 3;
constexpr int MaxSuffix = 9;

uint32_t groupAddress()
{
    in_addr group {};
    inet_aton(mdns::Group, &group);
    return group.s_addr;
}

} // namespace

MdnsResponder::~MdnsResponder()
{
    close();
}

bool MdnsResponder::open(const std::string &name, uint32_t address,
                         unsigned short port, bool multicast,
                         std::string &error)
{
    close();
    socket_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_ < 0) {
        error = "Could not open a network socket.";
        return false;
    }
    const int on = 1;
    setsockopt(socket_, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
#ifdef SO_REUSEPORT
    // A desktop already runs its own responder on 5353.
    setsockopt(socket_, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on));
#endif
    sockaddr_in local {};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(multicast ? INADDR_ANY : INADDR_LOOPBACK);
    local.sin_port = htons(port);
    if (bind(socket_, reinterpret_cast<sockaddr *>(&local), sizeof(local)) !=
        0) {
        error = "Port " + std::to_string(port) + " is in use.";
        close();
        return false;
    }
    if (multicast) {
        ip_mreq membership {};
        membership.imr_multiaddr.s_addr = groupAddress();
        membership.imr_interface.s_addr = htonl(INADDR_ANY);
        if (setsockopt(socket_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &membership,
                       sizeof(membership)) != 0) {
            error = "The network does not allow multicast.";
            close();
            return false;
        }
        const unsigned char ttl = 255;
        setsockopt(socket_, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    }
    socklen_t length = sizeof(local);
    port_ = getsockname(socket_, reinterpret_cast<sockaddr *>(&local),
                        &length) == 0
                ? ntohs(local.sin_port)
                : port;
    base_ = name;
    suffix_ = 1;
    address_ = address;
    multicast_ = multicast;
    return true;
}

void MdnsResponder::close()
{
    if (socket_ >= 0) {
        ::close(socket_);
        socket_ = -1;
    }
    port_ = 0;
}

std::string MdnsResponder::hostname() const
{
    return base_ + (suffix_ > 1 ? "-" + std::to_string(suffix_) : "") +
           ".local";
}

void MdnsResponder::send(const void *data, size_t size, uint32_t address,
                         unsigned short port)
{
    sockaddr_in to {};
    to.sin_family = AF_INET;
    to.sin_addr.s_addr = address;
    to.sin_port = htons(port);
    sendto(socket_, data, size, 0, reinterpret_cast<const sockaddr *>(&to),
           sizeof(to));
}

void MdnsResponder::claim()
{
    if (socket_ < 0 || !multicast_) {
        return;
    }
    using Clock = std::chrono::steady_clock;
    bool taken = true;
    while (taken && suffix_ <= MaxSuffix) {
        const std::string name = hostname();
        const std::vector<uint8_t> question = mdns::probe(name);
        taken = false;
        for (int round = 0; round < ProbeCount && !taken; ++round) {
            send(question.data(), question.size(), groupAddress(), mdns::Port);
            const Clock::time_point until =
                Clock::now() + std::chrono::milliseconds(ProbeGapMs);
            while (!taken) {
                const auto left =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        until - Clock::now())
                        .count();
                pollfd waiting {socket_, POLLIN, 0};
                if (left <= 0 ||
                    waitSockets(&waiting, 1, static_cast<int>(left), 5) <= 0) {
                    break;
                }
                uint8_t packet[1500];
                sockaddr_in from {};
                socklen_t length = sizeof(from);
                const ssize_t got =
                    recvfrom(socket_, packet, sizeof(packet), MSG_DONTWAIT,
                             reinterpret_cast<sockaddr *>(&from), &length);
                taken = got > 0 && from.sin_addr.s_addr != address_ &&
                        mdns::claims(packet, static_cast<size_t>(got), name);
            }
        }
        if (taken) {
            ++suffix_;
        }
    }
    // Telling everyone up front clears any old address a phone has cached.
    mdns::Asked everything;
    everything.address = true;
    const std::vector<uint8_t> announcement =
        mdns::answer(hostname(), address_, everything, false);
    send(announcement.data(), announcement.size(), groupAddress(), mdns::Port);
}

void MdnsResponder::serve(int timeoutMs)
{
    if (socket_ < 0) {
        return;
    }
    pollfd waiting {socket_, POLLIN, 0};
    // Questions can wait a few milliseconds; the other threads' sockets
    // cannot wait on this one (see socket_wait.hpp).
    if (waitSockets(&waiting, 1, timeoutMs, 10) <= 0) {
        return;
    }
    uint8_t packet[1500];
    sockaddr_in from {};
    socklen_t length = sizeof(from);
    const ssize_t got = recvfrom(socket_, packet, sizeof(packet), MSG_DONTWAIT,
                                 reinterpret_cast<sockaddr *>(&from), &length);
    mdns::Asked asked;
    if (got <= 0 ||
        !mdns::readQuery(packet, static_cast<size_t>(got), hostname(), asked)) {
        return;
    }
    const unsigned short fromPort = ntohs(from.sin_port);
    const bool legacy = fromPort != mdns::Port;
    const std::vector<uint8_t> reply =
        mdns::answer(hostname(), address_, asked, legacy);
    if (legacy || asked.unicast || !multicast_) {
        send(reply.data(), reply.size(), from.sin_addr.s_addr, fromPort);
    } else {
        send(reply.data(), reply.size(), groupAddress(), mdns::Port);
    }
}

} // namespace net
