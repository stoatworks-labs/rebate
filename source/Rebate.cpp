#include "Rebate.h"

#include "Controls.h"
#include "Diag.h"
#include "Model.h"
#include "Shaders.h"

#include <ffglex/FFGLScopedFBOBinding.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>

using namespace ffglex;
using namespace rebate;

static CFFGLPluginInfo PluginInfo(
	PluginFactory< Rebate >,                                     // Create method
	"RB01",                                                      // Plugin unique ID of maximum length 4.
	"SW Rebate",                                                 // Plugin name
	2,                                                           // API major version number
	1,                                                           // API minor version number
	0,                                                           // Plugin major version number
	1,                                                           // Plugin minor version number
	FF_EFFECT,                                                   // Plugin type
	"Colour negative film, and the scan of it.\n\nThe clip is the scene. It exposes three dye layers, which are developed along a characteristic curve into dyes carrying an orange mask, realised as grain, and then scanned and inverted the way a lab scanner does it.\n\nWhat falls out: the orange negative, mid-tone grain, blue shadows on expired stock, warm light leaks, cross-processing, and the rebate with its sprocket holes and edge print.",// Plugin description
	"Rebate FFGL effect"                                         // About
);

namespace
{
/// Frames that must agree before the host's clock unit is settled.
constexpr int kClockVotes = 4;

/// Seconds of host time a single frame is allowed to advance the clock by.
constexpr double kMaxFrameDelta = 0.25;

/// Film runs at 24 frames a second, so the grain changes 24 times a second
/// whatever the host's rate.
constexpr double kFilmRate = 24.0;

/// Warmth 1: the leak's spectrum on the red, green and blue layers. A leak
/// through the base reaches the red-sensitive layer (the bottom one) first,
/// and what gets through to the top is what the lower layers passed.
constexpr float kWarmLeak[ 3 ] = { 1.0f, 0.30f, 0.06f };

/// Wall clock, for hosts that never call SetTime. Steady rather than system,
/// so nothing here moves when the machine's clock is corrected.
double wallSeconds()
{
	using namespace std::chrono;
	static const steady_clock::time_point start = steady_clock::now();
	return duration_cast< duration< double > >( steady_clock::now() - start ).count();
}

/// glGetString returns nullptr when there is no current context, and feeding
/// that to std::string is undefined behaviour.
std::string glStringOrUnknown( GLenum name )
{
	const GLubyte* value = glGetString( name );
	return value ? reinterpret_cast< const char* >( value ) : "unknown";
}
} // namespace

