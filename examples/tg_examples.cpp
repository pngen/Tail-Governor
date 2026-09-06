#include "tailgovernor/tailgovernor.hpp"
#include <cstdio>
#include <memory>
#include <vector>

using namespace tailgovernor;

static TailGovernor mk_gov(std::int64_t base) {
    auto clk = std::make_shared<TestClock>(base);
    GovernorConfig gc; gc.clock = clk; gc.epoch = CoordinatorEpoch(1);
    gc.service = ServiceId(1); gc.service_generation = ServiceGeneration(1);
    gc.workload = WorkloadId(1); gc.workload_generation = WorkloadGeneration(1);
    TailGovernor g(gc);
    g.register_worker(WorkerId(1), WorkerBootId(1));
    return g;
}

static TailObjective p99(std::int64_t target) {
    TailObjective o; o.id = TailObjectiveId(1); o.generation = TailObjectiveGeneration(1);
    o.percentile = Percentile(99.0); o.target = Duration(target);
    o.window.window_duration = Duration(5'000'000'000LL); o.window.max_samples = 1000000;
    o.freshness = Duration(3'000'000'000LL); o.hard = true; o.recovery_fraction = 0.90;
    return o;
}

static void feed_simple(TailGovernor& g, std::uint64_t n, std::int64_t fast_ns, std::int64_t slow_ns, std::uint64_t slow_count) {
    for (std::uint64_t i = 0; i < n; ++i) {
        std::int64_t lat = (i + slow_count >= n) ? slow_ns : fast_ns;
        RequestLatency r; r.request_id = RequestId(i + 1); r.request_class = RequestClassId(0);
        r.worker = WorkerId(1); r.worker_boot = WorkerBootId(1); r.epoch = CoordinatorEpoch(1);
        r.service_generation = ServiceGeneration(1); r.workload_generation = WorkloadGeneration(1);
        r.arrival_ns = 1'000'000'000 + static_cast<std::int64_t>(i) * 10;
        r.completion_ns = r.arrival_ns + lat; r.publication_ns = r.completion_ns; r.captured_ns = r.publication_ns;
        r.total_latency = Duration(lat); r.status = RequestStatus::SUCCESS;
        std::int64_t qn = (lat >= slow_ns && slow_ns > 0 && slow_count > 0) ? (slow_ns - 2'000'000) : 0;
        if (qn > 0) r.phases.push_back({Phase::QUEUE_WAIT, Duration(qn)});
        r.phases.push_back({Phase::EXECUTION, Duration(lat - qn)});
        g.ingest(r);
    }
}

int main() {
    // 1. basic p99 objective
    { TailGovernor g = mk_gov(1'000'000'000); TailPolicy p; p.id=TailPolicyId(1); p.generation=TailPolicyGeneration(1); p.objectives.push_back(p99(10'000'000)); g.set_policy(p);
      feed_simple(g, 200, 2'000'000, 2'000'000, 0);
      auto e = g.evaluate(TailObjectiveId(1));
      std::printf("[basic p99] state=%s observed=%lldns (target=10ms)\n", to_string(e->state), (long long)e->observed.nanos()); }
    // 2. p50 stable, p99 bad
    { TailGovernor g = mk_gov(1'000'000'000); TailPolicy p; p.id=TailPolicyId(1); p.generation=TailPolicyGeneration(1); p.objectives.push_back(p99(10'000'000)); g.set_policy(p);
      feed_simple(g, 200, 2'000'000, 15'000'000, 10);
      auto e = g.evaluate(TailObjectiveId(1));
      std::printf("[stable p50 / bad p99] state=%s p50=%lldns p99=%lldns amp(p99/p50)=%.1f\n",
        to_string(e->state), (long long)(e->p50 ? e->p50->nanos() : 0), (long long)e->observed.nanos(),
        e->amplification.p99_over_p50 ? *e->amplification.p99_over_p50 : -1.0); }
    // 3. p999 insufficient samples
    { TailGovernor g = mk_gov(1'000'000'000); TailObjective o = p99(10'000'000); o.percentile = Percentile(99.9); TailPolicy p; p.id=TailPolicyId(1); p.generation=TailPolicyGeneration(1); p.objectives.push_back(o); g.set_policy(p);
      feed_simple(g, 10, 2'000'000, 2'000'000, 0);
      auto e = g.evaluate(TailObjectiveId(1));
      std::printf("[p999 insufficient] samples=%llu state=%s\n", (unsigned long long)e->sample_count, to_string(e->state)); }
    // 4. queue-tail attribution
    { TailGovernor g = mk_gov(1'000'000'000); TailPolicy p; p.id=TailPolicyId(1); p.generation=TailPolicyGeneration(1); p.objectives.push_back(p99(10'000'000)); g.set_policy(p);
      feed_simple(g, 200, 2'000'000, 15'000'000, 20);
      auto e = g.evaluate(TailObjectiveId(1));
      const char* cause = e->causes.empty() ? "UNKNOWN" : to_string(e->causes[0].category);
      std::printf("[queue tail] cause=%s intervention=%s\n", cause, to_string(e->selected_intervention)); }
    // 5. stale intervention
    { TailGovernor g = mk_gov(1'000'000'000); TailPolicy p; p.id=TailPolicyId(1); p.generation=TailPolicyGeneration(1); p.objectives.push_back(p99(10'000'000)); g.set_policy(p);
      auto ev = g.evaluate(TailObjectiveId(1));
      Intervention prop; prop.type=InterventionType::QUEUE_SHAPE; prop.objective=TailObjectiveId(1);
      prop.objective_generation=TailObjectiveGeneration(1); prop.authority=ev->authority; prop.status=InterventionStatus::PROPOSED;
      auto a = g.authorize(prop);
      if (a.is_ok()) { p.generation=TailPolicyGeneration(9); g.set_policy(p); auto d = g.dispatch(a->id);
        std::printf("[stale intervention] dispatch=%s code=%s\n", d.is_ok()?"ok":"rejected", d.is_ok()?"-":to_string(d.error().code)); } }
    // 6. post-action verification
    { TailGovernor g = mk_gov(1'000'000'000); TailPolicy p; p.id=TailPolicyId(1); p.generation=TailPolicyGeneration(1); p.objectives.push_back(p99(10'000'000)); g.set_policy(p);
      feed_simple(g, 200, 2'000'000, 15'000'000, 20);
      auto e = g.evaluate(TailObjectiveId(1));
      std::printf("[post-action] state before=%s intervention=%s (ack is not effect)\n", to_string(e->state), to_string(e->selected_intervention)); }
    return 0;
}
