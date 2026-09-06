#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <optional>
#include <memory>

#include "tailgovernor/status.hpp"
#include "tailgovernor/clock.hpp"
#include "tailgovernor/identity.hpp"
#include "tailgovernor/duration.hpp"
#include "tailgovernor/latency.hpp"
#include "tailgovernor/objective.hpp"
#include "tailgovernor/window.hpp"
#include "tailgovernor/cause.hpp"
#include "tailgovernor/intervention.hpp"
#include "tailgovernor/authority.hpp"
#include "tailgovernor/evaluation.hpp"

namespace tailgovernor {

// Runtime facts the governor uses to decide intervention feasibility. These model
// the availability of preemptible work, warm engines, capacity, failover
// candidates, and intervention budgets. They are advisory inputs, not a
// scheduler.
struct ResourceFacts {
    bool engine_available = true;
    bool warm_engine_ready = false;
    bool capacity_available = true;
    bool preemptible_work = true;
    bool failover_candidate = true;
    bool target_compatible = true;
    bool batch_above_min = true;
    bool protected_traffic_present = true;
    bool memory_headroom = true;
    // Intervention budgets (fractions remaining, 0..1).
    double budget_preemptions = 1.0;
    double budget_failovers = 1.0;
    double budget_batch_changes = 1.0;
    double budget_residency_changes = 1.0;
    double budget_load_shedding = 1.0;
};

// A structured decision: the evaluation, whether to act, and the chosen action.
struct InterventionDecision {
    TailEvaluation evaluation;
    bool would_act = false;
    Intervention intervention;
    std::vector<RejectedAlternative> rejected_alternatives;
    std::vector<RejectedAlternative> infeasible_alternatives;
};

// A rendered, non-correctness-critical explanation derived from an evaluation.
struct TailExplanation {
    TailEvaluation evaluation;
    std::string render_markdown() const;
    std::string render_json() const;
};

// Configuration for a TailGovernor instance. Only the fields needed to fence
// decisions are required; runtime generations are optional.
struct GovernorConfig {
    ClockPtr clock;
    CoordinatorEpoch epoch = CoordinatorEpoch(0);
    ServiceId service = ServiceId(0);
    ServiceGeneration service_generation = ServiceGeneration(0);
    WorkloadId workload = WorkloadId(0);
    WorkloadGeneration workload_generation = WorkloadGeneration(0);

    // Resource discipline: hard bounds.
    std::size_t max_services = 256;
    std::size_t max_classes_per_objective = 1024;
    std::size_t max_pending_interventions = 4096;
    std::size_t max_history = 100000;
};

// Tail Governor: the specialized runtime that governs p95/p99/p999 tail latency.
// It observes completed authoritative work, evaluates tail objectives, attributes
// tail causes, selects and authorizes bounded tail-specific interventions, and
// verifies their effect. It is narrower than a generic latency runtime.
class TailGovernor {
public:
    explicit TailGovernor(GovernorConfig cfg);

    TailGovernor(const TailGovernor&) = delete;
    TailGovernor& operator=(const TailGovernor&) = delete;
    TailGovernor(TailGovernor&&) noexcept;
    TailGovernor& operator=(TailGovernor&&) noexcept;
    ~TailGovernor();

    // ---- worker authority fencing ----
    Status register_worker(WorkerId id, WorkerBootId boot);
    Status unregister_worker(WorkerId id);

    // ---- policy / objectives ----
    void set_policy(TailPolicy policy);
    const TailPolicy& policy() const;

    // ---- runtime generation advancement (advance then reject old authority) ----
    void advance_epoch();                                   // coordinator restart
    void set_resource_generation(ResourceGeneration g);
    void set_queue_generation(QueueGeneration g);
    void set_batch_generation(BatchGeneration g);
    void set_recovery_generation(RecoveryGeneration g);
    void set_topology_generation(TopologyGeneration g);
    void set_engine_incarnation(EngineIncarnationId e);

    // ---- evidence ingestion (authoritative completed work only) ----
    Status ingest(const RequestLatency& ev);

    // ---- resource facts for feasibility decisions ----
    void set_resource_facts(const ResourceFacts& facts);
    const ResourceFacts& resource_facts() const;

    // ---- evaluation ----
    Result<TailEvaluation> evaluate(const TailObjectiveId& id);
    std::vector<TailEvaluation> evaluate_all();

    // ---- deterministic decision pipeline ----
    Result<InterventionDecision> decide();

    // ---- intervention lifecycle ----
    Result<Intervention> authorize(const Intervention& proposed);
    Result<Intervention> dispatch(InterventionId id);
    Status acknowledge(InterventionId id);
    PostOutcome record_outcome(InterventionId id, PostOutcome outcome);
    Status cancel(InterventionId id);
    Status supersede(InterventionId id, const std::string& reason = {});

    // ---- persistence (versioned + integrity checked) ----
    Status save(const std::string& path) const;
    Status load(const std::string& path);

    // ---- shutdown ----
    void begin_shutdown();

    // ---- inspection ----
    const std::vector<Intervention>& intervention_history() const;
    const std::vector<TailEvaluation>& evaluations() const;
    CoordinatorEpoch epoch() const;
    std::size_t window_count() const;

    // Access an objective's window (or create a view) for inspection.
    std::optional<ObservationWindow> window_copy(TailObjectiveId objective, RequestClassId cls) const;
    std::vector<TailEvaluation> inspect_window(TailObjectiveId objective, RequestClassId cls);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace tailgovernor
