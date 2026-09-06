#pragma once

#include <cstdint>
#include <limits>
#include <string>
#include <compare>
#include <stdexcept>

#include "tailgovernor/status.hpp"

namespace tailgovernor {

// A strongly typed exact duration, always in integer nanoseconds.
// Negative durations and overflow are rejected at the boundary.
class Duration {
public:
    using value_type = std::int64_t;

    constexpr Duration() noexcept = default;
    constexpr explicit Duration(value_type nanos) noexcept : ns_(nanos) {}

    // Construct from a validated non-negative nanosecond count.
    static Result<Duration> from_nanos(value_type nanos) noexcept {
        if (nanos < 0) return Result<Duration>::err(StatusCode::INVALID_INPUT, "negative duration");
        return Result<Duration>(Duration(nanos));
    }

    static constexpr Duration zero() noexcept { return Duration(0); }

    constexpr value_type nanos() const noexcept { return ns_; }
    constexpr bool is_zero() const noexcept { return ns_ == 0; }

    Duration operator+(Duration o) const noexcept {
        auto r = ns_ + o.ns_;
        // saturate on overflow rather than UB; but boundaries should have rejected
        return Duration(r < ns_ ? std::numeric_limits<value_type>::max() : r);
    }
    Duration operator-(Duration o) const noexcept {
        auto r = ns_ - o.ns_;
        return Duration(r > ns_ ? std::numeric_limits<value_type>::min() : r);
    }

    constexpr friend bool operator==(Duration a, Duration b) noexcept = default;
    constexpr friend auto operator<=>(Duration a, Duration b) noexcept = default;

    constexpr double as_millis() const noexcept { return static_cast<double>(ns_) / 1'000'000.0; }
    constexpr double as_micros() const noexcept { return static_cast<double>(ns_) / 1'000.0; }
    constexpr double as_seconds() const noexcept { return static_cast<double>(ns_) / 1'000'000'000.0; }

    // Deterministic textual form in nanoseconds.
    std::string to_string() const;

    // Deterministic canonical big-endian fixed-width encoding (8 bytes).
    void write_canonical(std::uint8_t out[8]) const noexcept {
        std::uint64_t bits = static_cast<std::uint64_t>(ns_);
        for (int i = 7; i >= 0; --i) { out[i] = static_cast<std::uint8_t>(bits & 0xFFu); bits >>= 8; }
    }

private:
    value_type ns_ = 0;
};

inline std::string Duration::to_string() const {
    std::string s = std::to_string(ns_);
    s += "ns";
    return s;
}

} // namespace tailgovernor
