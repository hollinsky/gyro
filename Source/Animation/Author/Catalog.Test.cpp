#include "Animation/Author/Catalog.h"

#include <algorithm>
#include <array>
#include <format>
#include <string_view>

#include "Testing/Test.h"

// The runtime half of Catalog.h's contract. The compile-time half is the static_assert block at the
// foot of that header — the anchors, the one drivable entry, participation, and the Unchanged claim —
// and is not repeated here.
//
// What is left is the properties that are about *two* entries rather than one, which is where cohesion
// actually lives. An entry can be defensible on its own and wrong beside its partner: a window that
// closes by a different route than it opened, or a menu that takes longer to leave than to arrive,
// look reasonable in every diff that produced them. That is the failure Docs/Animation.md describes,
// and pairs are the only place a machine can see it.

namespace
{
struct Pairing
{
	Transition Enter;
	Transition Exit;
	std::string_view Name;
};

// The enter and exit pairs. Not every transition has one — a workspace switch is its own inverse, a
// focus change has no direction, and a matched move is a single transition between two entities — so
// this is a list rather than a sweep, and an entry joining it is a deliberate claim that the two are
// each other's reverse.
constexpr std::array<Pairing, 2> Pairs{ {
	{ Transition::WindowOpen, Transition::WindowClose, "window" },
	{ Transition::MenuAppear, Transition::MenuDismiss, "menu" },
} };

[[nodiscard]] constexpr std::string_view Name(Channel channel) noexcept
{
	switch (channel)
	{
		case Channel::Translation:
			return "translation";
		case Channel::Rotation:
			return "rotation";
		case Channel::Scale:
			return "scale";
		case Channel::Opacity:
			return "opacity";
	}

	return "?";
}
} // namespace

// A thing leaves the way it arrived.
//
// The channels have to match, because participation is what the transition *is*: a window that grew
// in and faded out without scaling does not read as the reverse of its own entrance, it reads as two
// unrelated effects that happen to bracket the same window. The anchor has to match for the sharper
// reason — an entrance growing out of the control that summoned it and an exit collapsing toward the
// middle of the screen tells the user two different things about which control they hit, and
// Docs/Animation.md#exit-pixels is explicit that the collapse toward the control is information rather
// than decoration.
//
// The motions are deliberately not compared. Those may differ, and the next test is why.
GYRO_TEST(Catalog, AnExitAnimatesWhatItsEnterAnimated)
{
	for (const Pairing& pair : Pairs)
	{
		const Bundle& enter = Definition(pair.Enter);
		const Bundle& exit = Definition(pair.Exit);

		if (enter.Anchor != exit.Anchor)
		{
			GYRO_FAIL(std::format("the {} enters and exits about different anchors", pair.Name));
		}

		for (const Channel channel : AllChannels)
		{
			const bool entering = enter.Channels[channel].How != Disposition::Absent;
			const bool exiting = exit.Channels[channel].How != Disposition::Absent;

			if (entering != exiting)
			{
				GYRO_FAIL(
					std::format(
						"the {} {} {}, and the reverse does not",
						pair.Name,
						entering ? "enters with" : "exits with",
						Name(channel)
					)
				);
			}
		}
	}
}

// An exit is not slower than the enter it reverses.
//
// Not symmetry for its own sake — the asymmetry is the design. Arriving is worth watching because the
// thing arriving is what the user is about to use; leaving is not, because they have already decided
// and moved on, and an exit that takes its time reads as the system lagging behind them rather than as
// care. What is asserted is the direction of the inequality and never the amount, because the amount
// is exactly the kind of tuning the catalog exists to let a review change without a test arguing back.
//
// Compared in authored response rather than in resolved frequency, on purpose: response is seconds and
// is the unit a person tuned, so a failure reads in the same terms as the fix.
GYRO_TEST(Catalog, LeavingIsNeverSlowerThanArriving)
{
	constexpr MotionTable Table{};

	for (const Pairing& pair : Pairs)
	{
		const Bundle& enter = Definition(pair.Enter);
		const Bundle& exit = Definition(pair.Exit);

		for (const Channel channel : AllChannels)
		{
			if (enter.Channels[channel].How != Disposition::Animate ||
			    exit.Channels[channel].How != Disposition::Animate)
			{
				continue;
			}

			const double arriving = Table[enter.Channels[channel].Using].Response;
			const double leaving = Table[exit.Channels[channel].Using].Response;

			if (leaving > arriving)
			{
				GYRO_FAIL(
					std::format(
						"the {} takes {}s to leave on {} and {}s to arrive", pair.Name, leaving, Name(channel), arriving
					)
				);
			}
		}
	}
}

