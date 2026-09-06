# Nautilus drive badges

`liquid_nautilus.py` implements the Nautilus Python `InfoProvider` and maps the three
`user.liquid.state` values to theme emblems. It reads local attributes only and ignores non-file
URLs and paths outside liquid mounts.

Run `./install.sh` for a per-user installation or `./install.sh --prefix <path>` for a packaging
staging directory. The extension requires Nautilus Python and PyGObject. Run
`python3 test_badges.py` to verify the mapping without a desktop session. The parent
[`README.md`](../README.md) contains the screenshot procedure.
