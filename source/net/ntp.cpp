#include "ntp.hpp"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstring>

namespace ntp {
namespace {

// Seconds between the NTP epoch (1900) and the Unix epoch (1970).
constexpr uint64_t NtpToUnixSeconds = 2208988800ULL;
constexpr int MaxStrayPackets = 8;

constexpr uint8_t ModeClient = 3;
constexpr uint8_t ModeServer = 4;
constexpr uint8_t Version = 4;
constexpr uint8_t LeapUnsynchronised = 3;

void write64(uint8_t *data, uint64_t value)
{
    for (int index = 7; index >= 0; --index) {
        data[index] = static_cast<uint8_t>(value & 0xff);
        value >>= 8;
    }
}

uint64_t read64(const uint8_t *data)
{
    uint64_t value = 0;
    for (int index = 0; index < 8; ++index) {
        value = (value << 8) | data[index];
    }
    return value;
}

int64_t monotonicMilliseconds()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

class Socket {
public:
    explicit Socket(int descriptor) : descriptor_(descriptor) {}
    ~Socket()
    {
        if (descriptor_ >= 0) {
            close(descriptor_);
        }
    }
    Socket(const Socket &) = delete;
    Socket &operator=(const Socket &) = delete;
    int get() const { return descriptor_; }

private:
    int descriptor_;
};

bool resolve(const std::string &host, uint16_t port, sockaddr_in &address,
             std::string &error)
{
    addrinfo hints {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo *results = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &results) != 0 ||
        results == nullptr) {
        error = "Could not find " + host + ".";
        return false;
    }
    std::memcpy(&address, results->ai_addr, sizeof(address));
    address.sin_port = htons(port);
    freeaddrinfo(results);
    return true;
}

} // namespace

void buildRequest(uint8_t packet[PacketSize], uint64_t nonce)
{
    std::memset(packet, 0, PacketSize);
    packet[0] = static_cast<uint8_t>((Version << 3) | ModeClient);
    // SNTP clients may put any value in the transmit timestamp; the server
    // echoes it back as the originate timestamp.
    write64(packet + 40, nonce);
}

bool parseResponse(const uint8_t *packet, size_t size, uint64_t nonce,
                   int64_t &unixMilliseconds, std::string &error)
{
    if (size < PacketSize) {
        error = "The time server sent a short reply.";
        return false;
    }
    const uint8_t leap = packet[0] >> 6;
    const uint8_t mode = packet[0] & 0x7;
    const uint8_t stratum = packet[1];
    if (mode != ModeServer) {
        error = "The reply was not from a time server.";
        return false;
    }
    if (read64(packet + 24) != nonce) {
        error = "The reply did not match the request.";
        return false;
    }
    if (stratum == 0 || stratum > 15 || leap == LeapUnsynchronised) {
        error = "The time server is not synchronised.";
        return false;
    }

    const uint64_t transmit = read64(packet + 40);
    uint64_t seconds = transmit >> 32;
    if (seconds == 0) {
        error = "The time server sent no time.";
        return false;
    }
    // NTP seconds wrap in 2036; small values belong to the next era.
    if (seconds < 0x80000000ULL) {
        seconds += 0x100000000ULL;
    }
    const uint64_t fraction = transmit & 0xffffffffULL;
    unixMilliseconds =
        static_cast<int64_t>((seconds - NtpToUnixSeconds) * 1000 +
                             ((fraction * 1000) >> 32));
    return true;
}

Result query(const std::string &host, uint16_t port, int timeoutMilliseconds,
             const std::atomic<bool> *stop)
{
    Result result;
    result.server = host;

    sockaddr_in address {};
    if (!resolve(host, port, address, result.error)) {
        return result;
    }

    Socket socket(::socket(AF_INET, SOCK_DGRAM, 0));
    if (socket.get() < 0) {
        result.error = "Could not open a network socket.";
        return result;
    }

    const int64_t started = monotonicMilliseconds();
    const uint64_t nonce =
        (static_cast<uint64_t>(started) << 20) ^
        reinterpret_cast<uintptr_t>(&address) ^ 0x3d5d1ULL;
    uint8_t request[PacketSize];
    buildRequest(request, nonce);
    if (sendto(socket.get(), request, sizeof(request), 0,
               reinterpret_cast<const sockaddr *>(&address),
               sizeof(address)) != static_cast<ssize_t>(sizeof(request))) {
        result.error = "Could not reach " + host + ".";
        return result;
    }

    for (int attempt = 0; attempt < MaxStrayPackets;) {
        const int64_t elapsed = monotonicMilliseconds() - started;
        const int remaining = static_cast<int>(timeoutMilliseconds - elapsed);
        if (remaining <= 0 || (stop != nullptr && stop->load())) {
            break;
        }
        // In short waits, so stop is noticed.
        pollfd waiting {socket.get(), POLLIN, 0};
        const int ready = poll(&waiting, 1, std::min(remaining, 100));
        if (ready < 0) {
            break;
        }
        if (ready == 0) {
            continue;
        }
        ++attempt;

        uint8_t reply[PacketSize * 2];
        const ssize_t received =
            recvfrom(socket.get(), reply, sizeof(reply), 0, nullptr, nullptr);
        if (received < 0) {
            result.error = "Could not read the reply from " + host + ".";
            return result;
        }
        int64_t serverTime = 0;
        if (parseResponse(reply, static_cast<size_t>(received), nonce,
                          serverTime, result.error)) {
            // The reply took about half the round trip to arrive.
            const int64_t roundTrip = monotonicMilliseconds() - started;
            result.unixMilliseconds = serverTime + roundTrip / 2;
            result.ok = true;
            result.error.clear();
            return result;
        }
    }

    if (result.error.empty()) {
        result.error = host + " did not answer.";
    }
    return result;
}

Result queryAny(const std::vector<std::string> &hosts, uint16_t port,
                int timeoutMilliseconds, const std::atomic<bool> *stop)
{
    Result last;
    last.error = "No time servers to ask.";
    for (const std::string &host : hosts) {
        if (stop != nullptr && stop->load()) {
            break;
        }
        last = query(host, port, timeoutMilliseconds, stop);
        if (last.ok) {
            break;
        }
    }
    return last;
}

const std::vector<std::string> &defaultServers()
{
    static const std::vector<std::string> servers = {
        "time.cloudflare.com", "pool.ntp.org", "time.google.com"};
    return servers;
}

} // namespace ntp
