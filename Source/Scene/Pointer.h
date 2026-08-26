#pragma once

#include <algorithm>
#include <cmath>
#include <limits>
#include <span>

#include "Core/Handle.h"
#include "Geometry/Space.h"
#include "Scene/Output.h"

// Where the pointer is, as the world holds it.
//
// **It is in `Scene` rather than in `Protocol` for `Scene/Focus.h`'s reason, and decision 152 settles
// it from the other end.** Three consumers need this position and none of them can move: hit-testing
// for `wl_pointer.enter` walks the scene tree, decision 141's Floorplanner already centres a new
// window on *the output holding the pointer*, and the cursor is a node in the published scene that
// gyro draws its own glyph for. A copy of the position held frame-side was considered there and
// rejected — it is a second encoding of one fact, it needs a third channel to feed it, and it
// reclaims nothing, because the dispatch thread wakes on the input event and the forward ring is
// newest-wins. So there is one position, it is here, and everything asks it.
//
// **A displacement is applied as a displacement, and the position is never rounded.** The cursor's
// place on a screen is subpixel — `GlobalSpace` is `double` for this among other reasons — and
// rounding per event is how slow motion turns into a pointer that sticks and jumps: a run of
// quarter-pixel steps each rounded to nothing is a pointer that does not move at all until it
// suddenly moves a whole pixel. Quantization is an output's, at the moment the cursor is drawn, which
// is decision 52 applied to the one node that moves at input rate.
//
// **Every device with a cursor drives this one, which is what a seat means.** Two mice and a touchpad
// on one machine do not make three pointers; they make one that three things can push, and a person
// with a mouse in each hand is moving *the* cursor. So each displacement is applied as it arrives and
// the position is the running total — one publication per dispatch iteration follows, whatever the
// devices did inside it, because a thousand-hertz mouse into a newest-wins ring is a thousand
// positions of which one is read.
//
// **Not every device has a cursor, and this class cannot tell.** A touchscreen has none — several
// contacts and nothing to draw — and a tablet tool has *its own*, per tool, because a stylus hovering
// somewhere the mouse is not is the whole point of a tablet. So `Move` and `WarpTo` are for devices
// that drive the seat's cursor, and wiring `IInput::Touch` or `IInput::Tool` to them is a bug this
// header can only forbid rather than prevent: it would make the cursor jump to wherever a finger
// landed, which is the behaviour every touchscreen laptop that gets this wrong exhibits.
//
// **This belongs to the seat, and the seat outlives the sessions on it — which is where it parts
// company with focus.** They sit side by side in the store today and that is not the same claim about
// either. Focus is a fact about one person's windows and means nothing in somebody else's world, so it
// is per session. Where the pointer is is a fact about the desk: a hand on a mouse in front of a
// panel, stated in `GlobalSpace`, which is the *machine's* output layout and reads the same to every
// session presented on it.
//
// So switching users must not move the cursor, and neither must locking. Decision 43 makes locking an
// output reassignment — the panel goes to the greeter's session and the locked one keeps running —
// and a pointer held per session would teleport on the way there and teleport back on unlock, with
// the same mouse untouched on the same desk throughout. It is the same argument at login: the splash
// draws no cursor, the greeter draws one, and a person's session inherits where they left it rather
// than being handed somewhere gyro picked.
//
// A remote session is not a counterexample and is the reason the axis is the seat rather than the
// machine: it arrives with its own input and its own virtual output, which is a second seat with a
// second pointer on it — not a second pointer on this one.
//
// **What is per session is the tuning and the glyph, not the position.** An acceleration profile, a
// left-handed button map and a cursor size are a person's, and none of them is a coordinate: the
// curve is applied at ingest against a device's own configuration, and decision 152 has gyro drawing
// the glyph itself. Reconfiguring a device when a session comes to the front is an operation rather
// than a second copy of where the pointer is.
//
// **So the widening is a seat and not a second store.** This is a value with no static state and no
// identity in it, which is what keeps that a move rather than a rewrite — but when a store becomes
// per session, this does not go with it and `SceneFocus` does.
//
// **Acceleration is not here.** The curve is nonlinear in velocity, so accumulating raw displacements
// and accelerating the total is not the same pointer as accelerating each — it feels right when it is
// flung and mushy when it is eased. What arrives here has already been through it. `Core/Input.h`
// states the same rule at the other end of the path.

// SPEC: how close to an output's far edge the pointer may be placed. It is `wl_fixed`'s resolution,
// which makes it the finest position anything downstream can express — so a pointer clamped to it is
// at the edge as far as every consumer is concerned, and the coordinate is still *on* the output
// rather than one past it.
inline constexpr double PointerEdge = 1.0 / 256.0;

// The pointer, and the outputs it is allowed to be on.
//
// **Confinement is to the union of the outputs, and the union is not a rectangle.**
// Docs/Experience.md promises the pointer can reach every part of the screen, and the shape that
// breaks it is two monitors of different heights side by side: clamp each axis to the bounding box
// independently and the pointer walks into the empty corner beside the shorter one, where it is on no
// output and cannot be seen. Clamping to the *union* is what this class is mostly about, and the
// slide below is what keeps a diagonal push along an edge from stopping dead.
class ScenePointer
{
public:
	// Where it is, in the space the world is laid out in. Meaningful only once there is an output —
	// before that it is the origin and `On()` is null, which is the honest answer on a machine whose
	// panels have not come up.
	[[nodiscard]] Point<GlobalSpace> Position() const noexcept { return m_Position; }

	// Whether the cursor should be drawn. A machine driven by a touchscreen has a pointer position and
	// no pointer on screen, and a cursor parked in the middle of a kiosk display is the visible form of
	// getting that wrong. Set when a device that has a cursor moves it; cleared by a touch.
	[[nodiscard]] bool IsVisible() const noexcept { return m_Visible; }

