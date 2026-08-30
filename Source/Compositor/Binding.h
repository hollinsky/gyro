#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "Core/Input.h"
#include "Geometry/AxisTransform.h"
#include "Geometry/Space.h"
#include "Scene/Density.h"

// Which screen an absolute device is pointing at, and where on it.
//
// **This is decision 167, and it lives in the composition root because neither half of the process
// can answer it alone.** A touchscreen states a position on its own glass, as a fraction of that
// glass, and that is the whole of what it can honestly say — `Core/Input.h` refuses to carry a
// coordinate space for exactly this reason. Two things finish the sentence: *which* output the glass
// is in front of, and how the fraction lands on it. `Input` may not name an output and `Scene` may
// not name a libinput device, so the party that resolves both is the one that already fills a
// `SceneOutput` in.
//
// **A file rather than a hundred lines inside `Compositor.cpp`**, and the reason is that both traps
// here are invisible on the machine in front of you. A panel turned portrait and a panel at 2x are
// both *correct* until they are not, and the failure is a finger that lands somewhere it did not go
// on hardware nobody has on their desk. So the rungs and the arithmetic are stated where a test can
// run them against a rotated 2x output that no one has to own.

// One output, as the binding sees it. Four facts, and it is deliberately not a `SceneOutput`: what
// is wanted here is the connector's name and its millimetres, which the world's record does not
// carry, alongside the two fields of it that the mapping needs.
struct AbsolutePanel
{
	// The connector, as `GYRO_OUTPUT` would name it. Empty for a backend that has no connector — a
	// nested window, a headless sweep, a file — which is *not* a match for an empty property, since a
	// device with no property set does not have one to match with.
	std::string_view Connector{};

	// The panel's own image size, from EDID. Zero on either axis is no evidence rather than a size,
	// which `Scene/Density.h` already spells as `IsKnown`.
	PanelSize Size{};

	// What the panel scans out, which is what a fraction of the glass is a fraction *of*.
	PixelSize<DeviceSpace> Grid{};

	// `SceneOutput::Placement()`, carried rather than recomputed so that this file names no output
	// record and the mapping cannot drift from the one the frame walk uses.
	OutputAdapter Placement{};
};

// Which rung answered, which is what the log line says and the only reason the result is not a bare
// index. A person reading *bound by size* wants to know it was inferred; a person reading *unbound*
// wants to know which property fixes it.
enum class BindRung : std::uint8_t
{
	// `GYRO_OUTPUT` named a connector and it exists. Correct by construction rather than by inference.
	Property,
	// There is one output on this machine, so the ambiguity does not exist. The laptop, the tablet and
	// the kiosk, which is almost every machine with a touchscreen on it.
	Sole,
	// Exactly one panel is the same piece of glass, to within the tolerance below.
	Size,
	// Nothing answered, and the device's events are dropped.
	Unbound,
};

struct DeviceBinding
{
	BindRung Rung = BindRung::Unbound;
	std::size_t Output = 0;

	[[nodiscard]] constexpr bool IsBound() const noexcept { return Rung != BindRung::Unbound; }

	friend constexpr bool operator==(DeviceBinding, DeviceBinding) noexcept = default;
};

// **Ten millimetres per axis, and the number is EDID's rather than an engineering guess.** The format
// states an image size in two places — whole centimetres in the basic display block and millimetres
// in a detailed timing descriptor — so a panel that reports only the first arrives rounded to the
// nearest centimetre. A tolerance tighter than that unit refuses matches that are correct.
inline constexpr double GlassToleranceMm = 10.0;

// Whether a device's active area and a panel's image are the same piece of glass.
//
// **A digitizer is bonded to the panel behind it**, so the two agree to within a few millimetres of
// bezel overlap. Either side reporting nothing is no evidence and not a match against zero:
// `libinput_device_get_size` fails outright on a device without the data, and the connector says
// nothing on most projectors and every virtual output.
//
// The comparison is in the panel's own frame, so rotation never enters it. A monitor turned portrait
// in its stand is the same glass with the same millimetres; how it is *mounted* is `Land`'s question
// below and a different one.
[[nodiscard]] inline bool SameGlass(InputDeviceSize device, PanelSize panel) noexcept
{
	if (!panel.IsKnown() || device.WidthMm <= 0.0 || device.HeightMm <= 0.0)
	{
		return false;
	}

	return std::abs(device.WidthMm - static_cast<double>(panel.WidthMm)) <= GlassToleranceMm &&
	       std::abs(device.HeightMm - static_cast<double>(panel.HeightMm)) <= GlassToleranceMm;
}

