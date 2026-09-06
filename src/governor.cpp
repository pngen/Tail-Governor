#include "tailgovernor/governor.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <numeric>
#include <sstream>
#include <utility>
#include <vector>
#include <mutex>

namespace tailgovernor {
namespace {

// CRC-32 (IEEE) for persistence integrity.
std::uint32_t crc32(const std::uint8_t* data, std::size_t len, std::uint32_t crc = 0xFFFFFFFFu) {
    for (std::size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b) {
            std::uint32_t mask = static_cast<std::uint32_t>(-(static_cast<std::int32_t>(crc & 1u)));
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

struct GovernorWindowKey {
    TailObjectiveId objective;
    RequestClassId cls;
    bool operator<(const GovernorWindowKey& o) const {
        if (objective != o.objective) return objective < o.objective;
        return cls < o.cls;
    }
    bool operator==(const GovernorWindowKey& o) const {
        return objective == o.objective && cls == o.cls;
    }
};

const std::size_t kPersistMagic = 0x54474F56u;  // TGOV
const std::uint32_t kPersistVersion = 1u;
const std::size_t kMaxPersistBytes = 256u * 1024u * 1024u;

struct PercentileSet {
    std::optional<Duration> p50, p95, p99, p999, mean;
    bool p50_suff = false, p95_suff = false, p99_suff = false, p999_suff = false;
    std::uint64_t count = 0;
};

std::optional<Duration> q_aux(const std::vector<Duration>& sorted, Percentile p, std::uint64_t min_samples) {
    if (sorted.size() < min_samples) return std::nullopt;
    auto q = exact_quantile(sorted, p);
    if (!q) return std::nullopt;
    return *q;
}

PercentileSet compute_set(const std::vector<Duration>& sorted) {
    PercentileSet s;
    s.count = static_cast<std::uint64_t>(sorted.size());
    if (sorted.empty()) return s;
    s.p50 = q_aux(sorted, Percentile(50.0), 1u);
    s.p50_suff = s.p50.has_value();
    s.p95 = q_aux(sorted, Percentile(95.0), derived_min_samples(Percentile(95.0)));
    s.p95_suff = s.p95.has_value();
    s.p99 = q_aux(sorted, Percentile(99.0), derived_min_samples(Percentile(99.0)));
    s.p99_suff = s.p99.has_value();
    s.p999 = q_aux(sorted, Percentile(99.9), derived_min_samples(Percentile(99.9)));
    s.p999_suff = s.p999.has_value();
    long double sum = 0.0L;
    for (const auto& d : sorted) sum += static_cast<long double>(d.nanos());
    long double mean = sum / static_cast<long double>(sorted.size());
    s.mean = Duration(static_cast<std::int64_t>(mean));
    return s;
}

void decompose_tail(std::vector<PhaseContribution>& out, std::vector<TailCause>& causes,
                    ObservationWindow& w, std::int64_t now, Duration cutoff, Sufficiency suff);
void select_intervention(TailEvaluation& ev, const TailObjective& obj, const ResourceFacts& facts, std::int64_t now);
void build_what_would_change(TailEvaluation& ev, const TailObjective& obj, const ResourceFacts& facts);

// Serializes access to a governor from public operations. Recursive so
// composed public calls (evaluate_all -> evaluate) do not self-deadlock.
#define TG_LOCK() std::lock_guard<std::recursive_mutex> tg_lock_(impl_->mtx)

} // namespace

struct TailGovernor::Impl {
    GovernorConfig cfg;
    TailPolicy pol;
    ResourceFacts facts;
    std::map<WorkerId, WorkerBootId> worker_boots;
    std::map<GovernorWindowKey, ObservationWindow> windows;
    std::map<RequestId, EvidenceGeneration> seen;
    std::map<InterventionId, Intervention> interventions;
    std::map<TailObjectiveId, std::int64_t> last_intervention_ns;
    std::map<TailObjectiveId, TailState> last_state;
    std::map<TailObjectiveId, int> recover_streak;
    std::map<TailObjectiveId, bool> revalidate_needed;
    std::map<InterventionId, PostOutcome> outcomes;
    std::vector<TailEvaluation> evaluations;

    ResourceGeneration resource_gen = ResourceGeneration(0);
    QueueGeneration queue_gen = QueueGeneration(0);
    BatchGeneration batch_gen = BatchGeneration(0);
    RecoveryGeneration recovery_gen = RecoveryGeneration(0);
    TopologyGeneration topology_gen = TopologyGeneration(0);
    EngineIncarnationId engine_inc = EngineIncarnationId(0);
    bool shutting_down = false;
    std::uint64_t next_eval_id = 1;
    std::uint64_t next_int_id = 1;
    std::uint64_t next_dispatch_id = 1;

    // Serializes all public operations. Recursive so internal composition
    // (evaluate_all -> evaluate, decide -> authorize) does not self-deadlock.
    mutable std::recursive_mutex mtx;

    std::int64_t now() const { return cfg.clock ? cfg.clock->now_ns() : 0; }

    const TailObjective* find_objective(TailObjectiveId id) const {
        for (const auto& o : pol.objectives) if (o.id == id) return &o;
        return nullptr;
    }

    ObservationWindow& ensure_window(TailObjectiveId objective, RequestClassId cls) {
        GovernorWindowKey key{objective, cls};
        auto it = windows.find(key);
        if (it != windows.end()) return it->second;
        const TailObjective* obj = find_objective(objective);
        WindowConfig wc;
        if (obj) wc = obj->window;
        auto res = windows.emplace(key, ObservationWindow(wc));
        return res.first->second;
    }

    AuthorityContext current_authority(const TailObjective& obj) const {
        AuthorityContext a;
        a.epoch = cfg.epoch;
        a.service = cfg.service;
        a.service_generation = cfg.service_generation;
        a.workload = cfg.workload;
        a.workload_generation = cfg.workload_generation;
        a.policy = pol.id;
        a.policy_generation = pol.generation;
        a.objective = obj.id;
        a.objective_generation = obj.generation;
        a.queue_generation = queue_gen;
        a.batch_generation = batch_gen;
        a.resource_generation = resource_gen;
        a.recovery_generation = recovery_gen;
        a.topology_generation = topology_gen;
        a.engine_incarnation = engine_inc;
        return a;
    }
};

TailGovernor::TailGovernor(GovernorConfig cfg) : impl_(std::make_unique<Impl>()) {
    impl_->cfg = std::move(cfg);
}

TailGovernor::TailGovernor(TailGovernor&&) noexcept = default;
TailGovernor& TailGovernor::operator=(TailGovernor&&) noexcept = default;
TailGovernor::~TailGovernor() = default;

Status TailGovernor::register_worker(WorkerId id, WorkerBootId boot) {
    TG_LOCK();
    if (impl_->shutting_down) return Status(StatusCode::SHUTTING_DOWN, "register during shutdown");
    impl_->worker_boots[id] = boot;
    return Status();
}

Status TailGovernor::unregister_worker(WorkerId id) {
    TG_LOCK();
    impl_->worker_boots.erase(id);
    return Status();
}

void TailGovernor::set_policy(TailPolicy policy) { TG_LOCK(); impl_->pol = std::move(policy); }
const TailPolicy& TailGovernor::policy() const { TG_LOCK(); return impl_->pol; }

void TailGovernor::advance_epoch() {
    TG_LOCK();
    impl_->cfg.epoch = CoordinatorEpoch(impl_->cfg.epoch.value() + 1);
    for (const auto& o : impl_->pol.objectives) impl_->revalidate_needed[o.id] = true;
    impl_->windows.clear();
    impl_->seen.clear();
}

void TailGovernor::set_resource_generation(ResourceGeneration g) { TG_LOCK(); impl_->resource_gen = g; }
void TailGovernor::set_queue_generation(QueueGeneration g) { impl_->queue_gen = g; }
void TailGovernor::set_batch_generation(BatchGeneration g) { TG_LOCK(); impl_->batch_gen = g; }
void TailGovernor::set_recovery_generation(RecoveryGeneration g) { impl_->recovery_gen = g; }
void TailGovernor::set_topology_generation(TopologyGeneration g) { impl_->topology_gen = g; }
void TailGovernor::set_engine_incarnation(EngineIncarnationId e) { impl_->engine_inc = e; }

void TailGovernor::set_resource_facts(const ResourceFacts& facts) { TG_LOCK(); impl_->facts = facts; }
const ResourceFacts& TailGovernor::resource_facts() const { return impl_->facts; }

Status TailGovernor::ingest(const RequestLatency& ev) {
    TG_LOCK();
    if (impl_->shutting_down) return Status(StatusCode::SHUTTING_DOWN, "ingest during shutdown");
    if (ev.epoch != impl_->cfg.epoch)
        return Status(StatusCode::STALE_AUTHORITY, "evidence epoch is not current");
    if (ev.service_generation != impl_->cfg.service_generation)
        return Status(StatusCode::STALE_AUTHORITY, "evidence service generation is stale");
    if (ev.workload_generation != impl_->cfg.workload_generation)
        return Status(StatusCode::STALE_AUTHORITY, "evidence workload generation is stale");
    auto boot_it = impl_->worker_boots.find(ev.worker);
    if (boot_it == impl_->worker_boots.end())
        return Status(StatusCode::STALE_AUTHORITY, "evidence source worker is not registered");
    if (boot_it->second != ev.worker_boot)
        return Status(StatusCode::STALE_AUTHORITY, "evidence source worker boot is stale");
    if (!ev.is_authoritative_completion())
        return Status(StatusCode::INVALID_INPUT, "non-authoritative completion is not valid tail evidence");
    if (impl_->seen.find(ev.request_id) != impl_->seen.end())
        return Status(StatusCode::INVALID_INPUT, "duplicate completion");
    auto total = ev.total_latency.nanos() > 0 ? Result<Duration>(ev.total_latency) : ev.resolved_total();
    if (!total) return Status(total.error().code, total.error().message);
    std::int64_t phase_sum = 0;
    for (const auto& ph : ev.phases) {
        if (ph.duration.nanos() < 0) return Status(StatusCode::INVALID_INPUT, "negative phase duration");
        phase_sum += ph.duration.nanos();
        if (phase_sum > total->nanos()) return Status(StatusCode::INVALID_INPUT, "impossible phase sum exceeds total");
    }
    TailSample s;
    s.latency = *total;
    s.captured_ns = ev.captured_ns > 0 ? ev.captured_ns : ev.completion_ns;
    s.arrival_ns = ev.arrival_ns;
    for (const auto& ph : ev.phases) s.phases.push_back(ph);
    s.request_class = ev.request_class;
    s.worker_boot = ev.worker_boot;
    s.epoch = ev.epoch;
    s.retry_count = ev.retry_count;
    for (const auto& obj : impl_->pol.objectives) {
        if (!obj.enabled) continue;
        impl_->ensure_window(obj.id, RequestClassId(0)).insert(s);
        if (ev.request_class != RequestClassId(0)) impl_->ensure_window(obj.id, ev.request_class).insert(s);
        // Fresh current-epoch evidence clears the post-restart revalidation flag.
        impl_->revalidate_needed[obj.id] = false;
    }
    impl_->seen.emplace(ev.request_id, ev.evidence_generation);
    return Status();
}

Result<TailEvaluation> TailGovernor::evaluate(const TailObjectiveId& id) {
    TG_LOCK();
    if (impl_->shutting_down) return Result<TailEvaluation>::err(StatusCode::SHUTTING_DOWN, "shutdown");
    const TailObjective* obj = impl_->find_objective(id);
    if (!obj) return Result<TailEvaluation>::err(StatusCode::INVALID_INPUT, "unknown objective");
    if (!obj->enabled) return Result<TailEvaluation>::err(StatusCode::OBJECTIVE_INACTIVE, "objective disabled");
    const std::int64_t now = impl_->now();
    ObservationWindow& w = impl_->ensure_window(obj->id, obj->class_scope.value_or(RequestClassId(0)));
    const std::vector<Duration>& sorted = w.sorted(now);
    TailEvaluation ev;
    ev.id = EvaluationId(impl_->next_eval_id++);
    ev.generation = EvaluationGeneration(impl_->next_eval_id);
    ev.objective = obj->id;
    ev.objective_generation = obj->generation;
    ev.percentile = obj->percentile;
    ev.target = obj->target;
    ev.recovered_threshold = obj->recovery_threshold();
    ev.min_required = obj->effective_min_samples();
    ev.sample_count = w.size();
    ev.window_end_ns = now;
    ev.window_begin_ns = now - obj->window.window_duration.nanos();
    ev.latest_capture_ns = w.latest_captured_ns();
    ev.authority = impl_->current_authority(*obj);
    ev.revalidation_required = impl_->revalidate_needed[obj->id];
    std::int64_t age = ev.latest_capture_ns == 0 ? 0 : (now - ev.latest_capture_ns);
    bool stale = false;
    if (obj->freshness.nanos() > 0 && ev.latest_capture_ns > 0 && age > obj->freshness.nanos()) stale = true;
    ev.freshness_age_ms = static_cast<double>(age) / 1.0e6;
    PercentileSet set = compute_set(sorted);
    const std::uint64_t required = obj->effective_min_samples();
    const std::size_t n = sorted.size();
    bool obs_suff = n >= required;
    if (obs_suff) {
        auto q = exact_quantile(sorted, obj->percentile);
        if (q) ev.observed = *q; else obs_suff = false;
    }
    Sufficiency suff;
    if (impl_->revalidate_needed[obj->id]) suff = Sufficiency::STALE;
    else if (stale) suff = Sufficiency::STALE;
    else if (n == 0 || !obs_suff) suff = Sufficiency::INSUFFICIENT_SAMPLES;
    else suff = Sufficiency::SUFFICIENT;
    ev.sufficiency = suff;
    ev.p50 = set.p50; ev.p95 = set.p95; ev.p999 = set.p999;
    ev.amplification = compute_amplification(set.p50, set.p95, set.p99, set.p999, set.mean);
    TailState prev = impl_->last_state[obj->id];
    TailState state;
    if (impl_->revalidate_needed[obj->id]) state = TailState::REVALIDATION_REQUIRED;
    else if (suff == Sufficiency::STALE) state = TailState::STALE;
    else if (suff == Sufficiency::INSUFFICIENT_SAMPLES) state = TailState::INSUFFICIENT_EVIDENCE;
    else {
        const std::int64_t target = obj->target.nanos();
        const std::int64_t rec = obj->recovery_threshold().nanos();
        const std::int64_t v = ev.observed.nanos();
        bool degraded = (prev == TailState::VIOLATING || prev == TailState::SEVERE ||
                         prev == TailState::AT_RISK || prev == TailState::RECOVERING);
        int& streak = impl_->recover_streak[obj->id];
        if (v >= target) { streak = 0; state = (v >= 2 * target) ? TailState::SEVERE : TailState::VIOLATING; }
        else if (v > rec) { streak = 0; state = TailState::AT_RISK; }
        else {
            // Hysteresis: recovery requires a sustained run below the recovery
            // threshold before returning to HEALTHY, so the tail does not flap.
            if (degraded) { ++streak; state = (streak >= 2) ? TailState::HEALTHY : TailState::RECOVERING; }
            else { streak = 0; state = TailState::HEALTHY; }
        }
    }
    ev.state = state;
    impl_->last_state[obj->id] = state;
    decompose_tail(ev.phase_contributions, ev.causes, w, now, ev.observed, suff);
    select_intervention(ev, *obj, impl_->facts, now);
    if (state != TailState::VIOLATING && state != TailState::SEVERE && state != TailState::AT_RISK)
        ev.selected_intervention = InterventionType::NO_ACTION;
    impl_->evaluations.push_back(ev);
    return Result<TailEvaluation>(std::move(ev));
}

namespace {

void decompose_tail(std::vector<PhaseContribution>& out, std::vector<TailCause>& causes,
                    ObservationWindow& w, std::int64_t now, Duration cutoff, Sufficiency suff) {
    (void)suff;
    const std::vector<TailSample>& samples = w.samples_pruned(now);
    if (samples.empty()) return;
    // Select tail samples: those at/above cutoff, else the slowest handful.
    std::vector<const TailSample*> tail;
    for (const auto& s : samples) if (cutoff.nanos() > 0 && s.latency >= cutoff) tail.push_back(&s);
    if (tail.empty()) {
        // Take the slowest up to 64 samples.
        std::vector<const TailSample*> sorted_c;
        for (const auto& s : samples) sorted_c.push_back(&s);
        std::stable_sort(sorted_c.begin(), sorted_c.end(),
            [](const TailSample* a, const TailSample* b) { return a->latency.nanos() > b->latency.nanos(); });
        std::size_t k = std::min<std::size_t>(64, sorted_c.size());
        tail.assign(sorted_c.begin(), sorted_c.begin() + static_cast<std::ptrdiff_t>(k));
    }
    std::map<Phase, std::int64_t> totals;
    std::int64_t total_latency = 0;
    Phase dominant = Phase::OTHER;
    std::int64_t dominant_ns = -1;
    for (const auto* s : tail) {
        total_latency += s->latency.nanos();
        for (const auto& ph : s->phases) {
            totals[ph.phase] += ph.duration.nanos();
        }
    }
    // Determine dominant phase deterministically.
    for (const auto& kv : totals) {
        if (kv.second > dominant_ns || (kv.second == dominant_ns && static_cast<int>(kv.first) < static_cast<int>(dominant))) {
            dominant_ns = kv.second; dominant = kv.first;
        }
    }
    // Per-phase contributions.
    if (total_latency > 0) {
        for (const auto& kv : totals) {
            if (kv.second <= 0) continue;
            PhaseContribution pc;
            pc.phase = kv.first;
            pc.total = Duration(kv.second);
            pc.share = static_cast<double>(kv.second) / static_cast<double>(total_latency);
            pc.dominant = (kv.first == dominant);
            out.push_back(pc);
        }
    }
    // Cause classification with explicit attribution.
    if (totals.empty()) {
        TailCause uc; uc.category = TailCauseCategory::UNKNOWN; uc.attribution = Attribution::UNKNOWN_CAUSE;
        uc.confidence = 0.0; uc.evidence = "no phase evidence present";
        causes.push_back(uc);
        return;
    }
    // Confidence: fraction of tail samples whose dominant phase matches dominant.
    std::size_t matching = 0;
    for (const auto* s : tail) {
        Phase dp = s->dominant_phase();
        if (dp == dominant) ++matching;
    }
    double conf = tail.empty() ? 0.0 : static_cast<double>(matching) / static_cast<double>(tail.size());
    TailCause primary;
    primary.category = phase_to_cause(dominant);
    primary.attribution = Attribution::DOMINANT_CONTRIBUTOR;
    primary.confidence = conf;
    primary.excess = Duration(dominant_ns < 0 ? 0 : dominant_ns);
    primary.share = Duration(dominant_ns < 0 ? 0 : dominant_ns);
    primary.evidence = std::string(to_string(dominant)) + " dominates tail samples";
    causes.push_back(primary);
    for (const auto& kv : totals) {
        if (kv.first == dominant) continue;
        double share = static_cast<double>(kv.second) / static_cast<double>(total_latency);
        if (share >= 0.20) {
            TailCause sc;
            sc.category = phase_to_cause(kv.first);
            sc.attribution = Attribution::ASSOCIATED_CONTRIBUTOR;
            sc.confidence = share;
            sc.excess = Duration(kv.second);
            sc.share = Duration(kv.second);
            sc.evidence = std::string(to_string(kv.first)) + " is an associated contributor";
            causes.push_back(sc);
        }
    }
}

Infeasibility infeasibility_of(InterventionType t, const ResourceFacts& f) {
    switch (t) {
        case InterventionType::PREEMPT_LOWER_PRIORITY:
            if (!f.preemptible_work) return Infeasibility::NON_PREEMPTIBLE;
            if (f.budget_preemptions <= 0.0) return Infeasibility::EXCEEDS_HARD_ECONOMY;
            return Infeasibility::NONE;
        case InterventionType::WARM_RESIDENCY:
        case InterventionType::PROMOTE_WARM_ENGINE:
            if (!f.warm_engine_ready) return Infeasibility::ENGINE_UNAVAILABLE;
            if (f.budget_residency_changes <= 0.0) return Infeasibility::EXCEEDS_HARD_ECONOMY;
            return Infeasibility::NONE;
        case InterventionType::PIN_RESIDENCY:
            if (!f.engine_available) return Infeasibility::ENGINE_UNAVAILABLE;
            if (f.budget_residency_changes <= 0.0) return Infeasibility::EXCEEDS_HARD_ECONOMY;
            return Infeasibility::NONE;
        case InterventionType::RESERVE_CAPACITY:
            if (!f.capacity_available) return Infeasibility::CAPACITY_UNAVAILABLE;
            return Infeasibility::NONE;
        case InterventionType::FAILOVER:
            if (!f.failover_candidate) return Infeasibility::NO_FAILOVER_CANDIDATE;
            if (f.budget_failovers <= 0.0) return Infeasibility::EXCEEDS_HARD_ECONOMY;
            return Infeasibility::NONE;
        case InterventionType::CHANGE_PLACEMENT:
            if (!f.target_compatible) return Infeasibility::INCOMPATIBLE_TARGET;
            return Infeasibility::NONE;
        case InterventionType::REDUCE_BATCH_SIZE:
        case InterventionType::SPLIT_BATCH:
        case InterventionType::DISABLE_BATCH_FOR_CLASS:
            if (!f.batch_above_min) return Infeasibility::BELOW_MIN_BATCH;
            if (f.budget_batch_changes <= 0.0) return Infeasibility::EXCEEDS_HARD_ECONOMY;
            return Infeasibility::NONE;
        case InterventionType::SHED_LOAD:
        case InterventionType::DEFER_LOW_PRIORITY:
            if (f.protected_traffic_present) return Infeasibility::PROTECTED_TRAFFIC;
            if (f.budget_load_shedding <= 0.0) return Infeasibility::EXCEEDS_HARD_ECONOMY;
            return Infeasibility::NONE;
        default: return Infeasibility::NONE;
    }
}

double effect_factor(InterventionType t) {
    switch (t) {
        case InterventionType::BOUND_QUEUE_WAIT: return 0.8;
        case InterventionType::QUEUE_SHAPE: return 0.7;
        case InterventionType::LIMIT_QUEUE_DEPTH: return 0.6;
        case InterventionType::QUEUE_REPRIORITIZE: return 0.5;
        case InterventionType::SPLIT_BATCH: return 0.8;
        case InterventionType::REDUCE_BATCH_SIZE: return 0.6;
        case InterventionType::DISABLE_BATCH_FOR_CLASS: return 0.5;
        case InterventionType::WARM_RESIDENCY: return 0.8;
        case InterventionType::PIN_RESIDENCY: return 0.5;
        case InterventionType::PROMOTE_WARM_ENGINE: return 0.7;
        case InterventionType::RESERVE_CAPACITY: return 0.7;
        case InterventionType::RESERVE_BANDWIDTH: return 0.6;
        case InterventionType::PREEMPT_LOWER_PRIORITY: return 0.7;
        case InterventionType::DEFER_LOW_PRIORITY: return 0.4;
        case InterventionType::SHED_LOAD: return 0.5;
        case InterventionType::RECLAIM_MEMORY: return 0.4;
        case InterventionType::CHANGE_PLACEMENT: return 0.6;
        case InterventionType::ACCELERATE_RECOVERY: return 0.7;
        case InterventionType::REPLAN_RECOVERY: return 0.6;
        case InterventionType::FAILOVER: return 0.8;
        case InterventionType::ESCALATE: return 0.3;
        case InterventionType::MANUAL_INTERVENTION_REQUIRED: return 0.2;
        default: return 0.0;
    }
}

std::vector<InterventionType> candidates_for_cause(TailCauseCategory c) {
    switch (c) {
        case TailCauseCategory::QUEUEING: return {InterventionType::BOUND_QUEUE_WAIT, InterventionType::QUEUE_SHAPE, InterventionType::LIMIT_QUEUE_DEPTH, InterventionType::QUEUE_REPRIORITIZE};
        case TailCauseCategory::BATCHING: return {InterventionType::SPLIT_BATCH, InterventionType::REDUCE_BATCH_SIZE, InterventionType::DISABLE_BATCH_FOR_CLASS};
        case TailCauseCategory::COLD_RESIDENCY: return {InterventionType::WARM_RESIDENCY, InterventionType::PIN_RESIDENCY, InterventionType::PROMOTE_WARM_ENGINE};
        case TailCauseCategory::MODEL_OR_STATE_LOAD: return {InterventionType::WARM_RESIDENCY, InterventionType::PROMOTE_WARM_ENGINE, InterventionType::CHANGE_PLACEMENT};
        case TailCauseCategory::TRANSFER: return {InterventionType::RESERVE_BANDWIDTH, InterventionType::CHANGE_PLACEMENT, InterventionType::DEFER_LOW_PRIORITY};
        case TailCauseCategory::MEMORY_PRESSURE: return {InterventionType::RECLAIM_MEMORY, InterventionType::DEFER_LOW_PRIORITY, InterventionType::RESERVE_CAPACITY};
        case TailCauseCategory::RESOURCE_CONTENTION: return {InterventionType::RESERVE_CAPACITY, InterventionType::CHANGE_PLACEMENT, InterventionType::PROMOTE_WARM_ENGINE};
        case TailCauseCategory::EXECUTION_VARIANCE: return {InterventionType::CHANGE_PLACEMENT, InterventionType::RESERVE_CAPACITY, InterventionType::WARM_RESIDENCY};
        case TailCauseCategory::PREEMPTION: return {InterventionType::PREEMPT_LOWER_PRIORITY};
        case TailCauseCategory::RECOVERY: return {InterventionType::ACCELERATE_RECOVERY, InterventionType::REPLAN_RECOVERY, InterventionType::FAILOVER};
        case TailCauseCategory::RETRY: return {InterventionType::DEFER_LOW_PRIORITY, InterventionType::RESERVE_CAPACITY, InterventionType::ESCALATE};
        case TailCauseCategory::BACKEND_STALL: return {InterventionType::CHANGE_PLACEMENT, InterventionType::ACCELERATE_RECOVERY};
        case TailCauseCategory::PUBLICATION_DELAY: return {InterventionType::ESCALATE, InterventionType::MANUAL_INTERVENTION_REQUIRED};
        case TailCauseCategory::CAPACITY_SHORTAGE: return {InterventionType::RESERVE_CAPACITY, InterventionType::SHED_LOAD, InterventionType::DEFER_LOW_PRIORITY};
        case TailCauseCategory::PRIORITY_INVERSION: return {InterventionType::QUEUE_REPRIORITIZE, InterventionType::DEFER_LOW_PRIORITY, InterventionType::PREEMPT_LOWER_PRIORITY};
        case TailCauseCategory::UNKNOWN: return {};
    }
    return {};
}

double collateral_of(InterventionType t) {
    switch (t) {
        case InterventionType::PREEMPT_LOWER_PRIORITY: return 120'000'000.0;
        case InterventionType::SHED_LOAD: return 200'000'000.0;
        case InterventionType::FAILOVER: return 500'000'000.0;
        case InterventionType::RECLAIM_MEMORY: return 40'000'000.0;
        case InterventionType::BOUND_QUEUE_WAIT: return 10'000'000.0;
        case InterventionType::QUEUE_SHAPE: return 8'000'000.0;
        case InterventionType::SPLIT_BATCH: return 30'000'000.0;
        case InterventionType::DEFER_LOW_PRIORITY: return 60'000'000.0;
        default: return 5'000'000.0;
    }
}

void select_intervention(TailEvaluation& ev, const TailObjective& obj, const ResourceFacts& facts, std::int64_t now) {
    (void)now;
    ev.rejected_alternatives.clear();
    TailCauseCategory cause = TailCauseCategory::UNKNOWN;
    for (const auto& c : ev.causes) if (c.attribution == Attribution::DOMINANT_CONTRIBUTOR) { cause = c.category; break; }
    std::vector<InterventionType> cands = candidates_for_cause(cause);
    // With an UNKNOWN cause we do not guess a tail-specific action; the absence of
    // evidence must stay UNKNOWN rather than become a fabricated intervention.
    std::int64_t excess = ev.observed.nanos() - ev.recovered_threshold.nanos();
    if (excess < 0) excess = 0;
    struct Cand { InterventionType type; double eff; double collateral; };
    std::vector<Cand> feasible;
    for (auto t : cands) {
        Infeasibility inf = infeasibility_of(t, facts);
        double eff = effect_factor(t) * static_cast<double>(excess);  // nanoseconds
        double collateral = collateral_of(t);
        if (inf != Infeasibility::NONE) {
            RejectedAlternative ra;
            ra.type = t; ra.reason = inf;
            ra.detail = std::string(to_string(t)) + " rejected: " + to_string(inf);
            ev.rejected_alternatives.push_back(ra);
            if (!ev.binding_hard_constraint.has_value()) {
                if (inf == Infeasibility::VIOLATES_HARD_SLO || inf == Infeasibility::EXCEEDS_HARD_ECONOMY ||
                    inf == Infeasibility::PROTECTED_TRAFFIC || inf == Infeasibility::NON_PREEMPTIBLE ||
                    inf == Infeasibility::ENGINE_UNAVAILABLE || inf == Infeasibility::CAPACITY_UNAVAILABLE ||
                    inf == Infeasibility::NO_FAILOVER_CANDIDATE) {
                    ev.binding_hard_constraint = inf;
                }
            }
        } else {
            feasible.push_back({t, eff, collateral});
        }
    }
    std::stable_sort(feasible.begin(), feasible.end(), [](const Cand& a, const Cand& b) {
        if (a.eff != b.eff) return a.eff > b.eff;
        if (static_cast<int>(a.type) != static_cast<int>(b.type)) return static_cast<int>(a.type) < static_cast<int>(b.type);
        return a.collateral < b.collateral;
    });
    if (!feasible.empty()) {
        ev.selected_intervention = feasible[0].type;
        ev.expected_tail_reduction = Duration(static_cast<std::int64_t>(feasible[0].eff));
        ev.expected_collateral = Duration(static_cast<std::int64_t>(feasible[0].collateral));
        ev.uncertainty = 0.3;
        ev.uncertainty_reason = "modeled estimate, not measured";
        for (std::size_t i = 1; i < feasible.size(); ++i) {
            RejectedAlternative ra;
            ra.type = feasible[i].type; ra.reason = Infeasibility::NONE;
            ra.detail = std::string(to_string(feasible[i].type)) + " rejected: lower modeled effect";
            ev.rejected_alternatives.push_back(ra);
        }
    } else {
        ev.selected_intervention = InterventionType::NO_ACTION;
        if (!ev.binding_hard_constraint.has_value()) ev.uncertainty = 0.5;
        if (ev.uncertainty_reason.empty()) ev.uncertainty_reason = "no feasible intervention";
    }
    build_what_would_change(ev, obj, facts);
}

void build_what_would_change(TailEvaluation& ev, const TailObjective& obj, const ResourceFacts& facts) {
    (void)obj;
    ev.what_would_change.clear();
    auto add = [&ev](WhatWouldChange t, const char* n) { ev.what_would_change.push_back({t, n}); };
    if (ev.observed.nanos() > ev.recovered_threshold.nanos())
        add(WhatWouldChange::P99_BELOW_RECOVERY, "p99/p999 falls below the recovery threshold");
    if (!facts.capacity_available) add(WhatWouldChange::MORE_CAPACITY, "additional capacity appears");
    if (ev.sufficiency == Sufficiency::INSUFFICIENT_SAMPLES)
        add(WhatWouldChange::TAIL_SUFFICIENCY_IMPROVES, "sample sufficiency improves");
    if (!ev.p999.has_value()) add(WhatWouldChange::P999_EVIDENCE_FRESH, "p999 evidence becomes fresh");
    for (const auto& c : ev.causes) {
        if (c.category == TailCauseCategory::QUEUEING) add(WhatWouldChange::QUEUE_DEPTH_DROPS, "queue depth drops");
        if (c.category == TailCauseCategory::BATCHING) add(WhatWouldChange::BATCH_WAIT_DISAPPEARS, "batch wait disappears");
        if (c.category == TailCauseCategory::COLD_RESIDENCY) add(WhatWouldChange::WARM_ENGINE_READY, "warm engine becomes ready");
        if (c.category == TailCauseCategory::TRANSFER) add(WhatWouldChange::TRANSFER_PATH_CLEARS, "transfer path clears");
        if (c.category == TailCauseCategory::MEMORY_PRESSURE) add(WhatWouldChange::MEMORY_PRESSURE_FALLS, "memory pressure falls");
    }
    if (!facts.preemptible_work) add(WhatWouldChange::PREEMPTION_SAFE, "candidate preemption becomes safe");
    if (ev.binding_hard_constraint.has_value())
        add(WhatWouldChange::HARD_SLO_BLOCKS, "stronger SLO/cost constraint relaxes");
    if (ev.state == TailState::HEALTHY && !obj.class_scope.has_value())
        add(WhatWouldChange::HIGHER_PRIORITY_CLASS_ARRIVES, "a higher-priority request class arrives");
}


} // namespace

namespace {
struct Writer {
    std::vector<std::uint8_t> buf;
    void u8(std::uint8_t v) { buf.push_back(v); }
    void u32(std::uint32_t v) { for (int i = 0; i < 4; ++i) buf.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF)); }
    void u64(std::uint64_t v) { for (int i = 0; i < 8; ++i) buf.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF)); }
    void f64(double v) { std::uint64_t b; std::memcpy(&b, &v, 8); u64(b); }
    void bytes(const std::uint8_t* p, std::size_t len) { buf.insert(buf.end(), p, p + len); }
};
struct Reader {
    const std::uint8_t* p; std::size_t n; std::size_t pos = 0; bool ok = true;
    bool take(std::size_t k) { if (pos + k > n) { ok = false; return false; } pos += k; return true; }
    std::uint8_t u8() { if (!take(1)) return 0; return p[pos - 1]; }
    std::uint32_t u32() { if (!take(4)) return 0; std::uint32_t v = 0; for (int i = 0; i < 4; ++i) v |= (static_cast<std::uint32_t>(p[pos - 4 + i]) << (8 * i)); return v; }
    std::uint64_t u64() { if (!take(8)) return 0; std::uint64_t v = 0; for (int i = 0; i < 8; ++i) v |= (static_cast<std::uint64_t>(p[pos - 8 + i]) << (8 * i)); return v; }
    double f64() { std::uint64_t b = u64(); double v; std::memcpy(&v, &b, 8); return v; }
    bool bytes(void* out, std::size_t len) { if (!take(len)) return false; std::memcpy(out, p + pos - len, len); return true; }
};
} // namespace

