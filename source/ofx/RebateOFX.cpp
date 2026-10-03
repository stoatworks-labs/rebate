/// The OpenFX build of Rebate, for DaVinci Resolve, Nuke, Natron, Vegas and
/// other OFX hosts.
///
/// ------------------------------------------------------- what is shared
///
/// **The film.** `Model.cpp` (the stocks, the curve each develops to, the
/// scanner's profile), `Controls.cpp` (what every slider means), `Frame.cpp`
/// and `Font.cpp` (the rebate's geometry and its edge print) and
/// `render::Prepare` (the per-frame arithmetic -- speed and age fog per layer,
/// the leak's spectrum, the grain's film frame) are linked straight from
/// source: the same code the FFGL plugin runs. The per-pixel passes are the
/// one thing written twice, in `Render.cpp` against the GLSL in `Shaders.cpp`,
/// and `rbtest --cpu` renders both and compares them.
///
/// What this file does is marshalling: OFX's pixel formats in, float RGBA
/// through the film, and back out. Nothing is quantised on the way: an 8-bit
/// clip is widened to float once, a float clip goes through untouched --
/// values above 1 included, which the film takes as scene light above white,
/// exactly as the FFGL build's float copy does -- and only an integer output
/// is rounded, once, at the end.
///
/// --------------------------------------------- what is different, and why
///
/// **Auto Levels measures each frame on its own.** The FFGL build follows the
/// frame's least and most dense blocks with a quarter-second one-pole filter,
/// which integrates over every frame the host has shown it. An OFX host
/// renders frames in any order, alone and concurrently, and holds nothing
/// between them, so there is no previous frame's level to smooth from. The
/// measurement is exactly the one the FFGL build primes on; the smoothing is
/// absent rather than faked, and the plugin description says so. (Auto Levels
/// is off by default in both builds.)
///
/// **The grain's clock is the timeline.** The FFGL build takes the film frame
/// from the host's clock, `floor( seconds x 24 )`. Here seconds are the frame
/// number over the clip's frame rate, so a frame grains the same way however
/// and whenever it is rendered.
///
/// **No audio, no beat sync, no event buttons** -- the FFGL build has none to
/// drop. The About block is OFX's own: a folded group with real buttons.
///
/// ------------------------------------------------------------- and tiles
///
/// The film formats reduce the whole scene into the gate and Auto Levels
/// measures the whole picture, so no tile is enough to render from:
/// `setSupportsTiles( false )` is a statement of fact.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "ofxsImageEffect.h"
#include "ofxsMultiThread.h"

// After the OFX Support headers, which is where the OFX types come from.
#include "StoatworksAboutOFX.h"

#include "../Controls.h"
#include "../Model.h"
#include "../Render.h"

