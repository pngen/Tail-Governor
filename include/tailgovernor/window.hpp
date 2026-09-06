#pragma once

#include <cstdint>
#include <algorithm>
#include <vector>
#include <optional>

#include "tailgovernor/duration.hpp"
#include "tailgovernor/quantile.hpp"
#include "tailgovernor/latency.hpp"
#include "tailgovernor/status.hpp"

namespace tailgovernor {

enum class WindowMode {
    ROLLING_TIME,     // sliding time window
    ROLLING_COUNT,    // rolling sample count (most recent N)
    TUMBLING,         // tumbling fixed interval
    FIXED_RECENT      // fixed-duration recent window with a count cap
};

inline const char* to_string(WindowMode m) noexcept {
    switch (m) {
        case WindowMode::ROLLING_TIME: return "ROLLING_TIME";
        case WindowMode::ROLLING_COUNT: return "ROLLING_COUNT";
        case WindowMode::TUMBLING: return "TUMBLING";
        case WindowMode::FIXED_RECENT: return "FIXED_RECENT";
    }
    return "UNKNOWN";
}

struct WindowConfig {
    WindowMode mode = WindowMode::ROLLING_TIME;
    Duration window_duration = Duration(5'000'000'000LL);  // 5s
    std::uint64_t fixed_count = 1000;       // ROLLING_COUNT / cap
    std::uint64_t max_samples = 1'000'000;  // hard bound on retained samples
};

// A single retained latency sample including the phase evidence needed for
// tail-cause attribution. The latency is the authoritative total.
struct TailSample {
    Duration latency;
    std::int64_t captured_ns = 0;   // source capture time (for pruning)
    std::int64_t arrival_ns = 0;
    std::vector<PhaseLatency> phases;
    RequestClassId request_class = RequestClassId(0);
    WorkerBootId worker_boot = WorkerBootId(0);
    CoordinatorEpoch epoch = CoordinatorEpoch(0);
    std::uint32_t retry_count = 0;
    Phase worst_phase = Phase::OTHER;

    // Dominant phase by duration (ties broken deterministically by Phase order).
    Phase dominant_phase() const {
        if (phases.empty()) return Phase::OTHER;
        Phase best = phases[0].phase;
        Duration best_d = phases[0].duration;
        for (std::size_t i = 1; i < phases.size(); ++i) {
            if (phases[i].duration > best_d) { best_d = phases[i].duration; best = phases[i].phase; }
        }
        return best;
    }
};

// Bounded observation window. Pruning and evaluation use an explicitly passed
// "now" (nanoseconds), so tests are deterministic and need no sleeps.
class ObservationWindow {
public:
    explicit ObservationWindow(WindowConfig cfg = {}) : cfg_(cfg) {}

    const WindowConfig& config() const { return cfg_; }
    void set_config(WindowConfig cfg) { cfg_ = cfg; }

    std::size_t size() const { return samples_.size(); }

    bool empty() const { return samples_.empty(); }

    // Insert a sample. Respects the hard bound (oldest evicted when full).
    // Returns a status for invalid input.
    Status insert(TailSample s) {
        if (s.latency.nanos() < 0)
            return Status(StatusCode::INVALID_INPUT, "negative latency sample");
        samples_.push_back(std::move(s));
        if (samples_.size() > cfg_.max_samples && cfg_.max_samples > 0) {
            // keep the most recent max_samples
            std::size_t excess = samples_.size() - cfg_.max_samples;
            samples_.erase(samples_.begin(), samples_.begin() + static_cast<std::ptrdiff_t>(excess));
        }
        dirty_ = true;
        return Status();
    }

