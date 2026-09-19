# Incast Governor

Open-source, vendor-neutral C++20 runtime for generation-bound detection and prevention of
synchronized many-to-one traffic collapse across governed fabric resources.

**Version 1.0.0** · Apache License 2.0 · Copyright 2026 Summon Software Labs.

---

## The question this library answers

Given authoritative fan-in, destination, queue, buffer, rate, timing, capacity and service
evidence, **is synchronized many-to-one traffic creating or approaching incast collapse, which
senders contribute, what bounded mitigation is authorized, and when must that intervention be
relaxed, fenced, or revalidated?**

Incast Governor answers that question and nothing else. It converts an authoritative evidence
window into one deterministic, explainable decision:

* an incident classification (UNKNOWN / NoFanIn / OrdinaryManyToOne / SynchronizedIncastRisk /
  ActiveIncast / Recovery),
* a severity,
* the contributing sender population, ranked, with per-sender dispositions,
* a **bounded mitigation intent** — a request, never an enforcement action,
* an **authority vector** recording exactly which evidence classes and bindings justified it.

## Boundary

**Incast Governor owns** incast detection, classification and bounded mitigation *intent*.

**Incast Governor does not own** generic congestion state, rate enforcement, scheduling, pacing
execution, queue implementation, buffer allocation, admission, path placement, or sender-protocol
implementation. The runtime publishes intent; an external enforcement plane decides whether and
how to apply it. No code path in this repository writes a rate, a queue, a buffer budget, a
scheduler weight or a packet.

## Model

Strongly typed identities and generations for events, destinations, senders, flows, resources,
queues, evidence windows, policies, interventions, epochs, worker incarnations and provenance.
A `SenderIdentity` is a `(sender, boot incarnation, flow)` composite, so a bare identifier is
never sufficient authority to penalize traffic.

The governor represents active sender count, synchronized arrival window, aggregate offered rate,
destination service capacity, queue/buffer pressure, service class and priority, protected
senders, event timing, and mitigation generation. All externally influenced sizes, capacities,
rates, counters and time units pass through checked arithmetic.

## Detection

Four outcomes are distinguished, and **synchronization is never inferred from sender count**:

| Classification | Meaning |
| --- | --- |
| `NoFanIn` | The observed population is not many-to-one. |
| `OrdinaryManyToOne` | Many senders, arrivals **not** compressed into a synchronized window. |
| `SynchronizedIncastRisk` | Synchronized fan-in whose aggregate offer exceeds destination service capacity. |
| `ActiveIncast` | Synchronized fan-in actively collapsing service (rate, queue pressure or drops). |
| `Recovery` | A previously active incident now inside the recovery band. |
| `Unknown` | Required evidence is missing, stale, contradictory, reordered or out of window. |

Synchronization requires a population floor **and** an absolute arrival-compression bound **and**
a minimum share of aggregate demand carried inside the synchronized window. A 512-sender fan-in
spread over 900 µs of a 1 ms window classifies as ordinary many-to-one.

## Mitigation

Bounded intent may request sender staggering, pacing hints, priority-aware rate reduction,
temporary admission reduction, temporary buffer/headroom adjustment, or congestion escalation.
Every component of an intent is clamped into the policy's bounds before it can be authorized:
reduction basis points, admission reduction, headroom bytes, stagger slots, penalized sender
count, and duration.

## Invariants

These are enforced in code and covered by tests, including randomized property tests:

1. **Stronger obligations remain protected.** Protected senders (explicit obligation, or a service
   class above the configured mitigable ceiling) are never penalized. If the required reduction
   cannot be achieved without violating the protected floor, the governor escalates instead, or
   refuses with `ProtectedFloorConflict` when escalation is disabled.
2. **Stale sender/destination/capacity evidence invalidates the event.** Stale, contradictory,
   reordered, out-of-window or superseded evidence yields `Unknown` and no authority.
3. **UNKNOWN cannot authorize intervention.** A missing required evidence class denies authority
   outright.
4. **Identity collision cannot penalize the wrong sender.** Duplicate identities with different
   incarnations, or contradictory attributes on one identity, are detected; the collided identity
   is excluded from any penalization.
5. **A saturated population forbids sender-scoped penalization.** When the bounded population was
   clipped, the governor cannot prove which senders are protected, so only destination-scoped
   intent is authorized and counts are reported as lower bounds.
6. **Interventions relax only under an explicit recovery policy.** With no recovery policy a live
   intervention holds until its expiry boundary, which forces revalidation rather than a silent
   relaxation step.
