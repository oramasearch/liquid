<!-- Raw research source. Generated via Perplexity `perplexity_reason` (web-grounded)
     on 2026-09-01 for the liquid project. Lightly reformatted only. Note: this topic
     was gathered with the `reason` tier rather than `research` after the deeper tier
     timed out twice; it is still web-grounded and cited, but is somewhat less
     exhaustive than the other sources files in this tree.
     Treat as evidence, not as liquid design decisions - see README.md in this
     directory for the curated analysis and the implications drawn for liquid. -->

# GNU Autotools: Automake, Autoconf, Libtool

GNU Autotools is a family of generators rather than one build system:

- **Autoconf** converts `configure.ac` into a portable shell `configure` script.
- **Automake** converts `Makefile.am` into `Makefile.in`; `configure` later turns that into a configured `Makefile`.
- **Libtool** provides a portability layer for building and linking shared and static libraries.

The traditional consumer workflow is:

```sh
./configure [options]
make
make check
make install
```

A maintainer normally produces the user-facing source archive with `make distcheck`. That distinction - maintainer-time generation versus user-time configuration - is central to both Autotools' strengths and its weaknesses.

---

## 1. Automake

### 1.1 `Makefile.am` and primaries

`Makefile.am` is a declarative input file. Automake recognizes variable suffixes called **primaries**:

```make
bin_PROGRAMS = viewer
viewer_SOURCES = main.c window.c
viewer_LDADD = libui.la

lib_LTLIBRARIES = libui.la
libui_la_SOURCES = ui.c ui.h

include_HEADERS = ui.h
pkgdata_DATA = default-theme.css
```

| Primary | Meaning | Typical examples |
|---|---|---|
| `_PROGRAMS` | Executables to build and, unless prefixed with `noinst_`, install | `bin_PROGRAMS`, `noinst_PROGRAMS` |
| `_LTLIBRARIES` | Libtool libraries, normally producing `.la` build artifacts and shared-library outputs | `lib_LTLIBRARIES`, `noinst_LTLIBRARIES` |
| `_SOURCES` | Source files belonging to a target; also controls distribution unless marked `nodist_` | `app_SOURCES` |
| `_HEADERS` | Headers to install or distribute | `include_HEADERS`, `noinst_HEADERS` |
| `_DATA` | Architecture-independent data files to install or distribute | `pkgdata_DATA`, `dist_data_DATA` |

Automake also has `_LIBRARIES`, `_SCRIPTS`, `_MANS`, `_TEXINFOS`, `_JAVA`, and `_PYTHON`. A target's `_SOURCES` list tells Automake which files participate in compilation and which belong in the distribution archive. The official manual identifies `PROGRAMS`, `LTLIBRARIES`, `DATA`, and `HEADERS` as primary classes and explains that `LTLIBRARIES` invokes Libtool for library targets. [web:16]

Prefixes modify behavior: `bin_`/`lib_`/`include_`/`data_` select installation directories; `noinst_` builds without installing; `check_` builds a test-only target; `dist_` forces inclusion in a distribution; `nodist_` excludes a generated file.

### 1.2 Dependency tracking, `depcomp`, and GCC `-MD`

Automake's dependency tracking discovers header dependencies as a side effect of compilation. Because compiler interfaces differed substantially, Automake routes compilation through the auxiliary script **`depcomp`**, which knows how to request dependency output from different compilers and transform it into make-compatible rules. Dependency tracking is enabled by default and can be disabled with `--disable-dependency-tracking` or the `no-dependencies` option. [web:188]

With GCC the mechanism is conceptually:

```sh
gcc -MD -MP -MF .deps/foo.Tpo -c foo.c -o foo.o
```

`-MD` writes a make rule describing the source's included files while still compiling; `-MF` selects the dependency-file name, `-MT`/`-MQ` select or quote the target name. [web:190] `depcomp` matters because not every compiler has GCC's exact `-MD` interface. If it cannot determine a suitable invocation, Automake may disable automatic dependency tracking. [web:188]

