#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "Core/Fd.h"
#include "Core/Result.h"
#include "Seam/EventSource.h"
#include "Wire/Buffer.h"
#include "Wire/Message.h"
#include "Wire/Reader.h"

// A client connection to a Wayland host: the socket, the two buffers, and the table that says which
// object an arriving event belongs to.
//
// **It is an `IEventSource` rather than something a loop knows how to poll specially**, which is what
// lets decision 81's arrangement work at all. A nested output's presentation feedback arrives on this
// socket and is frame-side, so the connection is registered alongside a DRM device's descriptor and a
// simulated vblank and drained by the same loop, in the same order, with the same "drain before
// evaluating" ordering Frame/Loop.h calls a correctness property. Seam/EventSource.h's contract is
// exactly the one a socket wants: `Drain()` reads to empty, and finding nothing is success.
//
// **The runtime answers `wl_display` itself, and that is the one piece of protocol knowledge here.**
// Its two events are `error` and `delete_id`, and both are the runtime's business rather than a
// proxy's: an id may not be reused until the host says it is free, so recycling has to happen below
// whatever allocates, and a protocol error is the end of the connection rather than an event some
// object handles. Everything else — every interface, every opcode, every argument order — comes from
// generated code calling the accessors below, which is decision 2's split.
//
// **A protocol error's text does not fit in a `Core/Result.h` failure, and that is deliberate on
// Result's side rather than a gap here.** An `Error` holds a `string_view` over a literal with static
// storage, so it cannot carry a string the host just sent; `Drain()` reports `EPROTO` with a fixed
// context and parks the failing object, code, and message in `Fault()` for whatever logs it. Copying
// the host's message into an owned string on the failure path is the one allocation this module makes
// that is not a buffer growth, and the connection is already dead when it happens.
//
// **One thread pumps one connection for the whole of its life**, per decision 81 applied per object.
// Nothing here locks, and nothing here is safe to call from two threads. A dispatcher must not call
// `Drain` reentrantly — it may marshal requests, bind, and unbind freely, because those touch the
// output buffer and the object table rather than the receive buffer being walked.

namespace Wire
{
// What a bound object does with an event. A plain function pointer with a `void*` beside it rather
// than a `std::function`, because the generated dispatcher is a captureless `switch` and this is what
// a proxy costs: two pointers, no allocation, no indirect call through a type-erased wrapper.
//
// **`self` may be null, and a dispatcher must read the message anyway.** That is the whole handling
// of an event for an object the client has already destroyed. Both ends talk at once, so the host
// routinely has an event for an object on the wire before it has read the request destroying it —
// which means a stale event is the ordinary case rather than a host misbehaving. It cannot simply be
// skipped: descriptors travel out of band and are taken from a queue in the order messages consume
// them, so a message whose arguments are never read leaves that queue one entry out of step and the
// *next* message takes a descriptor belonging to the one thrown away. Nothing detects that — it is a
// valid descriptor for the wrong buffer.
//
// So the runtime keeps the dispatcher when a proxy is unbound and calls it with a null `self`. The
// generated code reads its arguments exactly as it always does — which is what consumes the
// descriptors and closes them — and guards only the call into the proxy:
//
//     const std::uint32_t name = reader.GetUint();
//     const std::string_view interface = reader.GetString();
//
//     if (self != nullptr)
//     {
//         static_cast<Registry*>(self)->Global(name, interface);
//     }
//
// Not a second generated function that reads and discards: two decoders for one interface can drift
// apart when an argument is added to one of them, and the symptom of that is the misaligned
// descriptor queue this exists to prevent. One decoder and one branch cannot drift.
using Dispatcher = void (*)(void* self, std::uint16_t opcode, MessageReader& reader);

// What the host said before it hung up. Held by the connection because `Error` cannot own the text.
struct ProtocolFault
{
	ObjectId Object = ObjectId::None;
	std::uint32_t Code = 0;
	std::string Message;
};

class Connection final : public IEventSource
{
public:
	Connection() = default;

	~Connection() override = default;

	// Connects to the host the environment names.
	//
	// `WAYLAND_SOCKET` first and it wins outright: a client launched by something that already opened
	// the socket inherits the descriptor rather than the path, which is how a sandboxed client reaches
	// a compositor whose socket it has no permission to open. It is unset once read — every client
	// does this, and the reason is that a descriptor number is not a thing to inherit twice: a child
	// spawned afterwards would find the variable naming a descriptor that is either closed or,
	// worse, reused for something else entirely.
	//
	// Otherwise `$XDG_RUNTIME_DIR/$WAYLAND_DISPLAY`, defaulting to `wayland-0`, with an absolute
	// `WAYLAND_DISPLAY` taken as the whole path.
	[[nodiscard]] Result<void> Open();

	// Seam/EventSource.h's two verbs.
	[[nodiscard]] RawFd Descriptor() const noexcept override { return m_Socket.Borrow(); }

	// Reads and demarshals until the socket has nothing left. Finding nothing is success; the host
	// closing the connection is not, and comes back as `EPIPE`.
	[[nodiscard]] Result<void> Drain() override;

	// Hands the buffered requests to the kernel. A partial write and `EAGAIN` are the ordinary
	// results of a host that has not read yet, not failures: what did not go stays buffered and the
	// next call carries it.
	[[nodiscard]] Result<void> Flush();

