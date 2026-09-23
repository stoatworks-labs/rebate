/**
 * Rebate — browser demo.
 *
 * Colour negative film, and the scan of it. The clip is the scene: it exposes
 * three dye layers through a spectral crossover, each layer is developed along a
 * characteristic curve into dye carrying an orange mask, the dye is realised as
 * grain, and the flat orange negative is scanned and inverted the way a lab
 * scanner does it.
 *
 * ------------------------------------------------------- what is the plugin's
 *
 * **The five passes run for real.** The shader constants below are
 * `kVertexBody`, `kModel`, `kCopyBody`, `kFilmBody`, `kBlocksBody`,
 * `kLevelsBody` and `kScanBody` from `source/Shaders.cpp`, copied across
 * unedited, and assembled the way the plugin's `assemble()` assembles them —
 * the version line, then the model library for the film, blocks and scan passes,
 * then the body. `demo/tools/check_shaders.py` compares every one of them to the
 * C++ character for character, checks both sides assemble the same passes the
 * same way, and `tools/verify.sh` runs it, because two copies of a shader is
 * exactly the arrangement that drifts.
 *
 * The glyph table is `source/Font.cpp`'s, row for row, and check_shaders.py
 * checks that too: it is data, not a port.
 *
 * ------------------------------------------------------- what is a port
 *
 * Everything the plugin computes on the CPU and hands the shaders as uniforms is
 * a **hand port to JavaScript, checked by nobody but a reader**:
 *
 *   - `source/Model.cpp` — the five stocks, `Develop()` (stock x process x push
 *     x mask -> the curve every layer follows), `ScannerProfile()`,
 *     `Inverts()`, `UnwantedSum()`, and the constants in `Model.h` (base,
 *     impurities, capacity, crossover, ageing).
 *   - `source/Controls.cpp` — every slider position to the number the shader is
 *     handed: stops, pixels, densities, the leak's exposure.
 *   - `source/Frame.cpp` — `Compute()` (where the gate, the perforations and the
 *     print bands sit on the strip, and the centre crop) and `BuildText()` (the
 *     edge print, drawn into a small texture).
 *   - the per-frame arithmetic in `Rebate::ProcessOpenGL` — speed and age fog per
 *     layer, the edge print's exposure, the leak's weights, the grain's film
 *     frame, the levels' smoothing step, when the levels are primed.
 *
 * The C++ does that arithmetic in float and double; this does it in double and
 * the uniforms round it to float on the way in, as the plugin's casts do.
 *
 * ------------------------------------------------------- what is missing
 *
 * - **The host clock.** The plugin reads Resolume's clock and votes on whether it
 *   counts seconds or milliseconds; this page has one clock, its own transport,
 *   and hands it straight over. The grain still changes at 24 film frames a
 *   second of that clock, and the levels still smooth over a quarter of a second
 *   of it with a step clamped at 0.25 s.
 * - **The two FF_TYPE_INTEGER controls** (Grain Seed, Frame Number) are
 *   dropdowns listing every value, because the kit has no integer control.
 * - **The About block**, as on every page in the suite.
 * - **The Perturb test hooks** are held at zero, which is what the plugin ships.
 * - **The Presets menu is this page's.** The plugin ships no presets (the Stock
 *   menu is its preset list); the rows here are the user guide's "Start here"
 *   walk, one click each, and are labelled as such on the page.
 *
 * Rebate has no audio path, so there is no audio caveat.
 *
 * What this page is NOT: it is the plugin's shaders, not the plugin. No
 * Resolume, no FFGL, no C++ — and GLSL ES 3.00 rather than desktop GL 4.1 core,
 * which the kit's `port()` handles. A pixel here is not evidence about a pixel
 * there. `rbtest` is what measures the model; nothing on this page measures
 * anything.
 */

import { mountDemo } from './vendor/demo.js';
import { Program, PassBuffer, GLError, bindTexture } from './vendor/gl.js';

//---------------------------------------------------------------------------
// Shaders — verbatim from source/Shaders.cpp. Do not edit here: copy the C++
// across and run `python3 demo/tools/check_shaders.py`.
//
// None of the GLSL contains a backtick or a backslash, so the template literals
// carry it exactly; check_shaders.py refuses any backslash on this side.
//---------------------------------------------------------------------------

const VERSION = '#version 410 core\n';

const VERTEX_BODY = `
layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

out vec2 uv;

void main()
{
	gl_Position = vPosition;
	uv = vUV;
}
`;

const MODEL = `
uniform float Gamma;       //straight-line slope, density per log10 H
uniform float Toe;         //log10 H where the straight line meets the fog level
uniform float Latitude;    //log10 H from the toe to the shoulder
uniform float Knee;        //how sharply the toe and the shoulder bend
uniform float Fog;         //dye with no exposure (negative) or full (positive)
uniform int PositiveImage; //1 for a reversal image: density falls with exposure
uniform int Couplers;      //1 when the film carries masking couplers
uniform float Capacity;    //the most dye a coupler can form
uniform mat3 Unwanted;     //row = channel, column = dye; zero diagonal
uniform vec3 Base;         //the clear base, per channel
uniform int Perturb;       //negative-control hooks; always 0 in the plugin

//softplus, written so it neither overflows nor loses the small end: for
//u << 0 it is e^(Knee u) / Knee, for u >> 0 it is u.
float softplus( float u )
{
	return max( u, 0.0 ) + log( 1.0 + exp( -Knee * abs( u ) ) ) / Knee;
}

//The characteristic curve, as coverage: the fraction of a layer's
//developable grains that developed, 0 below the toe and 1 past the
//shoulder. Its straight line has slope 1 / Latitude in c, which the dye step
//turns into Gamma in density.
vec3 coverage( vec3 logH )
{
	vec3 c;
	for( int i = 0; i < 3; ++i )
		c[ i ] = ( softplus( logH[ i ] - Toe ) - softplus( logH[ i ] - Toe - Latitude ) ) / Latitude;
	return clamp( c, 0.0, 1.0 );
}

//Coverage to dye, in each dye's own (wanted) density.
vec3 dyeAmount( vec3 c )
{
	float g     = Gamma * ( ( Perturb & 1 ) != 0 ? 0.9 : 1.0 );
	float range = g * Latitude;
	vec3 a      = PositiveImage != 0 ? vec3( Fog + range ) - range * c : vec3( Fog ) + range * c;
	if( Couplers != 0 )
		a = min( a, vec3( Capacity ) );
	return a;
}

//Dye to what each scanner channel sees. Every dye absorbs a little outside
//its own band (Unwanted). A masking coupler that has NOT formed dye is
//coloured with exactly the unwanted absorptions its dye would have had, so
//the mask adds Unwanted * ( Capacity - a ) and the sum of the two is
//Unwanted * Capacity whatever the dyes are: a constant, the orange base,
//which the scanner divides out.
vec3 channelDensity( vec3 a )
{
	vec3 d = Base + a + Unwanted * a;
	if( Couplers != 0 )
	{
		vec3 uncoupled = ( Perturb & 16 ) != 0 ? vec3( Capacity ) : vec3( Capacity ) - a;
		d += Unwanted * uncoupled;
	}
	return d;
}
`;

const COPY_BODY = `
uniform sampler2D InputTexture;
uniform vec2 MaxUV;     //the part of the input texture that is really picture
uniform vec2 HalfTexel; //half an input texel, in picture space

in vec2 uv;
out vec4 fragColor;

void main()
{
	//Half a texel in from the edge, so GL_LINEAR at the boundary does not take
	//half its weight from the texture's undrawn padding. At every interior
	//texel centre this is an exact copy: bilinear at a texel centre returns
	//the texel.
	vec2 picture = clamp( uv, HalfTexel, vec2( 1.0 ) - HalfTexel );
	fragColor = texture( InputTexture, picture * MaxUV );
}
`;

