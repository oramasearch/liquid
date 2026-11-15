# Forge client

**What.** `liquid/forge.hpp` is the coroutine-native client for reviewing one lake repository
without translating changes into pull requests. The constructor accepts a lake repository URL of
the form `https://host/v1/repositories/<repository-id>`. Every operation is a lazy Capy task and
uses liquid's private HTTP/1.1 implementation; no HTTP or JSON type appears in the public API.

**Status: implemented by liquid #201.** Reviews are bound to one `change_id`. Comments carry a
one-based `text_anchor`, mapped anchors record movement, approximation, or deletion, and branch
protections expose the outcome rules lake applies to pushes.

## Values and calls

`forge_review` contains its lake ID, change, `open`, `approved`, or `closed` state, reviewers, and
created and updated UTC timestamps. `forge_comment` contains its ID, review ID, optional TAAP
actor, body, source anchor, optional mapped anchor, and creation time. `forge_branch_protection`
contains the ref pattern, review, verification, and fast-forward requirements, plus allowed
pushers. `forge_client_options` carries the bearer token and a two-second default poll interval.

| Client call | Lake capability | Behavior |
|---|---|---|
| `list_reviews(state)` | `GET /v1/list_reviews` | Lists reviews, with an optional client-side state filter. |
| `get_review(id)` | `GET /v1/list_reviews` | Selects one review or returns `not_found`. |
| `create_review(change, reviewers)` | `POST /v1/create_review` | Opens a review on one change. |
| `set_review_state(id, state)` | `POST /v1/update_review` | Sets `open`, `approved`, or `closed`. |
| `list_comments(review)` | `GET /v1/list_review_comments` | Selects comments for one review. |
| `create_comment(review, body, anchor)` | `POST /v1/create_review_comment` | Creates an anchored comment. |
| `advance_anchors(repository, review, target)` | `POST /v1/map_review_anchors` | Computes text hunks and updates every commented path. |
| `branch_protections()` | `GET /v1/list_branch_protections` | Lists the repository's protection records. |
| `watch(review)` | sync plus both list calls | Returns a move-only polling sequence. |

Lake has no fourth review state for requested changes. A client requests changes by creating a
comment and leaving, or setting, the review state to `open`.

## Anchor advancement

`advance_anchors` loads the review's source change, calls
`repository::diff(source, target, {.text = true})`, groups existing comments by path, and sends one
map action for each path. The exact JSON body consumed by lake is pinned here because lake's API
page does not yet pin the hunk member names:

```json
{
  "repository_id": "repository-id",
  "source_change": "blake3:...",
  "target_change": "blake3:...",
  "path": "src/file.cpp",
  "hunks": [
    {"old_line": 4, "old_count": 2, "new_line": 5, "new_count": 2}
  ]
}
```

The returned comments are reloaded after all map actions. Their anchors contain the target change
and line, with a mapped disposition of `same`, `shifted`, `approximate`, or `deleted`. The public
`mapped_anchor` represents those as its `moved`, `approximate`, and `deleted` flags.

## Watch and transport

`watch` snapshots the selected review and its comment IDs, creates a private temporary repository
mirror, and performs an initial sync pull. Each `next()` awaits `runtime::delay(poll_interval)`,
pulls the repository, and reloads reviews and comments. A changed repository head yields
`head_moved`; a changed review timestamp yields `review_updated`; unseen comment IDs yield
`comment_added`. Events discovered in one poll are returned in that order. Existing state is the
baseline and is not emitted. Destroying the watch removes its temporary mirror.

This polling contract deliberately requires no lake streaming endpoint. Lake could reduce list
traffic by adding direct review lookup, review/comment filters, and a `since` cursor; the client
does not assume those capabilities.

The private HTTP client parses HTTP and HTTPS URLs, resolves and connects on the calling executor,
validates TLS peers, decodes content-length or chunked responses, and adds
`Authorization: Bearer <token>` when configured. `SSL_CERT_FILE` selects an explicit trust bundle;
otherwise platform default trust paths are used. `SSL_CERT_FILE` *replaces* the trust store, so
whoever sets it decides which servers are trusted. The bearer is a header only: it is never placed in
a URL, diagnostic, or command output.

**Plain http only where no network attacker sits between** (L-10, liquid#392). A request that
carries an `Authorization` header, or is marked sensitive (every Lake API call, so a sealing key or a
custody reply cannot be swapped, and every custody wrap or unwrap), goes over `http://` only to
loopback, to a Kubernetes Service inside the cluster (`*.svc`, `*.svc.cluster.local`), or anywhere
when `LIQUID_ALLOW_INSECURE_HTTP=1` is set. Anywhere else it is refused (`invalid_argument`) before a
name is resolved. A request with no credential, such as a digest-checked source archive, is not
affected.

**Every answer is bounded** (L-24, liquid#436 and liquid#395). A response is read for at most
`read_timeout` (120 s) and at most `maximum_response_bytes` (64 MiB) of body, plus its header;
past either it is an error. A TLS peer that closes without `close_notify` is accepted only when the
response states its length (`Content-Length` or chunked), which the parse checks. A body that runs
to the close could have been cut short, so it is `corrupt`.

Lake responses use `{ok, value}` or `{ok: false, error: {code, message, path}}`. The message is
preserved and `path` becomes `error::context`.

| Lake code | `liquid::errc` |
|---|---|
| `invalid_input` | `invalid_argument` |
| `unauthorized` | `unauthorized` |
| `mfa_required` | `unauthorized` |
| `not_found` | `not_found` |
| `conflict` | `conflict` |
| `store_unavailable` | `unavailable` |
| any other code | `internal` |

## Test double

The install exports `liquid_forge_double` and the `liquid-forge-double` executable. The binary is
started with `liquid-forge-double --listen <address>:<port>` and prints its repository URL once the
listener is ready. It holds reviews, comments, protection records, and the review state machine in
memory. The installed target carries its HTTP and TLS transport dependencies for both static and
shared libliquid packages, and serves these seven lake capabilities:

```text
GET  /v1/list_reviews
POST /v1/create_review
POST /v1/update_review
GET  /v1/list_review_comments
POST /v1/create_review_comment
POST /v1/map_review_anchors
GET  /v1/list_branch_protections
```

It also serves the repository sync routes used by `watch`. Its envelopes, errors, and three-state
review transitions match lake, and anchor mapping covers `same`, `shifted`, `approximate`, and
`deleted`. C++ integration tests can include `<liquid/test/forge_double.hpp>` and construct HTTPS
mode with a certificate and private key. The standalone process uses HTTP for local downstream
tests and stops on Ctrl-C.
