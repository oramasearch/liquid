# Deployment client

Liquid #84 adds the client half of deployment. Liquid links neither Zetta nor lake code. It calls
the Orama operator's control-plane REST capabilities through liquid's Corosio HTTP client, with
the optional `LIQUID_TOKEN` bearer credential.

Until the operator publishes its own definition, the exact provisional OpenAPI contract is
[`contracts/control-plane/openapi.json`](../../contracts/control-plane/openapi.json). The operator
leaf must serve `create_deployment`, `list_deployments`, `rollback_deployment`, and
`deployment_status` at its `/v1` surface with the ordinary `{ok,value}` or `{ok,error}` envelope.
The caller never needs a Kubernetes credential.

An environment is a named target. A deployment record contains a stable repository identifier,
environment, change, OCI reference with digest, optional producing action-result ID, opaque sealed
secret references, and (for rollbacks) the prior deployment ID. Tags are refused: the interim image
input is `--image <reference>@sha256:<digest>`. Issue #28 replaces only that source with its OCI
projection result.

`deploy::create` resolves an omitted change to the repository head. The same control-plane URL,
repository, environment, change, and image form the action key; a repeated request returns the
record already produced for that key. `deploy::rollback` accepts a prior record ID or change and
creates a new deployment that names `rollback_of`. Any record previously deployed to the same
environment is a legal rollback target.

Preview environments are named after their change. When repository configuration declares
`on_push: preview`, the operator creates the matching preview deployment after it observes a push;
teardown with the change remains operator work in #124. Repository `deploy` configuration owns the
control-plane URL, named environment settings, opaque secret references, and this fan-out choice.

## Open questions

- #124 must publish the operator-owned OpenAPI document and declare its credential minting and
  preview teardown behavior before liquid can remove this recorded contract.
