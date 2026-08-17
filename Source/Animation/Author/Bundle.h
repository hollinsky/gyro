#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

#include "Animation/Author/Drive.h"
#include "Animation/Author/Motion.h"

// A transition, as the catalog defines it.
//
// The catalog's unit is not a spring. Per-channel springs are what make motion feel designed rather
// than mechanical — position wants to be snappier than scale, and opacity generally should not
// bounce when geometry does — and they are also where incohesion multiplies fastest if call sites
// tune channels one at a time. So the unit is a *bundle*: the channels of one transition, designed
// together, named as a whole, and never reached into. See Docs/Animation.md#bundles-are-the-real-unit
// and Docs/Decisions.md decision 13.
//
// **Channels are labels here, not types.** A spring's parameters are scalar whatever its channel's
// value type is — Animation/Solve/Spring.h says so as the point of its concept — so nothing in a
// bundle is ever of the channel's type. The value type appears for the first time at the differ's
// AnimateTo call, where the member being written is known statically and no lookup is involved. So
// the geometric channels below are named without Geometry being needed to name them, and the module
// edge that does exist comes from one member and one member only: Animation/Author/Drive.h's travel,
// which is a displacement in a space that has to be the output-independent one.
//
// **What is deliberately not here.** Staggering — "each subsequent item starts twenty milliseconds
// later" — is absent because the answer is no. Every transition written today is unstaggered, that is
// correct for all of them, and a field defaulting to *no stagger* would change nothing about any of
// them. What the design does need is already settled and recorded rather than built: the delay and
// its cap are global modifiers beside Motion.h's speed, because an amount is tuning and must move the
// system together, while the *order* items go in — list order, or outward from a focal point — is per
// transition, because it is motion design and reads completely differently. Mechanically it costs
// nothing whenever it lands: a staggered entity is one whose spring carries an origin of t0 +
// i*delay, and Spring.h's SecondsSince already clamps below the origin, so a not-yet-started spring
// evaluates at its initial state and settles late by exactly the offset. One trap comes with it, worth writing down now
// because it is invisible later: a staggered *retarget* must sample the old spring at its own
// staggered origin rather than at t0, or the entity freezes where it was for the length of its delay
// and then starts, which is a visible stall on precisely the mid-flight interruption that uniform
// retargeting exists to keep smooth.
//
// The anchor, by contrast, is here from the first entry and had to be, because it fails that same
// test: a default would not be harmless. Docs/Animation.md#transforms makes it load-bearing — it is
// what makes a window grow out of the corner it was summoned from instead of out of its own middle,
// and most of what makes a transition read as intentional — so entries authored before the field
// existed would each need revisiting to decide something only their author knows, which is exactly
// the retrofit hazard reduced motion is first-class to avoid.

// The independently sprung quantities. One channel per spring rather than per component, which
// Docs/Decisions.md decision 17 and Animation/Solve/Spring.h both argue on settling grounds: three
// per-component thresholds is an axis-dependent criterion, so the same rotation would finish at
// different moments depending on where its axis pointed.
//
// Four, and the omissions are deliberate. Corner radius and blur radius wait on the material
// vocabulary, which Docs/Open.md leaves open; the anchor and the projection are declared by a
// transition rather than animated by one; and progress is not a channel but the parameter an
// interactive bundle's channels are functions of, so putting it here would make "every channel has a
// disposition" false for exactly one member.
enum class Channel : std::uint8_t
{
	Translation,
	Rotation,
	Scale,
	Opacity,
};

inline constexpr std::size_t ChannelCount = 4;

static_assert(static_cast<std::size_t>(Channel::Opacity) + 1 == ChannelCount);

inline constexpr std::array<Channel, ChannelCount> AllChannels{
	Channel::Translation,
	Channel::Rotation,
	Channel::Scale,
	Channel::Opacity,
};

// The list is the enumeration in order, so a pass over it is a pass over the enum. The size is fixed
// by ChannelCount already; what is left to go wrong is a duplicate standing in for the entry somebody
// meant to add, which reads correctly and silently drops a channel from every sweep below.
static_assert([] {
	for (std::size_t index = 0; index < AllChannels.size(); ++index)
	{
		if (static_cast<std::size_t>(AllChannels[index]) != index)
		{
			return false;
		}
	}

	return true;
}());

