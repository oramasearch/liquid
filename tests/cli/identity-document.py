"""Check verbatim sign-up output through the built CLI and public wire codec."""

import base64
import json
import os
from pathlib import Path
import re
import subprocess
import sys

cli, codec, home = sys.argv[1:]
env = dict(os.environ, LIQUID_HOME=home)


def run(*arguments, environment=env):
    return subprocess.run(
        [cli, "identity", *arguments],
        env=environment,
        capture_output=True,
        check=False,
    )


# Refused combinations must not create an identity or rotate its key.
refused_home = Path(home) / "refused"
refused_env = dict(env, LIQUID_HOME=str(refused_home))
for arguments in [
    ("show", "--document", "--json"),
    ("show", "--json", "--document"),
    ("rotate", "--document"),
]:
    result = run(*arguments, environment=refused_env)
    assert result.returncode == 2, (arguments, result.stderr)
    assert result.stdout == b"", (arguments, result.stdout)
    assert b"--document" in result.stderr, result.stderr
    assert not refused_home.exists(), "usage error created an identity"

result = run("show", "--document")
assert result.returncode == 0, result.stderr
assert result.stderr == b"", result.stderr
assert re.fullmatch(rb"[A-Za-z0-9_-]+", result.stdout), result.stdout
encoded = result.stdout
raw = base64.b64decode(encoded + b"=" * (-len(encoded) % 4), altchars=b"-_", validate=True)
assert base64.urlsafe_b64encode(raw).rstrip(b"=") == encoded

metadata = run("show", "--json")
assert metadata.returncode == 0, metadata.stderr
fingerprint = json.loads(metadata.stdout)["fingerprint"]
stored = Path(home) / "identity" / f"{fingerprint}.identity.cbor"
assert raw == stored.read_bytes(), "export differs from the stored canonical identity document"
verified = subprocess.run([codec, str(stored)], capture_output=True, check=False)
assert verified.returncode == 0, verified.stderr
for arguments in [("show", "--document"), ("--document",)]:
    repeated = run(*arguments)
    assert repeated.returncode == 0, repeated.stderr
    assert repeated.stdout == encoded, "export changed the signed document"
    assert repeated.stderr == b""
print("identity document: unpadded base64url, no whitespace, exact canonical CBOR, self-signature verified")
print("usage errors: --document with --json or rotate exits 2 without creating an identity")
