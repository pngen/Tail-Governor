#pragma once

#include <cmath>
#include <cstdint>
#include <algorithm>
#include <vector>
#include <limits>

#include "tailgovernor/duration.hpp"
#include "tailgovernor/status.hpp"

namespace tailgovernor {

// Percentile value: a number strictly in (0, 100]. p95=95.0, p99=99.0, p999=99.9.
// Represented as a double only for the percentile spec; latency values remain
// integer durations.
class Percentile {
public:
    constexpr Percentile() noexcept : p_(99.0) {}
    constexpr explicit Percentile(double p) noexcept : p_(p) {}

    static Result<Percentile> validate(double p) noexcept {
        if (!std::isfinite(p) || p <= 0.0 || p > 100.0)
            return Result<Percentile>::err(StatusCode::INVALID_INPUT, "invalid percentile");
        return Result<Percentile>(Percentile(p));
    }

    constexpr double value() const noexcept { return p_; }
    constexpr double as_fraction() const noexcept { return p_ / 100.0; }

    constexpr friend bool operator==(Percentile a, Percentile b) noexcept = default;
    constexpr friend auto operator<=>(Percentile a, Percentile b) noexcept = default;

private:
    double p_;
};

// Exact quantile definition (documented and immutable): nearest-rank.
// For N sorted-ascending samples and percentile p in (0,100], rank =
// ceil(p/100 * N), clamped to [1,N]; the result is samples[rank-1] (1-based).
// This yields the mathematically deterministic property that p95 <= p99 <= p999
// for the same sorted sample set (ceil is monotone non-decreasing).
Result<Duration> exact_quantile(const std::vector<Duration>& sorted_ascending, Percentile p) noexcept;

// Derived minimum sample count for a percentile. For p999 (99.9) this is 1000,
// p99 is 100, p95 is 20 — enough that the top tail bucket is populated.
// Bounded so it can never exceed a sane max.
std::uint64_t derived_min_samples(Percentile p) noexcept;

constexpr std::uint64_t kMinSampleCap = 10'000'000ULL;

// Sample sufficiency verdict for a single percentile evaluation.
enum class Sufficiency {
    SUFFICIENT,
    INSUFFICIENT_SAMPLES,
    INSUFFICIENT_WINDOW,
    STALE,
    UNKNOWN
};

inline const char* to_string(Sufficiency s) noexcept {
    switch (s) {
        case Sufficiency::SUFFICIENT: return "SUFFICIENT";
        case Sufficiency::INSUFFICIENT_SAMPLES: return "INSUFFICIENT_SAMPLES";
        case Sufficiency::INSUFFICIENT_WINDOW: return "INSUFFICIENT_WINDOW";
        case Sufficiency::STALE: return "STALE";
        case Sufficiency::UNKNOWN: return "UNKNOWN";
    }
    return "UNKNOWN";
}

inline Result<Duration> exact_quantile(const std::vector<Duration>& sorted_ascending, Percentile p) noexcept {
    if (sorted_ascending.empty())
        return Result<Duration>::err(StatusCode::INSUFFICIENT_EVIDENCE, "no samples");
    std::uint64_t n = sorted_ascending.size();
    double rank_d = std::ceil(p.as_fraction() * static_cast<double>(n));
    std::uint64_t rank = (rank_d < 1.0) ? 1ULL : static_cast<std::uint64_t>(rank_d);
    if (rank > n) rank = n;
    return Result<Duration>(sorted_ascending[rank - 1]);
}

inline std::uint64_t derived_min_samples(Percentile p) noexcept {
    double denom = 100.0 - p.value();
    if (denom <= 0.0) return 1ULL;  // p == 100: the max; just needs samples
    double v = 100.0 / denom;
    // tolerate float rounding so p999 (99.9) yields exactly 1000, p99 -> 100, p95 -> 20
    double r = std::ceil(v - 1e-7);
    if (!(r >= 1.0)) r = 1.0;
    std::uint64_t out = static_cast<std::uint64_t>(r);
    if (out > kMinSampleCap) out = kMinSampleCap;
    return out;
}

} // namespace tailgovernor
