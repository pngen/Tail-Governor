#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <optional>

#include "tailgovernor/identity.hpp"
#include "tailgovernor/duration.hpp"
#include "tailgovernor/status.hpp"

namespace tailgovernor {

// Latency phases. Every workload need not use every phase; evidence is optional
// per phase. Unknown phases are never invented.
enum class Phase {
    ADMISSION_WAIT,
    QUEUE_WAIT,
    BATCH_WAIT,
    PREFILL,
    DECODE,
    EXECUTION,
    TRANSFER,
    STATE_FETCH,
    MODEL_LOAD,
    ENGINE_WARMUP,
    MEMORY_RECLAIM,
    PREEMPTION_WAIT,
    RECOVERY_WAIT,
    RETRY_DELAY,
    BACKEND_WAIT,
    PUBLICATION,
    OTHER
};

const char* to_string(Phase p) noexcept;

// The authoritative final status of a completed request. Only SUCCESS with an
// authoritative publication is counted as valid tail latency evidence by
// default; ambiguous or cancelled work is not quietly counted.
enum class RequestStatus {
    SUCCESS,
    FAILED,
    TIMEOUT,
    CANCELLED,
    AMBIGUOUS
};

const char* to_string(RequestStatus s) noexcept;

// Per-phase duration evidence for a single completed request.
struct PhaseLatency {
    Phase phase = Phase::OTHER;
    Duration duration;
};

// Completed-authoritative-work latency evidence. This is the unit of evidence
// that can ever become a tail sample. Submission-only measurements are not
// expressed here.
struct RequestLatency {
    RequestId request_id;
    RequestClassId request_class = RequestClassId(0);
    ServiceId service = ServiceId(0);
    WorkloadId workload = WorkloadId(0);
    AttemptId attempt = AttemptId(0);

    RequestGeneration request_generation = RequestGeneration(0);
    WorkloadGeneration workload_generation = WorkloadGeneration(0);
    ServiceGeneration service_generation = ServiceGeneration(0);
    AttemptGeneration attempt_generation = AttemptGeneration(0);

    // Authoritative lifecycle timestamps (ns). Ordering: arrival <= admission
    // <= dispatch <= execution_start <= completion <= publication.
    std::int64_t arrival_ns = 0;
    std::int64_t admission_ns = 0;
    std::int64_t dispatch_ns = 0;
    std::int64_t execution_start_ns = 0;
    std::int64_t completion_ns = 0;
    std::int64_t publication_ns = 0;

    // Total user-visible latency (computed/validated against publication-arrival).
    Duration total_latency;

    // Optional per-phase evidence. Sum of phases must reconcile with total.
    std::vector<PhaseLatency> phases;

    RequestStatus status = RequestStatus::SUCCESS;
    std::uint32_t retry_count = 0;

    WorkerId worker = WorkerId(0);
    WorkerBootId worker_boot = WorkerBootId(0);
    CoordinatorEpoch epoch = CoordinatorEpoch(0);
    EvidenceId evidence_id = EvidenceId(0);
    EvidenceGeneration evidence_generation = EvidenceGeneration(0);
    std::int64_t captured_ns = 0;  // when this evidence was captured by the source

    bool has_publication() const noexcept { return publication_ns > 0; }

    // Only authoritative successful completions are valid tail evidence.
    bool is_authoritative_completion() const noexcept {
        return status == RequestStatus::SUCCESS && has_publication() && publication_ns >= completion_ns;
    }

    // Reconcile total latency from authoritative timestamps if not provided.
    Result<Duration> resolved_total() const {
        if (has_publication() && publication_ns >= arrival_ns) {
            auto d = Duration::from_nanos(publication_ns - arrival_ns);
            if (!d) return Result<Duration>::err(d.error().code, d.error().message);
            return d;
        }
        if (completion_ns >= arrival_ns) {
            auto d = Duration::from_nanos(completion_ns - arrival_ns);
            if (!d) return Result<Duration>::err(d.error().code, d.error().message);
            return d;
        }
        return Result<Duration>::err(StatusCode::INVALID_INPUT, "cannot reconcile request latency");
    }
};

inline const char* to_string(Phase p) noexcept {
    switch (p) {
        case Phase::ADMISSION_WAIT: return "ADMISSION_WAIT";
        case Phase::QUEUE_WAIT: return "QUEUE_WAIT";
        case Phase::BATCH_WAIT: return "BATCH_WAIT";
        case Phase::PREFILL: return "PREFILL";
        case Phase::DECODE: return "DECODE";
        case Phase::EXECUTION: return "EXECUTION";
        case Phase::TRANSFER: return "TRANSFER";
        case Phase::STATE_FETCH: return "STATE_FETCH";
        case Phase::MODEL_LOAD: return "MODEL_LOAD";
        case Phase::ENGINE_WARMUP: return "ENGINE_WARMUP";
        case Phase::MEMORY_RECLAIM: return "MEMORY_RECLAIM";
        case Phase::PREEMPTION_WAIT: return "PREEMPTION_WAIT";
        case Phase::RECOVERY_WAIT: return "RECOVERY_WAIT";
        case Phase::RETRY_DELAY: return "RETRY_DELAY";
        case Phase::BACKEND_WAIT: return "BACKEND_WAIT";
        case Phase::PUBLICATION: return "PUBLICATION";
        case Phase::OTHER: return "OTHER";
    }
    return "OTHER";
}

inline const char* to_string(RequestStatus s) noexcept {
    switch (s) {
        case RequestStatus::SUCCESS: return "SUCCESS";
        case RequestStatus::FAILED: return "FAILED";
        case RequestStatus::TIMEOUT: return "TIMEOUT";
        case RequestStatus::CANCELLED: return "CANCELLED";
        case RequestStatus::AMBIGUOUS: return "AMBIGUOUS";
    }
    return "UNKNOWN";
}

} // namespace tailgovernor
