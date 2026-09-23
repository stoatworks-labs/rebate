/**
	rbtest -- render Rebate offline, and read the film back out of it.

	Where the straight line of a characteristic curve lies, where it bends,
	whether the orange mask cancels, and how grain's variance depends on
	density are facts with one right answer each. Every check here renders a
	synthetic scene through the REAL plugin class in a headless GL context,
	reads the pixels back as floats, and measures -- densities out of the
	Negative view, colour out of the Positive view, variances over whole
	frames -- rather than eyeballing.

		rbtest --out /tmp/frame.png     a picture, on the test card
		rbtest --list                   every parameter, its kind and default
		rbtest --curve                  a grey wedge reads back D( log H ): the
		                                straight line's slope is gamma, and it
		                                meets fog at the toe and Dmax at the
		                                shoulder where the curve says
		rbtest --mask                   C-41 with the mask inverts a neutral wedge
		                                to neutral; without it, it does not
		rbtest --grain                  density variance is c ( 1 - c ) / N,
		                                peaks at c = 0.5, is zero at both ends
		rbtest --push                   +1 stop of push raises gamma x 1.15 and
		                                fog by 0.03
		rbtest --leak                   a leak's density follows the curve and
		                                saturates, and it is warm
		rbtest --cross                  E-6 film through C-41 goes warm, most in
		                                the shadows, where the model says
		rbtest --rebate                 holes black, border unexposed, edge print
		                                lighter than the border
		rbtest --seed                   grain is bit-identical for a seed and a
		                                frame, and a resize does not reshuffle it
		rbtest --resize                 the scanner's levels survive a resize
		rbtest --negative               every check above can FAIL
		rbtest --bench                  the render cost
		rbtest --dump-shaders DIR       the exact GLSL the plugin compiles
		rbtest --pipe                   raw frames in, raw frames out

	Every check drives a synthetic 60 fps clock through `SetTime`. Run each at
	two rasters at least -- the one you develop at and 320x180, which is what
	CI uses; a check that holds at one raster only is a check that was fitted
	to it. AGENTS.md has one line per check on where each tolerance comes
	from.

	`--script` is a plain text file of `frame  Parameter Name  value` lines,
	the same format as the fleet's other harnesses. `--pipe` takes the fleet's
	frame format:

		ffmpeg -i in.mov -f rawvideo -pix_fmt rgba - \
		  | rbtest --pipe --size 1920x1080 [--script cues.txt] \
		  | ffmpeg -f rawvideo -pix_fmt rgba -s 1920x1080 -i - out.mov
*/

#include "Controls.h"
#include "Frame.h"
#include "Model.h"
#include "Rebate.h"
#include "Shaders.h"

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <zlib.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

using namespace rebate;

namespace
{
//---------------------------------------------------------------------------
// A PNG writer. zlib ships with the OS, so this is a few chunk headers and a
// CRC rather than a dependency.
//---------------------------------------------------------------------------
void putU32( std::vector< unsigned char >& out, uint32_t value )
{
	out.push_back( static_cast< unsigned char >( value >> 24 ) );
	out.push_back( static_cast< unsigned char >( value >> 16 ) );
	out.push_back( static_cast< unsigned char >( value >> 8 ) );
	out.push_back( static_cast< unsigned char >( value ) );
}

void putChunk( std::vector< unsigned char >& out, const char* type, const std::vector< unsigned char >& data )
{
	putU32( out, static_cast< uint32_t >( data.size() ) );
	const size_t start = out.size();
	out.insert( out.end(), type, type + 4 );
	out.insert( out.end(), data.begin(), data.end() );
	uLong crc = crc32( 0L, Z_NULL, 0 );
	crc       = crc32( crc, out.data() + start, static_cast< uInt >( 4 + data.size() ) );
	putU32( out, static_cast< uint32_t >( crc ) );
}

bool writePng( const std::string& path, int width, int height, const std::vector< unsigned char >& rgba )
{
	std::vector< unsigned char > raw;
	raw.reserve( static_cast< size_t >( height ) * ( 1 + static_cast< size_t >( width ) * 4 ) );
	for( int y = 0; y < height; ++y )
	{
		raw.push_back( 0 );//filter: none
		const unsigned char* row = rgba.data() + static_cast< size_t >( y ) * width * 4;
		raw.insert( raw.end(), row, row + static_cast< size_t >( width ) * 4 );
	}

	uLongf compressedSize = compressBound( static_cast< uLong >( raw.size() ) );
	std::vector< unsigned char > compressed( compressedSize );
	if( compress2( compressed.data(), &compressedSize, raw.data(), static_cast< uLong >( raw.size() ), 6 ) != Z_OK )
		return false;
	compressed.resize( compressedSize );

	std::vector< unsigned char > png = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };

	std::vector< unsigned char > ihdr;
	putU32( ihdr, static_cast< uint32_t >( width ) );
	putU32( ihdr, static_cast< uint32_t >( height ) );
	ihdr.push_back( 8 );//bit depth
	ihdr.push_back( 6 );//truecolour with alpha
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	putChunk( png, "IHDR", ihdr );
	putChunk( png, "IDAT", compressed );
	putChunk( png, "IEND", {} );

	FILE* file = fopen( path.c_str(), "wb" );
	if( file == nullptr )
		return false;
	const size_t written = fwrite( png.data(), 1, png.size(), file );
	fclose( file );
	return written == png.size();
}

//---------------------------------------------------------------------------
// Sources. Rows are top-first in every source, the way a picture is, and
// flipped on the way into GL. The checks read back through the same flip,
// so "row 0" is the top of the picture everywhere in this file.
//---------------------------------------------------------------------------

/// The test card: a sky gradient over a warm ground, a row of greys from
/// black to white, six colour patches, a skin tone, a deep shadow and a
/// specular highlight, and a disc on a slow Lissajous path so consecutive
/// frames differ. Everything a film look acts on differently.
std::vector< unsigned char > buildCard( int width, int height, int frame )
{
	std::vector< unsigned char > card( static_cast< size_t >( width ) * height * 4 );

	const float w = static_cast< float >( width );
	const float h = static_cast< float >( height );
	const float t = static_cast< float >( frame );

	const float discX = 0.5f * w + 0.30f * w * std::sin( t * 0.05f );
	const float discY = 0.42f * h + 0.12f * h * std::sin( t * 0.037f + 1.1f );
	const float discR = 0.07f * h;

	for( int y = 0; y < height; ++y )
	{
		for( int x = 0; x < width; ++x )
		{
			const float u = ( static_cast< float >( x ) + 0.5f ) / w;
			const float v = ( static_cast< float >( y ) + 0.5f ) / h;

			//Sky above, ground below.
			float r, g, b;
			if( v < 0.55f )
			{
				const float k = v / 0.55f;
				r             = 0.25f + 0.45f * k;
				g             = 0.45f + 0.35f * k;
				b             = 0.85f - 0.05f * k;
			}
			else
			{
				r = 0.42f;
				g = 0.33f;
				b = 0.22f;
			}

			if( v > 0.84f )
			{
				//Eleven greys, black to white.
				const int step = std::min( 10, static_cast< int >( u * 11.0f ) );
				r = g = b = static_cast< float >( step ) / 10.0f;
			}
			else if( v > 0.66f && v < 0.80f )
			{
				static const float patches[ 8 ][ 3 ] = {
					{ 0.75f, 0.12f, 0.10f }, { 0.20f, 0.60f, 0.20f }, { 0.12f, 0.20f, 0.75f },
					{ 0.10f, 0.65f, 0.70f }, { 0.70f, 0.15f, 0.60f }, { 0.90f, 0.80f, 0.15f },
					{ 0.87f, 0.64f, 0.52f },//skin
					{ 0.03f, 0.03f, 0.03f },//deep shadow
				};
				const int patch = std::min( 7, static_cast< int >( u * 8.0f ) );
				r               = patches[ patch ][ 0 ];
				g               = patches[ patch ][ 1 ];
				b               = patches[ patch ][ 2 ];
			}

			//A specular highlight, top right.
			const float hx = u - 0.85f, hy = v - 0.15f;
			if( hx * hx + hy * hy < 0.0015f )
				r = g = b = 1.0f;

			const float px   = static_cast< float >( x ) + 0.5f;
			const float dx   = px - discX;
			const float dy   = ( static_cast< float >( y ) + 0.5f ) - discY;
			const float dist = std::sqrt( dx * dx + dy * dy );
			if( dist < discR )
			{
				const float edge = std::min( 1.0f, ( discR - dist ) / ( discR * 0.2f ) );
				r                = r + ( 0.95f - r ) * edge;
				g                = g + ( 0.55f - g ) * edge;
				b                = b + ( 0.15f - b ) * edge;
			}

			const size_t i = ( static_cast< size_t >( y ) * width + x ) * 4;
			card[ i + 0 ]  = static_cast< unsigned char >( std::clamp( r, 0.0f, 1.0f ) * 255.0f + 0.5f );
			card[ i + 1 ]  = static_cast< unsigned char >( std::clamp( g, 0.0f, 1.0f ) * 255.0f + 0.5f );
			card[ i + 2 ]  = static_cast< unsigned char >( std::clamp( b, 0.0f, 1.0f ) * 255.0f + 0.5f );
			card[ i + 3 ]  = 255;
		}
	}

	return card;
}

std::vector< unsigned char > buildFlat( int width, int height, int code )
{
	std::vector< unsigned char > image( static_cast< size_t >( width ) * height * 4 );
	const unsigned char value = static_cast< unsigned char >( std::clamp( code, 0, 255 ) );
	for( size_t i = 0; i < image.size(); i += 4 )
	{
		image[ i + 0 ] = image[ i + 1 ] = image[ i + 2 ] = value;
		image[ i + 3 ]                                   = 255;
	}
	return image;
}

/// sRGB, both ways, in double. The plugin decodes its input and encodes its
/// output with the same piecewise function in float.
double encodeSrgb( double x )
{
	return x <= 0.0031308 ? 12.92 * x : 1.055 * std::pow( x, 1.0 / 2.4 ) - 0.055;
}

double decodeSrgb( double v )
{
	return v <= 0.04045 ? v / 12.92 : std::pow( ( v + 0.055 ) / 1.055, 2.4 );
}

/// A float source: vertical steps of scene log exposure x_i (linear light
/// 10^x_i, sRGB-encoded). The values run past 1.0 on purpose: a scene is not
/// limited to the display's white, and a float input carries it exactly.
std::vector< float > buildWedge( int width, int height, const std::vector< double >& logs )
{
	std::vector< float > image( static_cast< size_t >( width ) * height * 4 );
	const int steps = static_cast< int >( logs.size() );
	for( int x = 0; x < width; ++x )
	{
		const int step    = std::min( steps - 1, x * steps / width );
		const float value = static_cast< float >( logs[ step ] < -30.0 ? 0.0 : encodeSrgb( std::pow( 10.0, logs[ step ] ) ) );
		for( int y = 0; y < height; ++y )
		{
			float* p = image.data() + ( static_cast< size_t >( y ) * width + x ) * 4;
			p[ 0 ] = p[ 1 ] = p[ 2 ] = value;
			p[ 3 ]                   = 1.0f;
		}
	}
	return image;
}

/// A flat float source at scene log exposure x (-inf, or below -30, is black).
std::vector< float > buildFlatLog( int width, int height, double logH )
{
	return buildWedge( width, height, std::vector< double >{ logH } );
}