Status TailGovernor::save(const std::string& path) const {
    TG_LOCK();
    Writer w;
    w.u64(impl_->cfg.epoch.value());
    w.u64(impl_->pol.id.value());
    w.u64(impl_->pol.generation.value());
    w.u32(static_cast<std::uint32_t>(impl_->pol.objectives.size()));
    for (const auto& o : impl_->pol.objectives) {
        w.u64(o.id.value()); w.u64(o.generation.value());
        w.f64(o.percentile.value()); w.u64(static_cast<std::uint64_t>(o.target.nanos()));
        w.u32(static_cast<std::uint32_t>(o.window.mode));
        w.u64(static_cast<std::uint64_t>(o.window.window_duration.nanos()));
        w.u64(o.window.max_samples); w.u64(o.window.fixed_count);
        w.u64(o.min_samples); w.u64(static_cast<std::uint64_t>(o.freshness.nanos()));
        w.u8(o.hard ? 1 : 0); w.f64(o.recovery_fraction);
        w.u64(static_cast<std::uint64_t>(o.cool_down.nanos()));
        w.u8(o.class_scope.has_value() ? 1 : 0);
        if (o.class_scope.has_value()) w.u64(o.class_scope->value());
        w.u32(static_cast<std::uint32_t>(static_cast<std::int32_t>(o.priority)));
        w.u8(o.enabled ? 1 : 0);
    }
    w.u32(static_cast<std::uint32_t>(impl_->interventions.size()));
    for (const auto& kv : impl_->interventions) {
        const Intervention& inv = kv.second;
        w.u64(inv.id.value()); w.u64(inv.generation.value());
        w.u32(static_cast<std::uint32_t>(inv.type));
        w.u32(static_cast<std::uint32_t>(inv.status));
        w.u64(inv.objective.value()); w.u64(inv.objective_generation.value());
        w.u32(static_cast<std::uint32_t>(inv.target_cause));
        w.u64(inv.target_class.value());
        w.u64(static_cast<std::uint64_t>(inv.proposed_at_ns));
        w.u64(static_cast<std::uint64_t>(inv.expiry_at_ns));
        w.u64(static_cast<std::uint64_t>(inv.estimated_tail_effect.nanos()));
        w.u8(inv.feasibility == Infeasibility::NONE ? 0 : 1);
        w.u32(static_cast<std::uint32_t>(static_cast<std::int32_t>(inv.feasibility)));
        w.f64(inv.confidence);
    }
    w.u32(static_cast<std::uint32_t>(impl_->outcomes.size()));
    for (const auto& kv : impl_->outcomes) {
        w.u64(kv.first.value());
        w.u32(static_cast<std::uint32_t>(kv.second));
    }
    w.u32(static_cast<std::uint32_t>(impl_->revalidate_needed.size()));
    for (const auto& kv : impl_->revalidate_needed) {
        w.u64(kv.first.value()); w.u8(kv.second ? 1 : 0);
    }
    w.u64(impl_->next_eval_id); w.u64(impl_->next_int_id);
    std::vector<std::uint8_t> file;
    auto put32 = [&file](std::uint32_t v) { for (int i = 0; i < 4; ++i) file.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF)); };
    auto put64 = [&file](std::uint64_t v) { for (int i = 0; i < 8; ++i) file.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF)); };
    put32(static_cast<std::uint32_t>(kPersistMagic));
    put32(kPersistVersion);
    put64(static_cast<std::uint64_t>(w.buf.size()));
    std::uint32_t crc = crc32(w.buf.data(), w.buf.size());
    put32(crc);
    file.insert(file.end(), w.buf.begin(), w.buf.end());
    std::string tmp = path + ".tmp";
    std::ofstream os(tmp, std::ios::binary | std::ios::trunc);
    if (!os) return Status(StatusCode::PERSISTENCE_CORRUPT, "cannot open state file for write");
    os.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
    os.close();
    if (os.fail()) return Status(StatusCode::PERSISTENCE_CORRUPT, "write failed");
    std::remove(path.c_str());
    std::rename(tmp.c_str(), path.c_str());
    return Status();
}

