#include "Gym/Card.h"

#include <cerrno>
#include <utility>

namespace
{
// A word in the layout Gym/Card.h states: alpha in the top byte, then red, green, blue, read as one
// little-endian 32-bit value. Spelled once here so that no region below writes a shift.
[[nodiscard]] constexpr std::uint32_t Pack(std::uint8_t alpha, std::uint8_t red, std::uint8_t green, std::uint8_t blue)
{
	return (static_cast<std::uint32_t>(alpha) << 24) | (static_cast<std::uint32_t>(red) << 16) |
	       (static_cast<std::uint32_t>(green) << 8) | static_cast<std::uint32_t>(blue);
}

// The card's own ground. Dark and slightly blue, and deliberately not black: a card that failed to
// import at all is nothing on the scene's own backdrop, and a card whose ground was black would make
// the two the same picture.
constexpr std::uint32_t Ground = Pack(255, 32, 34, 38);

constexpr std::uint32_t White = Pack(255, 255, 255, 255);
constexpr std::uint32_t Black = Pack(255, 0, 0, 0);

// The three primaries at full amplitude, which is what makes a channel-order bug a colour rather than
// a tint. A card of muted colours would report `ARGB` sampled as `ABGR` as *slightly off*, and slightly
// off is what everything else on this card already reports.
constexpr std::uint32_t Red = Pack(255, 255, 0, 0);
constexpr std::uint32_t Green = Pack(255, 0, 255, 0);
constexpr std::uint32_t Blue = Pack(255, 0, 0, 255);

// Half of linear one, encoded in sRGB: 1.055 * 0.5^(1/2.4) - 0.055 is 0.7354, which is 187.5 of 255.
//
// **This number is the whole gamma diagnostic and it only works beside the checkerboard.** A one-texel
// checkerboard of black and white averages to half the *light*, so under any minification a sampler
// that decodes before it filters resolves that patch to exactly this value and the two halves become
// one flat rectangle. A sampler that filters the encoded bytes resolves it to 128 instead, which is
// visibly darker than its neighbour — the error every naive image scaler in the world has, worth about
// a stop, and invisible on any picture that is not this one.
constexpr std::uint32_t MidGrey = Pack(255, 188, 188, 188);

// The two phases, as a bar that changes colour *and* side. Either alone would do; both together mean a
// swap that lands on the wrong buffer and a swap that does not land at all are different pictures.
constexpr std::uint32_t FirstPhase = Pack(255, 0, 220, 220);
constexpr std::uint32_t SecondPhase = Pack(255, 240, 210, 0);

// A rectangle in texels, half-open at the right and bottom for the reason every other rectangle in the
// tree is: adjacent regions share an edge coordinate and no texel is written twice.
struct Box
{
	std::int32_t Left = 0;
	std::int32_t Top = 0;
	std::int32_t Right = 0;
	std::int32_t Bottom = 0;
};

// The layout is sixteenths of the side, so that the card is the same instrument at 64 texels and at
// 512 and the regions land on the same fractions either way.
[[nodiscard]] constexpr std::int32_t Part(std::int32_t side, std::int32_t sixteenths) noexcept
{
	return side * sixteenths / 16;
}

// One of `count` equal columns of `box`, which is what the three colour bars are and nothing else
// wants. Computed from the ends rather than by accumulating a width, so the last column reaches the
// box's own right edge instead of leaving a seam of ground where the division did not come out even.
[[nodiscard]] constexpr Box Column(Box box, std::int32_t index, std::int32_t count) noexcept
{
	const std::int32_t span = box.Right - box.Left;

	return { box.Left + span * index / count, box.Top, box.Left + span * (index + 1) / count, box.Bottom };
}

class Canvas
{
public:
	Canvas(std::vector<std::uint32_t>& words, std::int32_t side) noexcept : m_Words{ &words }, m_Side{ side } {}

	void Fill(Box box, std::uint32_t word) noexcept
	{
		for (std::int32_t y = box.Top; y < box.Bottom; ++y)
		{
			for (std::int32_t x = box.Left; x < box.Right; ++x)
			{
				(*m_Words)
					[static_cast<std::size_t>(y) * static_cast<std::size_t>(m_Side) + static_cast<std::size_t>(x)] =
						word;
			}
		}
	}

	// A one-texel checkerboard. One texel rather than two, because the diagnostic is what a *filter*
	// does with the finest detail the buffer can carry: a coarser board survives minification as a
	// board and stops testing anything.
	void Checkerboard(Box box) noexcept
	{
		for (std::int32_t y = box.Top; y < box.Bottom; ++y)
		{
			for (std::int32_t x = box.Left; x < box.Right; ++x)
			{
				Fill({ x, y, x + 1, y + 1 }, ((x + y) % 2 == 0) ? White : Black);
			}
		}
	}

	// One-texel lines, along whichever axis the caller asked for. Both axes are drawn on every card
	// because a filter that is right in one direction and wrong in the other is an ordinary way to get
	// a separable resample wrong, and one block of stripes would report half of it.
	void Stripes(Box box, bool vertical) noexcept
	{
		for (std::int32_t y = box.Top; y < box.Bottom; ++y)
		{
			for (std::int32_t x = box.Left; x < box.Right; ++x)
			{
				Fill({ x, y, x + 1, y + 1 }, ((vertical ? x : y) % 2 == 0) ? White : Black);
			}
		}
	}

private:
	std::vector<std::uint32_t>* m_Words;
	std::int32_t m_Side;
};
} // namespace

