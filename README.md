# Data Center Registry

Canonical registry for data-center identities, site membership, control-plane
generations, lifecycle state, and authoritative facility metadata.

It is a portable C++20 library with a small inspection tool, no
third-party dependencies, and no telemetry.

---

## Systems boundary

**This repository owns** the authoritative physical *identity* model of a data
center as a facility control-plane object:

* canonical, stable data-center identities and their secondary keys;
* site membership, held as typed references to sites owned elsewhere;
* the registry generation that fences mutations, and the per-record revision
  that detects lost updates;
* administrative lifecycle state and the legal transitions between states;
* ownership attribution and provenance for every accepted change;
* compatibility contracts and deterministic extension metadata;
* durable, integrity-checked storage of that state and its recovery.

**This repository does not own**, and does not implement:

* the site object itself, or site-level network authority (Site Control Plane,
  Site Fabric);
* facility topology, racks, assets, or physical location records (Facility
  Topology, Rack Registry, Asset Registry, Physical Location Registry);
* control-plane epoch election or controller fencing (Control Plane Epoch). This
  registry records an externally issued epoch token and can reject a mutation
  that asserts a stale one; it never elects an epoch and never fences a
  controller;
* capacity, power, cooling, placement, reservations, maintenance orchestration,
  incident response, observability, or multi-site federation. Those appear here
  only as opaque typed references or as future consumers.

A `DataCenterId` is stable across ordinary metadata changes. When identity truly
changes, the replacement operation retires the old record and creates a new one
in a single generation, linking them in both directions. Retirement and
replacement are terminal: obsolete authority never returns.

---

## Building

Requirements: CMake 3.20 or newer, a C++20 compiler (MSVC 19.4x, GCC 11+ or
Clang 14+), and nothing else. There are no third-party dependencies.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Options: `DCR_BUILD_TOOLS`, `DCR_BUILD_TESTS`, `DCR_BUILD_EXAMPLES`,
`DCR_BUILD_BENCHMARKS`, `DCR_WARNINGS_AS_ERRORS` (default `ON`),
`DCR_ENABLE_ASAN` (default `OFF`). `CMakePresets.json` provides `release`,
`debug` and `asan` presets.

### Installing and consuming the package

```sh
cmake --install build --prefix /path/to/prefix
```

The install provides headers under `include/dcr`, the static library
`libdata_center_registry.a` (or `data_center_registry.lib`), the `dcr` tool in
`bin`, and a CMake package:

```cmake
find_package(data_center_registry 1.0 REQUIRED)
target_link_libraries(your_target PRIVATE data_center_registry::data_center_registry)
```

`examples/downstream_consumer` is a standalone project that does exactly this
against an installed prefix:

```sh
cmake -S examples/downstream_consumer -B /tmp/downstream -DCMAKE_PREFIX_PATH=/path/to/prefix
cmake --build /tmp/downstream
/tmp/downstream/dcr_downstream_consumer
```

---

## Architecture

```
include/dcr/          the public API, and the only headers consumers need
  identity.hpp        DataCenterId, SiteId, Alias, PrincipalId, OwnershipScopeId,
                      EpochIssuerId, ReasonCode, IdempotencyKey, CountryCode
  generation.hpp      RegistryGeneration, MetadataRevision, SequenceNumber,
                      EpochToken
  lifecycle.hpp       LifecycleState and the transition table
  provenance.hpp      who asserted a change, under what authority, and when
  compatibility.hpp   CompatibilityKey and the replacement rules
  metadata.hpp        facility metadata and deterministic extension metadata
  record.hpp          DataCenterRecord and the invariant gate
  commands.hpp        mutation commands and patches
  outcome.hpp         MutationStatus and MutationResult
  query.hpp           enumeration, history and statistics
  time.hpp            Timestamp and the injectable Clock
  cancellation.hpp    cooperative cancellation
  limits.hpp          bounded resource limits
  persistence.hpp     store options, open reports, integrity inspection
  snapshot.hpp        immutable snapshots
  registry.hpp        the registry

src/core/             the model, the mutation engine, the canonical codec
src/persistence/      the durable store
src/internal/         filesystem and text primitives
tools/                the dcr inspection and administration tool
tests/ benchmarks/ examples/
```