Status TailGovernor::load(const std::string& path) {
    TG_LOCK();
    std::ifstream is(path, std::ios::binary | std::ios::ate);
    if (!is) return Status(StatusCode::PERSISTENCE_CORRUPT, "cannot open state file");
    std::streamsize fsize = is.tellg();
    if (fsize < 20) return Status(StatusCode::PERSISTENCE_CORRUPT, "file too small");
    if (static_cast<std::size_t>(fsize) > kMaxPersistBytes) return Status(StatusCode::PERSISTENCE_CORRUPT, "file too large");
    is.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> file(static_cast<std::size_t>(fsize));
    is.read(reinterpret_cast<char*>(file.data()), fsize);
    if (!is) return Status(StatusCode::PERSISTENCE_CORRUPT, "read failed");

    auto get32 = [&file](std::size_t off) -> std::uint32_t {
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= (static_cast<std::uint32_t>(file[off + i]) << (8 * i));
        return v;
    };
    std::uint32_t magic = get32(0);
    std::uint32_t version = get32(4);
    std::uint64_t body_len = 0;
    for (int i = 0; i < 8; ++i) body_len |= (static_cast<std::uint64_t>(file[8 + i]) << (8 * i));
    if (magic != kPersistMagic) return Status(StatusCode::PERSISTENCE_CORRUPT, "bad magic");
    if (version != kPersistVersion) return Status(StatusCode::PERSISTENCE_CORRUPT, "unsupported version");
    if (body_len > kMaxPersistBytes) return Status(StatusCode::PERSISTENCE_CORRUPT, "body too large");
    if (20u + body_len != file.size()) return Status(StatusCode::PERSISTENCE_CORRUPT, "trailing garbage / truncated");
    std::uint32_t stored_crc = get32(16);
    std::uint32_t calc_crc = crc32(file.data() + 20, static_cast<std::size_t>(body_len));
    if (stored_crc != calc_crc) return Status(StatusCode::PERSISTENCE_CORRUPT, "checksum mismatch");

    Reader r{file.data() + 20, static_cast<std::size_t>(body_len), 0, true};
    (void)r.u64();  // saved CoordinatorEpoch (caller controls the post-restart epoch)
    TailPolicy newPol;
    newPol.id = TailPolicyId(r.u64());
    newPol.generation = TailPolicyGeneration(r.u64());
    std::uint32_t nObj = r.u32();
    if (nObj > 8192) return Status(StatusCode::PERSISTENCE_CORRUPT, "absurd objective count");
    for (std::uint32_t i = 0; i < nObj; ++i) {
        TailObjective o;
        o.id = TailObjectiveId(r.u64());
        o.generation = TailObjectiveGeneration(r.u64());
        o.percentile = Percentile(r.f64());
        o.target = Duration(static_cast<std::int64_t>(r.u64()));
        o.window.mode = static_cast<WindowMode>(r.u32());
        o.window.window_duration = Duration(static_cast<std::int64_t>(r.u64()));
        o.window.max_samples = r.u64();
        o.window.fixed_count = r.u64();
        o.min_samples = r.u64();
        o.freshness = Duration(static_cast<std::int64_t>(r.u64()));
        o.hard = r.u8() != 0;
        o.recovery_fraction = r.f64();
        o.cool_down = Duration(static_cast<std::int64_t>(r.u64()));
        bool has_class = r.u8() != 0;
        if (has_class) o.class_scope = RequestClassId(r.u64());
        o.priority = static_cast<std::int32_t>(r.u32());
        o.enabled = r.u8() != 0;
        newPol.objectives.push_back(o);
        impl_->revalidate_needed[o.id] = true;   // recovered state is not current
    }
    if (!r.ok) return Status(StatusCode::PERSISTENCE_CORRUPT, "truncated objective body");
    impl_->pol = std::move(newPol);

    std::uint32_t nInt = r.u32();
    if (nInt > 100000) return Status(StatusCode::PERSISTENCE_CORRUPT, "absurd intervention count");
    impl_->interventions.clear();
    for (std::uint32_t i = 0; i < nInt; ++i) {
        Intervention inv;
        inv.id = InterventionId(r.u64());
        inv.generation = InterventionGeneration(r.u64());
        inv.type = static_cast<InterventionType>(r.u32());
        inv.status = static_cast<InterventionStatus>(r.u32());
        inv.objective = TailObjectiveId(r.u64());
        inv.objective_generation = TailObjectiveGeneration(r.u64());
        inv.target_cause = static_cast<TailCauseCategory>(r.u32());
        inv.target_class = RequestClassId(r.u64());
        inv.proposed_at_ns = static_cast<std::int64_t>(r.u64());
        inv.expiry_at_ns = static_cast<std::int64_t>(r.u64());
        inv.estimated_tail_effect = Duration(static_cast<std::int64_t>(r.u64()));
        bool has_inf = r.u8() != 0;
        (void)has_inf;
        inv.feasibility = static_cast<Infeasibility>(r.u32());
        inv.confidence = r.f64();
        impl_->interventions[inv.id] = inv;
    }
    std::uint32_t nOut = r.u32();
    if (nOut > 100000) return Status(StatusCode::PERSISTENCE_CORRUPT, "absurd outcome count");
    impl_->outcomes.clear();
    for (std::uint32_t i = 0; i < nOut; ++i) {
        InterventionId id(r.u64());
        PostOutcome po = static_cast<PostOutcome>(r.u32());
        impl_->outcomes[id] = po;
    }
    std::uint32_t nReval = r.u32();
    if (nReval > 8192) return Status(StatusCode::PERSISTENCE_CORRUPT, "absurd revalidation count");
    for (std::uint32_t i = 0; i < nReval; ++i) {
        TailObjectiveId id(r.u64());
        (void)r.u8();
        // Dynamic observations are never current after a restart: force revalidation.
        impl_->revalidate_needed[id] = true;
    }
    impl_->next_eval_id = r.u64();
    impl_->next_int_id = r.u64();
    if (!r.ok) return Status(StatusCode::PERSISTENCE_CORRUPT, "truncated body");
    return Status();
}