namespace
{
constexpr const char* kPluginIdentifier = "com.stoatworks.rebate";
constexpr const char* kPluginName       = "Rebate";
constexpr const char* kPluginGrouping   = "Stoatworks";
constexpr const char* kPluginDescription =
	"Colour negative film, and the scan of it.\n\n"
	"The clip is the scene. It exposes three dye layers, which are developed "
	"along a characteristic curve into dyes carrying an orange mask, realised "
	"as grain, and then scanned and inverted the way a lab scanner does it.\n\n"
	"What falls out: the orange negative, mid-tone grain, blue shadows on "
	"expired stock, warm light leaks, cross-processing, and the rebate with its "
	"sprocket holes and edge print.\n\n"
	"The input is taken as display-encoded (sRGB / Rec.709-style) picture and "
	"the output is encoded the same way. Float input is used as it is, values "
	"above 1 included.\n\n"
	"Differences from the Resolume build: Auto Levels measures every frame on "
	"its own, because OpenFX renders frames in any order and there is no "
	"previous frame to smooth the scanner's levels against -- the Resolume "
	"build settles them over a quarter of a second. Grain changes 24 times a "
	"second of timeline time.\n\n"
	"https://stoatworks-labs.com";

// Script names. Permanent: saved projects refer to them.
constexpr const char* kParamExposure     = "exposure";
constexpr const char* kParamStock        = "stock";
constexpr const char* kParamPush         = "push";
constexpr const char* kParamAge          = "age";
constexpr const char* kParamProcess      = "process";
constexpr const char* kParamMask         = "maskOn";
constexpr const char* kParamGrainAmount  = "grainAmount";
constexpr const char* kParamGrainSize    = "grainSize";
constexpr const char* kParamGrainSeed    = "grainSeed";
constexpr const char* kParamLeakAmount   = "leakAmount";
constexpr const char* kParamLeakEdge     = "leakEdge";
constexpr const char* kParamLeakWarmth   = "leakWarmth";
constexpr const char* kParamLeakSpread   = "leakSpread";
constexpr const char* kParamView         = "view";
constexpr const char* kParamAutoLevels   = "autoLevels";
constexpr const char* kParamBlackPoint   = "blackPoint";
constexpr const char* kParamWhitePoint   = "whitePoint";
constexpr const char* kParamScannerGamma = "scannerGamma";
constexpr const char* kParamFormat       = "format";
constexpr const char* kParamEdgeText     = "edgeText";
constexpr const char* kParamFrameNumber  = "frameNumber";
constexpr const char* kParamMix          = "mix";

using namespace rebate;

const char* stockName( int index )
{
	return model::StockAt( index ).name;
}

//---------------------------------------------------------------------------
// Rows across the host's threads. The multi-thread suite when the host has
// one, a single call when it does not (the Support library's own fallback).
//---------------------------------------------------------------------------
class RowJob : public OFX::MultiThread::Processor
{
public:
	RowJob( int rows, const std::function< void( int, int ) >& body ) :
		rows( rows ), body( body )
	{
	}

