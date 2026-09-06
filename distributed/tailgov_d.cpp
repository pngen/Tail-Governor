#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "tailgovernor/tailgovernor.hpp"
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
#include <thread>
#include <map>

#pragma comment(lib, "ws2_32.lib")

using namespace tailgovernor;

namespace {

constexpr std::uint32_t kFrameMagic = 0x5447564Eu;  // TGVN
constexpr std::uint16_t kFrameVersion = 1u;
constexpr std::uint32_t kMaxFrame = 512u * 1024u;   // 512 KiB
constexpr std::size_t kHeaderSize = 4 + 2 + 4 + 4;  // magic, ver, len, crc

std::int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::uint32_t crc32(const std::uint8_t* d, std::size_t n) {
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < n; ++i) { crc ^= d[i]; for (int b = 0; b < 8; ++b) { std::uint32_t m = static_cast<std::uint32_t>(-(static_cast<std::int32_t>(crc & 1u))); crc = (crc >> 1) ^ (0xEDB88320u & m); } }
    return crc ^ 0xFFFFFFFFu;
}

class Winsock {
public:
    Winsock() { WSAStartup(MAKEWORD(2, 2), &wsadata_); }
    ~Winsock() { WSACleanup(); }
    bool ok() const { return wsadata_.wVersion == MAKEWORD(2, 2); }
private:
    WSADATA wsadata_{};
};

struct Sock {
    SOCKET fd = INVALID_SOCKET;
    ~Sock() { if (fd != INVALID_SOCKET) closesocket(fd); }
    bool valid() const { return fd != INVALID_SOCKET; }
};

bool send_all(SOCKET s, const std::uint8_t* p, std::size_t n) {
    std::size_t off = 0;
    while (off < n) {
        int r = ::send(s, reinterpret_cast<const char*>(p + off), static_cast<int>(n - off), 0);
        if (r <= 0) return false;
        off += static_cast<std::size_t>(r);
    }
    return true;
}

bool recv_exact(SOCKET s, std::uint8_t* p, std::size_t n) {
    std::size_t off = 0;
    while (off < n) {
        int r = ::recv(s, reinterpret_cast<char*>(p + off), static_cast<int>(n - off), 0);
        if (r <= 0) return false;
        off += static_cast<std::size_t>(r);
    }
    return true;
}

bool send_frame(SOCKET s, const std::vector<std::uint8_t>& payload, std::uint32_t& send_err) {
    if (payload.size() > kMaxFrame) { send_err = 1; return false; }
    std::vector<std::uint8_t> hdr;
    auto put32 = [&hdr](std::uint32_t v) { for (int i = 0; i < 4; ++i) hdr.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF)); };
    auto put16 = [&hdr](std::uint16_t v) { for (int i = 0; i < 2; ++i) hdr.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF)); };
    put32(kFrameMagic); put16(kFrameVersion); put32(static_cast<std::uint32_t>(payload.size()));
    put32(crc32(payload.data(), payload.size()));
    if (!send_all(s, hdr.data(), hdr.size())) { send_err = 2; return false; }
    if (!payload.empty() && !send_all(s, payload.data(), payload.size())) { send_err = 3; return false; }
    return true;
}

bool recv_frame(SOCKET s, std::vector<std::uint8_t>& payload) {
    std::uint8_t hdr[kHeaderSize];
    if (!recv_exact(s, hdr, kHeaderSize)) return false;
    auto rd32 = [&hdr](int off) { std::uint32_t v = 0; for (int i = 0; i < 4; ++i) v |= (static_cast<std::uint32_t>(hdr[off + i]) << (8 * i)); return v; };
    auto rd16 = [&hdr](int off) { std::uint16_t v = 0; for (int i = 0; i < 2; ++i) v |= (static_cast<std::uint16_t>(hdr[off + i]) << (8 * i)); return v; };
    if (rd32(0) != kFrameMagic) return false;
    if (rd16(4) != kFrameVersion) return false;
    std::uint32_t len = rd32(6);
    if (len > kMaxFrame) return false;
    std::uint32_t crc = rd32(10);
    payload.resize(len);
    if (len > 0 && !recv_exact(s, payload.data(), len)) return false;
    if (crc32(payload.data(), payload.size()) != crc) return false;
    return true;
}

