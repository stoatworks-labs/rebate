# rebate

> **AI-assisted project.** This codebase was created with [Claude](https://claude.com/claude-code)
> (Anthropic), directed and reviewed by a human author. The film is not asserted
> but measured: an offline harness drives the real plugin class in a headless GL
> context and reads each claim back out of the picture — a grey wedge's density
> climbs with slope γ and bends where the curve says, a neutral wedge through C-41
> with its orange mask inverts to neutral to 1e-6, grain's variance is
> c(1 − c)/N and peaks at c = 0.5 over 921,600 samples a field, one stop of push
> raises γ × 1.15 and fog by 0.03, a light leak rises and saturates on the
> shoulder and is warm, cross-processed film scans warm where the model says, the
> sprocket holes are black — with eleven negative controls that prove each check
> can fail. It has **never been loaded into Resolume on macOS**, where it is loaded by
> [oxbow](https://github.com/stoatworks-labs/oxbow), which is a real FFGL host and
> is not Resolume. On Windows, a build of v0.1.0 loads, registers and renders in
> Resolume Arena 7.27.1 with every control as declared, on software rendering. See
> [Status](#status).

Colour negative film, and the scan of it, as an FFGL effect for
[Resolume](https://resolume.com) Arena and Avenue.

![A test card on a strip of 35 mm colour negative, scanned and inverted: fine grain, a warm light leak burning in from the right edge across the rebate, sprocket holes, and edge print reading PT400, 11A, >12 and 12A](docs/hero.png)

<sub>One frame, rendered by `rbtest`, the offline harness — not captured from
Resolume. The 400-speed portrait negative on a 35 mm strip, with a leak.</sub>

<!-- downloads:start -->

## Download

**[v0.1.0](https://github.com/stoatworks-labs/rebate/releases/tag/v0.1.0)** — prebuilt for macOS and Windows. Pick your platform:

<details>
<summary><b>macOS</b> — Universal (Apple Silicon + Intel)</summary>

| Build | Download | Size |
| --- | --- | --- |
| Universal (Apple Silicon + Intel) · .dmg disk image | [`rebate-0.1.0-macos-universal.dmg`](https://github.com/stoatworks-labs/rebate/releases/download/v0.1.0/rebate-0.1.0-macos-universal.dmg) | 230 KB |
| Universal (Apple Silicon + Intel) · .zip archive | [`rebate-macos-universal.zip`](https://github.com/stoatworks-labs/rebate/releases/latest/download/rebate-macos-universal.zip) | 191 KB |

</details>

<details>
<summary><b>Windows</b> — x64</summary>

| Build | Download | Size |
| --- | --- | --- |
| x64 · .exe installer | [`rebate-0.1.0-windows-x86_64-setup.exe`](https://github.com/stoatworks-labs/rebate/releases/download/v0.1.0/rebate-0.1.0-windows-x86_64-setup.exe) | 226 KB |
| x64 · .zip archive | [`rebate-windows-x86_64.zip`](https://github.com/stoatworks-labs/rebate/releases/latest/download/rebate-windows-x86_64.zip) | 118 KB |

</details>

All builds, checksums and release notes: [github.com/stoatworks-labs/rebate/releases](https://github.com/stoatworks-labs/rebate/releases).

macOS builds are signed and notarised and open normally. The Windows builds are unsigned, so SmartScreen warns once.

<!-- downloads:end -->

## The one idea

A film look is usually a lookup table. This is the process.

The clip is the **scene**. It exposes the three dye layers of a colour negative
through the overlap of their spectral sensitivities. Each layer is developed along
a characteristic curve — a toe, a straight line of slope γ, a shoulder — into dye.
The dyes are impure, so the film carries masking couplers that cancel their
impurities, which is why a negative is orange. The dye is made of clouds in grain
cells. Then the flat orange negative is **scanned and inverted**, the way a lab
scanner does it: divide by the base, set the levels, invert.

![The same card as the negative itself on a light box: an orange strip with clear sprocket holes, dark edge print, and the picture inverted and orange](docs/negative.png)

<sub>`View: Negative` — what is on the film before the scanner inverts it.</sub>

## What falls out

None of these is drawn. Each is one stage of the process doing what it does:

- **The orange negative.** The mask: coloured coupler wherever dye did not form,
  carrying exactly the unwanted absorption that dye would have had. `View: Negative`
  shows it; the scanner divides it out.
- **Grain in the mid-tones.** Each cell holds N sites a dye cloud may land on, so
  the covered fraction's variance is c(1 − c)/N: largest at mid density, zero in
  clear shadows and blocked highlights. Nothing is tuned to put it there. `Stock`
  sets N, `Grain Size` the cell.
- **Blue shadows on expired stock.** Age fog is an exposure (radiation and heat in
  storage), it goes through the curve so it lifts the shadows and not the
  highlights, and the top, blue-sensitive layer takes the most of it. Scanned with
  the stock's normal profile, the blacks go blue and the highlights warm.
  `Auto Levels` — the lab re-measuring the roll — takes most of it back out.
- **The warm light leak.** Light through the base reaches the red-sensitive layer
  first. It is an exposure, so it saturates on the shoulder instead of adding
  without limit, and it fogs the rebate along with the picture.
- **Cross-processing.** Reversal film through C-41: no masking couplers, a steep
  curve, and a scanner that divides by an orange mask that is not there. The
  positive goes warm, most in the shadows, and the shift shrinks toward the
  highlights where the mask it expected would have been used up.
- **The rebate.** Unexposed film around the frame, which prints black; the edge
  print, pre-exposed at manufacture as a latent image, which prints light on it;
  the sprocket holes, clear, which clip to black. The stock codes and frame numbers
  are invented. No real manufacturer's name or mark appears anywhere.

### The honest limit

The model is three layers with one curve shape between them, a 3×3 spectral
crossover and a 3×3 dye-impurity matrix: enough for every mechanism above, not a
spectral simulation of any real stock. The stocks are invented and described by
their parameters (γ, grain sites, fog, age), not measured from anything. Grain is
per output pixel rather than per micron of film, so it does not scale with the
frame. Real development has adjacency effects, interlayer inhibition and a
coupler chemistry this does not model, and a real lab scanner has a colour
matrix and a tone curve of its own that here are one levels step and one gamma.

## Controls

| Group | |
| --- | --- |
| **Scene** | Exposure (±6 stops), Stock (Fine 100, Portrait 400, Grain 800, Slide 100, Expired 400), Push (−1 to +3 stops of development), Age. |
| **Process** | Process (C-41, E-6, Cross), Mask On. |
| **Grain** | Grain Amount (0 to the full physical fluctuation), Grain Size (1–8 px cells), Grain Seed. |
| **Leak** | Leak Amount, Leak Edge (Left, Right, Top, Bottom), Leak Warmth, Leak Spread. |
| **Scan** | View (Positive or Negative), Auto Levels, Black Point, White Point (densities above the base), Scanner Gamma. |
| **Frame** | Format (Full, 6x6, 35 mm), Edge Text On, Frame Number, Mix. |

The defaults are the Portrait 400, normally exposed and developed, grain at about
a third of its physical fluctuation, scanned **manually** with the lab's C-41
profile, on a 35 mm strip. Manual rather than auto so that Exposure, Age and Cross
do what they say out of the box; turn on Auto Levels for footage that needs the
lab's help.

## Status

**v0.1.0, and honestly early — 23 September 2026.**

### Measured offline, on macOS

`tools/verify.sh` passes on this machine (M4 Max, macOS 26.4.1) against a fresh
universal Release build, running every check at **two rasters**, 320×180 and
1280×720; the same checks also pass at 1920×1080, 640×480 and 333×187. What it
establishes, in numbers:

| check | result |
| --- | --- |
| `--curve` | for three negative stocks and all three channels, the straight line's slope reads back as **0.57948** for γ = 0.58 (band 0.57893–0.58004, from the curve's own shortfall inside its bends); the extended line meets fog **1.2e-3** log units from the toe and Dmax 1.2e-3 from the shoulder, against a derived 2.3e-3 |
| `--push` | +1 stop reads back γ × **1.15000** (stated 1.15, band ±0.0022) and fog **+0.030000** (stated +0.03, tolerance 2.1e-5) |
| `--mask` | a neutral wedge through C-41 with the mask, manually scanned: worst chroma **1.1e-6** in linear output RGB over 24 unclipped steps, **0.08** of a bound derived from float arithmetic |
| `--grain` | variance against c(1 − c)/N at seven densities: at c = 0.5, **1.9158e-2** against **1.9159e-2** (5σ interval ±1.4e-4, 921,600 samples); zero to **1e-21** at fog and at Dmax; the peak is at c = 0.5 in every channel |
| `--leak` | each half-stop of leak adds γ·log₁₀√2 = **0.0872** of density on the straight line, falling to **0.00001** 1.6 log units past the shoulder; the total never exceeds γ × latitude; at the edge the positive reads R 0.69 > G 0.30 > B 0.17 |
| `--cross` | every one of 22 unclipped steps scans R > G > B; the missing mask falls from **0.769** density unexposed to **0.003** at Dmax and never rises; no orange (0.033 against C-41's 0.802); γ 1.23 against 0.58 |
| `--rebate` | the holes transmit 1 − 1.4e-7; the border's density equals the unexposed picture's **exactly**; in the positive the holes and the border are **exactly 0** and the edge print reaches 0.85–1.0 |
| `--seed` | the same seed and frame twice: **0** differing values; another seed or the next film frame: 93% differ; the 320×180 render is bit-identical to the top-left of a 1280×720 one |
| `--resize` | across a resize to twice the raster the scanner's levels move exactly one smoothing step: **0.064493** of the way, α = 0.064493 |
| `--negative` | eleven perturbed models — γ × 0.9, push at half rate, Mask Off, couplers never consumed, grain ∝ density, a leak added after the curve, a cool leak, a scanner expecting no mask, opaque holes, the seed left out of the hash, a resize that re-primes — each **fails** its check |
| mutation | one character of the shipped GLSL (`Capacity ) - a` → `Capacity ) + a` in the mask) was caught by `--curve`, `--push`, `--mask`, `--grain`, `--leak` and `--rebate`, then reverted |
| `tools/sweep.py` | all **22** controls measurably change the picture |
| shaders | all 6, as the plugin assembles them, compile through `glslc` |
| `--pipe` | 2.5 frames in, exactly 2 out; an unknown cue refused |
| the bundle | universal (`x86_64 arm64`), exports `plugMain`, ad-hoc signs; `oxbow` reports `SW Rebate` / `RB01` / `effect` and renders 120 frames through `plugMain` |

Render cost at the defaults, best of three runs of 60 frames after a warm-up,
`glFinish` both sides, on a GPU shared with other work: **0.20 ms** at 720p,
**0.33 ms** at 1080p, **0.54 ms** at 1440p, **1.19 ms** at 4K — under a tenth of
a 60 fps frame. Auto Levels adds its block reduction, about 0.6 ms at 4K. macOS
figures only.

### In Resolume, on Windows

**Resolume Arena 7.27.1** (win-lab, Mesa llvmpipe, no GPU, 2026-09-24): a CI build of
v0.1.0 loads from Extra Effects, registers as `SW Rebate` / `RB01` / effect, all 28 host
controls match the declaration in name, order, type, range and default, it renders, and
Arena's log stays clean: 9 of 9 of the fleet gate's checks. 22 controls moved the
picture, 7 of them under a precondition; Grain Seed read inconclusive. Software
rendering says nothing about a GPU or about speed.

### Not established

It has **never been loaded into Resolume on macOS**. Everything above the Windows
section was compiled, rendered and measured offline against the real plugin class in a
headless CGL context, plus an `oxbow` load. How 22 controls in six groups present
in Arena's inspector on a Mac, whether the stock list reads well, and what the host's clock
does to the grain's film frame over a long session are untested. The look has been
seen on a synthetic test card and on Resolume's bundled demo clips through
`rbtest --pipe` (for the project video), never on camera footage of people or
places. No OpenFX port, not in
scope for 0.1.0. The [browser demo](https://rebate-demo.stoatworks-labs.com/)
runs the plugin's own five shaders in WebGL2, but its CPU half — the stocks, the
development, the scanner's profile and the film strip — is a hand port to
JavaScript, and nothing checks a port but a reader.

The [user guide](docs/USER-GUIDE.md) covers every control, what it does and why.

## Build

Needs CMake 3.15+, a C++17 compiler, and the FFGL SDK submodule.

```bash
git clone --recursive https://github.com/stoatworks-labs/rebate
cd rebate
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
cmake --install build     # into ~/Documents/Resolume Arena/Extra Effects
```

macOS builds are universal (Apple Silicon + Intel) by default; add
`-DCMAKE_OSX_ARCHITECTURES=arm64` for a faster dev build. Windows needs GLEW via vcpkg.

## Building and testing

The offline harness renders the real plugin class headlessly, on a synthetic 60 fps
clock:

```bash
./build/rbtest --out /tmp/frame.png --size 1920x1080   # the test card
./build/rbtest --list                                  # every control, kind and default
./build/rbtest --curve --push --mask --grain           # each claim, measured
./build/rbtest --leak --cross --rebate --seed --resize
./build/rbtest --negative                              # and the checks can fail
./build/rbtest --bench                                 # 720p through 4K
python3 tools/sweep.py                                 # no control is silently dead
tools/verify.sh                                        # all of it, on a fresh universal build
```

Every check takes `--size`; run it at 320×180 as well as the raster you care about.
Footage goes through the real shaders with `--pipe`, in the fleet's frame format:

```bash
ffmpeg -i in.mov -f rawvideo -pix_fmt rgba - \
  | ./build/rbtest --pipe --size 1920x1080 --script cues.txt \
  | ffmpeg -f rawvideo -pix_fmt rgba -s 1920x1080 -i - out.mov
```

See [`CLAUDE.md`](CLAUDE.md) for the full command reference and
[`AGENTS.md`](AGENTS.md) for the model and the traps.

<!-- attributions:start -->
This project is built on other people's work — see [ATTRIBUTIONS.md](ATTRIBUTIONS.md).
<!-- attributions:end -->

## Licence

MIT — see [LICENSE](LICENSE).
