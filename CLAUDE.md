# rebate

Colour negative film, and the scan of it, as an FFGL **effect** for Resolume
Arena/Avenue, and as an **OpenFX** filter (`com.stoatworks.rebate`, "Rebate" in
"Stoatworks") for Resolve/Vegas/Nuke/Natron. C++/GLSL, CMake MODULE → universal
`.bundle` (macOS) + Windows `.dll`; `Rebate.ofx.bundle` (macOS universal, Win64,
Linux x86_64). MIT.

Read `AGENTS.md` before changing the curve, the dyes and the mask, the grain, or the
scanner.

## Commands (CMake)
- Configure: `cmake -B build -DCMAKE_BUILD_TYPE=Release`
- Fast dev build: add `-DCMAKE_OSX_ARCHITECTURES=arm64`
- Universal (what ships): `cmake -B build-universal -DCMAKE_BUILD_TYPE=Release`
- Build: `cmake --build build --parallel`
- Install into Arena: `cmake --install build` — **not run from a session**, it writes
  into `~/Documents/Resolume Arena/Extra Effects`
- Render a frame offline: `./build/rbtest --out /tmp/f.png --size 1920x1080`
- Set anything by name: `--set "Stock=4" --set "Auto Levels=0" --set "View=1"`
  (0..1 for sliders, the real integer for integers, the element index for options)
- List parameters, kinds, defaults and ranges: `./build/rbtest --list`
- Other sources: `--source flat --level 128`, `--source white`
- The exact GLSL the plugin compiles: `./build/rbtest --dump-shaders DIR`
- Footage through the real shaders — **`--pipe`**, raw RGBA frames in, raw RGBA frames
  out, with `--size WxH`, `--fps N` (frame n is clocked at n / fps) and an optional
  `--script` of `frame Parameter Name value` cues, linearly interpolated between
  cues; a cue naming no parameter is refused with exit 2, a partial frame at the end
  of stdin ends the stream cleanly, a failed render or a closed stdout exits 1:
  `ffmpeg … -f rawvideo -pix_fmt rgba - | ./build/rbtest --pipe --size 1920x1080 [--script cues.txt] | ffmpeg …`
- The same footage through the **OpenFX build's CPU render**, no host: add
  `--engine cpu` (and `--filter-bits 8` to filter as this GPU does)
- OpenFX only, no FFGL SDK or GLEW: `-DREBATE_BUILD_FFGL=OFF`; no OpenFX: `-DBUILD_OFX=OFF`.
  The bundle lands at `build/Rebate.ofx.bundle`; nothing installs it
- An OFX host from the command line: `~/Projects/resolume/resolume-ofx-bridge/build/ofxprobe
  --dir build --render com.stoatworks.rebate --size 1920x1080 --out f.bmp`. It scans
  `/Library/OFX/Plugins` too and the first identifier match wins

## Verify
- Everything: `tools/verify.sh` (fresh universal build + glslc + the demo's shader
  copies + every check at 320x180 AND 1280x720, `--cpu` included + --pipe + the sweep +
  the bundle + the OpenFX bundle through ofxprobe, ~30 s)
- The demo's shaders and glyph table are still the plugin's: `python3 demo/tools/check_shaders.py`
- The curve: slope γ, toe and shoulder where it says: `./build/rbtest --curve`
- Push reads back γ × 1.15 and fog + 0.03: `./build/rbtest --push`
- The mask cancels (neutral in, neutral out): `./build/rbtest --mask`
- Grain variance c(1-c)/N, peak at 0.5: `./build/rbtest --grain`
- The leak follows the curve and is warm: `./build/rbtest --leak`
- Cross-processing scans warm as predicted: `./build/rbtest --cross`
- Holes black, border unexposed, edge print light: `./build/rbtest --rebate`
- Grain reproduces from its seed, at any raster: `./build/rbtest --seed`
- The scanner's levels survive a resize: `./build/rbtest --resize`
- The checks can fail: `./build/rbtest --negative`; one perturbation verbosely:
  `./build/rbtest --perturb BITS --mask` (bits in `Model.h`)
- The OpenFX build's CPU passes agree with the shaders: `./build/rbtest --cpu` (measures
  the GL's filter first; the minified edge print below ~245 rows is reported, not bounded)
- The OpenFX build's render cost: `./build/rbtest --bench-cpu --size 1920x1080`
- Every check takes `--size WxH`; CI runs them at 320x180
- No dead controls: `python3 tools/sweep.py` (`--size WxH`, `--jobs N`)
- Render cost: `./build/rbtest --bench` (best of three; the GPU here is shared)
- What a host sees: `~/Projects/resolume/oxbow/build/oxbow probe build-universal/Rebate.bundle`

## Notes
- **The film is a process, not a LUT.** Scene → crossover → log H → curve (coverage
  c) → dye → channel density with the mask → transmittance → scanner. `Model.h` holds
  the numbers; the `kModel` GLSL library in `Shaders.cpp` holds the arithmetic, and is
  assembled into the film, blocks and scan passes. A wrong curve is a GLSL fix —
  **and a `Render.cpp` fix**: the OpenFX build's CPU copy of the per-pixel passes,
  marked `//= mirrored` on both sides. `rbtest --cpu` fails when the two part.