### The layers

**Value types with validation at construction.** Identifiers are distinct types
per family and cannot be converted, compared or interchanged across families. A
`DataCenterId` is always already canonical, because the only way to obtain one
is to parse canonical text successfully. Metadata, timestamps, coordinates,
compatibility keys and country codes validate on construction. Nothing is
case-folded, trimmed or otherwise normalised into authoritative state: input is
either canonical or it is rejected.

**One record per facility.** `DataCenterRecord` holds identity, revision,
display name, aliases, site memberships, facility metadata, extension metadata,
ownership, compatibility key, lifecycle state, creation/retirement generation,
replacement link, and the provenance of its last change. `validate_record()`
and `validate_record_set()` are the single gate every record passes through
before it becomes authoritative, whether it arrived from a command or from a
decoded snapshot.

**One mutation engine.** Every command is *planned* against a base state and
produces either a rejection or a complete new state plus the outcome to report.
Planning is pure: it does not touch the store, the clock or a lock. Publication
is a separate step owned by the registry, which is what makes "validate
everything, then write" structural rather than a convention.

**One store.** `<root>` holds a writer lock, a small `CURRENT` pointer, immutable
published generations, a staging area, and a quarantine area for uncommitted
residue. Consumers never see these files through the API.

---

## Identity, generations and authority

Four counters with four different meanings, as four different types, because
mixing them is the classic way to authorise the wrong write:

| Type | Meaning | Range |
| --- | --- | --- |
| `RegistryGeneration` | registry-wide; +1 per committed mutation batch. The concurrency and fencing unit. | from 0 (empty registry) |
| `MetadataRevision` | per record; +1 per committed change to that record. The lost-update unit. | from 1 |
| `SequenceNumber` | registry-wide; +1 per recorded provenance entry. The history unit. | from 0 |
| `EpochToken` | external; issued by a control-plane-epoch authority. The registry records it and never elects it. | issuer-qualified |

Every counter is monotonic and checked: incrementing the maximum is an error,
never a wrap. Generation 0 is the empty registry and is never published; the
first commit produces generation 1.

`EpochToken` values compare only within one issuing authority. Comparing tokens
from different authorities has no defined order and returns no ordering rather
than a guess. A mutation may assert an epoch token: a token older than the
recorded one, or one from a different authority, is refused with
`stale_authority`; a newer token from the recorded authority is accepted and
becomes the recorded token.

### Preconditions

Every command that changes an existing record must carry an
`expected_generation`; updates and replacements may also carry an
`expected_revision`. A command whose desired end state already holds is
reported as `unchanged` *before* preconditions are consulted, because telling a
caller to re-read state and retry work that is already complete would be
misleading. Preconditions are enforced for everything that would actually
change state.

Advertisement of the precedence, in order:

1. command content is validated against the configured limits;
2. cancellation is honoured, so a cancelled mutation never even plans;
3. an idempotency key is resolved, so a retry reports the original outcome;
4. the target is located, so an unknown identity is `not_found`;
5. a command whose desired end state already holds is `unchanged`;
6. preconditions are enforced: stale generation, stale revision, stale
   external authority;
7. the change is applied and the whole resulting state is validated.

---

## Lifecycle

| From | Legal targets |
| --- | --- |
| `proposed` | `registered`, `retired` |
| `registered` | `active`, `degraded`, `maintenance`, `retired` |
| `active` | `degraded`, `maintenance`, `retired` |
| `degraded` | `active`, `maintenance`, `retired` |
| `maintenance` | `active`, `degraded`, `retired` |
| `retired` | none |
| `replaced` | none |

* A facility cannot become active without first being registered.
* `degraded` and `maintenance` are administrative classifications supplied by an
  authorised caller; this library does not detect incidents or orchestrate
  maintenance, and the reason code it records is opaque to it.
* A transition to the current state is `unchanged`, not an error. A transition
  to `retired` records the retirement generation and reason exactly as the
  dedicated retirement operation does.
