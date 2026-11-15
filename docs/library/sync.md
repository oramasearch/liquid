# Repository sync protocol

**What.** The HTTP/1.1 protocol used by libliquid clients and lake to exchange repository
metadata and content. lake's `docs/api/sync.md` mirrors this page.

**Status: implemented by liquid #30, #160, #165, #177, #189, #195, and the shared request factoring in
#201.** Metadata export and import are
bounded by caller-provided spans. Imports verify the selected metadata before changing repository
state, while reference authorization and application are reported per ref. Content negotiation and
transfer reuse `remote_blob_source` and the store's pack-entry framing. The public `sync_server`
owns protocol decoding and repository dispatch; `liquid serve sync` is its local HTTP/1.1 adapter.
Conflict objects participate in the same reachable graph as blobs and trees. Accepting a new
`main` ref rebuilds the destination's unresolved index from the version-3 change, so push and a
fresh pull preserve unresolved paths and their base, left, and right objects.
The endpoint, [forge client](forge-client.md), admission evidence, transparency lookups, and
sealed-secret key lookup share the private `src/http1/client.cpp` transport for socket, TLS,
request, and response handling. `src/net/http_client.*` supplies URL decomposition and reusable
request values; forge owns its JSON request and lake-envelope mapping over that transport.

## Repository URL and requests

The URL passed to `liquid push` or `liquid pull` identifies one repository. Clients append the
paths below. Requests and responses use `Content-Type: application/vnd.liquid.sync-v1` and may use
either `Content-Length` or HTTP/1.1 chunked transfer coding. Frame boundaries are independent of
HTTP read boundaries.

The sync adapter uses liquid's shared private Corosio HTTP/1.1 client for URL parsing, request and
response framing, chunked decoding, bearer authentication, and TLS. Protocol paths, media types,
and CBOR envelopes remain owned by the sync layer. An endpoint retains the caller-provided
repository URL as its remote-source identifier while the request client uses the parsed URL, so
metadata-only drive pulls register chunks against the same source that later hydration uses.

| Method and path | Request body | Successful response |
|---|---|---|
| `POST <repository>/sync/push` | client metadata stream | acknowledgement header, optionally followed by server metadata the client must import |
| `POST <repository>/sync/pull` | one header frame containing the client cursor and refs | metadata stream exported after that cursor |
| `POST <repository>/sync/find-missing` | one header frame containing `objects` | one header frame containing the missing subset |
| `POST <repository>/sync/chunks` | zero or more pack-entry frames | empty body |
| `GET <repository>/sync/chunks/<digest>?offset=<n>&length=<n>` | none | raw verified logical bytes for the requested range |

The metadata header is a canonical-CBOR map with `v: 1`, `cursor`, sorted `refs`, and sorted
`objects`. A push request may add `atomic: true`. A push reply may add `outcomes`, an array in
request ref order whose entries are `{name, outcome: "applied"}` or
`{name, outcome: "refused", reason}`. A push header cursor is the end of the exported client
journal. A pull request cursor is the last server cursor observed by that client. A response cursor
is the next opaque cursor to persist for that peer.
`objects` contains the roots referenced by the metadata stream. Missing-object negotiation walks
their content-addressed graph and sends only objects the receiver reports absent.

## Collaboration frames and epochs

Presence uses metadata item kind `5`. Its canonical CBOR body carries actor, session, path,
optional range, activity, and timestamp. Import removes these items from the persistent batch and
updates an in-memory presence table, so presence crosses a peer connection but cannot appear in a
journal, checkpoint, or historical replay. Entries expire 30 seconds after their timestamp.

Semantic lease changes remain signed repository drips so ordinary sync fans them out to editors.
Offline claims have advisory authority. A caller binds its authoritative sync peer through
`repository_options::authorize_lease`; the repository sends the complete proposed lease through
that task before changing local state. An error, including remote `errc::locked`, is final;
acceptance persists `lake` authority. The policy that accepts the claim belongs to lake.

