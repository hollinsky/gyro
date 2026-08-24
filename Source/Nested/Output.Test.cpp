#include "Nested/Output.h"

#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <print>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "Core/ColorState.h"
#include "Core/Result.h"
#include "Core/Signal.h"
#include "Geometry/Region.h"
#include "Geometry/Space.h"
#include "Nested/Peer.h"
#include "Seam/Allocator.h"
#include "Seam/Buffer.h"
#include "Seam/OutputConfiguration.h"
#include "Seam/Presenter.h"
#include "Seam/RenderTarget.h"
#include "Seam/Renderer.h"
#include "Seam/SyncPoint.h"
#include "Testing/Test.h"
#include "Wayland/Wayland.h"

// The presenter, against a host a test drives.
//
// **Every case here is one a real compositor cannot be asked to produce.** Discard a frame; hold
// every buffer at once; answer a configure with a size gyro did not ask for; offer only a modifier
// the device refuses; take a whole minute to release. Those are the paths that decide whether one
// window in a session freezes, whether a resize leaves a band of undefined pixels, and whether the
// frame clock predicts from a cadence with a hole in it — and none of them happens on a good day.
//
// **What is not faked is the wire.** The peer speaks gyro's own codec over a real `socketpair` with
// real descriptors crossing it, so an argument order reversed here shows up as a wrong value rather
// than as a test that agrees with the code. Wire/Connection.Test.cpp makes the same argument one
// module down.

namespace
{
using namespace Wayland;

// An allocator over `memfd`, which is `Virtual/Heap.h` with the module edge removed.
//
// `Nested` may not depend on `Virtual` — nothing in the graph joins them and nothing should — so the
// instrument is rebuilt here in twenty lines. What matters is that the descriptor is real, because it
// crosses `SCM_RIGHTS` to the peer and a test reads back what arrived.
//
// **It accepts linear and refuses everything else**, which is what keeps decision 138 an assertion
// rather than a description: the peer offers a tiled modifier first, the whole candidate set is
// handed over at once, and the ring still has to end up linear — because a provider's answer is
// bounded by what it can produce however the set is ordered.
class MemfdAllocator final : public IDmabufAllocator
{
public:
	[[nodiscard]] Result<DmabufBuffer>
	Allocate(PixelSize<DeviceSpace> size, std::uint32_t code, std::span<const std::uint64_t> modifiers) override
	{
		if (Refuse != 0)
		{
			return Failure(Refuse, "this allocator was told to refuse");
		}

		const PixelFormat format = FirstSupported(*this, code, modifiers);

		if (!format.IsValid())
		{
			return Failure(EINVAL, "this allocator does not tile");
		}

		const std::uint32_t stride = static_cast<std::uint32_t>(size.Width) * 4;
		const std::size_t bytes = std::size_t{ stride } * static_cast<std::size_t>(size.Height);

		const int backing = ::memfd_create("gyro-nested-test", MFD_CLOEXEC);

		if (backing < 0)
		{
			return Failure(errno, "creating a memfd for a nested target");
		}

		Fd descriptor{ backing };

		if (::ftruncate(descriptor.Get(), static_cast<::off_t>(bytes)) != 0)
		{
			return Failure(errno, "sizing a memfd for a nested target");
		}

		++Allocations;

		return DmabufBuffer{ std::move(descriptor), size, format, stride, Mapping{} };
	}

	[[nodiscard]] bool Supports(PixelFormat format) const noexcept override
	{
		return format.Code == FormatXrgb8888 && format.Modifier == ModifierLinear;
	}

	[[nodiscard]] std::string_view Name() const noexcept override { return "memfd"; }

	int Refuse = 0;
	std::uint64_t Allocations = 0;
};

// A renderer that does nothing but answer whether it has finished.
//
// The only verb a presenter ever calls on one is `IsComplete`, and only on the held-commit path — so
// this is the whole of what a nested output needs from the seam, and driving `Complete` by hand is
// how a composite that has not landed becomes a thing a test can hold still.
class Composite final : public IRenderer
{
public:
	[[nodiscard]] Result<void> BindTargets(std::span<const RenderTarget>, ColorState) override { return {}; }

	void ReleaseTargets() noexcept override {}