* `replaced` is unreachable through the transition table. It is set only by the
  replacement operation, which names a successor.
* Both terminal states accept no further lifecycle, metadata or membership
  change: retirement is not a route to reviving authority.
* A live record (`registered`, `active`, `degraded`, `maintenance`) must have
  exactly one primary site membership at all times.

### Site membership

A record carries at most one membership per site and at most one primary.
Attaching a membership that is already present with the same role is
`unchanged`; changing its role is an update. Detaching a site the record is not
a member of is `not_found`. Detaching the primary membership of a live record is
refused with `membership_violation`, because it would leave the record
inconsistent. A record in a terminal state accepts no membership change.

### Replacement

`replace_data_center` retires the predecessor and creates the successor in one
generation, or does neither. It refuses when:

* the predecessor does not exist (`not_found`), or is terminal
  (`terminal_state`);
* the successor identity already exists (`duplicate_identity`) or equals the
  predecessor (`invalid_argument`);
* the successor's compatibility key cannot represent everything the predecessor
  claims (`incompatible_replacement`): a different model major, an older minor,
  or a capability mask that drops a capability;
* the caller's generation is stale (`stale_generation`);
* an alias the successor claims belongs to a third record (`alias_conflict`).

An alias claimed by the predecessor *transfers* to the successor; aliases the
successor does not claim stay with the predecessor as history. The predecessor
records `replaced_by`, the successor records `replaces`, both changes share one
generation, and the the two history entries read in the order the operation
happens.

---

## Deterministic outcomes

Every rejection carries exactly one `ErrorCode`, whose token is part of the
public contract, plus a human-readable message and an optional field name. The
machine-readable categories are:

```
invalid_identity  invalid_metadata  invalid_argument  invalid_query
invalid_provenance  limit_exceeded  not_found  duplicate_identity
alias_conflict  duplicate_membership  membership_violation
stale_generation  stale_revision  stale_authority
illegal_transition  terminal_state  incompatible_replacement
idempotency_conflict
store_not_found  store_corrupt  store_incompatible_version  store_locked
store_io_error  store_read_only  store_limit_exceeded
cancelled  closed  internal_error
```

`is_retryable(code)` marks the categories for which re-reading current state and
retrying the same logical operation can succeed: `stale_generation`,
`stale_revision`, `store_locked` and `cancelled`.

A successful mutation reports one of `created`, `updated`, `unchanged` (the
command was valid and the state already matched it; nothing was written, no
generation moved, no provenance was recorded) or `replayed` (an idempotency key
matched a committed command, so the original outcome is reported again).

**Idempotency.** A command may carry an `IdempotencyKey`. The registry stores a
SHA-256 of the command's semantic content beside the key — excluding the
expected generation, the expected revision and the key itself, so that an honest
retry that re-read state still matches — and reports the original outcome on a
retry. Reusing a key with different content is `idempotency_conflict`, never a
silent re-application. The table is bounded and evicts oldest-first. No-op
outcomes are not recorded: they are recomputed deterministically instead.

---

## Persistence, integrity and recovery

```
<root>/registry.lock                        writer lock
<root>/CURRENT                              names the authoritative generation
<root>/generations/gen-<20 digits>.dcrs     immutable published snapshots
<root>/tmp/                                 staging; never authoritative
<root>/uncommitted/                         verified-but-uncommitted residue
```

**Publication protocol.** plan → validate → reserve the next generation → write
it into `tmp/` → flush it → re-read it and verify its digests → atomically
rename it into `generations/` → atomically replace `CURRENT` → flush the
directory → retire superseded generations beyond the retention window.

The commit point is the atomic replacement of `CURRENT`. A crash before it
leaves the previous generation authoritative and the new file unreferenced; a
crash after it leaves the new generation authoritative. Nothing in `tmp/` is
ever authoritative, and a reader never reads from `tmp/`.

**Format.** A container is an 88-byte header (magic, format version, endian
marker, payload length, payload SHA-256, header SHA-256) followed by the
canonical payload. The payload is a versioned, non-recursive, little-endian
encoding with explicit lengths; every count is checked against the bytes that
actually remain before anything is allocated; enum domains, identifier syntax,
text validity and canonical ordering are all re-checked on load. `CURRENT`
records the payload digest of the generation it names, so an edit that does not
also repair the pointer is caught without decoding anything.

