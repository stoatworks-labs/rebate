#pragma once

#include <string>

/**
	The five passes.

	1. **copy** -- the host's input into an RGBA32F texture of ours, with a
	   mip chain. A float copy so a float input (the harness's HDR wedge)
	   survives it exactly; mipmapped because the film formats crop and scale
	   the scene into a gate.

	2. **film** -- picture size, RGBA32F. The exposure and the development:
	   where the pixel is on the strip (gate, rebate, sprocket hole, edge
	   print), the scene's light through the spectral crossover into the three
	   layers, plus age fog, the latent edge print and the light leak, then
	   log H through the characteristic curve to each layer's MEAN coverage c.
	   `a` carries the hole fraction and whether the pixel is in the picture.

	3. **blocks** -- 64 x 36 at most. The mean channel density of the picture
	   area of each block of the film, from the mean coverage: what a lab
	   scanner's auto exposure looks at.

	4. **levels** -- 2 x 1. The darkest and densest block, smoothed over
	   time against the previous frame's levels (ping-ponged; primed on the
	   first frame, never cleared on a resize).

	5. **scan** -- to the host. Grain (a seeded Poisson draw of dye clouds
	   per cell per layer), the dyes, the orange mask, transmittance; then
	   either the negative on a light box (View: Negative) or the scanner's
	   base, levels and inversion (View: Positive), and the mix.

	The model -- the curve, the dyes, the mask -- is one GLSL library, `kModel`
	in Shaders.cpp, compiled into the film, blocks and scan passes. Each
	shader is assembled at run time from it, so `rbtest --dump-shaders DIR`
	writes out exactly the strings the plugin compiles, and that is what
	`tools/verify.sh` hands to glslc.
*/
namespace rebate::shaders
{

std::string Vertex();
std::string Copy();
std::string Film();
std::string Blocks();
std::string Levels();
std::string Scan();

} // namespace rebate::shaders