// What a transition does to one channel.
//
// Three cases and not two, and the middle one is the whole of why reduced motion is a dimension
// rather than a scaling. A slide-in that becomes a fade-in has a translation channel that is neither
// animated nor untouched: the window has to *be* at its model position immediately, which is a
// different statement from "this transition has no opinion about position". Collapsing the two makes
// the reduced path either leave windows at stale positions or seize channels it was never part of.
enum class Disposition : std::uint8_t
{
	Absent,    // not part of this transition; whatever the channel was doing, it continues
	Immediate, // lands on the model value now, with no movement
	Animate,   // sprung, under the motion beside it
};

// **A field that is not read is left at its default, and that is an invariant rather than tidiness.**
// Equality is defaulted, so it compares Using whether or not the disposition beside it reads one —
// which means an Immediate carrying Snappy and an Immediate carrying Standard are two values that
// behave identically and compare unequal. Everything that would notice is a property somebody will
// want later: that reducing twice changes nothing, that the ordinary policy is the authored table
// verbatim, that two entries agree. The factories below are what keep the invariant true, and they
// are the only construction in the catalog.
struct ChannelMotion
{
	Disposition How = Disposition::Absent;
	Motion Using = Motion::Standard; // read only when How is Animate, and default whenever it is not

	friend constexpr bool operator==(ChannelMotion, ChannelMotion) noexcept = default;
};

[[nodiscard]] constexpr ChannelMotion Animate(Motion motion) noexcept
{
	return { Disposition::Animate, motion };
}

[[nodiscard]] constexpr ChannelMotion Immediate() noexcept
{
	return { Disposition::Immediate, {} };
}

// The channels of one transition.
//
// A member per channel rather than an array indexed by the enum, because this is a structure people
// author by hand and a designated initializer is what makes it readable — and because an omitted
// member defaults to Absent, which is exactly the polarity wanted when the enum grows: a channel
// added later is part of no existing transition until somebody says it is. The subscript below is
// for the passes that must be generic over the vocabulary, which is the reduction and its tests.
struct ChannelTable
{
	ChannelMotion Translation{};
	ChannelMotion Rotation{};
	ChannelMotion Scale{};
	ChannelMotion Opacity{};

	friend constexpr bool operator==(ChannelTable, ChannelTable) noexcept = default;

	[[nodiscard]] constexpr const ChannelMotion& operator[](Channel channel) const noexcept
	{
		switch (channel)
		{
			case Channel::Rotation:
				return Rotation;
			case Channel::Scale:
				return Scale;
			case Channel::Opacity:
				return Opacity;
			case Channel::Translation:
				break;
		}

		return Translation;
	}

	// The object is non-const wherever this overload is reachable, so the cast never removes
	// constness an object actually had — which is what keeps it valid in a constant expression as
	// well as defined at runtime. Written this way rather than as a second switch so the two cannot
	// answer differently after a channel is added.
	[[nodiscard]] constexpr ChannelMotion& operator[](Channel channel) noexcept
	{
		return const_cast<ChannelMotion&>(std::as_const(*this)[channel]);
	}
};

// What a transition becomes when movement is not wanted.
//
// Reduced motion is routinely implemented wrong, and the wrong implementation is faster or less
// bouncy springs. It means **replacing movement with cross-fades** and dropping parallax and scale
// entirely: a window that slides in fades in instead. That is a different transition rather than a
// retuned one, which is why it is a dimension of every bundle from the start — retrofitting it means
// revisiting every entry, and each entry's reduced form is a decision only its author can make. See
// Docs/Animation.md#reduced-motion-is-a-policy-not-a-parameter.
//
// **Three forms, not five, and the collapse is the finding rather than an economy.** Fade-in,
// fade-out, and cross-fade differ only in what opacity is heading *for*, and that is model state the
// differ already holds — an enter targets one, an exit targets zero — so they are one form and the
// direction is not the catalog's to know. What remains is the fade; the transition too structural to
// fade at all, since there is no cross-fade of a size and a resize simply arrives; and the
// transition that was already pure opacity and is therefore its own reduced form.
//
// **There is no Custom here, and the asymmetry against Motion.h's escape hatch is deliberate.** A
// per-bundle reduced table is not an escape hatch — it is the rejected alternative arriving one entry
// at a time, and it would put the accessibility path back where two independently authored fades can
// drift apart on the path that gets the least review. A transition needing something these three
// cannot say wants a fourth *named* form, because a bespoke reduced shape that recurs is a form and
// one that does not is almost certainly a mistake. Matched geometry is the case expected to test
// this: a reduced matched-move is either a Cut or a genuine cross-fade between two endpoints, and
// the second is not expressible as a disposition table over one node's channels.
enum class ReducedForm : std::uint8_t
{
	Fade,
	Cut,
	Unchanged,
};