**Recovery.** `RecoveryPolicy::strict` (the default) accepts the store exactly
when `CURRENT` is present, readable, names a generation file that verifies, that
is the file `CURRENT` says it is, and no verified generation file exists that is
newer. Anything else is a hard failure: the registry refuses to guess which
state an operator meant. `RecoveryPolicy::last_known_good` is an explicit opt-in
that adopts the newest verified generation and reports exactly which generation
was discarded, because that choice can discard a committed generation.

**Uncommitted residue.** A generation file newer than `CURRENT` was never
committed, so a writable open moves it into `uncommitted/` — it is neither
adopted nor deleted, and the generation number it claimed becomes usable again.
Publication refuses to write over a file that already claims the target
generation with different content.

**Bounds.** Every unbounded resource is bounded and the bounds are persisted
with the state and re-applied on load: record count, aliases and memberships per
record, metadata entries and value sizes, name and address sizes, provenance
detail, retained history, retained idempotency entries, and the size of a
snapshot that will be read into memory. A store cannot widen its own limits: the
bound in force is the tighter of the persisted bound and the configured one.

---

## Concurrency model

The library creates no threads. Concurrency is caller-driven and bounded:

* Many threads may read concurrently. A read copies a shared pointer to
  immutable state under a short shared lock and then works outside every lock.
* Mutations are serialised by a single writer lock. At most one mutation per
  `Registry` instance is in flight.
* There are no callbacks, observers or subscriptions anywhere in the library, so
  no user code ever runs while an internal lock is held and re-entering the
  registry from a callback is impossible by construction.
* Lock order is fixed and documented in `registry.hpp`: the writer mutex is
  taken before the state mutex, never the reverse. Readers take only the state
  mutex. No path takes the writer mutex while holding the state mutex.
* Cross-process, exactly one writable `Registry` may exist per store root. The
  writer lock is held from open until close. A read-only open takes no lock at
  all and reads published, immutable generations.
* The store additionally refuses to publish a generation that is not the direct
  successor of the generation `CURRENT` names on disk, so a writer whose view is
  stale cannot overwrite a newer commit even if the lock were bypassed.

`close()` releases the writer lock and marks the registry closed. After close,
mutations and `refresh()` are rejected with `closed`; reads continue against the
last committed immutable state, because that state is still known exactly and
reporting zeros for it would be less truthful.

**Audit.** Ownership and call paths were audited for the classic hazards rather
than left to the tests to discover:

* *lock upgrade*: no path takes a shared lock and then tries to take it
  exclusively. A read copies a state pointer and releases; the writer path takes
  the writer mutex first and the state mutex second, for the few instructions of
  the pointer swap.
* *re-entrancy*: the library has no callbacks, so no user code can run while a
  lock is held. The clock is the only injected interface, and it is called
  before any state-guarding lock is taken.
* *writer lock held across re-entry*: the writer path never calls back into the
  registry; planning is a free function over an immutable base state.
* *joining workers while holding state*: the library creates no threads, so
  there is nothing to join and no worker can outlive its state.
* *shutdown*: `close()` takes the writer mutex, so it waits for an in-flight
  mutation to reach its commit point, and no further publication is possible
  afterwards. No path holds the state mutex while waiting for something that
  needs the writer mutex.
* *lock ordering*: writer mutex before state mutex, everywhere, with no path
  that takes them in the other order.
* *stale asynchronous completion*: there is no asynchronous work at all, and a
  cancelled mutation is checked again after its new generation has been written
  and verified and before that generation is published, so cancellation cannot
  publish late.
* *process and file lock inversion*: the writer lock *is* the file lock; no code
  path takes a second lock while holding it.

---

## C++ usage

