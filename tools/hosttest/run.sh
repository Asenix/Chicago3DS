#!/bin/sh
# Host-side checks: compiles main.c on a PC (gcc, Linux) against stub 3DS headers and runs the game logic.
# These do NOT replace building with devkitPro and testing on a 3DS.
cd "$(dirname "$0")"
set -e
echo "== syntax check (warnings only) =="
gcc -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable -Wno-sign-compare -fsyntax-only -Wno-implicit-function-declaration -Wno-int-conversion -Istub -Wno-missing-field-initializers driver.c 2>&1 | head -20 || true
echo "== free roam logic: doors, rooms, shops, train, wildlife =="
gcc -O1 -g -std=gnu11 -w -Istub -fsanitize=address,undefined -Wl,--unresolved-symbols=ignore-all freetest.c -o freetest -lm
./freetest
echo "== game modes (0 free roam, 1 deathmatch, 2 battle royale, 3 target practice) =="
gcc -O1 -g -std=gnu11 -w -Istub -fsanitize=address,undefined -Wl,--unresolved-symbols=ignore-all sim2.c -o sim2 -lm
for m in 1 2 3; do ./sim2 $m 3000 | tail -1; done
