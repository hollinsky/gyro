#include "Nested/Input.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "Core/Clock.h"
#include "Core/Result.h"
#include "Core/Signal.h"
#include "Geometry/Space.h"
#include "Nested/Host.h"
#include "Nested/Peer.h"
#include "Testing/Test.h"

// The host's pointer, from the wire to the signals the dispatch thread reads.
//
// **Driven through the real connection rather than by poking the queue**, which is the only way the
// two halves of this file are actually checked: a `wl_pointer.enter` naming a surface has to be
// attributed to the window that surface belongs to, and a fraction has to be taken against that
// window's size. Both are wire facts, and `Nested/Peer.h` is what lets a test state them.
//
// **The window here is a bare `wl_surface` rather than a `NestedOutput`**, deliberately. What the
// input path needs of a window is its surface id and its size, which is exactly what `Track` takes;
// building a presenter to supply them would put an allocator, a renderer and a dmabuf import in
// front of every assertion about a button.
//
// The thread rule is not exercised. Both halves run on the test's own thread, because what is being
// checked is the handoff's content — nothing dropped, nothing reordered, and the device named for
// the window it is on — and a second thread would only make the same assertions arrive late.

namespace
{
constexpr PixelSize<DeviceSpace> Extent{ 800, 600 };

// What crossed, in order, so a test asserts on a sequence rather than on a count.
struct Watcher
{
	void Observe(IInput& input)
	{
		m_Added.ConnectTo<&Watcher::OnAdded>(input.Added, *this);
		m_Position.ConnectTo<&Watcher::OnPosition>(input.Position, *this);
		m_Button.ConnectTo<&Watcher::OnButton>(input.Button, *this);
		m_Scroll.ConnectTo<&Watcher::OnScroll>(input.Scroll, *this);
		m_Key.ConnectTo<&Watcher::OnKey>(input.Key, *this);
	}

	// The strings on an `InputDevice` are borrowed for the duration of the emit, per `Core/Input.h`,
	// so what is kept is the record with copies of its own behind the views.
	void OnAdded(const InputDevice& device)
	{
		Names.push_back(std::string{ device.Name });
		Connectors.push_back(std::string{ device.Output });

		InputDevice kept = device;
		kept.Name = Names.back();
		kept.Output = Connectors.back();

		Devices.push_back(kept);
	}

	void OnPosition(const PointerPosition& event) { Positions.push_back(event); }

	void OnButton(const PointerButton& event) { Buttons.push_back(event); }

	void OnScroll(const PointerScroll& event) { Scrolls.push_back(event); }

	void OnKey(const KeyEvent& event) { Keys.push_back(event); }

	// The devices that are a window's pointer. The keyboard is announced when the seat admits it has
	// one, so it is already in `Devices` before any of these tests move a mouse — and it is neither
	// absolute nor bound to an output, which is the difference this filter is named for.
	[[nodiscard]] std::vector<InputDevice> Pointers() const
	{
		std::vector<InputDevice> pointers;

		for (const InputDevice& device : Devices)
		{
			if (device.Absolute)
			{
				pointers.push_back(device);
			}
		}

		return pointers;
	}

	std::vector<InputDevice> Devices;
	std::vector<PointerPosition> Positions;
	std::vector<PointerButton> Buttons;
	std::vector<PointerScroll> Scrolls;
	std::vector<KeyEvent> Keys;

	// Reserved so that a view handed out on one emit is not left dangling by the next push. A deque
	// would say the same thing without the number; this is one line and the counts here are tiny.
	std::vector<std::string> Names{ [] {
		std::vector<std::string> names;
		names.reserve(8);

		return names;
	}() };
	std::vector<std::string> Connectors{ [] {
		std::vector<std::string> connectors;
		connectors.reserve(8);

		return connectors;
	}() };

private:
	Connection<const InputDevice&> m_Added;
	Connection<const PointerPosition&> m_Position;
	Connection<const PointerButton&> m_Button;
	Connection<const PointerScroll&> m_Scroll;
	Connection<const KeyEvent&> m_Key;
};

// A host, a connection to it, and one surface standing in for a window.
struct Session
{
	Nested::Peer Host;
	MonotonicClock Clock;
	Nested::NestedHost Client{ Clock };
	Wayland::WlSurfaceIgnoring SurfaceEvents;
	Wayland::WlSurface Surface;
	Watcher Observed;

