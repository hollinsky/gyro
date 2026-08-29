#pragma once

#include <atomic>
#include <cstdint>
#include <thread>
#include <type_traits>

// An observer callback, with both directions of lifetime made structural.
//
// Docs/Architecture.md's seam puts four of these on IPresenter and eight on IInput, and the first
// is the one that sets every constraint: `Signal<const PresentationInfo&> Presented` is how
// FrameClock::Observe learns that a frame reached glass, which is the sole input to the prediction
// every deadline in the system is derived from. It fires on the frame thread, inside the frame
// section, so **emit may not allocate** — Core/DebugAllocator.cpp aborts otherwise.
//
// **The constraint bites in a narrower place than it looks.** Invoking a std::function does not
// allocate; constructing one does, and that happens at connect time, which is off the frame path and
// free. What actually fails is the thing nearly every signal implementation does: copying the slot
// list before iterating it, so that a disconnect from inside a handler cannot invalidate the
// iterator. That copy is the allocation. So the reentrancy strategy is the real constraint and the
// callback storage is a consequence of it — which is why the cursor fix-up below exists and why
// there is no defensive copy anywhere in this file.
//
// **Lifetime is bidirectional, and both directions run on every boot.** Decision 41 has simpledrm
// bind the framebuffer and the real driver replace it, which means Render tears down whole while
// Frame keeps running at SCHED_FIFO and the last frame stays on glass. During that window:
//
//   The signal dies first. The old presenter is destroyed and its signals go with it, while the
//   output's FrameClock survives — it must, it is the thing being Invalidate()d and re-seeded.
//
//   The observer dies first. An output is unplugged and Frame's per-output state is destroyed, while
//   the session and device are still alive serving other outputs and still emitting.
//
// The second is exactly the failure decision 2 names as the best-known bug family in compositors
// built on wl_listener, and it survived as the strongest of that decision's arguments. Reproducing it
// here would be an unusually direct way to lose it.
//
// **Handle and SlotAllocator do not already solve this.** They solve a name outliving what it named,
// and they work because the name is resolved *through a live owner* — IsValid is answerable because
// the allocator is still there to be asked. A signal has no such third party. The presenter and the
// frame's per-output state are peers, and the only thing outliving both is the composition root;
// consulting the root on every emit would put the root on the frame path. So the two parties have to
// know about each other directly. That is forced, not chosen, and the only real question was where
// the mutual knowledge is stored.
//
// **It is stored in the observer.** A Connection is a member of the observing object, and connecting
// links two nodes that already exist. Nothing is allocated by connect, emit, or disconnect, so
// "could a disconnect land inside the frame section?" stops being a question rather than being
// answered — decision 36's argument for mechanical enforcement, applied one level up.
//
// **Rejected: a signal-owned heap node with a movable Connection handle.** Allows a bare capturing
// lambda as an observer, which reads better at the composition root, and emit still never allocates.
// Rejected because disconnect then frees, so an observer destroyed inside the frame section aborts.
// That is not reachable today — hotplug is a dispatch-thread udev event and the root sequences
// migration — but "currently unreachable" is the weaker kind of guarantee, and this codebase has
// consistently spent to convert that kind into the other. Every observer named anywhere in the docs
// is a long-lived object that already exists (FrameClock, the frame loop's target cache, Render's
// device); not one is naturally a lambda.
//
// **Rejected: a fixed inline array of {context, thunk} pairs.** Gives emit contiguous memory, and it
// is dominated: "the signal dies first" still requires the signal to reach every observer, so it
// still needs the back-pointers, and it adds a capacity nobody can size. It is this design with an
// arbitrary refusal bolted on.
//
// **Emit is public, and the alternative was not worth its machinery.** Anything holding a reference
// to a signal can fire it, and nothing in the type stops a subsystem from emitting another's. The
// options for closing that are a pass-key type per owner or naming the owner as a template parameter
// — `Signal<IPresenter, const PresentationInfo&>` — and both spend real syntax on a mistake nobody
// makes by accident, in a set of interfaces no client can reach. Docs/Structure.md's rule applies:
// an interface with no prospect of a second buyer is a cost with no buyer.

