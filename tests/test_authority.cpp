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

TEST(authorize_lifecycle) {
    TailPolicy pol = mk_policy({mk_p99_objective(10'000'000, TailObjectiveId(1))});
    TailGovernor g = mk_gov(pol, 1'000'000'000);
    auto ev = g.evaluate(TailObjectiveId(1));
    REQUIRE(ev.is_ok());
    Intervention prop;
    prop.type = InterventionType::QUEUE_SHAPE;
    prop.objective = TailObjectiveId(1);
    prop.objective_generation = TailObjectiveGeneration(1);
    prop.authority = ev->authority;
    prop.status = InterventionStatus::PROPOSED;
    auto a = g.authorize(prop);
    REQUIRE(a.is_ok());
    CHECK(a->status == InterventionStatus::AUTHORIZED);
    CHECK(a->id != InterventionId(0));
    // dispatch + acknowledge + effective
    auto d = g.dispatch(a->id);
    REQUIRE(d.is_ok());
    CHECK(d->status == InterventionStatus::DISPATCHED);
    CHECK(g.acknowledge(a->id).ok());
    auto outcome = g.record_outcome(a->id, PostOutcome::EFFECTIVE);
    CHECK(outcome == PostOutcome::EFFECTIVE);
    CHECK(g.intervention_history().back().status == InterventionStatus::EFFECTIVE);
}

TEST(authorize_stale_authority) {
    TailPolicy pol = mk_policy({mk_p99_objective(10'000'000, TailObjectiveId(1))});
    TailGovernor g = mk_gov(pol, 1'000'000'000);
    auto ev = g.evaluate(TailObjectiveId(1));
    REQUIRE(ev.is_ok());
    Intervention prop;
    prop.type = InterventionType::QUEUE_SHAPE;
    prop.objective = TailObjectiveId(1);
    prop.objective_generation = TailObjectiveGeneration(1);
    prop.authority = ev->authority;
    prop.status = InterventionStatus::PROPOSED;
    g.advance_epoch();       // coordinator restart -> epoch advances
    auto a = g.authorize(prop);
    CHECK(!a.is_ok());
    CHECK(a.error().code == StatusCode::STALE_AUTHORITY);
}

TEST(authorize_infeasible) {
    TailPolicy pol = mk_policy({mk_p99_objective(10'000'000, TailObjectiveId(1))});
    TailGovernor g = mk_gov(pol, 1'000'000'000);
    auto ev = g.evaluate(TailObjectiveId(1));
    REQUIRE(ev.is_ok());
    Intervention prop;
    prop.type = InterventionType::WARM_RESIDENCY;
    prop.objective = TailObjectiveId(1);
    prop.objective_generation = TailObjectiveGeneration(1);
    prop.authority = ev->authority;
    prop.status = InterventionStatus::PROPOSED;
    // warm engine unavailable
    ResourceFacts f; f.warm_engine_ready = false; g.set_resource_facts(f);
    auto a = g.authorize(prop);
    CHECK(!a.is_ok());
    CHECK(a.error().code == StatusCode::INTERVENTION_INFEASIBLE);
}

TEST(predispatch_revalidation_supersede) {
    TailPolicy pol = mk_policy({mk_p99_objective(10'000'000, TailObjectiveId(1))});
    TailGovernor g = mk_gov(pol, 1'000'000'000);
    auto ev = g.evaluate(TailObjectiveId(1));
    REQUIRE(ev.is_ok());
    Intervention prop;
    prop.type = InterventionType::QUEUE_SHAPE;
    prop.objective = TailObjectiveId(1);
    prop.objective_generation = TailObjectiveGeneration(1);
    prop.authority = ev->authority;
    prop.status = InterventionStatus::PROPOSED;
    auto a = g.authorize(prop);
    REQUIRE(a.is_ok());
    // policy generation advances
    TailPolicy p2 = pol;
    p2.generation = TailPolicyGeneration(2);
    g.set_policy(p2);
    auto d = g.dispatch(a->id);
    CHECK(!d.is_ok());
    CHECK(d.error().code == StatusCode::STALE_AUTHORITY);
    // the intervention is now SUPERSEDED, not effective
    CHECK(g.intervention_history().back().status == InterventionStatus::SUPERSEDED);
}

TEST(execution_lost_ack_outcome_unknown) {
    TailPolicy pol = mk_policy({mk_p99_objective(10'000'000, TailObjectiveId(1))});
    TailGovernor g = mk_gov(pol, 1'000'000'000);
    auto ev = g.evaluate(TailObjectiveId(1));
    REQUIRE(ev.is_ok());
    Intervention prop; prop.type = InterventionType::QUEUE_SHAPE; prop.objective = TailObjectiveId(1);
    prop.objective_generation = TailObjectiveGeneration(1); prop.authority = ev->authority; prop.status = InterventionStatus::PROPOSED;
    auto a = g.authorize(prop); REQUIRE(a.is_ok());
    g.dispatch(a->id); g.acknowledge(a->id);
    auto o = g.record_outcome(a->id, PostOutcome::OUTCOME_UNKNOWN);
    CHECK(o == PostOutcome::OUTCOME_UNKNOWN);
    CHECK(g.intervention_history().back().status == InterventionStatus::OUTCOME_UNKNOWN);
}

TEST(cancel_vs_complete) {
    TailPolicy pol = mk_policy({mk_p99_objective(10'000'000, TailObjectiveId(1))});
    TailGovernor g = mk_gov(pol, 1'000'000'000);
    auto ev = g.evaluate(TailObjectiveId(1));
    REQUIRE(ev.is_ok());
    // build and authorize, dispatch, then cancel before ack
    Intervention prop; prop.type = InterventionType::QUEUE_SHAPE; prop.objective = TailObjectiveId(1);
    prop.objective_generation = TailObjectiveGeneration(1); prop.authority = ev->authority; prop.status = InterventionStatus::PROPOSED;
    auto a = g.authorize(prop); REQUIRE(a.is_ok());
    g.dispatch(a->id);
    CHECK(g.cancel(a->id).ok());
    CHECK(g.intervention_history().back().status == InterventionStatus::CANCELLED);
    // late completion on a cancelled action must not become success
    CHECK(g.record_outcome(a->id, PostOutcome::EFFECTIVE) == PostOutcome::OUTCOME_UNKNOWN);
    CHECK(g.intervention_history().back().status == InterventionStatus::CANCELLED);
}

TEST(cancel_past_irreversible_boundary) {
    TailPolicy pol = mk_policy({mk_p99_objective(10'000'000, TailObjectiveId(1))});
    TailGovernor g = mk_gov(pol, 1'000'000'000);
    auto ev = g.evaluate(TailObjectiveId(1));
    REQUIRE(ev.is_ok());
    Intervention prop; prop.type = InterventionType::QUEUE_SHAPE; prop.objective = TailObjectiveId(1);
    prop.objective_generation = TailObjectiveGeneration(1); prop.authority = ev->authority; prop.status = InterventionStatus::PROPOSED;
    auto a = g.authorize(prop); REQUIRE(a.is_ok());
    g.dispatch(a->id); g.acknowledge(a->id);
    // acknowledged = irreversible boundary; cancel is refused
    CHECK(g.cancel(a->id).code == StatusCode::INVALID_INPUT);
    CHECK(g.intervention_history().back().status == InterventionStatus::ACKNOWLEDGED);
}

TEST(shutdown_blocks_new_work) {
    TailPolicy pol = mk_policy({mk_p99_objective(10'000'000, TailObjectiveId(1))});
    TailGovernor g = mk_gov(pol, 1'000'000'000);
    g.begin_shutdown();
    RequestLatency r = mk_req(RequestId(1), 1'000'000'000, 1'000'004'000);
    CHECK(g.ingest(r).code == StatusCode::SHUTTING_DOWN);
    CHECK(!g.evaluate(TailObjectiveId(1)).is_ok());
}

int main() { return tgtest::run_all(); }
