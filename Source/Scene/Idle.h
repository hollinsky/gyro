#pragma once

#include <algorithm>
#include <optional>

#include "Core/Time.h"
#include "Core/Wake.h"
#include "Scene/Output.h"
#include "Scene/Store.h"

// Decision 58's ladder as the world holds it: when somebody last touched the machine, and which rung
// that puts it on.
//
// **One rung is built, and it is display off.** It saves the most and it is the one every output has —
// a backlight is an internal panel's alone, dimming has to compose with the brightness key before it can
// exist at all, and locking is an output reassignment with an envelope nothing is entitled to configure
// yet. Each of those arrives as a timeout here and a verb on the store, which is the shape this is
// built in.
//
// **It answers a `Wake`, and that is what stops it being a timer.** Architecture.md's invariant allows
// one armed instant per output and makes that instant the fold, so the timeout is one more contributor
// to it: a machine somebody is using arms the instant it would go dark, and a machine that has gone dark
// arms nothing at all, which is the state the rung exists to reach.
//
// **Activity is the seat's and not a session's** (58), and it is held as the instant of the latest
// event rather than as a count, because what a rung is measured from is when somebody last did
// something. One seat is one term, so every output goes dark together and comes back together; the
// per-session half of the fold decision 58 describes has nothing to fold until there is a second seat
// or a session whose idleness differs.
//
// **The event that lights the screens is reported rather than absorbed**, because what becomes of it is
// routing and routing is the composition root's. What the root does with it is swallow it: a key pressed
// at a dark screen was aimed at a picture nobody could see.
class IdleLadder
{
public:
	// Turn every output off after `timeout` without input, counting from `now`. Unset is a machine that
	// never goes dark, which is every run that did not ask.
	void DisplayOffAfter(Duration timeout, Instant now) noexcept
	{
		m_DisplayOff = timeout;
		m_Touched = now;
	}

	// Somebody did something at `when`. True where that lit outputs this ladder had turned off, and only
	// for the event that did it.
	bool Touch(SceneStore& scene, Instant when) noexcept
	{
		// The latest rather than the last, because nothing promises the seat's devices are drained in the
		// order their events happened, and a timeout measured from an older one would come early.
		m_Touched = std::max(m_Touched, when);

		if (!m_Dark)
		{
			return false;
		}

		m_Dark = false;
		Power(scene, true);

		return true;
	}

	// Take the rung that has fallen due by `now`, and say when the next one will.
	[[nodiscard]] Wake Step(SceneStore& scene, Instant now) noexcept
	{
		if (!m_DisplayOff || m_Dark)
		{
			return Wake::Never();
		}

		const Instant due = Advanced(m_Touched, *m_DisplayOff);

		if (now < due)
		{
			return Wake::At(due);
		}

		m_Dark = true;
		Power(scene, false);

		return Wake::Never();
	}

	[[nodiscard]] bool IsDark() const noexcept { return m_Dark; }

private:
	static void Power(SceneStore& scene, bool powered) noexcept
	{
		for (const SceneOutput& output : scene.Outputs())
		{
			scene.SetOutputPower(output.Id, powered);
		}
	}

	std::optional<Duration> m_DisplayOff{};
	Instant m_Touched{};
	bool m_Dark = false;
};
