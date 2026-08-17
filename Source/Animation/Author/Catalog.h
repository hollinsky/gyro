#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "Animation/Author/Bundle.h"

// The catalog itself: every transition gyro knows how to make, and the bundle each one is.
//
// Everything below this line is data. The machinery it is made of is Animation/Author/Motion.h's
// vocabulary, Animation/Author/Bundle.h's channel tables and reduced forms, and
// Animation/Author/Drive.h's gesture mapping; this file only says which combinations exist and what
// they are called. See Docs/Animation.md#the-motion-catalog and Docs/Decisions.md decision 13.
//
// **There is no table parameter here, and that absence is the enforcement.** Motion.h's Resolve takes
// a MotionTable because configuration overlays the vocabulary; Definition below takes a Transition and
// nothing else, because decision 13 requires that configuration expose the vocabulary and *never* the
// individual transitions. Retuning Motion::Standard has to move everything built on Standard together,
// and it does, because a configuration file's only reachable surface is the five rows in Motion.h. A
// parser that wanted to reach WindowOpen would have to be handed somewhere to put it, and there is
// nowhere. The granularity is a property of the signatures rather than of the parser's discipline.
//
// **The entries are provisional and are labelled so deliberately.** They exist to exercise the
// machinery end to end — every reduced form, both anchor policies, the drivable case, and the enter
// and exit pairing — rather than to be the final design. Which channels a window-open should animate
// and how fast is a question for a review with a screen in front of it, and every entry below will be
// revisited by one. What is *not* provisional is the shape: a transition names channels, names a
// reduced form, names an anchor, and says how it is driven, and no entry may say anything else.
//
// **What a catalog test can catch that a Bundle.h theorem cannot.** Bundle.h proves that reducing an
// authored table removes movement, for every table any bundle could hold. It proves nothing about
// ReducedForm::Unchanged, which is exempt by construction — it is the *declaration* that a transition
// was already pure opacity, and a declaration can be false. So the entries are where that claim is
// checked, along with the cohesion properties that are about two entries rather than one: an exit
// animating the same channels as its enter, and not being slower than it.

enum class Transition : std::uint8_t
{
	WindowOpen,
	WindowClose,
	MenuAppear,
	MenuDismiss,
	WorkspaceSwitch,
	FocusChange,
	MatchedMove,
};

inline constexpr std::size_t TransitionCount = 7;

// Unlike Motion.h's five, this number carries no ceiling. The motion vocabulary is capped because
// growth past seven is cohesion leaking; the transition list is capped by nothing but how many things
// the system does, and a shell that does more has more. The assertion is here for the reason the
// switch below needs it — a new enumerator has to be given an entry, not inherit one.
static_assert(static_cast<std::size_t>(Transition::MatchedMove) + 1 == TransitionCount);

inline constexpr std::array<Transition, TransitionCount> AllTransitions{
	Transition::WindowOpen,      Transition::WindowClose, Transition::MenuAppear,  Transition::MenuDismiss,
	Transition::WorkspaceSwitch, Transition::FocusChange, Transition::MatchedMove,
};

// The list is the enumeration in order, so every sweep below is a sweep over the catalog. The size is
// fixed by TransitionCount; what is left to go wrong is a duplicate standing in for the entry
// somebody meant to add, which reads correctly and would quietly exempt a transition from all of it.
static_assert([] {
	for (std::size_t index = 0; index < AllTransitions.size(); ++index)
	{
		if (static_cast<std::size_t>(AllTransitions[index]) != index)
		{
			return false;
		}
	}

	return true;
}());