namespace Detail
{
// Defined in Signal.cpp so that this header does not drag <unistd.h> in for a diagnostic. Reports and
// aborts; see there for why it writes rather than prints.
[[noreturn]] void ReportSignalThreadViolation(const char* what) noexcept;
} // namespace Detail

template<typename... Args>
class Signal;

// The observer's half. One per signal observed, held as a member of the observing object.
template<typename... Args>
class Connection
{
public:
	Connection() = default;

	~Connection() { Disconnect(); }

	Connection(const Connection&) = delete;
	Connection& operator=(const Connection&) = delete;

	// **Not movable, and this is the load-bearing half of the design rather than an omission.** The
	// links could be fixed up by a move constructor easily enough. The context pointer could not: it
	// names the observing object, and moving the connection cannot move what the connection points
	// at. A movable Connection would therefore relink correctly and then call the corpse, which is
	// strictly worse than not compiling.
	//
	// The consequence to design around: **observers have stable addresses.** Per-output state lives
	// in storage reserved to capacity or indexed by a SlotAllocator's slot, not in a vector that
	// reallocates — which is the arrangement Core/SlotAllocator.h already asks for on its own
	// grounds. An observer that genuinely must relocate disconnects and connects again.
	Connection(Connection&&) = delete;
	Connection& operator=(Connection&&) = delete;

	// The method is a template argument rather than a parameter so that the thunk below is a
	// captureless lambda and converts to a plain function pointer — which is what keeps a connection
	// two pointers of callback state instead of type-erased storage. Observer is deduced, so the call
	// site reads `m_Presented.ConnectTo<&FrameClock::Observe>(presenter.Presented, clock)`.
	//
	// Connecting an already-connected Connection disconnects it first. That is the one sensible
	// reading, and leaving it undefined would make re-wiring during migration a thing to remember.
	template<auto Method, typename Observer>
	void ConnectTo(Signal<Args...>& signal, Observer& observer)
	{
		static_assert(
			std::is_invocable_v<decltype(Method), Observer&, Args...>,
			"The method must be callable on the observer with the signal's arguments"
		);

		Attach(signal, static_cast<void*>(&observer), [](void* context, Args... args) {
			(static_cast<Observer*>(context)->*Method)(args...);
		});
	}

	// Idempotent, and safe after the signal has been destroyed — the signal nulls this on its way
	// out, so a destructor running in either order does the right thing. That symmetry is the point.
	void Disconnect() noexcept;

	[[nodiscard]] bool IsConnected() const noexcept { return m_Signal != nullptr; }

private:
	friend class Signal<Args...>;

	using Thunk = void (*)(void*, Args...);

	void Attach(Signal<Args...>& signal, void* context, Thunk thunk);

	Signal<Args...>* m_Signal = nullptr;
	Connection* m_Previous = nullptr;
	Connection* m_Next = nullptr;
	void* m_Context = nullptr;
	Thunk m_Thunk = nullptr;

	// Which emissions this connection is old enough to be part of. See Signal::Emit.
	std::uint64_t m_Serial = 0;
};

// The emitter's half. Held as a member of the object that reports the event.
template<typename... Args>
class Signal
{
	// A broadcast can never transfer ownership — to anyone. With N observers at most one could take
	// the value, and nothing in the signature says which. So a signal carries a fact, not a resource,
	// and `Signal<Fd>` fails here with a sentence rather than deeper in with a copy of a deleted copy
	// constructor. Core/Fd.h records the same rule from the other side, which is why RawFd exists.
	static_assert(
		(std::is_copy_constructible_v<Args> && ...),
		"A signal broadcasts a fact, never a resource: a move-only argument cannot reach N observers"
	);

public:
	Signal() = default;

	~Signal();

	// A signal is a member of the object that reports the event, and both halves of a copy would be
	// wrong: copying the observer list would give two objects the same observers, and not copying it
	// would silently produce a signal nobody is listening to. Moving has the observer-side problem
	// Connection's move does, in mirror image.
	Signal(const Signal&) = delete;
	Signal& operator=(const Signal&) = delete;
	Signal(Signal&&) = delete;
	Signal& operator=(Signal&&) = delete;

