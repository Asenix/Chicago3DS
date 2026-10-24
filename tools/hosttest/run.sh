#!/bin/sh
# Host-side tests for Chicago 3DS: compile source/main.c on a PC against the REAL libctru / citro3d / citro2d headers and run the
# game's own logic (city generation, doors, interiors, shops, train, wildlife, nav grid, bots, the four game modes, the music
# fallback and the real main() loop) under AddressSanitizer + UndefinedBehaviorSanitizer.
#
# What this is NOT: a 3DS build, a renderer, or a speed test. The 3DS libraries are replaced by stand-ins (hoststubs.c), so a PC
# says nothing about rendering, audio output, input feel, or frame rate. The authoritative build is `make` under devkitPro (the
# GitHub Actions workflow); the authoritative test is a real console.
#
# usage:  tools/hosttest/run.sh            run everything
#         CC=clang tools/hosttest/run.sh   another compiler (set SAN= to drop the sanitizers if its runtime is not installed)
#         STRICT=1 tools/hosttest/run.sh   treat any compiler warning in our code as a failure
#         DKP_INC=/opt/devkitpro/libctru/include tools/hosttest/run.sh   use an installed devkitPro instead of fetching headers
cd "$(dirname "$0")" || exit 1
. ./build_env.sh || { echo "could not set up the build environment (headers fetch failed?)"; exit 1; }
mkdir -p bin
RESULTS=""; FAILED=0
note() { RESULTS="$RESULTS
$1"; }

compile() {   # compile <test>: the full compiler output is shown; nothing is filtered
	printf '== compile %s\n' "$1"
	if ! $CC $CFLAGS $SAN "$1.c" hoststubs.c -o "bin/$1" -lm >"bin/$1.build.log" 2>&1; then
		cat "bin/$1.build.log"; note "FAIL  compile $1"; FAILED=1; return 1
	fi
	cat "bin/$1.build.log"
	if [ "${STRICT:-0}" = 1 ] && grep -q "warning:" "bin/$1.build.log"; then note "FAIL  compile $1 (warnings, STRICT=1)"; FAILED=1; return 1; fi
	return 0
}

run() {       # run <label> <command...>
	label=$1; shift
	printf '== run %s\n' "$label"
	"$@" >"bin/run.log" 2>&1; rc=$?
	cat "bin/run.log"
	if [ $rc -ne 0 ] || grep -qE "runtime error|AddressSanitizer|LeakSanitizer|^FAIL" "bin/run.log"; then note "FAIL  $label (exit $rc)"; FAILED=1; else note "ok    $label"; fi
}

for t in driver freetest sim2 tpsim musictest maintest budget; do compile "$t" || true; done

[ -x bin/driver ]   && run "city geometry: capacity, overlaps, lots"          ./bin/driver
[ -x bin/freetest ] && run "free roam: 43 doors, rooms, shops, train, wildlife" ./bin/freetest
if [ -x bin/sim2 ]; then
	run "bots + nav grid, free roam (600 frames)"              ./bin/sim2 0 600
	run "Deathmatch (6000 frames)"                             ./bin/sim2 1 6000
	run "Battle Royale (6000 frames)"                          ./bin/sim2 2 6000
	run "Target Practice spawn (3000 frames)"                  ./bin/sim2 3 3000
fi
[ -x bin/tpsim ]    && run "Target Practice, a full 5-round match"           ./bin/tpsim

if [ -x bin/musictest ]; then   # the music source order: romfs, SD card, built-in theme, silence
	T=$(mktemp -d); mkdir -p "$T/romfs" "$T/sdmc/3ds/Chicago3DS" "$T/empty"
	head -c 65536 /dev/zero >"$T/romfs/title.pcm"; head -c 65536 /dev/zero >"$T/sdmc/3ds/Chicago3DS/title.pcm"
	mkdir -p "$T/tiny"; printf 'ab' >"$T/tiny/title.pcm"
	run "music: romfs title.pcm streams"                  env HOST_ROMFS="$T/romfs" HOST_SDMC="$T/empty" ./bin/musictest romfs
	run "music: SD card title.pcm streams"                env HOST_ROMFS="$T/empty" HOST_SDMC="$T/sdmc" ./bin/musictest sdmc
	run "music: no file -> built-in theme"                env HOST_ROMFS="$T/empty" HOST_SDMC="$T/empty" ./bin/musictest none
	run "music: 2-byte file -> built-in theme"            env HOST_ROMFS="$T/tiny" HOST_SDMC="$T/empty" ./bin/musictest tiny
	run "music: thread cannot start -> built-in theme"    env HOST_ROMFS="$T/romfs" HOST_SDMC="$T/empty" ./bin/musictest threadfail
	run "music: no DSP firmware -> silent, no crash"      env HOST_ROMFS="$T/romfs" HOST_SDMC="$T/empty" ./bin/musictest nodsp
	rm -rf "$T"
fi

if [ -x bin/maintest ]; then    # the real main(): splash, title, menus, loading, play
	run "main loop: title orbit (1200 frames)"           ./bin/maintest title 1200
	run "main loop: Free Roam (3000 frames)"             ./bin/maintest free 3000
	run "main loop: Deathmatch (3000 frames)"            ./bin/maintest dm 3000
	run "main loop: Battle Royale (3000 frames)"         ./bin/maintest br 3000
	run "main loop: Target Practice (3000 frames)"       ./bin/maintest tp 3000
	run "main loop: vertex buffer allocation fails -> clean exit" env MAINTEST_FAILALLOC=1000000 ./bin/maintest free 3000
fi

if [ -x bin/budget ]; then
	run "draw budget, fog on"                            ./bin/budget 1
	run "draw budget, fog off"                           ./bin/budget 0
fi

printf '\n==== summary ====%s\n' "$RESULTS"
if [ "$FAILED" = 0 ]; then echo "ALL HOST TESTS PASSED (this is not a 3DS build or a speed test)"; else echo "HOST TESTS FAILED"; fi
exit "$FAILED"
