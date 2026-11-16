# Security

Threat models for liquid's trust boundaries, each written from the code at a stated commit. A
threat model lists the assets, actors and boundaries, the must-nevers with where each is enforced
and the test that proves it, and the defects found, each linked to its issue.

The security model itself is TAAP ([`../standards/taap-1-spec-draft.md`](../standards/taap-1-spec-draft.md),
[`../features/25-identity-attestation-and-transparency.md`](../features/25-identity-attestation-and-transparency.md));
the limits users must know are in [`../library/status.md`](../library/status.md#known-limits-in-one-place).

| Page | What it contains |
|---|---|
| [`delegation-threat-model.md`](delegation-threat-model.md) | `delegation/1`, attenuation, depth, scope, expiry, revocation and replay, and private multiplayer sessions: join, leave, publish and lease authority (liquid #418; defects #514 to #517). |