Result<Card> Card::Draw(std::int32_t side, AlphaMode alpha, CardPhase phase)
{
	if (side < MinimumSide)
	{
		return Failure(EINVAL, "a card below the minimum side has regions too small to read");
	}

	std::vector<std::uint32_t> words(static_cast<std::size_t>(side) * static_cast<std::size_t>(side), Ground);
	Canvas canvas{ words, side };

	const std::int32_t left = Part(side, 1);
	const std::int32_t right = Part(side, 15);
	const std::int32_t middle = Part(side, 8);

	// The orientation mark, and it is one square in one corner rather than a bracket in each. A single
	// asymmetric mark distinguishes all four of the ways an import can arrive turned — a vertical flip
	// puts it at the bottom, a horizontal one at the right, a half turn diagonally opposite — and the
	// three-mark version everybody draws costs a region to say the same thing.
	canvas.Fill({ left, Part(side, 1), Part(side, 3), Part(side, 3) }, White);

	// The channel bars. Full-amplitude red, green and blue, left to right in that order, so a card
	// sampled as `ABGR` reads blue-green-red and is wrong at a glance rather than under a colour picker.
	const Box bars{ left, Part(side, 3), right, Part(side, 6) };
	canvas.Fill(Column(bars, 0, 3), Red);
	canvas.Fill(Column(bars, 1, 3), Green);
	canvas.Fill(Column(bars, 2, 3), Blue);

	// The alpha pair, and both blocks are read against the scene's backdrop rather than against the
	// card — which is why Gym/Lanes.h's backdrop is a fill and not black.
	//
	// **Half-alpha white** is the fold: composited over a dark ground it is a mid grey, and a compositor
	// that has the convention backwards produces either a white that never dims or a grey twice as dark
	// as it should be. **Fully transparent** is the cruder question underneath it — whether alpha is
	// being read at all — and it is written as magenta because magenta is a colour the card uses nowhere
	// else, so a block of it appearing is unambiguous rather than a shade of something.
	//
	// Premultiplied is the same two texels with the colour folded in, which for the transparent block
	// leaves transparent black: the two spellings are *not* distinguishable there, and that is a
	// property of premultiplied alpha rather than something to work around. The straight card is where
	// that question is asked, and the two cards side by side is what asks it.
	const bool straight = alpha == AlphaMode::Straight;
	const Box alphaRow{ left, Part(side, 6), middle, Part(side, 10) };
	canvas.Fill(
		{ alphaRow.Left, alphaRow.Top, Part(side, 4), alphaRow.Bottom },
		straight ? Pack(128, 255, 255, 255) : Pack(128, 128, 128, 128)
	);
	canvas.Fill(
		{ Part(side, 4), alphaRow.Top, alphaRow.Right, alphaRow.Bottom },
		straight ? Pack(0, 255, 0, 255) : Pack(0, 0, 0, 0)
	);

	// The gamma pair: the checkerboard and the value it resolves to if the decode happens first.
	canvas.Checkerboard({ middle, Part(side, 6), Part(side, 11), Part(side, 10) });
	canvas.Fill({ Part(side, 11), Part(side, 6), right, Part(side, 10) }, MidGrey);

	// The stripe blocks, vertical on the left and horizontal on the right.
	canvas.Stripes({ left, Part(side, 10), middle, Part(side, 14) }, true);
	canvas.Stripes({ middle, Part(side, 10), right, Part(side, 14) }, false);

	// The phase bar: which buffer this is, as a colour on a side.
	const bool first = phase == CardPhase::First;
	canvas.Fill(
		{ first ? left : middle, Part(side, 14), first ? middle : right, Part(side, 15) },
		first ? FirstPhase : SecondPhase
	);

	// The border, drawn last so that every region is clipped by it rather than the other way round.
	//
	// **Two rings, white outside and black inside, and it is the source-rect diagnostic.** An image
	// sampled with the wrong rectangle, or clamped one texel short, loses or doubles one of these on one
	// edge — which is a thing a person sees on a still frame, where a half-texel error in the middle of
	// a photograph is not. It is also what makes `Source` being empty mean *the whole image*: a card
	// showing four complete rings is a card nobody cropped.
	for (std::int32_t ring = 0; ring < 2; ++ring)
	{
		const std::uint32_t word = ring == 0 ? White : Black;
		const std::int32_t inner = ring;
		const std::int32_t outer = side - ring;

		canvas.Fill({ inner, inner, outer, inner + 1 }, word);
		canvas.Fill({ inner, outer - 1, outer, outer }, word);
		canvas.Fill({ inner, inner, inner + 1, outer }, word);
		canvas.Fill({ outer - 1, inner, outer, outer }, word);
	}

	return Card{ side, std::move(words) };
}

std::uint32_t Card::At(std::int32_t x, std::int32_t y) const noexcept
{
	if (x < 0 || y < 0 || x >= m_Side || y >= m_Side)
	{
		return 0;
	}

	return m_Words[static_cast<std::size_t>(y) * static_cast<std::size_t>(m_Side) + static_cast<std::size_t>(x)];
}
