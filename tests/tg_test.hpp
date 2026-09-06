#pragma once
// Minimal deterministic test harness. No timeouts, no sleeps.
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <functional>
#include <sstream>

namespace tgtest {
struct TestCase { std::string name; std::function<void()> fn; };
inline std::vector<TestCase>& registry() { static std::vector<TestCase> r; return r; }
inline int& failures() { static int f = 0; return f; }
inline int& current() { static int c = 0; return c; }
struct Registrar { Registrar(const char* n, std::function<void()> f) { registry().push_back({n, std::move(f)}); } };

inline void report_fail(const char* file, int line, const std::string& msg) {
    ++failures();
    std::fprintf(stderr, "FAIL %s:%d [test %d]: %s\n", file, line, current(), msg.c_str());
}

#define TEST(name)     static void tg_test_##name();     static ::tgtest::Registrar tg_reg_##name(#name, &tg_test_##name);     static void tg_test_##name()

#define CHECK(cond) do { if (!(cond)) ::tgtest::report_fail(__FILE__, __LINE__, "CHECK: " #cond); } while (0)
#define CHECK_EQ(a, b) do { auto _a = (a); auto _b = (b); if (!(_a == _b)) {     std::ostringstream _os; _os << "CHECK_EQ(" #a ", " #b ") : " << _a << " != " << _b;     ::tgtest::report_fail(__FILE__, __LINE__, _os.str()); } } while (0)
#define REQUIRE(cond) do { if (!(cond)) { ::tgtest::report_fail(__FILE__, __LINE__, "REQUIRE: " #cond); return; } } while (0)

inline int run_all() {
    int c = 0;
    for (auto& t : registry()) {
        current() = ++c;
        int before = failures();
        try { t.fn(); }
        catch (const std::exception& e) { report_fail("<exception>", 0, std::string("uncaught: ") + e.what()); }
        catch (...) { report_fail("<exception>", 0, "uncaught unknown exception"); }
        if (failures() == before) std::printf("PASS test %d: %s\n", c, t.name.c_str());
    }
    std::printf("--- %d test(s), %d failure(s) ---\n", c, failures());
    return failures() == 0 ? 0 : 1;
}
} // namespace tgtest
