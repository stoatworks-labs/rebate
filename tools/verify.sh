#!/usr/bin/env bash
#
# Everything that can be checked without a host, in one go, in the order that
# fails fastest.
#
#   tools/verify.sh
#
# Each check answers a question none of the others can:
#
#   build         a FRESH universal Release build. Not the dev build: CMake
#                 latches the architecture list at the first target, so the
#                 only build worth measuring is one configured from nothing.
#   shaders       does every shader compile, through a real GLSL compiler,
#                 before a host has to find out. The shaders are assembled at
#                 run time from one model library, so the text compiled here is
#                 what `rbtest --dump-shaders` writes: the exact strings the
#                 plugin hands the driver.
#   demo          the browser demo's copies of those shaders, and of the
#                 glyph table, are still the plugin's, character for
#                 character. demo/plugin.js necessarily holds a second copy of
#                 every shader, and two copies drift quietly: the plugin keeps
#                 working, the page keeps working, and they stop being the same
#                 effect. Nothing else in this file looks at the page at all.
#   physics       every harness check, at TWO rasters: 320x180, which is what
#                 CI renders at, and 1280x720. A check that holds at one raster
#                 was fitted to it. Each is measured out of the picture:
#                   --curve     the straight line's slope is gamma; it meets
#                               fog at the toe and Dmax at the shoulder
#                   --push      one stop: gamma x 1.15, fog + 0.03
#                   --mask      C-41 with the mask inverts neutral to neutral
#                   --grain     variance c(1-c)/N, peak at 0.5, zero at ends
#                   --leak      follows the curve, saturates, is warm
#                   --cross     reversal film in C-41 scans warm, as predicted
#                   --rebate    holes black, border unexposed, print lighter
#                   --seed      grain bit-identical for a seed, at any raster
#                   --resize    the scanner's levels survive a resize
#                   --negative  every one of those FAILS on a perturbed model
#                   --cpu       the OpenFX build's CPU copy of the passes
#                               agrees with the shaders, and a control fails
#   sweep         does every control change the picture. A GLSL uniform whose
#                 name does not match the C++ is ignored without a word.
#   bench         the render cost, for the record. Not pass/fail.
#   registration  does the bundle contain a plugin at all -- a file-scope
#                 CFFGLPluginInfo nothing names, which a linker may drop while
#                 still producing a bundle that loads and exports plugMain.
#   lipo          is the build really universal.
#   plist         does CFBundleExecutable name the binary that is on disk.
#   codesign      the exact command the release job runs, against a copy.
#   oxbow         a real FFGL host loads the bundle and reports the name, id
#                 and type it sees -- the name field is not null-terminated
#                 and a host truncates silently past 16 characters.
#   openfx        the OpenFX bundle: CFBundleExecutable names the binary on
#                 disk (a plist copied from another repo passes everything
#                 until codesign at release), it exports OfxGetPlugin, it is
#                 universal, it ad-hoc signs, and ofxprobe loads it as
#                 com.stoatworks.rebate and renders a frame that is not its
#                 input.
#
set -uo pipefail

cd "$(dirname "$0")/.."

BUILD="${BUILD:-build-universal}"
failures=0

step() { printf '\n\033[1m== %s\033[0m\n' "$1"; }
pass() { printf '   \033[32mok\033[0m   %s\n' "$1"; }
fail() { printf '   \033[31mFAIL\033[0m %s\n' "$1"; failures=$(( failures + 1 )); }

step "build (fresh universal Release, $BUILD)"
rm -rf "$BUILD"
if cmake -B "$BUILD" -DCMAKE_BUILD_TYPE=Release >/dev/null 2>&1 \
   && cmake --build "$BUILD" --parallel >/dev/null 2>&1; then
	pass "builds"
else
	fail "build failed -- run: cmake -B $BUILD -DCMAKE_BUILD_TYPE=Release && cmake --build $BUILD"
	exit 1
fi

RBTEST="$BUILD/rbtest"

#---------------------------------------------------------------------------
# Every shader, through a real GLSL compiler.
#
# --target-env=opengl4.5 with -fauto-map-locations: glslc targets SPIR-V, which
# demands an explicit layout( location ) on every uniform and varying. Those are
# Vulkan rules and not GLSL ones, and without the flag every shader "fails" for
# reasons that have nothing to do with the code.
#
# glslc is optional -- `brew install shaderc` -- so a machine without it skips
# rather than fails. No shaders at all is a FAILURE: it means the dump broke.
#---------------------------------------------------------------------------
step "shaders"
if ! command -v glslc >/dev/null 2>&1; then
	printf '   skipped: glslc not installed (brew install shaderc)\n'
