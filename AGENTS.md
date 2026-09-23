# AGENTS.md — Rebate

Onboarding for whoever (or whatever) picks this up next. `CLAUDE.md` is the short
command reference; this is the *why*. Read "What is actually verified" before you
tell anybody this works.

---

## What the plugin is

Colour negative film, and the scan of it, as an FFGL 2.1 effect (`RB01`, shown as
`SW Rebate`) for Resolume Arena and Avenue. C++17 + GLSL 4.10, CMake, universal
macOS `.bundle` and a Windows `.dll`. MIT, intended home
`github.com/stoatworks-labs/rebate`.

Built 2026-09-23 in one session from the fleet's templates and
`specs/SPEC-rebate.md`: rosette for a subtractive model in densities, plumbicon for
a photochemical response with a tolerance table, pitch for the harness, the
verify script and the negative controls, regauss for grouped controls, tinsel for
`PassBuffer`, the sweep and CI, graticule for the 5x7 font.

---

## The one idea

**A film look is usually a LUT. This is the process.**

The clip is the scene. It exposes three layers of a colour negative; each is
developed along a characteristic curve into dye; the dyes' impurities are
cancelled by the orange mask; the dye is realised as clouds in grain cells; and a
scanner divides out the base, sets its levels and inverts. Every artefact an
operator would call "film" is one of those stages doing what it does:

| the stage | what comes out |
| --- | --- |
| masking couplers: coloured where no dye formed | **the orange negative**; neutral colour after inversion |
| a Poisson draw of dye clouds per grain cell | **grain** with variance c(1 − c)/N: loudest in the mid-tones, gone in clear shadows and blocked highlights |
| age fog as an *exposure*, the top layer taking most | **blue shadows and warm highlights** on expired stock, scanned with the fresh profile |
| a leak as an exposure through the base | a **warm** leak that **saturates** on the shoulder and fogs the rebate too |
| reversal film (no couplers) developed in C-41 | **cross-processing**: steep, no mask, and a scanner dividing by a mask that is not there — warm, most in the shadows |
| unexposed film, a latent edge print, clear holes | **the rebate**: black border, light edge print, black sprocket holes |

The pipeline, in order:

1. **Scene.** sRGB decode to linear, `× 2^Exposure`.
2. **Layer exposure.** A 3×3 spectral crossover (rows sum to 1, so a neutral scene
   exposes the three layers equally), `× speed` per layer, `+ age fog exposure`,
   `+ the leak`, `+ the latent edge print`. Then `x = log10 H`.
3. **Development.** Coverage `c(x) = (sp(x − Toe) − sp(x − Toe − L)) / L`, with
   `sp(u) = ln(1 + e^{ku}) / k` and k = 6 per log unit. `c` is the fraction of the
   layer's developable grains that developed.
4. **Dye.** `a = Fog + γ L c` for a negative (the mirror image for a reversal
   image). The straight line's slope is γ to within `γ(e^{−k dt} + e^{−k ds})`, and
   its extension meets Fog at exactly `x = Toe` and `Fog + γL` at exactly
   `x = Toe + L`: the harness reads those back.
5. **Channel density.** `D = Base + a + U a + Couplers · U (Capacity − a)`, where U
   is the dyes' unwanted absorptions (zero diagonal). The last two sum to
   `U · Capacity` whatever the dyes are — the orange base.
6. **Grain** acts on `c`, in the scan pass: N sites per cell per layer, each
   covered with probability exactly c (at least one cloud of a Poisson draw with
   mean −ln(1 − c)), `c' = c + Amount (K/N − c)`.