struct ReducedMotion
{
	ReducedForm Form = ReducedForm::Fade;

	// The fade's own pacing, and per bundle rather than fixed by the form. Reduced motion removes
	// *movement*, not rhythm: a fade standing in for a workspace switch should take about as long as
	// the movement it replaced, or the accessibility path runs at a tempo the rest of the system does
	// not share.
	//
	// Read only when Form is Fade, and left at its default whenever it is not — ChannelMotion's
	// invariant, for ChannelMotion's reason. A Cut that names a motion is an entry saying something
	// nothing reads, which a reader has to work out is noise rather than intent; Catalog.h asserts
	// over the real entries that none of them do.
	Motion Using = Motion::Standard;

	friend constexpr bool operator==(ReducedMotion, ReducedMotion) noexcept = default;
};

// Whether a transition is being watched or is being made accessible.
//
// Not a modifier in Motion.h, deliberately. Everything in that struct moves the whole system by a
// scalar; this substitutes one table for another, which is a different kind of operation and would
// read as a slider if it sat beside one.
enum class MotionPolicy : std::uint8_t
{
	Ordinary,
	Reduced,
};

namespace Detail
{
// Participating channels stop moving; absent ones stay absent. Pulled out so that the rule is stated
// once and the two forms that use it cannot disagree about it.
[[nodiscard]] constexpr ChannelMotion Snapped(ChannelMotion authored) noexcept
{
	return authored.How == Disposition::Absent ? ChannelMotion{} : Immediate();
}
} // namespace Detail

// The reduced form of an authored table.
//
// **Participation is inherited; disposition is not.** Which channels a transition touches is a
// property of the transition — a reduced fade must not seize a rotation this transition was never
// part of, since forcing Immediate on an unrelated channel would cancel whatever that channel was
// doing for somebody else. What happens to the channels it *does* touch is the form's entire content.
//
// Opacity is the one deliberate exception, and it is the exception the policy exists for: a window
// that slides in with no opacity channel at all still has to fade in when its movement is removed, so
// Fade adds the channel rather than merely retuning it.
[[nodiscard]] constexpr ChannelTable Reduce(const ChannelTable& authored, ReducedMotion reduced) noexcept
{
	if (reduced.Form == ReducedForm::Unchanged)
	{
		return authored;
	}

	ChannelTable table{};

	for (const Channel channel : AllChannels)
	{
		table[channel] = Detail::Snapped(authored[channel]);
	}

	if (reduced.Form == ReducedForm::Fade)
	{
		table.Opacity = Animate(reduced.Using);
	}

	return table;
}

// Where the fixed point of a transition's scale and rotation comes from.
//
// **A policy rather than a coordinate, because the coordinate is not knowable here.** The anchor is
// in the node's own space and depends on the node's extent, which the catalog does not have — and for
// the case that matters most it depends on something that does not exist until the gesture happens.
// A menu grows from where it was opened, and where it was opened is a property of the commit. So this
// names the *source* of the anchor and the scene resolves it, which is the same division of labour
// that keeps the differ's entity knowledge out of this module.
//
// Two, because two is what the design has arguments for. A named corner is the obvious third and it
// is not here on the same YAGNI grounds everything else was deferred on: adding it later is one
// enumerator and one line in whichever entry wants it, with nothing else disturbed.
//
// The anchor is not substituted by reduced motion, and does not need to be. With scale and rotation
// snapped there is nothing left for a fixed point to be fixed against, so the value is simply unread
// — which is why this sits beside the channel tables rather than inside them, along with the drive
// mapping and for the same reason decision 13 gives: reduced motion substitutes channels and must
// touch neither the parameterization nor the geometry a transition declares.
enum class AnchorPolicy : std::uint8_t
{
	Centre,      // the node's own middle; right whenever nothing summoned the transition
	SummonPoint, // where the commit says this came from — the click, the icon, the corner
};

