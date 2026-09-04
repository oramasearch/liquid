# Build input policy

liquid evaluates repository build-input policy before an action executes. A rule selects source
paths, the toolchain tree, or both, then requires a minimum number of verified TAAP attestations
from named identities. A failed `refuse` rule returns `errc::unauthorized`; a failed `warn` rule is
reported but does not stop the action.

The policy is part of the canonical repository document under `config.policy`. `LIQUID_REPO`
points to that document, so each repository has one versioned policy rather than a process-local or
machine-local setting. The policy is a typed `policy_config` in the public library API:

```text
policy_config {
  approved_builders: [TAAP identity, ...],
  rules: [
    {
      scope: paths | toolchain | all inputs,
      paths: [repository-relative glob, ...],
      require: [
        { profile: registered-profile, from: [TAAP identity | "approved-builders", ...],
          min_count: positive integer }
      ],
      on_missing: refuse | warn
    }
  ]
}
```

Only a `paths` rule has a non-empty `paths` list. `*` does not cross `/`, `**` does, and `?`
matches one non-`/` character. `toolchain` selects every blob record in the toolchain tree. `all
inputs` selects every blob record in both the source and toolchain trees.

Each `require` entry references an immutable profile from the
[attestation registry](attestations.md). `from` contains exact TAAP identity strings or the literal
`approved-builders`, which expands to `config.policy.approved_builders`. `min_count` counts distinct
verified issuer identities, not records, so one builder cannot satisfy a two-builder quorum by
signing twice. For `reproducibility/1`, only an attestation whose `agreement` attribute is true
counts.

Configuration validation rejects unknown profiles, empty requirements, empty authority sets,
duplicate approved builders, a `min_count` larger than the number of distinct permitted identities,
and path lists on non-path scopes. An old repository document without `config.policy` decodes as an
empty policy.

## Examples

Every dependency under `vendor/` must have SLSA provenance from an approved builder:

```cpp
liquid::policy_config policy{
    .approved_builders = {builder_a.taap_id, builder_b.taap_id},
    .rules = {{.scope = liquid::policy_scope::paths,
               .paths = {"vendor/**"},
               .require = {{.profile = "slsa_provenance/1",
                            .from = {"approved-builders"},
                            .min_count = 1}},
               .on_missing = liquid::policy_on_missing::refuse}},
};
```

Every source and toolchain blob must have successful reproducibility attestations from two
independent identities:

```cpp
liquid::policy_config policy{
    .rules = {{.scope = liquid::policy_scope::all_inputs,
               .require = {{.profile = "reproducibility/1",
                            .from = {builder_a.taap_id, builder_b.taap_id},
                            .min_count = 2}},
               .on_missing = liquid::policy_on_missing::refuse}},
};
```

Changing `on_missing` to `warn` preserves the full failed-input report while allowing execution.

## Evaluation and failure behavior

Before lookup or execution, the evaluator walks the selected Merkle trees and uses the store's
subject index to find attestation records whose subject is each blob-record digest. It loads each
record from the local store, verifies the TAAP signature, asks the profile registry to validate the
schema and freshness/status requirements, filters by issuer, and counts distinct accepted
identities. It never trusts a filename, unsigned side table, or caller-provided claim.

If a selected tree cannot be loaded, an attestation cannot be decoded, a signature or profile is
invalid, a revocable profile lacks required status evidence, or the authority set cannot meet the
quorum, the requirement is unsatisfied. A `refuse` rule fails closed. Its error names the input,
`config.policy.rules[N]`, the profile, required count, actual count, and the last verification
rejection when one exists. `liquid wrap --explain` receives the same error, and warning rules print
one `warning:` line per failed input.

The subject index is derived from canonical attestation records and rebuilt from the store's object
map when the store opens. Adding an attestation updates it in the same in-memory mutation that
publishes the object metadata. Missing or collected indexed records do not count.
`repository::store_attestation` is the public import operation for already-signed evidence; its
return value is the canonical record digest. Import does not mark an attestation trusted.

No rule passes because evidence is unavailable. This includes a wrapped build whose policy selects
the toolchain before `config.wrap.toolchain` has been pinned: a `refuse` rule stops that first build,
while a `warn` rule reports the unavailable tree.

## Cache isolation

The canonical BLAKE3 policy digest is appended to the candidate-key material alongside command,
cwd, environment, toolchain, sandbox, and platform. A policy change therefore derives a different
candidate even when every action field is otherwise identical. A result indexed under a looser
policy cannot be returned under a stricter policy.

## CLI

`policy check` evaluates an existing cached action without building. A target is an action digest,
result digest, or produced path:

```sh
liquid policy check blake3:<action-or-result-digest>
liquid policy check build/output.o
```

`policy explain` without a target prints the configured approved builders, rules, and policy
digest. With a target it also prints every selected input, matching rule, attestation count,
accepted identities, and satisfaction result:

```sh
liquid policy explain
liquid policy explain build/output.o
```

Both commands open the current liquid repository or the current project's persistent wrap
repository. `check` exits non-zero when a refuse rule fails. A report containing only warnings exits
successfully.

`liquid why <target>` also evaluates the current policy against the recorded action before it
reports a cache hit or ordinary miss. A failed refuse rule is reported as `policy refused`, with
the first failing input and rule in the detail. This makes a policy tightened after a build visible
for an already recorded target without executing it again.

## How to verify

Run the repository's static and shared proof commands. `BuildPolicy` tests cover refusal and
warning reports, unapproved builders, invalid signatures, unknown-profile rejection, distinct
two-identity reproducibility quorum, toolchain traversal, subject-index rebuild, pre-spawn refusal
on Linux, and candidate-key changes. The privileged Linux wrap test also proves that `why` reports
the current policy refusal for a recorded target.

The CLI commands are inspection operations; they do not execute a build or create attestations.

## How to change

Set policy when creating a repository through `repository_options::policy`, or replace the current
policy atomically with `repository::set_policy(policy_config)`. Replacement validates the complete
configuration before checkpointing the repository document. Existing attestations remain immutable
and become eligible or ineligible only through the new rule evaluation.

Import builder evidence with `repository::store_attestation(attestation)`. The policy evaluator,
not the import operation, decides whether that record is valid and authorized.

An incompatible rule meaning requires a new typed field or profile version and corresponding
canonical-encoding tests. Never reinterpret an existing profile name or omit the policy digest from
candidate derivation.

## Deferred

Fetching attestations from lake or a transparency log is Phase 6. A full policy/profile registry
service and SBOM-specific conditions are separate work. This implementation evaluates only records
already present in the local store.