Exclusive checkout rules and lock operations also remain signed repository drips. A version-4
lock drip carries the complete holder, time, reason, authority, path, and configuration where
applicable. Import verifies that the holder TAAP ID matches the drip author. Concurrent offline
locks from different holders produce an issue #40 conflict value with references to both canonical
lock records and retain the receiving checkout's claim. A caller installs
`repository_options::authorize_lock` for connected authoritative acquire and release decisions;
[locks.md](locks.md) defines both authority modes.

An epoch records the heads of the repository and directory documents at its checkpoint. Once the
acknowledgement condition is satisfied, the repository retires its pre-epoch operations and
drips. `repository_options::acknowledge_epoch` bridges the sync peer registry to compaction: it
receives the candidate heads and returns how many known peer cursors cover them. An older cursor
resets to a full checkpoint export; it never asks the peer to replay unavailable drips.

The wire version remains 1 for one compatibility version. A v1 decoder predating #189 ignores the
new header keys and therefore reads a new successful reply. A #189 client also reads a legacy
successful reply with no `outcomes` and reports every requested ref as applied. A legacy server
already treats pushes as all-or-nothing, so `atomic: true` remains safe when that server ignores the
new request key. A legacy authorization failure remains an HTTP error because it cannot supply
per-ref reasons.

An optional `Authorization: Bearer <token>` header carries lake authentication. The CLI reads the
token from `LIQUID_TOKEN`; credentials never appear in the URL or output.

## Drive client layering

Issue #195's [`drive_client`](drive.md) composes two ordinary endpoints around deterministic
three-tree reconciliation. Its `pull_metadata` operation imports tree and blob records into a
remote-view repository while registering payload chunks against a retained endpoint. The remote
view has an independent checkpoint namespace in the synchronized repository's own store.
Reconciliation then records and
publishes local changes before push. The client persists the pull cursor and an interrupted marker
as repository metadata before network transfer. Retrying starts at the last acknowledged cursor
and missing-object negotiation skips chunks already received.

The client's `drive_sync_report` retains pull and push transfer counts and the push's ordered
per-reference outcomes. A refusal reason is surfaced unchanged, the transfer marker clears because
the server replied, and the base tree remains unadvanced so a later run offers the local change
again. Credentials remain an option on the in-memory client and are not part of persisted drive
metadata.

## Metadata stream

Every metadata frame has a one-byte type, a four-byte unsigned big-endian payload length, and that
many payload bytes. Receivers reject unknown types, a length above 16 MiB, truncated frames, bytes
after the end frame, or a stream without an end frame.

| Type | Name | Canonical-CBOR payload |
|---:|---|---|
| 1 | header | the version, cursor, refs, and object roots described above |
| 2 | drip | `{author, body, signature}`; `body` is the exact signed canonical drip body |
| 3 | change | `{author, id, body}`; signed change bytes, digest, and verifiable TAAP key |
| 4 | directory | `{repository, node, root, message}`; two 16-byte IDs, a root marker, and an automerge v1 sync message |
| 5 | ref | `{name, change}` |
| 6 | presence | `{body}`; an ephemeral presence record, applied without changing durable state |
| 7 | identity | `{author, body}`; `body` is an author's identity document chain (`encode_identity_chain`) |
| 255 | end | empty |

Identity frames come first: one per distinct drip and change author whose chain the sender holds.
The sender takes the longer of the chain in its identity store and the one the repository keeps in
its metadata under `liquid.identity-chain.<taap_id>`: a repository records its local signer's chain
when that key first signs a drip there, and every chain it imports, so it can still send them after
the process's home is gone (a new container has a new home). Drips retain journal order. A change follows the drips
it cuts. Directory messages use the
existing checked CRDT receive path, are emitted only for directories reachable from the current
working root, are received into blank documents, and merge into the receiver's reachable candidate
directory documents. They are associated with their directory by `node`. Ref frames are applied
last. An empty repository
adopts the incoming repository and root IDs; a non-empty
repository with a different identity rejects the batch. Duplicate signed records and
already-observed CRDT messages are valid, making a retry idempotent.