	// **The reentrancy contract, which is the whole of what this function is for.**
	//
	//   A handler may disconnect itself, may disconnect another observer, and may destroy the signal.
	//   All three are handled by fixing up the live iteration cursors as part of the unlink, which is
	//   what a defensive copy of the list would otherwise have bought — at the cost of the allocation
	//   the frame section forbids.
	//
	//   A handler may connect a new observer, and that observer does not fire for the emission in
	//   flight. It is spelled with a serial rather than by remembering the tail, because a remembered
	//   tail is itself something a disconnect would then have to fix up, and a serial cannot be
	//   invalidated by anything. Disconnecting and reconnecting during an emission takes a new serial
	//   and so does not fire either, which is the same rule and not a special case.
	//
	//   A handler may emit this signal again. Frames nest on the stack, and each carries its own
	//   cursor and its own horizon.
	//
	// Emission order is connect order, and disconnect preserves it.
	void Emit(Args... args);

	[[nodiscard]] bool IsEmpty() const noexcept { return m_Head == nullptr; }

	// Walks, because nothing needs this except a test and keeping a counter correct across the
	// unlink paths would be one more thing for the destructor to get right.
	[[nodiscard]] std::size_t Count() const noexcept
	{
		std::size_t count = 0;

		for (const Connection<Args...>* node = m_Head; node != nullptr; node = node->m_Next)
		{
			++count;
		}

		return count;
	}

private:
	friend class Connection<Args...>;

	// One per Emit on the stack, linked outward so that a nested emission and a destruction during
	// either can both be answered without the signal owning any of it.
	struct EmitFrame
	{
		Connection<Args...>* Cursor;
		std::uint64_t Horizon;
		bool Destroyed;
		EmitFrame* Outer;
	};

	// Docs/Structure.md has signals intra-thread only: they are ordinary observer callbacks within one
	// thread, and a signal crossing the publication boundary would be a third channel where the design
	// turns on there being two. This is that sentence made mechanical.
	//
	// **A signal is claimed by the first thread to emit it, and keeps that claim for life.** Emission
	// from a second thread is the violation, it is unambiguous, and it is the shape the forbidden
	// third channel would actually take. A new presenter is a new Signal with a fresh claim, so
	// migration does not run into this.
	//
	// **Connect and disconnect are deliberately not checked, and the reason is worth recording because
	// the obvious version of this check is wrong.** Requiring wiring to be on the claimed thread
	// aborts on teardown: decision 41 has the composition root destroy the presenter during device
	// migration, so ~Signal runs off the frame thread and disconnects every observer from there. That
	// is correct code, and it happens on every boot. The check cannot tell it apart from the bug it
	// was aimed at — a hotplug on the dispatch thread wiring itself into a live signal — because both
	// are mutation from a thread that is not the emitter, and what separates them is whether the root
	// has quiesced the emitter, which is not a fact this object holds. A diagnostic that fires on the
	// correct path is worse than an absent one, so the ambiguous half is not guessed at.
	//
	// Unconditional rather than gated on a build flag, for Core/FrameSection.h's reason: it is one
	// relaxed exchange per emit, once per frame per output per signal, so there is no budget argument
	// to have and gating it would make the check absent from the build that ships.
	void BindEmitThread() noexcept
	{
		const std::thread::id self = std::this_thread::get_id();
		std::thread::id unclaimed{};

		if (m_Thread.compare_exchange_strong(unclaimed, self, std::memory_order_relaxed))
		{
			return;
		}

		if (unclaimed != self)
		{
			Detail::ReportSignalThreadViolation("emitted from a second thread");
		}
	}

	Connection<Args...>* m_Head = nullptr;
	Connection<Args...>* m_Tail = nullptr;
	EmitFrame* m_Frames = nullptr;

