#pragma once

#include "Controls.h"
#include "Frame.h"
#include "Model.h"

#include <functional>
#include <vector>

/**
	Rebate with no GL in it: the per-frame arithmetic both builds share, and a
	CPU copy of the per-pixel passes for the OpenFX build.

	------------------------------------------------------------- shared, once

	`HostValues` is every control as the FFGL host holds it -- the 0..1 sliders
	Controls.h converts, option indices, booleans as 0 or 1, Grain Seed and
	Frame Number as themselves -- and its member defaults ARE the plugin's
	defaults. The FFGL constructor and the OpenFX describe both read them, so
	the two inspectors open on the same film.

	`Prepare` is the per-frame arithmetic: the stock, the curve it develops to,
	the scanner's profile, speed and age fog per layer, the edge print's
	exposure, the leak's spectrum, the grain's film frame. Everything the
	passes are handed. `Rebate::ProcessOpenGL` uploads what it returns as
	uniforms; the OpenFX build hands it to the passes below. There is one copy.

	------------------------------------------------------------------ mirrored

	`Film`, `MeasureLevels` and `Scan` are the film, blocks + levels and scan
	passes of `Shaders.cpp`, on the CPU, in float, line for line -- the
	`Perturb` hooks included, so the copy reads against the GLSL without a
	gap. Each mirrored function carries a `//= mirrored` line naming the GLSL
	it copies, and `Shaders.cpp` carries one beside each pass naming this
	file. **Edit both.** `rbtest --cpu` renders the same frames through both
	and fails when they part.

	The CPU cannot be the GPU bit for bit, in two known places:

	  - **Texture filtering.** Bilinear here is exact float; a GPU's filter
	    weights are fixed point -- 8 fractional bits, rounded, on the Apple
	    GPU this was measured on, which `Uniforms::filterBits` reproduces for
	    the harness. Only the film formats' four-tap scene reduction and the
	    edge print are filtered, so only their hard edges differ; Full reads
	    its texel exactly, as the GPU does.
	  - **Transcendentals.** exp, log, pow and exp2 differ by a few ULP
	    between a C library and a GPU. That is far below an 8-bit step, but a
	    grain site's threshold is `uint( c x 2^24 )`, so a c one ULP apart can
	    flip one site in a few million: a single grain cell a fraction of a
	    step brighter or darker.

	------------------------------------------------- what OpenFX does not get

	**The scanner's smoothing.** The FFGL build's Auto Levels follow the
	frame's block extremes with a 0.25 s one-pole filter, which needs the
	previous frame's levels -- and an OpenFX host renders frames out of order,
	alone and concurrently. `MeasureLevels` is the measurement alone: what the
	FFGL build does on the frame it primes on. Each frame is levelled on its
	own.

	Rows are **bottom-up** throughout, as they are in GL and in OpenFX: row 0
	is the bottom of the picture, and the film's top-down coordinates are
	worked out from it exactly as the shaders work them out from
	gl_FragCoord.
*/
namespace rebate::render
{

/// Film runs at 24 frames a second, so the grain changes 24 times a second
/// of host or timeline time whatever the frame rate.
constexpr double kFilmRate = 24.0;

/// Warmth 1: the leak's spectrum on the red, green and blue layers. A leak
/// through the base reaches the red-sensitive layer (the bottom one) first,
/// and what gets through to the top is what the lower layers passed.
constexpr float kWarmLeak[ 3 ] = { 1.0f, 0.30f, 0.06f };

/// The block grid the scanner's auto levels look at, at most.
constexpr int kGridW = 64;
constexpr int kGridH = 36;

/// Every control, as the FFGL host holds it. The member defaults are the
/// plugin's: a 400-speed portrait negative, normally exposed and developed,
/// grain at a third of its physical fluctuation, no leak, scanned manually
/// with the lab's C-41 profile, on a 35 mm strip with its rebate. Manual
/// rather than Auto Levels by default, so that every control does what it
/// says out of the box: auto levels normalise away exposure, age fog and the
/// missing mask of a cross-process, which is what a lab does and not what an
/// operator reaching for Age expects.
struct HostValues
{
	//Scene
	float exposure = controls::ExposureParam( 0.0f );
	float stock    = 1.0f;//Portrait 400
	float push     = controls::PushParam( 0.0f );
	float age      = 0.0f;

	//Process
	float process = 0.0f;//C-41
	float maskOn  = 1.0f;

	//Grain
	float grainAmount = 0.35f;
	float grainSize   = controls::GrainSizeParam( 1.5f );
	float grainSeed   = 1.0f;

	//Leak
	float leakAmount = 0.0f;
	float leakEdge   = 1.0f;//Right
	float leakWarmth = 0.8f;
	float leakSpread = controls::LeakSpreadParam( 0.3f );

