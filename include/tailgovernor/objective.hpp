#pragma once

#include <cstdint>
#include <cstddef>
#include <optional>
#include <string>

#include "tailgovernor/identity.hpp"
#include "tailgovernor/duration.hpp"
#include "tailgovernor/quantile.hpp"
#include "tailgovernor/window.hpp"

namespace tailgovernor {

// A single p95/p99/p999 tail-latency objective.
struct TailObjective {
    TailObjectiveId id = TailObjectiveId(0);
    TailObjectiveGeneration generation = TailObjectiveGeneration(0);
    Percentile percentile{99.0};          // which tail metric this governs
    Duration target;                      // SLO target duration
    WindowConfig window;                  // evaluation window
    std::uint64_t min_samples = 0;        // 0 => derived from percentile
    Duration freshness;                   // max age of latest evidence before STALE
    bool hard = true;                     // hard SLO (constraint) vs soft advisory
    double recovery_fraction = 0.90;      // e.g. <90% of target is "recovered"
    Duration cool_down;                   // min time between interventions for this objective
    std::optional<RequestClassId> class_scope;  // empty => all classes
    int priority = 0;                     // higher wins as binding primary
    bool enabled = true;

    std::uint64_t effective_min_samples() const {
        return min_samples > 0 ? min_samples : derived_min_samples(percentile);
    }

    Duration violation_threshold() const { return target; }
    Duration recovery_threshold() const {
        if (recovery_fraction <= 0.0) return target;
        std::int64_t r = static_cast<std::int64_t>(static_cast<double>(target.nanos()) * recovery_fraction);
        return Duration(r < 0 ? 0 : r);
    }
};

// A tail policy bundles objectives sharing a policy identity/generation.
struct TailPolicy {
    TailPolicyId id = TailPolicyId(0);
    TailPolicyGeneration generation = TailPolicyGeneration(0);
    std::vector<TailObjective> objectives;
    std::string name;
};

} // namespace tailgovernor