- **The per-frame arithmetic is `render::Prepare`** (Render.cpp), which ProcessOpenGL
  and the OpenFX build both call; the defaults are `render::HostValues`, which the
  FFGL constructor and the OFX describe both read. One copy of each.
- **The OpenFX build is stateless**: every frame is a function of its own source,
  params and time. Auto Levels measures each frame alone (no smoothing); the grain's
  film frame is `floor( t / fps × 24 )`. Normalise 8-bit input by dividing, never by a
  reciprocal: the GPU's c / 255 is an ULP from c × (1/255), enough to flip a grain site.
- **The harness never re-types the model.** It takes constants from `Model.h`
  (γ, toe, latitude, N) and measures everything else out of the picture. `Coverage`
  in `Model.cpp` exists for the harness to *choose* inputs with, never to predict.
- **Nothing absolute crosses into GLSL.** The grain's film frame is `floor(now × 24)`
  reduced in double; the levels' smoothing step is computed from dt in double.
- **No mip chain on the scene.** `glGenerateMipmap` on a 4K RGBA32F texture cost
  11 ms a frame here. The film formats take four bilinear taps instead.
- **The level buffers are 2 × 1 and never reallocated**, so a resize cannot clear the
  scanner's state. `--resize` checks it.
- **`Perturb` bits are test hooks**, always 0 in the plugin; they exist so
  `--negative` can prove the checks fail.
- **Parameter names must be unique** — `--set` and the sweep find them by name.
- `SetParamInfo` clamps a STANDARD default into 0..1 before `SetParamRange` can widen
  it, so every slider is 0..1 and `Controls.cpp` holds the units, with inverses.
  `FF_TYPE_INTEGER` is exempt: Grain Seed and Frame Number hold their real values.
  Options are mapped by index in `Controls.cpp`.
- Override `SetTextParameter` to return FF_SUCCESS for the About block, or no host can
  instantiate the plugin at all.
- `rebate_core` is an OBJECT library, not STATIC — the plugin registers itself from a
  file-scope constructor nothing references by name. `rebate_model` (no GL) is OBJECT
  too, and every final target names it itself.
- `FFGLScopedFBOBinding.h` is not in the umbrella header; include `<ffglex/FFGLScopedFBOBinding.h>`.
- macOS build must be universal. Verify with `lipo`, never the build log.
- FFGL id is `RB01`, display name `SW Rebate`. OFX identifier `com.stoatworks.rebate`,
  label `Rebate`, bundle id `com.stoatworks.rebate.ofx` — permanent, saved projects
  refer to them, and to the OFX script names (`exposure`, `maskOn`, `processGroup`…).

## Not done yet
- **Never loaded into Resolume on macOS.** Everything numeric is measured offline on
  macOS, plus an `oxbow` load. The Windows CI build of v0.1.0 passed the Arena gate
  9 of 9 on win-lab (Arena 7.27.1, llvmpipe) on 2026-09-24; Grain Seed read
  inconclusive there.
- Seen on the synthetic card and on Resolume's demo clips (the project video, through
  `--pipe`), never on camera footage of people or places.
- No factory presets.
- **The OpenFX build has never been in Resolve, Vegas, Nuke or Natron.** It has met
  only the fleet's command-line OFX host (8-bit and float RGBA, scale 1, no tiles);
  Windows is compiled by CI, Linux is dlopened on Rocky 8 by CI, neither has rendered.
- **The browser demo's CPU half is a port and nothing checks it.** `demo/plugin.js`
  re-implements Model.cpp, Controls.cpp, Frame.cpp and the per-frame arithmetic
  (`render::Prepare`, and the levels' step in `ProcessOpenGL`) in JavaScript. `check_shaders.py` covers the GLSL and the glyph
  table only. Change any of those and change the page by hand to match. The user guide is
  `docs/USER-GUIDE.md`; every claim in it is read from the code, so change both together.
- `StoatworksAbout.h` and `ATTRIBUTIONS.md` are generated by stoatworks-backend's
  `sync-about.py` and `sync-attributions.py`; edit the master lists there, not here.

## Browser demo

`demo/` is the page at **rebate-demo.stoatworks-labs.com**, deployed from
`wrangler.toml` with `cf-run npx wrangler deploy` — no build step; what is committed
is what is served. `demo/vendor/` is the shared kit from
`~/Projects/infrastructure/stoatworks-backend/resolume-demo/` (plus
`support-footer/support-footer.js`) and is not a place to edit. There is no deploy
workflow yet: a push to main does NOT redeploy it.

Verify a deploy **by content, never by status code** — a stale page answers 200:

    curl -s 'https://rebate-demo.stoatworks-labs.com/?cb=1' | grep -o '<title>[^<]*'

## Diagnostics

`source/Diag.{h,cpp}` — log file only, no crash handler (this runs inside Resolume).

    ~/Library/Logs/rebate/rebate.YYYY-MM-DD.log        (macOS)
    %LOCALAPPDATA%\rebate\logs\rebate.YYYY-MM-DD.log   (Windows)