Result<Intervention> TailGovernor::authorize(const Intervention& proposed) {
    TG_LOCK();
    if (impl_->shutting_down) return Result<Intervention>::err(StatusCode::SHUTTING_DOWN, "shutdown");
    if (proposed.type == InterventionType::NO_ACTION) return Result<Intervention>::err(StatusCode::INVALID_INPUT, "cannot authorize NO_ACTION");
    if (proposed.status != InterventionStatus::PROPOSED) return Result<Intervention>::err(StatusCode::INVALID_INPUT, "must be PROPOSED to authorize");
    const TailObjective* obj = impl_->find_objective(proposed.objective);
    if (!obj || !obj->enabled) return Result<Intervention>::err(StatusCode::OBJECTIVE_INACTIVE, "objective not active");
    AuthorityContext cur = impl_->current_authority(*obj);
    if (!AuthorityContext::same(proposed.authority, cur)) return Result<Intervention>::err(StatusCode::STALE_AUTHORITY, "intervention authority is stale");
    Infeasibility inf = infeasibility_of(proposed.type, impl_->facts);
    if (inf != Infeasibility::NONE) return Result<Intervention>::err(StatusCode::INTERVENTION_INFEASIBLE, std::string("intervention infeasible: ") + to_string(inf));
    auto it = impl_->last_intervention_ns.find(proposed.objective);
    if (it != impl_->last_intervention_ns.end() && obj->cool_down.nanos() > 0) {
        std::int64_t elapsed = impl_->now() - it->second;
        if (elapsed < obj->cool_down.nanos()) return Result<Intervention>::err(StatusCode::INVALID_INPUT, "intervention cooldown active");
    }
    Intervention out = proposed;
    out.status = InterventionStatus::AUTHORIZED;
    if (out.id == InterventionId(0)) out.id = InterventionId(impl_->next_int_id++);
    if (out.generation == InterventionGeneration(0)) out.generation = InterventionGeneration(impl_->next_int_id);
    out.proposed_at_ns = impl_->now();
    impl_->interventions[out.id] = out;
    impl_->last_intervention_ns[out.objective] = impl_->now();
    return Result<Intervention>(out);
}

