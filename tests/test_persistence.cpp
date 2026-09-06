#include "tg_test.hpp"
#include "tg_fixtures.hpp"
#include <fstream>
#include <vector>

using namespace tailgovernor;

static std::vector<char> read_file(const std::string& p) {
    std::ifstream is(p, std::ios::binary);
    return std::vector<char>((std::istreambuf_iterator<char>(is)), std::istreambuf_iterator<char>());
}
static void write_file(const std::string& p, const std::vector<char>& v) {
    std::ofstream os(p, std::ios::binary | std::ios::trunc);
    os.write(v.data(), static_cast<std::streamsize>(v.size()));
}

static TailGovernor build_gov(const std::string& path) {
    TailPolicy pol = mk_policy({mk_p99_objective(10'000'000, TailObjectiveId(1))});
    GovernorConfig c = mk_config(1'000'000'000);
    TailGovernor g(c);
    g.set_policy(pol);
    g.register_worker(WorkerId(1), WorkerBootId(1));
    feed(g, WorkerId(1), WorkerBootId(1), 1'000'000'000, RequestClassId(0), 200, std::vector<std::int64_t>(200, 2'000'000));
    auto ev = g.evaluate(TailObjectiveId(1));
    if (ev.is_ok()) {
        // authorize a real intervention so history is persisted
        Intervention prop; prop.type = InterventionType::QUEUE_SHAPE; prop.objective = TailObjectiveId(1);
        prop.objective_generation = TailObjectiveGeneration(1); prop.authority = ev->authority; prop.status = InterventionStatus::PROPOSED;
        g.authorize(prop);
    }
    g.save(path);
    return g;
}

TEST(persistence_roundtrip) {
    std::string path = "test_persist.bin";
    TailGovernor g = build_gov(path);
    CHECK(g.policy().objectives.size() == 1);
    // restart coordinator with a fresh epoch
    GovernorConfig c = mk_config(1'000'000'000, CoordinatorEpoch(3));
    TailGovernor g2(c);
    CHECK(g2.load(path).ok());
    CHECK(g2.policy().objectives.size() == 1);
    CHECK(g2.epoch() == CoordinatorEpoch(3));
    CHECK(g2.intervention_history().size() >= 1);
    std::remove(path.c_str());
}

TEST(persistence_deterministic) {
    std::string a = "test_persist_a.bin", b = "test_persist_b.bin";
    TailGovernor g = build_gov(a);
    g.save(b);
    CHECK(read_file(a) == read_file(b));
    std::remove(a.c_str()); std::remove(b.c_str());
}

TEST(persistence_corruption) {
    std::string path = "test_persist_c.bin";
    TailGovernor g = build_gov(path);
    auto v = read_file(path);
    // flip a byte in the body (after 20-byte header)
    CHECK(v.size() > 21);
    v[20] = static_cast<char>(v[20] ^ 0x55);
    std::string bad = "test_persist_c_bad.bin";
    write_file(bad, v);
    GovernorConfig c = mk_config(1'000'000'000, CoordinatorEpoch(3));
    TailGovernor g2(c);
    CHECK(g2.load(bad).code == StatusCode::PERSISTENCE_CORRUPT);
    std::remove(path.c_str()); std::remove(bad.c_str());
}

TEST(persistence_truncation) {
    std::string path = "test_persist_t.bin";
    TailGovernor g = build_gov(path);
    auto v = read_file(path);
    v.resize(v.size() - 6);
    std::string bad = "test_persist_t_bad.bin";
    write_file(bad, v);
    GovernorConfig c = mk_config(1'000'000'000, CoordinatorEpoch(3));
    TailGovernor g2(c);
    CHECK(g2.load(bad).code == StatusCode::PERSISTENCE_CORRUPT);
    std::remove(path.c_str()); std::remove(bad.c_str());
}

TEST(persistence_trailing_garbage) {
    std::string path = "test_persist_g.bin";
    TailGovernor g = build_gov(path);
    auto v = read_file(path);
    v.push_back('x'); v.push_back('y'); v.push_back('z');
    std::string bad = "test_persist_g_bad.bin";
    write_file(bad, v);
    GovernorConfig c = mk_config(1'000'000'000, CoordinatorEpoch(3));
    TailGovernor g2(c);
    CHECK(g2.load(bad).code == StatusCode::PERSISTENCE_CORRUPT);
    std::remove(path.c_str()); std::remove(bad.c_str());
}

TEST(persistence_unknown_version) {
    std::string path = "test_persist_v.bin";
    TailGovernor g = build_gov(path);
    auto v = read_file(path);
    // version field at bytes 4..7; set to 999
    v[4] = static_cast<char>(0xE7); v[5] = static_cast<char>(0x03); v[6] = 0; v[7] = 0;   // version = 999
    std::string bad = "test_persist_v_bad.bin";
    write_file(bad, v);
    GovernorConfig c = mk_config(1'000'000'000, CoordinatorEpoch(3));
    TailGovernor g2(c);
    CHECK(g2.load(bad).code == StatusCode::PERSISTENCE_CORRUPT);
    std::remove(path.c_str()); std::remove(bad.c_str());
}

TEST(persistence_bad_magic) {
    std::string path = "test_persist_m.bin";
    TailGovernor g = build_gov(path);
    auto v = read_file(path);
    v[0] = 0x00; v[1] = 0x00; v[2] = 0x00; v[3] = 0x00;
    std::string bad = "test_persist_m_bad.bin";
    write_file(bad, v);
    GovernorConfig c = mk_config(1'000'000'000, CoordinatorEpoch(3));
    TailGovernor g2(c);
    CHECK(g2.load(bad).code == StatusCode::PERSISTENCE_CORRUPT);
    std::remove(path.c_str()); std::remove(bad.c_str());
}

int main() { return tgtest::run_all(); }
