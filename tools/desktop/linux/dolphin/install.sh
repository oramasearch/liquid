#!/bin/sh
# SPDX-License-Identifier: MIT

set -eu

install_prefix="${XDG_DATA_HOME:-${HOME}/.local/share}"
if [ "${1:-}" = "--prefix" ]; then
    if [ "$#" -ne 2 ]; then
        echo "usage: install.sh [--prefix path]" >&2
        exit 2
    fi
    install_prefix="$2"
elif [ "$#" -ne 0 ]; then
    echo "usage: install.sh [--prefix path]" >&2
    exit 2
fi

source_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
libexec_dir="${install_prefix}/liquid"
service_dir="${install_prefix}/dbus-1/services"
autostart_dir="${XDG_CONFIG_HOME:-${HOME}/.config}/autostart"
install -d "$libexec_dir" "$service_dir" "$autostart_dir"
install -m 0755 "${source_dir}/liquid_overlay_provider.py" \
    "${libexec_dir}/liquid-overlay-provider"
sed "s|@LIBEXEC@|${libexec_dir}|g" \
    "${source_dir}/org.oramasearch.Liquid.Overlay1.service" \
    >"${service_dir}/org.oramasearch.Liquid.Overlay1.service"
sed "s|@LIBEXEC@|${libexec_dir}|g" \
    "${source_dir}/liquid-overlay-provider.desktop" \
    >"${autostart_dir}/liquid-overlay-provider.desktop"
chmod 0644 "${service_dir}/org.oramasearch.Liquid.Overlay1.service"
chmod 0644 "${autostart_dir}/liquid-overlay-provider.desktop"
echo "installed Dolphin overlay provider; restart Dolphin and the user session service"