Result<Intervention> TailGovernor::dispatch(InterventionId id) {
    TG_LOCK();
    if (impl_->shutting_down) return Result<Intervention>::err(StatusCode::SHUTTING_DOWN, "shutdown");
    auto it = impl_->interventions.find(id);
    if (it == impl_->interventions.end()) return Result<Intervention>::err(StatusCode::INVALID_INPUT, "unknown intervention");
    Intervention& inv = it->second;
    if (inv.status != InterventionStatus::AUTHORIZED) return Result<Intervention>::err(StatusCode::INVALID_INPUT, "must be AUTHORIZED to dispatch");
    const TailObjective* obj = impl_->find_objective(inv.objective);
    if (!obj || !obj->enabled) { inv.status = InterventionStatus::FAILED; return Result<Intervention>::err(StatusCode::OBJECTIVE_INACTIVE, "objective no longer active"); }
    AuthorityContext cur = impl_->current_authority(*obj);
    if (!AuthorityContext::same(inv.authority, cur)) {
        inv.status = InterventionStatus::SUPERSEDED;
        return Result<Intervention>::err(StatusCode::STALE_AUTHORITY, "intervention superseded by authority change");
    }
    Infeasibility inf = infeasibility_of(inv.type, impl_->facts);
    if (inf != Infeasibility::NONE) { inv.status = InterventionStatus::FAILED; return Result<Intervention>::err(StatusCode::INTERVENTION_INFEASIBLE, "resources no longer support it"); }
    inv.status = InterventionStatus::DISPATCHED;
    return Result<Intervention>(inv);
}