	[[nodiscard]] Result<Submission> Record(const RecordRequest&) override
	{
		return Failure(ENOSYS, "this renderer does not draw");
	}

	[[nodiscard]] bool IsComplete(SyncPoint point) const override { return point.IsImmediate() || Complete; }

	[[nodiscard]] std::size_t CollectCosts(std::span<GpuCost>) override { return 0; }

	bool Complete = false;
};

// What a test hears from the seam. Signals are intra-thread and everything here runs on one, which is
// the same arrangement `FrameOutput` has.
class Observer
{
public:
	void Watch(IPresenter& presenter)
	{
		m_OnPresented.ConnectTo<&Observer::OnPresented>(presenter.Presented, *this);
		m_OnMissed.ConnectTo<&Observer::OnMissed>(presenter.Missed, *this);
		m_OnReconfigured.ConnectTo<&Observer::OnReconfigured>(presenter.Reconfigured, *this);
		m_OnInvalidated.ConnectTo<&Observer::OnInvalidated>(presenter.TargetsInvalidated, *this);
	}

	std::vector<PresentationInfo> Presented;
	std::vector<OutputConfiguration> Reconfigured;
	std::uint64_t Missed = 0;
	std::uint64_t Invalidated = 0;

	// The order the two transition signals arrived in, which is the half Seam/Presenter.h fixes and
	// Frame/Loop.h leans on: the images go before the new configuration exists.
	std::vector<int> Order;

private:
	void OnPresented(const PresentationInfo& info) { Presented.push_back(info); }

	void OnMissed() { ++Missed; }

	void OnReconfigured(const OutputConfiguration& achieved)
	{
		Reconfigured.push_back(achieved);
		Order.push_back(1);
	}

	void OnInvalidated()
	{
		++Invalidated;
		Order.push_back(0);
	}

	Connection<const PresentationInfo&> m_OnPresented;
	Connection<> m_OnMissed;
	Connection<const OutputConfiguration&> m_OnReconfigured;
	Connection<> m_OnInvalidated;
};

constexpr PixelSize<DeviceSpace> Extent{ 640, 360 };

[[nodiscard]] OutputConfiguration Panel()
{
	return OutputConfiguration{ .Generation = 1,
		                        .Resolution = Extent,
		                        .Period = std::chrono::nanoseconds{ 16'666'666 },
		                        .Format = PixelFormat{ FormatXrgb8888, 0, ModifierLinear } };
}

// The host, the connection, the allocator and one window, brought up together and driven from one
// thread afterwards.
struct Session
{
	Nested::Peer Host;
	Nested::NestedHost Client;
	MemfdAllocator Allocator;
	Composite Renderer;
	Observer Watcher;
	std::unique_ptr<Nested::NestedOutput> Window;

	[[nodiscard]] Result<void> Open(Nested::NestedOutputPolicy policy = {})
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

		Window = std::make_unique<Nested::NestedOutput>(Client, Allocator, &Renderer, Panel(), std::move(policy));

		Result<void> built{};
		Nested::PumpWhile(Host, [&] { built = Window->Open(); });

		Watcher.Watch(*Window);

		return built;
	}

	// One turn of the loop's drain, with the host answering first — which is the order Frame/Loop.h
	// calls a correctness property.
	void Turn()
	{
		(void)Host.Pump();
		(void)Client.Drain();
		(void)Host.Pump();
	}

	// Acquire, present a whole-output frame, and let it reach the wire.
	[[nodiscard]] std::optional<std::uint32_t> Frame(SyncPoint acquire = SyncPoint::Immediate())
	{
		const std::optional<std::uint32_t> target = Window->AcquireTarget();

		if (!target)
		{
			return std::nullopt;
		}

		Region<DeviceSpace> damage;
		damage.Add(PixelRect<DeviceSpace>{ {}, Extent });

		const PresentLayer layer{ .Target = *target,
			                      .Blend = BlendMode::Opaque,
			                      .Acquire = acquire,
			                      .Source = {},
			                      .Destination = { {}, Extent },
			                      .Damage = damage,
			                      .Color = ColorState::Srgb() };

		if (!Window->Present({ &layer, 1 }))
		{
			return std::nullopt;
		}

		(void)Host.Pump();

		return target;
	}
};
} // namespace

