#include "Shaders.h"

namespace rebate::shaders
{
namespace
{
const char* const kVersion = "#version 410 core\n";

//---------------------------------------------------------------------------
// The vertex shader every pass shares.
//---------------------------------------------------------------------------
const char* const kVertexBody = R"(
layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

out vec2 uv;

void main()
{
	gl_Position = vPosition;
	uv = vUV;
}
)";

//---------------------------------------------------------------------------
// The model. A fragment, not a shader: no #version, no main. The film, blocks
// and scan passes are each assembled around this one string, so there is one
// curve, one set of dyes and one mask in the plugin.
//
// Layers and channels are both indexed r, g, b: the red-sensitive layer forms
// cyan dye, which the scanner's red channel reads. See Model.h.
//---------------------------------------------------------------------------
const char* const kModel = R"(
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
)";

//---------------------------------------------------------------------------
// Pass 1: copy.
//---------------------------------------------------------------------------
const char* const kCopyBody = R"(
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
)";

//---------------------------------------------------------------------------
// Pass 2: the film. Exposure and development, per pixel, to mean coverage.
//
// Film coordinates are millimetres: u along the strip from the frame centre,
// v across it from the top edge. The output is top-down here -- row 0 is the
// top of the picture -- whatever GL's origin is.
//---------------------------------------------------------------------------
const char* const kFilmBody = R"(
uniform sampler2D Picture;   //the scene, sRGB-encoded, mipmapped, top at uv.y = 1
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

		vec2 inGate = clamp( ( film - Gate.xy ) / ( Gate.zw - Gate.xy ), 0.0, 1.0 );
		vec2 source = Crop.xy + inGate * Crop.zw;
		scene       = texture( Picture, vec2( source.x, 1.0 - source.y ) ).rgb;

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
)";

//---------------------------------------------------------------------------
// Pass 3: blocks. Mean channel density of the picture area, per block.
//---------------------------------------------------------------------------
const char* const kBlocksBody = R"(
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
)";

//---------------------------------------------------------------------------
// Pass 4: levels. Texel 0 is the least dense block, texel 1 the most.
//---------------------------------------------------------------------------
const char* const kLevelsBody = R"(
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
)";

//---------------------------------------------------------------------------
// Pass 5: scan. Grain, dyes, mask, transmittance, and the scanner.
//
// The grain is integer hashing (a PCG output mix), never fract( sin ). A
// cell's draw is a pure function of ( Seed, GrainFrame, cell, layer, site ),
// and the cell is counted from the TOP-LEFT pixel, so the same seed and frame
// give the same grain at any raster where the pixels coincide.
//---------------------------------------------------------------------------
const char* const kScanBody = R"(
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
)";

std::string assemble( const char* body, bool withModel )
{
	std::string s = kVersion;
	if( withModel )
		s += kModel;
	s += body;
	return s;
}
} // namespace

std::string Vertex()
{
	return assemble( kVertexBody, false );
}

std::string Copy()
{
	return assemble( kCopyBody, false );
}

std::string Film()
{
	return assemble( kFilmBody, true );
}

std::string Blocks()
{
	return assemble( kBlocksBody, true );
}

std::string Levels()
{
	return assemble( kLevelsBody, false );
}

std::string Scan()
{
	return assemble( kScanBody, true );
}

} // namespace rebate::shaders