Status TailGovernor::acknowledge(InterventionId id) {
    TG_LOCK();
    auto it = impl_->interventions.find(id);
    if (it == impl_->interventions.end()) return Status(StatusCode::INVALID_INPUT, "unknown intervention");
    Intervention& inv = it->second;
    if (inv.status != InterventionStatus::DISPATCHED) return Status(StatusCode::INVALID_INPUT, "must be DISPATCHED to acknowledge");
    inv.status = InterventionStatus::ACKNOWLEDGED;
    return Status();
}

PostOutcome TailGovernor::record_outcome(InterventionId id, PostOutcome outcome) {
    TG_LOCK();
    auto it = impl_->interventions.find(id);
    if (it == impl_->interventions.end()) return PostOutcome::OUTCOME_UNKNOWN;
    Intervention& inv = it->second;
    if (inv.is_terminal()) return PostOutcome::OUTCOME_UNKNOWN;
    switch (outcome) {
        case PostOutcome::EFFECTIVE: inv.status = InterventionStatus::EFFECTIVE; break;
        case PostOutcome::PARTIALLY_EFFECTIVE: inv.status = InterventionStatus::INEFFECTIVE; break;
        case PostOutcome::INEFFECTIVE: inv.status = InterventionStatus::INEFFECTIVE; break;
        case PostOutcome::WORSENED_TAIL: inv.status = InterventionStatus::INEFFECTIVE; break;
        case PostOutcome::CREATED_SECONDARY_VIOLATION: inv.status = InterventionStatus::FAILED; break;
        case PostOutcome::INSUFFICIENT_POST_ACTION_EVIDENCE: inv.status = InterventionStatus::OUTCOME_UNKNOWN; break;
        case PostOutcome::OUTCOME_UNKNOWN: inv.status = InterventionStatus::OUTCOME_UNKNOWN; break;
    }
    impl_->outcomes[id] = outcome;
    return outcome;
}

