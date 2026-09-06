#pragma once

#include <string>
#include <vector>
#include <optional>

#include "tailgovernor/identity.hpp"
#include "tailgovernor/duration.hpp"
#include "tailgovernor/quantile.hpp"
#include "tailgovernor/tail_state.hpp"
#include "tailgovernor/cause.hpp"
#include "tailgovernor/intervention.hpp"

namespace tailgovernor {

// Structured triggers that, if they occurred, would change the current decision.
enum class WhatWouldChange {
    P99_BELOW_RECOVERY,
    MORE_CAPACITY,
    QUEUE_DEPTH_DROPS,
    BATCH_WAIT_DISAPPEARS,
    WARM_ENGINE_READY,
    TRANSFER_PATH_CLEARS,
    MEMORY_PRESSURE_FALLS,
    HIGHER_PRIORITY_CLASS_ARRIVES,
    PREEMPTION_SAFE,
    TAIL_SUFFICIENCY_IMPROVES,
    P999_EVIDENCE_FRESH,
    HARD_SLO_BLOCKS,
    COST_CONSTRAINT_RELAXES
};

inline const char* to_string(WhatWouldChange w) noexcept {
    switch (w) {
        case WhatWouldChange::P99_BELOW_RECOVERY: return "P99_BELOW_RECOVERY";
        case WhatWouldChange::MORE_CAPACITY: return "MORE_CAPACITY";
        case WhatWouldChange::QUEUE_DEPTH_DROPS: return "QUEUE_DEPTH_DROPS";
        case WhatWouldChange::BATCH_WAIT_DISAPPEARS: return "BATCH_WAIT_DISAPPEARS";
        case WhatWouldChange::WARM_ENGINE_READY: return "WARM_ENGINE_READY";
        case WhatWouldChange::TRANSFER_PATH_CLEARS: return "TRANSFER_PATH_CLEARS";
        case WhatWouldChange::MEMORY_PRESSURE_FALLS: return "MEMORY_PRESSURE_FALLS";
        case WhatWouldChange::HIGHER_PRIORITY_CLASS_ARRIVES: return "HIGHER_PRIORITY_CLASS_ARRIVES";
        case WhatWouldChange::PREEMPTION_SAFE: return "PREEMPTION_SAFE";
        case WhatWouldChange::TAIL_SUFFICIENCY_IMPROVES: return "TAIL_SUFFICIENCY_IMPROVES";
        case WhatWouldChange::P999_EVIDENCE_FRESH: return "P999_EVIDENCE_FRESH";
        case WhatWouldChange::HARD_SLO_BLOCKS: return "HARD_SLO_BLOCKS";
        case WhatWouldChange::COST_CONSTRAINT_RELAXES: return "COST_CONSTRAINT_RELAXES";
    }
    return "UNKNOWN";
}

// A rejected intervention alternative with its structured rejection reason.
struct RejectedAlternative {
    InterventionType type = InterventionType::NO_ACTION;
    Infeasibility reason = Infeasibility::NONE;
    std::string detail;
};

// The structured what-would-change record.
struct WhatWouldChangeRecord {
    WhatWouldChange trigger;
    std::string note;
};

// Per-phase decomposition of the tail.
struct PhaseContribution {
    Phase phase;
    Duration total;        // summed across tail samples
    double share = 0.0;    // share of total in [0,1]
    bool dominant = false;
};

// The authoritative tail evaluation for one objective scope.
struct TailEvaluation {
    EvaluationId id = EvaluationId(0);
    EvaluationGeneration generation = EvaluationGeneration(0);

    TailObjectiveId objective = TailObjectiveId(0);
    TailObjectiveGeneration objective_generation = TailObjectiveGeneration(0);
    Percentile percentile{99.0};

    TailState state = TailState::UNKNOWN;
    Sufficiency sufficiency = Sufficiency::UNKNOWN;

    Duration target;
    Duration recovered_threshold;
    Duration observed;                 // actual metric value
    std::optional<Duration> p50;
    std::optional<Duration> p95;
    std::optional<Duration> p99_for_p95;  // present when the objective is p95
    std::optional<Duration> p999;

    std::uint64_t sample_count = 0;
    std::uint64_t min_required = 0;
    std::int64_t window_begin_ns = 0;
    std::int64_t window_end_ns = 0;
    std::int64_t latest_capture_ns = 0;
    double freshness_age_ms = 0.0;

    Amplification amplification;
    std::vector<TailCause> causes;
    std::vector<PhaseContribution> phase_contributions;

    InterventionType selected_intervention = InterventionType::NO_ACTION;
    InterventionId selected_intervention_id = InterventionId(0);
    std::vector<RejectedAlternative> rejected_alternatives;

    std::optional<Infeasibility> binding_hard_constraint;
    Duration expected_tail_reduction;
    Duration expected_collateral;
    double uncertainty = 0.0;
    std::string uncertainty_reason;

    std::vector<WhatWouldChangeRecord> what_would_change;
    AuthorityContext authority;

    bool revalidation_required = false;
};

} // namespace tailgovernor
