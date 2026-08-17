#include "Animation/Author/Bundle.h"

#include <array>
#include <format>
#include <string_view>

#include "Testing/Test.h"

// The runtime half of Bundle.h's contract. The compile-time half is the static_assert block at the
// foot of that header, and it is unusually large there because the whole file is arithmetic on
// enumerations — the central property is proved for every table any bundle could hold rather than
// sampled, so it is not repeated here.
//
// What is left is the sweep the theorem does not cover. That proof is about the Fade form, which is
// the one with content; these are about the *vocabulary* being closed under the same invariants, so
// that a fourth form added later has to satisfy them rather than merely compile.

namespace
{
constexpr std::array<ReducedForm, 3> AllForms{ ReducedForm::Fade, ReducedForm::Cut, ReducedForm::Unchanged };

[[nodiscard]] constexpr std::string_view Name(ReducedForm form) noexcept
{
	switch (form)
	{
		case ReducedForm::Fade:
			return "Fade";
		case ReducedForm::Cut:
			return "Cut";
		case ReducedForm::Unchanged:
			return "Unchanged";
	}

	return "?";
}

[[nodiscard]] constexpr std::string_view Name(Channel channel) noexcept
{
	switch (channel)
	{
		case Channel::Translation:
			return "Translation";
		case Channel::Rotation:
			return "Rotation";
		case Channel::Scale:
			return "Scale";
		case Channel::Opacity:
			return "Opacity";
	}

	return "?";
}

// The shapes a real bundle takes: geometry alone, geometry with a fade already on it, opacity alone,
// and everything at once. Between them they cover every participation pattern the invariants below
// care about.
constexpr std::array<ChannelTable, 4> Shapes{ {
	{ .Translation = Animate(Motion::Standard) },
	{ .Translation = Animate(Motion::Snappy),
	  .Scale = Animate(Motion::Expressive),
	  .Opacity = Animate(Motion::Gentle) },
	{ .Opacity = Animate(Motion::Gentle) },
	{ .Translation = Animate(Motion::Standard),
	  .Rotation = Animate(Motion::Expressive),
	  .Scale = Animate(Motion::Standard),
	  .Opacity = Animate(Motion::Gentle) },
} };
} // namespace

// The accessibility path's one hard rule, over the whole form vocabulary rather than over one form.
//
// Unchanged is exempt by construction and by intent: it is the declaration that a transition was
// already pure opacity, so whatever it animates is what it animated before. The exemption is only
// safe because the form has to be *named* — a bundle cannot arrive at it by omission, which is the
// difference between this and a reduced path that silently inherits.
GYRO_TEST(Bundle, NoReducedFormMovesGeometry)
{
	for (const ReducedForm form : AllForms)
	{
		if (form == ReducedForm::Unchanged)
		{
			continue;
		}

		for (const ChannelTable& authored : Shapes)
		{
			const ChannelTable reduced = Reduce(authored, { form, Motion::Standard });

			for (const Channel channel : AllChannels)
			{
				if (channel == Channel::Opacity || reduced[channel].How != Disposition::Animate)
				{
					continue;
				}

				GYRO_FAIL(std::format("{} animates {} — reduced motion replaces movement", Name(form), Name(channel)));
			}
		}
	}
}

// Participation is inherited by every form, which is what stops a reduced transition cancelling
// motion that belongs to somebody else. A channel absent from the authored table is absent from the
// reduced one — with opacity under Fade the single deliberate exception, since substituting a fade
// for movement is the entire policy.
GYRO_TEST(Bundle, NoReducedFormSeizesAChannelItWasNotGiven)
{
	for (const ReducedForm form : AllForms)
	{
		for (const ChannelTable& authored : Shapes)
		{
			const ChannelTable reduced = Reduce(authored, { form, Motion::Standard });

			for (const Channel channel : AllChannels)
			{
				const bool substituted = form == ReducedForm::Fade && channel == Channel::Opacity;

				if (substituted || authored[channel].How != Disposition::Absent)
				{
					continue;
				}

				GYRO_CHECK_EQ(reduced[channel].How, Disposition::Absent);
			}
		}
	}
}

// Reducing a reduced table changes nothing, for every form.
//
// Not a curiosity. The policy can change while transitions are in flight — a preference toggled, or
// a configuration reload — and a reduction that drifted on reapplication would make the accessibility
// path depend on how many times it had been asked for, which is the sort of thing that presents as
// "it goes wrong if you toggle it twice" and is never reproduced.
GYRO_TEST(Bundle, ReducingIsStableUnderRepetition)
{
	for (const ReducedForm form : AllForms)
	{
		for (const ChannelTable& authored : Shapes)
		{
			const ReducedMotion reduced{ form, Motion::Gentle };
			const ChannelTable once = Reduce(authored, reduced);

			GYRO_CHECK_EQ(Reduce(once, reduced), once);
		}
	}
}

// The default path pays nothing for the existence of the other one. Worth asserting rather than
// assuming, because the cheap way to implement a policy is a branch inside every read, and this is
// the shape that keeps it to one substitution at resolution.
GYRO_TEST(Bundle, TheOrdinaryPolicyIsTheAuthoredTableVerbatim)
{
	for (const ChannelTable& authored : Shapes)
	{
		for (const ReducedForm form : AllForms)
		{
			const Bundle bundle{ .Channels = authored, .Reduced = { form, Motion::Gentle } };

			GYRO_CHECK_EQ(Channels(bundle, MotionPolicy::Ordinary), authored);
		}
	}
}