// One entry of the catalog.
//
// **Five members and four things said, and the difference is what keeps the fifth from being a
// precedent.** Docs/Animation.md#bundles-are-the-real-unit permits a bundle four statements and
// refuses it a fifth, and all four are motion design: what the channels do, what they become when
// movement is not wanted, where the geometry is fixed, and how a gesture drives it. GroupOpacity is
// not one of them. It declares what the transition *costs to draw* — decision 60's flattening — and
// it is keyed by transition only because the transition is what knows whether a subtree is underneath
// it. The test for anything wanting to join it is that question rather than the count: a member that
// changes how the motion reads is a fifth thing said and is refused; a member that changes what the
// renderer must allocate is this one, and wants decision 60's argument rather than decision 13's. See
// Docs/Decisions.md decision 70.
//
// It is also policy-invariant, which is a limit worth knowing before something needs otherwise. One
// flag serves both the ordinary and the reduced table, so a transition needing a group on one path
// and not the other cannot say so. Catalog.h's WorkspaceSwitch is exactly that case and is fine only
// because its ordinary path animates no opacity at all, which makes the flag unread rather than
// wrong. The first entry that fades on both paths and needs a group on one is what turns this into a
// pair — and it is a pair rather than a per-policy table, since the reduced substitution's domain is
// the channels and must stay there.
//
// Group opacity is a flag rather than a derived property because Docs/Decisions.md decision 60 makes
// it a real cost: a bundle that fades a subtree has to fade it as one object, which means flattening,
// or the windows inside show through each other at every value between the endpoints. Which entries
// declare one is open in Docs/Open.md and is not answered here — "every fade" is the wrong answer,
// since a single window fading out needs no group and paying for an offscreen there would put a
// render target on the most frequent transition in the system. What is not open is that the field
// belongs here from the first entry: every reduced variant that covers a subtree sets it, so the
// accessibility path exercises flattening constantly and is not a cheaper path.
struct Bundle
{
	ChannelTable Channels{};
	ReducedMotion Reduced{};

	// The two things a transition declares rather than animates. Both sit outside the channel tables
	// because both survive the reduced substitution untouched — a gesture that stops tracking is not
	// reduced, it is broken, which is decision 13's statement that the two dimensions interact rather
	// than compose.
	AnchorPolicy Anchor = AnchorPolicy::Centre;
	DriveMapping Drive{};

	bool GroupOpacity = false;

	// Whether this transition is one the user makes rather than one they watch. Derived from the
	// mapping rather than flagged beside it, for Animatable's reason: a bundle with no travel cannot
	// be driven anywhere, so a second field would be a restatement of the first that could disagree
	// with it.
	[[nodiscard]] constexpr bool IsInteractive() const noexcept { return Drive.IsDrivable(); }

	friend constexpr bool operator==(Bundle, Bundle) noexcept = default;
};

// The table a commit resolves against, for one transition under one policy.
[[nodiscard]] constexpr ChannelTable Channels(const Bundle& bundle, MotionPolicy policy) noexcept
{
	return policy == MotionPolicy::Reduced ? Reduce(bundle.Channels, bundle.Reduced) : bundle.Channels;
}

// The contract everything downstream assumes, and here it is unusually strong: the whole of this
// file is arithmetic on enumerations, so what would ordinarily be a test over every catalog entry is
// a theorem about one function.

static_assert(ChannelTable{}[Channel::Translation].How == Disposition::Absent, "An omitted channel is not touched");

// The factories leave the unread field at its default, which is what makes every equality in this
// file and in the tests beside it mean what it appears to mean rather than nearly that.
static_assert(Immediate().Using == ChannelMotion{}.Using);
static_assert(Detail::Snapped(Animate(Motion::Expressive)) == Immediate(), "A snapped channel keeps no motion");
static_assert(Detail::Snapped(ChannelMotion{}) == ChannelMotion{});

