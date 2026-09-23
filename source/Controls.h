#pragma once

/**
	Host parameters are 0..1; these are what they mean.

	`CFFGLPluginManager::SetParamInfo` clamps a STANDARD default into 0..1
	*before* returning, and `SetParamRange` can only be called afterwards, so a
	parameter declared in stops cannot declare a default in stops. Every
	slider here is therefore a plain 0..1 float and the conversions live in
	this one file, which the plugin and the harness both use.

	Every mapping that a check needs to hit exactly has an inverse, and the
	neutral positions are chosen to land on their neutral value EXACTLY in
	binary: Exposure 0.5 is 0 stops (0.5 x 12 - 6), Push 0.25 is 0 stops
	(0.25 x 4 - 1), Scanner Gamma 0.5 is 1 (exp2( 0 )), Grain Size 0 is one
	pixel (exp2( 0 )).

	Options are mapped by INDEX. An option parameter's range reads back 0..1
	from the SDK whatever its element count, so nothing outside this file
	should reason from the range. `FF_TYPE_INTEGER` (Grain Seed, Frame
	Number) holds its real value.
*/
namespace rebate::controls
{

/// Exposure: -6 to +6 stops, linear. 0.5 is exactly 0.
float ExposureStops( float value );
float ExposureParam( float stops );

/// Push: -1 (a one-stop pull) to +3 stops of development, linear. 0.25 is 0.
float PushStops( float value );
float PushParam( float stops );

/// Age: 0 to 1, linear, added to the stock's own age and clamped at 1.
float Age( float value );

/// Grain Amount: 0 to 1, linear. 1 is the full physical fluctuation of a
/// Poisson draw per cell; 0 is exactly none.
float GrainAmount( float value );

/// Grain Size: 1 to 8 output pixels per grain cell, geometric. 0 is 1 px.
float GrainCellPixels( float value );
float GrainSizeParam( float pixels );

/// Leak Amount: 0 is no leak at all; above that, the leak's exposure at its
/// edge, from 4 stops under to 10 stops over mid grey, geometric.
double LeakExposure( float value );
float LeakParam( double exposure );

/// Leak Spread: the leak's falloff length, 0.05 to 1 frame height,
/// geometric.
float LeakSpread( float value );
float LeakSpreadParam( float heights );

/// Black Point: 0 to 0.6 density above the profile's base, linear.
float BlackPoint( float value );
float BlackPointParam( float density );

/// White Point: 0.6 to 3.0 density above the profile's base, linear.
float WhitePoint( float value );
float WhitePointParam( float density );

/// Scanner Gamma: 0.5 to 2, geometric. 0.5 is exactly 1.
float ScannerGamma( float value );
float ScannerGammaParam( float gamma );

/// Option counts, and names in their menu order.
constexpr int kProcessCount = 3;
const char* ProcessName( int index );
constexpr int kLeakEdgeCount = 4;
const char* LeakEdgeName( int index );
constexpr int kViewCount = 2;
const char* ViewName( int index );
constexpr int kFormatCount = 3;
const char* FormatName( int index );

/// An option's value to its index, rounded and clamped.
int OptionIndex( float value, int count );

} // namespace rebate::controls