/// The centre column of step i of an n-step wedge on a raster `width` wide.
int stepColumn( int i, int steps, int width )
{
	const int x0 = ( i * width + steps - 1 ) / steps;//first column x with x * steps / width == i
	int x1       = x0;
	while( x1 + 1 < width && ( x1 + 1 ) * steps / width == i )
		++x1;
	return ( x0 + x1 ) / 2;
}

//---------------------------------------------------------------------------
// GL plumbing.
//---------------------------------------------------------------------------
CGLContextObj createContext()
{
	const CGLPixelFormatAttribute accelerated[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAAccelerated,
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	const CGLPixelFormatAttribute software[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};

	CGLPixelFormatObj format = nullptr;
	GLint formatCount        = 0;
	if( CGLChoosePixelFormat( accelerated, &format, &formatCount ) != kCGLNoError || format == nullptr )
	{
		if( CGLChoosePixelFormat( software, &format, &formatCount ) != kCGLNoError || format == nullptr )
			return nullptr;
	}

	CGLContextObj context = nullptr;
	const CGLError error  = CGLCreateContext( format, nullptr, &context );
	CGLDestroyPixelFormat( format );
	if( error != kCGLNoError )
		return nullptr;

	CGLSetCurrentContext( context );
	return context;
}

GLuint makeTexture( int width, int height, GLint internalFormat, GLenum type, const void* pixels )
{
	GLuint texture = 0;
	glGenTextures( 1, &texture );
	glBindTexture( GL_TEXTURE_2D, texture );
	glTexImage2D( GL_TEXTURE_2D, 0, internalFormat, width, height, 0, GL_RGBA, type, pixels );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );
	return texture;
}

GLuint makeFramebuffer( GLuint texture )
{
	GLuint fbo = 0;
	glGenFramebuffers( 1, &fbo );
	glBindFramebuffer( GL_FRAMEBUFFER, fbo );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0 );
	return fbo;
}

template< typename T >
std::vector< T > flipRows( const std::vector< T >& image, int width, int height )
{
	std::vector< T > flipped( image.size() );
	const size_t stride = static_cast< size_t >( width ) * 4;
	for( int y = 0; y < height; ++y )
		std::copy( image.begin() + static_cast< long >( ( height - 1 - y ) * stride ),
		           image.begin() + static_cast< long >( ( height - y ) * stride ),
		           flipped.begin() + static_cast< long >( y * stride ) );
	return flipped;
}

//---------------------------------------------------------------------------
// Parameters by display name, so the automation reads as English.
//---------------------------------------------------------------------------
struct NamedParameter
{
	std::string name;
	unsigned int index;
	unsigned int type;
	float value;
	float low;
	float high;
};

const char* kindName( const NamedParameter& p )
{
	if( p.index >= Rebate::PT_ABOUT_FIRST )
		return "about";
	switch( p.type )
	{
	case FF_TYPE_BOOLEAN: return "bool";
	case FF_TYPE_EVENT: return "event";
	case FF_TYPE_OPTION: return "option";
	case FF_TYPE_INTEGER: return "integer";
	case FF_TYPE_BUFFER: return "buffer";
	case FF_TYPE_TEXT: return "text";
	case FF_TYPE_STANDARD: return "standard";
	default: return "other";
	}
}

std::vector< NamedParameter > listParameters( Rebate& plugin )
{
	std::vector< NamedParameter > list;
	for( unsigned int i = 0; i < Rebate::PT_COUNT; ++i )
	{
		const char* const name = plugin.GetParamName( i );
		NamedParameter p;
		p.name  = name ? name : "?";
		p.index = i;
		p.type  = plugin.GetParamType( i );
		p.value = plugin.GetFloatParameter( i );
		p.low   = 0.0f;
		p.high  = 1.0f;
		//An option's range reads back 0..1 whatever its element count, so
		//the element count is the range. An integer's real range is real.
		if( p.type == FF_TYPE_OPTION )
			p.high = static_cast< float >( std::max( 1u, plugin.GetNumParamElements( i ) ) - 1u );
		else if( p.type == FF_TYPE_INTEGER )
		{
			const RangeStruct range = plugin.GetParamRange( i );
			p.low                   = range.min;
			p.high                  = range.max;
		}
		list.push_back( p );
	}
	return list;
}

int indexOfParameter( Rebate& plugin, const std::string& name )
{
	for( const NamedParameter& p : listParameters( plugin ) )
		if( p.name == name )
			return static_cast< int >( p.index );
	return -1;
}

bool applySetting( Rebate& plugin, const std::string& assignment, std::string& error )
{
	const size_t equals = assignment.find( '=' );
	if( equals == std::string::npos )
	{
		error = "expected Name=Value";
		return false;
	}

	const std::string name  = assignment.substr( 0, equals );
	const std::string value = assignment.substr( equals + 1 );
	const int index         = indexOfParameter( plugin, name );
	if( index < 0 )
	{
		error = "no parameter called '" + name + "'";
		return false;
	}

	plugin.SetFloatParameter( static_cast< unsigned int >( index ), std::strtof( value.c_str(), nullptr ) );
	return true;
}

bool set( Rebate& plugin, const char* name, float value )
{
	std::string error;
	char buffer[ 64 ];
	std::snprintf( buffer, sizeof( buffer ), "%.9g", value );
	if( applySetting( plugin, std::string( name ) + "=" + buffer, error ) )
		return true;
	std::fprintf( stderr, "%s\n", error.c_str() );
	return false;
}

//---------------------------------------------------------------------------
// A session: the plugin, its input and output, and the clock that drives it.
//---------------------------------------------------------------------------
struct Session
{
	Rebate plugin;
	int width  = 0;
	int height = 0;
	double fps = 60.0;
	/// Render into an RGBA32F framebuffer rather than RGBA8. The physics
	/// checks read float so their tolerances can be float-derived.
	bool floatOutput = true;

	GLuint sourceTexture = 0;
	GLuint outputTexture = 0;
	GLuint outputFBO     = 0;
	FFGLTextureStruct inputStruct  = {};
	FFGLTextureStruct* inputs[ 1 ] = { nullptr };
	ProcessOpenGLStruct process    = {};

	/// The source is always an RGBA32F texture: 8-bit sources upload into it
	/// normalised, float sources (the wedges) exactly.
	void makeTargets()
	{
		sourceTexture = makeTexture( width, height, GL_RGBA32F, GL_FLOAT, nullptr );
		outputTexture = floatOutput ? makeTexture( width, height, GL_RGBA32F, GL_FLOAT, nullptr )
		                            : makeTexture( width, height, GL_RGBA8, GL_UNSIGNED_BYTE, nullptr );
		outputFBO     = makeFramebuffer( outputTexture );

		inputStruct.Width = inputStruct.HardwareWidth = static_cast< FFUInt32 >( width );
		inputStruct.Height = inputStruct.HardwareHeight = static_cast< FFUInt32 >( height );
		inputStruct.Handle                              = sourceTexture;
		inputs[ 0 ]                                     = &inputStruct;

		process.numInputTextures = 1;
		process.inputTextures    = inputs;
		process.HostFBO          = outputFBO;
	}

	void dropTargets()
	{
		if( outputFBO )
			glDeleteFramebuffers( 1, &outputFBO );
		if( outputTexture )
			glDeleteTextures( 1, &outputTexture );
		if( sourceTexture )
			glDeleteTextures( 1, &sourceTexture );
		outputFBO = outputTexture = sourceTexture = 0;
	}

	bool begin( int w, int h )
	{
		width  = w;
		height = h;

		FFGLViewportStruct viewport = {};
		viewport.width              = static_cast< FFUInt32 >( width );
		viewport.height             = static_cast< FFUInt32 >( height );
		if( plugin.InitGL( &viewport ) != FF_SUCCESS )
		{
			std::fprintf( stderr, "InitGL failed -- see the diagnostics log for which shader\n" );
			return false;
		}
		makeTargets();
		return true;
	}

	/// What a host does when the clip or the composition changes size: hand
	/// the SAME instance a differently sized input. No DeInitGL.
	void resize( int w, int h )
	{
		dropTargets();
		width  = w;
		height = h;
		makeTargets();
	}

	bool renderAt( int frame )
	{
		//A synthetic clock, and it has to be synthetic: left to the wall
		//clock the harness renders a hundred frames in a few milliseconds and
		//the grain never moves on. The unit is declared, not inferred.
		plugin.SetClockScaleForTest( 1.0 );
		plugin.SetTime( static_cast< double >( frame ) / fps );

		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glViewport( 0, 0, width, height );
		glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
		glClear( GL_COLOR_BUFFER_BIT );
		const bool ok = plugin.ProcessOpenGL( &process ) == FF_SUCCESS;
		if( !ok )
			std::fprintf( stderr, "ProcessOpenGL failed on frame %d\n", frame );
		return ok;
	}

	/// Render one frame of 8-bit `pixels` (top row first) at frame `frame`.
	bool render( int frame, const std::vector< unsigned char >& pixels )
	{
		const std::vector< unsigned char > flipped = flipRows( pixels, width, height );
		glBindTexture( GL_TEXTURE_2D, sourceTexture );
		glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, flipped.data() );
		glBindTexture( GL_TEXTURE_2D, 0 );
		return renderAt( frame );
	}

	/// Render one frame of float `pixels` (top row first).
	bool render( int frame, const std::vector< float >& pixels )
	{
		const std::vector< float > flipped = flipRows( pixels, width, height );
		glBindTexture( GL_TEXTURE_2D, sourceTexture );
		glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_FLOAT, flipped.data() );
		glBindTexture( GL_TEXTURE_2D, 0 );
		return renderAt( frame );
	}

	/// The output, top row first, 8-bit.
	std::vector< unsigned char > readBack()
	{
		std::vector< unsigned char > pixels( static_cast< size_t >( width ) * height * 4 );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data() );
		return flipRows( pixels, width, height );
	}

	/// The output, top row first, as floats.
	std::vector< float > readBackFloat()
	{
		std::vector< float > pixels( static_cast< size_t >( width ) * height * 4 );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( 0, 0, width, height, GL_RGBA, GL_FLOAT, pixels.data() );
		return flipRows( pixels, width, height );
	}

	void end()
	{
		plugin.DeInitGL();
		dropTargets();
	}
};

//---------------------------------------------------------------------------
// The baseline every check starts from: the Portrait 400 negative, normally
// exposed and developed, no age, C-41 with its mask, NO grain, no leak, the
// Negative or Positive view as the check wants, a MANUAL scan, the Full
// format and no mix. Each check then moves the one or two things it is about.
//---------------------------------------------------------------------------
constexpr int kPortrait = 1;
constexpr float kBlackPoint = 0.05f;
constexpr float kWhitePoint = 1.25f;

void baseline( Rebate& p )
{
	set( p, "Exposure", controls::ExposureParam( 0.0f ) );
	set( p, "Stock", static_cast< float >( kPortrait ) );
	set( p, "Push", controls::PushParam( 0.0f ) );
	set( p, "Age", 0.0f );
	set( p, "Process", 0.0f );
	set( p, "Mask On", 1.0f );
	set( p, "Grain Amount", 0.0f );
	set( p, "Grain Size", 0.0f );
	set( p, "Grain Seed", 1.0f );
	set( p, "Leak Amount", 0.0f );
	set( p, "Leak Edge", 0.0f );
	set( p, "Leak Warmth", 1.0f );
	set( p, "Leak Spread", controls::LeakSpreadParam( 0.3f ) );
	set( p, "View", 0.0f );
	set( p, "Auto Levels", 0.0f );
	set( p, "Black Point", controls::BlackPointParam( kBlackPoint ) );
	set( p, "White Point", controls::WhitePointParam( kWhitePoint ) );
	set( p, "Scanner Gamma", controls::ScannerGammaParam( 1.0f ) );
	set( p, "Format", 0.0f );
	set( p, "Edge Text On", 1.0f );
	set( p, "Frame Number", 12.0f );
	set( p, "Mix", 1.0f );
}

