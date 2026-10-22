#!/usr/bin/env python3
"""Pin the service image privilege, runtime-package, and listener contracts."""

import json
from pathlib import Path
import re
import sys
import unittest


ROOT = Path(sys.argv.pop(1)) if len(sys.argv) > 1 else Path(__file__).resolve().parents[2]
SERVICES = {
    "admission/Dockerfile": "/var/lib/liquid-admission",
    "node-store/Dockerfile": "/var/lib/liquid-node-store",
    "workers/Dockerfile.scheduler": "/var/lib/liquid-scheduler",
    "workers/Dockerfile.worker": "/var/lib/liquid-worker",
}
RUNTIME_PACKAGES = {
    "ca-certificates", "libstdc++6", "libssl3t64", "zlib1g", "libfuse3-3", "libpcsclite1",
    "fuse3", "util-linux", "mount",
}


def instructions(name):
    source = (ROOT / "deploy" / name).read_text()
    lines = [line.strip() for line in re.sub(r"\\\n\s*", " ", source).splitlines()
             if line.strip() and not line.lstrip().startswith("#")]
    return [keyword.upper() + " " + value for keyword, value in (line.split(None, 1) for line in lines)]


class ServiceTemplates(unittest.TestCase):
    def test_runtime_contains_no_ci_toolchain(self):
        self.assertEqual({str(path.relative_to(ROOT / "deploy"))
                          for path in (ROOT / "deploy").glob("**/Dockerfile*")}, set(SERVICES))
        for name in SERVICES:
            with self.subTest(image=name):
                lines = instructions(name)
                self.assertEqual([line for line in lines if line.startswith("FROM ")],
                                 ["FROM ubuntu:24.04"])
                self.assertFalse(any("LIQUID_BASE" in line for line in lines))
                installs = [line for line in lines if "apt-get install" in line]
                self.assertEqual(len(installs), 1)
                packages = installs[0].split("--no-install-recommends ", 1)[1].split(" && ")[0].split()
                self.assertTrue(set(packages) <= RUNTIME_PACKAGES, packages)
                self.assertTrue({"ca-certificates", "libssl3t64", "libstdc++6", "libfuse3-3"}
                                <= set(packages))
                if name.startswith("node-store/") or name.endswith("Dockerfile.worker"):
                    self.assertIn("fuse3", packages)
                if name.endswith("Dockerfile.worker"):
                    self.assertTrue({"util-linux", "mount"} <= set(packages))

    def test_every_service_runs_as_nonroot(self):
        for name in SERVICES:
            with self.subTest(image=name):
                lines = instructions(name)
                self.assertEqual([line for line in lines if line.startswith("USER ")],
                                 ["USER 65532:65532"])
                self.assertIn("ENV LIQUID_HOME=/var/lib/liquid-identity", lines)
                setup = " ".join(line for line in lines if line.startswith("RUN "))
                self.assertIn("groupadd --gid 65532 liquid", setup)
                self.assertIn("useradd --system --uid 65532 --gid 65532", setup)
                self.assertIn("--home-dir /var/lib/liquid-identity --shell /usr/sbin/nologin", setup)
                self.assertFalse(any(line.startswith("RUN ")
                                     for line in lines[lines.index("USER 65532:65532") + 1:]))

    def test_state_is_owned_by_service_and_binaries_are_readonly(self):
        for name, store in SERVICES.items():
            with self.subTest(image=name):
                lines = instructions(name)
                setups = [line for line in lines if line.startswith("RUN ")]
                self.assertTrue(setups, "state directories must be provisioned before USER")
                setup = " ".join(setups)
                self.assertIn("install -d -o 65532 -g 65532 -m 0700 " + store
                              + " /var/lib/liquid-identity", setup)
                self.assertIn("COPY --chmod=0555 ${LIQUID_BINARY} /usr/local/bin/liquid", lines)
                volumes = json.loads(next(line[7:] for line in lines if line.startswith("VOLUME ")))
                self.assertIn(store, volumes)
                self.assertIn("/var/lib/liquid-identity", volumes)
                if name.startswith("node-store/"):
                    self.assertIn("install -d -o 65532 -g 65532 -m 0755 /run/liquid", setup)
                if name.endswith("Dockerfile.worker"):
                    self.assertIn("COPY --chmod=0555 ${LIQUID_READ_GUARD} "
                                  "/usr/local/libexec/liquid/liquid-read-guard", lines)

    def test_control_listeners_default_to_loopback(self):
        for name in SERVICES:
            with self.subTest(image=name):
                command = json.loads(next(line[4:] for line in instructions(name)
                                          if line.startswith("CMD ")))
                address = command[command.index("--control-listen") + 1]
                self.assertTrue(address.startswith("127.0.0.1:"), address)


if __name__ == "__main__":
    unittest.main()
