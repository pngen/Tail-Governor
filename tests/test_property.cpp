#include "tg_test.hpp"
#include "tg_fixtures.hpp"
#include <thread>
#include <atomic>
#include <vector>
#include <random>

using namespace tailgovernor;

TEST(percentile_invariants) {
    // Deterministic LCG sample set; ordered percentiles must be non-decreasing.
    std::uint64_t seed = 0x12345678;
    auto next = [&]() { seed = seed * 6364136223846793005ULL + 1442695040888963407ULL; return seed; };
    for (std::size_t n : {10u, 100u, 1000u, 10000u}) {
        std::vector<Duration> v; v.reserve(n);
        for (std::size_t i = 0; i < n; ++i) v.push_back(Duration(static_cast<std::int64_t>(next() % 1'000'000'000)));
        std::sort(v.begin(), v.end());
        auto q50 = exact_quantile(v, Percentile(50.0));
        auto q95 = exact_quantile(v, Percentile(95.0));
        auto q99 = exact_quantile(v, Percentile(99.0));
        auto q999 = exact_quantile(v, Percentile(99.9));
        REQUIRE(q50 && q95 && q99 && q999);
        CHECK(q50->nanos() <= q95->nanos());
        CHECK(q95->nanos() <= q99->nanos());
        CHECK(q99->nanos() <= q999->nanos());
    }
}

TEST(stale_evaluation_no_current_authority) {
    auto pol = mk_policy({mk_p99_objective(10'000'000, TailObjectiveId(1))});
    GovernorConfig c = mk_config(1'000'000'000);
    TailGovernor g(c);
    g.set_policy(pol);
    g.register_worker(WorkerId(1), WorkerBootId(1));
    // produce a valid evaluation
    feed(g, WorkerId(1), WorkerBootId(1), 1'000'000'000, RequestClassId(0), 200, std::vector<std::int64_t>(200, 12'000'000));
    auto evOld = g.evaluate(TailObjectiveId(1)); REQUIRE(evOld.is_ok());
    // coordinator restarts (epoch advances); the pre-restart evaluation becomes stale
    g.advance_epoch();
    // an intervention built against the stale authority must be rejected
    Intervention prop; prop.type = InterventionType::QUEUE_SHAPE; prop.objective = TailObjectiveId(1);
    prop.objective_generation = TailObjectiveGeneration(1); prop.authority = evOld->authority; prop.status = InterventionStatus::PROPOSED;
    auto a = g.authorize(prop);
    CHECK(!a.is_ok());
    CHECK(a.error().code == StatusCode::STALE_AUTHORITY);
}

TEST(lifecycle_terminal_is_terminal) {
    auto pol = mk_policy({mk_p99_objective(10'000'000, TailObjectiveId(1))});
    GovernorConfig c = mk_config(1'000'000'000);
    TailGovernor g(c);
    g.set_policy(pol);
    g.register_worker(WorkerId(1), WorkerBootId(1));
    auto ev = g.evaluate(TailObjectiveId(1)); REQUIRE(ev.is_ok());
    Intervention prop; prop.type = InterventionType::QUEUE_SHAPE; prop.objective = TailObjectiveId(1);
    prop.objective_generation = TailObjectiveGeneration(1); prop.authority = ev->authority; prop.status = InterventionStatus::PROPOSED;
    auto a = g.authorize(prop); REQUIRE(a.is_ok());
    g.dispatch(a->id); g.acknowledge(a->id);
    CHECK(g.record_outcome(a->id, PostOutcome::EFFECTIVE) == PostOutcome::EFFECTIVE);
    // terminal stays terminal; a late outcome is not accepted and cannot flip status
    CHECK(g.record_outcome(a->id, PostOutcome::INEFFECTIVE) == PostOutcome::OUTCOME_UNKNOWN);
    CHECK(g.intervention_history().back().status == InterventionStatus::EFFECTIVE);
}

TEST(deterministic_canonical_state_same_decision) {
    auto run = [](std::uint64_t seed) -> std::pair<std::uint64_t, InterventionType> {
        auto clk = std::make_shared<TestClock>(1'000'000'000);
        GovernorConfig c = mk_config(1'000'000'000); c.clock = clk;
        TailGovernor g(c); g.set_policy(mk_policy({mk_p99_objective(10'000'000, TailObjectiveId(1))}));
        g.register_worker(WorkerId(1), WorkerBootId(1));
        std::vector<std::int64_t> vals;
        for (int i = 0; i < 200; ++i) vals.push_back((seed % 3 == 0) ? 12'000'000 : 2'000'000);
        feed(g, WorkerId(1), WorkerBootId(1), 1'000'000'000, RequestClassId(0), 200, vals, CoordinatorEpoch(1), 1);
        auto ev = g.evaluate(TailObjectiveId(1));
        if (!ev) return {0, InterventionType::NO_ACTION};
        return { ev->observed.nanos(), ev->selected_intervention };
    };
    auto a = run(1234567); auto b = run(1234567);
    CHECK(a.first == b.first);
    CHECK(a.second == b.second);
}

TEST(concurrent_ingest_evaluate) {
    auto clk = std::make_shared<TestClock>(1'000'000'000);
    GovernorConfig c = mk_config(1'000'000'000); c.clock = clk;
    TailGovernor g(c);
    g.set_policy(mk_policy({mk_p99_objective(10'000'000, TailObjectiveId(1))}));
    g.register_worker(WorkerId(1), WorkerBootId(1));
    std::atomic<int> errors{0};
    auto work = [&](WorkerId wk, WorkerBootId boot, std::uint64_t start) {
        std::vector<PhaseLatency> ph = { {Phase::EXECUTION, Duration(2'000'000)} };
        for (int i = 0; i < 500; ++i) {
            std::int64_t pub = 1'000'000'000 + static_cast<std::int64_t>(i) * 10 + 2'000'000;
            RequestLatency r = mk_req(RequestId(start + static_cast<std::uint64_t>(i)), 1'000'000'000 + static_cast<std::int64_t>(i) * 10, pub, RequestClassId(0), wk, boot, CoordinatorEpoch(1), 0, ph);
            if (g.ingest(r).code != StatusCode::OK) ++errors;
            if (i % 100 == 0) { auto ev = g.evaluate(TailObjectiveId(1)); if (!ev.is_ok()) ++errors; }
        }
        // exercise lifecycle concurrently
        auto ev = g.evaluate(TailObjectiveId(1));
        if (ev.is_ok()) {
            Intervention prop; prop.type = InterventionType::QUEUE_SHAPE; prop.objective = TailObjectiveId(1);
            prop.objective_generation = TailObjectiveGeneration(1); prop.authority = ev->authority; prop.status = InterventionStatus::PROPOSED;
            auto a = g.authorize(prop); if (a.is_ok()) { g.dispatch(a->id); g.acknowledge(a->id); g.record_outcome(a->id, PostOutcome::EFFECTIVE); }
        }
    };
    std::vector<std::thread> ts;
    for (int t = 0; t < 4; ++t) ts.emplace_back(work, WorkerId(1), WorkerBootId(1), static_cast<std::uint64_t>(t) * 1'000'000 + 1);
    for (auto& th : ts) th.join();   // no deadlock / hang = pass
    CHECK(errors.load() == 0);
}

int main() { return tgtest::run_all(); }