namespace Detail
{
// A window arriving. Scale and opacity, and no translation — the window is at its layout position from
// the first frame and grows into it, because a window that also slides has to slide from somewhere and
// nothing in the model says where. Anchored at the summon point so it grows out of whatever launched
// it, which is the difference between a window appearing and a window being opened by the thing the
// user clicked.
//
// Opacity is snappier than scale on purpose: a window that is still translucent while it is nearly
// full size reads as unfinished, so the fade lands first and the geometry catches up.
inline constexpr Bundle WindowOpen{
	.Channels = { .Scale = Animate(Motion::Standard), .Opacity = Animate(Motion::Snappy) },
	.Reduced = { .Form = ReducedForm::Fade, .Using = Motion::Standard },
	.Anchor = AnchorPolicy::SummonPoint,
};

// A window leaving, and faster than it arrived. An exit is not the enter reversed: the user has
// decided and moved on, and a leisurely exit reads as the system lagging behind them rather than as
// care. The pairing test below enforces the direction of that inequality rather than the amount.
//
// No opacity group, and this is the one entry where that is a mechanism claim rather than a judgement.
// Docs/Animation.md#exit-pixels snapshots the window at full resolution into a single
// compositor-owned texture, and a single texture is already flat — so the fade is a group fade without
// an offscreen, for free, on the most frequent transition in the system. If an exit ever runs off the
// live subtree instead, this flag has to be revisited with it.
inline constexpr Bundle WindowClose{
	.Channels = { .Scale = Animate(Motion::Snappy), .Opacity = Animate(Motion::Snappy) },
	.Reduced = { .Form = ReducedForm::Fade, .Using = Motion::Snappy },
	.Anchor = AnchorPolicy::SummonPoint,
};

// A menu opening. The same shape as a window, faster throughout, and the anchor is load-bearing rather
// than nice: a menu grows out of the control that opened it, and that is what tells the user which
// control they hit. Menus are the most repeated transition in the system — dozens a minute inside one
// application — so Snappy on both channels is the whole design.
inline constexpr Bundle MenuAppear{
	.Channels = { .Scale = Animate(Motion::Snappy), .Opacity = Animate(Motion::Snappy) },
	.Reduced = { .Form = ReducedForm::Fade, .Using = Motion::Snappy },
	.Anchor = AnchorPolicy::SummonPoint,
};

// A menu closing, collapsing back toward where it came from.
//
// The one entry that declares an opacity group, and the asymmetry against MenuAppear is real rather
// than an oversight: a menu can only ever have a submenu open at dismissal, never at appearance, and a
// submenu overlapping its parent while both fade is precisely the per-node alpha that decision 60
// rejects — the parent shows through the child at every value between the endpoints. So the group is
// declared where a subtree can exist and not where it cannot.
inline constexpr Bundle MenuDismiss{
	.Channels = { .Scale = Animate(Motion::Snappy), .Opacity = Animate(Motion::Snappy) },
	.Reduced = { .Form = ReducedForm::Fade, .Using = Motion::Snappy },
	.Anchor = AnchorPolicy::SummonPoint,
	.GroupOpacity = true,
};

// Moving between workspaces, and the only drivable entry.
//
// **The travel is ergonomic and not geometric, which is why a constant can be correct here.** A
// workspace is an output wide, so the tempting travel is the output's width — and it is wrong, because
// the gesture happens on a touchpad whose extent belongs to the hand rather than to the display.
// Deriving travel from the output would make the same swipe mean a different fraction of the
// transition on every monitor, and would make an external display change how far a finger has to move
// on a laptop's own touchpad. What the number wants to be is a comfortable long swipe, which is a
// property of people.
//
// **Where the release spring's motion lives, since it is visibly not here.** An interactive transition
// is a bundle whose channels are functions of one scalar p, and on release p springs to an end — under
// Motion::Interactive, always, by what that vocabulary entry is for. It is a rule rather than a field
// on the drive mapping because a per-bundle release motion is the incohesion the vocabulary exists to
// prevent: two swipes that let go differently is exactly the failure decision 13 describes. If it ever
// has to vary, it is one field, and the argument for adding it has to be made then.
//
// The channel table is still meaningful for a driven transition, and this is where that is easiest to
// see: a workspace switched from the keyboard has no gesture and no p, so it springs Translation under
// Interactive in the ordinary way. Same bundle, two regimes, and the difference is whether a finger is
// down.
//
// The group is for the reduced path rather than the ordinary one. A workspace is a subtree of many
// windows, so cross-fading one needs flattening while sliding one does not — which is
// Docs/Animation.md's point that the accessibility path exercises decision 60 constantly and is not a
// cheaper path.
//
// The fade is Interactive too, and this entry is where that rule earns itself. A gesture keeps its
// gesture pacing whether it slides or fades: the reduced form of a transition the user is *making* is
// still being made, so a fade at some watching tempo would be slower than the movement it stands in
// for and would lag a finger that is still down. Standard was written here first and the pacing test
// below caught it.
inline constexpr Bundle WorkspaceSwitch{
	.Channels = { .Translation = Animate(Motion::Interactive) },
	.Reduced = { .Form = ReducedForm::Fade, .Using = Motion::Interactive },
	.Drive = { .Travel = { 1200.0, 0.0 }, .RubberBand = 0.1 },
	.GroupOpacity = true,
};

// Focus moving between windows: the dimming of what lost it and the lift of what gained it.
//
// The entry that is its own reduced form. It animates opacity and nothing else, so there is no
// movement to replace and Unchanged is a true statement rather than an opt-out — which the catalog
// test checks, because Bundle.h's theorem cannot: Unchanged is exempt from the reduction by
// construction, so nothing but an assertion over the real entries stops a moving transition from
// claiming it.
//
// Gentle because focus follows the user's attention rather than leading it. A focus change that
// announced itself would be competing with whatever the user just did to cause it.
inline constexpr Bundle FocusChange{
	.Channels = { .Opacity = Animate(Motion::Gentle) },
	.Reduced = { .Form = ReducedForm::Unchanged },
};

// One entity becoming another continuously: a window growing out of its overview thumbnail, an
// alt-tab tile staying put when the mode changes. See Docs/Animation.md#matched-geometry.
//
// Translation snappier than scale, which is the per-channel design Docs/Animation.md#transforms argues
// for and is most visible here. The rectangle does distort en route — it reaches its destination
// position while still resizing — and that is the intent rather than a defect: matched geometry that
// moved and resized in lockstep reads as a rigid slide, which is the cross-fade this mechanism exists
// to replace wearing a different costume.
//
// **The entry that will test whether three reduced forms are enough.** Cut, because there is no
// cross-fade of a size — a reduced resize simply arrives — but the honest alternative is a genuine
// cross-fade between the two endpoints, and Bundle.h records why that is not expressible as a
// disposition table over one node's channels. If a reduced matched-move needs to be more than a cut,
// what it needs is a fourth named form, and this entry is where that will be discovered.
inline constexpr Bundle MatchedMove{
	.Channels = { .Translation = Animate(Motion::Snappy), .Scale = Animate(Motion::Standard) },
	.Reduced = { .Form = ReducedForm::Cut },
};
} // namespace Detail

