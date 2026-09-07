#include "Protocol/Fractional.h"

#include <array>
#include <span>

#include "Geometry/Scale.h"
#include "Protocol/Context.h"
#include "Protocol/Surface.h"
#include "Scene/Output.h"
#include "Testing/Test.h"

// What scale a window is asked for, and who is asked.
//
// **The fold is where all the judgement is, and it is a free function so that it can be asked
// directly.** Everything else this protocol does is bookkeeping — one object per surface, one event
// per change — and the thing that can actually be wrong on a panel is which number comes out of a
// reach mask.
//
// **An object here has no `wl_resource` behind it**, which is `Viewporter.Test.cpp`'s arrangement:
// `PreferredScale` on an absent resource writes nothing to a wire nobody is reading, so what a `Send`
// did is asserted through `Sent` — the value the object will compare the next one against, and
// therefore the thing that decides whether a client hears anything at all.

namespace
{
[[nodiscard]] SceneOutput At(double scale)
{
	SceneOutput output{};
	output.Density = Scale::FromDouble(scale);

	return output;
}
} // namespace

GYRO_TEST(Fractional, TheScaleIsTheMaximumOverTheOutputsAWindowIsOn)
{
	const std::array<SceneOutput, 3> outputs{ At(1.0), At(1.5), At(2.0) };

	// The laptop panel and the external monitor beside it, with a window straddling both. Decision 56
	// takes the maximum rather than the larger overlap: the overlap rule flips at the halfway line of a
	// drag, which is the middle of the gesture and the worst moment available.
	GYRO_CHECK_EQ(PreferredScale(0b011, outputs), Scale::FromDouble(1.5));
	GYRO_CHECK_EQ(PreferredScale(0b101, outputs), Scale::FromDouble(2.0));
	GYRO_CHECK_EQ(PreferredScale(0b010, outputs), Scale::FromDouble(1.5));

	// And the numerator is what goes on the wire, in the protocol's own 120ths.
	GYRO_CHECK_EQ(PreferredScale(0b010, outputs).Numerator(), 180);
}

GYRO_TEST(Fractional, AWindowOnNoOutputYetIsToldTheDensestPanel)
{
	const std::array<SceneOutput, 3> outputs{ At(1.0), At(2.0), At(1.25) };

	// A client creates this object before its first commit, so its surface is on nothing and whatever it
	// hears is what it sizes its first buffer at. Too high costs one buffer larger than it needed to be;
	// too low is every window on a HiDPI panel opening soft and then popping sharp a frame later.
	GYRO_CHECK_EQ(PreferredScale(0, outputs), Scale::FromDouble(2.0));
}

GYRO_TEST(Fractional, NoOutputsAtAllIsIdentity)
{
	GYRO_CHECK_EQ(PreferredScale(0, std::span<const SceneOutput>{}), Scale{});
	GYRO_CHECK_EQ(PreferredScale(0b101, std::span<const SceneOutput>{}), Scale{});
}

GYRO_TEST(Fractional, ABitPastTheEndOfTheOutputSetNamesNothing)
{
	const std::array<SceneOutput, 2> outputs{ At(1.0), At(1.5) };

	// The mask is wider than the set it indexes, and a bit above the set is a display that is not there
	// rather than one at some default scale. What is folded is the outputs that exist.
	GYRO_CHECK_EQ(PreferredScale(0b1000, outputs), Scale{});
	GYRO_CHECK_EQ(PreferredScale(0b1010, outputs), Scale::FromDouble(1.5));
}

GYRO_TEST(Fractional, AnEventGoesOutOnceAndOnlyWhenTheScaleMoves)
{
	HostContext context;
	ClientSurface surface{ context };
	ClientFractionalScale fractional{ context, &surface };

	GYRO_CHECK(!fractional.Sent().has_value());

	// **The absence before the first send is the point of the `optional`.** Identity is an ordinary
	// preferred scale, so an object that started out holding `Scale{}` would silently withhold the first
	// event from every client on an unscaled panel — which is most of them.
	fractional.Send(Scale{});

	GYRO_REQUIRE(fractional.Sent().has_value());
	GYRO_CHECK_EQ(*fractional.Sent(), Scale{});

	fractional.Send(Scale::FromDouble(1.5));

	GYRO_CHECK_EQ(*fractional.Sent(), Scale::FromDouble(1.5));

	// And the comparison is what keeps `SyncOutputEntry` off the wire: it runs on every dispatch wakeup,
	// which is input rate for as long as somebody is dragging a window.
	fractional.Send(Scale::FromDouble(1.5));

	GYRO_CHECK_EQ(*fractional.Sent(), Scale::FromDouble(1.5));
}

GYRO_TEST(Fractional, ASurfaceCarriesOneOfTheseAndTheSecondIsRefused)
{
	HostContext context;
	ClientSurface surface{ context };
	ClientFractionalScale first{ context, &surface };
	ClientFractionalScale second{ context, &surface };

	GYRO_CHECK(surface.AdoptFractionalScale(first));
	GYRO_CHECK_EQ(surface.FractionalScale(), &first);

	// `fractional_scale_exists`, raised by the caller. The object the client asked for stays inert
	// rather than becoming a second thing speaking for one surface's scale.
	GYRO_CHECK(!surface.AdoptFractionalScale(second));
	GYRO_CHECK_EQ(surface.FractionalScale(), &first);

	second.ForgetSurface();
}

GYRO_TEST(Fractional, DestroyingTheObjectStopsTheEvents)
{
	HostContext context;
	ClientSurface surface{ context };

	{
		ClientFractionalScale fractional{ context, &surface };

		GYRO_REQUIRE(surface.AdoptFractionalScale(fractional));
	}

	// **And leaves the surface itself untouched**, which is the whole difference from a viewport going
	// away: that one takes a size the client wrote with it, and this one was never anything but a
	// suggestion gyro was sending.
	GYRO_CHECK_EQ(surface.FractionalScale(), nullptr);
}