	[[nodiscard]] Result<void> Open(bool withWindow = true)
	{
		if (const Result<void> opened = Host.Open(); !opened)
		{
			return opened;
		}

		Result<void> connected{};
		Nested::PumpWhile(Host, [&] { connected = Client.Open(); });

		if (!connected)
		{
			return connected;
		}

		Observed.Observe(Client.Input());

		if (withWindow)
		{
			Surface = Client.Globals().Compositor.CreateSurface(SurfaceEvents);
			Client.Input().Track(0, Surface.Id(), Extent);
		}

		// **The flush is the real thing's, arriving early.** `get_pointer` is sent from inside the
		// seat's capability handler, which runs on a drain, and `NestedHost::Drain` flushes last — so in
		// a running session it is on the wire before anything could use it. Here there has been no drain
		// yet, so this is where it goes out, and the peer has to read it before a test can send an event
		// through the object it creates.
		if (const Result<void> flushed = Client.Flush(); !flushed)
		{
			return flushed;
		}

		static_cast<void>(Host.Pump());

		return {};
	}

	// One turn of the whole path: the host's events reach the connection, the connection's drain fills
	// the queue, and the queue's drain emits. Two drains because they are two threads in the real
	// thing, and the order between them is the whole of the handoff.
	void Turn()
	{
		static_cast<void>(Client.Drain());
		static_cast<void>(Client.Input().Drain());
	}
};
} // namespace

GYRO_TEST(NestedInput, AHostWithASeatGivesTheClientAPointer)
{
	Session session;

	GYRO_REQUIRE(session.Open().has_value());
	GYRO_CHECK(session.Host.HasPointer());
	GYRO_CHECK_EQ(session.Host.Unhandled, std::uint32_t{ 0 });
}

GYRO_TEST(NestedInput, AHostWithNoSeatIsStillAHostToNestIn)
{
	Session session;

	session.Host.Globals = { Nested::PeerGlobal{ Wayland::WlCompositor::WireName, 6 },
		                     Nested::PeerGlobal{ Wayland::XdgWmBase::WireName, 6 },
		                     Nested::PeerGlobal{ Wayland::ZwpLinuxDmabufV1::WireName, 5 },
		                     Nested::PeerGlobal{ Wayland::WpPresentation::WireName, 1 } };

	GYRO_REQUIRE(session.Open().has_value());
	GYRO_CHECK(!session.Host.HasPointer());

	// The source exists whether or not the host has a seat, so the composition root wires one
	// descriptor either way and a session with no mouse is a session rather than a failure.
	GYRO_CHECK(session.Client.Input().Descriptor().IsValid());
}

GYRO_TEST(NestedInput, EnteringAWindowAnnouncesADeviceNamedForItAndHidesTheHostCursor)
{
	Session session;

	GYRO_REQUIRE(session.Open().has_value());
	GYRO_REQUIRE(session.Host.SendPointerEnter(400.0, 300.0).has_value());

	session.Turn();

	GYRO_REQUIRE_EQ(session.Observed.Pointers().size(), std::size_t{ 1 });
	GYRO_CHECK(session.Observed.Pointers().front().Absolute);
	GYRO_CHECK_EQ(session.Observed.Pointers().front().Output, std::string_view{ "nested-0" });

	// No millimetres, so `Compositor/Binding.h`'s size rung is offered no evidence and the connector is
	// what binds this device.
	GYRO_CHECK(!session.Observed.Pointers().front().Size.has_value());

	// gyro draws its own pointer, so the host's has to go.
	static_cast<void>(session.Host.Pump());
	GYRO_CHECK_EQ(session.Host.CursorsHidden, std::uint32_t{ 1 });

	// The enter carries a position, so a cursor appears where the pointer already is rather than at
	// the last place it was seen or at the origin.
	GYRO_REQUIRE_EQ(session.Observed.Positions.size(), std::size_t{ 1 });
	GYRO_CHECK_EQ(session.Observed.Positions.front().NormalizedX, 0.5);
	GYRO_CHECK_EQ(session.Observed.Positions.front().NormalizedY, 0.5);
}