// What a transition is.
//
// A switch with no default label, which is what makes a new enumerator a build failure rather than a
// silent alias: -Wswitch is in the warning set and GYRO_WERROR is on by default, so an entry omitted
// here does not compile. The trailing return exists only because the language requires one, and unlike
// Motion.h's fallthrough it is not a safety net for a value cast in from outside the enumeration —
// nothing outside gyro can name a Transition, so there is no such value to catch.
[[nodiscard]] constexpr const Bundle& Definition(Transition transition) noexcept
{
	switch (transition)
	{
		case Transition::WindowOpen:
			return Detail::WindowOpen;
		case Transition::WindowClose:
			return Detail::WindowClose;
		case Transition::MenuAppear:
			return Detail::MenuAppear;
		case Transition::MenuDismiss:
			return Detail::MenuDismiss;
		case Transition::WorkspaceSwitch:
			return Detail::WorkspaceSwitch;
		case Transition::FocusChange:
			return Detail::FocusChange;
		case Transition::MatchedMove:
			return Detail::MatchedMove;
	}

	return Detail::WindowOpen;
}

// The table a commit resolves against, named the way a call site has it: a transition and the policy
// in force. The two-step through Definition is available and is what the tests use; this is the form
// that keeps the differ from ever holding a Bundle long enough to reach into one.
[[nodiscard]] constexpr ChannelTable Channels(Transition transition, MotionPolicy policy) noexcept
{
	return Channels(Definition(transition), policy);
}