const FILM_BODY = `
uniform sampler2D Picture;   //the scene, sRGB-encoded, bilinear, top at uv.y = 1
uniform sampler2D Text;      //the edge print, one texel per glyph pixel
uniform ivec2 Size;
uniform int Format;          //0 full, 1 roll film, 2 35 mm
uniform float FilmWidth;     //mm across the strip: the output's height
uniform float MmPerPixel;
uniform vec4 Gate;           //the picture: u left, v top, u right, v bottom
uniform vec4 Crop;           //the scene the gate shows: x, y, w, h, top-down
uniform int Holes;
uniform vec2 BandA;          //edge print line A: v top, v bottom
uniform vec2 BandB;          //line B
uniform float StripLeft;     //u of the text strip's first column
uniform vec2 TextSize;       //texels
uniform float GlyphsPerMm;
uniform float TextExposure;  //the latent edge print, pre-exposed at manufacture

uniform mat3 Crossover;      //scene rgb to layer exposure; rows sum to one
uniform vec3 Speed;          //per layer: 2^Exposure x the speed age left it
uniform vec3 FogExposure;    //age fog, an exposure per layer

uniform float LeakExposure;  //at the edge, centre of the edge; 0 is no leak
uniform int LeakEdge;        //0 left, 1 right, 2 top, 3 bottom
uniform float LeakSpread;    //falloff length, frame heights
uniform vec3 LeakWeights;    //the leak's spectrum on the three layers

const float PerfPitch  = 4.75;
const float PerfWidth  = 2.794;
const float PerfHeight = 1.981;
const float PerfEdge   = 2.01;
const float PerfRadius = 0.5;

in vec2 uv;
out vec4 fragColor;

vec3 decodeSrgb( vec3 v )
{
	vec3 lo = v / 12.92;
	vec3 hi = pow( max( ( v + 0.055 ) / 1.055, vec3( 0.0 ) ), vec3( 2.4 ) );
	return mix( lo, hi, step( vec3( 0.04045 ), v ) );
}

float overlap( float a0, float a1, float b0, float b1 )
{
	return max( 0.0, min( a1, b1 ) - max( a0, b0 ) );
}

//How much of this pixel is a perforation, from a rounded rectangle's signed
//distance ramped over one pixel.
float holeCoverage( vec2 film )
{
	float k   = floor( film.x / PerfPitch );
	float cu  = ( k + 0.5 ) * PerfPitch;
	float top = PerfEdge + 0.5 * PerfHeight;
	float cv  = film.y < 0.5 * FilmWidth ? top : FilmWidth - top;
	vec2 d    = abs( film - vec2( cu, cv ) ) - vec2( 0.5 * PerfWidth - PerfRadius, 0.5 * PerfHeight - PerfRadius );
	float sdf = length( max( d, vec2( 0.0 ) ) ) + min( max( d.x, d.y ), 0.0 ) - PerfRadius;
	return clamp( 0.5 - sdf / MmPerPixel, 0.0, 1.0 );
}

//The latent edge print at this point of a band, 0..1.
float edgePrint( vec2 film, vec2 band, float rowBase )
{
	if( band.y <= band.x || film.y < band.x || film.y >= band.y )
		return 0.0;
	float row = ( film.y - band.x ) / ( band.y - band.x ) * 7.0;
	row       = clamp( row, 0.5, 6.5 ) + rowBase;
	float col = ( film.x - StripLeft ) * GlyphsPerMm;

	//Explicit gradients: this is inside a branch that is not uniform across
	//the quad, where implicit derivatives are undefined. One output pixel is
	//MmPerPixel of film in both directions.
	vec2 dx = vec2( MmPerPixel * GlyphsPerMm / TextSize.x, 0.0 );
	vec2 dy = vec2( 0.0, MmPerPixel / ( band.y - band.x ) * 7.0 / TextSize.y );
	return textureGrad( Text, vec2( col / TextSize.x, row / TextSize.y ), dx, dy ).r;
}

void main()
{
	ivec2 pix  = ivec2( gl_FragCoord.xy );
	float topY = float( Size.y ) - gl_FragCoord.y;//pixel centre, top-down

	vec3 scene;
	float gate    = 1.0;
	float filmPart = 1.0;
	float latent  = 0.0;

	if( Format == 0 )
	{
		//The picture is the frame: sample our copy at exactly this texel.
		scene = texelFetch( Picture, pix, 0 ).rgb;
	}
	else
	{
		vec2 film = vec2( ( gl_FragCoord.x - 0.5 * float( Size.x ) ) * MmPerPixel, topY * MmPerPixel );
		float h   = 0.5 * MmPerPixel;

		gate = overlap( film.x - h, film.x + h, Gate.x, Gate.z ) / MmPerPixel
		     * overlap( film.y - h, film.y + h, Gate.y, Gate.w ) / MmPerPixel;

		//The scene is reduced into the gate by up to about 1.6x. Four bilinear
		//taps a quarter of the pixel's footprint either side of its centre
		//are a box over that footprint -- what a mip level would have given,
		//without generating one: glGenerateMipmap on a 4K float picture cost
		//11 ms a frame on this machine, most of the plugin.
		vec2 inGate    = clamp( ( film - Gate.xy ) / ( Gate.zw - Gate.xy ), 0.0, 1.0 );
		vec2 source    = Crop.xy + inGate * Crop.zw;
		vec2 footprint = MmPerPixel / ( Gate.zw - Gate.xy ) * Crop.zw;
		vec2 q         = 0.25 * footprint;
		scene = 0.25 * ( texture( Picture, vec2( source.x - q.x, 1.0 - ( source.y - q.y ) ) ).rgb
		               + texture( Picture, vec2( source.x + q.x, 1.0 - ( source.y - q.y ) ) ).rgb
		               + texture( Picture, vec2( source.x - q.x, 1.0 - ( source.y + q.y ) ) ).rgb
		               + texture( Picture, vec2( source.x + q.x, 1.0 - ( source.y + q.y ) ) ).rgb );

		if( Holes != 0 && ( Perturb & 64 ) == 0 )
			filmPart = 1.0 - holeCoverage( film );

		latent = max( edgePrint( film, BandA, 0.0 ), edgePrint( film, BandB, 7.0 ) );
	}

	vec3 light = Crossover * decodeSrgb( scene );
	vec3 H     = Speed * light * gate + FogExposure + TextExposure * latent;

	//The leak: light through the base at one edge, reaching the red-sensitive
	//layer first. It is an EXPOSURE, so it goes through the curve with the
	//scene and saturates on the shoulder.
	vec3 leak = vec3( 0.0 );
	if( LeakExposure > 0.0 )
	{
		float height = float( Size.y );
		float d, along;
		if( LeakEdge == 0 )      { d = gl_FragCoord.x / height;                   along = topY / height; }
		else if( LeakEdge == 1 ) { d = ( float( Size.x ) - gl_FragCoord.x ) / height; along = topY / height; }
		else if( LeakEdge == 2 ) { d = topY / height;                             along = gl_FragCoord.x / float( Size.x ); }
		else                     { d = ( height - topY ) / height;                along = gl_FragCoord.x / float( Size.x ); }
		float t       = ( along - 0.5 ) / 0.3;
		float lateral = 0.55 + 0.45 * exp( -t * t );
		vec3 weights  = ( Perturb & 256 ) != 0 ? LeakWeights.bgr : LeakWeights;
		leak          = LeakExposure * exp( -d / LeakSpread ) * lateral * weights;
	}

	vec3 c;
	if( ( Perturb & 8 ) != 0 )
	{
		//Negative control: the leak added as coverage, linearly, after the
		//curve -- so it never saturates.
		c = coverage( log2( max( H, vec3( 1e-12 ) ) ) * 0.30102999566 );
		c += leak / exp2( ( Toe + Latitude ) * 3.32192809489 );
	}
	else
		c = coverage( log2( max( H + leak, vec3( 1e-12 ) ) ) * 0.30102999566 );

	//alpha: the fraction of the pixel that is film rather than hole, plus 2
	//inside the picture.
	fragColor = vec4( c, filmPart + ( gate > 0.5 ? 2.0 : 0.0 ) );
}
`;

const BLOCKS_BODY = `
uniform sampler2D Film;
uniform ivec2 Size;
uniform ivec2 Grid;

in vec2 uv;
out vec4 fragColor;

void main()
{
	ivec2 b  = ivec2( gl_FragCoord.xy );
	vec2 lo  = vec2( b ) * vec2( Size ) / vec2( Grid );
	vec2 hi  = vec2( b + ivec2( 1 ) ) * vec2( Size ) / vec2( Grid );
	vec2 cell = ( hi - lo ) / 8.0;

	vec3 sum = vec3( 0.0 );
	float n  = 0.0;
	for( int j = 0; j < 8; ++j )
		for( int i = 0; i < 8; ++i )
		{
			ivec2 p = clamp( ivec2( floor( lo + ( vec2( i, j ) + 0.5 ) * cell ) ), ivec2( 0 ), Size - ivec2( 1 ) );
			vec4 f  = texelFetch( Film, p, 0 );
			if( f.a < 1.5 )
				continue;
			sum += channelDensity( dyeAmount( f.rgb ) );
			n += 1.0;
		}

	fragColor = n > 0.0 ? vec4( sum / n, n ) : vec4( 0.0 );
}
`;

const LEVELS_BODY = `
uniform sampler2D Blocks;
uniform sampler2D Previous;
uniform ivec2 Grid;
uniform float Alpha;  //one-pole step toward this frame's measurement
uniform int Prime;    //first frame: take the measurement outright

in vec2 uv;
out vec4 fragColor;

void main()
{
	int which = int( gl_FragCoord.x );
	vec3 lo   = vec3( 1e9 );
	vec3 hi   = vec3( -1e9 );
	bool any  = false;
	for( int j = 0; j < Grid.y; ++j )
		for( int i = 0; i < Grid.x; ++i )
		{
			vec4 b = texelFetch( Blocks, ivec2( i, j ), 0 );
			if( b.a <= 0.0 )
				continue;
			lo  = min( lo, b.rgb );
			hi  = max( hi, b.rgb );
			any = true;
		}

	vec4 previous = texelFetch( Previous, ivec2( which, 0 ), 0 );
	if( !any )
	{
		fragColor = previous;
		return;
	}

	vec3 measured = which == 0 ? lo : hi;
	fragColor     = vec4( Prime != 0 ? measured : previous.rgb + ( measured - previous.rgb ) * Alpha, 1.0 );
}
`;

