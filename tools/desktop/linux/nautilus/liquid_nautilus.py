"""GNOME Files badge provider for liquid drive mounts."""

import os
from urllib.parse import unquote, urlparse


STATE_ATTRIBUTE = "user.liquid.state"
BADGES = {
    "online-only": "emblem-synchronizing",
    "available-offline": "emblem-default",
    "not-synced-here": "emblem-unreadable",
}


def badge_for_state(state):
    return BADGES.get(state)


def state_for_path(path):
    try:
        value = os.getxattr(path, STATE_ATTRIBUTE)
    except (AttributeError, OSError, UnicodeError):
        return None
    return value.decode("utf-8", errors="strict")


try:
    from gi.repository import GObject, Nautilus
except ImportError:
    GObject = None
    Nautilus = None


if GObject is not None and Nautilus is not None:

    class LiquidDriveBadges(GObject.GObject, Nautilus.InfoProvider):
        def update_file_info(self, file_info):
            uri = file_info.get_uri()
            parsed = urlparse(uri)
            if parsed.scheme != "file":
                return Nautilus.OperationResult.COMPLETE
            badge = badge_for_state(state_for_path(unquote(parsed.path)))
            if badge:
                file_info.add_emblem(badge)
            return Nautilus.OperationResult.COMPLETE