// The contract everything downstream assumes. The catalog is data, so what is checked here is what
// makes an entry well formed rather than what makes the machinery correct — that is Bundle.h's and
// Motion.h's, and neither is repeated.

// Each entry is the one its name says, checked at the two places a switch can go wrong: the first
// case and the last.
static_assert(Definition(Transition::WindowOpen).Anchor == AnchorPolicy::SummonPoint);
static_assert(Definition(Transition::MatchedMove).Reduced.Form == ReducedForm::Cut);

// The anchor is declared exactly where something summoned the transition. Windows and menus are opened
// by a control the user hit and grow out of it; a workspace switch, a focus change, and a matched move
// have no summoning point to grow from, and the centre is right for all three.
static_assert(Definition(Transition::MenuAppear).Anchor == AnchorPolicy::SummonPoint);
static_assert(Definition(Transition::MenuDismiss).Anchor == AnchorPolicy::SummonPoint);
static_assert(Definition(Transition::WorkspaceSwitch).Anchor == AnchorPolicy::Centre);
static_assert(Definition(Transition::FocusChange).Anchor == AnchorPolicy::Centre);

namespace Detail
{
// The entries a user drives rather than watches, written as a list because there will be more of
// them — an overview swipe and a swipe-to-dismiss are both coming, and each should extend this line
// rather than rewrite the assertion underneath it.
inline constexpr std::array<Transition, 1> Drivable{ Transition::WorkspaceSwitch };
} // namespace Detail

// Checked over the whole catalog rather than about the entries named, which is the direction that
// matters: what is being caught is travel appearing on an entry nobody meant to make drivable, since
// that is the change a default could make and a diff would not show as one.
static_assert(
	[] {
		for (const Transition transition : AllTransitions)
		{
			bool expected = false;

			for (const Transition drivable : Detail::Drivable)
			{
				expected = expected || drivable == transition;
			}

			if (Definition(transition).IsInteractive() != expected)
			{
				return false;
			}
		}

		return true;
	}(),
	"A transition becomes one the user makes rather than one they watch by being given travel"
);

// A form that does not read a motion does not name one, which is Bundle.h's invariant checked where
// entries are written. An unread value is not harmless here: it compares, so two entries identical in
// everything that runs can differ, and a reader has to work out that a Cut's motion is noise rather
// than a claim about how the cut is paced.
static_assert(
	[] {
		for (const Transition transition : AllTransitions)
		{
			const ReducedMotion reduced = Definition(transition).Reduced;

			if (reduced.Form != ReducedForm::Fade && reduced.Using != ReducedMotion{}.Using)
			{
				return false;
			}
		}

		return true;
	}(),
	"Only a fade names the motion its fade takes"
);

// Every entry participates in at least one channel. An entry animating nothing is not a transition
// with no opinion — it is an entry somebody started and did not finish, and it would resolve to a cut
// everywhere with no error at any layer below this one.
static_assert([] {
	for (const Transition transition : AllTransitions)
	{
		bool participates = false;

		for (const Channel channel : AllChannels)
		{
			participates = participates || Definition(transition).Channels[channel].How != Disposition::Absent;
		}

		if (!participates)
		{
			return false;
		}
	}

	return true;
}());

// **Unchanged is a claim about the entry, and here is where it is checked.** Bundle.h's theorem covers
// Fade and Cut for every table any bundle could hold and cannot cover this one: Unchanged returns the
// authored table untouched, so an entry that moves geometry and claims to be its own reduced form
// would animate movement on the accessibility path with every layer below agreeing it was fine.
static_assert(
	[] {
		for (const Transition transition : AllTransitions)
		{
			if (Definition(transition).Reduced.Form != ReducedForm::Unchanged)
			{
				continue;
			}

			for (const Channel channel : AllChannels)
			{
				if (channel != Channel::Opacity && Definition(transition).Channels[channel].How != Disposition::Absent)
				{
					return false;
				}
			}
		}

		return true;
	}(),
	"Only a transition that was already pure opacity may declare itself its own reduced form"
);
