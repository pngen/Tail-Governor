#pragma once

#include <string>
#include <optional>
#include <vector>

#include "tailgovernor/identity.hpp"
#include "tailgovernor/authority.hpp"
#include "tailgovernor/duration.hpp"
#include "tailgovernor/cause.hpp"

namespace tailgovernor {

// Tail-specific intervention classes. Tail Governor authorizes tail-specific
// runtime actions; adjacent runtimes execute the mechanism.
enum class InterventionType {
    NO_ACTION,
    QUEUE_REPRIORITIZE,
    QUEUE_SHAPE,
    LIMIT_QUEUE_DEPTH,
    BOUND_QUEUE_WAIT,
    REDUCE_BATCH_SIZE,
    SPLIT_BATCH,
    DISABLE_BATCH_FOR_CLASS,
    WARM_RESIDENCY,
    PIN_RESIDENCY,
    PROMOTE_WARM_ENGINE,
    RESERVE_CAPACITY,
    RESERVE_BANDWIDTH,
    PREEMPT_LOWER_PRIORITY,
    DEFER_LOW_PRIORITY,
    SHED_LOAD,
    RECLAIM_MEMORY,
    CHANGE_PLACEMENT,
    ACCELERATE_RECOVERY,
    REPLAN_RECOVERY,
    FAILOVER,
    ESCALATE,
    MANUAL_INTERVENTION_REQUIRED
};

inline const char* to_string(InterventionType t) noexcept {
    switch (t) {
        case InterventionType::NO_ACTION: return "NO_ACTION";
        case InterventionType::QUEUE_REPRIORITIZE: return "QUEUE_REPRIORITIZE";
        case InterventionType::QUEUE_SHAPE: return "QUEUE_SHAPE";
        case InterventionType::LIMIT_QUEUE_DEPTH: return "LIMIT_QUEUE_DEPTH";
        case InterventionType::BOUND_QUEUE_WAIT: return "BOUND_QUEUE_WAIT";
        case InterventionType::REDUCE_BATCH_SIZE: return "REDUCE_BATCH_SIZE";
        case InterventionType::SPLIT_BATCH: return "SPLIT_BATCH";
        case InterventionType::DISABLE_BATCH_FOR_CLASS: return "DISABLE_BATCH_FOR_CLASS";
        case InterventionType::WARM_RESIDENCY: return "WARM_RESIDENCY";
        case InterventionType::PIN_RESIDENCY: return "PIN_RESIDENCY";
        case InterventionType::PROMOTE_WARM_ENGINE: return "PROMOTE_WARM_ENGINE";
        case InterventionType::RESERVE_CAPACITY: return "RESERVE_CAPACITY";
        case InterventionType::RESERVE_BANDWIDTH: return "RESERVE_BANDWIDTH";
        case InterventionType::PREEMPT_LOWER_PRIORITY: return "PREEMPT_LOWER_PRIORITY";
        case InterventionType::DEFER_LOW_PRIORITY: return "DEFER_LOW_PRIORITY";
        case InterventionType::SHED_LOAD: return "SHED_LOAD";
        case InterventionType::RECLAIM_MEMORY: return "RECLAIM_MEMORY";
        case InterventionType::CHANGE_PLACEMENT: return "CHANGE_PLACEMENT";
        case InterventionType::ACCELERATE_RECOVERY: return "ACCELERATE_RECOVERY";
        case InterventionType::REPLAN_RECOVERY: return "REPLAN_RECOVERY";
        case InterventionType::FAILOVER: return "FAILOVER";
        case InterventionType::ESCALATE: return "ESCALATE";
        case InterventionType::MANUAL_INTERVENTION_REQUIRED: return "MANUAL_INTERVENTION_REQUIRED";
    }
    return "UNKNOWN";
}

// Intervention lifecycle. ACKNOWLEDGED (mechanism accepted it) is deliberately
// distinct from EFFECTIVE (the tail measurably improved).
enum class InterventionStatus {
    PROPOSED,
    AUTHORIZED,
    DISPATCHED,
    ACKNOWLEDGED,
    EFFECTIVE,
    INEFFECTIVE,
    FAILED,
    CANCELLED,
    SUPERSEDED,
    EXPIRED,
    OUTCOME_UNKNOWN
};

inline const char* to_string(InterventionStatus s) noexcept {
    switch (s) {
        case InterventionStatus::PROPOSED: return "PROPOSED";
        case InterventionStatus::AUTHORIZED: return "AUTHORIZED";
        case InterventionStatus::DISPATCHED: return "DISPATCHED";
        case InterventionStatus::ACKNOWLEDGED: return "ACKNOWLEDGED";
        case InterventionStatus::EFFECTIVE: return "EFFECTIVE";
        case InterventionStatus::INEFFECTIVE: return "INEFFECTIVE";
        case InterventionStatus::FAILED: return "FAILED";
        case InterventionStatus::CANCELLED: return "CANCELLED";
        case InterventionStatus::SUPERSEDED: return "SUPERSEDED";
        case InterventionStatus::EXPIRED: return "EXPIRED";
        case InterventionStatus::OUTCOME_UNKNOWN: return "OUTCOME_UNKNOWN";
    }
    return "UNKNOWN";
}

// Named, structured feasibility rejection reasons.
enum class Infeasibility {
    NONE,
    NON_PREEMPTIBLE,
    ENGINE_UNAVAILABLE,
    CAPACITY_UNAVAILABLE,
    INCOMPATIBLE_TARGET,
    NO_FAILOVER_CANDIDATE,
    BELOW_MIN_BATCH,
    PROTECTED_TRAFFIC,
    VIOLATES_HARD_SLO,
    EXCEEDS_HARD_ECONOMY,
    STALE_AUTHORITY,
    SHUTTING_DOWN
};

inline const char* to_string(Infeasibility f) noexcept {
    switch (f) {
        case Infeasibility::NONE: return "NONE";
        case Infeasibility::NON_PREEMPTIBLE: return "NON_PREEMPTIBLE";
        case Infeasibility::ENGINE_UNAVAILABLE: return "ENGINE_UNAVAILABLE";
        case Infeasibility::CAPACITY_UNAVAILABLE: return "CAPACITY_UNAVAILABLE";
        case Infeasibility::INCOMPATIBLE_TARGET: return "INCOMPATIBLE_TARGET";
        case Infeasibility::NO_FAILOVER_CANDIDATE: return "NO_FAILOVER_CANDIDATE";
        case Infeasibility::BELOW_MIN_BATCH: return "BELOW_MIN_BATCH";
        case Infeasibility::PROTECTED_TRAFFIC: return "PROTECTED_TRAFFIC";
        case Infeasibility::VIOLATES_HARD_SLO: return "VIOLATES_HARD_SLO";
        case Infeasibility::EXCEEDS_HARD_ECONOMY: return "EXCEEDS_HARD_ECONOMY";
        case Infeasibility::STALE_AUTHORITY: return "STALE_AUTHORITY";
        case Infeasibility::SHUTTING_DOWN: return "SHUTTING_DOWN";
    }
    return "UNKNOWN";
}

// Structured economics. Hard constraints first, then deterministic ranking over
// these named factors — never an opaque score.
struct InterventionEconomics {
    double expected_p99_reduction = 0.0;    // ms-equivalent effect (reduction)
    double expected_p999_reduction = 0.0;
    double p50_p95_impact = 0.0;            // negative = worsening (ms)
    double throughput_impact = 0.0;         // negative = loss (req/s)
    double capacity_cost = 0.0;             // resources consumed (0..1)
    double memory_cost = 0.0;               // bytes
    double transfer_cost = 0.0;             // bytes
    double displaced_work = 0.0;            // displaced requests
    double fairness_impact = 0.0;           // negative = worsens fairness
    double recovery_risk = 0.0;             // 0..1
    double preemption_resume_cost = 0.0;    // ms
    double expected_effect_duration = 0.0;  // ns
    double uncertainty = 0.0;               // 0..1