```cpp
#include <dcr/data_center_registry.hpp>
using namespace dcr;

OpenOptions options;
options.store.root = "/var/lib/dccp/registry";
options.store.mode = StoreOpenMode::open_or_create;

auto opened = Registry::open(options);
if (!opened.has_value()) { /* report opened.error() */ }
Registry& registry = *opened.value().registry;

RegisterCommand registration;
registration.context.provenance.principal = *PrincipalId::parse("operator");
registration.id = *DataCenterId::parse("dc-ashburn-01");
registration.draft.display_name = "Ashburn One";
registration.draft.compatibility = *CompatibilityKey::make(1, 0, 0);
registration.draft.memberships = {
    SiteMembership{*SiteId::parse("site-us-east"), MembershipRole::primary}};
registration.initial_state = LifecycleState::registered;

auto created = registry.register_data_center(registration);   // created
auto again   = registry.register_data_center(registration);   // duplicate_identity

UpdateMetadataCommand update;
update.context = registration.context;
update.context.precondition.expected_generation = registry.generation();
update.id = registration.id;
update.patch.display_name = FieldPatch<std::string>::set("Ashburn One (Building A)");
auto updated = registry.update_metadata(update);              // updated

auto snapshot = registry.snapshot();                          // immutable view
for (const DataCenterRecord& record : snapshot.value().records()) { /* ... */ }
```

`examples/quick_start.cpp` and `examples/lifecycle_and_recovery.cpp` are
compiled and run as part of the test suite.

---

## Inspection and administration tool

`dcr` is a consumer of the same public API. It cannot bypass a precondition, a
lifecycle rule or a generation check, because it has no access the library does
not give it. Arguments are validated before the store is touched.

```
dcr inspect  --root <dir> [--json]        summarise the store and its integrity
dcr verify   --root <dir> [--json]        audit integrity; exit 3 if inconsistent
dcr list     --root <dir> [--json] [--state S] [--site I] [--order O] [--limit N] [--no-terminal]
dcr show     --root <dir> --id <dc-id> [--json]
dcr history  --root <dir> [--id <dc-id>] [--limit N] [--json]
dcr stats    --root <dir> [--json]
dcr limits   --root <dir>

dcr init     --root <dir>
dcr register --root <dir> --id I --name N [--site S] [--state S] [--alias A]
             [--compatibility K] [--region R] [--operator O] [--scope S]
dcr set-name    --root <dir> --id I --name N --expected-generation G
dcr transition  --root <dir> --id I --to S --expected-generation G [--reason R]
dcr attach-site --root <dir> --id I --site S --expected-generation G [--state ROLE]
dcr detach-site --root <dir> --id I --site S --expected-generation G
dcr retire      --root <dir> --id I --expected-generation G [--reason R]
dcr replace     --root <dir> --id I --successor J --name N --expected-generation G
                [--site S] [--state S] [--compatibility K] [--reason R]

common: --principal <id>  --detail <text>  --idempotency-key <key>  --json
```

Exit codes: `0` success, `2` usage error, `3` the registry rejected the
operation (the error category is printed), `4` the store could not be opened or
read. Output is deterministic; `--json` gives a stable machine-readable shape.

```
$ dcr init --root /srv/dccp/registry
created an empty registry at /srv/dccp/registry (generation 0)
$ dcr register --root /srv/dccp/registry --id dc-ashburn-01 --name "Ashburn One" \
      --site site-us-east --state registered --compatibility 1.0+0x0000000000000001
created dc-ashburn-01 at generation 1 (revision 1)
$ dcr set-name --root /srv/dccp/registry --id dc-ashburn-01 --name "Ashburn One (A)" \
      --expected-generation 1
updated dc-ashburn-01 at generation 2 (revision 2)
$ dcr set-name --root /srv/dccp/registry --id dc-ashburn-01 --name "Too Soon" \
      --expected-generation 1
rejected: stale_generation (precondition.expected_generation): the mutation is based on
generation 1 but the registry is at generation 2
$ dcr verify --root /srv/dccp/registry
store       /srv/dccp/registry
files       2 checked, 0 unusable
CURRENT     valid
consistent  yes
```

---

## Validation performed

Everything below was run on the checked-in sources. The suites contain no
timeouts of any kind; a hang is treated as a defect.

**Release and Debug, MSVC 19.44 x64, `/W4 /WX` (zero first-party warnings).**
Seven CTest targets pass in both configurations:

