#!/usr/bin/env bash
# SPDX-License-Identifier: MIT

set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "usage: $0 <output-tsv>" >&2
  exit 2
fi

output=$1
image="debian:bookworm@sha256:6ebd97fa83deb272194a2cf015b3d26a4d538e9ad3a7a79d544c8af5b0a01443"
paths=(
  /usr/bin/ar
  /usr/bin/autoconf
  /usr/bin/automake
  /usr/bin/cmake
  /usr/bin/g++
  /usr/bin/gcc
  /usr/bin/make
  /usr/bin/ninja
  /usr/bin/pkg-config
  /usr/bin/python3
  /usr/bin/ranlib
)

policy_for() {
  case "$1" in
    /usr/bin/ar|/usr/bin/g++|/usr/bin/gcc|/usr/bin/make|/usr/bin/ranlib)
      printf 'implicit'
      ;;
    *)
      printf 'explicit'
      ;;
  esac
}

{
  printf '# derived-by: dpkg-query -S in %s\n' "$image"
  printf '# path\tpackage\tpolicy\tdpkg-query-output\n'
  for path in "${paths[@]}"; do
    query=$(dpkg-query -S "$path")
    package=${query%%:*}
    package=${package%%:*}
    policy=$(policy_for "$path")
    printf '%s\t%s\t%s\t%s\n' "$path" "$package" "$policy" "$query"
  done
} >"$output"
