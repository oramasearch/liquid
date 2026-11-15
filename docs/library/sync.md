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

The sender validates the complete missing-object response against that exact offered closure,
including each digest's algorithm, before loading payloads or uploading any object (liquid #446).
Any unoffered identity returns `corrupt`, even if it exists elsewhere in the local store or follows
valid requested identities. Empty roots permit only an empty missing response. This check lives in
`sync_session::send`, so it applies to HTTP pushes and callers supplying their own
`remote_blob_source`; valid subsets still load payload chunks only when requested.

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
token from `LIQUID_TOKEN`; credentials never appear in the URL or output. `liquid serve sync
--token-file` requires the same header on every route ([Local server command](#local-server-command)).

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
([identity.md](identity.md#how-a-verifier-obtains-and-trusts-another-persons-chain)). The same
holds for a change as for a drip: a change whose author names one identity but is signed by
another's key is refused on import (liquid#429). The push authorizer (`sync_authorizer`) runs
before that import, so the `author` it sees is still a claim; authorize on the authenticated
caller.

## Upload quarantine (liquid#403)

Uploads arrive before the push that authorizes them, so `sync_server` does not write them to the
store. `POST /sync/chunks` verifies each frame and stages the objects the store lacks in a
quarantine: a private directory (mode 0700) created inside `sync_server_options::staging_directory`,
or the system temporary directory, and removed with the server.

- **Authorized push.** When a push imports anything (at least one ref accepted, or no refs at all),
  the staged objects its `objects` roots reach move into the store, children first and each digest
  verified again, before its metadata is imported. The walk follows the same edges as
  `sync_session::send`, so it reaches every object the push uploaded.
- **Refused push.** When every ref is refused, an atomic push is refused, flow admission refuses it,
  or an item's author does not verify, the staged objects its roots reach are discarded. A refused
  push leaves nothing in the store.
- **Quota.** The quarantine holds at most `upload_quota_bytes` (default 1 GiB; `serve sync
  --upload-quota <bytes>`) at once. A chunk request that would exceed it is refused whole with
  `errc::busy` (HTTP 429, `scope=upload`) and stages nothing. Objects already staged are not
  charged again. A push whose uploads exceed the quota cannot complete until the quota is raised.
- **Expiry.** An object no push claims within `upload_ttl` (default one hour) is discarded at the
  next upload.
- **Visibility.** `find-missing` answers from the store alone and range reads serve only stored
  objects, so a staged object is neither visible nor readable, and a retried push uploads it again
  without being charged twice.

Concurrent pushes share the quarantine. A refused push discards an object a concurrent push also
uploaded; that push then fails to import and its retry uploads the object again. Authorization still
runs before anything is stored, and an accepted push moves only what its own roots reach. When
authorization accepts a push but import then fails validation, its objects are already in the store,
unreferenced, for collection to remove.

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

The receiver bounds both lengths of every frame to 16 MiB, then charges each frame's declared
uncompressed length against a 64 MiB budget for the whole request. Repeated frames are charged
every time they appear, so a request whose frames together declare more than 64 MiB is refused
with `invalid_argument` before anything is decompressed or stored (liquid#435). An accepted request
is then decoded one frame at a time: the receiver decompresses the payload, verifies its
algorithm-tagged digest, and stores the logical bytes through the ordinary store path before it
decodes the next frame, so at most one decoded frame is held at once. A frame that fails
decompression or verification ends the request with an error; frames stored before it remain,
each already verified. Trust-domain encryption, when
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
repository buckets to each `sync_item`. The actor bucket is the **authenticated pusher's**, never
the author a record claims, because that name is not verified until import (liquid#508). The host
names the pusher, the session or TAAP identity that authenticated the request, in
`sync_server_options::pusher` or per request with `handle_push(request, pusher)`; Lake is to pass
its signed-in caller, which it does not do yet. `actor_overrides` are keyed the same way. A push with no pusher is charged to
the shared `unauthenticated_pusher` bucket, which is what `liquid serve sync` does today because it
does not authenticate pushes. A refusal returns the existing HTTP 429 mapping;
the canonical error body also carries `retry_after_ms` and `scope`. The complete metadata batch is
left unapplied, so a rejected peer operation cannot enter causal history. `max_update_bytes`
applies to each item's body.

The actor bucket is keyed on an item's author only after that author is proven (liquid#403): before
admission, every authored item is checked against the author's identity chain, the stored one or
the one the push carries. An identity frame must hold a valid chain named after its author, and a
drip or change must be signed by a key in that chain. Any other frame kind that names an author,
or an item that fails this check, refuses the whole push with that item's index before any budget
is spent, so a peer cannot exhaust another author's budget with records it cannot sign. Import
repeats the full verification. The budget remains per author, not per connection: a peer that
replays an author's genuinely signed records still spends that author's budget.

`sync_endpoint::push` projects a flow rejection into `sync_report::retry_after_ms` and
`exhausted_scope` rather than hiding the peer's retry signal in transport text. `liquid push`
prints `flow_rejected scope=<scope> retry_after_ms=<value>` and exits unsuccessfully. A rejected
push leaves the repository document, directory documents, drips, changes, refs, and the store
unchanged: its uploads are discarded with it.

### Retrying a refusal that carries `Retry-After`

A peer that is briefly unable to serve answers `503` (or `429` for a rate limit) with a
`Retry-After` header. Lake does this with `Retry-After: 1` while a workspace store is handed over
during a rollout, while another lake process holds the store's lease, and while a store is full
(liquid #340). `sync_endpoint` sends such a request again after the wait the header asks for,
read as delay-seconds or as an HTTP-date in any of the three forms RFC 9110 requires a recipient
to accept (`net/retry_after.cpp`). It applies to every request the client makes: find-missing,
chunk uploads and range reads, push and pull. Each is safe to repeat. Find-missing, pull and
range reads change nothing; a chunk upload is content-addressed; and a push refused with `503` or
`429` was not applied, because lake refuses the whole batch before import and liquid's flow
control rejects it before import.

The retries are bounded per request by two `sync_endpoint_options`:

| Option | Default | Meaning |
|---|---:|---|
| `retry_attempts` | 8 | The most times one request is sent, the first included. `1` turns retrying off. |
| `retry_budget` | 30 s | The most one request waits in total. A wait that would pass it is not taken. |

When either is spent, or the requested wait would pass the budget, the last refusal is returned
exactly as before (`503` maps to `unavailable`, `429` to `busy` or the flow-rejection report). A
`503` or `429` without `Retry-After`, a malformed `Retry-After`, and every other status are never
retried. liquid's own `serve sync` sends no `Retry-After`, so its flow rejections still surface
at once as `sync_report::retry_after_ms`. `http1::response` and `net_detail::http_response` carry
every response header field, so the client reads `Retry-After` from the response it received.

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
ref is accepted, it skips metadata import entirely and discards the push's quarantined uploads.
Graph walks read change records from the store or the quarantine. With `atomic: true`, any refusal
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
from the same non-atomic push apply. `--upload-quota <bytes>` sets the quarantine quota.

Transport security and authentication (liquid#403) use the `listener_security` rules shared with
`serve reapi` ([`liquid/listener.hpp`](../../include/liquid/listener.hpp)):

- A listener on any address other than `127.0.0.0/8` or `::1` is refused at startup, before the
  repository is opened, unless it has TLS (`--tls-cert` and `--tls-key`) **and** client
  authentication: `--token-file <path>` or `--client-ca <path>`. There is no plaintext mode off
  loopback.
- `--token-file` reads a bearer token from a regular file owned by the user running liquid with no
  group or other permissions; the token never travels in argv. Every route then requires
  `Authorization: Bearer <token>`, compared in constant time, and a request without it is answered
  `401 unauthorized` before its body is read. Clients send it from `LIQUID_TOKEN`.
- `--client-ca` requires every TLS client to present a certificate that CA signs; the handshake fails
  otherwise. `liquid push` and `liquid pull` do not yet present client certificates, so they reach
  such a listener only through another client or a terminating proxy; use `--token-file` for them.
- Each connection serves one request (`Connection: close`) and a request body is limited to 64 MiB,
  so the per-connection upload bound is the request bound; the quarantine quota bounds all staged
  uploads together.

`--allow-ref` is authorization, not authentication: it decides which refs an authenticated client
may move. Embed `sync_server` with an authorization callback for any other policy.

## How to verify

The sync tests exchange two on-disk repositories through `sync_server`, prove that a second
exchange transfers only newly introduced chunks, retry unchanged metadata, and pull into an empty
repository. They also corrupt a TAAP signature, associate newly reachable changes with their refs,
report false for a divergent update's fast-forward field, apply mixed outcomes, preserve refusal
reasons, keep an all-refused repository unchanged, leave no uploaded object of a refused push in the
store, quarantine uploads under the quota and refuse a push beyond it, keep a forged author from
spending the named author's flow budget, make atomic pushes all-or-nothing, convert an authorizer
exception into per-ref refusals, and decode a legacy reply. The CLI tests spawn `liquid serve sync
--allow-ref main`, observe one line per ref for ordinary and atomic pushes, run the real `liquid
push` and `liquid pull` commands, and require `--token-file` to refuse a missing or wrong token;
`cli-serve-listen` requires every insecure non-loopback listener to be refused. Static, shared, and
Linux-container proofs run these tests with the rest of the suite.
