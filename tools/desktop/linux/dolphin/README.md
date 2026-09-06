# Dolphin drive badges

`liquid_overlay_provider.py` publishes the three `user.liquid.state` badge mappings on the user
D-Bus. The service and desktop files start it on demand or at KDE login; it reads local attributes
only.

Run `./install.sh` for a per-user installation or `./install.sh --prefix <path>` for a packaging
staging directory. The provider requires PyGObject. Run `python3 test_badges.py` to verify the
mapping without a desktop session. The parent [`README.md`](../README.md) contains the screenshot
procedure.
