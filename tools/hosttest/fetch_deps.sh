#!/bin/sh
# Fetches the REAL libctru / citro3d / citro2d headers (pinned commits) into .deps/ so the host tests compile against
# the same declarations the 3DS build uses. Skipped when DKP_INC points at an installed devkitPro include dir
# (e.g. /opt/devkitpro/libctru/include, which already contains the citro3d and citro2d headers).
set -e
cd "$(dirname "$0")"
fetch() { # name sha
  if [ -d ".deps/$1/.git" ] && [ "$(git -C .deps/$1 rev-parse HEAD 2>/dev/null)" = "$2" ]; then return; fi
  rm -rf ".deps/$1"; mkdir -p ".deps/$1"
  git -C ".deps/$1" init -q
  git -C ".deps/$1" remote add origin "https://github.com/devkitPro/$1.git"
  git -C ".deps/$1" fetch -q --depth 1 origin "$2"
  git -C ".deps/$1" checkout -q FETCH_HEAD
}
fetch libctru  9b55eda44cf80b971503991e0c78f9bc8fe50425
fetch citro3d  9f21cf7b380ce6f9e01a0420f19f0763e5443ca7
fetch citro2d  147b02aae021da61b1f620446ad2892ecc45411e
echo "headers ready in $(pwd)/.deps"