    // Retreat expired samples per mode given 'now'. Returns the count removed.
    std::size_t prune(std::int64_t now) {
        if (samples_.empty()) return 0;
        std::size_t before = samples_.size();
        switch (cfg_.mode) {
            case WindowMode::ROLLING_TIME:
            case WindowMode::FIXED_RECENT: {
                std::int64_t cutoff = now - cfg_.window_duration.nanos();
                samples_.erase(
                    std::remove_if(samples_.begin(), samples_.end(),
                        [cutoff](const TailSample& s) { return s.captured_ns < cutoff; }),
                    samples_.end());
                if (cfg_.mode == WindowMode::FIXED_RECENT && samples_.size() > cfg_.fixed_count) {
                    std::size_t excess = samples_.size() - cfg_.fixed_count;
                    samples_.erase(samples_.begin(), samples_.begin() + static_cast<std::ptrdiff_t>(excess));
                }
                break;
            }
            case WindowMode::ROLLING_COUNT: {
                if (samples_.size() > cfg_.fixed_count) {
                    std::size_t excess = samples_.size() - cfg_.fixed_count;
                    samples_.erase(samples_.begin(), samples_.begin() + static_cast<std::ptrdiff_t>(excess));
                }
                break;
            }
            case WindowMode::TUMBLING: {
                // Retain samples since the last tumbling boundary.
                std::int64_t interval = cfg_.window_duration.nanos();
                if (interval <= 0) break;
                std::int64_t bucket = (now / interval) * interval;
                std::int64_t cutoff = bucket;
                samples_.erase(
                    std::remove_if(samples_.begin(), samples_.end(),
                        [cutoff](const TailSample& s) { return s.captured_ns < cutoff; }),
                    samples_.end());
                break;
            }
        }
        if (samples_.size() != before) dirty_ = true;
        return before - samples_.size();
    }

    // Clear all retained samples.
    void clear() { samples_.clear(); dirty_ = true; }

    // Access in insertion order.
    const std::vector<TailSample>& samples() const { return samples_; }
    const std::vector<TailSample>& samples_pruned(std::int64_t now) { prune(now); return samples_; }

    // Access the sorted latency values (lazy, recomputed only when dirty).
    const std::vector<Duration>& sorted(std::int64_t now) {
        prune(now);
        if (dirty_) rebuild_sorted();
        return sorted_;
    }

    // Most recent captured timestamp (0 if empty).
    std::int64_t latest_captured_ns() const {
        if (samples_.empty()) return 0;
        std::int64_t m = samples_[0].captured_ns;
        for (const auto& s : samples_) if (s.captured_ns > m) m = s.captured_ns;
        return m;
    }

    // latest sample arrival
    std::int64_t latest_arrival_ns() const {
        if (samples_.empty()) return 0;
        std::int64_t m = samples_[0].arrival_ns;
        for (const auto& s : samples_) if (s.arrival_ns > m) m = s.arrival_ns;
        return m;
    }

    // Approximate quantile via nearest-rank on the sorted values.
    Result<std::pair<Duration, Sufficiency>> percentile(Percentile p, std::uint64_t min_samples, std::int64_t now) {
        const std::vector<Duration>& vals = sorted(now);
        std::size_t n = vals.size();
        if (n < min_samples)
            return Result<std::pair<Duration, Sufficiency>>(
                std::make_pair(vals.empty() ? Duration(0) : vals.back(), Sufficiency::INSUFFICIENT_SAMPLES));
        auto q = exact_quantile(vals, p);
        if (!q) return Result<std::pair<Duration, Sufficiency>>::err(q.error().code, q.error().message);
        return Result<std::pair<Duration, Sufficiency>>(std::make_pair(*q, Sufficiency::SUFFICIENT));
    }

private:
    void rebuild_sorted() {
        sorted_.clear();
        sorted_.reserve(samples_.size());
        for (const auto& s : samples_) sorted_.push_back(s.latency);
        std::sort(sorted_.begin(), sorted_.end());
        dirty_ = false;
    }

    WindowConfig cfg_;
    std::vector<TailSample> samples_;
    std::vector<Duration> sorted_;
    bool dirty_ = false;
};

} // namespace tailgovernor