	// The next unused client id, reserved but not yet bound. `ObjectId::None` where the id space is
	// exhausted, which takes four billion live objects and is here so the caller has something to
	// check rather than a wrapped id.
	//
	// **Bind before marshalling the id.** Not enforceable here — the request that names a new id is
	// generated code and this module never sees it — but it is what lets `Unbind` tell an id the host
	// has never heard of from one it may still be sending events for, and every client in the tree
	// already writes it this way.
	[[nodiscard]] ObjectId Allocate();

	// Size the object table for this many client ids up front.
	//
	// **Because a connection whose ids are allocated on the frame path must not grow there.** A nested
	// output asks for a `wp_presentation_feedback` object per commit, and `Present` runs inside
	// Core/FrameSection.h's guard, where an allocation is an abort rather than a hiccup. In the steady
	// state nothing grows — the host destroys each feedback object as soon as it has spoken and the id
	// comes back through `delete_id` — but the first frames of a session climb to the high-water mark
	// and there is no reason to discover that from a stack trace. Sized once by whoever knows how many
	// windows and how deep a ring, which is the composition root.
	//
	// Capacity only: an id is still handed out by `Allocate` in the order it always was, and reserving
	// more than a connection ever uses costs eight bytes an id and nothing else.
	void Reserve(std::size_t ids);

	// Points an id at a proxy. A client id must have come from `Allocate` and not yet be bound; a
	// host-allocated id — one at or above `FirstServerId`, arriving as a `new_id` in an event — is
	// bound directly, because nothing on this side handed it out.
	[[nodiscard]] Result<void> Bind(ObjectId id, void* self, Dispatcher dispatch);

	// The proxy is gone. **The id is not free yet**, and that is the protocol rather than caution: a
	// client may not reuse an id until the host has sent `delete_id` for it, because the host may
	// still have events in flight naming the old object. The slot stays reserved until then, and its
	// dispatcher stays with it so those events can still be read — see `Dispatcher`.
	//
	// An id that was allocated and never bound is the one case that frees immediately: nothing on the
	// wire has ever named it, so there is no `delete_id` coming and nothing in flight to answer. That
	// rests on `Allocate`'s rule that a proxy is bound before its id is marshalled.
	void Unbind(ObjectId id) noexcept;

	// Where a `MessageWriter` marshals. Public because that is the whole relationship between the two
	// types, and named rather than made a friend so the round-trip test can drive a buffer with no
	// connection behind it at all.
	[[nodiscard]] OutputBuffer& Output() noexcept { return m_Out; }

	// What the host said, where `Drain` came back `EPROTO`. Empty otherwise.
	[[nodiscard]] const std::optional<ProtocolFault>& Fault() const noexcept { return m_Fault; }

	// The first failure the connection latched, if any. Once set, every verb returns it: a connection
	// that has seen a malformed message has lost track of where messages begin, and there is no
	// resynchronising a stream with no framing marks in it.
	[[nodiscard]] const std::optional<Error>& Failed() const noexcept { return m_Error; }

private:
	// What an id is, and the four states between "handed out" and "handed back" are all reachable.
	// `Deleted` is the one worth naming: the host frees an id as soon as it destroys the object, which
	// for a `wl_callback` is immediately after it sends `done` — so `delete_id` routinely arrives
	// while the proxy is still bound and the client destroys it a moment later.
	//
	// `Retired` keeps its dispatcher and loses its object, which is what makes an event arriving in
	// that window readable rather than fatal. Whether a slot can be dispatched to is therefore
	// `Dispatch != nullptr` rather than a state: `Free` and `Reserved` have never had one.
	enum class Slot : std::uint8_t
	{
		Free,
		Reserved,
		Live,
		Retired,
		Deleted,
	};

	struct Entry
	{
		void* Self = nullptr;
		Dispatcher Dispatch = nullptr;
		Slot State = Slot::Free;
	};

	[[nodiscard]] std::unexpected<Error> Fail(int code, std::string_view context);

	[[nodiscard]] Result<void> Adopt(Fd socket);

	// Every complete message in the receive buffer, dispatched.
	[[nodiscard]] Result<void> DispatchAvailable();

	[[nodiscard]] Result<void> DispatchDisplay(MessageReader& reader);

	[[nodiscard]] Entry* Find(ObjectId id) noexcept;

	// Returns rather than latching quietly: a `delete_id` for an id nobody holds is a host that has
	// lost track of the connection, and the caller is the one that turns it into the end of `Drain`.
	[[nodiscard]] Result<void> Recycle(ObjectId id);

	Fd m_Socket;
	OutputBuffer m_Out;
	InputBuffer m_In;

	// Indexed by id minus one, so slot zero is `wl_display` and is never consulted — the display is
	// answered before the table is reached.
	std::vector<Entry> m_Client;

	// Host-allocated ids, as pairs rather than a second flat table. The range starts at 0xff000000 and
	// an index into it is the id minus that, which a hostile or merely sparse host could make
	// enormous; the live count cannot be, because it is objects the host is actually holding. A linear
	// scan over the handful a nested backend ever has beats reserving sixteen million slots against
	// the possibility of one.
	std::vector<std::pair<ObjectId, Entry>> m_Server;

	std::vector<ObjectId> m_Recycled;
	std::uint32_t m_NextId = 2;

	std::optional<ProtocolFault> m_Fault;
	std::optional<Error> m_Error;
};
} // namespace Wire