Status TailGovernor::cancel(InterventionId id) {
    TG_LOCK();
    auto it = impl_->interventions.find(id);
    if (it == impl_->interventions.end()) return Status(StatusCode::INVALID_INPUT, "unknown intervention");
    Intervention& inv = it->second;
    if (inv.is_terminal()) return Status();
    if (inv.status == InterventionStatus::PROPOSED || inv.status == InterventionStatus::AUTHORIZED || inv.status == InterventionStatus::DISPATCHED) {
        inv.status = InterventionStatus::CANCELLED;
        return Status();
    }
    return Status(StatusCode::INVALID_INPUT, "past irreversible cancellation boundary");
}

Status TailGovernor::supersede(InterventionId id, const std::string& reason) {
    TG_LOCK();
    auto it = impl_->interventions.find(id);
    if (it == impl_->interventions.end()) return Status(StatusCode::INVALID_INPUT, "unknown intervention");
    Intervention& inv = it->second;
    if (inv.is_terminal()) return Status();
    inv.status = InterventionStatus::SUPERSEDED;
    if (!reason.empty()) inv.reason += " (superseded: " + reason + ")";
    return Status();
}

namespace {
TailEvaluation summary_from_window(const TailObjective& obj, const ObservationWindow& w, std::int64_t now) {
    TailEvaluation ev;
    ev.objective = obj.id;
    ev.objective_generation = obj.generation;
    ev.percentile = obj.percentile;
    ev.target = obj.target;
    ev.recovered_threshold = obj.recovery_threshold();
    ev.sample_count = w.size();
    ev.min_required = obj.effective_min_samples();
    ev.window_end_ns = now;
    ev.window_begin_ns = now - obj.window.window_duration.nanos();
    ev.latest_capture_ns = w.latest_captured_ns();
    const std::vector<Duration>& sorted = const_cast<ObservationWindow&>(w).sorted(now);
    if (sorted.empty()) { ev.state = TailState::INSUFFICIENT_EVIDENCE; ev.sufficiency = Sufficiency::INSUFFICIENT_SAMPLES; return ev; }
    auto setq = [&](Percentile p, std::uint64_t minv, std::optional<Duration>& out) {
        if (sorted.size() >= minv) { auto q = exact_quantile(sorted, p); if (q) out = *q; }
    };
    setq(Percentile(50.0), 1u, ev.p50);
    setq(Percentile(95.0), derived_min_samples(Percentile(95.0)), ev.p95);
    setq(Percentile(99.0), derived_min_samples(Percentile(99.0)), ev.p99_for_p95);
    setq(Percentile(99.9), derived_min_samples(Percentile(99.9)), ev.p999);
    std::uint64_t required = obj.effective_min_samples();
    if (sorted.size() >= required) { auto q = exact_quantile(sorted, obj.percentile); if (q) ev.observed = *q; ev.sufficiency = Sufficiency::SUFFICIENT; }
    else { ev.sufficiency = Sufficiency::INSUFFICIENT_SAMPLES; }
    if (ev.sufficiency == Sufficiency::SUFFICIENT) {
        std::int64_t t = obj.target.nanos();
        if (ev.observed.nanos() >= t) ev.state = (ev.observed.nanos() >= 2 * t) ? TailState::SEVERE : TailState::VIOLATING;
        else if (ev.observed.nanos() > obj.recovery_threshold().nanos()) ev.state = TailState::AT_RISK;
        else ev.state = TailState::HEALTHY;
    } else {
        ev.state = TailState::INSUFFICIENT_EVIDENCE;
    }
    return ev;
}
} // namespace