//---------------------------------------------------------------------------
Rebate::Rebate()
{
	SetMinInputs( 1 );
	SetMaxInputs( 1 );

	//The grain is a function of the film frame, and the film frame is a
	//function of the host's clock: a re-render of the same composition must
	//grain the same frame the same way.
	SetTimeSupported( true );

	//---------------------------------------------------------------------
	// Defaults. A 400-speed portrait negative, normally exposed and
	// developed, grain at a third of its physical fluctuation, no leak,
	// scanned manually with the lab's C-41 profile, on a 35 mm strip with
	// its rebate. Manual rather than Auto Levels by default, so that every
	// control does what it says out of the box: auto levels normalise away
	// exposure, age fog and the missing mask of a cross-process, which is
	// what a lab does and not what an operator reaching for Age expects.
	//---------------------------------------------------------------------
	params[ PT_EXPOSURE ] = controls::ExposureParam( 0.0f );
	params[ PT_STOCK ]    = 1.0f;//Portrait 400
	params[ PT_PUSH ]     = controls::PushParam( 0.0f );
	params[ PT_AGE ]      = 0.0f;

	params[ PT_PROCESS ] = 0.0f;//C-41
	params[ PT_MASK ]    = 1.0f;

	params[ PT_GRAIN_AMOUNT ] = 0.35f;
	params[ PT_GRAIN_SIZE ]   = controls::GrainSizeParam( 1.5f );
	params[ PT_GRAIN_SEED ]   = 1.0f;

	params[ PT_LEAK_AMOUNT ] = 0.0f;
	params[ PT_LEAK_EDGE ]   = 1.0f;//Right
	params[ PT_LEAK_WARMTH ] = 0.8f;
	params[ PT_LEAK_SPREAD ] = controls::LeakSpreadParam( 0.3f );

	params[ PT_VIEW ]          = 0.0f;//Positive
	params[ PT_AUTO_LEVELS ]   = 0.0f;
	params[ PT_BLACK_POINT ]   = controls::BlackPointParam( 0.05f );
	params[ PT_WHITE_POINT ]   = controls::WhitePointParam( 1.25f );
	params[ PT_SCANNER_GAMMA ] = controls::ScannerGammaParam( 1.0f );

	params[ PT_FORMAT ]       = 2.0f;//35 mm
	params[ PT_EDGE_TEXT ]    = 1.0f;
	params[ PT_FRAME_NUMBER ] = 12.0f;
	params[ PT_MIX ]          = 1.0f;

	//---------------------------------------------------------------------
	// Declaration. Every ranged FF_TYPE_STANDARD parameter is a plain 0..1
	// float: SetParamInfo clamps a STANDARD default into 0..1 *before* a
	// range can be attached (SDK b1afaf9). The conversions live in
	// Controls.cpp. Option lists are in their natural order and not sorted:
	// the stocks run slow to fast, then the two special cases; the processes
	// and formats are short and ordered by how often they will be wanted.
	//---------------------------------------------------------------------
	auto declareOptions = [ this ]( unsigned int id, const char* name, int count, const char* ( *nameAt )( int ) ) {
		SetOptionParamInfo( id, name, static_cast< unsigned int >( count ), params[ id ] );
		for( int i = 0; i < count; ++i )
			SetParamElementInfo( id, static_cast< unsigned int >( i ), nameAt( i ), static_cast< float >( i ) );
	};
	auto declareInteger = [ this ]( unsigned int id, const char* name, float lo, float hi ) {
		SetParamInfo( id, name, FF_TYPE_INTEGER, params[ id ] );
		SetParamRange( id, lo, hi );
	};

	SetParamInfof( PT_EXPOSURE, "Exposure", FF_TYPE_STANDARD );
	declareOptions( PT_STOCK, "Stock", model::kStockCount, []( int i ) { return model::StockAt( i ).name; } );
	SetParamInfof( PT_PUSH, "Push", FF_TYPE_STANDARD );
	SetParamInfof( PT_AGE, "Age", FF_TYPE_STANDARD );

	declareOptions( PT_PROCESS, "Process", controls::kProcessCount, controls::ProcessName );
	SetParamInfo( PT_MASK, "Mask On", FF_TYPE_BOOLEAN, true );

	SetParamInfof( PT_GRAIN_AMOUNT, "Grain Amount", FF_TYPE_STANDARD );
	SetParamInfof( PT_GRAIN_SIZE, "Grain Size", FF_TYPE_STANDARD );
	declareInteger( PT_GRAIN_SEED, "Grain Seed", 0.0f, 999.0f );

	SetParamInfof( PT_LEAK_AMOUNT, "Leak Amount", FF_TYPE_STANDARD );
	declareOptions( PT_LEAK_EDGE, "Leak Edge", controls::kLeakEdgeCount, controls::LeakEdgeName );
	SetParamInfof( PT_LEAK_WARMTH, "Leak Warmth", FF_TYPE_STANDARD );
	SetParamInfof( PT_LEAK_SPREAD, "Leak Spread", FF_TYPE_STANDARD );

	declareOptions( PT_VIEW, "View", controls::kViewCount, controls::ViewName );
	SetParamInfo( PT_AUTO_LEVELS, "Auto Levels", FF_TYPE_BOOLEAN, false );
	SetParamInfof( PT_BLACK_POINT, "Black Point", FF_TYPE_STANDARD );
	SetParamInfof( PT_WHITE_POINT, "White Point", FF_TYPE_STANDARD );
	SetParamInfof( PT_SCANNER_GAMMA, "Scanner Gamma", FF_TYPE_STANDARD );

	declareOptions( PT_FORMAT, "Format", controls::kFormatCount, controls::FormatName );
	SetParamInfo( PT_EDGE_TEXT, "Edge Text On", FF_TYPE_BOOLEAN, true );
	declareInteger( PT_FRAME_NUMBER, "Frame Number", 0.0f, 99.0f );
	SetParamInfof( PT_MIX, "Mix", FF_TYPE_STANDARD );

	for( FFUInt32 i = PT_EXPOSURE; i <= PT_AGE; ++i )
		SetParamGroup( i, "Scene" );
	for( FFUInt32 i = PT_PROCESS; i <= PT_MASK; ++i )
		SetParamGroup( i, "Process" );
	for( FFUInt32 i = PT_GRAIN_AMOUNT; i <= PT_GRAIN_SEED; ++i )
		SetParamGroup( i, "Grain" );
	for( FFUInt32 i = PT_LEAK_AMOUNT; i <= PT_LEAK_SPREAD; ++i )
		SetParamGroup( i, "Leak" );
	for( FFUInt32 i = PT_VIEW; i <= PT_SCANNER_GAMMA; ++i )
		SetParamGroup( i, "Scan" );
	for( FFUInt32 i = PT_FORMAT; i <= PT_MIX; ++i )
		SetParamGroup( i, "Frame" );

	// The About block. Inline rather than through a helper: SetParamInfo is
	// protected on CFFGLPlugin, so nothing outside the class can call it.
	SetParamInfo( PT_ABOUT_FIRST, "About", FF_TYPE_TEXT, stoatworks::about::defaultText() );
	{
		FFUInt32 aboutId = PT_ABOUT_FIRST + 1;
		for( const auto& b : stoatworks::about::buttons() )
			SetParamInfo( aboutId++, b.label, FF_TYPE_EVENT, false );
	}
	for( FFUInt32 i = PT_ABOUT_FIRST; i < PT_COUNT; ++i )
		SetParamGroup( i, "About" );

	FFGLLog::LogToHost( "Created Rebate effect" );

	diag::init();
}

