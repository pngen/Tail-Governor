#pragma once

#include <array>
#include <cstdint>
#include <cstddef>
#include <functional>
#include <string>
#include <tuple>
#include <type_traits>

namespace tailgovernor {

// ---- tag types (declared before any specialization) ----
struct CoordinatorEpochTag      {};
struct ServiceIdTag             {};
struct ServiceGenerationTag     {};
struct WorkloadIdTag            {};
struct WorkloadGenerationTag    {};
struct RequestIdTag             {};
struct RequestClassIdTag        {};
struct RequestGenerationTag     {};
struct AttemptIdTag             {};
struct AttemptGenerationTag     {};
struct DispatchIdTag            {};
struct WorkerIdTag              {};
struct WorkerBootIdTag          {};
struct EngineIdTag              {};
struct EngineIncarnationIdTag   {};
struct TailPolicyIdTag          {};
struct TailPolicyGenerationTag  {};
struct TailObjectiveIdTag       {};
struct TailObjectiveGenerationTag {};
struct EvidenceIdTag            {};
struct EvidenceGenerationTag    {};
struct ObservationWindowIdTag   {};
struct EvaluationIdTag          {};
struct EvaluationGenerationTag  {};
struct InterventionIdTag        {};
struct InterventionGenerationTag {};
struct ResourceGenerationTag    {};
struct QueueGenerationTag       {};
struct BatchGenerationTag       {};
struct ResidencyGenerationTag   {};
struct RecoveryGenerationTag    {};
struct TopologyGenerationTag    {};

namespace detail {
template<typename Tag> inline const char* id_prefix() { return "id"; }
template<> inline const char* id_prefix<CoordinatorEpochTag>()      { return "epoch"; }
template<> inline const char* id_prefix<ServiceIdTag>()             { return "svc"; }
template<> inline const char* id_prefix<ServiceGenerationTag>()     { return "svcgen"; }
template<> inline const char* id_prefix<WorkloadIdTag>()            { return "wl"; }
template<> inline const char* id_prefix<WorkloadGenerationTag>()    { return "wlgen"; }
template<> inline const char* id_prefix<RequestIdTag>()             { return "req"; }
template<> inline const char* id_prefix<RequestClassIdTag>()        { return "cls"; }
template<> inline const char* id_prefix<RequestGenerationTag>()     { return "reqgen"; }
template<> inline const char* id_prefix<AttemptIdTag>()             { return "att"; }
template<> inline const char* id_prefix<AttemptGenerationTag>()     { return "attgen"; }
template<> inline const char* id_prefix<DispatchIdTag>()            { return "dsp"; }
template<> inline const char* id_prefix<WorkerIdTag>()              { return "wk"; }
template<> inline const char* id_prefix<WorkerBootIdTag>()          { return "boot"; }
template<> inline const char* id_prefix<EngineIdTag>()              { return "eng"; }
template<> inline const char* id_prefix<EngineIncarnationIdTag>()   { return "enginc"; }
template<> inline const char* id_prefix<TailPolicyIdTag>()          { return "pol"; }
template<> inline const char* id_prefix<TailPolicyGenerationTag>()  { return "polgen"; }
template<> inline const char* id_prefix<TailObjectiveIdTag>()       { return "obj"; }
template<> inline const char* id_prefix<TailObjectiveGenerationTag>() { return "objgen"; }
template<> inline const char* id_prefix<EvidenceIdTag>()            { return "ev"; }
template<> inline const char* id_prefix<EvidenceGenerationTag>()    { return "evgen"; }
template<> inline const char* id_prefix<ObservationWindowIdTag>()   { return "win"; }
template<> inline const char* id_prefix<EvaluationIdTag>()          { return "eval"; }
template<> inline const char* id_prefix<EvaluationGenerationTag>()  { return "evalgen"; }
template<> inline const char* id_prefix<InterventionIdTag>()        { return "int"; }
template<> inline const char* id_prefix<InterventionGenerationTag>() { return "intgen"; }
template<> inline const char* id_prefix<ResourceGenerationTag>()    { return "resgen"; }
template<> inline const char* id_prefix<QueueGenerationTag>()       { return "qgen"; }
template<> inline const char* id_prefix<BatchGenerationTag>()       { return "bgen"; }
template<> inline const char* id_prefix<ResidencyGenerationTag>()   { return "rgen"; }
template<> inline const char* id_prefix<RecoveryGenerationTag>()    { return "recgen"; }
template<> inline const char* id_prefix<TopologyGenerationTag>()    { return "topogen"; }
} // namespace detail

// Strongly typed 64-bit identity. The Tag type is a phantom type used only to
// keep domains apart at compile time. Underlying storage is a deterministic
// uint64. All operations (ordering, hashing, canonical serialization, equality)
// are deterministic.
template <typename Tag>
class Id {
public:
    using tag_type = Tag;
    using value_type = std::uint64_t;

