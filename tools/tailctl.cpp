#include "tailgovernor/tailgovernor.hpp"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <memory>

using namespace tailgovernor;

static int cmd_demo() {
    auto clk = std::make_shared<TestClock>(1'000'000'000);
    GovernorConfig gc; gc.clock = clk; gc.epoch = CoordinatorEpoch(1);
    gc.service = ServiceId(1); gc.service_generation = ServiceGeneration(1);
    gc.workload = WorkloadId(1); gc.workload_generation = WorkloadGeneration(1);
    TailGovernor g(gc);

    TailObjective obj;
    obj.id = TailObjectiveId(1); obj.generation = TailObjectiveGeneration(1);
    obj.percentile = Percentile(99.0); obj.target = Duration(10'000'000);
    obj.window.window_duration = Duration(5'000'000'000LL); obj.window.max_samples = 1000000;
    obj.freshness = Duration(3'000'000'000LL); obj.hard = true; obj.recovery_fraction = 0.90;
    TailPolicy pol; pol.id = TailPolicyId(1); pol.generation = TailPolicyGeneration(1); pol.name = "demo";
    pol.objectives.push_back(obj);
    g.set_policy(pol);
    g.register_worker(WorkerId(1), WorkerBootId(1));

    // 90 fast + 10 slow (queue-dominated) requests -> a p99 tail.
    for (int i = 0; i < 200; ++i) {
        RequestLatency r;
        r.request_id = RequestId(i + 1); r.request_class = RequestClassId(0);
        r.worker = WorkerId(1); r.worker_boot = WorkerBootId(1); r.epoch = CoordinatorEpoch(1);
        r.service_generation = ServiceGeneration(1); r.workload_generation = WorkloadGeneration(1);
        std::int64_t qn = (i < 180) ? 0 : 12'000'000;
        r.arrival_ns = 1'000'000'000 + static_cast<std::int64_t>(i) * 10;
        r.completion_ns = r.arrival_ns + 2'000'000 + qn;
        r.publication_ns = r.completion_ns; r.captured_ns = r.publication_ns;
        r.total_latency = Duration(2'000'000 + qn);
        r.status = RequestStatus::SUCCESS;
        if (qn > 0) r.phases.push_back({Phase::QUEUE_WAIT, Duration(qn)});
        r.phases.push_back({Phase::EXECUTION, Duration(2'000'000)});
        g.ingest(r);
    }
    auto ev = g.evaluate(TailObjectiveId(1));
    if (!ev) { std::fprintf(stderr, "evaluate failed\n"); return 1; }
    std::printf("%s", TailExplanation{*ev}.render_markdown().c_str());
    auto dec = g.decide();
    if (dec) {
        std::printf("would_act: %s\n", dec->would_act ? "true" : "false");
        if (dec->would_act) std::printf("authorized: %s (status %s)\n", to_string(dec->intervention.type), to_string(dec->intervention.status));
        for (const auto& ra : dec->rejected_alternatives) std::printf("rejected: %s -> %s\n", to_string(ra.type), to_string(ra.reason));
    }
    return 0;
}

static int cmd_validate(const std::string& path) {
    GovernorConfig gc; gc.clock = std::make_shared<SystemClock>(); gc.epoch = CoordinatorEpoch(1);
    TailGovernor g(gc);
    Status s = g.load(path);
    if (!s.ok()) { std::printf("INVALID: %s (%s)\n", to_string(s.code), s.message.c_str()); return 1; }
    std::printf("VALID: %zu objective(s), %zu intervention(s), epoch=%llu\n",
        g.policy().objectives.size(), g.intervention_history().size(),
        static_cast<unsigned long long>(g.epoch().value()));
    return 0;
}

int main(int argc, char** argv) {
    if (argc >= 2 && std::string(argv[1]) == "demo") return cmd_demo();
    if (argc >= 3 && std::string(argv[1]) == "validate-state") return cmd_validate(argv[2]);
    std::fprintf(stderr, "usage: tailctl demo | validate-state <state-file>\n");
    return 2;
}
