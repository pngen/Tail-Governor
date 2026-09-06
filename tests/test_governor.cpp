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

TEST(ingest_authority_fencing) {
    TailPolicy pol = mk_policy({mk_p99_objective(10'000'000, TailObjectiveId(1))});
    TailGovernor g = mk_gov(pol, 1'000'000'000);
    RequestLatency good = mk_req(RequestId(1), 1'000'000'000, 1'000'004'000, RequestClassId(0), WorkerId(1), WorkerBootId(1), CoordinatorEpoch(1));
    CHECK(g.ingest(good).ok());
    // stale epoch
    RequestLatency badEpoch = mk_req(RequestId(2), 1'000'000'000, 1'000'004'000, RequestClassId(0), WorkerId(1), WorkerBootId(1), CoordinatorEpoch(9));
    CHECK(g.ingest(badEpoch).code == StatusCode::STALE_AUTHORITY);
    // stale boot
    RequestLatency badBoot = mk_req(RequestId(3), 1'000'000'000, 1'000'004'000, RequestClassId(0), WorkerId(1), WorkerBootId(99), CoordinatorEpoch(1));
    CHECK(g.ingest(badBoot).code == StatusCode::STALE_AUTHORITY);
    // unregistered worker
    RequestLatency noWorker = mk_req(RequestId(4), 1'000'000'000, 1'000'004'000, RequestClassId(0), WorkerId(77), WorkerBootId(1), CoordinatorEpoch(1));
    CHECK(g.ingest(noWorker).code == StatusCode::STALE_AUTHORITY);
}

TEST(authoritative_completion_only) {
    TailPolicy pol = mk_policy({mk_p99_objective(10'000'000, TailObjectiveId(1))});
    TailGovernor g = mk_gov(pol, 1'000'000'000);
    // ambiguous completion is not authoritative
    RequestLatency amb = mk_req(RequestId(5), 1'000'000'000, 1'000'004'000, RequestClassId(0), WorkerId(1), WorkerBootId(1), CoordinatorEpoch(1), 0, {}, RequestStatus::AMBIGUOUS);
    CHECK(g.ingest(amb).code == StatusCode::INVALID_INPUT);
    // timeout is not counted as success evidence
    RequestLatency to = mk_req(RequestId(6), 1'000'000'000, 1'000'004'000, RequestClassId(0), WorkerId(1), WorkerBootId(1), CoordinatorEpoch(1), 0, {}, RequestStatus::TIMEOUT);
    CHECK(g.ingest(to).code == StatusCode::INVALID_INPUT);
    // success is accepted
    RequestLatency ok = mk_req(RequestId(7), 1'000'000'000, 1'000'004'000, RequestClassId(0), WorkerId(1), WorkerBootId(1), CoordinatorEpoch(1));
    CHECK(g.ingest(ok).ok());
}

TEST(duplicate_completion_rejected) {
    TailPolicy pol = mk_policy({mk_p99_objective(10'000'000, TailObjectiveId(1))});
    TailGovernor g = mk_gov(pol, 1'000'000'000);
    RequestLatency r = mk_req(RequestId(10), 1'000'000'000, 1'000'004'000, RequestClassId(0), WorkerId(1), WorkerBootId(1), CoordinatorEpoch(1));
    CHECK(g.ingest(r).ok());
    CHECK(g.ingest(r).code == StatusCode::INVALID_INPUT);  // duplicate completion must not double count
}

TEST(phase_reconciliation) {
    TailPolicy pol = mk_policy({mk_p99_objective(10'000'000, TailObjectiveId(1))});
    TailGovernor g = mk_gov(pol, 1'000'000'000);
    std::vector<PhaseLatency> spill = { {Phase::QUEUE_WAIT, Duration(10'000'000)}, {Phase::EXECUTION, Duration(5'000'000)} };
    RequestLatency r = mk_req(RequestId(20), 1'000'000'000, 1'008'000'000, RequestClassId(0), WorkerId(1), WorkerBootId(1), CoordinatorEpoch(1), 0, spill);
    // phases sum 15ms > total 8ms => reject impossible phase sum
    CHECK(g.ingest(r).code == StatusCode::INVALID_INPUT);
    std::vector<PhaseLatency> ok = { {Phase::QUEUE_WAIT, Duration(3'000'000)}, {Phase::EXECUTION, Duration(4'000'000)} };
    RequestLatency okr = mk_req(RequestId(21), 1'000'000'000, 1'008'000'000, RequestClassId(0), WorkerId(1), WorkerBootId(1), CoordinatorEpoch(1), 0, ok);
    CHECK(g.ingest(okr).ok());
}

TEST(stable_p50_bad_p99) {
    // 190 fast (~2ms) + 10 slow (~15ms); p99 breaches the 10ms target while p50 stays healthy.
    TailPolicy pol = mk_policy({mk_p99_objective(10'000'000, TailObjectiveId(1))});
    TailGovernor g = mk_gov(pol, 1'000'000'000);
    for (int i = 0; i < 200; ++i) {
        std::int64_t lat = (i >= 190) ? 15'000'000 : 2'000'000;
        RequestLatency r = mk_req(RequestId(i + 1), 1'000'000'000 + i * 10, 1'000'000'000 + i * 10 + lat,
            RequestClassId(0), WorkerId(1), WorkerBootId(1), CoordinatorEpoch(1), 0,
            std::vector<PhaseLatency>{{Phase::QUEUE_WAIT, Duration(lat - 2'000'000)}, {Phase::EXECUTION, Duration(2'000'000)}});
        g.ingest(r);
    }
    auto ev = g.evaluate(TailObjectiveId(1));
    REQUIRE(ev.is_ok());
    CHECK(ev->state == TailState::VIOLATING);
    CHECK(ev->observed.nanos() > 10'000'000);
    CHECK(ev->p50.has_value());
    CHECK(ev->p50->nanos() < 10'000'000);
    CHECK(ev->sufficiency == Sufficiency::SUFFICIENT);
    CHECK(ev->selected_intervention != InterventionType::NO_ACTION);
}

TEST(healthy_p99_catastrophic_p999) {
    // 995 fast samples (~2ms) + 5 huge (~500ms). p99 stays healthy while p999 is catastrophic.
    std::vector<TailObjective> objs;
    objs.push_back(mk_p99_objective(10'000'000, TailObjectiveId(1)));
    auto p999 = mk_p999_objective(10'000'000, TailObjectiveId(3));
    p999.min_samples = 1000;   // p999 needs an honest sample base
    objs.push_back(p999);
    TailPolicy pol = mk_policy(std::move(objs));
    TailGovernor g = mk_gov(pol, 1'000'000'000);
    std::vector<std::int64_t> vals;
    for (int i = 0; i < 995; ++i) vals.push_back(2'000'000);
    for (int i = 0; i < 5; ++i) vals.push_back(500'000'000);
    feed(g, WorkerId(1), WorkerBootId(1), 1'000'000'000, RequestClassId(0), 1000, vals);
    auto ev99 = g.evaluate(TailObjectiveId(1));
    REQUIRE(ev99.is_ok());
    CHECK(ev99->state == TailState::HEALTHY);
    CHECK(ev99->observed.nanos() <= 10'000'000);
    auto ev999 = g.evaluate(TailObjectiveId(3));
    REQUIRE(ev999.is_ok());
    CHECK(ev999->state == TailState::VIOLATING || ev999->state == TailState::SEVERE);
    CHECK(ev999->observed.nanos() > 10'000'000);
    CHECK(ev999->amplification.p999_over_p50.has_value());
    CHECK(*ev999->amplification.p999_over_p50 > 10.0);
}

int main() { return tgtest::run_all(); }
