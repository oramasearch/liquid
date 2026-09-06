#!/bin/sh
set -eu

if [ "$#" -ne 3 ]; then
    echo "usage: tools/grammars/add.sh <language> <repository> <commit>" >&2
    exit 2
fi

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
exec python3 "$script_dir/manage.py" add "$1" "$2" "$3"
