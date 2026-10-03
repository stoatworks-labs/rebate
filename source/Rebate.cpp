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
	// Defaults: a 400-speed portrait negative, scanned manually, on a 35 mm
	// strip. They live in render::HostValues, which the OpenFX build reads
	// too, so the two inspectors open on the same film; the reasoning for
	// each is there.
	//---------------------------------------------------------------------
	const render::HostValues defaults;
	params[ PT_EXPOSURE ] = defaults.exposure;
	params[ PT_STOCK ]    = defaults.stock;
	params[ PT_PUSH ]     = defaults.push;
	params[ PT_AGE ]      = defaults.age;

	params[ PT_PROCESS ] = defaults.process;
	params[ PT_MASK ]    = defaults.maskOn;

	params[ PT_GRAIN_AMOUNT ] = defaults.grainAmount;
	params[ PT_GRAIN_SIZE ]   = defaults.grainSize;
	params[ PT_GRAIN_SEED ]   = defaults.grainSeed;

	params[ PT_LEAK_AMOUNT ] = defaults.leakAmount;
	params[ PT_LEAK_EDGE ]   = defaults.leakEdge;
	params[ PT_LEAK_WARMTH ] = defaults.leakWarmth;
	params[ PT_LEAK_SPREAD ] = defaults.leakSpread;

	params[ PT_VIEW ]          = defaults.view;
	params[ PT_AUTO_LEVELS ]   = defaults.autoLevels;
	params[ PT_BLACK_POINT ]   = defaults.blackPoint;
	params[ PT_WHITE_POINT ]   = defaults.whitePoint;
	params[ PT_SCANNER_GAMMA ] = defaults.scannerGamma;

	params[ PT_FORMAT ]       = defaults.format;
	params[ PT_EDGE_TEXT ]    = defaults.edgeText;
	params[ PT_FRAME_NUMBER ] = defaults.frameNumber;
	params[ PT_MIX ]          = defaults.mix;

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
	SetParamInfo( PT_MASK, "Mask On", FF_TYPE_BOOLEAN, defaults.maskOn > 0.5f );

	SetParamInfof( PT_GRAIN_AMOUNT, "Grain Amount", FF_TYPE_STANDARD );
	SetParamInfof( PT_GRAIN_SIZE, "Grain Size", FF_TYPE_STANDARD );
	declareInteger( PT_GRAIN_SEED, "Grain Seed", 0.0f, 999.0f );

	SetParamInfof( PT_LEAK_AMOUNT, "Leak Amount", FF_TYPE_STANDARD );
	declareOptions( PT_LEAK_EDGE, "Leak Edge", controls::kLeakEdgeCount, controls::LeakEdgeName );
	SetParamInfof( PT_LEAK_WARMTH, "Leak Warmth", FF_TYPE_STANDARD );
	SetParamInfof( PT_LEAK_SPREAD, "Leak Spread", FF_TYPE_STANDARD );

	declareOptions( PT_VIEW, "View", controls::kViewCount, controls::ViewName );
	SetParamInfo( PT_AUTO_LEVELS, "Auto Levels", FF_TYPE_BOOLEAN, defaults.autoLevels > 0.5f );
	SetParamInfof( PT_BLACK_POINT, "Black Point", FF_TYPE_STANDARD );
	SetParamInfof( PT_WHITE_POINT, "White Point", FF_TYPE_STANDARD );
	SetParamInfof( PT_SCANNER_GAMMA, "Scanner Gamma", FF_TYPE_STANDARD );

	declareOptions( PT_FORMAT, "Format", controls::kFormatCount, controls::FormatName );
	SetParamInfo( PT_EDGE_TEXT, "Edge Text On", FF_TYPE_BOOLEAN, defaults.edgeText > 0.5f );
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
render::HostValues Rebate::hostValues() const
{
	render::HostValues v;
	v.exposure     = params[ PT_EXPOSURE ];
	v.stock        = params[ PT_STOCK ];
	v.push         = params[ PT_PUSH ];
	v.age          = params[ PT_AGE ];
	v.process      = params[ PT_PROCESS ];
	v.maskOn       = params[ PT_MASK ];
	v.grainAmount  = params[ PT_GRAIN_AMOUNT ];
	v.grainSize    = params[ PT_GRAIN_SIZE ];
	v.grainSeed    = params[ PT_GRAIN_SEED ];
	v.leakAmount   = params[ PT_LEAK_AMOUNT ];
	v.leakEdge     = params[ PT_LEAK_EDGE ];
	v.leakWarmth   = params[ PT_LEAK_WARMTH ];
	v.leakSpread   = params[ PT_LEAK_SPREAD ];
	v.view         = params[ PT_VIEW ];
	v.autoLevels   = params[ PT_AUTO_LEVELS ];
	v.blackPoint   = params[ PT_BLACK_POINT ];
	v.whitePoint   = params[ PT_WHITE_POINT ];
	v.scannerGamma = params[ PT_SCANNER_GAMMA ];
	v.format       = params[ PT_FORMAT ];
	v.edgeText     = params[ PT_EDGE_TEXT ];
	v.frameNumber  = params[ PT_FRAME_NUMBER ];
	v.mix          = params[ PT_MIX ];
	return v;
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

	//---------------------------------------------------------------------
	// What the controls say, and the grain's film frame: render::Prepare,
	// which the OpenFX build calls too. Every uniform below comes from it.
	//---------------------------------------------------------------------
	const render::Uniforms u        = render::Prepare( hostValues(), width, height, now, perturb );
	const model::Curve& curve       = u.curve;
	const frame::Geometry& geometry = u.geometry;

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
	const int gridW      = u.gridW;
	const int gridH      = u.gridH;
	const bool allocated = picture.Ensure( width, height, GL_RGBA32F, PassBuffer::Sampling::Linear )
	                       && film.Ensure( width, height, GL_RGBA32F, PassBuffer::Sampling::Nearest )
	                       && blocks.Ensure( gridW, gridH, GL_RGBA32F, PassBuffer::Sampling::Nearest )
	                       && levels[ 0 ].Ensure( 2, 1, GL_RGBA32F, PassBuffer::Sampling::Nearest )
	                       && levels[ 1 ].Ensure( 2, 1, GL_RGBA32F, PassBuffer::Sampling::Nearest );
	if( !allocated )
	{
		diag::error( "could not allocate the pass buffers at " + std::to_string( width ) + "x" + std::to_string( height ) );
		return FF_FAIL;
	}
	uploadText( geometry, u.stock->code, u.frameNumber, u.edgeText );

	const bool resized = lastWidth != 0 && ( lastWidth != width || lastHeight != height );
	lastWidth          = width;
	lastHeight         = height;
	if( resized && ( perturb & model::kPerturbResizeClears ) )
		levelsPrimed = false;

	const bool autoActive = u.autoActive;
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
	// 1. Copy. Float, so a float input survives exactly; no mip chain,
	// because generating one for a 4K float picture cost 11 ms a frame.
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
		filmShader.Set( "TextExposure", u.textExposure );

		float crossover[ 9 ];
		for( int r = 0; r < 3; ++r )
			for( int c = 0; c < 3; ++c )
				crossover[ r * 3 + c ] = static_cast< float >( model::kCrossover[ r ][ c ] );
		glUniformMatrix3fv( filmShader.FindUniform( "Crossover" ), 1, GL_TRUE, crossover );
		filmShader.Set( "Speed", u.speed[ 0 ], u.speed[ 1 ], u.speed[ 2 ] );
		filmShader.Set( "FogExposure", u.fogExposure[ 0 ], u.fogExposure[ 1 ], u.fogExposure[ 2 ] );

		filmShader.Set( "LeakExposure", static_cast< float >( u.leakExposure ) );
		filmShader.Set( "LeakEdge", u.leakEdge );
		filmShader.Set( "LeakSpread", u.leakSpread );
		filmShader.Set( "LeakWeights", u.leakWeights[ 0 ], u.leakWeights[ 1 ], u.leakWeights[ 2 ] );
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

		scanShader.Set( "View", u.view );
		scanShader.Set( "Invert", u.invert ? 1 : 0 );
		scanShader.Set( "AutoLevels", u.autoLevels ? 1 : 0 );
		scanShader.Set( "ProfileBase", static_cast< float >( u.profile.base[ 0 ] ), static_cast< float >( u.profile.base[ 1 ] ),
		                static_cast< float >( u.profile.base[ 2 ] ) );
		scanShader.Set( "ProfileGamma", static_cast< float >( u.profile.gamma ) );
		scanShader.Set( "BlackPoint", u.blackPoint );
		scanShader.Set( "WhitePoint", u.whitePoint );
		scanShader.Set( "ScanGamma", u.scanGamma );

		scanShader.Set( "GrainAmount", u.grainAmount );
		scanShader.Set( "GrainCell", u.grainCell );
		scanShader.Set( "GrainSites", u.grainSites );
		scanShader.Set( "Seed", u.seed );
		scanShader.Set( "GrainFrame", u.grainFrame );
		scanShader.Set( "MixAmount", u.mix );
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
