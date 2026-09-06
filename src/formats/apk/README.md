# Alpine packages

The unsigned Tier A writer emits the apk v2 control and data gzip members. `.PKGINFO` records the
compressed data member's SHA-256 digest plus derived `so:` dependencies. Both members use liquid's
own fixed stored-block deflate encoder. The minimal reader splits the members, verifies their tar
structure, and exposes `.PKGINFO` and installed paths.