enum class Msg : std::uint8_t {
    HELLO = 1, LATENCY = 2, INTERVENTION = 3, INTERVENTION_ACK = 4,
    REPORT = 5, OUTCOME = 6, SHUTDOWN = 7, RESTART = 8
};

struct Buf { std::vector<std::uint8_t> v; };
void put_u8(Buf& b, std::uint8_t x) { b.v.push_back(x); }
void put_u32(Buf& b, std::uint32_t x) { for (int i = 0; i < 4; ++i) b.v.push_back(static_cast<std::uint8_t>((x >> (8 * i)) & 0xFF)); }
void put_u64(Buf& b, std::uint64_t x) { for (int i = 0; i < 8; ++i) b.v.push_back(static_cast<std::uint8_t>((x >> (8 * i)) & 0xFF)); }
void put_i64(Buf& b, std::int64_t x) { put_u64(b, static_cast<std::uint64_t>(x)); }
void put_str(Buf& b, const std::string& s) { put_u32(b, static_cast<std::uint32_t>(s.size())); b.v.insert(b.v.end(), s.begin(), s.end()); }

struct Rd { const std::uint8_t* p; std::size_t n; std::size_t pos = 0; bool ok = true;
    bool take(std::size_t k) { if (pos + k > n) { ok = false; return false; } pos += k; return true; }
    std::uint8_t u8() { if (!take(1)) return 0; return p[pos - 1]; }
    std::uint32_t u32() { if (!take(4)) return 0; std::uint32_t x = 0; for (int i = 0; i < 4; ++i) x |= (static_cast<std::uint32_t>(p[pos - 4 + i]) << (8 * i)); return x; }
    std::uint64_t u64() { if (!take(8)) return 0; std::uint64_t x = 0; for (int i = 0; i < 8; ++i) x |= (static_cast<std::uint64_t>(p[pos - 8 + i]) << (8 * i)); return x; }
    std::int64_t i64() { return static_cast<std::int64_t>(u64()); }
    std::string str() { std::uint32_t len = u32(); if (!ok || len > 65536u) { ok = false; return {}; } if (!take(len)) { ok = false; return {}; } return std::string(reinterpret_cast<const char*>(p + pos - len), len); }
};

void serialize_hello(Buf& b, const std::string& name, WorkerId wk, WorkerBootId boot, std::uint64_t epoch) {
    put_u8(b, static_cast<std::uint8_t>(Msg::HELLO));
    put_str(b, name); put_u64(b, wk.value()); put_u64(b, boot.value()); put_u64(b, epoch);
}
struct HelloInfo { std::string name; WorkerId wk; WorkerBootId boot; std::uint64_t epoch; };
HelloInfo deserialize_hello(const std::uint8_t* p, std::size_t n) {
    Rd r{p, n, 0, true};
    HelloInfo h; h.name = r.str(); h.wk = WorkerId(r.u64()); h.boot = WorkerBootId(r.u64()); h.epoch = r.u64();
    return h;
}

void serialize_latency(Buf& b, const RequestLatency& r) {
    put_u8(b, static_cast<std::uint8_t>(Msg::LATENCY));
    put_u64(b, r.request_id.value()); put_u64(b, r.request_class.value());
    put_u64(b, r.worker.value()); put_u64(b, r.worker_boot.value()); put_u64(b, r.epoch.value());
    put_u64(b, r.service_generation.value()); put_u64(b, r.workload_generation.value());
    put_i64(b, r.arrival_ns); put_i64(b, r.completion_ns); put_i64(b, r.publication_ns);
    put_i64(b, r.captured_ns); put_u64(b, static_cast<std::uint64_t>(r.total_latency.nanos()));
    put_u32(b, static_cast<std::uint32_t>(r.status));
    put_u32(b, r.retry_count);
    put_u32(b, static_cast<std::uint32_t>(r.phases.size()));
    for (const auto& p : r.phases) { put_u32(b, static_cast<std::uint32_t>(p.phase)); put_i64(b, p.duration.nanos()); }
}

