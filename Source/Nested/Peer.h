#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "Core/Fd.h"
#include "Core/Result.h"
#include "Seam/RenderTarget.h"
#include "Wire/Buffer.h"
#include "Wire/Message.h"
#include "Wire/Reader.h"

// The host compositor a test plays, on the far end of a `socketpair`.
//
// **It is an instrument in the module rather than a fixture beside one test**, which is
// Virtual/Heap.h's argument applied to a different absence. That file exists because
// `/dev/udmabuf` is not reachable in a container and every rule *above* the allocator would
// otherwise be untestable there; this exists because a Wayland compositor is not reachable in CI,
// and every rule above the connection — how a configure becomes a mode change, what a discarded
// frame does to the loop, which target a release frees — would otherwise be untestable anywhere but
// somebody's desktop.
//
// **What it can do that a real host cannot.** Nested/Output.h's hard cases are all things no
// compositor can be asked to produce on demand: discard a frame, hold every buffer at once, answer a
// configure with a size gyro did not ask for, refuse a modifier, hang up mid-commit. A test drives
// each of them here in two lines. That is the same argument decision 119 makes for `Wire` being
// exercised by a peer the test wrote rather than by a real server.
//
// **It is not a compositor and must not grow into one.** It knows the requests a nested output
// actually sends and nothing else: enough to hand out ids, remember which interface each one is, and
// let a test speak an event back. An unknown opcode is recorded rather than answered, so a request
// this does not model shows up as a count in a test rather than as a peer that quietly agrees.
//
// **The codec is gyro's own, on both ends.** Wire/Connection.Test.cpp's argument: a second
// marshaller written to check the first is a second thing to keep in step, and the failure it would
// catch — an argument order reversed — is one both copies would share. What is checked here is the
// *protocol* rather than the codec, and the codec checks itself.

namespace Nested
{
// The interfaces this peer knows how to be. Which one an id is decides how its requests are read.
enum class PeerObject : std::uint8_t
{
	Unknown,
	Display,
	Registry,
	Callback,
	Compositor,
	Surface,
	Shell,
	XdgSurface,
	Toplevel,
	Dmabuf,
	Params,
	FeedbackObject,
	Buffer,
	Presentation,
	PresentationFeedback,
	SyncobjManager,
	SyncobjSurface,
	SyncobjTimeline,
	DecorationManager,
	Decoration,
};

// One global the peer advertises, and the version it claims.
struct PeerGlobal
{
	std::string_view Interface;
	std::uint32_t Version = 1;
};

// What a commit carried, recorded so a test can assert on the wire rather than on the presenter that
// wrote it.
struct PeerCommit
{
	Wire::ObjectId Surface = Wire::ObjectId::None;
	Wire::ObjectId Buffer = Wire::ObjectId::None;
	Wire::ObjectId Feedback = Wire::ObjectId::None;

	// The rectangles `damage_buffer` named, in the order they arrived.
	std::vector<PixelRect<DeviceSpace>> Damage;

	bool HasAcquire = false;
	bool HasRelease = false;
	std::uint64_t Acquire = 0;
	std::uint64_t Release = 0;
};

// One `wl_buffer` the peer built out of a `zwp_linux_buffer_params_v1`.
struct PeerBuffer
{
	Wire::ObjectId Id = Wire::ObjectId::None;
	std::int32_t Width = 0;
	std::int32_t Height = 0;
	std::uint32_t Format = 0;
	std::uint64_t Modifier = 0;

	// The descriptors the client passed, kept open so a test can prove they were real and so closing
	// them is the peer's business rather than a leak.
	std::vector<Fd> Planes;
};

class Peer
{
public:
	Peer() = default;

	// The pair, with the client end published through `WAYLAND_SOCKET` so `Wire::Connection::Open`
	// picks it up. Returns the descriptor the client will adopt.
	[[nodiscard]] Result<void> Open();

	// Read everything the client has sent and answer what this peer answers itself — a registry's
	// globals, a sync's callback, a configure for a new toplevel. Everything else is recorded for a
	// test to act on.
	[[nodiscard]] Result<void> Pump();

	// Push what this peer has queued. Called by every `Send*` below, so a test never has to remember.
	[[nodiscard]] Result<void> Flush();

	// What the peer advertises. Set before `Open`; the default is everything a nested output wants.
	std::vector<PeerGlobal> Globals;

	// The dmabuf feedback this peer answers with. `Open` fills an empty `Offered` with `DefaultOffer`,
	// so a host that genuinely offers nothing says so with the flag rather than by leaving a field
	// blank — which is the difference between *a test that did not set this* and *a host whose only
	// device went away*, and only one of those is worth writing a test about.
	std::uint64_t MainDevice = 0xe280;
	std::vector<PixelFormat> Offered;
	bool OfferNothing = false;

	// The first configure's size. Zero is *you choose*, which is what a floating window manager sends
	// and what lets gyro pick `--output`.
	std::int32_t ConfigureWidth = 0;
	std::int32_t ConfigureHeight = 0;

