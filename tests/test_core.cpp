#include "tg_test.hpp"
#include "tailgovernor/tailgovernor.hpp"

using namespace tailgovernor;

static bool canonical_bytes_eq(const Id<RequestIdTag>& x, const Id<RequestIdTag>& y) {
    return x.canonical_bytes() == y.canonical_bytes();
}

TEST(identity_deterministic) {
    RequestId a(12345), b(12345), c(67890);
    CHECK(a == b);
    CHECK(a != c);
    CHECK(a.value() == 12345);
    CHECK(a.to_string() == b.to_string());
    CHECK(canonical_bytes_eq(a, b));
    CHECK(b < c);
    CHECK_EQ(a.to_string(), std::string("req#0000000000003039"));
}

TEST(duration_validation) {
    auto d = Duration::from_nanos(1000);
    CHECK(d.is_ok());
    CHECK_EQ(d->nanos(), 1000LL);
    auto neg = Duration::from_nanos(-5);
    CHECK(!neg.is_ok());
    CHECK(neg.error().code == StatusCode::INVALID_INPUT);
    CHECK(Duration::from_nanos(0)->is_zero());
}

TEST(percentile_exact) {
    std::vector<Duration> v = { Duration(1), Duration(2), Duration(3), Duration(4), Duration(5), Duration(6), Duration(7), Duration(8), Duration(9), Duration(10) };
    // nearest-rank for p50 on 10 samples: rank=ceil(0.5*10)=5 -> value 5
    auto q50 = exact_quantile(v, Percentile(50.0));
    CHECK(q50.is_ok());
    CHECK_EQ(q50->nanos(), 5LL);
    auto q90 = exact_quantile(v, Percentile(90.0));
    CHECK_EQ(q90->nanos(), 9LL);
    auto q99 = exact_quantile(v, Percentile(99.0));
    CHECK_EQ(q99->nanos(), 10LL);
    // p95 <= p99 <= p999 invariant
    auto q95 = exact_quantile(v, Percentile(95.0));
    auto q999 = exact_quantile(v, Percentile(99.9));
    CHECK(q95->nanos() <= q99->nanos());
    CHECK(q99->nanos() <= q999->nanos());
    // empty rejects
    std::vector<Duration> e;
    auto qe = exact_quantile(e, Percentile(99.0));
    CHECK(!qe.is_ok());
    CHECK(qe.error().code == StatusCode::INSUFFICIENT_EVIDENCE);
}

TEST(percentile_duplicates_boundaries) {
    std::vector<Duration> v = { Duration(7), Duration(7), Duration(7), Duration(7), Duration(7), Duration(7), Duration(7), Duration(7), Duration(7), Duration(7) };
    auto q99 = exact_quantile(v, Percentile(99.0));
    CHECK_EQ(q99->nanos(), 7LL);
    // 99 samples: p99 rank = ceil(0.99*99)=ceil(98.01)=99 -> max
    std::vector<Duration> w; for (int i=0;i<99;++i) w.push_back(Duration(i+1));
    auto q = exact_quantile(w, Percentile(99.0));
    CHECK_EQ(q->nanos(), 99LL);
    // 100 samples p99 rank = ceil(99)=99 -> value 99 (the 99th)
    std::vector<Duration> u; for (int i=0;i<100;++i) u.push_back(Duration(i+1));
    auto q2 = exact_quantile(u, Percentile(99.0));
    CHECK_EQ(q2->nanos(), 99LL);
}

TEST(min_samples_rule) {
    CHECK_EQ(derived_min_samples(Percentile(95.0)), 20ULL);
    CHECK_EQ(derived_min_samples(Percentile(99.0)), 100ULL);
    CHECK_EQ(derived_min_samples(Percentile(99.9)), 1000ULL);
    // 10 samples -> p999 is flagged insufficient
    ObservationWindow w;
    for (int i = 0; i < 10; ++i) w.insert(TailSample{Duration(i + 1), 1000 + i, 1000 + i, {}, RequestClassId(0), WorkerBootId(1), CoordinatorEpoch(0), 0, Phase::OTHER});
    auto r = w.percentile(Percentile(99.9), derived_min_samples(Percentile(99.9)), 2000);
    CHECK(r.is_ok());
    CHECK(r->second == Sufficiency::INSUFFICIENT_SAMPLES);
    // 1000 samples -> p999 sufficient
    ObservationWindow w2;
    for (int i = 0; i < 1000; ++i) w2.insert(TailSample{Duration(i + 1), 1000 + i, 1000 + i, {}, RequestClassId(0), WorkerBootId(1), CoordinatorEpoch(0), 0, Phase::OTHER});
    auto r2 = w2.percentile(Percentile(99.9), derived_min_samples(Percentile(99.9)), 5000);
    CHECK(r2.is_ok());
    CHECK(r2->second == Sufficiency::SUFFICIENT);
}