using Settings = std::vector< std::pair< const char*, float > >;

/// Render `frames` frames of a float source through a fresh instance and read
/// the last one back as floats.
bool renderFloat( int width, int height, const Settings& settings, const std::vector< float >& source, int perturb,
                  std::vector< float >& out, int frames = 3 )
{
	Session session;
	baseline( session.plugin );
	for( const auto& s : settings )
		if( !set( session.plugin, s.first, s.second ) )
			return false;
	session.plugin.SetPerturbForTest( perturb );
	if( !session.begin( width, height ) )
		return false;
	for( int frame = 0; frame < frames; ++frame )
		if( !session.render( frame, source ) )
		{
			session.end();
			return false;
		}
	out = session.readBackFloat();
	session.end();
	return true;
}

//---------------------------------------------------------------------------
// Reading pixels.
//---------------------------------------------------------------------------
const float* pixelAt( const std::vector< float >& image, int width, int x, int y )
{
	return image.data() + ( static_cast< size_t >( y ) * width + x ) * 4;
}

/// Channel density out of the Negative view: the output is the negative's
/// transmittance on a light box, sRGB-encoded.
double densityOf( double encoded )
{
	return -std::log10( std::max( decodeSrgb( encoded ), 1e-30 ) );
}

const char* verdict( bool ok )
{
	return ok ? "ok" : "FAILED";
}

/// Whether the scanner clipped any channel of this linear rgb: 0 is exact
/// (expand() returns 0 for p <= 0), but white comes back through the sRGB
/// encode as 1 +- 2 ULP, so anything within 1e-5 of 1 is white.
bool clipped( const std::vector< double >& o )
{
	return std::min( { o[ 0 ], o[ 1 ], o[ 2 ] } ) <= 0.0 || std::max( { o[ 0 ], o[ 1 ], o[ 2 ] } ) >= 1.0 - 1e-5;
}

/// One ULP of a float at magnitude m.
double ulp( double m )
{
	return std::ldexp( 1.0, std::ilogb( std::max( std::fabs( m ), 1e-30 ) ) - 23 );
}

/// How far a density read back from the Negative view can be from the one
/// the shader computed. The channel density is a sum of six terms of
/// magnitude <= 4, each correctly rounded (at most half an ULP of 4 each,
/// 1.4e-6); exp2 (GLSL 4.10 §8.2: 3 ULP) and the sRGB encode's pow (inherited
/// from exp2 and log2: ~9 ULP relative), then the harness's decode in double
/// multiplies the pow's relative error by 2.4 -- about 25 ULP of relative
/// error on T, which is 25 x 1.2e-7 x log10(e) = 1.3e-6 in density. 2.7e-6
/// all told; the bound is four times that.
constexpr double kDensityRead = 1e-5;

//---------------------------------------------------------------------------
// The curve: what the harness knows about it is Model.h's constants, never
// a re-typed formula. Everything asserted is measured out of the picture.
//---------------------------------------------------------------------------

/// The slope fraction D'( x ) / gamma is sigma( k( x - toe ) ) - sigma( k( x -
/// shoulder ) ), which lies in [ 1 - e^(-k dt) - e^(-k ds), 1 ] at distances
/// dt, ds from the toe and the shoulder. This is the lower edge's shortfall.
double slopeShortfall( double dt, double ds )
{
	return std::exp( -model::kKnee * dt ) + std::exp( -model::kKnee * ds );
}

/// A float scene exposure wedge from `lo` to `hi` log10 H in `n` steps.
std::vector< double > logRamp( double lo, double hi, int n )
{
	std::vector< double > xs;
	for( int i = 0; i < n; ++i )
		xs.push_back( lo + ( hi - lo ) * i / ( n - 1 ) );
	return xs;
}

/// What a straight-line reading of a wedge found, for one channel.
struct LineReading
{
	double slope      = 0.0;
	double slopeLow   = 0.0;///< the slope must be at least this
	double slopeHigh  = 0.0;///< and at most this
	double floor      = 0.0;///< density of the least exposed step
	double ceiling    = 0.0;///< density of the most exposed step
	double toe        = 0.0;///< where the straight line meets the floor
	double shoulder   = 0.0;///< where it meets the ceiling
	double toeTol     = 0.0;
	double shoulderTol = 0.0;
	int first = -1, last = -1;///< the steps the line was read between
};

/// Read the straight line of one channel of a Negative-view wedge.
///
/// The line is the secant between the first and last steps at least `margin`
/// log units inside the toe and the shoulder. Its slope lies within
/// gamma x [ 1 - shortfall, 1 ] (the mean-value theorem: a secant slope is a
/// value the derivative takes between its ends), plus the read error of two
/// densities over the span.
///
/// Where the extended line meets the floor: the measured density at step i1
/// is the asymptote Floor + gamma( x1 - toe ) plus a residual of at most
/// ( gamma / k ) e^(-k ( x1 - toe )) from the toe's softplus (and the same
/// from the shoulder's), the floor is off the true fog level by
/// ( gamma / k ) e^(-k ( toe - x0 )), and the slope error is carried over the
/// extrapolation distance. The tolerance is the sum of those, divided by
/// the slope, plus the read error -- computed, not chosen.
LineReading readLine( const std::vector< double >& xs, const std::vector< double >& d, const model::Curve& curve,
                      double margin )
{
	LineReading r;
	const double shoulder = curve.toe + curve.latitude;
	for( size_t i = 0; i < xs.size(); ++i )
	{
		if( xs[ i ] >= curve.toe + margin && xs[ i ] <= shoulder - margin )
		{
			if( r.first < 0 )
				r.first = static_cast< int >( i );
			r.last = static_cast< int >( i );
		}
	}
	if( r.first < 0 || r.last <= r.first )
		return r;

	const double x1 = xs[ r.first ], x2 = xs[ r.last ];
	const double g  = curve.gamma;
	const double k  = model::kKnee;
	r.floor         = d.front();
	r.ceiling       = d.back();
	r.slope         = ( d[ r.last ] - d[ r.first ] ) / ( x2 - x1 );

	const double read      = 2.0 * kDensityRead / ( x2 - x1 );
	const double shortfall = std::max( slopeShortfall( x1 - curve.toe, shoulder - x1 ), slopeShortfall( x2 - curve.toe, shoulder - x2 ) );
	r.slopeLow             = g * ( 1.0 - shortfall ) - read;
	r.slopeHigh            = g + read;

	const double slopeErr  = g * shortfall + read;
	const double residual1 = ( g / k ) * ( std::exp( -k * ( x1 - curve.toe ) ) + std::exp( -k * ( shoulder - x1 ) ) );
	const double residual2 = ( g / k ) * ( std::exp( -k * ( x2 - curve.toe ) ) + std::exp( -k * ( shoulder - x2 ) ) );
	const double floorOff  = ( g / k ) * std::exp( -k * ( curve.toe - xs.front() ) );
	const double ceilOff   = ( g / k ) * std::exp( -k * ( xs.back() - shoulder ) );

	r.toe    = x1 - ( d[ r.first ] - r.floor ) / r.slope;
	r.toeTol = ( residual1 + floorOff + 2.0 * kDensityRead ) / r.slope + ( x1 - curve.toe ) * slopeErr / r.slope;

	r.shoulder    = x2 + ( r.ceiling - d[ r.last ] ) / r.slope;
	r.shoulderTol = ( residual2 + ceilOff + 2.0 * kDensityRead ) / r.slope + ( shoulder - x2 ) * slopeErr / r.slope;
	return r;
}

/// A wedge through the Negative view: per step, per channel, density.
bool negativeWedge( int width, int height, const Settings& settings, const std::vector< double >& xs, int perturb,
                    std::vector< std::vector< double > >& densities )
{
	Settings all = settings;
	all.push_back( { "View", 1.0f } );
	std::vector< float > out;
	if( !renderFloat( width, height, all, buildWedge( width, height, xs ), perturb, out ) )
		return false;

	densities.assign( 3, std::vector< double >( xs.size() ) );
	const int steps = static_cast< int >( xs.size() );
	for( int i = 0; i < steps; ++i )
	{
		const float* p = pixelAt( out, width, stepColumn( i, steps, width ), height / 2 );
		for( int c = 0; c < 3; ++c )
			densities[ c ][ static_cast< size_t >( i ) ] = densityOf( p[ c ] );
	}
	return true;
}

/// A wedge through the Positive view: per step, linear output rgb.
bool positiveWedge( int width, int height, const Settings& settings, const std::vector< double >& xs, int perturb,
                    std::vector< std::vector< double > >& linear )
{
	std::vector< float > out;
	if( !renderFloat( width, height, settings, buildWedge( width, height, xs ), perturb, out ) )
		return false;

	linear.assign( xs.size(), std::vector< double >( 3 ) );
	const int steps = static_cast< int >( xs.size() );
	for( int i = 0; i < steps; ++i )
	{
		const float* p = pixelAt( out, width, stepColumn( i, steps, width ), height / 2 );
		for( int c = 0; c < 3; ++c )
			linear[ static_cast< size_t >( i ) ][ c ] = decodeSrgb( p[ c ] );
	}
	return true;
}

/// Enough steps for a wedge to hold a few per log unit at any raster down
/// to 320 wide: 41 steps is 7.8 px a step there.
constexpr int kWedgeSteps = 41;

//---------------------------------------------------------------------------
// --curve
//
// A grey step wedge, 6.8 log units wide in 41 steps, through the Negative
// view of every negative stock. Each channel's density is read back and:
//
//   - the straight line's slope, read as a secant well inside the bends,
//     equals the stock's gamma within the derived bound;
//   - the straight line, extended, meets the floor (the unexposed density)
//     at x = Toe and the ceiling at x = Toe + Latitude -- which is where the
//     curve says the toe and the shoulder are.
//
// The x axis is exact: the wedge is a float texture, neutral, and each row
// of the crossover sums to one, so every layer's log H is the step's own.
//---------------------------------------------------------------------------
int runCurve( int width, int height, int perturb = 0, bool quiet = false )
{
	int failures = 0;
	for( int s = 0; s < model::kStockCount; ++s )
	{
		const model::Stock& stock = model::kStocks[ s ];
		if( stock.reversal || stock.age > 0.0 )
			continue;//the slide stock in C-41 is cross-processing; expired stock has age speed loss

		const model::Curve curve = model::Develop( stock, model::kC41, 0.0, true );
		const std::vector< double > xs =
			logRamp( curve.toe - 2.1, curve.toe + curve.latitude + 2.0, kWedgeSteps );

		std::vector< std::vector< double > > d;
		if( !negativeWedge( width, height, { { "Stock", static_cast< float >( s ) } }, xs, perturb, d ) )
			return failures + 1;

		for( int c = 0; c < 3; ++c )
		{
			const LineReading r = readLine( xs, d[ c ], curve, 1.0 );
			const bool slopeOk  = r.first >= 0 && r.slope >= r.slopeLow && r.slope <= r.slopeHigh;
			const bool toeOk    = std::fabs( r.toe - curve.toe ) <= r.toeTol;
			const bool shOk     = std::fabs( r.shoulder - ( curve.toe + curve.latitude ) ) <= r.shoulderTol;
			const bool ok       = slopeOk && toeOk && shOk;
			if( !quiet )
				std::printf( "curve %-12s %c: slope %.5f in [%.5f, %.5f]  toe %+.5f (off %.1e, tol %.1e)  shoulder %+.5f "
				             "(off %.1e, tol %.1e)  %s\n",
				             stock.name, "RGB"[ c ], r.slope, r.slopeLow, r.slopeHigh, r.toe, r.toe - curve.toe, r.toeTol,
				             r.shoulder, r.shoulder - curve.toe - curve.latitude, r.shoulderTol, verdict( ok ) );
			if( !ok )
				++failures;
		}
	}

	if( !quiet )
		std::printf( "%s\n", failures == 0 ? "curve: every channel's straight line has slope gamma and bends at the toe and the shoulder"
		                                    : "curve: FAILURES" );
	return failures;
}