else
	dir="$( mktemp -d )"
	"$RBTEST" --dump-shaders "$dir" >/dev/null
	n=0; bad=0
	for shader in "$dir"/*.vert "$dir"/*.frag; do
		[ -e "$shader" ] || continue
		n=$(( n + 1 ))
		if ! glslc --target-env=opengl4.5 -fauto-map-locations "$shader" -o /dev/null 2>"$dir/err"; then
			printf '   %s does not compile\n' "$( basename "$shader" )"
			sed "s|$dir/||; s|^|      |" "$dir/err"
			bad=$(( bad + 1 ))
		fi
	done
	rm -rf "$dir"
	if [ "$n" -eq 0 ]; then
		fail "no shaders were dumped"
	elif [ "$bad" -eq 0 ]; then
		pass "all $n shaders compile"
	else
		fail "$bad of $n shaders do not compile"
	fi
fi


#---------------------------------------------------------------------------
# The browser demo's copy of the same GLSL.
#
# `demo/plugin.js` cannot include a C++ file, so it carries its own copy of
# every shader piece, and of Font.cpp's glyph table. This compares the two
# character for character, and checks both sides assemble the five passes the
# same way -- reformatting counts, deliberately, because "it is only
# whitespace" is how a real change gets waved through. It says nothing about the
# demo's PORT of Model.cpp, Controls.cpp and Frame.cpp; only a reader checks
# that.
#---------------------------------------------------------------------------
step "demo: the browser copy of the shaders"
if [ -f demo/tools/check_shaders.py ]; then
	log="$( mktemp )"
	if python3 demo/tools/check_shaders.py >"$log" 2>&1; then
		pass "$( tail -1 "$log" )"
	else
		fail "the demo's shaders have drifted from source/ -- copy the C++ across"
		grep -v '^ok' "$log" | sed 's/^/      /'
	fi
	rm -f "$log"
else
	printf '   skipped: no demo/\n'
fi


for size in 320x180 1280x720; do
	step "physics at $size"
	for check in curve push mask grain leak cross rebate seed resize negative cpu; do
		if out=$("$RBTEST" --$check --size $size 2>&1); then
			pass "rbtest --$check: $( printf '%s\n' "$out" | grep -v '^$' | tail -1 )"
		else
			fail "rbtest --$check at $size"
			printf '%s\n' "$out" | sed 's/^/      /'
		fi
	done
done

#---------------------------------------------------------------------------
# --pipe, in the fleet's frame format. Two and a half frames in must be exactly
# two frames out and a clean exit -- a partial frame is the end of the stream,
# never a frame -- and a cue naming no parameter must be refused rather than
# silently doing nothing to a take.
#---------------------------------------------------------------------------
step "pipe"
frame=$(( 64 * 36 * 4 ))
raw=$( mktemp ); cues=$( mktemp )
head -c $(( frame * 5 / 2 )) /dev/zero > "$raw"
got=$( "$RBTEST" --pipe --size 64x36 < "$raw" 2>/dev/null | wc -c | tr -d ' ' )
status=${PIPESTATUS[0]}
if [ "$status" -eq 0 ] && [ "$got" = "$(( frame * 2 ))" ]; then
	pass "2.5 frames in, exactly 2 frames out, clean exit"
else
	fail "2.5 frames in gave $got bytes out (want $(( frame * 2 ))), exit $status"
fi
# Read from a file, not a pipe: a writer killed by SIGPIPE would fail the
# pipeline whatever rbtest did, and the refusal would pass for the wrong reason.
printf '0 No Such Control 0.5\n' > "$cues"
"$RBTEST" --pipe --size 64x36 --script "$cues" < "$raw" >/dev/null 2>&1
status=$?
if [ "$status" -eq 2 ]; then
	pass "a cue naming no parameter is refused (exit 2)"
else
	fail "a cue naming no parameter gave exit $status, not 2"
fi
# A reader that hangs up early (`| head -c 1`, ffmpeg dying) must end the run
# with exit 1 and a message, not SIGPIPE's silent 141.
head -c $(( frame * 20 )) /dev/zero > "$raw"
"$RBTEST" --pipe --size 64x36 < "$raw" 2>/dev/null | head -c 1 >/dev/null
status=${PIPESTATUS[0]}
if [ "$status" -eq 1 ]; then
	pass "a closed stdout ends the run with exit 1, not SIGPIPE"
else
	fail "a closed stdout gave exit $status, not 1"
fi
rm -f "$raw" "$cues"

step "sweep"
if out=$(python3 tools/sweep.py --binary "$RBTEST" 2>/dev/null); then
	pass "$( printf '%s\n' "$out" | tail -1 )"
else
	fail "tools/sweep.py reports a dead control"
	printf '%s\n' "$out" | grep -E '^DEAD|DEAD CONTROLS' | sed 's/^/      /'
fi

step "bench (for the record)"
"$RBTEST" --bench --frames 60 2>&1 | sed -n '3,7p' | sed 's/^/   /'
printf '   OpenFX (CPU):\n'
"$RBTEST" --bench-cpu --size 1920x1080 --frames 10 2>&1 | sed -n '2,3p' | sed 's/^/   /'

BUNDLE="$BUILD/Rebate.bundle"
BIN="$BUNDLE/Contents/MacOS/Rebate"

if [ "$(uname)" = "Darwin" ] && [ -d "$BUNDLE" ]; then
	step "registration"
	# `nm ... | grep -q X` FAILS when grep FINDS its match under `set -o pipefail`:
	# grep exits at once, nm takes SIGPIPE, and the pipeline reports failure.
	# Capture and match instead of piping.
	syms=$(nm -gU "$BIN" 2>/dev/null)
	case "$syms" in
		*_plugMain*) pass "exports plugMain" ;;
		*) fail "no plugMain -- the bundle contains no plugin" ;;
	esac

	step "lipo"
	archs=$(lipo -archs "$BIN" 2>/dev/null)
	case "$archs" in *arm64*) pass "arm64 present" ;; *) fail "no arm64 (got: $archs)" ;; esac
	case "$archs" in *x86_64*) pass "x86_64 present" ;; *) fail "no x86_64 (got: $archs) -- a universal build was asked for" ;; esac

	step "plist"
	exe=$(/usr/libexec/PlistBuddy -c "Print :CFBundleExecutable" "$BUNDLE/Contents/Info.plist" 2>/dev/null)
	ident=$(/usr/libexec/PlistBuddy -c "Print :CFBundleIdentifier" "$BUNDLE/Contents/Info.plist" 2>/dev/null)
	if [ -n "$exe" ] && [ -f "$BUNDLE/Contents/MacOS/$exe" ]; then
		pass "CFBundleExecutable ($exe) is on disk"
	else
		fail "CFBundleExecutable is '$exe' but no such binary exists -- codesign will fail after the tag"
	fi
	if [ "$ident" = "com.stoatworks.ffgl.rebate" ]; then
		pass "CFBundleIdentifier is $ident"
	else
		fail "CFBundleIdentifier is '$ident'"
	fi

	step "codesign"
	tmp=$(mktemp -d)
	cp -R "$BUNDLE" "$tmp/" 2>/dev/null
	if codesign --force --sign - --timestamp=none "$tmp/Rebate.bundle" >/dev/null 2>&1; then
		pass "ad-hoc signs (the command the release job runs)"
	else
		fail "ad-hoc signing failed"
	fi
	rm -rf "$tmp"

	step "oxbow"
	OXBOW="${OXBOW:-../oxbow/build/oxbow}"
	[ -x "$OXBOW" ] || OXBOW="$HOME/Projects/resolume/oxbow/build/oxbow"
	if [ -x "$OXBOW" ]; then
		probe=$("$OXBOW" probe "$BUNDLE" 2>&1)
		for want in "name:        SW Rebate" "id:          RB01" "type:        effect"; do
			case "$probe" in
				*"$want"*) pass "host sees '$want'" ;;
				*) fail "host does not see '$want' -- see: $OXBOW probe $BUNDLE" ;;
			esac
		done
		self=$("$OXBOW" selftest "$BUNDLE" 2>&1)
		case "$self" in
			*"selftest:    PASS"*) pass "instantiates through plugMain and renders 120 frames" ;;
			*) fail "oxbow selftest did not pass -- see: $OXBOW selftest $BUNDLE" ;;
		esac
	else
		printf '   skipped: oxbow not built at %s\n' "$OXBOW"
	fi
fi

#---------------------------------------------------------------------------
# The OpenFX bundle.
#
# cmake/InfoOFX.plist.in is copied from repo to repo, and a copy with the
# previous plugin's name in CFBundleExecutable does not fail the build: the
# bundle assembles, lipo and nm pass, ofxprobe loads it and renders a correct
# frame. It fails at RELEASE time, in codesign, with a message that never
# mentions the plist. So: the plist against the binary on disk, and the exact
# codesign the release job runs, against a copy.
#
# ofxprobe scans /Library/OFX/Plugins as well as --dir, and the first bundle
# declaring an identifier wins -- an installed Rebate there would be what got
# probed. Checked rather than assumed.
#---------------------------------------------------------------------------
OFX_BUNDLE="$BUILD/Rebate.ofx.bundle"
OFX_BIN="$OFX_BUNDLE/Contents/MacOS/Rebate.ofx"
if [ "$(uname)" = "Darwin" ]; then
	step "openfx"
	if [ ! -d "$OFX_BUNDLE" ]; then
		fail "no OpenFX bundle at $OFX_BUNDLE (built with -DBUILD_OFX=OFF?)"
	else
		exe=$(/usr/libexec/PlistBuddy -c "Print :CFBundleExecutable" "$OFX_BUNDLE/Contents/Info.plist" 2>/dev/null)
		ident=$(/usr/libexec/PlistBuddy -c "Print :CFBundleIdentifier" "$OFX_BUNDLE/Contents/Info.plist" 2>/dev/null)
		if [ -n "$exe" ] && [ -f "$OFX_BUNDLE/Contents/MacOS/$exe" ]; then
			pass "CFBundleExecutable ($exe) is on disk"
		else
			fail "CFBundleExecutable is '$exe' but no such binary exists -- codesign will fail after the tag"
		fi
		if [ "$ident" = "com.stoatworks.rebate.ofx" ]; then
			pass "CFBundleIdentifier is $ident"
		else
			fail "CFBundleIdentifier is '$ident'"
		fi

		syms=$(nm -gU "$OFX_BIN" 2>/dev/null)
		case "$syms" in
			*_OfxGetPlugin*) pass "exports OfxGetPlugin" ;;
			*) fail "no OfxGetPlugin -- no host will see a plugin" ;;
		esac

		archs=$(lipo -archs "$OFX_BIN" 2>/dev/null)
		case "$archs" in *arm64*) pass "arm64 present" ;; *) fail "no arm64 (got: $archs)" ;; esac
		case "$archs" in *x86_64*) pass "x86_64 present" ;; *) fail "no x86_64 (got: $archs)" ;; esac

		tmp=$(mktemp -d)
		cp -R "$OFX_BUNDLE" "$tmp/" 2>/dev/null
		if codesign --force --sign - --timestamp=none "$tmp/Rebate.ofx.bundle" >/dev/null 2>&1; then
			pass "ad-hoc signs (the command the release job runs)"
		else
			fail "ad-hoc signing the OpenFX bundle failed"
		fi
		rm -rf "$tmp"

		OFXPROBE="${OFXPROBE:-../resolume-ofx-bridge/build/ofxprobe}"
		[ -x "$OFXPROBE" ] || OFXPROBE="$HOME/Projects/resolume/resolume-ofx-bridge/build/ofxprobe"
		if [ ! -x "$OFXPROBE" ]; then
			printf '   skipped: ofxprobe not built at %s\n' "$OFXPROBE"
		else
			installed=$(grep -rl "com.stoatworks.rebate" /Library/OFX/Plugins 2>/dev/null)
			if [ -n "$installed" ]; then
				fail "an installed bundle in /Library/OFX/Plugins declares com.stoatworks.rebate; ofxprobe may load it instead: $installed"
			fi
			manifest=$("$OFXPROBE" --dir "$BUILD" --manifest com.stoatworks.rebate 2>&1)
			case "$manifest" in
				*"\"label\": \"Rebate\""*"\"grouping\": \"Stoatworks\""*) pass "ofxprobe loads com.stoatworks.rebate as Rebate, in Stoatworks" ;;
				*) fail "ofxprobe does not describe com.stoatworks.rebate as Rebate / Stoatworks -- see: $OFXPROBE --dir $BUILD --manifest com.stoatworks.rebate" ;;
			esac
			case "$manifest" in
				*"\"bundlePath\": \"$PWD/$OFX_BUNDLE\""*|*"\"bundlePath\": \"$OFX_BUNDLE\""*) pass "the bundle ofxprobe loaded is this build's" ;;
				*) fail "ofxprobe loaded com.stoatworks.rebate from somewhere other than $OFX_BUNDLE" ;;
			esac
			out="$(mktemp -d)/ofx.bmp"
			result=$("$OFXPROBE" --dir "$BUILD" --render com.stoatworks.rebate --size 640x360 --out "$out" 2>&1)
			if ! grep -q "rendered" <<<"$result"; then
				fail "the OpenFX bundle does not render"
				sed 's/^/       /' <<<"$result"
			elif grep -qE "^ *0 of [0-9]+ bytes differ" <<<"$result"; then
				fail "the OpenFX bundle renders its input unchanged"
			else
				pass "renders ($(grep -oE '[0-9]+ of [0-9]+ bytes differ' <<<"$result") from the input)"
			fi
			rm -rf "$(dirname "$out")"
		fi
	fi
fi

printf '\n'
if [ "$failures" -eq 0 ]; then
	printf '\033[32mall checks passed\033[0m\n'
else
	printf '\033[31m%d check(s) failed\033[0m\n' "$failures"
fi
exit $(( failures > 0 ? 1 : 0 ))