TEST(window_pruning_time) {
    ObservationWindow w;
    WindowConfig cfg; cfg.mode = WindowMode::ROLLING_TIME; cfg.window_duration = Duration(1000);
    w.set_config(cfg);
    for (int i = 0; i < 5; ++i) w.insert(TailSample{Duration(i + 1), 100 + i, 100 + i, {}, RequestClassId(0), WorkerBootId(1), CoordinatorEpoch(0), 0, Phase::OTHER});
    CHECK_EQ(w.size(), 5u);
    auto removed = w.prune(100);
    CHECK_EQ(removed, 0u);
    w.prune(1050);
    CHECK_EQ(w.size(), 5u);
    // remove those captured < 1080 -> only 104 kept? cutoff=80, keep captured>=80
    w.prune(1080);
    auto removed2 = w.prune(2000);
    CHECK_EQ(removed2, 5u);
    CHECK_EQ(w.size(), 0u);
}

TEST(window_out_of_order_equal) {
    ObservationWindow w;
    w.insert(TailSample{Duration(5), 50, 0, {}, RequestClassId(0), WorkerBootId(1), CoordinatorEpoch(0), 0, Phase::OTHER});
    w.insert(TailSample{Duration(9), 40, 0, {}, RequestClassId(0), WorkerBootId(1), CoordinatorEpoch(0), 0, Phase::OTHER});
    w.insert(TailSample{Duration(7), 50, 0, {}, RequestClassId(0), WorkerBootId(1), CoordinatorEpoch(0), 0, Phase::OTHER});
    w.insert(TailSample{Duration(3), 60, 0, {}, RequestClassId(0), WorkerBootId(1), CoordinatorEpoch(0), 0, Phase::OTHER});
    // out-of-order insertion; sorted must be deterministic
    const auto& s = w.sorted(100);
    CHECK_EQ(s.size(), 4u);
    CHECK_EQ(s[0].nanos(), 3LL);
    CHECK_EQ(s[1].nanos(), 5LL);
    CHECK_EQ(s[2].nanos(), 7LL);
    CHECK_EQ(s[3].nanos(), 9LL);
}

TEST(amplification) {
    auto a = compute_amplification(Duration(10), Duration(20), Duration(50), Duration(100), Duration(15));
    CHECK(a.p99_over_p50.has_value());
    CHECK(a.p999_over_p50.has_value());
    CHECK(a.p999_over_p95.has_value());
    CHECK(*a.p99_over_p50 > 1.0);
    CHECK(*a.p999_over_p50 > *a.p99_over_p50);
    auto zero = compute_amplification(Duration(0), std::nullopt, std::nullopt, std::nullopt, std::nullopt);
    CHECK(!zero.p99_over_p50.has_value());
}

TEST(phase_cause_mapping) {
    CHECK(phase_to_cause(Phase::QUEUE_WAIT) == TailCauseCategory::QUEUEING);
    CHECK(phase_to_cause(Phase::BATCH_WAIT) == TailCauseCategory::BATCHING);
    CHECK(phase_to_cause(Phase::ENGINE_WARMUP) == TailCauseCategory::COLD_RESIDENCY);
    CHECK(phase_to_cause(Phase::TRANSFER) == TailCauseCategory::TRANSFER);
    CHECK(phase_to_cause(Phase::MEMORY_RECLAIM) == TailCauseCategory::MEMORY_PRESSURE);
    CHECK(phase_to_cause(Phase::RETRY_DELAY) == TailCauseCategory::RETRY);
    CHECK(phase_to_cause(Phase::OTHER) == TailCauseCategory::UNKNOWN);
}

int main() { return tgtest::run_all(); }