//---------------------------------------------------------------------------
// --push
//
// +1 stop of Push against none, on the same wedge. The model's stated
// amounts (Model.h) are gamma x ( 1 + 0.15 ) and fog + 0.03. Both are read
// back: gamma as the ratio of the two measured straight-line slopes, each
// within its own derived band, and fog as the difference of the two measured
// floors -- the base and the mask are the same film in both, so everything
// else in the floor cancels.
//---------------------------------------------------------------------------
int runPush( int width, int height, int perturb = 0, bool quiet = false )
{
	const model::Stock& stock = model::kStocks[ kPortrait ];
	const model::Curve c0     = model::Develop( stock, model::kC41, 0.0, true );
	const model::Curve c1     = model::Develop( stock, model::kC41, 1.0, true );
	const std::vector< double > xs = logRamp( c0.toe - 2.1, c0.toe + c0.latitude + 2.0, kWedgeSteps );

	std::vector< std::vector< double > > d0, d1;
	if( !negativeWedge( width, height, {}, xs, perturb, d0 )
	    || !negativeWedge( width, height, { { "Push", controls::PushParam( 1.0f ) } }, xs, perturb, d1 ) )
		return 1;

	int failures = 0;
	for( int c = 0; c < 3; ++c )
	{
		const LineReading r0 = readLine( xs, d0[ c ], c0, 1.0 );
		const LineReading r1 = readLine( xs, d1[ c ], c1, 1.0 );

		//The ratio's band from the two slopes' bands, each relative to its
		//own gamma.
		const double stated = 1.0 + model::kPushGamma;
		const double ratio  = r1.slope / r0.slope;
		const double lo     = stated * ( r1.slopeLow / c1.gamma ) / ( r0.slopeHigh / c0.gamma );
		const double hi     = stated * ( r1.slopeHigh / c1.gamma ) / ( r0.slopeLow / c0.gamma );

		//Fog: the floors sit 2.1 log units below the toe, off the fog level by
		//at most ( gamma / k ) e^( -2.1 k ) each.
		const double fogRise = r1.floor - r0.floor;
		const double fogTol  = 2.0 * kDensityRead + ( c0.gamma + c1.gamma ) / model::kKnee * std::exp( -2.1 * model::kKnee );

		const bool ok = ratio >= lo && ratio <= hi && std::fabs( fogRise - model::kPushFog ) <= fogTol;
		if( !quiet )
			std::printf( "push %c: gamma x %.5f (stated %.3f, band [%.5f, %.5f])  fog %+.6f (stated %+.3f, tol %.1e)  %s\n",
			             "RGB"[ c ], ratio, stated, lo, hi, fogRise, model::kPushFog, fogTol, verdict( ok ) );
		if( !ok )
			++failures;
	}

	if( !quiet )
		std::printf( "%s\n", failures == 0 ? "push: one stop raises gamma and fog by the model's stated amounts" : "push: FAILURES" );
	return failures;
}

//---------------------------------------------------------------------------
// --mask
//
// The headline claim. A neutral wedge through C-41 with the mask, scanned
// MANUALLY -- one black point and one white point for all three channels,
// the lab scanner's C-41 profile -- must invert to neutral. The chroma of
// each step is reported in the scan's own linear-light RGB as the spread
// max( rgb ) - min( rgb ), against a bound derived from float arithmetic:
//
//   the density error (kDensityRead's first term, 1.4e-6, twice for the
//   difference of two channels, plus half an ULP of the profile's base),
//   through the scanner's slope: p moves by delta / ( White - Black ), and
//   out = ( 10^( R p ) - 1 ) / ( 10^R - 1 ) moves by at most
//   R ln10 10^R / ( 10^R - 1 ) times that; plus 16 ULP of relative error
//   from exp2 and the encode's pow, plus 8 ULP absolute from 10^x - 1.
//
// Manual, because the lab's per-channel auto levels would normalise a
// per-channel SCALE away, and an uncancelled impurity on a neutral IS a
// per-channel scale -- auto levels would pass an unmasked film. That is why
// the mask was invented for printing at fixed filtration, and it is what
// this checks.
//
// The negative controls: Mask Off (the spec's), and couplers that are never
// consumed (the mask present but not cancelling anything) -- both must fail.
//---------------------------------------------------------------------------
int runMask( int width, int height, int perturb = 0, bool quiet = false, bool maskOff = false )
{
	const model::Stock& stock  = model::kStocks[ kPortrait ];
	const model::Curve curve   = model::Develop( stock, model::kC41, 0.0, true );
	const model::Profile prof  = model::ScannerProfile( stock, model::kC41 );
	const std::vector< double > xs = logRamp( curve.toe - 0.5, curve.toe + curve.latitude + 0.5, kWedgeSteps );

	Settings settings;
	if( maskOff )
		settings.push_back( { "Mask On", 0.0f } );

	std::vector< std::vector< double > > out;
	if( !positiveWedge( width, height, settings, xs, perturb, out ) )
		return 1;

	const double R      = ( kWhitePoint - kBlackPoint ) / prof.gamma;
	const double ln10   = std::log( 10.0 );
	const double dOut   = R * ln10 * std::pow( 10.0, R ) / ( std::pow( 10.0, R ) - 1.0 );
	const double dDens  = 2.0 * 1.4e-6 + 0.5 * ulp( prof.base[ 2 ] );
	const double dP     = dDens / ( kWhitePoint - kBlackPoint );

	int failures = 0, unclipped = 0;
	double worstRatio = 0.0, worstSpread = 0.0;
	for( size_t i = 0; i < xs.size(); ++i )
	{
		const auto& o  = out[ i ];
		const double mx = std::max( { o[ 0 ], o[ 1 ], o[ 2 ] } );
		const double mn = std::min( { o[ 0 ], o[ 1 ], o[ 2 ] } );
		if( clipped( o ) )
			continue;//clipped by the scanner's points
		++unclipped;
		const double spread = mx - mn;
		const double bound  = dOut * dP + 16.0 * ulp( mx ) + 8.0 * ulp( 1.0 );
		worstRatio          = std::max( worstRatio, spread / bound );
		worstSpread         = std::max( worstSpread, spread );
		if( spread > bound )
			++failures;
	}
	if( unclipped < 10 )
		++failures;//too few steps inside the scanner's points to mean anything

	if( !quiet )
	{
		std::printf( "mask %s: %d of %zu steps inside the scanner's points; worst spread %.2e = %.2f of its bound  %s\n",
		             maskOff ? "off" : "on ", unclipped, xs.size(), worstSpread, worstRatio, verdict( failures == 0 ) );
		std::printf( "%s\n", failures == 0 ? "mask: a neutral wedge through C-41 with the mask inverts to neutral, manually scanned"
		                                    : "mask: FAILURES" );
	}
	return failures;
}

//---------------------------------------------------------------------------
// --grain
//
// Flat fields at seven densities through the Negative view, Grain Amount 1,
// one-pixel cells, the Portrait 400's N = 32 sites. In every field and every
// channel the sample variance of density over ALL W x H pixels is compared
// with ( Dmax - Dmin )^2 c ( 1 - c ) / N, where Dmin and Dmax are the two
// end fields' measured mean densities and c is this field's measured mean
// density on that scale. Nothing about the prediction is taken from the
// shader but N.
//
// The confidence interval is the sample variance's own: for n iid draws of
// ( Dmax - Dmin ) K / N with K ~ Binomial( N, c ),
//
//   Var( s^2 ) = ( mu4 - sigma^4 ( n - 3 ) / ( n - 1 ) ) / n,
//   mu4 = ( ( Dmax - Dmin ) / N )^4 N c ( 1 - c ) ( 1 + 3 ( N - 2 ) c ( 1 - c ) ),
//
// and the check allows five standard deviations. At the two ends that
// interval degenerates, so the end fields are bounded by a Poisson tail on
// the number of flipped sites instead: lambda = n N ( 1 - c ) at most, with
// ( 1 - c ) bounded by the float resolution of the coverage and the 2^-24
// resolution of the draw's threshold; the variance is allowed
// lambda + 5 sqrt( lambda ) + 5 flips' worth.
//
// And the peak: of the five interior fields the largest variance must be the
// one at c = 0.5. The negative control makes the variance proportional to c,
// which fails the peak and the per-field agreement.
//---------------------------------------------------------------------------
int runGrain( int width, int height, int perturb = 0, bool quiet = false )
{
	const model::Stock& stock = model::kStocks[ kPortrait ];
	const model::Curve curve  = model::Develop( stock, model::kC41, 0.0, true );
	const int N               = stock.grainSites;
	const double n            = static_cast< double >( width ) * height;

	//The fields: black, five interior coverages, and 2.5 log units past the
	//shoulder. The interior exposures are chosen with the curve in double --
	//used to CHOOSE, not to predict.
	const double targets[] = { 0.15, 0.30, 0.50, 0.70, 0.85 };
	std::vector< double > logs = { -40.0 };
	for( double t : targets )
	{
		double lo = curve.toe - 3.0, hi = curve.toe + curve.latitude + 3.0;
		for( int it = 0; it < 200; ++it )
		{
			const double mid = 0.5 * ( lo + hi );
			( model::Coverage( curve, mid ) < t ? lo : hi ) = mid;
		}
		logs.push_back( 0.5 * ( lo + hi ) );
	}
	logs.push_back( curve.toe + curve.latitude + 2.5 );

	const Settings settings = { { "View", 1.0f }, { "Grain Amount", 1.0f }, { "Grain Size", 0.0f } };

	//mean[ field ][ channel ], var[ field ][ channel ]
	std::vector< std::vector< double > > mean( logs.size(), std::vector< double >( 3 ) ), var = mean;
	for( size_t f = 0; f < logs.size(); ++f )
	{
		std::vector< float > out;
		if( !renderFloat( width, height, settings, buildFlatLog( width, height, logs[ f ] ), perturb, out ) )
			return 1;
		for( int c = 0; c < 3; ++c )
		{
			//Two passes, so a field of identical values has a variance of
			//exactly zero rather than the rounding of a large difference.
			std::vector< double > d;
			d.reserve( out.size() / 4 );
			double sum = 0.0;
			for( size_t i = 0; i < out.size(); i += 4 )
			{
				d.push_back( densityOf( out[ i + static_cast< size_t >( c ) ] ) );
				sum += d.back();
			}
			const double m = sum / n;
			double sum2    = 0.0;
			for( double v : d )
				sum2 += ( v - m ) * ( v - m );
			mean[ f ][ c ] = m;
			var[ f ][ c ]  = sum2 / ( n - 1.0 );
		}
	}

	int failures = 0;
	for( int c = 0; c < 3; ++c )
	{
		const double dmin  = mean.front()[ c ];
		const double dmax  = mean.back()[ c ];
		const double range = dmax - dmin;
		const double unit  = range / N;//one site's worth of density

		double peakVar = -1.0;
		size_t peakAt  = 0;
		for( size_t f = 0; f < logs.size(); ++f )
		{
			const bool end    = f == 0 || f + 1 == logs.size();
			const double cbar = std::clamp( ( mean[ f ][ c ] - dmin ) / range, 0.0, 1.0 );
			const double pred = range * range * cbar * ( 1.0 - cbar ) / N;
			double allowed;
			if( end )
			{
				//The float coverage at the ends is within 2.5e-7 of 0 or 1
				//(the softplus difference rounds at ULP( 5.7 ) / Latitude), and
				//the threshold is quantised to 2^-24.
				const double tail   = 2.5e-7 + std::ldexp( 1.0, -24 ) + std::min( cbar, 1.0 - cbar );
				const double lambda = n * N * tail;
				allowed             = ( lambda + 5.0 * std::sqrt( lambda ) + 5.0 ) * unit * unit / n + kDensityRead * kDensityRead;
			}
			else
			{
				const double pq  = cbar * ( 1.0 - cbar );
				const double mu4 = std::pow( unit, 4 ) * N * pq * ( 1.0 + 3.0 * ( N - 2 ) * pq );
				const double v2  = ( mu4 - pred * pred * ( n - 3.0 ) / ( n - 1.0 ) ) / n;
				allowed          = 5.0 * std::sqrt( std::max( v2, 0.0 ) ) + kDensityRead * kDensityRead;
				if( var[ f ][ c ] > peakVar )
				{
					peakVar = var[ f ][ c ];
					peakAt  = f;
				}
			}

			const bool ok = end ? var[ f ][ c ] <= allowed : std::fabs( var[ f ][ c ] - pred ) <= allowed;
			if( !quiet )
				std::printf( "grain %c c=%.4f: variance %.4e, c(1-c)/N predicts %.4e, allowed +-%.1e  %s\n", "RGB"[ c ], cbar,
				             var[ f ][ c ], pred, allowed, verdict( ok ) );
			if( !ok )
				++failures;
		}

		const bool peakOk = peakAt == 3;//logs[ 3 ] is the c = 0.5 field
		if( !quiet )
			std::printf( "grain %c: the largest interior variance is at c=%.2f  %s\n", "RGB"[ c ],
			             peakAt >= 1 && peakAt <= 5 ? targets[ peakAt - 1 ] : -1.0, verdict( peakOk ) );
		if( !peakOk )
			++failures;
	}

	if( !quiet )
	{
		std::printf( "grain: %d samples per field per channel, %zu fields, N = %d, 5 sigma\n", static_cast< int >( n ),
		             logs.size(), N );
		std::printf( "%s\n", failures == 0 ? "grain: variance is c(1-c)/N, peaks at c = 0.5, and is zero at fog and at Dmax"
		                                    : "grain: FAILURES" );
	}
	return failures;
}