//---------------------------------------------------------------------------
FFResult Rebate::InitGL( const FFGLViewportStruct* vp )
{
	diag::info( std::string( "GL vendor=" ) + glStringOrUnknown( GL_VENDOR )
	            + " renderer=" + glStringOrUnknown( GL_RENDERER )
	            + " version=" + glStringOrUnknown( GL_VERSION ) );

	const std::string vertex = shaders::Vertex();
	struct
	{
		FFGLShader* shader;
		std::string fragment;
		const char* name;
	} const stages[] = {
		{ &copyShader, shaders::Copy(), "copy" },
		{ &filmShader, shaders::Film(), "film" },
		{ &blocksShader, shaders::Blocks(), "blocks" },
		{ &levelsShader, shaders::Levels(), "levels" },
		{ &scanShader, shaders::Scan(), "scan" },
	};

	for( const auto& stage : stages )
	{
		if( stage.shader->Compile( vertex, stage.fragment ) )
			continue;

		//Returning FF_FAIL here is invisible to the operator: the effect
		//simply does nothing in Resolume, with no message anywhere. These two
		//lines are the only record of which pass it was.
		diag::error( std::string( "the " ) + stage.name + " shader failed to compile - the effect will do nothing" );
		FFGLLog::LogToHost( "Rebate: shader failed to compile" );
		DeInitGL();
		return FF_FAIL;
	}

	if( !quad.Initialise() )
	{
		diag::error( "quad geometry failed to initialise" );
		FFGLLog::LogToHost( "Rebate: quad geometry failed to initialise" );
		DeInitGL();
		return FF_FAIL;
	}

	glGenTextures( 1, &textTexture );
	textKey.clear();
	levelsPrimed  = false;
	autoWasActive = false;
	lastWidth = lastHeight = 0;

	diag::info( "initialised" );

	//Use base-class init as the success result so it retains the viewport.
	return CFFGLPlugin::InitGL( vp );
}

//---------------------------------------------------------------------------
FFResult Rebate::SetTime( double time )
{
	hostTimeSeen = true;
	return CFFGLPlugin::SetTime( time );
}