GYRO_TEST(NestedOutput, AWindowComesUpUnderTheFirstModifierBothEndsAccept)
{
	Session session;

	GYRO_REQUIRE(session.Open().has_value());
	GYRO_REQUIRE(session.Window->Status().has_value());

	// Decision 120's negotiation, end to end. The peer offers a tiled modifier first because a real
	// host puts its best band first; this allocator will only take linear; and the ring is linear
	// without either end having been told the answer.
	GYRO_CHECK_EQ(session.Window->Configuration().Format.Modifier, ModifierLinear);
	GYRO_CHECK_EQ(session.Window->Configuration().Format.Code, FormatXrgb8888);

	// **What was achieved rather than what was asked for**, which is how a request the hardware could
	// not honour reports itself. Here they agree; the test that they *can* disagree is the resize
	// below.
	GYRO_CHECK_EQ(session.Window->Configuration().Resolution, Extent);

	const std::span<const RenderTarget> targets = session.Window->Targets();

	GYRO_REQUIRE_EQ(targets.size(), std::size_t{ Nested::DefaultNestedTargets });
	GYRO_CHECK(targets[0].IsValid());
	GYRO_CHECK_EQ(targets[0].Size, Extent);

	// And each one crossed to the host as a real `wl_buffer` over a real descriptor.
	GYRO_REQUIRE_EQ(session.Host.Buffers.size(), std::size_t{ Nested::DefaultNestedTargets });

	for (const Nested::PeerBuffer& buffer : session.Host.Buffers)
	{
		GYRO_CHECK_EQ(buffer.Width, Extent.Width);
		GYRO_CHECK_EQ(buffer.Height, Extent.Height);
		GYRO_CHECK_EQ(buffer.Format, FormatXrgb8888);
		GYRO_CHECK_EQ(buffer.Modifier, ModifierLinear);
		GYRO_REQUIRE_EQ(buffer.Planes.size(), std::size_t{ 1 });
		GYRO_CHECK(buffer.Planes[0].IsValid());
	}

	GYRO_CHECK_EQ(session.Host.Unhandled, std::uint64_t{ 0 });
}

GYRO_TEST(NestedOutput, AHostThatOffersNothingUsableLeavesTheTargetSetEmpty)
{
	// A host that will import only `NV12`. The window still exists — the surface is created and the
	// role taken — and it has no images, which is the state the frame loop already tolerates between
	// `TargetsInvalidated` and `Reconfigured`. `Status()` is where the reason is, rather than a crash
	// at startup on somebody's unusual desktop.
	Session session;
	session.Host.Offered = { PixelFormat{ FormatNv12, 0, ModifierLinear } };

	const Result<void> opened = session.Open();

	GYRO_REQUIRE(!opened.has_value());
	GYRO_CHECK_EQ(opened.error().Code(), ENOTSUP);
	GYRO_CHECK(session.Window->Targets().empty());
}

GYRO_TEST(NestedOutput, PresentPutsAttachDamageFeedbackAndCommitOnTheWire)
{
	Session session;
	GYRO_REQUIRE(session.Open().has_value());

	const std::size_t before = session.Host.Commits.size();
	const std::optional<std::uint32_t> target = session.Frame();

	GYRO_REQUIRE(target.has_value());
	GYRO_REQUIRE_EQ(session.Host.Commits.size(), before + 1);

	const Nested::PeerCommit& commit = session.Host.Commits.back();

	GYRO_CHECK_EQ(commit.Surface, session.Host.Surface());
	GYRO_CHECK(commit.Buffer != Wire::ObjectId::None);

	// **Damage in the buffer's own coordinates**, which is what `damage_buffer` takes and why
	// Geometry/Region.h is templated on the space — mapping a device rectangle through a surface
	// transform would round in a backend rather than at the seam.
	GYRO_REQUIRE_EQ(commit.Damage.size(), std::size_t{ 1 });
	GYRO_CHECK_EQ(commit.Damage[0].Extent, Extent);

	// A feedback object per commit, which is the clock's only input.
	GYRO_CHECK(commit.Feedback != Wire::ObjectId::None);

	// **Flushed inside `Present` rather than left to the next drain**, which is what this whole
	// assertion rests on: nothing has been drained since, and the frame is already on the host's side
	// of the socket. Left buffered it would sit there across the wait the loop is about to enter — for
	// the feedback that would never come.
	GYRO_CHECK_EQ(session.Window->Commits, std::uint64_t{ 1 });
}

