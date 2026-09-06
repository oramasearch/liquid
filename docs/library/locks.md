# Exclusive checkout locks

**What.** Issue #42 adds repository-configured path locks, the `liquid lock` and `liquid unlock`
commands, signed synchronization records, connected authorization hooks, and daemon delivery for
editors.

**Status: implemented on the liquid client.** Offline locks are advisory repository records and
connected locks use a caller-supplied lake authorization task. The authoritative lake service is a
separate lake leaf.

## Configuration

A path can be locked only when it matches at least one named rule in repository configuration:

```liquid
lock generated-assets {
  paths: [path "assets/**", path "models/*.bin"]
}
```

`*` and `?` match within one path component. `**` also crosses `/`. Patterns are repository
relative; absolute paths, empty patterns, trailing `/`, and parent traversal are rejected. Rule
names are unique and each rule has at least one pattern.

`liquid init` lowers the selected top-level `.liquid` file and stores its lock configuration in
the repository document. `repository_options::locks` supplies the same configuration to library
callers. `repository::set_lock_configuration` replaces it with a signed configuration drip, and
`repository::lock_configuration` returns the effective value.

## Record and write behavior

An `exclusive_lock` records:

- the repository-relative path;
- the holder's TAAP actor and key fingerprint;
- the acquisition time as whole UTC seconds;
- the non-empty reason; and
- whether its authority is `advisory` or `lake`.

`repository::lock(path, reason)` is idempotent for the current holder. A lock applies to the named
path and descendants. The holder may continue writing, moving, and removing its locked content.
A different actor receives `errc::locked`; the error message contains the holder identity,
acquisition time, and reason. Only the holder can release the record. Successful acquire and
release operations append signed version-4 drips and survive repository reopen.

The command line uses the checkout daemon when available:

```sh
liquid lock assets/model.bin --reason "replace training snapshot"
liquid unlock assets/model.bin
```

The default reason is `exclusive checkout`. A path outside every configured rule is rejected.

## Authority model

There are two modes with the same public record and write checks.

### Offline advisory

Without `repository_options::authorize_lock`, acquisition is recorded locally with advisory
authority. It remains effective while disconnected and is exchanged as ordinary signed repository
metadata. This is advisory because two disconnected actors may each acquire the same path before
either observes the other.

During sync, a lock whose holder matches the signed drip author is accepted. Equal holder records
are idempotent. Concurrent records for different holders remain local and are surfaced as an issue
#40 `conflict` value at the locked path. Its two references identify canonical lock records, so a
client can show both claims without silently choosing a winner.

### Connected authoritative

`repository_options::authorize_lock` and `daemon_options::authorize_lock` bind an asynchronous
authority. Before local state changes, liquid sends a `lock_authorization_request` containing the
acquire or release operation and complete proposed record. An error is final and no drip is
written. Acceptance stores `lock_authority::lake`; subsequent local writes respect the stored
answer.

The callback is the client boundary only. Lake must authorize the principal, repository, path,
operation, and current holder using its policy surface, then return the authoritative decision.
A lake-authoritative record cannot be released through a repository handle that has no connected
authorizer. No lake implementation or dependency is linked into libliquid.

## Daemon delivery

The local daemon accepts framed `lock` and `unlock` requests. After a successful repository
operation it calls `daemon_target::deliver_locks` with the complete current lock set for every
target. Lock snapshots do not advance a target's persisted drip cursor. Editor targets can render
holder, time, reason, and authority directly from the public values.

## Public API

The public values live in `liquid/session.hpp`; repository and daemon operations are in their
respective headers:

```cpp
struct lock_rule { std::string name; std::vector<std::string> paths; };
struct lock_config { std::vector<lock_rule> rules; };
enum class lock_authority { advisory, lake };
struct exclusive_lock { std::string path; actor holder; std::chrono::sys_seconds since;
                        std::string reason; lock_authority authority; };
enum class lock_operation { acquire, release };
struct lock_authorization_request { lock_operation operation; exclusive_lock value; };

runtime::task<void> repository::lock(std::string path, std::string reason);
runtime::task<void> repository::unlock(std::string path);
runtime::task<std::vector<exclusive_lock>> repository::locks();
runtime::task<void> daemon::lock(std::string path, std::string reason);
runtime::task<void> daemon::unlock(std::string path);
```

## Verification

Repository tests cover holder writes, another identity's detailed refusal, holder-only release,
connected acquire/release authorization, persistence, and divergent offline reconciliation. Daemon
tests cover complete lock-set delivery to every target and lock/unlock requests. The CLI test
initializes rules from `.liquid`, acquires with a reason, and releases through the daemon.

## Deferred

- Lake leaf: **authoritative exclusive-lock service and policy authorization**. It owns durable
  connected lock state and the policy decision behind the client callback.
