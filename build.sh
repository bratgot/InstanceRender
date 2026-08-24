#!/usr/bin/env bash
# Configure and build InstanceRender on Linux.
#
#   ./build.sh /usr/local/Nuke17.1v1                    # one Nuke
#   ./build.sh /usr/local/Nuke17.1v1 --install          # + into ~/.nuke
#   EMBREE_ROOT=/opt/embree4 ./build.sh /usr/local/Nuke16.1v1
#
# Use the compiler Foundry documents for that Nuke: gcc 9 for Nuke 14, gcc 11
# for 15 to 17. One build directory per MINOR version, because the NDK is not
# compatible across them.
#
# WHAT HAS AND HAS NOT BEEN PROVEN. The sources compile with g++ 11.5 on
# AlmaLinux 9 and link into a correctly named InstanceRender.so. Nothing has
# been RUN: there is no Linux Nuke on the machine this was written on, so the
# link was against stub libraries - and a shared MODULE on Linux happily links
# with undefined symbols, so that proves the build system, not that the plugin
# resolves against a real libDDImage. Treat the first real build as the test.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"

nuke="${1:-${NUKE_ROOT:-/usr/local/Nuke17.1v1}}"
shift || true
install=0
for a in "$@"; do
  case "$a" in
    --install) install=1 ;;
    *) echo "unknown argument: $a"; exit 1 ;;
  esac
done

if [ ! -e "$nuke/libDDImage.so" ]; then
  echo "no libDDImage.so in $nuke - pass the Nuke install root as the first argument"
  exit 1
fi

ver="$(basename "$nuke" | sed -E 's/^Nuke([0-9]+\.[0-9]+).*/\1/')"
bd="$here/build$ver"

cmake -S "$here" -B "$bd" \
  -DCMAKE_BUILD_TYPE=Release \
  -DNUKE_ROOT="$nuke" \
  ${EMBREE_ROOT:+-DEMBREE_ROOT="$EMBREE_ROOT"} \
  ${TBB_INCLUDE_DIR:+-DTBB_INCLUDE_DIR="$TBB_INCLUDE_DIR"} \
  ${BOOST_INCLUDE_DIR:+-DBOOST_INCLUDE_DIR="$BOOST_INCLUDE_DIR"}

cmake --build "$bd" -j"$(nproc)"

if [ "$install" = 1 ]; then
  cmake --install "$bd" --prefix "$HOME/.nuke"
fi
echo "done: $bd"
