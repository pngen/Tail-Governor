#pragma once

#include <optional>

#include "tailgovernor/identity.hpp"

namespace tailgovernor {

// A snapshot of the generations/environments that govern whether an evaluation
// or intervention is current. Only the applicable generations are filled in;
// unnecessary ones stay null. This is what makes a stale decision reject
// without mutating current state.
struct AuthorityContext {
    CoordinatorEpoch epoch = CoordinatorEpoch(0);
    ServiceId service = ServiceId(0);
    ServiceGeneration service_generation = ServiceGeneration(0);
    WorkloadId workload = WorkloadId(0);
    WorkloadGeneration workload_generation = WorkloadGeneration(0);
    TailPolicyId policy = TailPolicyId(0);
    TailPolicyGeneration policy_generation = TailPolicyGeneration(0);
    TailObjectiveId objective = TailObjectiveId(0);
    TailObjectiveGeneration objective_generation = TailObjectiveGeneration(0);

    std::optional<QueueGeneration> queue_generation;
    std::optional<BatchGeneration> batch_generation;
    std::optional<ResourceGeneration> resource_generation;
    std::optional<WorkerBootId> worker_boot;
    std::optional<EngineIncarnationId> engine_incarnation;
    std::optional<RecoveryGeneration> recovery_generation;
    std::optional<TopologyGeneration> topology_generation;

    static bool same(const AuthorityContext& a, const AuthorityContext& b) {
        if (a.epoch != b.epoch) return false;
        if (a.service != b.service) return false;
        if (a.service_generation != b.service_generation) return false;
        if (a.workload != b.workload) return false;
        if (a.workload_generation != b.workload_generation) return false;
        if (a.policy != b.policy) return false;
        if (a.policy_generation != b.policy_generation) return false;
        if (a.objective != b.objective) return false;
        if (a.objective_generation != b.objective_generation) return false;
        if (a.queue_generation != b.queue_generation) return false;
        if (a.batch_generation != b.batch_generation) return false;
        if (a.resource_generation != b.resource_generation) return false;
        if (a.worker_boot != b.worker_boot) return false;
        if (a.engine_incarnation != b.engine_incarnation) return false;
        if (a.recovery_generation != b.recovery_generation) return false;
        if (a.topology_generation != b.topology_generation) return false;
        return true;
    }
};

} // namespace tailgovernor
