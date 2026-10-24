# sourced by run.sh: sets CC, CFLAGS and INC for compiling the game against the real headers
if [ -n "$DKP_INC" ] && [ -f "$DKP_INC/3ds.h" ]; then
  INC="-isystem $DKP_INC"
else
  ./fetch_deps.sh >/dev/null
  # -isystem: warnings inside the 3DS headers themselves (they assume 32-bit pointers) are not ours to fix
  INC="-isystem .deps/libctru/libctru/include -isystem .deps/citro3d/include -isystem .deps/citro2d/include"
fi
CC=${CC:-gcc}
# Real errors stay errors: implicit declarations (an error by default in GCC 14), wrong pointer types, int<->pointer mixups.
STRICT="-Werror=implicit-function-declaration -Werror=incompatible-pointer-types -Werror=int-conversion -Werror=return-type -Werror=implicit-int"
WARN="-Wall -Wextra -Wno-unused-parameter -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable -Wno-sign-compare -Wno-missing-field-initializers"
CFLAGS="-std=gnu11 -O1 -g -D__3DS__ -Ishim $INC $STRICT $WARN"
# shift-base: the real citro2d C2D_Color32() macro shifts 255 << 24 in a signed int, which UBSAN flags but ARM GCC defines; it is not our code.
SAN=${SAN-"-fsanitize=address,undefined -fno-sanitize=shift-base -fno-omit-frame-pointer"}