    friend bool operator==(const InterventionEconomics&, const InterventionEconomics&) = default;
};

// A concrete tail intervention with its authority snapshot, expiry, and collateral.
struct Intervention {
    InterventionId id = InterventionId(0);
    InterventionGeneration generation = InterventionGeneration(0);
    InterventionType type = InterventionType::NO_ACTION;
    InterventionStatus status = InterventionStatus::PROPOSED;

    TailCauseCategory target_cause = TailCauseCategory::UNKNOWN;
    RequestClassId target_class = RequestClassId(0);

    // Authority this intervention was built against.
    AuthorityContext authority;

    TailObjectiveId objective = TailObjectiveId(0);
    TailObjectiveGeneration objective_generation = TailObjectiveGeneration(0);

    Duration estimated_tail_effect;      // expected p99/p999 reduction (ns)
    InterventionEconomics economics;
    Infeasibility feasibility = Infeasibility::NONE;
    double confidence = 0.0;

    std::int64_t proposed_at_ns = 0;
    std::int64_t expiry_at_ns = 0;       // 0 => never
    std::string reason;

    bool is_terminal() const noexcept {
        switch (status) {
            case InterventionStatus::EFFECTIVE:
            case InterventionStatus::INEFFECTIVE:
            case InterventionStatus::FAILED:
            case InterventionStatus::CANCELLED:
            case InterventionStatus::SUPERSEDED:
            case InterventionStatus::EXPIRED:
            case InterventionStatus::OUTCOME_UNKNOWN:
                return true;
            default:
                return false;
        }
    }
};

// Closed-loop post-action verification outcomes.
enum class PostOutcome {
    EFFECTIVE,
    PARTIALLY_EFFECTIVE,
    INEFFECTIVE,
    WORSENED_TAIL,
    CREATED_SECONDARY_VIOLATION,
    INSUFFICIENT_POST_ACTION_EVIDENCE,
    OUTCOME_UNKNOWN
};

inline const char* to_string(PostOutcome o) noexcept {
    switch (o) {
        case PostOutcome::EFFECTIVE: return "EFFECTIVE";
        case PostOutcome::PARTIALLY_EFFECTIVE: return "PARTIALLY_EFFECTIVE";
        case PostOutcome::INEFFECTIVE: return "INEFFECTIVE";
        case PostOutcome::WORSENED_TAIL: return "WORSENED_TAIL";
        case PostOutcome::CREATED_SECONDARY_VIOLATION: return "CREATED_SECONDARY_VIOLATION";
        case PostOutcome::INSUFFICIENT_POST_ACTION_EVIDENCE: return "INSUFFICIENT_POST_ACTION_EVIDENCE";
        case PostOutcome::OUTCOME_UNKNOWN: return "OUTCOME_UNKNOWN";
    }
    return "UNKNOWN";
}

} // namespace tailgovernor