// The panels a device's millimetres are consistent with, written into `into` and counted.
//
// Exposed rather than folded into `Bind` because the failure needs it: a tie leaves the device
// unbound, and the log line has to name the connectors it could not choose between. Asking the same
// predicate again is what keeps the message and the decision from ever disagreeing.
[[nodiscard]] inline std::size_t
SizeCandidates(const InputDevice& device, std::span<const AbsolutePanel> panels, std::span<std::size_t> into)
{
	if (!device.Size.has_value())
	{
		return 0;
	}

	std::size_t found = 0;

	for (std::size_t index = 0; index < panels.size(); ++index)
	{
		if (!SameGlass(*device.Size, panels[index].Size))
		{
			continue;
		}

		if (found < into.size())
		{
			into[found] = index;
		}

		++found;
	}

	return found;
}

// Which output an absolute device is bound to, from the first of three sources that answers.
//
// **Where none of them does, the device is bound to nothing and its events are dropped.** A touch
// that goes nowhere is better than a touch on the wrong screen, and that is the whole of the
// argument: a press that lands on a monitor a person is not looking at operates controls they cannot
// see, and nothing on screen indicates where the finger went. The failure is silent, it is
// destructive, and the person experiencing it cannot attribute it. An unresponsive touchscreen is
// none of those — it is the first thing anyone reports, and the log line has the fix in it.
//
// There is deliberately no fourth rung. *The first output*, *the largest output* and *the one holding
// the pointer* are each a way of always producing an answer, and an answer produced without evidence
// is the wrong-screen failure with the log line removed — the last is the worst of them, because it
// makes a touchscreen's target depend on where somebody last left the mouse, so the same finger on
// the same glass lands in two different places a minute apart.
[[nodiscard]] inline DeviceBinding Bind(const InputDevice& device, std::span<const AbsolutePanel> panels)
{
	if (panels.empty())
	{
		return {};
	}

	// **The property always wins, and it is the only source that is correct by construction.** Device
	// access is already a udev rule, so the file a person edits to let gyro *open* the touchscreen is
	// the file they edit to say which panel it is glued to. A property naming a connector that is not
	// here falls through rather than binding to nothing on purpose: a person who unplugs the monitor
	// named in their rule and plugs the touchscreen into the only other one gets the sole-output rung.
	if (!device.Output.empty())
	{
		for (std::size_t index = 0; index < panels.size(); ++index)
		{
			if (panels[index].Connector == device.Output)
			{
				return { .Rung = BindRung::Property, .Output = index };
			}
		}
	}

	if (panels.size() == 1)
	{
		return { .Rung = BindRung::Sole, .Output = 0 };
	}

	// **Uniqueness is required, and a tie leaves the device unbound.** Two panels of the same size — a
	// pair of matched monitors, a laptop beside an external screen of its own diagonal — produce two
	// candidates and no way to choose between them.
	std::size_t only = 0;

	if (SizeCandidates(device, panels, { &only, 1 }) == 1)
	{
		return { .Rung = BindRung::Size, .Output = only };
	}

	return {};
}

// Where a fraction of a device's own glass lands in the world.
//
// **The fraction goes onto the device grid and is unprojected from there, and mapping it onto the
// output's global rectangle instead is the tempting one-liner that is wrong on any panel that is
// turned.** The glass rotates with the panel it is glued to, so the device's own axes are the device
// grid's axes and not the layout's — on a portrait-mounted monitor the two differ by a quarter turn,
// and a person touching the top of the screen would press the side of it. The scale factor is the
// same mistake one order smaller: on a 2x panel the layout rectangle is half the grid, and a fraction
// stretched onto it is right only because the two errors are proportional, until the output is turned
// and they are not.
//
// **The unprojection is derived from the placement rather than maintained beside it**, which is
// decision 16 applied to a coordinate: `AxisTransform::Inverse` is exact for an output adapter by
// construction, so this is arithmetic gyro already trusts in the other direction every frame rather
// than a second mapping to keep in agreement with the first.
//
// **The fraction spans the whole grid — a touchscreen fills rather than fits.** There is no
// letterboxing because there is no aspect to preserve: the digitizer *is* the panel's surface, so the
// corners of one are the corners of the other. That does not hold for a graphics tablet, which is a
// separate slab with its own aspect ratio, and decision 167 leaves what to do about it to the commit
// that builds a tablet protocol.
[[nodiscard]] inline Point<GlobalSpace> Land(const AbsolutePanel& panel, double normalizedX, double normalizedY)
{
	// Double throughout, because device space is single precision and rounding the fraction to float
	// before it is unprojected would put a fraction of a device pixel on every contact — which is a
	// coordinate a person drags a scrollbar with.
	const Point<DeviceSpace, double> onGlass{
		normalizedX * static_cast<double>(panel.Grid.Width),
		normalizedY * static_cast<double>(panel.Grid.Height),
	};

	return panel.Placement.Inverse().Map(onGlass);
}