	void multiThreadFunction( unsigned int index, unsigned int count ) override
	{
		const int y0 = static_cast< int >( static_cast< int64_t >( rows ) * index / count );
		const int y1 = static_cast< int >( static_cast< int64_t >( rows ) * ( index + 1 ) / count );
		if( y1 > y0 )
			body( y0, y1 );
	}

private:
	int rows;
	const std::function< void( int, int ) >& body;
};

void hostThreads( int rows, const std::function< void( int, int ) >& body )
{
	RowJob job( rows, body );
	job.multiThread();
}

//---------------------------------------------------------------------------
// Marshalling: OFX pixels to premultiplied float RGBA with row 0 at the
// bottom -- the orientation OFX itself uses, and GL's, so nothing flips --
// and back. The film takes premultiplied colour as the scene: a transparent
// pixel is no light, which is black film-side, and the scan is opaque.
//---------------------------------------------------------------------------
template< typename Pixel, int Components, int Maximum >
void gather( const OFX::Image* src, const OfxRectI& bounds, bool premultiplied, std::vector< float >& out )
{
	const int width     = bounds.x2 - bounds.x1;
	const int height    = bounds.y2 - bounds.y1;
	const float maximum = static_cast< float >( Maximum );

	//Divided, not multiplied by a reciprocal: c / 255 correctly rounded is
	//what GL's normalised upload gives the FFGL build, and c x ( 1 / 255 )
	//is an ULP away for some codes -- which is enough, now and then, to move
	//a grain site across its threshold.
	for( int y = 0; y < height; ++y )
	{
		float* row = out.data() + static_cast< size_t >( y ) * width * 4;
		for( int x = 0; x < width; ++x )
		{
			const Pixel* px = static_cast< const Pixel* >( src->getPixelAddress( bounds.x1 + x, bounds.y1 + y ) );
			float* dst      = row + static_cast< size_t >( x ) * 4;
			if( px == nullptr )
			{
				dst[ 0 ] = dst[ 1 ] = dst[ 2 ] = dst[ 3 ] = 0.0f;
				continue;
			}

			const float a = Components == 4 ? static_cast< float >( px[ 3 ] ) / maximum : 1.0f;
			for( int c = 0; c < 3; ++c )
			{
				const float v = static_cast< float >( px[ c ] ) / maximum;
				dst[ c ]      = premultiplied ? v : v * a;
			}
			dst[ 3 ] = a;
		}
	}
}

template< typename Pixel, int Components, int Maximum >
void scatter( const std::vector< float >& in, OFX::Image* dst, const OfxRectI& bounds, const OfxRectI& window,
              bool premultiplied )
{
	const int width   = bounds.x2 - bounds.x1;
	const float scale = static_cast< float >( Maximum );

	const int x1 = std::max( window.x1, bounds.x1 ), x2 = std::min( window.x2, bounds.x2 );
	const int y1 = std::max( window.y1, bounds.y1 ), y2 = std::min( window.y2, bounds.y2 );

	for( int y = y1; y < y2; ++y )
	{
		for( int x = x1; x < x2; ++x )
		{
			Pixel* px = static_cast< Pixel* >( dst->getPixelAddress( x, y ) );
			if( px == nullptr )
				continue;

			const float* source = in.data() + ( static_cast< size_t >( y - bounds.y1 ) * width + ( x - bounds.x1 ) ) * 4;
			const float a       = source[ 3 ];

			for( int c = 0; c < 3; ++c )
			{
				float v = source[ c ];
				if( !premultiplied )
					v = a > 0.0f ? v / a : 0.0f;

				//Integer formats clamp and round, once, here. Float is left
				//alone: the scan is already in 0..1, and Mix below 1 may
				//legitimately carry a float source's values outside it.
				if( Maximum != 1 )
					v = std::clamp( v, 0.0f, 1.0f );
				px[ c ] = static_cast< Pixel >( Maximum == 1 ? v : std::lround( v * scale ) );
			}

			if( Components == 4 )
				px[ 3 ] = static_cast< Pixel >( Maximum == 1 ? a : std::lround( std::clamp( a, 0.0f, 1.0f ) * scale ) );
		}
	}
}

class RebateOFXPlugin : public OFX::ImageEffect
{
public:
	explicit RebateOFXPlugin( OfxImageEffectHandle handle ) :
		OFX::ImageEffect( handle )
	{
		dstClip = fetchClip( kOfxImageEffectOutputClipName );
		srcClip = fetchClip( kOfxImageEffectSimpleSourceClipName );

		exposure     = fetchDoubleParam( kParamExposure );
		stock        = fetchChoiceParam( kParamStock );
		push         = fetchDoubleParam( kParamPush );
		age          = fetchDoubleParam( kParamAge );
		process      = fetchChoiceParam( kParamProcess );
		maskOn       = fetchBooleanParam( kParamMask );
		grainAmount  = fetchDoubleParam( kParamGrainAmount );
		grainSize    = fetchDoubleParam( kParamGrainSize );
		grainSeed    = fetchIntParam( kParamGrainSeed );
		leakAmount   = fetchDoubleParam( kParamLeakAmount );
		leakEdge     = fetchChoiceParam( kParamLeakEdge );
		leakWarmth   = fetchDoubleParam( kParamLeakWarmth );
		leakSpread   = fetchDoubleParam( kParamLeakSpread );
		view         = fetchChoiceParam( kParamView );
		autoLevels   = fetchBooleanParam( kParamAutoLevels );
		blackPoint   = fetchDoubleParam( kParamBlackPoint );
		whitePoint   = fetchDoubleParam( kParamWhitePoint );
		scannerGamma = fetchDoubleParam( kParamScannerGamma );
		format       = fetchChoiceParam( kParamFormat );
		edgeText     = fetchBooleanParam( kParamEdgeText );
		frameNumber  = fetchIntParam( kParamFrameNumber );
		mix          = fetchDoubleParam( kParamMix );
	}