std::vector<TailEvaluation> TailGovernor::evaluate_all() {
    TG_LOCK();
    std::vector<TailEvaluation> out;
    for (const auto& o : impl_->pol.objectives) {
        if (!o.enabled) continue;
        auto r = evaluate(o.id);
        if (r) out.push_back(*r);
    }
    return out;
}

Result<InterventionDecision> TailGovernor::decide() {
    TG_LOCK();
    if (impl_->shutting_down) return Result<InterventionDecision>::err(StatusCode::SHUTTING_DOWN, "shutdown");
    auto evs = evaluate_all();
    const TailEvaluation* bind = nullptr;
    for (const auto& ev : evs) {
        bool degrading = ev.state == TailState::VIOLATING || ev.state == TailState::SEVERE;
        if (!degrading) continue;
        if (!bind) { bind = &ev; continue; }
        std::int64_t e = ev.observed.nanos() - ev.recovered_threshold.nanos();
        std::int64_t be = bind->observed.nanos() - bind->recovered_threshold.nanos();
        if (e > be) bind = &ev;
        else if (e == be) {
            int p1 = 0, p2 = 0;
            for (const auto& o : impl_->pol.objectives) { if (o.id == ev.objective) p1 = o.priority; if (o.id == bind->objective) p2 = o.priority; }
            if (p1 > p2) bind = &ev;
            else if (p1 == p2 && ev.objective.value() < bind->objective.value()) bind = &ev;
        }
    }
    InterventionDecision d;
    if (!bind) {
        for (const auto& ev : evs) if (ev.state == TailState::AT_RISK) { if (!bind) bind = &ev; }
    }
    if (!bind) {
        d.evaluation = evs.empty() ? TailEvaluation{} : evs.front();
        d.would_act = false;
        return Result<InterventionDecision>(std::move(d));
    }
    d.evaluation = *bind;
    if (bind->selected_intervention != InterventionType::NO_ACTION && bind->sufficiency == Sufficiency::SUFFICIENT) {
        Intervention prop;
        prop.type = bind->selected_intervention;
        prop.target_class = RequestClassId(0);
        for (const auto& o : impl_->pol.objectives) if (o.id == bind->objective) { if (o.class_scope) prop.target_class = *o.class_scope; break; }
        for (const auto& c : bind->causes) if (c.attribution == Attribution::DOMINANT_CONTRIBUTOR) prop.target_cause = c.category;
        prop.objective = bind->objective;
        prop.objective_generation = bind->objective_generation;
        prop.authority = bind->authority;
        prop.estimated_tail_effect = bind->expected_tail_reduction;
        prop.confidence = 1.0 - bind->uncertainty;
        prop.proposed_at_ns = impl_->now();
        prop.expiry_at_ns = impl_->now() + 5'000'000'000LL;
        prop.reason = std::string("governed tail: ") + to_string(prop.type);
        auto ar = authorize(prop);
        if (ar) { d.would_act = true; d.intervention = *ar; }
    }
    for (const auto& ra : bind->rejected_alternatives) {
        if (ra.reason != Infeasibility::NONE) d.infeasible_alternatives.push_back(ra);
        else d.rejected_alternatives.push_back(ra);
    }
    return Result<InterventionDecision>(std::move(d));
}

void TailGovernor::begin_shutdown() { TG_LOCK(); impl_->shutting_down = true; }

const std::vector<Intervention>& TailGovernor::intervention_history() const {
    // collect sorted
    static thread_local std::vector<Intervention> scratch;
    scratch.clear();
    for (const auto& kv : impl_->interventions) scratch.push_back(kv.second);
    return scratch;
}

const std::vector<TailEvaluation>& TailGovernor::evaluations() const { return impl_->evaluations; }
CoordinatorEpoch TailGovernor::epoch() const { return impl_->cfg.epoch; }
std::size_t TailGovernor::window_count() const { return impl_->windows.size(); }

std::optional<ObservationWindow> TailGovernor::window_copy(TailObjectiveId objective, RequestClassId cls) const {
    TG_LOCK();
    auto it = impl_->windows.find(GovernorWindowKey{objective, cls});
    if (it == impl_->windows.end()) return std::nullopt;
    return it->second;
}

std::vector<TailEvaluation> TailGovernor::inspect_window(TailObjectiveId objective, RequestClassId cls) {
    const TailObjective* obj = impl_->find_objective(objective);
    if (!obj) return {};
    auto w = window_copy(objective, cls);
    if (!w) { /* create empty */ w = ObservationWindow(obj->window); }
    std::vector<TailEvaluation> out;
    out.push_back(summary_from_window(*obj, *w, impl_->now()));
    return out;
}






std::string TailExplanation::render_markdown() const {
    std::ostringstream os;
    const TailEvaluation& e = evaluation;
    os << "## Tail Evaluation\n";
    os << "- objective: " << e.objective.to_string() << "\n";
    os << "- percentile: " << e.percentile.value() << "\n";
    os << "- target: " << e.target.to_string() << "\n";
    os << "- observed: " << e.observed.to_string() << "\n";
    os << "- state: " << to_string(e.state) << "\n";
    os << "- sufficiency: " << to_string(e.sufficiency) << "\n";
    os << "- samples: " << e.sample_count << " (min " << e.min_required << ")\n";
    os << "- freshness age (ms): " << e.freshness_age_ms << "\n";
    if (e.amplification.p99_over_p50) os << "- p99/p50: " << *e.amplification.p99_over_p50 << "\n";
    if (e.amplification.p999_over_p50) os << "- p999/p50: " << *e.amplification.p999_over_p50 << "\n";
    os << "- selected intervention: " << to_string(e.selected_intervention) << "\n";
    if (e.binding_hard_constraint) os << "- binding hard constraint: " << to_string(*e.binding_hard_constraint) << "\n";
    os << "- expected tail reduction: " << e.expected_tail_reduction.to_string() << "\n";
    os << "- uncertainty: " << e.uncertainty << "\n";
    os << "- what-would-change:\n";
    for (const auto& w : e.what_would_change) os << "  - " << to_string(w.trigger) << ": " << w.note << "\n";
    return os.str();
}

std::string TailExplanation::render_json() const {
    std::ostringstream os;
    const TailEvaluation& e = evaluation;
    os << "{\"objective\":\"" << e.objective.to_string() << "\",";
    os << "\"percentile\":" << e.percentile.value() << ",";
    os << "\"target_ns\":" << e.target.nanos() << ",";
    os << "\"observed_ns\":" << e.observed.nanos() << ",";
    os << "\"state\":\"" << to_string(e.state) << "\",";
    os << "\"sufficiency\":\"" << to_string(e.sufficiency) << "\",";
    os << "\"samples\":" << e.sample_count << ",\"min_required\":" << e.min_required << ",";
    os << "\"selected_intervention\":\"" << to_string(e.selected_intervention) << "\"}";
    return os.str();
}

} // namespace tailgovernor

