#include "tailgovernor/tailgovernor.hpp"
#include <chrono>
#include <cstdio>
#include <memory>
#include <vector>

using namespace tailgovernor;

static double now_s() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

int main() {
    const std::size_t scales[] = {100, 1000, 10000, 100000, 1000000};
    for (std::size_t n : scales) {
        auto clk = std::make_shared<TestClock>(1'000'000'000);
        GovernorConfig gc; gc.clock = clk; gc.epoch = CoordinatorEpoch(1);
        gc.service = ServiceId(1); gc.service_generation = ServiceGeneration(1);
        gc.workload = WorkloadId(1); gc.workload_generation = WorkloadGeneration(1);
        TailGovernor g(gc);
        TailObjective o; o.id=TailObjectiveId(1); o.generation=TailObjectiveGeneration(1); o.percentile=Percentile(99.0);
        o.target=Duration(10'000'000); o.window.mode=WindowMode::ROLLING_COUNT; o.window.fixed_count=1'000'000;
        o.window.max_samples=2'000'000; o.freshness=Duration(3'000'000'000LL); o.hard=true; o.recovery_fraction=0.90;
        TailPolicy p; p.id=TailPolicyId(1); p.generation=TailPolicyGeneration(1); p.objectives.push_back(o);
        g.set_policy(p); g.register_worker(WorkerId(1), WorkerBootId(1));

        double t0 = now_s();
        for (std::size_t i = 0; i < n; ++i) {
            RequestLatency r; r.request_id=RequestId(i+1); r.request_class=RequestClassId(0);
            r.worker=WorkerId(1); r.worker_boot=WorkerBootId(1); r.epoch=CoordinatorEpoch(1);
            r.service_generation=ServiceGeneration(1); r.workload_generation=WorkloadGeneration(1);
            r.arrival_ns=1'000'000'000 + static_cast<std::int64_t>(i)*10;
            std::int64_t lat = (i % 50 == 0) ? 12'000'000 : 2'000'000;
            r.completion_ns=r.arrival_ns+lat; r.publication_ns=r.completion_ns; r.captured_ns=r.publication_ns;
            r.total_latency=Duration(lat); r.status=RequestStatus::SUCCESS;
            g.ingest(r);
        }
        double t1 = now_s();

        double t2 = now_s();
        auto ev = g.evaluate(TailObjectiveId(1));
        double t3 = now_s();
        double t4 = now_s();
        auto dec = g.decide();
        double t5 = now_s();

        std::printf("N=%-8zu ingest=%8.3fs (%.0f req/s)  eval=%8.4fs  decide=%8.4fs  state=%s\n",
            n, t1 - t0, static_cast<double>(n) / (t1 - t0), t3 - t2, t5 - t4,
            ev ? to_string(ev->state) : "-");
    }
    return 0;
}
