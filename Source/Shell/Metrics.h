#pragma once

#include <algorithm>
#include <cstdint>

#include "Geometry/Scale.h"
#include "Geometry/Space.h"
#include "Text/Font.h"

// Every number the run bar is drawn from, stated once in logical pixels and resolved to device
// pixels at one scale.
//
// **Logical pixels are the only unit a shell is entitled to choose in.** `--ui-size` is one angular
// preference and every output derives its scale from it (164), so a logical pixel is the compositor's
// promise about how big something is *at the eye* — the same card subtends the same angle on a 27"
// desk monitor and on a laptop panel held at half the distance. A shell that picked a fraction of the
// panel instead would be a shell whose launcher grows when a person plugs in a bigger screen and
// shrinks when they sit closer to it, which is backwards both times.
//
// That is what the bar used to do, and the argument it inherited was borrowed rather than made: a
// fixed grid is right for the recovery console (38), which has to be legible when nothing else on the
// machine works and so cannot depend on a scale having been derived correctly. The bar is not in that
// position — it is summoned into a running session by a person whose viewing distance the compositor
// already has — so it takes the derived answer.
//
// **The face is the one exception, and it is the ladder's doing rather than a choice.** Spleen is a
// bitmap at six sizes and a glyph does not scale, so the text lands on whichever rung is nearest the
// device height asked for here. `Text` is the cell that actually came back, which is why everything
// vertical is derived from it: a card sized from the nominal height would leave a gap under the text
// wherever the rung and the request disagree, and they disagree at most scales.
struct BarMetrics
{
	// The numbers the shell chooses, all of them logical pixels or percentages. Nested rather than
	// beside the resolved fields below so that the two cannot be confused at a use site: every read of
	// `Logical::` is a number that still has to be converted, and every read without it is device
	// pixels ready to index a buffer with.
	struct Logical
	{
		// How tall the query is set, before the ladder rounds it. Roughly a heading — large enough to
		// read at a glance, small enough that a long command still fits the card.
		static constexpr std::int32_t TextHeight = 24;

		// Air between the text and the card's edge. Horizontal is larger than vertical because the card
		// is much wider than it is tall, and equal padding on a wide box reads as tight at the ends.
		static constexpr std::int32_t PadX = 28;
		static constexpr std::int32_t PadY = 20;

		// The corner. Large enough to read as deliberate rather than as a rectangle somebody softened,
		// and well under half the card's height so the sides stay straight.
		static constexpr std::int32_t Radius = 16;

		// The card's edge, and the caret. Both are a hairline by intent: at any scale above 1 they are
		// drawn at more than one device pixel and still read as a hairline, which is most of why the
		// exact rational is worth reaching for at all.
		static constexpr std::int32_t Edge = 1;
		static constexpr std::int32_t Caret = 2;

		// How wide the card is allowed to get, and the least it may shrink to before it is refused. A
		// launcher spanning a 34" ultrawide would be a line of text with half a metre of empty card
		// after it, so the width is capped and the card centres in whatever is left.
		static constexpr std::int32_t MaxWidth = 720;
		static constexpr std::int32_t MinWidth = 320;

		// The least gap between the card and the edges of the screen, which is what caps the width on a
		// small panel.
		static constexpr std::int32_t Margin = 48;

		// Where the card sits down the screen, as a percentage of the output's height. Above centre
		// rather than on it: a person summoning a launcher is about to read what they typed, eyes rest
		// naturally above the middle of a screen, and it is where a launcher covers least of what is
		// behind it.
		static constexpr std::int32_t TopPercent = 30;
	};

	// Resolves every number above against one output and one scale. `logical` is the output's extent in
	// logical pixels, which is what `xdg_toplevel.configure_bounds` carries.
	//
	// Sizes round up and positions round to nearest, which is Geometry/Scale.h's stated split: a card
	// must cover the logical box it was allotted, and where it sits should land on the device grid
	// rather than consistently drift one way.
	[[nodiscard]] static BarMetrics For(PixelSize<BufferSpace> logical, Scale scale) noexcept
	{
		BarMetrics metrics;

		metrics.Surface = { scale.DeviceFromLogical(logical.Width, Rounding::Up),
			                scale.DeviceFromLogical(logical.Height, Rounding::Up) };

		// The face first, because everything vertical is measured from the cell it returns rather than
		// from `TextHeight`. See the note above the struct.
		metrics.Text = &Nearest(scale.DeviceFromLogical(Logical::TextHeight, Rounding::Nearest));

		metrics.PadX = scale.DeviceFromLogical(Logical::PadX, Rounding::Up);
		metrics.PadY = scale.DeviceFromLogical(Logical::PadY, Rounding::Up);
		metrics.Radius = scale.DeviceFromLogical(Logical::Radius, Rounding::Up);
		metrics.Edge = std::max(scale.DeviceFromLogical(Logical::Edge, Rounding::Nearest), 1);
		metrics.Caret = std::max(scale.DeviceFromLogical(Logical::Caret, Rounding::Nearest), 1);

		// Clamped in logical pixels and converted once, rather than converting three numbers and
		// clamping in device pixels: the cap is a statement about how wide a launcher should look, and
		// resolving it per output would make the same card two widths on one desk.
		const std::int32_t room = logical.Width - 2 * Logical::Margin;
		const std::int32_t wide = std::min(Logical::MaxWidth, std::max(Logical::MinWidth, room));

		metrics.Card = { scale.DeviceFromLogical(wide, Rounding::Up),
			             metrics.Text->CellSize().Height + 2 * metrics.PadY };

		// Refused rather than squeezed. A card wider than the screen it is on is a launcher a person
		// cannot read the end of, and there is nothing useful to draw instead — see `Bar::Draw`.
		metrics.Fits = room >= Logical::MinWidth && metrics.Card.Height + 2 * metrics.PadY <= metrics.Surface.Height;

		metrics.Origin = { (metrics.Surface.Width - metrics.Card.Width) / 2,
			               scale.DeviceFromLogical(logical.Height * Logical::TopPercent / 100, Rounding::Nearest) };

		// Kept on screen even where the percentage would not put it there, which only bites on a very
		// short output — a projector at 480 lines, or a panel rotated into a letterbox.
		metrics.Origin.Y = std::min(metrics.Origin.Y, metrics.Surface.Height - metrics.Card.Height - metrics.PadY);
		metrics.Origin.Y = std::max(metrics.Origin.Y, metrics.PadY);

		return metrics;
	}

	// The whole surface, which covers the output — see Bar.h for why the bar is not card-sized.
	PixelSize<BufferSpace> Surface{};

	// The card itself, and where its top-left corner sits in the surface.
	PixelSize<BufferSpace> Card{};
	PixelPoint<BufferSpace> Origin{};

	// The face the ladder returned. Never null: `Nearest` clamps at both ends of the ladder.
	const Face* Text = nullptr;

	std::int32_t PadX = 0;
	std::int32_t PadY = 0;
	std::int32_t Radius = 0;
	std::int32_t Edge = 0;
	std::int32_t Caret = 0;

	// Whether there is room to draw at all. False is a screen too small for a launcher rather than an
	// error: the bar stays down and says so once.
	bool Fits = false;
};