### 1.3 `SUBDIRS`: recursive make

The conventional layout has one `Makefile.am` per directory:

```make
SUBDIRS = lib src tests
```

`SUBDIRS` determines directories built for normal targets such as `all` and `check`; `DIST_SUBDIRS` records directories that must also be considered for distribution and cleaning even when a conditional build omits them. By default Automake processes subdirectories depth-first. [web:108]

Recursive make gives each component local ownership, but **each recursive invocation sees only a fragment of the dependency graph**. The top-level make cannot necessarily know that a header produced in `lib/` is needed by an object compiled in `src/`.

### 1.4 "Recursive Make Considered Harmful"

Peter Miller's argument is that the problem is **partitioning one dependency DAG into several incomplete DAGs**. Typical symptoms: incorrect or fragile build ordering; unnecessary serialization between subdirectories; missed dependencies and stale objects; poor parallel-build behavior; duplicated convenience-library archive and link steps; difficulty expressing cross-directory dependencies. The paper's central conclusion is that recursive make is harmful because the recursively invoked makefiles are incomplete views of the actual graph. [web:106]

Automake can support a **non-recursive** arrangement with `subdir-objects`:

```make
AM_INIT_AUTOMAKE([subdir-objects])
bin_PROGRAMS = app
app_SOURCES = src/main.c lib/parser.c lib/parser.h
```

This gives more accurate dependencies, better parallelism, fewer convenience libraries, and one configuration point. [web:113] The trade-off is that a large monolithic `Makefile.am` can be hard to organize. Modern build systems such as Meson and CMake represent the project globally and delegate execution to a backend such as Ninja, avoiding the classic recursive-make pattern.

### 1.5 `make dist` and `make distcheck`

`make dist` creates a release archive containing source files listed through primaries, files marked `dist_`, generated `configure`/`Makefile.in`/helper scripts, and selected documentation.

`make distcheck` is substantially more valuable. It builds a fresh archive and then: unpacks it; performs a VPATH/out-of-tree build; runs `make`; runs `make check`; installs into a temporary location; runs `make installcheck`; tests uninstalling and cleaning; and checks the result is self-contained. The documentation specifically notes it uses a read-only source tree for its VPATH build. [web:18]

A successful `make dist` means only "an archive was assembled"; a successful `make distcheck` is evidence the archive is usable as a release artifact.

### 1.6 VPATH and out-of-tree builds

```sh
tar xf project-1.0.tar.xz
mkdir project-build && cd project-build
../project-1.0/configure --prefix=/usr
make
```

Automake uses make's VPATH machinery to find source prerequisites in the source tree while placing outputs in the build tree. [web:183] Benefits: multiple configurations from one checkout; a read-only source tree; clean separation of generated files; easier packaging; reduced risk of distributing build products. VPATH is not the same as parallel compilation. [web:183]

---

## 2. Autoconf

### 2.1 `configure.ac` and the generated interface

```m4
AC_INIT([sample], [1.0], [bugs@example.org])
AM_INIT_AUTOMAKE([foreign subdir-objects])
AC_PROG_CC
AC_CONFIG_HEADERS([config.h])
AC_CONFIG_FILES([Makefile src/Makefile])
AC_OUTPUT
```

Autoconf processes it with GNU M4 and emits `configure`, a shell script that runs on the user's system. [web:36] The separation is:

```text
configure.ac + M4 macros -> configure
Makefile.am              -> Makefile.in
configure + Makefile.in  -> Makefile
```

Users of a properly prepared release tarball normally need a shell, compiler, make, and ordinary Unix utilities - not Autoconf or Automake themselves.

### 2.2 M4 macro layering

1. **M4 expands macros** such as `AC_INIT`, `AC_PROG_CC`, `AC_CHECK_HEADERS`.
2. Those macros emit shell code.
3. The generated shell code compiles, links, or runs small test programs.
4. Results become substitutions, conditionals, cache entries, or generated headers.

