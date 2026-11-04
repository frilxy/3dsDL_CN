#include "../check.hpp"

#include "ntp.hpp"

#include <chrono>
#include <cstdlib>

namespace {

uint16_t port(const char *name)
{
    const char *value = std::getenv(name);
    return static_cast<uint16_t>(value != nullptr ? std::atoi(value) : 0);
}

// The fixture servers answer with 2026-10-12 06:00:00.5 UTC.
constexpr int64_t FixedMs = 1791784800LL * 1000 + 500;

} // namespace

TEST("ntp: reads the time from a healthy server")
{
    const ntp::Result result = ntp::query("127.0.0.1", port("NTP_GOOD"), 2000);
    CHECK(result.ok);
    CHECK(result.unixMilliseconds >= FixedMs);
    CHECK(result.unixMilliseconds < FixedMs + 2000);
    CHECK(result.error.empty());
}

TEST("ntp: rejects unsynchronised and mismatched replies")
{
    ntp::Result result = ntp::query("127.0.0.1", port("NTP_UNSYNCED"), 1000);
    CHECK(!result.ok);
    CHECK(result.error.find("not synchronised") != std::string::npos);

    result = ntp::query("127.0.0.1", port("NTP_MISMATCHED"), 1000);
    CHECK(!result.ok);
    CHECK(result.error.find("did not match") != std::string::npos);
}

TEST("ntp: gives up on silent servers and tries the next one")
{
    const auto started = std::chrono::steady_clock::now();
    // Port 9 (discard) has no listener on 127.0.0.1 in the test setup.
    ntp::Result result = ntp::query("127.0.0.1", 9, 800);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    CHECK(!result.ok);
    CHECK(elapsed < std::chrono::seconds(3));

    result = ntp::queryAny({"no-such-host.invalid", "127.0.0.1"},
                           port("NTP_GOOD"), 1500);
    CHECK(result.ok);
    CHECK_EQ(result.server, std::string("127.0.0.1"));
}