const SCAN_BODY = `
uniform sampler2D Film;
uniform sampler2D Levels;
uniform sampler2D Source;
uniform vec2 MaxUV;
uniform ivec2 Size;

uniform int View;          //0 positive, 1 the negative on a light box
uniform int Invert;        //1 for a negative image, 0 for a slide
uniform int AutoLevels;
uniform vec3 ProfileBase;  //what the scanner's profile divides by
uniform float ProfileGamma;
uniform float BlackPoint;  //density above the base that prints black
uniform float WhitePoint;  //density above the base that prints white
uniform float ScanGamma;

uniform float GrainAmount;
uniform float GrainCell;   //output pixels per grain cell
uniform int GrainSites;    //N: developable sites per cell per layer
uniform int Seed;
uniform int GrainFrame;

uniform float MixAmount;

in vec2 uv;
out vec4 fragColor;

uint pcg( uint v )
{
	uint state = v * 747796405u + 2891336453u;
	uint word  = ( ( state >> ( ( state >> 28u ) + 4u ) ) ^ state ) * 277803737u;
	return ( word >> 22u ) ^ word;
}

//Each cell holds N sites per layer. A site is covered when at least one dye
//cloud lands on it: a Poisson draw with mean -ln( 1 - c ), so covered with
//probability exactly c. The covered fraction K / N has mean c and variance
//c ( 1 - c ) / N -- largest at c = 0.5 and zero at both ends, with nothing
//tuned to put it there.
vec3 grain( vec3 c )
{
	if( GrainAmount <= 0.0 || GrainSites <= 0 )
		return c;

	ivec2 pix  = ivec2( gl_FragCoord.xy );
	int top    = Size.y - 1 - pix.y;
	uvec2 cell = uvec2( floor( vec2( float( pix.x ), float( top ) ) / GrainCell ) );
	uint seed  = ( Perturb & 128 ) != 0 ? 0u : uint( Seed );
	uint base  = pcg( seed + pcg( uint( GrainFrame ) + pcg( cell.x + pcg( cell.y ) ) ) );

	vec3 result;
	for( int layer = 0; layer < 3; ++layer )
	{
		float mean     = clamp( c[ layer ], 0.0, 1.0 );
		uint threshold = uint( mean * 16777216.0 );
		uint h         = pcg( base + uint( layer ) * 2654435769u );
		int covered    = 0;
		for( int s = 0; s < GrainSites; ++s )
			if( ( pcg( h + uint( s ) ) >> 8u ) < threshold )
				++covered;

		float deviation = float( covered ) / float( GrainSites ) - mean;
		if( ( Perturb & 4 ) != 0 )
			deviation *= mean < 1.0 ? inversesqrt( 1.0 - mean ) : 0.0;
		result[ layer ] = mean + GrainAmount * deviation;
	}
	return result;
}

//A scanner's log-domain level p (0 black, 1 white) back to linear light,
//spanning R log units: ( 10^( R p ) - 1 ) / ( 10^R - 1 ). Zero at p = 0
//EXACTLY -- which is what makes a clipped sprocket hole black rather than
//nearly black -- and one at p = 1.
float expand( float p, float R )
{
	if( p <= 0.0 )
		return 0.0;
	if( p >= 1.0 )
		return 1.0;
	float k = R * 3.32192809489;
	if( k < 1e-4 )
		return p;
	return ( exp2( k * p ) - 1.0 ) / ( exp2( k ) - 1.0 );
}

float encodeSrgb( float x )
{
	x = clamp( x, 0.0, 1.0 );
	return x <= 0.0031308 ? 12.92 * x : 1.055 * pow( x, 1.0 / 2.4 ) - 0.055;
}

void main()
{
	vec4 film  = texelFetch( Film, ivec2( gl_FragCoord.xy ), 0 );
	float part = film.a >= 1.5 ? film.a - 2.0 : film.a;//film, not hole

	vec3 D = channelDensity( dyeAmount( grain( film.rgb ) ) );
	vec3 T = part * exp2( -D * 3.32192809489 ) + vec3( 1.0 - part );

	//The density the scanner measures. Where the pixel is all film that is D
	//itself; where it is partly hole, the light through both.
	vec3 measured = part >= 1.0 ? D : -log2( T ) * 0.30102999566;

	vec3 linear;
	if( View == 1 )
		linear = T;
	else
	{
		vec3 p;
		vec3 span;
		if( AutoLevels != 0 )
		{
			vec3 least = texelFetch( Levels, ivec2( 0, 0 ), 0 ).rgb;
			vec3 most  = texelFetch( Levels, ivec2( 1, 0 ), 0 ).rgb;
			span       = max( most - least, vec3( 1e-3 ) );
			p          = Invert != 0 ? ( measured - least ) / span : ( most - measured ) / span;
		}
		else
		{
			span    = vec3( max( WhitePoint - BlackPoint, 1e-3 ) );
			vec3 d  = measured - ProfileBase;
			p       = ( d - vec3( BlackPoint ) ) / span;
			if( Invert == 0 )
				p = vec3( 1.0 ) - p;
		}

		vec3 R = span / ProfileGamma * ScanGamma;
		linear = vec3( expand( p.r, R.r ), expand( p.g, R.g ), expand( p.b, R.b ) );
	}

	vec4 scanned = vec4( encodeSrgb( linear.r ), encodeSrgb( linear.g ), encodeSrgb( linear.b ), 1.0 );
	if( MixAmount >= 1.0 )
	{
		//Exactly the scan: mix( a, b, 1 ) is allowed to round.
		fragColor = scanned;
		return;
	}
	vec4 source = texture( Source, gl_FragCoord.xy / vec2( Size ) * MaxUV );
	fragColor   = mix( source, scanned, MixAmount );
}
`;

/**
 * `rebate::shaders::assemble` — the version line, then the model library if the
 * pass runs the model, then the body. The table below is the plugin's
 * `Vertex()` .. `Scan()`, and check_shaders.py holds the two side by side.
 */
function assemble(body, withModel) {
  let s = VERSION;
  if (withModel) s += MODEL;
  s += body;
  return s;
}

const VERTEX = assemble(VERTEX_BODY, false);
const COPY = assemble(COPY_BODY, false);
const FILM = assemble(FILM_BODY, true);
const BLOCKS = assemble(BLOCKS_BODY, true);
const LEVELS = assemble(LEVELS_BODY, false);
const SCAN = assemble(SCAN_BODY, true);

//---------------------------------------------------------------------------
// A port of source/Model.h and source/Model.cpp — the film as numbers.
//
// Layers and channels are both indexed R, G, B: layer 0 is red-sensitive and
// forms cyan dye, which the scanner's red channel reads.
//---------------------------------------------------------------------------

const kMidGrey = 0.18;
const kNegativeToeBelowMid = 1.3;
const kNegativeLatitude = 2.7;
const kKnee = 6.0;
const kPushGamma = 0.15;
const kPushFog = 0.03;

const kC41 = 0;
const kE6 = 1;
const kCross = 2;

const kBase = [0.06, 0.06, 0.07];

const kImpurity = [
  [1.00, 0.06, 0.01],
  [0.18, 1.00, 0.08],
  [0.08, 0.32, 1.00],
];

const kCapacity = 2.4;

const kCrossover = [
  [0.85, 0.12, 0.03],
  [0.08, 0.84, 0.08],
  [0.02, 0.10, 0.88],
];

const kAgeSpeedLoss = [0.05, 0.10, 0.25];
const kAgeFog = [0.40, 0.80, 3.00];

const kMaxGrainSites = 64;

/** `model::kStocks` — name, edge code, reversal, gamma, grain sites, fog, age. */
const kStocks = [
  { name: 'Fine 100', code: 'FN100', reversal: false, gamma: 0.62, grainSites: 64, fog: 0.10, age: 0.0 },
  { name: 'Portrait 400', code: 'PT400', reversal: false, gamma: 0.58, grainSites: 32, fog: 0.14, age: 0.0 },
  { name: 'Grain 800', code: 'GR800', reversal: false, gamma: 0.60, grainSites: 14, fog: 0.18, age: 0.0 },
  { name: 'Slide 100', code: 'SL100', reversal: true, gamma: 0.60, grainSites: 56, fog: 0.08, age: 0.0 },
  { name: 'Expired 400', code: 'PT400', reversal: false, gamma: 0.58, grainSites: 28, fog: 0.14, age: 0.6 },
];

const clamp = (v, lo, hi) => (v < lo ? lo : v > hi ? hi : v);
const clamp01 = (v) => clamp(v, 0, 1);

/** `std::lround`: half away from zero, where Math.round goes half up. */
const lround = (x) => Math.sign(x) * Math.round(Math.abs(x));

/** `model::StockAt`. */
const stockAt = (index) => kStocks[clamp(index, 0, kStocks.length - 1)];

/** `model::MidGreyLog`. */
const midGreyLog = () => Math.log10(kMidGrey);

/** `model::UnwantedSum`. */
function unwantedSum(channel) {
  let sum = 0;
  for (let d = 0; d < 3; d += 1) if (d !== channel) sum += kImpurity[channel][d];
  return sum;
}

/** `model::ReversalEmulsion`. */
const reversalEmulsion = (stock, process) => stock.reversal || process === kCross;

/** `model::Inverts`. */
const inverts = (process) => process !== kE6;

/** `model::Develop`, with the test hook's pushGainScale at the plugin's 1. */
function develop(stock, process, push, maskOn) {
  const reversal = reversalEmulsion(stock, process);
  const e6 = process === kE6;
  const gain = 1.0 + kPushGamma * push;
  const mid = midGreyLog();

  const curve = { gamma: 0.6, toe: 0.0, latitude: 2.7, fog: 0.1, positive: false, couplers: false };
  if (!reversal && !e6) {
    //The colour negative, as designed.
    curve.gamma = stock.gamma * gain;
    curve.latitude = kNegativeLatitude;
    curve.toe = mid - kNegativeToeBelowMid;
    curve.fog = stock.fog + kPushFog * push;
    curve.positive = false;
    curve.couplers = maskOn;
  } else if (!reversal && e6) {
    //A negative emulsion through reversal chemistry: a flat positive that
    //keeps its orange mask.
    curve.gamma = 1.5 * stock.gamma * gain;
    curve.latitude = 2.2;
    curve.toe = mid - 0.5 * curve.latitude;
    curve.fog = 0.5 * stock.fog + kPushFog * push;
    curve.positive = true;
    curve.couplers = maskOn;
  } else if (reversal && !e6) {
    //Cross-processing: reversal film in C-41. A negative, steep and short,
    //with no masking couplers because reversal film never had any.
    curve.gamma = 2.0 * stock.gamma * gain;
    curve.latitude = 2.0;
    curve.toe = mid - 1.0;
    curve.fog = 0.5 * stock.fog + kPushFog * push;
    curve.positive = false;
    curve.couplers = false;
  } else {
    //A slide, as designed.
    curve.gamma = 3.0 * stock.gamma * gain;
    curve.latitude = 1.55;
    curve.toe = mid - 0.5 * curve.latitude;
    curve.fog = 0.5 * stock.fog + kPushFog * push;
    curve.positive = true;
    curve.couplers = false;
  }

  curve.fog = Math.max(curve.fog, 0.0);
  return curve;
}