GYRO_TEST(NestedInput, AMotionIsAFractionOfTheWindowItArrivedIn)
{
	Session session;

	GYRO_REQUIRE(session.Open().has_value());
	GYRO_REQUIRE(session.Host.SendPointerEnter(0.0, 0.0).has_value());
	GYRO_REQUIRE(session.Host.SendPointerMotion(200.0, 450.0).has_value());
	GYRO_REQUIRE(session.Host.SendPointerFrame().has_value());

	session.Turn();

	GYRO_REQUIRE_EQ(session.Observed.Positions.size(), std::size_t{ 2 });
	GYRO_CHECK_EQ(session.Observed.Positions.back().NormalizedX, 0.25);
	GYRO_CHECK_EQ(session.Observed.Positions.back().NormalizedY, 0.75);

	// One device however many events it produced: the window is the device, and a second announcement
	// would be a second binding for the composition root to resolve.
	GYRO_CHECK_EQ(session.Observed.Pointers().size(), std::size_t{ 1 });
}

GYRO_TEST(NestedInput, AButtonCarriesTheKernelsCodeUnconverted)
{
	Session session;

	GYRO_REQUIRE(session.Open().has_value());
	GYRO_REQUIRE(session.Host.SendPointerEnter(10.0, 10.0).has_value());

	// `BTN_LEFT`, which is what the wire carries and what `Core/Input.h` carries — the point being that
	// nothing in between translates it.
	GYRO_REQUIRE(session.Host.SendPointerButton(0x110, true).has_value());
	GYRO_REQUIRE(session.Host.SendPointerButton(0x110, false).has_value());

	session.Turn();

	GYRO_REQUIRE_EQ(session.Observed.Buttons.size(), std::size_t{ 2 });
	GYRO_CHECK_EQ(session.Observed.Buttons.front().Code, std::uint32_t{ 0x110 });
	GYRO_CHECK(session.Observed.Buttons.front().Pressed);
	GYRO_CHECK(!session.Observed.Buttons.back().Pressed);
	GYRO_CHECK_EQ(session.Observed.Buttons.front().Device, session.Observed.Pointers().front().Id);
}

GYRO_TEST(NestedInput, AScrollTakesItsSourceFromTheGroupAndItsDetentFromValue120)
{
	Session session;

	GYRO_REQUIRE(session.Open().has_value());
	GYRO_REQUIRE(session.Host.SendPointerEnter(10.0, 10.0).has_value());

	// The order a host actually sends: the source, then the high-resolution step, then the distance it
	// describes. Reading `value120` after the axis it belongs to is the defect this pins.
	GYRO_REQUIRE(session.Host.SendPointerAxisSource(1).has_value());
	GYRO_REQUIRE(session.Host.SendPointerAxisValue120(0, 120).has_value());
	GYRO_REQUIRE(session.Host.SendPointerAxis(0, 15.0).has_value());
	GYRO_REQUIRE(session.Host.SendPointerFrame().has_value());

	session.Turn();

	GYRO_REQUIRE_EQ(session.Observed.Scrolls.size(), std::size_t{ 1 });
	GYRO_CHECK(session.Observed.Scrolls.front().Source == ScrollSource::Finger);
	GYRO_CHECK(session.Observed.Scrolls.front().Axis == ScrollAxis::Vertical);
	GYRO_CHECK_EQ(session.Observed.Scrolls.front().Distance, 15.0);
	GYRO_CHECK_EQ(session.Observed.Scrolls.front().Clicks120, 120.0);
	GYRO_CHECK(!session.Observed.Scrolls.front().Stop);
}

