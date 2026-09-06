#include "tg_test.hpp"
#include "tg_fixtures.hpp"

using namespace tailgovernor;

static TailGovernor mk_gov(TailPolicy pol, std::shared_ptr<TestClock>& clk) {
    GovernorConfig c = mk_config(1'000'000'000);
    c.clock = clk;
    TailGovernor g(c);
    g.set_policy(pol);
    g.register_worker(WorkerId(1), WorkerBootId(1));
    return g;
}

static void feed_phased(TailGovernor& g, std::int64_t base, RequestClassId cls, std::uint64_t n, Phase phase, std::int64_t phase_ns, std::int64_t exec_ns) {
    for (std::uint64_t i = 0; i < n; ++i) {
        std::vector<PhaseLatency> ph = { {phase, Duration(phase_ns)}, {Phase::EXECUTION, Duration(exec_ns)} };
        std::int64_t pub = base + static_cast<std::int64_t>(i) * 10 + phase_ns + exec_ns;
        RequestLatency r = mk_req(RequestId(i + 5000000), base + static_cast<std::int64_t>(i) * 10, pub, cls, WorkerId(1), WorkerBootId(1), CoordinatorEpoch(1), 0, std::move(ph));
        g.ingest(r);
    }
}

TEST(request_class_isolation) {
    std::vector<TailObjective> objs;
    objs.push_back(mk_p99_objective(10'000'000, TailObjectiveId(1)));
    objs.push_back(mk_p99_objective(10'000'000, TailObjectiveId(2)));
    objs.back().class_scope = RequestClassId(1);
    objs.push_back(mk_p99_objective(10'000'000, TailObjectiveId(3)));
    objs.back().class_scope = RequestClassId(5);
    auto clk = std::make_shared<TestClock>(1'000'000'000);
    TailGovernor g = mk_gov(mk_policy(std::move(objs)), clk);
    std::vector<std::int64_t> fast(100, 2'000'000);
    feed(g, WorkerId(1), WorkerBootId(1), 1'000'000'000, RequestClassId(1), 100, fast, CoordinatorEpoch(1), 1);
    std::vector<std::int64_t> slow(100, 15'000'000);
    feed(g, WorkerId(1), WorkerBootId(1), 1'000'000'000, RequestClassId(5), 100, slow, CoordinatorEpoch(1), 100000);
    auto eg = g.evaluate(TailObjectiveId(1));
    REQUIRE(eg.is_ok());
    CHECK(eg->state == TailState::VIOLATING);
    auto ec5 = g.evaluate(TailObjectiveId(3));
    REQUIRE(ec5.is_ok());
    CHECK(ec5->state == TailState::VIOLATING);
    auto ec1 = g.evaluate(TailObjectiveId(2));
    REQUIRE(ec1.is_ok());
    CHECK(ec1->state == TailState::HEALTHY);
}

TEST(cause_queueing_and_intervention) {
    auto clk = std::make_shared<TestClock>(1'000'000'000);
    TailGovernor g = mk_gov(mk_policy({mk_p99_objective(10'000'000, TailObjectiveId(1))}), clk);
    feed_phased(g, 1'000'000'000, RequestClassId(0), 200, Phase::QUEUE_WAIT, 12'000'000, 1'000'000);
    auto ev = g.evaluate(TailObjectiveId(1));
    REQUIRE(ev.is_ok());
    CHECK(ev->causes.size() >= 1);
    CHECK(ev->causes[0].category == TailCauseCategory::QUEUEING);
    CHECK(ev->causes[0].attribution == Attribution::DOMINANT_CONTRIBUTOR);
    CHECK(ev->selected_intervention == InterventionType::BOUND_QUEUE_WAIT);
}

TEST(cause_batching) {
    auto clk = std::make_shared<TestClock>(1'000'000'000);
    TailGovernor g = mk_gov(mk_policy({mk_p99_objective(10'000'000, TailObjectiveId(1))}), clk);
    feed_phased(g, 1'000'000'000, RequestClassId(0), 200, Phase::BATCH_WAIT, 12'000'000, 1'000'000);
    auto ev = g.evaluate(TailObjectiveId(1));
    REQUIRE(ev.is_ok());
    CHECK(ev->causes[0].category == TailCauseCategory::BATCHING);
    CHECK(ev->selected_intervention == InterventionType::SPLIT_BATCH);
}

TEST(cause_residency_engine_unavailable) {
    auto clk = std::make_shared<TestClock>(1'000'000'000);
    TailGovernor g = mk_gov(mk_policy({mk_p99_objective(10'000'000, TailObjectiveId(1))}), clk);
    feed_phased(g, 1'000'000'000, RequestClassId(0), 200, Phase::ENGINE_WARMUP, 12'000'000, 1'000'000);
    ResourceFacts f; f.warm_engine_ready = false; g.set_resource_facts(f);
    auto ev = g.evaluate(TailObjectiveId(1));
    REQUIRE(ev.is_ok());
    CHECK(ev->causes[0].category == TailCauseCategory::COLD_RESIDENCY);
    CHECK(ev->selected_intervention == InterventionType::PIN_RESIDENCY);
    bool rejected_warm = false;
    for (const auto& ra : ev->rejected_alternatives) if (ra.type == InterventionType::WARM_RESIDENCY && ra.reason == Infeasibility::ENGINE_UNAVAILABLE) rejected_warm = true;
    CHECK(rejected_warm);
}

TEST(hard_constraint_best_action_rejected) {
    auto clk = std::make_shared<TestClock>(1'000'000'000);
    TailGovernor g = mk_gov(mk_policy({mk_p99_objective(10'000'000, TailObjectiveId(1))}), clk);
    feed_phased(g, 1'000'000'000, RequestClassId(0), 200, Phase::RECOVERY_WAIT, 12'000'000, 1'000'000);
    ResourceFacts f; f.failover_candidate = false; g.set_resource_facts(f);
    auto ev = g.evaluate(TailObjectiveId(1));
    REQUIRE(ev.is_ok());
    CHECK(ev->causes[0].category == TailCauseCategory::RECOVERY);
    CHECK(ev->selected_intervention == InterventionType::ACCELERATE_RECOVERY);
    bool rejected_failover = false;
    for (const auto& ra : ev->rejected_alternatives) if (ra.type == InterventionType::FAILOVER && ra.reason == Infeasibility::NO_FAILOVER_CANDIDATE) rejected_failover = true;
    CHECK(rejected_failover);
    CHECK(ev->binding_hard_constraint.has_value());
}

TEST(deterministic_decision) {
    auto run = []() -> std::pair<Duration, InterventionType> {
        auto clk = std::make_shared<TestClock>(1'000'000'000);
        TailGovernor g = mk_gov(mk_policy({mk_p99_objective(10'000'000, TailObjectiveId(1))}), clk);
        feed_phased(g, 1'000'000'000, RequestClassId(0), 200, Phase::QUEUE_WAIT, 12'000'000, 1'000'000);
        auto ev = g.evaluate(TailObjectiveId(1));
        return { ev->observed, ev->selected_intervention };
    };
    auto a = run(); auto b = run();
    CHECK(a.first.nanos() == b.first.nanos());
    CHECK(a.second == b.second);
}

TEST(no_valid_action_when_cause_unknown) {
    auto clk = std::make_shared<TestClock>(1'000'000'000);
    TailGovernor g = mk_gov(mk_policy({mk_p99_objective(10'000'000, TailObjectiveId(1))}), clk);
    feed_phased(g, 1'000'000'000, RequestClassId(0), 200, Phase::OTHER, 12'000'000, 1'000'000);
    auto ev = g.evaluate(TailObjectiveId(1));
    REQUIRE(ev.is_ok());
    CHECK(ev->causes[0].category == TailCauseCategory::UNKNOWN);
    CHECK(ev->selected_intervention == InterventionType::NO_ACTION);
}

TEST(all_interventions_infeasible) {
    auto clk = std::make_shared<TestClock>(1'000'000'000);
    TailGovernor g = mk_gov(mk_policy({mk_p99_objective(10'000'000, TailObjectiveId(1))}), clk);
    feed_phased(g, 1'000'000'000, RequestClassId(0), 200, Phase::PREEMPTION_WAIT, 12'000'000, 1'000'000);
    ResourceFacts f; f.preemptible_work = false; g.set_resource_facts(f);
    auto ev = g.evaluate(TailObjectiveId(1));
    REQUIRE(ev.is_ok());
    CHECK(ev->causes[0].category == TailCauseCategory::PREEMPTION);
    CHECK(ev->selected_intervention == InterventionType::NO_ACTION);
    CHECK(ev->binding_hard_constraint == Infeasibility::NON_PREEMPTIBLE);
}

TEST(what_would_change_present) {
    auto clk = std::make_shared<TestClock>(1'000'000'000);
    TailGovernor g = mk_gov(mk_policy({mk_p99_objective(10'000'000, TailObjectiveId(1))}), clk);
    feed_phased(g, 1'000'000'000, RequestClassId(0), 200, Phase::QUEUE_WAIT, 12'000'000, 1'000'000);
    auto ev = g.evaluate(TailObjectiveId(1));
    REQUIRE(ev.is_ok());
    CHECK(!ev->what_would_change.empty());
    bool has_queue = false;
    for (const auto& w : ev->what_would_change) if (w.trigger == WhatWouldChange::QUEUE_DEPTH_DROPS) has_queue = true;
    CHECK(has_queue);
}

TEST(cooldown_blocks_repeat) {
    auto pol = mk_policy({mk_p99_objective(10'000'000, TailObjectiveId(1))});
    pol.objectives[0].cool_down = Duration(1'000'000'000);
    auto clk = std::make_shared<TestClock>(1'000'000'000);
    TailGovernor g = mk_gov(pol, clk);
    auto ev = g.evaluate(TailObjectiveId(1));
    REQUIRE(ev.is_ok());
    Intervention p1; p1.type = InterventionType::QUEUE_SHAPE; p1.objective = TailObjectiveId(1);
    p1.objective_generation = TailObjectiveGeneration(1); p1.authority = ev->authority; p1.status = InterventionStatus::PROPOSED;
    auto a1 = g.authorize(p1); REQUIRE(a1.is_ok());
    Intervention p2 = p1;
    auto a2 = g.authorize(p2);
    CHECK(!a2.is_ok());
}

TEST(hysteresis_recovery) {
    auto clk = std::make_shared<TestClock>(1'000'000'000);
    TailGovernor g = mk_gov(mk_policy({mk_p99_objective(10'000'000, TailObjectiveId(1))}), clk);
    std::vector<std::int64_t> slow(200, 12'000'000);
    feed(g, WorkerId(1), WorkerBootId(1), 1'000'000'000, RequestClassId(0), 200, slow, CoordinatorEpoch(1), 1);
    auto e1 = g.evaluate(TailObjectiveId(1)); REQUIRE(e1.is_ok()); CHECK(e1->state == TailState::VIOLATING);
    clk->set(1'000'000'000 + 6'000'000'000LL);
    std::vector<std::int64_t> fast(200, 2'000'000);
    feed(g, WorkerId(1), WorkerBootId(1), 1'000'000'000 + 6'000'000'000LL, RequestClassId(0), 200, fast, CoordinatorEpoch(1), 2000);
    auto e2 = g.evaluate(TailObjectiveId(1)); REQUIRE(e2.is_ok()); CHECK(e2->state == TailState::RECOVERING);
    feed(g, WorkerId(1), WorkerBootId(1), 1'000'000'000 + 6'000'000'000LL + 2000, RequestClassId(0), 200, fast, CoordinatorEpoch(1), 4000);
    auto e3 = g.evaluate(TailObjectiveId(1)); REQUIRE(e3.is_ok()); CHECK(e3->state == TailState::HEALTHY);
}

int main() { return tgtest::run_all(); }
