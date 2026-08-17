#include "Animation/Author/Motion.h"

#include <array>
#include <format>
#include <string_view>

#include "Testing/Test.h"

// The runtime half of Motion.h's contract. The compile-time half is the static_assert block at the
// foot of that header — the regimes the table's entries land in, the per-key overlay, and the speed
// modifier's guards — and is not repeated here.
//
// What is left is the property that cannot be written as an assertion about one value, because it is
// a statement about the vocabulary as a whole rather than about any entry in it.

namespace
{
struct Named
{
	Motion Which{};
	std::string_view Name;
};

constexpr std::array<Named, MotionCount> Vocabulary{ {
	{ Motion::Standard, "Standard" },
	{ Motion::Snappy, "Snappy" },
	{ Motion::Gentle, "Gentle" },
	{ Motion::Expressive, "Expressive" },
	{ Motion::Interactive, "Interactive" },
} };

// Relative on the larger of the two, so the comparison means the same thing at both ends of the
// range: a fortieth of a second between two fast motions is a large difference and between two slow
// ones is nothing.
[[nodiscard]] double RelativeDifference(double left, double right) noexcept
{
	const double larger = left > right ? left : right;

	return larger > 0.0 ? Magnitude(left - right) / larger : 0.0;
}
} // namespace

// The failure this whole file exists to prevent, turned into something a build can catch.
//
// Docs/Animation.md#the-motion-catalog opens on it: a window-open at (0.42, 0.83) beside a
// menu-appear at (0.45, 0.80), each defensible alone, and a system that feels subtly incoherent
// forever after because no reviewer can see two files at once. Once the numbers live in one table
// the mistake is no longer invisible — but it is still easy, because adding a sixth motion a little
// to the left of an existing one is exactly what someone does when a transition does not feel quite
// right and the honest answer is that it named the wrong motion.
//
// So the vocabulary is required to be *perceptibly* distinct rather than merely distinct. Two
// entries differing in the third decimal place are one entry with two names, and the second name is
// how a call site starts expressing taste that belongs in the catalog.
//
// Fifteen percent is a working figure rather than a measured threshold, chosen well below the
// spacing the current table actually has so that it fails on a new near-duplicate rather than on the
// entries it was written against.
GYRO_TEST(Motion, TheVocabularyIsPerceptiblyDistinct)
{
	constexpr double Threshold = 0.15;
	constexpr MotionTable Table{};

	for (std::size_t left = 0; left < Vocabulary.size(); ++left)
	{
		for (std::size_t right = left + 1; right < Vocabulary.size(); ++right)
		{
			const MotionParameters first = Table[Vocabulary[left].Which];
			const MotionParameters second = Table[Vocabulary[right].Which];

			const double response = RelativeDifference(first.Response, second.Response);
			const double damping = Magnitude(first.Damping - second.Damping);

			if (response < Threshold && damping < Threshold)
			{
				GYRO_FAIL(
					std::format(
						"{} ({}s, {}) and {} ({}s, {}) are the same motion with two names",
						Vocabulary[left].Name,
						first.Response,
						first.Damping,
						Vocabulary[right].Name,
						second.Response,
						second.Damping
					)
				);
			}
		}
	}
}

// Every entry has to survive the trip into coefficients, not merely sit in range. A response that
// clamped or a damping ratio that fell back would mean the number in the source is not the number
// the system runs, and the table would be documentation rather than configuration.
GYRO_TEST(Motion, EveryEntryResolvesToWhatItSays)
{
	constexpr MotionTable Table{};

	for (const Named entry : Vocabulary)
	{
		const MotionParameters authored = Table[entry.Which];
		const SpringParameters<double> resolved = Resolve<double>(entry.Which, Table);

		GYRO_CHECK_EQ(resolved, ParametersFromResponse(authored.Response, authored.Damping));
		GYRO_CHECK(resolved.Frequency > 0.0);
		GYRO_CHECK(resolved.Damping > 0.0);
	}
}

// The property that makes the modifier safe to expose: it moves everything together. A configuration
// that could reorder the vocabulary — make Gentle quicker than Snappy — would be a per-transition
// control wearing a global's name, which is the thing decision 13 refuses.
GYRO_TEST(Motion, SpeedPreservesTheOrderingOfTheVocabulary)
{
	constexpr MotionTable Table{};

	for (const double speed : { MinimumSpeed, 0.5, 1.0, 2.0, MaximumSpeed })
	{
		const MotionModifiers modifiers{ .Speed = speed };

		const double snappy = Resolve<double>(Motion::Snappy, Table, modifiers).Frequency;
		const double standard = Resolve<double>(Motion::Standard, Table, modifiers).Frequency;
		const double gentle = Resolve<double>(Motion::Gentle, Table, modifiers).Frequency;

		// A shorter response is a higher frequency, so the quick end of the vocabulary is the high
		// end here.
		GYRO_CHECK(snappy > standard);
		GYRO_CHECK(standard > gentle);
	}
}

// Damping is untouched by pacing, which is the half a naive "make it faster" setting gets wrong.
// Reducing the response and leaving the ratio alone keeps the shape of every motion and changes only
// how long it takes; scaling both would make the whole system less bouncy as it got quicker, and
// nobody asked for that.
GYRO_TEST(Motion, SpeedChangesPacingAndNotShape)
{
	constexpr MotionTable Table{};

	for (const Named entry : Vocabulary)
	{
		const SpringParameters<double> ordinary = Resolve<double>(entry.Which, Table);
		const SpringParameters<double> quick = Resolve<double>(entry.Which, Table, { .Speed = 2.0 });

		GYRO_CHECK_EQ(quick.Damping, ordinary.Damping);
		GYRO_CHECK_EQ(quick.Frequency, ordinary.Frequency * 2.0);
	}
}
