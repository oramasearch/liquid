#!/bin/sh
set -eu

if [ "$#" -ne 1 ]; then
    echo "usage: tools/grammars/remove.sh <language>" >&2
    exit 2
fi

script_directory=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
exec python3 "$script_directory/manage.py" remove "$1"