	// Everything the client has done, in order.
	std::vector<PeerCommit> Commits;
	std::vector<PeerBuffer> Buffers;

	// Requests this peer has no case for. A test asserts it is zero, which is what turns *the peer did
	// not model that* into a failure rather than into a silent agreement.
	std::uint64_t Unhandled = 0;

	// Whether the client ever set a syncobj role on its surface.
	[[nodiscard]] bool IsExplicitlySynchronized() const noexcept { return m_SyncSurface != Wire::ObjectId::None; }

	// Events, each one the thing a test is actually about.
	[[nodiscard]] Result<void> SendConfigure(std::int32_t width, std::int32_t height);

	[[nodiscard]] Result<void>
	SendPresented(std::size_t commit, std::uint64_t nanoseconds, std::uint32_t refresh, std::uint64_t sequence);

	[[nodiscard]] Result<void> SendDiscarded(std::size_t commit);

	[[nodiscard]] Result<void> SendRelease(Wire::ObjectId buffer);

	[[nodiscard]] Result<void> SendPing(std::uint32_t serial);

	[[nodiscard]] Result<void> SendClose();

	// The last thing a host does. `Drain` on the client answers `EPIPE`, which is what
	// `IBackend::IsFinished` reads.
	void HangUp() noexcept;

	// How many pongs came back, so a test can say the liveness check is answered rather than assume it.
	std::uint64_t Pongs = 0;

	// The last serial this peer sent, and whether the client acknowledged it.
	std::uint32_t Serial = 0;
	std::uint32_t Acked = 0;

	[[nodiscard]] Wire::ObjectId Surface() const noexcept { return m_Surface; }

	[[nodiscard]] Wire::ObjectId Toplevel() const noexcept { return m_Toplevel; }

private:
	void Bind(Wire::ObjectId id, PeerObject kind);

	[[nodiscard]] PeerObject KindOf(Wire::ObjectId id) const noexcept;

	void Dispatch(Wire::MessageHeader header, Wire::MessageReader& reader);

	void DispatchDisplay(Wire::MessageReader& reader);

	void DispatchRegistry(Wire::MessageReader& reader);

	void DispatchDmabuf(Wire::ObjectId target, Wire::MessageReader& reader);

	void DispatchParams(Wire::ObjectId target, Wire::MessageReader& reader);

	void DispatchSurface(Wire::ObjectId target, Wire::MessageReader& reader);

	void DispatchSyncobj(Wire::ObjectId target, Wire::MessageReader& reader);

	// The feedback conversation, sent whole because the protocol says it is only coherent between
	// `done` events.
	void SendFeedback(Wire::ObjectId feedback);

	// The commit currently being assembled — attach, damage and the sync points arrive as separate
	// requests and become one thing at `commit`, which is what double-buffered state means.
	PeerCommit m_Building;

	Fd m_Socket;
	Fd m_Table;

	Wire::OutputBuffer m_Out;
	Wire::InputBuffer m_In;

	std::vector<std::pair<Wire::ObjectId, PeerObject>> m_Objects;

	Wire::ObjectId m_Registry = Wire::ObjectId::None;
	Wire::ObjectId m_Surface = Wire::ObjectId::None;
	Wire::ObjectId m_Toplevel = Wire::ObjectId::None;
	Wire::ObjectId m_XdgSurface = Wire::ObjectId::None;
	Wire::ObjectId m_SyncSurface = Wire::ObjectId::None;

	// Whether the window's first configure has gone out. The protocol has exactly one of these before
	// a buffer may be attached, and sending a second unprompted would be a resize the client never
	// asked for — which is a test's to send, not this peer's to volunteer.
	bool m_Configured = false;

	std::uint32_t m_Name = 1;
};

// Run the peer on a thread of its own for as long as `work` takes, then hand it back.
//
// **It exists because two of the client's calls block and this peer is what unblocks them.**
// `NestedHost::Open` roundtrips twice and `NestedOutput::Open` once more, each waiting on an answer
// only the far end can give — which is the real arrangement with both ends in one process. Outside
// the window the peer is pumped synchronously by whoever is driving, so a test reads `Commits` and
// `Buffers` with nothing else touching them.
//
// A final synchronous pump after the join, so that anything the client sent as its last act is read
// before the caller looks.
template<typename Work>
void PumpWhile(Peer& peer, Work&& work)
{
	{
		std::jthread pumping{ [&peer](std::stop_token stop) {
			while (!stop.stop_requested())
			{
				(void)peer.Pump();
				std::this_thread::sleep_for(std::chrono::microseconds{ 200 });
			}
		} };

		work();
	}

	(void)peer.Pump();
}

// The formats a peer offers by default: linear `XR24` and a made-up tiled modifier ahead of it, so
// that *the host ranked and the device vetoed* is exercised rather than assumed. The tiled one is
// first because a real host puts its best band first, and no software device will take it.
[[nodiscard]] std::vector<PixelFormat> DefaultOffer();
} // namespace Nested