/** `model::ScannerProfile`, with the test hook's maskAssumed at the plugin's true. */
function scannerProfile(stock, process) {
  const profile = { base: [0, 0, 0], gamma: 0.6 };
  if (inverts(process)) {
    for (let c = 0; c < 3; c += 1) profile.base[c] = kBase[c] + stock.fog + unwantedSum(c) * kCapacity;
    profile.gamma = stock.gamma;
  } else {
    const slide = develop(stock, kE6, 0.0, false);
    for (let c = 0; c < 3; c += 1) profile.base[c] = kBase[c] + slide.fog;
    profile.gamma = slide.gamma;
  }
  return profile;
}

//---------------------------------------------------------------------------
// A port of source/Controls.cpp — what a 0..1 slider means. Only the forward
// mappings the plugin uses at render time, plus the inverses its constructor
// uses for the defaults.
//---------------------------------------------------------------------------

const exposureStops = (v) => clamp01(v) * 12.0 - 6.0;
const exposureParam = (stops) => clamp01((stops + 6.0) / 12.0);
const pushStops = (v) => clamp01(v) * 4.0 - 1.0;
const pushParam = (stops) => clamp01((stops + 1.0) / 4.0);
const ageControl = (v) => clamp01(v);
const grainAmount = (v) => clamp01(v);
const grainCellPixels = (v) => 2 ** (3.0 * clamp01(v));
const grainSizeParam = (pixels) => clamp01(Math.log2(Math.max(pixels, 1.0)) / 3.0);

function leakExposure(v) {
  v = clamp01(v);
  if (v <= 0.0) return 0.0;
  return kMidGrey * 2 ** (-4.0 + 14.0 * v);
}
function leakParam(exposure) {
  if (exposure <= 0.0) return 0.0;
  return clamp01((Math.log2(exposure / kMidGrey) + 4.0) / 14.0);
}

const leakSpread = (v) => 0.05 * 20.0 ** clamp01(v);
const leakSpreadParam = (heights) => clamp01(Math.log(Math.max(heights, 0.05) / 0.05) / Math.log(20.0));
const blackPoint = (v) => 0.6 * clamp01(v);
const blackPointParam = (density) => clamp01(density / 0.6);
const whitePoint = (v) => 0.6 + 2.4 * clamp01(v);
const whitePointParam = (density) => clamp01((density - 0.6) / 2.4);
const scannerGamma = (v) => 2 ** (2.0 * clamp01(v) - 1.0);
const scannerGammaParam = (gamma) => clamp01((Math.log2(Math.max(gamma, 0.5)) + 1.0) / 2.0);

const kProcessNames = ['C-41', 'E-6', 'Cross'];
const kLeakEdgeNames = ['Left', 'Right', 'Top', 'Bottom'];
const kViewNames = ['Positive', 'Negative'];
const kFormatNames = ['Full', '6x6', '35 mm'];

/** `controls::OptionIndex`. */
const optionIndex = (value, count) => clamp(lround(value), 0, count - 1);

//---------------------------------------------------------------------------
// A port of source/Frame.cpp — the strip, the gate, the holes, the edge print.
//
// Film coordinates are millimetres: u along the strip from the frame centre,
// v across it from the top edge. The film's full width fills the output's
// height.
//---------------------------------------------------------------------------

const kFull = 0;
const kSquare = 1;
const kStrip = 2;

const kGlyphRowsPerMm = 7.0;

// `font::kWidth`, `kHeight`, `kAdvance`, `kFirst`, `kCount`.
const kFontWidth = 5;
const kFontHeight = 7;
const kFontAdvance = kFontWidth + 1;
const kFontFirst = 32;
const kFontCount = 96;

/** `frame::centreCrop`. */
function centreCrop(sceneAspect, gateAspect, crop) {
  if (sceneAspect > gateAspect) {
    const w = gateAspect / sceneAspect;
    crop[0] = 0.5 * (1.0 - w);
    crop[1] = 0.0;
    crop[2] = w;
    crop[3] = 1.0;
  } else {
    const h = sceneAspect / gateAspect;
    crop[0] = 0.0;
    crop[1] = 0.5 * (1.0 - h);
    crop[2] = 1.0;
    crop[3] = h;
  }
}

/** `frame::Compute`. */
function computeGeometry(format, width, height) {
  const g = {
    format: clamp(format, 0, 2),
    filmWidthMm: 0.0,
    mmPerPixel: 0.0,
    gate: [0, 0, 0, 0],
    crop: [0, 0, 1, 1],
    holes: false,
    bandA: [0, 0],
    bandB: [0, 0],
    stripLeftMm: 0.0,
  };
  const aspect = width / Math.max(height, 1);

  if (g.format === kFull) {
    //No film to speak of: the picture is the frame. The film pass does not
    //read any of this; it is filled in so nothing is left undefined.
    g.filmWidthMm = height;
    g.mmPerPixel = 1.0;
    g.gate = [-0.5 * width, 0.0, 0.5 * width, height];
    return g;
  }

  if (g.format === kSquare) {
    //120 roll film: 61.5 mm wide, a 56 x 56 frame, no perforations. The
    //edge print sits in the top 2.75 mm margin.
    g.filmWidthMm = 61.5;
    g.gate = [-28.0, 2.75, 28.0, 58.75];
    g.holes = false;
    g.bandA = [0.9, 1.9];
  } else {
    //35 mm: a 36 x 24 frame centred on a 35 mm strip.
    g.filmWidthMm = 35.0;
    g.gate = [-18.0, 5.5, 18.0, 29.5];
    g.holes = true;
    g.bandA = [4.25, 5.25];
    g.bandB = [29.75, 30.75];
  }

  g.mmPerPixel = g.filmWidthMm / Math.max(height, 1);
  centreCrop(aspect, (g.gate[2] - g.gate[0]) / (g.gate[3] - g.gate[1]), g.crop);

  const halfWidthMm = 0.5 * width * g.mmPerPixel;
  g.stripLeftMm = -halfWidthMm - 2.0;
  return g;
}

/** `font::Glyph` and `font::Bit`, over the table at the foot of this section. */
function fontBit(code, x, y) {
  if (x < 0 || x >= kFontWidth || y < 0 || y >= kFontHeight) return false;
  if (code < kFontFirst || code >= kFontFirst + kFontCount) return false;
  return GLYPHS[code - kFontFirst][y][x] === '#';
}

/** `frame::drawLabel` — `text` centred at column `centre`, on line 0 or 1. */
function drawLabel(strip, text, centre, line) {
  const n = text.length;
  const span = n * kFontAdvance - 1;
  const left = lround(centre - 0.5 * span);
  const rowAt = line * kFontHeight;

  for (let i = 0; i < n; i += 1) {
    const code = text.charCodeAt(i) & 0xff;
    for (let y = 0; y < kFontHeight; y += 1) {
      for (let x = 0; x < kFontWidth; x += 1) {
        const column = left + i * kFontAdvance + x;
        if (column < 0 || column >= strip.width || !fontBit(code, x, y)) continue;
        strip.pixels[(rowAt + y) * strip.width + column] = 255;
      }
    }
  }
}

const wrap100 = (n) => ((n % 100) + 100) % 100;

/** `frame::BuildText` — the edge print as a bitmap, and the key it was drawn for. */
function buildText(g, code, frameNumber, on) {
  const strip = { width: 1, height: 14, pixels: null, key: '' };
  strip.key = `${g.format}|${g.stripLeftMm.toFixed(6)}|${code}|${frameNumber}|${on ? 1 : 0}`;

  const spanMm = g.format === kFull ? 1.0 : -2.0 * g.stripLeftMm;
  strip.width = Math.max(1, Math.ceil(spanMm * kGlyphRowsPerMm));
  strip.height = 2 * kFontHeight;
  strip.pixels = new Uint8Array(strip.width * strip.height);

  if (!on || g.format === kFull) return strip;

  //Column of u = 0 mm.
  const origin = -g.stripLeftMm * kGlyphRowsPerMm;
  const leftU = g.stripLeftMm;
  const rightU = -g.stripLeftMm;

  if (g.format === kStrip) {
    //Line A: the stock code on every half frame, between the numbers.
    for (let k = Math.floor((leftU - 9.5) / 19.0) - 1; 9.5 + 19.0 * k < rightU + 19.0; k += 1) {
      drawLabel(strip, code, origin + (9.5 + 19.0 * k) * kGlyphRowsPerMm, 0);
    }

    //Line B: frame numbers. The number under each frame's centre with an
    //arrow, and the number with an A halfway to the next.
    for (let k = Math.floor(leftU / 19.0) - 1; 19.0 * k < rightU + 19.0; k += 1) {
      const half = Math.floor(k / 2.0);
      // C++ `k % 2 == 0` — JavaScript's % keeps the dividend's sign the same way.
      const label = k % 2 === 0 ? `>${wrap100(frameNumber + half)}` : `${wrap100(frameNumber + half)}A`;
      drawLabel(strip, label, origin + 19.0 * k * kGlyphRowsPerMm, 1);
    }
  } else {
    //Roll film: the stock code in the top margin every 30 mm. Frame numbers
    //on 120 live on the backing paper, so there are none here.
    for (let k = Math.floor((leftU - 15.0) / 30.0) - 1; 15.0 + 30.0 * k < rightU + 30.0; k += 1) {
      drawLabel(strip, code, origin + (15.0 + 30.0 * k) * kGlyphRowsPerMm, 0);
    }
  }

  return strip;
}