	void render( const OFX::RenderArguments& args ) override
	{
		std::unique_ptr< OFX::Image > dst( dstClip->fetchImage( args.time ) );
		std::unique_ptr< OFX::Image > src( srcClip->fetchImage( args.time ) );
		if( dst == nullptr || src == nullptr )
			OFX::throwSuiteStatusException( kOfxStatFailed );

		const OFX::BitDepthEnum depth       = dst->getPixelDepth();
		const OFX::PixelComponentEnum comps = dst->getPixelComponents();
		if( comps != OFX::ePixelComponentRGBA && comps != OFX::ePixelComponentRGB )
			OFX::throwSuiteStatusException( kOfxStatErrUnsupported );
		if( src->getPixelDepth() != depth || src->getPixelComponents() != comps )
			OFX::throwSuiteStatusException( kOfxStatErrImageFormat );

		//The film's raster is the output's: its full width fills the output's
		//height, wherever the source's data happens to stop.
		const OfxRectI bounds = dst->getBounds();
		const int width       = bounds.x2 - bounds.x1;
		const int height      = bounds.y2 - bounds.y1;
		if( width <= 0 || height <= 0 )
			return;

		//OFX time is FRAMES. Seconds come from the clip's frame rate, and a
		//host that reports zero would otherwise divide by it.
		double fps = srcClip->getFrameRate();
		if( !( fps > 0.0 ) )
			fps = dstClip->getFrameRate();
		if( !( fps > 0.0 ) )
			fps = 24.0;

		render::Uniforms u = render::Prepare( valuesAt( args.time ), width, height, args.time / fps );

		//A proxy or draft render is a smaller raster of the same picture. The
		//geometry is already in film millimetres of the output's height;
		//the grain cell is in pixels, so it shrinks with the picture and a
		//reduced render point-samples the full one's grain.
		if( args.renderScale.x > 0.0 && args.renderScale.x < 1.0 )
			u.grainCell = static_cast< float >( u.grainCell * args.renderScale.x );

		//An RGB clip has no alpha to be premultiplied by; treating it as
		//premultiplied is what makes the round trip an identity there.
		const bool premultiplied =
			comps != OFX::ePixelComponentRGBA || srcClip->getPreMultiplication() != OFX::eImageUnPreMultiplied;

		std::vector< float > frame( static_cast< size_t >( width ) * height * 4 );
		switch( depth )
		{
		case OFX::eBitDepthUByte:
			comps == OFX::ePixelComponentRGBA ? gather< unsigned char, 4, 255 >( src.get(), bounds, premultiplied, frame )
			                                  : gather< unsigned char, 3, 255 >( src.get(), bounds, premultiplied, frame );
			break;
		case OFX::eBitDepthUShort:
			comps == OFX::ePixelComponentRGBA ? gather< unsigned short, 4, 65535 >( src.get(), bounds, premultiplied, frame )
			                                  : gather< unsigned short, 3, 65535 >( src.get(), bounds, premultiplied, frame );
			break;
		case OFX::eBitDepthFloat:
			comps == OFX::ePixelComponentRGBA ? gather< float, 4, 1 >( src.get(), bounds, premultiplied, frame )
			                                  : gather< float, 3, 1 >( src.get(), bounds, premultiplied, frame );
			break;
		default:
			OFX::throwSuiteStatusException( kOfxStatErrUnsupported );
		}

		//The whole frame, in place: the scan reads each source pixel (for
		//Mix) before it writes the same pixel and nothing else.
		render::Apply( u, frame.data(), frame.data(), hostThreads );

		switch( depth )
		{
		case OFX::eBitDepthUByte:
			comps == OFX::ePixelComponentRGBA
				? scatter< unsigned char, 4, 255 >( frame, dst.get(), bounds, args.renderWindow, premultiplied )
				: scatter< unsigned char, 3, 255 >( frame, dst.get(), bounds, args.renderWindow, premultiplied );
			break;
		case OFX::eBitDepthUShort:
			comps == OFX::ePixelComponentRGBA
				? scatter< unsigned short, 4, 65535 >( frame, dst.get(), bounds, args.renderWindow, premultiplied )
				: scatter< unsigned short, 3, 65535 >( frame, dst.get(), bounds, args.renderWindow, premultiplied );
			break;
		case OFX::eBitDepthFloat:
			comps == OFX::ePixelComponentRGBA
				? scatter< float, 4, 1 >( frame, dst.get(), bounds, args.renderWindow, premultiplied )
				: scatter< float, 3, 1 >( frame, dst.get(), bounds, args.renderWindow, premultiplied );
			break;
		default:
			OFX::throwSuiteStatusException( kOfxStatErrUnsupported );
		}
	}