    constexpr Id() noexcept : value_(0) {}
    constexpr explicit Id(value_type v) noexcept : value_(v) {}

    constexpr value_type value() const noexcept { return value_; }
    explicit constexpr operator bool() const noexcept { return value_ != 0; }

    constexpr Id& operator++() noexcept { ++value_; return *this; }

    constexpr friend bool operator==(Id a, Id b) noexcept = default;
    constexpr friend bool operator!=(Id a, Id b) noexcept = default;
    constexpr friend auto operator<=>(Id a, Id b) noexcept = default;

    std::array<std::uint8_t, 8> canonical_bytes() const noexcept {
        std::array<std::uint8_t, 8> out{};
        std::uint64_t v = value_;
        for (int i = 7; i >= 0; --i) { out[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(v & 0xFFu); v >>= 8; }
        return out;
    }

    std::string to_string() const {
        char buf[16];
        const char* hex = "0123456789abcdef";
        std::uint64_t v = value_;
        for (int i = 15; i >= 0; --i) { buf[i] = hex[v & 0xFu]; v >>= 4; }
        return std::string(detail::id_prefix<Tag>()) + "#" + std::string(buf, 16);
    }

private:
    value_type value_;
};

// ---- identity aliases ----
using CoordinatorEpoch       = Id<CoordinatorEpochTag>;
using ServiceId              = Id<ServiceIdTag>;
using ServiceGeneration      = Id<ServiceGenerationTag>;
using WorkloadId             = Id<WorkloadIdTag>;
using WorkloadGeneration     = Id<WorkloadGenerationTag>;
using RequestId              = Id<RequestIdTag>;
using RequestClassId         = Id<RequestClassIdTag>;
using RequestGeneration      = Id<RequestGenerationTag>;
using AttemptId              = Id<AttemptIdTag>;
using AttemptGeneration      = Id<AttemptGenerationTag>;
using DispatchId             = Id<DispatchIdTag>;
using WorkerId               = Id<WorkerIdTag>;
using WorkerBootId           = Id<WorkerBootIdTag>;
using EngineId               = Id<EngineIdTag>;
using EngineIncarnationId    = Id<EngineIncarnationIdTag>;
using TailPolicyId           = Id<TailPolicyIdTag>;
using TailPolicyGeneration   = Id<TailPolicyGenerationTag>;
using TailObjectiveId        = Id<TailObjectiveIdTag>;
using TailObjectiveGeneration = Id<TailObjectiveGenerationTag>;
using EvidenceId             = Id<EvidenceIdTag>;
using EvidenceGeneration     = Id<EvidenceGenerationTag>;
using ObservationWindowId    = Id<ObservationWindowIdTag>;
using EvaluationId           = Id<EvaluationIdTag>;
using EvaluationGeneration   = Id<EvaluationGenerationTag>;
using InterventionId         = Id<InterventionIdTag>;
using InterventionGeneration = Id<InterventionGenerationTag>;
using ResourceGeneration     = Id<ResourceGenerationTag>;
using QueueGeneration        = Id<QueueGenerationTag>;
using BatchGeneration        = Id<BatchGenerationTag>;
using ResidencyGeneration    = Id<ResidencyGenerationTag>;
using RecoveryGeneration     = Id<RecoveryGenerationTag>;
using TopologyGeneration     = Id<TopologyGenerationTag>;

struct WorkerRoleTag {};
enum class WorkerRole { NONE, WORKER_A, WORKER_B };

} // namespace tailgovernor

namespace std {
template <typename Tag>
struct hash<tailgovernor::Id<Tag>> {
    size_t operator()(const tailgovernor::Id<Tag>& id) const noexcept {
        std::uint64_t x = id.value();
        x ^= x >> 33; x *= 0xff51afd7ed558ccdULL; x ^= x >> 33;
        x *= 0xc4ceb9fe1a85ec53ULL; x ^= x >> 33;
        return static_cast<size_t>(x);
    }
};
} // namespace std