//---------------------------------------------------------------------------
// The 5x7 glyph table — source/Font.cpp's `kGlyphs`, row for row, codes 32 to
// 127. Data, not a port: check_shaders.py compares every row to the C++.
//---------------------------------------------------------------------------

const GLYPHS = [
  ['.....', '.....', '.....', '.....', '.....', '.....', '.....'], // 32 space
  ['..#..', '..#..', '..#..', '..#..', '.....', '.....', '..#..'], // 33 !
  ['.#.#.', '.#.#.', '.#.#.', '.....', '.....', '.....', '.....'], // 34 "
  ['.#.#.', '.#.#.', '#####', '.#.#.', '#####', '.#.#.', '.#.#.'], // 35 #
  ['..#..', '.####', '#.#..', '.###.', '..#.#', '####.', '..#..'], // 36 $
  ['##..#', '##..#', '...#.', '..#..', '.#...', '#..##', '#..##'], // 37 %
  ['.##..', '#..#.', '#.#..', '.#...', '#.#.#', '#..#.', '.##.#'], // 38 &
  ['..#..', '..#..', '.#...', '.....', '.....', '.....', '.....'], // 39 '
  ['...#.', '..#..', '.#...', '.#...', '.#...', '..#..', '...#.'], // 40 (
  ['.#...', '..#..', '...#.', '...#.', '...#.', '..#..', '.#...'], // 41 )
  ['.....', '..#..', '#.#.#', '.###.', '#.#.#', '..#..', '.....'], // 42 *
  ['.....', '..#..', '..#..', '#####', '..#..', '..#..', '.....'], // 43 +
  ['.....', '.....', '.....', '.....', '.##..', '..#..', '.#...'], // 44 ,
  ['.....', '.....', '.....', '#####', '.....', '.....', '.....'], // 45 -
  ['.....', '.....', '.....', '.....', '.....', '.##..', '.##..'], // 46 .
  ['.....', '....#', '...#.', '..#..', '.#...', '#....', '.....'], // 47 /
  ['.###.', '#...#', '#..##', '#.#.#', '##..#', '#...#', '.###.'], // 48 0
  ['..#..', '.##..', '..#..', '..#..', '..#..', '..#..', '.###.'], // 49 1
  ['.###.', '#...#', '....#', '...#.', '..#..', '.#...', '#####'], // 50 2
  ['#####', '...#.', '..#..', '...#.', '....#', '#...#', '.###.'], // 51 3
  ['...#.', '..##.', '.#.#.', '#..#.', '#####', '...#.', '...#.'], // 52 4
  ['#####', '#....', '####.', '....#', '....#', '#...#', '.###.'], // 53 5
  ['..##.', '.#...', '#....', '####.', '#...#', '#...#', '.###.'], // 54 6
  ['#####', '....#', '...#.', '..#..', '.#...', '.#...', '.#...'], // 55 7
  ['.###.', '#...#', '#...#', '.###.', '#...#', '#...#', '.###.'], // 56 8
  ['.###.', '#...#', '#...#', '.####', '....#', '...#.', '.##..'], // 57 9
  ['.....', '.##..', '.##..', '.....', '.##..', '.##..', '.....'], // 58 :
  ['.....', '.##..', '.##..', '.....', '.##..', '..#..', '.#...'], // 59 ;
  ['...#.', '..#..', '.#...', '#....', '.#...', '..#..', '...#.'], // 60 <
  ['.....', '.....', '#####', '.....', '#####', '.....', '.....'], // 61 =
  ['.#...', '..#..', '...#.', '....#', '...#.', '..#..', '.#...'], // 62 >
  ['.###.', '#...#', '....#', '...#.', '..#..', '.....', '..#..'], // 63 ?
  ['.###.', '#...#', '....#', '.##.#', '#.#.#', '#.#.#', '.###.'], // 64 @
  ['.###.', '#...#', '#...#', '#####', '#...#', '#...#', '#...#'], // 65 A
  ['####.', '#...#', '#...#', '####.', '#...#', '#...#', '####.'], // 66 B
  ['.###.', '#...#', '#....', '#....', '#....', '#...#', '.###.'], // 67 C
  ['###..', '#..#.', '#...#', '#...#', '#...#', '#..#.', '###..'], // 68 D
  ['#####', '#....', '#....', '####.', '#....', '#....', '#####'], // 69 E
  ['#####', '#....', '#....', '####.', '#....', '#....', '#....'], // 70 F
  ['.###.', '#...#', '#....', '#.###', '#...#', '#...#', '.####'], // 71 G
  ['#...#', '#...#', '#...#', '#####', '#...#', '#...#', '#...#'], // 72 H
  ['.###.', '..#..', '..#..', '..#..', '..#..', '..#..', '.###.'], // 73 I
  ['..###', '...#.', '...#.', '...#.', '...#.', '#..#.', '.##..'], // 74 J
  ['#...#', '#..#.', '#.#..', '##...', '#.#..', '#..#.', '#...#'], // 75 K
  ['#....', '#....', '#....', '#....', '#....', '#....', '#####'], // 76 L
  ['#...#', '##.##', '#.#.#', '#.#.#', '#...#', '#...#', '#...#'], // 77 M
  ['#...#', '#...#', '##..#', '#.#.#', '#..##', '#...#', '#...#'], // 78 N
  ['.###.', '#...#', '#...#', '#...#', '#...#', '#...#', '.###.'], // 79 O
  ['####.', '#...#', '#...#', '####.', '#....', '#....', '#....'], // 80 P
  ['.###.', '#...#', '#...#', '#...#', '#.#.#', '#..#.', '.##.#'], // 81 Q
  ['####.', '#...#', '#...#', '####.', '#.#..', '#..#.', '#...#'], // 82 R
  ['.####', '#....', '#....', '.###.', '....#', '....#', '####.'], // 83 S
  ['#####', '..#..', '..#..', '..#..', '..#..', '..#..', '..#..'], // 84 T
  ['#...#', '#...#', '#...#', '#...#', '#...#', '#...#', '.###.'], // 85 U
  ['#...#', '#...#', '#...#', '#...#', '#...#', '.#.#.', '..#..'], // 86 V
  ['#...#', '#...#', '#...#', '#.#.#', '#.#.#', '#.#.#', '.#.#.'], // 87 W
  ['#...#', '#...#', '.#.#.', '..#..', '.#.#.', '#...#', '#...#'], // 88 X
  ['#...#', '#...#', '#...#', '.#.#.', '..#..', '..#..', '..#..'], // 89 Y
  ['#####', '....#', '...#.', '..#..', '.#...', '#....', '#####'], // 90 Z
  ['.###.', '.#...', '.#...', '.#...', '.#...', '.#...', '.###.'], // 91 [
  ['.....', '#....', '.#...', '..#..', '...#.', '....#', '.....'], // 92 backslash
  ['.###.', '...#.', '...#.', '...#.', '...#.', '...#.', '.###.'], // 93 ]
  ['..#..', '.#.#.', '#...#', '.....', '.....', '.....', '.....'], // 94 ^
  ['.....', '.....', '.....', '.....', '.....', '.....', '#####'], // 95 _
  ['.#...', '..#..', '...#.', '.....', '.....', '.....', '.....'], // 96 `
  ['.....', '.....', '.###.', '....#', '.####', '#...#', '.####'], // 97 a
  ['#....', '#....', '#.##.', '##..#', '#...#', '#...#', '####.'], // 98 b
  ['.....', '.....', '.###.', '#....', '#....', '#...#', '.###.'], // 99 c
  ['....#', '....#', '.##.#', '#..##', '#...#', '#...#', '.####'], // 100 d
  ['.....', '.....', '.###.', '#...#', '#####', '#....', '.###.'], // 101 e
  ['..##.', '.#..#', '.#...', '###..', '.#...', '.#...', '.#...'], // 102 f
  ['.....', '.....', '.####', '#...#', '.####', '....#', '.###.'], // 103 g
  ['#....', '#....', '#.##.', '##..#', '#...#', '#...#', '#...#'], // 104 h
  ['..#..', '.....', '.##..', '..#..', '..#..', '..#..', '.###.'], // 105 i
  ['...#.', '.....', '..##.', '...#.', '...#.', '#..#.', '.##..'], // 106 j
  ['#....', '#....', '#..#.', '#.#..', '##...', '#.#..', '#..#.'], // 107 k
  ['.##..', '..#..', '..#..', '..#..', '..#..', '..#..', '.###.'], // 108 l
  ['.....', '.....', '##.#.', '#.#.#', '#.#.#', '#...#', '#...#'], // 109 m
  ['.....', '.....', '#.##.', '##..#', '#...#', '#...#', '#...#'], // 110 n
  ['.....', '.....', '.###.', '#...#', '#...#', '#...#', '.###.'], // 111 o
  ['.....', '.....', '####.', '#...#', '####.', '#....', '#....'], // 112 p
  ['.....', '.....', '.####', '#...#', '.####', '....#', '....#'], // 113 q
  ['.....', '.....', '#.##.', '##..#', '#....', '#....', '#....'], // 114 r
  ['.....', '.....', '.####', '#....', '.###.', '....#', '####.'], // 115 s
  ['.#...', '.#...', '###..', '.#...', '.#...', '.#..#', '..##.'], // 116 t
  ['.....', '.....', '#...#', '#...#', '#...#', '#..##', '.##.#'], // 117 u
  ['.....', '.....', '#...#', '#...#', '#...#', '.#.#.', '..#..'], // 118 v
  ['.....', '.....', '#...#', '#...#', '#.#.#', '#.#.#', '.#.#.'], // 119 w
  ['.....', '.....', '#...#', '.#.#.', '..#..', '.#.#.', '#...#'], // 120 x
  ['.....', '.....', '#...#', '#...#', '.####', '....#', '.###.'], // 121 y
  ['.....', '.....', '#####', '...#.', '..#..', '.#...', '#####'], // 122 z
  ['...#.', '..#..', '..#..', '.#...', '..#..', '..#..', '...#.'], // 123 {
  ['..#..', '..#..', '..#..', '..#..', '..#..', '..#..', '..#..'], // 124 |
  ['.#...', '..#..', '..#..', '...#.', '..#..', '..#..', '.#...'], // 125 }
  ['.....', '.#...', '#.#.#', '...#.', '.....', '.....', '.....'], // 126 ~
  ['.....', '.....', '.....', '.....', '.....', '.....', '.....'], // 127 del
];

