# Inline secret scanning and redaction

**What.** Local capture scans changed file chunks before storing or appending a drip. A finding
blocks by default and is reported as `errc::unauthorized` with only its path, detector identifier,
and line number. Every accepted scanned drip references a signed `scan_result/1` attestation.
Repository redaction replaces a path and makes the replaced blob digest unreadable before it
returns.

**Why.** Automatic capture records writes that a developer might delete before naming a change.
Scanning after append would therefore be too late. Diagnostics and attestations deliberately omit
matched values so the scanner cannot become a second disclosure channel.

## Detector set

The private implementation in `src/scan/` uses bounded token matchers and line-oriented parsers,
not a third-party regular-expression engine.

| Detector id | Shape |
|---|---|
| `aws-access-key` | `AKIA` followed by 16 uppercase letters or digits |
| `aws-secret-key` | a 40-character base64 value in AWS secret-key assignment context |
| `github-token` | `ghp_`, `gho_`, and `github_pat_` token forms |
| `slack-token` | the `xox` Slack token families |
| `google-api-key` | `AIza` followed by 35 base64url characters |
| `private-key` | RSA, EC, OpenSSH, or PGP private-key PEM headers |
| `jwt` | three base64url components of at least eight characters, with the first beginning `eyJ` |
| `generic-assignment` | `password`, `secret`, or `token` assignments with at least 16 characters and at least 3.5 bits of Shannon entropy per character |
| `dotenv-assignment` | uppercase `.env`-style keys with values of at least 20 characters and at least 4.0 bits of Shannon entropy per character |

Each detector has matching and harmless cases under `tests/scan/`. Entropy is computed over byte
frequencies in the value after surrounding quotes are removed. It is a supporting signal, not a
claim that arbitrary high-entropy text is a credential.

The JWT matcher walks maximal base64url runs once and searches each run once for its earliest
`eyJ` prefix. Two flags record whether preceding dot-separated runs can supply a header and
payload. Every suffix candidate in one run shares its following components, so its earliest
prefix is sufficient: later prefixes only shorten the header. Each later run can still begin a
new token. Work is linear in line length and auxiliary storage is constant, including on long
invalid runs with repeated prefixes (liquid #440, LSR-130). Capture and direct detector callers
use the same matcher; allow rules, line markers, and matches from other detectors do not change
its work bound. Input is neither truncated nor refused by size.

## Capture decision and repository configuration

`capture_options::scan` defaults to `true`. `scan_config.allow` is a list of path rules; each rule
has a glob and zero or more detector ids. An empty detector list permits every detector at that
path. `*` stays within a path component, `**` crosses components, and `?` matches one non-slash
character. `scan_config.ignore_lines` defaults to the literal marker
`liquid:allow-secret`. The marker permits findings only on the line that carries it.

Those are the only two exceptions. A path rule and a line marker are both copied into the
attestation as `{path, detector, line, reason}` with reason `allow-list` or `line-marker`. The raw
line and matched value are never copied.

Capture compares FastCDC chunk ids with the last accepted chunk list for the path. On the first
observation or after restart it scans the complete file. On later observations it runs detectors
only on full lines intersecting changed chunks, preserving line context and line numbers. A
blocked observation does not replace the accepted chunk list, but its filesystem snapshot is
saved so repeated watcher hints cannot publish the same block again.

`capture_status::blocked_drips` and `last_block` expose the safe coordinates. The existing
subscriber callback receives a value whose `block` is set and which is not a journaled drip.
Accepted values have no block and carry `scan_attestation`. Setting `scan` to false is an explicit
repository policy choice and produces no scan attestation.

## `scan_result/1`

The first concrete `attestation` record profile is canonical CBOR:

```text
{
  v: 1, kind: "attestation", subject: drip digest, profile: "scan_result/1",
  detectors: [detector id...], findings: uint, at: seconds,
  issuer: actor, allowances: [{path, detector, line, reason}...], sig: TAAP signature
}
```

The signature is over the same record without `sig`, using the TAAP artifact type
`attestation`. The record is written to the capture store before the drip journal append, and the
drip record stores its digest. `verify(attestation)` reconstructs the signed body and verifies it
against the embedded issuer public key, exact registered profile, drip subject kind, and 24-hour
maximum age; the focused test exercises that public verifier. Its original bytes remain stable
under the registry. See [`attestations.md`](attestations.md) for the shared verification model.

## Redaction

`repository::redact(path, replacement, change?)` is the direct caller-supplied form. Without a
change id it replaces the current value; with one it removes the value at that historical change.
`repository::redact(path, change?)` uses the current working value as the caller-supplied
replacement. Without a change id it targets the most recent prior write, which supports the
sequence `write(path, replacement); redact(path)`. With a change id it targets that snapshot.

Redaction performs these steps:

1. write and persist the replacement;
2. append a signed `redaction` operation naming the path and replaced blob digest;
3. remove the old blob record and unshared old content objects from the store object map;
4. commit tombstones for their physical frames, rebuild the index without those frames, and run a
   compaction-only pass over every affected pack;
5. read the old blob digest and require `errc::not_found` before returning.

The journal remains append-only. Its redaction marker names the earlier drip, and journal readers
resolve markers over the full log so an earlier record is returned with `redacted == true`.
Content shared with the replacement stays live, while the old blob record itself is always
removed. This preserves the replacement without leaving the historical path readable. If the
same old blob record remains at another working path, redaction returns `errc::busy`; that path
must be replaced or redacted too, because deleting a content-addressed value underneath a live
path would corrupt the working tree.

Redaction cannot recall copies already pulled to another machine, backups made outside liquid, or
plaintext disclosed before the operation. A replica removes the content when it receives the
redaction. Removing lake's already replicated copy is Phase 6 work.

## Verification and measured cost

`tests/scan/` covers every detector, both exception mechanisms, safe blocking details, attestation
storage and signature verification, and verified redaction of the old blob and content digest.
JWT tests require four long invalid component shapes to finish within a combined two-second
budget, preserve valid candidates after malformed prefixes and valid components above 64 KiB,
and find JWT and private-key shapes after a two-MiB prefix.
`tests/crdt/test_journal.cpp` proves the physical journal grows while its logical reader marks the
earlier drip redacted.

The Release `liquid-capture-measure` target wrote 100 ordinary files in one 20 ms capture window
on the macOS development host. One recorded run produced:

```text
window=20ms append-p50=220.917us append-p99=220.917us scan-p50=130.386us scan-p99=130.386us visible-p50=43.9578ms visible-p99=43.9578ms
```

With one sample, p50 and p99 are the same observation. Scanner work was 130.386 µs for the
100-file drip, or about 1.30 µs per file. Visibility includes the configured coalescing window,
filesystem delivery, storage, signing, attestation storage, and notification.

## Open questions

- Persisting accepted chunk lists would avoid a complete rescan after process restart; the current
  restart behavior is conservative and correct.
- Scanning encrypted structured values belongs with the structured-encryption implementation;
  this path scans the changed plaintext bytes presented to capture.