//---------------------------------------------------------------------------
// --leak
//
// A leak is an exposure through the base, added to H before the curve. Two
// claims, both read out of the picture.
//
// It follows the curve: on a black scene, at the leak's own edge, the red
// channel's density is read for a ladder of leak exposures one stop apart.
// Each one-stop increment of the density a leak adds must be gamma log10 2
// where the red layer's exposure is on the straight line (within the
// shortfall band), must not be negative, and past the shoulder must fall
// below gamma log10 2 x e^( -k ( x - shoulder ) ) -- it saturates. The total
// can never exceed gamma x Latitude.
//
// It is warm: on a mid-grey scene the positive at the leak's edge has
// R > G > B, each gap larger than the neutral bound of --mask, because the
// leak lands in the red-sensitive layer first.
//
// Negative controls: the leak added linearly after the curve (never
// saturates), and its spectrum reversed (cool).
//---------------------------------------------------------------------------
int runLeak( int width, int height, int perturb = 0, bool quiet = false )
{
	const model::Stock& stock = model::kStocks[ kPortrait ];
	const model::Curve curve  = model::Develop( stock, model::kC41, 0.0, true );
	const double shoulder     = curve.toe + curve.latitude;
	const float spread        = 0.3f;

	//Where the probe pixel is: column 0, the middle row. Its distance from
	//the left edge is half a pixel; the shader's top-down row centre is
	//py + 0.5, which sits at the lateral profile's peak.
	const int px = 0, py = height / 2;
	const double d       = ( px + 0.5 ) / height;
	const double t       = ( ( py + 0.5 ) / height - 0.5 ) / 0.3;
	const double lateral = 0.55 + 0.45 * std::exp( -t * t );
	const double atProbe = std::exp( -d / spread ) * lateral;

	const std::vector< float > black = buildFlatLog( width, height, -40.0 );
	const Settings base = { { "View", 1.0f }, { "Leak Edge", 0.0f }, { "Leak Warmth", 1.0f },
		                    { "Leak Spread", controls::LeakSpreadParam( spread ) } };

	std::vector< float > out;
	if( !renderFloat( width, height, base, black, perturb, out ) )
		return 1;
	const double unleaked = densityOf( pixelAt( out, width, px, py )[ 0 ] );

	//A ladder of leak exposures half a stop apart, from three stops under
	//mid grey to ten over. The x of each rung is what the plugin's own
	//control mapping makes of the slider position the harness sends.
	int failures = 0;
	std::vector< double > added, logs;
	for( int k = 0; k <= 26; ++k )
	{
		Settings s        = base;
		const float param = controls::LeakParam( model::kMidGrey * std::exp2( -3.0 + 0.5 * k ) );
		s.push_back( { "Leak Amount", param } );
		if( !renderFloat( width, height, s, black, perturb, out ) )
			return 1;
		added.push_back( densityOf( pixelAt( out, width, px, py )[ 0 ] ) - unleaked );
		logs.push_back( std::log10( controls::LeakExposure( param ) * atProbe ) );
	}

	int straight = 0, saturated = 0;
	for( size_t k = 0; k + 1 < added.size(); ++k )
	{
		const double inc  = added[ k + 1 ] - added[ k ];
		const double x0 = logs[ k ], x1 = logs[ k + 1 ];
		const double rung = x1 - x0;
		const double read = 2.0 * kDensityRead;
		bool ok           = inc >= -read;
		const char* where = "bend";
		double lo = -read, hi = curve.gamma * rung + read;
		if( x0 >= curve.toe + 1.0 && x1 <= shoulder - 1.0 )
		{
			where = "line";
			++straight;
			lo = curve.gamma * rung * ( 1.0 - std::max( slopeShortfall( x0 - curve.toe, shoulder - x0 ),
			                                            slopeShortfall( x1 - curve.toe, shoulder - x1 ) ) ) - read;
			ok = ok && inc >= lo && inc <= hi;
		}
		else if( x0 >= shoulder + 1.0 )
		{
			where = "past";
			++saturated;
			hi = curve.gamma * rung * std::exp( -model::kKnee * ( x0 - shoulder ) ) + read;
			ok = ok && inc <= hi;
		}
		if( !quiet )
			std::printf( "leak  rung %2zu: log H %+.3f -> %+.3f  adds %+.5f  [%s: %.5f .. %.5f]  %s\n", k, x0, x1, inc, where,
			             lo, hi, verdict( ok ) );
		if( !ok )
			++failures;
	}
	const bool capOk = added.back() <= curve.gamma * curve.latitude + 2.0 * kDensityRead;
	if( !capOk || straight < 2 || saturated < 1 )
		++failures;
	if( !quiet )
		std::printf( "leak: %d increments on the straight line, %d past the shoulder; total %.4f <= gamma x L = %.4f  %s\n",
		             straight, saturated, added.back(), curve.gamma * curve.latitude,
		             verdict( capOk && straight >= 2 && saturated >= 1 ) );

	//Warm.
	{
		Settings s = base;
		s[ 0 ]     = { "View", 0.0f };
		s.push_back( { "Leak Amount", controls::LeakParam( 4.0 * model::kMidGrey ) } );
		if( !renderFloat( width, height, s, buildFlatLog( width, height, model::MidGreyLog() ), perturb, out ) )
			return failures + 1;
		const float* p = pixelAt( out, width, px, py );
		const double r = decodeSrgb( p[ 0 ] ), g = decodeSrgb( p[ 1 ] ), b = decodeSrgb( p[ 2 ] );
		const double bound = 1e-4;//three orders above --mask's neutral bound
		const bool ok      = r - g > bound && g - b > bound;
		if( !quiet )
			std::printf( "leak warm: at the edge R %.4f  G %.4f  B %.4f  %s\n", r, g, b, verdict( ok ) );
		if( !ok )
			++failures;
	}

	if( !quiet )
		std::printf( "%s\n", failures == 0 ? "leak: the density a leak adds follows the curve and saturates, and it is warm"
		                                    : "leak: FAILURES" );
	return failures;
}