The macro search order includes Autoconf's installed macros and optional site/package macro files such as `acsite.m4` and `aclocal.m4`; later definitions override earlier ones. [web:32] `aclocal.m4` is normally assembled by `aclocal`, collecting third-party and project-local macros from `/usr/share/aclocal` and a project's `m4/` directory. A modern project uses `AC_CONFIG_MACRO_DIRS([m4])`. Autoconf documents `configure.ac`, `aclocal.m4`, `autoconf`, and `autoheader` as separate stages. [web:35]

### 2.3 `AC_CHECK_*` feature tests

Autoconf's philosophy is to test capabilities rather than identify systems by name:

```m4
AC_CHECK_HEADERS([stdint.h sys/stat.h])
AC_CHECK_FUNCS([strlcpy clock_gettime])
AC_CHECK_LIB([m], [sqrt])
AC_CHECK_TYPES([long long])
AC_CHECK_SIZEOF([void *])
AC_CHECK_DECLS([O_CLOEXEC])
```

Autoconf documents `AC_CHECK_HEADER` as compiling a test to determine whether a header is usable. [web:34] This handles vendor Unix variants better than a hard-coded OS list, but contributes to slow configuration and can produce false positives/negatives when tests are poorly written.

### 2.4 `autoheader` and `config.h`

`AC_CONFIG_HEADERS([config.h])` makes `autoheader` generate `config.h.in`; `configure` converts it into build-specific `config.h`:

```c
#define HAVE_STDINT_H 1
#define HAVE_CLOCK_GETTIME 1
#define SIZEOF_VOID_P 8
```

`config.h` is normally an internal package header, not a public API header. [web:33]

### 2.5 `PKG_CHECK_MODULES`

From `pkg.m4`, normally supplied by `pkg-config`/`pkgconf`:

```m4
PKG_CHECK_MODULES([GTK], [gtk4 >= 4.10], [], [AC_MSG_ERROR([GTK 4 is required])])
AC_SUBST([GTK_CFLAGS])
AC_SUBST([GTK_LIBS])
```

The macro must be visible to Autoconf - through the system macro directory, `aclocal.m4`, or a project `m4/` directory - so a project must ensure `pkg.m4` is available and run `aclocal -I m4` or an equivalent `autoreconf` setup. [web:45] Dependency metadata is centralized in `.pc` files rather than reconstructed with fragile `AC_CHECK_LIB`/`-L`/`-l` combinations; but missing, stale, or incorrectly installed `.pc` files become configuration failures.

### 2.6 Cross-compilation: `--build`, `--host`, `--target`

| Option | Meaning |
|---|---|
| `--build` | Machine on which configuration and compilation run |
| `--host` | Machine on which the resulting program will run |
| `--target` | Machine for which compiler tools themselves produce code; mainly relevant when building a compiler or toolchain |

`--host` enables cross-compilation mode; `--target` is usually not needed for an ordinary application or library. [web:156]

The difficulty is that many configuration questions are inherently runtime questions. A compiler can produce target binaries, but the build machine may not be able to execute them, so `AC_RUN_IFELSE` cannot obtain a result in the usual way; without a cross-compilation fallback, Autoconf may warn or abort. [web:165]

Cross-builds commonly break because: a test program is accidentally run on the build machine; a test result is incorrectly cached from the build system; build tools and target tools are confused; host libraries are found instead of target sysroot libraries; a package assumes `sizeof`, endianness, or ABI properties can be learned by execution; third-party macros were written only for native builds; generated helper programs are compiled for the wrong machine.

### 2.7 Why `./configure` is notoriously slow

