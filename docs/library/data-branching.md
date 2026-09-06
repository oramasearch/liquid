# Data branching for agent sessions

**Decision.** A session owns one disposable branch for each configured application database.
Sandboxed actions opened under that session receive the branch URL through the database's declared
environment variable. They never receive the base URL. PostgreSQL template copies are the portable
baseline; a filesystem reflink of a local, quiescent cluster is the fast path when the host and
filesystem can support a second server safely.

This is application-data isolation. It does not put liquid's repository store in PostgreSQL, add a
database client library to libliquid, or change the rule that liquid's store has no third-party
database or key-value-store dependency.

## Configuration and ownership

Repository creation accepts this persisted configuration:

```text
config.data.databases: [
  {
    name: "app",
    url_env: "DATABASE_URL",
    base_url: "postgresql://user:password@127.0.0.1:5432/app",
    mechanism: template | reflink | auto
  }
]
```

`name` is the stable repository-local name used by the API and CLI. `url_env` is unique across the
configuration, and `base_url` names a PostgreSQL database rather than a server default. Credentials
remain in the repository document under the same custody as the rest of its configuration; CLI
output never prints the base URL. `repository_options::data` supplies the typed configuration at
creation, and reopen uses the persisted value.

A live branch record contains its ID, session ID, configured database name, branch URL, mechanism,
and creation time. Private promotion state additionally retains the base URL and a base-content
fingerprint captured at the branch point. Branch records are repository state, not liquid store
objects and not application-schema objects. A `(session, database)` pair has at most one live
branch.

## Lifecycle

`data_branch` exposes four operations:

1. **`create`** validates the configured database, creates an isolated PostgreSQL database or data
   directory, captures the base fingerprint, and publishes the live record only after the database
   is reachable. A partial database is removed before an error is returned.
2. **`list`** returns every live record in stable creation order. `liquid status` includes the same
   records so abandoned session data is visible.
3. **`discard`** removes the branch database and then removes the live record. A future eligible
   reflink branch also stops its server and removes its cloned directory. Repeating discard for an
   absent ID reports `not_found`.
4. **`promote`** fingerprints the base again. If it differs from the branch-point fingerprint,
   promotion is refused with `conflict` and a reason that the base changed. If it agrees, the base
   database is replaced by the branch and the live record is removed. The old base is retained under
   a temporary name until the branch has successfully taken the base name, so a failed rename can be
   rolled back.

Promotion does not merge rows, schemas, migrations, or transaction logs. A base write after the
branch point is a hard conflict even when it touched an unrelated table. This conservative rule
keeps promotion deterministic and prevents a successful-looking loss of another session's data.

Branch, discard, and promote serialize through a repository-local operation lock. The database
work completes before the updated repository document is checkpointed. Recovery may therefore find
an untracked database after a process stops between those two steps; generated names carry the
configured database and session identity so a later cleanup command can identify them without treating an
unrelated database as liquid-owned.

## PostgreSQL mechanisms

### Template copy

The portable implementation uses `CREATE DATABASE ... TEMPLATE ...` through PostgreSQL client
tools. It makes a complete independent copy, so its duration and bytes grow with the base. PostgreSQL
requires the template database to have no concurrent sessions during the copy. liquid reports that
server refusal; it does not terminate an unknown application connection to make creation succeed.

Promotion connects through a maintenance database, checks the saved base fingerprint, terminates
connections only after the conflict check, renames the base aside, renames the branch into place,
and drops the old base. This requires an owner or administrator role able to create, rename, and
drop databases.

### Reflink fast path

The fast path clones a local PostgreSQL cluster directory with copy-on-write filesystem operations
and starts a second `postgres` process on a session-owned port. It is eligible only when all of these
conditions hold:

- the cluster data directory is local and readable by the liquid process;
- the source cluster is quiescent, with no live `postmaster.pid`;
- the destination filesystem proves a copy-on-write clone operation;
- compatible `postgres` and control binaries are present; and
- a private port can be reserved for the session server.

APFS `clonefile` and Linux `FICLONE` are the supported probes. btrfs and XFS commonly satisfy the
Linux probe; ext4 commonly does not. A clone taken from a running PostgreSQL data directory is not a
consistent snapshot and is never attempted. An explicit `reflink` request returns `unsupported`
when any eligibility check fails. `auto` chooses reflink only when every check passes and otherwise
uses the template mechanism.

The cloned cluster contains the base cluster's databases, although only the configured database URL
is exported to the session. This mechanism therefore needs more host isolation than template copy
and is useful primarily for local agent sandboxes whose base cluster can be stopped at the branch
point.

The prototype implements the APFS/Linux reflink capability probe and the `auto` eligibility
selector, while the complete branch lifecycle currently uses template copies. The prescribed
database declaration has a URL but no local cluster directory, control program, or ownership
contract; liquid therefore cannot safely stop and clone that cluster. `auto` falls back to
`template`, and an explicit `reflink` declaration returns `unsupported`, until that local-cluster
metadata is part of the interface.

## Sandboxed client actions

libliquid links no `libpq`. Once per branch, discard, promotion, or test operation it resolves
`psql`, `createdb`, `dropdb`, and `pg_dump` from `pg_config --bindir`, falling back to `PATH` when
`pg_config` is unavailable. The data-action record stores the canonical absolute executable path;
the data-action profile's existing installed-tool roots permit that path and invoke it directly.
It does not copy a client binary into the repository's temporary action directory, which preserves
the host's code-signing and execution policy. The action still creates its toolchain directory for
generated scripts. This exception does not grant ordinary sandboxed actions access to an arbitrary
external executable. The data-action sandbox retains the fixed environment and filesystem
restrictions while explicitly enabling network access for the configured PostgreSQL endpoint.
Ordinary build actions remain network-denied.

Every implemented potentially waiting step is coroutine-native. Client processes, filesystem
capability probes, and process waits run through `runtime::offload` or the sandbox executor. No
coroutine calls `block_on`, and the implementation creates no raw thread. The same rule applies to
future directory cloning, server launch, and shutdown.

When `wrap` is given a session ID, it looks up that session's live branch for every configured
database and adds `url_env=branch.url` to the action environment before action identity is computed.
It refuses the wrap when a configured branch is absent. A sibling session resolves its own records,
so both action cache identity and runtime access remain session-specific.

## Fingerprint and concurrency limit

The prototype fingerprints the logical PostgreSQL contents with a deterministic plain `pg_dump`
projection that excludes ownership and privileges and removes PostgreSQL 17's randomized
`\\restrict` guards. The same client version and flags are used at branch and promotion. This
detects schema and row changes without installing triggers or extensions
in the user's database. The base must reject writes while the promotion fingerprint and rename
sequence runs; otherwise a write after the comparison could be lost. The implementation obtains the
database-level exclusion available to the configured administrative role and refuses promotion when
it cannot establish it.

The fingerprint is a prototype guard, not a general data-merging protocol. Large databases pay the
full dump-read cost during creation and promotion, and tables without a stable dump order may cause a
conservative refusal.

## Explicit exclusions

- Divergent-data merge or conflict resolution.
- MySQL, SQLite, document stores, object stores, queues, caches, and other database engines.
- Hosted branching services or a page server operated by liquid.
- Schema-per-session rewriting, which does not isolate migrations or destructive statements.
- External services reached by the application outside a declared database URL.
- The agent delegation/session model; this design consumes its session ID.
- Managed data services owned by lake or the Orama operator.