//---------------------------------------------------------------------------
// --cross
//
// Cross-processing: the Portrait 400 treated as reversal film (no masking
// couplers) through C-41, scanned manually with the lab's C-41 profile --
// which divides by an orange mask that is not there. The model predicts, per
// channel, a density shortfall against the profile of
//
//   S_c ( Capacity - a ),   S_c = the channel's unwanted-absorption sum,
//
// largest in blue (S = 0.40), then green (0.26), then red (0.07): the positive
// goes WARM (B < G < R), and the shift shrinks toward the highlights as dye
// forms, because the mask the scanner expected would have been consumed
// there. The Negative view shows why: unexposed, the cross-processed film
// has no orange (blue minus red density a fraction of C-41's), and its
// straight line is steep.
//
// All measured: the order of the positive's channels at every unclipped
// step; the Negative view's missing-mask density ( profile_b - profile_r ) -
// ( D_b - D_r ) positive and non-increasing with exposure; the orange and the
// slope against a C-41 render of the same stock.
//
// The negative control: the scanner expecting no mask. Then the shift runs
// the other way and the order check fails.
//---------------------------------------------------------------------------
int runCross( int width, int height, int perturb = 0, bool quiet = false )
{
	const model::Stock& stock  = model::kStocks[ kPortrait ];
	const model::Curve cross   = model::Develop( stock, model::kCross, 0.0, true );
	const model::Curve normal  = model::Develop( stock, model::kC41, 0.0, true );
	const model::Profile prof  = model::ScannerProfile( stock, model::kC41 );
	const std::vector< double > xs = logRamp( cross.toe - 2.0, cross.toe + cross.latitude + 2.0, kWedgeSteps );
	const Settings settings    = { { "Process", static_cast< float >( model::kCross ) } };

	int failures = 0;

	//The positive's direction, on a finer wedge across the part of the
	//curve the scanner's points leave unclipped: the cross curve is steep.
	const std::vector< double > fine = logRamp( cross.toe + 0.4, cross.toe + 1.4, kWedgeSteps );
	std::vector< std::vector< double > > out;
	if( !positiveWedge( width, height, settings, fine, perturb, out ) )
		return 1;
	int unclipped = 0, wrong = 0;
	for( size_t i = 0; i < fine.size(); ++i )
	{
		const auto& o = out[ i ];
		if( clipped( o ) )
			continue;
		++unclipped;
		const double bound = 16.0 * ulp( o[ 0 ] ) + 8.0 * ulp( 1.0 );
		if( !( o[ 0 ] - o[ 1 ] > bound && o[ 1 ] - o[ 2 ] > bound ) )
			++wrong;

	}
	const bool warmOk = unclipped >= 10 && wrong == 0;
	if( !quiet )
		std::printf( "cross positive: %d unclipped steps, %d not R > G > B  %s\n", unclipped, wrong, verdict( warmOk ) );
	if( !warmOk )
		++failures;

	//The Negative view: the missing mask shrinks with exposure; no orange;
	//steep.
	std::vector< std::vector< double > > dc, dn;
	if( !negativeWedge( width, height, settings, xs, perturb, dc ) || !negativeWedge( width, height, {}, xs, perturb, dn ) )
		return failures + 1;

	const double expected = prof.base[ 2 ] - prof.base[ 0 ];
	int rises             = 0;
	double previous       = 1e9, first = 0.0, last = 0.0;
	for( size_t i = 0; i < xs.size(); ++i )
	{
		const double missing = expected - ( dc[ 2 ][ i ] - dc[ 0 ][ i ] );
		if( i == 0 )
			first = missing;
		last = missing;
		if( missing > previous + 2.0 * kDensityRead + 4.0 * ulp( expected ) )
			++rises;
		previous = missing;
	}
	const bool shrinkOk = first > 0.0 && last < first && rises == 0;
	if( !quiet )
		std::printf( "cross negative: missing mask (blue - red) %.4f unexposed -> %.4f at Dmax, never rising  %s\n", first, last,
		             verdict( shrinkOk ) );
	if( !shrinkOk )
		++failures;

	const double orangeCross  = dc[ 2 ].front() - dc[ 0 ].front();
	const double orangeNormal = dn[ 2 ].front() - dn[ 0 ].front();
	const LineReading lc      = readLine( xs, dc[ 0 ], cross, 0.7 );
	const std::vector< double > xn = logRamp( normal.toe - 2.1, normal.toe + normal.latitude + 2.0, kWedgeSteps );
	std::vector< std::vector< double > > dn2;
	if( !negativeWedge( width, height, {}, xn, perturb, dn2 ) )
		return failures + 1;
	const LineReading ln = readLine( xn, dn2[ 0 ], normal, 1.0 );
	const bool maskOk    = orangeCross < 0.25 * orangeNormal;
	const bool steepOk   = lc.first >= 0 && ln.first >= 0 && lc.slope > 1.5 * ln.slope;
	if( !quiet )
		std::printf( "cross negative: orange (blue - red, unexposed) %.4f against C-41's %.4f; slope %.4f against %.4f  %s\n",
		             orangeCross, orangeNormal, lc.slope, ln.slope, verdict( maskOk && steepOk ) );
	if( !maskOk || !steepOk )
		++failures;

	if( !quiet )
		std::printf( "%s\n", failures == 0 ? "cross: reversal film through C-41 has no mask, is steep, and scans warm where the model says"
		                                    : "cross: FAILURES" );
	return failures;
}

//---------------------------------------------------------------------------
// --rebate
//
// The 35 mm format on a black scene, no grain, a manual scan.
//
//   Negative view: every probed sprocket hole transmits fully (T = 1 within
//   4 ULP: exp2( 0 ) may be 3 ULP off); the border (between the film edge
//   and the holes) has exactly the density of the unexposed picture area --
//   both are unexposed film, measured in the same frame; the edge print is
//   denser than the border.
//
//   Positive view: holes and border are exactly 0 (the scanner clips below
//   its black point, and expand() returns 0 for p <= 0 by construction); the
//   edge print is lighter than the border by at least a quarter of white;
//   with Edge Text On off, the print band is exactly the border.
//
// The negative control makes the holes opaque.
//---------------------------------------------------------------------------
int runRebate( int width, int height, int perturb = 0, bool quiet = false )
{
	const frame::Geometry g = frame::Compute( frame::kStrip, width, height );
	const std::vector< float > black = buildFlatLog( width, height, -40.0 );
	auto toPixel = [ & ]( double u, double v, int& x, int& y ) {
		x = static_cast< int >( std::floor( 0.5 * width + u / g.mmPerPixel ) );
		y = static_cast< int >( std::floor( v / g.mmPerPixel ) );
	};

	//Holes: the four nearest the frame centre, top and bottom rows.
	std::vector< std::pair< int, int > > holes;
	for( int k = -2; k < 2; ++k )
		for( int row = 0; row < 2; ++row )
		{
			int x, y;
			const double u = ( k + 0.5 ) * frame::kPerfPitch;
			const double v = row == 0 ? frame::kPerfEdge + 0.5 * frame::kPerfHeight
			                          : g.filmWidthMm - frame::kPerfEdge - 0.5 * frame::kPerfHeight;
			toPixel( u, v, x, y );
			holes.emplace_back( x, y );
		}
	int borderX, borderY, pictureX, pictureY;
	toPixel( 0.5 * frame::kPerfPitch, 1.0, borderX, borderY );
	toPixel( 0.0, 0.5 * ( g.gate[ 1 ] + g.gate[ 3 ] ), pictureX, pictureY );

	//The print band's rows whose pixels lie wholly inside band A.
	int bandTop = static_cast< int >( std::ceil( g.bandA[ 0 ] / g.mmPerPixel ) );
	int bandBot = static_cast< int >( std::floor( g.bandA[ 1 ] / g.mmPerPixel ) ) - 1;
	if( bandBot < bandTop )
		bandBot = bandTop;

	const Settings strip = { { "Format", static_cast< float >( frame::kStrip ) } };
	int failures         = 0;

	//Negative view.
	{
		Settings s = strip;
		s.push_back( { "View", 1.0f } );
		std::vector< float > out;
		if( !renderFloat( width, height, s, black, perturb, out ) )
			return 1;
		double worstHole = 0.0;
		for( const auto& h : holes )
			for( int c = 0; c < 3; ++c )
				worstHole = std::max( worstHole, std::fabs( decodeSrgb( pixelAt( out, width, h.first, h.second )[ c ] ) - 1.0 ) );
		const bool holesOk = worstHole <= 4.0 * ulp( 1.0 );

		double worstBorder = 0.0, borderDensity = 0.0;
		for( int c = 0; c < 3; ++c )
		{
			const double b = densityOf( pixelAt( out, width, borderX, borderY )[ c ] );
			const double p = densityOf( pixelAt( out, width, pictureX, pictureY )[ c ] );
			worstBorder    = std::max( worstBorder, std::fabs( b - p ) );
			if( c == 2 )
				borderDensity = b;
		}
		const bool borderOk = worstBorder <= 2.0 * kDensityRead;

		double densest = 0.0;
		for( int y = bandTop; y <= bandBot; ++y )
			for( int x = 0; x < width; ++x )
				densest = std::max( densest, densityOf( pixelAt( out, width, x, y )[ 2 ] ) );
		const bool printOk = densest > borderDensity + 0.1;

		if( !quiet )
			std::printf( "rebate negative: holes transmit 1 - %.1e; border - unexposed picture %.1e; print %.3f over a border of "
			             "%.3f (blue)  %s\n",
			             worstHole, worstBorder, densest, borderDensity, verdict( holesOk && borderOk && printOk ) );
		failures += holesOk ? 0 : 1;
		failures += borderOk ? 0 : 1;
		failures += printOk ? 0 : 1;
	}

	//Positive view, with and without the edge print.
	for( int text = 1; text >= 0; --text )
	{
		Settings s = strip;
		s.push_back( { "Edge Text On", static_cast< float >( text ) } );
		std::vector< float > out;
		if( !renderFloat( width, height, s, black, perturb, out ) )
			return failures + 1;
		double holeMax = 0.0;
		for( const auto& h : holes )
			for( int c = 0; c < 3; ++c )
				holeMax = std::max( holeMax, static_cast< double >( pixelAt( out, width, h.first, h.second )[ c ] ) );
		double border = 0.0;
		for( int c = 0; c < 3; ++c )
			border = std::max( border, static_cast< double >( pixelAt( out, width, borderX, borderY )[ c ] ) );
		double lightest = 0.0;
		for( int y = bandTop; y <= bandBot; ++y )
			for( int x = 0; x < width; ++x )
				lightest = std::max( lightest, decodeSrgb( pixelAt( out, width, x, y )[ 0 ] ) );

		const bool holesOk = holeMax == 0.0;
		const bool borderOk = border == 0.0;
		const bool printOk  = text ? lightest > 0.25 : lightest == 0.0;
		if( !quiet )
			std::printf( "rebate positive, edge print %s: holes %.1e, border %.1e, lightest in the print band %.4f  %s\n",
			             text ? "on " : "off", holeMax, border, lightest, verdict( holesOk && borderOk && printOk ) );
		failures += holesOk ? 0 : 1;
		failures += borderOk ? 0 : 1;
		failures += printOk ? 0 : 1;
	}

	if( !quiet )
		std::printf( "%s\n", failures == 0 ? "rebate: holes black, border unexposed, edge print lighter than the border"
		                                    : "rebate: FAILURES" );
	return failures;
}

//---------------------------------------------------------------------------
// --seed
//
// Grain is a pure function of ( seed, film frame, cell, layer, site ). So,
// on a flat grey, Grain Amount 1:
//
//   - two fresh instances rendering the same frames are bit-identical;
//   - another seed differs almost everywhere, and so does the next film
//     frame;
//   - the cells are counted from the top-left pixel, so this raster's render
//     is bit-identical to the matching corner of a render at another raster
//     (320x180 against 640x360 at 320x180, 320x180 against this raster
//     otherwise). A resize does not reshuffle the grain.
//
// The negative control makes the hash ignore the seed.
//---------------------------------------------------------------------------
int runSeed( int width, int height, int perturb = 0, bool quiet = false )
{
	const Settings grainy = { { "Grain Amount", 1.0f }, { "Grain Size", 0.0f } };
	auto grey             = [ & ]( int w, int h ) { return buildFlatLog( w, h, model::MidGreyLog() ); };
	constexpr int kFrames = 10;//frame 9 is film frame 3 at 60 fps; frame 10 is film frame 4

	auto differing = []( const std::vector< float >& a, const std::vector< float >& b ) {
		size_t n = 0;
		for( size_t i = 0; i < a.size(); ++i )
			if( a[ i ] != b[ i ] )
				++n;
		return n;
	};

	std::vector< float > a, b, other, next;
	Settings seeded = grainy;
	if( !renderFloat( width, height, grainy, grey( width, height ), perturb, a, kFrames )
	    || !renderFloat( width, height, grainy, grey( width, height ), perturb, b, kFrames ) )
		return 1;
	seeded.push_back( { "Grain Seed", 2.0f } );
	if( !renderFloat( width, height, seeded, grey( width, height ), perturb, other, kFrames )
	    || !renderFloat( width, height, grainy, grey( width, height ), perturb, next, kFrames + 1 ) )
		return 1;

	const size_t total    = a.size() / 4 * 3;
	const size_t same     = differing( a, b );
	const size_t bySeed   = differing( a, other );
	const size_t byFrame  = differing( a, next );
	const bool repeatOk   = same == 0;
	const bool seedOk     = bySeed > total / 2;
	const bool frameOk    = byFrame > total / 2;

	//Across rasters.
	const bool atSmall = width == 320 && height == 180;
	const int sw = 320, sh = 180;
	const int bw = atSmall ? 640 : width, bh = atSmall ? 360 : height;
	std::vector< float > small, big;
	if( !renderFloat( sw, sh, grainy, grey( sw, sh ), perturb, small, kFrames )
	    || !renderFloat( bw, bh, grainy, grey( bw, bh ), perturb, big, kFrames ) )
		return 1;
	size_t corner = 0;
	for( int y = 0; y < sh; ++y )
		for( int x = 0; x < sw; ++x )
			for( int c = 0; c < 4; ++c )
				if( pixelAt( small, sw, x, y )[ c ] != pixelAt( big, bw, x, y )[ c ] )
					++corner;
	const bool rasterOk = bw >= sw && bh >= sh && corner == 0;

	const int failures = !repeatOk + !seedOk + !frameOk + !rasterOk;
	if( !quiet )
	{
		std::printf( "seed: same seed and frame twice, %zu of %zu values differ  %s\n", same, total, verdict( repeatOk ) );
		std::printf( "seed: another seed, %zu differ  %s\n", bySeed, verdict( seedOk ) );
		std::printf( "seed: the next film frame, %zu differ  %s\n", byFrame, verdict( frameOk ) );
		std::printf( "seed: %dx%d against the top-left of %dx%d, %zu differ  %s\n", sw, sh, bw, bh, corner, verdict( rasterOk ) );
		std::printf( "%s\n", failures == 0 ? "seed: grain is bit-identical for a seed and a frame, at any raster" : "seed: FAILURES" );
	}
	return failures;
}