A generated `configure` can be a very large sequential shell program. Slowness comes from: shell startup and process creation for thousands of commands; repeated `sed`, `grep`, `awk`, `expr`, and filesystem operations; compiling and linking many tiny test programs; sometimes running them; serial execution; repeated dependency and path searches; M4-generated portability layers and extensive logging. One analysis summarizes it as huge shell scripts, creation/compilation/execution of small programs, and sequential I/O-bound execution. [web:41]

Caching with `--cache-file=config.cache` helps repeated configurations, but a cache produced for one compiler, sysroot, host, or option set can silently contaminate another configuration.

---

## 3. Libtool

### 3.1 What `.la` files are

`libfoo.la` is not the ELF shared object; it is Libtool metadata describing a library and its relationships - installed library name, archive and shared-library files, dependent Libtool libraries, linker flags, installation directory, and whether it is a convenience library. The Libtool manual describes `.la` as storing, among other things, the destination directory given by `-rpath` and library dependencies. [web:10]

### 3.2 Why distributions delete `.la` files

On modern ELF systems, ordinary dynamic linking and `pkg-config` generally provide what `.la` files historically supplied. Fedora recommends `.la` files not be included, and its packaging review rules say they must be removed when built. [web:2][web:6]

Objections: `.la` files can retain dependency paths that are no longer valid; may force unnecessary static dependencies; can cause overlinking; can prevent clean mixing of libraries built by different toolchains; are usually consumed only by Libtool; add packaging noise. The removal is not risk-free - Fedora notes that removing an existing `.la` file can itself be an API/ABI packaging change because other Libtool-based packages may have referenced it. [web:2]

### 3.3 Wrapper scripts

During an in-tree or uninstalled build, Libtool may create a wrapper script around an executable so the program finds freshly built libraries before installation, commonly via `LD_LIBRARY_PATH` or equivalents. This supports running `./src/my-program` before `make install`. It is useful for tests, but contributes to the perception that Libtool hides too much of the actual linker and loader behavior.

### 3.4 `current:revision:age` versioning

```text
-version-info current:revision:age
```

- **current**: the newest interface number implemented;
- **revision**: implementation revision of that interface;
- **age**: how many previous interface numbers remain supported.

The library supports interface numbers from `current - age` through `current`. Documented update rules: source changes without interface changes increment `revision`; added/removed/changed interfaces increment `current` and reset `revision`; newly added backward-compatible interfaces increment `age`; removed or changed interfaces reset `age` to zero. `3:12:1` means current interface 3, revision 12, and compatibility with interface 2. [web:3] This is not the project's human-facing release version; package release information belongs in `-release`.

### 3.5 RPATH handling

Building an installable shared library commonly requires `-rpath /usr/lib`, telling Libtool the eventual installation directory so it can select the platform's naming convention, create symlinks, and arrange runtime search behavior. Without `-rpath`, a `.la` output is generally treated as a convenience library rather than an installable shared library. [web:1]

RPATH is resented because an embedded build-time path can be undesirable; distribution and multiarch layouts vary; it can defeat administrator-controlled library lookup; private paths can leak into binaries; and it interacts poorly with relocatable installations and security policy.

### 3.6 Why Libtool is resented

Libtool was designed to smooth over genuinely difficult 1990s Unix linker/loader differences, and those strengths remain real. Modern criticisms: it creates several layers of intermediate artifacts (`.lo`, `.la`, wrapper scripts); obscures actual compiler and linker commands; has complicated quoting and platform logic; can be slow; interacts badly with distribution dependency policies; encourages `.la` metadata that distributions later delete; and historically handled cross-compilation imperfectly. A contemporary assessment summarizes the opposing view: Autotools offer exceptional legacy-Unix portability and a large macro ecosystem, but have a steep learning curve, difficult debugging, and Libtool behavior that can be broken or harmful for modern packaging. [web:136]

---

## 4. The xz-utils CVE-2024-3094 backdoor

### 4.1 What happened

The affected xz-utils releases were **5.6.0 and 5.6.1**. The attack did not add an obvious backdoor to a normal C source file. Instead, build-time machinery assembled and injected code into `liblzma` under specific conditions.

