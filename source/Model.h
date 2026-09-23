#pragma once

/**
	The film, as numbers. Everything the plugin uploads to the shaders and
	everything the harness needs to state an expectation comes from here, so
	there is one answer to what a stock is.

	**Layers and channels are both indexed R, G, B.** Layer 0 is the
	red-sensitive layer (the bottom one, nearest the base), which forms CYAN
	dye; layer 1 is green-sensitive and forms MAGENTA; layer 2 is the
	blue-sensitive top layer and forms YELLOW. A dye is indexed by the layer
	that formed it, and a scanner channel by the colour of light it reads, so
	"dye 0 in channel 0" is cyan dye seen by red light -- its wanted
	absorption.

	**The characteristic curve** of every layer, in every process, is

		c( x ) = ( sp( x - Toe ) - sp( x - Toe - Latitude ) ) / Latitude
		sp( u ) = ln( 1 + e^( Knee u ) ) / Knee

	with x = log10 H. c runs from 0 (nothing developed) to 1 (the shoulder),
	and the dye a layer carries is Fog + Gamma x Latitude x c for a negative,
	or its mirror image for a reversal (positive) image. Well inside the
	straight line the slope dD/dx is Gamma to within Gamma x ( e^(-Knee dt) +
	e^(-Knee ds) ), dt and ds being the distances to the toe and the
	shoulder; the straight line's extension meets the fog level at exactly
	x = Toe and the shoulder level at exactly Toe + Latitude. That is what
	lets the harness read the curve back out of a picture and say where it
	bends.

	**Grain** acts on c, not on density: c is the fraction of a layer's
	developable grains that developed, which is what a Poisson draw of dye
	clouds per grain cell can realise. See the scan shader.
*/
namespace rebate::model
{

//---------------------------------------------------------------------------
// The curve.
//---------------------------------------------------------------------------

/// Scene mid grey, linear. The exposure the camera was metered for.
constexpr double kMidGrey = 0.18;

/// log10 of mid grey. A metered negative puts mid grey this far above the
/// toe: 1.3 log units is 4.3 stops of shadow detail.
double MidGreyLog();
constexpr double kNegativeToeBelowMid = 1.3;

/// Log units from toe to shoulder in a colour negative: nine stops.
constexpr double kNegativeLatitude = 2.7;

/// How sharply the toe and the shoulder bend, per log10 unit. The bend is
/// about 4 / kKnee = 0.67 log units wide, two stops.
constexpr double kKnee = 6.0;

/// Push: development time in stops. Each stop multiplies the straight-line
/// gamma by ( 1 + kPushGamma ) (linearly: gamma( p ) = gamma0 ( 1 + 0.15 p ))
/// and adds kPushFog of chemical fog density. These are the model's stated
/// amounts; `rbtest --push` reads them back out of a wedge.
constexpr double kPushGamma = 0.15;
constexpr double kPushFog   = 0.03;

/// Process options.
enum Process
{
	kC41   = 0,///< colour negative chemistry
	kE6    = 1,///< reversal chemistry: the film becomes a positive
	kCross = 2,///< reversal FILM through C-41 chemistry
	kProcessCount = 3
};

//---------------------------------------------------------------------------
// The dyes.
//---------------------------------------------------------------------------

/// The density of the clear film base, per scanner channel.
constexpr double kBase[ 3 ] = { 0.06, 0.06, 0.07 };

/// Scanner channel c's absorption by dye d, per unit of dye d's own
/// (wanted) density: row = channel R, G, B; column = dye cyan, magenta,
/// yellow. The diagonal is 1 by definition. The off-diagonal terms are the
/// dyes' unwanted absorptions: cyan dye absorbs some green and a little
/// blue; magenta absorbs a good deal of blue. They are what the orange mask
/// exists to cancel.
constexpr double kImpurity[ 3 ][ 3 ] = {
	{ 1.00, 0.06, 0.01 },
	{ 0.18, 1.00, 0.08 },
	{ 0.08, 0.32, 1.00 },
};

/// The sum of a channel's unwanted absorptions, S_c = sum over d != c.
double UnwantedSum( int channel );

/// Masking coupler capacity, in units of dye density: the most dye each
/// layer's couplers can form. Uncoupled coupler is coloured and carries
/// exactly the unwanted absorptions of the dye it would have formed, so the
/// mask in channel c is sum_d U[c][d] ( Capacity - a_d ), and the total
/// unwanted absorption is sum_d U[c][d] Capacity whatever the dyes are: a
/// constant, the orange base. Dye cannot form beyond the coupler, so a masked
/// layer's dye is clamped here (coupler exhaustion -- only reached at an
/// extreme push).
constexpr double kCapacity = 2.4;

/// Scene RGB (linear) to each layer's exposure: row = layer R, G, B. Each row
/// sums to 1, so a neutral scene exposes the three layers equally. The
/// off-diagonal terms are the spectral crossover: a real layer's
/// sensitivity overlaps its neighbours'.
constexpr double kCrossover[ 3 ][ 3 ] = {
	{ 0.85, 0.12, 0.03 },
	{ 0.08, 0.84, 0.08 },
	{ 0.02, 0.10, 0.88 },
};

//---------------------------------------------------------------------------
// Ageing. The top (blue-sensitive) layer is hit hardest: it is the fastest
// emulsion and the first to see everything.
//---------------------------------------------------------------------------

/// Speed lost at Age 1, in log10 H, per layer R, G, B.
constexpr double kAgeSpeedLoss[ 3 ] = { 0.05, 0.10, 0.25 };

/// Age fog is an EXPOSURE, not a density: background radiation and heat
/// accumulated in storage. It goes through the curve, so it lifts the
/// shadows and leaves the highlights. At Age 1, as a multiple of the toe
/// exposure 10^Toe, per layer R, G, B.
constexpr double kAgeFog[ 3 ] = { 0.40, 0.80, 3.00 };

//---------------------------------------------------------------------------
// Stocks. Invented names; described by their parameters.
//---------------------------------------------------------------------------
struct Stock
{
	const char* name;     ///< what the Stock menu says
	const char* code;     ///< what the edge print says
	bool reversal;        ///< a reversal (slide) emulsion: no masking couplers
	double gamma;         ///< straight-line gamma as a C-41 negative
	int grainSites;       ///< N: developable sites per grain cell per layer
	double fog;           ///< chemical fog density at normal development
	double age;           ///< intrinsic age, added to the Age control
};

constexpr int kStockCount = 5;
extern const Stock kStocks[ kStockCount ];
const Stock& StockAt( int index );

/// Grain sites are bounded because the scan shader loops over them.
constexpr int kMaxGrainSites = 64;

//---------------------------------------------------------------------------
// A developed film: the curve every layer follows, and what else is in it.
//---------------------------------------------------------------------------
struct Curve
{
	double gamma    = 0.6; ///< straight-line slope, density per log10 H
	double toe      = 0.0; ///< log10 H where the straight line meets Fog
	double latitude = 2.7; ///< log10 H from toe to shoulder
	double fog      = 0.1; ///< dye density with no exposure (negative), or
	                       ///< at full exposure (positive)
	bool positive   = false;///< a reversal image: density FALLS with exposure
	bool couplers   = false;///< masking couplers present (the orange mask)
};

/// The film a stock becomes under a process, pushed `push` stops, with or
/// without its mask. `pushGainScale` is a test hook: 1 in the plugin.
Curve Develop( const Stock& stock, int process, double push, bool maskOn, double pushGainScale = 1.0 );

/// Whether the emulsion is a reversal one: the stock is, or Cross treats it
/// as one.
bool ReversalEmulsion( const Stock& stock, int process );

/// Whether the scanner inverts: the image is a negative unless the chemistry
/// was E-6.
bool Inverts( int process );

/// What the scanner's own profile assumes, for a manual scan. For C-41
/// chemistry the lab's scanner is set for a MASKED colour negative of the
/// stock's gamma, fresh and normally developed, whatever the film really is:
/// cross-processed reversal film has no mask, and the scanner divides by one
/// anyway. For E-6 it expects an unmasked slide. `maskAssumed` is a test
/// hook, true in the plugin.
struct Profile
{
	double base[ 3 ] = { 0, 0, 0 };///< the unexposed density it divides by
	double gamma     = 0.6;        ///< what it takes the straight line to be
};
Profile ScannerProfile( const Stock& stock, int process, bool maskAssumed = true );

/// Coverage c( x ) and the dye a layer carries, in double, for the harness to
/// choose its inputs with. Not used to state what a check expects.
double Coverage( const Curve& curve, double logH );
double Dye( const Curve& curve, double c );

/// Channel densities of three dye amounts, with the mask when the film has
/// couplers.
void ChannelDensity( const Curve& curve, const double dye[ 3 ], double out[ 3 ] );

//---------------------------------------------------------------------------
// Negative-control hooks. The plugin always runs with 0; each bit perturbs
// the MODEL (not the harness's expectation) so a check can be shown to fail.
//---------------------------------------------------------------------------
enum Perturb : int
{
	kPerturbGamma       = 1 << 0,///< the curve's gamma x 0.9, in the shader
	kPerturbPushGain    = 1 << 1,///< push raises gamma at half the stated rate
	kPerturbGrainLaw    = 1 << 2,///< grain variance proportional to c, not c(1-c)
	kPerturbLeakLinear  = 1 << 3,///< the leak added as coverage after the curve
	kPerturbCouplersFixed = 1 << 4,///< couplers not consumed: the mask never cancels
	kPerturbCrossProfile = 1 << 5,///< the scanner expects no mask under C-41
	kPerturbHolesOpaque = 1 << 6,///< sprocket holes carry the base density
	kPerturbSeedIgnored = 1 << 7,///< grain hash ignores Grain Seed
	kPerturbLeakCool    = 1 << 8,///< the leak's spectrum reversed, blue-weighted
	kPerturbResizeClears = 1 << 9,///< a resize re-primes the scanner's levels
};

} // namespace rebate::model
