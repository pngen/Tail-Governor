#pragma once
#include "tailgovernor/tailgovernor.hpp"
#include <memory>
#include <vector>

using namespace tailgovernor;

inline GovernorConfig mk_config(std::int64_t base_ns, CoordinatorEpoch epoch = CoordinatorEpoch(1)) {
    GovernorConfig c;
    c.clock = std::make_shared<TestClock>(base_ns);
    c.epoch = epoch;
    c.service = ServiceId(1);
    c.service_generation = ServiceGeneration(1);
    c.workload = WorkloadId(1);
    c.workload_generation = WorkloadGeneration(1);
    return c;
}

inline TailObjective mk_p99_objective(std::int64_t target_ns, TailObjectiveId id = TailObjectiveId(1)) {
    TailObjective o;
    o.id = id;
    o.generation = TailObjectiveGeneration(1);
    o.percentile = Percentile(99.0);
    o.target = Duration(target_ns);
    o.window.window_duration = Duration(5'000'000'000LL);
    o.window.max_samples = 1000000;
    o.min_samples = 0;
    o.freshness = Duration(2'000'000'000LL);
    o.hard = true;
    o.recovery_fraction = 0.90;
    o.cool_down = Duration(0);
    o.priority = 0;
    o.enabled = true;
    return o;
}

inline TailObjective mk_p95_objective(std::int64_t target_ns, TailObjectiveId id = TailObjectiveId(2)) {
    auto o = mk_p99_objective(target_ns, id);
    o.percentile = Percentile(95.0);
    return o;
}

inline TailObjective mk_p999_objective(std::int64_t target_ns, TailObjectiveId id = TailObjectiveId(3)) {
    auto o = mk_p99_objective(target_ns, id);
    o.percentile = Percentile(99.9);
    return o;
}

inline TailPolicy mk_policy(std::vector<TailObjective> objs, TailPolicyId pid = TailPolicyId(1), TailPolicyGeneration g = TailPolicyGeneration(1)) {
    TailPolicy p;
    p.id = pid;
    p.generation = g;
    p.name = "test policy";
    p.objectives = std::move(objs);
    return p;
}

inline RequestLatency mk_req(RequestId id, std::int64_t arrival, std::int64_t pub,
        RequestClassId cls = RequestClassId(0), WorkerId wk = WorkerId(1),
        WorkerBootId boot = WorkerBootId(1), CoordinatorEpoch epoch = CoordinatorEpoch(1),
        std::uint32_t retry = 0, std::vector<PhaseLatency> phases = {},
        RequestStatus status = RequestStatus::SUCCESS) {
    RequestLatency r;
    r.request_id = id;
    r.request_class = cls;
    r.worker = wk;
    r.worker_boot = boot;
    r.epoch = epoch;
    r.arrival_ns = arrival;
    r.completion_ns = pub;
    r.publication_ns = pub;
    r.captured_ns = pub;
    r.status = status;
    r.retry_count = retry;
    r.phases = std::move(phases);
    r.total_latency = Duration(pub - arrival);
    r.evidence_id = EvidenceId(id.value());
    r.evidence_generation = EvidenceGeneration(1);
    r.service_generation = ServiceGeneration(1);
    r.workload_generation = WorkloadGeneration(1);
    return r;
}

inline void feed(TailGovernor& g, WorkerId wk, WorkerBootId boot, std::int64_t base_arrival,
        RequestClassId cls, std::uint64_t n, const std::vector<std::int64_t>& latencies,
        CoordinatorEpoch epoch = CoordinatorEpoch(1), std::uint64_t start_id = 1) {
    for (std::uint64_t i = 0; i < n; ++i) {
        std::int64_t lat = latencies[static_cast<std::size_t>(i % latencies.size())];
        RequestLatency r = mk_req(RequestId(start_id + i), base_arrival + static_cast<std::int64_t>(i) * 10,
            base_arrival + static_cast<std::int64_t>(i) * 10 + lat, cls, wk, boot,
            epoch, 0, {});
        g.ingest(r);
    }
}