	void changedParam( const OFX::InstanceChangedArgs& args, const std::string& paramName ) override
	{
		// The About links open a browser and change nothing about the render.
		if( stoatworks::about::ofx::changedParam( args, paramName ) )
			return;
	}

private:
	/// The controls at `time`, in the FFGL host's own shape, so the same
	/// Controls.cpp converts them.
	render::HostValues valuesAt( double time ) const
	{
		const auto slider = [ time ]( OFX::DoubleParam* p ) { return static_cast< float >( p->getValueAtTime( time ) ); };
		//ChoiceParam answers through an out parameter, unlike every other
		//param type in the Support library.
		const auto choice = [ time ]( OFX::ChoiceParam* p ) {
			int value = 0;
			p->getValueAtTime( time, value );
			return static_cast< float >( value );
		};
		const auto flag    = [ time ]( OFX::BooleanParam* p ) { return p->getValueAtTime( time ) ? 1.0f : 0.0f; };
		const auto integer = [ time ]( OFX::IntParam* p ) { return static_cast< float >( p->getValueAtTime( time ) ); };

		render::HostValues v;
		v.exposure     = slider( exposure );
		v.stock        = choice( stock );
		v.push         = slider( push );
		v.age          = slider( age );
		v.process      = choice( process );
		v.maskOn       = flag( maskOn );
		v.grainAmount  = slider( grainAmount );
		v.grainSize    = slider( grainSize );
		v.grainSeed    = integer( grainSeed );
		v.leakAmount   = slider( leakAmount );
		v.leakEdge     = choice( leakEdge );
		v.leakWarmth   = slider( leakWarmth );
		v.leakSpread   = slider( leakSpread );
		v.view         = choice( view );
		v.autoLevels   = flag( autoLevels );
		v.blackPoint   = slider( blackPoint );
		v.whitePoint   = slider( whitePoint );
		v.scannerGamma = slider( scannerGamma );
		v.format       = choice( format );
		v.edgeText     = flag( edgeText );
		v.frameNumber  = integer( frameNumber );
		v.mix          = slider( mix );
		return v;
	}

	OFX::Clip* dstClip = nullptr;
	OFX::Clip* srcClip = nullptr;

