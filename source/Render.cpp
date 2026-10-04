#include "Render.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace rebate::render
{

//---------------------------------------------------------------------------
// The per-frame arithmetic. Once, for both builds.
//---------------------------------------------------------------------------
Uniforms Prepare( const HostValues& v, int width, int height, double seconds, int perturb )
{
	Uniforms u;
	u.width   = width;
	u.height  = height;
	u.perturb = perturb;

	//The grain's film frame, reduced in double: nothing absolute crosses into
	//the passes, because Resolume's clock overflows a float.
	const double filmFrames = std::floor( std::max( seconds, 0.0 ) * kFilmRate + 1e-6 );
	u.grainFrame            = static_cast< int >( std::fmod( filmFrames, 16777216.0 ) );

	const model::Stock& stock = model::StockAt( controls::OptionIndex( v.stock, model::kStockCount ) );
	const int process         = controls::OptionIndex( v.process, controls::kProcessCount );
	const bool maskOn         = v.maskOn > 0.5f;
	const double exposure     = controls::ExposureStops( v.exposure );
	const double push         = controls::PushStops( v.push );
	const double age          = std::clamp( stock.age + controls::Age( v.age ), 0.0, 1.0 );

	u.stock   = &stock;
	u.curve   = model::Develop( stock, process, push, maskOn, ( perturb & model::kPerturbPushGain ) ? 0.5 : 1.0 );
	u.profile = model::ScannerProfile( stock, process, ( perturb & model::kPerturbCrossProfile ) == 0 );
	u.invert  = model::Inverts( process );

	const double toeExposure = std::pow( 10.0, u.curve.toe );
	for( int i = 0; i < 3; ++i )
	{
		u.speed[ i ]       = static_cast< float >( std::exp2( exposure ) * std::pow( 10.0, -age * model::kAgeSpeedLoss[ i ] ) );
		u.fogExposure[ i ] = static_cast< float >( age * model::kAgeFog[ i ] * toeExposure );
	}
	u.textExposure = static_cast< float >( std::pow( 10.0, u.curve.toe + 0.8 * u.curve.latitude ) );

	u.grainAmount = controls::GrainAmount( v.grainAmount );
	u.grainCell   = controls::GrainCellPixels( v.grainSize );
	u.grainSites  = std::clamp( stock.grainSites, 1, model::kMaxGrainSites );
	u.seed        = std::clamp( static_cast< int >( std::lround( v.grainSeed ) ), 0, 999 );

	u.leakExposure     = controls::LeakExposure( v.leakAmount );
	u.leakEdge         = controls::OptionIndex( v.leakEdge, controls::kLeakEdgeCount );
	const float warmth = std::clamp( v.leakWarmth, 0.0f, 1.0f );
	u.leakSpread       = controls::LeakSpread( v.leakSpread );
	for( int i = 0; i < 3; ++i )
		u.leakWeights[ i ] = 1.0f + ( kWarmLeak[ i ] - 1.0f ) * warmth;

	u.view       = controls::OptionIndex( v.view, controls::kViewCount );
	u.autoLevels = v.autoLevels > 0.5f;
	u.autoActive = u.autoLevels && u.view == 0;
	u.blackPoint = controls::BlackPoint( v.blackPoint );
	u.whitePoint = controls::WhitePoint( v.whitePoint );
	u.scanGamma  = controls::ScannerGamma( v.scannerGamma );

	u.format      = controls::OptionIndex( v.format, controls::kFormatCount );
	u.edgeText    = v.edgeText > 0.5f;
	u.frameNumber = std::clamp( static_cast< int >( std::lround( v.frameNumber ) ), 0, 99 );
	u.mix         = std::clamp( v.mix, 0.0f, 1.0f );

	u.geometry = frame::Compute( u.format, width, height );
	u.gridW    = std::min( kGridW, width );
	u.gridH    = std::min( kGridH, height );
	return u;
}

//---------------------------------------------------------------------------
// The mirror. Everything below copies GLSL in Shaders.cpp and is in float on
// purpose: the point is the GPU's answer, not a better one.
//---------------------------------------------------------------------------
namespace
{
struct vec3
{
	float x = 0.0f, y = 0.0f, z = 0.0f;
	float& operator[]( int i ) { return i == 0 ? x : ( i == 1 ? y : z ); }
	float operator[]( int i ) const { return i == 0 ? x : ( i == 1 ? y : z ); }
};

constexpr float kLog2Of10 = 3.32192809489f;
constexpr float kLog10Of2 = 0.30102999566f;

/// The model library's uniforms, cast as `Rebate::ProcessOpenGL` casts them
/// for the upload. Unwanted is row = channel, column = dye, zero diagonal.
struct Model
{
	float gamma, toe, latitude, knee, fog, capacity;
	bool positive, couplers;
	float unwanted[ 3 ][ 3 ];
	float base[ 3 ];
	int perturb;

	explicit Model( const Uniforms& u )
	{
		gamma    = static_cast< float >( u.curve.gamma );
		toe      = static_cast< float >( u.curve.toe );
		latitude = static_cast< float >( u.curve.latitude );
		knee     = static_cast< float >( model::kKnee );
		fog      = static_cast< float >( u.curve.fog );
		capacity = static_cast< float >( model::kCapacity );
		positive = u.curve.positive;
		couplers = u.curve.couplers;
		for( int r = 0; r < 3; ++r )
		{
			for( int c = 0; c < 3; ++c )
				unwanted[ r ][ c ] = r == c ? 0.0f : static_cast< float >( model::kImpurity[ r ][ c ] );
			base[ r ] = static_cast< float >( model::kBase[ r ] );
		}
		perturb = u.perturb;
	}

	//= mirrored: kModel softplus() in Shaders.cpp
	float softplus( float u ) const
	{
		return std::max( u, 0.0f ) + std::log( 1.0f + std::exp( -knee * std::fabs( u ) ) ) / knee;
	}

	//= mirrored: kModel coverage() in Shaders.cpp
	vec3 coverage( const vec3& logH ) const
	{
		vec3 c;
		for( int i = 0; i < 3; ++i )
			c[ i ] = std::clamp( ( softplus( logH[ i ] - toe ) - softplus( logH[ i ] - toe - latitude ) ) / latitude, 0.0f, 1.0f );
		return c;
	}

	//= mirrored: kModel dyeAmount() in Shaders.cpp
	vec3 dyeAmount( const vec3& c ) const
	{
		const float g     = gamma * ( ( perturb & 1 ) != 0 ? 0.9f : 1.0f );
		const float range = g * latitude;
		vec3 a;
		for( int i = 0; i < 3; ++i )
		{
			a[ i ] = positive ? ( fog + range ) - range * c[ i ] : fog + range * c[ i ];
			if( couplers )
				a[ i ] = std::min( a[ i ], capacity );
		}
		return a;
	}

	/// `Unwanted * a`, as a column-major mat3 times a vec3 is evaluated.
	vec3 unwantedTimes( const vec3& a ) const
	{
		vec3 r;
		for( int c = 0; c < 3; ++c )
			r[ c ] = unwanted[ c ][ 0 ] * a[ 0 ] + unwanted[ c ][ 1 ] * a[ 1 ] + unwanted[ c ][ 2 ] * a[ 2 ];
		return r;
	}

	//= mirrored: kModel channelDensity() in Shaders.cpp
	vec3 channelDensity( const vec3& a ) const
	{
		const vec3 impurity = unwantedTimes( a );
		vec3 d;
		for( int c = 0; c < 3; ++c )
			d[ c ] = base[ c ] + a[ c ] + impurity[ c ];
		if( couplers )
		{
			vec3 uncoupled;
			for( int c = 0; c < 3; ++c )
				uncoupled[ c ] = ( perturb & 16 ) != 0 ? capacity : capacity - a[ c ];
			const vec3 mask = unwantedTimes( uncoupled );
			for( int c = 0; c < 3; ++c )
				d[ c ] += mask[ c ];
		}
		return d;
	}
};

//---------------------------------------------------------------------------
// Sampling, as GL_LINEAR with GL_CLAMP_TO_EDGE does it.
//
// `steps` is the filterBits test hook as a count: 0 for exact float weights
// (the OpenFX build), 2^bits to round each weight to a GPU's fixed point,
// -2^bits to truncate it.
//---------------------------------------------------------------------------
float weight( float f, float steps )
{
	if( steps > 0.0f )
		return std::floor( f * steps + 0.5f ) / steps;
	if( steps < 0.0f )
		return std::floor( f * -steps ) / -steps;
	return f;
}

/// One channel of a single-channel level at texture coordinate (s, t).
float bilinear1( const std::vector< float >& level, int w, int h, float s, float t, float steps )
{
	const float u = s * static_cast< float >( w ) - 0.5f;
	const float v = t * static_cast< float >( h ) - 0.5f;
	const float fu = std::floor( u ), fv = std::floor( v );
	const float a = weight( u - fu, steps ), b = weight( v - fv, steps );
	const int i0 = std::clamp( static_cast< int >( fu ), 0, w - 1 ), i1 = std::clamp( static_cast< int >( fu ) + 1, 0, w - 1 );
	const int j0 = std::clamp( static_cast< int >( fv ), 0, h - 1 ), j1 = std::clamp( static_cast< int >( fv ) + 1, 0, h - 1 );
	const float* r0 = level.data() + static_cast< size_t >( j0 ) * w;
	const float* r1 = level.data() + static_cast< size_t >( j1 ) * w;
	const float top = r0[ i0 ] + ( r0[ i1 ] - r0[ i0 ] ) * a;
	const float bot = r1[ i0 ] + ( r1[ i1 ] - r1[ i0 ] ) * a;
	return top + ( bot - top ) * b;
}

/// RGB of an RGBA image at texture coordinate (s, t).
vec3 bilinear3( const float* image, int w, int h, float s, float t, float steps )
{
	const float u = s * static_cast< float >( w ) - 0.5f;
	const float v = t * static_cast< float >( h ) - 0.5f;
	const float fu = std::floor( u ), fv = std::floor( v );
	const float a = weight( u - fu, steps ), b = weight( v - fv, steps );
	const int i0 = std::clamp( static_cast< int >( fu ), 0, w - 1 ), i1 = std::clamp( static_cast< int >( fu ) + 1, 0, w - 1 );
	const int j0 = std::clamp( static_cast< int >( fv ), 0, h - 1 ), j1 = std::clamp( static_cast< int >( fv ) + 1, 0, h - 1 );
	const float* p00 = image + ( static_cast< size_t >( j0 ) * w + i0 ) * 4;
	const float* p10 = image + ( static_cast< size_t >( j0 ) * w + i1 ) * 4;
	const float* p01 = image + ( static_cast< size_t >( j1 ) * w + i0 ) * 4;
	const float* p11 = image + ( static_cast< size_t >( j1 ) * w + i1 ) * 4;
	vec3 out;
	for( int k = 0; k < 3; ++k )
	{
		const float top = p00[ k ] + ( p10[ k ] - p00[ k ] ) * a;
		const float bot = p01[ k ] + ( p11[ k ] - p01[ k ] ) * a;
		out[ k ]        = top + ( bot - top ) * b;
	}
	return out;
}

/// textureGrad on the edge print, GL_LINEAR_MIPMAP_LINEAR / GL_LINEAR, for a
/// footprint of `rhoX` texels across and `rhoY` down (GL 4.1 section 3.8.11:
/// lambda = log2 of the larger, magnification at lambda <= 0, otherwise a
/// blend of the two levels either side).
float sampleText( const TextMips& text, float s, float t, float rhoX, float rhoY, float steps )
{
	const int last     = static_cast< int >( text.level.size() ) - 1;
	const float lambda = std::log2( std::max( rhoX, rhoY ) );
	if( !( lambda > 0.0f ) || last == 0 )
		return bilinear1( text.level[ 0 ], text.width[ 0 ], text.height[ 0 ], s, t, steps );
	if( lambda >= static_cast< float >( last ) )
		return bilinear1( text.level[ last ], text.width[ last ], text.height[ last ], s, t, steps );
	const int d1     = static_cast< int >( std::floor( lambda ) );
	const float frac = lambda - static_cast< float >( d1 );
	const float a    = bilinear1( text.level[ d1 ], text.width[ d1 ], text.height[ d1 ], s, t, steps );
	const float b    = bilinear1( text.level[ d1 + 1 ], text.width[ d1 + 1 ], text.height[ d1 + 1 ], s, t, steps );
	return a + ( b - a ) * weight( frac, steps );
}

//---------------------------------------------------------------------------
// The film pass's uniforms, cast as they are uploaded.
//---------------------------------------------------------------------------
struct FilmUniforms
{
	int sizeX, sizeY, format, holes, leakEdge;
	float filmWidth, mmPerPixel, gate[ 4 ], crop[ 4 ], bandA[ 2 ], bandB[ 2 ], stripLeft, textSize[ 2 ], glyphsPerMm;
	float textExposure, crossover[ 3 ][ 3 ], speed[ 3 ], fogExposure[ 3 ];
	float leakExposure, leakSpread, leakWeights[ 3 ];
	float filterSteps;

	FilmUniforms( const Uniforms& u, const TextMips& text )
	{
		filterSteps = u.filterBits > 0 ? std::ldexp( 1.0f, u.filterBits )
		                               : ( u.filterBits < 0 ? -std::ldexp( 1.0f, -u.filterBits ) : 0.0f );
		const frame::Geometry& g = u.geometry;
		sizeX       = u.width;
		sizeY       = u.height;
		format      = g.format;
		holes       = g.holes ? 1 : 0;
		filmWidth   = static_cast< float >( g.filmWidthMm );
		mmPerPixel  = static_cast< float >( g.mmPerPixel );
		for( int i = 0; i < 4; ++i )
		{
			gate[ i ] = static_cast< float >( g.gate[ i ] );
			crop[ i ] = static_cast< float >( g.crop[ i ] );
		}
		for( int i = 0; i < 2; ++i )
		{
			bandA[ i ] = static_cast< float >( g.bandA[ i ] );
			bandB[ i ] = static_cast< float >( g.bandB[ i ] );
		}
		stripLeft     = static_cast< float >( g.stripLeftMm );
		textSize[ 0 ] = static_cast< float >( text.width[ 0 ] );
		textSize[ 1 ] = static_cast< float >( text.height[ 0 ] );
		glyphsPerMm   = static_cast< float >( frame::kGlyphRowsPerMm );
		textExposure  = u.textExposure;
		for( int r = 0; r < 3; ++r )
		{
			for( int c = 0; c < 3; ++c )
				crossover[ r ][ c ] = static_cast< float >( model::kCrossover[ r ][ c ] );
			speed[ r ]       = u.speed[ r ];
			fogExposure[ r ] = u.fogExposure[ r ];
			leakWeights[ r ] = u.leakWeights[ r ];
		}
		leakExposure = static_cast< float >( u.leakExposure );
		leakEdge     = u.leakEdge;
		leakSpread   = u.leakSpread;
	}
};

constexpr float kPerfPitch  = 4.75f;
constexpr float kPerfWidth  = 2.794f;
constexpr float kPerfHeight = 1.981f;
constexpr float kPerfEdge   = 2.01f;
constexpr float kPerfRadius = 0.5f;

//= mirrored: kFilmBody decodeSrgb() in Shaders.cpp
float decodeSrgb( float v )
{
	const float lo = v / 12.92f;
	const float hi = std::pow( std::max( ( v + 0.055f ) / 1.055f, 0.0f ), 2.4f );
	return v < 0.04045f ? lo : hi;
}

//= mirrored: kFilmBody overlap() in Shaders.cpp
float overlap( float a0, float a1, float b0, float b1 )
{
	return std::max( 0.0f, std::min( a1, b1 ) - std::max( a0, b0 ) );
}

//= mirrored: kFilmBody holeCoverage() in Shaders.cpp
float holeCoverage( const FilmUniforms& f, float fx, float fy )
{
	const float k   = std::floor( fx / kPerfPitch );
	const float cu  = ( k + 0.5f ) * kPerfPitch;
	const float top = kPerfEdge + 0.5f * kPerfHeight;
	const float cv  = fy < 0.5f * f.filmWidth ? top : f.filmWidth - top;
	const float dx  = std::fabs( fx - cu ) - ( 0.5f * kPerfWidth - kPerfRadius );
	const float dy  = std::fabs( fy - cv ) - ( 0.5f * kPerfHeight - kPerfRadius );
	const float mx  = std::max( dx, 0.0f ), my = std::max( dy, 0.0f );
	const float sdf = std::sqrt( mx * mx + my * my ) + std::min( std::max( dx, dy ), 0.0f ) - kPerfRadius;
	return std::clamp( 0.5f - sdf / f.mmPerPixel, 0.0f, 1.0f );
}

//= mirrored: kFilmBody edgePrint() in Shaders.cpp
float edgePrint( const FilmUniforms& f, const TextMips& text, float fx, float fy, const float band[ 2 ], float rowBase )
{
	if( band[ 1 ] <= band[ 0 ] || fy < band[ 0 ] || fy >= band[ 1 ] )
		return 0.0f;
	float row       = ( fy - band[ 0 ] ) / ( band[ 1 ] - band[ 0 ] ) * 7.0f;
	row             = std::clamp( row, 0.5f, 6.5f ) + rowBase;
	const float col = ( fx - f.stripLeft ) * f.glyphsPerMm;

	//textureGrad's explicit gradients, in texels of level 0.
	const float rhoX = f.mmPerPixel * f.glyphsPerMm / f.textSize[ 0 ] * f.textSize[ 0 ];
	const float rhoY = f.mmPerPixel / ( band[ 1 ] - band[ 0 ] ) * 7.0f / f.textSize[ 1 ] * f.textSize[ 1 ];
	return sampleText( text, col / f.textSize[ 0 ], row / f.textSize[ 1 ], rhoX, rhoY, f.filterSteps );
}

//---------------------------------------------------------------------------
// The scan pass's uniforms.
//---------------------------------------------------------------------------
struct ScanUniforms
{
	int sizeX, sizeY, view, invert, autoLevels, grainSites, seed, grainFrame;
	float profileBase[ 3 ], profileGamma, blackPoint, whitePoint, scanGamma, grainAmount, grainCell, mixAmount;

	explicit ScanUniforms( const Uniforms& u )
	{
		sizeX      = u.width;
		sizeY      = u.height;
		view       = u.view;
		invert     = u.invert ? 1 : 0;
		autoLevels = u.autoLevels ? 1 : 0;
		for( int i = 0; i < 3; ++i )
			profileBase[ i ] = static_cast< float >( u.profile.base[ i ] );
		profileGamma = static_cast< float >( u.profile.gamma );
		blackPoint   = u.blackPoint;
		whitePoint   = u.whitePoint;
		scanGamma    = u.scanGamma;
		grainAmount  = u.grainAmount;
		grainCell    = u.grainCell;
		grainSites   = u.grainSites;
		seed         = u.seed;
		grainFrame   = u.grainFrame;
		mixAmount    = u.mix;
	}
};

//= mirrored: kScanBody pcg() in Shaders.cpp
uint32_t pcg( uint32_t v )
{
	const uint32_t state = v * 747796405u + 2891336453u;
	const uint32_t word  = ( ( state >> ( ( state >> 28u ) + 4u ) ) ^ state ) * 277803737u;
	return ( word >> 22u ) ^ word;
}

//= mirrored: kScanBody grain() in Shaders.cpp
vec3 grain( const ScanUniforms& s, int perturb, const vec3& c, int px, int py )
{
	if( s.grainAmount <= 0.0f || s.grainSites <= 0 )
		return c;

	const int top        = s.sizeY - 1 - py;
	const uint32_t cellX = static_cast< uint32_t >( std::floor( static_cast< float >( px ) / s.grainCell ) );
	const uint32_t cellY = static_cast< uint32_t >( std::floor( static_cast< float >( top ) / s.grainCell ) );
	const uint32_t seed  = ( perturb & 128 ) != 0 ? 0u : static_cast< uint32_t >( s.seed );
	const uint32_t base  = pcg( seed + pcg( static_cast< uint32_t >( s.grainFrame ) + pcg( cellX + pcg( cellY ) ) ) );

	vec3 result;
	for( int layer = 0; layer < 3; ++layer )
	{
		const float mean        = std::clamp( c[ layer ], 0.0f, 1.0f );
		const uint32_t threshold = static_cast< uint32_t >( mean * 16777216.0f );
		const uint32_t h        = pcg( base + static_cast< uint32_t >( layer ) * 2654435769u );
		int covered             = 0;
		for( int site = 0; site < s.grainSites; ++site )
			if( ( pcg( h + static_cast< uint32_t >( site ) ) >> 8u ) < threshold )
				++covered;

		float deviation = static_cast< float >( covered ) / static_cast< float >( s.grainSites ) - mean;
		if( ( perturb & 4 ) != 0 )
			deviation *= mean < 1.0f ? 1.0f / std::sqrt( 1.0f - mean ) : 0.0f;
		result[ layer ] = mean + s.grainAmount * deviation;
	}
	return result;
}

//= mirrored: kScanBody expand() in Shaders.cpp
float expand( float p, float R )
{
	if( p <= 0.0f )
		return 0.0f;
	if( p >= 1.0f )
		return 1.0f;
	const float k = R * kLog2Of10;
	if( k < 1e-4f )
		return p;
	return ( std::exp2( k * p ) - 1.0f ) / ( std::exp2( k ) - 1.0f );
}

//= mirrored: kScanBody encodeSrgb() in Shaders.cpp
float encodeSrgb( float x )
{
	x = std::clamp( x, 0.0f, 1.0f );
	return x <= 0.0031308f ? 12.92f * x : 1.055f * std::pow( x, 1.0f / 2.4f ) - 0.055f;
}

/// What the scan hands back. Not mirrored: the GLSL always encodes. For a
/// linear OpenFX clip, the linear value, clamped as the encode clamps it, so
/// that the two outputs differ by the encode and nothing else.
float encodeOutput( float x, bool linearClip )
{
	return linearClip ? std::clamp( x, 0.0f, 1.0f ) : encodeSrgb( x );
}

void serial( int rows, const std::function< void( int, int ) >& body )
{
	body( 0, rows );
}
} // namespace

//---------------------------------------------------------------------------
TextMips BuildText( const Uniforms& u )
{
	const frame::TextStrip strip = frame::BuildText( u.geometry, u.stock ? u.stock->code : "", u.frameNumber, u.edgeText );

	TextMips mips;
	int w = strip.width, h = strip.height;
	std::vector< float > level( static_cast< size_t >( w ) * h );
	for( size_t i = 0; i < level.size(); ++i )
		level[ i ] = static_cast< float >( strip.pixels[ i ] ) / 255.0f;
	mips.level.push_back( level );
	mips.width.push_back( w );
	mips.height.push_back( h );

	while( w > 1 || h > 1 )
	{
		const int nw = std::max( 1, w / 2 ), nh = std::max( 1, h / 2 );
		const std::vector< float >& above = mips.level.back();
		std::vector< float > next( static_cast< size_t >( nw ) * nh );
		for( int y = 0; y < nh; ++y )
			for( int x = 0; x < nw; ++x )
			{
				const int x0 = std::min( 2 * x, w - 1 ), x1 = std::min( 2 * x + 1, w - 1 );
				const int y0 = std::min( 2 * y, h - 1 ), y1 = std::min( 2 * y + 1, h - 1 );
				const float mean = 0.25f * ( above[ static_cast< size_t >( y0 ) * w + x0 ] + above[ static_cast< size_t >( y0 ) * w + x1 ]
				                             + above[ static_cast< size_t >( y1 ) * w + x0 ] + above[ static_cast< size_t >( y1 ) * w + x1 ] );
				next[ static_cast< size_t >( y ) * nw + x ] = std::round( mean * 255.0f ) / 255.0f;
			}
		mips.level.push_back( next );
		mips.width.push_back( nw );
		mips.height.push_back( nh );
		w = nw;
		h = nh;
	}
	return mips;
}

//---------------------------------------------------------------------------
//= mirrored: kFilmBody main() in Shaders.cpp
void Film( const Uniforms& u, const float* picture, const TextMips& text, float* film, int y0, int y1 )
{
	const FilmUniforms f( u, text );
	const Model m( u );
	const int width  = f.sizeX;
	const int height = f.sizeY;

	for( int y = y0; y < y1; ++y )
	{
		const float fragY = static_cast< float >( y ) + 0.5f;
		const float topY  = static_cast< float >( height ) - fragY;//pixel centre, top-down

		for( int x = 0; x < width; ++x )
		{
			const float fragX = static_cast< float >( x ) + 0.5f;

			vec3 scene;
			float gate     = 1.0f;
			float filmPart = 1.0f;
			float latent   = 0.0f;

			if( f.format == 0 )
			{
				//The picture is the frame: exactly this texel.
				const float* p = picture + ( static_cast< size_t >( y ) * width + x ) * 4;
				scene          = vec3{ p[ 0 ], p[ 1 ], p[ 2 ] };
			}
			else
			{
				const float fx = ( fragX - 0.5f * static_cast< float >( width ) ) * f.mmPerPixel;
				const float fy = topY * f.mmPerPixel;
				const float h  = 0.5f * f.mmPerPixel;

				gate = overlap( fx - h, fx + h, f.gate[ 0 ], f.gate[ 2 ] ) / f.mmPerPixel
				       * overlap( fy - h, fy + h, f.gate[ 1 ], f.gate[ 3 ] ) / f.mmPerPixel;

				//Four bilinear taps a quarter of the pixel's footprint either
				//side of its centre: a box over the footprint.
				const float inX  = std::clamp( ( fx - f.gate[ 0 ] ) / ( f.gate[ 2 ] - f.gate[ 0 ] ), 0.0f, 1.0f );
				const float inY  = std::clamp( ( fy - f.gate[ 1 ] ) / ( f.gate[ 3 ] - f.gate[ 1 ] ), 0.0f, 1.0f );
				const float srcX = f.crop[ 0 ] + inX * f.crop[ 2 ];
				const float srcY = f.crop[ 1 ] + inY * f.crop[ 3 ];
				const float qx   = 0.25f * ( f.mmPerPixel / ( f.gate[ 2 ] - f.gate[ 0 ] ) * f.crop[ 2 ] );
				const float qy   = 0.25f * ( f.mmPerPixel / ( f.gate[ 3 ] - f.gate[ 1 ] ) * f.crop[ 3 ] );
				const vec3 a     = bilinear3( picture, width, height, srcX - qx, 1.0f - ( srcY - qy ), f.filterSteps );
				const vec3 b     = bilinear3( picture, width, height, srcX + qx, 1.0f - ( srcY - qy ), f.filterSteps );
				const vec3 c     = bilinear3( picture, width, height, srcX - qx, 1.0f - ( srcY + qy ), f.filterSteps );
				const vec3 d     = bilinear3( picture, width, height, srcX + qx, 1.0f - ( srcY + qy ), f.filterSteps );
				for( int k = 0; k < 3; ++k )
					scene[ k ] = 0.25f * ( a[ k ] + b[ k ] + c[ k ] + d[ k ] );

				if( f.holes != 0 && ( u.perturb & 64 ) == 0 )
					filmPart = 1.0f - holeCoverage( f, fx, fy );

				latent = std::max( edgePrint( f, text, fx, fy, f.bandA, 0.0f ), edgePrint( f, text, fx, fy, f.bandB, 7.0f ) );
			}

			//Not mirrored: a linear OpenFX clip is light already. The GLSL
			//always decodes, because Resolume's input is always encoded.
			const vec3 decoded = u.linearClip ? scene : vec3{ decodeSrgb( scene.x ), decodeSrgb( scene.y ), decodeSrgb( scene.z ) };
			vec3 light;
			for( int r = 0; r < 3; ++r )
				light[ r ] = f.crossover[ r ][ 0 ] * decoded[ 0 ] + f.crossover[ r ][ 1 ] * decoded[ 1 ] + f.crossover[ r ][ 2 ] * decoded[ 2 ];

			vec3 H;
			for( int i = 0; i < 3; ++i )
				H[ i ] = f.speed[ i ] * light[ i ] * gate + f.fogExposure[ i ] + f.textExposure * latent;

			//The leak: an exposure through the base at one edge.
			vec3 leak;
			if( f.leakExposure > 0.0f )
			{
				const float heightF = static_cast< float >( height );
				float d, along;
				if( f.leakEdge == 0 )      { d = fragX / heightF;                                    along = topY / heightF; }
				else if( f.leakEdge == 1 ) { d = ( static_cast< float >( width ) - fragX ) / heightF; along = topY / heightF; }
				else if( f.leakEdge == 2 ) { d = topY / heightF;                                     along = fragX / static_cast< float >( width ); }
				else                       { d = ( heightF - topY ) / heightF;                       along = fragX / static_cast< float >( width ); }
				const float t       = ( along - 0.5f ) / 0.3f;
				const float lateral = 0.55f + 0.45f * std::exp( -t * t );
				const float falloff = f.leakExposure * std::exp( -d / f.leakSpread ) * lateral;
				for( int i = 0; i < 3; ++i )
					leak[ i ] = falloff * ( ( u.perturb & 256 ) != 0 ? f.leakWeights[ 2 - i ] : f.leakWeights[ i ] );
			}

			vec3 c;
			if( ( u.perturb & 8 ) != 0 )
			{
				//Negative control: the leak added as coverage, after the curve.
				vec3 logH;
				for( int i = 0; i < 3; ++i )
					logH[ i ] = std::log2( std::max( H[ i ], 1e-12f ) ) * kLog10Of2;
				c                   = m.coverage( logH );
				const float shoulder = std::exp2( ( m.toe + m.latitude ) * kLog2Of10 );
				for( int i = 0; i < 3; ++i )
					c[ i ] += leak[ i ] / shoulder;
			}
			else
			{
				vec3 logH;
				for( int i = 0; i < 3; ++i )
					logH[ i ] = std::log2( std::max( H[ i ] + leak[ i ], 1e-12f ) ) * kLog10Of2;
				c = m.coverage( logH );
			}

			float* out = film + ( static_cast< size_t >( y ) * width + x ) * 4;
			out[ 0 ]   = c.x;
			out[ 1 ]   = c.y;
			out[ 2 ]   = c.z;
			out[ 3 ]   = filmPart + ( gate > 0.5f ? 2.0f : 0.0f );
		}
	}
}

//---------------------------------------------------------------------------
//= mirrored: kBlocksBody main() and kLevelsBody main() in Shaders.cpp, with
//  Prime always set -- see Render.h.
Levels MeasureLevels( const Uniforms& u, const float* film )
{
	const Model m( u );
	const int width = u.width, height = u.height;
	const float sizeX = static_cast< float >( width ), sizeY = static_cast< float >( height );
	const float gridX = static_cast< float >( u.gridW ), gridY = static_cast< float >( u.gridH );

	Levels levels;
	vec3 lo{ 1e9f, 1e9f, 1e9f };
	vec3 hi{ -1e9f, -1e9f, -1e9f };

	for( int by = 0; by < u.gridH; ++by )
		for( int bx = 0; bx < u.gridW; ++bx )
		{
			const float loX = static_cast< float >( bx ) * sizeX / gridX, loY = static_cast< float >( by ) * sizeY / gridY;
			const float hiX = static_cast< float >( bx + 1 ) * sizeX / gridX, hiY = static_cast< float >( by + 1 ) * sizeY / gridY;
			const float cellX = ( hiX - loX ) / 8.0f, cellY = ( hiY - loY ) / 8.0f;

			vec3 sum;
			float n = 0.0f;
			for( int j = 0; j < 8; ++j )
				for( int i = 0; i < 8; ++i )
				{
					const int px   = std::clamp( static_cast< int >( std::floor( loX + ( static_cast< float >( i ) + 0.5f ) * cellX ) ), 0, width - 1 );
					const int py   = std::clamp( static_cast< int >( std::floor( loY + ( static_cast< float >( j ) + 0.5f ) * cellY ) ), 0, height - 1 );
					const float* f = film + ( static_cast< size_t >( py ) * width + px ) * 4;
					if( f[ 3 ] < 1.5f )
						continue;
					const vec3 d = m.channelDensity( m.dyeAmount( vec3{ f[ 0 ], f[ 1 ], f[ 2 ] } ) );
					for( int k = 0; k < 3; ++k )
						sum[ k ] += d[ k ];
					n += 1.0f;
				}

			if( !( n > 0.0f ) )
				continue;
			for( int k = 0; k < 3; ++k )
			{
				const float mean = sum[ k ] / n;
				lo[ k ]          = std::min( lo[ k ], mean );
				hi[ k ]          = std::max( hi[ k ], mean );
			}
			levels.measured = true;
		}

	if( levels.measured )
		for( int k = 0; k < 3; ++k )
		{
			levels.least[ k ] = lo[ k ];
			levels.most[ k ]  = hi[ k ];
		}
	return levels;
}

//---------------------------------------------------------------------------
//= mirrored: kScanBody main() in Shaders.cpp
void Scan( const Uniforms& u, const float* film, const Levels& levels, const float* source, float* out, int y0, int y1 )
{
	const ScanUniforms s( u );
	const Model m( u );
	const int width = s.sizeX;

	for( int y = y0; y < y1; ++y )
		for( int x = 0; x < width; ++x )
		{
			const size_t at  = ( static_cast< size_t >( y ) * width + x ) * 4;
			const float* f   = film + at;
			const float part = f[ 3 ] >= 1.5f ? f[ 3 ] - 2.0f : f[ 3 ];//film, not hole

			const vec3 D = m.channelDensity( m.dyeAmount( grain( s, u.perturb, vec3{ f[ 0 ], f[ 1 ], f[ 2 ] }, x, y ) ) );
			vec3 T, measured;
			for( int k = 0; k < 3; ++k )
			{
				T[ k ]        = part * std::exp2( -D[ k ] * kLog2Of10 ) + ( 1.0f - part );
				measured[ k ] = part >= 1.0f ? D[ k ] : -std::log2( T[ k ] ) * kLog10Of2;
			}

			vec3 linear;
			if( s.view == 1 )
				linear = T;
			else
			{
				vec3 p, span;
				if( s.autoLevels != 0 )
				{
					for( int k = 0; k < 3; ++k )
					{
						span[ k ] = std::max( levels.most[ k ] - levels.least[ k ], 1e-3f );
						p[ k ]    = s.invert != 0 ? ( measured[ k ] - levels.least[ k ] ) / span[ k ]
						                          : ( levels.most[ k ] - measured[ k ] ) / span[ k ];
					}
				}
				else
				{
					const float manual = std::max( s.whitePoint - s.blackPoint, 1e-3f );
					for( int k = 0; k < 3; ++k )
					{
						span[ k ]     = manual;
						const float d = measured[ k ] - s.profileBase[ k ];
						p[ k ]        = ( d - s.blackPoint ) / span[ k ];
						if( s.invert == 0 )
							p[ k ] = 1.0f - p[ k ];
					}
				}

				for( int k = 0; k < 3; ++k )
					linear[ k ] = expand( p[ k ], span[ k ] / s.profileGamma * s.scanGamma );
			}

			const float scanned[ 4 ] = { encodeOutput( linear.x, u.linearClip ), encodeOutput( linear.y, u.linearClip ),
			                             encodeOutput( linear.z, u.linearClip ), 1.0f };
			float* o                 = out + at;
			if( s.mixAmount >= 1.0f )
			{
				//Exactly the scan.
				for( int k = 0; k < 4; ++k )
					o[ k ] = scanned[ k ];
				continue;
			}
			const float* in = source + at;
			float mixed[ 4 ];
			for( int k = 0; k < 4; ++k )
				mixed[ k ] = in[ k ] * ( 1.0f - s.mixAmount ) + scanned[ k ] * s.mixAmount;
			for( int k = 0; k < 4; ++k )
				o[ k ] = mixed[ k ];
		}
}

//---------------------------------------------------------------------------
void Apply( const Uniforms& u, const float* input, float* output, const Parallel& parallel )
{
	const Parallel& run = parallel ? parallel : Parallel( serial );
	const TextMips text = BuildText( u );

	std::vector< float > film( static_cast< size_t >( u.width ) * u.height * 4 );
	run( u.height, [ & ]( int y0, int y1 ) { Film( u, input, text, film.data(), y0, y1 ); } );

	const Levels levels = u.autoActive ? MeasureLevels( u, film.data() ) : Levels{};

	run( u.height, [ & ]( int y0, int y1 ) { Scan( u, film.data(), levels, input, output, y0, y1 ); } );
}

float DecodeSrgb( float v )
{
	return decodeSrgb( v );
}

float EncodeSrgb( float x )
{
	return encodeSrgb( x );
}

} // namespace rebate::render