// The accessibility path, over the entries rather than over the mechanism.
//
// Bundle.h proves that Reduce removes movement from any table it is given, so this cannot fail while
// the reduction is the only path from an authored entry to a reduced one. That is the point: it is
// the assertion that the reduction *stays* the only path. A per-bundle reduced table is the rejected
// alternative most likely to arrive later, one entry at a time and each time defensibly, and it would
// land here rather than in a mechanism test — which would keep passing.
GYRO_TEST(Catalog, NoEntryMovesGeometryOnTheReducedPath)
{
	for (const Transition transition : AllTransitions)
	{
		const ChannelTable reduced = Channels(transition, MotionPolicy::Reduced);

		for (const Channel channel : AllChannels)
		{
			if (channel == Channel::Opacity || reduced[channel].How != Disposition::Animate)
			{
				continue;
			}

			GYRO_FAIL(std::format("a reduced transition animates {}", Name(channel)));
		}
	}
}

// Reduced motion removes movement and not rhythm.
//
// A fade standing in for a transition should take about as long as the movement it replaced, or the
// accessibility path runs at a tempo the rest of the system does not share — which is its own kind of
// incohesion, and the harder one to notice because so few people ever see both paths.
//
// Compared against the *slowest* channel, which is what sets how long a transition is perceived to
// take. Against every channel it would be a much stronger claim and a wrong one: a window whose
// opacity is deliberately snappier than its scale would oblige its own reduced form to be the quicker
// of the two, which is the per-channel design being turned into a constraint on the thing that
// replaces it.
//
// A bound rather than an equality, because the vocabulary is coarse — five entries, so the nearest
// available motion is often a third away — and because a fade replacing a large expressive movement is
// legitimately quicker than the movement was. What is refused is the direction that actually hurts:
// an accessibility path that drags.
GYRO_TEST(Catalog, AFadeKeepsThePacingOfWhatItReplaced)
{
	constexpr MotionTable Table{};

	for (const Transition transition : AllTransitions)
	{
		const Bundle& bundle = Definition(transition);

		if (bundle.Reduced.Form != ReducedForm::Fade)
		{
			continue;
		}

		double slowest = 0.0;

		for (const Channel channel : AllChannels)
		{
			if (bundle.Channels[channel].How == Disposition::Animate)
			{
				slowest = std::max(slowest, Table[bundle.Channels[channel].Using].Response);
			}
		}

		const double fading = Table[bundle.Reduced.Using].Response;

		if (fading > slowest)
		{
			GYRO_FAIL(std::format("a fade of {}s stands in for a transition that took {}s", fading, slowest));
		}
	}
}

// Every entry resolves, and resolving is what a commit actually does with one.
//
// The catalog names motions and the vocabulary holds numbers, and nothing below this file rechecks
// that the join produces a usable spring. A motion whose row was retuned out of range would be
// silently clamped at ingest, so what is asserted is that no entry's parameters move on the way
// through — the number in the source is the number the system runs.
GYRO_TEST(Catalog, EveryAnimatedChannelResolvesToTheParametersItNames)
{
	constexpr MotionTable Table{};

	for (const Transition transition : AllTransitions)
	{
		for (const MotionPolicy policy : { MotionPolicy::Ordinary, MotionPolicy::Reduced })
		{
			const ChannelTable channels = Channels(transition, policy);

			for (const Channel channel : AllChannels)
			{
				if (channels[channel].How != Disposition::Animate)
				{
					continue;
				}

				const MotionParameters authored = Table[channels[channel].Using];

				GYRO_CHECK_EQ(Overlay(authored, { authored.Response, authored.Damping }), authored);
				GYRO_CHECK_EQ(
					Resolve<double>(channels[channel].Using, Table),
					ParametersFromResponse(authored.Response, authored.Damping)
				);
			}
		}
	}
}
