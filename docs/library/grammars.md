# Tree-sitter grammar distribution

**Decision.** Tier 2 grammars are fetched at configure time from archives of exact public upstream
commits. Every archive has a CMake `URL_HASH SHA256` check. The runtime and grammar identities,
generated parser, hand-written scanner, tags query, ABI, licence, and extension registrations live
in one canonical, deterministically sorted lock file:
[`grammars.lock`](../../modules/frontends/grammars/grammars.lock).

The generated parsers are not stored in this repository. The complete set is much larger than the
code that integrates it, while a digest-checked public archive is byte-equivalent to storing the
same input here. An aggregate organisation copy would add fifty upstream synchronization duties
without improving verification. `LIQUID_GRAMMAR_MIRROR` can redirect the locked archive paths to an
organisation or air-gapped mirror later without changing parser identities.

## Trust position

A tree-sitter grammar is executable input. Generated `parser.c` enters the syntax module, and a
hand-written scanner is arbitrary C or C++ that runs inside the parser process. Nothing downloaded
without its expected SHA-256 is compiled. The archive digest authenticates every fetched byte; the
per-file parser, scanner, and tags digests make the reviewed executable inputs visible and are
recomputed by CTest. Fetching these exact bytes therefore has the same trust boundary as vendoring
them. A changed upstream tag, branch, archive, generated parser, scanner, or query is rejected.

The SHA-256 of the canonical lock is the grammar set identity. `liquid version --json` reports it,
and syntactic `symbols/1` records carry it as `grammar`. This proves which reviewed parser set
produced a derived record. Compiler-origin symbol records omit the field.
`liquid tiers` loads this module and derives one Tier 2 row per locked grammar with that same digest;
new lock entries therefore enter the generated matrix without a second inventory.

## Lock format and build controls

The version 1 JSON object contains `runtime` and a language-sorted `grammars` array. Each grammar
records `language`, `repository`, full `commit`, archive SHA-256, `parser` path and SHA-256, every
compiled `scanners` path and SHA-256, tree-sitter ABI version, SPDX licence expression, exported C
`symbol`, extensions, and an optional `tags` path and SHA-256. A null `tags` value means parsing is
available but `units` and syntactic symbols are explicitly unsupported.