bool deserialize_latency(std::vector<std::uint8_t>& body, RequestLatency& r) {
    Rd rd{body.data(), body.size(), 1, true};
    r.request_id = RequestId(rd.u64()); r.request_class = RequestClassId(rd.u64());
    r.worker = WorkerId(rd.u64()); r.worker_boot = WorkerBootId(rd.u64()); r.epoch = CoordinatorEpoch(rd.u64());
    r.service_generation = ServiceGeneration(rd.u64()); r.workload_generation = WorkloadGeneration(rd.u64());
    r.arrival_ns = rd.i64(); r.completion_ns = rd.i64(); r.publication_ns = rd.i64();
    r.captured_ns = rd.i64(); r.total_latency = Duration(rd.i64());
    r.status = static_cast<RequestStatus>(rd.u32()); r.retry_count = rd.u32();
    std::uint32_t nphase = rd.u32();
    if (nphase > 64u) return false;
    for (std::uint32_t i = 0; i < nphase; ++i) { PhaseLatency pl; pl.phase = static_cast<Phase>(rd.u32()); pl.duration = Duration(rd.i64()); r.phases.push_back(pl); }
    return rd.ok;
}

const char* RESULT_OK = "RESULT: PASS";
const char* RESULT_FAIL = "RESULT: FAIL";

SOCKET tcp_connect(const std::string& host, int port) {
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return INVALID_SOCKET;
    sockaddr_in sa{}; sa.sin_family = AF_INET; sa.sin_port = htons(static_cast<u_short>(port));
    inet_pton(AF_INET, host.c_str(), &sa.sin_addr);
    if (::connect(s, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) == SOCKET_ERROR) { closesocket(s); return INVALID_SOCKET; }
    return s;
}

bool send_msg(SOCKET s, Buf& b) { std::uint32_t e = 0; return send_frame(s, b.v, e); }
bool recv_msg(SOCKET s, std::vector<std::uint8_t>& out) { return recv_frame(s, out); }
std::uint8_t msg_type(const std::vector<std::uint8_t>& body) { return body.empty() ? 0 : body[0]; }

// Spawn a child worker process via the same executable.
struct Child {
    HANDLE proc = nullptr;
    DWORD pid = 0;
    ~Child() { if (proc) { TerminateProcess(proc, 0); CloseHandle(proc); } }
};