7. **Scan.** `T = 10^{−D}`. `View: Negative` shows T on a light box. `View:
   Positive`: manual — subtract the profile's base, one black point and one white
   point for all three channels — or auto — per-channel levels from the frame's
   least and most dense blocks, smoothed over time. Then
   `out = (10^{R p} − 1) / (10^R − 1)`, exactly 0 at p ≤ 0 and 1 at p ≥ 1, with
   `R` the scene log range the levels span (by the profile's γ) times Scanner Gamma.

### What falls out, and what does not

- **Neutral stays neutral** through C-41 with the mask under a manual scan, to
  1e-6 — without any special case. It is the mask's arithmetic.
- **Auto Levels hides a missing mask on neutrals.** An uncancelled impurity on a
  neutral is a per-channel *scale*, and per-channel levels remove scales. That is
  historically true (the mask was invented for printing at fixed filtration) and it
  is why `--mask` runs a manual scan.
- **The grain peak is not placed.** c(1 − c)/N is what a binomial count does.
- **Not modelled:** spectral dye curves (three numbers per dye), interlayer
  effects, adjacency (edge) effects, a real scanner's colour matrix and tone curve,
  halation, gate weave, dust. Grain is per output pixel, so it does not scale with
  the frame. The stocks are invented and described by parameters, not measured.

---

## The shape of the code

| File | What it is |
| --- | --- |
| `source/Model.{h,cpp}` | The film as numbers: curve constants, base, impurities, crossover, ageing, the five stocks, `Develop()` (stock × process × push → curve), `ScannerProfile()`, the `Perturb` hook bits. `Coverage`/`Dye` in double, for the harness to choose inputs with. |
| `source/Controls.{h,cpp}` | What a 0..1 slider means, with inverses. Neutral positions land exactly in binary. |
| `source/Frame.{h,cpp}` | Film geometry in millimetres (gate, perforations, print bands, the centre crop) and the edge-print bitmap. |
| `source/Font.{h,cpp}` | graticule's 5x7 font, unchanged. |
| `source/Shaders.{h,cpp}` | `kModel` (the GLSL library) and the five pass bodies, assembled at run time. |
| `source/PassBuffer.*` | tinsel's FFGLFBO with the leak fixed. |
| `source/Rebate.{h,cpp}` | The plugin: parameters, the clock, buffers, the passes. |
| `source/Diag.{h,cpp}` | A log file, for the shader that will not compile. |
| `tools/rbtest/` | The offline harness: renders, measures, benchmarks, pipes, dumps shaders. |
| `tools/sweep.py` | No control is silently dead. |
| `tools/verify.sh` | All of it, at two rasters, plus the release-time checks done locally. |

Five passes: **copy** (float, no mips) → **film** (exposure and development to mean
coverage, RGBA32F; alpha carries the hole fraction and the picture flag) →
**blocks** (≤ 64×36 mean channel densities of the picture area; Auto Levels only) →
**levels** (2×1 ping-pong: least and most dense block, smoothed) → **scan** (grain,
dyes, mask, transmittance, scanner, mix; straight to the host).

---

## Traps

Roughly in the order they will bite.

### ☠️ glGenerateMipmap on a 4K float picture is 11 ms

The first bench put the defaults at 12.3 ms a frame at 3840×2160 — three quarters
of a 60 fps frame. Grain was the obvious suspect and was innocent (the same figure
with Grain Amount 0). Disabling one pass at a time found it: `GenerateMipmaps()` on
the RGBA32F scene copy, which exists so the film formats can reduce the scene into
the gate. Without it the whole plugin is 1.2 ms. The film formats only ever reduce
by about 1.6×, so four bilinear taps a quarter of the footprint either side of the
pixel centre stand in for the mip level. Do not put the mip chain back without
timing it, and time every pass on its own before optimising the one that looks
expensive.

### ☠️ White is not exactly 1 after the sRGB encode

`--cross` first failed 18 of 23 steps, all of them saturated white. The clip test
asked `max(rgb) >= 1.0`, but `1.055 × pow(1, 1/2.4) − 0.055` in float is 1 − 1 ULP,
so a clipped step read as unclipped and its equal channels as "not R > G > B". The
same bug inflated `--mask`'s unclipped count from 24 to 36 (harmlessly: a clipped
step's spread is zero). Black is exact — `expand()` returns 0 for p ≤ 0 by
construction — white is within 1e-5. `clipped()` in the harness says so, once.

### ☠️ Auto Levels normalises away the things the controls are for

With Auto Levels as the default, Expired 400 looked like Portrait 400, Exposure did
almost nothing, and the cross-process lost its missing-mask shift: per-channel
levels remove per-channel offsets and scales, which is exactly what age fog, a
missing mask and an exposure change are to a first order. That is what a lab does,
and it is not what an operator reaching for Age expects. The default is a manual
scan with the stock's C-41 profile; Auto Levels is there for footage.

### ☠️ Age as a density offset cancels; age as a slow top layer goes yellow

The spec asks for blue-cast shadows on expired stock. Two first attempts could not
give them. Fog as a *density offset* is uniform, so it is in the rebate too and any
base measured from the film cancels it. Speed loss in the blue layer means *less*
yellow dye, which inverts to *yellow*, not blue. What gives blue shadows is fog as
an **exposure** (background radiation and heat are exposures), going through the
curve so it lifts the shadows and not the highlights, scanned against the stock's
*fresh* profile. The first constants let the speed loss dominate (a yellow cast
everywhere); `kAgeFog` and `kAgeSpeedLoss` were then set by eye on the test card so
the fog shows — the direction is structural, the magnitudes are a choice. With
Auto Levels on, the lab takes most of it back out, which is also true to life.

### ☠️ The leak ladder in whole stops put one rung on the straight line

`--leak` asserts each rung's density increment equals γ × the rung's log step
where both ends are at least one log unit inside the bends. The straight line of
this curve is 0.7 log units long by that definition, and whole-stop rungs (0.3 log)
landed only one increment inside it. Half-stop rungs give three. A check that
cannot find the region it tests should fail, and it did (it requires two).

### Implicit derivatives inside a non-uniform branch (caught in review)

The edge print is read inside `if( in the band )`, and the band is not uniform
across a pixel quad; implicit derivatives there are undefined, which on another
driver means garbage mip selection. Caught reading the shader before it ever ran
wrong here: it reads `textureGrad` with the footprint computed from `MmPerPixel`.

### ☠️ Mutation-test only a committed tree

pitch's trap, observed rather than repeated: the mutation below was applied to a
clean, committed tree and reverted with `git checkout`, so nothing uncommitted was
lost with it.

### Inherited from the fleet, and all still true here

`ScopedFBOBinding` does not restore the viewport (the host's is captured first and
put back before the scan pass); every `ffglex::Scoped*` clears to 0 on exit, so every
`Ensure()` and the edge-print upload happen before anything binds a texture;
`FFGLFBO::Release()` leaks the colour texture, which is why `PassBuffer::Destroy()`
deletes it first; `SetParamInfo` clamps a STANDARD default into 0..1 before
`SetParamRange` can widen it; the core is an **OBJECT** library; `SetTextParameter`
must return `FF_SUCCESS` for the About block; the harness drives a synthetic clock;
`nm | grep -q` fails under pipefail when grep succeeds; an option's range reads back
0..1 whatever its element count; Resolume's clock overflows a float, so the grain's
film frame and the levels' smoothing step are reduced in double; the scanner's
levels are primed on the first frame (and whenever Auto Levels is switched on), so
there is no one-pole ramp from zero after a clip trigger; the level buffers are
2 × 1 and are never reallocated on a resize.

---

## Would this hold on another rasteriser, at another raster?

One line per check. Every tolerance is derived, not fitted; every check ran at
320×180 and 1280×720 in `verify.sh`, and at 1920×1080, 640×480 and 333×187 by hand.
Densities are read from the Negative view through a float framebuffer; the read
error `kDensityRead` = 1e-5 is four times the 2.7e-6 that a density summed from six
correctly-rounded terms of magnitude ≤ 4 (1.4e-6), `exp2` (3 ULP, GLSL 4.10 §8.2)
and the sRGB encode's `pow` (≈ 9 ULP, × 2.4 on decode) can account for.

| check | what it measures | tolerance and where it comes from | raster dependence |
| --- | --- | --- | --- |
| `--curve` slope | secant slope of each channel between the first and last wedge steps ≥ 1 log unit inside both bends | **[γ(1 − e^{−k dt} − e^{−k ds}), γ] ± 2·read/span**: the mean-value theorem on the curve's own derivative bounds (0.57893–0.58004 for γ 0.58) | none: the wedge is a float texture and the step centres are sampled, so x is exact at any raster wide enough for 41 steps (≥ ~2 px each) |
| `--curve` toe, shoulder | where the extended secant meets the measured floor and ceiling | the softplus residual `(γ/k) e^{−k m}` at the fit points and at the floor/ceiling steps, plus the slope's error carried over the extrapolation, over the slope: **2.3e-3** log units | none |
| `--push` | ratio of two measured slopes; difference of two measured floors | ratio: the two slope bands combined (**±0.0022** on 1.15); fog: 2 × read + both floors' residual 2.1 log units below the toe (**2.1e-5**) | none |
| `--mask` | max − min of linear output RGB at every unclipped step, manual scan | `R ln10 10^R/(10^R − 1)` × (2 × 1.4e-6 + ½ ULP of the profile base)/(White − Black), + 16 ULP relative (exp2, pow), + 8 ULP absolute (`10^x − 1`). **Not** exact cancellation: the mask and the profile are summed in different orders, and the bound says by how much | none; ≥ 10 steps must be unclipped |
| `--grain` interior | sample variance over all W×H pixels vs `(Dmax − Dmin)² c(1 − c)/N` with c, Dmin, Dmax measured | **5σ of the sample variance's own sampling distribution**, `(μ4 − σ⁴(n−3)/(n−1))/n` with the binomial's μ4; 5.6e-4 at 320×180, 1.4e-4 at 1280×720 | the interval shrinks as √n; it is computed from n, not assumed |
| `--grain` ends | variance at fog and 2.5 log units past the shoulder | a Poisson tail on flipped sites: λ = nN(2.5e-7 + 2^-24 + c̃), where 2.5e-7 is the float resolution of c near 0 or 1 (softplus difference at ULP(5.7)/L) and 2^-24 the draw's threshold; allowed (λ + 5√λ + 5) sites' worth | scales with n, derived |
| `--grain` peak | which interior field has the largest variance | exact: the argmax must be the c = 0.5 field | none |
| `--leak` line | density added per half-stop rung, both ends ≥ 1 log unit inside the bends | **[γ·step(1 − shortfall), γ·step] ± 2 read** — the same derivative bound as `--curve` | the probe is column 0, half a pixel from the edge: its exposure is computed from the raster, so x is exact at any raster |
| `--leak` saturation | increment 1+ log units past the shoulder; total | increment ≤ γ·step·e^{−k(x − shoulder)} + 2 read; total ≤ γL + 2 read | none |
| `--leak` warm | R > G > B at the edge | gaps > 1e-4, three orders above `--mask`'s neutral bound | none |
| `--cross` direction | R > G > B at every unclipped step of a fine wedge | each gap > 16 ULP relative + 8 ULP absolute | ≥ 10 unclipped steps required |
| `--cross` missing mask | (profile_b − profile_r) − (D_b − D_r) from the Negative view, per step | positive at the first step, smaller at the last, never rising by more than 2 read + 4 ULP | none |
| `--cross` no mask, steep | cross orange vs C-41 orange; slopes | ratios (< 0.25; > 1.5×): the model gives 0.04 and 2.1×, so these are coarse by design — they say *which*, not *how much* | none |
| `--rebate` holes | Negative-view transmittance at hole centres | **4 ULP of 1**: `exp2(0)` may be 3 ULP off | hole centres come from the geometry in mm, ≥ 5 px inside a hole at 180 rows |
| `--rebate` border | border density vs unexposed picture density, same frame | **2 read**; measured 0 | none |
| `--rebate` positive | holes and border exactly 0; print > 0.25 | **exact 0**: `expand()` returns 0 for p ≤ 0 by construction, sRGB-encode(0) = 0, and Mix = 1 skips `mix()`. Print: 0.85 at 180 rows, 1.0 at 720 — sub-pixel glyphs at 180 average down, which is why the floor is a quarter | the print band's rows are chosen from the geometry at each raster |
| `--seed` | bit equality; fraction differing | **exact**: a pure function of integers; > 50% differ for another seed or frame (93% measured) | the cross-raster comparison is part of the check: cells are counted from the top-left pixel |
| `--resize` | fraction of the way the levels moved in one frame after a resize | **1e-4** on α = 0.0645: each component is prev + (meas − prev)α, a few ULP of values ≤ 3 over a gap required to be ≥ 0.05 | the check resizes to twice whatever raster it is given |

Deliberately NOT relied on: exact cancellation of the mask against the profile
(`--mask` bounds the difference), `pow(1, x) == 1` (white is tested within 1e-5,
not for equality), `mix(a, b, 1) == b` (the scan returns early at Mix 1), and
implicit derivatives in non-uniform control flow (`textureGrad`).

What might still differ on another rasteriser: the 4-tap scene reduction and the
edge print's mip selection are filtered, so a software rasteriser's `texture()` may
round differently — no check reads the scene through a film format except
`--rebate`, which uses a black scene and a lower bound on the print. The grain hash
is integer arithmetic and must agree bit for bit on any conforming driver.

### The negative controls

`rbtest --negative` runs eleven, and `--perturb BITS` runs any check verbosely
against one. Each perturbs the *plugin's* model — a `Perturb` bit the shipped
plugin carries at zero, or a real control — never the harness's expectation.

| perturbation | what fails, measured at 320×180 |
| --- | --- |
| γ × 0.9 in the shader | `--curve`: slope 0.5215 against a band of 0.5789–0.5800 |
| push raising γ at half the stated rate | `--push`: γ × 1.075 against 1.150 ± 0.002 |
| Mask On off (the spec's) | `--mask` |
| couplers never consumed (the mask present, cancelling nothing) | `--mask` |
| grain variance ∝ c instead of c(1 − c) | `--grain`: variance 5.4e-2 at c = 0.7 against 1.6e-2; the peak moves to c = 0.85 |
| the leak added as coverage after the curve | `--leak`: straight-line increments wrong, total 2.26 > γL = 1.57 |
| the leak's spectrum reversed | `--leak` warm: R 0.17 < B 0.69 |
| the scanner expecting no mask under C-41 | `--cross`: 16 of 16 unclipped steps fail R > G > B (the perturbed model puts blue highest) |
| opaque sprocket holes | `--rebate`: holes transmit 0.07 |
| the seed left out of the hash | `--seed`: another seed differs in 0 values |
| a resize that re-primes the levels | `--resize`: the levels move 1.0 of the way, not 0.0645 |

### The mutation

One character of the shipped GLSL, on a clean committed tree: in `channelDensity`,
`vec3( Capacity ) - a` → `vec3( Capacity ) + a` (the mask growing with the dye
instead of being consumed by it). Caught by `--curve`, `--push`, `--mask`,
`--grain`, `--leak` and `--rebate`. `--cross` passed, correctly — reversal film has
no couplers, so the line never runs — and so did `--seed` and `--resize`, which do
not depend on the dyes. Reverted with `git checkout source/Shaders.cpp`; the tree was
clean before and after.

---

## Decisions taken without asking

- **Manual scan by default** (see the trap). Black Point 0.05 and White Point 1.25
  density above the profile's base: seven stops of scene between them at γ 0.58.
- **The scanner's manual profile is the stock's fresh C-41 profile, masked,
  whatever the film really is.** That is what makes a cross-process scan warm and an
  expired roll's shadows blue, and it is what a lab does when it runs a roll as
  C-41. Under E-6 the profile is an unmasked slide's.
- **Process × stock.** The emulsion is reversal if the stock is (Slide 100) or the
  Process is Cross; the chemistry is E-6 or C-41. So Slide 100 in C-41 *is*
  cross-processing (identical to Cross), a negative stock in E-6 is a flat positive
  that keeps its orange mask (reverse cross-processing), and Mask On only matters
  for a negative emulsion.
- **The spec's "cyan/yellow shift".** The model gives a negative with no orange
  (cool against the orange the scanner expects) and a positive that goes warm, most
  in the shadows. `--cross` checks the direction the model predicts; this reading
  of the spec's phrase is mine.
- **Push is development only.** γ × (1 + 0.15 p) and fog + 0.03 p; the camera's
  under-exposure that usually goes with a push is `Exposure`'s job.
- **Grain changes at 24 fps of host time** (film runs at 24), from `floor(now × 24)`
  in double. Cells are output pixels, counted from the top-left.
- **Grain Amount scales the physical deviation**, 1 being the full binomial
  fluctuation; the default 0.35 because the physical figure at N = 32 is heavier
  than most operators want.
- **Auto levels** are the least and most dense 64×36 block means of the picture area
  (not pixels, so a specular highlight does not set white), smoothed with τ =
  0.25 s, primed on the first frame and whenever Auto Levels is switched on, not on
  a resize.
- **The leak** runs from 4 stops under to 10 stops over mid grey at its edge, falls
  off exponentially over Leak Spread frame heights, with a gentle lateral bump
  centred on the edge; Warmth 1 is (1, 0.30, 0.06) on the red, green and blue layers.
- **The rebate:** the film's width fills the output's height; one frame, the rest of
  the strip unexposed; the scene centre-cropped to the gate. 35 mm has the common
  negative perforation and two print lines; 6x6 has a stock code and no frame numbers
  (120 carries them on the backing paper).
- **Output alpha is 1** where the scan is shown: film is opaque.
- **No factory presets.** The Stock menu is the preset list the spec asked for,
  described by parameters in `Model.cpp`.
- **Test hooks live in the shipped plugin** (the `Perturb` bits, in the shader and in
  `Develop`/`ScannerProfile`), always zero: the negative controls must perturb the
  plugin's model, not the harness's expectation.
- **Shaders are assembled at run time** from one model library, and verify.sh
  compiles what `rbtest --dump-shaders` writes, which is the plugin's own strings.

---

## What is actually verified, and what is assumed

### Verified by measurement, on an M4 Max running macOS 26.4.1 (2026-09-23)

Every number is `tools/verify.sh` on this machine against a fresh universal Release
build, at 320×180 and 1280×720, with the same checks passing at 1920×1080, 640×480
and 333×187 by hand.

- **Curve.** For Fine 100, Portrait 400 and Grain 800, all three channels: slope
  0.61945 / 0.57948 / 0.59946 against γ 0.62 / 0.58 / 0.60, inside the derived
  bands; toe and shoulder 1.2e-3 log units from where the curve says, tolerance 2.3e-3.
- **Push.** γ × 1.15000 and fog +0.030000 in every channel.
- **Mask.** Worst chroma 1.1e-6 over 24 unclipped steps, 0.08 of its bound.
- **Grain.** Every field in every channel inside its 5σ interval (e.g. c = 0.5:
  1.9158e-2 against 1.9159e-2 ± 1.4e-4 at 921,600 samples); the ends ~1e-21; the
  peak at c = 0.5.
- **Leak.** 0.08723 per half-stop on the straight line against γ·log₁₀√2 = 0.08732
  (band from 0.08718); 1e-5 by 1.6 log units past the shoulder; total 1.5660 = γL;
  R 0.69 > G 0.30 > B 0.17 at the edge.
- **Cross.** 22 of 22 unclipped steps R > G > B; missing mask 0.769 → 0.003,
  never rising; orange 0.033 against 0.802; slope 1.231 against 0.580.
- **Rebate.** Holes 1 − 1.4e-7; border = unexposed picture exactly; positive holes
  and border exactly 0; print 0.85 (180 rows) / 1.00 (720 rows); print off, exactly 0.
- **Seed.** Bit-identical repeats; 93% differ for another seed or frame; 320×180
  bit-identical to the corner of 640×360 and 1280×720.
- **Resize.** 0.064493 of the way against α = 0.064493, all six level components.
- **Negative controls.** All eleven fail their check.
- **Mutation.** Caught by six checks (above).
- **No dead controls**, all 22, with the four About buttons skipped.
- **Every shader compiles** through `glslc`, as the plugin assembles it.
- **`--pipe`** returns exactly two frames for two and a half, and refuses an unknown cue.
- **The bundle** is universal, exports `_plugMain`, carries `com.stoatworks.ffgl.rebate`,
  ad-hoc signs, and `oxbow` reports `SW Rebate` / `RB01` / `effect` and renders 120
  frames through `plugMain`.
- **Render cost** at the defaults, best of three runs of 60 frames after a warm-up,
  `glFinish` both sides, on a shared GPU:

  | | ms/frame | % of a 60fps frame |
  | --- | --- | --- |
  | 1280×720 | 0.20 | 1.2% |
  | 1920×1080 | 0.33 | 2.0% |
  | 2560×1440 | 0.54 | 3.2% |
  | 3840×2160 | 1.19 | 7.2% |

  Auto Levels adds the block reduction: 1.85 ms at 4K in a separate run of the dev
  build. Before the mip-chain trap was found the 4K figure was 12.3 ms.

### Assumed, or not done

- ☠️ **Never loaded into Resolume**, on either platform. Everything was compiled,
  rendered and measured offline against the real plugin class in a headless CGL
  context, plus an `oxbow` load.
- **Never seen on footage.** Every picture so far is the synthetic test card. The
  look of skin, foliage and night exteriors through this model is unjudged, and the
  constants (crossover, impurities, stock γs, age fog, grain N) were chosen by
  reasoning and by eye on the card, not fitted to any real stock.
- **The clock-unit voting** is readout's, which has met Arena; this plugin has not.
- **The Windows build is CI-only** and CI cannot run yet.
- **Not verified at 4K**, only benchmarked there.
- **The film formats' scene reduction** (four bilinear taps) is unmeasured beyond the
  sweep; at reductions over 2× it would alias. The formats reduce by at most ~1.6×.
- **E-6 through a manual scan** uses the same Black/White Point controls, which are
  scaled for a negative's density range; Auto Levels suits E-6 better.
- **No OpenFX port and no browser demo.** Not required for 0.1.0.
- **`StoatworksAbout.h` and `ATTRIBUTIONS.md` are provisional hand copies** with
  `guide=""`, in the shape the fleet's syncs generate; register the project and
  re-run the syncs before the first release. The About facts were chosen so the
  button count — and so the parameter count — does not change when regenerated.
- **Nothing has been through a show.**

---

## Open questions

- **Should grain be in film units?** Per output pixel it is raster-exact and cheap,
  but a 35 mm frame at 720p and at 4K then has different grain relative to the
  picture. Film-unit cells would need averaging below a pixel, which changes the
  variance law the check measures (by the number of cells per pixel — also closed
  form).
- **Should Auto Levels clip a percentage** rather than take block extremes? Block
  means are already robust to a specular pixel; a percentile would be more like a
  lab scanner and costs a histogram.
- **Is the manual default right for a VJ?** It makes every control honest, but a
  badly exposed clip stays badly exposed. A factory preset with Auto Levels on
  might be the friendlier way in.
- **The spec's cyan/yellow wording** — see the decision above.
- **Neighbouring frames on the strip** are unexposed; showing the previous and next
  frame (the same clip, a frame apart) would look more like a contact strip.

---

## Siblings

- **pitch** — the harness, verify and CI shape, the negative controls, the
  `--pipe` contract (and its later fix for a failed render or a closed stdout).
- **rosette** — a subtractive model done in densities, multiplicative inks.
- **plumbicon** — a photochemical-style response with a tolerance table per check.
- **regauss** — grouped controls.
- **tinsel** — `PassBuffer`, `sweep.py`, and the fleet's trap list.
- **graticule** — the 5x7 font, used here for the edge print.
- **oxbow** — `oxbow probe` and `oxbow selftest` are what load this bundle as a host.