The runtime is tree-sitter commit
[`a467ea8502d9`](https://github.com/tree-sitter/tree-sitter/commit/a467ea8502d95562171f97953a6dc5b2a8622609).
The inventory selects reviewed source-bearing commits. LaTeX, SQL, and Swift use the last reviewed
commit that publishes generated parser sources rather than a newer source-only archive. Perl uses
the active public `ganezdragon/tree-sitter-perl` parser because the organisation-maintained
alternative did not publish a generated `parser.c`.

`LIQUID_GRAMMARS` is a CMake list selecting a build subset; its default is `all`. The value accepts
language names separated by semicolons. `LIQUID_GRAMMAR_MIRROR` replaces the
`https://github.com` root while preserving `owner/repository/archive/commit.tar.gz`. A mirror must
serve byte-identical archives because the lock hash is still enforced.

## Adding, bumping, or removing a grammar

Use the procedure in [`tools/grammars/README.md`](../../tools/grammars/README.md). The normal add or
bump command is:

```sh
tools/grammars/add.sh <language> <repository> <commit>
```

Review the upstream commit and scanner before running it. In the pull request, identify the upstream
review, keep the lock and generated table in the same change, and check:

- [ ] The repository and full commit are the intended public upstream.
- [ ] All generated parser and hand-written scanner changes were reviewed.
- [ ] The licence and tree-sitter ABI remain supported.
- [ ] `grammar-lock-digests` and the per-language sample tests pass.
- [ ] `python3 tools/grammars/manage.py generate-docs` leaves no diff.

Removing an entry uses the same review path and states why Tier 2 support was withdrawn.
Run `tools/grammars/remove.sh <language>` so the lock and inventory remain synchronized.

The digest proof is registered as CTest `grammar-lock-digests`. It recomputes each configured
`parser.c`, scanner, and `queries/tags.scm` SHA-256 from the fetched source directory and compares
it with the lock; CMake separately verifies every runtime and grammar archive through `URL_HASH`.

## Locked inventory

<!-- BEGIN GENERATED GRAMMARS -->
| Language | Extensions | Upstream commit | ABI | Tags | Licence |
|---|---|---|---:|---|---|
| bash | `.bash`, `.bash_profile`, `.bashrc`, `.ebuild`, `.eclass`, `.sh` | [`a06c2e4415e9`](https://github.com/tree-sitter/tree-sitter-bash/commit/a06c2e4415e9bc0346c6b86d401879ffb44058f7) | 15 | unsupported | MIT |
| c | `.c`, `.h` | [`b780e47fc780`](https://github.com/tree-sitter/tree-sitter-c/commit/b780e47fc780ddc8da13afa35a3f4ed5c157823d) | 15 | pinned | MIT |
| c# | `.cs` | [`9150f7d56bb4`](https://github.com/tree-sitter/tree-sitter-c-sharp/commit/9150f7d56bb47f1a809fa23623f1ba1413e93fa9) | 15 | pinned | MIT |
| c++ | `.cc`, `.cpp`, `.cxx`, `.h`, `.hpp`, `.hxx` | [`8b5b49eb196b`](https://github.com/tree-sitter/tree-sitter-cpp/commit/8b5b49eb196bec7040441bee33b2c9a4838d6967) | 15 | pinned | MIT |
| cmake | `.cmake`, `CMakeLists.txt` | [`ca627bb58286`](https://github.com/uyha/tree-sitter-cmake/commit/ca627bb5828616b6246aafdc3c3222789e728e37) | 14 | unsupported | MIT |
| css | `.css` | [`dda5cfc5722c`](https://github.com/tree-sitter/tree-sitter-css/commit/dda5cfc5722c429eaba1c910ca32c2c0c5bb1a3f) | 15 | unsupported | MIT |
| dart | `.dart` | [`be07cf7118d3`](https://github.com/UserNobody14/tree-sitter-dart/commit/be07cf7118d3dba06236a3f19541685a68209934) | 15 | pinned | ISC |
| dockerfile | `.container`, `.Containerfile`, `.docker`, `.dockerfile`, `Dockerfile` | [`971acdd90856`](https://github.com/camdencheek/tree-sitter-dockerfile/commit/971acdd908568b4531b0ba28a445bf0bb720aba5) | 14 | unsupported | ISC |
| elixir | `.ex`, `.exs` | [`4b0c7118760a`](https://github.com/elixir-lang/tree-sitter-elixir/commit/4b0c7118760af58a2e7081bbc8396e136f820b37) | 14 | pinned | Apache-2.0 |
| elm | `.elm` | [`e1e8fea161a1`](https://github.com/elm-tooling/tree-sitter-elm/commit/e1e8fea161a1e66f3997855d316be2a43e4e956f) | 15 | pinned | MIT |
| erlang | `.app`, `.app.src`, `.erl`, `.escript`, `.hrl`, `.rebar.config` | [`6ba4c762eb30`](https://github.com/WhatsApp/tree-sitter-erlang/commit/6ba4c762eb3065495e3db85697ffeecdf364ce35) | 15 | unsupported | Apache-2.0 |
| fortran | `.F90`, `.f90`, `.F95`, `.f95` | [`7edacd2b21aa`](https://github.com/stadelmanma/tree-sitter-fortran/commit/7edacd2b21aa80057d9725384a1304a1c758e0f8) | 15 | pinned | MIT |
| go | `.go` | [`2346a3ab1bb3`](https://github.com/tree-sitter/tree-sitter-go/commit/2346a3ab1bb3857b48b29d779a1ef9799a248cd7) | 15 | pinned | MIT |
| haskell | `.hs`, `.hs-boot` | [`0975ef72fc3c`](https://github.com/tree-sitter/tree-sitter-haskell/commit/0975ef72fc3c47b530309ca93937d7d143523628) | 14 | unsupported | MIT |
| hcl | `.hcl` | [`64ad62785d44`](https://github.com/tree-sitter-grammars/tree-sitter-hcl/commit/64ad62785d442eb4d45df3a1764962dafd5bc98b) | 15 | unsupported | Apache-2.0 |
| html | `.html` | [`73a3947324f6`](https://github.com/tree-sitter/tree-sitter-html/commit/73a3947324f6efddf9e17c0ea58d454843590cc0) | 14 | unsupported | MIT |
| java | `.java` | [`e10607b45ff7`](https://github.com/tree-sitter/tree-sitter-java/commit/e10607b45ff745f5f876bfa3e94fbcc6b44bdc11) | 14 | pinned | MIT |
| javascript | `.cjs`, `.js`, `.jsx`, `.mjs` | [`58404d8cf191`](https://github.com/tree-sitter/tree-sitter-javascript/commit/58404d8cf191d69f2674a8fd507bd5776f46cb11) | 15 | pinned | MIT |
| json | `.json` | [`254c42a64764`](https://github.com/tree-sitter/tree-sitter-json/commit/254c42a6476413b776221e03982ac8ae159eeb72) | 14 | unsupported | MIT |
| julia | `.jl` | [`e0f9dcd180fd`](https://github.com/tree-sitter/tree-sitter-julia/commit/e0f9dcd180fdcfcfa8d79a3531e11d99e79321d3) | 15 | unsupported | MIT |
| kotlin | `.kt`, `.kts` | [`1852ea17b7f6`](https://github.com/fwcd/tree-sitter-kotlin/commit/1852ea17b7f60fb3f9d84e0b1555d56b46b39fb1) | 14 | pinned | MIT |
| latex | `.tex` | [`a6c812704b3d`](https://github.com/latex-lsp/tree-sitter-latex/commit/a6c812704b3d3e1541b0853aa0d6d561301320e1) | 14 | unsupported | MIT |
| lua | `.lua` | [`10fe0054734e`](https://github.com/tree-sitter-grammars/tree-sitter-lua/commit/10fe0054734eec83049514ea2e718b2a56acd0c9) | 15 | pinned | MIT |
| make | `.mk`, `Makefile` | [`a4b9187417d6`](https://github.com/alemuller/tree-sitter-make/commit/a4b9187417d6be349ee5fd4b6e77b4172c6827dd) | 13 | unsupported | MIT |
| markdown | `.md` | [`a0a00f817d02`](https://github.com/tree-sitter-grammars/tree-sitter-markdown/commit/a0a00f817d02412bd92c54d316f164d827b57b5c) | 15 | unsupported | MIT |
| nix | `.nix` | [`3d0173d903e6`](https://github.com/nix-community/tree-sitter-nix/commit/3d0173d903e630b6e14d17f1cf79488791379ded) | 13 | pinned | MIT |
| objective-c | `.h`, `.m`, `.objc` | [`181a81b8f23a`](https://github.com/tree-sitter-grammars/tree-sitter-objc/commit/181a81b8f23a2d593e7ab4259981f50122909fda) | 14 | unsupported | MIT |
| ocaml | `.ml` | [`a4ce49a6c17e`](https://github.com/tree-sitter/tree-sitter-ocaml/commit/a4ce49a6c17e88e7ca8350cfb666749d9a5c6630) | 15 | pinned | MIT |
| perl | `.pl`, `.pm`, `.t` | [`5b97493ce706`](https://github.com/ganezdragon/tree-sitter-perl/commit/5b97493ce70686e22ec8b9ea261437147e342b49) | 15 | unsupported | MIT |
| php | `.php` | [`3fda2fb95771`](https://github.com/tree-sitter/tree-sitter-php/commit/3fda2fb9577166c6399834917f9844f30370beea) | 15 | pinned | MIT |
| protocol-buffers | `.proto` | [`e9f6b43f6844`](https://github.com/treywood/tree-sitter-proto/commit/e9f6b43f6844bd2189b50a422d4e2094313f6aa3) | 14 | unsupported | MIT |
| python | `.py` | [`26855eabccb1`](https://github.com/tree-sitter/tree-sitter-python/commit/26855eabccb19c6abf499fbc5b8dc7cc9ab8bc64) | 15 | pinned | MIT |
| r | `.R`, `.r` | [`58a22794466c`](https://github.com/r-lib/tree-sitter-r/commit/58a22794466c0fc15b0d3b40531db751593721e8) | 14 | pinned | MIT |
| ruby | `.rb` | [`ad907a69da0c`](https://github.com/tree-sitter/tree-sitter-ruby/commit/ad907a69da0c8a4f7a943a7fe012712208da6dee) | 14 | pinned | MIT |
| rust | `.rs` | [`77a3747266f4`](https://github.com/tree-sitter/tree-sitter-rust/commit/77a3747266f4d621d0757825e6b11edcbf991ca5) | 15 | pinned | MIT |
| scala | `.sbt`, `.scala` | [`db390f312a54`](https://github.com/tree-sitter/tree-sitter-scala/commit/db390f312a54b04b13790e1767bfac32665c17ac) | 15 | pinned | MIT |
| scss | `.scss` | [`c478c6868648`](https://github.com/serenadeai/tree-sitter-scss/commit/c478c6868648eff49eb04a4df90d703dc45b312a) | 13 | unsupported | MIT |
| solidity | `.sol` | [`048fe686cb1f`](https://github.com/JoranHonig/tree-sitter-solidity/commit/048fe686cb1fde267243739b8bdbec8fc3a55272) | 15 | pinned | MIT |
| sql | `.sql` | [`84a2b208f072`](https://github.com/DerekStride/tree-sitter-sql/commit/84a2b208f072a2ca78fc59b5fa51cbdbf9c5aa37) | 13 | unsupported | MIT |
| svelte | `.svelte` | [`ae5199db4775`](https://github.com/tree-sitter-grammars/tree-sitter-svelte/commit/ae5199db47757f785e43a14b332118a5474de1a2) | 14 | unsupported | MIT |
| swift | `.swift` | [`f47512c92fb3`](https://github.com/alex-pinkus/tree-sitter-swift/commit/f47512c92fb3183b5b56c9ef40a442fdeb942853) | 14 | pinned | MIT |
| toml | `.toml` | [`64b56832c2cf`](https://github.com/tree-sitter-grammars/tree-sitter-toml/commit/64b56832c2cffe41758f28e05c756a3a98d16f41) | 14 | unsupported | MIT |
| tsx | `.tsx` | [`75b3874edb2d`](https://github.com/tree-sitter/tree-sitter-typescript/commit/75b3874edb2dc714fb1fd77a32013d0f8699989f) | 14 | pinned | MIT |
| typescript | `.ts` | [`75b3874edb2d`](https://github.com/tree-sitter/tree-sitter-typescript/commit/75b3874edb2dc714fb1fd77a32013d0f8699989f) | 14 | pinned | MIT |
| verilog | `.sv`, `.svh`, `.v`, `.vh` | [`227d277b6a1a`](https://github.com/tree-sitter/tree-sitter-verilog/commit/227d277b6a1a5e2bf818d6206935722a7503de08) | 14 | unsupported | MIT |
| vue | `.vue` | [`ce8011a414fd`](https://github.com/tree-sitter-grammars/tree-sitter-vue/commit/ce8011a414fdf8091f4e4071752efc376f4afb08) | 15 | unsupported | MIT |
| xml | `.rng`, `.svg`, `.xml`, `.xsd`, `.xsl`, `.xslt` | [`5000ae8f22d1`](https://github.com/tree-sitter-grammars/tree-sitter-xml/commit/5000ae8f22d11fbe93939b05c1e37cf21117162d) | 14 | unsupported | MIT |
| yaml | `.yaml`, `.yml` | [`a1c4812a73ec`](https://github.com/tree-sitter-grammars/tree-sitter-yaml/commit/a1c4812a73ec5e089de8e441fdea3a921e8d5079) | 15 | unsupported | MIT |
| zig | `.zig` | [`a80a6e9be81b`](https://github.com/maxxnino/tree-sitter-zig/commit/a80a6e9be81b33b182ce6305ae4ea28e29211bd5) | 14 | unsupported | MIT |
<!-- END GENERATED GRAMMARS -->

## Measurements

Measurements use a clean Ninja target build on macOS with Homebrew Clang and record the immediate
load average. Module size is the produced shared object in bytes. Per-language parsing uses 100
iterations of the committed fixture and reports the mean nanoseconds per parse; these are development-host
observations, not cross-host benchmarks.

```text
before: build_s=11.07 module_bytes=4375664 loadavg={ 162.40 88.87 74.21 }
after: build_s=7.09 module_bytes=89472032 loadavg={ 83.57 81.70 92.53 }
```

The build command was `/usr/bin/time -p cmake --build <clean-build> --target
liquid-frontend-tree-sitter`. The lower elapsed result despite compiling 49 grammars reflects the
different concurrent host load and warm download/file-system caches; the binary-size change is the
comparable distribution cost.

Per-grammar fixture parsing at load average `{ 85.03 82.14 92.56 }`, in mean nanoseconds:

```text
bash 7038                 c 5841                    c# 11461
c++ 7778                  cmake 6679                css 3473
dart 5892                 dockerfile 2533           elixir 7625
elm 6510                  erlang 6758               fortran 8715
go 4462                   haskell 10223             hcl 2832
html 6015                 java 5977                 javascript 4081
json 1804                 julia 6177                kotlin 6320
latex 7543                lua 3457                  make 2812
markdown 6029             nix 3066                  objective-c 5612
ocaml 5621                perl 6149                 php 6591
protocol-buffers 3515     python 5285               r 5533
ruby 6897                 rust 4784                 scala 13237
scss 3530                 solidity 7533             sql 5477
svelte 5565               swift 9109                toml 1702
tsx 7135                  typescript 4845           verilog 10850
vue 6560                  xml 4193                   yaml 3217
zig 5375
```