bool spawn_worker(const char* name, const char* host, int port, std::uint64_t boot, Child& out) {
    char exe[MAX_PATH]; GetModuleFileNameA(nullptr, exe, MAX_PATH);
    char cmd[1024];
    std::snprintf(cmd, sizeof(cmd), "\"%s\" worker %s %s %d %llu", exe, name, host, port, static_cast<unsigned long long>(boot));
    STARTUPINFOA si{}; si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    BOOL r = CreateProcessA(nullptr, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    if (!r) return false;
    out.proc = pi.hProcess;
    out.pid = static_cast<DWORD>(pi.dwProcessId);
    CloseHandle(pi.hThread);
    return true;
}

int run_worker(const std::string& name, const std::string& host, int port, std::uint64_t boot) {
    Winsock ws; if (!ws.ok()) return 2;
    Sock s; s.fd = tcp_connect(host, port);
    if (!s.valid()) { std::fprintf(stderr, "worker %s connect failed\n", name.c_str()); return 1; }
    WorkerId wk = (name == "A") ? WorkerId(100) : WorkerId(200);
    Buf hello; serialize_hello(hello, name, wk, WorkerBootId(boot), 1);
    if (!send_msg(s.fd, hello)) return 1;
    std::int64_t counter = 0;
    for (;;) {
        std::vector<std::uint8_t> body;
        if (!recv_msg(s.fd, body)) return 1;
        std::uint8_t t = msg_type(body);
        if (t == static_cast<std::uint8_t>(Msg::SHUTDOWN)) return 0;
        if (t == static_cast<std::uint8_t>(Msg::REPORT)) {
            Rd r{body.data() + 1, body.size() - 1, 0, true};
            std::uint32_t count = r.u32();
            std::int64_t base = r.i64();
            std::int64_t qn = r.i64();
            std::int64_t ex = r.i64();
            if (count > 100000) return 1;
            for (std::uint32_t i = 0; i < count; ++i) {
                RequestLatency lr;
                lr.request_id = RequestId(static_cast<std::uint64_t>(wk.value()) * 1000000u + static_cast<std::uint64_t>(counter++));
                lr.request_class = RequestClassId(0);
                lr.worker = wk; lr.worker_boot = WorkerBootId(boot); lr.epoch = CoordinatorEpoch(1);
                lr.service_generation = ServiceGeneration(1); lr.workload_generation = WorkloadGeneration(1);
                lr.arrival_ns = base + static_cast<std::int64_t>(i) * 10;
                lr.completion_ns = lr.arrival_ns + qn + ex;
                lr.publication_ns = lr.completion_ns;
                lr.captured_ns = lr.publication_ns;
                lr.total_latency = Duration(qn + ex);
                lr.status = RequestStatus::SUCCESS;
                lr.retry_count = 0;
                if (qn > 0) lr.phases.push_back({Phase::QUEUE_WAIT, Duration(qn)});
                if (ex > 0) lr.phases.push_back({Phase::EXECUTION, Duration(ex)});
                Buf b; serialize_latency(b, lr);
                if (!send_msg(s.fd, b)) return 1;
            }
        } else if (t == static_cast<std::uint8_t>(Msg::INTERVENTION)) {
            Buf ack; put_u8(ack, static_cast<std::uint8_t>(Msg::INTERVENTION_ACK));
            send_msg(s.fd, ack);
        } else {
            return 1;
        }
    }
}

std::vector<TailEvaluation> g_results;

RequestLatency mk_stale(WorkerId wk, WorkerBootId boot);

bool read_hello(SOCKET s, HelloInfo& h) {
    std::vector<std::uint8_t> body;
    if (!recv_frame(s, body)) return false;
    if (msg_type(body) != static_cast<std::uint8_t>(Msg::HELLO)) return false;
    h = deserialize_hello(body.data() + 1, body.size() - 1);
    return true;
}

std::uint64_t req_report(SOCKET s, std::uint64_t count, std::int64_t base_ns, std::int64_t qn, std::int64_t exn, TailGovernor& g) {
    Buf b; put_u8(b, static_cast<std::uint8_t>(Msg::REPORT));
    put_u32(b, static_cast<std::uint32_t>(count)); put_i64(b, base_ns); put_i64(b, qn); put_i64(b, exn);
    if (!send_msg(s, b)) return 0;
    std::uint64_t received = 0;
    for (std::uint64_t i = 0; i < count; ++i) {
        std::vector<std::uint8_t> body;
        if (!recv_msg(s, body)) break;
        if (msg_type(body) != static_cast<std::uint8_t>(Msg::LATENCY)) break;
        RequestLatency lr;
        if (!deserialize_latency(body, lr)) break;
        g.ingest(lr);
        ++received;
    }
    return received;
}

bool send_intervention(SOCKET s, InterventionType t) {
    Buf b; put_u8(b, static_cast<std::uint8_t>(Msg::INTERVENTION));
    put_u32(b, static_cast<std::uint32_t>(t));
    if (!send_msg(s, b)) return false;
    std::vector<std::uint8_t> ack;
    if (!recv_msg(s, ack)) return false;
    return msg_type(ack) == static_cast<std::uint8_t>(Msg::INTERVENTION_ACK);
}

TailGovernor make_gov() {
    GovernorConfig c;
    c.clock = std::make_shared<SystemClock>();
    c.epoch = CoordinatorEpoch(1);
    c.service = ServiceId(1);
    c.service_generation = ServiceGeneration(1);
    c.workload = WorkloadId(1);
    c.workload_generation = WorkloadGeneration(1);
    TailGovernor g(c);
    TailObjective obj;
    obj.id = TailObjectiveId(1);
    obj.generation = TailObjectiveGeneration(1);
    obj.percentile = Percentile(99.0);
    obj.target = Duration(10'000'000);
    obj.window.mode = WindowMode::ROLLING_COUNT;
    obj.window.fixed_count = 200;
    obj.window.max_samples = 400;
    obj.min_samples = 0;
    obj.freshness = Duration(30'000'000'000LL);
    obj.hard = true;
    obj.recovery_fraction = 0.90;
    obj.cool_down = Duration(0);
    obj.priority = 0;
    obj.enabled = true;
    TailPolicy pol;
    pol.id = TailPolicyId(1);
    pol.generation = TailPolicyGeneration(1);
    pol.name = "distributed policy";
    pol.objectives.push_back(obj);
    g.set_policy(pol);
    g.register_worker(WorkerId(100), WorkerBootId(1));
    g.register_worker(WorkerId(200), WorkerBootId(1));
    return g;
}

bool scenario_a(SOCKET a, SOCKET b, TailGovernor& g) {
    req_report(a, 200, now_ns(), 0, 2'000'000, g);
    req_report(b, 200, now_ns(), 12'000'000, 1'000'000, g);
    auto ev = g.evaluate(TailObjectiveId(1));
    if (!ev.is_ok() || ev->state != TailState::VIOLATING) return false;
    if (ev->causes.empty() || ev->causes[0].category != TailCauseCategory::QUEUEING) return false;
    if (!send_intervention(b, InterventionType::QUEUE_SHAPE)) return false;
    req_report(b, 200, now_ns(), 1'000'000, 1'000'000, g);
    auto after = g.evaluate(TailObjectiveId(1));
    if (!after.is_ok()) return false;
    return after->observed.nanos() <= 10'000'000 && after->state != TailState::VIOLATING;
}

RequestLatency mk_stale(WorkerId wk, WorkerBootId boot) {
    RequestLatency r;
    r.request_id = RequestId(111000 + wk.value());
    r.request_class = RequestClassId(0);
    r.worker = wk; r.worker_boot = boot; r.epoch = CoordinatorEpoch(1);
    r.service_generation = ServiceGeneration(1); r.workload_generation = WorkloadGeneration(1);
    r.arrival_ns = 5'000'000'000; r.completion_ns = 5'000'004'000; r.publication_ns = 5'000'004'000;
    r.captured_ns = 5'000'004'000;
    r.total_latency = Duration(4'000);
    r.status = RequestStatus::SUCCESS; r.retry_count = 0;
    return r;
}

bool scenario_b(SOCKET asock, TailGovernor& g, Child& a_child) {
    if (a_child.proc) { TerminateProcess(a_child.proc, 0); WaitForSingleObject(a_child.proc, INFINITE); }
    closesocket(asock);
    // Fresh authority: worker A comes back under a new WorkerBootId, fencing the old boot out.
    g.register_worker(WorkerId(100), WorkerBootId(2));
    if (g.ingest(mk_stale(WorkerId(100), WorkerBootId(1))).code != StatusCode::STALE_AUTHORITY) return false;
    if (g.ingest(mk_stale(WorkerId(100), WorkerBootId(2))).code != StatusCode::OK) return false;
    return true;
}

} // namespace

bool scenario_c() {
    TailGovernor g = make_gov();
    auto ev = g.evaluate(TailObjectiveId(1));
    if (!ev.is_ok()) return false;
    Intervention prop;
    prop.type = InterventionType::QUEUE_SHAPE;
    prop.objective = TailObjectiveId(1);
    prop.objective_generation = TailObjectiveGeneration(1);
    prop.authority = ev->authority;
    prop.status = InterventionStatus::PROPOSED;
    auto a = g.authorize(prop);
    if (!a.is_ok()) return false;
    TailPolicy p2 = g.policy();
    p2.generation = TailPolicyGeneration(2);
    g.set_policy(p2);
    auto d = g.dispatch(a->id);
    return !d.is_ok() && d.error().code == StatusCode::STALE_AUTHORITY;
}

bool scenario_d(const std::string& statefile) {
    TailGovernor g1 = make_gov();
    for (int i = 0; i < 200; ++i) {
        RequestLatency r = mk_stale(WorkerId(200), WorkerBootId(1));
        r.request_id = RequestId(50000 + i);
        r.arrival_ns = 1'000'000'000 + static_cast<std::int64_t>(i) * 10;
        r.completion_ns = r.arrival_ns + 2'000'000; r.publication_ns = r.completion_ns; r.captured_ns = r.publication_ns;
        r.total_latency = Duration(2'000'000);
        g1.ingest(r);
    }
    if (!g1.save(statefile).ok()) return false;
    GovernorConfig c; c.clock = std::make_shared<SystemClock>(); c.epoch = CoordinatorEpoch(9);
    c.service = ServiceId(1); c.service_generation = ServiceGeneration(1);
    c.workload = WorkloadId(1); c.workload_generation = WorkloadGeneration(1);
    TailGovernor g2(c);
    g2.register_worker(WorkerId(200), WorkerBootId(1));
    if (!g2.load(statefile).ok()) return false;
    auto ev = g2.evaluate(TailObjectiveId(1));
    if (!ev.is_ok() || ev->state != TailState::REVALIDATION_REQUIRED) return false;
    RequestLatency fresh = mk_stale(WorkerId(200), WorkerBootId(1));
    fresh.request_id = RequestId(60000); fresh.epoch = CoordinatorEpoch(9);
    fresh.arrival_ns = 9'000'000'000; fresh.completion_ns = 9'000'002'000; fresh.publication_ns = 9'000'002'000; fresh.captured_ns = 9'000'002'000;
    fresh.total_latency = Duration(2'000'000);
    if (g2.ingest(fresh).code != StatusCode::OK) return false;
    auto ev2 = g2.evaluate(TailObjectiveId(1));
    return ev2.is_ok() && ev2->state != TailState::REVALIDATION_REQUIRED;
}

bool scenario_e() {
    TailGovernor g = make_gov();
    ResourceFacts f; f.failover_candidate = false;
    g.set_resource_facts(f);
    std::int64_t base = now_ns();
    for (int i = 0; i < 200; ++i) {
        RequestLatency r = mk_stale(WorkerId(200), WorkerBootId(1));
        r.request_id = RequestId(70000 + i);
        r.arrival_ns = base + static_cast<std::int64_t>(i) * 10;
        r.completion_ns = r.arrival_ns + 13'000'000; r.publication_ns = r.completion_ns; r.captured_ns = r.publication_ns;
        r.total_latency = Duration(13'000'000);
        r.phases.push_back({Phase::RECOVERY_WAIT, Duration(12'000'000)});
        r.phases.push_back({Phase::EXECUTION, Duration(1'000'000)});
        g.ingest(r);
    }
    auto ev = g.evaluate(TailObjectiveId(1));
    if (!ev.is_ok()) return false;
    return ev->selected_intervention == InterventionType::ACCELERATE_RECOVERY && ev->binding_hard_constraint.has_value();
}

int run_coordinator(const std::string& statefile, const std::string& resultfile) {
    Winsock ws; if (!ws.ok()) return 2;
    std::vector<std::string> lines;
    auto record = [&lines](const char* s, bool pass) { lines.push_back(std::string(s) + ": " + (pass ? "PASS" : "FAIL")); };

    SOCKET ls = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (ls == INVALID_SOCKET) return 2;
    sockaddr_in sa{}; sa.sin_family = AF_INET; sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK); sa.sin_port = 0;
    if (::bind(ls, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) == SOCKET_ERROR) { closesocket(ls); return 2; }
    socklen_t slen = sizeof(sa);
    getsockname(ls, reinterpret_cast<sockaddr*>(&sa), &slen);
    int port = ntohs(sa.sin_port);
    ::listen(ls, 8);

    TailGovernor g = make_gov();
    Child a_child, b_child;
    bool spawn_ok = spawn_worker("A", "127.0.0.1", port, 1, a_child) && spawn_worker("B", "127.0.0.1", port, 1, b_child);
    record("SCENARIO_SPAWN_WORKERS", spawn_ok);
    Sock asock, bsock;
    auto hop = [&](SOCKET& s) { s = ::accept(ls, nullptr, nullptr); return s != INVALID_SOCKET; };
    bool ok_a = hop(asock.fd), ok_b = hop(bsock.fd);
    record("SCENARIO_ACCEPT_WORKERS", ok_a && ok_b);
    HelloInfo hA, hB;
    bool ha = ok_a && read_hello(asock.fd, hA) && hA.name == "A";
    bool hb = ok_b && read_hello(bsock.fd, hB) && hB.name == "B";
    record("SCENARIO_WORKER_HELLO", ha && hb);
    if (ha) g.register_worker(WorkerId(100), WorkerBootId(1));
    if (hb) g.register_worker(WorkerId(200), WorkerBootId(1));

    record("SCENARIO_A_QUEUE_TAIL", ha && hb && scenario_a(asock.fd, bsock.fd, g));
    record("SCENARIO_B_WORKER_KILL", ha && scenario_b(asock.fd, g, a_child));
    Child a2;
    a_child.proc = nullptr;
    Sock a2sock;
    bool fresh = false;
    if (spawn_worker("A", "127.0.0.1", port, 2, a2)) {
        if (hop(a2sock.fd)) {
            HelloInfo hA2;
            if (read_hello(a2sock.fd, hA2) && hA2.name == "A") {
                g.register_worker(WorkerId(100), WorkerBootId(2));
                RequestLatency freshA = mk_stale(WorkerId(100), WorkerBootId(2));
                freshA.request_id = RequestId(999999 + static_cast<std::uint64_t>(a2.pid));
                fresh = g.ingest(freshA).ok();
            }
        }
    }
    record("SCENARIO_B_FRESH_BOOT", fresh);
    record("SCENARIO_C_STALE_INTERVENTION", scenario_c());
    record("SCENARIO_D_RESTART_REVALIDATION", scenario_d(statefile));
    record("SCENARIO_E_CONFLICTING_ACTION", scenario_e());

    Buf sh; put_u8(sh, static_cast<std::uint8_t>(Msg::SHUTDOWN));
    if (ok_b) send_msg(bsock.fd, sh);
    closesocket(asock.fd); closesocket(bsock.fd); closesocket(a2sock.fd); closesocket(ls);

    std::ofstream rf(resultfile, std::ios::trunc);
    bool all_pass = true;
    for (const auto& l : lines) { std::printf("%s\n", l.c_str()); rf << l << "\n"; if (l.find(": FAIL") != std::string::npos) all_pass = false; }
    rf.close();
    return all_pass ? 0 : 1;
}

int main(int argc, char** argv) {
    if (argc >= 6 && std::string(argv[1]) == "worker") {
        return run_worker(argv[2], argv[3], std::atoi(argv[4]), std::strtoull(argv[5], nullptr, 10));
    }
    if (argc >= 3 && std::string(argv[1]) == "coordinator") {
        std::string state = (argc >= 4) ? argv[3] : "dist_state.bin";
        std::string result = (argc >= 5) ? argv[4] : "dist_result.txt";
        return run_coordinator(state, result);
    }
    std::fprintf(stderr, "usage: tailgov_d worker <A|B> <host> <port> <boot> | coordinator <state> <result>\n");
    return 2;
}