	//Scan
	float view         = 0.0f;//Positive
	float autoLevels   = 0.0f;
	float blackPoint   = controls::BlackPointParam( 0.05f );
	float whitePoint   = controls::WhitePointParam( 1.25f );
	float scannerGamma = controls::ScannerGammaParam( 1.0f );

	//Frame
	float format      = 2.0f;//35 mm
	float edgeText    = 1.0f;
	float frameNumber = 12.0f;
	float mix         = 1.0f;
};

/// What one frame's passes are handed: every uniform, before it is cast to
/// float for the upload.
struct Uniforms
{
	int width   = 0;
	int height  = 0;
	int perturb = 0;

	const model::Stock* stock = nullptr;
	model::Curve curve;
	model::Profile profile;
	bool invert = true;

	float speed[ 3 ]       = { 1.0f, 1.0f, 1.0f };
	float fogExposure[ 3 ] = { 0.0f, 0.0f, 0.0f };
	float textExposure     = 0.0f;

	float grainAmount = 0.0f;
	float grainCell   = 1.0f;
	int grainSites    = 1;
	int seed          = 0;
	int grainFrame    = 0;

	double leakExposure    = 0.0;
	int leakEdge           = 0;
	float leakSpread       = 1.0f;
	float leakWeights[ 3 ] = { 1.0f, 1.0f, 1.0f };

	int view          = 0;
	bool autoLevels   = false;
	bool autoActive   = false;///< Auto Levels on AND looked at (View: Positive)
	float blackPoint  = 0.0f;
	float whitePoint  = 1.0f;
	float scanGamma   = 1.0f;

	int format      = 0;
	bool edgeText   = true;
	int frameNumber = 0;
	float mix       = 1.0f;

	frame::Geometry geometry;
	int gridW = 1;
	int gridH = 1;

	/// Test hook for the CPU passes, 0 outside the harness: quantise bilinear
	/// weights to this many fractional bits, as a GPU's fixed-point texture
	/// filter does -- rounded when positive, truncated when negative (Apple's
	/// GPUs round to 8). 0 is exact float, which is what the OpenFX build
	/// renders with: the better answer, and not the GPU's. `rbtest --cpu`
	/// measures the GPU's filter, compares the arithmetic with this set to
	/// match, and reports what is left at 0.
	int filterBits = 0;
};

/// The per-frame arithmetic, for a raster of `width` x `height` at `seconds`
/// of host (FFGL) or timeline (OpenFX) time. `perturb` is the test hook,
/// always 0 outside the harness.
Uniforms Prepare( const HostValues& values, int width, int height, double seconds, int perturb = 0 );

//---------------------------------------------------------------------------
// The CPU passes. Images are RGBA float, tightly packed, rows bottom-up.
//---------------------------------------------------------------------------

/// The edge print as the GPU samples it: `frame::BuildText`'s strip as level
/// 0 of a mip chain, each level a 2 x 2 box of the one above rounded to 8
/// bits, as an R8 texture's glGenerateMipmap gives. The chain only matters
/// below about 245 rows (35 mm) or 430 (6x6); above that one output pixel
/// is less than a glyph pixel and the GPU magnifies level 0.
struct TextMips
{
	std::vector< std::vector< float > > level;///< 0..1, row 0 first
	std::vector< int > width;
	std::vector< int > height;
};
TextMips BuildText( const Uniforms& u );

/// Pass 2, the film: exposure and development to mean coverage, for rows
/// [y0, y1). `picture` is the scene (what the FFGL copy pass leaves in its
/// float buffer: the input, untouched); `film` receives coverage in rgb and
/// the hole fraction + 2 inside the picture in alpha, as the GPU's RGBA32F
/// film buffer holds them.
void Film( const Uniforms& u, const float* picture, const TextMips& text, float* film, int y0, int y1 );

/// Passes 3 and 4 without the smoothing: the least and most dense block of
/// the picture area, in channel density. `measured` is false when no block
/// held any picture, and the levels are then zero.
struct Levels
{
	float least[ 3 ] = { 0.0f, 0.0f, 0.0f };
	float most[ 3 ]  = { 0.0f, 0.0f, 0.0f };
	bool measured    = false;
};
Levels MeasureLevels( const Uniforms& u, const float* film );

/// Pass 5, the scan, for rows [y0, y1). `source` is the input (for Mix),
/// `out` receives the scanned picture. `out` may be `source`: each pixel is
/// read before it is written and nothing else is.
void Scan( const Uniforms& u, const float* film, const Levels& levels, const float* source, float* out, int y0, int y1 );

/// Run `body( y0, y1 )` over [0, rows), however the caller likes to split it.
using Parallel = std::function< void( int rows, const std::function< void( int, int ) >& body ) >;

/// Every pass in order, the whole frame. `input` and `output` are width x
/// height RGBA float, bottom-up, and may be the same buffer.
void Apply( const Uniforms& u, const float* input, float* output, const Parallel& parallel );

} // namespace rebate::render