//---------------------------------------------------------------------------
// --resize
//
// The scanner's auto levels are the plugin's only state across frames. They
// are smoothed toward each frame's measurement with a one-pole step
// alpha = 1 - e^( -dt / tau ), primed outright on the first frame. A host
// that changes the clip's size hands the SAME instance a new raster; the
// picture and film buffers reallocate, the 2 x 1 level buffers must not.
//
// Dark wedge S1 at W x H for five frames (primed on the first, steady after),
// then the host switches to the bright wedge S2 at 2W x 2H for one frame. The
// levels must have moved exactly alpha of the way from S1's to S2's (S2's as a
// fresh instance at 2W x 2H measures them): continuity, not a restart. The
// tolerance is float: each component is prev + ( meas - prev ) alpha, a few
// ULP of values near 1, over a difference the check requires to be at least
// 0.05 -- 1e-4 is ten times that bound.
//
// The negative control re-primes the levels on a resize, which jumps all the
// way.
//---------------------------------------------------------------------------
int runResize( int width, int height, int perturb = 0, bool quiet = false )
{
	const model::Stock& stock = model::kStocks[ kPortrait ];
	const model::Curve curve  = model::Develop( stock, model::kC41, 0.0, true );
	const Settings settings   = { { "Auto Levels", 1.0f } };
	const std::vector< double > dark   = logRamp( curve.toe + 0.2, curve.toe + 1.2, 8 );
	const std::vector< double > bright = logRamp( curve.toe + 0.9, curve.toe + 2.4, 8 );
	const int W2 = 2 * width, H2 = 2 * height;

	float before[ 6 ], after[ 6 ], target[ 6 ];
	{
		Session session;
		baseline( session.plugin );
		for( const auto& s : settings )
			set( session.plugin, s.first, s.second );
		session.plugin.SetPerturbForTest( perturb );
		if( !session.begin( width, height ) )
			return 1;
		const std::vector< float > s1 = buildWedge( width, height, dark );
		for( int frame = 0; frame < 5; ++frame )
			session.render( frame, s1 );
		session.plugin.ReadLevelsForTest( before );

		session.resize( W2, H2 );
		session.render( 5, buildWedge( W2, H2, bright ) );
		session.plugin.ReadLevelsForTest( after );
		session.end();
	}
	{
		Session session;
		baseline( session.plugin );
		for( const auto& s : settings )
			set( session.plugin, s.first, s.second );
		if( !session.begin( W2, H2 ) )
			return 1;
		session.render( 0, buildWedge( W2, H2, bright ) );
		session.plugin.ReadLevelsForTest( target );
		session.end();
	}

	const double alpha = 1.0 - std::exp( -( 1.0 / 60.0 ) / Rebate::kLevelsTau );
	int failures       = 0;
	for( int i = 0; i < 6; ++i )
	{
		const double gap      = target[ i ] - before[ i ];
		const double fraction = gap != 0.0 ? ( after[ i ] - before[ i ] ) / gap : 0.0;
		const bool ok         = std::fabs( gap ) >= 0.05 && std::fabs( fraction - alpha ) <= 1e-4;
		if( !quiet )
			std::printf( "resize %s %c: %.5f -> %.5f of the way to %.5f: %.6f (alpha %.6f)  %s\n", i < 3 ? "black" : "white",
			             "RGB"[ i % 3 ], before[ i ], after[ i ], target[ i ], fraction, alpha, verdict( ok ) );
		if( !ok )
			++failures;
	}
	if( !quiet )
		std::printf( "%s\n", failures == 0 ? "resize: the scanner's levels carry across a resize, one smoothing step at a time"
		                                    : "resize: FAILURES" );
	return failures;
}

//---------------------------------------------------------------------------
// --negative
//
// A check that cannot fail is not a check. Each of these perturbs the MODEL
// -- through a hook the shipped plugin carries at zero, or a real control --
// and asserts that the check catches it.
//---------------------------------------------------------------------------
int runNegative( int width, int height )
{
	struct Control
	{
		const char* name;
		int failuresSeen;
	};
	const Control controls[] = {
		{ "curve with the gamma x 0.9                       ", runCurve( width, height, model::kPerturbGamma, true ) },
		{ "push raising gamma at half the stated rate       ", runPush( width, height, model::kPerturbPushGain, true ) },
		{ "mask with Mask On off                            ", runMask( width, height, 0, true, true ) },
		{ "mask with couplers that are never consumed       ", runMask( width, height, model::kPerturbCouplersFixed, true ) },
		{ "grain with variance proportional to density      ", runGrain( width, height, model::kPerturbGrainLaw, true ) },
		{ "leak added linearly after the curve              ", runLeak( width, height, model::kPerturbLeakLinear, true ) },
		{ "leak with its spectrum reversed                  ", runLeak( width, height, model::kPerturbLeakCool, true ) },
		{ "cross with a scanner that expects no mask        ", runCross( width, height, model::kPerturbCrossProfile, true ) },
		{ "rebate with opaque sprocket holes                ", runRebate( width, height, model::kPerturbHolesOpaque, true ) },
		{ "seed with the seed left out of the hash          ", runSeed( width, height, model::kPerturbSeedIgnored, true ) },
		{ "resize that re-primes the levels                 ", runResize( width, height, model::kPerturbResizeClears, true ) },
	};

	int failures = 0;
	for( const Control& c : controls )
	{
		const bool ok = c.failuresSeen > 0;
		std::printf( "negative %s: %s  %s\n", c.name, ok ? "it failed" : "it PASSED", verdict( ok ) );
		if( !ok )
			++failures;
	}
	std::printf( "%s\n", failures == 0 ? "negative: every perturbed model is caught" : "negative: FAILURES -- a check cannot fail" );
	return failures;
}

//---------------------------------------------------------------------------
// --bench
//---------------------------------------------------------------------------
double benchAt( Rebate& plugin, int width, int height, int frames, double fps )
{
	Session session;
	session.floatOutput = false;
	for( const NamedParameter& p : listParameters( plugin ) )
		if( p.index < Rebate::PT_ABOUT_FIRST && p.type != FF_TYPE_BUFFER && p.type != FF_TYPE_EVENT )
			session.plugin.SetFloatParameter( p.index, p.value );
	session.fps = fps;
	if( !session.begin( width, height ) )
		return -1.0;

	const std::vector< unsigned char > card = buildCard( width, height, 0 );

	const int warmup = 20;
	for( int frame = 0; frame < warmup; ++frame )
		session.render( frame, card );
	glFinish();

	//The best of three runs. This machine's GPU is shared with whatever else
	//is rendering, and a run that lost the GPU for a few milliseconds measures
	//the contention, not the plugin; the minimum is the one nothing else
	//interrupted. The card is uploaded once and not per frame, so the upload
	//is not in the figure.
	double best = 1e9;
	for( int run = 0; run < 3; ++run )
	{
		const auto start = std::chrono::steady_clock::now();
		for( int frame = 0; frame < frames; ++frame )
			session.renderAt( warmup + run * frames + frame );
		glFinish();
		const auto end       = std::chrono::steady_clock::now();
		const double seconds = std::chrono::duration< double >( end - start ).count();
		best                 = std::min( best, seconds * 1000.0 / static_cast< double >( frames ) );
	}

	session.end();
	return best;
}

int runBench( Rebate& plugin, int frames, double fps )
{
	struct Size
	{
		const char* name;
		int width, height;
	};
	const Size sizes[] = {
		{ "1280x720  ", 1280, 720 },
		{ "1920x1080 ", 1920, 1080 },
		{ "2560x1440 ", 2560, 1440 },
		{ "3840x2160 ", 3840, 2160 },
	};

	std::printf( "%d frames each, best of three runs, after a 20-frame warm-up, glFinish both sides.\n\n", frames );
	std::printf( "resolution     ms/frame   equivalent fps   %% of a 60fps frame\n" );

	for( const Size& size : sizes )
	{
		const double ms = benchAt( plugin, size.width, size.height, frames, fps );
		std::printf( "%s    %7.3f       %8.0f            %5.1f%%\n", size.name, ms, ms > 0.0 ? 1000.0 / ms : 0.0,
		             ms / 16.667 * 100.0 );
	}

	std::printf( "\nThe scan pass dominates: N grain draws per layer per pixel (N = 32 for\n"
	             "the default stock). Whatever the settings above were, they are what was\n"
	             "measured; run with --set to measure something else.\n" );
	return 0;
}

//---------------------------------------------------------------------------
// --dump-shaders
//---------------------------------------------------------------------------
int dumpShaders( const std::string& dir )
{
	const std::pair< const char*, std::string > files[] = {
		{ "vertex.vert", shaders::Vertex() }, { "copy.frag", shaders::Copy() },   { "film.frag", shaders::Film() },
		{ "blocks.frag", shaders::Blocks() }, { "levels.frag", shaders::Levels() }, { "scan.frag", shaders::Scan() },
	};
	for( const auto& f : files )
	{
		std::ofstream out( dir + "/" + f.first );
		if( !out )
		{
			std::fprintf( stderr, "cannot write %s/%s\n", dir.c_str(), f.first );
			return 1;
		}
		out << f.second;
	}
	std::printf( "wrote %zu shaders to %s\n", sizeof( files ) / sizeof( files[ 0 ] ), dir.c_str() );
	return 0;
}

//---------------------------------------------------------------------------
// --pipe cue sheet: one 'frame Name Value' per line. Same format as the rest
// of the fleet, so one filming script drives any of them.
//---------------------------------------------------------------------------
using Track = std::vector< std::pair< int, float > >;