The malicious release tarballs contained a modified `m4/build-to-host.m4` **that was absent from the public Git source tree**. The Git tree contained seemingly innocuous compressed test fixtures, while the release archive added the wiring that decoded those fixtures and inserted the resulting object into the build. [web:81]

```text
build-to-host.m4
    -> extracts/deobfuscates staged data
    -> decodes shell scripts and an object file
    -> modifies the liblzma build
    -> links malicious code into liblzma
    -> affects sshd through the systemd/liblzma linkage path
```

CERT-EU describes the staged extraction of `bad-3-corrupt_lzma2.xz` and `good-large_compressed.lzma`, production of `liblzma_la-crc64-fast.o`, and the resulting manipulation of OpenSSH's `RSA_public_decrypt` resolution path. [web:82]

### 4.2 Why the tarball/Git difference mattered

Downstream distributions normally build **release tarballs**, not arbitrary Git checkouts. The attacker exploited that trust boundary: reviewers inspected commits and the Git tree; packagers downloaded a release archive; the archive contained additional or modified build machinery; the payload activated during configuration/build rather than when a source file was visibly compiled. "The Git repository is clean" did not imply "the distributed source archive is clean."

The malicious `build-to-host.m4` was especially effective because M4 files look like ordinary configuration infrastructure. They are interpreted indirectly, often expanded into large generated shell scripts and makefiles, and are not usually reviewed with the same scrutiny as C or Rust source. The build system itself became a code-generation and injection stage.

### 4.3 Why generated Autotools machinery is an attractive hiding place

1. **The source of execution is indirect.** A small M4 macro can emit complicated shell code.
2. **Generated files are large and noisy.** A suspicious command disappears among thousands of generated lines.
3. **Maintainers and packagers have different artifacts.** Maintainers review Git; packagers consume tarballs.
4. **Build-time code has high privilege.** It runs with the developer or package-builder's permissions.
5. **The mechanism is conditional.** The xz payload targeted particular architectures, libc/toolchain, and packaging environments, reducing exposure during casual testing.
6. **"Test data" is trusted.** Compressed fixtures are normally assumed inert rather than encoded executable payloads.

The important conclusion is not that M4 or Autotools inherently caused CVE-2024-3094. Equivalent attacks could target CMake scripts, Meson files, Python generators, release scripts, vendored dependencies, or CI. The structural weakness was **unverified release-specific build input combined with a build system capable of executing arbitrary commands**.

### 4.4 Community conclusions

- verify the exact release artifact, not only the Git repository;
- make release archives reproducible from reviewed source;
- compare generated artifacts with independently generated results;
- sign and verify tarballs and provenance;
- minimize opaque generated build code;
- inspect build scripts and "test data," not only production source;
- use isolated, reproducible, preferably hermetic package builds;
- avoid assuming a build system's generated files are harmless.

At least some projects decided to stop shipping generated Autotools files because they were difficult to verify against the repository and depended on the exact Autotools version used by the release infrastructure. [web:143]

---

## 5. Maintainer and distro-packager sentiment

### 5.1 Why maintainers often dislike Autotools

M4 quoting and macro indirection; generated-file churn; cryptic `configure` failures; recursive make and fragile build ordering; slow configuration; difficult cross-compilation; Libtool's extra abstraction and `.la` files; the need to maintain Autoconf, Automake, Libtool, `m4/`, helper scripts, and release rules; and the cost of supporting multiple parallel build systems during migration. A small conceptual change can require regenerating enormous `configure` and `Makefile.in` files, making code review less direct.

### 5.2 Why packagers still value the conventions

The counter-argument is operational rather than aesthetic. A release tarball with generated files gives a packager a stable, familiar interface:

```sh
./configure --prefix=/usr --libdir=/usr/lib --disable-static
make
make check
make DESTDIR="$pkgdir" install
```