GYRO_TEST(NestedInput, TheFingersLeavingThePadIsAnIncrementWithNoDistance)
{
	Session session;

	GYRO_REQUIRE(session.Open().has_value());
	GYRO_REQUIRE(session.Host.SendPointerEnter(10.0, 10.0).has_value());
	GYRO_REQUIRE(session.Host.SendPointerAxisSource(1).has_value());
	GYRO_REQUIRE(session.Host.SendPointerAxisStop(1).has_value());

	session.Turn();

	GYRO_REQUIRE_EQ(session.Observed.Scrolls.size(), std::size_t{ 1 });
	GYRO_CHECK(session.Observed.Scrolls.front().Stop);
	GYRO_CHECK(session.Observed.Scrolls.front().Axis == ScrollAxis::Horizontal);
	GYRO_CHECK_EQ(session.Observed.Scrolls.front().Distance, 0.0);
}

GYRO_TEST(NestedInput, NothingCrossesWhileThePointerIsOnSomebodyElsesWindow)
{
	Session session;

	GYRO_REQUIRE(session.Open().has_value());
	GYRO_REQUIRE(session.Host.SendPointerEnter(10.0, 10.0).has_value());
	GYRO_REQUIRE(session.Host.SendPointerLeave().has_value());
	GYRO_REQUIRE(session.Host.SendPointerMotion(100.0, 100.0).has_value());
	GYRO_REQUIRE(session.Host.SendPointerButton(0x110, true).has_value());

	session.Turn();

	// The enter's own position, and nothing after the leave. What a person sees is the cursor parked
	// where they took the mouse off the window, which is what it does at the edge of a monitor.
	GYRO_CHECK_EQ(session.Observed.Positions.size(), std::size_t{ 1 });
	GYRO_CHECK(session.Observed.Buttons.empty());
}

GYRO_TEST(NestedInput, AnEventBeforeAnyWindowIsTrackedIsDropped)
{
	Session session;

	GYRO_REQUIRE(session.Open(false).has_value());
	GYRO_REQUIRE(session.Host.SendPointerMotion(100.0, 100.0).has_value());

	session.Turn();

	GYRO_CHECK(session.Observed.Positions.empty());
	GYRO_CHECK(session.Observed.Pointers().empty());
}

GYRO_TEST(NestedInput, AResizeChangesWhatAFractionIsTakenAgainstAndNotTheDevice)
{
	Session session;

	GYRO_REQUIRE(session.Open().has_value());
	GYRO_REQUIRE(session.Host.SendPointerEnter(400.0, 300.0).has_value());

	session.Turn();

	// What `NestedOutput` does on every configure it adopts.
	session.Client.Input().Track(0, session.Surface.Id(), PixelSize<DeviceSpace>{ 1600, 1200 });

	GYRO_REQUIRE(session.Host.SendPointerMotion(400.0, 300.0).has_value());

	session.Turn();

	GYRO_REQUIRE_EQ(session.Observed.Positions.size(), std::size_t{ 2 });
	GYRO_CHECK_EQ(session.Observed.Positions.back().NormalizedX, 0.25);
	GYRO_CHECK_EQ(session.Observed.Positions.back().NormalizedY, 0.25);

	// Still one device, still called what it was called: a window that changed size is the same piece
	// of glass, and re-announcing it would leave the root with two bindings for one pointer.
	GYRO_REQUIRE_EQ(session.Observed.Pointers().size(), std::size_t{ 1 });
	GYRO_CHECK_EQ(session.Client.Input().Connector(0), std::string_view{ "nested-0" });
}

