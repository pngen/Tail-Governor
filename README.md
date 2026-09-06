# Tail Governor

Tail Governor is an open-source, vendor-neutral C++20 runtime for governing p95, p99, and
p999 tail latency across admission, batching, queue shaping, residency, preemption, recovery,
and resource decisions in heterogeneous AI infrastructure.

The core systems question it answers:

- Why is the slow tail becoming unacceptable now?
- Which workloads or runtime conditions are causing it?
- What intervention can reduce it without violating stronger obligations?
- Under what authority may that intervention execute?

The thesis: average latency does not protect users from the tail. A healthy mean can coexist
with a disastrous p99. A healthy p95 can conceal a catastrophic p999. Tail Governor makes the
slow tail an explicit governed systems boundary.

## Exact systems boundary

Tail Governor owns:

- tail-latency objective ingestion (p95/p99/p999);
- tail-state evaluation and exact quantile handling;
- per-request latency evidence, decomposition, and slow-request classification;
- tail-cause attribution (queueing, batching, residency, transfer, memory, retry, recovery,
  preemption, execution variance, and unknown);
- tail amplification and heavy-tail detection;
- deadline/tail-risk prediction;
- intervention selection, feasibility, economics, and bounded authority;
- hysteresis, cooldown, and post-intervention verification;
- historical reconstruction and stale-evidence/stale-authority rejection.

Tail Governor is deliberately narrower and deeper than a generic latency runtime. It does not
become a general observability system, a general SLO system, an inference scheduler, or a
generic latency runtime. It governs the slow tail.

Adjacent-rung separation is rigorous: Tail Governor consumes a tail-latency objective from
SLO Fabric, and it emits typed, authority-bearing interventions that adjacent runtimes
(Admission Fabric, Batch Fabric, Preemption Fabric, Engine Residency, Recovery Planner, Memory
Pressure, Bandwidth Governor, Resource Broker) execute. Tail Governor does not implement those
mechanisms. It is not the general execution-cost optimizer (Cost Governor is the next layer).

## Building and installing

Requirements: CMake 3.20+, a C++20 compiler, and (optionally) a CUDA 12.8+ toolkit for the
real-hardware proof. The standalone core does not require CUDA or any other Summon Software
Labs repository.

    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build
    ctest --test-dir build -C Release
    cmake --install build --prefix <prefix>

To build the optional CUDA real-hardware proof:

    cmake -S . -B build-cuda -G Ninja -DTAILGOVERNOR_WITH_CUDA=ON -DCMAKE_BUILD_TYPE=Release
    cmake --build build-cuda
    build-cuda/cuda/tail_cuda_proof

The installed package exports an independent CMake package named TailGovernor, usable from a
downstream project with:

    find_package(TailGovernor CONFIG REQUIRED)
    target_link_libraries(my_app PRIVATE TailGovernor::tailgovernor)

## p95 / p99 / p999 semantics

Tail metrics are exact (not approximated). Each objective selects one percentile: P95, P99, or
P99.9 (a p999 is the 99.9th percentile). Every objective carries a target duration, evaluation
window, minimum sample count, freshness requirement, hard/soft policy, violation threshold,
recovery threshold, hysteresis, cooldown, optional request-class scope, priority, objective
generation, and authority.

## Quantile algorithm

The exact quantile definition is fixed and documented: nearest-rank. For N samples sorted
ascending and percentile p in (0,100], the rank is ceil(p/100 * N), clamped to [1,N], and the
result is the sample at that rank (1-based). This is deterministic and yields the invariant that
p95 <= p99 <= p999 for the same sorted sample set, because ceil is monotone non-decreasing.
The core never stores floating-point latency; latencies are integer nanoseconds.

## Sample sufficiency

Tail metrics require enough evidence. A verdict is produced per percentile:

- SUFFICIENT
- INSUFFICIENT_SAMPLES
- INSUFFICIENT_WINDOW
- STALE
- UNKNOWN

The derived minimum-sample rule is ceil(100 / (100 - p)), bounded: p95 needs 20, p99 needs 100,
and p999 needs 1000. A goal-specific min_samples can override it. Ten samples never silently
produce a trustworthy p999. Percentiles with insufficient evidence are labeled, not presented as
measured truth.

