# Incast Governor — architecture, concurrency and recovery notes

Copyright 2026 Summon Software Labs. Apache License 2.0.

This document describes the implementation as it exists. Nothing here is aspirational.

---

## 1. Module map

```
incast/core.hpp        versioning, Error/Outcome, checked arithmetic, CRC-32C, hashing
incast/ids.hpp         strong identities, generations, epochs, boot incarnations,
                       composite SenderIdentity, collision kinds, deterministic RNG
incast/model.hpp       units (Timestamp/Duration/Rate/Bytes/BasisPoints), service classes,
                       evidence states and stamps, observations, the EvidenceBundle,
                       derived FanInMetrics
incast/policy.hpp      thresholds, hysteresis, protection, mitigation bounds, authority rules
incast/detect.hpp      synchronization evidence, classification, refusals, contribution ranking
incast/mitigate.hpp    authority vector, mitigation intent, intent synthesis
incast/lifecycle.hpp   intervention lifecycle state machine, fencing, epochs, recovery
incast/explain.hpp     bounded explanation structure and deterministic rendering
incast/governor.hpp    GovernorState, Decision, and the Governor engine
incast/state.hpp       versioned, CRC-checked binary state container
incast/durable.hpp     journal, snapshot, atomic writes, recovery report
incast/codec.hpp       bounds-checked little-endian binary reader/writer
incast/transport.hpp   sockets and the length-prefixed, CRC-checked frame codec
incast/coordinator.hpp protocol messages, epoch ledger, Coordinator, GovernorWorker
incast/synthetic.hpp   synthetic evidence construction for tests and benchmarks
```

## 2. Data flow of one decision

```
EvidenceBundle ──► structural validation ──► authority binding checks
      │                                            (destination, epoch, boot, worker,
      │                                             lease, lease expiry, destination
      │                                             generation regression)
      │
      ├──► check_authority()  ── per-class evidence state, freshness horizon,
      │                          ordering inside the source incarnation, provenance
      │
      ├──► compute_metrics()  ── population, arrivals, aggregate offer, protected and
      │                          penalizable demand, affected queue, pressure, drops
      │
      ├──► compute_synchronization() ── absolute bound + relative compression +
      │                                 synchronized share + population floor
      │
      ├──► classify_instantaneous() + hysteresis ──► incident kind and severity
      │
      ├──► rank_contributions() ── deterministic ranking, per-sender disposition
      │
      ├──► synthesize() ── bounded intent + authority vector
      │
      ▼
   Decision + Explanation (bounded, deterministic)
```

The engine is a pure function of *(evidence, policy, clock, retained state)*. The retained state
is explicit (`GovernorState`) and serialisable, so the same inputs always produce byte-identical
rendered explanations; this is asserted by unit and property tests.

## 3. Concurrency contract

`Governor` holds exactly one non-recursive `std::mutex`. The contract, verified by inspection and
by tests:

* **No public method calls another public method.** Every public entry point takes the lock once
  and calls only private helpers.
* **No lock upgrade.** There is one mutex; no read/write lock exists, so re-entrant upgrade is
  structurally impossible.
* **No callback under the lock.** `set_decision_observer` installs a callback that
  `evaluate()` invokes **after** the lock is released. A test installs an observer that calls
  `governor.interventions()` and `governor.snapshot_state()` — re-entering the same mutex — and
  completes.
* **No I/O under the lock.** `evaluate()` performs no file, socket or allocator-hook I/O.
  Durability is the caller's responsibility and happens outside the engine.
* **No worker joining under the lock.** The engine owns no threads.

`Coordinator` holds two mutexes:

* `mutex_` — the epoch, the lease table and statistics.
* `sessions_mutex_` — the registry of live sessions and their owning threads.

Contract and audit results:

| Hazard | Finding |
| --- | --- |
| Read-lock → write-lock re-entry | Not applicable: no reader/writer lock exists anywhere. |
| Write lock held across callbacks | No callback is registered in the coordinator. |
| Mutex re-entry through callbacks | No callback path exists; `handle_control` never calls a method that re-locks the mutex it holds. |
| Event emission while holding a lock | Every `channel.send()` executes outside all lock scopes. |
| Worker shutdown while holding locks workers need | `run()` joins worker threads with **no** lock held; it snapshots the session registry under `sessions_mutex_`, releases it, then joins. |
| Joining a thread while holding state that thread needs | `run()` snapshots sessions, shuts their sockets down outside the lock, releases the lock, then joins. Session threads only need `sessions_mutex_` to unregister, which is free by then. |
| Reversed lock ordering on cancellation | Only one nesting rule exists: `mutex_` is never held while `sessions_mutex_` is acquired, and never the reverse. `stop()` touches only `sessions_mutex_`. |
| Progress callbacks that re-enter mutable state | The only callback in the codebase is the decision observer, invoked with no lock held. |
| Shutdown paths that prevent work completing | `stop()` wakes the accept loop with a self-connection, then half-closes every live session — `shutdown()` is the only socket call that is safe against a concurrent `recv` on the same handle. The listener itself is closed by `run()` after the accept loop exits, so no thread ever closes a handle another thread is blocked on. |
| Nested resource acquisition with inconsistent global ordering | Single global order: `mutex_` before `sessions_mutex_`, never the reverse; no other pair nests. |

### Shutdown and cancellation semantics

* Cancellation is real: `Governor::retire` moves an intervention to `Retired`, a terminal state.
  No later evidence can revive that identity; a fresh incident produces a new identity and
  generation. A test asserts this.