// **The property the whole design exists for, proved rather than sampled.** A table in which every
// channel participates is the maximum case, because Reduce only ever maps a participating channel to
// Immediate and never the other way — so establishing it here establishes it for every table any
// bundle could hold, including channels added after this was written.
static_assert(
	[] {
		constexpr ChannelTable Everything{
			.Translation = Animate(Motion::Snappy),
			.Rotation = Animate(Motion::Snappy),
			.Scale = Animate(Motion::Snappy),
			.Opacity = Animate(Motion::Snappy),
		};

		const ChannelTable reduced = Reduce(Everything, { ReducedForm::Fade, Motion::Gentle });

		for (const Channel channel : AllChannels)
		{
			const bool geometric = channel != Channel::Opacity;

			if (geometric && reduced[channel].How != Disposition::Immediate)
			{
				return false;
			}
		}

		return reduced.Opacity == Animate(Motion::Gentle);
	}(),
	"Whatever a bundle authors, reducing it animates opacity and moves nothing"
);

// A window that slides in with no opacity channel still fades in, which is the substitution rather
// than a subtraction and is the half a derived reduced form gets wrong.
static_assert(
	Reduce({ .Translation = Animate(Motion::Standard) }, { ReducedForm::Fade, Motion::Standard }).Opacity ==
	Animate(Motion::Standard)
);
static_assert(
	Reduce({ .Translation = Animate(Motion::Standard) }, { ReducedForm::Fade, Motion::Standard }).Translation ==
	Immediate()
);

// Participation is inherited, so a channel this transition never touched is not seized by its
// reduced form — including under Fade, where every other channel is being snapped.
static_assert(
	Reduce({ .Translation = Animate(Motion::Standard) }, { ReducedForm::Fade, Motion::Standard }).Rotation.How ==
		Disposition::Absent,
	"A reduced fade does not cancel a rotation it was never part of"
);

// Cut is a fade with the fade removed, and Unchanged is the identity — the entry that was
// already pure opacity and says so rather than duplicating itself.
static_assert(
	Reduce(
		{ .Translation = Animate(Motion::Standard), .Opacity = Animate(Motion::Gentle) },
		{ ReducedForm::Cut, Motion::Standard }
	) == ChannelTable{ .Translation = Immediate(), .Opacity = Immediate() }
);
static_assert([] {
	constexpr ChannelTable Authored{ .Opacity = Animate(Motion::Gentle) };

	return Reduce(Authored, { ReducedForm::Unchanged, Motion::Standard }) == Authored;
}());

// Resolution is a substitution and not a retune: the ordinary policy is the authored table verbatim,
// byte for byte, so nothing about having an accessibility path costs the default path anything.
static_assert([] {
	constexpr Bundle Slide{
		.Channels = { .Translation = Animate(Motion::Standard), .Scale = Animate(Motion::Expressive) },
		.Reduced = { ReducedForm::Fade, Motion::Standard },
	};

	return Channels(Slide, MotionPolicy::Ordinary) == Slide.Channels &&
	       Channels(Slide, MotionPolicy::Reduced).Scale == Immediate();
}());

// A bundle is not interactive until it says how far a gesture travels, so an ordinary entry needs no
// opinion about gestures at all.
static_assert(!Bundle{}.IsInteractive());
static_assert(Bundle{ .Drive = { .Travel = { 1200.0, 0.0 } } }.IsInteractive());

// The centre is the anchor a transition gets by saying nothing, which is right wherever nothing
// summoned it — and wrong in a way its own entry has to correct rather than inherit.
static_assert(Bundle{}.Anchor == AnchorPolicy::Centre);

// **Reduced motion keeps the driven half, and here that is structural rather than intended.**
// Replacing movement with a fade is right for a transition being watched and wrong for one being
// made: a gesture that does not track is not reduced, it is broken. The substitution's whole domain
// is the channel table, so neither the mapping nor the anchor is reachable from it — which is
// decision 13's statement that the two dimensions interact rather than compose, made true by where
// the members sit rather than by anything the reduction remembers to avoid.
static_assert([] {
	constexpr Bundle Swipe{
		.Channels = { .Translation = Animate(Motion::Interactive), .Opacity = Animate(Motion::Gentle) },
		.Reduced = { ReducedForm::Fade, Motion::Gentle },
		.Anchor = AnchorPolicy::SummonPoint,
		.Drive = { .Travel = { 1200.0, 0.0 } },
	};

	return Swipe.IsInteractive() && Swipe.Anchor == AnchorPolicy::SummonPoint &&
	       Channels(Swipe, MotionPolicy::Reduced).Translation == Immediate();
}());
