#pragma once

#include <string>
#include <cstdint>
#include <optional>

#include "tailgovernor/duration.hpp"
#include "tailgovernor/quantile.hpp"

namespace tailgovernor {

enum class TailState {
    HEALTHY,
    ELEVATED,
    AT_RISK,
    VIOLATING,
    SEVERE,
    RECOVERING,
    INSUFFICIENT_EVIDENCE,
    REVALIDATION_REQUIRED,
    STALE,
    UNKNOWN
};

inline const char* to_string(TailState s) noexcept {
    switch (s) {
        case TailState::HEALTHY: return "HEALTHY";
        case TailState::ELEVATED: return "ELEVATED";
        case TailState::AT_RISK: return "AT_RISK";
        case TailState::VIOLATING: return "VIOLATING";
        case TailState::SEVERE: return "SEVERE";
        case TailState::RECOVERING: return "RECOVERING";
        case TailState::INSUFFICIENT_EVIDENCE: return "INSUFFICIENT_EVIDENCE";
        case TailState::REVALIDATION_REQUIRED: return "REVALIDATION_REQUIRED";
        case TailState::STALE: return "STALE";
        case TailState::UNKNOWN: return "UNKNOWN";
    }
    return "UNKNOWN";
}

// Tail amplification ratios. Only ratios with a valid, non-zero denominator are
// produced. Amplification is signal, not automatic proof of violation.
struct Amplification {
    std::optional<double> p99_over_p50;
    std::optional<double> p999_over_p50;
    std::optional<double> p99_over_mean;
    std::optional<double> p999_over_p95;
};

inline Amplification compute_amplification(
        std::optional<Duration> p50, std::optional<Duration> p95,
        std::optional<Duration> p99, std::optional<Duration> p999,
        std::optional<Duration> mean) {
    Amplification a;
    if (p50 && !p50->is_zero()) {
        if (p99) a.p99_over_p50 = static_cast<double>(p99->nanos()) / static_cast<double>(p50->nanos());
        if (p999) a.p999_over_p50 = static_cast<double>(p999->nanos()) / static_cast<double>(p50->nanos());
    }
    if (mean && !mean->is_zero() && p99) a.p99_over_mean = static_cast<double>(p99->nanos()) / static_cast<double>(mean->nanos());
    if (p95 && !p95->is_zero() && p999) a.p999_over_p95 = static_cast<double>(p999->nanos()) / static_cast<double>(p95->nanos());
    return a;
}

} // namespace tailgovernor
