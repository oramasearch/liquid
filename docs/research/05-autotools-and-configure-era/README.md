# Autotools and the configure era

> Evidence: [`sources.md`](sources.md)

## Why this matters to liquid

Two reasons, and the second is the important one.

1. **Compatibility.** An enormous amount of software still ships as
   `./configure && make && make install`. liquid cannot replace the build world
   without consuming these projects.
2. **A security lesson.** The xz-utils backdoor (CVE-2024-3094) succeeded
   specifically because of a structural property of the Autotools release model.
   That property is not unique to Autotools, and liquid must be designed so the
   same attack is impossible rather than merely unlikely.

## What Autotools actually is

Three generators, often confused with each other:

```
configure.ac  --autoconf-->  configure
Makefile.am   --automake-->  Makefile.in
configure + Makefile.in  -->  Makefile
```

- **Autoconf** turns `configure.ac` (M4 macros) into a portable shell script that
  probes the target system by compiling, linking and sometimes running small test
  programs.
- **Automake** turns declarative `Makefile.am` into `Makefile.in`.
- **Libtool** abstracts shared-library building and linking across Unix variants.

A user of a properly prepared tarball needs only a shell, a compiler, make and
standard utilities — **not** Autoconf or Automake. That is the whole point, and it
is also the vulnerability.

## What it gets right

It is worth being fair, because the parts that work explain why it persists:

- **Feature tests, not OS name checks.** `AC_CHECK_HEADERS`/`AC_CHECK_FUNCS` ask
  "does this work here?" rather than "is this Solaris?" That is the correct
  approach to portability and it aged far better than the alternative.
- **`make distcheck`.** Genuinely excellent, and underappreciated. It builds a
  fresh tarball, unpacks it, does a **VPATH build from a read-only source tree**,
  runs the tests, installs to a temp prefix, runs `installcheck`, then uninstalls
  and verifies cleanliness. It is a real release gate. Most modern build systems
  have no equivalent.
- **VPATH / out-of-tree builds** as a first-class concept.
- **A stable packaging contract.** `--prefix`, `--libdir`, `--enable-*`,
  `--with-*`, `DESTDIR` staging, and the standard target set. Distribution
  packagers have automated against this for thirty years.
- **`depcomp`** — automatic header dependency tracking via `gcc -MD`, normalised
  across compilers that all spelled it differently. The same "observe what was
  actually read" idea that [06-build-and-compiler-caching](../06-build-and-compiler-caching/)
  identifies as the right primitive.

## What people hate

- **M4.** Quoting is famously treacherous; a macro layer emitting a shell layer
  makes debugging two levels removed from the failure.
- **`./configure` is slow.** Thousands of serialised probes, each spawning
  processes, compiling and linking tiny programs, plus constant `sed`/`grep`/`expr`.
  It is I/O- and process-bound and almost entirely sequential. Caching with
  `--cache-file` helps but silently contaminates across compilers and sysroots.
- **Cross-compilation breaks constantly.** `--build`/`--host`/`--target` are
  correct in principle, but many configuration questions are inherently *runtime*
  questions, and `AC_RUN_IFELSE` cannot run a target binary on the build machine.
  Without an explicit cross fallback, configure warns or aborts. Host libraries get
  found instead of sysroot libraries; build tools get confused with host artifacts.
- **Recursive make.** `SUBDIRS` partitions one dependency DAG into several
  incomplete ones — Peter Miller's "Recursive Make Considered Harmful". Symptoms:
  wrong ordering, poor parallelism, stale objects, missed cross-directory
  dependencies. Automake *can* do non-recursive builds with `subdir-objects`, but
  the recursive layout remains the default and the norm.
- **Libtool.** `.la` files that distributions actively delete (Fedora's packaging
  guidelines *require* their removal); wrapper scripts that obscure what actually
  ran; `current:revision:age` versioning that few people get right; `-rpath`
  embedding that fights multiarch and relocatable installs; slow relinking.
- **Generated-file churn.** A one-line change regenerates enormous `configure` and
  `Makefile.in` files, making review meaningless.

## The xz backdoor: the structural lesson

The affected releases were xz-utils 5.6.0 and 5.6.1. The mechanism:

- The malicious `m4/build-to-host.m4` was **present in the release tarball and
  absent from the Git repository**.
