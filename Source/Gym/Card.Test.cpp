#include "Gym/Card.h"

#include <cerrno>
#include <cstdint>

#include "Core/ColorState.h"
#include "Testing/Test.h"

// The card, checked for the properties that make it a measurement rather than a picture.
//
// Colours and fractions are taste and are not asserted, for Gym/Lanes.Test.cpp's reason — they change
// the first time somebody looks at the thing on a panel. What is asserted is what a wrong answer makes
// *unreadable*: a border that does not close, a phase that is not distinguishable from its twin, two
// alpha spellings that differ somewhere other than where alpha is, a checkerboard that is not half
// white by count and therefore does not average to the value beside it.

namespace
{
constexpr std::int32_t Side = 128;

[[nodiscard]] Result<Card> Drawn(AlphaMode alpha = AlphaMode::Premultiplied, CardPhase phase = CardPhase::First)
{
	return Card::Draw(Side, alpha, phase);
}

[[nodiscard]] constexpr std::uint8_t Alpha(std::uint32_t word) noexcept
{
	return static_cast<std::uint8_t>(word >> 24);
}

[[nodiscard]] constexpr std::uint8_t Red(std::uint32_t word) noexcept
{
	return static_cast<std::uint8_t>(word >> 16);
}

[[nodiscard]] constexpr std::uint8_t Green(std::uint32_t word) noexcept
{
	return static_cast<std::uint8_t>(word >> 8);
}

[[nodiscard]] constexpr std::uint8_t Blue(std::uint32_t word) noexcept
{
	return static_cast<std::uint8_t>(word);
}
} // namespace

GYRO_TEST(GymCard, ACardTooSmallToReadIsRefused)
{
	const Result<Card> small = Card::Draw(Card::MinimumSide - 1, AlphaMode::Premultiplied, CardPhase::First);

	GYRO_REQUIRE(!small);
	GYRO_CHECK_EQ(small.error().Code(), EINVAL);

	GYRO_CHECK(Card::Draw(Card::MinimumSide, AlphaMode::Premultiplied, CardPhase::First).has_value());
}

// The importer is handed a length and a stride and reads the buffer through them, so a card whose
// three answers do not agree is a sampler walking off the end of an allocation rather than a picture
// that is wrong.
GYRO_TEST(GymCard, TheExtentTheStrideAndTheLengthAgree)
{
	const Result<Card> card = Drawn();

	GYRO_REQUIRE(card);
	GYRO_CHECK_EQ(card->Size().Width, Side);
	GYRO_CHECK_EQ(card->Size().Height, Side);
	GYRO_CHECK_EQ(card->Stride(), static_cast<std::uint32_t>(Side) * 4U);
	GYRO_CHECK_EQ(card->Bytes().size(), static_cast<std::size_t>(card->Stride()) * static_cast<std::size_t>(Side));
}

// The border is what says the whole image was sampled, so it has to close on all four edges — a ring
// that is missing its bottom row reports a crop that is not there and hides one that is.
GYRO_TEST(GymCard, BothBorderRingsCloseOnEveryEdge)
{
	const Result<Card> card = Drawn();

	GYRO_REQUIRE(card);

	for (std::int32_t along = 0; along < Side; ++along)
	{
		GYRO_REQUIRE(Red(card->At(along, 0)) == 255 && Green(card->At(along, 0)) == 255);
		GYRO_REQUIRE(Red(card->At(along, Side - 1)) == 255 && Green(card->At(along, Side - 1)) == 255);
		GYRO_REQUIRE(Red(card->At(0, along)) == 255 && Green(card->At(0, along)) == 255);
		GYRO_REQUIRE(Red(card->At(Side - 1, along)) == 255 && Green(card->At(Side - 1, along)) == 255);
	}

	// The inner ring, one texel in, everywhere except where the outer ring's corners already turned.
	for (std::int32_t along = 1; along < Side - 1; ++along)
	{
		GYRO_REQUIRE(Red(card->At(along, 1)) == 0 && Green(card->At(along, 1)) == 0);
		GYRO_REQUIRE(Blue(card->At(along, 1)) == 0 && Alpha(card->At(along, 1)) == 255);
		GYRO_REQUIRE(Red(card->At(1, along)) == 0 && Green(card->At(1, along)) == 0);
		GYRO_REQUIRE(Blue(card->At(1, along)) == 0);
	}
}

// One mark, in one corner. The point of the region is that the four ways an import can arrive turned
// are four different pictures, which holds only if the mark's three mirror images are *not* it.
GYRO_TEST(GymCard, TheOrientationMarkIsInOneCornerOnly)
{
	const Result<Card> card = Drawn();

	GYRO_REQUIRE(card);

	// The mark as a *block* rather than a texel, because the card is white in several other places —
	// the border, the stripes, the checkerboard — and a single white texel proves nothing about any of
	// them. What distinguishes the corner is that it is solidly white over a region, which is a thing
	// none of those is.
	const std::int32_t near = Side * 1 / 16;
	const std::int32_t far = Side * 3 / 16;

	const auto solidlyWhite = [&card](std::int32_t left, std::int32_t top) {
		for (std::int32_t y = top; y < top + (Side * 2 / 16); ++y)
		{
			for (std::int32_t x = left; x < left + (Side * 2 / 16); ++x)
			{
				if (Red(card->At(x, y)) != 255 || Green(card->At(x, y)) != 255 || Blue(card->At(x, y)) != 255)
				{
					return false;
				}
			}
		}

		return true;
	};

	GYRO_CHECK(solidlyWhite(near, near));
	GYRO_CHECK(!solidlyWhite(Side - far, near));
	GYRO_CHECK(!solidlyWhite(near, Side - far));
	GYRO_CHECK(!solidlyWhite(Side - far, Side - far));
}

