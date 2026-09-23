#pragma once

#include "Frame.h"
#include "PassBuffer.h"
#include "StoatworksAboutParams.h"

#include <FFGLSDK.h>

#include <string>

/**
	Rebate -- colour negative film, and the scan of it, as an FFGL effect.

	**The one idea.** A film look is usually a lookup table. This is the
	process. The clip is the SCENE: it exposes the three dye layers of a
	colour negative through a spectral crossover, each layer is developed
	along a characteristic curve into dye (with the orange mask that cancels
	the dyes' own impurities), the dye is realised as clouds in grain cells,
	and the flat orange negative is then scanned and inverted the way a lab
	scanner does it. Every film artefact is one of those stages doing what it
	does:

	  - the orange negative (View: Negative) is the mask;
	  - grain is loudest in the mid-tones because a Poisson draw's variance is
	    c ( 1 - c ) / N;
	  - expired stock has blue shadows because age fog is an exposure and the
	    top layer takes most of it;
	  - a light leak is warm and saturates because it is an exposure through
	    the base, where the red-sensitive layer is;
	  - cross-processing is reversal film with no mask, developed steep, and
	    scanned by a scanner that divides by a mask that is not there;
	  - the rebate is unexposed film, the edge print a latent image, and the
	    sprocket holes clear film base.

	**Five passes**, in `Shaders.h`. The model is `Model.h` (numbers) and the
	`kModel` GLSL library (arithmetic). See AGENTS.md for the traps.
*/
class Rebate : public CFFGLPlugin
{
public:
	Rebate();

	//CFFGLPlugin
	FFResult InitGL( const FFGLViewportStruct* vp ) override;
	FFResult ProcessOpenGL( ProcessOpenGLStruct* pGL ) override;
	FFResult DeInitGL() override;

	FFResult SetFloatParameter( unsigned int index, float value ) override;
	float GetFloatParameter( unsigned int index ) override;
	FFResult SetTime( double time ) override;

	char* GetTextParameter( unsigned int index ) override;

	/// Declared only so the About line can accept its own default.
	/// instantiateGL pushes every declared default back through the setters
	/// and deletes the whole instance if one fails, and CFFGLPlugin's
	/// SetTextParameter is a stub that returns exactly that failure.
	FFResult SetTextParameter( unsigned int index, const char* value ) override;

	/// Clock test hook: the harness DECLARES its unit rather than leaving the
	/// voting to infer one.
	void SetClockScaleForTest( double scale );

	/// Negative-control hooks, a bitmask of `model::Perturb`. Always 0 in the
	/// plugin; each bit perturbs the model so a check can be shown to fail.
	void SetPerturbForTest( int bits );

	/// The scanner's smoothed levels as they stand: least dense rgb, then
	/// most dense rgb. For `rbtest --resize`. Needs the GL context current.
	bool ReadLevelsForTest( float out[ 6 ] );

	/// Everything the operator can reach, in the order Resolume shows them.
	enum ParamID : FFUInt32
	{
		//Scene
		PT_EXPOSURE,
		PT_STOCK,
		PT_PUSH,
		PT_AGE,

		//Process
		PT_PROCESS,
		PT_MASK,

		//Grain
		PT_GRAIN_AMOUNT,
		PT_GRAIN_SIZE,
		PT_GRAIN_SEED,

		//Leak
		PT_LEAK_AMOUNT,
		PT_LEAK_EDGE,
		PT_LEAK_WARMTH,
		PT_LEAK_SPREAD,

		//Scan
		PT_VIEW,
		PT_AUTO_LEVELS,
		PT_BLACK_POINT,
		PT_WHITE_POINT,
		PT_SCANNER_GAMMA,

		//Frame
		PT_FORMAT,
		PT_EDGE_TEXT,
		PT_FRAME_NUMBER,
		PT_MIX,

		//About. FFGL has no window, so the name, the version and the links are
		//parameters the host draws. Last, so no saved composition's ids shift.
		PT_ABOUT_FIRST,
		PT_COUNT = PT_ABOUT_FIRST + stoatworks::about::kParamCount
	};

	/// The block grid the scanner's auto levels look at, at most.
	static constexpr int kGridW = 64;
	static constexpr int kGridH = 36;

	/// The scanner's levels settle with this time constant, in seconds: a lab
	/// scanner sets a roll once, so a per-frame auto exposure that pumped with
	/// every bright object would be the wrong model.
	static constexpr double kLevelsTau = 0.25;

private:
	/// The host's clock in seconds, whatever unit it arrived in.
	double nowSeconds();

	bool uploadText( const rebate::frame::Geometry& geometry, const char* code, int frameNumber, bool on );

	ffglex::FFGLShader copyShader;
	ffglex::FFGLShader filmShader;
	ffglex::FFGLShader blocksShader;
	ffglex::FFGLShader levelsShader;
	ffglex::FFGLShader scanShader;
	ffglex::FFGLScreenQuad quad;

	rebate::PassBuffer picture;   ///< the scene, RGBA32F, mipmapped
	rebate::PassBuffer film;      ///< mean coverage per layer, and the region
	rebate::PassBuffer blocks;    ///< mean channel density per block
	rebate::PassBuffer levels[ 2 ];///< ping-pong, 2 x 1: never reallocated
	int levelsCurrent = 0;
	bool levelsPrimed = false;
	bool autoWasActive = false;
	int lastWidth     = 0;
	int lastHeight    = 0;

	GLuint textTexture = 0;
	int textWidth      = 1;
	int textHeight     = 14;
	std::string textKey;

	//--- the clock (readout's unit voting) -----------------------------------
	bool hostTimeSeen   = false;
	double clockScale   = 0.0;///< 0 until decided; then 1.0 or 0.001
	double wallStart    = -1.0;
	double lastWallTime = -1.0;
	double lastRawTime  = -1.0;
	int secondsVotes    = 0;
	int millisVotes     = 0;
	double lastNow      = -1.0;
	int clockFrames     = 0;

	int perturb = 0;

	/// Zero-initialised: the About block's ids are never stored to, so
	/// without this GetFloatParameter hands the host whatever was on the
	/// stack for them.
	float params[ PT_COUNT ] = {};

	/// GetTextParameter hands the host a bare pointer, so the string has to
	/// outlive the call.
	std::string aboutText;
};