//The unit voting is readout's, unchanged: the ratio of the host's clock
//delta to a steady clock's names the unit outright, and nothing plausible
//sits between 1 and 1000.
double Rebate::nowSeconds()
{
	const double wallNow = wallSeconds();
	if( wallStart < 0.0 )
		wallStart = wallNow;

	if( !hostTimeSeen || hostTime < 0.0 )
		return wallNow - wallStart;

	const double raw = hostTime;

	if( clockScale == 0.0 && lastRawTime >= 0.0 && lastWallTime >= 0.0 )
	{
		const double hostDelta = raw - lastRawTime;
		const double wallDelta = wallNow - lastWallTime;

		//A paused host, a looping clip or a stalled frame tells us nothing.
		if( hostDelta > 0.0 && wallDelta >= 0.0005 )
		{
			const double ratio = hostDelta / wallDelta;
			if( ratio > 0.1 && ratio < 10.0 )
				++secondsVotes;
			else if( ratio > 100.0 && ratio < 10000.0 )
				++millisVotes;

			if( secondsVotes >= kClockVotes || millisVotes >= kClockVotes )
				clockScale = millisVotes > secondsVotes ? 0.001 : 1.0;
		}
	}
	lastRawTime  = raw;
	lastWallTime = wallNow;

	//Until the unit is settled, run on the real clock rather than assume one:
	//wrong in origin but right in rate, where assuming seconds would be a
	//thousand times fast on Resolume.
	return clockScale != 0.0 ? raw * clockScale : wallNow - wallStart;
}

//---------------------------------------------------------------------------
bool Rebate::uploadText( const frame::Geometry& geometry, const char* code, int frameNumber, bool on )
{
	frame::TextStrip strip = frame::BuildText( geometry, code, frameNumber, on );
	if( strip.key == textKey && textTexture != 0 )
		return true;

	glBindTexture( GL_TEXTURE_2D, textTexture );
	glPixelStorei( GL_UNPACK_ALIGNMENT, 1 );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_R8, strip.width, strip.height, 0, GL_RED, GL_UNSIGNED_BYTE, strip.pixels.data() );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glGenerateMipmap( GL_TEXTURE_2D );
	glBindTexture( GL_TEXTURE_2D, 0 );

	textWidth  = strip.width;
	textHeight = strip.height;
	textKey    = strip.key;
	return true;
}

