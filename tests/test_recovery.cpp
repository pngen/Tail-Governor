#include "tg_test.hpp"
#include "tg_fixtures.hpp"

using namespace tailgovernor;

static TailGovernor mk_gov(TailPolicy pol, std::int64_t base, CoordinatorEpoch epoch = CoordinatorEpoch(1)) {
    GovernorConfig c = mk_config(base, epoch);
    TailGovernor g(c);
    g.set_policy(pol);
    g.register_worker(WorkerId(1), WorkerBootId(1));
    return g;
}

static const std::vector<std::int64_t> kLat = { 2'000'000, 2'000'000, 2'000'000, 2'000'000, 2'000'000, 2'000'000, 2'000'000, 2'000'000, 2'000'000, 12'000'000 };

TEST(coordinator_restart_revalidation) {
    TailPolicy pol = mk_policy({mk_p99_objective(10'000'000, TailObjectiveId(1))});
    TailGovernor g = mk_gov(pol, 1'000'000'000);
    feed(g, WorkerId(1), WorkerBootId(1), 1'000'000'000, RequestClassId(0), 200, kLat);
    auto ev = g.evaluate(TailObjectiveId(1));
    REQUIRE(ev.is_ok());
    CHECK(ev->state != TailState::REVALIDATION_REQUIRED);
    CHECK(ev->sufficiency == Sufficiency::SUFFICIENT);
    std::string path = "test_recovery_state.bin";
    CHECK(g.save(path).ok());
    // Coordinator restarts: fresh epoch, durable state reloaded, dynamic observations become stale.
    GovernorConfig c = mk_config(1'000'000'000, CoordinatorEpoch(7));
    TailGovernor g2(c);
    g2.set_policy(pol);
    g2.register_worker(WorkerId(1), WorkerBootId(1));
    CHECK(g2.load(path).ok());
    auto ev2 = g2.evaluate(TailObjectiveId(1));
    REQUIRE(ev2.is_ok());
    CHECK(ev2->state == TailState::REVALIDATION_REQUIRED);
    CHECK(ev2->sufficiency == Sufficiency::STALE);
    // old-epoch evidence rejects after restart
    RequestLatency oldEpoch = mk_req(RequestId(10001), 1'000'000'000, 1'000'004'000, RequestClassId(0), WorkerId(1), WorkerBootId(1), CoordinatorEpoch(1));
    CHECK(g2.ingest(oldEpoch).code == StatusCode::STALE_AUTHORITY);
    // survivors republish fresh current-epoch evidence -> revalidation clears
    feed(g2, WorkerId(1), WorkerBootId(1), 1'000'000'000, RequestClassId(0), 200, kLat, CoordinatorEpoch(7));
    auto ev3 = g2.evaluate(TailObjectiveId(1));
    REQUIRE(ev3.is_ok());
    CHECK(ev3->state != TailState::REVALIDATION_REQUIRED);
    CHECK(ev3->sufficiency == Sufficiency::SUFFICIENT);
    std::remove(path.c_str());
}

TEST(worker_boot_replay_rejected) {
    TailPolicy pol = mk_policy({mk_p99_objective(10'000'000, TailObjectiveId(1))});
    TailGovernor g = mk_gov(pol, 1'000'000'000);
    g.register_worker(WorkerId(1), WorkerBootId(1));
    feed(g, WorkerId(1), WorkerBootId(1), 1'000'000'000, RequestClassId(0), 50, kLat);
    // worker A dies/restarts: a new WorkerBootId is current
    g.register_worker(WorkerId(1), WorkerBootId(2));
    // stale traffic from the old boot is rejected
    RequestLatency stale = mk_req(RequestId(9099), 1'000'000'000, 1'000'004'000, RequestClassId(0), WorkerId(1), WorkerBootId(1), CoordinatorEpoch(1));
    CHECK(g.ingest(stale).code == StatusCode::STALE_AUTHORITY);
    // fresh boot-2 traffic is accepted
    RequestLatency fresh = mk_req(RequestId(9100), 1'000'000'000, 1'000'004'000, RequestClassId(0), WorkerId(1), WorkerBootId(2), CoordinatorEpoch(1));
    CHECK(g.ingest(fresh).ok());
}

TEST(stale_intervention_completion_after_policy_roll) {
    TailPolicy pol = mk_policy({mk_p99_objective(10'000'000, TailObjectiveId(1))});
    TailGovernor g = mk_gov(pol, 1'000'000'000);
    auto ev = g.evaluate(TailObjectiveId(1));
    REQUIRE(ev.is_ok());
    Intervention prop; prop.type = InterventionType::QUEUE_SHAPE; prop.objective = TailObjectiveId(1);
    prop.objective_generation = TailObjectiveGeneration(1); prop.authority = ev->authority; prop.status = InterventionStatus::PROPOSED;
    auto a = g.authorize(prop); REQUIRE(a.is_ok());
    // policy generation rolls before dispatch
    TailPolicy p2 = pol; p2.generation = TailPolicyGeneration(5); g.set_policy(p2);
    auto d = g.dispatch(a->id);
    CHECK(!d.is_ok());
    CHECK(d.error().code == StatusCode::STALE_AUTHORITY);
    // late completion cannot turn a superseded action into success
    CHECK(g.record_outcome(a->id, PostOutcome::EFFECTIVE) == PostOutcome::OUTCOME_UNKNOWN);
    CHECK(g.intervention_history().back().status == InterventionStatus::SUPERSEDED);
}

int main() { return tgtest::run_all(); }