// The keyboard.
//
// **What is checked here is mostly what is *not* forwarded**, because that is where a nested keyboard
// goes wrong: a keymap that leaks a descriptor, an enter that types characters nobody pressed, and a
// focus change that leaves a modifier down for the rest of the session. Each of those is a test
// below, and the plain *a key crosses* one is the shortest of them.

GYRO_TEST(NestedInput, AHostWithASeatGivesTheClientAKeyboardAndNamesItAtOnce)
{
	Session session;

	GYRO_REQUIRE(session.Open().has_value());
	GYRO_CHECK(session.Host.HasKeyboard());

	session.Turn();

	// Announced when the seat admits it has one rather than at the first keystroke, which is the
	// opposite of the pointer's rule: a pointer has a window to be bound to and a keyboard has nothing,
	// so there is nothing to wait for.
	GYRO_REQUIRE_EQ(session.Observed.Devices.size(), std::size_t{ 1 });
	GYRO_CHECK(!session.Observed.Devices.front().Absolute);
	GYRO_CHECK(session.Observed.Devices.front().Output.empty());
	GYRO_CHECK_EQ(session.Host.Unhandled, std::uint32_t{ 0 });
}

GYRO_TEST(NestedInput, AKeyCarriesTheKernelsCodeUnconverted)
{
	Session session;

	GYRO_REQUIRE(session.Open().has_value());
	GYRO_REQUIRE(session.Host.SendKeyboardEnter({}).has_value());

	// `KEY_A`, which is what the wire carries and what `Core/Input.h` carries. No keymap is consulted
	// on the way, which is the rule `Seam/Input.h` states and the reason the escape chord cannot be
	// moved by somebody selecting Dvorak in the surrounding session.
	GYRO_REQUIRE(session.Host.SendKeyboardKey(30, 1).has_value());
	GYRO_REQUIRE(session.Host.SendKeyboardKey(30, 0).has_value());

	session.Turn();

	GYRO_REQUIRE_EQ(session.Observed.Keys.size(), std::size_t{ 2 });
	GYRO_CHECK_EQ(session.Observed.Keys.front().Code, std::uint32_t{ 30 });
	GYRO_CHECK(session.Observed.Keys.front().Pressed);
	GYRO_CHECK(!session.Observed.Keys.back().Pressed);

	// One device for the whole connection, and it is not a window's.
	GYRO_CHECK_EQ(session.Observed.Keys.front().Device, session.Observed.Devices.front().Id);
	GYRO_CHECK(session.Observed.Pointers().empty());
}

GYRO_TEST(NestedInput, TheKeysHeldOnEntryAreNotTypedIntoAnything)
{
	Session session;

	GYRO_REQUIRE(session.Open().has_value());

	// `KEY_LEFTSHIFT` and `KEY_A` already down when focus arrives. `wl_keyboard` says in as many words
	// that a client must not replay these as presses, and gyro's reason for obeying is concrete: the
	// replay would reach a terminal inside the nested session as a character nobody typed.
	const std::array<std::uint32_t, 2> held{ 42, 30 };

	GYRO_REQUIRE(session.Host.SendKeyboardEnter(held).has_value());

	session.Turn();

	GYRO_CHECK(session.Observed.Keys.empty());
}