| Target | What it proves |
| --- | --- |
| `dcr_unit` | 104 test cases: value types, SHA-256 vectors, the lifecycle table exhaustively, registration, updates, membership, retirement, replacement, persistence, corruption, adversarial input, property sequences, concurrency |
| `dcr_unit_repeat` | the whole unit suite repeated, so nothing leaks between instances |
| `dcr_multiprocess` | independent operating-system processes: writer fencing, handover, stale-writer refusal, crash recovery, lock release on process death |
| `dcr_multiprocess_repeat` | the same suite three times |
| `dcr_cli` | the tool driven through its command line: exit codes, preconditions, lifecycle, shared store with the library |
| `dcr_example_quick_start`, `dcr_example_lifecycle_and_recovery` | the published examples compile and run |

**Property and invariant tests.** Fixed, recorded seeds (1, 2, 3, 42, 1337,
65537, 99991, 1000003 for the long sequences, and others per test) drive
randomised operation sequences of registers, updates, transitions, membership
changes, retirements and replacements. After every operation the suite asserts:
canonical identity uniqueness; one owner per alias; monotonic revisions; the
generation advancing by exactly one per commit and never for a no-op; terminal
records never accepting a change; replacement links agreeing in both directions
with matching generations; exactly one primary site per live record; and that a
rejected command leaves the authoritative digest byte-identical. A second run
drives two registries with the same command sequence and asserts identical
snapshots after every step. A third closes and reopens the store repeatedly and
asserts the digest, every counter and every record are reproduced exactly.

**Adversarial input.** A truncated payload, a payload whose declared length
disagrees with its file, a bad magic, a wrong endian marker, a future format
version, a flipped header digest, an oversized file, a `CURRENT` that names a
path outside the store, a store with hundreds of generation files, a symlinked
generation file, an unrelated directory that must be left untouched, record
invariants violated directly, and — with the container digests and the `CURRENT`
pointer digest *both* recomputed so that only semantic validation remains — a
duplicate alias, an out-of-domain lifecycle byte, and an in-domain but
inconsistent state. Each is refused; an edited payload that is still a valid
state is accepted, and that boundary is documented rather than hidden. 200
random single-bit corruptions of a published generation were refused, and
restoring the original file made the store usable again.

**Crash and restart.** A real helper process leaves staging residue and a
truncated generation file behind and terminates without unwinding; the parent
recovers to the last committed generation, clears the staging residue, sets the
truncated file aside, and continues writing. A second helper process opens the
store for writing and terminates without releasing the lock; the operating
system releases it with the process and the store is immediately writable again.

**Stale-writer fencing.** In separate processes: a writer holding generation 1
is refused after another process has committed generation 3; the store digest is
unchanged and the record it tried to overwrite still holds its original value;
with the current generation the same mutation commits. Below the registry, a
`SnapshotStore` opened at generation 0 is refused when it tries to publish
generation 1 while `CURRENT` names generation 2.

**Concurrency.** Eight threads against one record commit 200 updates in total:
the revision, the generation and the history count are each exactly 201, and no
update is lost. Eight threads race for the same generation: exactly one wins and
the other seven are told `stale_generation`. Four readers enumerate while a
writer commits: no reader ever observes a partial state, a missing primary site,
or a generation moving backwards. Four writers are interrupted by `close()`:
the generation equals the number of commits exactly, every later attempt is
refused with `closed`, and the store reopens with the same state.

**AddressSanitizer.** `-DDCR_ENABLE_ASAN=ON` (MSVC `/fsanitize=address`) builds
and runs the suite clean; see the limitation below about the runtime DLL.

---

## Performance

`dcr_benchmarks` measures completed operations on a real durable store in the
system temporary directory. Every durable figure includes the flush, because a
commit that is not flushed is not a commit. The workload is **SYNTHETIC**: the
records are generated, not imported from a real facility. The figures below are
one machine's numbers, not a hardware or production claim.

Release, MSVC 19.44, Windows 11, local NVMe, one core, 25 measured operations
per size. Medians in microseconds:

| Operation | 100 records | 1 000 records | 3 000 records |
| --- | --- | --- | --- |
| durable register | 10 397 | 12 047 | 21 670 |
| durable metadata update | 10 744 | 12 101 | 20 629 |
| durable lifecycle transition | 11 950 | 11 586 | 19 807 |
| enumerate every record | 123 | 231 | 1 234 |
| enumerate by site | 7 | 29 | 111 |
| point lookup by identity | 0.4 | 0.3 | 0.4 |
| alias lookup | 0.9 | 2.8 | 11 |
| history page | 4.7 | 3.6 | 7 |
| statistics (includes the payload digest) | 168 | 951 | 4 232 |

Canonical payload size: 22 KB at 100 records, 237 KB at 1 000, 721 KB at
3 000. Reopening and verifying the newest generation takes 4.4 ms, 7.8 ms and
21.9 ms respectively. Every benchmark store verified its integrity after the run,
and the benchmark reports the final digest.

**How to read these numbers.** A commit rewrites the whole snapshot, so durable
mutation cost grows linearly with registry size: it is dominated by writing,
flushing and re-reading the generation file, and it is the price of publishing a
single verified, self-describing artefact rather than a log plus periodic
compaction. For a facility registry — thousands of records, mutations in the
tens per day — that trade buys a store that can be audited, copied and verified
as one file. It is not a design for a million records with a high write rate.
Reads are unaffected: point lookup and alias lookup are sub-microsecond at every
size measured, and the read paths take no writer lock.

Building a registry by registering records one at a time is the slowest possible
way to reach a given size, because each commit rewrites everything written so
far: 0.95 s for 100 records, 10.0 s for 1 000, 47.3 s for 3 000 with relaxed
durability (no fsync). This is reported rather than hidden.

---

## Genuine limitations

* **One writer per store, one store per registry.** There is no replication, no
  sharding and no multi-writer coordination. A second writable open of the same
  root is refused with `store_locked`.
* **A commit rewrites the whole snapshot.** See the performance section. There
  is no incremental journal, no log-structured merge, and no compaction.
* **Integrity checking detects corruption, not a privileged attacker.** A party
  who can rewrite a generation file *and* recompute its digests *and* repair the
  `CURRENT` pointer can change authoritative state. What such a party cannot do
  is make the registry accept a state that violates an invariant: schema, enum
  domains, ordering, cross-record links and every record invariant are re-checked
  on load. Protecting the store from a privileged attacker requires file
  permissions, which are the operator's responsibility.
* **No authentication or authorisation.** The registry records who asserted a
  change; it does not verify that claim, and it does not enforce tenancy policy.
  An in-process caller with a `Registry` handle can submit any command it can
  express. Authority is a boundary for the caller to enforce.
* **Windows does not flush the directory.** `FlushFileBuffers` on the generation
  file plus `MoveFileExW` with `MOVEFILE_WRITE_THROUGH` is what provides
  durability there; a directory handle cannot be flushed through Win32 without a
  volume handle and administrative rights. The POSIX path fsyncs the directory.
* **The independent-process harness is exercised on Windows only.** The library
  has a POSIX path (flock, `fsync`, `posix_spawn` in the test harness), but it is
  compiled rather than run here, and it is not claimed as proved.
* **AddressSanitizer needs its runtime on `PATH`.** With MSVC
  `/fsanitize=address` the runtime is a DLL
  (`clang_rt.asan_dynamic-x86_64.dll`); the test executables need its directory
  on `PATH`. That is stated here rather than hidden.
* **Timestamps come from an injected clock.** Provenance records what the
  supplied clock said. The library does not detect a clock stepping backwards;
  that is an environment fault which provenance should record rather than hide.
* **History is a bounded window.** Old entries are dropped and the drop count is
  part of the state, so a consumer can tell that the retained window does not
  reach back to the beginning. Per-record history is a filter over that window,
  so a very busy registry can evict one record's older entries before another's.
* **No real hardware is involved.** Nothing here talks to a PDU, a UPS, a
  cooling system, a BMS, a switch or an accelerator. The tests exercise a
  library and a filesystem.

---

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