//---------------------------------------------------------------------------
// The chain — a port of Rebate::ProcessOpenGL, the plugin's five passes in the
// plugin's order.
//---------------------------------------------------------------------------

/** `kGridW`, `kGridH`: the block grid the scanner's auto levels look at. */
const kGridW = 64;
const kGridH = 36;

/** `kLevelsTau`: the scanner's levels settle with this time constant. */
const kLevelsTau = 0.25;

/** Seconds of clock a single frame may advance by (`kMaxFrameDelta`). */
const kMaxFrameDelta = 0.25;

/** Film runs at 24 frames a second, so the grain changes 24 times a second. */
const kFilmRate = 24.0;

/** Warmth 1: the leak's spectrum on the red, green and blue layers. */
const kWarmLeak = [1.0, 0.30, 0.06];

/**
 * The scene copy is RGBA32F sampled with GL_LINEAR in the plugin. Desktop GL
 * filters float textures as a matter of course; WebGL2 only with
 * OES_texture_float_linear, and a float texture with a LINEAR filter and no
 * extension is INCOMPLETE — it samples as black, which here would be a strip of
 * unexposed film with no picture on it. So where the extension is missing the
 * copy is RGBA16F, which WebGL2 always filters, and the page says so.
 *
 * Probed once up front, on a throwaway context, so the disclosure at the foot
 * of the page can say which of the two this browser is running.
 */
function probeFloatLinear() {
  try {
    const context = document.createElement('canvas').getContext('webgl2');
    if (!context) return null;
    const ok = context.getExtension('OES_texture_float_linear') !== null;
    context.getExtension('WEBGL_lose_context')?.loseContext();
    return ok;
  } catch {
    return null;
  }
}

const FLOAT_LINEAR = probeFloatLinear();

