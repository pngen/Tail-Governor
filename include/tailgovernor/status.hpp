#pragma once

#include <string>
#include <utility>
#include <variant>
#include <stdexcept>

namespace tailgovernor {

// Typed status codes. Errors are never reduced to opaque strings.
enum class StatusCode {
    OK,
    INVALID_INPUT,
    INSUFFICIENT_EVIDENCE,
    STALE_EVIDENCE,
    STALE_AUTHORITY,
    OBJECTIVE_INACTIVE,
    POLICY_SUPERSEDED,
    WINDOW_INVALID,
    INTERVENTION_INFEASIBLE,
    INTERVENTION_FAILED,
    OUTCOME_UNKNOWN,
    PERSISTENCE_CORRUPT,
    PROTOCOL_ERROR,
    RESOURCE_EXHAUSTED,
    CANCELLED,
    SHUTTING_DOWN
};

const char* to_string(StatusCode c) noexcept;

// A typed status: a code plus an optional human-detailed message.
// This is the never-just-a-string error carrier.
struct Status {
    StatusCode code = StatusCode::OK;
    std::string message;

    Status() noexcept = default;
    Status(StatusCode c, std::string msg = {}) : code(c), message(std::move(msg)) {}

    bool ok() const noexcept { return code == StatusCode::OK; }
    explicit operator bool() const noexcept { return ok(); }

    friend bool operator==(const Status&, const Status&) = default;
};

inline const char* to_string(StatusCode c) noexcept {
    switch (c) {
        case StatusCode::OK: return "OK";
        case StatusCode::INVALID_INPUT: return "INVALID_INPUT";
        case StatusCode::INSUFFICIENT_EVIDENCE: return "INSUFFICIENT_EVIDENCE";
        case StatusCode::STALE_EVIDENCE: return "STALE_EVIDENCE";
        case StatusCode::STALE_AUTHORITY: return "STALE_AUTHORITY";
        case StatusCode::OBJECTIVE_INACTIVE: return "OBJECTIVE_INACTIVE";
        case StatusCode::POLICY_SUPERSEDED: return "POLICY_SUPERSEDED";
        case StatusCode::WINDOW_INVALID: return "WINDOW_INVALID";
        case StatusCode::INTERVENTION_INFEASIBLE: return "INTERVENTION_INFEASIBLE";
        case StatusCode::INTERVENTION_FAILED: return "INTERVENTION_FAILED";
        case StatusCode::OUTCOME_UNKNOWN: return "OUTCOME_UNKNOWN";
        case StatusCode::PERSISTENCE_CORRUPT: return "PERSISTENCE_CORRUPT";
        case StatusCode::PROTOCOL_ERROR: return "PROTOCOL_ERROR";
        case StatusCode::RESOURCE_EXHAUSTED: return "RESOURCE_EXHAUSTED";
        case StatusCode::CANCELLED: return "CANCELLED";
        case StatusCode::SHUTTING_DOWN: return "SHUTTING_DOWN";
    }
    return "UNKNOWN";
}

// Typed exceptions that carry a StatusCode.
class TailGovernorError : public std::runtime_error {
public:
    explicit TailGovernorError(StatusCode code, std::string msg = {})
        : std::runtime_error(msg.empty() ? std::string(to_string(code)) : std::move(msg)),
          code_(code), message_(msg.empty() ? std::string(to_string(code)) : std::move(msg)) {}

    StatusCode code() const noexcept { return code_; }
    const std::string& what_msg() const noexcept { return message_; }

private:
    StatusCode code_;
    std::string message_;
};

// A lightweight Result<T> (std::expected-like) for C++20.
template <typename T>
class Result {
public:
    Result(T value) : storage_(std::in_place_type<T>, std::move(value)), ok_(true) {}
    Result(Status status) : storage_(std::in_place_type<Status>, std::move(status)), ok_(false) {}

    static Result ok(T v) { return Result(std::move(v)); }
    static Result err(StatusCode code, std::string msg = {}) { return Result(Status(code, std::move(msg))); }

    bool is_ok() const noexcept { return ok_; }
    explicit operator bool() const noexcept { return ok_; }

    T& value() {
        if (!ok_) throw TailGovernorError(StatusErrorCode(), StatusErrorMsg());
        return std::get<T>(storage_);
    }
    const T& value() const {
        if (!ok_) throw TailGovernorError(StatusErrorCode(), StatusErrorMsg());
        return std::get<T>(storage_);
    }

    Status& error() {
        if (ok_) throw TailGovernorError(StatusCode::INVALID_INPUT, "Result has no error");
        return std::get<Status>(storage_);
    }
    const Status& error() const {
        if (ok_) throw TailGovernorError(StatusCode::INVALID_INPUT, "Result has no error");
        return std::get<Status>(storage_);
    }

    T& operator*() { return value(); }
    const T& operator*() const { return value(); }
    T* operator->() { return &value(); }
    const T* operator->() const { return &value(); }

private:
    StatusCode StatusErrorCode() const { return std::get<Status>(storage_).code; }
    std::string StatusErrorMsg() const { return std::get<Status>(storage_).message; }

    std::variant<T, Status> storage_;
    bool ok_;
};

} // namespace tailgovernor
