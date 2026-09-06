#pragma once

#include <string>
#include <vector>

#include "tailgovernor/latency.hpp"
#include "tailgovernor/duration.hpp"

namespace tailgovernor {

// Typed tail cause categories. A result can have a primary cause, secondary
// causes, and explicit provenance. When evidence is ambiguous the cause remains
// UNKNOWN; we never force a single cause.
enum class TailCauseCategory {
    QUEUEING,
    BATCHING,
    COLD_RESIDENCY,
    MODEL_OR_STATE_LOAD,
    TRANSFER,
    MEMORY_PRESSURE,
    RESOURCE_CONTENTION,
    EXECUTION_VARIANCE,
    PREEMPTION,
    RECOVERY,
    RETRY,
    BACKEND_STALL,
    PUBLICATION_DELAY,
    CAPACITY_SHORTAGE,
    PRIORITY_INVERSION,
    UNKNOWN
};

inline const char* to_string(TailCauseCategory c) noexcept {
    switch (c) {
        case TailCauseCategory::QUEUEING: return "QUEUEING";
        case TailCauseCategory::BATCHING: return "BATCHING";
        case TailCauseCategory::COLD_RESIDENCY: return "COLD_RESIDENCY";
        case TailCauseCategory::MODEL_OR_STATE_LOAD: return "MODEL_OR_STATE_LOAD";
        case TailCauseCategory::TRANSFER: return "TRANSFER";
        case TailCauseCategory::MEMORY_PRESSURE: return "MEMORY_PRESSURE";
        case TailCauseCategory::RESOURCE_CONTENTION: return "RESOURCE_CONTENTION";
        case TailCauseCategory::EXECUTION_VARIANCE: return "EXECUTION_VARIANCE";
        case TailCauseCategory::PREEMPTION: return "PREEMPTION";
        case TailCauseCategory::RECOVERY: return "RECOVERY";
        case TailCauseCategory::RETRY: return "RETRY";
        case TailCauseCategory::BACKEND_STALL: return "BACKEND_STALL";
        case TailCauseCategory::PUBLICATION_DELAY: return "PUBLICATION_DELAY";
        case TailCauseCategory::CAPACITY_SHORTAGE: return "CAPACITY_SHORTAGE";
        case TailCauseCategory::PRIORITY_INVERSION: return "PRIORITY_INVERSION";
        case TailCauseCategory::UNKNOWN: return "UNKNOWN";
    }
    return "UNKNOWN";
}

// Latency phases map to tail cause categories where the mapping is principled.
inline TailCauseCategory phase_to_cause(Phase p) noexcept {
    switch (p) {
        case Phase::ADMISSION_WAIT:
        case Phase::QUEUE_WAIT: return TailCauseCategory::QUEUEING;
        case Phase::BATCH_WAIT: return TailCauseCategory::BATCHING;
        case Phase::MODEL_LOAD:
        case Phase::ENGINE_WARMUP: return TailCauseCategory::COLD_RESIDENCY;
        case Phase::STATE_FETCH: return TailCauseCategory::MODEL_OR_STATE_LOAD;
        case Phase::TRANSFER: return TailCauseCategory::TRANSFER;
        case Phase::MEMORY_RECLAIM: return TailCauseCategory::MEMORY_PRESSURE;
        case Phase::EXECUTION:
        case Phase::PREFILL:
        case Phase::DECODE:
        case Phase::BACKEND_WAIT: return TailCauseCategory::EXECUTION_VARIANCE;
        case Phase::PREEMPTION_WAIT: return TailCauseCategory::PREEMPTION;
        case Phase::RECOVERY_WAIT: return TailCauseCategory::RECOVERY;
        case Phase::RETRY_DELAY: return TailCauseCategory::RETRY;
        case Phase::PUBLICATION: return TailCauseCategory::PUBLICATION_DELAY;
        case Phase::OTHER: return TailCauseCategory::UNKNOWN;
    }
    return TailCauseCategory::UNKNOWN;
}

// Attribution strength. We never claim causal proof from correlation alone.
enum class Attribution {
    DOMINANT_CONTRIBUTOR,
    ASSOCIATED_CONTRIBUTOR,
    UNKNOWN_CAUSE
};

inline const char* to_string(Attribution a) noexcept {
    switch (a) {
        case Attribution::DOMINANT_CONTRIBUTOR: return "DOMINANT_CONTRIBUTOR";
        case Attribution::ASSOCIATED_CONTRIBUTOR: return "ASSOCIATED_CONTRIBUTOR";
        case Attribution::UNKNOWN_CAUSE: return "UNKNOWN_CAUSE";
    }
    return "UNKNOWN_CAUSE";
}

struct TailCause {
    TailCauseCategory category = TailCauseCategory::UNKNOWN;
    Attribution attribution = Attribution::UNKNOWN_CAUSE;
    double confidence = 0.0;     // in [0,1]
    Duration excess = Duration(0);  // above the binding target
    Duration share = Duration(0);   // dominant phase duration
    std::string evidence;         // short human summary, never used for decisions
};

} // namespace tailgovernor