## Windowing

Evaluation windows are bounded and injectable-clock driven (deterministic tests need no sleeps).
Supported modes: ROLLING_TIME (sliding window), ROLLING_COUNT (most recent N), TUMBLING (fixed
interval), and FIXED_RECENT (duration + count cap). Boundary timestamps, out-of-order evidence,
equal timestamps, late/duplicate/stale/future samples, clock rollback, empty windows, and
window expiration are handled.

## Latency phases

Completed authoritative work carries explicit phase durations where evidence exists:
ADMISSION_WAIT, QUEUE_WAIT, BATCH_WAIT, PREFILL, DECODE, EXECUTION, TRANSFER, STATE_FETCH,
MODEL_LOAD, ENGINE_WARMUP, MEMORY_RECLAIM, PREEMPTION_WAIT, RECOVERY_WAIT, RETRY_DELAY,
BACKEND_WAIT, PUBLICATION, and OTHER. Workloads need not use every phase. Phase sums reconcile
against the authoritative total; impossible phase sums are rejected at ingestion. Only
authoritative successful completions (with a publication time) are valid tail evidence; duplicate
completions, cancelled work, and non-authoritative completions never count.

## Tail attribution

For the slow-tail bucket, Tail Governor decomposes latency into phase contributions, computes the
dominant phase deterministically, maps it to a typed cause, and reports attribution strength:
DOMINANT_CONTRIBUTOR, ASSOCIATED_CONTRIBUTOR, or UNKNOWN_CAUSE. It never claims causal proof
from correlation alone. When evidence is ambiguous, the cause remains UNKNOWN.

## Tail amplification

Amplification ratios are computed with honest denominators: p99/p50, p999/p50, p99/mean, and
p999/p95. Only ratios with a valid, non-zero denominator are produced. Amplification is signal,
not automatic proof of violation.

## Intervention model

Interventions are typed and authority-bearing. Supported classes include queue shaping,
batch-change, residency, capacity/bandwidth reserve, preemption/defer/shed load, memory reclaim,
placement change, recovery acceleration/plan/failover, escalation, and manual-intervention
required. Each intervention carries a reason, target, expected tail effect, collateral effect,
confidence, source evaluation, authority generations, expiry, policy, and provenance.

Feasibility is validated before selection: cannot preempt non-preemptible work, cannot warm an
unavailable engine, cannot reserve unavailable capacity, cannot move state to an incompatible
target, cannot fail over with no valid candidate, cannot reduce a batch below the legal minimum,
cannot shed protected traffic, cannot violate a stronger hard SLO, and cannot exceed hard cost or
resource constraints. Rejected alternatives are exposed with explicit reasons.

## Deterministic selection

The selection pipeline is: ingest current evidence, validate authority, build the window,
validate sample sufficiency, compute percentiles, classify tail state, decompose the tail,
identify dominant contributors, construct feasible interventions, apply hard constraints,
estimate effect and collateral, deterministically rank, authorize, pre-dispatch revalidate,
observe post-action evidence, and verify the tail response. Identical canonical inputs yield
identical decisions; the pipeline never depends on hash-map iteration. The binding objective is
the deterministic primary severity among violating p95/p99/p999 objectives.

## Authority and generations

Actionable interventions are fenced to the relevant generations: CoordinatorEpoch,
ServiceGeneration, WorkloadGeneration, TailPolicyGeneration, TailObjectiveGeneration,
EvidenceGeneration, EvaluationGeneration, InterventionGeneration, plus applicable runtime
generations (Queue, Batch, Resource, Recovery, Topology) and worker boot / engine incarnation.
A stale intervention rejects without mutating current state. Pre-dispatch revalidation confirms
the epoch, service/workload generation, objective/policy generation, target existence, worker
boot/incarnation, resource availability, and absence of new hard constraints.

## Hysteresis and cooldown