	void Show() noexcept { m_Visible = true; }
	void Hide() noexcept { m_Visible = false; }

	// The output the pointer is over, or null where there is none — which is what the Floorplanner
	// asks and what a `set_cursor` has to name a scale for.
	[[nodiscard]] OutputId On(std::span<const SceneOutput> outputs) const noexcept
	{
		for (const SceneOutput& output : outputs)
		{
			if (Contains(output.Bounds, m_Position))
			{
				return output.Id;
			}
		}

		return {};
	}

	// Move by a displacement that has already been accelerated. Returns where it ended up, which is not
	// where it was pushed if the push left the union.
	//
	// **A push that leaves the union slides along it rather than stopping.** Dragging up and to the
	// right along the top edge of a screen should keep travelling right, and a version that refuses the
	// whole displacement because one component of it was illegal is a pointer that jams in every
	// corner. So the diagonal is tried, then each axis alone, and only a push with nowhere to go at all
	// falls through to the clamp below.
	Point<GlobalSpace> Move(Offset<GlobalSpace> delta, std::span<const SceneOutput> outputs)
	{
		m_Visible = true;

		if (outputs.empty())
		{
			// Nothing to be confined to and nothing to be drawn on. The displacement is still taken, so a
			// pointer does not silently reset when a monitor is asleep.
			m_Position = m_Position + delta;

			return m_Position;
		}

		const Point<GlobalSpace> wanted = m_Position + delta;

		if (Inside(outputs, wanted))
		{
			m_Position = wanted;

			return m_Position;
		}

		// One axis at a time, larger component first: a push that is mostly rightwards should keep its
		// rightwards travel when its upwards travel is what left the screen.
		const bool horizontalFirst = std::abs(delta.X) >= std::abs(delta.Y);

		const Point<GlobalSpace> first{ horizontalFirst ? wanted.X : m_Position.X,
			                            horizontalFirst ? m_Position.Y : wanted.Y };
		const Point<GlobalSpace> second{ horizontalFirst ? m_Position.X : wanted.X,
			                             horizontalFirst ? wanted.Y : m_Position.Y };

		if (Inside(outputs, first))
		{
			m_Position = first;
		}
		else if (Inside(outputs, second))
		{
			m_Position = second;
		}
		else
		{
			m_Position = Nearest(outputs, wanted);
		}

		return m_Position;
	}

	// Put it somewhere: a warp, a touch that took over the cursor, a tablet mapped onto an output.
	//
	// **A warp is clamped and never slid.** It states a destination rather than a direction, so there is
	// no travel to preserve — the nearest place on the union is the whole of what can be honoured.
	Point<GlobalSpace> WarpTo(Point<GlobalSpace> position, std::span<const SceneOutput> outputs)
	{
		m_Position = outputs.empty() ? position : Nearest(outputs, position);

		return m_Position;
	}

	// The outputs changed under it — one was unplugged, or somebody dragged a monitor in a settings
	// panel. Called by whoever set them, because a pointer left stranded in the space a display used to
	// occupy is a pointer nobody can find and no motion can rescue: every displacement from out there
	// slides along a union it is not touching.
	void Reconfine(std::span<const SceneOutput> outputs)
	{
		if (outputs.empty() || Inside(outputs, m_Position))
		{
			return;
		}

		m_Position = Nearest(outputs, m_Position);
	}

private:
	// Half-open, which is the same convention `Rect` is read with everywhere else: two outputs abutting
	// at x = 1920 put the column at 1920 on the right-hand one and not on both.
	[[nodiscard]] static bool Contains(const Rect<GlobalSpace>& bounds, Point<GlobalSpace> point) noexcept
	{
		return point.X >= bounds.Left() && point.X < bounds.Right() && point.Y >= bounds.Top() &&
		       point.Y < bounds.Bottom();
	}

	[[nodiscard]] static bool Inside(std::span<const SceneOutput> outputs, Point<GlobalSpace> point) noexcept
	{
		return std::any_of(outputs.begin(), outputs.end(), [point](const SceneOutput& output) {
			return Contains(output.Bounds, point);
		});
	}

	// The closest point on the union, which is the closest of the per-output clamps. Linear in outputs
	// and called only when a push already failed, so the cost lands on the frame somebody drove into a
	// corner rather than on every motion.
	[[nodiscard]] static Point<GlobalSpace> Nearest(std::span<const SceneOutput> outputs, Point<GlobalSpace> point)
	{
		Point<GlobalSpace> best = point;
		double closest = std::numeric_limits<double>::infinity();

		for (const SceneOutput& output : outputs)
		{
			const Rect<GlobalSpace>& bounds = output.Bounds;

			if (bounds.IsEmpty())
			{
				continue;
			}

			// The far edges are exclusive, so the clamp stops one expressible step short of them rather
			// than landing on a coordinate `Contains` would then refuse — which would leave `Reconfine`
			// putting the pointer somewhere `On` reports as nowhere.
			const Point<GlobalSpace> candidate{
				std::clamp(point.X, bounds.Left(), std::max(bounds.Left(), bounds.Right() - PointerEdge)),
				std::clamp(point.Y, bounds.Top(), std::max(bounds.Top(), bounds.Bottom() - PointerEdge)),
			};

			const Offset<GlobalSpace> away = candidate - point;
			const double distance = (away.X * away.X) + (away.Y * away.Y);

			if (distance < closest)
			{
				closest = distance;
				best = candidate;
			}
		}

		return best;
	}

	Point<GlobalSpace> m_Position{};

	// False until something with a cursor moves. A compositor that draws a pointer before a mouse has
	// ever been touched is one that draws a pointer on a tablet.
	bool m_Visible = false;
};