	// Starts at 1 so that zero is the serial no connection carries, which is what a disconnected node
	// is reset to. Sixty-four bits because Core/Handle.h's argument for not wrapping applies here with
	// less excuse: this counter is per signal and would have to survive two billion connects to one
	// output's presenter.
	std::uint64_t m_NextSerial = 1;

	// Relaxed throughout. This is a diagnostic about which thread owns the signal, not a
	// synchronization edge — a signal that ordered anything between threads would be the third channel
	// the whole rule exists to prevent.
	std::atomic<std::thread::id> m_Thread{};

	static_assert(
		std::atomic<std::thread::id>::is_always_lock_free,
		"The thread check must not put a lock inside Emit; see Docs/Decisions.md decision 36"
	);
};

template<typename... Args>
Signal<Args...>::~Signal()
{
	// Every emission in flight is told first, so that a handler further up the stack does not resume
	// walking a list belonging to an object that no longer exists.
	for (EmitFrame* frame = m_Frames; frame != nullptr; frame = frame->Outer)
	{
		frame->Destroyed = true;
	}

	// Each observer is left in the state it would be in had it disconnected itself, which is what
	// makes its own destructor a no-op whenever it eventually runs. Disconnect maintains m_Head, so
	// this terminates.
	while (m_Head != nullptr)
	{
		m_Head->Disconnect();
	}
}

template<typename... Args>
void Signal<Args...>::Emit(Args... args)
{
	BindEmitThread();

	EmitFrame frame{ m_Head, m_NextSerial, false, m_Frames };
	m_Frames = &frame;

	while (frame.Cursor != nullptr)
	{
		Connection<Args...>* node = frame.Cursor;

		// Advanced before the call, not after. After the call the node may have disconnected itself
		// and no longer knows its successor; before it, the cursor points at the successor and any
		// disconnect of *that* is caught by the fix-up in Connection::Disconnect.
		frame.Cursor = node->m_Next;

		if (node->m_Serial < frame.Horizon)
		{
			node->m_Thunk(node->m_Context, args...);
		}

		// Nothing below this line may touch a member: the handler is entitled to have destroyed the
		// signal, and the frame it wrote to is on this stack rather than in the object.
		if (frame.Destroyed)
		{
			return;
		}
	}

	m_Frames = frame.Outer;
}

template<typename... Args>
void Connection<Args...>::Attach(Signal<Args...>& signal, void* context, Thunk thunk)
{
	Disconnect();

	m_Signal = &signal;
	m_Context = context;
	m_Thunk = thunk;
	m_Serial = signal.m_NextSerial++;
	m_Previous = signal.m_Tail;
	m_Next = nullptr;

	if (signal.m_Tail != nullptr)
	{
		signal.m_Tail->m_Next = this;
	}
	else
	{
		signal.m_Head = this;
	}

	signal.m_Tail = this;
}

template<typename... Args>
void Connection<Args...>::Disconnect() noexcept
{
	if (m_Signal == nullptr)
	{
		return;
	}

	Signal<Args...>& signal = *m_Signal;

	// Every emission in flight that was about to visit this node is moved past it. Done before the
	// relink, because it reads m_Next, and after this loop m_Next is cleared.
	for (typename Signal<Args...>::EmitFrame* frame = signal.m_Frames; frame != nullptr; frame = frame->Outer)
	{
		if (frame->Cursor == this)
		{
			frame->Cursor = m_Next;
		}
	}

	if (m_Previous != nullptr)
	{
		m_Previous->m_Next = m_Next;
	}
	else
	{
		signal.m_Head = m_Next;
	}

	if (m_Next != nullptr)
	{
		m_Next->m_Previous = m_Previous;
	}
	else
	{
		signal.m_Tail = m_Previous;
	}

	// Fully reset rather than merely unlinked, so that a disconnected connection is indistinguishable
	// from one that was never connected. Anything less leaves a stale context pointer in an object a
	// debugger will be read in.
	m_Signal = nullptr;
	m_Previous = nullptr;
	m_Next = nullptr;
	m_Context = nullptr;
	m_Thunk = nullptr;
	m_Serial = 0;
}