* `Governor::shutdown` stops accepting work (every later evaluation is refused), retires every
  live intervention, resets recovery accounting to a valid baseline, and advances the state
  generation. Cancelled work never reports success and never mutates authoritative state.
* `Coordinator::stop` is idempotent and never blocks on a thread it would have to join itself.

## 4. Durability and transactional mutation

Durable mutation follows *validate → bind authority → plan → journal → verify → commit → retire*:

1. The caller evaluates evidence; the engine returns a `Decision` and an advanced `GovernorState`.
2. `DurableStore::commit` encodes the state into a container with magic, format version, state
   generation, explicit payload length and payload CRC-32C.
3. The record is appended to the journal and `fflush` + `_commit`/`fsync`ed before the call
   returns. A durable mutation is never acknowledged before it is durable.
4. When the record count crosses the configured threshold, a snapshot is written to a temporary
   file, synchronised, and atomically renamed over the previous snapshot; only then is the
   superseded journal rotated away. A crash between those steps loses nothing.
5. `max_state_bytes` refuses an oversized payload before it reaches disk.

### What recovery does and does not restore

| Category | Behaviour |
| --- | --- |
| Durable configuration and history | Restored: policy identity and generation, destination, window history, evaluation counters. |
| Committed authoritative state | Restored as data. |
| Unfinished attempts | Reported (`has_unfinished_attempts`). |
| Ambiguous outcomes | A structurally complete tail record that fails its checksum is reported as `journal_tail_ambiguous` and is **not** trusted. |
| Stale live authority | Dropped. Recovered interventions become `Revalidating` candidates. |
| Liveness, telemetry freshness, leases, epochs, worker authority | **Never restored.** Recovery clears the epoch and boot incarnation in the state, and `Governor::restore_state` also clears the live binding, so a restarted governor refuses every decision until authority is re-bound. |
| Evidence requiring revalidation | The recovered state sets `requires_revalidation`; the flag only clears once every intervention has left `Revalidating`. |

## 5. Epochs, fencing and incarnation boundaries

* The coordinator owns a monotonically increasing epoch, persisted in a CRC-checked ledger before
  a grant that depends on it is issued. A coordinator restart loads the ledger and continues
  strictly above the highest epoch ever issued, so an epoch is never reused.
* A lease binds *(lease, destination, destination generation, epoch, boot incarnation, worker,
  policy generation)*. A different live incarnation claiming the same destination supersedes the
  previous holder and advances the epoch.
* Every decision publish is validated against lease identity, revocation, epoch, boot incarnation,
  worker, lease expiry and monotonic sequence. Each rejection carries a specific refusal reason.
* Live interventions are fenced when the epoch, boot incarnation, worker or lease no longer match,
  or when the lease or intervention horizon has passed. Expiry is a **revalidation boundary**, not
  a silent relaxation.

## 6. Evidence ordering

Each evidence class carries a per-source monotonic sequence plus the boot incarnation that
produced it. Sequences are only compared inside one incarnation: a restarted producer legitimately
restarts its sequence space (its previous authority is already fenced by the epoch and lease
binding), while a replay inside one incarnation is refused as `ReorderedEvidence`.

## 7. REAL / SYNTHETIC / UNSUPPORTED

**REAL (validated in this repository)**

* The governance engine, policy evaluation, classification, intent synthesis, lifecycle, fencing,
  serialisation, journal/snapshot durability and recovery.
* Real OS processes: the coordinator, worker and control client are separate executables.
* Real framed transport over real loopback TCP with length-prefixed, CRC-32C-checked frames.
* Hard process kill (`TerminateProcess` / `SIGKILL`) and restart, epoch advancement, incarnation
  fencing and stale-epoch/boot/lease rejection, proven across processes.
* MSVC 19.44 (`/W4 /WX`, C++20), Debug and Release; AddressSanitizer builds and clean runs;
  MSVC `/analyze` static analysis.
* Installed CMake package consumed by an independent downstream `find_package` project.

**SYNTHETIC (fabricated evidence, clearly labelled)**

* All evidence windows used by the test suite and the benchmark are fabricated by
  `incast/synthetic.hpp`. The benchmark prints `PROVENANCE: SYNTHETIC` in its own output.
* Benchmarks measure completed governance decisions on fabricated populations. Importing numbers
  from a physical network, packet captures, switch counters or NIC telemetry would be fabrication.

**UNSUPPORTED (not implemented, not validated, not claimed)**

* No physical fabric, switch ASIC, NIC, DPU, RDMA/RoCE, NVLink, InfiniBand or optical validation.
* No multi-host or multi-switch topology validation; loopback is not a fabric.
* No enforcement plane: the runtime emits intent only.
* No POSIX validation in this environment. The transport layer contains a POSIX path
  (`SIGKILL`, `posix_spawn`, BSD sockets) that compiles only on non-Windows hosts; it has not
  been executed here and is not claimed as verified.
* No real-time, hardware-timestamped or PTP-synchronised clock source; timestamps come from
  `std::chrono::steady_clock`.

## 8. Structural bounds

Everything externally influenced is bounded before allocation or acceptance:
`kMaxSendersPerWindow` (8192), `kMaxQueueObservations` (512), `kMaxPenalizedSenders` (1024),
`kMaxWindowHistory` (256), `kMaxInterventions` (512), `kMaxExplanationNotes` (128),
`kMaxExplanationBytes` (64 KiB), `kMaxFrameBytes` (1 MiB), `kMaxJournalRecordBytes` (256 KiB),
`kMaxJournalRecordsBeforeSnapshot` (4096), `kMaxStateBytes` (4 MiB default),
`kMaxScenarioEvents` (1 048 576), `kMaxIdentifierChars` (96).