//---------------------------------------------------------------------------
FFResult Rebate::ProcessOpenGL( ProcessOpenGLStruct* pGL )
{
	if( pGL->numInputTextures < 1 || pGL->inputTextures[ 0 ] == nullptr )
		return FF_FAIL;

	const FFGLTextureStruct& input = *pGL->inputTextures[ 0 ];
	if( input.Width == 0 || input.Height == 0 )
		return FF_FAIL;

	const int width  = static_cast< int >( input.Width );
	const int height = static_cast< int >( input.Height );

	//The host's viewport, read before anything of ours changes it.
	//ScopedFBOBinding restores the framebuffer binding and only that.
	GLint hostViewport[ 4 ] = { 0, 0, 0, 0 };
	glGetIntegerv( GL_VIEWPORT, hostViewport );

	//---------------------------------------------------------------------
	// The clock. Only two things read it: the grain's film frame, and the
	// scanner's level smoothing. Both reduce in double here; nothing
	// absolute crosses into GLSL.
	//---------------------------------------------------------------------
	const double now = nowSeconds();
	double dt        = 0.0;
	if( lastNow >= 0.0 )
		dt = std::clamp( now - lastNow, 0.0, kMaxFrameDelta );
	lastNow = now;

	if( ++clockFrames == 60 )
		diag::info( "host clock at frame 60: raw=" + std::to_string( hostTime ) + " scale=" + std::to_string( clockScale )
		            + " seconds=" + std::to_string( now ) );

	const double filmFrames = std::floor( std::max( now, 0.0 ) * kFilmRate + 1e-6 );
	const int grainFrame    = static_cast< int >( std::fmod( filmFrames, 16777216.0 ) );

	//---------------------------------------------------------------------
	// What the controls say.
	//---------------------------------------------------------------------
	const model::Stock& stock = model::StockAt( controls::OptionIndex( params[ PT_STOCK ], model::kStockCount ) );
	const int process         = controls::OptionIndex( params[ PT_PROCESS ], controls::kProcessCount );
	const bool maskOn         = params[ PT_MASK ] > 0.5f;
	const double exposure     = controls::ExposureStops( params[ PT_EXPOSURE ] );
	const double push         = controls::PushStops( params[ PT_PUSH ] );
	const double age          = std::clamp( stock.age + controls::Age( params[ PT_AGE ] ), 0.0, 1.0 );

	const model::Curve curve = model::Develop( stock, process, push, maskOn,
	                                           ( perturb & model::kPerturbPushGain ) ? 0.5 : 1.0 );
	const model::Profile profile =
		model::ScannerProfile( stock, process, ( perturb & model::kPerturbCrossProfile ) == 0 );
	const bool invert = model::Inverts( process );

	const double toeExposure = std::pow( 10.0, curve.toe );
	float speed[ 3 ], fogExposure[ 3 ];
	for( int i = 0; i < 3; ++i )
	{
		speed[ i ]       = static_cast< float >( std::exp2( exposure ) * std::pow( 10.0, -age * model::kAgeSpeedLoss[ i ] ) );
		fogExposure[ i ] = static_cast< float >( age * model::kAgeFog[ i ] * toeExposure );
	}
	const float textExposure = static_cast< float >( std::pow( 10.0, curve.toe + 0.8 * curve.latitude ) );

	const float grainAmount = controls::GrainAmount( params[ PT_GRAIN_AMOUNT ] );
	const float grainCell   = controls::GrainCellPixels( params[ PT_GRAIN_SIZE ] );
	const int grainSites    = std::clamp( stock.grainSites, 1, model::kMaxGrainSites );
	const int seed          = std::clamp( static_cast< int >( std::lround( params[ PT_GRAIN_SEED ] ) ), 0, 999 );

	const double leakExposure = controls::LeakExposure( params[ PT_LEAK_AMOUNT ] );
	const int leakEdge        = controls::OptionIndex( params[ PT_LEAK_EDGE ], controls::kLeakEdgeCount );
	const float warmth        = std::clamp( params[ PT_LEAK_WARMTH ], 0.0f, 1.0f );
	const float leakSpread    = controls::LeakSpread( params[ PT_LEAK_SPREAD ] );
	float leakWeights[ 3 ];
	for( int i = 0; i < 3; ++i )
		leakWeights[ i ] = 1.0f + ( kWarmLeak[ i ] - 1.0f ) * warmth;

	const int view        = controls::OptionIndex( params[ PT_VIEW ], controls::kViewCount );
	const bool autoLevels = params[ PT_AUTO_LEVELS ] > 0.5f;
	const float blackPt   = controls::BlackPoint( params[ PT_BLACK_POINT ] );
	const float whitePt   = controls::WhitePoint( params[ PT_WHITE_POINT ] );
	const float scanGamma = controls::ScannerGamma( params[ PT_SCANNER_GAMMA ] );

	const int format      = controls::OptionIndex( params[ PT_FORMAT ], controls::kFormatCount );
	const bool edgeText   = params[ PT_EDGE_TEXT ] > 0.5f;
	const int frameNumber = std::clamp( static_cast< int >( std::lround( params[ PT_FRAME_NUMBER ] ) ), 0, 99 );

	const frame::Geometry geometry = frame::Compute( format, width, height );

	//---------------------------------------------------------------------
	// Buffers, and the text. Every allocation and every upload happens here,
	// before anything binds a texture: allocating leaves the active unit
	// bound to nothing, and the symptom of getting the order wrong is correct
	// on every frame except the one that allocates.
	//
	// The two level buffers are 2 x 1 whatever the raster, so a resize never
	// reallocates them -- and so never clears the levels the scanner has
	// settled on. That is the photofinish trap, avoided by construction.
	//---------------------------------------------------------------------
	const int gridW      = std::min( kGridW, width );
	const int gridH      = std::min( kGridH, height );
	const bool allocated = picture.Ensure( width, height, GL_RGBA32F, PassBuffer::Sampling::Mipmapped )
	                       && film.Ensure( width, height, GL_RGBA32F, PassBuffer::Sampling::Nearest )
	                       && blocks.Ensure( gridW, gridH, GL_RGBA32F, PassBuffer::Sampling::Nearest )
	                       && levels[ 0 ].Ensure( 2, 1, GL_RGBA32F, PassBuffer::Sampling::Nearest )
	                       && levels[ 1 ].Ensure( 2, 1, GL_RGBA32F, PassBuffer::Sampling::Nearest );
	if( !allocated )
	{
		diag::error( "could not allocate the pass buffers at " + std::to_string( width ) + "x" + std::to_string( height ) );
		return FF_FAIL;
	}
	uploadText( geometry, stock.code, frameNumber, edgeText );

	const bool resized = lastWidth != 0 && ( lastWidth != width || lastHeight != height );
	lastWidth          = width;
	lastHeight         = height;
	if( resized && ( perturb & model::kPerturbResizeClears ) )
		levelsPrimed = false;

	const bool autoActive = autoLevels && view == 0;
	if( autoActive && !autoWasActive )
		levelsPrimed = false;//switched on: take the next measurement outright
	autoWasActive = autoActive;

	const float alpha = static_cast< float >( 1.0 - std::exp( -dt / kLevelsTau ) );

	const FFGLTexCoords maxCoords = GetMaxGLTexCoords( input );

	//The model's uniforms, for each pass that runs the model library.
	float unwanted[ 9 ];
	for( int r = 0; r < 3; ++r )
		for( int c = 0; c < 3; ++c )
			unwanted[ r * 3 + c ] = r == c ? 0.0f : static_cast< float >( model::kImpurity[ r ][ c ] );
	auto setModel = [ & ]( FFGLShader& shader ) {
		shader.Set( "Gamma", static_cast< float >( curve.gamma ) );
		shader.Set( "Toe", static_cast< float >( curve.toe ) );
		shader.Set( "Latitude", static_cast< float >( curve.latitude ) );
		shader.Set( "Knee", static_cast< float >( model::kKnee ) );
		shader.Set( "Fog", static_cast< float >( curve.fog ) );
		shader.Set( "PositiveImage", curve.positive ? 1 : 0 );
		shader.Set( "Couplers", curve.couplers ? 1 : 0 );
		shader.Set( "Capacity", static_cast< float >( model::kCapacity ) );
		//Row-major in C, so transpose on the way in: then Unwanted * a in GLSL
		//is row = channel, as written.
		glUniformMatrix3fv( shader.FindUniform( "Unwanted" ), 1, GL_TRUE, unwanted );
		shader.Set( "Base", static_cast< float >( model::kBase[ 0 ] ), static_cast< float >( model::kBase[ 1 ] ),
		            static_cast< float >( model::kBase[ 2 ] ) );
		shader.Set( "Perturb", perturb );
	};

	//---------------------------------------------------------------------
	// 1. Copy, with a mip chain.
	//---------------------------------------------------------------------
	{
		ScopedFBOBinding fbo( picture.GetGLID(), ScopedFBOBinding::RB_REVERT );
		picture.ResizeViewPort();
		ScopedShaderBinding shader( copyShader.GetGLID() );
		ScopedSamplerActivation sampler( 0 );
		Scoped2DTextureBinding texture( input.Handle );

		copyShader.Set( "InputTexture", 0 );
		copyShader.Set( "MaxUV", maxCoords.s, maxCoords.t );
		copyShader.Set( "HalfTexel", 0.5f / static_cast< float >( width ), 0.5f / static_cast< float >( height ) );
		quad.Draw();
	}
	picture.GenerateMipmaps();

	//---------------------------------------------------------------------
	// 2. The film: exposure and development, to mean coverage.
	//---------------------------------------------------------------------
	{
		ScopedFBOBinding fbo( film.GetGLID(), ScopedFBOBinding::RB_REVERT );
		film.ResizeViewPort();
		ScopedShaderBinding shader( filmShader.GetGLID() );
		ScopedSamplerActivation sampler0( 0 );
		Scoped2DTextureBinding pictureTexture( picture.TextureID() );
		ScopedSamplerActivation sampler1( 1 );
		Scoped2DTextureBinding textBinding( textTexture );

		setModel( filmShader );
		filmShader.Set( "Picture", 0 );
		filmShader.Set( "Text", 1 );
		glUniform2i( filmShader.FindUniform( "Size" ), width, height );
		filmShader.Set( "Format", geometry.format );
		filmShader.Set( "FilmWidth", static_cast< float >( geometry.filmWidthMm ) );
		filmShader.Set( "MmPerPixel", static_cast< float >( geometry.mmPerPixel ) );
		filmShader.Set( "Gate", static_cast< float >( geometry.gate[ 0 ] ), static_cast< float >( geometry.gate[ 1 ] ),
		                static_cast< float >( geometry.gate[ 2 ] ), static_cast< float >( geometry.gate[ 3 ] ) );
		filmShader.Set( "Crop", static_cast< float >( geometry.crop[ 0 ] ), static_cast< float >( geometry.crop[ 1 ] ),
		                static_cast< float >( geometry.crop[ 2 ] ), static_cast< float >( geometry.crop[ 3 ] ) );
		filmShader.Set( "Holes", geometry.holes ? 1 : 0 );
		filmShader.Set( "BandA", static_cast< float >( geometry.bandA[ 0 ] ), static_cast< float >( geometry.bandA[ 1 ] ) );
		filmShader.Set( "BandB", static_cast< float >( geometry.bandB[ 0 ] ), static_cast< float >( geometry.bandB[ 1 ] ) );
		filmShader.Set( "StripLeft", static_cast< float >( geometry.stripLeftMm ) );
		filmShader.Set( "TextSize", static_cast< float >( textWidth ), static_cast< float >( textHeight ) );
		filmShader.Set( "GlyphsPerMm", static_cast< float >( frame::kGlyphRowsPerMm ) );
		filmShader.Set( "TextExposure", textExposure );

		float crossover[ 9 ];
		for( int r = 0; r < 3; ++r )
			for( int c = 0; c < 3; ++c )
				crossover[ r * 3 + c ] = static_cast< float >( model::kCrossover[ r ][ c ] );
		glUniformMatrix3fv( filmShader.FindUniform( "Crossover" ), 1, GL_TRUE, crossover );
		filmShader.Set( "Speed", speed[ 0 ], speed[ 1 ], speed[ 2 ] );
		filmShader.Set( "FogExposure", fogExposure[ 0 ], fogExposure[ 1 ], fogExposure[ 2 ] );

		filmShader.Set( "LeakExposure", static_cast< float >( leakExposure ) );
		filmShader.Set( "LeakEdge", leakEdge );
		filmShader.Set( "LeakSpread", leakSpread );
		filmShader.Set( "LeakWeights", leakWeights[ 0 ], leakWeights[ 1 ], leakWeights[ 2 ] );
		quad.Draw();
	}

	//---------------------------------------------------------------------
	// 3 and 4. The scanner's auto levels, only when they are looked at.
	//---------------------------------------------------------------------
	if( autoActive )
	{
		{
			ScopedFBOBinding fbo( blocks.GetGLID(), ScopedFBOBinding::RB_REVERT );
			blocks.ResizeViewPort();
			ScopedShaderBinding shader( blocksShader.GetGLID() );
			ScopedSamplerActivation sampler( 0 );
			Scoped2DTextureBinding filmTexture( film.TextureID() );

			setModel( blocksShader );
			blocksShader.Set( "Film", 0 );
			glUniform2i( blocksShader.FindUniform( "Size" ), width, height );
			glUniform2i( blocksShader.FindUniform( "Grid" ), gridW, gridH );
			quad.Draw();
		}

		const int next = 1 - levelsCurrent;
		{
			ScopedFBOBinding fbo( levels[ next ].GetGLID(), ScopedFBOBinding::RB_REVERT );
			levels[ next ].ResizeViewPort();
			ScopedShaderBinding shader( levelsShader.GetGLID() );
			ScopedSamplerActivation sampler0( 0 );
			Scoped2DTextureBinding blocksTexture( blocks.TextureID() );
			ScopedSamplerActivation sampler1( 1 );
			Scoped2DTextureBinding previousTexture( levels[ levelsCurrent ].TextureID() );

			levelsShader.Set( "Blocks", 0 );
			levelsShader.Set( "Previous", 1 );
			glUniform2i( levelsShader.FindUniform( "Grid" ), gridW, gridH );
			levelsShader.Set( "Alpha", alpha );
			levelsShader.Set( "Prime", levelsPrimed ? 0 : 1 );
			quad.Draw();
		}
		levelsCurrent = next;
		levelsPrimed  = true;
	}

	//---------------------------------------------------------------------
	// 5. The scan, straight to the host.
	//---------------------------------------------------------------------
	{
		glBindFramebuffer( GL_FRAMEBUFFER, pGL->HostFBO );
		glViewport( hostViewport[ 0 ], hostViewport[ 1 ], hostViewport[ 2 ], hostViewport[ 3 ] );

		ScopedShaderBinding shader( scanShader.GetGLID() );
		ScopedSamplerActivation sampler0( 0 );
		Scoped2DTextureBinding filmTexture( film.TextureID() );
		ScopedSamplerActivation sampler1( 1 );
		Scoped2DTextureBinding levelsTexture( levels[ levelsCurrent ].TextureID() );
		ScopedSamplerActivation sampler2( 2 );
		Scoped2DTextureBinding sourceTexture( input.Handle );

		setModel( scanShader );
		scanShader.Set( "Film", 0 );
		scanShader.Set( "Levels", 1 );
		scanShader.Set( "Source", 2 );
		scanShader.Set( "MaxUV", maxCoords.s, maxCoords.t );
		glUniform2i( scanShader.FindUniform( "Size" ), width, height );

		scanShader.Set( "View", view );
		scanShader.Set( "Invert", invert ? 1 : 0 );
		scanShader.Set( "AutoLevels", autoLevels ? 1 : 0 );
		scanShader.Set( "ProfileBase", static_cast< float >( profile.base[ 0 ] ), static_cast< float >( profile.base[ 1 ] ),
		                static_cast< float >( profile.base[ 2 ] ) );
		scanShader.Set( "ProfileGamma", static_cast< float >( profile.gamma ) );
		scanShader.Set( "BlackPoint", blackPt );
		scanShader.Set( "WhitePoint", whitePt );
		scanShader.Set( "ScanGamma", scanGamma );

		scanShader.Set( "GrainAmount", grainAmount );
		scanShader.Set( "GrainCell", grainCell );
		scanShader.Set( "GrainSites", grainSites );
		scanShader.Set( "Seed", seed );
		scanShader.Set( "GrainFrame", grainFrame );
		scanShader.Set( "MixAmount", std::clamp( params[ PT_MIX ], 0.0f, 1.0f ) );
		quad.Draw();
	}

	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
FFResult Rebate::DeInitGL()
{
	copyShader.FreeGLResources();
	filmShader.FreeGLResources();
	blocksShader.FreeGLResources();
	levelsShader.FreeGLResources();
	scanShader.FreeGLResources();
	quad.Release();
	picture.Destroy();
	film.Destroy();
	blocks.Destroy();
	levels[ 0 ].Destroy();
	levels[ 1 ].Destroy();
	if( textTexture != 0 )
	{
		glDeleteTextures( 1, &textTexture );
		textTexture = 0;
	}
	textKey.clear();
	levelsPrimed = false;

	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
FFResult Rebate::SetFloatParameter( unsigned int index, float value )
{
	if( index >= PT_COUNT )
		return FF_FAIL;

	// An About button is a press, not a value to keep: it opens a browser and
	// nothing about the effect changes.
	if( index >= PT_ABOUT_FIRST )
		return stoatworks::about::handleParam( index - PT_ABOUT_FIRST, value ) ? FF_SUCCESS : FF_FAIL;

	params[ index ] = value;
	return FF_SUCCESS;
}

float Rebate::GetFloatParameter( unsigned int index )
{
	if( index >= PT_COUNT )
		return 0.0f;

	return params[ index ];
}

//---------------------------------------------------------------------------
char* Rebate::GetTextParameter( unsigned int index )
{
	if( index == PT_ABOUT_FIRST )
	{
		aboutText = stoatworks::about::textParam( 0 );
		return const_cast< char* >( aboutText.c_str() );
	}

	return CFFGLPlugin::GetTextParameter( index );
}

FFResult Rebate::SetTextParameter( unsigned int index, const char* value )
{
	// See the declaration: the base class fails, and a failed default deletes
	// the instance. The About line is display-only, so there is genuinely
	// nothing to store -- but it has to say so successfully.
	if( index == PT_ABOUT_FIRST )
		return FF_SUCCESS;

	return CFFGLPlugin::SetTextParameter( index, value );
}

//---------------------------------------------------------------------------
void Rebate::SetClockScaleForTest( double scale )
{
	clockScale = scale;
}

void Rebate::SetPerturbForTest( int bits )
{
	perturb = bits;
}

bool Rebate::ReadLevelsForTest( float out[ 6 ] )
{
	const GLuint texture = levels[ levelsCurrent ].TextureID();
	if( texture == 0 )
		return false;
	float texels[ 8 ] = {};
	glBindTexture( GL_TEXTURE_2D, texture );
	glGetTexImage( GL_TEXTURE_2D, 0, GL_RGBA, GL_FLOAT, texels );
	glBindTexture( GL_TEXTURE_2D, 0 );
	for( int i = 0; i < 3; ++i )
	{
		out[ i ]     = texels[ i ];
		out[ 3 + i ] = texels[ 4 + i ];
	}
	return true;
}