	OFX::DoubleParam* exposure     = nullptr;
	OFX::ChoiceParam* stock        = nullptr;
	OFX::DoubleParam* push         = nullptr;
	OFX::DoubleParam* age          = nullptr;
	OFX::ChoiceParam* process      = nullptr;
	OFX::BooleanParam* maskOn      = nullptr;
	OFX::DoubleParam* grainAmount  = nullptr;
	OFX::DoubleParam* grainSize    = nullptr;
	OFX::IntParam* grainSeed       = nullptr;
	OFX::DoubleParam* leakAmount   = nullptr;
	OFX::ChoiceParam* leakEdge     = nullptr;
	OFX::DoubleParam* leakWarmth   = nullptr;
	OFX::DoubleParam* leakSpread   = nullptr;
	OFX::ChoiceParam* view         = nullptr;
	OFX::BooleanParam* autoLevels  = nullptr;
	OFX::DoubleParam* blackPoint   = nullptr;
	OFX::DoubleParam* whitePoint   = nullptr;
	OFX::DoubleParam* scannerGamma = nullptr;
	OFX::ChoiceParam* format       = nullptr;
	OFX::BooleanParam* edgeText    = nullptr;
	OFX::IntParam* frameNumber     = nullptr;
	OFX::DoubleParam* mix          = nullptr;
};

//---------------------------------------------------------------------------
// Description helpers.
//---------------------------------------------------------------------------
/// A group's script name is `<label>Group`, never the label itself: "Process"
/// would otherwise differ from the Process choice's script name by case
/// alone, and nothing in OFX promises a host compares names case-sensitively.
OFX::GroupParamDescriptor* defineGroup( OFX::ImageEffectDescriptor& desc, OFX::PageParamDescriptor* page,
                                        const char* name, const char* label )
{
	OFX::GroupParamDescriptor* group = desc.defineGroupParam( name );
	group->setLabels( label, label, label );
	page->addChild( *group );
	return group;
}

void defineSlider( OFX::ImageEffectDescriptor& desc, OFX::PageParamDescriptor* page, OFX::GroupParamDescriptor* group,
                   const char* name, const char* label, const char* hint, double value )
{
	OFX::DoubleParamDescriptor* param = desc.defineDoubleParam( name );
	param->setLabels( label, label, label );
	param->setHint( hint );
	param->setRange( 0.0, 1.0 );
	param->setDisplayRange( 0.0, 1.0 );
	param->setDoubleType( OFX::eDoubleTypePlain );
	param->setDefault( value );
	param->setParent( *group );
	page->addChild( *param );
}

void defineChoice( OFX::ImageEffectDescriptor& desc, OFX::PageParamDescriptor* page, OFX::GroupParamDescriptor* group,
                   const char* name, const char* label, const char* hint, int count, const char* ( *labelFor )( int ),
                   float value )
{
	OFX::ChoiceParamDescriptor* param = desc.defineChoiceParam( name );
	param->setLabels( label, label, label );
	param->setHint( hint );
	for( int i = 0; i < count; ++i )
		param->appendOption( labelFor( i ) );
	param->setDefault( controls::OptionIndex( value, count ) );
	param->setAnimates( false );
	param->setParent( *group );
	page->addChild( *param );
}

void defineToggle( OFX::ImageEffectDescriptor& desc, OFX::PageParamDescriptor* page, OFX::GroupParamDescriptor* group,
                   const char* name, const char* label, const char* hint, float value )
{
	OFX::BooleanParamDescriptor* param = desc.defineBooleanParam( name );
	param->setLabels( label, label, label );
	param->setHint( hint );
	param->setDefault( value > 0.5f );
	param->setAnimates( false );
	param->setParent( *group );
	page->addChild( *param );
}

void defineInteger( OFX::ImageEffectDescriptor& desc, OFX::PageParamDescriptor* page, OFX::GroupParamDescriptor* group,
                    const char* name, const char* label, const char* hint, int lo, int hi, float value )
{
	OFX::IntParamDescriptor* param = desc.defineIntParam( name );
	param->setLabels( label, label, label );
	param->setHint( hint );
	param->setRange( lo, hi );
	param->setDisplayRange( lo, hi );
	param->setDefault( static_cast< int >( std::lround( value ) ) );
	param->setParent( *group );
	page->addChild( *param );
}

mDeclarePluginFactory( RebatePluginFactory, {}, {} );
} // namespace

void RebatePluginFactory::describe( OFX::ImageEffectDescriptor& desc )
{
	desc.setLabels( kPluginName, kPluginName, kPluginName );
	desc.setPluginGrouping( kPluginGrouping );
	desc.setPluginDescription( kPluginDescription );

	desc.addSupportedContext( OFX::eContextFilter );
	desc.addSupportedContext( OFX::eContextGeneral );

	desc.addSupportedBitDepth( OFX::eBitDepthUByte );
	desc.addSupportedBitDepth( OFX::eBitDepthUShort );
	desc.addSupportedBitDepth( OFX::eBitDepthFloat );

	// The film formats reduce the whole scene into the gate and Auto Levels
	// measures the whole picture: no tile is enough. Each frame is a function
	// of its own source frame, its own parameters and its own time, so there is
	// no temporal access and nothing held between renders.
	desc.setSupportsTiles( false );
	desc.setTemporalClipAccess( false );
	desc.setRenderThreadSafety( OFX::eRenderFullySafe );
	desc.setSupportsMultiResolution( true );
	desc.setSupportsMultipleClipPARs( false );
	desc.setSupportsMultipleClipDepths( false );
}