std::map< std::string, Track > loadScript( const std::string& path, std::string& error )
{
	std::map< std::string, Track > tracks;
	std::ifstream file( path );
	if( !file )
	{
		error = "cannot open " + path;
		return tracks;
	}

	std::string line;
	int lineNumber = 0;
	while( std::getline( file, line ) )
	{
		++lineNumber;
		const size_t hash = line.find( '#' );
		if( hash != std::string::npos )
			line.erase( hash );
		std::istringstream in( line );

		int frame = 0;
		if( !( in >> frame ) )
			continue;

		std::vector< std::string > words;
		std::string word;
		while( in >> word )
			words.push_back( word );
		if( words.size() < 2 )
		{
			error = path + ":" + std::to_string( lineNumber ) + ": expected `frame Parameter Name value`";
			return {};
		}

		const float value = std::strtof( words.back().c_str(), nullptr );
		words.pop_back();
		std::string name = words.front();
		for( size_t i = 1; i < words.size(); ++i )
			name += " " + words[ i ];

		tracks[ name ].emplace_back( frame, value );
	}

	for( auto& entry : tracks )
		std::sort( entry.second.begin(), entry.second.end() );
	return tracks;
}

float valueAt( const Track& track, int frame )
{
	if( track.empty() )
		return 0.0f;
	if( frame <= track.front().first )
		return track.front().second;
	if( frame >= track.back().first )
		return track.back().second;

	for( size_t i = 1; i < track.size(); ++i )
	{
		if( frame <= track[ i ].first )
		{
			const auto& a    = track[ i - 1 ];
			const auto& b    = track[ i ];
			const float span = static_cast< float >( b.first - a.first );
			const float t    = span > 0.0f ? ( static_cast< float >( frame - a.first ) / span ) : 1.0f;
			return a.second + ( b.second - a.second ) * t;
		}
	}
	return track.back().second;
}

//---------------------------------------------------------------------------
void usage()
{
	std::printf(
		"rbtest -- render and measure the Rebate colour-negative effect\n"
		"\n"
		"  --out PATH          render the test card through the plugin (default /tmp/rebate.png)\n"
		"  --size WxH          raster (default 1280x720); --width N / --height N also accepted\n"
		"  --frames N          frames to render before reading back (default 40)\n"
		"  --fps N             synthetic frame rate driving the clock (default 60)\n"
		"  --source S          card (default), flat, or white\n"
		"  --level N           the flat source's code value, 0..255 (default 128)\n"
		"  --set \"Name=V\"      set a parameter by its display name. Repeatable.\n"
		"  --list              print every parameter, its kind, default and range, then exit\n"
		"  --curve             a grey wedge reads back the characteristic curve: slope gamma, toe, shoulder\n"
		"  --mask              C-41 with the mask inverts a neutral wedge to neutral\n"
		"  --grain             density variance is c(1-c)/N, peaks at c = 0.5, zero at the ends\n"
		"  --push              one stop of push raises gamma and fog by the stated amounts\n"
		"  --leak              a leak's density follows the curve and saturates; it is warm\n"
		"  --cross             E-6 film through C-41 scans warm, most in the shadows\n"
		"  --rebate            holes black, border unexposed, edge print lighter\n"
		"  --seed              grain is bit-identical for a seed and a frame, at any raster\n"
		"  --resize            the scanner's levels survive a resize\n"
		"  --negative          every check above can fail\n"
		"  --perturb BITS      run the checks against a perturbed model (Model.h), verbosely\n"
		"  --bench             time ProcessOpenGL at 720p through 4K\n"
		"  --dump-shaders DIR  write the exact GLSL the plugin compiles\n"
		"  --pipe              raw RGBA frames on stdin, raw RGBA frames on stdout\n"
		"  --script PATH       parameter cues for --pipe: 'frame Name Value'\n"
		"  --help\n" );
}
} // namespace

int main( int argc, char** argv )
{
	std::string outPath = "/tmp/rebate.png";
	std::string scriptPath;
	std::string sourceName = "card";
	std::string dumpDir;
	int width      = 1280;
	int height     = 720;
	int frames     = 40;
	int level      = 128;
	int perturb    = 0;
	double fps     = 60.0;
	bool wantList  = false;
	bool wantBench = false;
	bool wantPipe  = false;
	std::vector< std::string > settings;
	std::vector< std::string > checks;

	for( int i = 1; i < argc; ++i )
	{
		const std::string argument = argv[ i ];
		const bool hasNext         = i + 1 < argc;

		if( argument == "--help" )
		{
			usage();
			return 0;
		}
		else if( argument == "--out" && hasNext )
			outPath = argv[ ++i ];
		else if( argument == "--script" && hasNext )
			scriptPath = argv[ ++i ];
		else if( argument == "--source" && hasNext )
			sourceName = argv[ ++i ];
		else if( argument == "--level" && hasNext )
			level = std::atoi( argv[ ++i ] );
		else if( argument == "--perturb" && hasNext )
			perturb = std::atoi( argv[ ++i ] );
		else if( argument == "--dump-shaders" && hasNext )
			dumpDir = argv[ ++i ];
		else if( argument == "--size" && hasNext )
		{
			const std::string size = argv[ ++i ];
			const size_t x         = size.find( 'x' );
			if( x == std::string::npos )
			{
				std::fprintf( stderr, "--size wants WxH\n" );
				return 2;
			}
			width  = std::atoi( size.substr( 0, x ).c_str() );
			height = std::atoi( size.substr( x + 1 ).c_str() );
		}
		else if( argument == "--width" && hasNext )
			width = std::atoi( argv[ ++i ] );
		else if( argument == "--height" && hasNext )
			height = std::atoi( argv[ ++i ] );
		else if( argument == "--frames" && hasNext )
			frames = std::atoi( argv[ ++i ] );
		else if( argument == "--fps" && hasNext )
			fps = std::strtod( argv[ ++i ], nullptr );
		else if( argument == "--set" && hasNext )
			settings.push_back( argv[ ++i ] );
		else if( argument == "--list" )
			wantList = true;
		else if( argument == "--bench" )
			wantBench = true;
		else if( argument == "--pipe" )
			wantPipe = true;
		else if( argument == "--curve" || argument == "--mask" || argument == "--grain" || argument == "--push"
		         || argument == "--leak" || argument == "--cross" || argument == "--rebate" || argument == "--seed"
		         || argument == "--resize" || argument == "--negative" )
			checks.push_back( argument );
		else
		{
			std::fprintf( stderr, "unknown argument: %s\n", argument.c_str() );
			usage();
			return 2;
		}
	}

	if( width <= 0 || height <= 0 || frames <= 0 || fps <= 0.0 )
	{
		std::fprintf( stderr, "width, height, frames and fps must all be positive\n" );
		return 2;
	}

	if( !dumpDir.empty() )
		return dumpShaders( dumpDir );

	if( wantList )
	{
		//No GL needed, so it is answered before a context is made -- which also
		//means it works on a machine where creating one fails, and in CI.
		Rebate plugin;
		std::printf( "%3s  %-16s  %-9s  %-8s  %s\n", "id", "name", "kind", "default", "range" );
		for( const NamedParameter& p : listParameters( plugin ) )
			std::printf( "%3u  %-16s  %-9s  %.4f    [%g..%g]\n", p.index, p.name.c_str(), kindName( p ), p.value, p.low,
			             p.high );
		return 0;
	}

	CGLContextObj context = createContext();
	if( context == nullptr )
	{
		std::fprintf( stderr, "could not create an OpenGL context\n" );
		return 1;
	}

	auto finish = [ & ]( int result ) {
		CGLSetCurrentContext( nullptr );
		CGLDestroyContext( context );
		return result;
	};

	if( !checks.empty() )
	{
		int failures = 0;
		for( const std::string& check : checks )
		{
			int result = 0;
			if( check == "--curve" )
				result = runCurve( width, height, perturb );
			else if( check == "--mask" )
				result = runMask( width, height, perturb );
			else if( check == "--grain" )
				result = runGrain( width, height, perturb );
			else if( check == "--push" )
				result = runPush( width, height, perturb );
			else if( check == "--leak" )
				result = runLeak( width, height, perturb );
			else if( check == "--cross" )
				result = runCross( width, height, perturb );
			else if( check == "--rebate" )
				result = runRebate( width, height, perturb );
			else if( check == "--seed" )
				result = runSeed( width, height, perturb );
			else if( check == "--resize" )
				result = runResize( width, height, perturb );
			else if( check == "--negative" )
				result = runNegative( width, height );
			failures += result;
			std::printf( "\n" );
		}
		return finish( failures == 0 ? 0 : 1 );
	}

	Session session;
	session.fps         = fps;
	session.floatOutput = false;

	for( const std::string& setting : settings )
	{
		std::string error;
		if( applySetting( session.plugin, setting, error ) )
			continue;
		std::fprintf( stderr, "--set %s: %s\n", setting.c_str(), error.c_str() );
		return finish( 2 );
	}

	if( wantBench )
		return finish( runBench( session.plugin, frames, fps ) );

	if( !session.begin( width, height ) )
		return finish( 1 );

	if( wantPipe )
	{
		std::map< unsigned int, Track > automation;
		if( !scriptPath.empty() )
		{
			std::string error;
			const std::map< std::string, Track > tracks = loadScript( scriptPath, error );
			if( !error.empty() )
			{
				std::fprintf( stderr, "%s\n", error.c_str() );
				return finish( 2 );
			}
			for( const auto& entry : tracks )
			{
				const int index = indexOfParameter( session.plugin, entry.first );
				if( index < 0 )
				{
					std::fprintf( stderr, "script names '%s', which is not a parameter (try --list)\n", entry.first.c_str() );
					return finish( 2 );
				}
				automation[ static_cast< unsigned int >( index ) ] = entry.second;
			}
		}

		std::vector< unsigned char > frame( static_cast< size_t >( width ) * height * 4 );
		for( int index = 0;; ++index )
		{
			size_t got = 0;
			while( got < frame.size() )
			{
				const ssize_t n = read( STDIN_FILENO, frame.data() + got, frame.size() - got );
				if( n <= 0 )
					break;
				got += static_cast< size_t >( n );
			}
			if( got < frame.size() )
				break;

			for( const auto& track : automation )
				session.plugin.SetFloatParameter( track.first, valueAt( track.second, index ) );

			if( !session.render( index, frame ) )
				break;

			const std::vector< unsigned char > out = session.readBack();
			size_t written                         = 0;
			while( written < out.size() )
			{
				const ssize_t put = write( STDOUT_FILENO, out.data() + written, out.size() - written );
				if( put <= 0 )
					break;
				written += static_cast< size_t >( put );
			}
		}

		session.end();
		return finish( 0 );
	}

	for( int frame = 0; frame < frames; ++frame )
	{
		std::vector< unsigned char > pixels;
		if( sourceName == "flat" )
			pixels = buildFlat( width, height, level );
		else if( sourceName == "white" )
			pixels = buildFlat( width, height, 255 );
		else
			pixels = buildCard( width, height, frame );
		if( !session.render( frame, pixels ) )
			return finish( 1 );
	}

	const std::vector< unsigned char > image = session.readBack();
	session.end();

	if( !writePng( outPath, width, height, image ) )
	{
		std::fprintf( stderr, "could not write %s\n", outPath.c_str() );
		return finish( 1 );
	}

	std::printf( "wrote %s (%dx%d, %d frames)\n", outPath.c_str(), width, height, frames );
	return finish( 0 );
}