function createRenderer(gl, quad) {
  const copyShader = new Program(gl, VERTEX, COPY, 'copy');
  const filmShader = new Program(gl, VERTEX, FILM, 'film');
  const blocksShader = new Program(gl, VERTEX, BLOCKS, 'blocks');
  const levelsShader = new Program(gl, VERTEX, LEVELS, 'levels');
  const scanShader = new Program(gl, VERTEX, SCAN, 'scan');

  const pictureFormat = gl.getExtension('OES_texture_float_linear') ? gl.RGBA32F : gl.RGBA16F;

  const picture = new PassBuffer(gl, { filter: 'linear' });//the scene, bilinear
  const film = new PassBuffer(gl, { filter: 'nearest' });//mean coverage per layer, and the region
  const blocks = new PassBuffer(gl, { filter: 'nearest' });//mean channel density per block
  const levels = [new PassBuffer(gl, { filter: 'nearest' }), new PassBuffer(gl, { filter: 'nearest' })];
  let levelsCurrent = 0;
  let levelsPrimed = false;
  let autoWasActive = false;
  let lastNow = -1;

  const textTexture = gl.createTexture();
  let textWidth = 1;
  let textHeight = 14;
  let textKey = '';

  /** `Rebate::uploadText` — rebuilt only when what it says changes. */
  function uploadText(geometry, code, frameNumber, on) {
    const strip = buildText(geometry, code, frameNumber, on);
    if (strip.key === textKey) return;

    gl.bindTexture(gl.TEXTURE_2D, textTexture);
    gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.R8, strip.width, strip.height, 0, gl.RED, gl.UNSIGNED_BYTE, strip.pixels);
    gl.pixelStorei(gl.UNPACK_ALIGNMENT, 4);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR_MIPMAP_LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    gl.generateMipmap(gl.TEXTURE_2D);
    gl.bindTexture(gl.TEXTURE_2D, null);

    textWidth = strip.width;
    textHeight = strip.height;
    textKey = strip.key;
  }

  // The plugin's glUniform2i calls: the kit's Program has no ivec2 setter.
  const setIVec2 = (program, name, x, y) => {
    const location = program.location(name);
    if (location !== null) gl.uniform2i(location, x, y);
  };
  // glUniformMatrix3fv( ..., GL_TRUE, ... ): row-major in, as the plugin does.
  // WebGL2, unlike WebGL1, accepts transpose = true.
  const setMat3RowMajor = (program, name, values) => {
    const location = program.location(name);
    if (location !== null) gl.uniformMatrix3fv(location, true, values);
  };

  const unwanted = new Float32Array(9);
  for (let r = 0; r < 3; r += 1) {
    for (let c = 0; c < 3; c += 1) unwanted[r * 3 + c] = r === c ? 0.0 : kImpurity[r][c];
  }
  const crossover = new Float32Array(9);
  for (let r = 0; r < 3; r += 1) {
    for (let c = 0; c < 3; c += 1) crossover[r * 3 + c] = kCrossover[r][c];
  }

  return {
    render({ input, params, width, height, time }) {
      //-------------------------------------------------------------------
      // The clock. The page's own, in seconds; see the disclosure.
      //-------------------------------------------------------------------
      const now = time;
      let dt = 0.0;
      if (lastNow >= 0.0) dt = clamp(now - lastNow, 0.0, kMaxFrameDelta);
      lastNow = now;

      const filmFrames = Math.floor(Math.max(now, 0.0) * kFilmRate + 1e-6);
      const grainFrame = filmFrames % 16777216;

      //-------------------------------------------------------------------
      // What the controls say.
      //-------------------------------------------------------------------
      const stock = stockAt(optionIndex(params.get('stock'), kStocks.length));
      const process = optionIndex(params.get('process'), kProcessNames.length);
      const maskOn = params.get('mask') > 0.5;
      const exposure = exposureStops(params.get('exposure'));
      const push = pushStops(params.get('push'));
      const age = clamp(stock.age + ageControl(params.get('age')), 0.0, 1.0);

      const curve = develop(stock, process, push, maskOn);
      const profile = scannerProfile(stock, process);
      const invert = inverts(process);

      const toeExposure = 10 ** curve.toe;
      const speed = [0, 0, 0];
      const fogExposure = [0, 0, 0];
      for (let i = 0; i < 3; i += 1) {
        speed[i] = 2 ** exposure * 10 ** (-age * kAgeSpeedLoss[i]);
        fogExposure[i] = age * kAgeFog[i] * toeExposure;
      }
      const textExposure = 10 ** (curve.toe + 0.8 * curve.latitude);

      const amount = grainAmount(params.get('grainAmount'));
      const grainCell = grainCellPixels(params.get('grainSize'));
      const grainSites = clamp(stock.grainSites, 1, kMaxGrainSites);
      const seed = clamp(lround(params.get('grainSeed')), 0, 999);

      const leak = leakExposure(params.get('leakAmount'));
      const leakEdge = optionIndex(params.get('leakEdge'), kLeakEdgeNames.length);
      const warmth = clamp(params.get('leakWarmth'), 0.0, 1.0);
      const spread = leakSpread(params.get('leakSpread'));
      const leakWeights = kWarmLeak.map((w) => 1.0 + (w - 1.0) * warmth);

      const view = optionIndex(params.get('view'), kViewNames.length);
      const autoLevels = params.get('autoLevels') > 0.5;
      const blackPt = blackPoint(params.get('blackPoint'));
      const whitePt = whitePoint(params.get('whitePoint'));
      const scanGamma = scannerGamma(params.get('scannerGamma'));

      const format = optionIndex(params.get('format'), kFormatNames.length);
      const edgeText = params.get('edgeText') > 0.5;
      const frameNumber = clamp(lround(params.get('frameNumber')), 0, 99);

      const geometry = computeGeometry(format, width, height);

      //-------------------------------------------------------------------
      // Buffers, and the text, before anything binds a texture. The level
      // buffers are 2 x 1 whatever the raster, so a resize never reallocates
      // them and never clears what the scanner has settled on.
      //-------------------------------------------------------------------
      const gridW = Math.min(kGridW, width);
      const gridH = Math.min(kGridH, height);
      picture.ensure(width, height, pictureFormat);
      film.ensure(width, height, gl.RGBA32F);
      blocks.ensure(gridW, gridH, gl.RGBA32F);
      levels[0].ensure(2, 1, gl.RGBA32F);
      levels[1].ensure(2, 1, gl.RGBA32F);
      uploadText(geometry, stock.code, frameNumber, edgeText);

      // The plugin re-primes the levels on a resize only under a test hook,
      // so a resize here does not re-prime them either.

      const autoActive = autoLevels && view === 0;
      if (autoActive && !autoWasActive) levelsPrimed = false;//switched on: take the next measurement outright
      autoWasActive = autoActive;

      const alpha = 1.0 - Math.exp(-dt / kLevelsTau);

      const setModel = (shader) => {
        shader.set('Gamma', curve.gamma);
        shader.set('Toe', curve.toe);
        shader.set('Latitude', curve.latitude);
        shader.set('Knee', kKnee);
        shader.set('Fog', curve.fog);
        shader.setInt('PositiveImage', curve.positive ? 1 : 0);
        shader.setInt('Couplers', curve.couplers ? 1 : 0);
        shader.set('Capacity', kCapacity);
        setMat3RowMajor(shader, 'Unwanted', unwanted);
        shader.set('Base', kBase[0], kBase[1], kBase[2]);
        shader.setInt('Perturb', 0);
      };

      gl.disable(gl.BLEND);

      //-------------------------------------------------------------------
      // 1. Copy. Float, so the scene survives the chain unquantised.
      //-------------------------------------------------------------------
      picture.bind();
      copyShader.use();
      bindTexture(gl, 0, input.texture);
      copyShader.setSampler('InputTexture', 0);
      copyShader.set('MaxUV', 1, 1);
      copyShader.set('HalfTexel', 0.5 / width, 0.5 / height);
      quad.draw();

      //-------------------------------------------------------------------
      // 2. The film: exposure and development, to mean coverage.
      //-------------------------------------------------------------------
      film.bind();
      filmShader.use();
      bindTexture(gl, 0, picture.texture);
      bindTexture(gl, 1, textTexture);

      setModel(filmShader);
      filmShader.setSampler('Picture', 0);
      filmShader.setSampler('Text', 1);
      setIVec2(filmShader, 'Size', width, height);
      filmShader.setInt('Format', geometry.format);
      filmShader.set('FilmWidth', geometry.filmWidthMm);
      filmShader.set('MmPerPixel', geometry.mmPerPixel);
      filmShader.set('Gate', geometry.gate[0], geometry.gate[1], geometry.gate[2], geometry.gate[3]);
      filmShader.set('Crop', geometry.crop[0], geometry.crop[1], geometry.crop[2], geometry.crop[3]);
      filmShader.setInt('Holes', geometry.holes ? 1 : 0);
      filmShader.set('BandA', geometry.bandA[0], geometry.bandA[1]);
      filmShader.set('BandB', geometry.bandB[0], geometry.bandB[1]);
      filmShader.set('StripLeft', geometry.stripLeftMm);
      filmShader.set('TextSize', textWidth, textHeight);
      filmShader.set('GlyphsPerMm', kGlyphRowsPerMm);
      filmShader.set('TextExposure', textExposure);

      setMat3RowMajor(filmShader, 'Crossover', crossover);
      filmShader.set('Speed', speed[0], speed[1], speed[2]);
      filmShader.set('FogExposure', fogExposure[0], fogExposure[1], fogExposure[2]);

      filmShader.set('LeakExposure', leak);
      filmShader.setInt('LeakEdge', leakEdge);
      filmShader.set('LeakSpread', spread);
      filmShader.set('LeakWeights', leakWeights[0], leakWeights[1], leakWeights[2]);
      quad.draw();

      //-------------------------------------------------------------------
      // 3 and 4. The scanner's auto levels, only when they are looked at.
      //-------------------------------------------------------------------
      if (autoActive) {
        blocks.bind();
        blocksShader.use();
        bindTexture(gl, 0, film.texture);
        bindTexture(gl, 1, null);
        setModel(blocksShader);
        blocksShader.setSampler('Film', 0);
        setIVec2(blocksShader, 'Size', width, height);
        setIVec2(blocksShader, 'Grid', gridW, gridH);
        quad.draw();

        const next = 1 - levelsCurrent;
        levels[next].bind();
        levelsShader.use();
        bindTexture(gl, 0, blocks.texture);
        bindTexture(gl, 1, levels[levelsCurrent].texture);
        levelsShader.setSampler('Blocks', 0);
        levelsShader.setSampler('Previous', 1);
        setIVec2(levelsShader, 'Grid', gridW, gridH);
        levelsShader.set('Alpha', alpha);
        levelsShader.setInt('Prime', levelsPrimed ? 0 : 1);
        quad.draw();

        levelsCurrent = next;
        levelsPrimed = true;
      }

      //-------------------------------------------------------------------
      // 5. The scan, straight to the canvas — this page's host framebuffer.
      //-------------------------------------------------------------------
      gl.bindFramebuffer(gl.FRAMEBUFFER, null);
      gl.viewport(0, 0, width, height);

      scanShader.use();
      bindTexture(gl, 0, film.texture);
      bindTexture(gl, 1, levels[levelsCurrent].texture);
      bindTexture(gl, 2, input.texture);

      setModel(scanShader);
      scanShader.setSampler('Film', 0);
      scanShader.setSampler('Levels', 1);
      scanShader.setSampler('Source', 2);
      scanShader.set('MaxUV', 1, 1);
      setIVec2(scanShader, 'Size', width, height);

      scanShader.setInt('View', view);
      scanShader.setInt('Invert', invert ? 1 : 0);
      scanShader.setInt('AutoLevels', autoLevels ? 1 : 0);
      scanShader.set('ProfileBase', profile.base[0], profile.base[1], profile.base[2]);
      scanShader.set('ProfileGamma', profile.gamma);
      scanShader.set('BlackPoint', blackPt);
      scanShader.set('WhitePoint', whitePt);
      scanShader.set('ScanGamma', scanGamma);

      scanShader.set('GrainAmount', amount);
      scanShader.set('GrainCell', grainCell);
      scanShader.setInt('GrainSites', grainSites);
      scanShader.setInt('Seed', seed);
      scanShader.setInt('GrainFrame', grainFrame);
      scanShader.set('MixAmount', clamp(params.get('mix'), 0.0, 1.0));
      quad.draw();

      // Leave no texture of ours bound: the clip renderer draws into the input
      // texture next frame, and a texture bound on a unit while it is also the
      // render target is a feedback loop WebGL refuses.
      for (let unit = 0; unit < 3; unit += 1) bindTexture(gl, unit, null);
      gl.activeTexture(gl.TEXTURE0);
    },
  };
}

//---------------------------------------------------------------------------
// The controls, read out of the plugin's own constructor (Rebate.cpp). Same
// names, same groups, same order, same defaults, same dropdown elements. The
// defaults are the constructor's own expressions, through the ported inverses.
//
// Absent: the About block, which is text and buttons that open a browser.
//---------------------------------------------------------------------------

const signed = (x, digits = 1) => `${x >= 0 ? '+' : '−'}${Math.abs(x).toFixed(digits)}`;
const integers = (count) => Array.from({ length: count }, (_, i) => String(i));

// The page's presets. NOT the plugin's: it ships none, and its Stock menu is
// the preset list the spec asked for. These are the user guide's "Start here"
// walk, one click each; anything a row does not name is at its default.
const PRESETS = {
  'Guide: the negative on a light box': { view: 1 },
  'Guide: three stops under': { exposure: exposureParam(-3) },
  'Guide: three stops over': { exposure: exposureParam(3) },
  'Guide: a warm leak, four stops over': { leakAmount: leakParam(kMidGrey * 2 ** 4) },
  'Guide: expired stock': { stock: 4 },
  'Guide: cross-processed': { process: 2 },
  'Guide: Mask On off': { mask: 0 },
  'Guide: coarse grain that survives video': { stock: 2, grainAmount: 0.8, grainSize: grainSizeParam(3) },
  'Guide: Auto Levels, the lab': { autoLevels: 1 },
  'Guide: full frame, no strip': { format: 0 },
};

const floatNote = FLOAT_LINEAR === false
  ? 'This browser lacks it, so here that copy is RGBA16F, half float — which holds an 8-bit clip to well under one code value, but is not the plugin\'s buffer.'
  : 'This browser has it, so the copy is RGBA32F as in the plugin; on one without it the page falls back to RGBA16F for that one buffer and says so here.';