void RebatePluginFactory::describeInContext( OFX::ImageEffectDescriptor& desc, OFX::ContextEnum )
{
	OFX::ClipDescriptor* srcClip = desc.defineClip( kOfxImageEffectSimpleSourceClipName );
	srcClip->addSupportedComponent( OFX::ePixelComponentRGBA );
	srcClip->addSupportedComponent( OFX::ePixelComponentRGB );
	srcClip->setTemporalClipAccess( false );
	srcClip->setSupportsTiles( false );

	OFX::ClipDescriptor* dstClip = desc.defineClip( kOfxImageEffectOutputClipName );
	dstClip->addSupportedComponent( OFX::ePixelComponentRGBA );
	dstClip->addSupportedComponent( OFX::ePixelComponentRGB );
	dstClip->setSupportsTiles( false );

	// The same controls, groups, 0..1 ranges and defaults as the FFGL build --
	// the defaults are literally the same struct -- so the two inspectors read
	// identically and one user guide covers both.
	OFX::PageParamDescriptor* page = desc.definePageParam( "Controls" );
	const render::HostValues d;

	//------------------------------------------------------------------ Scene
	OFX::GroupParamDescriptor* scene = defineGroup( desc, page, "sceneGroup", "Scene" );
	defineSlider( desc, page, scene, kParamExposure, "Exposure",
	              "The camera's exposure, -6 to +6 stops across the slider; 0.5 is exactly 0 stops.", d.exposure );
	defineChoice( desc, page, scene, kParamStock, "Stock",
	              "The film. Faster stocks have fewer, larger dye clouds per grain cell, so louder grain. Slide 100 is a "
	              "reversal film; Expired 400 is Portrait 400 at an intrinsic age of 0.6.",
	              model::kStockCount, stockName, d.stock );
	defineSlider( desc, page, scene, kParamPush, "Push",
	              "Development time, -1 (a one-stop pull) to +3 stops; 0.25 is normal. Each stop raises gamma by 15% and "
	              "chemical fog by 0.03.",
	              d.push );
	defineSlider( desc, page, scene, kParamAge, "Age",
	              "Storage age, added to the stock's own. Fog as an exposure, the top layer taking most: blue shadows, "
	              "warm highlights.",
	              d.age );

	//---------------------------------------------------------------- Process
	OFX::GroupParamDescriptor* processGroup = defineGroup( desc, page, "processGroup", "Process" );
	defineChoice( desc, page, processGroup, kParamProcess, "Process",
	              "The chemistry. Cross runs reversal film through C-41: steep, no mask, and scanned by a scanner that "
	              "expects one.",
	              controls::kProcessCount, controls::ProcessName, d.process );
	defineToggle( desc, page, processGroup, kParamMask, "Mask On",
	              "The orange mask: masking couplers that cancel the dyes' unwanted absorptions. Off, the manual "
	              "scanner still divides by the mask it expects, and the picture goes dark, red and muddy. No effect "
	              "on Slide 100 or under Cross, which have no couplers.",
	              d.maskOn );

	//------------------------------------------------------------------ Grain
	OFX::GroupParamDescriptor* grainGroup = defineGroup( desc, page, "grainGroup", "Grain" );
	defineSlider( desc, page, grainGroup, kParamGrainAmount, "Grain Amount",
	              "How much of the physical fluctuation to show: 1 is the full binomial c(1-c)/N, 0 is none.",
	              d.grainAmount );
	defineSlider( desc, page, grainGroup, kParamGrainSize, "Grain Size",
	              "Pixels per grain cell, 1 to 8, geometric; 0 is one pixel.", d.grainSize );
	defineInteger( desc, page, grainGroup, kParamGrainSeed, "Grain Seed",
	               "Which grain. The same seed and timeline frame always grain the same way.", 0, 999, d.grainSeed );

	//------------------------------------------------------------------- Leak
	OFX::GroupParamDescriptor* leakGroup = defineGroup( desc, page, "leakGroup", "Leak" );
	defineSlider( desc, page, leakGroup, kParamLeakAmount, "Leak Amount",
	              "Light through the base at one edge, as an exposure: 0 is none, then 4 stops under to 10 over mid "
	              "grey. It goes through the curve, so it saturates.",
	              d.leakAmount );
	defineChoice( desc, page, leakGroup, kParamLeakEdge, "Leak Edge", "Which edge the light gets in at.",
	              controls::kLeakEdgeCount, controls::LeakEdgeName, d.leakEdge );
	defineSlider( desc, page, leakGroup, kParamLeakWarmth, "Leak Warmth",
	              "0 is a neutral leak; 1 reaches mostly the red-sensitive layer, as light through the base does.",
	              d.leakWarmth );
	defineSlider( desc, page, leakGroup, kParamLeakSpread, "Leak Spread",
	              "How far the leak reaches, 0.05 to 1 frame height, geometric.", d.leakSpread );

	//------------------------------------------------------------------- Scan
	OFX::GroupParamDescriptor* scanGroup = defineGroup( desc, page, "scanGroup", "Scan" );
	defineChoice( desc, page, scanGroup, kParamView, "View",
	              "Positive is the scan, inverted. Negative is the film itself on a light box, orange mask and all.",
	              controls::kViewCount, controls::ViewName, d.view );
	defineToggle( desc, page, scanGroup, kParamAutoLevels, "Auto Levels",
	              "Per-channel levels from the frame's least and most dense blocks, as a lab scanner sets them. Measured "
	              "on every frame on its own here (the Resolume build smooths them over a quarter second). Removes much "
	              "of what Exposure, Age and Cross do, as a lab would.",
	              d.autoLevels );
	defineSlider( desc, page, scanGroup, kParamBlackPoint, "Black Point",
	              "Manual scan of a negative: the density above the profile's base that prints black, 0 to 0.6. "
	              "Under E-6 it sets the density that prints white.",
	              d.blackPoint );
	defineSlider( desc, page, scanGroup, kParamWhitePoint, "White Point",
	              "Manual scan of a negative: the density above the profile's base that prints white, 0.6 to 3.0. "
	              "Under E-6 it sets the density that prints black.",
	              d.whitePoint );
	defineSlider( desc, page, scanGroup, kParamScannerGamma, "Scanner Gamma",
	              "The scanner's contrast, 0.5 to 2, geometric; 0.5 is exactly 1.", d.scannerGamma );

	//------------------------------------------------------------------ Frame
	OFX::GroupParamDescriptor* frameGroup = defineGroup( desc, page, "frameGroup", "Frame" );
	defineChoice( desc, page, frameGroup, kParamFormat, "Format",
	              "Full: the picture fills the frame. 6x6: a square frame on roll film. 35 mm: the strip, with its "
	              "sprocket holes and edge print. The film's width fills the output's height.",
	              controls::kFormatCount, controls::FormatName, d.format );
	defineToggle( desc, page, frameGroup, kParamEdgeText, "Edge Text On", "The latent edge print in the rebate.",
	              d.edgeText );
	defineInteger( desc, page, frameGroup, kParamFrameNumber, "Frame Number",
	               "The number the 35 mm edge print gives this frame. Roll film has none.", 0, 99, d.frameNumber );
	defineSlider( desc, page, frameGroup, kParamMix, "Mix", "Wet/dry against the untouched input.", d.mix );

	// The Stoatworks About block: a read-only credit line and one push button per
	// link, in a group that starts folded. Last, so it sits under the effect's
	// own controls.
	stoatworks::about::ofx::describe( desc, page );
}

OFX::ImageEffect* RebatePluginFactory::createInstance( OfxImageEffectHandle handle, OFX::ContextEnum )
{
	return new RebateOFXPlugin( handle );
}

void OFX::Plugin::getPluginIDs( OFX::PluginFactoryArray& ids )
{
	// Deliberately leaked: a by-value static would register an exit-time
	// destructor inside this module, and a host that dlclose()s the bundle
	// before process exit then jumps through a dangling pointer.
	static RebatePluginFactory* factory =
		new RebatePluginFactory( kPluginIdentifier, PLUGIN_VERSION_MAJOR, PLUGIN_VERSION_MINOR );
	ids.push_back( factory );
}