That interface offers: no requirement for the end user to install Autoconf or Automake; predictable `--prefix`, `--libdir`, `--enable-*`, `--with-*` options; `DESTDIR` staging; separate source and build directories; support for many Unix variants; standard targets (`all`, `check`, `install`, `uninstall`, `clean`, `distclean`); and source archives buildable without a project-specific bootstrap environment.

One assessment lists legacy-platform coverage, feature-based portability, cross-compilation support, standardized tarball interfaces, and minimal release dependencies among Autotools' principal strengths. [web:136]

The practical consensus:

- **For a stable release interface and broad Unix portability:** Autotools remains dependable.
- **For active development, fast CI, modern IDE integration, Windows, and maintainability:** Meson or CMake is often preferable.
- **For packagers:** removing or changing the conventional interface has real migration costs.

---

## 6. Migration to Meson and CMake

### 6.1 What projects gained

Faster configuration; clearer, less indirect build descriptions; first-class out-of-tree builds; better global dependency graphs; better Ninja integration; native project generators or IDE integration; more coherent cross-compilation models; easier conditional targets and dependency objects; less dependence on shell, M4, and Unix command-line utility behavior. Meson is deliberately non-Turing-complete and emphasizes readable declarations; its dependency mechanism can use `pkg-config`, CMake package metadata, frameworks, or fallback subprojects. [web:104]

### 6.2 What projects lost or had to rebuild

Decades of Autoconf portability macros; support for obscure Unix systems; the standard `configure` option vocabulary; downstream packager familiarity; mature `make distcheck` release behavior; existing third-party `configure` integrations; fine-grained control over arbitrary shell and make rules; and the ability to build a release tarball without installing a newer build tool. They also acquire new bootstrap dependencies (Meson: Python + Meson + Ninja; CMake: CMake + backend).

Maintaining both systems for a transition period can be worse than either alone. GTK's maintainers explicitly cited the cost of maintaining two build systems when dropping the Autotools build in favor of Meson. [web:63]

### 6.3 Project examples

| Project family | Migration | Main gains | Main costs/trade-offs |
|---|---|---|---|
| **GNOME / GLib / GTK** | Autotools to Meson | Faster configuration and builds, cleaner syntax, better Windows/macOS/Android integration, easier CI and maintenance. GLib's developers described Meson as easier and better integrated on non-Linux platforms. [web:70] | Meson became a build-time dependency; projects had to reproduce Autotools options and distribution behavior. Maintaining both during transition was burdensome. |
| **systemd** | Autotools to Meson | A single modern build description, faster configuration, clearer options, easier integration with contemporary Linux development. Buildroot's migration notes describe systemd as converted to Meson. [web:92] | Downstream build recipes had to translate `--enable/disable` and `--with/without` into Meson `-D` options and remove Autoconf cache-variable handling. |
| **GStreamer** | Autotools to Meson | Faster builds on all platforms; particularly large improvements on Windows because Meson does not depend on recursive make and shell execution in the same way. [web:71] | Had to port plugin/documentation and development-environment behavior; Autotools was retained temporarily before removal. [web:74] |
| **X.org / X server** | Autotools to Meson | Faster generation and builds, shorter and less error-prone build files, better cross-platform support, substantial reduction in build-system code. [web:121][web:123] | Early Meson support lacked feature parity; developers initially maintained both. Migration had to preserve SDK, driver, test, and non-Linux behavior. |
| **CMake adopters generally** | Autotools to CMake | Broad OS/compiler support, Visual Studio/Xcode/IDE integration, `find_package()` configuration files, large ecosystem. A PROJ migration proposal cited lower maintenance, simpler CI, faster builds. [web:177] | CMake has its own language quirks, policies, version compatibility issues, and packaging semantics; does not automatically preserve Autotools' ABI, options, or static/shared behavior. |