7. **Hysteresis prevents oscillation.** Escalation is immediate; de-escalation requires a dwell,
   and a window the classifier does not yet trust holds the intervention at its current magnitude
   instead of weakening it.

## Runtime and distribution

* **Governor** — a deterministic, stateless-with-respect-to-inputs engine. Identical evidence,
  policy and clock produce byte-identical explanations. All public entry points are serialized by
  one non-recursive mutex; no callback, socket operation or allocation hook ever runs while that
  mutex is held.
* **Coordinator** — a real OS process owning the epoch and the destination lease table, reached
  over real loopback TCP with length-prefixed, CRC-32C-checked frames (20-byte header,
  `kMaxFrameBytes` = 1 MiB bound).
* **Worker** — a real OS process that acquires a destination lease, runs a governor and publishes
  every decision back to the coordinator, which accepts or rejects it against the lease, epoch,
  boot incarnation, worker and sequence.

**Durable state never restores liveness.** Recovery distinguishes durable configuration and
history, committed authoritative state, unfinished attempts, ambiguous outcomes, stale live
authority, and evidence requiring revalidation. Every recovered intervention becomes a
*revalidation candidate*; a restore clears the live authority binding entirely, so a restarted
governor refuses every decision until authority is re-bound from the coordinator, and a recovered
intervention from a superseded incarnation is fenced rather than resumed.

Journal and snapshot records are versioned, length-prefixed and CRC-32C checked. A torn or corrupt
journal tail is discarded and reported as ambiguous rather than trusted.

## Build

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Release and Debug both compile under `/W4 /WX` (MSVC) or `-Wall -Wextra -Wpedantic -Wshadow
-Wconversion -Wsign-conversion -Wold-style-cast -Werror` (GCC/Clang). AddressSanitizer is
available through `-DINCAST_GOVERNOR_ENABLE_ASAN=ON`. No test declares a timeout: a hang is
treated as a defect to diagnose, never as something to hide behind a watchdog.

### Tools

| Tool | Purpose |
| --- | --- |
| `incast-governor` | `evaluate`, `selftest`, `version` — one synthetic window to a rendered explanation. |
| `incast-governor-coordinator` | Owns the epoch and lease table; serves framed TCP sessions. |
| `incast-governor-worker` | Acquires a lease, runs a governor, publishes decisions, optionally journals state. |
| `incast-governor-ctl` | Token-authenticated control plane: `status`, `advance-epoch`, `revoke-all`, `shutdown`. |
| `incast-governor-bench` | Synthetic governance benchmark. |

```
incast-governor evaluate --senders 256 --spread-ns 20000 --service-bps 40000000000 \
                         --queue-pressure-bp 9200 --drops 16 --json
incast-governor selftest
```

## Install and consume

```
cmake --install build --config Release --prefix /path/to/prefix
cmake -S examples/downstream_consumer -B consumer -DCMAKE_PREFIX_PATH=/path/to/prefix
cmake --build consumer --config Release
```

The installed package exports `IncastGovernor::incast_governor` and a version file compatible on
the same major version. `examples/downstream_consumer` is an independent project that consumes
only the installed headers and the exported target.

## Evidence classes and provenance

Eight evidence classes carry an explicit state: `Present`, `Unknown`, `Stale`,
`Contradictory`, `Reordered`, `OutOfWindow` or `Superseded`. Each class must be authoritative
before an intervention is authorized, and each carries a provenance identity and generation, an
observation instant, an optional validity horizon and a per-source monotonic sequence. Sequences
are scoped to the source incarnation: a restarted producer legitimately restarts its sequence
space, while a replay inside one incarnation is refused as reordered.

## Real vs synthetic

Every capability in this repository is exercised against **synthetic fabricated evidence**. The
governor has never been run against a physical fabric, an RDMA or RoCE NIC, a switch ASIC, a DPU,
a multi-switch topology or an optical network, and no such validation is claimed. Distribution is
validated across **real OS processes** over **real loopback TCP** — loopback is not a physical
fabric. The synthetic generator (`incast/synthetic.hpp`) states this in its own header.

## Repository layout

```
include/incast/   public headers (core, ids, model, policy, detect, mitigate, lifecycle,
                  governor, explain, state, durable, transport, coordinator, synthetic)
src/              implementation
tools/            coordinator, worker, control client and CLI
bench/            synthetic governance benchmark
tests/            unit, integration, property, adversarial, restart and multiprocess suites
examples/         independent downstream find_package consumer
cmake/            package configuration template
docs/             architecture and operational notes
```

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the module map, the concurrency and
lock-ordering contract, and the deadlock audit.

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