GYRO_TEST(NestedOutput, MoreThanOneLayerIsRefusedRatherThanPartlyDrawn)
{
	Session session;
	GYRO_REQUIRE(session.Open().has_value());

	const std::optional<std::uint32_t> target = session.Window->AcquireTarget();
	GYRO_REQUIRE(target.has_value());

	PresentLayer layer{};
	layer.Target = *target;
	layer.Destination = { {}, Extent };

	const PresentLayer pair[2] = { layer, layer };

	const Result<void> refused = session.Window->Present(pair);

	GYRO_REQUIRE(!refused.has_value());
	GYRO_CHECK_EQ(refused.error().Code(), EINVAL);

	// Nothing partial went out. An assigner that produced two layers would otherwise be silently
	// ignored on this backend and discovered on the one with planes.
	GYRO_CHECK_EQ(session.Window->Commits, std::uint64_t{ 0 });
}

GYRO_TEST(NestedOutput, PresentedCarriesTheHostsOwnCadenceIntoTheTimebase)
{
	Session session;
	GYRO_REQUIRE(session.Open().has_value());
	GYRO_REQUIRE(session.Frame().has_value());

	GYRO_REQUIRE(session.Host.SendPresented(0, 3'000'000'123ULL, 16'666'666, 4210).has_value());
	session.Turn();

	GYRO_REQUIRE_EQ(session.Watcher.Presented.size(), std::size_t{ 1 });

	const PresentationInfo& info = session.Watcher.Presented.front();

	// Decision 57's conversion, at the one place a nested timestamp enters gyro: seconds and
	// nanoseconds are separate on the wire because the protocol predates 64-bit arguments, and nothing
	// downstream sees either again.
	GYRO_CHECK_EQ(Monotonic::ToNanoseconds(info.PresentedAt), std::int64_t{ 3'000'000'123 });

	// The interval the output *actually ran at*, which is what the servo learns from — never what gyro
	// asked for.
	GYRO_CHECK_EQ(info.Period, Duration{ 16'666'666 });
	GYRO_CHECK_EQ(info.Sequence, std::uint64_t{ 4210 });
	GYRO_CHECK(info.Vsync);

	// The host said its timestamp came from display hardware *and* it is quoting CLOCK_MONOTONIC, so
	// the clock may call the prediction precise. Either half missing and it may not.
	GYRO_CHECK(info.HardwareClock);
}

GYRO_TEST(NestedOutput, ADiscardedFrameIsAMissRatherThanSilence)
{
	// The case Seam/Presenter.h's fourth signal exists for. The commit was accepted, so the loop has
	// marked this output flip-pending and will not serve it again; a host that drops the frame and says
	// nothing would leave this window dark for good while the others carry on.
	Session session;
	GYRO_REQUIRE(session.Open().has_value());
	GYRO_REQUIRE(session.Frame().has_value());

	GYRO_REQUIRE(session.Host.SendDiscarded(0).has_value());
	session.Turn();

	GYRO_CHECK_EQ(session.Watcher.Missed, std::uint64_t{ 1 });
	GYRO_CHECK(session.Watcher.Presented.empty());
	GYRO_CHECK_EQ(session.Window->Discarded, std::uint64_t{ 1 });

	// **And the buffer is still the host's**, which is the difference between this and a failed
	// commit: the frame was never shown and the image is still out on loan. A presenter that took it
	// back here would hand the renderer a target the host is reading.
	GYRO_CHECK_EQ(session.Window->Targets().size(), std::size_t{ Nested::DefaultNestedTargets });
}

GYRO_TEST(NestedOutput, ARingTheHostIsHoldingStallsRatherThanReusingAnImage)
{
	Session session;
	GYRO_REQUIRE(session.Open().has_value());

	std::vector<std::uint32_t> committed;

	for (std::uint32_t frame = 0; frame < Nested::DefaultNestedTargets; ++frame)
	{
		const std::optional<std::uint32_t> target = session.Frame();

		GYRO_REQUIRE(target.has_value());
		committed.push_back(*target);

		// Presented, so the target moves from committed to held — which is where a real host leaves it
		// until it has finished reading.
		GYRO_REQUIRE(session.Host.SendPresented(frame, 1'000'000ULL * (frame + 1), 16'666'666, frame).has_value());
		session.Turn();
	}

	// Every image is out on loan. Seam/Presenter.h calls this an ordinary answer rather than a
	// failure: the frame loop skips this output, because waiting here would put another process's flow
	// control on gyro's frame thread.
	GYRO_CHECK(!session.Window->AcquireTarget().has_value());

	// One back, and exactly that one.
	GYRO_REQUIRE(session.Host.SendRelease(session.Host.Buffers[committed.front()].Id).has_value());
	session.Turn();

	const std::optional<std::uint32_t> reacquired = session.Window->AcquireTarget();

	GYRO_REQUIRE(reacquired.has_value());
	GYRO_CHECK_EQ(*reacquired, committed.front());
}

GYRO_TEST(NestedOutput, AResizeReleasesTheImagesAndThenReconfigures)
{
	// Docs/Architecture.md#nested-wayland's second deliberate feature: a window resize *is* a mode
	// change. Building it now is what makes real hotplug the same path rather than a rewrite — and it
	// is exercised every time somebody drags the corner of the window they are developing in.
	Session session;
	GYRO_REQUIRE(session.Open().has_value());

	const std::uint64_t generation = session.Window->Configuration().Generation;
	const std::uint64_t allocations = session.Allocator.Allocations;

	GYRO_REQUIRE(session.Host.SendConfigure(800, 600).has_value());
	session.Turn();

	// **The images go first and the transition completes second**, which is the order Seam/Presenter.h
	// fixes and Frame/Loop.h leans on: the whole-output damage `TargetsInvalidated` accumulates is in
	// the extent the output *had*, so a mode that grew is re-damaged after the adoption rather than
	// before it.
	GYRO_REQUIRE_EQ(session.Watcher.Order.size(), std::size_t{ 2 });
	GYRO_CHECK_EQ(session.Watcher.Order[0], 0);
	GYRO_CHECK_EQ(session.Watcher.Order[1], 1);

	GYRO_REQUIRE_EQ(session.Watcher.Reconfigured.size(), std::size_t{ 1 });

	const OutputConfiguration& achieved = session.Watcher.Reconfigured.front();

	GYRO_CHECK_EQ(achieved.Resolution, (PixelSize<DeviceSpace>{ 800, 600 }));

	// The generation moves, so a completion is legible as *this* one rather than a superseded one —
	// which is what it is for on a panel and what makes the two paths one path.
	GYRO_CHECK(achieved.Generation > generation);

	// A whole new ring, at the new extent.
	GYRO_CHECK_EQ(session.Allocator.Allocations, allocations + Nested::DefaultNestedTargets);
	GYRO_REQUIRE_EQ(session.Window->Targets().size(), std::size_t{ Nested::DefaultNestedTargets });
	GYRO_CHECK_EQ(session.Window->Targets()[0].Size, (PixelSize<DeviceSpace>{ 800, 600 }));

	// And the configure was acknowledged, which is what says gyro read the whole of it.
	GYRO_CHECK_EQ(session.Host.Acked, session.Host.Serial);
}

GYRO_TEST(NestedOutput, AConfigureThatChangesNothingIsNotAModeChange)
{
	// A host re-configures for reasons that are not a resize — a focus change, a tiling state, a
	// maximize that was already in effect. Treating each of those as a mode change would release and
	// rebuild the whole ring, which on a busy desktop is a stutter with no cause anybody could find.
	Session session;
	GYRO_REQUIRE(session.Open().has_value());

	const std::uint64_t allocations = session.Allocator.Allocations;

	GYRO_REQUIRE(session.Host.SendConfigure(Extent.Width, Extent.Height).has_value());
	session.Turn();

	// And zero, which is the host saying *you choose* — the ordinary configure a floating window
	// manager sends and the one that lets `--output` decide.
	GYRO_REQUIRE(session.Host.SendConfigure(0, 0).has_value());
	session.Turn();

	GYRO_CHECK_EQ(session.Watcher.Invalidated, std::uint64_t{ 0 });
	GYRO_CHECK(session.Watcher.Reconfigured.empty());
	GYRO_CHECK_EQ(session.Allocator.Allocations, allocations);

	// Acknowledged all the same: an unanswered configure is a host that thinks gyro has stopped
	// reading.
	GYRO_CHECK_EQ(session.Host.Acked, session.Host.Serial);
}

GYRO_TEST(NestedOutput, WithoutExplicitSyncACommitWaitsForTheComposite)
{
	// The fallback path, and its whole cost in one test. A host with no `wp_linux_drm_syncobj_v1`
	// cannot be handed a point the GPU has not reached, so the commit is *held* — never waited on
	// inline, which is what `Present` may never block means — and released by a later drain once
	// `IRenderer::IsComplete` says the pixels exist.
	Session session;
	session.Host.Globals = { Nested::PeerGlobal{ WlCompositor::WireName, 6 },
		                     Nested::PeerGlobal{ XdgWmBase::WireName, 6 },
		                     Nested::PeerGlobal{ ZwpLinuxDmabufV1::WireName, 5 },
		                     Nested::PeerGlobal{ WpPresentation::WireName, 1 } };

	GYRO_REQUIRE(session.Open().has_value());
	GYRO_REQUIRE(!session.Client.HasExplicitSync());

	const std::size_t before = session.Host.Commits.size();

	// A point on a timeline that has not been reached. The descriptor is any valid one — what makes it
	// a wait is that it is not immediate and the renderer says no.
	session.Renderer.Complete = false;

	const std::optional<std::uint32_t> target = session.Frame(SyncPoint{ session.Client.Connection().Descriptor(), 7 });

	GYRO_REQUIRE(target.has_value());

	// Nothing on the wire yet, and `Present` returned all the same: the frame loop is free to go on
	// doing other outputs.
	GYRO_CHECK_EQ(session.Host.Commits.size(), before);
	GYRO_CHECK_EQ(session.Window->Held(), std::uint64_t{ 1 });

	// And it asks to be looked at again, because no file becomes readable when a GPU finishes. That
	// instant is the whole of what this path costs and is why the log says its pacing figures are not
	// to be trusted.
	GYRO_CHECK(session.Window->NextEvent() != Instant{ Duration::max() });

	// A drain that finds the composite still outstanding changes nothing.
	session.Turn();
	GYRO_CHECK_EQ(session.Host.Commits.size(), before);

	session.Renderer.Complete = true;
	session.Turn();

	GYRO_REQUIRE_EQ(session.Host.Commits.size(), before + 1);
	GYRO_CHECK(session.Host.Commits.back().Buffer != Wire::ObjectId::None);

	// Quiet again, so an idle session arms nothing.
	GYRO_CHECK(session.Window->NextEvent() == Instant{ Duration::max() });

	// Neither point was named, because there is no surface role to name one on.
	GYRO_CHECK(!session.Host.Commits.back().HasAcquire);
	GYRO_CHECK(!session.Host.IsExplicitlySynchronized());
}

GYRO_TEST(NestedOutput, WithExplicitSyncBothPointsAreNamedOrNeitherIs)
{
	// `wp_linux_drm_syncobj_v1` refuses a commit that sets an acquire point and no release point, and
	// refuses one that attaches a buffer with no acquire point at all — so the role is taken on the
	// first non-immediate point and from then on *every* commit names both. Half of that pair is the
	// end of the connection, which on a nested session is every window going away at once.
	Session session;
	GYRO_REQUIRE(session.Open().has_value());

	if (!session.Client.HasExplicitSync())
	{
		// No DRM node to mint release timelines on, which is the container case. The path this test is
		// about does not exist there, and the fallback it drops to has a test of its own above.
		std::println("  skipped NestedOutput.WithExplicitSyncBothPointsAreNamedOrNeitherIs: no DRM node");

		return;
	}

	// An immediate point first. A device that finishes inside `Record` never takes the role at all,
	// which is what keeps the floor tier off this path entirely.
	GYRO_REQUIRE(session.Frame().has_value());
	GYRO_CHECK(!session.Host.IsExplicitlySynchronized());
	GYRO_CHECK(!session.Host.Commits.back().HasAcquire);
	GYRO_REQUIRE(session.Host.SendPresented(0, 1'000'000, 16'666'666, 0).has_value());
	session.Turn();

	// And now one the GPU has not reached. The commit goes out *immediately* naming it, which is the
	// whole point: the host plans its own frame around gyro's rather than learning about it late.
	session.Renderer.Complete = false;

	const std::size_t before = session.Host.Commits.size();
	GYRO_REQUIRE(session.Frame(SyncPoint{ session.Client.Connection().Descriptor(), 7 }).has_value());

	GYRO_REQUIRE_EQ(session.Host.Commits.size(), before + 1);
	GYRO_CHECK_EQ(session.Window->Held(), std::uint64_t{ 0 });

	const Nested::PeerCommit& commit = session.Host.Commits.back();

	GYRO_CHECK(session.Host.IsExplicitlySynchronized());
	GYRO_CHECK(commit.HasAcquire);
	GYRO_CHECK_EQ(commit.Acquire, std::uint64_t{ 7 });

	// Both or neither. The release point is on this target's own timeline, monotone, and the first one
	// named on it.
	GYRO_CHECK(commit.HasRelease);
	GYRO_CHECK_EQ(commit.Release, std::uint64_t{ 1 });

	// The image is not free until the host signals that point, and this peer never does — so the
	// target stays out on loan rather than coming back because a `wl_buffer.release` happened to
	// arrive. Two images left, which is the ring under one frame of pressure.
	GYRO_REQUIRE(session.Host.SendPresented(before, 2'000'000, 16'666'666, 1).has_value());
	session.Turn();

	GYRO_CHECK(session.Window->AcquireTarget().has_value());
}

GYRO_TEST(NestedOutput, APingIsAnsweredWhileTheOutputIsIdle)
{
	// `xdg_wm_base.ping` is the one event on this connection that is a liveness check rather than
	// information, and the answer has to leave *while nothing is being drawn* — which is exactly when
	// a flush left to the next `Present` would never happen, and when a desktop would offer to kill
	// gyro for being unresponsive.
	Session session;
	GYRO_REQUIRE(session.Open().has_value());

	GYRO_REQUIRE(session.Host.SendPing(99).has_value());
	session.Turn();

	GYRO_CHECK_EQ(session.Host.Pongs, std::uint64_t{ 1 });
}

GYRO_TEST(NestedOutput, ClosingTheWindowIsAnEndingRatherThanAFailure)
{
	// The ordinary way to quit a nested session. It is not an error, nothing fails, and the
	// composition root is what turns it into a shutdown — which is why it is a fact this answers
	// rather than a signal it emits.
	Session session;
	GYRO_REQUIRE(session.Open().has_value());

	GYRO_CHECK(!session.Window->IsClosed());

	GYRO_REQUIRE(session.Host.SendClose().has_value());
	session.Turn();

	GYRO_CHECK(session.Window->IsClosed());
	GYRO_CHECK(session.Client.Connection().Failed() == std::nullopt);
}

GYRO_TEST(NestedOutput, AnUnpoweredOutputRefusesToPresent)
{
	// Decision 73 puts power on the mode-setting verb rather than on the frame, and this is the half
	// that shows up on the frame side: a loop still committing to a dark output is a bug that
	// otherwise surfaces as battery life rather than as a picture.
	Session session;
	GYRO_REQUIRE(session.Open().has_value());

	OutputConfiguration off = session.Window->Configuration();
	off.Powered = false;
	off.Generation += 1;

	session.Window->Reconfigure(off);
	session.Turn();

	GYRO_REQUIRE_EQ(session.Watcher.Reconfigured.size(), std::size_t{ 1 });
	GYRO_CHECK(!session.Watcher.Reconfigured.front().Powered);
	GYRO_CHECK(session.Window->Targets().empty());

	PresentLayer layer{};
	layer.Destination = { {}, Extent };

	const Result<void> refused = session.Window->Present({ &layer, 1 });

	GYRO_REQUIRE(!refused.has_value());
	GYRO_CHECK_EQ(refused.error().Code(), EINVAL);
}
