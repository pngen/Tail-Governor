#pragma once

#include <cstdint>
#include <memory>
#include <chrono>

namespace tailgovernor {

// Abstract monotonic clock. All time in integer nanoseconds.
// Injectable so deterministic tests do not depend on real time or sleeps.
class Clock {
public:
    virtual ~Clock() = default;
    virtual std::int64_t now_ns() const = 0;
};

class SystemClock final : public Clock {
public:
    std::int64_t now_ns() const override {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }
};

// A manual test clock whose time can be set/advanced deterministically.
class TestClock final : public Clock {
public:
    explicit TestClock(std::int64_t start_ns = 0) : now_(start_ns) {}

    std::int64_t now_ns() const override { return now_; }
    void set(std::int64_t ns) { now_ = ns; }
    void advance(std::int64_t ns) { now_ += ns; }

private:
    std::int64_t now_ = 0;
};

using ClockPtr = std::shared_ptr<const Clock>;

} // namespace tailgovernor
