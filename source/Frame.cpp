#include "Frame.h"

#include "Font.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace rebate::frame
{
namespace
{
/// Centre-crop the scene (aspect `sceneAspect`, width / height) to a gate of
/// aspect `gateAspect`.
void centreCrop( double sceneAspect, double gateAspect, double crop[ 4 ] )
{
	if( sceneAspect > gateAspect )
	{
		const double w = gateAspect / sceneAspect;
		crop[ 0 ]      = 0.5 * ( 1.0 - w );
		crop[ 1 ]      = 0.0;
		crop[ 2 ]      = w;
		crop[ 3 ]      = 1.0;
	}
	else
	{
		const double h = sceneAspect / gateAspect;
		crop[ 0 ]      = 0.0;
		crop[ 1 ]      = 0.5 * ( 1.0 - h );
		crop[ 2 ]      = 1.0;
		crop[ 3 ]      = h;
	}
}

/// Draw `text` centred at column `centre`, on line `line` (0 or 1).
void drawLabel( TextStrip& strip, const std::string& text, double centre, int line )
{
	const int n     = static_cast< int >( text.size() );
	const int span  = n * font::kAdvance - 1;
	const int left  = static_cast< int >( std::lround( centre - 0.5 * span ) );
	const int rowAt = line * font::kHeight;

	for( int i = 0; i < n; ++i )
	{
		const int code = static_cast< unsigned char >( text[ static_cast< size_t >( i ) ] );
		for( int y = 0; y < font::kHeight; ++y )
			for( int x = 0; x < font::kWidth; ++x )
			{
				const int column = left + i * font::kAdvance + x;
				if( column < 0 || column >= strip.width || !font::Bit( code, x, y ) )
					continue;
				strip.pixels[ static_cast< size_t >( rowAt + y ) * strip.width + column ] = 255;
			}
	}
}

int wrap100( int n )
{
	return ( ( n % 100 ) + 100 ) % 100;
}
} // namespace

Geometry Compute( int format, int width, int height )
{
	Geometry g;
	g.format            = std::clamp( format, 0, 2 );
	const double aspect = static_cast< double >( width ) / static_cast< double >( std::max( height, 1 ) );

	if( g.format == kFull )
	{
		//No film to speak of: the picture is the frame. The film pass does not
		//read any of this; it is filled in so nothing is left undefined.
		g.filmWidthMm = height;
		g.mmPerPixel  = 1.0;
		g.gate[ 0 ]   = -0.5 * width;
		g.gate[ 1 ]   = 0.0;
		g.gate[ 2 ]   = 0.5 * width;
		g.gate[ 3 ]   = height;
		return g;
	}

	if( g.format == kSquare )
	{
		//120 roll film: 61.5 mm wide, a 56 x 56 frame, no perforations. The
		//edge print sits in the top 2.75 mm margin.
		g.filmWidthMm = 61.5;
		g.gate[ 0 ]   = -28.0;
		g.gate[ 1 ]   = 2.75;
		g.gate[ 2 ]   = 28.0;
		g.gate[ 3 ]   = 58.75;
		g.holes       = false;
		g.bandA[ 0 ]  = 0.9;
		g.bandA[ 1 ]  = 1.9;
	}
	else
	{
		//35 mm: a 36 x 24 frame centred on a 35 mm strip. The perforations are
		//2.01 to 3.99 mm in from each edge; the edge print sits in the 1.5 mm
		//between the holes and the picture, the stock code above and the frame
		//numbers below.
		g.filmWidthMm = 35.0;
		g.gate[ 0 ]   = -18.0;
		g.gate[ 1 ]   = 5.5;
		g.gate[ 2 ]   = 18.0;
		g.gate[ 3 ]   = 29.5;
		g.holes       = true;
		g.bandA[ 0 ]  = 4.25;
		g.bandA[ 1 ]  = 5.25;
		g.bandB[ 0 ]  = 29.75;
		g.bandB[ 1 ]  = 30.75;
	}

	g.mmPerPixel = g.filmWidthMm / static_cast< double >( std::max( height, 1 ) );
	centreCrop( aspect, ( g.gate[ 2 ] - g.gate[ 0 ] ) / ( g.gate[ 3 ] - g.gate[ 1 ] ), g.crop );

	const double halfWidthMm = 0.5 * width * g.mmPerPixel;
	g.stripLeftMm            = -halfWidthMm - 2.0;
	return g;
}

TextStrip BuildText( const Geometry& g, const char* code, int frameNumber, bool on )
{
	TextStrip strip;
	char key[ 128 ];
	std::snprintf( key, sizeof( key ), "%d|%.6f|%s|%d|%d", g.format, g.stripLeftMm, code, frameNumber, on ? 1 : 0 );
	strip.key = key;

	const double spanMm = g.format == kFull ? 1.0 : -2.0 * g.stripLeftMm;
	strip.width         = std::max( 1, static_cast< int >( std::ceil( spanMm * kGlyphRowsPerMm ) ) );
	strip.height        = 2 * font::kHeight;
	strip.pixels.assign( static_cast< size_t >( strip.width ) * strip.height, 0 );

	if( !on || g.format == kFull )
		return strip;

	//Column of u = 0 mm.
	const double origin = -g.stripLeftMm * kGlyphRowsPerMm;
	const double leftU  = g.stripLeftMm;
	const double rightU = -g.stripLeftMm;

	if( g.format == kStrip )
	{
		//Line A: the stock code on every half frame, between the numbers.
		for( int k = static_cast< int >( std::floor( ( leftU - 9.5 ) / 19.0 ) ) - 1;
		     9.5 + 19.0 * k < rightU + 19.0; ++k )
			drawLabel( strip, code, origin + ( 9.5 + 19.0 * k ) * kGlyphRowsPerMm, 0 );

		//Line B: frame numbers. The number under each frame's centre with an
		//arrow, and the number with an A halfway to the next.
		for( int k = static_cast< int >( std::floor( leftU / 19.0 ) ) - 1; 19.0 * k < rightU + 19.0; ++k )
		{
			char label[ 16 ];
			const int half = static_cast< int >( std::floor( k / 2.0 ) );
			if( k % 2 == 0 )
				std::snprintf( label, sizeof( label ), ">%d", wrap100( frameNumber + half ) );
			else
				std::snprintf( label, sizeof( label ), "%dA", wrap100( frameNumber + half ) );
			drawLabel( strip, label, origin + 19.0 * k * kGlyphRowsPerMm, 1 );
		}
	}
	else
	{
		//Roll film: the stock code in the top margin every 30 mm. Frame
		//numbers on 120 live on the backing paper, not the film, so there are
		//none and Frame Number does nothing in this format.
		for( int k = static_cast< int >( std::floor( ( leftU - 15.0 ) / 30.0 ) ) - 1;
		     15.0 + 30.0 * k < rightU + 30.0; ++k )
			drawLabel( strip, code, origin + ( 15.0 + 30.0 * k ) * kGlyphRowsPerMm, 0 );
	}

	return strip;
}

} // namespace rebate::frame
