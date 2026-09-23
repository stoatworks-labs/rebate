#pragma once

#include <cstdint>
#include <string>
#include <vector>

/**
	The rebate: where the picture sits on the strip of film, where the
	sprocket holes are, and what the edge print says.

	Film coordinates are millimetres. `u` runs ALONG the strip from the centre
	of the frame, `v` runs ACROSS it from the top edge of the film. The strip
	runs horizontally and the film's full width fills the output's height, the
	way a strip scanner sees it. Everything the film pass needs is a plain
	number from here; the text is a small bitmap built on the CPU from the
	fleet's 5x7 font and uploaded once whenever it changes.

	The perforation is the common negative perforation, 2.794 x 1.981 mm on a
	4.75 mm pitch, eight to a 38 mm frame, 2.01 mm in from each edge. The edge
	print is invented: a stock code, and frame numbers every half frame with
	an A on the half. No real manufacturer's name or mark appears anywhere.
*/
namespace rebate::frame
{

enum Format
{
	kFull   = 0,///< the picture fills the output: no rebate at all
	kSquare = 1,///< 6x6 on 61.5 mm roll film: a square frame, no holes
	kStrip  = 2,///< 35 mm: 36 x 24 frame, perforations, edge print
};

constexpr double kPerfPitch  = 4.75;
constexpr double kPerfWidth  = 2.794;///< along the strip
constexpr double kPerfHeight = 1.981;///< across the strip
constexpr double kPerfEdge   = 2.01; ///< film edge to the near side of the hole
constexpr double kPerfRadius = 0.5;

/// Edge print is 1 mm tall: seven glyph rows to the millimetre, and the
/// glyph pixels are square.
constexpr double kGlyphRowsPerMm = 7.0;

struct Geometry
{
	int format          = kFull;
	double filmWidthMm  = 0.0;///< across the strip; the output's height
	double mmPerPixel   = 0.0;
	/// The gate: the exposed picture, in film mm (u left/right, v top/bottom).
	double gate[ 4 ]    = { 0, 0, 0, 0 };
	/// The part of the scene the gate shows, as a top-down uv rectangle
	/// (x, y, width, height): a centre crop to the gate's aspect.
	double crop[ 4 ]    = { 0, 0, 1, 1 };
	bool holes          = false;
	/// Text bands, v top and v bottom in mm. A band whose bottom is not
	/// below its top carries nothing.
	double bandA[ 2 ]   = { 0, 0 };
	double bandB[ 2 ]   = { 0, 0 };
	/// The text strip's first column, in u mm.
	double stripLeftMm  = 0.0;
};

Geometry Compute( int format, int width, int height );

/// The edge print as a bitmap: two lines of kGlyph rows (line A rows 0..6,
/// line B rows 7..13), one texel per glyph pixel, 255 where exposed.
struct TextStrip
{
	int width  = 1;
	int height = 14;
	std::vector< uint8_t > pixels;
	/// What was drawn, so the plugin rebuilds only when this changes.
	std::string key;
};

TextStrip BuildText( const Geometry& geometry, const char* code, int frameNumber, bool on );

} // namespace rebate::frame
