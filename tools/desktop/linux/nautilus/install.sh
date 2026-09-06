#!/bin/sh
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
extension_dir="${install_prefix}/nautilus-python/extensions"
install -d "$extension_dir"
install -m 0644 "${source_dir}/liquid_nautilus.py" "${extension_dir}/liquid_nautilus.py"
echo "installed Nautilus extension; run: nautilus -q"