The named GNOME, systemd, GStreamer, and X.org migrations were principally **Meson** migrations, not CMake migrations. CMake is more established where Visual Studio, Xcode, `find_package()`, or vendor IDE integration is central; Meson is often chosen when readable build descriptions, fast Ninja builds, and a smaller conceptual surface matter more.

### Bottom line

Autotools' design optimizes for **portable release artifacts and decades of Unix compatibility**. Meson and CMake optimize for **maintainable project descriptions, modern development workflows, fast incremental builds, and cross-platform tooling**.

The traditional `./configure && make && make install` workflow is not merely nostalgia. It is a packaging contract that distributions know how to automate. Projects migrate because maintaining the machinery behind that contract - M4, recursive make, Libtool, generated files, cross-platform shell behavior - has become more expensive than preserving the contract is worth for their supported platforms.

## Citations

[1] http://www.gnu.org/s/libtool/manual/libtool.pdf
[2] https://docs.fedoraproject.org/en-US/packaging-guidelines/
[3] https://www.gnu.org/software/libtool/manual/libtool.html
[6] https://docs.fedoraproject.org/en-US/packaging-guidelines/ReviewGuidelines/
[10] https://www.gnu.org/software/libtool/manual/html_node/Linking-libraries.html
[16] https://www.gnu.org/software/automake/manual/1.13.1/automake.pdf
[18] https://www.gnu.org/software/automake/manual/1.16.2/html_node/Checking-the-Distribution.html
[32] https://www.gnu.org/software/autoconf/manual/autoconf-2.67/autoconf.html
[33] http://www.gnu.org/software/autoconf/manual/autoconf-2.66/autoconf.html
[34] http://www.gnu.org/software/autoconf/manual/autoconf-2.61/autoconf.pdf
[35] https://www.gnu.org/software/autoconf/manual/autoconf-2.68/html_node/Making-configure-Scripts.html
[36] https://www.gnu.org/software/autoconf/manual/autoconf-2.71/html_node/Making-configure-Scripts.html
[41] https://jmmv.dev/2022/06/autoconf-caching.html
[45] https://www.gnupg.org/documentation/manuals/gpgme/Using-Automake.html
[63] https://discourse.gnome.org/t/removing-the-autotools-build-for-gtk-3/12764
[70] https://blogs.gnome.org/gtk/2018/07/11/news-from-glib-2-58/
[71] http://gstreamer-devel.230.s1.nabble.com/Cerbero-GStreamer-recipes-are-now-built-with-Meson-td4688295.html
[74] https://lists.freedesktop.org/archives/gstreamer-devel/2019-October/073113.html
[81] https://safeguard.sh/resources/blog/xz-utils-backdoor-technical-breakdown
[82] https://cert.europa.eu/publications/security-advisories/2024-032/
[92] https://lists.buildroot.org/pipermail/buildroot/2017-December/575159.html
[104] https://mesonbuild.com/Reference-manual_functions_dependency.html
[106] https://accu.org/journals/overload/14/71/miller_2004/
[108] https://www.gnu.org/software/automake/manual/1.16.4/html_node/Subdirectories.html
[113] https://autotools.info/automake/nonrecursive.html
[121] https://www.phoronix.com/news/Xorg-Meson-Almost-Gold
[123] https://lists.x.org/archives/xorg-devel/2017-April/053512.html
[136] https://www.owlfolio.org/development/autoconf-swot/
[143] https://bugs.openmpt.org/view.php?id=1768
[156] https://www.gnu.org/software/autoconf/manual/autoconf-2.70/html_node/Specifying-Target-Triplets.html
[165] https://www.gnu.org/software/autoconf/manual/autoconf-2.72/html_node/Runtime.html
[177] https://proj.org/en/stable/community/rfc/rfc-7.html
[183] https://www.gnu.org/software/automake/manual/html_node/VPATH-Builds.html
[188] https://www.gnu.org/software/automake/manual/html_node/Dependencies.html
[190] https://gcc.gnu.org/onlinedocs/gcc/Preprocessor-Options.html