Before a checkpoint or export, each reachable directory document is reconciled with the working
tree. This includes parent documents for intermediate directories created by a nested-path write,
so a receiver never observes a child document without its reachable parent entry. Directory CRDT
writes preserve the entry kind (`blob`, `tree`, or `repo`) along with the node, mode, and object
reference. Enumeration reads the current CRDT tree keys instead of reconstructing the current
directory from compacted operation history. Wire synchronization loads the compact current state
as a full document before producing Automerge messages.

Repository checkpoints prefer compact bytes and immediately check that they reopen as a full
sync-capable document. Conflict-heavy states that fail that check use complete upstream history
bytes instead. Import opens a full candidate before receiving peer messages, so a retry can name a
dependency older than a compact checkpoint head.

Before import changes state, the receiver parses every frame, verifies and stores each identity
chain, verifies each actor identifier and TAAP signature against the author's stored chain, checks each change digest and referenced object, applies every selected directory
message to a blank checked receiver, merges each received document into the corresponding
candidate through the checked patch-returning path, and validates the
accepted refs. A validation failure rejects the selected import. The error `item` is the zero-based
metadata-frame index that failed.

A drip or change is attributed to its author only through the author's identity chain (liquid#383).
The receiver accepts an identity frame's chain only when it starts at the initial document named
after its own key, names the frame's author, extends whatever chain is already stored for that
identity, and has every new rotation signed by the key it replaces; it is then stored in the
receiver's `LIQUID_HOME`, even if a later frame fails. A drip or change whose author has no stored
chain is refused as an unknown signer (`errc::unauthorized`, `is_unknown_signer`), and one signed by
a key outside the author's chain is refused as `signing key is not in the identity chain`. A peer
that sends no identity frames, such as one built before liquid#383, therefore cannot deliver another
person's records; its own records are accepted once the receiver trusts its chain by hand
([identity.md](identity.md#how-a-verifier-obtains-and-trusts-another-persons-chain)).

## Chunk stream

Chunk uploads concatenate the store's unencrypted physical pack-entry frames without a pack file
header, table, or footer:

```text
8 bytes  "LQFRAME1"
4 bytes  uncompressed length, little-endian
4 bytes  compressed length, little-endian
1 byte   digest algorithm
32 bytes digest
N bytes  zstd payload
```

The receiver bounds both lengths, decompresses the payload, verifies its algorithm-tagged digest,
and stores the logical bytes through the ordinary store path. Trust-domain encryption, when
configured, is applied by the receiving store; encrypted physical frames are never sent. A chunk
range response is logical content rather than a partial compressed frame and is verified as part
of the complete object read.

## Errors

Non-success responses use `Content-Type: application/vnd.liquid.error-v1` and a canonical-CBOR map
`{v: 1, code, message, context, item?}`. `code` is the stable `liquid::errc` name.

| HTTP status | liquid error |
|---:|---|
| 400 | `invalid_argument`, `corrupt` |
| 401 | `unauthorized` |
| 404 | `not_found` |
| 409 | `already_exists`, `conflict` |
| 423 | `locked` |
| 429 | `busy` |
| 503 | `unavailable` |
| 500 | `io`, `internal` |

Before metadata import, `sync_server` applies the repository's actor, directory-document, and
repository buckets to each authored `sync_item`. A refusal returns the existing HTTP 429 mapping;
the canonical error body also carries `retry_after_ms` and `scope`. The complete metadata batch is
left unapplied, so a rejected peer operation cannot enter causal history. `max_update_bytes`
applies to each item's body.

`sync_endpoint::push` projects a flow rejection into `sync_report::retry_after_ms` and
`exhausted_scope` rather than hiding the peer's retry signal in transport text. `liquid push`
prints `flow_rejected scope=<scope> retry_after_ms=<value>` and exits unsuccessfully. Object
uploads may already exist in the content-addressed store, but the repository document, directory
documents, drips, changes, and refs remain unchanged.

An interrupted request has no success response and is retried from the last acknowledged cursor.
Servers do not advance that cursor until metadata import and required chunk publication both
succeed. A successful reply acknowledges the batch even when it reports refusals; refs are exported
on every later push, so a refused ref is offered again without rewinding the drip cursor.

## Public server and client contract

`sync_server(repository&, store&, options)` requires the repository to use that store. The
repository-only constructor uses the repository's existing store and is intended for adapters such
as the CLI. Its task-returning handlers accept or produce byte spans for push, pull, missing-object
queries, pack-frame uploads, and logical range reads; the canonical codecs remain private.
`sync_server::encode_error` produces the error body described above so an adapter need only choose
the transport status.

`sync_server_options::authorize` is an optional asynchronous callback for changes and reference
updates. Its second argument is a span of `sync_ref_update` values:

```cpp
struct sync_ref_update {
    std::string name;
    std::optional<change_id> old_head;
    change_id new_head;
    bool fast_forward;
};

enum class sync_ref_verdict_kind : std::uint8_t { accepted = 1, refused = 2 };
struct sync_ref_verdict {
    std::string name;
    sync_ref_verdict_kind verdict;
    std::string reason;
};
```

The callback returns `runtime::task<std::vector<sync_ref_verdict>>`. A returned vector reads more
clearly in the public header than a caller-mutated output span: the coroutine result owns every
reason string, and there is no parallel-span lifetime or indexing contract. It contains exactly one
verdict for every input update, identified by name. Wrong counts, duplicate names, and names absent
from the input are authorizer contract errors.

Before invoking the callback, push reads the server's current refs and walks the stored change
graph from every proposed head. A new ref is a fast-forward. An update is a fast-forward when its
old head is reachable from its proposed new head; an unchanged head is therefore also a
fast-forward. The callback never trusts a client-provided fast-forward claim.

Callbacks preserve metadata-frame order while skipping drips and directory frames. A change
callback receives the ref updates whose proposed graph newly reaches that change compared with the
corresponding old graph, in reference-frame order. A change already reachable from a ref's old
head is not presented under that ref again. A reference callback receives a one-element span with
its name, the server's pre-batch head when one exists, proposed head, and computed fast-forward
value. Thus all change decisions precede the exported ref frames that follow them, while each
decision has the complete ref context.

All graph walks and callbacks finish before repository import starts. The first refusal recorded for
a ref supplies its outcome reason verbatim. Refusals do not prevent later callbacks from evaluating
the other refs. A callback task error or thrown exception refuses every ref with its message; no
exception leaves the coroutine boundary.

The server imports shared drip, change, and directory metadata with the accepted ref frames, leaves
refused refs at their pre-push heads, and returns one `sync_ref_outcome` per requested ref. When no
ref is accepted, it skips metadata import entirely. Object uploads happen before authorization and
remain in the content-addressed store regardless of every outcome. With `atomic: true`, any refusal
prevents metadata import: originally refused refs retain their reasons, and otherwise accepted refs
use `atomic push refused because another reference was refused`.

`repository::export_drips` writes as many `sync_item` values as fit in the supplied span and
returns the next cursor plus an end flag. `repository::import_drips` accepts a span, verifies it as
one atomic batch, and reports the number imported. `sync_session` sends or receives an object graph
through a `remote_blob_source` after `find_missing`. `sync_endpoint` implements that source and the
push/pull requests over the vendored `oramasearch/http` module. A caller may put its last observed
cursor and the push's `atomic` choice in `sync_endpoint_options`; `sync_report::references` exposes
the applied or refused result and reason for each ref, and the returned report supplies the next
cursor to retain. All waiting operations are Capy tasks.

`sync_session::receive_metadata(roots)` is the image-root variant. Each root is a tree record. It
range-reads and digest-verifies absent tree and blob records, stores those records locally, and
stops at each decoded blob instead of reading its chunks. The session registers the blob's
whole-content digest, ordered chunks, and sizes with `{source, instance}` from its constructor.
`sync_metadata_report::trees` and `blobs` count records fetched from the peer; local records are
walked but not counted again. `bytes_remote` sums the registered chunk sizes not already local.
Consequently `store::has(blob.content)` reports `remote` for a wholly absent payload immediately
after receipt, and a later store or VFS read hydrates through the registered source.
`sync_endpoint::pull_metadata` applies the same metadata-only traversal to an ordinary pull and
returns `bytes_remote` in its report. Callers retain and register that endpoint under the repository
URL before allowing later reads.

## Trust configuration stays with the checkout that set it

**What (liquid #420).** A repository document holds keys that decide whom a checkout trusts and
what it runs. None of them travels with a clone, a pull, a push, a drive sync or a composition. A
peer can neither set them nor lower them; a value that matters is always one the local user, or
the libliquid caller, set on this checkout.

| Key | Controls | Travels? | Where a local value comes from |
|---|---|---|---|
| `config.remote` (`endpoint`, `instance_name`, `backend`, `tls.enabled`, `tls.verify_peer`, `tls.certificate_authority`, `tls.client_certificate`, `tls.client_key`, `headers`, `publish`) | which REAPI cache or scheduler builds trust, whether its certificate is checked, which client key and bearer headers it receives, and whether results are published to it | no | `repository_options::remote` at creation; `wrap` keeps it in its cache repository under `$XDG_CACHE_HOME/liquid/wrap/` or `~/.cache/liquid/wrap/` ([reapi.md](reapi.md#configuration-and-cli)) |
| `config.hot_update` (`restart_hook`, `targets`) | a command the daemon runs and the sockets it delivers patches to | no | `repository_options::hot_update` at creation ([hot-update.md](hot-update.md#restart-contract-and-daemon)) |
| `config.policy` (`approved_builders`, `rules`) | which builders and attestation authorities a build input must satisfy | no | `repository_options::policy` and `repository::set_policy` |
| `config.deploy` (`control_plane`, environments) | where `liquid deploy` sends `LIQUID_TOKEN` | no | `repository_options::deploy` at creation |
| `config.agents`, `config.encrypt` path rules, `config.cache.publish`, `config.verify`, `config.flow`, `config.retention`, `config.data` | base enforcement for agents, which paths are encrypted, cache publication, reproducibility sampling, rate limits, retention, and data-branch database URLs | no | `repository_options` at creation |
| `config.tuf` (repository metadata) | the pinned TUF root and rollback counters | no | `liquid registry trust pin` after the operator compares the root's SHA-256 out of band ([tuf.md](tuf.md#trust-bootstrap)) |
| `drive.*` (repository metadata) | the drive remote URL, state and device settings | no | `drive_client::open` and the drive settings commands |
| `liquid.identity-chain.<id>` (repository metadata) | an author's key history | yes, by design | each chain is self-verifying from its identifier and is re-verified on receipt; trust decisions stay in the local identity store |
| Lock rules and locks | exclusive-checkout coordination | yes, by design, as signed version-4 drips | they restrict writes and never grant trust or run anything |
| Tracked `.liquid` configuration files | packages, targets, sources, generators, lock rules | yes, as repository content | a build or test runs them only when the user asks for one; `liquid init` reads only their lock rules |

Release policy (`.liquid/release/policy.cbor`), approvals, daemon, capture, index and replication
state are files under the checkout's `.liquid/` directory, so they follow the checkout-local path
rule below.

**Why each one is local-only.** Sync carries only drips, changes, directory documents, refs,
presence and identity chains (`repository::export_drips` in `src/repository/repository_impl.cpp`).
`repository::import_drips` starts its candidate from the local document and changes only the
working tree, refs, `head`, conflict values, drips, locks and identity-chain metadata, so every
other configuration field and every other metadata key is the local value after an import.
`liquid clone` is a drive client over a freshly created local repository (`src/cli/clone.cpp`),
so a clone starts from default configuration and keeps it. Composition (`nest`, `combine`) copies a
tree and its history into the destination, and `split` creates a fresh repository; neither copies
the source's configuration. The CRDT root schema reserves an empty `config` map that nothing reads,
so a peer that writes into it changes nothing.

**Checkout-local paths are never repository content.** The repository marker and journal, the
object store, and liquid's state directories live inside the checkout: `LIQUID_REPO`,
`LIQUID_JOURNAL`, `.liquid/`, `.liquid-store/`, `.liquid-build/`, `.liquid-data/`,
`.liquid-wrap-cache/` and `.liquid-capture-cookie-*` (`src/repository/local_state.hpp`). Tree
import never records them, whatever the caller's `!` patterns say, so a push cannot carry them. A
peer can still name them in a tree it builds by hand, so a drive drops them from every tree it
flattens and refuses to write one, and a clone can never replace the local repository marker (and
with it every key above), the store, or release policy. Before liquid #420 a drive published
these paths from a peer's tree like any other file.

**Existing repositories.** Every value in a repository document was set through the local API
(`repository_options`, `set_policy`, `set_metadata`, `registry trust pin`), because no import path
writes configuration, so existing values keep working without migration. The one way a remote
value could have reached a checkout was a drive writing a peer's `LIQUID_REPO` over the local one;
a checkout synchronized from an untrusted drive remote before this change should be cloned again.

**Repository transfer is not a clone.** `repository::transfer` copies a repository between two
stores of the same owner, configuration included, because it moves one repository rather than
receiving another ([store.md](store.md#whole-repository-transfer)).

**Tests.** `Sync.trust_configuration_never_travels_with_exported_drips` sets `config.remote` with
peer verification off, a client key and a bearer header, a restart hook and target, a deployment
control plane, agent rules, cache publication and a `config.tuf` state on a source, imports its
complete export into an empty repository, and checks that none of them arrived, before and after
reopening. The other configuration fields take the same path through the document.
`Sync.pull_and_drive_clone_leave_trust_configuration_behind` does the same through `sync_server`
over HTTP with a pull and with a drive clone. `Drive.peer_tree_cannot_write_checkout_local_state`
serves a tree that names every checkout-local path and checks that a drive writes none of them.
`Repository.import_tree_never_records_checkout_local_state` checks the import side.

## Local server command

`liquid serve sync <path>` opens an existing repository and serves the five routes above with the
vendored `oramasearch/http` router and serializer. It binds `127.0.0.1:8080` by default;
`--listen <address>` and `--port <port>` select another listener. Repeating `--allow-ref <glob>`
installs a reference policy that permits only names matching at least one supplied glob; `*`
matches any sequence and `?` matches one character. A disallowed ref is refused while allowed refs
from the same non-atomic push apply. Without that flag the command supplies no authentication
policy. Keep the loopback default for local exchange, or embed `sync_server` with an authorization
callback when exposing a service boundary.

## How to verify

The sync tests exchange two on-disk repositories through `sync_server`, prove that a second
exchange transfers only newly introduced chunks, retry unchanged metadata, and pull into an empty
repository. They also corrupt a TAAP signature, associate newly reachable changes with their refs,
report false for a divergent update's fast-forward field, apply mixed outcomes, preserve refusal
reasons, keep an all-refused repository unchanged, retain uploaded objects, make atomic pushes
all-or-nothing, convert an authorizer exception into per-ref refusals, and decode a legacy reply.
The CLI test spawns `liquid serve sync --allow-ref main`, observes one line per ref for ordinary and
atomic pushes, and runs the real `liquid push` and `liquid pull` commands. Static, shared, and
Linux-container proofs run these tests with the rest of the suite.