mountDemo({
  name: 'Rebate',
  pluginId: 'RB01',
  tagline:
    'Colour negative film, and the scan of it. The clip exposes three dye layers, which are developed along a characteristic curve into dyes carrying an orange mask, realised as grain, and then scanned and inverted the way a lab scanner does it. The orange negative, mid-tone grain, blue shadows on expired stock, warm light leaks, cross-processing and the rebate with its sprocket holes are what the process does, not a lookup table.',
  repo: 'https://github.com/stoatworks-labs/rebate',
  blurb:
    'It runs Rebate\'s own five GLSL passes, copied unedited from the repository into WebGL2, on generated clips in this page. The film\'s numbers — the stocks, the development, the scanner\'s profile, the strip and its edge print — are the plugin\'s C++ ported by hand to JavaScript, and only a reader checks that port.',

  // Film is opaque: at Mix 1 the output alpha is exactly 1 and the black
  // rebate is picture, not transparency.
  showBackdrop: false,

  // The film, blocks and levels buffers are RGBA32F render targets, as in the
  // plugin. WebGL2 makes that an extension where desktop GL simply has it.
  needFloat: true,

  params: [
    { id: 'exposure', name: 'Exposure', type: 'standard', default: exposureParam(0.0), group: 'Scene',
      display: (v) => `${signed(exposureStops(v))} stops`,
      hint: 'The camera\'s exposure, −6 to +6 stops. It multiplies the scene\'s light before the film sees it, so it goes through the curve: under, the shadows fall into the toe; over, the highlights roll off on the shoulder instead of clipping.' },
    { id: 'stock', name: 'Stock', type: 'option', default: 1, group: 'Scene',
      elements: kStocks.map((s) => s.name),
      hint: 'Five invented films, described by their parameters: contrast, grain sites per cell, fog and age. Expired 400 is Portrait 400 with an age of 0.6 of its own. Slide 100 is reversal film: no mask, and in C-41 it is cross-processed.' },
    { id: 'push', name: 'Push', type: 'standard', default: pushParam(0.0), group: 'Scene',
      display: (v) => `${signed(pushStops(v))} stops`,
      hint: 'Extra development, −1 (a pull) to +3 stops. Each stop raises γ by 15% and adds 0.03 of chemical fog. Development only: the under-exposure that goes with a push is Exposure\'s job.' },
    { id: 'age', name: 'Age', type: 'standard', default: 0.0, group: 'Scene',
      hint: 'Added to the stock\'s own age, stopping at 1. Age fog is an exposure that goes through the curve, the blue-sensitive top layer taking the most, so the shadows go blue and the highlights warm under the manual scan.' },

    { id: 'process', name: 'Process', type: 'option', default: 0, group: 'Process',
      elements: kProcessNames,
      hint: 'C-41 is negative chemistry; E-6 reversal, so the scanner does not invert; Cross treats the film as reversal film in C-41: steep, no mask, and a scanner dividing by a mask that is not there.' },
    { id: 'mask', name: 'Mask On', type: 'boolean', default: 1, group: 'Process',
      hint: 'The masking couplers. Off, the dyes\' impurities are not cancelled and the manual scan still divides by the mask it expects: dark, red and muddy. Does nothing for Slide 100 or under Cross.' },

    { id: 'grainAmount', name: 'Grain Amount', type: 'standard', default: 0.35, group: 'Grain',
      hint: 'How much of the physical fluctuation to keep: 1 is the full binomial figure for the stock\'s grain sites, 0 exactly none. Grain is loudest in the mid-tones because a count varies most at half coverage.' },
    { id: 'grainSize', name: 'Grain Size', type: 'standard', default: grainSizeParam(1.5), group: 'Grain',
      display: (v) => `${grainCellPixels(v).toFixed(2)} px`,
      hint: 'The grain cell, 1 to 8 output pixels, counted from the top-left.' },
    { id: 'grainSeed', name: 'Grain Seed', type: 'option', default: 1, group: 'Grain',
      elements: integers(1000),
      hint: 'An FF_TYPE_INTEGER, 0 to 999, in the plugin; a dropdown here because the page\'s panel has no integer control. The same seed, film frame and pixel always give the same grain.' },

    { id: 'leakAmount', name: 'Leak Amount', type: 'standard', default: 0.0, group: 'Leak',
      display: (v) => (leakExposure(v) <= 0 ? 'off' : `${signed(Math.log2(leakExposure(v) / kMidGrey))} st vs grey`),
      hint: '0 is no leak. Above that, the leak\'s exposure at its edge, from 4 stops under mid grey to 10 over. It is an exposure through the base, so it saturates on the shoulder and fogs the rebate too.' },
    { id: 'leakEdge', name: 'Leak Edge', type: 'option', default: 1, group: 'Leak',
      elements: kLeakEdgeNames,
      hint: 'Which edge of the output the leak comes from; a little stronger along the middle of that edge.' },
    { id: 'leakWarmth', name: 'Leak Warmth', type: 'standard', default: 0.8, group: 'Leak',
      hint: 'At 0 the leak exposes all three layers equally; at 1 the red layer fully, green at 30% and blue at 6%.' },
    { id: 'leakSpread', name: 'Leak Spread', type: 'standard', default: leakSpreadParam(0.3), group: 'Leak',
      display: (v) => `${leakSpread(v).toFixed(2)} frame h`,
      hint: 'How far in the leak reaches: its falloff length, 0.05 to 1 frame height, from the edge of the output.' },

    { id: 'view', name: 'View', type: 'option', default: 0, group: 'Scan',
      elements: kViewNames,
      hint: 'Positive is the scan. Negative is the film on a light box: orange for a C-41 negative, the edge print dark, the sprocket holes clear.' },
    { id: 'autoLevels', name: 'Auto Levels', type: 'boolean', default: 0, group: 'Scan',
      hint: 'The scanner measures each frame per channel — the least and most dense of 64×36 blocks — smoothed over a quarter of a second. It takes most of an exposure error, an age cast or a missing mask back out, as a lab does.' },
    { id: 'blackPoint', name: 'Black Point', type: 'standard', default: blackPointParam(0.05), group: 'Scan',
      display: (v) => `D ${blackPoint(v).toFixed(2)}`,
      hint: 'For a negative, the density above the profile\'s base that prints black, 0 to 0.6. Does nothing under Auto Levels.' },
    { id: 'whitePoint', name: 'White Point', type: 'standard', default: whitePointParam(1.25), group: 'Scan',
      display: (v) => `D ${whitePoint(v).toFixed(2)}`,
      hint: 'For a negative, the density above the base that prints white, 0.6 to 3.0. Does nothing under Auto Levels.' },
    { id: 'scannerGamma', name: 'Scanner Gamma', type: 'standard', default: scannerGammaParam(1.0), group: 'Scan',
      display: (v) => `${scannerGamma(v).toFixed(2)}`,
      hint: 'The scanner\'s tone curve, 0.5 to 2, exactly 1 in the middle.' },

    { id: 'format', name: 'Format', type: 'option', default: 2, group: 'Frame',
      elements: kFormatNames,
      hint: 'Full fills the output with the scan. 6x6 is 120 roll film, a square frame and no holes. 35 mm is a 36×24 frame with perforations, the stock code above and frame numbers below.' },
    { id: 'edgeText', name: 'Edge Text On', type: 'boolean', default: 1, group: 'Frame',
      hint: 'The edge print: a latent image exposed at manufacture, so it prints light in the positive and dark on the negative. Invented codes; no real manufacturer\'s mark.' },
    { id: 'frameNumber', name: 'Frame Number', type: 'option', default: 12, group: 'Frame',
      elements: integers(100),
      hint: 'An FF_TYPE_INTEGER, 0 to 99, in the plugin; a dropdown here. The number under the frame on 35 mm, with the A numbers at the half frames. Nothing in 6x6 or Full.' },
    { id: 'mix', name: 'Mix', type: 'standard', default: 1.0, group: 'Frame',
      hint: 'The scan against the untouched clip. At 1 the output is exactly the scan, fully opaque.' },
  ],

  sources: ['scene', 'ramp', 'bars', 'grid', 'spot', 'detail'],

  presets: PRESETS,

  differences: [
    'The CPU half is a port. Everything the plugin computes in C++ and hands the shaders — the five stocks and the curve each develops into (Model.cpp), every slider\'s conversion (Controls.cpp), where the frame, the sprocket holes and the print bands sit on the strip and the edge print itself (Frame.cpp), and the per-frame arithmetic around them — is re-implemented in JavaScript in this page. The shaders and the glyph table are checked against the plugin\'s character for character by the repository\'s verify script; the port is checked by nobody but a reader.',
    'The Presets menu is this page\'s, not the plugin\'s. Rebate ships no presets (its Stock menu is the preset list); the rows here are the user guide\'s "Start here" walk, one click each, and anything a row does not name is at its default.',
    'The clock is this page\'s transport, not a host\'s. The plugin reads Resolume\'s clock and works out whether it counts seconds or milliseconds; here there is one clock and it is handed straight over. The grain still re-draws at 24 film frames a second of it — so it stands still when paused, and Step advances it every two or three presses — and Auto Levels still smooths over a quarter of a second of it.',
    'Grain Seed and Frame Number are FF_TYPE_INTEGER parameters in the plugin. The panel here has no integer control, so each is a dropdown listing every value it can take (0–999 and 0–99).',
    `The plugin copies the clip into an RGBA32F texture and reads it with a linear filter. WebGL2 filters a 32-bit float texture only with the OES_texture_float_linear extension. ${floatNote}`,
    'The clips are 8-bit, so there is nothing brighter than white in them for Exposure to pull back down. The plugin keeps a float input exactly, and its own harness drives it with an HDR wedge; that is not something this page can show.',
    'The plugin\'s About block — its name, version and links, drawn by the host as parameters — is not here.',
    'Start with View → Negative to see the orange mask, then Leak Amount, then Stock → Expired 400 and Process → Cross, in that order: it is the user guide\'s walk, and the Presets menu has each step. Ramps and steps is the clip where the grain\'s mid-tone peak and the curve\'s toe and shoulder are easiest to read.',
    'The model\'s numerical proof — the curve\'s slope read back as 0.57948 for a γ of 0.58, a neutral wedge through the mask coming out neutral to 1.1e-6, grain variance matching c(1 − c)/N over 921,600 samples, eleven perturbed models each caught — is an offline harness in the repository. Nothing on this page measures anything.',
  ],

  createRenderer,
});
