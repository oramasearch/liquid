"""Session D-Bus overlay provider for the liquid Dolphin integration."""

import os
from urllib.parse import unquote, urlparse


STATE_ATTRIBUTE = "user.liquid.state"
BADGES = {
    "online-only": "emblem-synchronizing",
    "available-offline": "emblem-default",
    "not-synced-here": "emblem-unreadable",
}
BUS_NAME = "org.oramasearch.Liquid.Overlay1"
OBJECT_PATH = "/org/oramasearch/Liquid/Overlay1"


def badge_for_state(state):
    return BADGES.get(state, "")


def local_path(value):
    parsed = urlparse(value)
    if parsed.scheme == "file":
        return unquote(parsed.path)
    if parsed.scheme:
        return None
    return value


def badge_for_path(value):
    path = local_path(value)
    if path is None:
        return ""
    try:
        state = os.getxattr(path, STATE_ATTRIBUTE).decode("utf-8", errors="strict")
    except (AttributeError, OSError, UnicodeError):
        return ""
    return badge_for_state(state)


INTROSPECTION_XML = """
<node>
  <interface name="org.oramasearch.Liquid.Overlay1">
    <method name="GetOverlay">
      <arg type="s" name="url" direction="in"/>
      <arg type="s" name="icon" direction="out"/>
    </method>
    <method name="GetOverlays">
      <arg type="as" name="urls" direction="in"/>
      <arg type="a{ss}" name="icons" direction="out"/>
    </method>
  </interface>
</node>
"""


def main():
    from gi.repository import Gio, GLib

    node = Gio.DBusNodeInfo.new_for_xml(INTROSPECTION_XML)
    interface = node.interfaces[0]
    loop = GLib.MainLoop()

    def handle_call(connection, sender, path, interface_name, method, parameters, invocation):
        del connection, sender, path, interface_name
        if method == "GetOverlay":
            invocation.return_value(GLib.Variant("(s)", (badge_for_path(parameters.unpack()[0]),)))
            return
        if method == "GetOverlays":
            values = {
                value: badge
                for value in parameters.unpack()[0]
                if (badge := badge_for_path(value))
            }
            invocation.return_value(GLib.Variant("(a{ss})", (values,)))
            return
        invocation.return_dbus_error(BUS_NAME + ".UnknownMethod", method)

    def acquired(connection, name):
        del name
        connection.register_object(OBJECT_PATH, interface, handle_call, None, None)

    owner = Gio.bus_own_name(
        Gio.BusType.SESSION,
        BUS_NAME,
        Gio.BusNameOwnerFlags.NONE,
        acquired,
        None,
        None,
    )
    try:
        loop.run()
    finally:
        Gio.bus_unown_name(owner)


if __name__ == "__main__":
    main()