GYRO_TEST(NestedInput, FocusLeavingReleasesEverythingStillHeld)
{
	Session session;

	GYRO_REQUIRE(session.Open().has_value());
	GYRO_REQUIRE(session.Host.SendKeyboardEnter({}).has_value());

	// `KEY_LEFTALT` down, and then the person alt-tabs away — so the release happens somewhere gyro
	// cannot see it. A compositor that kept `Alt` down here turns every keystroke after it into a
	// shortcut nobody asked for.
	GYRO_REQUIRE(session.Host.SendKeyboardKey(56, 1).has_value());
	GYRO_REQUIRE(session.Host.SendKeyboardLeave().has_value());

	session.Turn();

	GYRO_REQUIRE_EQ(session.Observed.Keys.size(), std::size_t{ 2 });
	GYRO_CHECK(session.Observed.Keys.front().Pressed);
	GYRO_CHECK_EQ(session.Observed.Keys.back().Code, std::uint32_t{ 56 });
	GYRO_CHECK(!session.Observed.Keys.back().Pressed);

	// And released once: a second leave has nothing left to put up.
	GYRO_REQUIRE(session.Host.SendKeyboardLeave().has_value());

	session.Turn();

	GYRO_CHECK_EQ(session.Observed.Keys.size(), std::size_t{ 2 });
}

GYRO_TEST(NestedInput, ASeatThatLosesItsKeyboardPutsWhatWasHeldBackUp)
{
	Session session;

	GYRO_REQUIRE(session.Open().has_value());
	GYRO_REQUIRE(session.Host.SendKeyboardEnter({}).has_value());
	GYRO_REQUIRE(session.Host.SendKeyboardKey(56, 1).has_value());

	session.Turn();

	// The pointer capability alone. Nothing further will ever arrive on the keyboard, so the release
	// that would have come has to be manufactured here or never happen at all.
	GYRO_REQUIRE(session.Host.SendSeatCapabilities(0x1U).has_value());

	session.Turn();

	GYRO_REQUIRE_EQ(session.Observed.Keys.size(), std::size_t{ 2 });
	GYRO_CHECK(!session.Observed.Keys.back().Pressed);
	GYRO_CHECK_EQ(session.Observed.Keys.back().Code, std::uint32_t{ 56 });
}

GYRO_TEST(NestedInput, AKeymapIsTakenAndDropped)
{
	Session session;

	GYRO_REQUIRE(session.Open().has_value());

	// A real descriptor on the wire, which the client owns the moment it reads the message. What is
	// checked is that it goes nowhere: gyro compiles its own keymap from `XKB_DEFAULT_*` and a host's
	// layout has no way into `Seam/Input.h`, which carries keycodes on purpose.
	GYRO_REQUIRE(session.Host.SendKeyboardKeymap().has_value());
	GYRO_REQUIRE(session.Host.SendKeyboardRepeatInfo(25, 600).has_value());
	GYRO_REQUIRE(session.Host.SendKeyboardModifiers(0x1U).has_value());
	GYRO_REQUIRE(session.Host.SendKeyboardEnter({}).has_value());
	GYRO_REQUIRE(session.Host.SendKeyboardKey(30, 1).has_value());

	session.Turn();

	// The key still arrives, and none of the three events before it turned into one.
	GYRO_REQUIRE_EQ(session.Observed.Keys.size(), std::size_t{ 1 });
	GYRO_CHECK_EQ(session.Observed.Keys.front().Code, std::uint32_t{ 30 });
	GYRO_CHECK_EQ(session.Host.Unhandled, std::uint32_t{ 0 });
}

GYRO_TEST(NestedInput, ARepeatedKeyIsNotASecondPress)
{
	Session session;

	GYRO_REQUIRE(session.Open().has_value());
	GYRO_REQUIRE(session.Host.SendKeyboardEnter({}).has_value());
	GYRO_REQUIRE(session.Host.SendKeyboardKey(30, 1).has_value());

	// A host at version ten may send `repeated`. gyro's own clients repeat for themselves against
	// gyro's rate, so forwarding this would be a key held down twice as fast as the hand holding it.
	GYRO_REQUIRE(session.Host.SendKeyboardKey(30, 2).has_value());
	GYRO_REQUIRE(session.Host.SendKeyboardKey(30, 2).has_value());

	session.Turn();

	GYRO_CHECK_EQ(session.Observed.Keys.size(), std::size_t{ 1 });
}
