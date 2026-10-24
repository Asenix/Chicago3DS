Host tests for Chicago 3DS. Run ./run.sh (about a minute and a half). See the "Testing" section of the top-level README.md.

Files
  run.sh          builds and runs everything; prints a summary; exit code 0 only if every test passed
  build_env.sh    compiler flags (sourced by run.sh); strict: implicit declarations, bad pointer types etc. are errors
  fetch_deps.sh   fetches the pinned libctru / citro3d / citro2d headers into .deps/ (skip with DKP_INC=<devkitPro include dir>)
  hoststubs.c     stand-ins for the 3DS library functions the game calls (linked without --unresolved-symbols=ignore-all)
  shim/           two tiny headers the Linux build needs
  driver.c        city geometry: capacity limits, overlaps, lot bounds
  freetest.c      free roam: 43 doors, interiors, shops, train, wildlife, dynamic vertex budget
  sim2.c          bots + nav grid; modes 0..3 (free roam, deathmatch, battle royale, target practice spawn)
  tpsim.c         a full Target Practice match
  musictest.c     title music source order and fallbacks (HOST_ROMFS / HOST_SDMC point "romfs:/" and "sdmc:/" at temp folders)
  maintest.c      runs the real main() with scripted input; vertex buffer / memory / bounds / NaN checks; allocation-failure path
  budget.c        draw-budget scan (vertices per frame) using the game's real visibility function

Options
  CC=clang ./run.sh        another compiler (add SAN= if its sanitizer runtime is not installed)
  STRICT=1 ./run.sh        a compiler warning in our code fails the run
  DKP_INC=/opt/devkitpro/libctru/include ./run.sh

Deliberately NOT done here: rendering, audio output, input devices, timing. A PC cannot say how fast a 3DS draws a frame.
Compiler warnings are never filtered; the only -Wno- flags are unused-function/variable/parameter, sign-compare and
missing-field-initializers, and the sanitizer excludes shift-base (the real C2D_Color32 macro shifts 255<<24 in an int,
which ARM GCC defines).