Tail suppression must not flap. Violation begins above the target; recovery requires a sustained
run below the recovery threshold (recovery_fraction of the target, default 0.90) before returning
to HEALTHY. Cooldown bounds repeated authorities for actions such as preemption, failover,
residency/batch-policy changes, capacity scaling, and load shedding. Both use the injectable
clock; there are no sleeps.

## Persistence and restart

Durable coordinator state (tail policies, objectives, intervention history, completed outcomes,
stable identities, the epoch) is saved with a versioned, integrity-checked, deterministic binary
format: explicit version, bounded decode, CRC-32 integrity, atomic save, and rejection of
truncation, corruption, trailing garbage, and unknown versions. A coordinator restart advances the
epoch and marks dynamic observations REVALIDATION_REQUIRED; recovered measurements never silently
become current, and survivors must republish fresh evidence before current evaluation resumes.

## Distributed proof

A real multiprocess proof (tailgov_d) uses independent OS processes over framed, versioned,
CRC-protected TCP loopback. A coordinator/governor process spawns Worker A and Worker B, frames
are bounded and safe under concurrent writes and partial reads, and the scenarios exercise a real
worker OS kill, a fresh WorkerBootId fencing out stale traffic, a genuine OS-process
coordinator termination and fresh-incarnation restart (the persisted state is reloaded, the
coordinator epoch advances, recovered observations are REVALIDATION_REQUIRED, surviving
workers reconnect and republish fresh evidence, and old-epoch/stale traffic rejects), a stale
intervention rejecting before dispatch, and a hard-constraint conflict resolved to the legal
action. All scenarios pass.

## CUDA proof

A real-hardware proof runs on the installed GPU (tested on an NVIDIA GeForce RTX 5090, sm_120,
CUDA 12.9). It allocates real device memory, performs H2D, runs repeated real kernels, syncs
every measured operation, performs D2H, verifies CPU parity, captures completed host-observed
end-to-end latency, induces a bounded deterministic host-side queue-delay tail, shows p99 breach a
target while the governor identifies queueing and selects an intervention, applies a reference
intervention, measures fresh post-action evidence that recovers p99, and confirms the device-memory
accounting returns to baseline. Host-observed end-to-end latency is distinguished from device
execution time. The proof is real, not synthetic.

## REAL / SYNTHETIC / UNSUPPORTED

- The distributed proof is REAL (independent OS processes, real TCP, real process kill).
- The CUDA proof is REAL on the specific GPU above (sm_120, RTX 5090).
- Synthetic scenarios (multi-node queue hotspot, cross-NUMA penalties, remote residency,
  shared-link contention, recovery storms) are not implemented as physical measurements; the
  deterministic unit tests use synthetic fixtures and are labeled as such.
- UNSUPPORTED: hardware kernel preemption, multi-GPU tail behavior, NVLink, RDMA, GPUDirect, MIG,
  and hardware failover are not claimed.

## Benchmarks

tg_bench measures completed work — latency-sample ingestion, exact percentile calculation, tail
classification, and intervention selection — across 100, 1,000, 10,000, 100,000, and 1,000,000
samples.

## Examples and CLI

tg_examples runs the p99 objective, stable-p50/bad-p99, p999-insufficient-samples, queue-tail, and
stale-intervention examples. tailctl provides a focused CLI (demo, validate-state). Examples and
the CLI execute as tests.

## Limitations

- Only exact nearest-rank quantiles are implemented; there is no approximate-quantile mode.
- The CUDA proof uses a host-side queue-delay tail. GPU-state drift across phases (the 5090 is a
  shared, sometimes-throttled desktop device) means the cross-phase median is not perfectly stable;
  the governed p99 tail and its recovery are the accepted gates, and p50 is reported transparently.
- MSVC AddressSanitizer (/fsanitize=address with the Ninja + cl driver) was run on the core
  unit/property/adversarial/concurrency suite and on the distributed multiprocess proof; both
  pass with zero findings. A separate Debug configuration also builds with MSVC runtime checks
  (/RTC).
- Tail Governor does not implement admission, batching, preemption, residency, recovery, or
  bandwidth mechanisms; it emits typed intents for adjacent runtimes.

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.

