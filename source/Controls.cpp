#include "Controls.h"

#include "Model.h"

#include <algorithm>
#include <cmath>

namespace rebate::controls
{
namespace
{
float clamp01( float v )
{
	return std::clamp( v, 0.0f, 1.0f );
}
} // namespace

float ExposureStops( float value )
{
	return clamp01( value ) * 12.0f - 6.0f;
}

float ExposureParam( float stops )
{
	return clamp01( ( stops + 6.0f ) / 12.0f );
}

float PushStops( float value )
{
	return clamp01( value ) * 4.0f - 1.0f;
}

float PushParam( float stops )
{
	return clamp01( ( stops + 1.0f ) / 4.0f );
}

float Age( float value )
{
	return clamp01( value );
}

float GrainAmount( float value )
{
	return clamp01( value );
}

float GrainCellPixels( float value )
{
	return std::exp2( 3.0f * clamp01( value ) );
}

float GrainSizeParam( float pixels )
{
	return clamp01( std::log2( std::max( pixels, 1.0f ) ) / 3.0f );
}

double LeakExposure( float value )
{
	const float v = clamp01( value );
	if( v <= 0.0f )
		return 0.0;
	return model::kMidGrey * std::exp2( -4.0 + 14.0 * static_cast< double >( v ) );
}

float LeakParam( double exposure )
{
	if( exposure <= 0.0 )
		return 0.0f;
	return clamp01( static_cast< float >( ( std::log2( exposure / model::kMidGrey ) + 4.0 ) / 14.0 ) );
}

float LeakSpread( float value )
{
	return 0.05f * std::pow( 20.0f, clamp01( value ) );
}

float LeakSpreadParam( float heights )
{
	return clamp01( std::log( std::max( heights, 0.05f ) / 0.05f ) / std::log( 20.0f ) );
}

float BlackPoint( float value )
{
	return 0.6f * clamp01( value );
}

float BlackPointParam( float density )
{
	return clamp01( density / 0.6f );
}

float WhitePoint( float value )
{
	return 0.6f + 2.4f * clamp01( value );
}

float WhitePointParam( float density )
{
	return clamp01( ( density - 0.6f ) / 2.4f );
}

float ScannerGamma( float value )
{
	return std::exp2( 2.0f * clamp01( value ) - 1.0f );
}

float ScannerGammaParam( float gamma )
{
	return clamp01( ( std::log2( std::max( gamma, 0.5f ) ) + 1.0f ) / 2.0f );
}

const char* ProcessName( int index )
{
	static const char* const names[ kProcessCount ] = { "C-41", "E-6", "Cross" };
	return names[ std::clamp( index, 0, kProcessCount - 1 ) ];
}

const char* LeakEdgeName( int index )
{
	static const char* const names[ kLeakEdgeCount ] = { "Left", "Right", "Top", "Bottom" };
	return names[ std::clamp( index, 0, kLeakEdgeCount - 1 ) ];
}

const char* ViewName( int index )
{
	static const char* const names[ kViewCount ] = { "Positive", "Negative" };
	return names[ std::clamp( index, 0, kViewCount - 1 ) ];
}

const char* FormatName( int index )
{
	static const char* const names[ kFormatCount ] = { "Full", "6x6", "35 mm" };
	return names[ std::clamp( index, 0, kFormatCount - 1 ) ];
}

int OptionIndex( float value, int count )
{
	return std::clamp( static_cast< int >( std::lround( value ) ), 0, count - 1 );
}

} // namespace rebate::controls
