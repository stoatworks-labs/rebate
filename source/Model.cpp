#include "Model.h"

#include <algorithm>
#include <cmath>

namespace rebate::model
{

// Five invented stocks. The grain sites are what `Stock` means to the grain:
// fewer, larger clouds per cell in a faster emulsion, so a larger c(1-c)/N.
// The expired 400 is the portrait 400 at an intrinsic age of 0.6.
const Stock kStocks[ kStockCount ] = {
	//  name              code       reversal  gamma  N   fog   age
	{ "Fine 100",       "FN100",   false,    0.62,  64, 0.10, 0.0 },
	{ "Portrait 400",   "PT400",   false,    0.58,  32, 0.14, 0.0 },
	{ "Grain 800",      "GR800",   false,    0.60,  14, 0.18, 0.0 },
	{ "Slide 100",      "SL100",   true,     0.60,  56, 0.08, 0.0 },
	{ "Expired 400",    "PT400",   false,    0.58,  28, 0.14, 0.6 },
};

const Stock& StockAt( int index )
{
	return kStocks[ std::clamp( index, 0, kStockCount - 1 ) ];
}

double MidGreyLog()
{
	return std::log10( kMidGrey );
}

double UnwantedSum( int channel )
{
	double sum = 0.0;
	for( int d = 0; d < 3; ++d )
		if( d != channel )
			sum += kImpurity[ channel ][ d ];
	return sum;
}

bool ReversalEmulsion( const Stock& stock, int process )
{
	return stock.reversal || process == kCross;
}

bool Inverts( int process )
{
	return process != kE6;
}

Curve Develop( const Stock& stock, int process, double push, bool maskOn, double pushGainScale )
{
	const bool reversal = ReversalEmulsion( stock, process );
	const bool e6       = process == kE6;
	const double gain   = 1.0 + kPushGamma * pushGainScale * push;
	const double mid    = MidGreyLog();

	Curve curve;
	if( !reversal && !e6 )
	{
		//The colour negative, as designed.
		curve.gamma    = stock.gamma * gain;
		curve.latitude = kNegativeLatitude;
		curve.toe      = mid - kNegativeToeBelowMid;
		curve.fog      = stock.fog + kPushFog * push;
		curve.positive = false;
		curve.couplers = maskOn;
	}
	else if( !reversal && e6 )
	{
		//A negative emulsion through reversal chemistry: a flat positive that
		//keeps its orange mask.
		curve.gamma    = 1.5 * stock.gamma * gain;
		curve.latitude = 2.2;
		curve.toe      = mid - 0.5 * curve.latitude;
		curve.fog      = 0.5 * stock.fog + kPushFog * push;
		curve.positive = true;
		curve.couplers = maskOn;
	}
	else if( reversal && !e6 )
	{
		//Cross-processing: reversal film in C-41. A negative, steep and short,
		//with no masking couplers because reversal film never had any.
		curve.gamma    = 2.0 * stock.gamma * gain;
		curve.latitude = 2.0;
		curve.toe      = mid - 1.0;
		curve.fog      = 0.5 * stock.fog + kPushFog * push;
		curve.positive = false;
		curve.couplers = false;
	}
	else
	{
		//A slide, as designed.
		curve.gamma    = 3.0 * stock.gamma * gain;
		curve.latitude = 1.55;
		curve.toe      = mid - 0.5 * curve.latitude;
		curve.fog      = 0.5 * stock.fog + kPushFog * push;
		curve.positive = true;
		curve.couplers = false;
	}

	curve.fog = std::max( curve.fog, 0.0 );
	return curve;
}

Profile ScannerProfile( const Stock& stock, int process, bool maskAssumed )
{
	Profile profile;
	if( Inverts( process ) )
	{
		for( int c = 0; c < 3; ++c )
			profile.base[ c ] = kBase[ c ] + stock.fog + ( maskAssumed ? UnwantedSum( c ) * kCapacity : 0.0 );
		profile.gamma = stock.gamma;
	}
	else
	{
		const Curve slide = Develop( stock, kE6, 0.0, false );
		for( int c = 0; c < 3; ++c )
			profile.base[ c ] = kBase[ c ] + slide.fog;
		profile.gamma = slide.gamma;
	}
	return profile;
}

namespace
{
double softplus( double u )
{
	return std::max( u, 0.0 ) + std::log1p( std::exp( -kKnee * std::fabs( u ) ) ) / kKnee;
}
} // namespace

double Coverage( const Curve& curve, double logH )
{
	const double c = ( softplus( logH - curve.toe ) - softplus( logH - curve.toe - curve.latitude ) ) / curve.latitude;
	return std::clamp( c, 0.0, 1.0 );
}

double Dye( const Curve& curve, double c )
{
	const double range = curve.gamma * curve.latitude;
	double a           = curve.positive ? curve.fog + range - range * c : curve.fog + range * c;
	if( curve.couplers )
		a = std::min( a, kCapacity );
	return a;
}

void ChannelDensity( const Curve& curve, const double dye[ 3 ], double out[ 3 ] )
{
	for( int c = 0; c < 3; ++c )
	{
		double d = kBase[ c ] + dye[ c ];
		for( int k = 0; k < 3; ++k )
		{
			if( k == c )
				continue;
			d += kImpurity[ c ][ k ] * dye[ k ];
			if( curve.couplers )
				d += kImpurity[ c ][ k ] * ( kCapacity - dye[ k ] );
		}
		out[ c ] = d;
	}
}

} // namespace rebate::model