- The Git tree held only innocuous-looking *compressed test fixtures*.
- During `configure`, the injected macro extracted and deobfuscated those fixtures,
  produced `liblzma_la-crc64-fast.o`, and modified the build to link it into
  `liblzma` — which reaches `sshd` via the systemd linkage path, where it
  manipulated OpenSSH's `RSA_public_decrypt` resolution.

Why this worked so well:

1. **Reviewers read Git; packagers build tarballs.** The trust boundary between
   those two artifacts was unverified.
2. **Execution is indirect.** A small M4 macro expands into a large generated shell
   script; the malicious command hides among thousands of generated lines.
3. **Build-time code runs with the builder's privileges.**
4. **It was conditional** — targeting specific architectures, libc and packaging
   environments — so it did not fire during casual testing.
5. **"Test data" is assumed inert.** Compressed fixtures are not reviewed as code.

The essential point: **this is not an M4 problem.** The identical attack works
against a CMake script, a `meson.build`, a `setup.py`, a `build.rs`, a vendored
dependency, or a CI definition. The structural weakness is *unverified,
release-specific build input combined with a build system that can execute
arbitrary commands.*

The community's conclusions: build from Git rather than tarballs; make release
archives reproducible from reviewed source; sign artifacts and record provenance;
minimise opaque generated build machinery; review build scripts and test data as
code; and build in isolated, hermetic environments.

## The migration landscape

GNOME/GLib/GTK, systemd, GStreamer and X.org all moved to **Meson** (not CMake).
Gains: much faster configuration, readable declarative syntax, better Windows and
macOS support, a global dependency graph instead of recursive make. Costs: a new
bootstrap dependency (Python + Meson + Ninja), loss of decades of accumulated
portability macros, loss of the familiar `configure` option vocabulary, and no
`distcheck` equivalent. GTK's maintainers explicitly cited the cost of maintaining
*two* build systems during transition as the reason to finally drop Autotools.

The packager's counter-argument is real and should not be dismissed: the
`./configure && make && make install` interface is a **contract that distributions
have automated against**. Breaking it imposes migration cost on everyone
downstream.

## Implications for liquid

1. **Ingest Autotools projects; do not require porting.** liquid must be able to
   run `./configure && make` inside a sandboxed, dependency-observing action and
   capture the result — gaining caching, distribution and provenance for a project
   that has changed nothing. This is the adoption path. Same for CMake and Meson.

2. **Replace configure-time probing with a resolved, cached fact base.** Most
   `AC_CHECK_*` probes ask questions whose answers depend only on
   (toolchain, sysroot, target). That tuple is content-addressable, so the answers
   can be computed once, cached globally, shared, and *reused across every project
   on that platform*. This turns minutes of serialised probing into a cache hit,
   and it fixes cross-compilation at the same time — because the fact base for a
   target is looked up, not executed.

3. **Never ship generated build machinery that is not derivable from reviewed
   source.** The xz lesson, stated as a design rule. In liquid there should be no
   "release tarball" that differs from the repository. If a build needs generated
   input, the generator runs as a recorded, reproducible action whose output is
   verifiable — so a tarball/repo divergence is not merely discouraged, it is
   *unrepresentable*.

4. **Treat build definitions and test fixtures as executable code for review and
   provenance purposes.** Opaque binary fixtures that participate in a build are a
   red flag liquid can detect mechanically: an action whose inputs include
   undecodable binary data that reaches a compiler or linker should be
   surfaceable.

5. **Steal `make distcheck`.** A single command that verifies a project builds from
   a clean distribution, out of tree, from a read-only source, passes tests,
   installs, and uninstalls cleanly, is a genuinely good idea that modern systems
   dropped. liquid should have it, and it should be free — because liquid already
   sandboxes and content-addresses everything the check needs.

6. **Non-recursive by construction.** liquid has one global action graph; the
   recursive-make failure mode simply does not arise. Worth stating explicitly as a
   property, since it is one of the loudest complaints in this space.

7. **Preserve the packaging contract at the edges.** liquid should be able to
   *emit* a conventional source tarball with a working `configure`-equivalent
   interface, and to honour `--prefix`/`DESTDIR` semantics when producing OS
   packages ([10](../10-os-packaging/)). Winning packagers means meeting them where
   they are.