// Full amplitude, one channel each, left to right. A card sampled with the channels rotated reads as
// three primaries in a different order, which is only a diagnostic if this order is the one it is read
// against.
GYRO_TEST(GymCard, TheChannelBarsAreThePrimariesInOrder)
{
	const Result<Card> card = Drawn();

	GYRO_REQUIRE(card);

	const std::int32_t row = Side * 4 / 16;
	const std::int32_t span = Side * 14 / 16;

	const std::uint32_t red = card->At(Side / 16 + span / 6, row);
	const std::uint32_t green = card->At(Side / 16 + span / 2, row);
	const std::uint32_t blue = card->At(Side / 16 + (span * 5) / 6, row);

	GYRO_CHECK(Red(red) == 255 && Green(red) == 0 && Blue(red) == 0);
	GYRO_CHECK(Red(green) == 0 && Green(green) == 255 && Blue(green) == 0);
	GYRO_CHECK(Red(blue) == 0 && Green(blue) == 0 && Blue(blue) == 255);
}

// Half the texels white by count is what makes the patch average to half the *light*, which is the
// only reason the solid value beside it is 188 rather than 128. Counted rather than eyeballed, because
// a board that came out 51% white would still look like a board and would quietly move the value the
// diagnostic is read against.
GYRO_TEST(GymCard, TheCheckerboardIsHalfWhiteByCountAndTheSolidBesideItIsTheLinearHalf)
{
	const Result<Card> card = Drawn();

	GYRO_REQUIRE(card);

	const std::int32_t top = Side * 7 / 16;
	const std::int32_t bottom = Side * 9 / 16;
	const std::int32_t left = Side * 8 / 16 + 1;
	const std::int32_t right = Side * 11 / 16 - 1;

	std::int32_t white = 0;
	std::int32_t counted = 0;

	for (std::int32_t y = top; y < bottom; ++y)
	{
		for (std::int32_t x = left; x < right; ++x)
		{
			const std::uint32_t word = card->At(x, y);

			GYRO_REQUIRE(Red(word) == 0 || Red(word) == 255);

			white += Red(word) == 255 ? 1 : 0;
			++counted;
		}
	}

	GYRO_CHECK_EQ(white * 2, counted);

	const std::uint32_t solid = card->At(Side * 12 / 16, Side * 8 / 16);

	GYRO_CHECK_EQ(Red(solid), 188);
	GYRO_CHECK_EQ(Green(solid), 188);
	GYRO_CHECK_EQ(Blue(solid), 188);
}

// A swap that did not land has to be a different picture from a swap that landed, and it is only that
// if the two phases differ where the eye is looking. They must also agree everywhere else, or the
// alternation stops being attributable to the swap.
GYRO_TEST(GymCard, TheTwoPhasesDifferInTheirOwnBandAndNowhereElse)
{
	const Result<Card> first = Drawn(AlphaMode::Premultiplied, CardPhase::First);
	const Result<Card> second = Drawn(AlphaMode::Premultiplied, CardPhase::Second);

	GYRO_REQUIRE(first && second);

	const std::int32_t band = Side * 14 / 16;
	std::int32_t differing = 0;

	for (std::int32_t y = 0; y < Side; ++y)
	{
		for (std::int32_t x = 0; x < Side; ++x)
		{
			if (first->At(x, y) == second->At(x, y))
			{
				continue;
			}

			GYRO_REQUIRE(y >= band);
			++differing;
		}
	}

	GYRO_CHECK(differing > 0);
}

// The same card folded and unfolded. The two must be one picture once composited, so they must differ
// *only* where alpha is not one — anywhere else is a fold applied to an opaque texel, which composites
// to the same thing and would make the pair say nothing.
GYRO_TEST(GymCard, TheAlphaSpellingsDifferOnlyWhereAlphaIsNotOpaque)
{
	const Result<Card> premultiplied = Drawn(AlphaMode::Premultiplied);
	const Result<Card> straight = Drawn(AlphaMode::Straight);

	GYRO_REQUIRE(premultiplied && straight);

	for (std::int32_t y = 0; y < Side; ++y)
	{
		for (std::int32_t x = 0; x < Side; ++x)
		{
			if (premultiplied->At(x, y) == straight->At(x, y))
			{
				continue;
			}

			GYRO_REQUIRE(Alpha(premultiplied->At(x, y)) != 255);
			GYRO_REQUIRE(Alpha(premultiplied->At(x, y)) == Alpha(straight->At(x, y)));
		}
	}

	// And the fold itself, at the one texel where it is checkable by hand: white at half alpha is half
	// white premultiplied, and unchanged straight.
	const std::int32_t inside = Side * 7 / 16;
	const std::int32_t column = Side * 2 / 16;

	GYRO_CHECK_EQ(Alpha(straight->At(column, inside)), 128);
	GYRO_CHECK_EQ(Red(straight->At(column, inside)), 255);
	GYRO_CHECK_EQ(Red(premultiplied->At(column, inside)), 128);
}
