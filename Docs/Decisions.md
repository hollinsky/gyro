# Decision Log

Design decisions with their rationale and, more usefully, the alternatives that were rejected and
why. Questions not yet settled are in [Open.md](Open.md); answering one of them produces an entry
here. Constraints gyro did not choose and cannot answer alone are in
[KernelWishlist.md](KernelWishlist.md), which several decisions here cite for the readings behind
them.

Sections are thematic and numbering is chronological, so a later section is not a lower-level one —
it is simply where the argument had reached. Cross-references are the structure that matters.

Where a decision has been revised, the superseded position is kept as a rejected alternative rather
than deleted — the reasoning that led somewhere wrong is the most useful part of a decision log.
A revision is marked inline and dated at the paragraph it touched, so that a decision carries its
own history. There is deliberately no global ledger of what revised what: it would duplicate every
one of those markers, and a list that must be appended to on every edit is a list that goes stale.
Older revisions predating the convention are not all marked, and a decision whose text was rewritten
without one is not thereby unrevised.

Most of this was recorded before implementation. What implementation has supplied since is the
occasion to ask rather than the answer: 67 came from a promise in
[Experience.md](Experience.md#the-picture-is-correct) that the narrower rule could not keep, 68 from
the arithmetic of what a client has already rasterized, and 70 and 71 from trying to *write a
transition down* — which is the argument for building the motion catalog early rather than last.
69 is the first to change a type rather than a rule, and the argument for making it then was that
the type had one caller.

Five entries were settled by going and reading the source an argument rested on — 2, 49, 73, 76,
and 106 — and each carries that reading in its own text. The rules they taught are in
[AGENTS.md](../AGENTS.md#how-decisions-get-made), because they are instructions to whoever works
this list next rather than history. In short: an argument that names its source can be retired by an
afternoon of reading; read even when you expect to be confirmed, since a right answer on a wrong
argument survives review and fails in the field; and a question that resists the reading may be
malformed, in which case suspect the rule upstream of it; and a question of the form *does X forward
Y* is answered by enumerating everything X does send, since a grep that finds nothing only proves
the grep.

Detail lives in [Architecture.md](Architecture.md) and [Animation.md](Animation.md); this file is
the short answer to "why didn't we do X".

---

## Platform

### 1. The nested backend drives raw Wayland protocol, not Vulkan WSI

Rendering targets are dmabuf-backed `VkImage`s in every backend, so the renderer never sees a
`VkSwapchainKHR`. Nested achieves that by driving `zwp_linux_dmabuf_v1`, `presentation-time`, and
`wp_linux_drm_syncobj_v1` directly.

**Rejected: `VK_KHR_wayland_surface` + swapchain.** Fastest route to pixels, but it forks the
renderer — a swapchain in one backend and dmabufs in the other — which is the exact thing the seam
exists to prevent. It also hides presentation timing (`VK_KHR_present_wait` and
`VK_GOOGLE_display_timing` have patchy coverage and give less than `wp_presentation_feedback` gives
for free), hides explicit sync, and hides modifier negotiation.

### 2. gyro owns the protocol seam; libwayland implements the server codec

The shadow object model and the seam above it are gyro's from line zero. The **client** half of the
codec is gyro's, because
[decision 1](#1-the-nested-backend-drives-raw-wayland-protocol-not-vulkan-wsi) needs it for the
nested backend and there is no alternative that does not fork the renderer. The **server** half is
`libwayland-server`, behind that seam.

*(Revised three times, all on 2026-08-16. The first two revisions replaced this decision's
rationale; this one reverses its conclusion. Both superseded rationales and the superseded
conclusion are kept below.)*

**The decisive argument was checked, and it did not survive.** It held that libwayland resolves
allocation failure and internal invariant violations by calling `wl_abort()`, and compiles in
assertions whose disposition depends on how the distribution built it — so a linked library could
terminate, at a moment of its own choosing and with no way to intercept, the one process whose death
takes every client belonging to every user on the machine. The reading, against `1.26.0-9-ged0b9f1`:

- **18 `wl_abort()` call sites in the server library.** `wayland-client.c` is not linked into
  `libwayland-server`, which removes 11 of the tree's 29 sites before analysis begins.
- **Allocation failure never aborts. Not once.** Every OOM path returns `NULL` or `-1`, and the
  caller answers with `wl_client_post_no_memory` or `wl_resource_post_no_memory` — a protocol error
  killing the offending client, which is exactly the policy
  [decision 27](#27-resource-accounting-is-attribution-not-per-user-fairness) prescribes for gyro's
  own limits. Verified at `wl_resource_create` on both its failure paths, at the demarshal step in
  `wl_client_connection_data`, at registry bind and `wl_display.sync`, at
  `ring_buffer_ensure_space`, and on the event-send path, where `handle_array` sets `client->error`
  and lets `destroy_client_with_error` run.
- **There are no assertions to have a disposition.** Three `assert()` calls reach the server's
  include set, all in `timespec-util.h`, inside two functions `event-loop.c` never calls. Nothing
  depends on how a distribution set `NDEBUG`, and the reason is that upstream removed the dependency
  deliberately: 1.24's `0cecde3 src: switch asserts to wl_abort` and `58bb6c7 src: Finish assert()
  clean-up` are what took the server-path count from 6 to 18.
- **Two sites are reachable from client input**, both in `wl_closure_invoke`: a resource whose
  `implementation` is still NULL, and a hole at `implementation[opcode]`. The opcode is
  bounds-checked against `interface->method_count` first, so the second needs gyro to advertise a
  version whose request set exceeds the function pointers it supplied. Both require a gyro bug as
  the precondition, and `b26180d` added the first in May 2025 to replace a NULL-deref segfault — so
  what is being chosen there is an abort over crashing anyway, not an abort over recovery.
- **The remaining 16** are gyro's own API misuse at gyro's own call site (shm pool and buffer
  refcount underflow, unbalanced `begin_access` / `end_access`, `wl_array` use-after-release,
  `wl_global_remove` twice), libwayland's internal timer-heap invariants, or arithmetic guards and
  startup paths unreachable in practice.

So the premise is false in both halves. What remains terminates gyro for bugs gyro would equally
have to not write in its own implementation, and every failure mode a *client* can drive is already
resolved the way this document resolves them everywhere else — kill the client, keep the compositor.

**What that leaves of the three supporting arguments.** The first was already weakened to a
preference: dropping a client's event source and letting the kernel socket buffer apply backpressure
is a real lever, it works under libwayland, and under
[decision 45](#45-protocol-dispatch-is-a-thread-not-a-task) the traffic is on a thread nothing above
it waits on. The second was already recorded as an even trade, and it inverts cleanly — the
process-global SIGBUS handler is inherited rather than chosen, which is what this entry always said
the alternative was. The third **survives intact and is now the strongest of them**: no `wl_list`,
`wl_signal`, or `wl_listener` object model at the boundary, where destroy-listener use-after-free is
the best-known bug family in compositors built this way. But it argues for the shadow object model
and the bindings, which gyro builds regardless — not for a demarshaller and a socket manager, which
is where the cost estimate lives.

**One new argument, pointing the same way.** The two client-reachable aborts are precisely the
failure class a generated, typed dispatch table makes structurally impossible: a table that cannot
have a hole, and a resource that cannot be published to a client before it has an implementation.
That is an argument for gyro's *bindings*, which are being written either way. Under libwayland it
costs a thin wrapper over `wl_resource_set_implementation` that refuses to hand out an id first —
cheaper than the codec it would otherwise justify.

**What is unchanged.** The shadow object model is gyro's, with its own lifetime discipline, because
decision 45 has the frame thread read published state and a `wl_resource` is destroyed synchronously
on the dispatch thread when its client goes away. The scene reads a snapshot, never protocol
objects. Bindings are generated at build time by a host tool with a hand-rolled parser for the XML
subset the protocols use — no scripting-language dependency, no third-party XML library, no
generated code in the tree. Cross-thread object lifetime is still gyro's problem and still not
retrofittable.

**What changes in practice.** libwayland's `wl_event_loop` fd is nested on the dispatch ring, which
[decision 3](#3-io_uring-event-loop-via-liburing) already prices at a wakeup per dispatch on a
thread that owes no frame. Continuous fuzzing of the demarshaller moves off gyro's cost column and
onto upstream's, where that code is the most-exercised part of the ecosystem. `wl_shm`'s SIGBUS
trampoline is inherited. The 3–6 weeks to parity and the interop tail are not spent.

**Rejected: writing the server codec on the strength of the restart-boundary argument.** The
position this decision held until the reading was done, and the reason it is worth keeping is the
shape of the error rather than the conclusion. The argument was never verified, it was load-bearing
for a commitment measured in weeks, and this entry's own deferral clause named the exact condition
that would retire it — *"if the answer is 'allocation failure only, and gyro is dead in that case
regardless', this falls back to the three supporting arguments and the margin is then thin enough
that the interim becomes the answer."* The answer came back weaker than that clause anticipated: not
allocation failure only, but not allocation failure at all. The deferral is the part that worked.
Committing first and reading afterwards would have spent the largest single line item in the
estimate on a premise that was wrong.

**Rejected: the priority-inversion rationale.** Recorded originally as the decisive argument: that
libwayland's per-message allocation exposed the `SCHED_FIFO` thread to a malloc arena lock held by a
normal-priority client thread. Clients are separate processes with separate glibc arenas, so that
mechanism cannot occur at all.

**Rejected: the repair, that client traffic drives allocation on our own real-time thread.** True as
stated, and the form the argument should always have taken — not *their* lock blocking us, but
*their* message volume deciding how often our frame thread enters the allocator. It was overtaken
within a day by [decision 45](#45-protocol-dispatch-is-a-thread-not-a-task), which takes dispatch
off that thread entirely; allocation on the dispatch thread costs one client some latency and costs
the frame nothing. All three failures are recorded at length because the pattern is the lesson: this
decision had three rationales and outlived every one of them, and the real-time framing that carried
two of them was never what it was about.

**Not foreclosed: the in-tree server half.** The seam is what keeps it a swap rather than a rewrite,
and nothing is built on libwayland that would have to be unbuilt. Three things would reopen it, and
they are written down so that "we always meant to" is not itself one of them: a `wl_abort` reachable
from client input with no gyro bug in front of it, in a version we would have to ship; a per-message
dispatch budget turning out to matter under a real client load; or an object-lifetime or shm hazard
that proves cheaper to own than to work around. Absent one of those, the ambition is retired and the
weeks go to
[decision 29](#29-outputs-are-periodic-real-time-tasks-the-test-allocates-effect-budget), which is
where this project is actually differentiated.

**Cost accepted:** a dependency whose failure modes are now inventoried rather than assumed, and two
`wl_abort` sites that a wrapper over resource creation closes. The abort inventory is a snapshot of
one version and the trend is upward — 6 sites in 1.23, 18 in 1.24 — so it wants re-reading on major
version bumps rather than being treated as settled forever.

### 3. io_uring event loop via liburing

`IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN`, timer-first.

The justification is **determinism, not throughput** — collapsing ~40 syscalls to one saves tens of
microseconds against a 16.6 ms budget, which is noise. What earns its place is `DEFER_TASKRUN`
running kernel completion work where we ask for it rather than preempting mid-render.

**One ring per thread, and `SINGLE_ISSUER` is why.** *(Revised 2026-08-16.)* The flag admits exactly
one submitting thread per ring, so [decision 45](#45-protocol-dispatch-is-a-thread-not-a-task)'s
split gives the frame thread and the dispatch thread a ring each. That is the better arrangement
rather than a concession to it: the frame thread's ring now carries timers and KMS and nothing whose
cost or volume a client influences, which is precisely the property `DEFER_TASKRUN` is being bought
for. The dispatch thread's ring is a convenience there and would be defensible as plain epoll.

**Rejected: `wl_event_loop` as the frame loop.** Makes the frame deadline just another timer
competing with client traffic. Note the narrowing: this rejects `wl_event_loop` on the *frame*
thread, and decision 2 now puts libwayland's loop on the dispatch thread, nested — which the next
paragraph already priced and which the frame ring never sees. **Rejected: own epoll loop.** Fine,
and it would give up little; io_uring earns the frame ring on `DEFER_TASKRUN` alone.

**Rejected: "the two decisions are one decision."** Recorded in decision 2 originally, and the
mechanism is real: libwayland does its own buffered `recvmsg` / `sendmsg`, so nesting it inside
io_uring carries the complexity while forfeiting the benefit. The conclusion drawn from it was not.
libwayland nests perfectly well via `wl_event_loop_get_fd()` polled through io_uring, at the cost of
a wakeup per dispatch and the loss of batching on socket I/O — and on the dispatch thread, which is
where decision 45 puts that traffic, neither of those costs anything that can be measured against a
frame. The two decisions are independent, and decision 2 no longer draws support from this one.

*(Annotated 2026-08-16, after decision 2 reversed.)* That last paragraph was written to sever a
dependency and has turned out to be load-bearing in the other direction: the nesting it describes as
hypothetical is now the arrangement. Nothing in it changes, which is the useful part — the cost was
priced before there was any incentive to price it favourably.

**The floor is a capability, not a version, and it is smaller than it was.** *(Added 2026-08-16.)*
Decision 2's reversal removes multishot `recvmsg` from the requirement outright — libwayland does
its own buffered `recvmsg` and `sendmsg`, so gyro never issues one — which leaves the frame ring's
`SINGLE_ISSUER | DEFER_TASKRUN` as the whole of it. Probed rather than inferred, because a version
number errs in both directions: distributions backport flags into older kernels, and
`io_uring_disabled` removes them from newer ones. gyro attempts the exact configuration at startup
and reports which flag failed. **No epoll fallback is built**, on the floor tier's own argument
inverted — an alternate frame loop that nobody would ever execute is not a recovery path, it is
untested code. Target kernels are current ones by choice; the past has the past's compositors.

**Open: the determinism claim is unmeasured, and it is the shape decision 2 just lost.** This entry
rests on `DEFER_TASKRUN` keeping kernel completion work off the render, and that mechanism has been
argued rather than observed, exactly as decision 2's abort premise was until it was read. The
measurement is absolute `IORING_OP_TIMEOUT` wake accuracy under `SCHED_FIFO`, with the flag and
without, against a `timerfd` and epoll control under load. It wants the frame loop's shape, so it
belongs beside the schedulability sweep in the headless harness rather than now — but it is recorded
here rather than only in the open list, because the lesson is that an unverified premise is most
dangerous while it is still comfortable.

### 45. Protocol dispatch is a thread, not a task

Wire dispatch, demarshalling, input, and client buffer import run on a **dispatch thread**. The
frame thread reads committed state across a one-way publication boundary and never touches a socket,
a client, or an allocator.

A commit that lands before an output's record point is in that frame; one that lands after it is in
the next. This is the semantics Wayland already has — clients are paced by frame callbacks and have
no say in when a compositor reads them — promoted to a design property instead of being left as an
accident of how the loop was scheduled.

**What the frame thread stops containing is everything a client can influence the cost of.**
Per-message allocation, `wl_shm` mapping and the SIGBUS window it opens, dmabuf import, and Vulkan
resource creation from client buffers were all implicitly on the frame path, and the last two are
far larger than the allocation
[decision 2](#2-gyro-owns-the-protocol-seam-libwayland-implements-the-server-codec) was originally written
about. A hostile or merely chatty client now cannot reach the frame thread at all, which is a
stronger property than bounding what it costs us.

**Input is on the dispatch thread too, drained first.** It arrives from evdev rather than from
clients, but it is the same kind of work — external data, unbounded library calls, a publication to
the frame thread — and it belongs on the same side of the boundary. Ordering within the thread is
input, then client traffic under a per-client budget, which is decision 2's deterministic-dispatch
argument finally landing somewhere it applies.

*(Annotated 2026-08-16, after decision 2 reversed.)* That ordering survives the reversal, because it
is a property of this loop rather than of the codec underneath it: evdev is drained before
`wl_event_loop_dispatch` is called at all. What does not survive is the *granularity* — a
message-granular per-client budget is not something libwayland offers, so the budget is coarse,
dropping a client's event source and letting the kernel socket buffer push back. Decision 2 records
why that is now judged sufficient.

**`SCHED_FIFO`, in the lowest real-time band.** Not `SCHED_OTHER`: a commit arriving shortly before
the record point should make that frame, and a normal-priority thread may not be scheduled in time
under load to publish it. The ordering rule is general, and it follows from what blocks on what:

```
	frame thread     highest         waits on device workers, never on dispatch
	device workers   one below       software path only
	dispatch thread  lowest RT band  nothing above it waits on it
```

Device workers outrank dispatch because the frame thread *waits* on them — see
[decision 40](#40-software-rendering-is-a-device-not-a-backend-and-it-is-the-floor-tier) — and
dispatch goes last because the frame thread never does. Absolute levels differ between the
accelerated and software paths; the order does not. Detail in
[Architecture.md](Architecture.md#threads).

**The invariant that makes it work: the two threads share no lock.** Publication is
single-producer / single-consumer and wait-free on the reader — the frame thread swaps in the newest
published snapshot, never blocks, never allocates, and cannot be delayed by a thread it outranks. A
shared mutex would rebuild, in a worse and less visible form, exactly the priority inversion this
whole area of the design exists to avoid.

**Reclamation is deferred, not reference-counted.** The frame thread publishes the sequence number
of the last snapshot it consumed and the dispatch thread frees everything below it. Shared ownership
across the boundary is what a naive design reaches for, and it puts `free` on the frame path wearing
a destructor's clothes, where no debug allocator will catch it.

**The snapshot is offset-addressed POD, and the frame side bounds-checks what it resolves.**
*(Added 2026-08-16.)* Pointers and standard-library containers in the published representation would
be the obvious choice and they foreclose something worth keeping:
[decision 49](#49-the-restart-boundary-is-made-cheap-where-it-can-be-and-stated-where-it-cannot)
holds open the option of dispatch becoming a *process* rather than a thread, which is the only
mechanism that meaningfully shrinks what a restart costs. That move is a change of medium — shared
memory instead of a heap — and it is a change of shape only if the snapshot cannot live in a mapping
whose base address differs between the two sides. Offsets survive that; pointers do not.
[Decision 15](#15-identity-is-a-generational-handle)'s generational handles already supply most of
the checking half. The whole affordance is a representation choice, it is free at line zero, and it
is the reason this decision does not have to be right about threads-versus-processes today.

**Cross-thread lifetime is the real price.** A client may destroy a surface while the frame thread
holds it. The single-threaded design gets that right for nothing and this one has to build it:
[decision 15](#15-identity-is-a-generational-handle)'s generational handles already supply the
detection, and [decision 20](#20-exit-animations-use-full-resolution-snapshots) already establishes
that an entity outlives its protocol object, so the shape exists — but the discipline is new, and it
is not retrofittable.

**Two claims here were revised by
[decision 50](#50-the-world-is-authored-on-the-dispatch-thread-the-snapshot-carries-coefficients).**
*(2026-08-16.)* This decision did not say where the world the wire protocol describes actually
lives, and the answer is the dispatch thread — window-management mechanism, the entity graph, and
every spring, published across the same boundary as coefficients. It also called the boundary
one-way, which is not true in the aggregate: frame callbacks, presentation feedback, buffer release,
and decision 46's blit hold all run frame → dispatch, and are better designed as one bounded reverse
channel than accreted as exceptions to a claim of exclusivity.

**Rejected: dispatch on the frame thread, time-boxed.** The original design, recorded in decision 29
as a task in the schedulability set and in the event loop as budgeted traffic drained ahead of the
per-output loop. It fails on granularity: a time box bounds only the interval between checks, the
checks fall between messages, and so the real bound is the cost of one message — which includes an
allocation, a page fault, an shm map, or a dmabuf import. The budget was a target dressed as a
bound. It also charged every client's traffic against the frame budget, which is the wrong party
paying.

**Rejected: `SCHED_OTHER` for the dispatch thread.** Removes the priority question entirely and
gives up input-to-photon latency under system load, which is the part the user actually feels.

**Rejected: a dispatch thread per client, or sharded by connection.** Not needed on a laptop's
client count, and cheap to add later precisely because the publication boundary is already the only
channel to the frame thread — sharding becomes a routing change rather than a redesign.

**Rejected: a render thread per output.** A different split, separately rejected in
[decision 29](#29-outputs-are-periodic-real-time-tasks-the-test-allocates-effect-budget) for a
different reason — it parallelizes command recording when the constraint is GPU execution. Noted
here because the two are easily confused: this decision moves work *off* the frame thread rather
than dividing what remains on it.

### 50. The world is authored on the dispatch thread; the snapshot carries coefficients

Window-management mechanism, the entity graph, the differ, and every `Animatable` live on the
dispatch thread beside the wire protocol and input. The frame thread receives them through the
publication boundary decision 45 already establishes — one channel, not a second one — and what
crosses is spring **coefficients**, never evaluated values.

This is the question decision 45 left unasked. It named two threads and assigned dispatch "wire
protocol, input, buffer import", and never said where the world those describe actually lives.

**Spring state is the same shape as client surface state, which is why no second boundary is
needed.** A retarget writes `(u₀, v₀, t₀, ω, ζ, target)` and nothing touches it again until the next
commit retargets it: authored on one side at event rate, consumed on the other at frame rate,
immutable in between. That is decision 45's publication exactly, with the compositor as a second
author alongside the clients.

It works only because of [decision 11](#11-springs-are-closed-form-not-numerically-integrated). An
integrated spring has to be *advanced* by whoever evaluates it, so the frame thread would mutate
what it reads, and the boundary would become bidirectional and stateful — which is a lock across the
priority ordering that the entire thread split exists to prevent. Closed form is what keeps the
publication one-way. It is the payoff of decision 11 least visible from the solver.

**Coefficients rather than values, and this is an invariant rather than a preference.**
[Decision 28](#28-the-frame-clock-is-per-output) has two outputs evaluating one scene at two
presentation times in a single iteration. Publishing evaluated values would collapse that back to
one timeline silently — the same defect decision 11 rejects, reintroduced at the boundary instead of
in the solver, and invisible until someone plugs in a second monitor. The frame thread evaluates per
output, from coefficients, always.

Three things fall out that did not need designing separately:

- **[Decision 16](#16-nodes-own-their-properties-the-active-set-is-mirrored)'s active set stops
  being a mirror and becomes a serialization output.** Dispatch emits active springs contiguously
  into the snapshot at publish time, so the frame thread's walk is cache-friendly by construction
  and offset-addressed as decision 45 requires. "Derived, never maintained" gets stronger rather
  than weaker: there is no second representation that could desync, only a serialization of the
  first.
- **Settling needs no back-channel.** Settling time is analytic, so dispatch knows when each spring
  settles without evaluating anything, and schedules its own timer. Destruction on settle,
  retiring-set drainage, and atlas rectangle release therefore all land on the side that is allowed
  to allocate — routed through the consumed-sequence watermark, since the frame thread may still be
  rendering a snapshot that names what is being freed.
- **The degradation mode is input latency, never judder.** Dispatch overrunning delays the next
  publication. The frame thread evaluates the snapshot it already holds, and because that snapshot
  carries live coefficients rather than values, every in-flight animation keeps producing exactly
  correct results at exactly the right presentation times. Only *new* input is late. A `Tick(delta)`
  design cannot make that trade, because its snapshot goes stale the moment it stops being advanced.

**The boundary is not one-way, and decision 45 was wrong to say it was.** Frame callbacks,
`wp_presentation_feedback`, `wl_buffer.release`, and the per-buffer hold that
[decision 46](#46-exit-snapshots-come-from-a-pre-reserved-per-output-atlas-exhaustion-finishes-exits-early)'s
deferred blit needs are all frame-thread knowledge that dispatch requires, and decision 45 already
half-admits this by putting the consumed-sequence watermark and the buffer hold in the reader's
hands. So there is a second SPSC channel, frame → dispatch, and it is better designed once than
accreted as four special cases. It is wait-free on the *writer* this time, which makes it bounded
with a stated overflow policy — an easy bound, since records per frame cannot exceed outputs ×
surfaces.

**Rejected: a third thread for window-management policy.** It isolates gesture latency from a chatty
client more strictly, and costs three boundaries instead of one — input in, scene out, protocol
replies back — with the policy side needing to read client state it does not own. Bidirectional
publication between two threads is exactly where the no-shared-lock discipline stops being free. It
stays cheap to add later provided policy is written against an explicit events-in / commands-out
interface rather than reaching into protocol objects, which is the same affordance decision 45
already names for sharding dispatch by connection.

**Rejected: the world on the frame thread.** The differ, the dirty set, matched geometry, and the
transition resolver all allocate, and
[decision 36](#36-frame-path-discipline-is-enforced-mechanically-not-by-review) makes that
mechanically impossible. Animation.md had already reached the same place from the other direction —
commits happen at event rate, evaluation at frame rate — without stating the consequence.

**Rejected: evaluated values in the snapshot.** Cheaper on the frame thread and fatal to decision
28, as above. Worth recording as rejected rather than merely absent, because it is the obvious
implementation and its failure mode is a configuration nobody tests on.

**Open: what paces publication.** Eager per resolved commit is simplest and serializes the scene
once per input event, which a 1000 Hz mouse makes real; pacing to the fastest output's period costs
up to a period of gesture latency, which is the worst thing available to spend. The likely answer is
eager with a copy-on-write arena, so unchanged subtrees are shared between consecutive snapshots and
emit cost is proportional to the dirty set that
[decision 14](#14-declarative-commits-with-dirty-tracking) already computes — offsets survive that
fine. Start with eager full re-emit and measure; the representation is offset-addressed either way,
so the arena strategy is contained.

**The arena is less contained than this says.** *(Annotated 2026-08-22.)* The sentence above is true
of the offsets and not of the indices.
[Decision 86](#86-the-published-scene-is-a-preorder-tree-model-values-inline-coefficients-by-reference)
has a node name its active channels by index into runs whose packing is global, so a shared,
unchanged subtree can be invalidated by a node elsewhere becoming active and renumbering a run.
[Decision 90](#90-the-snapshots-runs-are-one-per-channel-and-the-frame-side-validates-the-tree-it-walks)
records the fork that falls out — node-ordered runs, which make the frame thread's reads monotonic,
against append-ordered runs, which keep indices valid across publications. The instruction here is
unchanged: ship the full re-emit and measure.

**The structural half is now settled.** *(Annotated 2026-08-22.)* This decision fixed what crosses
for *animation* and never said what the coefficients are coefficients of, which stayed unnoticed
while [Snapshot.h](../Source/Publication/Snapshot.h) had no `Scene` writing into it.
[Decision 86](#86-the-published-scene-is-a-preorder-tree-model-values-inline-coefficients-by-reference)
answers it: the topology crosses as a preorder run of node records carrying their own subtree
lengths, a node names its active channels by index into the runs this decision established and
carries its settled ones inline, and the flattened alternative turns out to be unauthorable rather
than merely cheaper. The pacing question above is untouched by it.

### 74. The forward ring recycles only below the watermark, and a full ring defers

*(Settles the flow control [decision 45](#45-protocol-dispatch-is-a-thread-not-a-task) left implicit,
surfaced 2026-08-17 building `Publication/Ring.h`.)*

The frame thread takes the **newest** published snapshot and skips whatever it passed. A snapshot is
complete scene state rather than a delta, so one nobody read costs nothing. The invariant that buys
this is worth stating because it is what a later addition would break: **nothing may be owed once per
published snapshot — only once per rendered frame.** Frame callbacks, presentation feedback, and
buffer releases are all owed per frame, so they survive skipping; a per-snapshot obligation would
not, and would fail silently.

A snapshot's slot is derived from its sequence, and the writer never touches a slot at or above the
watermark. That is what makes the reader wait-free without a re-check, and the argument is short
enough to record. The slot a sequence *S* lands in was last occupied by *S − depth*, so rewriting the
slot the reader is reading means publishing *S + depth*, which requires *S* to be strictly below the
watermark. But the watermark is a sequence the frame thread itself reported, so it is at most the
sequence the frame thread holds, which is strictly below *S* — the snapshot it is reaching for is one
it has not held yet. *S <* watermark *≤* held *< S* is a contradiction.

**Rejected: scanning the slots for the newest.** It reads fields the writer may be writing, which is
a data race whatever the sequence numbers say.

**Rejected: a seqlock re-check on the reader.** Read the slot, re-read its sequence stamp, retry if it
moved. This is the conventional answer and it is lock-free rather than wait-free — bounded retries in
practice, unbounded on paper — on the one thread decision 45 makes wait-free a *requirement* for. The
cost of being wrong about "bounded in practice" is a frame thread paused inside a composite.

**Rejected: a reader-side intent flag**, which is a handshake, which is a lock wearing a flag's
clothes across exactly the priority ordering the thread split exists to prevent.

**Rejected: dropping the snapshot when the ring is full.** Nearly harmless, and the exception is what
kills it: if the dropped publish is the last one before the scene quiesces, nothing republishes it and
the frame thread renders stale state until something unrelated moves. Rare, silent, and reads to a
user as *the window sometimes ends up in the wrong place*.

**Rejected: backpressure on dispatch.** No deadlock — the frame thread never waits on dispatch, so
the wait is in the permitted direction — but it makes dispatch's latency a function of the frame
thread, and a frame thread blocked on a modeset would stop dispatch draining clients, pushing the
stall out into client sockets.

So a refused publish is **deferred**: the dispatch side retains the bytes it already built and retries
at the top of its next iteration, and a newer serialisation *supersedes* the pending one rather than
queueing behind it — queueing would deliver a scene the world has already moved past at the cost of
the one the frame thread wants. Memory is bounded at depth + 1 snapshots and dispatch never blocks.

The depth is four. Three is the floor — one held by the frame thread, one newest, one for the writer
to build into — and the fourth is a frame of slack, so an ordinary late frame never reaches the
deferral path at all.

**Cost accepted:** the dispatch loop grows a flush step distinct from its publish step, and one
snapshot's worth of memory sits idle in the common case where nothing was ever deferred.

### 75. The return channel is one report per frame; per-surface facts are derived, not sent

*(Settles the "stated policy for what happens when it fills" that
[Architecture.md](Architecture.md#the-publication-boundary) names and leaves open, surfaced 2026-08-17
building `Publication/Return.h`.)*

The return channel is wait-free on the **writer**, so it is bounded, so something has to happen when
it fills. Dropping is the obvious answer and it is wrong: a lost `wl_surface.frame` callback is not a
hitch but a **hang**, because a client waiting on it never draws again.

The way out is to notice what the frame thread knows that dispatch does not, which is less than it
looks. Dispatch *authored* the snapshot, so it already knows which surfaces sequence *S* contained; a
report saying *S was presented at T* lets it derive the frame callbacks and the buffer releases
itself. That is decision 50's publish-coefficients-and-evaluate-per-output run backwards, and it
collapses the channel from outputs × surfaces to **one fixed-size report per frame**. What is left
irreducible is the per-buffer hold, which Architecture.md already names as the one thing the watermark
cannot express.

And then the sizing problem dissolves against something that is already true: **the frame thread
cannot allocate, so every set it holds is fixed-capacity.** The number of holds that can release in
one frame has a ceiling whether this decision states one or not. Size the record from that ceiling and
the queue from a generous number of frames of dispatch latency, and there is no drop policy left to
write. Two paths remain and neither loses anything — a full queue is merged into the writer's own
staged report, which is legal because it is the only writer, and releases that do not fit are
*refused*, so the frame thread keeps holding those buffers for another frame.

**The watermark therefore rides the channel literally**, as Architecture.md says, rather than as a
monotone atomic beside it. The atomic was the right answer while the queue could drop — a lost
watermark stalls reclamation — and became unnecessary the moment nothing could be lost. Recorded as
rejected because it is the shape this reaches for first and the reason it is not needed is not
obvious from the queue alone.

**Rejected: a queue templated on its payload.** Architecture.md's claim that *a third channel would be
a design error rather than an addition* has to be enforceable by something, and a concrete record is
what enforces it: a template is an invitation to instantiate a second queue for the next kind of
frame-thread knowledge somebody needs. The friction of extending one record is the feature. It also
keeps [decision 49](#49-the-restart-boundary-is-made-cheap-where-it-can-be-and-stated-where-it-cannot)'s
process option alive, since a templated queue is a POD obligation thrown away for nothing.

**Rejected: fixing the full field set now.** The presented sequence and its timestamp, the per-output
measured costs that feed budgets, the VRR servo's observations — all of them belong in this record and
none has a producer until the frame loop exists. The *shape* is what is being fixed, because the shape
decides the sizing and the loss policy and cannot be changed later; a field costs a recompile of two
halves that are always built together.

The hold names its buffer with a [generational handle](#15-identity-is-a-generational-handle), so a
stale release compares unequal rather than naming whatever occupies the slot now. That is decision 15
doing its work at the one place in the design where cross-thread lifetime is admitted to be genuinely
new, and retrofitting identity into a record after the fact is where that bug lives.

**Cost accepted:** dispatch must be able to derive per-surface consequences from a presented sequence,
which means keeping the authoring side of a snapshot addressable until that sequence is reported. It
is the same retention the watermark already implies, so it is a constraint made explicit rather than a
new one.

### 4. All three backends are in scope: nested, headless, DRM

Nested is the daily driver and unlocks RenderDoc, validation layers, ASan, and gdb. Headless is what
makes golden-image tests and injected missed-deadline tests possible. DRM is the product.

### 5. The DRM backend is designed from specification, with no hardware spike

Consequence accepted: the presenter interface is deliberately over-provisioned — plane assignment,
per-plane fencing, mode-set versus page-flip — so the DRM backend has room to fit without a
redesign. Nested and headless will carry interface surface they do not use. Spots designed from
documentation rather than observed behaviour are marked `// SPEC:`.

**Rejected: a throwaway ~500-line spike** proving atomic modeset plus Vulkan dmabuf import plus page
flip before freezing the seam.

### 6. No macOS port; development continues over SSH

**Rejected: nested on macOS.** It requires porting the entire lower half to a platform missing every
primitive it is built on. No dmabuf, so every client falls back to `wl_shm` with a CPU upload per
frame — testing a buffer path that is not the production path. MoltenVK has no
`VK_EXT_image_drm_format_modifier` and no `VK_KHR_external_memory_fd`, so the presenter interface is
inexpressible. libwayland's server loop is epoll-based and needs shimming. And there are
essentially no Wayland clients on macOS — GTK's macOS backend is Quartz, and QtWayland on macOS is
unsupported — so the compositor would have nothing to display.

`Core/`, `Scene/`, and `Animation/` stay free of Linux headers regardless, so portable unit tests
build anywhere. That is free and worth doing on its own merits, and it is mechanical rather than
aspirational: a module declares `PORTABLE` to `gyro_add_module`, which enrols it in
`CMake/CheckPortability.cmake`.

### 7. Session claiming is deferred; basu rejected

`ISession` is defined and stubbed for nested and headless. The implementation is chosen when the DRM
backend needs it, by which point it will be clear how reusable the protocol layer's SCM_RIGHTS and
socket machinery turned out to be — the main input into whether hand-rolling D-Bus is nearly free.

**Rejected: basu.** Not in Fedora repositories, so it would have to be built — meaning
`ExternalProject_Add` driving meson from CMake. Its motivation was keeping elogind viable, but
elogind is not packaged either, so it would buy portability to distributions we are not on, paid for
in build complexity on the one we are.

Remaining candidates are libsystemd's sd-bus (packaged, zero build complexity, systemd-coupled) and
a hand-rolled D-Bus client (~1000 lines, zero dependencies). Note that no frame-path argument
applies here — this runs once at startup and then idles on a signal, so it is nowhere near the frame
thread even before [decision 45](#45-protocol-dispatch-is-a-thread-not-a-task) moves the boundary.

**Likely retired by decision 24.** With no VTs, permanent DRM master taken by first-open, device
access by udev rule, and session lifecycle arriving through the listener handover, there may be no
remaining reason for gyro to speak D-Bus at all. The residual dependency on logind is via
`pam_systemd` creating `XDG_RUNTIME_DIR` — a deployment dependency, not a runtime coupling. This
becomes final when the DRM backend proves device pause and resume needs nothing more.

The boot chain in decisions 37–43 strengthens this without closing it. logind is still in the
picture for *user sessions* — the login agent runs an ordinary PAM stack including `pam_systemd`,
which is how `XDG_RUNTIME_DIR`, `systemd --user`, and therefore portals and pipewire all come to
exist. But that coupling is the login agent's, not gyro's, and gyro never holds a bus connection.

**Suspend was the strongest remaining candidate for needing one, and it does not.**
*(Annotated 2026-08-16.)* System suspend requires a delay inhibitor and a handshake before the
machine stops, which is a logind interface and looks like the case that finally forces a bus client
into gyro. [Decision 59](#59-suspend-is-a-handshake-on-the-control-connection-resume-is-a-modeset)
routes it through the greeter's session agent instead — a peer that is persistent, already
PAM-adjacent, and already holding a connection to gyro whose EOF is load-bearing. So this deferral
survives the one event that seemed most likely to end it, and the remaining question is unchanged:
whether device pause and resume needs anything the DRM backend cannot get from first-open master.

### 8. Platform stack via pkg-config; CPM for C++ libraries

The distinction is **not** meson versus CMake. wayland, libdrm, libinput, and libxkbcommon are all
meson or autotools, and that is invisible because distributions package them. The distinction is
whether we ever have to *build* it.

### 9. volk and runtime shader compilation in; VMA and a test framework out

Runtime shader compilation is for hot-reloading effect shaders during development, which pairs with
the nested backend for iterating on blur. A compositor's allocation patterns are few and simple
enough that VMA's convenience does not outweigh direct control.

**Runtime compilation turns out to be a production path.** *(Added 2026-08-16.)*
[Decision 62](#62-effect-composition-is-an-optimization-and-the-unfused-path-is-the-reference) fuses
effect chains into pipeline variants and compiles a missing one off the frame path while the frame
is drawn unfused, so the facility taken here for development convenience is what a shipped machine
uses to fill its variant cache. The constraint that arrives with it is that compilation must never
be on the frame thread, which decision 62 states.

### 77. A signal's observers are links the observers own

*(Decided 2026-08-17, when `Signal<>` was built as the first of `Seam`'s three prerequisites in
`Core`.)*

`Signal<>` stores its observer list as an intrusive doubly-linked list whose nodes are `Connection`
members of the observing objects. Connect, emit, and disconnect allocate nothing. A `Connection` is
neither copyable nor movable.

**The constraint that forces it is bidirectional lifetime, and both directions run on every boot.**
[Decision 41](#41-device-migration-is-exercised-on-every-boot) has `simpledrm` replaced by the real
driver while `Frame` keeps running and the last frame stays on glass, so `Render` tears down whole
underneath a live frame thread. In that window a signal outlives its observer — an output is
unplugged and `Frame`'s per-output state dies while the session serves other outputs — and an
observer outlives its signal, because the old presenter's `Presented` dies while the `FrameClock`
observing it survives to be `Invalidate()`d and re-seeded. Neither is exotic and neither can be
ordered away.

**`Handle` and `SlotAllocator` do not already answer it**, which is worth stating because they answer
the question that sounds the same. [Decision 15](#15-identity-is-a-generational-handle)'s
generation works because a stale id is resolved *through a live owner*; `IsValid` is answerable
because the allocator is there to be asked. A signal has no third party. The presenter and the
frame's per-output state are peers, the only thing outliving both is the composition root, and
consulting the root on every emit would put the root on the frame path. So the two parties must know
about each other directly. That much is forced; the decision is only about where the knowledge sits.

**Putting it in the observer is what makes the frame-path property structural rather than
remembered.** `Presented` fires inside the frame section, so
[decision 36](#36-frame-path-discipline-is-enforced-mechanically-not-by-review) applies to emit. The
part that is easy to get wrong is *where* it applies: invoking a `std::function` does not allocate,
and constructing one happens at connect time, which is off the frame path and free. What allocates is
the defensive copy of the observer list that nearly every signal implementation makes so that a
disconnect from inside a handler cannot invalidate the iterator. Removing the copy is the whole
design problem, and it is answered by fixing up the live iteration cursors as part of the unlink.

**Immovability is load-bearing and is not an omission.** A move constructor could relink the list; it
could not move the context pointer, which names the observing object. A movable `Connection` would
relink correctly and then call the corpse. The consequence to design around is that observers have
stable addresses — per-output state lives in storage reserved to capacity or indexed by a slot, which
is what [`SlotAllocator`](Structure.md#geometry-is-not-part-of-core) already asks for on its own grounds.

**The reentrancy contract.** A handler may disconnect itself, disconnect another observer, or destroy
the signal. A handler may connect a new observer, and that observer does not fire for the emission in
flight — spelled with a per-connection serial rather than a remembered tail, because a remembered
tail is itself something a disconnect would have to fix up. A handler may re-emit the signal;
emissions nest on the stack. Emission order is connect order.

**Rejected: `std::vector<std::function<>>`.** The default, and it fails on the copy above rather than
on the storage. Everything else it would cost — type-erased indirection, a `std::function` whose
capture crossed the small-object threshold becoming an allocation at a connect nobody was watching —
is secondary to that.

**Rejected: a signal-owned heap node with a movable `Connection` handle.** Admits a bare capturing
lambda as an observer, which reads better at the composition root, and emit still never allocates.
Rejected because disconnect then frees, so an observer destroyed inside the frame section aborts. That
is not reachable today — hotplug is a dispatch-thread udev event and the root sequences migration —
but *currently unreachable* is the weaker kind of guarantee, and decision 36's argument is precisely
about converting that kind into the other. Every observer the design names is a long-lived object that
already exists: `FrameClock`, the frame loop's target cache, `Render`'s device. Not one is naturally a
lambda.

**Rejected: a fixed inline array of `{context, thunk}` pairs.** Gives emit contiguous memory and is
dominated: *the signal dies first* still requires the signal to reach every observer, so the
back-pointers remain, and it adds a capacity nobody can size.

**Rejected: `wl_listener` as-is.** [Decision 2](#2-gyro-owns-the-protocol-seam-libwayland-implements-the-server-codec)
records destroy-listener use-after-free as the best-known bug family in compositors built this way,
and that argument survived as the strongest reason for gyro owning its own object model. The shape
here is the same graph; what differs is that both unlink paths are destructors rather than a call
someone has to remember.

**A signal is claimed by the first thread to emit it, and wiring is deliberately unchecked.**
[Structure.md](Structure.md#orchestration) has signals intra-thread only, and the emit claim makes
that mechanical: a second thread emitting aborts. The obvious companion check — requiring connect and
disconnect on the claimed thread — was written, and it is wrong. It aborts on teardown, because
migration has the composition root destroy the presenter off the frame thread, so `~Signal`
disconnects every observer from there. That is correct code running on every boot, and nothing in the
object distinguishes it from the bug the check was aimed at, since what separates them is whether the
root has quiesced the emitter. Recorded because the check is cheap to re-add and looks like an
oversight until the counterexample is stated.

**What this does not cover.** A signal never crosses the publication boundary; that is
[decision 45](#45-protocol-dispatch-is-a-thread-not-a-task)'s two channels and
[decision 73](#73-the-frame-thread-initiates-reconfiguration-and-never-performs-it) turns on it. And
a signal broadcasts a fact, never a resource: with N observers at most one could take an owned value
and nothing in the signature says which, which is why `ISession`'s device signals carry a borrowed
descriptor rather than an owning one.

---

### 78. `Present()` takes a layer list, and the composite is one member of it

*(Decided 2026-08-17, when `Seam` was written. Resolves a contradiction inside
[Architecture.md](Architecture.md): its `IPresenter` block took a target index and its
[what to build before it is needed](Architecture.md#what-to-build-before-it-is-needed) list said a
layer list, and the two had coexisted since the seam was first written down.)*

`Present()` takes `std::span<const PresentLayer>`, ordered bottom first, and returns `Result<void>`.
A layer carries a source, an acquire point, damage, a source crop, a destination rectangle, a blend
mode, and a color state. The GPU-composited remainder is one more layer in that list rather than a
separate concept, and z is the list order rather than a field.

**What a person gets out of it, since that is what decides whether it is worth the shape.** Plane
offload pays in three places and all three are already promised. It is
[promise 6](Experience.md#doing-nothing-costs-nothing) in its near-idle form — an idle machine
drawing nothing is the easy case, and the hard one is a machine awake at 60 Hz that need not
composite: fullscreen video to a scaling pipe with the GPU powered down, which is hours of battery on
a tablet. It is headroom for [admission control](Architecture.md#admission-control), since a layer
that was offloaded is not in `C`, and *effects give way before frames do* needs something left to
give. And it is most of the reachable win on a weak machine, where the GPU is the thermal budget.
Against that, every promotion is an opportunity to break
[promise 4](Experience.md#the-picture-is-correct) — the picture changing when the machine changes how
it is drawing it — which is why promotion is already conditional on the color pipeline expressing
the same transform, on opacity, and on non-overlap.

**The signature is the cheap half, and it is not the argument.** Widening `Present(index, …)` across
three backends is mechanical. What is not mechanical is the vocabulary underneath it, and every item
leaks into a module that does not exist yet: damage accumulates per *plane* rather than per output,
because `FB_DAMAGE_CLIPS` is a plane property; the composite is *one layer* rather than *the frame*,
or else the remainder-layer framing is a retrofit into code where the composite target is privileged;
a promoted client buffer is held to the next flip and crosses the return channel as a scanout hold,
which nothing accounts for today; and `C` is the cost of what was *not* offloaded. Hand the frame
loop a target index and every one of those will be written on the assumption that a frame is one
image. **That assumption is the thing being prevented, and the signature is only where it is
spelled.**

**Which fields, decided against the rule that argues the other way.**
[Publication/Return.h](../Source/Publication/Return.h) records that a field nobody writes is wrong in
detail by the time somebody writes it, and that rule is right — it argues against speculative fields
and not against the list. So a layer carries what has a writer or a settled rule today. Blend is
there because the **cursor** forces it: it is the one thing gyro promotes that is neither opaque nor
non-overlapping, so *opaque only* is a rule about client content and not a property of the interface.
Color state is there because a promoted layer is what programs a plane's degamma, CTM, and gamma,
and because [decision 47](#47-compositing-happens-in-linear-light-at-wide-primaries) already fixes it
completely — transcription rather than invention. Plane capability descriptors, `TEST_ONLY`
negotiation, and per-plane refusal reporting are not there, because the assigner that would produce
them does not exist and a descriptor rich enough to be an oracle is a descriptor that lies.

**The honest limit, stated rather than papered over.** The only source a layer can name today is an
index into `Targets()`. Naming a promoted *client* buffer is not answerable yet: turning a dmabuf
into a scanout framebuffer is a kernel allocation that must not happen inside the frame section,
while the presenter is frame-side, so the import has no home until plane assignment says where it
lives. That is in [Open.md](Open.md). What it does not cost is this shape — a cursor image and a
virtual output's imported buffers are both targets, so the multi-layer path has callers before a
client is ever promoted.

**The commit reports at the call site, and that is the same decision seen from the other end.** A
commit can fail without blocking: the device was removed, master was revoked, the layer set is not
expressible. `Present()` therefore returns `Result<void>` rather than nothing, so the frame loop
learns where it can still act — fall to the floor tier, skip the output — and so a permanently
failing output cannot masquerade as one that is merely slow. Success means the commit was accepted
and never that anything reached the glass; that arrives as `Presented`, or does not arrive.

**Rejected: a target index now, a layer list when plane assignment lands.** The straightforward
reading of the existing code block, and the one that costs nothing today. It is rejected on the
vocabulary above rather than on the refactor: the modules that would be written in the meantime are
`Frame`, `Render`, and the backends, which is every module that would have to change.

**Rejected: the full field set including plane capabilities and a negotiated refusal.** The other
end of the same axis. It invents the storage for a decision — what a plane can express — that
[Architecture.md](Architecture.md#what-to-build-before-it-is-needed) already says is settled by
`TEST_ONLY` rather than by a descriptor, and it would be a schema written against no assigner.

**Rejected: a separate cursor verb, added now.** [Decision 29](#29-outputs-are-periodic-real-time-tasks-the-test-allocates-effect-budget)
exempts the cursor plane from the budget *because it updates independently of the composite*, which
means there is a commit that is not a frame and this interface has no room for it. That is
[decision 73](#73-the-frame-thread-initiates-reconfiguration-and-never-performs-it)'s lesson arriving
a second time — the seam was over-provisioned for the frame-side things and had no room for the one
that is not — and it is recorded in [Open.md](Open.md) rather than answered here, because the answer
depends on input latency measurements nobody has taken and on whether the cursor is a plane at all on
the floor tier.

---

### 79. The console is a renderer, not a presenter

*(Decided 2026-08-17. Settles the *whether `Console` shares `Seam`'s presenter* entry in
[Structure.md](Structure.md), which had asked whether the pre-Vulkan console is a third
implementation of `IPresenter` or a path beside the seam entirely.)*

Neither. **The axis is who writes the pixels, not who owns the images.** `RenderTarget` exists so
that a writer can bind memory the presenter allocated, and the console's writer is a CPU blitter — so
the console is an `IRenderer` named `Blit`, paired with the same presenter everything else uses.

**The lifetime is what makes this the natural cut rather than a clever one.** The DRM presenter
allocates dumb buffers before Vulkan is up, because that is all it can do without a GPU driver, and
allocates through GBM once it is. That is *the same output* across the transition: the same frame
clock, the same damage accumulation, the same page-flip events feeding the same prediction. What
changes is the writer. A second presentation path would have to grow its own clock, its own damage,
and its own presentation feedback, and would then have to hand all three over at the moment gyro can
least afford a discontinuity — [the firmware handoff](Architecture.md#from-firmware-to-gyro), where
[promise 1](Experience.md#one-continuous-image) says the screen never flashes.

**It is [decision 40](#40-software-rendering-is-a-device-not-a-backend-and-it-is-the-floor-tier)'s shape reused.**
Software rendering is a device rather than a backend for the same reason: the thing that varies is
how pixels are produced, and making it vary along the *presentation* axis instead would fork the
part that is identical. `Blit` is the floor beneath that floor — no Vulkan at all, not even lavapipe
— and it is reached on every boot rather than in an emergency.

**What it costs the seam, and it is the right cost.** `RenderTarget` carries discriminated memory:
dmabuf, or a CPU mapping. A renderer handed the kind it cannot use refuses rather than assumes, which
makes a composition-root miswiring a branch somebody wrote instead of a cast that happens to work.
One field and one check, against a parallel path.

**One consequence to sequence rather than to discover.** [Structure.md](Structure.md) puts `Console`
on its own thread, and [decision 77](#77-a-signals-observers-are-links-the-observers-own) has a
signal claimed by the first thread to emit it. A presenter emitting `Presented` to the console thread
and later to the frame thread would abort on the claim. So the console's presenter instance is
destroyed and rebuilt at the handoff — which is what
[decision 41](#41-device-migration-is-exercised-on-every-boot) already does on every boot, sequenced
by the composition root, and not a new mechanism.

**Rejected: `Console` as a third `IPresenter`.** The reading the open item proposed. It duplicates
the clock, the damage path, and the feedback plumbing, and it puts a handover at the one moment the
picture must not change.

**Rejected: `RenderTarget` opaque, with the writer matched to the presenter by the composition
root.** Tempting, because it keeps the seam narrow: if only the matched pair understands the payload,
the seam does not have to describe two kinds of memory. It is rejected because a seam whose data only
its own pair can read is not a seam — the whole point of the type is that a *substitutable* writer
can bind it, and an opaque handle makes the substitution a convention the root has to get right
silently.

**Rejected: exporting dumb buffers as dmabuf so that one memory kind suffices.** Works on many
drivers and would collapse the variant. It is rejected on where it fails: the console exists
precisely when the GPU driver is not up, so the floor path would depend on an export that can fail on
exactly the configuration the floor exists to serve — and the fallback for that failure is the mapped
path this would have deleted.

### 80. The frame loop is a step; the composition root owns the wait

*(Decided 2026-08-17, while asking what `Frame` may call to wait when
[Structure.md](Structure.md#frame-is-portable) says it is portable and
[Architecture.md](Architecture.md#the-frame-loop) says it waits on `io_uring`.)*

**`Frame` exposes one iteration and returns; the `while` above it belongs to the composition root.**
The loop's body — acquire the snapshot, drain what has arrived, evaluate, record, present, publish
the watermark — is portable and is the whole of the timing policy. The waiting is a platform shim in
`Compositor` whose entire contract is *wake at or after this instant, or earlier when a registered
descriptor is readable*.

**This is a consequence rather than a new choice**, and that is the argument for it.
[Structure.md](Structure.md#orchestration) already has the composition root construct "the clock, the
backend, the rendering device, the publication channel, the scene, the protocol, **and the
threads**." A thread's loop belongs to whoever constructs the thread. Anything else prises the
threads back out of the root one interface at a time.

**The step returns a `Wake`**, which is [decision 69](#69-settling-answers-with-a-wake-idleness-folds-a-monoid-not-an-or)'s
type doing a second job it was already shaped for. The shim arms nothing on `Settled`, one absolute
timeout on `Timed`, and re-arms on `Continuous` without asking the fold again — and
[doing nothing must cost nothing](Architecture.md#doing-nothing-must-cost-nothing) becomes a headless
assertion on a return value rather than an inspection of a real ring. It takes no `now`, because
[Core/Clock.h](../Source/Core/Clock.h) already says the loop reads the clock once per iteration and
`IClock` is handed to it.

**The step is handed no readiness set, and that is the load-bearing half.** Draining a backend's
completions before evaluating is a *correctness* ordering: a `Presented` that lands after
`FrameClock` was read yields a deadline one iteration stale, which surfaces as jitter with no cause.
Put that ordering in the shim and it lives in the one part of the loop the
[schedulability sweep](Architecture.md#outputs-are-independent-periodic-tasks) never executes, which
is precisely the rot [Structure.md](Structure.md#frame-is-portable) makes `Frame` portable to avoid.
So the step drains every source every iteration and the shim is permitted to wake spuriously. It
decides nothing, so it can get nothing wrong.

What that costs is one read returning `EAGAIN` per source per wakeup that some other source caused —
and a source is per *device*, so an ordinary machine has one. Against the tens of microseconds
[Why io_uring](Architecture.md#why-io_uring) already accepts against a 16.6 ms budget, it is not
measurable. If a multi-GPU machine ever makes it so, readiness can be threaded in on the usual gate.

**Which makes the drain a seam member of its own rather than a member of `IPresenter`.** A presenter
is one output's — [decision 78](#78-present-takes-a-layer-list-and-the-composite-is-one-member-of-it)'s
`PresentationInfo` carries no output identity because the presenter already *is* that answer. A
backend's event descriptor is not one output's: KMS has one DRM file per device carrying page-flip
events for every CRTC on it, and the nested backend has one connection carrying feedback for every
window it opened. One descriptor and N presenters, both times. Hung off `IPresenter`, the shim
registers the same file N times and each presenter's drain reads its siblings' events. So
`Seam/EventSource.h` is the backend's, at whatever granularity its descriptor actually has, and the
presenters are what it emits into.

`Descriptor()` may be invalid, and that case is the second argument against a readiness set. A
headless backend's flips are a function of the `ManualClock` a test drives and no file becomes
readable when one falls due; it is drained like anything else and reports what the clock says. A
source with no descriptor could never appear in a readiness set, so headless would present nothing —
on the configuration every scheduling test runs.

**Rejected: a wait interface in `Seam`.** `IWaiter`: arm at an `Instant`, wait, report which sources
fired, with `io_uring` and a test double behind it. It passes
[the fake test](Structure.md#orchestration) and buys no coverage the step does not, since the double
is not `io_uring` either way. What sinks it is that it inverts control: `Frame` would then own the
`while`, and therefore thread lifetime, shutdown, and `SCHED_FIFO` — every one of them platform, and
each needing a further seam to keep the module portable. It also has to re-declare `io_uring`'s
surface in portable terms and widen every time a source kind is added.

**Rejected: a POSIX wait in `Frame`, with `io_uring` as an optimization above it.** `ppoll` is
reachable from the portable tier, so this compiles. It ships two implementations of the wait where
only one ever runs, which is [the floor tier](Architecture.md#the-floor-tier)'s argument inverted —
a path nobody exercises is a path that is broken when it is reached.

**Rejected: the shim drains and then steps.** The smallest version of the readiness set, and it
fails the same way: the ordering that matters is in the untested half.

### 81. A source is pumped by one thread; nested opens one connection, pumped by the frame thread

*(Decided 2026-08-17, as the case that decides whether
[decision 80](#80-the-frame-loop-is-a-step-the-composition-root-owns-the-wait)'s "exactly one thread
pumps a source" is a rule or a preference. Revised 2026-08-22: the rule stands, the nested
application was impossible, and what replaced it is the third alternative this entry originally
rejected.)*

**Every `IEventSource` is pumped by exactly one thread for the whole of its life**, which is what
lets `Drain()` hold no lock at all. That half is untouched by the revision, and it is the half the
rest of the tree leans on.

**Nested is the one backend where that is not free.** gyro is a client of a host compositor, and one
connection carries `wp_presentation_feedback`, which is frame-side, beside `wl_seat` input, which is
dispatch-side. [Structure.md](Structure.md#threads-are-a-second-partition) splits presentation from
input by thread, so one connection *looks* like two threads on one socket.

**So the nested backend opens one connection to the host, and the frame thread pumps it.** *(Revised
2026-08-22; originally two connections, one per thread, which cannot work — see the first rejection
below.)* Presentation feedback is then already on the thread that wants it, and input is handed
across by the nested backend itself.

**Superseded: two connections to the host, one per thread.** *(This entry's original conclusion,
2026-08-17. Retired 2026-08-22 by reading `wayland.xml`, `presentation-time.xml`, and
`xdg-foreign-unstable-v2.xml` against libwayland `1.26.0-9-ged0b9f1` — the same tree
[decision 2](#2-gyro-owns-the-protocol-seam-libwayland-implements-the-server-codec) was read on.)*
**Two `wl_display` connections are two clients, not two views of one.** `wl_client_create` takes a
socket fd and gives that client its own object map (`src/wayland-server.c:585`, `:611`), and an
object argument is resolved in *that* map — `wl_map_lookup(&client->objects, ...)` at `:441`, a miss
posting `invalid object %u` at `:446`. An id means nothing in the other connection.

Both halves then fail, symmetrically:

- **Input never arrives.** `wl_pointer.enter` and `wl_keyboard.enter` each carry an
  `object interface="wl_surface"` argument naming the surface taking focus, and an event goes out on
  its own resource's client: `handle_array` sends through `resource->client->connection`
  (`src/wayland-server.c:254`). The dispatch connection owns no surface, so nothing it binds can ever
  be the focus. What it *would* get is `wl_keyboard.keymap` — an fd and a format, no surface argument
  — and then nothing. A seat that hands over a keymap and never a keystroke is worse than one that
  fails to bind, because it looks like it worked.
- **Feedback cannot be asked for.** `wp_presentation.feedback` takes
  `<arg name="surface" type="object" interface="wl_surface"/>`, so the frame connection would have to
  name a surface the dispatch connection created, and the host answers `invalid object`.

libwayland names the mistake outright, which is the strongest evidence available that no host does
this: `verify_objects` refuses to marshal any event whose object argument belongs to another client
and logs *"compositor bug: The compositor tried to use an object from one client in a '%s.%s' for a
different client"* (`src/wayland-server.c:196–226`). Whichever connection owns the windows owns both
halves, and no protocol moves a surface between clients. `xdg-foreign` is the only thing that crosses
a client boundary at all, and it exports *parenting*: `xdg_imported` has exactly `destroy` and
`set_parent_of`, so an importer can make a window a child of a foreign one and can do nothing else
with it — not receive its input, not ask after its presentation.

**Reinstated: one connection read by the frame thread, input forwarded to dispatch.** *(2026-08-22;
rejected below on 2026-08-17.)* The rejection was that this "puts wire decoding for a client-facing
connection on the frame thread", and that sentence does not survive asking *which* connection. It is
host-facing. It carries gyro's own traffic for gyro's own windows — a surface per output, a configure
on resize, a feedback per output per frame, and one seat — with no untrusted peer on the far end and
no per-client budget to enforce. [Decision 45](#45-protocol-dispatch-is-a-thread-not-a-task) took a
*server* dispatch loop off the frame thread, whose cost is a function of how many clients are running
and what they choose to send; none of that generalises to a bounded stream gyro authored both ends
of. And nested [forces `SCHED_FIFO` off](Architecture.md#nested-wayland), so the decode is not landing
on a real-time thread in the first place. What survives of the rejection is the second sentence — the
cost below — which is not the one it led with.

**The cost, recorded rather than argued away: nested needs a frame-to-dispatch handoff of its own.**
Input decoded frame-side has to reach the dispatch thread, and that is a third channel where
[the design turns on there being two](Architecture.md#the-publication-boundary). Three things bound
it. It is **nested's alone** — owned by the nested backend, not a general mechanism, and nothing
outside that backend may reach for it. It is **not needed until input exists**: there is no input
path in the tree, and until there is, the connection carries feedback and configure and nothing
crosses at all. And when it is built, the shape to reach for first is
[decision 83](#83-dispatchs-publication-is-an-event-source)'s — a queue behind an `IEventSource` the
dispatch thread already drains — which makes it a second instance of a mechanism rather than a second
mechanism, the way the stop path and dispatch's nudge already are. The direction is the harmless one:
frame → dispatch owes no deadline, so a late handoff is a late keystroke rather than a missed frame.

**Rejected: one connection partitioned by event queue.** The known answer, and libwayland-client
implements exactly it, so the price is readable rather than arguable. Against
`1.26.0-9-ged0b9f1`, the same tree [decision 2](#2-gyro-owns-the-protocol-seam-libwayland-implements-the-server-codec)
was read on: `wl_display_prepare_read_queue` takes `display->mutex`
(`src/wayland-client.c:1978`), and a thread that is not the one reading blocks in
`pthread_cond_wait(&display->reader_cond, &display->mutex)` inside `read_events`
(`src/wayland-client.c:1786`) until the reader finishes; `wl_display_dispatch_queue_pending` takes
the same mutex (`src/wayland-client.c:2256`). Per-queue dispatch partitions *delivery* and does not
partition the *connection* — there is one socket, so there is one reader, so there is a lock every
thread meets.

That lock spans the publication boundary and it makes the frame thread wait on the dispatch thread,
which are the two things [Structure.md](Structure.md#orchestration) forbids by name and the priority
inversion [decision 61](#61-the-frame-thread-is-sched_fifo-the-earliest-deadline-first-schedule-is-gyros-not-the-kernels)'s
ordering exists to prevent. gyro writes its own client codec under
[decision 2](#2-gyro-owns-the-protocol-seam-libwayland-implements-the-server-codec), so this is not a
constraint inherited from a library — it is a design gyro would have to reproduce deliberately, and
libwayland is the evidence for what reproducing it costs. *(Revised 2026-08-22: the conclusion drawn
here was "two sockets and a second registry bind are cheaper than one lock on that edge", and two
sockets are not available at any price. The rejection itself stands — one connection with one reader
is what gyro builds, and the reader is a thread rather than a lock.)*

**Rejected: one connection read by the dispatch thread, feedback forwarded to the frame thread.**
Tempting because a channel between the threads already exists, and wrong in direction: the return
channel runs frame → dispatch, so this is a third channel where
[the design turns on there being two](Architecture.md#the-publication-boundary). Nor can it ride the
forward one — the reader takes the newest snapshot and skips the rest, so feedback crossing there
would be *dropped*, and a `FrameClock` that misses observations is the thing `Invalidate()` exists
to represent rather than something to build. It also puts the clock's sole input behind the
scheduling latency of the thread that owes no deadline, which would make `IsPrecise()` a claim gyro
could not keep. *(This is the rejection that decides the direction, and the revision above leaves it
standing untouched: both remaining candidates cost a third channel, and only this one puts it on the
edge with a deadline.)*

### 82. The renderer is handed an evaluated draw list, not a scene

*(Decided 2026-08-21, while asking what `IRenderer::Record` could take that both a Vulkan device and
[decision 79](#79-the-console-is-a-renderer-not-a-presenter)'s `Blit` could be handed, given that the
scene and material vocabularies are [open](Open.md).)*

**What crosses the render seam is a flat span of evaluated draw items, produced by `Frame` into its
own arena and consumed by whichever renderer is bound.** Not a scene, not a snapshot view, and not a
node vocabulary — a list of quads, each with a source, a colour state, an opacity, and a material
name.

**The argument that settles it is that the values have to be evaluated by somebody.**
[Decision 50](#50-the-world-is-authored-on-the-dispatch-thread-the-snapshot-carries-coefficients)
puts spring *coefficients* in the snapshot and evaluation on the frame thread, so a renderer reading
the snapshot directly would either evaluate a second time or read values that are not there. Once
`Frame` is the evaluator, something has to carry what it evaluated, and that something is this.

**It also keeps the two waists independent, which is the part that would have been expensive to
undo.** [Structure.md](Structure.md#two-waists) has `Publication` and `Seam` resting on `Core` and
`Geometry` alone, with the composition root the only thing that knows both sides of either. Routing
frame content through `Publication` types would have given `Seam` an edge to the other waist for the
benefit of one interface, and the edge is the kind that is never removed afterwards.

**The list is a preorder tree in disguise, and the disguise is one integer.** A group item names the
`Count` items following it, which composite into an offscreen and are then drawn as one — which is
[decision 60](#60-group-opacity-requires-flattening-per-node-alpha-is-not-a-group-fade)'s flattening,
and a flat list cannot express it at all. `Count` is the length of the run rather than a child count,
so a renderer finds the members by arithmetic and a nested group's own run sits inside its parent's.
Preorder is free: [decision 55](#55-transforms-are-3d-the-scene-is-a-painters-algorithm) composites
in strict tree order, so the painter's order and the traversal order are the same order.

**A node's transform crosses as its projected quad, with a weight per corner.** The chain that
produced it is a product of decomposed TRS transforms and is not itself one, so it cannot cross as a
`NodeTransform`; [Geometry/NodeTransform.h](../Source/Geometry/NodeTransform.h) says it should not
cross as a matrix either, because the layout and the precision at which translation folds against the
output origin belong to the render path. Four corners is what survives both, it is what
`NodeTransform::Apply` already produces a point at a time, and it is where the double-to-float fold
between global and device space happens. The weights are the accumulated perspective divisors that
`Apply` computes and discards, carried because interpolating a texture across a projected quad
without them is the affine warp early 3D consoles are remembered for.

**One derived fact travels with the quad**: `TransformClass`, the classification of the composed
buffer-to-device map. [Decision 56](#56-clients-render-at-the-ceiling-and-gyro-downscales) makes
minification ordinary and [decision 67](#67-the-settled-snap-is-unconditional) puts settled geometry
on the device grid, so the common case is resample-free and has to take the sharp path. Recovering
that from four floats means comparing floats for equality, which is a guess; the producer knows
because it held the transform that reduced. It is the same reasoning
[Structure.md](Structure.md#geometry-is-not-part-of-core) gives for the predicate having one home at
all — three independently derived answers is how they drift apart.

**What this does not settle, deliberately.** The material catalogue stays empty and the item carries a
name with no parameters beside it, which is
[decision 33](#33-effects-are-named-materials-not-parameterized-filter-calls) held rather than
extended. And nothing here mints a texture: import is dispatch-side, because a `wl_buffer` arrives on
the dispatch thread and must not become a device image inside the frame section, so it belongs to the
renderer's *other* half in the sense `Publication` and `Animation` already split — `Reader` and
`Publisher`, `Solve` and `Author`. That half is written when there is a protocol layer to call it.

**What it costs.** `Frame` walks the scene to build a list a renderer then walks again, and the
renderer cannot see anything above an item — no parent, no subtree except a group's own run. Both are
the price of the item being flat bytes with no lifetime, which is what lets it sit in an arena on the
frame thread and be read by a device that did not build it.

**Rejected: the renderer reads the published snapshot.** The reading
[Structure.md](Structure.md#the-modules) invites, since `Render` already has a `Publication` edge. It
fails on the evaluation argument above before the layering one is reached: the snapshot carries
coefficients, and a renderer that resolved them would be the second evaluator of the same spring.

**Rejected: `Frame` hands over a scene subtree in the shell's node vocabulary.** It would let the
renderer decide grouping and culling for itself. Rejected twice over — `Frame` may not name `Scene`
and the edge is the one [Structure.md](Structure.md#the-modules) says must never be added, and the
vocabulary is [open](Open.md) and wants designing with the material catalogue and the motion
catalogue together. A header written against it now would settle by accident what three documents say
should be settled on purpose.

**Rejected: a flat list, with grouping added when flattening is built.** The cheap version, and it is
[decision 78](#78-present-takes-a-layer-list-and-the-composite-is-one-member-of-it)'s rejected
alternative arriving a second time on the neighbouring interface. Decision 60 is already taken;
widening a flat list into a structured one afterwards touches the frame loop, both renderers, and the
damage path at once, which is every consumer there is.

**Rejected: a blend mode on the item, matching `PresentLayer`.** Symmetry, and wrong here. A presenter
needs one because KMS has a property to program; a renderer has every input the flag would carry — the
alpha mode in the colour state, the opacity, whether the source format has alpha — so the flag would
be a fourth answer able to disagree with the three it came from.

**Rejected: a capability query beside the refusal.** `BindTargets` refuses memory it cannot import,
which is [decision 79](#79-the-console-is-a-renderer-not-a-presenter)'s branch-somebody-wrote. A
`Capabilities()` accessor beside it would answer the same question a second way, and two answers that
can disagree is how a composition root ends up trusting the wrong one.

### 83. Dispatch's publication is an event source

*(Decided 2026-08-21, while writing `Frame`'s step signature and asking what wakes an idle frame
thread. Neither [decision 80](#80-the-frame-loop-is-a-step-the-composition-root-owns-the-wait) nor
Architecture.md's inventory of the frame ring answers it, and the invariant it belongs to had assumed
it.)*

**Dispatch signals a new publication on a descriptor the composition root registers as an
`IEventSource`, beside the backend's.**
[Decision 58](#58-idle-is-a-ladder-gyro-executes-and-does-not-choose)'s invariant is *when nothing is
animating and nothing has committed, no timer is armed and the frame thread blocks indefinitely*, and
the second clause names a commit as the thing that ends the block. But the scene's contribution to the
fold arrives *inside the snapshot*, which only a running frame thread reads, and
[Architecture.md](Architecture.md#the-frame-loop) inventories that thread's ring as timers and KMS. So
a frame thread that folded to `Settled` has nothing to wake it, and the first commit after an idle
output is composited whenever some unrelated wakeup next happens — on a genuinely idle machine, never.
The invariant was stated as though a commit reached the frame thread, and nothing carried it.

**It is a source rather than a mechanism, and that is the whole of the argument.** Decision 80 already
has the shim register descriptors it knows nothing about and the step drain every source on every
iteration, so a nudge fits that shape with no new verb, no new ordering, and no change to the step's
signature. `Descriptor()` is an eventfd, `Drain()` reads the counter, and the snapshot acquire that
already follows the drain is what picks up the publication. The drain-before-evaluate ordering
decision 80 makes load-bearing for `Presented` is the same ordering this needs, for the same reason
and at no additional cost.

**The direction of the descriptor is what keeps
[decision 61](#61-the-frame-thread-is-sched_fifo-the-earliest-deadline-first-schedule-is-gyros-not-the-kernels)'s
priority order intact.** Dispatch writes and the frame thread reads. An `eventfd` write is a counter
increment that never blocks on the reader, so the higher-priority thread never waits on the lower one
— the property [decision 81](#81-a-source-is-pumped-by-one-thread-nested-opens-one-connection-pumped-by-the-frame-thread)
rejected a shared connection for violating, arriving here for free because this channel carries no
data at all. Nothing is read back across it: the descriptor says *something was published* and the
ring says what. It is therefore not the third channel
[the publication boundary](Architecture.md#the-publication-boundary) forbids, which carries state; it
is the doorbell on a channel that already exists.

**Signalling unconditionally is correct, and it costs nothing where the cost would matter.** A nudge
arriving while the frame thread is already running wakes it ahead of its timer, and the step then
assesses and skips — which decision 80 licenses in as many words, since the shim is permitted to wake
spuriously and the timing decision belongs to `Assess` rather than to the wakeup. The waste is bounded
by dispatch's own iteration rate, and it is *zero* in the case the invariant is about: an idle machine
publishes nothing, so it writes nothing, so it wakes nothing. **The invariant survives verbatim**,
which is why the conditional form below is an optimisation rather than the design.

**Deferred: signalling only when the frame thread would not otherwise hear it.** The return channel
already runs frame → dispatch and `FrameReport` already carries a spare word, so the step could report
the `Wake` it returned and dispatch could write only against a `Settled` one. It is a real saving on a
busy system, and it introduces a lost wakeup: dispatch may read *not idle*, publish, and decline to
signal, all inside the window before the frame thread posts its idle report and sleeps. Closing it
means the step re-checking the ring after posting and before returning `Never()` — the ordinary
double-check, wait-free here, and cheap. Worth doing when the waste has been measured rather than
assumed, and recorded now because the hazard is invisible from the optimisation. Carried in
[Open.md](Open.md).

**Rejected: the frame thread polls the ring on a floor cadence.** A minimum wakeup rate — ten hertz,
one hertz — needs no descriptor at all, and is what a system with no idle ambitions does. It does not
serve decision 58's invariant, it abandons it: *no timer armed* becomes *a timer always armed*, and
every idle machine pays that forever to save one descriptor on the busy ones.

**Rejected: dispatch arms the frame thread's timer directly.** Dispatch authored the wake, so it could
set the timeout. That puts a frame-thread scheduling decision on the dispatch thread, which is where
decision 80 refuses to put the drain ordering and for the same reason — and it requires the shim to
expose an arm-from-elsewhere verb, which is the wait interface that decision rejected, reached from
the other end.

**Rejected: folding the nudge into the backend's source.** One descriptor fewer, by having the backend
own an eventfd dispatch also writes. It breaks
[decision 81](#81-a-source-is-pumped-by-one-thread-nested-opens-one-connection-pumped-by-the-frame-thread)'s rule that a source
has one writer as surely as one reader, and it makes a headless backend — whose flips are a function
of a `ManualClock` — the owner of a channel that has nothing to do with presentation.

### 84. The snapshot's per-output run is indexed under a set generation

*(Decided 2026-08-21, while asking what binds the snapshot's positional wake schedule to an output
set.)*

**The snapshot header carries the generation of the output set it was authored against, and the frame
loop indexes no per-output run whose generation is not its own.** Per-output data crosses positionally
— [Snapshot.h](../Source/Publication/Snapshot.h) already spells the wake schedule as one `Wake` per
output in output order — and position is the right identity: the fold
[decision 69](#69-settling-answers-with-a-wake-idleness-folds-a-monoid-not-an-or) describes happens on
the dispatch side, per output, before publication, so `Wake` needs no output field and `Sooner` stays
a reduction over values that carry none. What position lacks is any statement that the two sides mean
the same outputs by it.

**The failure this prevents is the silent one, which is why it is a decision rather than a bounds
check.** A run that is too short is caught by `span::size()`, and one that is too long is harmless. A
run that is *the same length* against a different set is neither: `Wakes()[2]` is then another
output's schedule, and the result is an output that arms nothing and never repaints, or one that never
folds to settled and never idles — the two failures
[decision 58](#58-idle-is-a-ladder-gyro-executes-and-does-not-choose)'s invariant exists to prevent,
arriving by the one path the invariant cannot see. Hotplug produces exactly that shape, since one
output replacing another leaves the count alone.

**Generation is already this codebase's answer to this class, and this is a third instance rather than
a third mechanism.** [Budget](../Source/Frame/Budget.h) drops a GPU cost whose generation has moved,
because a timestamp outlives the configuration it was taken under;
`OutputConfiguration::Generation` makes a late reconfiguration completion legible for
[decision 73](#73-the-frame-thread-initiates-reconfiguration-and-never-performs-it), because two
requests can be outstanding at once. Both are authored by the side that changes and checked by the
side that consumes, and both are cheap because the check is an integer comparison at the point of use.

**The two generations answer different questions and both are wanted.** The set generation is the
header's and answers *do these runs mean my outputs at all*; decision 73's is per output and answers
*is this output's mode request newer than what I have achieved*. Merging them would have every mode
change renumber the world, and keeping only the per-output one would leave the index unguarded — a
generation attached to slot 2 is a statement about slot 2's occupant and not about who occupies it. So
the per-output run carries the wake and the reconfiguration generation together, one record rather
than two runs indexed the same way and kept in agreement by hand, and the header carries the set
generation above both.

**`SnapshotHeader::Reserved` is where the set generation goes**, which is the four bytes that header
already spells out rather than leaves as implicit padding — reserved for the reason reserved fields
exist, and spendable without moving a field or bumping `SnapshotVersion`, since nothing reads it today
and both halves of the boundary ship together.

**A mismatch is no information, not partial information.** The loop treats the scene as contributing
nothing for every output — arms nothing from the scene side, renders nothing new — rather than
indexing as far as the shorter run allows. The asymmetry is the argument. An output that misses one
iteration's animation is invisible, because the window is the interval between dispatch renumbering
and the loop adopting the matching set, decision 73's handshake is what closes it, and
[decision 83](#83-dispatchs-publication-is-an-event-source)'s source guarantees the loop is awake to
see the snapshot that matches. An output that reads its neighbour's schedule is a bug that survives
review.

**Rejected: an output identity on `Wake`.** It puts the identity in the element, gives one fact two
spellings, and widens a `Core` type for a single consumer. `Sooner` folds wakes; identity does not
fold, so it would be a field every reduction in the system has to drop.

**Rejected: a stable per-output handle in each record, matched rather than indexed.** The stronger
form, and `Handle` is already in `Core` with the generational checking to do it. It answers no more
than the set generation does — a record whose handle the loop does not recognise leaves it exactly
where a mismatch does, holding nothing it can use — and it pays per element what a generation pays per
snapshot, turning every index into a lookup on the frame path. Worth revisiting if the run ever needs
to be sparse, which today it does not: every output the loop holds has a slot.

**Rejected: publishing the output set itself and diffing it.** Fully self-describing, and it removes
the shared numbering rather than guarding it. It puts a comparison shaped like an allocation inside the
frame section to serve a fact that changes on hotplug, which makes every iteration pay for the rare
one — the trade [decision 50](#50-the-world-is-authored-on-the-dispatch-thread-the-snapshot-carries-coefficients)
already refuses when it puts coefficients rather than values on the wire.

### 86. The published scene is a preorder tree; model values inline, coefficients by reference

*(Decided 2026-08-22, on asking what `Scene` actually produces.
[Decision 50](#50-the-world-is-authored-on-the-dispatch-thread-the-snapshot-carries-coefficients)
settled the animation half of the publication boundary and was silent about the structural half.)*

**The snapshot carries the scene's topology, as a preorder run of node records each naming the length
of its own subtree.** Decision 50 put spring coefficients on the wire and evaluation on the frame
thread; it never said what those coefficients are coefficients *of*.
[Snapshot.h](../Source/Publication/Snapshot.h) shows the gap plainly — three coefficient runs and a
wake schedule, and nothing from which a `DrawItem` could be built. This is the other half of that
decision, taken four decisions later because nothing needed it until there was a `Scene`.

**The flat alternative is not merely worse; it is unauthorable, and one field proves it.**
[Decision 82](#82-the-renderer-is-handed-an-evaluated-draw-list-not-a-scene)'s `DrawGroup::Count` is
a run length over *emitted* items, and the emitted list is post-cull: back faces go by
`NodeTransform::FacesViewer` over the composed chain — **the signed area of the projected quad, per
[decision 93](#93-the-quad-is-assembled-in-frame-and-the-back-face-is-the-signed-area), because
that predicate reads one node and the answers do not multiply** *(revised 2026-08-22)* — and anything
wholly outside the target is gone before the list is built. Both tests are post-evaluation and per instance. So emission depends on
evaluation, `Count` depends on emission, and the dispatch thread — which by decision 50 evaluates
nothing — cannot compute it. The repair is to publish skeletons and let the frame thread fix the
counts up after culling, at which point dispatch has published a tree with extra steps *and* a second
representation that can disagree with the first, which is exactly what
[decision 16](#16-nodes-own-their-properties-the-active-set-is-mirrored)'s "derived, never
maintained" exists to refuse.

Three things that read as arguments for flattening dissolve the same way. A group's offscreen sits at
its subtree's **screen-space** bound, which is evaluated and per instance, so dispatch declares which
subtrees are groups and never places them. **Ordering is the only thing a dispatch-side flatten
actually saves**, and [decision 55](#55-transforms-are-3d-the-scene-is-a-painters-algorithm)'s strict
tree order makes it free on whichever side does it. And `TimeScale`
([decision 19](#19-hierarchical-time-is-a-per-subtree-timescale-only)) composes down the tree, which
is a traversal it gets for nothing once there is a traversal to hang it on.

**Preorder plus subtree length, rather than parent indices.** The frame side's walk is then a linear
scan over a contiguous run with an explicit transform stack, and skipping a hidden or wholly-culled
subtree is an addition rather than a test per node — which is what a scene with one workspace visible
out of nine costs on every frame. Parent indices are equally pointer-free and cannot express that
skip. It is also the encoding `DrawGroup::Count` already uses, so the source tree and the emitted list
are read the same way rather than in two idioms; the asymmetry that makes the length legal here and
illegal there is that a structural length counts **nodes**, which dispatch knows, and not emissions,
which it does not.

**A node names each channel by index where it is moving and carries it inline where it is not.** The
coefficient runs are the *active* set — that is decision 16 as decision 50 re-derived it, and the
cache-friendly walk over only what moves is the whole of what it buys — so a node cannot simply hold
six run indices, because most nodes at most instants have no active channel at all. What it holds per
channel is an index or a sentinel, and beside it the settled value. That is
[model versus presentation](Animation.md#model-versus-presentation) crossing the boundary in its own
shape: the model value inline, the coefficients by reference, and for a channel at rest the two agree
by construction because a settled spring *is* its model value.

**Rejected: a spring per channel on every node**, making the runs dense and the indices implicit. It
is simpler at the reader and it makes the run length `O(nodes × channels)` on a scene where almost
nothing is animating, which gives up the one property the active set exists for and makes a still
desktop cost the same to publish as a moving one — on the eager-per-commit path
[decision 50](#50-the-world-is-authored-on-the-dispatch-thread-the-snapshot-carries-coefficients)
leaves open at its foot.

**Rejected: parent indices, matched rather than nested.** The ordinary flat-tree encoding, and it
loses nothing except the subtree skip — which is the operation the frame thread performs most and the
only one whose cost scales with what is *not* on screen.

**Rejected: a flattened skeleton list, per the `Count` argument above.** Recorded rather than merely
absent because it is the shape the render seam invites: `IRenderer::Record` takes a flat span, so a
snapshot carrying the same thing looks like it would save `Frame` a pass. It would, and it cannot be
built.

**Two of these did not survive contact.** *(Revised 2026-08-22.)*
[Decision 90](#90-the-snapshots-runs-are-one-per-channel-and-the-frame-side-validates-the-tree-it-walks)
builds the run and changes two things here. The runs a node indexes into are now **one per channel**
rather than the two this decision inherited, because scale and rotation are three-component and
opacity is not, so the single-precision run could not be one stride — and the finer split is what
makes a channel index unambiguous rather than merely unpadded. And *a settled spring is its model
value* is true of every channel except the one it was most needed for: a rotation is sprung over a
deviation anchored at its target, so a settled one is zero and carries no orientation. The rule
becomes **a node carries whatever reconstitutes its value**, of which inline-when-settled is the
common case. The topology also moves out of `Runs` into a named header entry, since it is not a
channel and an index that could reach it would defeat the paragraph above.

**Consequences.** `SnapshotRun` grows a node run now and gains client damage and
[decision 46](#46-exit-snapshots-come-from-a-pre-reserved-per-output-atlas-exhaustion-finishes-exits-early)'s
capture requests later; `SnapshotVersion` bumps, which is free while both halves of the boundary still
ship together. `Frame` walks a tree to build a list a renderer walks again — decision 82 already
accepted that cost and this is where the first of the two walks comes from.

### 87. A type both halves of the world name lives below both waists, not in `Seam`

*(Decided 2026-08-22, on reading what a `DrawItem` carries against who produces each of its fields.)*

**`TextureId` and `ColorState` move to `Core`; `Scene` declares its own output record and the
composition root converts.** The rule is
[Structure.md](Structure.md#region-is-in-geometry-and-reachability-is-why)'s and is already load
bearing once — *a type that a dispatch-side module and a frame-side module must both name lives in
`Core`, `Geometry`, or a waist both can reach, never in `Seam`* — and what this decision adds is that
the rule has three more instances and resolves them differently.

**The reading that found it.** Take
[decision 82](#82-the-renderer-is-handed-an-evaluated-draw-list-not-a-scene)'s `DrawItem` field by
field and ask who produces the value. `Quad`, `Sampling`, `Opacity`, and `Radius` are frame-derived,
which is the decision working. `TextureId` is an import's identity, `ColorState` is what the client
declared, and `Material` is what the shell named — all three authored on the dispatch side, all three
declared in `Seam`. `Scene` depends on `Core`, `Geometry`, `Animation`, and `Publication`; `Protocol`
on `Core`, `Geometry`, and `Scene`. Neither may say `Seam`, so `CheckLayering.cmake` fails the day
`Scene` stores any of them, and its report names a missing edge rather than this paragraph.

**`TextureId` goes to `Core`, and it unblocks a hole rather than merely avoiding one.**
[Open.md](Open.md)'s *how a texture is minted, and who holds it* has import happening dispatch-side
because a `wl_buffer` must not become a device image inside the frame section — and today neither
`Protocol` nor `Scene` can name the type such an import would return. The entry has no legal caller,
not merely no design, and nobody had noticed because nothing has called it. The move is nearly
notional besides: it is `Core/Handle.h`'s generational handle in everything but its declaration.

**`ColorState` goes to `Core` as well, and `Geometry` is the wrong answer for it.** `Region` landed
in `Geometry` because it is rect arithmetic over the coordinate spaces and `Rect<S, T>` was already
there; a colour state is not geometry, and putting it there would quietly redefine that module as
*domain content that is not `Core`* rather than what
[Structure.md](Structure.md#geometry-is-not-part-of-core) says it is. It meets `Core`'s own test
instead: it depends on nothing, it names no other domain's vocabulary, and it has more than one caller
before it has two implementations. [ColorState.h](../Source/Core/ColorState.h)'s own header said it
sat in `Seam` "because both halves of the seam need it and neither owns it" — that sentence stays
true and turns out to have named the wrong pair of halves.

**`OutputConfiguration` is translated rather than relocated, and the difference is the point.**
`Scene` needs an output model — [decision 69](#69-settling-answers-with-a-wake-idleness-folds-a-monoid-not-an-or)'s
wake fold is per output and happens dispatch-side before publication,
[decision 67](#67-the-settled-snap-is-unconditional)'s snap is to an output's device grid, and
[decision 32](#32-a-surfaces-frame-cadence-follows-its-fastest-output)'s cadence needs to know which
outputs a surface intersects. Almost none of what `OutputConfiguration` carries serves any of that:
it is what `Reconfigure` asks for and `Reconfigured` reports achieved, a negotiation between the frame
thread and a backend. So `Scene` declares the record it actually wants — identity, the set generation,
the global rectangle, the exact scale, the device grid — and the composition root fills it in, which
it may do because it is already the only thing that knows both sides of both waists.

**Why not translate all of them, since translation is available.** Because it is right for a fact that
changes on hotplug and wrong for one that changes per commit. A texture identity is per surface and
per frame, so a translation layer is a map maintained and consulted on the frame path, which is an
allocation and a lookup where neither is permitted. The axis is how often the fact moves, not how
awkward the type is to relocate.

**Rejected: `Scene` stores an opaque `uint32_t` and the root reconstitutes it.** It satisfies the
build check and gives one fact two spellings, which is the objection
[decision 82](#82-the-renderer-is-handed-an-evaluated-draw-list-not-a-scene) already made to a blend
flag and [decision 84](#84-the-snapshots-per-output-run-is-indexed-under-a-set-generation) to an
output identity on `Wake`. It also puts the reconstitution somewhere, and every candidate is on the
frame path.

**Rejected: giving `Scene` a `Seam` edge.** One line in `gyro_add_module` and the rule stops being
true. `Seam` is on the frame side's edge set and not on the world's, and what would be given up is the
composition root being the only thing that knows both sides of a waist — the property that lets a
presenter be swapped during device migration without the scene noticing.

**Left open: where `Material` lives**, and it is the one that may cost a module. It is the shell's
vocabulary, so it plausibly belongs to `Scene` itself — and `Seam` cannot depend on `Scene`, which
makes it the first of these four that neither relocation nor translation obviously answers. It is
[open](Open.md) alongside the material vocabulary it names, since deciding what is in the set and
deciding where the set lives are close enough to be one reading. A fourth base module below both
waists is the shape that would resolve it, and one header is not enough reason to create one.

**The module exists, and it was the node record that paid for it.** *(Revised 2026-08-22.)*
[Decision 91](#91-the-worlds-vocabulary-is-a-module-of-its-own-below-both-waists) creates `World` on
the strength of a record `Frame` must walk and `Scene` must write, which is this entry's rule one
level up and is unimplementable rather than merely homeless. Half of what was bundled here comes
apart with it: *where* `Material` lives is answered, and *what is in the set* is still open. The
bundling was a consequence of there being no home rather than a real coupling between the two
questions, and `Material` stays in `Seam` until the vocabulary lands because moving it before then
would be a move nobody could check.

### 90. The snapshot's runs are one per channel, and the frame side validates the tree it walks

*(Decided 2026-08-22, building the node run
[decision 86](#86-the-published-scene-is-a-preorder-tree-model-values-inline-coefficients-by-reference)
specified. Three things that decision left implicit, one of which amends
[decision 88](#88-an-instance-is-a-node-the-published-scene-is-a-dag).)*

**One coefficient run per channel, and the count follows the channel set rather than the other way
round.** [Snapshot.h](../Source/Publication/Snapshot.h) carried three runs: translation springs at
double, "everything else" at single, and the driven ramp. The middle one was never a decision to
pack. It was written when everything else was assumed to be scalar, and
[decision 17](#17-transforms-are-decomposed-into-trs-with-per-channel-springs) makes scale and the
rotation log map three-component — so `Spring<Vector3<float>>` is fifty-six bytes against
`Spring<float>`'s thirty-two, and one homogeneous run holding both is either impossible or padded.

Padding is the wrong direction on the merits rather than merely on the bytes. Opacity is the most
animated channel in the system — every enter, every exit, every cross-fade — and
[decision 71](#71-reduced-motion-is-three-named-forms-not-a-per-bundle-reduced-table) makes fades the
*substitute* for movement, so for a reader who has asked for reduced motion it is very nearly the only
channel that ever moves. Taxing it seventy-five percent to share a stride with scale spends the most
on the population decision 71 exists to serve.

Nothing pushes back, because **a run costs one sixteen-byte directory entry**. There is no
allocation, no indirection, and no lookup — the reader indexes a fixed array in the header. So the
rule is one run per channel, and adding a channel is adding a run rather than re-striding one that
already works, which is how blur and corner radius will arrive when the material vocabulary
[Open.md](Open.md) holds open is settled.

Two channels of the same element type still get their own run, and that case is what makes the rule
per *channel* rather than per element type: scale and rotation are both `Spring<Vector3<float>>`, so
merged, an index meant for one would resolve happily against the other and nothing downstream could
tell. Separate, **a node's channel index is a position within its own channel's array**, and pointing
a scale index at a rotation spring stops being expressible rather than being caught.

**`Vector3` was not a `SpringValue`, which is why none of this had come up.** The solver asks its
value type for a whole-vector magnitude, and only the scalar spelling existed —
[Retarget.Test.cpp](../Source/Animation/Author/Retarget.Test.cpp) exercised the vector path through a
locally defined stand-in, so the shape was proven and the real type had never been asked. Geometry
gains `Magnitude(Vector3<T>)` beside its own `Length`, and the adapter sits there rather than as a
trait inside Animation because the concept reaches it by argument-dependent lookup and
[Structure.md](Structure.md) forbids the frame half of Animation from naming Geometry. Two names for
one number, which is the cheaper of the two prices available.

**The topology is not a channel, so it is a named header entry rather than a sixth run.** `Runs` is
indexed by channel and that index is what a node record holds; an entry in it that is not a channel
would make the scene reachable by an index a node could name. `Nodes` sits beside the wake schedule
instead, addressed by name.

**A node must carry whatever reconstitutes its value, and inline-when-settled is the common case of
that rule rather than the rule.** Decision 86 says a node names a channel by index where it is moving
and carries the value inline where it is not, "and for a channel at rest the two agree by construction
because a settled spring *is* its model value." That holds for three channels and fails for rotation.
[Animation.md](Animation.md#transforms) anchors the log map **at the target**, so a rotation
spring is over a deviation whose target is zero: a settled one reads `(0, 0)` and says nothing about
which way the node faces. The orientation is the chart's base point and has to cross unconditionally,
active or not. Stated as availability rather than as a storage trick, the rule survives the next
chart-valued channel; stated as decision 86 stated it, the exception is invisible at rest and appears
only once something rotates.

**The frame thread validates the tree as it walks it, and decision 88's guarantee is not enough on its
own.** Decision 88 puts acyclicity and bounded reference depth dispatch-side, "like every other
invariant the authoring side owes the frame side", on the grounds that the frame thread cannot afford
to detect a cycle. But the file it would be walking already refuses to trust its writer:
`RunWithinBounds` exists so that a corrupt or truncated mapping yields an empty run rather than a read
past the end, precisely because [decision 49](#49-the-restart-boundary-is-made-cheap-where-it-can-be-and-stated-where-it-cannot) holds open
dispatch becoming a separate *process*.

The asymmetry is in what the two failures cost. A bad run offset is a garbage read. A cycle, or a
subtree length that overruns, is an **unbounded walk inside the frame section on a `SCHED_FIFO`
thread in the only compositor the machine has** — nothing preempts it, and what saves the box is
[decision 22](#22-gyro-runs-as-a-dedicated-unprivileged-uid-with-cap_sys_nice-and-nothing-else)'s
`RLIMIT_RTTIME` killing gyro, which takes every session's UI with it. That is the most expensive
failure in the system, and it is the one being taken on trust. So the walk carries a depth counter
against a cap and checks each subtree length against what remains in the run: one comparison per node,
on a walk that already does one, and the cost becomes bounded by construction rather than by promise.
A malformed tree then draws nothing where the bad subtree was — a missing thumbnail rather than a dead
session, which is the same conservative direction as every other ingest in the codebase. Decision 88's
dispatch-side guarantee stays; it stops being the only thing standing between a bug and the machine.

**The depth counter is not the bound, and the arena is.** *(Revised 2026-08-22.)*
[Decision 95](#95-the-scene-vocabulary-is-four-kinds-a-material-is-a-field-not-a-kind) found the hole
while encoding the reference node: depth bounds a *tree* and the scene is a DAG, so sixty-four
containers each holding two references to the one before them sit at depth sixty-four and emit 2⁶⁴
items — this paragraph's own failure, reached through breadth rather than through a cycle and reached
inside the cap rather than by exceeding it. What actually bounds the walk is decision 82's arena,
sized by the admitted plan, and the walk stopping when it is full. The depth counter and the
subtree-length check stay; they were never the whole of it.

**Rejected: springs inline in the node record**, the denormalized form, which needs no indices at all.
Four springs inline is about two hundred and forty bytes per node, always, where a settled node needs
about fifty-six — half a megabyte per snapshot against a couple hundred kilobytes on a scene of a few
thousand nodes, re-emitted as often as input arrives. It is also worse on the *hot* path, which is the
counterintuitive half: the common case is a node that is not moving, and inline that means dragging
240 bytes through cache to use 56 of them. The narrow record wins the walk, not only the wire. The
trade is two cache lines instead of one for a node that *is* moving, which is the right side of it,
because moving is a handful of nodes and not-moving is all of them.

**Rejected: springs carrying their owner's node index**, so the frame thread streams each channel
array end to end into a scratch slot per node. Tighter loops, no sentinel test, trivially vectorised —
and it gives up the subtree skip, because it would evaluate every spring in a hidden workspace before
the walk ever discovered the workspace was hidden. The skip is the whole reason the tree carries
subtree lengths, so evaluation has to be *driven by* the walk to inherit it. **That is what fixes the
direction of the reference**: the node reaches for the spring, and not the other way round. Worth
recording because the inverse is the faster-looking shape and the reason it loses is not performance
at the loop level.

**Rejected: one run per element type** rather than per channel. Strictly fewer runs, and it merges
exactly the two channels — scale and rotation — whose confusion an index cannot detect.

**Consequences.** `SnapshotVersion` bumps to 2, which is free while both halves ship together.
`SnapshotRun`'s enumerators are the channel set, and the order is part of the format rather than a
convenience, since a node record names a channel by that index. The node record itself is not defined
here: the waist reserves the slot without naming the record, exactly as it does for decision 72's
driven ramp, because what a node *is* waits on the vocabulary Open.md holds open.

**The record is `World`'s, and a node names its channels by slot rather than by that index.**
*(Revised 2026-08-22.)*
[Decision 91](#91-the-worlds-vocabulary-is-a-module-of-its-own-below-both-waists) places it: `Scene`
writes it, and the validating walk above means `Frame` reads it, so it belongs below both waists.
The sentence here that a node *names a channel by that index* is true of what a slot means and not of
how it is spelled — the record carries one named slot per channel rather than an array `SnapshotRun`
indexes, because `World` may not reach the waist's schema and a second spelling of the run order is
what this entry's own argument against merged runs exists to avoid. What still waits on the vocabulary
is what a node *carries*, not where the record lives.

**And a debt, recorded rather than paid.** Decision 50 says the copy-on-write arena — share unchanged
subtrees between consecutive snapshots so a gesture does not re-serialise the scene per input event —
is "contained", because the representation is offset-addressed either way. That was written before the
scene had a relational encoding. A shared, unchanged subtree still holds indices into channel arrays
whose packing is global, so one node elsewhere becoming active can renumber a run and invalidate a
subtree that did not change. There is a fork nobody has had to take yet: **node-ordered runs**, which
are what make the frame thread's reads monotonic, against **append-ordered runs**, which keep indices
valid across publications and give that up. Inline springs would have had no such problem, and that is
the one real argument in their favour. It does not change the answer — decision 50's own instruction
is to ship the full re-emit and measure — but the arena is less contained than it was, and whoever
builds it should find that here rather than discover it.

### 91. The world's vocabulary is a module of its own, below both waists

*(Decided 2026-08-22, on trying to write the node record
[decision 90](#90-the-snapshots-runs-are-one-per-channel-and-the-frame-side-validates-the-tree-it-walks)
had reserved the slot for. Takes
[decision 87](#87-a-type-both-halves-of-the-world-name-lives-below-both-waists-not-in-seam)'s deferred
fourth module.)*

**`World` is created, on `Geometry` alone, and it holds the published node record.**
[Snapshot.h](../Source/Publication/Snapshot.h) said the record was `Scene`'s to define. That is a
sentence `CheckLayering.cmake` fails the day it becomes true: decision 90 puts the tree's validation
on the frame thread's *own walk*, so `Frame` reads these fields, and
[Structure.md](Structure.md#the-modules) calls the absent `Frame`-to-`Scene` edge the one thing in
that document worth enforcing rather than describing. Decision 87's rule settles where such a type
goes — below both waists, never in `Seam` — and the record is that rule one level up.

**The parallel the waist drew for itself breaks in a specific place, and that is what says where the
fix goes.** Publication carries spring coefficients without naming `Spring`, and the arrangement works
on two legs: the waist names a run rather than an element, *and* both namers can reach the element's
home, because `Animation` is below both of them. Only the second leg fails for the node record.
`Scene` is not below anything. So the waist is already right and needs no change — `PutNodes` and
`Nodes<T>()` are templates exactly as `Put` and `Run<T>` are, and `Publication` keeps its edge set at
`Core` and `Geometry`.

**`Core` is not available, and the reason is its own test.** A node record has to name a transform and
an extent, which are `Geometry`'s, and `Core` may not say `Geometry`. That is what makes this a module
rather than a header move — decision 87 could send `TextureId` and `ColorState` to `Core` precisely
because neither of them needs a coordinate.

**They stay in `Core`, and the line is worth stating before somebody asks.** Decision 87 put them
there because `Core` was the only legal home, and creating `World` removes that reason without
changing the answer: `Core` holds primitives that happen to be world-visible — a handle, a pixel
encoding, a texture — and `World` holds the authored vocabulary, what a node *is* and what it is
dressed in. The mechanical test *does it need `Geometry`* happens to sort all of them the same way,
which is reassuring and is not the rule; a future world type of pure scalars would sort wrong.

**The record carries what the walk needs and nothing that waits on the vocabulary.** Decisions 86, 90,
19, and 55 pin a subtree length, a per-channel index-or-sentinel beside the model value, the
orientation as the chart's base point, the anchor and projection, the extent the bounding radius is
taken over, a time scale, and the hidden and group declarations. A node *kind*, a material, a texture,
a color state, and the encoding of decision 88's subtree reference are all the shell's scene
vocabulary, which [Open.md](Open.md) holds open — and that list is not merely undesigned, it is known
wrong: [decision 51](#51-the-shell-is-a-per-session-client-gyro-owns-mechanism)'s four kinds do not contain the
reference node decision 88 added. So the kind enum follows `Material` rather than shipping beside the
record, on the same reasoning: a set nobody has designed is a set that will be replaced, and both are
one field in a record that has no callers.

**The vocabulary landed and the record is complete.** *(Revised 2026-08-22.)*
[Decision 95](#95-the-scene-vocabulary-is-four-kinds-a-material-is-a-field-not-a-kind) settles the
set the paragraph above defers to, so the four fields it names arrive: a kind, a material, an
elevation, and one content index whose run the kind selects. The reasoning here holds — the enum did
wait for a design rather than shipping as a placeholder — and what changed is that decision 51's list
turned out to be wrong on its axis rather than merely short, which is what made settling it urgent
instead of tidy.

**The channel slots are named rather than an array indexed by `SnapshotRun`.** That enum is the
waist's schema, which [Snapshot.h](../Source/Publication/Snapshot.h) argues belongs at the waist, and
`World` may not reach it — so an array here would carry a second spelling of the run order, which is
the objection decision 87 made to giving one fact two spellings. Named, the correspondence is nominal
and the compiler checks it at the site that asks for the run. Nothing is lost with it: reconstitution
differs per channel anyway, since rotation goes back through `FromDeviation` and no other channel
does, so there was never a generic loop to write.

**Rejected: `Geometry` takes the record.** It is the cheapest-looking answer, since `Geometry` is
already below everything and already holds the transform. `Scale`, `Space`, and `Region` are the
coordinate grid and its reachability; *what a node is* is not that, and admitting it makes `Geometry`
the home for anything that happens to need a `Vector3`.

**Rejected: `Frame` declares its own node record and the composition root converts** — decision 87's
`OutputConfiguration` move, applied one level up. It dies on decision 87's own axis, which is how
often the fact moves: an output configuration changes on hotplug, and a node record changes per node
per frame, so the conversion is a pass over the whole scene on the frame path and a second layout that
can disagree with the wire. That second half is the objection
[decision 86](#86-the-published-scene-is-a-preorder-tree-model-values-inline-coefficients-by-reference)
already made to publishing skeletons and fixing them up after culling.

**Rejected: `Publication` declares the record.** It is legal — the waist reaches `Geometry` — and it
gives up the property the waist was built for. A field added to a node would then be a change to the
boundary both threads bind to, and `SnapshotVersion` would move for a reason that has nothing to do
with the format.

**Rejected: waiting until `Scene` has callers.** [Decision 69](#69-settling-answers-with-a-wake-idleness-folds-a-monoid-not-an-or)'s
trigger is not the size of the question but the size of what has been built on the current answer, and
it is at zero. Decision 87 deferred this because one homeless header was not enough reason to create a
module; what changed is not that the count went up but that the next commit could not be written
without it.

**Consequences.** `Structure.md`'s graph gains a node between the base and the waists. `World` sits
below `Seam` rather than beside it the moment `Material` moves, because `DrawItem` carries one — so
the position is forced by decision 87's third field rather than chosen. `SnapshotVersion` does not
move: the header already reserved `Nodes`, and what changed is that something now writes and reads it.
[Open.md](Open.md)'s *where `Material` lives* is answered and its *what is in the set* is not, which is
the untying decision 87 could not do because there was no home to untie it from.

### 111. An entity is a node's authoring side; the store is one tree

*(Decided 2026-08-22, on asking what `Scene` **is** rather than what it produces. Decisions 86, 87,
and 88 were one commit and they settled the published form; none of them said what holds it.)*

**There is one kind of object. An entity is what the dispatch side holds, a
[`Node`](../Source/World/Node.h) is the record it publishes, and the two are one to one.**
[Decision 86](#86-the-published-scene-is-a-preorder-tree-model-values-inline-coefficients-by-reference)
made the published scene a preorder run and
[decision 95](#95-the-scene-vocabulary-is-four-kinds-a-material-is-a-field-not-a-kind) fixed what a
record carries; neither said whether the thing that writes them is a tree of the same shape, a set of
entities each owning a little subtree, or something else. It is the first, and what settles it is
asking what needs identity rather than what looks like an object.

**The shape you reach for is entities owning subtrees, and what kills it is that the parts need
identity too.** A window looks like one thing with an internal structure, and decision 95 already
forces that structure:
`wl_subsurface.place_below` names the parent surface itself as a legal reference, so a toplevel is a
container holding its below-subsurfaces, its own surface, and its above-subsurfaces. So make the
container the entity and the three children its parts. Except that each of those parts has a
`wl_buffer` to release, a `wl_surface.frame` callback to fire, and damage of its own, and
[decision 75](#75-the-return-channel-is-one-report-per-frame-per-surface-facts-are-derived-not-sent)
requires dispatch to *derive* all three from a presented sequence — which means every one of them has
to be addressable in what was published. The parts need identity for reasons that have nothing to do
with animation, so they become entities anyway, and the two-level shape has a level nobody uses.

**One to one is also what makes `SubtreeLength` a single pass.** The length is a run length over
nodes, so an entity that emitted several records would need a second accounting to know what its own
subtree contributed before its children's could be added to it — a second count maintained beside
the recursion that already computes it, which is
[decision 16](#16-nodes-own-their-properties-the-active-set-is-mirrored)'s *derived, never
maintained* given away for no gain. One to one makes the length fall out of the walk.

**Identity is on every entity, including the structural ones nobody names.**
[Decision 15](#15-identity-is-a-generational-handle) already says *for every animatable entity
whether protocol-backed or compositor-invented*, and a container forced by `place_below` is
compositor-invented. It also pays forward: [Open.md](Open.md)'s per-node damage entry wants a stable
identity on the record, and an identity that already exists on the authoring side is a field to
publish rather than a mechanism to invent afterwards. The lifetime
[decision 45](#45-protocol-dispatch-is-a-thread-not-a-task) and
[decision 20](#20-exit-animations-use-full-resolution-snapshots) require — *an entity outlives its
protocol object* — is then the slot's and not the resource's, which is decision 15 already working
rather than anything this decision adds.

**The store is a slot map and intrusive links.** [`SlotAllocator`](../Source/Core/SlotAllocator.h)
owns the entities and mints decision 15's handle; the tree is parent, first child, and next sibling
held as handles rather than pointers, so a stale link compares unequal rather than naming whatever
occupies the slot now. Links rather than a contiguous child array because the commonest structural
change in a desktop is a sibling reorder — clicking a window raises it, and
[decision 55](#55-transforms-are-3d-the-scene-is-a-painters-algorithm) makes z the list order — which
is a pointer swap here and a move of everything above it there. The publisher chases those links, on
the thread that is allowed to.

**The top level is a list rather than a root**, which is what
[`Evaluator`](../Source/Frame/Evaluator.h) already walks: it starts at index zero and runs to the end
of the node run treating that span as siblings. A distinguished root would be a node every walk pays
for and no reader needs, and it would have to carry a transform that is always the identity.

**The per-kind payload is held out of line on this side too, for a different reason than on the
wire.** Decision 95 keeps what a leaf draws in per-kind runs so that the frame walk does not drag a
payload through cache; here the argument is only that most entities have none — a container has no
texture, no damage, and no colour state, and
[`Region`](../Source/Geometry/Region.h) alone is a quarter of a kilobyte at its fixed capacity. The
two arguments are unrelated and they produce the same layout, which is worth noticing because it
means the publisher's per-kind emission copies a record rather than building one.

**Rejected: entities owning subtrees of nodes**, above. It is the intuitive shape and it dissolves
under decision 75.

**Rejected: the store *is* the published run** — dispatch mutates the preorder array in place and
publication becomes a `memcpy`. Genuinely attractive, and it fails on insertion: a node added in the
middle shifts every index above it, every enclosing `SubtreeLength`, and every backward reference
target decision 95 requires, so opening a window is an O(world) move. It also makes a handle a
position, which decision 15 refuses in one sentence — *independent of tree position*, because
position is a property that animates.

**Rejected: a scene per output.** The obvious way to make each output's walk cheap, and it makes a
window straddling two outputs into two entities with two sets of springs — which is the configuration
[decision 32](#32-a-surfaces-frame-cadence-follows-its-fastest-output) makes first class and the
duplication [decision 88](#88-an-instance-is-a-node-the-published-scene-is-a-dag) refuses for
thumbnails on exactly the same grounds. One world in global space, viewed through
[decision 97](#97-an-outputs-placement-is-published-the-modes-half-of-the-view-meets-it-in-the-walk)'s
per-output adapter, is what the published form already assumes.

**Rejected: identity only where something asks for it**, as a sparse side map from handle to node.
It saves eight bytes on a structural container and it makes the per-node damage decision 101 wants
unbuildable in the direction it wants it, since the identity has to exist *before* the frame that
would use it to compare two publications.

**Consequences.** Publication is a full re-serialisation of the node run, and the copy-on-write
repair [decision 50](#50-the-world-is-authored-on-the-dispatch-thread-the-snapshot-carries-coefficients)'s
open pacing question proposes does not reach it: any insertion renumbers indices and lengths, so
there is no patch smaller than the run. What survives is narrower and more useful — the node run
changes only when topology, flags, content, or the *active set* changes, and a retarget inside an
already-active channel changes none of those, so the common case during a gesture is a fresh
coefficient run beside a byte-identical node run. Whether copying those bytes beats re-walking to
produce them is a measurement, and it is [open](Open.md) with the pacing question it narrows.

### 113. Client damage is a region on the entity, in buffer space, and it is cumulative

*(Decided 2026-08-22. [Structure.md](Structure.md#region-is-in-geometry-and-reachability-is-why)
says `Protocol` receives a client's rectangles, `Scene` stores them, and the publisher serialises
them in `BufferSpace`. It does not say in what, or when they clear, and the second has a wrong answer
that looks right.)*

**It is a [`Region<BufferSpace>`](../Source/Geometry/Region.h) on the image entity, unioned across
commits, published whole with every snapshot, and cleared against the presented sequence.**

**The region type rather than a rectangle list of its own**, because the collapse rule is already
written and is already the right one: past sixteen rectangles the set becomes its own bounding box,
which costs bandwidth, where a set that dropped a rectangle would leave stale pixels on glass. That
is the only direction that is not a defect, and it is the same argument whether the producer is a
client or the frame side.

**In buffer space, and the conversion happens at ingest.** `wl_surface.damage_buffer` is already in
buffer coordinates; `wl_surface.damage` is in surface coordinates, deprecated and still emitted by
live toolkits, and the two are related by a buffer scale and transform the client may change in the
same commit. So the surface-space form is converted where it arrives and nothing downstream carries
two spellings — [decision 57](#57-one-timebase-clock_monotonic-converted-at-ingest-and-nowhere-else)'s
rule about the timebase, applied to a coordinate. Buffer space is also the only one that survives the
change: rectangles banked in surface space before a `set_buffer_scale` would have to be reinterpreted
after it, against a scale that is no longer the one they were recorded under.

**Cumulative, because the ring may skip.** The obvious form is a delta — publish what changed since
the last publication, then clear — and
[decision 74](#74-the-forward-ring-recycles-only-below-the-watermark-and-a-full-ring-defers) forbids
it in one sentence: the frame thread takes the *newest* snapshot and skips whatever it passed, so
**nothing may be owed once per published snapshot — only once per rendered frame.** A delta is owed
per snapshot. Two publications inside one frame — ordinary, since dispatch publishes per resolved
commit and a client can commit twice in 16 ms — would lose the first one's rectangles, and the
failure is the one the region type exists to prevent, arriving through the channel instead of through
the capacity. So the snapshot carries the accumulation, which is complete state like everything else
that crosses.

**Cleared against the presented sequence, which is the same derivation that releases the buffer.**
Decision 75 has dispatch derive per-surface consequences from *sequence S was presented at T*, and a
surface's damage is one more of them: it may be dropped once every output showing that surface has
presented a sequence at or after the one that carried it. The multi-output case is why the rule has
to name every output rather than any — a window straddling a 60 Hz panel and a 144 Hz one is
presented at different moments, and clearing on the first would leave the second compositing a
region it never saw. The accepted cost is that on a mixed-rate configuration the faster output
recomposites a surface's rectangles once or twice more than it needed to, which is a superset and
therefore correct by the same argument as the collapse.

**On the wire it is a run of its own and the image names a span of it.** `ImageContent` goes from
forty-eight bytes to fifty-six, and the header gains a `Damage` entry beside `Images` and `Solids`
for [decision 97](#97-an-outputs-placement-is-published-the-modes-half-of-the-view-meets-it-in-the-walk)'s
reason: it is not a channel, so it is named rather than indexed by `RunIndex`. Decision 86's
consequences promised client damage by name and put it in `SnapshotRun`; decisions 90 and 97 have
since moved everything that is not a channel out of that array, so the promise is kept in the place
those two left for it rather than in the one 86 named.

**Rejected: a delta per publication**, above. Recorded rather than omitted because it is what every
compositor with a single-threaded loop does, correctly, and the thing that makes it wrong here is a
property of the ring rather than of damage.

**Rejected: the region inline in `ImageContent`.** `Region` is fixed capacity, so it is trivially
copyable and would cross with no run at all — at sixteen rectangles and a count on *every* image,
which is a quarter of a kilobyte per window to say that one caret blinked. The run costs an offset
and a count and carries what is used.

**Rejected: damage on the node record.** It is per surface and not per node: a container has none, a
reference has its target's, and a solid's damage is the fact that it changed at all. Eight bytes on
every node in the tree to say so is decision 95's colour-state argument arriving on a second field.

**Rejected: no client damage at all, on the strength of
[decision 101](#101-damage-is-the-whole-output-while-anything-moves-and-per-node-damage-needs-an-identity-the-record-does-not-carry).**
Tempting today and would stay tempting forever: damage is the whole output while anything moves, so a
client's rectangles change nothing until there is a partial-composite path to feed. That is the state
that never ends — the coarse rule stays because nothing produced the fine one, and decision 101 says
in its own words that client surface damage *has no carrier* until there is a protocol layer to mint
it. This is the carrier, and the run lands when `Protocol` mints the first rectangle rather than now.

**Consequences.** Decision 101's open entry narrows to its second half: the client's damage has a
home and a space, and what is still undecided is per-*node* damage, which needs an identity on the
record and a partial-composite path to spend it on.

### 115. `Scene` drains the return channel, and `Protocol` observes what it derives

*(Decided 2026-08-22. [Structure.md](Structure.md#the-runtime)'s runtime graph drew
`Return --> Protocol`, which is where the facts end up rather than where the channel is read.)*

**`Scene` drains the return channel at the top of each dispatch iteration and emits what it derives
as a signal `Protocol` observes.** Decision 75 collapsed the channel to one fixed-size report per
frame precisely because dispatch authored the snapshot and can derive the per-surface consequences
itself. Which module on that side does the deriving was left open, and three things decide it the
same way.

**gyro runs before any client exists.** The boot splash publishes a scene and the frame thread
presents it, and the snapshots have to be reclaimed against decision 74's watermark with no
`Protocol` in the process at all —
[decision 37](#37-gyro-owns-the-display-from-firmware-handoff-onward-there-are-no-vts)'s continuous
image is gyro-to-gyro the whole way, and
[decision 79](#79-the-console-is-a-renderer-not-a-presenter)'s recovery console has no protocol
behind it either. A drain that lives in `Protocol` stops the ring the moment there is nothing to talk
to, and the symptom is a compositor that runs out of snapshot slots on the screen it exists to keep
showing.

**The derivation needs what dispatch authored.** Decision 75's accepted cost — *keeping the
authoring side of a snapshot addressable until that sequence is reported* — is concretely a run of
[handles](../Source/Core/Handle.h) retained beside each in-flight snapshot and freed at the watermark
with the arena it sits next to. A handle is a `Core` type rather than a world record, so it rides in
`Publication`'s dispatch half without that module naming `World`, exactly as the node run does. What
turns a handle into a `wl_surface` is `Protocol`'s; what turns a presented sequence into a set of
handles is not.

**And the edge runs the wrong way for the arrangement that reads most naturally.** `Protocol`
depends on `Scene` and `Scene` may not say `Protocol`, so `Scene` cannot hand anybody a frame
callback. What it can do is declare a `Signal<>` and let the observer own the link, which is
[decision 77](#77-a-signals-observers-are-links-the-observers-own)'s shape and is legal because both
modules are the dispatch thread and a signal is intra-thread by rule. `Session` connects to the same
signal rather than a second one.

**Four consumers, one drain, and only one of them is `Protocol`'s.** The watermark goes to the
publisher, where it frees everything below it (74). The exit-blit hold's release is the atlas's (46)
and never leaves `Scene`. Client damage cleared by a presented sequence is
[decision 113](#113-client-damage-is-a-region-on-the-entity-in-buffer-space-and-it-is-cumulative)'s,
also `Scene`'s. Only the frame callbacks, `wp_presentation_feedback`, and `wl_buffer.release` cross
to `Protocol` at all — which is the sharpest form of the argument above, since three quarters of what
the report carries never leaves the module that drains it.

**Draining first is an addition to [decision 45](#45-protocol-dispatch-is-a-thread-not-a-task)'s
ordering rather than a change to it.** That decision drains input, then client traffic under a
budget. The return report is fixed-size and bounded, and it is the thing a client is *waiting* on: a
`wl_surface.frame` callback delivered at the top of the iteration gives the client the whole of that
iteration to render into, where the same callback at the bottom gives it the next one. Input keeps
its place immediately after, and nothing about `t₀` depends on the order, since it is the event's own
timestamp and not the moment of handling.

**Rejected: `Protocol` drains and asks `Scene` which surfaces sequence *S* contained.** Legal by the
graph and the natural reading, since the facts are protocol facts. It fails on the boot path above,
and it makes reclamation — a memory-safety property of the ring — conditional on somebody having a
client to notify.

**Rejected: a second return channel for `Scene`'s own facts**, so that the atlas hold and the damage
clear travel separately from the protocol ones.
[Architecture.md](Architecture.md#the-publication-boundary)'s *a third channel would be a design
error rather than an addition* is the whole answer, and decision 75 already declined to template the
queue for exactly this reason: the friction of extending one record is the feature.

**Rejected: `Scene` writing the facts onto the entities and `Protocol` scanning for them.** A pull,
with no signal and no edge problem, and its cost is proportional to the world rather than to what was
presented — which is what decision 16's *derived, never maintained* and decision 89's *neither phase
walks the world* both refuse, arriving on the return path.

**Consequences.** Structure.md's runtime graph gains `Scene` on the return leg, and the arrow from
`Scene` to `Protocol` is deliberately not drawn there, because it is an intra-thread observer call
and the graph is about the two channels. `Scene`'s dependency row does not change: `Signal` and
`Handle` are `Core`'s and it already has them.

---

## Animation

### 10. The animation system is built first

Animation quality *is* frame pacing; they are the same problem. Building it first means the
scheduler gets attacked while the codebase is small enough to restructure around the answer.

### 11. Springs are closed-form, not numerically integrated

Exact analytic solutions for the underdamped, critically damped, and overdamped regimes.

**Rejected: semi-implicit Euler / RK4.** Stateful and timestep-dependent, so a dropped frame yields
accumulated error rather than the correct value; cannot be evaluated at an arbitrary predicted
presentation time; and not reproducible, which forecloses golden-image testing. Closed form also
makes settling time analytic, which is what lets the compositor drop to idle cleanly.

**"Drop to idle cleanly" is a power invariant, not a tidiness one.** *(Annotated 2026-08-16.)*
[Decision 58](#58-idle-is-a-ladder-gyro-executes-and-does-not-choose) states it as a requirement —
no timer armed and the frame thread blocked indefinitely when nothing is animating — which is only
answerable exactly because settling time is analytic. An integrated spring cannot say when it is
finished without being advanced, so it must be woken to discover it has nothing to do.

**Reproducibility is per toolchain rather than absolute.** *(Annotated 2026-08-16, while writing the
solver.)* The rejection above leans on closed form being reproducible where integration is not, and
that is true of the algorithm and not quite true of the arithmetic. `exp`, `sin`, and `sqrt` are not
correctly rounded by any mandate, and libm implementations differ in the last place between versions
and across platforms — so two golden images produced from identical coefficients on different
machines may disagree in their final bits.

The conclusion survives, because the difference between the two is one of kind rather than degree:
an integrated spring's divergence is stateful, accumulates with whatever frame pattern the machine
happened to produce, and is unbounded, while this is a last-place difference in a stateless function
of time. What narrows is the scope of the claim. **A golden image is an artefact of a stated
toolchain**, comparing them wants a tolerance or a pinned one, and asserting bit equality across a
glibc update is asserting something nobody ever promised. Worth recording because the untightened
form of this sentence is the kind that gets believed for a year and then fails a CI image rebuild
with no apparent cause.

### 12. Springs are parameterized as (response, dampingRatio)

Legible and independent — how fast, and how bouncy.

**Rejected: physical (stiffness, damping, mass)**, which `CASpringAnimation` exposes and which is
miserable to tune because all three interact.

### 13. A closed motion vocabulary, with runtime configuration exposing only that vocabulary

Call sites cannot name spring parameters; numeric construction lives in the catalog alone. The
catalog's unit is a *transition* bundling per-channel springs designed together, not a single
spring.

Configuration is loaded in all builds — reduced motion is a genuine accessibility requirement, not
a development convenience. Cohesion survives because configuration exposes the five-to-seven named
motions and global modifiers, never individual transitions: retuning `Motion::Standard` moves
everything built on it together.

**Rejected: compile-time-only catalog.** Tuning feel is inherently iterative and a rebuild per tweak
means it does not happen. **Rejected: exposing transitions to configuration.** That is precisely how
system-wide cohesion is lost.

**The enforcement got stronger than "cannot".** *(Added 2026-08-16.)* Written in-process, this was a
convention backed by review and a greppable escape hatch.
[Decision 51](#51-the-shell-is-a-per-session-client-gyro-owns-mechanism) puts the shell on the far
side of a protocol whose vocabulary has no way to express a damping ratio, so the closure is
structural for every caller outside gyro itself.

Reduced motion is a first-class dimension of each bundle, not a scalar — properly implemented it
replaces movement with cross-fades rather than making springs faster.

**The catalog holds one thing that is not a spring.** *(Annotated 2026-08-16.)*
[Decision 65](#65-interactive-transitions-are-driven-by-a-progress-parameter-not-by-a-moving-target)
puts the map from gesture displacement to progress — travel distance, rubber-banding past the ends,
detent placement — in the catalog beside the springs, because it is motion design by every test this
decision applies and would otherwise land in the shell. Being driven is a dimension of a bundle
alongside reduced motion, and the two interact rather than compose: reduced motion substitutes a
bundle's channels and must *not* touch its parameterization, since a gesture that stops tracking is
not reduced.

**The escape hatch is a call rather than a name.** *(Revised 2026-08-17.)* This was written with
`Motion::Custom` as an enumerator a call site reaches for instead of `Motion::Standard`, which is
the natural shape while the catalog's unit is a spring. It is not available once the unit is a
bundle: an enumerator is resolved by looking it up in the configurable table, and the whole content
of the hatch is that it is *not* in that table — giving it a row would make it a sixth motion that
configuration retunes, which is the opposite of an escape hatch. So it is a direct construction of
spring coefficients from a response and a damping ratio. Everything the original clause asked for
survives and one thing improves: it is greppable in one command, obvious in review, and unlike a
name it cannot be reached from a bundle at all. Only the spelling was superseded, and only because
this decision's own conclusion made the old one unbuildable.

The greppability clause makes one demand on the spelling, and it is easy to miss. The hatch is a
*name of its own* rather than the ingest conversion the authoring path already runs through — that
function is called for every motion in the table and by every test that builds a spring, so a hatch
spelled as it would be a hatch whose one command returns the machinery. The name is what carries the
claim; the arithmetic underneath is deliberately the same, since a hatch that skipped the clamps
would also skip the settling floor those clamps exist to hold.

**What a bundle holds, and what a reduced form may be.** *(Annotated 2026-08-17.)* Both halves are
worked out in [decision
70](#70-a-bundle-is-channels-a-reduced-form-an-anchor-policy-and-a-drive-mapping) and [decision
71](#71-reduced-motion-is-three-named-forms-not-a-per-bundle-reduced-table), which carry the
rejected alternatives each generated. This decision is unchanged by them: the catalog is still a
closed set of transitions referenced by name, holding numbers nothing outside the vocabulary can
reach.

### 14. Declarative commits with dirty tracking

Shell code mutates world state inside a commit; the system derives transitions from the catalog. You
cannot forget to animate a property because you never animate properties, and a newly added
animatable property animates correctly everywhere with no call-site changes.

**Rejected: explicit `AnimateTo` plus transaction scopes.** Far less machinery, but forgetting a
property stays a live failure mode — and it is the most common way this kind of system rots.
**Rejected: CoreAnimation-style implicit animation on assignment.** Ambient state invisible at the
call site, and surprise animations cost frames that were not budgeted.

**Rejected: snapshot-and-compare diffing.** `SetModel` records changes as they happen, so commit
cost is proportional to what changed rather than to world size, with no allocation beyond a dirty
list.

Cost accepted: several weeks of machinery, and near-impossible to retrofit — every call site written
against an imperative API would need rewriting.

**"Shell code" is a client, not a caller.** *(Revised 2026-08-16.)* This decision was written with
`Commit` as a C++ call from gyro's own window-management code.
[Decision 51](#51-the-shell-is-a-per-session-client-gyro-owns-mechanism) makes the shell a
per-session client, so a commit arrives over the protocol and is resolved on the dispatch thread by
[decision 50](#50-the-world-is-authored-on-the-dispatch-thread-the-snapshot-carries-coefficients).
Nothing in the model changes — the differ, the dirty set, the shared `t₀`, and uniform interruption
are all unaffected, and the shared origin becomes *more* valuable, since it is what lets several
shell processes reacting to one input event move as a single gesture.

**A gesture is one commit, not a commit per event.** *(Annotated 2026-08-16.)* This decision notes
that gesture tracking commits at input rate and answers it by resolving the dirty set at most once
per frame.
[Decision 65](#65-interactive-transitions-are-driven-by-a-progress-parameter-not-by-a-moving-target)
makes the answer stronger by removing the traffic rather than pacing it: a driven transition is
established by one commit at gesture start, and the events that follow retarget a single progress
parameter instead of dirtying the channels it feeds. The differ runs once per gesture, and what a
1000 Hz device produces afterwards is one coefficient tuple republished.

**The pacing and the staged model are both withdrawn.** *(Revised 2026-08-22.)*
[Decision 89](#89-a-commit-resolves-in-two-phases-a-change-becomes-motion-where-its-inputs-are-complete)
keeps everything above and replaces *resolve the dirty set at most once per frame* with a phase
boundary drawn at input availability: a property change retargets at the write, and anything whose
inputs are the rest of the commit — matching, lifetime, atlas reservation, derived geometry — resolves
at close. The annotation above had already removed the traffic that motivated the pacing; what 89 adds
is that the pacing was also *wrong*, since two commits inside one frame carry different `t₀` and
resolving them together discards the motion between them. `SetModel` goes with it: the model value is
the spring target, so setting the model is retargeting and there is nothing to stage. The rejection of
snapshot-and-compare above is untouched — the argument was against comparison passes, not for
deferral.

### 15. Identity is a generational handle

`{ uint32 Index, uint32 Generation }` from a slot map, for every animatable entity whether
protocol-backed or compositor-invented. Independent of tree position; runtime-only.

**Rejected: protocol identity.** Fails on lifetime — when a client destroys its `xdg_toplevel` the
resource is gone but the window still has to animate out. Identity tied to protocol lifetime cannot
express an entity that outlives its protocol object, and it covers none of the compositor-invented
entities. **Rejected: pointer identity.** Addresses are reused after free, and the failure mode is a
new entity silently inheriting a dead one's animation state.

### 16. Nodes own their properties; the active set is mirrored

Readable at the call site, cache-friendly in the evaluation pass. Safe because the mirror is
*derived, never maintained* — registration happens only inside `Animatable<T>`'s own methods, so no
other code is able to make the two representations disagree.

**It is a serialization output rather than a mirror.** *(Revised 2026-08-16.)* Under
[decision 50](#50-the-world-is-authored-on-the-dispatch-thread-the-snapshot-carries-coefficients)
the two representations are not even on the same thread: dispatch owns the nodes and emits the
active springs contiguously into the published snapshot, which the frame thread walks. The safety
argument survives intact and gets simpler — there is no second live representation that could
disagree, only a serialization of the first.

### 17. Transforms are decomposed into TRS with per-channel springs

Matrices are never interpolated; lerping them shears and collapses. Per-channel springs let position
be snappier than scale, which is what makes motion feel designed rather than mechanical.

**The transform is three-dimensional, and rotation is a quaternion.** *(Revised 2026-08-16.)*
[Decision 55](#55-transforms-are-3d-the-scene-is-a-painters-algorithm) widens this without
disturbing it. Matrices are still never interpolated and channels still spring independently, but
rotation becomes one channel carrying a quaternion sprung in the log map rather than a scalar angle,
and an anchor point — under-specified here even in two dimensions — becomes explicit.

### 18. Matched geometry is in the first cut

Match keys plus the differ's exit/enter pairing pass. Without it, window-to-overview and similar
transitions are cross-fades, which is what "not cohesive" looks like in practice. Retrofitting means
revisiting every transition that already exists.

Developed against one real transition — window to overview thumbnail — rather than a synthetic test,
since that exercises mismatched aspect ratios, cross-tree parenting, and mid-gesture interruption at
once.

**The thumbnail is a minification.** *(Revised 2026-08-16.)* That transition is a large downscale,
which makes it the first thing in the system to depend on
[decision 56](#56-clients-render-at-the-ceiling-and-gyro-downscales)'s mip chain. Without one it
aliases, and it aliases worst exactly while the geometry is moving.

### 19. Hierarchical time is a per-subtree TimeScale only

Buys subtree slow-motion for debugging without per-property tree walks.

**Rejected: full CAMediaTiming.** Most of it (`beginTime`, `timeOffset`, `speed`, `repeatCount`,
`autoreverses`, `fillMode`) is redundant once springs handle interruption by retargeting rather than
by timeline scrubbing.

**The sample instant is per instance.** *(Revised 2026-08-16.)*
[Decision 64](#64-the-scene-is-instantiable-an-instance-has-identity-a-clock-and-a-permission) makes
the scene renderable into several targets at once, and
[decision 28](#28-the-frame-clock-is-per-output)'s per-output clocks mean two of those targets run
at different rates. So *when* the world is sampled belongs to the instance rather than to the frame,
which widens this decision without adding the timing model it rejects: `TimeScale` is still the only
hierarchy, and the rejection above still holds for the reason given.

**The rejection holds; the reason given for it did not.** *(Revised 2026-08-16.)* [Decision
65](#65-interactive-transitions-are-driven-by-a-progress-parameter-not-by-a-moving-target) finds
that retargeting subsumes `speed = 0` plus `timeOffset` for *interruption* and not for *driving*, so
a transition the user holds halfway was not expressible by anything this decision left standing.
What it adds is a progress parameter, which is not time: no `beginTime`, no `repeatCount`, no
`autoreverses`, and `TimeScale` still the only hierarchy. That is the second widening in two
decisions, and the shape is the same both times — the conclusion survives, the sentence justifying
it does not. Worth reading as a caution about this entry specifically, since it is the shortest
decision in the log and has now been the least reliable.

**The word *instance* has since narrowed.** *(Annotated 2026-08-22.)*
[Decision 88](#88-an-instance-is-a-node-the-published-scene-is-a-dag) makes an instance a node
expanded inline rather than a target of its own, so what samples its own time is an *output* —
including [decision 26](#26-remote-presentation-is-a-virtual-output-with-client-supplied-targets)'s
virtual one, which is where the different rates in the paragraph above actually come from. The
widening survives unchanged; it is narrower than the word it was written with.

### 20. Exit animations use full-resolution snapshots

Blit the last committed frame into a compositor-owned texture. *(Revised 2026-08-16.)* Where that
texture comes from, and when the blit is recorded, are
[decision 46](#46-exit-snapshots-come-from-a-pre-reserved-per-output-atlas-exhaustion-finishes-exits-early).

**Rejected: holding the client buffer for the length of the exit.** The memory does survive client
death — dmabuf refcounting handles that correctly — but three problems remain. Surface destruction
is not client destruction, so withholding `wl_buffer.release` for a 300 ms exit stalls the buffer
rotation of long-lived applications closing menus and popups. Exit animations do their heaviest
sampling (scale plus blur, multiple passes) on a buffer whose modifier was chosen for the client's
rendering or for scanout, so zero-copy is cheap at close time and potentially expensive on all ~43
subsequent frames. And pinning is client-controlled — a 4K RGBA buffer is ~33 MB, with the client
choosing both count and rate — so bounding it requires a fallback that is the snapshot path anyway.

**Only the third of those holds at every duration**, and noticing that is what opens decision 46's
deferral. A hold measured in one frame rather than 43 costs a long-lived application one buffer for
~7 ms and samples the foreign layout exactly once — which is the sampling the snapshot was going to
do anyway. The rejection above is a rejection of the *duration*, not of the reference.

**Withdrawn: the SIGBUS argument.** *(2026-08-16.)* A fourth ground was originally recorded — that
holding `wl_shm` buffers for hundreds of milliseconds past surface destruction widens the window in
which a client can truncate the backing fd and fault us mid-frame. It answers a case that does not
exist. Vulkan cannot sample a client's shm mapping, so every shm commit is already uploaded into a
compositor-owned image in order to be composited at all, on the dispatch thread where
[decision 45](#45-protocol-dispatch-is-a-thread-not-a-task) puts mapping and resource creation
alike. At close time those pixels are already ours and there is no client buffer to hold.
Snapshotting adds no shm SIGBUS exposure whatsoever; all of it belongs to the per-commit upload of a
*live* surface, which happens whether or not exit animations exist. Kept rather than deleted because
it inverted the difficulty of the two buffer types for six days: shm is the easy case, and dmabuf —
where the pixels genuinely exist nowhere else — is the hard one.

**Rejected: `VK_EXT_external_memory_host` for shm.** Importing the client's mapping rather than
uploading from it removes the copy the paragraph above depends on, and re-opens the withdrawn
argument in a worse form: a truncated fd stops being a SIGBUS we can catch and turn into a protocol
error, and becomes a GPU-side fault we mostly cannot. The wrong trade for a process with no restart
boundary.

**Rejected: downscaled snapshots.** Cheaper, but introduces a quality cliff that varies by
transition. The objection does not weaken under memory pressure, which is exactly where reaching for
it is most tempting — see decision 46's eviction rule.

**Superseded: a narrower zero-copy special case.** One optimization was previously held open — take
a reference rather than a snapshot when the client is *fully gone* and the exit is fade-only, since
neither the rotation-stall nor the sampling-cost argument applies there. Decision 46's one-frame
hold is strictly better: it applies to every exit rather than to a rare conjunction of two
conditions, and it buys frame-budget headroom rather than a copy. Both cannot survive, because
keeping the special case open is an invitation to build it.

### 46. Exit snapshots come from a pre-reserved per-output atlas; exhaustion finishes exits early

Decision 20 settles what an exit snapshot *is*. This settles where it lives, when it is written, and
what happens when there is no room — which is the part with a user-visible failure mode.

The requirement is not that exit animations look good. It is that **an exit animation is never
conditional on resource state.** The common exit is a menu dismissing or a tooltip fading inside a
long-lived application, dozens of times a minute, and it is doing perceptual work: the menu
collapsing back toward the control it came from is what tells the user which control they hit. One
that animates most of the time and pops the rest reads as a stutter, and the user attributes it to
the application that just closed — the same misattribution decision 20 rejects a long buffer hold
for.

**Storage is a per-output atlas, reserved at output configuration and never grown.** A snapshot
becomes a rectangle from a shelf packer plus a blit into a subregion, so no Vulkan object is created
on the frame path — which is what
[decision 36](#36-frame-path-discipline-is-enforced-mechanically-not-by-review) requires and what a
`vkCreateImage` at close time would violate. The atlas is also the layout the effect path wants:
exit animations blur, and blurring the whole retiring set out of one image is one descriptor and one
set of barriers rather than N. Fragmentation needs no compaction, because every occupant dies within
one exit transition and the pool therefore drains to empty on any idle moment.

**Capacity is denominated in output render-target equivalents, not bytes.** A byte count is right
only on the machine it was tuned on. As a multiple of the output's own render target it scales with
resolution and monitor count, it is per output like `renderBudget` in
[decision 29](#29-outputs-are-periodic-real-time-tasks-the-test-allocates-effect-budget), and it is
expressed in a cost the rest of this document already reasons about. The multiple itself is not
guessed — see the open list.

**Exhaustion is answered by finishing older exits early**, hard-settling their springs so their
rectangles free. In a storm of closing popups nobody is watching the twenty-ninth, so "old things
finished fast" is imperceptible, while the entity actually under the cursor still animates. Same
move as
[decision 30](#30-budget-shortfalls-are-answered-by-spending-less-chunking-and-early-rendering-are-contingencies):
answer a shortfall by spending less, in a way the eye reads as speed rather than as breakage.

**Slots are attributed to the connection that caused them**, per
[decision 27](#27-resource-accounting-is-attribution-not-per-user-fairness), and pressure is
resolved against the offending client's own slots first. That costs a counter and turns a global
degradation into one localized on whoever caused it: a client destroying surfaces in a loop degrades
its own exits and nobody else's.

**The blit is deferrable, and the reservation is what bounds the deferral.** A full-screen snapshot
is ~33 MB read and ~33 MB written, which on a laptop iGPU sharing memory bandwidth with the CPU is a
meaningful fraction of a 2–3 ms `renderBudget` — landing on the frame where the user just clicked
something and the application behind it is at its busiest. An unbounded count of blits is an
unbounded term in `C`, which decision 29 does not permit. So the rectangle is reserved when the
retirement is observed — a free-list pop, no allocation — and the blit is recorded in whichever of
the next frames has room. Two facts make late capture safe: a destroyed surface cannot commit again,
so the pixels do not change while we wait; and the first frame of an exit can sample the client
buffer exactly as a live surface does, so the snapshot only has to serve frames two onward. Policy
does not branch on buffer type. dmabuf holds the client's import for a frame or two, shm holds the
upload the compositor already owns, and both are bounded by the same reservation.

**The atlas admits bounded-lifetime occupants only.** Everything above rests on every occupant dying
within roughly 300 ms. A last-good-frame held for an unresponsive client is the same shape with an
unbounded lifetime, and one immortal occupant destroys the drain property for all the rest. Such a
consumer gets its own storage and its own eviction policy, not a slot here.

**On device loss the retiring set is dropped**, not preserved. Readback is impossible when a
Thunderbolt GPU is unplugged, so a path that reclaims snapshots without touching the GPU has to
exist; hard-settling springs and freeing rectangles is entirely CPU-side, so it does. And because
that path must exist for the violent case, planned migration in
[decision 41](#41-device-migration-is-exercised-on-every-boot) uses it too rather than carrying a
second path that only ever runs during a hardware fault.

**Rejected: a fixed-size slot pool.** Fully deterministic, and either wasteful on every popup or
unable to hold a maximized window. Window extents are continuous; slot sizes are not.

**Rejected: a general-purpose suballocator.** More flexible, and the flexibility answers a problem
this one does not have. Uniform short lifetime is precisely what makes shelf packing sufficient, so
a general allocator would carry compaction and fragmentation machinery to solve something that
solves itself — the same reasoning by which
[decision 9](#9-volk-and-runtime-shader-compilation-in-vma-and-a-test-framework-out) declines VMA.

**Rejected: downscaling under pressure** as the alternative to eviction. It preserves the animation
count at the cost of quality, which sounds like the better trade until its landing point is checked:
the cliff falls on whichever window happens to close during the pressure, including the one being
watched. Eviction degrades the exits nobody is looking at.

**Cost accepted, and it is at the publication boundary.** Deferring a blit means the frame thread
holds a client buffer past the commit that carried it, which the consumed-sequence watermark in
[decision 45](#45-protocol-dispatch-is-a-thread-not-a-task) cannot express — withholding the
watermark would stall reclamation of every unrelated commit below it. The deferral therefore needs
an explicit per-buffer hold released when the blit lands, alongside the watermark rather than
through it. Small, but it is new machinery and it is the honest price of the one-frame hold.

**Cost accepted:** eviction destroys the entity, so an evicted exit cannot resurrect
([Animation.md](Animation.md#lifetime)) — reopening it enters fresh instead of reversing. That only
happens under pressure, and the converse helps: resurrection frees a slot, so repeated open-close
relieves the pressure it creates.

**A gesture cannot produce an immortal occupant.** *(Annotated 2026-08-16.)* The admission rule
above takes bounded lifetime as a property of exits, and [decision
65](#65-interactive-transitions-are-driven-by-a-progress-parameter-not-by-a-moving-target)
introduces the one shape that could break it: a swipe-to-dismiss held indefinitely under a finger,
whose "exit" has no duration at all. It is closed there rather than here, by the entity not retiring
until the gesture resolves — so no rectangle is reserved while a finger is down, and there is no
driven occupant for eviction to fail to hard-settle. Recorded because the rule reads as safe by
construction and is in fact safe by a constraint living in another decision.

### 65. Interactive transitions are driven by a progress parameter, not by a moving target

*(Revises [decision 19](#19-hierarchical-time-is-a-per-subtree-timescale-only).)*

A transition the user is *making* is parameterized by a scalar `p ∈ [0, 1]` that the gesture drives
directly, rather than by springs whose targets the gesture moves. `p` is an ordinary `Animatable`
with two regimes — driven while a finger is down, sprung to an end on release — and every channel in
the bundle is a function of it. Mechanism in
[Animation.md](Animation.md#interactive-transitions).

**What this buys is holding and scrubbing**, which
[Experience.md](Experience.md#one-hand-made-all-of-it) now asserts: a transition stops where the
fingers stop, for as long as they rest there, and runs backwards if they reverse. It is not an
increment on interruption. A system can retarget perfectly, as decisions 11 and 14 already let this
one do, and still be structurally unable to express a transition held halfway.

**Rejected: the gesture moves the springs' targets** — `Motion::Interactive`, retargeted per input
event. This is what Animation.md assumed until 2026-08-16, and it fails three ways. A critically
damped spring tracking a target moving at `v` sits `2v/ω` behind it — ~95 px at a 0.3 s response and
a 1000 px/s scrub — and that error inverts through zero on every reversal, so the picture swims
rather than merely lagging. There is no world state for the moving target to read, because [decision
14](#14-declarative-commits-with-dirty-tracking) holds `Workspace.Visible` and not *47% open*. And
something has to map displacement onto per-channel values every event: if that something is the
shell, then the shell authors motion, and [decision
51](#51-the-shell-is-a-per-session-client-gyro-owns-mechanism) and [decision
13](#13-a-closed-motion-vocabulary-with-runtime-configuration-exposing-only-that-vocabulary) fall
together. The third objection is the decisive one, because it survives solving the other two.

**This is not the timing model decision 19 rejected, and the distinction is exact.** Progress is not
time. None of `beginTime`, `timeOffset`, `speed`, `repeatCount`, `autoreverses`, or `fillMode`
arrives with it, `TimeScale` stays the only hierarchy, and 19's rejection holds on its own terms.
What was wrong is narrower, and it was Animation.md's phrasing of the rationale rather than the
decision's conclusion: retargeting subsumes `speed = 0` plus `timeOffset` for *interruption*, not
for *driving*.
[Decision 64](#64-the-scene-is-instantiable-an-instance-has-identity-a-clock-and-a-permission)
widened 19 once already from the other side. The pattern is worth naming — 19's conclusion has now
survived two challenges and its stated rationale has survived neither, which is the reading decision
2 recommends and the reason this log records arguments rather than outcomes.

**Progress crosses the publication boundary as coefficients, and that constraint shaped the design
rather than following from it.** [Decision
50](#50-the-world-is-authored-on-the-dispatch-thread-the-snapshot-carries-coefficients) forbids
evaluated values in the snapshot so that the frame thread can produce two correct answers for two
outputs in one iteration. A bare `p` is one number on both, which pins a 144 Hz panel to the
staleness of the last input event rather than to its own presentation time — decision 28's defect
reintroduced at the boundary, invisible until a second monitor is attached. So the driven regime
publishes `(p₀, v₀, t₀, horizon)`, and the frame thread evaluates
`p(T) = p₀ + v₀ · clamp(T − t₀, 0, horizon)` per output. That is the first-order case of the shape
a spring already has, so the two regimes are one mechanism with two coefficient sets rather than a
scalar path beside a spring path.
`(T − t₀)` is the input-to-photon gap [decision
57](#57-one-timebase-clock_monotonic-converted-at-ingest-and-nowhere-else) makes a subtraction, so
this corrects a known latency per output rather than predicting anything.

**The horizon is load-bearing twice**, which is why it is in the coefficient tuple and not a tuning
constant somewhere. It bounds the extrapolation, so a reversal cannot overshoot on a stale velocity.
And it is what makes a held gesture *settle*: past `t₀ + horizon` the expression is constant, so
[decision 58](#58-idle-is-a-ladder-gyro-executes-and-does-not-choose)'s no-timer-armed invariant is
reachable with a gesture still in progress — a state no other system treats as idle. Its cost is an
abrupt stop running on by `v₀ · horizon`, answered by treating the absence of an update as
information: dispatch republishes with `v₀ = 0` at the horizon, one timer per gesture-stop, on the
analytic-settle machinery decision 50 already has.

**Rejected: publishing `p` as a scalar and re-deriving channels on the dispatch thread.** The
simplest version, and it leaves the frame thread untouched. It republishes O(affected channels) per
input event where the coefficient form republishes one tuple, it puts the extrapolation in as many
places as there are channels, and it is the evaluated value decision 50 refuses. The cost of the
chosen form is honest and is the frame thread growing a second evaluation kind, which is admissible
only because the curve set is closed by decision 13 and therefore cannot arrive from a client.

**Rejected: a driven entity may retire.** Swipe-to-dismiss is the obvious first interactive exit,
and letting it start retiring at gesture start would put an occupant of unbounded lifetime into
[decision
46](#46-exit-snapshots-come-from-a-pre-reserved-per-output-atlas-exhaustion-finishes-exits-early)'s
atlas, which admits bounded ones only, and whose eviction path resolves pressure by hard-settling
springs that a finger is holding. The entity therefore stays live until the gesture resolves. This
costs nothing — the client still exists and its surface is composited normally — and it is the
reason decision 46 needs no exception.

**Cost accepted:** interactive-versus-driven becomes a dimension of every bundle's definition, the
way reduced motion already is under decision 13, so it is first-cut work rather than an addition.
Retrofitting means revisiting every transition in the catalog, which is
[decision 18](#18-matched-geometry-is-in-the-first-cut)'s argument arriving for the third time. The
gesture recognizer, the displacement mapping, and a velocity estimator are new machinery on the
dispatch thread, and the estimator is the one whose quality is felt directly.

**Prior art, and it is not all Apple's.** `UIViewPropertyAnimator`'s `fractionComplete` is this
design exactly, and `NSEvent`'s `trackSwipeWithScrollDeltaX:` hands an application a `gestureAmount`
that is a progress parameter by another name; `CAMediaTiming`'s `speed = 0` plus `timeOffset` is the
older and worse form of the same idea, which is what makes decision 19's rejection of it easy to
over-read. GNOME Shell's `SwipeTracker` has done progress-driven gestures with velocity handoff
since version 40, and does them well — so the gap this closes is not that Linux lacks the
interaction, but that it exists in one shell rather than in the layer underneath, where per-output
evaluation, the coefficient boundary, and idle-while-held are available to it.

### 70. A bundle is channels, a reduced form, an anchor policy, and a drive mapping

*(Elaborates [decision
13](#13-a-closed-motion-vocabulary-with-runtime-configuration-exposing-only-that-vocabulary).)*

Four members, and the shape is more constrained than the count suggests. Mechanism in
[Animation.md](Animation.md#bundles-are-the-real-unit).

**A channel has three dispositions and not two**, which is the load-bearing part. Absent, immediate,
or sprung under a named motion — and the middle one exists because a slide-in that becomes a fade-in
has a translation channel that is *neither* animated nor untouched. The window has to be at its
model position immediately, and that is a different statement from *this transition has no opinion
about position*. Collapse the two and the reduced path either leaves windows at stale positions or
seizes channels it was never part of, and which of the two you get depends on which way you
collapsed them.

**Channels are labels rather than types**, so the catalog names translation, rotation, scale, and
opacity without `Animation` needing to know what any of them is. A spring's parameters are scalar
whatever its channel's value type is, by [decision
17](#17-transforms-are-decomposed-into-trs-with-per-channel-springs)'s per-channel construction, so
a bundle holds a motion per channel and never a value. The one place a type does appear is the drive
mapping's travel, which is why `Animation` depends on `Geometry` at all — see
[Structure.md](Structure.md#animations-geometry-edge-is-one-member).

**The anchor is a policy rather than a coordinate**, because the coordinate is not knowable in the
catalog. It is in the node's own space and depends on the node's extent, and for the case that
matters most it depends on something that does not exist until the gesture happens: a menu grows
from where it was opened. So a bundle names the anchor's *source* and the scene resolves it, which
is the same division of labour that keeps the differ's entity knowledge out of the module.

**The anchor and the mapping sit beside the channel table rather than inside it**, and that is what
makes decision 13's *the two dimensions interact rather than compose* structural instead of
intended. The reduced substitution's entire domain is the channel table, so it cannot reach either
one even by mistake — a gesture that stops tracking is not reduced, it is broken, and now nothing
has to remember that.

**Rejected: a stagger field.** Every transition worth writing today is unstaggered, so a field
defaulting to *no stagger* would change nothing about any of them, and it is the one member that
would have been inert data. What the design needs is recorded instead of built: the delay and its
cap are global modifiers beside speed, because an amount is tuning and must move the system
together, while the *order* items go in — list order, or outward from a focal point — is per
transition, because it reads completely differently and is motion design. One trap comes with it and
is worth writing down while it is cheap: **a staggered retarget must sample the old spring at its
own staggered origin**, not at `t₀`, or a mid-flight interruption freezes each entity where it was
for the length of its delay and then starts — a visible stall on exactly the case uniform
retargeting exists to keep smooth.

**Rejected: deferring the anchor the same way.** It fails the same test in the other direction. A
default is not harmless here — growing out of the middle rather than out of the corner a window was
summoned from is most of what makes a transition read as intentional — so entries authored before
the field existed would each need revisiting to decide something only their author knows. That is
the retrofit hazard reduced motion is first-class to avoid, arriving through a smaller door.

**The release motion is a rule rather than a fifth member.** A driven bundle's progress springs to
an end on release, and that spring is `Motion::Interactive` always, by what that vocabulary entry is
for. A per-bundle release motion would be exactly the incohesion decision 13 exists to prevent — two
swipes that let go differently — so it is not expressible. If it ever has to vary it is one field,
and the argument has to be made then rather than assumed now.

**The struct has five members, and the fifth is not a fifth thing said.** *(Annotated 2026-08-17.)*
`Bundle` also carries whether the transition needs an opacity group, and the title above does not
count it because the four are the statements this decision is about — what the motion *is* — while
the group flag declares what the transition costs to draw. It belongs to
[decision 60](#60-group-opacity-requires-flattening-per-node-alpha-is-not-a-group-fade) and is keyed
by transition only because the transition is what knows whether a subtree is underneath it; which
entries set it is [open](Open.md), and no answer there changes how anything moves.

That distinction is the test for the next member, and it is a better test than the count. **A member
that changes how the motion reads is refused by decision 13** — the count is downstream of that, not
the reason for it. **A member that changes what the renderer must allocate has to win decision 60's
argument instead**, which is a cost argument and answers to the frame budget rather than to cohesion.
A member that cannot be sorted into one of the two is the case to stop on, because it is probably two
members.

**One limit is worth recording while it costs nothing.** The flag is policy-invariant: one bool
serves both the authored table and its reduced form, so a transition needing a group on one path and
not the other cannot say so. `Transition::WorkspaceSwitch` is already that case and is fine only
because its ordinary path animates no opacity, which makes the flag unread rather than wrong. The
first entry that fades on both paths and needs a group on one turns this into a pair — and a pair
rather than a per-policy table, since the reduced substitution's domain is the channels and
[decision 71](#71-reduced-motion-is-three-named-forms-not-a-per-bundle-reduced-table) is why it must
stay there.

### 71. Reduced motion is three named forms, not a per-bundle reduced table

*(Elaborates [decision
13](#13-a-closed-motion-vocabulary-with-runtime-configuration-exposing-only-that-vocabulary).)*

`Fade`, `Cut`, `Unchanged`, and a bundle names one. Mechanism in
[Animation.md](Animation.md#reduced-motion-is-a-policy-not-a-parameter).

**Rejected: five forms.** The obvious vocabulary is fade-in, fade-out, cross-fade, cut, and
unchanged. The first three are one form: they differ only in what opacity is heading *for*, and that
is model state [decision 14](#14-declarative-commits-with-dirty-tracking)'s differ already holds —
an enter targets one, an exit targets zero. The direction is not the catalog's to know, and a
vocabulary that names it is one where an entry can be authored inconsistent with its own transition.

**Rejected: a per-bundle reduced channel table**, which is the flexible answer and the one that
arrives by itself if nothing is decided. It is decision 13's rejected *exposing transitions to
configuration* wearing accessibility's clothes: two independently authored fades, free to drift
apart, on the path that gets the least review and whose users are least able to tolerate a defect. A
transition needing something these three cannot say wants a fourth **named** form — a bespoke
reduced shape that recurs is a form, and one that does not is almost certainly a mistake.

**Rejected: deriving the reduced form from the authored channels.** Attractive because it cannot go
stale, and it gets the interesting case exactly backwards. A window that slides in with *no opacity
channel at all* still has to fade in when its movement is removed, so the reduction has to **add** a
channel rather than retune the ones present, and nothing in the authored table says which motion
that fade should take. Derivation also cannot express `Cut`, since a bundle that animates a size
looks identical to one that animates a position.

**Two rules hold across all three forms.** *Participation is inherited and disposition is not* — a
reduced form may snap a channel its transition already touched and may never seize one it did not,
because forcing an unrelated rotation to land immediately cancels whatever that rotation was doing
for somebody else. Opacity under `Fade` is the single deliberate exception, and it is the one the
policy exists for. And *reduced motion removes movement, not rhythm*: a fade standing in for a
workspace switch takes about as long as the movement it replaced, or the accessibility path runs at
a tempo the rest of the system does not share.

**`Unchanged` is a claim about an entry rather than an escape from the rule.** It returns the
authored table untouched, so it is the one form under which a transition could animate movement on
the accessibility path with every layer below agreeing it was fine. It is safe only because it must
be *named* — a bundle cannot arrive at it by omission — and because *only a transition that was
already pure opacity may declare it* is checked over the real entries rather than argued.

**Cost accepted:** the vocabulary will be tested first by matched geometry, and may lose. A reduced
matched-move is either a cut or a genuine cross-fade between two endpoints, and the second is not
expressible as a disposition table over one node's channels. `Cut` is the entry today; if it is
wrong, what it needs is a fourth form and not a per-bundle table, and the argument above is the one
that has to be beaten to get there.

### 72. The driven regime is a distinct record; the snapshot's arrays stay homogeneous

*(Settles [Open.md](Open.md)'s "where the driven regime lives", surfaced 2026-08-16 writing
`Animatable`. Turns on [decision
50](#50-the-world-is-authored-on-the-dispatch-thread-the-snapshot-carries-coefficients) and [decision
65](#65-interactive-transitions-are-driven-by-a-progress-parameter-not-by-a-moving-target).)*

Driven progress — `(p₀, v₀, t₀, horizon)`, read as `p₀ + v₀·clamp(T − t₀, 0, horizon)` — is its own
type on the dispatch side and its own array in the snapshot. `Animatable` stays spring-only and
uniform, so the flat array of active springs stays one shape, and the driven records travel beside it
as a second homogeneous run. The frame thread walks two fixed-stride, offset-addressed arrays, each
with a fixed evaluation kind and no per-record discriminant.

The ramp is a second closed form and not a corner of the first — decision 65 settled that it is "not
reachable from any `(ω, ζ)`", and its wake settles at an *authored* instant that `Spring::WakeAt`
cannot compute. So this is not a choice about whether to fold it into `Spring`, which is impossible,
but only about where the discriminant that has to exist somewhere should live.

**Rejected: a discriminated pair inside `Animatable<float>`**, which is the shape that keeps
[Animation.md](Animation.md#progress-is-an-ordinary-animatable)'s *an `Animatable<float>` like any
other* literally true, and it is rejected on where the tag lands. The driven arm only ever inhabits
*progress* — one scalar per gesture in flight, usually none — but `Animatable<float>` is also
opacity, blur radius, and corner radius. A tag on the type puts a dead ramp arm and a branch on every
one of those, and grows every record they serialize into. The snapshot array becomes tagged with it:
a branch in the frame thread's hot loop, and every spring record padded to the larger arm.
Offset-addressed POD wants fixed-stride homogeneous runs — the flat array of active springs
[Animation.md](Animation.md#storage) has the publisher emit — and a tagged array of two arm sizes is
neither. It also breaks the invariant `Animatable` was written to hold, that a property is exactly
its coefficients and carries no discriminant beside them.

**"Like any other" is a claim about the free regime, and that is where it earns its keep.** The
sprung regime *is* an `Animatable<float>`: a progress released toward an end springs, rests, and
settles like any property, and a gesture taking one over in flight reads its `(x, v)` and retargets
like any other. That is the composition with the idle fold and with interruption the phrase is
asserting. The driven regime is a distinct closed form by decision 65's own finding — its exactness
comes from being a direct map and not from any `(ω, ζ)` — and giving a distinct closed form a
distinct type is honesty rather than a second mechanism.

**It is not the "scalar path beside a spring path" decision 65 rejected.** That phrase named
publishing an evaluated `p` and re-deriving the channels on the dispatch thread — a different data
flow, and the evaluated value decision 50 forbids at the boundary. Two coefficient arrays, both
evaluated per output at predicted presentation time by a closed form, are the *one mechanism with two
coefficient sets* decision 65 chose; the mechanism is publish-coefficients-and-evaluate-per-output,
and a second array is not a second mechanism. Nor is it a second boundary: decision 50's "one
channel, not a second one" is about the thread crossing, and how many arrays live inside one snapshot
is not the crossing.

**`NextWake` was the member Open.md said decides it, and it decides for this shape.** It comes out as
two methods on two types rather than one branch: `Spring::WakeAt` over the spring array, and a
driven-progress wake that is `Continuous` until `t₀ + horizon` and `Settled` past it — the horizon
decision 65 already arms a `v₀ = 0` republish at. Each type answers its own wake and the fold reduces
both, which is what the commutative monoid in
[Animation.md](Animation.md#settling-answers-with-a-wake-not-a-boolean) is for.

**Cost accepted:** the differ and the gesture handler carry a regime switch for the one progress
channel — a retarget in each direction, reading the ramp's `(p, v)` to spring it home on release and
the spring's `(x, v)` to seed a ramp on takeover. It is the same retarget as everywhere, performed
once per gesture rather than per channel, and it never touches the frame path. Animation.md's *like
any other* gains the clause that it means the free regime, and the snapshot representation is fixed: a
spring-coefficient array and a driven-progress array, both offset-addressed, the second usually empty.

### 89. A commit resolves in two phases; a change becomes motion where its inputs are complete

*(Decided 2026-08-22, settling [Structure.md](Structure.md#open)'s "where the differ lives". Revises
[decision 14](#14-declarative-commits-with-dirty-tracking)'s dirty-set timing, and the `SetModel` the
same decision names.)*

**A property change retargets at the write, under the commit's shared `t₀`. Everything whose inputs
are the rest of the commit resolves at close. The differ is `Scene`'s.** Structure.md placed the
differ in `Scene` so that `Animation` could stay a pure library of springs and catalog and be built
first per [decision 10](#10-the-animation-system-is-built-first);
[Animation.md](Animation.md#declarative-commits) described the same machinery in a way that read the
other way, and the entry asked for the two to be reconciled before either was written. The module
half turns out to be the smaller one. What the reconciliation actually had to settle is *when* a
mutation becomes motion, and that question has a different answer for a channel than for an entity.

**Phase one is eager because deferral discards motion that happened.** Decision 14 answers gesture
traffic by resolving the dirty set at most once per frame. The case that breaks it is two commits
inside one frame, with different `t₀`, touching one property — ordinary against 1000 Hz input and a
60 Hz output. A window opens at `t₀₁`; eight milliseconds later a second event focuses and moves it.
Resolved once per frame, the second target wins and the opening never existed. Resolved at the write,
the second is a retarget from the true `(x, v)` of an open already eight milliseconds in progress,
which is [uniform interruption](Animation.md#declarative-commits) working. The first is right for the
same reason `t₀` is the event's timestamp and not `now`: **render what happened, not a summary of
it.** The traffic that motivated the pacing is separately gone —
[decision 65](#65-interactive-transitions-are-driven-by-a-progress-parameter-not-by-a-moving-target)
makes a gesture one commit and a republished tuple, which decision 14 already records.

Eager costs nothing to be eager. `AnimateTo` is `Begin(motion, PresentationState(t₀))`: four floats,
no allocation, no lookup, since the commit's bundle resolved at open. And two retargets at the same
`t₀` are **exactly** idempotent — the second samples the first's spring at its own origin and reads
back identical `(x, v)` — so repeated writes to one channel inside one commit are arithmetic rather
than motion, and the last target wins with no trace of the others.

*(2026-08-22, on writing phase one.)* **Exactly** cost one line to be true rather than nearly true.
Two of the three spring regimes return the initial condition from the closed form exactly at `t₀`;
the overdamped branch recovers the offset as a sum of two coefficients it divided by the root span,
which lands a unit in the last place away. So
[`Animatable::PresentationState`](../Source/Animation/Author/Animatable.h) reads the initial condition
back at the origin instead of solving for it. The regime is only reachable through a configured
damping — the catalog is critical or underdamped throughout — but a property that drifts a ULP per
write drifts once per input event, which is the rate this paragraph exists to make safe.

**The model value is the spring target, and there is no second field.** `Animatable::Model()` returns
`Spring::Target`, so a settled property and its model are one number by construction. Decision 14's
`SetModel` implies a staged value resolved later, and staging is not observable — a commit is a
transaction on the dispatch thread, so nothing reads the model between a mutation and the close. What
it would cost is an invariant to maintain across resurrection, hard settle on resume, atlas eviction,
and device migration, each of which rewrites springs wholesale. Drift there is not a hitch that
passes: it is a window whose logical position and animated destination permanently disagree, so
clicks land where it is not and layout packs against a ghost. That is
[decision 16](#16-nodes-own-their-properties-the-active-set-is-mirrored)'s *derived, never maintained*
with a user-facing failure attached. **Setting the model is retargeting.**

**Phase two exists because some inputs are the rest of the commit.** Four things are in it, and the
fourth is what makes it a rule rather than a list.

- **Matching.** [Animation.md](Animation.md#matched-geometry) is explicit that a move is emitted when
  a key-`K` exit and a key-`K` enter occur *in the same commit*. Resolved at the write, both halves
  have already been launched by the time the match is knowable.
- **Lifetime**, including the resurrection of a retiring entity, which is the same set operation seen
  from the other side.
- **Atlas reservation.** An exit is not only arithmetic:
  [Animation.md](Animation.md#when-the-blit-happens) reserves the rectangle *when the retirement is
  observed*. Eager exits therefore reserve for entities that turn out to be moves, and under
  [decision 46](#46-exit-snapshots-come-from-a-pre-reserved-per-output-atlas-exhaustion-finishes-exits-early)
  that pressure settles *other* exits early — menus and tooltips snapping out instead of fading, on
  precisely the busy commit where several things move at once. The user who pays is not the one whose
  window matched.
- **Derived geometry**, which is where the rule came from. The anchor is
  [declared by a transition rather than animated by one](Animation.md#bundles-are-the-real-unit) and
  a bundle names its *source* while the scene resolves the coordinate against the node's extent. A
  transition resolved at the write captures the extent as it stood then, so changing the extent later
  in the same commit leaves the anchor resolved against a size the node never had — a window growing
  out of the wrong corner, which is the defect the anchor field exists to prevent.

So the phase boundary is stated as availability rather than as an enumeration: **phase one retargets
channels and resolves nothing derived; every derivation is phase two.** Layout is thereby where it
already belonged — a pass at close rather than arithmetic inline in a commit body — and the rule does
not need extending the first time a derived quantity is added.

**Atomicity is unaffected, and the checking of it is worth recording.** Three things could have torn
and none do. The frame thread never reads dispatch state, only a published snapshot through the ring,
and publication is at close at the earliest — so write order inside a commit is invisible across the
waist by construction ([decision 45](#45-protocol-dispatch-is-a-thread-not-a-task),
[decision 74](#74-the-forward-ring-recycles-only-below-the-watermark-and-a-full-ring-defers)). Every
eager write uses the commit's `t₀`, fixed at open rather than read per write, so opacity and scale
share an origin whatever order they were written in. And under the paced publication
[decision 50](#50-the-world-is-authored-on-the-dispatch-thread-the-snapshot-carries-coefficients)
leaves open, two commits coalescing into one snapshot lose nothing, because the earlier commit's
motion is already baked into the `(x, v)` the later one's retarget sampled. Coalescing whole commits
is lossless in the only currency the frame side reads, which is the same property that makes phase one
correct.

**Rejected: resolving everything at close**, decision 14 read literally. It is right for phase two and
it collapses the multi-commit case above, on the input bursts where gestures live. It also needs the
staged model value, so the two halves of decision 14's wording fail together rather than separately.

**Rejected: resolving everything at the write.** Simplest, and it breaks matching, spends atlas
rectangles on entities that are about to become moves, and makes a commit body order-sensitive for
anything derived. The three failures are one failure: an operation was resolved before its inputs
existed.

**Rejected: the differ in `Animation`.** Everything in phase two is entity knowledge — identity, match
keys, the retiring set, the atlas — and decision 10 builds `Animation` before there are entities to
know. [Bundle.h](../Source/Animation/Author/Bundle.h) and
[Animatable.h](../Source/Animation/Author/Animatable.h) were written against the differ as a *caller*
for this reason, which is the placement holding under its own weight rather than by having been
written down first.

**Consequences.** Dirty tracking survives and relocates: what remains of it is the *publication* set —
which nodes to re-serialize — which is decision 50's open pacing question and belongs to
`Publication` rather than to the differ. `Animation` stays a pure library. Structure.md's open entry
closes, and Animation.md's *dirty tracking, not snapshot diffing* section is rewritten around the two
phases, since the heading now names a publication concern rather than a resolve queue.

### 112. A commit is a scope with an origin, and the wire says when it closes

*(Decided 2026-08-22.
[Decision 89](#89-a-commit-resolves-in-two-phases-a-change-becomes-motion-where-its-inputs-are-complete)
settled *when* a mutation becomes motion and left the transaction itself undescribed: where `t₀`
comes from once a commit is a wire request, and what "close" means when the caller is `Protocol`
handling `wl_surface.commit` rather than a shell making one call.)*

**A commit is a scope on the dispatch thread rather than an object with a lifetime. One is open at a
time, it carries an author and an origin, and it closes when the wire request that opened it
completes.** Everything below follows from noticing that three different things open one and only two
of them start motion.

**The three authors.** The shell, over the protocol, responding to an input event. gyro itself,
handling input it routes — a driven gesture's republished tuple under
[decision 65](#65-interactive-transitions-are-driven-by-a-progress-parameter-not-by-a-moving-target)
is a mutation like any other. And a client, at `wl_surface.commit`. The first two carry the input
event's timestamp, which is what
[decision 51](#51-the-shell-is-a-per-session-client-gyro-owns-mechanism) needs so that two shell
processes reacting to one event compose into a single gesture with no coordination between them. The
third carries no timestamp at all, and that is not an omission.

**A client commit starts no motion, and three cases have to be checked rather than one.** The
channels that spring are translation, scale, rotation, opacity, and the driven ramp, and what a
client authors is content, an extent, damage, and subsurface order. A client resizing itself changes
`Extent`, which has no coefficient slot and never had one. `wl_subsurface.set_position` does write a
translation, and [decision 68](#68-a-subsurface-snaps-like-any-other-settled-node) already says what
happens to it: it snaps to the grid the client rasterized against, because a spring between a video
player's controls and the screen would make them lag the video underneath. And a resize from a
window's left or top edge writes a translation too — gyro derives the origin from the acked geometry
and the resize edge, so the node moves on the client's commit — which is the interaction decision 51
says users judge most harshly, and interposing a spring there is the rubber-banding every shipping
compositor has. In all three the value is *set*; none of them starts a motion.

**So the rule is about motion rather than about writes, and an origin is required exactly where one
starts.** Decision 89 makes setting a model value *be* a retarget, so *set this without starting
anything* has to be sayable at all — which is [Open.md](Open.md)'s trajectory question arriving with
a case attached, and the reason a transition meaning *none* has to exist whatever is decided there.
Given that, a commit that starts motion and carries no origin is a bug, and the only alternative to
failing on it is stamping `now` — which adds a frame of lag to whatever it started, invisibly, on the
one axis [Animation.md](Animation.md#timing-and-rates) calls most of what makes a system feel like it
is tracking a finger rather than following it. A shell commit that omits an origin is a protocol
error for the same reason. If a client-driven change ever does want motion, it is given an origin
deliberately rather than acquiring one by default.

**The origin is clamped forward to the dispatch thread's own `now`, and nothing else needs clamping.**
A `t₀` in the future is not a late start: a closed-form spring evaluated before its origin is a
growing exponential, so a client that stamps a commit ten seconds ahead publishes coefficients that
reach the frame thread as unbounded coordinates and a quad with no finite extent. Clamping forward
makes the elapsed time non-negative by construction, because dispatch's `now` is at or before the
frame thread's read of the clock, which is at or before the presentation instant it evaluates for —
which is why the frame side needs no second check here, having plenty of its own under
[decision 90](#90-the-snapshots-runs-are-one-per-channel-and-the-frame-side-validates-the-tree-it-walks).
The backward direction is self-limiting: a stale origin reads as a motion that has already finished,
and a decaying exponential evaluated far along is settled rather than wrong.

**The rule stands and the reason above is wrong, which is worth recording rather than editing away.**
*(Corrected 2026-08-22, on writing the clamp.)* The frame side cannot see a growing exponential,
because [`Spring::Evaluate`](../Source/Animation/Solve/Spring.h) takes its elapsed time through
`Detail::SecondsSince`, which returns zero for any instant at or before the origin. A commit stamped
ten seconds ahead therefore evaluates to the state the motion *began* in, for ten seconds, and then
starts. So the failure is not unbounded coordinates: it is a window that was dragged and does not
move, for exactly as long as the stamp was wrong, with nothing anywhere reporting anything. That is
the same clamp for a worse-behaved reason — a defect that reads as the compositor having hung is
harder to attribute than one that draws garbage — and it is decision 49's shape again, an answer that
was right resting on an argument that was not.

**Close is per wire transaction, and phase two runs there.** `wl_surface.commit` is atomic for one
surface by Wayland's own definition, so it closes one commit; a shell's commit request closes
another. Batching an iteration's traffic into one transaction is cheaper and wrong twice: it mixes
origins, and it puts two unrelated clients' commits into one phase two, where a match key declared by
one could pair with the other's. The cost of not batching is small for the reason decision 89 gives —
phase two costs what the commit *touched*, and what a client commit touches is a lifetime and
sometimes an extent.

**Commits do not nest, and the double buffering Wayland requires is `Protocol`'s.** A synchronized
subsurface's state is held until its parent commits, and `xdg_surface.ack_configure` pairs a
configure with the commit that satisfies it. Both are staging in *wire* vocabulary and both resolve
before anything reaches the entity store, so the scene never sees a partially applied surface and
never needs a nested scope. That is what lets the commit be a member of the scene with reusable work
lists rather than an allocation per transaction, on a path a client can drive at its own rate.

**Rejected: `t₀` from the dispatch thread's clock**, which is the shape that arrives by accident the
first time a commit has no timestamp to inherit. It is the failure the rule above exists to make
impossible rather than to discourage.

**Rejected: one commit per dispatch iteration**, above. It is the arrangement a single-threaded
compositor has for free, and what it costs here is the origin and the match-key isolation.

**Rejected: a commit as a queued object resolved later.** Decision 89 already rejects deferral on its
own grounds; this is recorded separately because the *object* form is what invites deferral back —
something with a lifetime is something that can be held, and phase one is eager.

**Consequences.** [Open.md](Open.md)'s *the scene vocabulary closes arrangement and not trajectory*
narrows. Its candidate rule is that every mutation names a `Transition`, and it asks whether "a shell
that wants to place a window without animating it" is a real enough case to be worth the rule's cost.
The case is real and it is not the shell: three client-authored writes above are mutations that must
not animate, so the vocabulary needs a transition meaning *none* whatever is decided for shells. What
stays open is whether a shell may name it freely, which is the half that decides whether the catalog
is enforceable.

### 114. Retirement is the author going away, and resurrection is the author's alone

*(Decided 2026-08-22. Two thirds of this was already answered elsewhere, and one of the answers has
an example that is wrong.)*

**What was already settled, said once so it is not decided twice.** *Who drains the retiring set* is
[Architecture.md](Architecture.md#the-publication-boundary)'s and
[Animation.md](Animation.md#storage)'s: the settle time is analytic, so the dispatch thread schedules
entity destruction, retiring-set drainage, and atlas release without anything observing an
evaluation, and the wake it arms is a term in
[decision 69](#69-settling-answers-with-a-wake-idleness-folds-a-monoid-not-an-or)'s fold rather than a
timer of its own. *What resurrection buys* is [Animation.md](Animation.md#lifetime)'s: the id stays
live, the spring retargets from its current `(x, v)` and reverses smoothly, and the atlas rectangle
comes back. What is new is what the retiring set *is*, what puts an entity into it, and who may take
one out.

**The retiring set is a flag on the entity and a term in the wake fold, not a second container.**
Animation.md says an entity "moves to a **retiring** set — still evaluated and rendered, but
invisible to layout, focus, and hit-testing", and *moves to* is a figure of speech that a store would
implement literally and wrongly. A retiring entity is still drawn, so it is still published, so it is
still in the tree at the position it had — a separate container would have to be spliced back into
the preorder run to be serialised at all, and where it spliced would be a second answer to a question
the tree already answers. What the rest of that sentence describes is a predicate three readers
apply, not a place the entity is kept.

**An entity has exactly one author, and it retires when that author goes away.** The author is the
connection that created it — a client for its surfaces, the shell for the arrangement it builds, gyro
for what gyro invents. The rule matters because the alternative is a round trip: a client destroys a
surface, gyro tells the shell, the shell removes the node, and the exit starts a frame or two late
under an origin the shell had to invent because no input event caused it.
[Animation.md](Animation.md#exit-pixels) sets the bar that forbids that — **an exit animation is
never conditional on resource state** — and a shell busy indexing a filesystem is the worst version
of the same conditionality, since the window would not leave the screen at all. So a client's destroy
request retires the subtree that client authored, in place, under whatever container it was parented
into, and the shell learns about it whenever it next reads.

**Retirement is not removal, and the distinction is what keeps a workspace switch cheap.** A window
moved between workspaces, hidden, or reparented is not retiring — it is a link change or a flag, and
the entity is untouched. Only destruction by the author retires. Without the distinction, decision
95's overview — the real windows under a hidden container, the thumbnails below referencing them —
would retire nine workspaces' worth of windows every time the arrangement changed.

**It gives [decision 51](#51-the-shell-is-a-per-session-client-gyro-owns-mechanism)'s floor policy
its mechanism for free.** The shell disconnecting takes down everything the shell authored and
nothing any client did, which is *gyro keeps showing windows under default policy while the shell
restarts*, obtained from the lifetime rule rather than built beside it. Whether those nodes exit or
simply vanish is [open](Open.md), and the pull is toward vanish: a desktop's entire chrome animating
gracefully out is a statement about a crash that the user should probably not be shown.

**Resurrection belongs to the author, which takes it away in the case Animation.md leads with.** Only
an author can name an entity in order to re-create it, and the author of an entity that retired
*because its author disappeared* is by definition gone. That is right for everything the shell and
gyro invent — a workspace hidden and re-shown, a switcher tile, a drag placeholder — and it removes
**the menu**, which is the headline example: dismissing a menu destroys an `xdg_popup` and normally
its `wl_surface` with it, so reopening it produces a *new* surface and a new entity, and there is
nothing to resurrect. Fast repeated open-close is therefore served by
[decision 18](#18-matched-geometry-is-in-the-first-cut)'s matching rather than by resurrection, and
the two are different mechanisms with different prices: resurrection keeps one entity's springs,
while matching hands geometry from one entity to another and needs a key that nobody currently mints
— a client does not declare match keys and the shell does not know a popup was reopened. That is
[open](Open.md), and the correction is worth more than the answer, because the sentence it corrects
claims that behaviour is *entirely* what makes fast repeated actions feel right.

**Resurrection costs nothing inside the commit that retired the entity, and that is why lifetime is
phase two.** Decision 89 puts lifetime and atlas reservation at close so that a retire and a
re-create in one transaction cancel before anything is spent: no exit launched, no rectangle taken,
no blit queued. The case that makes this load-bearing rather than tidy is a shell that rebuilds its
arrangement declaratively, since remove-then-add is what a declarative rebuild looks like from the
store's side — resolved at the write it would reserve an atlas rectangle per node per commit, which
is [decision 46](#46-exit-snapshots-come-from-a-pre-reserved-per-output-atlas-exhaustion-finishes-exits-early)'s
pressure arriving from the authoring side and finishing *other* connections' exits early. Later
resurrection costs what decision 46 already prices: the rectangle if one was taken, and the blit if
it was recorded, which the frame thread may still be holding and which releases through the return
channel.

**Rejected: retirement on removal from the tree**, above. It is the reading Animation.md's "model
state removes the entity" invites, written when the model was gyro's own window management rather
than a shell's arrangement, and it cannot survive a workspace switch.

**Rejected: a separate retiring container**, above.

**Rejected: the shell retiring client-authored entities**, above — the round trip, and the exit that
does not happen when the shell is wedged.

**Rejected: invalidating the handle at retirement rather than at destruction.** Considered because
identity stops being *authorable* the moment the author is gone, which would make "resurrection is
the author's alone" true by construction rather than by rule. It breaks matching: decision 18 needs a
retiring entity to still be nameable so that a key-`K` enter can pair with it, and the pairing
happens after retirement by definition. Recorded because it is the tidy version and it removes the
mechanism the paragraph above just leaned on.

**Consequences.** Animation.md's *Lifetime* section needs the menu example corrected and the retiring
set restated as a flag; its *Declarative commits* section already reads correctly under
[decision 112](#112-a-commit-is-a-scope-with-an-origin-and-the-wire-says-when-it-closes). Open.md
gains the match-key question and the shell-disconnect one.

---

## Sessions

Detail in [Architecture.md](Architecture.md#sessions-and-users). The through-line: one process for
the machine means the isolation the kernel gives a per-session compositor for free is now gyro's
job, and the target is a laptop that is one user almost all of the time.

### 21. Many sessions connected, one presented locally

Clients of a session that is not on screen stay alive and warm. Switching users is not a teardown.

This is also what makes locking cheap. Because the greeter is a permanently connected session,
[decision 43](#43-lock-and-greeter-are-one-ui-locking-is-an-output-reassignment) can make locking an
output reassignment with no process start on either side of it.

**Rejected: one session at a time**, tearing down every client at logout. Isolation becomes nearly
free, and on a single-user laptop that is most of the benefit for none of the work. It was rejected
because it forecloses both fast user switching and remote presentation, and because the part it
saves — connection identity plumbing — is the cheap part. The expensive part is the wire
implementation that assumed a single tenant, and that gets written either way.

**Rejected: multi-seat**, two people on two monitors with two keyboards concurrently. A system-layer
compositor is unusually well placed to do this properly, and it is still not what a laptop is. Seats
stay plural in the interfaces because multi-output requires it regardless, so this is deferred
rather than designed out.

### 22. gyro runs as a dedicated unprivileged uid, with `CAP_SYS_NICE` and nothing else

`mlockall` comes from `LimitMEMLOCK=` rather than `CAP_IPC_LOCK`, device access from udev rules,
and DRM master implicitly from opening a device that has none. `SCHED_FIFO` comes from
`LimitRTPRIO=` — the rlimit rather than the capability, deliberately, so that real-time scheduling
does not depend on a capability that may later be dropped. Modelled on `WindowServer`, which runs
as `_windowserver` and reaches the GPU through IOKit ACLs rather than through privilege.

**`CAP_SYS_NICE` is taken for GPU context priority, and for nothing else.** *(Revised 2026-08-15;
originally recorded as no capabilities at all.)*

The real-time design is entirely CPU-side — `SCHED_FIFO`, `DEFER_TASKRUN`, no allocation on the
frame path — but the resource that actually blocks a composite is the GPU queue, where gyro is one
submitter among all clients. Clients reach the GPU through render nodes (`/dev/dri/renderD*`), which
require neither DRM master nor gyro's cooperation, so nothing in decisions 3, 29, or 45 constrains
them. A client's 10 ms batch delays gyro's composite by 10 ms regardless of what priority gyro's
threads run at.

Presentation is not the leak. gyro holds DRM master permanently, so every pixel reaches a connector
through gyro's atomic commit; there is no exclusive-fullscreen bypass on Linux, and direct scanout
of a client buffer is still gyro flipping. `wp_drm_lease_v1` is the sole exception and gyro decides
whether to grant one. Direct scanout in fact sharpens the problem rather than softening it: on the
frame where a fullscreen game is scanned out directly, gyro's own GPU work is near zero while the
*other* output's composite queues behind that game's batch.

The only lever is a high-priority queue — `VK_KHR_global_priority`, backed by a high-priority DRM
context — and the drivers gate it differently. i915 and xe require `CAP_SYS_NICE` for above-normal
context priority; amdgpu permits it for a DRM master, which gyro is. Taking the capability makes the
behaviour uniform rather than silently correct on AMD and silently degraded on Intel.

Mechanics:

- Request `VK_QUEUE_GLOBAL_PRIORITY_HIGH`, not `REALTIME`. Realtime is intended for the few things
  that must run ahead of a compositor and is refused by drivers more often than it is granted.
- Query with `VK_EXT_global_priority_query` and treat `VK_ERROR_NOT_PERMITTED_KHR` as a warning, not
  a fatal error. gyro runs correctly at default priority and merely defends its budget less well.
- Nested and headless never request it, consistent with their exemption from `SCHED_FIFO`,
  `mlockall`, and DRM master. Neither does the software path, where there is no GPU queue to
  prioritize — see
  [decision 40](#40-software-rendering-is-a-device-not-a-backend-and-it-is-the-floor-tier) for the
  thread priorities that replace it.
- **High priority reduces blocking; it does not eliminate it.** Preemption granularity is
  hardware-dependent and a long-running dispatch may not yield. The blocking term in
  [decision 29](#29-outputs-are-periodic-real-time-tasks-the-test-allocates-effect-budget) and the
  frame-drop backstop in [decision 35](#35-a-miss-costs-one-frame-bounded-by-the-floor-composite)
  both stand unchanged.

**What the capability concedes.** After a remote code execution in gyro — the threat this decision
exists to bound — `CAP_SYS_NICE` additionally grants setting scheduling policy, priority, affinity,
and I/O priority on other processes, plus `migrate_pages` and `move_pages`. That is a local
denial-of-service primitive. It grants no file access, no device access, no access to other
processes' memory, and no path to root. gyro can already take the machine's UI to black, so DoS is
not a new capability in kind. The trade is a narrow escalation of an attack that already ends the
user's session, bought against the only mechanism that defends the frame budget from its dominant
source of interference.

**Rejected: no capabilities, accepting default GPU priority.** This was the original decision. It is
internally inconsistent: it builds a real-time system whose every mechanism addresses CPU scheduling
while leaving the contended resource undefended, and it makes the timing guarantees of decisions
29–31 conditional on client behaviour. Utilization measured with no priority is not the utilization
the schedulability test is computed against.

**Rejected: running as root.** The process parses untrusted wire data from every account on the
machine, and its failure takes every session on that machine with it. That is the worst available
candidate for ambient privilege. *(Annotated 2026-08-16: this once read "using a hand-written
codec", which decision 2's reversal retired. The argument does not depend on it — the codec runs in
this address space whoever wrote it.)* `CAP_SYS_NICE` is not a step onto that
path — it is one bounded capability with one named use, and the rest of the set stays refused.

**Note:** `drmSetMaster()` wants `CAP_SYS_ADMIN`, which is effectively root. It is not needed —
first-open confers master, gyro never hands off, and Plymouth is a unit ordering problem. Needing
that capability remains an ordering bug, not a requirement.

**`mlockall` is not available in the form it looks like.** *(Added 2026-08-16.)* `LimitMEMLOCK=`
with `mlockall(MCL_CURRENT | MCL_FUTURE)` is the obvious reading of the table above, and it locks
far more than the frame path: every subsequent mapping in the process, which includes the dispatch
thread's allocations and every `wl_shm` pool gyro maps under
[decision 45](#45-protocol-dispatch-is-a-thread-not-a-task) — whose size and count a client chooses.
That hands a client the ability to decide how much of the machine's memory gyro pins unswappably,
which [decision 27](#27-resource-accounting-is-attribution-not-per-user-fairness) attributes but
deliberately does not bound, since its limits are survival backstops an order of magnitude above
anything real. A resource a client selects is the wrong thing to put behind a backstop.

What is wanted is residency for the frame thread's working set, not for the process, so either the
locking is targeted from the start or client pool mappings are `munlock`ed immediately after
mapping. `MCL_ONFAULT` narrows the exposure to pages actually touched and does not remove it. The
distinction is the same one
[decision 36](#36-frame-path-discipline-is-enforced-mechanically-not-by-review) draws about the
debug allocator: these are properties of the frame thread, and a process-wide mechanism that happens
to cover it also covers the thread that must be free to allocate.

**Open.** Whether to drop `CAP_SYS_NICE` from the permitted set once the Vulkan device and its
queues exist. Context priority is fixed at creation, so the capability is only needed across
initialization, and `LimitRTPRIO=` means `SCHED_FIFO` survives the drop for threads created
afterwards. Blocked on GPU hotplug and device-loss recovery policy, which may require re-creating
the device — and a capability that can be re-raised from the permitted set is worth much less than
one that is gone.

### 23. Connection identity comes from the listening socket, per user

One listener per user, in that user's `XDG_RUNTIME_DIR`. The listener is the credential: a
connection's session is known before its first byte, with no per-message checks.

**Per user rather than per session.** *(Revised 2026-08-16; originally recorded as per session.)*
[Decision 44](#44-a-session-is-a-user-identity-is-the-uid) establishes that a user has at most one
session, which collapses the distinction entirely — there is one listener either way, and naming it
after the user is what removes the need to name it at all.

**Rejected: one shared socket plus `SO_PEERCRED`.** Kernel-verified, cheap, and with no lifecycle to
manage — genuinely attractive. It loses on placement: the socket must then live outside any runtime
directory, which relies on clients handling an absolute `WAYLAND_DISPLAY` and breaks flatpak's
bind-mount of the socket out of `XDG_RUNTIME_DIR`. This objection is about a *machine-global* socket
and is untouched by the revision above, since a per-user socket in the user's own runtime directory
keeps every property the objection turns on. `SO_PEERCRED` is still checked at accept, and decision
24 makes that check load-bearing rather than redundant.

**Rejected: a per-user proxy process.** Forwarding connections means forwarding fds, doubling the
syscalls on exactly the path decisions 2 and 45 exist to keep cheap and bounded.

**Rejected: per-session listeners.** The original form of this decision, justified on the grounds
that a remote login against a machine whose local session is still running is two sessions for one
user. [Decision 26](#26-remote-presentation-is-a-virtual-output-with-client-supplied-targets) had
already made that false by turning remote presentation into an *output* — the justification outlived
the case it named by one decision. `WindowServer` remains the precedent, but the part that transfers
is the audit token's **uid**; its audit session does not.

### 44. A session is a user; identity is the uid

A user has **at most one session**, so `uid` names it completely. gyro mints no session name, the
offering agent asserts none, and no identifier exists anywhere in the system that a user could
choose, forge, or collide with.

This answers what [decision 24](#24-the-listener-is-handed-to-gyro-not-created-by-it) left open — it
binds a listener to "a session" without saying what names one. Every consumer of that name turns out
to be answered by a uid or by an fd:

| Consumer                           | Answered by                                  |
| ---------------------------------- | -------------------------------------------- |
| Routing an accepted connection     | the listener fd — decision 23                |
| Rejecting a laundered connection   | peer uid from `SO_PEERCRED` — decision 24    |
| Session start and session end      | the control connection — decision 24         |
| Switching to a live session        | uid — decision 42                            |
| Output assignment and lock state   | an internal handle, never named outside gyro |

**The case that would have forced more machinery does not exist.** The whole difficulty is one
scenario: the same user wanting two simultaneous independent desktops. Interrogated, it has no
occupant. Someone who logs in over SSH and launches an application onto the physical screen wants to
*join* the running session, not fork a second one — creating one there is the wrong answer, not a
missing feature. Someone who wants their desktop on a tablet at a different resolution while the
desk monitor shows something else wants **two outputs**, which
[decision 26](#26-remote-presentation-is-a-virtual-output-with-client-supplied-targets) already
delivers with its own frame clock, resolution, damage, and row in the admission set — and which
[Architecture.md](Architecture.md#a-session-is-not-a-display) already permits, since presentation is
a mapping that need not be onto. Someone using waypipe never needs an output on the far side at all.
What remains is "two independent window sets as one uid", which is a workspace feature wearing a
session's clothes.

**Same-uid separation would be theatre, not a boundary.** Two same-uid sessions are one kernel trust
domain: either can `ptrace` the other's clients, read `~/.config`, and signal at will. A boundary
gyro drew there would be one the kernel does not draw and gyro could not enforce. Sandbox boundaries
land on the *listener* via `wp_security_context_v1` instead, which
[decision 23](#23-connection-identity-comes-from-the-listening-socket-per-user) already anticipates,
and those compose without reference to sessions.

What this deletes, all of which was on the table before the case was examined: a minted session id,
a session class distinguishing local from remote logins, a privileged ticket by which the login
agent would vouch for that class, the kernel audit session id (`/proc/<pid>/sessionid`) as a
discriminator and with it a soft dependency on `CONFIG_AUDIT` and `pam_loginuid`, `SO_PEERPIDFD` and
the kernel floor bump it implies, and a collision convention for two agents writing socket names
into one runtime directory.

It also unblocks decision 42. Fast user switching reads "does uid U have a session? switch to it,
else create it" — the ambiguity of *which* of a user's sessions to switch to cannot arise.

**Rejected: gyro mints an opaque handle as the identity.** The shape suggested by
[decision 15](#15-identity-is-a-generational-handle), and gyro does still hold sessions in a slot
map internally. But a minted identity is only worth naming when something outside gyro must
pronounce it, and under one-session-per-user nothing does. The handle stays an implementation
detail rather than becoming an identity.

**Rejected: the kernel audit session id.** `/proc/<pid>/sessionid`, set by `pam_loginuid`, immutable
without `CAP_AUDIT_CONTROL`, and the direct analogue of the audit session in `WindowServer`'s audit
token. Genuinely unforgeable and genuinely per-login. It fails on fitness before it fails on cost:
it is a bare counter, so it distinguishes *logins* but not *kinds* of login — sshd sets one exactly
as a local login does — and the discrimination that was wanted was local-versus-remote. Paying a
soft dependency on `CONFIG_AUDIT` and a PAM stack for a discriminator that does not discriminate is
the wrong trade twice over.

**Rejected: the logind session id** (`XDG_SESSION_ID`). Re-introduces the D-Bus coupling decision 24
spent its effort removing, and would arrive via an agent running as the user, making it asserted
rather than verified.

**Deferred, not foreclosed.** Sessions stay plural in the interfaces — a session remains a
first-class object with a handle, a seat, a lock state, a focus stack, and an output assignment.
Only the *lookup* from uid to session is one-to-one, so widening it later is a map change rather
than a rewrite. That is exactly [decision 21](#21-many-sessions-connected-one-presented-locally)'s
position on multi-seat, and it is cheap for the same reason: the expensive part is a wire
implementation that does not assume a single tenant, and decision 21 already commits to paying it.

**Consequence: lock state is a property of a session, hence of a user.** Under
[decision 43](#43-lock-and-greeter-are-one-ui-locking-is-an-output-reassignment) locking is an
output reassignment, and the locked session keeps running and keeps any remote output. With one
session per user this reads simply — locking is per user, and remote access to a locally locked
machine reaches the same session that is locked out of the panel, which is the behaviour
[Architecture.md](Architecture.md#a-session-is-not-a-display) already describes.

### 24. The listener is handed to gyro, not created by it

A small session agent running as the user creates the listening socket in its own runtime directory
and passes the fd to gyro over a world-writable control socket. gyro verifies the offering process
with `SO_PEERCRED` and binds the listener to that uid's session — which
[decision 44](#44-a-session-is-a-user-identity-is-the-uid) makes unambiguous, since the uid names
the session completely.

The capability saving is secondary. What earns this is that **the handover is the session-start
event** — gyro learns that a session exists because a listener arrived, not because it subscribed to
a signal. The helper's control connection stays open for the session's lifetime, so its EOF is the
session-end event too. Combined with permanent DRM master and no VTs, this leaves gyro needing no
D-Bus client at all, retiring decision 7 rather than answering it.

**Rejected: gyro creates the listener.** One fewer moving part, but it needs `CAP_CHOWN` to hand the
socket to the user, puts gyro in the business of writing into user-owned directories, and still
leaves session-start discovery to be solved separately.

Consequences, all of which are requirements rather than regrets:

- **The per-connection `SO_PEERCRED` check becomes load-bearing.** A user can bind a permissive
  socket and offer it, so without a per-connection check another user connecting through it would be
  attributed to the offering user's session — landing inside their clipboard and surfaces. gyro
  therefore **rejects any connection whose peer uid does not match the session's user.** Under the
  rejected alternative that check is redundant, which is exactly why its status must be recorded:
  it is not belt-and-braces and must not be optimized off the accept path.
- **`SO_PEERCRED` bounds who offers; the offer itself must be validated separately.** *(Added
  2026-08-16.)* What arrives is a file descriptor, and an fd is not self-describing. A hostile offer
  can be a socket that is not listening, a socket bound in another network namespace, a pipe, or —
  worst — an `AF_INET` listener, which would leave gyro accepting connections from the network,
  where `SO_PEERCRED` yields nothing and the load-bearing check above passes an empty credential
  rather than failing. The offer is therefore checked before it is accepted: `SO_DOMAIN` is
  `AF_UNIX`, `SO_TYPE` is `SOCK_STREAM`, `SO_ACCEPTCONN` is set, and the bound path resolves inside
  the offering uid's runtime directory. Recorded for the same reason as the bullet above — it reads
  like defence in depth and is the opposite.
- **The control socket is an unauthenticated entry point** into the one process whose death takes
  every session on the machine. Bounded by `SO_PEERCRED` on the offer, the fd validation above, a
  per-uid offer cap, and one accepted listener per session.
- **The handshake is a stable ABI** that must survive gyro restarting — all listeners are lost, so
  helpers re-offer — and version skew across upgrades.
- **`WAYLAND_DISPLAY` must be in the session environment before any client starts.** Ordering bugs
  here present as "sometimes applications cannot find the display", which is a miserable class of
  bug to diagnose.
- **The greeter has no user session**, so the privileged login agent performs the handover for the
  greeter's dedicated uid. The login agent is therefore a hard dependency, not an optional one.
- **Nested and headless bind their own listener**, with no helper. This fits the existing rule that
  the development backends are exempt from `SCHED_FIFO`, `mlockall`, and DRM master.

### 25. The lock screen is a client; the compositor owns lock state, not lock UI

gyro knows whether a session is locked and enforces everything that follows from it. It does not
know how to draw a lock screen or a greeter. Authentication lives in the privileged login agent;
gyro never sees a credential. `loginwindow` is the same arrangement.

**Rejected: in-process lock and greeter UI.** Tempting for cohesion, since these are the transitions
most visible to the user. It drags text shaping and UI layout into a `SCHED_FIFO` process whose
entire thesis is doing one thing perfectly.

**Rejected: lock as pure client policy**, with no compositor-side state. Then a lock client crashing
exposes the desktop, which is the exact failure this split exists to make impossible.

[Decision 43](#43-lock-and-greeter-are-one-ui-locking-is-an-output-reassignment) later makes the
lock screen and the greeter the same client, which sharpens this rather than changing it: gyro's
ownership of lock state becomes load-bearing, because one greeter now serves every session and must
be respawnable without the machine falling open.

[Decision 51](#51-the-shell-is-a-per-session-client-gyro-owns-mechanism) later generalizes the split
itself. The argument here is not about lock screens — it is that UI with a design does not belong in
a `SCHED_FIFO` process, which applies to a panel and a launcher identically, and the state/pixels
division is the shape gyro uses for the whole shell.

### 26. Remote presentation is a virtual output with client-supplied targets

A `System`-tier client registers an output — size, format, and a ring of dmabufs it owns — and gyro
treats it as an output like any other: its own frame clock, its own place in the scene graph, its
own damage, its own row in the admission set. gyro renders into the client's buffers. What the
client does with them afterwards is not gyro's business.

**gyro does not know what a codec or a network is.** *(Revised 2026-08-15; originally recorded as a
remote output implementing `IPresenter` in-process.)* Remote desktop is one consumer of this
mechanism, a screen recorder wanting per-output pre-composite frames is another, and so is a tablet
used as a second display. Getting the word "remote" out of gyro's vocabulary is most of what makes
the decision safe.

The insight the original decision rested on survives unchanged, and it is the whole point: the
session renders *for* that consumer, at its resolution and its cadence, with damage intact,
zero-copy into memory the consumer already owns. Decision 1 is what makes it cheap — because the
renderer never sees a `VkSwapchainKHR`, a virtual output is another presenter rather than a fork.

**Rejected: screencopy or PipeWire capture over a local composite.** This is how remote desktop is
done on Linux today and it is why it is tolerated rather than used: compositing happens for a
display nobody is watching, the copy discards damage, the cadence is unrelated to how the frame was
produced, and input arrives through a side channel with no relationship to the frame clock.

**Rejected: the encoder and transport inside gyro.** The original form of this decision. It makes
exactly the move
[decision 25](#25-the-lock-screen-is-a-client-the-compositor-owns-lock-state-not-lock-ui) refuses
for lock UI — and worse, because a network stack is a *remote* attack surface on the one process
whose death takes every session on the machine, where the local wire protocol at least requires an
account on it first.
It also drags congestion control, TLS, and a protocol implementation into a `SCHED_FIFO` process
whose thesis is doing one thing perfectly.

**Rejected: encoder in-process, transport out.** Tempting, since encode is GPU work gyro already
schedules and it avoids a format conversion at the boundary. It brings rate control, bitstream
syntax, and codec session management with it, and Vulkan Video encode coverage is uneven enough that
a VA-API fallback would arrive alongside. The saving does not pay for the surface.

**Rejected: `IPresenter` proxied over IPC.** If `Present()` is a socket write it can block on the
frame path, which [decision 36](#36-frame-path-discipline-is-enforced-mechanically-not-by-review)
makes a test failure. The boundary is buffer handoff plus a non-blocking signal, never a remoted
method call.

Consequences, all requirements rather than regrets:

- **Buffer ownership inverts, and targets are dmabuf only.** `IPresenter` 's rule that the backend
  owns the images still holds — an encoder has format and modifier constraints the renderer cannot
  know — but here the constraint-holder is a client, so gyro *imports* targets and renders into
  them. Writing into foreign memory is new. `wl_shm` is refused outright: a dmabuf's size is fixed
  at allocation, so the truncate-and-fault attack that forces a SIGBUS handler for shm has no
  equivalent here, and permitting shm would import it.
- **A virtual output is in the admission set, and its frames are dropped rather than deferred.** It
  consumes real GPU time and genuinely blocks local outputs, so it contributes to `B` and must be
  admitted. But its period derives from flow control, so a network stall would otherwise make it
  unbounded. **It must never be able to make a local output miss.**
- **The allocation is negotiated, not asserted.** A client asking for 4K at 120 Hz would blow the
  budget; gyro answers with what
  [decision 29](#29-outputs-are-periodic-real-time-tasks-the-test-allocates-effect-budget) says it
  can afford, and the client takes it or asks for less. This is the only place a client influences
  the admission set at all, which is precisely why it is a negotiation and not a request.
- **Backpressure needs no new mechanism.** A target the client has not returned means
  `AcquireTarget()` yields nothing, which is already decision 35's skip case.
- **Injected input enters the ordinary pipeline.** The original decision criticised capture stacks
  for input arriving through a side channel. Out-of-process that is unavoidable, since the client
  receives input from the network and injects it. What is still claimed — and it is the part that
  matters — is that injected events carry timestamps and enter the same pipeline as local input, so
  `t₀` is the event time rather than the handling time.
- **A virtual output belongs to a session**, which decision 23 makes free: a client connecting
  through session S's listener can only address session S. Remote *login*, where the session does
  not exist yet, goes through the privileged login agent exactly as the greeter does in decision 24.

**Cost accepted:** concurrent presentation targets means concurrent frame clocks on one `SCHED_FIFO`
thread, with a virtual output's clock derived from flow control rather than vblank and `IsPrecise()`
false by construction. Multi-output already implied this; this makes it unavoidable.

### 27. Resource accounting is attribution, not per-user fairness

Every fd, shm mapping, and dmabuf import is attributed to its connection. Limits exist only as
survival backstops, an order of magnitude above anything real, and are enforced by killing the
offending client with a protocol error.

**Rejected: per-user resource budgets.** On a machine that is one user almost all of the time,
fair-share-between-users is a constraint with no beneficiary, and every threshold becomes a ceiling
some legitimate workload eventually hits. The reason for any limit is not fairness between users —
it is what the next paragraph makes precise. A session compositor that dies loses one login and is
restarted by its greeter; gyro dying costs incomparably more.

**What "gyro has no restart boundary" means, precisely.** *(Revised 2026-08-16.)* Taken literally
the phrase is false, and it is load-bearing in enough places — decisions 2, 22, 26, and 36 all lean
on it, and this decision is where it is stated generally — that the imprecision was worth removing.
gyro can be restarted, and this design already says so in three separate places:
[decision 24](#24-the-listener-is-handed-to-gyro-not-created-by-it) makes the handover a stable ABI
*specifically* so that helpers can re-offer their listeners after gyro restarts,
[decision 37](#37-gyro-owns-the-display-from-firmware-handoff-onward-there-are-no-vts) runs
`gyro --console` from a systemd `OnFailure=` unit, and
[decision 40](#40-software-rendering-is-a-device-not-a-backend-and-it-is-the-floor-tier) has
`RLIMIT_RTTIME` deliberately kill a runaway real-time thread.

The accurate claim is that **the restart boundary is ruinously expensive, not absent.** gyro dying
takes every client belonging to every user on the machine, not one login's worth, and the machine
comes back to a greeter with all work lost for everybody on it. Recovery exists; nothing that was
open survives it. That is a difference of degree from a session compositor and it is a very large
one, which is why the limits in this decision exist at all and why the failure arguments elsewhere
hold. It is not the difference of *kind* the shorter phrase implies, and decisions resting on it
should rest on this version — which they now do.

Two things follow that the stronger phrasing obscured, and both are taken up by
[decision 49](#49-the-restart-boundary-is-made-cheap-where-it-can-be-and-stated-where-it-cannot),
which is what this revision exists to make possible. A restart is a real recovery path, so it
deserves the treatment
[decisions 34](#34-effect-quality-is-a-tier-gyro-chooses-and-the-floor-tier-is-the-recovery-path),
38, and 41 all give recovery paths: exercised rather than assumed. And a deliberate death is
sometimes the better outcome — see decision 40's watchdog, which exists because the one state worse
than a dead gyro is a live one holding a `SCHED_FIFO` core on a machine with no VT.

Attribution is kept unconditionally because it costs a counter increment, it is the first thing
wanted when diagnosing memory, and it is the part that cannot be retrofitted afterwards.

Client send-buffer backpressure is *not* filed here. Unbounded buffering is a memory bug at one user
as much as at ten, so it belongs with the protocol layer's backpressure work.

---

## Shell

Recorded 2026-08-16, immediately after
[decision 50](#50-the-world-is-authored-on-the-dispatch-thread-the-snapshot-carries-coefficients)
and prompted by it: having settled which *thread* owns the world, the next question was whose *code*
does. Detail in [Architecture.md](Architecture.md#the-shell).

### 51. The shell is a per-session client; gyro owns mechanism

Window-management policy and every piece of shell chrome — panel, launcher, overview, switcher,
notifications — run as clients at the user's own uid. gyro owns mechanism: z-order, hit-testing,
input routing, the entity graph, transitions, materials, the background, the frame loop, and
everything that spans sessions.

**One compositor per machine makes built-in policy a category error.** Baking one desktop's window
management into the system layer makes it opinionated about something properly per-user, and it puts
the code whose requirements will churn for years inside the process whose restart boundary
[decision 49](#49-the-restart-boundary-is-made-cheap-where-it-can-be-and-stated-where-it-cannot)
prices as ruinous. A per-session, per-uid shell reclaims exactly the isolation
[decision 21](#21-many-sessions-connected-one-presented-locally) gives away by serving every user
from one process.

Most of the design already pointed here without saying so.
[Decision 25](#25-the-lock-screen-is-a-client-the-compositor-owns-lock-state-not-lock-ui) refuses
in-process UI on the grounds that text shaping and layout do not belong in a `SCHED_FIFO` process,
which is an argument about panels and launchers as much as about lock screens;
[filtered globals](Architecture.md#filtered-globals) already places layer-shell,
foreign-toplevel-management, and output configuration in the System tier; the greeter is already a
client. This decision is that generalization stated once. The precedent runs the same way:
`WindowServer` does not own Mission Control or Spaces — the Dock process does, and it drives them by
handing the render server transactions rather than by producing frames.

**The rule that makes it work: the shell declares and configures; it is never in a per-event loop.**
That is the principle [decision 33](#33-effects-are-named-materials-not-parameterized-filter-calls)
and
[decision 13](#13-a-closed-motion-vocabulary-with-runtime-configuration-exposing-only-that-vocabulary)
already apply — name a material and gyro decides what it costs, name a transition and gyro owns the
springs — extended once more. Direct manipulation is then declarative too: the shell does not
receive motion events and reply with positions, it declares that an entity tracks the pointer until
release, and gyro runs that at device rate with no round trip at all.

#### Several clients, not one

One policy client; chrome as separate clients. The reason is crash radius, and it is
one-directional: bundled, a bug in the launcher's search takes window management with it, and the
user's recourse is the thing they can no longer reach. Split, the launcher blinks out and comes
back.

The usual reason to bundle is cohesion — entering the overview must animate the panel out and the
thumbnails in as one gesture, and across two processes those are two commits arriving at different
times. gyro does not have that problem, and the reason is a rule already recorded: **`t₀` is the
input event's timestamp, not the moment of handling.** Two processes reacting to the same event
stamp the same origin, so gyro composes them into one gesture; and if one commit arrives a frame
later, decision 50's coefficients mean it renders *already in progress* by exactly the right amount
rather than starting late. Multi-process cohesion is free here in a way it is not in any design that
integrates.

What is not free is login, which is not input-driven and so has no shared origin to inherit. That
wants an explicit session-ready barrier rather than each client appearing when it happens to be
warm.

#### Continuous manipulation is gyro's; discrete state changes may round trip

Resize is the case that sets the line. It is already a round trip — gyro configures, the client
renders, the client commits, and only then does the edge move — which is why resize rubber-bands on
every Wayland compositor shipping today. Putting the shell in that loop makes it
gyro → shell → gyro → client → gyro on the interaction users judge most harshly.

Maximize is the opposite, and the difference is instructive rather than a matter of degree: gyro
animates the window's geometry immediately and the client's pixels catch up over a frame or two, so
felt latency is governed by when the *animation* starts, not by when the client has rendered. A
shell hop costs one frame of animation start against no reference point.

So drag, resize, and swipe are gyro mechanisms, with the shell declaring constraints ahead of time —
minimum and maximum sizes, snap targets, tiling gravity. Maximize, tile, move-to-workspace, and
placement of a new window may round trip. The failure modes sort the same way, which is the check
that the line is in the right place: a wedged shell under this split means maximize stops working,
and under the other it means windows cannot be resized. The first is a desktop misbehaving; the
second is a broken machine.

**Consistency matters more than speed on this path.** A configure that takes 8 ms every time reads
as a physical response; one that varies between 1 and 20 ms reads as unreliability. That is a second
argument for keeping policy out of the process that also runs the launcher's file indexing.

**What "declares" means for a swipe.** *(Annotated 2026-08-16.)* This decision keeps swipe inside
gyro and describes the shell's side as constraints declared ahead of time, which is enough for drag
and resize and was never worked out for a gesture that scrubs a transition.
[Decision 65](#65-interactive-transitions-are-driven-by-a-progress-parameter-not-by-a-moving-target)
supplies it: the shell binds a gesture to a named transition and its endpoints, once, and gyro owns
the recognizer, the displacement mapping, the progress parameter, and the release. The shell learns
that the gesture began, committed, or was cancelled, and those arrive whenever — none of them is in
a loop. That is the case this rule was most likely to lose, since a swipe is the interaction where
a per-event request looks most reasonable, and it is now the case that demonstrates it.

It corrects the drag sentence above as well, which said the entity tracks the pointer *under*
`Motion::Interactive`. That was the moving-target model 65 rejects, written before there was a
distinction to violate. Drag is the degenerate driven case — the mapping is identity and the
tracking is exact — and `Motion::Interactive` is the motion of the residual: the pushback at a
constraint, the pull into a snap, the settle on release. Never the motion of the finger, which no
spring should be interposed in.

#### A closed set of node kinds, arbitrary composition, motion only from the catalog

The shell describes scenes from a closed vocabulary of node kinds — surface reference, snapshot
reference, solid, effect layer — composable into arbitrary trees, and moved only by catalog
transitions.

The extremes are both wrong. A fully closed vocabulary, where gyro knows windows, thumbnails, tiles
and nothing else, gives perfect cohesion and forecloses every new idiom — no fisheye dock, no radial
menu, no coverflow switcher, because there is no node to express them with. A fully open one gets
all of those, and the first thing that happens is a shell that animates its own way, at which point
[Animation.md](Animation.md)'s first priority is lost quietly and permanently rather than in
something anyone reviews.

**Cohesion lives in the motion, not in the arrangement.** The shell may invent any arrangement and
cannot invent a spring. Two desktops on gyro then feel like the same machine while looking nothing
alike, the way two macOS applications feel like macOS with entirely different layouts. It is the
same trade decision 33 makes for materials — closed set, arbitrary placement — one level up, and it
comes with a falsifiable test: **if a shell can produce motion that does not match the catalog, the
line is in the wrong place.**

A wire vocabulary is also *stronger* enforcement than decision 13 currently has. In-process, "call
sites cannot name spring parameters" is a convention backed by review and a greppable escape hatch.
Across a protocol, the shell has no way to express a damping ratio at all.

The second argument for the closed set is shell restart. With known node kinds gyro can keep showing
windows under default policy while the shell is upgraded or respawns, and the session stays coherent
through a hiccup; with anonymous nodes gyro holds a pile of things it cannot interpret and must
either freeze or drop them. **That closed set is the floor policy** — the vocabulary that lets a
shell be replaced is the same one that lets gyro run without one, which is
[decision 34](#34-effect-quality-is-a-tier-gyro-chooses-and-the-floor-tier-is-the-recovery-path)'s
argument arriving in a second place.

**The four kinds above were a sketch and are superseded.** *(Revised 2026-08-22.)*
[Decision 95](#95-the-scene-vocabulary-is-four-kinds-a-material-is-a-field-not-a-kind) settles the
set, and it is four again and a different four: surface reference and snapshot reference are one
kind, an effect layer turns out to be a *field* rather than a kind, and a container — a node with
subnodes and no content of its own — is missing here and is the one kind the encoding cannot do
without. Everything this section argues survives that unchanged; what changes is only the list.

#### gyro owns the background

The background is gyro's, not a client's, and it does more work than it appears to. Blur always has
a backdrop, so `Material::Glass` over a bare desktop has no degenerate case and needs no fallback.
Boot continuity becomes client-free: firmware BGRT to gyro's own background is gyro-to-gyro the
whole way, so
[decision 37](#37-gyro-owns-the-display-from-firmware-handoff-onward-there-are-no-vts)'s single
continuous image no longer depends on any client being up. And both shell restart and the
session-ready gate below get a floor to sit on, so neither shows black.

The shell supplies it as a **buffer**, not a path, and gyro persists a copy:

- **A raw dump plus a small header, never an encoded image.** The point of the cache is that gyro
  needs no decoder; accepting a path or a PNG would put an image parser back inside the process with
  the expensive restart, which
  [decision 27](#27-resource-accounting-is-attribution-not-per-user-fairness) states precisely.
- **The header carries the color state**, since every surface has one under
  [decision 47](#47-compositing-happens-in-linear-light-at-wide-primaries) and a background reloaded
  next boot without it renders in the wrong space. Stored as delivered rather than pre-converted, so
  it reuses the client-buffer import path instead of adding a second one.
- **Written by a helper thread**, never the frame thread and not the dispatch thread either, where a
  multi-megabyte write would stall input for every session on the machine. Animation.md already
  establishes the pattern for configuration parsing.
- **Debounced.** Rotating-wallpaper slideshows are common and would otherwise write to disk every
  thirty seconds; only a background that has been stable for some interval is persisted, while the
  in-memory copy tracks changes immediately.
- **In gyro's own state directory, keyed by uid**, since gyro cannot and should not write into a
  user's home. Which means gyro accumulates per-user data outside the user's control, and deleted
  accounts leave images behind — a cleanup rule, carried as open below.
- **Shown at a mode it was not captured at**, which is the scale-and-place computation decision 37
  already needs for BGRT rather than new machinery.

**gyro never composites a user's cached background before that user has authenticated.** There is
real pressure to do otherwise — showing a selected user's wallpaper on the login screen looks
excellent and macOS does it — and it would put user-controlled pixels on a screen where a
*different* user may be about to type a password. A wallpaper is an arbitrary image and so is a
photograph of a convincing password field.
[Decision 43](#43-lock-and-greeter-are-one-ui-locking-is-an-output-reassignment) buys structural
anti-spoofing by ensuring a locked session is not composited at all, and this would reopen it
through a side door. After authentication the question dissolves: the lock screen shows the
wallpaper of the user who locked it, which is the uid that supplied it, and nobody can phish
themselves.

That restriction costs a nicety and returns something larger. Decision 43 records "the locked screen
cannot show user-owned content" as an accepted cost, because the greeter is a different uid and
cannot read the locked user's wallpaper. Since gyro holds the copy, wallpaper comes off that list
without weakening the property — gyro composites the image and the greeter never receives it. The
most visible piece of *this is my machine* returns as a consequence of the isolation rather than as
an exception to it. Notifications and media controls stay out of scope for the reasons already given
there.

#### An output is not reassigned until the incoming session's shell has presented

`graphical-session.target` to a shell that has presented is a second or two. Without a gate, login
is greeter, then blank, then shell — three beats where decision 37 promises one continuous image.
With it, and with the background above, it is greeter, then that user's background, then the shell
fading in over it. gyro therefore needs a session-ready signal, and it is the same signal the login
staggering above wants.

**Rejected: shell in-process.** What [decision 14](#14-declarative-commits-with-dirty-tracking)
assumed when it was written — `Commit` as a C++ call from gyro's own window-management code. It is
the lowest-latency arrangement available and it is the one that makes gyro a desktop environment
rather than a system layer, puts churning policy inside the ruinous restart boundary, and makes the
shell unreplaceable.

**Rejected: one bundled shell client.** The conventional shape, and it would be right if cohesion
across processes were expensive. Shared `t₀` makes it free, which leaves only the crash radius, and
that argument runs one way.

**Rejected: an open scene vocabulary.** Maximum expressiveness, and it trades away the one property
this project exists to have. It also leaves gyro unable to interpret its own scene across a shell
restart.

**Rejected: the shell in the continuous-manipulation loop.** Conceptually cleaner — all policy in
one place — and it makes resize worse than every compositor gyro is trying to beat.

**Rejected: a background supplied as a file path.** Simpler for the shell and it puts an image
decoder in gyro. **Rejected: showing a cached background before authentication**, above.

### 95. The scene vocabulary is four kinds; a material is a field, not a kind

*(Decided 2026-08-22, settling [Open.md](Open.md)'s *the shell's scene vocabulary* and filling the
fields [decision 91](#91-the-worlds-vocabulary-is-a-module-of-its-own-below-both-waists) left out of
the node record. Corrects the sketch in decision 51 above.)*

**A node is a `Container`, an `Image`, a `Solid`, or a `Reference`; what dresses it is a field on
every one of them.** Decision 51 sketched surface reference, snapshot reference, solid, and effect
layer, and [Open.md](Open.md) has carried that list as a sketch rather than a design ever since. The
replacement is four again and a different four, which is why the corrections are worth more than the
count: two of the sketch's entries are one kind, one of them is not a kind at all, and the kind that
is missing is the one the tree cannot be encoded without.

#### A surface reference and a snapshot reference are one kind

[Texture.h](../Source/Core/Texture.h) already made this argument for the id space and decision 82's
`DrawTexture` for the seam, both from the same case: [exit pixels](Animation.md#exit-pixels) turn a
closing window's live surface into a compositor-owned snapshot *while the exit is running*. As two
kinds, the node changes kind under a running spring — so either it is destroyed and rebuilt, which is
the failure [decision 18](#18-matched-geometry-is-in-the-first-cut) is in the first cut to prevent, or the
kind is mutable, which gives up what the closed set is for, since decision 51's floor policy is gyro
interpreting its own scene while the shell restarts.

What that costs on screen is the transition [Animation.md](Animation.md#exit-pixels) builds around: a
menu collapsing back toward the control that opened it, restarting its collapse at the moment the
client released its buffer, dozens of times a minute inside one application. So it is one kind, and
who owns the pixels is a question for the half of the renderer that imported them.

#### An effect layer is a field, and so is an elevation

Decision 33's material is a *dressing*, and decision 82's `DrawItem` already carries `Dress` beside
`Content`. As a kind, a glass window is not sayable: it becomes an effect-layer node stacked over a
surface node, which is two transforms and two corner radii that agree only while nothing moves, and
every frame in which they disagree is a bright seam around a translucent panel. As a field, the
sketch's intent survives untouched — a container dressed `Glass` draws the blurred backdrop and
nothing else, which is what an effect layer was.

The same slot takes the second dressing.
[Decision 96](#96-the-frame-is-the-compositors-and-the-header-is-the-apps) puts window shadows on an
`Elevation` — named levels rather than a blur and an offset, which is decision 33's argument applied
to a third axis. Two bytes, each orthogonal to the kind and to the other, because a glass panel casts
a shadow too.

#### A container is a node with subnodes and no content

The kind the sketch has no room for, and it is forced rather than convenient.
`wl_subsurface.place_below` names the *parent surface itself* as a legal reference, so a subsurface
may sit beneath its parent's own pixels. Decision 55 makes z the list order, so the only encoding of
that in a preorder run is a container whose children are the below-subsurfaces, the parent's own
surface, and the above-subsurfaces, in that order. An ordinary toplevel is therefore already a
container, alongside every opacity group (decision 60), every workspace, and every overview grid.

Without the kind, a container is a fully transparent `Solid`: one draw item per container per frame,
sampling nothing and covering nothing, on a node whose whole purpose is to hold a transform.

#### `Solid` is mostly gyro's own, and the boot path is why it survives

It is the thinnest of the four, so it is worth saying what it is for: the background before or without
a wallpaper, the firmware background colour
[decision 37](#37-gyro-owns-the-display-from-firmware-handoff-onward-there-are-no-vts)'s continuous
image continues into, the letterbox fill when a wallpaper's aspect does not match the mode, and the
dimming overlay [Open.md](Open.md)'s *backlight without a backlight* contemplates. A shell wanting a
coloured rectangle uses its own surface; this is not really shell vocabulary.

The alternative is a 1×1 texture stretched, and it loses in one place, which happens to be the place
that matters: `Blit` ([decision 79](#79-the-console-is-a-renderer-not-a-presenter)) paints a fill with
no device, no import, and no texture lifetime, and `Blit` is the renderer running at the moment the
handoff from firmware has to stay continuous.

#### Content rides in per-kind runs

Decision 90's rule one level over. A node carries a kind and one index-or-sentinel; the kind says
which run the index is a position in, and a container names nothing. `Image` is a texture id, the
texels to sample, the rect the frame treatment applies to
([decision 96](#96-the-frame-is-the-compositors-and-the-header-is-the-apps)), and a colour state —
forty-eight bytes. `Solid` is four components and a colour state — twenty-four.

**A colour state belongs in those runs rather than on the node.** A container has no pixels and so has
nothing that means anything as light; eight bytes on every node in the tree to say so is the padding
decision 90 refused for springs, arriving on a different field.

**A reference needs no run at all**, because its whole payload is one node index — so `Content` *is*
the target when the kind is `Reference`. One field with two readings, and they are the same reading: a
reference's content is a node.

#### A reference points backwards, and a cycle stops being expressible

Decision 88 owes acyclicity and a bounded reference depth to the frame side, and decision 90 declines
to let that promise be the only thing between a bug and the machine. Requiring the target's index to
be *lower* than the referencing node's discharges the first half structurally: a cycle cannot be
written down, so there is nothing to detect and nothing to trust.

What it costs is an authoring order — a subtree is published before every presentation of it — which
is how an overview is written anyway: the real windows near the top of the run under a hidden
container, the thumbnails below pointing back at them. A reference expands the referenced root's flags
as authored, which is why the originals are hidden by hiding their *parent* rather than each of them.

#### Decision 90's depth counter is not the work bound, and the arena is

*(A reading that changes decision 90 rather than restating it.)* That entry bounds the walk with a
depth counter and a subtree-length check, against an unbounded traversal at `SCHED_FIFO` whose
survivable outcome is [decision 22](#22-gyro-runs-as-a-dedicated-unprivileged-uid-with-cap_sys_nice-and-nothing-else)'s
`RLIMIT_RTTIME` taking every session's UI at once. **Depth is not that bound once the scene is a DAG.**
Sixty-four nodes, each a container holding two references to the one before it, is a depth of
sixty-four and 2⁶⁴ emissions — the exact failure that entry was written to prevent, reached through
breadth rather than through a cycle, and reached *inside* the depth cap rather than by exceeding it.

The bound that does hold is one the design already has and had not been asked to carry: decision 82's
draw list is built into `Frame`'s own arena, sized by the admitted plan, because
[decision 36](#36-frame-path-discipline-is-enforced-mechanically-not-by-review) forbids allocating
there. **The walk stops when the arena is full**, stated here as behaviour rather than left as a
property of the storage. A malformed or hostile tree then draws a short frame — the same conservative
direction as decision 90's missing thumbnail, and as every other ingest in the codebase.

#### Rejected

**Rejected: a union inline in the node record.** The obvious shape, and it charges every container and
every group twenty-four bytes to carry what only a leaf has. It is decision 90's rejected inline
spring at a smaller scale and it loses the same way — the common node is the one with nothing in the
union, and the walk drags the payload through cache in order not to use it.

**Rejected: keeping surface and snapshot apart, per decision 51.** The honest reading of where pixels
come from, and the render seam had already collapsed it. Two kinds is a kind that changes while a
spring is running.

**Rejected: `EntityId` as the reference target**, resolved through a side table.
[Decision 15](#15-identity-is-a-generational-handle)'s handle is what identity is everywhere else, so
it reads as the safer choice. It costs a lookup per reference inside the frame section and a table
published beside the run, and it buys nothing an index does not: decision 88 is explicit that a
reference is *not* a second identity, so the target is a position in a tree rather than a thing to
name.

**Rejected: forward references with a cycle check.** More expressive by exactly the cases nobody has,
and the check is what the backward rule removes. A rule that makes the bad state unrepresentable beats
a test that catches it, on a walk where the test would run per node per frame.

**Rejected: a node kind that knows what a window is.** The set says nothing about toplevels, popups,
or thumbnails, which is decision 51's fisheye-dock test holding: the moment the scene knows what a
window is, the arrangements that are not windows stop being expressible.

**Rejected: clipping and masking** — an omission rather than a refutation, recorded so it is not
assumed away. A node's children are not clipped to its extent, and `Group` is not a substitute, since
its offscreen sits at the subtree's own screen-space bound and therefore *contains* the overflow
rather than cutting it. What that costs is an overview tile that cannot crop a window with a popup
hanging off it. Recoverable by a shell clipping in its own surface, and carried in
[Open.md](Open.md).

**Rejected: blend modes beyond `over`**, which is not yet a question anybody has asked and is cheaper
to answer before they do. Multiply and screen are defined in the encoding their operands arrived in,
so one node over two backdrops in different colour states would mean two different things — decision
47's arithmetic giving way at the one place a screenshot would not show it.

#### Consequences

`World/Node.h` gains `Kind`, `Material`, `Elevation`, and `Content`, taking the record from 120 bytes
to 128 — two cache lines exactly, where 120 straddled, so the walk's indexing becomes a shift.
`Material` moves from `Seam` to `World` as decision 91 said it would, and `Seam` gains its `World`
edge. `SnapshotRun` gains a run per content kind.

**`SnapshotVersion` does not move, and the trigger is worth stating once rather than deciding again
per record.** Decisions 86 and 90 each say the version bumps "which is free while both halves of the
boundary still ship together", and that clause is an argument for why bumping *costs* nothing rather
than a reason to do it. What the number is for is a reader that could be looking at a writer's bytes
across a version gap, and there is no such reader: one binary, and nothing has been run. The per-run
check is already stronger for everything below the header — `RunEntry` carries the writer's element
size and alignment and the reader compares them against the type it is asked to resolve, so a node
record that drifts yields an empty run rather than a reinterpretation, without the version being
consulted at all. **The version starts earning its keep when there are two artefacts that can
disagree**, which is [decision 49](#49-the-restart-boundary-is-made-cheap-where-it-can-be-and-stated-where-it-cannot)'s
dispatch-as-a-separate-process, and it moves once when that arrives rather than once per field.

What stays open is the *contents* of the two dressing enums rather than their shape: which materials
exist and which of them are pointwise (decision 33), and where the elevation levels sit. Both ship
with the one enumerator the design already names for itself and are filled in at a review with a
screen, the way [Catalog.h](../Source/Animation/Author/Catalog.h) says its own entries will be.
*(That review is [decisions 103 to 105](#103-a-dressing-is-named-by-what-it-does-to-light-the-material-set-is-glass-and-smoke),
and it cost one more field than this entry expected: a shadow has to animate, so the record gains a
scalar — which the reserved tail absorbs at 128 bytes unchanged — 2026-08-22.)*

### 96. The frame is the compositor's and the header is the app's

*(Decided 2026-08-22, on asking how a shadow reaches a node. Revises
[decision 48](#48-linear-blending-is-a-visible-ecosystem-change-and-gyro-takes-it)'s stated exit,
which cannot be reached.)*

**Decoration is not one thing, and `xdg_decoration`'s single client-or-server choice is a category
error rather than a coarse approximation.** It bundles six things that have three different owners:

| Part                  | Owner              | Why                                                                                              |
| --------------------- | ------------------ | ------------------------------------------------------------------------------------------------ |
| Shadow                | the compositor     | It depends on what is behind the window and on the blending model, neither of which a client sees |
| Corner and silhouette | the compositor     | At every window edge in every screenshot, which makes it the most cohesion-critical pixel there is |
| Border, focus outline | the compositor     | Focus is state the compositor already owns                                                        |
| Resize region         | the compositor     | Decision 51 already keeps resize out of the round trip                                            |
| **Header contents**   | **the app**        | Firefox's tab strip, a terminal's tabs, an IDE's toolbar — a compositor cannot lay out a tab bar  |
| Window controls       | negotiated         | The compositor knows which buttons the user configured and in what order; the app may place them   |

A toolkit that refuses server-side decorations is refusing row five, and the protocol makes it refuse
rows one through four along with it. That is the whole defect, and it is why the system that gets this
right does not offer the choice at all.

**Cohesion lives in the frame and the motion, not in the header — which is decision 51's own argument
one level down.** That decision says the shell may invent any arrangement and cannot invent a spring.
The same claim for applications: an app may invent any header and cannot invent a shadow, a
silhouette, or the way its window opens. Safari, Xcode, and Finder share nothing above the content
area and read as one machine, which is the evidence the split is in the right place; a system title
bar forced above Firefox's own tab strip is the evidence that the other one is not.

**macOS's consistency comes from one shared toolkit rather than from server-side drawing, and that is
the half that does not transfer.** `WindowServer` computes a window's shadow from its alpha channel —
`-[NSWindow invalidateShadow]` exists because the server caches a silhouette and has to be told when
the shape changed — but the rounded corner is drawn by AppKit's frame view, in process, and it is
identical everywhere because every application links the same AppKit. Linux has no such toolkit. So on
Wayland the frame is the compositor's or it is nobody's, and *nobody's* is the status quo.

#### A server-side frame is shell chrome, parented into the window's subtree

gyro cannot draw a title.
[Decision 25](#25-the-lock-screen-is-a-client-the-compositor-owns-lock-state-not-lock-ui) keeps text
shaping and layout out of a `SCHED_FIFO` process, and decision 51 already makes panels, switchers, and
notifications the shell's; a decoration frame is that category. So an application that wants a frame
gets one from the shell, and gyro places it as a **child node of the window's own node**.

**That arrangement is only available here.** Every other compositor draws the frame in process
precisely because a separate one cannot keep up with a window being dragged. Under decision 95's tree
the frame composes under the same transform in the same frame as the window it decorates, so it cannot
shear or lag by construction — and a maximize animates the window and its frame as one object because
they *are* one object. Cross-process cohesion is free here for the reason decision 51 gives about
shared `t₀`, arriving a second time through the transform.

**The floor is no title bar rather than no window.** With no shell — boot, restart, a wedged one — a
window that asked for a frame is drawn without one and stays usable, since gyro owns move, resize, and
close as mechanism. The negotiated mode is *held* across the gap rather than flipped to client-side,
which would force a redraw in every affected client twice per restart to buy a few hundred
milliseconds of title bar on a machine that is visibly recovering anyway.

#### Elevation is a field the compositor may decline to draw

Decision 95 puts an `Elevation` on every node — named levels, with the shell declaring which windows
are lifted and gyro deciding what a level costs, which is decision 33's rule on a third axis. **Under
client decoration gyro sets it to none and draws nothing**, because the client has already drawn a
shadow into its buffer and two shadows are worse than either one.

The vocabulary carries no notion of client-side decoration in order to do that. Window management
knows the negotiated mode and sets the field; the scene stays a scene. It is the same separation that
keeps a blur radius out of `Material`.

#### A minimum corner radius, applied to the window geometry rect

gyro rounds every window to at least a floor radius, and *minimum* is what makes that safe. Against a
client's own radius `r`:

- gyro's `R` greater than `r` — gyro's cut lies inside the client's curve and removes it whole, so the
  corner is gyro's.
- gyro's `R` smaller than `r` — gyro's cut lies outside the curve and removes nothing visible, so the
  corner is the client's.

There is no value at which the two blend into a corner belonging to neither, which is exactly what a
*fixed* radius produces and is why the floor is stated as one. Set at the low end of where toolkits
already cluster, it does nothing to GTK, Qt, or Firefox and squares up the outliers — Xwayland, SDL,
Java — which is also the population with no client-drawn shadow to conflict with.

**It applies to the window geometry rect rather than to the buffer**, which is the one thing this
needs from a client and the reason `Image` content carries a frame rect. A client that draws a shadow
sets `xdg_surface.set_window_geometry` to its visible bounds — it must, or every compositor tiles it
with gaps — so rounding the node's whole extent would round a corner of the shadow margin that nobody
can see. For a client that never sets it, the frame rect is the whole extent and the rounding is
simply right.

**The X11 half of that was wrong, and the frame rect for an X11 client is one gyro computes.**
*(Revised 2026-08-22.)* This paragraph read "for a client that never sets it, which is most of
Xwayland". No X11 client ever sets it, because Xwayland gives a rootless client no window geometry to
set — so the whole extent is right only where the client draws no shadow, and where it draws one
gyro's own window manager reads `_GTK_FRAME_EXTENTS` off the X connection and computes the rect. See
[decision 106](#106-an-x11-client-has-no-window-geometry-gyros-window-manager-computes-the-frame-rect).

**And it applies without discarding the margin.** The rounded-rect test is bounded to the frame rect,
and fragments outside it pass through untouched, so nothing a client drew outside its declared bounds
is ever thrown away. The failure mode is gyro rounding a rect that was not the window — visible and
local — rather than gyro silently deleting content.

**Radius goes to zero when a window is fullscreen or tiled edge to edge**, from gyro's own layout state
rather than from anything a client says. Black corners on video is the failure that rule exists to
prevent, and [decision 67](#67-the-settled-snap-is-unconditional)'s settled snap is what makes the
tiled case exact rather than nearly so.

**One artefact, named rather than discovered.** A client that both draws a generous shadow and rounds
more tightly than the floor shows a light crescent at each corner, where gyro's cut pulls the window
back past where that client's shadow begins. It needs both conditions at once, and it is the argument
for setting the floor at the low end rather than at whatever looks best on gyro's own windows.

#### Rejected: cropping the buffer to the window geometry rect

The complete answer, and what would make decision 48's residue reducible in principle: sample only the
visible bounds, and a client's shadow never reaches the composite at all. Rejected because the safe
version of it is not available.

Trusting the hint means discarding whatever a client drew outside its declared bounds, unseen, on
every frame forever. The verifier that would fix that — scan the inset at import and refuse to crop a
margin holding opaque texels, which is
[decision 63](#63-effects-declare-their-kind-and-their-damage-the-verifier-keeps-them-honest)'s rule
applied to a second hint — works for `wl_shm` and does not work for dmabuf, where it is a GPU readback
on the import path. So the check is unavailable for exactly the buffers that carry the case, and what
is left is trust.

It stays available, and it is worth revisiting the moment a client can *tell* us it has stopped
drawing a shadow — at which point there is nothing left to crop.

#### Rejected: server-side decorations as the answer, which is decision 48 as written

That entry answers the flattened-shadow problem with "the answer is that gyro draws the shadows",
reduces the exposure to clients insisting on CSD, and accepts the residue *"until it adopts
server-side decorations"*. GTK has declined server-side decorations as a matter of policy, so as
written that is not a horizon: it is a permanent cost, on most of what an ordinary user runs.

The correction is that CSD-insisting and shadow-insisting are not the same refusal. A toolkit wants
row five of the table above and has no position on rows one through four — and it already publishes
the extent of its shadow to every compositor on the wire. **The exit is a protocol that lets it say
so.**

#### The ecosystem move, and it asks for nothing

`xdg_decoration`'s all-or-nothing shape is why it was not adopted, so proposing a broader version of
it fails the same way. What is missing is smaller and points the other direction: **the compositor
publishes its frame parameters — corner radius, focus treatment, control set and order, and whether it
draws shadows — and clients draw their own to match.** Informational, nothing surrendered, no header
bar given up, and the same shape as `wp_fractional_scale` or `xdg_toplevel.configure_bounds`, both
adopted quickly for that reason.

A protocol that asks GTK to give up its header bar will not be adopted. One that tells GTK what radius
to use, and that gyro will draw the shadow if it stops, might be — and it retires the corner residue
rather than relocating it, which the crop does not.

**What gyro draws once a client stops.** The shadow is derived from the window's own alpha silhouette,
which is `WindowServer`'s mechanism above and needs no radius agreement, no shape protocol, and no
special case for a tooltip with a tail or a terminal at eighty percent opacity. Cached and invalidated
on shape change, which is
[decision 46](#46-exit-snapshots-come-from-a-pre-reserved-per-output-atlas-exhaustion-finishes-exits-early)'s
per-output atlas doing a second job.

*(Revised 2026-08-22 by [decision 104](#104-an-elevation-is-a-height-under-one-light-and-the-shadow-is-analytic),
which makes the analytic rounded-rect the default and keeps this as the second path for a genuinely
irregular shape. A silhouette needs a blur chain per shape, which puts every shadow on screen onto
decision 34's ladder — so the first thing a busy machine would spend is the depth of the whole
picture. It is also undefined for two of the four node kinds, since a container and a reference have
no alpha and decision 99 dresses both.)*

#### Rejected: gyro drawing the title bar itself

The obvious shortcut, and it wants a text shaper, a font stack, and a layout engine inside the process
[decision 49](#49-the-restart-boundary-is-made-cheap-where-it-can-be-and-stated-where-it-cannot)
prices as ruinous to restart — which decision 25 already refused once, for lock screens, on exactly
these grounds. It also has to be *configurable*, since button order and title formatting are
per-desktop taste, and that is policy inside the system layer, which is decision 51's whole objection.

#### Rejected: a fixed corner radius rather than a floor

Stronger cohesion on paper. It requires gyro to cut *inside* a client's curve wherever that client
rounds more than gyro does, discarding an antialiased edge and replacing it with one at a different
radius — and to know the client's radius in order to tell when that is happening, which nothing on the
wire says. The floor needs neither, and has no value at which it produces a corner belonging to
neither party.

**Left open.** Where the floor radius sits is a number and wants a screen rather than an argument, as
does the elevation set. Both are in [Open.md](Open.md). What the frame rect means for an X11 client
was the third, and reading Xwayland settled it in
[decision 106](#106-an-x11-client-has-no-window-geometry-gyros-window-manager-computes-the-frame-rect).

### 106. An X11 client has no window geometry; gyro's window manager computes the frame rect

*(Decided 2026-08-22, on reading Xwayland to settle what
[decision 96](#96-the-frame-is-the-compositors-and-the-header-is-the-apps) left open. The reading
answered a different question than the one asked, and revises 96's fallback rule.)*

**Xwayland does not forward `_GTK_FRAME_EXTENTS` into window geometry, because a rootless X11 client
has no window geometry to forward it into.** The question assumed a translation that was missing;
what is missing is the destination. The reading, against upstream `867976b` of 2026-08-20:

- **`_GTK_FRAME_EXTENTS` appears nowhere in the X server tree.** Neither does any spelling of frame
  extents. Xwayland watches exactly one X property — `_XWAYLAND_ALLOW_COMMITS`, whose writes XACE
  restricts to the window-manager client and the server itself.
- **`xdg_surface.set_window_geometry` is never called.** The sole `set_window_geometry` in the whole
  repository is Xephyr's XCB host code, which is a different thing entirely.
- **There is no `xdg_surface` on a rootless client surface at all.** `xwl_create_root_surface` is
  guarded by `!rootless` and wraps the *root* window, for the nested and `-rootful` cases. A
  rootless client gets a bare `wl_surface` plus `xwayland_surface_v1`, bound at version 1, whose one
  request is `set_serial`. There is no role object on it carrying a geometry field.
- **The complete per-surface state Xwayland sends is six requests**: `attach`, `damage`, `commit`,
  `set_buffer_scale`, `set_input_region`, and `set_serial`. `set_opaque_region` and the viewport
  requests exist only on the rootful path. Enumerating what does cross is what makes this
  conclusive; a grep that finds nothing only proves the grep.

**So the frame rect for an X11 client is gyro's to compute, in gyro's own X11 window manager, over
the X connection.** The property is read off the client window and subtracted from the window's
bounds, and the result is what decision 96's radius applies to. This is not a workaround: it is
where the property has always been consumed. On this machine the atom is present in `libmutter-18`,
`libgtk-3`, `libgtk-4`, `libKF5WindowSystem`, and `libwnck-3` — every toolkit that draws a shadow
and every window manager that has to account for one — and absent from the shipped `Xwayland`
binary and from all of libwayland. It is a window-manager protocol that never had a Wayland leg.

**What breaks if gyro skips it.** A GTK or Qt application under X11 draws its own shadow into a
buffer larger than the window, so rounding the buffer's extent puts gyro's corners on the outer edge
of the shadow — a rounded rectangle hanging in empty space several pixels outside where the window
visibly ends, on every X11 window on the screen. That is more conspicuous than not rounding at all,
and it is the artefact decision 96 rounds the frame rect specifically to avoid. The same rect is
what [decision 104](#104-an-elevation-is-a-height-under-one-light-and-the-shadow-is-analytic)'s
analytic shadow is cast from, so getting it wrong offsets gyro's shadow from the window as well as
its corners — and on a client already drawing its own, offsets it from that one too.

**Revises decision 96's fallback.** That entry said the frame rect is the whole extent "for a client
that never sets it, which is most of Xwayland". No X11 client ever sets it, and the whole extent is
right only for the ones that draw no shadow — `xterm`, an SDL game, anything pre-CSD. Those are
still the majority and the fallback still holds for them; it is now the fallback for a client with
no `_GTK_FRAME_EXTENTS` on it rather than for X11 generally.

#### Rejected: taking the visible rect from the input region

`wl_surface.set_input_region` is the one geometry-adjacent thing that does cross, fed from the X
input shape by `xwl_window_set_input_region`. If a toolkit excluded its shadow from the input shape,
gyro would have the visible rect on the Wayland connection already, with no X round trip and no
property to track. It does not: GTK's `update_shadow_width` calls `gdk_x11_window_set_shadow_width`,
which does one `XChangeProperty` of `_GTK_FRAME_EXTENTS` and nothing else, and `gtkwindow.c` never
combines an input shape for the shadow. The input region is unset for the windows this matters for,
and where a client does set one it is answering a different question — where clicks land, not where
the window is.

#### Rejected: asking for the forwarding to be added upstream

`xwayland_surface_v1` could carry the extents, and Xwayland already parses X properties for
`_XWAYLAND_ALLOW_COMMITS`. It would be the wrong place: the compositor running Xwayland is by
construction the X11 window manager as well, so it already has the connection, already reads the
property for the other things a window manager needs it for, and would be receiving over Wayland
something it can read directly. The forwarding buys a round trip's latency for a second source of
truth to disagree with.

**Left open.** The extents arrive on the X connection and the buffer arrives on the Wayland
connection with no ordering between them, so a resizing GTK window can present a frame cut at the
previous extents — corners in the wrong place for a frame or two, exactly when the eye is on the
window. `_XWAYLAND_ALLOW_COMMITS` is the lever, since gyro is the window-manager client permitted to
write it and clearing it holds Xwayland's commit until the new property has been read. Whether a
stall per resize frame is worth paying is a question for when the X11 half exists; it is in
[Open.md](Open.md).

---

## Presentation timing

Detail in [Architecture.md](Architecture.md#presentation-timing). Mixed-refresh multi-monitor — a
144 Hz laptop panel beside a 60 Hz projector — is where compositors are worst today and where gyro
should be decisively best. It is also an everyday configuration rather than an exotic one.

### 28. The frame clock is per output

Each output owns a `FrameClock` fed by its own presentation feedback. There is no clock for the
machine, and no global predicted presentation time on the render path. A frame is
`(output, predicted presentation time)`.

This is what [decision 11](#11-springs-are-closed-form-not-numerically-integrated) buys, and the
payoff is larger than the one originally recorded for it. Because animation is a pure function of
time, one scene can be evaluated at two different presentation times in the same iteration and both
results are exact — so a window dragged across the seam between two outputs is correct on both
simultaneously. Anything integrating numerically has one mutable timeline and one delta per
iteration, and therefore *cannot* express this at all.

**Rejected: one global clock.** This is the near-universal design and it produces the near-universal
bug: plug a 60 Hz external into a 144 Hz laptop and the panel's animations drop to 60. Mutter and
KWin both moved to per-output clocks years ago, which fixes scheduling while leaving the shared
`Tick(delta)` timeline underneath, so the visible symptom survives the fix.

**Rejected: one global clock at the fastest rate, gated per output.** Render everything at 144 and
present to the 60 every other-ish frame. Refresh rates are not integer multiples, so gating beats
against the true period, and it burns power rendering frames for an output that cannot show them.

**A frame is also `(output, device grid)`.** *(Revised 2026-08-16.)*
[Decision 54](#54-settled-geometry-snaps-to-the-outputs-device-grid) gives per-output evaluation a
second job. One model position must resolve to different device-aligned positions on outputs of
different densities, so the snap has nowhere else it could live — and it costs almost nothing,
because the traversal was already per output for the reason above.

### 57. One timebase: `CLOCK_MONOTONIC`, converted at ingest and nowhere else

Every timestamp inside gyro is monotonic nanoseconds. Conversion happens once, at each ingest, and
no other code in the process observes a clock at all.

**This does not contradict decision 28, and the distinction is the whole content of both.** There is
no global *clock* — no machine-wide predicted presentation time, and a frame is
`(output, presentation time)`. There is exactly one *timebase* — one dial every clock in the system
reads. The first says outputs do not share a schedule; the second says their timestamps are
comparable. Neither implies the other, both are required, and a design that states only the first is
the one that discovers the second by accident.

gyro does not get to choose the dial. Every interface it already speaks chose for it:

| Ingest                              | Native form                                        |
| ----------------------------------- | -------------------------------------------------- |
| libinput                            | `CLOCK_MONOTONIC`, **microseconds**                 |
| DRM page flip                       | `CLOCK_MONOTONIC` **iff `DRM_CAP_TIMESTAMP_MONOTONIC`** |
| `wp_presentation_feedback` (nested) | whatever the host advertised                        |
| io_uring timeouts                   | `CLOCK_MONOTONIC` unless a flag says otherwise      |
| `wp_presentation` → clients         | gyro advertises `clock_id`, once, at bind           |

**The last row is what makes this a decision rather than a convention.** `clock_id` is a global
property sent at bind time and never revised, but gyro's outputs are per device and hotpluggable. A
gyro that adopted whatever clock its DRM device offered would silently change the meaning of every
timestamp already delivered to every media player on the machine the moment a user plugged in an
adapter whose driver does not set the capability. So the advertised clock is `CLOCK_MONOTONIC` by
fiat, and a device that disagrees is converted at *its own* ingest, per device, with the offset
re-estimated when [decision 41](#41-device-migration-is-exercised-on-every-boot) migrates.

**Rejected: `CLOCK_BOOTTIME`, for idle timers or anywhere else.** It looks correct and it is the
interesting rejection, because Linux's `CLOCK_MONOTONIC` does not advance across suspend and
`CLOCK_BOOTTIME` does. Mixing them is a bug that appears exactly once, on the first lid-open: the
frame ring's timers and `FrameClock`'s observations end up in domains separated by the suspend
duration, every output believes it is hours late, and the timing policy in
[decision 35](#35-a-miss-costs-one-frame-bounded-by-the-floor-composite) fails every branch forever.
Untestable in CI, invisible nested, and it lands on real hardware the first time somebody shuts a
laptop.

What makes the rejection clean rather than merely cautious is that **there is no legitimate
`CLOCK_BOOTTIME` consumer in this design**, and the apparent one is instructive. *Lock after fifteen
minutes; the lid was shut for forty of them* wants suspended time counted, and under a monotonic
timer the machine resumes with three minutes elapsed and does not lock. The fix is not a second
clock, it is lock-on-suspend as an explicit rule —
[decision 59](#59-suspend-is-a-handshake-on-the-control-connection-resume-is-a-modeset) — which is
wanted anyway for reasons that have nothing to do with timers. Generally: **every place that seems
to want wall time including suspend is a place that wants to react to the resume event.** One clock
plus an event beats two clocks, because the two clocks agree everywhere except the one case nobody
tests.

**Rejected: `CLOCK_REALTIME` anywhere but a log line.** It steps — NTP, timezone, a user correcting
it — and a stepped clock on the frame path is a frame scheduled into last Tuesday. A clock widget
wants real time and a clock widget is the shell's problem, which is to say a client's.

**Rejected: `CLOCK_MONOTONIC_RAW`**, which is tempting precisely because it is the unslewed one. NTP
frequency-slews `CLOCK_MONOTONIC`, and DRM vblank timestamps are slewed identically because they are
the same clock. Using the raw one would introduce a drift against the only timestamps it would ever
be compared against — correctness bought at the cost of the property that mattered.

Three consequences, all cheap now and none retrofittable:

- **Instants and durations are different types.** The `FrameClock` sketch originally returned
  `Nanoseconds` for both `NextDeadline()` and `Period()`, which is the conflation that lets a
  cross-domain subtraction compile. `Instant - Instant → Duration`, `Instant + Duration → Instant`,
  and `Instant + Instant` ill-formed.
- **Absolute timeouts only**, `IORING_TIMEOUT_ABS` against `NextWakeup()`. Converting an instant to
  a relative timeout requires sampling *now* and races with everything between the sample and the
  submit — and the frame loop's whole thesis is that it does not have races with itself.
- **`clock_gettime` is reachable from exactly one place.** This is
  [decision 36](#36-frame-path-discipline-is-enforced-mechanically-not-by-review)'s enforcement
  argument applied one level lower than it was written. Not merely no ambient *now* on the render
  path — the time source itself is injected, or the headless fake clock is a fiction and golden
  images are not reproducible.

**What it buys beyond hygiene.** With one domain, input-to-photon is a subtraction rather than an
estimate: kernel event timestamp, publication, record, submit, flip timestamp, all comparable, per
output, logged and regression-tested. That also *contains* the injected-input clock offset the
[open list](Open.md) carries against
[decision 26](#26-remote-presentation-is-a-virtual-output-with-client-supplied-targets) — a remote
machine's clock is a foreign domain, and with this decision it is converted by one estimator at one
ingest rather than leaking inward to be discovered later as "remote feels strange".

### 29. Outputs are periodic real-time tasks; the test allocates effect budget

Outputs are genuinely independent. The only thing coupling them is one frame thread and one GPU
queue, so this is the classic periodic task problem and is treated as exactly that: each output has
a period `P` and an execution time `C`, deadline equals period, scheduling is
earliest-deadline-first, and composites are non-preemptive.

**`P` is a minimum inter-arrival time, not an observed period.** *(Added 2026-08-16.)*

On a fixed-refresh output those are the same number and the distinction is invisible, which is why
this was written the other way first. On a variable-refresh one they are not, and taking the
observed period silently voids the guarantee this decision exists to make: the test's whole claim is
that it holds over every interval without watching the schedule, and an output whose arrivals a
client controls may demand a frame at the panel's maximum rate at any moment, having demanded them
at half that rate for the previous hour. So `P` is **the shortest interval in which an output may
demand a frame**. That is the standard sporadic-task reading, processor-demand analysis holds under
a minimum separation exactly as it does under a period, and no symbol in the test below changes.

What it costs, and who decides which reading applies to a given output, is
[decision 66](#66-arrival-control-is-an-input-to-admission-control).

**`C` is a budget gyro enforces, not a cost it observes.** *(Revised 2026-08-15.)*

`C` is not a property of an output. It is a property of an output *and what is on screen*, and the
peak is not a full workspace sitting still — it is an overview transition with a dozen blurred
thumbnails animating at once. Configuration time is precisely when that is least knowable. Taking
`C` as a measured worst case permanently degrades an output for a peak that occurs seconds per hour;
taking it as a recent observation means re-running the test continuously, which is the reactive
design this decision exists to reject.

So the test is read in the other direction. `P` and the floor composite cost are given, and the test
is solved for the `C` each output may spend. Admission control's output is not a verdict but an
allocation — *this output may spend 2.9 ms per frame* — and a configuration is infeasible only when
that allocation falls below the floor composite.

Two things make this expressible rather than wishful. Effects are declared as materials rather than
as filter calls ([decision 33](#33-effects-are-named-materials-not-parameterized-filter-calls)), so
quality is gyro's to choose. And commits are declarative
([decision 14](#14-declarative-commits-with-dirty-tracking)), so gyro knows what it is about to draw
before it draws it — the peak does not have to be predicted at plug time because it is capped at
commit time, one frame ahead, by the only party that decides what a frame contains.

Measurement does not disappear. Its role changes from input to validation: measured cost confirms
the allocation was honest and steps the quality tier when it was not ([decision
34](#34-effect-quality-is-a-tier-gyro-chooses-and-the-floor-tier-is-the-recovery-path)).

**Scope, stated honestly.** Plain composition is not a scheduling problem and has not been for a
decade. A 4K composite of opaque windows, with occlusion culling and damage tracking, is a fraction
of a millisecond on any integrated GPU of the last ten years, and a static desktop costs
approximately nothing. What makes `C` large is blur, which also defeats damage tracking because a
blurred backdrop must re-render whenever anything behind it moves. The population this protects is
therefore narrow and worth naming rather than implying: **integrated graphics driving a large
external display, during blur-heavy moments.** On a discrete GPU with two ordinary panels the
allocation is generous and never binds. This is not a headline feature — it is what keeps the
headline feature from being the reason the machine stutters.

**On the software device, infeasible is the ordinary case.** *(Added 2026-08-16.)*
[Decision 40](#40-software-rendering-is-a-device-not-a-backend-and-it-is-the-floor-tier) makes
lavapipe a device rather than a backend, so this test runs there unchanged — and on two outputs it
will routinely solve to an allocation below the floor composite, because a floor composite on a CPU
rasterizer is not small. That state is not an error to report and refuse.
[Decision 41](#41-device-migration-is-exercised-on-every-boot) guarantees it on **every single
boot**, before the real driver has loaded, and it is also what a machine with a broken GPU driver
runs on permanently.

Every rung is gone in that state. There is no quality tier beneath the floor;
[decision 31](#31-vrr-is-a-scheduling-degree-of-freedom-not-only-a-latency-feature)'s lever needs a
VRR panel that `simpledrm` does not expose; and decision 30's chunking and early rendering are
demoted contingencies that will not exist for a long time. What remains is the last rung —
[decision 30](#30-budget-shortfalls-are-answered-by-spending-less-chunking-and-early-rendering-are-contingencies)'s
deliberate frame drop, taken from the output without input focus with ties broken toward the slower
refresh — applied as steady-state policy rather than as a contingency, so an output runs at the rate
the device can actually sustain instead of the rate it advertises.

Stated because it was previously reached by falling off the end of this decision rather than by it:
**`Admit()` returns a degraded plan, never a refusal.** A configuration the user asked for is always
presented. Infeasibility selects how much is given up and where, and the answer is never a black
screen — which matters most precisely on the path that has no GPU to blame it on.

**The schedulability test takes phase as no input at all.** It is a processor-demand test over
interval lengths, not a simulation of a schedule, so it holds at every phase relationship by
construction. A task set that passes meets every deadline at every phase relationship and the
outputs never influence each other. One that fails is not rescued by modelling phase — nor, and this
is the form of the claim that looks like an exception, by *controlling* it. Variable refresh makes a
phase relationship something gyro can hold rather than merely observe;
[decision 31](#31-vrr-is-a-scheduling-degree-of-freedom-not-only-a-latency-feature) records why that
still does not make it a rung.

```
	U    = Σᵢ Cᵢ / Pᵢ

	h(L) = Σᵢ ⌊L / Pᵢ⌋ · Cᵢ            demand — work with a deadline at or before L
	B(L) = max{ qₖ : Pₖ > L }          blocking — the longest non-preemptible chunk of
	                                    any output whose deadline falls after L

	feasible  ⟺  U ≤ 1  and  ∀ L ∈ 𝓛 :  h(L) + B(L) ≤ L
```

`qₖ` is output `k` 's largest non-preemptible chunk, which is `Cₖ` for an unchunked output.
[Decision 30](#30-budget-shortfalls-are-answered-by-spending-less-chunking-and-early-rendering-are-contingencies)
's chunking enters here and only here: it reduces `B`, never `h`. That is the precise statement of
why chunking helps and why it cannot help a set that fails on `U`.

**`B` is sized to composite cost, and nothing that is not a composite may enter it.** *(Added
2026-08-17.)* Stated because it was an assumption rather than a rule for as long as nothing else
asked. Every `qₖ` above is a composite, so `B` is bounded by what
[decision 34](#34-effect-quality-is-a-tier-gyro-chooses-and-the-floor-tier-is-the-recovery-path)'s
tier already bounds — but nothing in the test notices a term arriving from somewhere else, and
several want to: a mode set, a connector probe holding a device-wide lock across an EDID read, a
DPMS transition, a buffer import during migration. Any one of them inline is a `B` an order of
magnitude past the 2.9 ms this test quotes a 60 Hz projector, and it is charged to every output at
once rather than to the one being serviced.
[Decision 73](#73-the-frame-thread-initiates-reconfiguration-and-never-performs-it) settles the first
of them and states the rule generally: such work is *initiated* by the frame thread and performed
elsewhere, so it never becomes a `qₖ`.

`𝓛` is every multiple of every `Pᵢ` up to the synchronous busy period `L* = Σ Cᵢ / (1 − U)`, capped
by the hyperperiod. `L*` diverges as `U → 1`, so sets above `U = 0.95` are rejected before the loop
runs — a set with no headroom for estimation error is not one to admit anyway. Time is integer
nanoseconds throughout, so the floors are exact rather than an approximation of a continuous model.

Two outputs reduce this to the rule previously recorded here. At `L = P_fast`, `h = C_fast` and
`B = C_slow`, giving `C_fast + C_slow ≤ P_fast`: on a 144 Hz panel with a 4 ms composite beside a
60 Hz projector, **the projector's whole composite must fit in under 2.9 ms.** Utilization is only
0.88 — the set is utilization-feasible and non-preemptively infeasible, so the slack exists but is
in the wrong place. Every mechanism in decisions 30 and 31 exists to move work into it.

**Rejected: `C_fast + max(C_other) ≤ P_fast` as the test itself.** This was the form first recorded
here and it is necessary but not sufficient, because it accounts for one fast job plus blocking
rather than the accumulated demand of every job due within the interval. Two 144 Hz panels at 3 ms
each beside a 60 Hz projector at 2 ms passes it — `3 + 3 ≤ 6.944` — and misses: with the projector's
job in flight at the critical instant, the second panel completes at `2 + 3 + 3 = 8 ms` against a
6.944 ms deadline. Utilization is 0.98 and every individual pair is fine. Three displays is an
ordinary desk, so this is not a corner.

**Protocol dispatch and input are not in the set.** *(Revised 2026-08-16.)*
[Decision 45](#45-protocol-dispatch-is-a-thread-not-a-task) puts them on a thread the frame thread
outranks and never blocks on, so they are preempted rather than accumulated and enter neither `h`
nor `B`. The set is the outputs, and only the outputs.

This is not bookkeeping. The dispatch task's period was the *fastest* output's, so it entered `h` at
the highest multiplicity of any term in the test — in the 144 + 60 case above it was competing
directly against the 2.9 ms the projector is quoted. Removing it does not just simplify the test, it
hands that budget back to composites.

**Rejected: dispatch as a task in the set.** The original form, and correct for as long as dispatch
shared the frame thread. It is kept because it names the general rule, which is not "work on another
thread is free": **work belongs in the test exactly when the frame thread waits on it.** Decision
40's device workers are inside `C` by that rule despite being other threads; dispatch is outside it
because nothing above it ever waits.

**Rejected: a rolling collision forecast with a reactive escalation ladder.** This was the design
first recorded here, and it is the wrong shape. It solves per collision, at runtime, from observed
phase, a problem that has a static phase-independent answer — and relying on observed phase is
specifically invalid, because drift guarantees every phase relationship eventually occurs. Drift's
only role in the design is that negative one: it makes getting lucky impossible, which is why demand
over intervals is the only honest thing to design against.

The test runs at configuration change — output added or removed, mode set, or a measured cost moving
the floor — and the frame loop afterwards executes a static plan rather than deciding anything. The
one runtime timing decision it retains is the record-time check in
[decision 35](#35-a-miss-costs-one-frame-bounded-by-the-floor-composite).

**Rejected: `C` as a measured worst case, with the test as a verdict.** This was the form first
recorded here and it fails on its own terms. Worst-case `C` is a scene property, unknown at
configuration time, and dominated by a peak that is both rare and compositor-driven. A test fed a
number it cannot know either refuses configurations that work or admits ones that do not, and there
is no way to tell which from inside the test.

**Rejected: fixed priority, primary output first.** Simpler, but "primary" is a UI concept with no
timing meaning, and it starves the same output every time, making the second monitor permanently and
consistently worse.

**Rejected: round-robin or fair share.** Ignores deadlines, which are the only thing that matters.

**Rejected: a render thread per output.** Genuinely attractive here — the scene is safe to read
concurrently precisely because evaluation is pure, so none of the usual locking objections apply. It
is rejected because it parallelizes the cheap half: two outputs' GPU work serializes on the queue no
matter how it was recorded, so threads help command recording and not execution, and this is a
GPU-time problem. Available later if CPU recording ever dominates; it is not the answer to this.

### 61. The frame thread is `SCHED_FIFO`; the earliest-deadline-first schedule is gyro's, not the kernel's

The frame thread runs `SCHED_FIFO` at a fixed priority, and decision 29's earliest-deadline-first
schedule over outputs is executed in userspace. Linux ships a kernel EDF scheduler —
`SCHED_DEADLINE`, a constant-bandwidth server over global EDF — and a document that describes its own
scheduler as *"the frame thread is a uniprocessor running them earliest-deadline-first"* owes a
reason for not using the one in the kernel. Four, and the first is decisive on its own.

**`sched_attr` cannot express the task set.** A `SCHED_DEADLINE` thread carries one
`(runtime, deadline, period)` triple. Decision 29's frame thread serves N outputs with N periods
against one non-preemptive submission path, and there is no encoding of that in a single
reservation. Both escapes cost more than they return:

- **A thread per output, each with its own reservation.** This dissolves the uniprocessor model the
  test rests on — the kernel would spread them across CPUs under global EDF — and, worse, it admits
  on the wrong resource. The kernel's admission test is over *CPU* bandwidth. The term that decides
  feasibility here is `B`, the blocking from a non-preemptive composite already in flight on a GPU
  the scheduler cannot observe. The 144 + 60 case above is utilization-feasible at `U = 0.88` and
  non-preemptively infeasible; a kernel admitting on `U` says yes to exactly the set this decision
  exists to catch. gyro would keep its own test regardless, and then run two schedulers that
  disagree.
- **One thread with `period = P_fast` and `runtime = Σ C`.** Expressible, and it asserts the trivial
  bound while discarding the analysis. It is `U ≤ 1` with extra syscalls.

Re-arming the reservation per frame with the next deadline is the third variant, and it puts a
`sched_setattr` on the frame path to redefine a CBS period while that period is live. The frame
loop's thesis is that it has no races with itself.

The single-output case fits `SCHED_DEADLINE` perfectly, and that is the tell rather than the
consolation. A scheduling policy that is expressive for one display and not for three is the same
failure [decision 28](#28-the-frame-clock-is-per-output) rejects on the clock axis, arriving one
layer down.

**Throttling is the wrong answer to an overrun, and decision 29 guarantees overruns.** CBS enforces
by descheduling: exceed `runtime` and the thread does not run again until its next period, at
whatever instruction the overrun lands on. That is not a rare fault here. `C` is a property of the
scene as much as of the output — the argument above turns on it — so a mispredicted `C` is the
expected case, and the recorded answer to it is
[decision 34](#34-effect-quality-is-a-tier-gyro-chooses-and-the-floor-tier-is-the-recovery-path)'s
tier step and [decision 35](#35-a-miss-costs-one-frame-bounded-by-the-floor-composite)'s bounded
single-frame miss, both of which require the thread to keep running in order to execute them.

The sharp edge is not the dropped frame, which happens either way. It is **lock-holder preemption
that nothing can undo.** Throttled inside `vkQueueSubmit`, the frame thread holds a driver-internal
lock for the remainder of its period and every device worker needing that lock stalls behind it.
This is the inversion
[decision 40](#40-software-rendering-is-a-device-not-a-backend-and-it-is-the-floor-tier)'s priority
ordering exists to prevent, inflicted from above rather than from below, and deadline inheritance
does not reach it — a throttled task is not *blocked on* anything, so there is no holder to boost.
On a machine with no VT — [decision 37](#37-gyro-owns-the-display-from-firmware-handoff-onward-there-are-no-vts)
— that is a poor shape to build in deliberately.

`RLIMIT_RTTIME` already provides the half of this that is wanted. It measures continuous CPU time
without blocking, which for a thread that blocks every frame is a per-frame runtime watchdog — but
delivered as a signal gyro reacts to, not as a deschedule the kernel imposes.

**Admission can fail at a modeset, and decision 29 has no vocabulary for that.** `sched_setattr`
returns `EBUSY` when a reservation does not fit the root domain's remaining bandwidth, which is
capped by `sched_rt_runtime_us` — 95% by default — and shared with every other `SCHED_DEADLINE` task
on the machine. The moment gyro would re-arm is a hotplug or a mode set: the least margin, the least
notice. Decision 29 ends on **`Admit()` returns a degraded plan, never a refusal.** A kernel `EBUSY`
is a refusal arriving from a layer gyro cannot renegotiate with, so a `SCHED_FIFO` fallback would
exist anyway and both paths would be carried.

**The reservation would guard the resource that does not bind.** The frame thread is blocked on a
semaphore for most of a frame; what delays a composite is a client's batch ahead of gyro's in the GPU
queue, which is
[decision 22](#22-gyro-runs-as-a-dedicated-unprivileged-uid-with-cap_sys_nice-and-nothing-else)'s
entire subject and is answered by `VK_QUEUE_GLOBAL_PRIORITY_HIGH`. CPU bandwidth is the resource
already in surplus.

Four smaller frictions, none decisive and all real:

| | Under `SCHED_DEADLINE` |
| --- | --- |
| Affinity | Constrained to the root domain rather than freely set; shielding a core means partitioning with cpusets, on a boot service that must come up unconfigured on arbitrary hardware |
| Placement | Global EDF migrates the frame thread across CPUs; a pinned `SCHED_FIFO` thread keeps its working set |
| Worker ordering | No rung exists between `SCHED_DEADLINE` and `SCHED_FIFO` 99, so decision 40's *workers one below the frame thread* stops being expressible and flattens |
| Privilege | Always requires `CAP_SYS_NICE`, re-coupling real-time scheduling to the capability decision 22 deliberately took `LimitRTPRIO=` to avoid |

`fork()` from a `SCHED_DEADLINE` task also fails with `EAGAIN` absent `SCHED_FLAG_RESET_ON_FORK`.
Moot if only the frame thread carries the policy, and worth knowing before it is not.

**What `SCHED_DEADLINE` would buy, at full strength.** It is the only mechanism on Linux that makes
gyro unpreemptible by other real-time tasks — an `rtkit`-boosted audio thread, anything a user
`chrt`s — which for a process whose pitch is *one compositor per machine, hitting every frame* is
not nothing. And `SCHED_FLAG_DL_OVERRUN` delivers `SIGXCPU` on overrun, a free budget-overrun
detector. Both are obtainable more cheaply: a `SCHED_FIFO` priority chosen above what `rtkit` grants
gets most of the first, and `RLIMIT_RTTIME` is the second without the deschedule.

**Rejected: `SCHED_DEADLINE`, one reservation per output thread.** The strongest form, and it trades
the model in decision 29 for kernel admission over a resource that is not the constraint.

**Rejected: `SCHED_DEADLINE` re-armed per frame.** A syscall on the frame path mutating a live CBS
period.

**Rejected: `SCHED_DEADLINE` over-provisioned, `SCHED_FLAG_RECLAIM` on, used only for isolation.**
The coherent middle, and the one worth stating: `runtime` set well above the userspace allocation so
throttling never fires, CBS taken purely for its precedence over `SCHED_FIFO`, gyro's own EDF still
doing the real work. It buys one property — immunity to other real-time tasks — at the cost of the
affinity constraint, the runtime admission failure, and the flattened priority ordering, and it does
not clear.

**Rejected: `SCHED_RR`.** Its timeslicing is only visible among threads of equal priority, and there
are none. It adds a preemption point and buys nothing.

**Rejected: `SCHED_OTHER`, with or without `nice`, which is what every other compositor does.** The
real datum, and it is also why every other compositor is compared on features rather than on frame
pacing. A boot service with no VT to fall back to is the wrong process to leave at the mercy of an
unrelated build.

Two consequences that are constants rather than arguments:

- **The priority number is chosen against what `rtkit` grants, and written down.** `rtkit`'s ceiling
  is well below 99 and PipeWire asks for a specific level; gyro's frame thread must sit above both,
  its device workers one below it, and dispatch below that — with a stated number for each rather
  than a constant discovered by reading the source later. `LimitRTPRIO=` in the unit is the ceiling
  that has to accommodate them.
- **`sched_rt_runtime_us` is the same knob in both designs, and Architecture.md was imprecise about
  it.** A spinning `SCHED_FIFO` thread is not quite a hard lock under the default 95% cap —
  `SCHED_OTHER` still gets 5% of a CPU. It is a hard lock on a tuned system that has set the knob to
  `-1`, and on a defaulted one it is a machine with 5% of a CPU, a frozen display, and no VT, which
  is the same deployment outcome by a slower route. `RLIMIT_RTTIME` and decision 37's `sysrq`
  requirement both stand; the justification is corrected, not withdrawn.

### 30. Budget shortfalls are answered by spending less; chunking and early rendering are contingencies

When decision 29's allocation is smaller than the current scene wants, the first response — and
almost always the only one — is to **spend less**: step the effect quality tier down on the output
that can least afford it, per
[decision 34](#34-effect-quality-is-a-tier-gyro-chooses-and-the-floor-tier-is-the-recovery-path).
The first rung of that ladder is close to invisible, it composes with everything else, and it costs
no renderer machinery at all.

Two mechanisms remain for the case where the allocation falls below the floor composite — where
there is genuinely no quality tier that fits. Both move work into slack that exists elsewhere in the
cycle, which is what an infeasible-but-under-utilized set needs.

**Both are gated on measurement and neither is first-cut work.** *(Demoted 2026-08-15.)* They are
real renderer surgery — chunking restructures submission, early rendering needs a third target per
output and pulls frame callbacks forward — and they exist to rescue a case that spending less on
effects should already have handled. Building them before a measured configuration demonstrates the
quality ladder is insufficient is building the expensive answer to a problem that may not survive
contact with the cheap one. Chunking additionally rests on an assumption nobody has verified: that a
submission boundary is a scheduling opportunity for the GPU rather than merely a place we stopped
recording.

What already exists is the half that costs nothing: the frame loop names the frame it is recording
rather than deriving it from the last one presented, so a deeper pipeline is a number the loop
states and an early frame's animation is evaluated at its own presentation time. Everything else
described here is unbuilt.

**Chunking** splits an output's work at render-pass and submission boundaries, reducing the blocking
term from `max(C_other)` to `max(chunk)`. Blur pass chains split naturally, which is where `C` comes
from in the first place.

**Early rendering** produces a frame one period ahead of its normal record time and holds the target
for its flip. In scheduling terms it relaxes the release-time constraint: the job may run in any
window within the period preceding its deadline rather than only the one immediately before it.

**It splits a frame into two scheduled events, and only the first one moves.** The commit must still
land in the frame's own window: issued at the early record point it would flip at the *previous*
frame's vblank, showing the early frame a period early and the one before it not at all. So the
output still wakes at its ordinary commit point, and what that wake costs is a `Present` rather than
a composite. An output at lead `k` therefore contributes the earlier of two instants to the wake
fold — the placed record point of the frame it is producing, and the commit point of the frame it
already holds.

**An early frame is bound by its placement, not by its own deadline.** That deadline is `k` periods
away, so [decision 35](#35-a-miss-costs-one-frame-bounded-by-the-floor-composite)'s record-time
check would admit the work however long it ran; what has to hold is the end of the gap the schedule
placed it in, since overrunning *that* is what blocks the fast output. The binding deadline is the
earlier of the two, and at lead 0 the placement is the deadline — which is why the check reads as
one number today.

Neither alone is general. Early rendering fixes cases where the slow job fits in *some* gap — in the
144 + 60 example the gap is 2.944 ms and a contiguous 5 ms composite fits in none of them. Together,
chunk to fit the gaps and spread across several fast periods, they approach the preemptive EDF bound
of `U ≤ 1`, which is the uniprocessor optimum.

The content trade on early rendering is exact, which is what makes it safe: **animation state is
correct** — `Evaluate(T)` is exact for any future T, so nothing is extrapolated — and **client
content freshness is the entire cost**. So the policy follows from what an output is showing:
compositor-driven content such as a workspace switch loses literally nothing; a fullscreen video or
game loses a frame. Gated per output on whether any surface there committed recently, and **never
applied to the input-focused output.**

Frame callbacks are pulled forward with the frame. Because the schedule is static and known, clients
on that output can be asked for content earlier — recovering freshness entirely for clients that
render for a stated presentation time, and neutral for those that draw on callback.

**Rejected: rendering everything a frame ahead**, i.e. a deeper pipeline. Pays a frame of latency on
every surface all the time to fix a configuration-specific problem.

**Rejected: dropping frames as the first response.** Retained as the last rung when nothing else
applies, with the loser being the output without input focus, ties broken toward the slower refresh.
As a first response it chooses a visible artefact over an invisible one. Its trigger is not a policy
judgement but the record-time check in
[decision 35](#35-a-miss-costs-one-frame-bounded-by-the-floor-composite).

**Cost accepted:** an output using early rendering needs three targets in flight — scanning out,
pending flip, being recorded. The ring depth is also what enforces the bound: `AcquireTarget()`
answers nothing while every target is held, so a double-buffered output cannot run ahead whatever
else is true, and a lead is something a plan grants rather than something the frame loop falls into.

**This is not triple buffering in the sense that costs latency.** The conventional version pipelines
every frame through a three-deep queue and pays a period on all of them. Here the third target is
*allocated*, not *occupied*: pipeline depth stays at one, recording still happens late at
`deadline − renderBudget − safety`, and the extra period is paid only on frames actually rendered
early — on an output that, by the rule above, nobody is interacting with. The cost is VRAM. Steady
state input latency is unchanged, and the focused output's is unchanged unconditionally.

### 31. VRR is a scheduling degree of freedom, not only a latency feature

When admission control finds a set infeasible, a variable-refresh output's period is the first input
adjusted — it is the only term in the test that can change without giving anything up. This is also
the only control gyro has over *when* a vblank happens, and a VRR laptop panel beside a fixed
external is a common configuration.

Rate-limited, converging over several frames rather than jumping, which answers three constraints at
once: staying inside the panel's supported range, avoiding the brightness flicker many panels show
on abrupt refresh changes, and keeping each step below perceptibility.

**The lever is the period and never the phase.** *(Added 2026-08-16.)* The original form of this
decision said "phase and period", and half of that was wrong. Lengthening a period changes `P` and
therefore changes what
[decision 29](#29-outputs-are-periodic-real-time-tasks-the-test-allocates-effect-budget)'s test
computes; aligning a phase changes no input to it and therefore cannot move a set from infeasible to
feasible. The servo converges on a target period, and a phase relationship is whatever falls out.

**Rejected: phase servoing as a rung**, which is what "phase and period" implied. It is not merely
inert — it is unsound in a specific way. Variable refresh does make phase controllable rather than
merely observable, so a servo could hold a favourable relationship that
[drift](Architecture.md#phase-drift-is-not-something-to-track) would otherwise walk through. But a
set admitted on that basis is feasible only while the lock holds, and the lock breaks at the two
moments least convenient for it: when the panel's range runs out, and when a client takes the
refresh rate under decision 66. That converts a scheduling guarantee into a servo's uptime. Phase
alignment may improve an already-feasible set — coalescing two outputs' wakeups is a real if
unmeasured power argument — and may never be the reason one is admitted.

**The servo commands a period on the `FrameClock`; the presentation seam is untouched.** *(Added
2026-08-16.)* `NextDeadline()` moves, the frame loop arms its existing timer against it, and on KMS
with variable refresh enabled a flip presents at or after its commit — so shaping the clock is
shaping presentation. The clock keeps the commanded period and the observed one separately, because
they disagree throughout every ramp and permanently on a panel that will not comply.

**Rejected: a target presentation time on `Present()`.** The obvious shape, and it buys one thing
gyro does not need — expressing the servo nested, where the host owns the vblank and a host window
has no VRR range to servo within anyway. It costs the seam's direction: the backend feeds the clock
through `Observe()` and the clock never commands the backend, and a validated target time inverts
that for a request no backend can honestly validate, since panels misreport their ranges. Closing
the loop on an observation instead is both smaller and the right shape for a controller whose plant
lies. What would reopen it is tearing control, which maps to `DRM_MODE_PAGE_FLIP_ASYNC` and is
genuinely per-present rather than a property of the clock. Enabling variable refresh on the CRTC at
all is not this: it is set with a mode, and belongs to
[decision 73](#73-the-frame-thread-initiates-reconfiguration-and-never-performs-it)'s reconfiguration
path.

**The lever is flip cadence; the property is configuration.** *(Added 2026-08-17, from the kernel
reading behind decision 73.)* This decision was written as though *enabling* variable refresh were
the act with scheduling consequence. It is not. Once VRR is active the effective refresh interval is
controlled entirely by **when a flip is submitted** — on i915 by the `TRANS_PUSH` write that
terminates the stretched vblank, issued for every ordinary commit, and on amdgpu by a vmin/vmax
adjustment reached from the per-flip surface-address path. No property is set and no modeset occurs.
The servo's period command therefore reaches the panel through the clock and the flip timing it
already shapes, exactly as the paragraph above claims, and the claim is stronger than it knew: the
mechanism it describes is the *only* one, not a convenient one.

Toggling `VRR_ENABLED` is the opposite. i915 forces a full modeset on every toggle and routes it to
an ordered device-wide workqueue; amdgpu handles it cheaply. Since the lever does not need the
toggle, **variable refresh is enabled at configuration time and modulated by cadence thereafter**,
which costs nothing on either driver and is not a rung the servo reaches for.

**The range is discovered, not negotiated**, and this is a real input the test did not have.
`[vmin, vmax]` is derived from the current mode's timings together with the connector's EDID-reported
range, with no userspace-settable property for it on i915 at all. The window is fixed when the mode
is set, so the servo's authority is bounded by a number gyro does not choose and learns only after
choosing a mode. That makes it an input to admission control rather than a footnote to the servo —
and it is one of the things [KernelWishlist.md](KernelWishlist.md) records gyro would rather
negotiate.

**Rejected: a keepalive commit to hold an idle panel's rate.** It would arm a timer on a static
screen, which [decision 58](#58-idle-is-a-ladder-gyro-executes-and-does-not-choose) forbids outright
and which would land hardest on battery-powered machines, the same ones most likely to have a VRR
panel. Nothing needs it — the panel holds its last scanned-out buffer unaided. The clock does go
bad, and differently from the fixed case: an idle fixed panel keeps its period and merely
accumulates drift, while an idle VRR panel has changed period, so its last observation is wrong
rather than stale. Going idle therefore invalidates the clock, and the next frame is damage-driven,
presents when ready, and re-seeds it.

**Rejected: VRR as a latency feature only**, present-when-ready and nothing else. That is what
everyone does and it leaves the one available scheduling lever unused.

**Rejected: servoing against content-driven VRR.** If a client is driving the refresh rate for its
own reasons it wins, and admission control falls through to the next rung. The consequence for the
test is not a matter of policy and is worked out in
[decision 66](#66-arrival-control-is-an-input-to-admission-control), which is the price of this
decision rather than a separate subject.

*(Narrowed 2026-08-17.)* This rejection stands and its scope does not. It reads *content-driven* as
one thing, and [decision 76](#76-cadence-authority-follows-predictability-not-foreground) divides it:
a client whose next arrival gyro cannot predict cannot be servoed against, which is the case argued
here and is unchanged, while a client that has **stated** its cadence can be servoed *to*. Matching a
declared period is not fighting a producer for the rate; it is the servo doing what it already does
against a setpoint it was handed rather than one it chose. So what is rejected here is servoing
against *unpredictably* content-driven VRR, and the predictable case — a video player naming its
timestamps, which is the commonest fullscreen content there is — never reaches this alternative at
all. The original wording said "a fullscreen client" and that word was doing no work; it is dropped
above rather than kept, since decision 76 removes fullscreen from the predicate entirely.

Written from specification; panel ranges, flicker thresholds, and whether a cursor-plane commit
disturbs the refresh timer are marked `// SPEC:` where they land in code. The servo's own logic is
not in that category — it commands a period and reads back an observation, both of which the
headless fake clock can supply, refusal included.

### 66. Arrival control is an input to admission control

*(Added 2026-08-16.)* Decision 29 makes `P` the shortest interval in which an output may demand a
frame. Decision 31 makes a variable-refresh output's period gyro's to lengthen — but only while gyro
is the one deciding when frames are demanded, and decision 31 also hands that decision to a
fullscreen client whenever one wants it. So `P` for a VRR output depends on **who controls its
arrivals**: the commanded period when that is gyro, and the panel's minimum period when it is a
client, regardless of the rate that client is currently choosing. Control changing hands is
therefore a change to an input of the test, and re-runs it.

The rule reads like bookkeeping, so it is worth recording as the failure it prevents. gyro servoes a
144 Hz panel to 100 Hz to fit a projector beside it, and admits the set. A game goes fullscreen and
takes the refresh rate. `P` returns to 6.944 ms, demand rises past what the projector was quoted,
and the projector begins missing — *quietly*, because [decision
35](#35-a-miss-costs-one-frame-bounded-by-the-floor-composite) contains a miss by falling to the
floor tier rather than by reporting one. Under the previous, event-shaped trigger list — output
added or removed, mode set, measured cost moving — nothing had changed, so admission control never
re-ran and never stepped the projector's quality tier, which had been available throughout. The
trigger is now stated as any input to the test, and this is one.

**The plans are precomputed, not solved on the transition.** `𝓛` is not a usefully bounded loop and
the frame thread cannot host it. It does not have to: control of each VRR output is binary, so a
configuration with `n` of them has `2ⁿ` plans, `n` is one or two on real hardware, and decision 29's
`Admit()` solves all of them at configuration change. The transition is a plan swap, which preserves
the property that the frame loop runs a schedule it was handed. Beyond a small `n` the surplus
outputs are planned as client-controlled, which is the pessimistic direction and cannot produce a
miss.

**Detection errs toward client control.** Reading a client as in control when it is not costs a
rung; reading it as not in control when it is costs another output its deadlines. The readings are
not symmetric, so the policy is not either: the client-controlled state is entered readily and left
only on hysteresis, the same shape decision 32 needs for cadence and for the same reason.

*(Completed 2026-08-17.)* What establishes it was carried as an open question about a signal until
[decision 76](#76-cadence-authority-follows-predictability-not-foreground) found there is no such
signal and that the condition itself had never been stated. The predicate is that the output carries
an active producer whose next arrival gyro cannot predict — a client that commits and never says when
it will commit next. The asymmetry above is what makes that the right way to read silence: a client
that has stated nothing is assumed to have taken the rate. The hysteresis survives, and applies to
the inference fallback rather than to a client that has spoken.

**Rejected: budgeting every VRR output at its minimum period, always.** One plan, no detection, no
hysteresis, no swap — and it is the honest conservative answer, which is what makes it worth stating
why it loses. It forecloses decision 31 entirely. A period gyro cannot rely on holding is not a
lever it can be admitted against, so rung 1 disappears and VRR collapses back to the latency-only
feature decision 31 exists to reject. The machinery here is precisely the price of that rung, and
the rung is worth it: it is the only response to an infeasible set that costs the user nothing at
all.

**Rejected: holding the flip to rate-limit a client back to its admitted period.** This converts an
uncontrolled arrival process back into a controlled one and would make the whole problem disappear.
It also spends exactly the latency variable refresh exists to buy, on the output most likely to be
the focused one, which inverts decision 30's rule that the output nobody is interacting with is the
one that gives something up. Falling through to the quality tier instead takes the cost where it is
not felt.

**Rejected: re-evaluating continuously rather than on transition.** The reactive design decision 29
rejects, arriving by another road. A client's instantaneous rate is not the input; which party may
choose the rate is, and that changes rarely.

**Why the cost lands on the projector** is cited rather than argued.
[How it degrades](Experience.md#how-it-degrades) already fixes it: the display without your attention
gives way, ties go to the slower one, and effects give way before frames do. A game holding the
focused panel's refresh rate while the projector spends an effect tier is that rule applied
literally. It is also why the flip-holding alternative above is foreclosed rather than merely
worse — it would take the cost from the panel under someone's hands, which that promise refuses
unconditionally. What remains unwritten there is narrower than it looks: not what happens to the
rest of the machine, which follows from focus, but what a full-screen application is promised
*directly* rather than as a consequence of being the focused one.

### 76. Cadence authority follows predictability, not foreground

*(Decided 2026-08-17, against wayland-protocols. Settles the client-cadence entry in
[Open.md](Open.md), supplies the predicate
[decision 66](#66-arrival-control-is-an-input-to-admission-control) left open, and narrows
[decision 31](#31-vrr-is-a-scheduling-degree-of-freedom-not-only-a-latency-feature)'s rejection of
servoing against content-driven VRR.)*

An output is **client-paced when it carries an active producer whose next arrival gyro cannot
predict**, and gyro-paced otherwise. Being fullscreen grants nothing.

**The rule this replaces was circular.** Architecture.md had it that a fullscreen client *driving the
refresh rate for its own reasons* takes the decision away, and resolved the conflict in the client's
favour. That resolution is sound and is kept. But it never says *when* the client has the rate — it
defines client control as the client controlling — so the term Open.md had been carrying as a
question about a signal could not be answered in that form. It was asking what evidence establishes a
condition that had never been stated. The missing piece is a predicate, and a predicate is not a
signal.

**No client can state a rate, and the protocol set is not going to grow one.** Read at
`wayland-protocols` afb614d5, where all three candidates are still `version="1"`:

- **`wp_fifo_v1`** is a readiness constraint, not a rate. `set_barrier` arms a condition at a latching
  deadline and `wait_barrier` holds the next update until it clears, so a client pairing them is
  throttled to one update per refresh cycle. It reads *backwards* from what decision 66 wanted: a
  client using fifo has handed cadence to the compositor. It is also advisory twice — the compositor
  may clear the barrier early for forward progress, and may ignore it entirely when the surface is
  occluded — and the protocol tells clients not to rely on it for throttling.
- **`wp_commit_timing_v1`** is a deadline for one frame: present as closely as possible to, but not
  before, an instant in the compositor's presentation clock. No interval and no repetition, and at
  most one outstanding, since a second raises `timestamp_exists`. Exactly one frame of horizon.
- **`wp_tearing_control_v1`** is `vsync` or `async`, and the protocol says outright that the
  compositor may respect or ignore it dynamically. It is about tearing against latency and says
  nothing about a rate — neither necessary, since a variable-refresh client typically asks for
  `vsync` and takes its latency from the variable period, nor sufficient.

**Rate information in Wayland flows compositor to client and never back.** Grepping the whole
protocol tree for a refresh rate returns two hits, both in `presentation-time`'s `presented` event,
both describing what the compositor tells the client. A client never asks for the rate because it has
no request with which to ask: it commits, and a variable-refresh flip presents when it commits. The
transfer of control is gyro's own act. **There was never a signal to find, and the search was for
evidence of something gyro already knew.**

**So the predicate is predictability — which is the one thing a client can state.** The three
protocols answer a question adjacent to the one they were consulted for, and answer it well:

| What the client does | What gyro can do | Authority |
| --- | --- | --- |
| Posts a `wp_commit_timing_v1` timestamp | Hit the named instant | gyro's |
| Pairs `wp_fifo_v1` barriers | Pace it at the refresh cycle it asked for | gyro's |
| Commits freely, states nothing | Nothing but react | the client's |

**The fold already holds this: a free-running client is an active producer contributing no `Wake`.**
It commits, and it never says when it will commit next. That is checkable rather than inferred, it is
naturally sticky in a way a timing heuristic is not — a protocol binding is stable across a client's
life where an observed rate flickers frame to frame — and it errs the way decision 66 requires,
because a client that says nothing is read as having taken the rate.

**One question at two timescales, which is why decision 73 wanted the same thing.** Open.md recorded
*two decisions, one signal* and was right, though not for the reason it gave. Occupancy is *when is
the next arrival*, read per instant, and it is what decision 73's deferral predicate needs. Authority
is *can the next arrival be predicted at all*, read as a standing property of a surface, and it is
what decision 66 needs. Both are the same information at different timescales, and both reduce
through [decision 69](#69-settling-answers-with-a-wake-idleness-folds-a-monoid-not-an-or)'s fold —
which gains a contributor class it did not have, since it covered gyro-authored motion and not a
client committing steadily. A timestamped commit is `Timed`; a standing fifo pairing is `Continuous`
at the refresh period. Without this the predicate reads *settled* between two video frames and
reconfigures into the hitch it exists to prevent.

**What it changes for a person.** The blast radius of the old rule was every fullscreen surface. A
fullscreen video player, a slide deck, a terminal — all of them took the rate on a state that says
nothing about timing, and each one cost a second display a quality tier under decision 66. Under this
predicate a video player that states its cadence never takes the rate at all and the projector pays
nothing; a slide deck and a terminal are not continuous producers and never entered the question; and
an actual free-running game takes the rate and the projector spends a tier, which is decision 66
working as written. The worked example in that decision is a laptop beside a **projector**, where the
likeliest content on either display is video — the exact case the old rule got wrong.

**It also sets the incentive the right way round.** The more a client states about its cadence, the
more scheduling authority gyro keeps, and the better gyro's promises to *other* outputs hold. A
client that says nothing is treated pessimistically and costs someone a tier. That gradient improves
as the ecosystem improves without requiring it to, which is the opposite of a rule where declaring
fullscreen seizes the panel.

**The claim is bounded: strictly better where the protocols are used, no worse where they are not.**
Almost nothing uses fifo or commit-timing yet, so inference from observed commits carries the load
initially, and that is where decision 66's hysteresis constant still lives. This decision does not
retire that constant. It confines it to the case where nothing better is available, and makes the
answer exact wherever a client has spoken.

**Rejected: fullscreen as the trigger** — the position this replaces. Fullscreen is a
window-management state standing in for a timing declaration, and it is wrong in both directions: a
fullscreen video player gets authority it does not want, and a windowed game gets none though its
situation is identical. Fullscreen does carry a real fact — it is what makes direct scanout available,
and therefore what makes a high rate affordable — but that is a *cost* argument, and
[admission control](#29-outputs-are-periodic-real-time-tasks-the-test-allocates-effect-budget) already
owns cost. Conflating cost with authority is most of why the rule read as arbitrary. Removing
fullscreen from the predicate does not lose the fact; it relocates it to where it was handled anyway.

**Rejected: hunting for a positive *I have the rate* signal.** The shape Open.md's entry assumed, and
an afternoon of reading says it does not exist and cannot, because the direction of the protocol is
wrong. Kept because the entry was well-formed and still unanswerable: it named its source, which is
what let it be retired, and what the source said was *no*.

**Rejected: `wp_content_type_v1` as the classifier.** `video` and `game` are the populations this
decision sorts, and it is the only one of the four that describes a standing property rather than
per-frame state. It is also explicitly a hint the compositor may ignore, so it can never carry a
scheduling guarantee. It has one real use — classifying a fullscreen game before it has committed
anything, which is a latency argument on the handover rather than a correctness one — and that is a
refinement this decision does not need to take a position on.

**Rejected: two independent mechanisms, one per decision.** Reached first, and abandoned when the
circularity above came out: with authority defined as a policy fact gyro asserts, the two questions
look unrelated, and the reasoning that they need opposite biases is sound as far as it goes.
Splitting them costs the observation that makes both cheap — that they read one fold at two
timescales — and would have built a second detection path for a question already answered by the
first.

**Left open: a game that pairs fifo barriers.** Under this predicate it keeps gyro in control, which
follows from the protocol's own semantics and differs from what other compositors do and possibly
from what such a client expects. Recorded as unsettled rather than resolved by the general rule,
because it is the one case where the predicate and the client's likely intent disagree.

**Advertising fifo and commit-timing is a consequence and not a prerequisite.** The inference
fallback is adequate for the budget decision 73 sets, so the protocols improve the answer without
gating it. They are named in [Architecture.md](Architecture.md#filtered-globals) so the reason to
advertise them is on record, and the scope of protocol support is settled elsewhere and later.

### 32. A surface's frame cadence follows its fastest output

A surface visible on two outputs gets one `wl_surface.frame` cadence, driven by the fastest output
it touches, with hysteresis, and never switched mid-animation.

One cadence is forced rather than chosen: a surface has one buffer and one frame callback queue, so
per-output cadence is not expressible in the protocol.

Deferring the switch to the end of a surface's own animation is precise rather than heuristic,
because settling time is analytic — another consequence of decision 11.

**Rejected: the slowest output touched.** Reproduces exactly the bug decision 28 exists to remove.

**Rejected: the output holding most of the surface's area.** Sounds principled and oscillates during
a drag across the seam. The visible artefact is the *switch*, not the rate, so a policy that
switches more often is worse even when each individual choice is better justified.

### 73. The frame thread initiates reconfiguration and never performs it

*(Decided 2026-08-17, against the kernel's DRM core. Settles the `IPresenter` mode-setting entry in
[Open.md](Open.md), and revises the presumptive answer recorded there on 2026-08-16.)*

`IPresenter` gains a second verb. `Reconfigure()` is invoked from the frame thread when a per-output
generation carried in the snapshot moves, it **returns before the hardware is programmed**, and
completion arrives as an event the loop already polls for. `Present()` may never block: no
device-wide lock, no wait on another output's commit. An output between the two is not presenting,
and the loop tolerates that.

**Five callers want a mode set, and none of them is the frame thread deciding.** Adopting the
firmware mode at boot; [device migration](#41-device-migration-is-exercised-on-every-boot) when the
real driver displaces `simpledrm`;
[resume](#59-suspend-is-a-handshake-on-the-control-connection-resume-is-a-modeset); a user or
configuration changing an output's mode; and
[decision 31](#31-vrr-is-a-scheduling-degree-of-freedom-not-only-a-latency-feature)'s variable-refresh
enable. The first three are composition-root sequences already; the fourth is dispatch-side policy;
the fifth is a property of the mode. The frame thread only ever *observes* the result, which is what
`TargetsInvalidated` already is.

**Which is why the obvious placement is closed off.** Putting mode setting on a dispatch-side
interface makes `TargetsInvalidated` a signal across the publication boundary, and
[Structure.md](Structure.md#orchestration) has signals intra-thread. So the operation is on the seam
not because the seam wanted it but because the *observation* is already there and cannot move. The
snapshot carries a per-output generation; the loop transitions when the generation moves; two
channels stay two; and the ordering of a mode change against the scene change that assumes it is
defined by construction rather than arranged separately.

**The frame thread initiates and does not execute, and this is the half the kernel decided.**
`drm_atomic_nonblocking_commit()` runs the full check — including the driver's `atomic_check` hook —
synchronously on the calling thread before queueing anything, so `DRM_MODE_ATOMIC_NONBLOCK` defers
commitment and not validation. amdgpu uses that latitude to take every modeset lock on the device
and wait on every CRTC's outstanding commit, on the caller's thread. Inline, that is a `B(L)` term of
a frame period against a test that quotes a 60 Hz projector 2.9 ms. See
[KernelWishlist.md](KernelWishlist.md) for the readings and their line citations.

**The general rule, of which this is the first instance.**
[Decision 29](#29-outputs-are-periodic-real-time-tasks-the-test-allocates-effect-budget)'s `B(L)` is
the longest non-preemptible chunk on the frame thread, and it is sized to composite cost because
until now nothing else asked to enter it. **Nothing that is not a composite may.** Mode setting is
one member of a class with others already visible: a connector probe holds `connection_mutex` across
an EDID transaction for tens to hundreds of milliseconds, DPMS is ~100 ms by
[Architecture.md's own table](Architecture.md#idle-and-power), and buffer import during migration is
unbounded. Each would otherwise arrive as its own local puzzle and be answered inconsistently.

**Adoption is an argument, and the kernel performs the check.** *Adopt the existing mode rather than
modeset unconditionally* is load-bearing for re-exec, for the `simpledrm` handoff, and for crash
recovery. It does not need a query path: the kernel demands `ALLOW_MODESET` only when the committed
state actually differs, so **committing without the flag is the assertion that this is an adoption,
and `-EINVAL` is the kernel answering that it is not.** Try bare, escalate on rejection. The check
belongs to the party that knows, and gyro never reads back hardware state to make it.

The flag is therefore **never a constant**. On AMD silicon below `IP_VERSION(3, 2, 0)` any commit
carrying it resets every plane on the CRTC regardless of what changed, turning an ordinary flip into
the device-wide stall above. Setting it defensively is the natural reading of the fast path and it is
the expensive mistake.

**Reconfiguration waits for quiet, within a tight budget.** The stall is invisible when nothing is
moving, so the policy is to reconfigure when nothing is — and the predicate is a read of
[decision 69](#69-settling-answers-with-a-wake-idleness-folds-a-monoid-not-an-or)'s fold rather than
a clock. `Settled` reconfigures immediately. `Timed(when)` inside the budget defers to `when`, an
instant already known. `Continuous`, or `Timed` beyond the budget, reconfigures now and takes the
hitch. **gyro never waits speculatively**: it defers only where quiet is known to arrive and known
when, which is exactly the distinction `Wake` was given a third case to express.

*(Completed 2026-08-17.)* The fold this reads had a hole when this decision was written: it covered
gyro-authored motion and not a client committing steadily, so an output showing nothing but a video
folded to *settled* in the 41 ms between frames and this predicate fired a modeset into the middle of
it — the deferral causing the hitch it exists to prevent.
[Decision 76](#76-cadence-authority-follows-predictability-not-foreground) closes it by making a
client's stated cadence a contributor to the same fold, so the predicate is unchanged and what it
reads is now complete. A client that states nothing falls back to inference from observed commits,
which is the right place for a heuristic: being wrong costs one budget's worth of hitch, bounded by
the tight budget below, rather than another output's deadlines.

The budget is tight — tens of milliseconds, not an animation length. Experience.md's *latency, not
judder* is a statement about how the system **degrades**, not a licence to add delay to a deliberate
act, and the user who changed a display setting is watching. A tight budget also bounds the
mechanism's own failure: a wrong prediction costs the budget and nothing else.

Not every caller may defer. Resume and migration cannot wait for anything, and hotplug should not,
since the EDID probe alone can exceed any budget worth setting.

*(Revised 2026-08-17, when `Seam` was written.)* **The completion carries the achieved
configuration rather than being bare.** `Reconfigured` was written above as `Signal<>` — a fact, on
the reading that the frame loop re-reads whatever it needs afterwards. That does not survive contact
with the one thing it must report. `[vmin, vmax]` is derived from the mode and the panel's reported
range and **cannot be asked for**: the servo learns its own authority only after a mode is set, which
this decision's own text already says. A bare completion would therefore need a query path beside it,
for a value the kernel does not answer cleanly and that this decision refuses to read hardware state
back for. Carrying `const OutputConfiguration&` answers it and answers a second question for free —
a request the hardware could not honour reports itself by the achieved configuration differing from
the wanted one, so there is no failure signal to add and no ambiguity about which of two outstanding
requests completed, because the generation is echoed in the same record.

**Rejected: a bare completion plus a separate failure signal.** The shape that keeps `Signal<>` and
answers the refusal case. It adds a fourth signal, splits *what happened* across two of them, and
still leaves the variable-refresh range with nowhere to arrive.

**What is promised.** An output mid-reconfiguration holds its last frame — decision 41's admitted
multi-frame stall, extended to one more caller with the same guarantee that the last frame stays on
glass. On a driver that serializes device-wide, unrelated outputs hold with it.

**Rejected: the frame thread performing the transition itself.** *(The presumptive answer recorded in
Open.md on 2026-08-16, superseded 2026-08-17.)* It was reached correctly from everything then on
record — two channels stay two, the transition happens on the thread owning the presenter, ordering
is defined by construction — and it did not survive reading the kernel. Its unexamined premise was
that `DRM_MODE_ATOMIC_NONBLOCK` meant what its name says. Everything else in that argument survives;
only *performs* became *initiates*. Kept because the entry recorded blocking cost as the thing that
would overturn it, and blocking cost is not in fact what did: the duration was never the problem, the
synchronous validation phase was.

**Rejected: mode setting on a dispatch-side interface.** The straightforward placement, given that
every caller is dispatch-side or composition-root. It makes `TargetsInvalidated` cross the
publication boundary, and a signal that crosses is not a signal.

**Rejected: stop-the-world — the root quiesces the frame thread, sets the mode, resumes it.** It
matches migration and resume exactly and costs nothing there, because both hold a static picture
anyway. It is rejected on the fourth caller: stalling every output for a user changing one display's
resolution is the routine case paying the emergency case's price. Note that a device-wide driver
still produces this outcome — the difference is that gyro no longer *imposes* it where the hardware
does not.

**Rejected: a control request the loop drains per iteration.** Keeps the other outputs running and is
a third crossing of a boundary [decision 45](#45-protocol-dispatch-is-a-thread-not-a-task) is built
on there being exactly two of.

**Rejected: adapting the policy per driver.** i915 decouples modesets from flips and would let a
live mode change proceed without holding unrelated outputs. Taking that means carrying a quirk model
that is not one axis — the two vendors are expensive in opposite places, i915 forcing a full modeset
on every VRR toggle where amdgpu does not, amdgpu trapping on `ALLOW_MODESET` where i915 does not —
covering only the drivers that were read, on behaviour that is an acknowledged `TODO` upstream. What
it buys is a latency, in a case where the user is already waiting on something they asked for. The
single path is *not* levelling down: gyro declines to block and holds only what the hardware makes
it hold, so a decoupling driver is simply faster at the same code.

**Rejected: a wall-clock deferral timeout.** The shape reached before `Wake` was consulted. It
spends its whole budget for nothing in precisely the case that most needs the deferral — continuous
motion, where quiet never arrives — and decision 69's third case already names that case distinctly.
A timeout would have made the answer depend on a number; the fold makes it depend on a fact.

**A second consumer for a signal that is not yet chosen.** The fold covers gyro-authored motion. A
client committing video frames may not contribute to it, so the predicate could read `Settled`
between two video frames and reconfigure into the hitch it exists to avoid. That wants the same
signal [Open.md](Open.md) is already trying to identify for
[decision 66](#66-arrival-control-is-an-input-to-admission-control) — *what says a client controls
the refresh rate* — which now has two decisions resting on it rather than one.

Read against `linux-next` at `4477a78374a5`. Every kernel claim above is cited to file and line in
[KernelWishlist.md](KernelWishlist.md), which also records what gyro would want instead and what
would let this decision's workarounds be deleted.

---

## Effects

Recorded 2026-08-15, alongside the revision of decisions 29 and 30; decisions 62 and 63 on
2026-08-16. These are what make `C` a number gyro controls rather than one it discovers.

### 33. Effects are named materials, not parameterized filter calls

A surface declares a material from a closed vocabulary — `Material::Glass`, `Material::Sidebar`,
`Material::Hud` — and gyro decides what that means this frame. *(Those three names were a sketch. The
set is `Glass` and `Smoke`, per [decision 103](#103-a-dressing-is-named-by-what-it-does-to-light-the-material-set-is-glass-and-smoke),
which retires `Sidebar` as a role name and renames `Hud` for the same reason — 2026-08-22.)* Shell code cannot name a blur radius,
a pass count, or a chain resolution, in exactly the way
[decision 13](#13-a-closed-motion-vocabulary-with-runtime-configuration-exposing-only-that-vocabulary)
forbids it naming spring parameters.

This is what makes quality a compositor decision at all. If the call site says *Gaussian blur,
radius 24, five passes*, then degrading means silently overriding what a caller explicitly asked
for, and every effect site becomes an independent look-and-feel decision that drifts out of cohesion
the same way independently tuned springs do.

It is also the only arrangement the architecture can support, for a reason that has nothing to do
with cohesion: **only the compositor has the backdrop.** A client cannot blur what is behind its own
window, because that content belongs to processes it cannot see. macOS reaches the same conclusion
from the same constraint — `NSVisualEffectMaterial` names a material, and the render server inside
`WindowServer` renders it, which is why vibrancy picks up another application's window behind a
sidebar.

**Rejected: parameterized effect calls.** Ergonomically obvious and immediately expressive. It
forecloses
[decision 34](#34-effect-quality-is-a-tier-gyro-chooses-and-the-floor-tier-is-the-recovery-path)
entirely, and it is a rewrite of every effect call site once the first machine needs to spend less.

**Cost accepted:** the vocabulary has to be designed before it is needed, and a material that does
not exist cannot be asked for. Same trade as the motion catalog, made deliberately for the same
reason.

**Each material is also classified pointwise or gathering.** *(Added 2026-08-16.)* One extra
property on every entry in the vocabulary, and
[decision 63](#63-effects-declare-their-kind-and-their-damage-the-verifier-keeps-them-honest) spends
it three times — fusibility, damage expansion, and cacheability. It is a second reason the
vocabulary wants designing as a set rather than accumulating: whether the pointwise half stays small
decides how many pipeline variants
[decision 62](#62-effect-composition-is-an-optimization-and-the-unfused-path-is-the-reference) has
to precompile.

### 34. Effect quality is a tier gyro chooses, and the floor tier is the recovery path

A material renders at a quality tier. The ladder, for blur:

1. **Internal resolution of the pass chain.** The cheapest lever and very nearly invisible — the
   result is about to be blurred anyway.
2. **Pass count.**
   *(Built 2026-08-22, and the arithmetic sharpened both rungs. The first is exactly invisible only
   in the sense that the sigma is preserved; the second preserves the sigma and changes the kernel's
   shape, because three boxes make a near-Gaussian and two make a triangle — which is why this order
   is right and not merely conventional. And the ladder bottoms out below a chain divisor of eight,
   where the extract and the final composite dominate what a material costs and the blur itself is a
   fraction of one pass over the panel. Decision 117.)*
3. **The material is not rendered** — an opaque or simply tinted fill.

Radius is not on the ladder. It is a design property of the material; varying it changes what the
system looks like rather than what it costs to look that way. Neither is precision: dropping bit
depth in the pass chain does not make a look cheaper, it makes it wrong, and banding does not read
as blur. Internal resolution is admissible for exactly the reason those two are not — a result about
to be blurred tolerates it, and nothing about the design changes.

**The first rung is only invisible in linear light.** *(Added 2026-08-16.)* Changing chain internal
resolution is a downscale, and a downscale performed in an encoded space darkens its result — so
under sRGB-space compositing the rung described above as very nearly invisible would shift the
picture's brightness at every tier step, making the cheapest lever the most conspicuous one. That is
one of three places where work already recorded here silently depended on an answer this document
did not have until [decision 47](#47-compositing-happens-in-linear-light-at-wide-primaries); the
other two are decision 13's cross-fades and decision 18's matched geometry.

**A startup capability probe picks the tier; measurement corrects it.** The probe runs the real pass
chain at two or three sizes once at startup, rather than consulting a vendor and model table that
would be wrong within a year. The tier it picks is then stable, and stability is the point: a
quality level that drifts with load reads as cheap even when no frame is missed, and cohesion is the
first priority in [Animation.md](Animation.md#priorities). Apple avoids this problem by knowing
every GPU it will ever run on and tiering statically per device class; lacking that table, a probe
is the nearest available equivalent. Measured cost then exists to catch what a probe cannot predict
— thermal throttling, a client saturating the GPU, an unusually expensive scene.

**Tier changes are sticky and asymmetric**: down quickly, up slowly, never during an animation.
Settling time is analytic ([decision 11](#11-springs-are-closed-form-not-numerically-integrated)),
so *when is it safe to change* has an exact answer rather than a heuristic one. A tier recomputed
per frame makes effects flicker at the margin, which is a worse artefact than the dropped frame it
avoids.

**That rule governs this ladder and not decision 35's floor tier, and the difference is a gap rather
than an exemption.** *(Added 2026-08-22.)* The record-time check picks the floor composite per frame,
and at rung 3 that is a backdrop snapping flat and back inside one period — the artefact the
paragraph above rejects, arriving through another door, and correlated with animation because that is
when the check fires. Whether the answer is that the floor composite is defined to be visually
continuous rather than absent, or that flooring inherits the stickiness above, is [open](Open.md).

*(The artefact is now reachable rather than predicted —
[decision 117](#117-a-gather-reads-the-target-it-is-drawing-into-the-numbers-live-in-seam-and-the-tier-rides-the-request)
makes `RenderMode::Floor` mean *this material draws its tint alone*, which is the first behaviour
either renderer attaches to that enumerator. Two frames either side of a floored one are a blurred
backdrop, a flat tint, and a blurred backdrop again. Nothing has been drawn on a panel yet to watch it
happen on, which is why this stays open rather than being settled by the change that produced it —
2026-08-22.)*

**The cut point is computed at commit time, not per frame.** Effects are recorded in declared
priority order against a running cost sum; where that sum crosses the allocation is where optional
work stops. Because commits are declarative, this is known before the frame is recorded, and the
frame loop executes a decided plan like everything else.

**Cost is measured, not modelled.** Analytic GPU cost models are unreliable in a way that is easy to
underestimate: the same pass at the same size varies with bandwidth contention, cache behaviour, and
clock state, and clock state varies with thermal and power conditions. A table computed at boot is
wrong by lunchtime. But we render these passes every frame and can timestamp them, so cost is keyed
on `(material, region area bucket, pass count)` and is simply what it cost last time. Thermal
throttling then self-corrects: the numbers rise, the allocation does not, the tier steps down.

**The floor tier is a first-class path, exercised from the beginning.** A supported render mode used
by tests and headless golden images, not a fallback that only runs in an emergency — a recovery path
that has never run is broken when it is needed. Its cost `C_min` is a design target rather than a
residue, because [decision 35](#35-a-miss-costs-one-frame-bounded-by-the-floor-composite) makes it
the bound on the shock the system can absorb.

It also has a permanent occupant.
[Decision 40](#40-software-rendering-is-a-device-not-a-backend-and-it-is-the-floor-tier) puts
software rendering here rather than giving it a policy of its own, which means the floor tier is not
only exercised — it is what a machine with a broken GPU driver actually runs on.

**Rejected: dynamic adaptation as the primary mechanism**, with no static tier underneath it. It
optimizes the wrong thing — frame timing improves while visual consistency degrades, and on a system
whose stated first priority is that everything moves as one system, a look that varies with load is
the more expensive failure.

**Fusion is a second axis and must not touch this one.** *(Added 2026-08-16.)*
[Decision 62](#62-effect-composition-is-an-optimization-and-the-unfused-path-is-the-reference)
introduces a performance axis that is invisible by construction, and the rule that keeps the two
apart — cost samples from a frame drawn on a non-final variant are not admitted to the table above —
is stated there.

### 62. Effect composition is an optimization, and the unfused path is the reference

A surface carries several effects at once: a material, a dim, a tint, the alpha of a fade. Executed
naively each is a pass over a render target, so bandwidth is linear in the count on exactly the
machines [decision 29](#29-outputs-are-periodic-real-time-tasks-the-test-allocates-effect-budget)
names. **Effects therefore compile to fragment contributions that gyro fuses into one pipeline where
it can and executes as separate passes where it cannot — and the fused form is never required for
correctness.** Every effect exists as a standalone pass. Fusion is selected when a variant is
available and skipped when it is not.

**Gathers segment the chain, which is what makes the variant space enumerable.** A pointwise effect
— opacity, tint, saturation, [decision 58](#58-idle-is-a-ladder-gyro-executes-and-does-not-choose)'s
dim — is a function of one sample and composes with its neighbours as function composition. A
gathering effect — a blur chain, refraction — reads a neighbourhood and cannot consume a value that
has not been written, so it forces materialisation regardless of what precedes it. The fusible unit
is therefore a *maximal run of pointwise effects between two gathers*, not an arbitrary subset of
the vocabulary, and the variants worth precompiling number the contiguous runs over a small closed
pointwise set rather than 2ⁿ over everything.
[Decision 33](#33-effects-are-named-materials-not-parameterized-filter-calls)'s closed vocabulary
pays here for a second reason, having been taken for cohesion.

**No frame blocks on compilation.** Pipeline creation is milliseconds of CPU, and a cold-cache stall
inside a transition is a missed frame — a defect under
[decision 35](#35-a-miss-costs-one-frame-bounded-by-the-floor-composite), not a hiccup. The lattice
is built at startup and persisted; a variant that is nevertheless absent is drawn unfused that frame
and compiled off the frame path.
[Decision 9](#9-volk-and-runtime-shader-compilation-in-vma-and-a-test-framework-out)'s runtime
compilation, taken for hot-reloading during development, is what this runs on. The worker is an
ordinary thread —
[decision 61](#61-the-frame-thread-is-sched_fifo-the-earliest-deadline-first-schedule-is-gyros-not-the-kernels)
makes the *frame* thread `SCHED_FIFO` and says nothing about the rest — so what it can cause is core
contention rather than preemption, bounded the way
[decision 40](#40-software-rendering-is-a-device-not-a-backend-and-it-is-the-floor-tier) bounds
lavapipe's: by reserving cores, not by capping time.

**The unfused path is the oracle.** Two implementations of one rendering, one of them readable and
always available, is a test: draw both, assert they agree. That makes a generated shader nobody
wrote falsifiable rather than merely inspectable, and it is decision 34's floor-tier argument again
— the simple path is a reference exercised constantly, not an emergency mode that has never run.

**Agreement is a precision question, and it is the hard part.** A fused chain keeps intermediates in
registers at full precision; separate passes round-trip through a render target and round at every
boundary. [Experience.md](Experience.md#the-picture-is-correct) requires that the image not change
when the machine changes how it draws it, and that promise now covers the fusion decision itself.
[Decision 47](#47-compositing-happens-in-linear-light-at-wide-primaries) fixes the space the
arithmetic happens in; what remains is the intermediate's storage format, which has to be chosen so
the two paths agree *below the perceptual threshold* rather than merely closely.

**A variant is bound when an animation begins and held for its duration.** A compile completing
mid-transition must not swap the path under a moving picture: whatever difference remains between
the two then arrives at an arbitrary moment, attributable to nothing. Same rule and the same reason
as decision 34's tier stickiness.

**Fusion never influences tier.** They are orthogonal — fusion is invisible and buys performance,
tier is visible and is chosen once and held. The failure to foreclose is a cold cache after every
boot and every upgrade producing expensive unfused frames, decision 34's measured cost reading them
as pressure, and the tier stepping down: the machine would look worse for the first minutes of a
session and better afterwards, which is precisely the breathing quality decision 34 exists to
prevent. Concretely, **cost samples taken from a frame drawn on a non-final variant are not admitted
to decision 34's cost table.**

**Rejected: mandatory fusion**, with the whole lattice precompiled and no separate-pass path. It is
where this decision started. It makes the vocabulary's size a hard constraint rather than a design
choice, has no answer for a missing variant except to stall, and forecloses caching a gather's
result across frames — a static blurred backdrop behind a still window should cost nothing, and
fused into its consumer it recomputes every frame.

**Rejected: no fusion, always separate passes.** Simple and uniform, and it spends the bandwidth
this decision exists to save on integrated graphics driving a large external display, which is the
exact population decision 29 protects.

**Rejected: appending a gather to a fused run.** The tempting shape is a fused prefix with the
remaining effect on the tail. It is sound only when that tail is genuinely last in declared order
and pointwise: a neighbourhood read cannot consume registers, and an effect belonging in the
*middle* of a run requires the run to split. So the general mechanism is contiguous-run fusion, and
every effect keeps its standalone pass for the splits.

**Cost accepted:** two paths for every combination, and a shader in a capture that nobody wrote. The
first is answered by the oracle test, the second by being able to emit any variant's source on
demand.

**Prior art, and where it stops.** Core Image concatenates kernels into as few passes as the
hardware needs, and Compiz composed plugin-supplied fragment snippets into one program in 2007. The
idea is old, and it was not carried into any current Wayland compositor, which is the more
interesting half. What neither had to answer is the paragraph on precision above: fusion is their
only path, so no second picture has to match.

### 63. Effects declare their kind and their damage; the verifier keeps them honest

**Every effect declares whether it is pointwise or gathering and, when gathering, the bound by which
its output exceeds its input's extent.** One property does three jobs — it decides fusibility under
[decision 62](#62-effect-composition-is-an-optimization-and-the-unfused-path-is-the-reference), it
expands damage, and it decides whether a result can be cached across frames. Three uses of one
declaration is the usual sign it is the right thing to have asked for.

**It is declared per material and tier, never per site.** Decision 33 puts radius in the material
and decision 34 puts internal resolution and pass count on a ladder gyro walks, so the extent by
which a blur exceeds its input is a function of `(material, tier)` and of nothing at the call site.
That is a small table computed when the tier is chosen, rather than an expansion recomputed per
frame at the sites least able to afford one.

**Damage propagates backwards, and the propagation has to terminate.** A backdrop material samples
what is behind it, so a change *behind* `Material::Glass` dirties the glass, and whatever samples
the glass is dirtied in turn.
[Decision 60](#60-group-opacity-requires-flattening-per-node-alpha-is-not-a-group-fade) already
fixes the rule that makes this acyclic: a backdrop is the composite as of the group's base, so a
material samples strictly below itself in tree order and can never sample its own output.
Propagation depth is then bounded by nesting depth. Without that rule, a blinking cursor behind one
pane of glass eventually dirties the screen — decision 58's ladder and
[Experience.md](Experience.md#doing-nothing-costs-nothing)'s local-cost promise both lost to a cycle
nobody drew deliberately.

**Over-declaration is the failure that does not report itself.** Under-declaring leaves residue — a
fringe or a trail at the edge of something that moved — which is visible, attributable, and gets
fixed. Over-declaring costs frames on an idle machine and looks like nothing at all. A contract with
one silent direction wants a verifier rather than review, which is
[decision 36](#36-frame-path-discipline-is-enforced-mechanically-not-by-review)'s discipline pointed
at a second thing.

**The verifier renders full-output and diffs.** A debug mode composites the whole output, compares
against the incremental result, and asserts in both directions: nothing changed outside the declared
region, and the declared region is not grossly larger than what changed. It runs headless over the
material vocabulary and belongs beside decision 58's idle assertion, which is the same class of test
— a claim about frames that were *not* drawn, which nothing else can catch.

**Expansion enters accumulation, not the frame.** Architecture's rule that damage accumulates per
output since its last successful present is what makes a skipped frame recoverable, so an effect's
expansion is accumulated there. A frame dropped under decision 35 then cannot lose the expanded
region and leave residue on the next one — which is the bug this would otherwise introduce into the
one path that must not have it.

**Region arithmetic gets a complexity cap.** Damage is CPU work on the frame path, and past some
rectangle count computing what to redraw costs more than redrawing. The region is bounded to a fixed
count and collapses to its enclosing bound beyond it.

**Rejected: deriving expansion from the effect rather than declaring it.** gyro writes every effect,
so support could in principle be read out of the shader. It would be correct and it would not be
*checkable*: a derivation and the thing it describes are one artefact, so nothing disagrees when
both are wrong. The declaration exists in order to be contradicted by the verifier.

**Rejected: full-output damage whenever any effect is on screen.** Trivially correct, and it makes a
blinking cursor behind a translucent panel cost a full composite — the exact case decision 58 and
*doing nothing costs nothing* are written against.

**Cost accepted:** a table that must be kept honest as the vocabulary grows, defended mechanically
rather than by review.

### 103. A dressing is named by what it does to light; the material set is `Glass` and `Smoke`

*(Decided 2026-08-22, settling [Open.md](Open.md)'s *the material vocabulary* and *which materials
are pointwise*. Designed beside [decision 104](#104-an-elevation-is-a-height-under-one-light-and-the-shadow-is-analytic)
because that file says the two are twins, and §4 of this pair is what came of taking it literally.)*

**Both dressing vocabularies are named by what the thing does to light — never by the role it is
usually put to, and never by degree.** The rule is one sentence and it decides most of the contents.

*Role names* — `Sidebar`, `Titlebar`, `Menu` — are
[decision 51](#51-the-shell-is-a-per-session-client-gyro-owns-mechanism)'s fisheye-dock failure
arriving one field over from where
[decision 95](#95-the-scene-vocabulary-is-four-kinds-a-material-is-a-field-not-a-kind) already
refused it. That entry rejects *a node kind that knows what a window is*, because the moment
the scene knows what a window is, the arrangements that are not windows stop being expressible. A
material called `Sidebar` knows what a sidebar is, and a shell that dresses a workspace switcher with
it is not wrong so much as unsayable-about.

*Degree names* — `Thin`, `Regular`, `Thick` — are
[decision 33](#33-effects-are-named-materials-not-parameterized-filter-calls)'s rejected
parameterized call with the number spelled in English. `UIBlurEffect.Style` is what that becomes: five
thicknesses across three appearances, which is a slider with a vocabulary bolted on, and every site
picks its own point on it. Decision 33 forbids a radius at the call site; a vocabulary ordered by
radius hands it back.

#### The set

```
None
Glass    a blurred, tinted backdrop
Smoke    dark, heavier, and legible over anything
```

**`Glass` sits over content the user arranged, so it can be thin.** Shell panels, the dock,
popovers, notifications, the launcher, the overview's wallpaper. It is the workhorse and it is what
almost everything on a desktop wants.

**`Smoke` sits over content gyro did not choose**, and that is the whole of the difference. A volume
overlay appears over a film, a white page, or a photograph, so **its opacity is set from a worst-case
contrast floor rather than from taste** — an arithmetic difference rather than a stylistic one. It is
not a heavier `Glass`; it has a different obligation, and a shell cannot substitute one for the other
and get a legible result.

**Two materials that differ by what they must survive is what stops a third and a fourth arriving.**
A proposal for a new material has to name a backdrop the existing two fail against. *The designer
wanted it lighter* is not one, and under a set ordered by weight it would have been.

**Retires `Material::Sidebar`**, which this entry, [Architecture.md](Architecture.md#materials-not-filter-calls)
and Open.md all named. It has no optics `Glass` does not: on macOS `.sidebar` differs from
`.hudWindow` by being *within-window*, and gyro has no within-window blur.

#### The cap is five, and it is tighter than the motion vocabulary's seven

Two reasons at once, and the first inverts the usual intuition. **A blur difference is visible in a
still screenshot; a spring difference needs motion to see.** The incohesion
[decision 13](#13-a-closed-motion-vocabulary-with-runtime-configuration-exposing-only-that-vocabulary)
describes — (0.42, 0.83) beside (0.45, 0.80), both defensible, the system subtly wrong forever after
— is therefore *cheaper* to perceive here than on the axis it was written for. A dock and a top bar
whose blurs differ by ten percent is a screen anyone can see is not one machine.

Second, growth is not free the way a transition's is. Each gathering material is a pass boundary
[decision 62](#62-effect-composition-is-an-optimization-and-the-unfused-path-is-the-reference) can
never fuse away, plus a row in decision 34's cost table and a row in
[decision 63](#63-effects-declare-their-kind-and-their-damage-the-verifier-keeps-them-honest)'s
expansion table.

#### The pointwise column ships empty, and that is the answer rather than an omission

**Every material in the set is gathering.** What Open.md wanted from this question was a size for
decision 62's variant lattice, and an empty pointwise column gives it a much better one than a
populated column would have. What is worth keeping is the rule for the first entry that arrives:

> A pointwise material must justify why it is not a `Solid` node or a field on the node, because
> those already do every pointwise thing the scene can express.

Exactly one justification survives it, and it is decision 95's own: the treatment must track the
node's extent and radius *exactly*, so a second node carrying it is a second transform that agrees
only while nothing moves. The two candidates that clear that bar are a backdrop **desaturation** and
a **multiplicative tint** — neither expressible as a `Solid` under `over`, which decision 95 fixed as
the only blend mode. Neither is needed by anything on a screen today.

**The classification is fixed per material and does not vary with tier.** The cute answer is
otherwise available — gather at high tiers, go pointwise at low ones — and it is foreclosed because
fusibility depends on the classification, so a tier-dependent classification makes the *variant set*
tier-dependent and couples the two axes decision 62 spends a paragraph holding apart.

#### What the lattice actually costs, counted

Per item, in order:

- **Reads no input at all:** decision 104's analytic shadow. Cheaper than pointwise, and it segments
  nothing.
- **Pointwise, in one fixed order:** the colour-state conversion, the corner-radius mask, per-node
  opacity, and decision 62's dim where an output has no backlight to dim. Four.
- **Gathering:** the material, and there is at most one per item, because `Dress` is one field.

Contiguous runs over four ordered elements is ten, and a gather splits a chain into at most two of
them. **The lattice is tens of variants precompiled at startup rather than 2ⁿ over the vocabulary**,
which is what decision 62 needed the vocabulary to be designed as a set in order to know.

*(This count is wrong and [decision 116](#116-the-pointwise-lattice-is-two-run-bits-and-a-conversion-selector-and-the-outputs-colour-state-moves-to-the-binding)
is the correction, made on building it. Three of the four elements are not axes: opacity and the dim
are one multiply of data, and the conversion is a selector that is its own mask. And the
contiguous-runs formula does not apply — a gather can land at only one position in an item's chain,
because `Dress` is one field and decision 99 puts the dressing over the node's own content, so the
chain admits two runs rather than ten. The conclusion this paragraph exists to support survives
whole: the set is enumerable before gyro boots.)*

#### Rejected: within-window blur

macOS's `.withinWindow`, where a sidebar blurs its own window's scrolled content rather than the
desktop. It requires the parent's subtree to be materialised before the material samples it — a
render target, which is [decision 60](#60-group-opacity-requires-flattening-per-node-alpha-is-not-a-group-fade)'s
group, arriving silently from a field rather than from a declaration. And the case it buys is
unreachable anyway: an application's sidebar is inside one client's one buffer, which gyro cannot see
into.

#### Rejected: a tint that adapts to backdrop luminance

It is the thing that makes macOS's translucency work rather than look like a gimmick, and per frame
it breathes — a panel over a playing film changing tone at every cut, which is
[Experience.md](Experience.md#how-it-degrades)'s *quality does not visibly fluctuate* failing in the
one place a user is looking. A hysteretic version is a time constant nobody has measured, so it is an
Open.md number rather than a first-cut entry. `Smoke` exists partly because the case adaptivity was
for is answered by committing instead.

#### Rejected: keeping `Hud`

The incumbent name in this entry and in Architecture.md, and it is a role name by the rule at the top
of this one — an exception on the rule's first application, which is where a rule is either load
bearing or decorative. `Smoke` says the optics, and smoked glass is literally *dark, blurred, and
legible over whatever is behind it*.

**Left open.** The tint values, the radii, and `Smoke`'s contrast floor are numbers and want a screen.
What is settled here is which materials exist and what each is for.

### 104. An elevation is a height under one light, and the shadow is analytic

*(Decided 2026-08-22, settling [Open.md](Open.md)'s *the elevation set*, including the part of it
that asks what a level does at the floor tier. Revises
[decision 96](#96-the-frame-is-the-compositors-and-the-header-is-the-apps)'s shadow mechanism, below.)*

```
None       in the plane
Resting    lifted off the desktop and left there
Floating   lifted off everything; unattached and transient
```

Named by **what the node is lifted off**, which is neither a role nor a number, per
[decision 103](#103-a-dressing-is-named-by-what-it-does-to-light-the-material-set-is-glass-and-smoke).
`None` is the wallpaper, a tiled or fullscreen window, and decision 96's client-decorated window.
`Resting` is an ordinary floating window. `Floating` is a menu, a tooltip, a notification, an overlay.

#### A level is a height, and there is one light

From a height gyro derives offset, softness, and opacity by **two constants held for the whole
system**. The light is parallel and vertical rather than a point source somewhere on the screen, so
two windows at one level cast identical shadows wherever they sit.

That is `MotionTable` : `Motion` exactly, and the configuration rule arrives structurally rather than
by discipline: configuration retunes the two constants, which moves every level together, and
configuration cannot reach a level because a level has nowhere to put a radius. Same shape as
[Motion.h](../Source/Animation/Author/Motion.h)'s argument that a bundle holds a `Motion` and a
`Motion` has nowhere to put a damping ratio.

**What a person sees.** A Linux desktop today is every toolkit's client-drawn shadow being its own
light: a GTK menu's shadow falls one way over a Qt window's falling another, at a different softness
and a different weight, and the screen reads as a collage of applications rather than as one surface
with things on it. One light is the whole of the difference, and unlike a motion it is visible in a
screenshot before anything moves.

#### Three levels, and the cap is perceptual rather than economic

Shadow ranks depth badly. Past about three lifted levels a person reads *floating* and stops
counting, which is why Material Design's twenty-four dp steps resolve to roughly four distinguishable
looks in practice. So the cap is not what a level costs — decision 104's shadow costs almost nothing —
it is that a level nobody can see leaves a shell author choosing between two identical looks, and
therefore choosing inconsistently.

**Rejected: a middle level for a dialog on its window.** macOS distinguishes a sheet from a menu
clearly and it is the obvious fourth. Two lifted levels is the safer read against the cap above, and
the middle one is recoverable as a single enumerator that reaches no call site not already switching
on the enum — which is the shape decision 95 fixed for exactly this.

#### Focus is not a level, and neither is a drag

macOS gives the focused window a much larger shadow and it is a good depth cue. It cannot be a level
here: focus is state gyro owns (decision 96), so a focus level would have gyro tell window management
the focus so that window management could tell gyro the elevation. It is
[Catalog.h](../Source/Animation/Author/Catalog.h)'s `FocusChange` on opacity, which exists and already
does the job. A drag lift is the same — decision 51 keeps drag inside gyro — so it is gyro's own
modulation of a declared level rather than a level a shell declares.

Both exclusions shrink the set, which is why they are here rather than in a footnote: the four-level
sketch this started from spent two of its levels on states the shell was never going to be the one to
know.

#### The shadow is analytic, and that is what makes the floor tier's promise true

*(This revises decision 96's stated mechanism. That entry answers "what gyro draws once a client
stops" with `WindowServer`'s: derive the shadow from the window's own alpha silhouette, cached in
decision 46's atlas and invalidated on shape change.)*

**An alpha silhouette needs a blur chain per distinct silhouette, which puts every shadow on screen
onto [decision 34](#34-effect-quality-is-a-tier-gyro-chooses-and-the-floor-tier-is-the-recovery-path)'s
ladder — whose third rung is *the material is not rendered*.** So the first thing a machine under
pressure would spend is the depth of the entire picture, all at once, and
[Experience.md](Experience.md#how-it-degrades) promises the opposite: what is spent first is "a small
amount of quality in something that was about to be blurred anyway."

**A rounded-rect shadow has a closed form, and gyro already knows the rect.** Decision 96 rounds the
window geometry rect to gyro's own floor radius, so the silhouette gyro *draws* is one gyro authored.
[Decision 106](#106-an-x11-client-has-no-window-geometry-gyros-window-manager-computes-the-frame-rect)
widens that to the population it would have been weakest on: an X11 client sends no geometry at all,
so gyro's window manager computes the rect rather than receiving one, and there is no Xwayland
surface whose true silhouette gyro would be approximating.
Analytic, that shadow costs arithmetic over its own extent and nothing else: no render target, no pass
chain, no cache, no invalidation on content change, and it reads no input, so it composites inside the
item's own pass. **It is therefore not on decision 34's ladder at all** — a floored frame keeps every
shadow and gives up the blur behind a panel, which is the degradation that was promised. That is
Open.md's *what a level does at the floor tier*, and the answer is nothing.

Two further consequences fall out. Decision 63's expansion becomes **a constant per level**, known
when the level is, rather than a `(material, tier)` table computed when the tier is chosen. And the
derivation had to differ by kind regardless: a container and a reference have no alpha, and
[decision 99](#99-a-dressing-draws-over-the-nodes-own-extent-whatever-the-nodes-kind) dresses both, so
an alpha silhouette is undefined for half the node kinds. **The rect path is not an optimisation of
the silhouette path; it is the only path for two of the four.**

**What it gives up, named rather than discovered.** A popover with a tail gets a rectangular shadow,
and the tail then reads as stuck flat against the window while the body floats. Decision 96's
silhouette mechanism stays available for exactly that, as a second path selected per node rather than
as the default for all of them — which is the direction that keeps the common case free and pays only
where the shape is genuinely irregular.

#### A node's material must not sample its own shadow

Both dressings land on one `DrawItem`. Within it the shadow is drawn around the quad while the
material samples what is behind. If the material samples the target *after* the item's own shadow is
composited, **a glass panel darkens itself at its own edges** — a dark halo inside every translucent
panel, worst where the panel is thinnest and the backdrop reads through most.

Decision 60's rule that a material samples the composite as of the group's base does not reach this,
because a node's own shadow is not below it in tree order; it is the same item. So: **within one item,
the material samples the target as of before the item began.** One sentence now, and otherwise a
fringe nobody would attribute to elevation.

#### Blur needs no animated channel, and the material design is what settles it

[Snapshot.h](../Source/Publication/Snapshot.h) reserves the possibility — *blur and corner radius are
absent because the material vocabulary they belong to is open*. Blur turns out not to want one. A
panel appears by fading in at full blur strength, and the overview's wallpaper blurs by a `Glass`
container's opacity going from zero to one over it.

**The artefact, named:** a blurred backdrop at half opacity over the sharp one is a fifty-fifty mix,
which reads as haze rather than as a half-radius blur. It is what macOS does when a sheet appears, it
is fine, and a real radius ramp would be a per-frame chain re-parameterisation for a difference nobody
has asked for.

#### Consequences

`World/Elevation.h` gains `Resting` and `Floating`. `DrawItem` carries the *derived* shadow rather
than the level, because nothing in the renderer switches on a level — which removes `Seam`'s
`World/Elevation.h` edge, the direction decision 82 wants a draw list to move in. The two light
constants join `MotionTable` as a table configuration may overlay and individual levels may not.

**Left open.** The heights and the two constants are numbers and want a screen, as does whether a
rotated node's shadow shears with it or rides its quad — a card flip is the only case, and it is a
transition nobody has written.

### 105. Relief is one scalar: the corner radius and the shadow move together

*(Decided 2026-08-22, answering [Open.md](Open.md)'s *the corner radius and the layout state that
zeroes it have no carrier*. It is here rather than beside decision 96 because it was forced by the
node record while sizing decision 104, and the record is what decides it.)*

**A node carries one animated scalar — its relief — and both the corner radius and the shadow height
are that scalar times a constant gyro owns.** At full relief a window has its floor radius and its
level's shadow; at none it is square and flat. The shell holds the *fact* — this window is framed, or
it is fullscreen — and gyro holds both numbers, which is exactly the split Open.md's own entry said
the fullscreen case pointed at.

#### The record is what decides it, and the arithmetic is worth stating

Decision 95 landed `Node` on 128 bytes — two cache lines exactly, so the walk's indexing is a shift.
The tail is three one-byte enumerations and five spelled reserved bytes. **One more `std::uint32_t`
coefficient slot fits there; two takes the record to 136 and a third line.** Checked rather than
reasoned: 128, 128, 136.

So the radius and the shadow cannot both be independently animated channels without giving up what
decision 95 bought, and something has to arbitrate. They turn out to want the same scalar anyway.

**The shadow has to animate**, and both places a level changes on a node that stays on screen are
watched ones: a **maximize**, where the shadow must be gone by the time the window fills the display
and a cut at the end pops at the edge the eye is already on; and a **pick-up**, which is by definition
the thing being touched.

**The radius has to move at the same moments and no others.** Decision 96 takes the radius to zero
when a window is fullscreen or tiled edge to edge, which is the same maximize. There is no transition
in which one moves and the other holds.

#### The case that would break it, and why it does not

A tiled window that keeps a shadow after its radius is zeroed. It dissolves on decision 96's own
wording: the radius goes to zero only *edge to edge*, and a shadow between two abutting windows is a
dark seam rather than a depth cue. A layout with gaps is not edge to edge, so both survive together.

#### Gyro's own modulations apply after, not to the scalar

Decision 104 keeps the drag lift and the focus cue out of the level, and they stay out of relief for
the same reason. Gyro scales the *derived* height; it does not push relief past one — which would
round a window's corners harder for being picked up, a thing nobody asked for and which the shared
scalar would otherwise deliver.

#### Consequences

`Node` gains `float Relief` and one `ReliefSpring` slot, landing in the reserved tail at 128 bytes
unchanged. `Channel` goes from four to five and `SnapshotRun` gains a run, so every catalog entry's
channel table and [Bundle.h](../Source/Animation/Author/Bundle.h)'s reduction theorem range over it —
which is the reach into `Animation` Open.md predicted when it called the two vocabularies twins,
arriving from the shadow rather than from the blur.

**Rejected: two channels, and pay the third cache line.** Only worth it if the radius must move
independently of the height, and the case above is the one candidate.

**Rejected: leaving the radius uncarried.** It is the smaller change today and it costs the fifth
`Channel` entry twice — once for the shadow now and once for the radius later, each looking like a
small addition on its own, which is how a four-entry enum becomes an eleven-entry one.

**Left open.** Where the floor radius sits is still a number and still wants a screen with GTK, Qt,
and an Xwayland application on it at once. This entry decides the carrier, not the value.

---

## Frame integrity

### 35. A miss costs one frame, bounded by the floor composite

Three promises, in descending order of strength. They must not be collapsed into one, because only
the first is unconditional:

1. **Animation state is exactly correct on every rendered frame.** Unconditional, from closed form.
   A missed frame corrupts nothing; the next frame is evaluated at its own presentation time and is
   exact.
2. **An overrun of up to `P − C_min` costs exactly one frame.** Conditional on the floor tier
   existing and on the record-time check below.
3. **A larger overrun costs `⌈overrun / P⌉` frames** — a GPU reset, a driver stall, a client's 50 ms
   batch — and animation is still exactly correct throughout, so it reads as a brief hold rather
   than a lurch.

One event is outside all three. Device migration
([decision 41](#41-device-migration-is-exercised-on-every-boot)) is an admitted multi-frame stall
during which nothing is rendered at all; what is promised there is that the last frame remains on
glass throughout, which is a different guarantee and is stated separately for that reason.

The third is where closed form pays most and is least obvious. Every system stutters sometimes; the
difference is whether the stutter leaves the animation in the wrong place afterwards. A
`Tick(delta)` system's missed frame becomes a double-delta jump, and the animation is wrong from
then on.

**Recovery is not automatic.** If frame N's work is still executing when frame N+1 must be recorded,
N+1 is late too, and a systematic underestimate cascades indefinitely. Frame N+1 is reachable only
if `t_done + C_min ≤ D`, which is precisely why `C_min` is a design target: it is the size of the
shock the system can absorb.

**One runtime timing decision, taken per output at its record point.** Frame N's completion is
polled non-blockingly through its timeline semaphore — timeline semaphores are already the sync
primitive throughout, so this costs nothing new:

```
	now + C_planned ≤ deadline   →  render the planned tier
	now + C_min     ≤ deadline   →  render the floor tier
	otherwise                    →  skip this output's frame, target the next deadline
```

The third case matters as much as the second. Submitting work that will also be late keeps the GPU
busy and deepens the cascade; skipping is what stops it. This is decision 30's deliberate frame drop
with a precise trigger rather than a policy judgement.

**The three lines are a fixpoint, and applying them once is a deadlock.** *(Added 2026-08-22.)* The
third line's *target the next deadline* is the first line's input: the check runs again against the
deadline it just named, and it keeps dropping frames until it names one the work fits before. That
is not an embellishment of the rule — it is the third promise above, since `⌈overrun / P⌉` frames
dropped and the next one rendered is what the fixpoint computes and a single pass is not.

Applying the three lines once was the form first built, and it starves an output permanently. The
deadline in each line is the frame the output *owes*, that frame comes from the frame clock's anchor,
and only a flip advances the anchor. So an output whose work no longer fits before the frame it owes
renders nothing, presents nothing, produces no flip, and is judged against the same stale anchor a
period later with `now` further past it. The equality can never hold again. Two things reach that
state and neither is exotic: an output blocked for longer than its own period by another output's
composite, and **any** output coming out of idle — which is every output, before the first thing that
ever wants a frame arrives.

The cascade argument is untouched by the correction, because what the third line refuses is
submitting for a frame *already missed*. Work aimed at the frame it can still reach is by
construction not late, and the release constraint is unchanged in the process: the frame reached is
the earliest whose deadline is at or after the predicted finish, so work never starts more than one
period before the deadline it is aimed at — which is exactly the window the first line already
admitted for the frame owed. Aiming *further* out than that would be a lead, and decision 30 grants
a lead from a plan rather than from the record-time check falling into one.

**Transient and systematic overruns need different responses.** A one-off stall is answered by the
floor tier for one frame and nothing else changes. A wrong cost estimate is not: the floor tier
recovers this frame, the next frame repeats the mistake, and the result is a sawtooth. So an overrun
also moves the measured high-water mark, which steps the quality tier down (decision 34). Two
mechanisms, two timescales, and omitting the second makes the first oscillate.

**Consequence: damage accumulates per output across skipped frames.** Damage is *since the last
successful present on this output*, never *this frame's damage*. Modelling it the other way turns a
skipped frame into a correctness bug rather than a timing one. The frame loop must correspondingly
be written to permit an output not rendering in a given iteration.

**Rejected: "hits every frame" as the stated goal.** It is not achievable against a degenerate
client workload — high GPU priority is bounded by the hardware's preemption granularity, per
[decision 22](#22-gyro-runs-as-a-dedicated-unprivileged-uid-with-cap_sys_nice-and-nothing-else) —
and stating an unachievable goal makes the real target fuzzy. The enforceable version is the three
promises above plus decision 36.

### 36. Frame-path discipline is enforced mechanically, not by review

The dominant causes of stutter in existing compositors are not scheduling failures. They are
unbounded operations on the frame path: garbage collection pauses, per-message allocation, malloc
arena contention, synchronous IPC and file I/O on the main loop, and normal-priority scheduling.
gyro's answers are already recorded — decisions 3, 11, 22, 28, 45 — but as invariants scattered
across three documents rather than as the thing being promised.

**Note which decision is absent from that list.** *(Revised 2026-08-16.)* Half those causes are
client-facing, and the answer to every one of them is
[decision 45](#45-protocol-dispatch-is-a-thread-not-a-task) — a thread boundary — rather than
[decision 2](#2-gyro-owns-the-protocol-seam-libwayland-implements-the-server-codec). Owning the wire
protocol makes message handling cheap and bounded, which is worth having and is not what protects
the frame. The distinction is worth stating because the two were conflated for this design's entire
first pass: the frame is protected by what is *not on the thread*, not by what is fast.

*(Confirmed 2026-08-16.)* This paragraph was written before decision 2 reversed and is the reason
the reversal cost nothing here. Nothing in this decision's list moves when the server codec becomes
somebody else's, which is the test a claim of this kind should pass.

Stated as the target: **no operation on the frame path may take unbounded time.** Every missed frame
should be traceable to something measured and mispredicted, never to something nobody knew was
there.

Enforced by a debug allocator that aborts. Global `operator new` is overridden in debug builds
against a thread-local flag set around the frame section, so an allocation on the frame path is a
test failure rather than a code review finding. **Thread-local is the operative word**: the dispatch
thread allocates freely and must, so the check is a property of the frame thread's frame section and
never of the process. It is roughly thirty lines written before there is anything to catch, and
archaeology if written afterwards.

The testable form of decision 35, in headless against a fake clock: inject an overrun of
`≤ P − C_min` and assert exactly one frame is missed; inject a larger one and assert the miss count
is `⌈overrun / P⌉` **and** that frame `N + k` is pixel-identical to a run in which no overrun
occurred. That tests the two promises independently, and the second half is what makes promise 1
falsifiable rather than merely asserted.

And the test decision 45 makes expressible, which nothing in the previous design could state:
**flood the socket and assert the frame timing does not move.** A client sending as fast as the
kernel will take it, in headless against a fake clock, must produce a miss count of zero and frames
pixel-identical to a run with no client traffic at all. The old design could only assert that the
dispatch budget was honoured — a claim about a mechanism, where this is a claim about the outcome.

**Rejected: a percentile frame-time target.** A p99.9 admits one frame in a thousand over budget and
says nothing about whether the misses cluster. Isolated misses and consecutive misses are different
products, and the metric that cannot tell them apart is measuring the wrong thing.

---

## Boot and recovery

Detail in [Architecture.md](Architecture.md#boot-and-the-display-lifetime). Decision 24 left gyro
with no session dependencies at all, which makes it a boot service rather than a login one. These
decisions follow that through to its conclusion: if gyro can start before everything else, it
should, and then it owes the machine everything the things it displaced used to provide.

### 37. gyro owns the display from firmware handoff onward; there are no VTs

The firmware's BGRT logo stays on screen until gyro's first modeset, because nothing else ever
touches the framebuffer — no Plymouth, no fbcon, no initramfs output. gyro reads
`/sys/firmware/acpi/bgrt/{image,xoffset,yoffset}`, reproduces the logo in its own mode at the
corresponding position, and animates from it into the greeter. The whole boot is one continuous
image.

Two things make this cheap. `fbcon=off` on the kernel command line is sufficient — Fedora ships
`CONFIG_VT=y`, `CONFIG_VT_CONSOLE=y`, and `CONFIG_FRAMEBUFFER_CONSOLE=y`, so disabling at runtime
needs no custom kernel and leaves the VT machinery underneath as a panic path that simply never
reaches the display. And with nothing bound to the framebuffer, "zero output until gyro" is not
something to arrange; it is what remains once everything else is removed.

The offsets are in the *firmware's* mode, so reproducing the logo is a scale-and-place computation
rather than a memcpy. Getting it wrong produces a visible jump at precisely the moment this decision
exists to make seamless.

**What replaces VTs.** `Ctrl+Alt+F2` no longer exists, so gyro owes the machine a recovery console —
[decision 38](#38-the-pre-vulkan-console-is-a-permanent-subsystem-not-a-bootstrap). Three further
consequences are not gyro's to solve and become deployment requirements instead:

- **Kernel panics become invisible**, and there are two answers rather than one. `CONFIG_DRM_PANIC`
  draws the panic straight onto the DRM framebuffer with no VT and no fbcon involved — its own
  Kconfig help text names this exact arrangement, *useful when using a user-space console instead of
  fbcon* — and `drm.panic_screen=kmsg` makes it the tail of the log rather than an apology. Every
  driver on gyro's path implements the hook, `simpledrm` included, so the boot device is covered as
  well as the final one. `pstore` stays enabled behind it, for what the handler cannot draw, and
  gyro should surface that on the next boot. *(Added 2026-08-16, from the reading behind decision
  49.)*
- **A wedged gyro leaves the keyboard as the only way in**, so `sysrq` must be enabled, and it does
  two jobs here rather than one. The familiar keys end the process; `SysRq-V` — registered by the
  DRM core for every device, help text *force-fb* — forces the in-kernel client to restore, which on
  a machine with no VT is the only key that puts a picture *back* rather than taking one away. What
  it restores depends on an in-kernel client still being registered under `fbcon=off`, which is
  [open](Open.md).
- **gyro failing to start leaves no local way into the machine** — worse than "no UI", because there
  is no console to fall back to. A systemd `OnFailure=` unit runs `gyro --console`, which reaches
  the console path without touching Vulkan.

**Rejected: keeping Plymouth and handing off.** This is the conventional arrangement and it is the
source of the flash everyone has learned to expect. Plymouth exists because at boot there is no
compositor; gyro is a compositor with no session dependencies, so after switch-root that premise is
simply false. Keeping Plymouth would mean owning a handoff, a second theming system, and a second
piece of software that must agree with gyro about the mode.

**Rejected: a custom kernel with `CONFIG_VT=n`.** Removes the VT machinery properly, and costs a
kernel build on every machine that runs gyro. `fbcon=off` achieves the visible result, and retaining
VT support underneath is a benefit rather than a compromise — it is somewhere for a panic to go.

**Rejected: a recovery console as an ordinary client**, which is what
[decision 25](#25-the-lock-screen-is-a-client-the-compositor-owns-lock-state-not-lock-ui) 's logic
would otherwise suggest. A client needs gyro running, a working Vulkan device, and a session to
connect through — precisely the three things that are absent whenever a recovery console is what you
need.

### 38. The pre-Vulkan console is a permanent subsystem, not a bootstrap

DRM dumb buffers, CPU blits, an embedded bitmap font, and a fixed glyph grid. No Vulkan, no GPU
driver, no text shaping. It exists because four separate requirements turn out to be the same code:

1. **BGRT continuation** at boot, per decision 37.
2. **Verbose boot output** — the behaviour `Esc`-during-boot has in Plymouth today.
3. **The recovery console** that replaces VTs.
4. **The failure display** when Vulkan or the GPU will not initialize.

This is not the in-process UI
[decision 25](#25-the-lock-screen-is-a-client-the-compositor-owns-lock-state-not-lock-ui) refuses.
There is no text shaping, no layout engine, and no styling — a fixed grid of pre-rendered glyphs is
what a recovery console *should* be, precisely because it must work when nothing else does. The
distinction is that decision 25 refuses UI that has a design; this has an interface.

It also runs on every boot rather than only in failure, which is the same argument
[decision 34](#34-effect-quality-is-a-tier-gyro-chooses-and-the-floor-tier-is-the-recovery-path)
makes for the floor tier: the path you need when things are broken must not be the path nobody has
executed since it was written.

**Rejected: a separate recovery binary.** More robust against gyro's own code being broken, but it
is a second program that must be kept working, kept in sync with gyro's device handling, and tested.
The failure class actually being defended against is "Vulkan or GPU initialization failed", not "the
executable is corrupt", and one binary with an early path that touches no Vulkan covers it.

**Rejected: deferring it until the DRM backend needs a recovery story.** The initramfs plan
([decision 39](#39-running-from-the-initramfs-is-deferred-and-deliberately-not-foreclosed)) needs
the identical code to prompt for a passphrase, so building it once and early is cheaper than
discovering the fourth caller later.

### 39. Running from the initramfs is deferred, and deliberately not foreclosed

The intended end state is gyro starting inside the initramfs and re-executing from the real root
after switch-root, using the mechanism systemd uses on itself — serialize state, `execve` the binary
from the new root, restore. That is what gives full-disk encryption somewhere to prompt. **Both the
initramfs and FDE are out of scope initially.**

It is far easier for gyro than for systemd, because of one fact: **at switch-root there are no
clients.** systemd has to serialize units, jobs, and fds; gyro carries four things — the DRM fd, the
mode it set, the buffer currently scanning out, and the input fds.

The property that makes it seamless: **DRM master is a property of the open file description, so it
survives `execve` with `FD_CLOEXEC` cleared.** Master is never released, nothing else can claim it,
no mode is reset, and no flicker occurs. GEM handles live on the same `drm_file`, so the image on
screen stays valid across the exec. Vulkan does not survive — but there is no Vulkan in an initramfs
to lose, because Mesa plus LLVM is far too large to ship there, which is why the initramfs gyro is
exactly decision 38's console.

*(Confirmed by reading, 2026-08-16.)* The reading that settled
[decision 49](#49-the-restart-boundary-is-made-cheap-where-it-can-be-and-stated-where-it-cannot)
verifies this from the other side, and strengthens it. The kernel tears a framebuffer off its planes
in `drm_file_free()`, which runs only when the open file description is released — so clearing
`FD_CLOEXEC` does not merely preserve a handle across the exec, it keeps that release path from
running at all. Master, the mode, the GEM handles, and the image on screen are held by one fact
rather than four, and the initramfs re-exec and the crash restart are consequently one mechanism
rather than two similar ones.

Deferred rather than designed out, so what matters now is not foreclosing it. Two affordances, both
close to free and both recorded under
[what to build before it is needed](Architecture.md#what-to-build-before-it-is-needed):

- **The platform seam takes an already-open DRM fd as its primary path**, with "open one yourself"
  as the default provider rather than the only one. gyro already treats "receive an fd from
  elsewhere" as first-class, from decision 24's listener handover.
- **gyro adopts an existing mode rather than unconditionally modesetting.** Read the current CRTC
  state and modeset only on difference. This is needed four separate times: re-exec, the
  `simpledrm` → real-driver migration in
  [decision 41](#41-device-migration-is-exercised-on-every-boot), recovery after a crash under
  [decision 49](#49-the-restart-boundary-is-made-cheap-where-it-can-be-and-stated-where-it-cannot),
  and any Plymouth takeover we are ever forced into. *(Fourth caller added 2026-08-16.)* The
  fd-survival property this decision rests on turns out to be the same property that keeps the last
  frame on glass across a restart, which is a larger payoff than the case it was recorded for.

**Rejected: no initramfs at all.** It is the cleanest version of decision 37 and it makes encrypted
root impossible, since a passphrase prompt has nowhere to live. TPM2-backed unlock covers the happy
path, but a firmware update that changes the PCRs then leaves a machine that boots to a black screen
with no way to enter a recovery key. The initramfs should exist and be silent, producing output only
when it genuinely needs input.

### 49. The restart boundary is made cheap where it can be, and stated where it cannot

[Decision 27](#27-resource-accounting-is-attribution-not-per-user-fairness) settles what a restart
costs. This settles what to do about it — describing a boundary is not choosing one, and three
separate things had been travelling under a single phrase: whether the process dies, whether the
**display** dies with it, and whether the **clients** do. Only the third is forced.

**Most failures must not be process-fatal at all, and that is where the value is.** Decision 27
already answers a misbehaving client with a protocol error that kills the client. Generalised, and
stated as a standard rather than left as a habit: parse failures, resource exhaustion, protocol
violations, and unsupportable client requests are **client-fatal by construction**, never
process-fatal. Everything below exists for the bugs that escape that discipline. It is not a
mechanism to design toward, and a system reaching for it often has already lost.

**The display survives a restart, and this is nearly free.**
[Decision 39](#39-running-from-the-initramfs-is-deferred-and-deliberately-not-foreclosed) rests on
DRM master being a property of the open file description, which is what makes switch-root re-exec
seamless. A crash borrows the same property, and more literally than "the fd is saved and restored"
suggests: `SCM_RIGHTS` passes a reference to the *same* open file description rather than a copy, so
while the service manager holds one the description never reaches a refcount of zero and is
therefore never released. The fd reaches the store via `sd_notify` with `FDSTORE=1` — an `AF_UNIX`
datagram, roughly thirty lines, no libsystemd, and emphatically not D-Bus, so
[decision 7](#7-session-claiming-is-deferred-basu-rejected)'s position is untouched. The coupling is
to the service manager gyro already needs for `LimitRTPRIO=`, `LimitMEMLOCK=`, and `OnFailure=`.

The consumer already exists. Decision 39's rule that gyro adopts an existing mode rather than
unconditionally modesetting counts recovery after a crash among its callers, so the failure becomes
*the last frame holds, then a greeter fades in* rather than *the machine goes black*, at the cost of
an affordance already committed to.

**The fd store is not insurance. It is the mechanism.** *(Settled by reading, 2026-08-16.)* This was
recorded as a `// SPEC:` question — whether the framebuffer survives `drm_file` teardown on the
strength of the plane's reference — and the answer is no, by explicit ABI rather than by accident.
`drm_file_free()` calls `drm_fb_release()` *before* it releases master, so the teardown runs while
the dying file can still commit. `drm_fb_release()` branches on the framebuffer's refcount, and a
refcount above one is exactly the on-screen case, because a plane scanning a buffer out holds a
reference to it. That branch reaches `drm_framebuffer_remove()`, whose comment states the rule
outright — *drm ABI mandates that we remove any deleted framebuffers from active usage* — and thence
`atomic_remove_fb()`, which walks every plane holding the framebuffer, sets its `fb` and `crtc` to
`NULL`, and commits; if that commit is rejected it retries with the CRTC's `active` cleared and its
mode unset. **The kernel performs a modeset to take gyro's last frame off the glass, synchronously,
as part of `close()`.**

So there is no fallback in which the CRTC keeps scanning out on its own. Every promise that the last
frame holds — across a crash, a restart, or a re-exec — rests entirely on the file description never
being released, which is what the fd store here and `FD_CLOEXEC` clearing in
[decision 39](#39-running-from-the-initramfs-is-deferred-and-deliberately-not-foreclosed) separately
achieve. Two consequences follow that the original framing would not have produced:

- **`FDSTORE=1` is sent immediately after first-open master**, before Vulkan initialization and
  before anything else that can fail. The interval between taking master and populating the store is
  the entire width of gyro's exposure to a black screen, so it is made as narrow as the code allows
  rather than left wherever the startup path happens to reach it.
- **Closing the DRM fd blocks on a modeset.** `drm_fb_release()` schedules the removal onto a
  workqueue and then flushes it, so a deliberate close is not cheap and gyro's death is not instant.
  It costs nothing on the frame path and is worth knowing on the shutdown path.

**Rejected: the reasoning this decision was first recorded with.** It read that with `fbcon=off` and
no other master, nothing would re-modeset when gyro's fd closed, so the CRTC ought to keep scanning
out and the fd store merely turned *probably* into *by construction*. The premise was wrong in a way
worth keeping: it went looking for a second party that might disturb the display and correctly found
none, when the party that disturbs it is the closing file's own release path. The conclusion
survived the premise — which is the outcome most in need of catching, since a sound mechanism
resting on an unsound argument is one tidying-up away from being deleted as redundant.

**A restart loop is worse than a crash**, so restart is rate-limited and exhausting the limit falls
through to `gyro --console`. That is decision 37's `OnFailure=` unit doing the job it already had,
and it converges with decision 43's undecided greeter respawn policy — both are "the thing that gets
you back into the machine is itself crashing", and they want one answer rather than two. The failure
being guarded is specific: a malformed message a client re-sends on reconnect, crashing gyro every
time, with the last frame frozen on glass indefinitely *because the mechanism above worked*.

**Clients do not survive, and that is the definition rather than a defect.** A connection is an fd;
when the process holding it dies the fd closes, and every toolkit treats that as fatal. So the
restart contract is: helpers re-offer their listeners
([decision 24](#24-the-listener-is-handed-to-gyro-not-created-by-it)), every client on the machine
is gone, and the machine returns to a greeter. Because that is a real recovery path it earns the
treatment decisions 34, 38, and 41 give recovery paths — exercised rather than assumed, which for
the re-offer means testing it long before an upgrade is the first thing to depend on it.

**Rejected: a per-user proxy holding client connections** across a restart. Already rejected in
[decision 23](#23-connection-identity-comes-from-the-listening-socket-per-user) on syscall grounds,
and the durability framing makes it worse rather than better: a proxy able to replay state to a
restarted gyro would have to understand every protocol object, which is a second compositor with the
crash relocated into it.

**Rejected: reconnect-and-restore as a protocol extension.** The clean answer if the ecosystem were
ours to move. It is not, and a mechanism that works only for clients which opted in has a failure
mode that varies per application — worse than a uniform one, because it cannot be reasoned about or
tested as a single behaviour.

**Deferred, and it is the only lever that actually shrinks a restart: dispatch as a process per
session.** The blast radius is large because one address space holds both the riskiest code and the
most catastrophic responsibility: the demarshaller terminates untrusted input from every account on
the machine, and it currently shares an address space with DRM master. Split it, and a demarshaller
crash costs one user's session, which is the failure mode a session compositor has by construction.

*(Annotated 2026-08-16.)* [Decision 2](#2-gyro-owns-the-protocol-seam-libwayland-implements-the-server-codec)
now puts libwayland's demarshaller there rather than gyro's, which changes who is responsible for
fuzzing it and changes nothing about this argument — the code is no less untrusted-input-facing for
being upstream's, and it is in the same address space either way.

[Decision 45](#45-protocol-dispatch-is-a-thread-not-a-task) pre-figures this almost entirely. The
publication boundary is already the only channel, already single-producer / single-consumer, already
wait-free on the reader, already deferred reclamation rather than shared ownership. Crossing a
process boundary is a change of *medium* — shared memory and a ring — rather than of shape, and the
buffers are already fds. Decision 45 says as much about sharding: cheap to add later precisely
because the boundary is the only channel.

It is not built now, for two reasons. It is a large change to the highest-performance path in the
system. And it has one genuinely awkward consequence: the frame side would be reading memory a
lower-trust process can corrupt, so it must treat the published snapshot as untrusted, which is real
frame-path cost that partially spends the isolation it buys. Decision 45's placement of dmabuf
import would have to move as well, since a dispatch process creating `VkImage` s and exporting them
is worse than importing on the render side.

**What keeps it available is one constraint, taken now:** the published snapshot is
**offset-addressed POD**, and handle resolution on the frame side is bounds-checked — recorded in
decision 45 and in [Architecture.md](Architecture.md#what-to-build-before-it-is-needed) with the
rest of the not-retrofittable set. A snapshot built on pointers or standard-library containers
cannot move into a shared mapping without rewriting every consumer; one built on offsets can. That
is the whole affordance and it is free at line zero.

**One requirement that bites only if the split lands, recorded before it is load-bearing: restart
fails closed on lock state.** Under
[decision 43](#43-lock-and-greeter-are-one-ui-locking-is-an-output-reassignment) locking is an
output reassignment held in gyro's memory, and a restart loses it. Today that opens no hole — every
client dies, so every session is gone, and the machine returns to a greeter that is locked by
definition. The moment sessions survive a render-process restart it becomes a lock bypass by crash,
which is the oldest trick there is. **A session whose lock state cannot be reconstructed is locked,
never unlocked.**

**Consequence for decision 2, recorded rather than left to be met later.** Its decisive argument was
that `wl_abort()` puts the machine's display going out at a moment a linked library chose outside
gyro's control. With the display surviving a restart, that sentence is weaker: what a library-chosen
abort costs is every client on the machine, not the display itself. The margin over libwayland
narrows a second time, on top of the narrowing decision 2 already records. The conclusion does not
move — every client on the machine is a great deal to lose to somebody else's `assert` — but
decision 2 is now deferred rather than committed, and this is one more reason to do the abort
reading before a demarshaller is written rather than after.

*(Superseded later the same day.)* The reading was done, and the conclusion did move: there is no
`assert` to lose anything to, and no abort on allocation failure at all. See
[decision 2](#2-gyro-owns-the-protocol-seam-libwayland-implements-the-server-codec). The paragraph
above is kept because its final clause is the one that mattered — this decision's narrowing is what
made the reading worth doing before the demarshaller rather than after, and doing it in that order
is what saved the weeks.

---

## Idle and power

Detail in [Architecture.md](Architecture.md#idle-and-power). Decisions 37–39 cover the two ends of
the display's lifetime; this is the middle, and it is the part a user meets several times a day
rather than once a boot. Two claims drive it. **Resume is the boot seam's twin** — the same
continuity argument decision 37 makes about BGRT, at a seam that occurs ten times more often and
that every Linux laptop visibly fails. And **the frames gyro does not draw are as much the product
as the frames it does**, because a compositor that hits every deadline and costs two hours of
battery has not succeeded at anything.

### 58. Idle is a ladder gyro executes and does not choose

The rungs are mechanism and closed. The timeouts are configuration. The decision to suspend the
machine is neither, and belongs to somebody else.

Each rung trades wake latency for power, which is the same shape as
[decision 34](#34-effect-quality-is-a-tier-gyro-chooses-and-the-floor-tier-is-the-recovery-path)'s
quality ladder and wants the same closure for the same reason:

| Rung                      | Why only gyro can execute it                        | Wake latency |
| ------------------------- | --------------------------------------------------- | ------------ |
| Dim                       | must dim everything, a fullscreen game included      | instant      |
| Backlight off, CRTC live  | a connector property; gyro holds master              | instant      |
| CRTC off (`ACTIVE=0`)     | an atomic commit; gyro holds master                  | ~100 ms      |
| Lock                      | an output reassignment — [decision 43](#43-lock-and-greeter-are-one-ui-locking-is-an-output-reassignment) | — |
| Suspend                   | **not gyro's** — see [decision 59](#59-suspend-is-a-handshake-on-the-control-connection-resume-is-a-modeset) | seconds |

**Dimming is the backlight, not an overlay**, so gyro drives `/sys/class/backlight` and the udev
rule granting it joins the DRM and input rules already in
[decision 22](#22-gyro-runs-as-a-dedicated-unprivileged-uid-with-cap_sys_nice-and-nothing-else)'s
table. An alpha overlay is the obvious alternative and it is wrong twice: it cannot go below the
panel's black, and it changes color rendition on the way down. It also collides productively with
[decision 47](#47-compositing-happens-in-linear-light-at-wide-primaries) — brightness changes
available headroom rather than scaling pixels, so **the idle dim and the user's brightness key are
one mechanism.** The dim composes with the user's setting rather than overwriting it, and restores
it exactly, or HDR headroom silently changes when the screen dims.

**The dim ramp is the most-watched animation in the system** — more often seen than any window
transition, and animated by nobody. It is free here: dimming is a compositor-owned `Animatable`, so
it is a catalog motion under
[decision 13](#13-a-closed-motion-vocabulary-with-runtime-configuration-exposing-only-that-vocabulary),
and undimming on input is interruption-by-retarget, which springs do natively. Sub-frame response to
a keypress falls out rather than being engineered.

**The shell sets values; the lock rung has an envelope.** Dim and blank are comfort and battery, and
there is no reason for gyro to argue about them. Lock is the one rung with a security consequence,
so the system configures a maximum lock delay and a shell may only be stricter. The shipped envelope
on a laptop is permissive — "never lock" is a legitimate thing to want at home — and its existence
is what makes gyro deployable somewhere it is not. Two properties make this work rather than merely
sound right. Overrides are **state in gyro, not a live subscription**, so they persist when a shell
dies: an override that evaporated on shell crash would change the ladder's behaviour at exactly the
wrong moment, and one that required a live shell would let a hung one hold the screen open forever.
And the shell **names rungs without inventing them**, which is decisions 13 and 33's closure applied
a third time.

**On battery versus on AC is a ladder selector, not a ladder.** Both profiles live in gyro's
configuration and something above selects between them. gyro does not read
`/sys/class/power_supply`: power source is not a display concern, and a gyro that sampled it is
exactly the first awkward case decision 51's open item warns about, where "declare, don't drive"
quietly becomes a per-event request.

**Idleness is a fold, and this is the part no session compositor has to solve.** Every other
compositor's idle state is per session because the process is. gyro's terms are unusual:

- Activity is a property of a **seat**, not a session.
- An output's power state follows the session **currently assigned to it**, so a locked panel is
  governed by the greeter's ladder and a locked user's inhibitor must not keep it lit.
- A session with no local output but a live consumer of a
  [virtual output](#26-remote-presentation-is-a-virtual-output-with-client-supplied-targets) is
  **not idle**, and the machine must not suspend under it.
  [Architecture.md](Architecture.md#a-session-is-not-a-display) already says a session with no
  output assigned is not suspended; this is that sentence's power-management consequence, which was
  never drawn.

**Injected input must not wake a local panel**, and this falls out of decision 26 rather than being
added to it. Injected input enters the same pipeline as local input so that `t₀` is the event time,
which is right for animation and wrong for display power: somebody remoting into a locked machine
sitting on a desk would otherwise light up its screen in a room they cannot see. Activity is
attributed to a seat, and a virtual output's input drives that output and nothing else.

**Inhibition is mechanism too, and three details are where implementations go wrong.** An
`zwp_idle_inhibit_v1` inhibitor is effective only while its surface is genuinely *presented* —
mapped, on an output, on an output that is on — which gyro is uniquely able to evaluate and which
most compositors approximate as "mapped", the reason a backgrounded video tab keeps a laptop awake.
An inhibitor is a resource a client spends the user's battery with, so it is attributed under
[decision 27](#27-resource-accounting-is-attribution-not-per-user-fairness), and the attribution is
a *feature*: "what is keeping this machine awake" is a question users ask and no Linux desktop
answers. And "the user has not touched anything" and "the system may go idle" are two different
facts — `ext-idle-notify-v1` grew an input-only notification because conflating them breaks activity
tracking — so both are answerable.

**Doing nothing must cost nothing**, and the invariant is hard: **when nothing is animating and
nothing has committed, no timer is armed and the frame thread blocks indefinitely.** Analytic
settling ([decision 11](#11-springs-are-closed-form-not-numerically-integrated)) is what makes that
exact rather than heuristic — dispatch knows the instant the last spring settles, so it knows when
there is nothing to wake for. Corollaries: cursor motion over a static screen touches the cursor
plane and triggers no composite; a blinking terminal cursor on one output does not wake the other,
which per-output damage already gives. It is falsifiable, so it belongs beside the schedulability
test in the headless harness — static scene, N seconds, assert zero composites.

**Rejected: idle as session state.** The obvious model, and it breaks on two configurations this
design already has. Fast user switching gives one machine several sessions with different idleness;
virtual outputs give one session presence with no local input at all. Both produce the same bug —
a machine that blanks or suspends while someone is using it, or one that never does.

**Rejected: the ladder as shell policy, with gyro executing per-rung commands.** More flexible, and
it puts the lock rung behind a client's liveness. A hung shell must not be able to hold the screen
unlocked, which is the same argument
[decision 25](#25-the-lock-screen-is-a-client-the-compositor-owns-lock-state-not-lock-ui) makes
about lock state generally, and it is why this decision is the split it is rather than a delegation.

**Rejected: an in-process screen-saver, or any drawn idle UI.** Same refusal as decision 25 for the
same reason. Dim is a scalar and a black screen is the absence of a frame; neither is UI with a
design. Anything more elaborate is a client, on an output like any other.

### 59. Suspend is a handshake on the control connection; resume is a modeset

Suspend crosses the connection [decision 24](#24-the-listener-is-handed-to-gyro-not-created-by-it)
already established, and **gyro still never holds a bus connection** — which is what keeps
[decision 7](#7-session-claiming-is-deferred-basu-rejected)'s deferral alive rather than quietly
spending it.

**Why a handshake at all.** Before the machine suspends, gyro must lock and *present* the locked
state, or the machine wakes showing the desktop for several frames while the lock screen paints —
a bug that has shipped in real desktops and that a user experiences as their unlocked session being
briefly readable by whoever opened the lid. It must also quiesce the frame loop, because it is about
to schedule against a clock that stops —
[decision 57](#57-one-timebase-clock_monotonic-converted-at-ingest-and-nowhere-else). Neither is
expressible as a reaction after the fact; both need the machine to wait.

**The greeter's session agent is the machine-level peer.** It is persistent, it exists from boot
under decision 43, it already runs a PAM stack, and it already holds a connection whose EOF means
something. It takes logind's `delay` inhibitor, sends `suspending`, waits for gyro's acknowledgement
inside the delay window, and releases. Symmetrically on resume. No new dependency, no new channel,
and the one already there was built to be exactly this shape.

**Resume is a modeset, not a wakeup**, and the checklist is the decision:

- **Every `FrameClock` is invalid.** Its last observation is from before the suspend and its period
  may describe a mode that no longer exists, because the machine may have been docked with the lid
  shut. Feeding that into `NextDeadline()` yields a deadline in the deep past, and decision 35's
  policy then fails every branch forever. This is the one latent defect in the design as previously
  written, and the fix is an explicit invalidate-and-reseed rather than anything clever.
- **Outputs may have changed**, so resume implies hotplug reconciliation and re-running admission
  control before the first frame.
- **Hard-settle every spring and drain the retiring set.** The mechanism exists twice already —
  [decision 46](#46-exit-snapshots-come-from-a-pre-reserved-per-output-atlas-exhaustion-finishes-exits-early)
  for atlas exhaustion and [decision 41](#41-device-migration-is-exercised-on-every-boot) for device
  loss. A user should not resume into the middle of yesterday's 300 ms transition, and the first
  live frame after resume should be a static full-quality composite rather than the worst frame the
  system will ever produce.
- **`VK_ERROR_DEVICE_LOST` is expected here**, not exceptional, particularly with a discrete GPU
  that powered off. Decision 41's migration path *is* the resume path, which is a payoff for a
  decision taken entirely for other reasons.
- **The client burst is real.** Every frame callback that did not fire for nine hours fires at once.
  [Decision 45](#45-protocol-dispatch-is-a-thread-not-a-task)'s per-client budget bounds it, but it
  lands at the moment the user is looking hardest.

**Which is why the last frame is the resume image.** KMS holds the scanout buffer across suspend —
the same property decision 41 relies on for migration — so if the locked state was presented before
suspending, resume is *the panel lighting up already showing the correct final image*. No black
frame, no flash of pre-suspend content, no repaint race. That is the whole user-visible point of
this decision, and it is decision 37's continuity argument at a seam that recurs.

Hibernate is the same shape: monotonic does not advance, KMS state is rebuilt by the kernel, the
checklist is unchanged.

**Rejected: gyro speaking D-Bus to logind directly.** The straightforward implementation, and it
spends decision 7's deferral on a use that does not need it. It also puts a bus client in the
process whose death takes every session on the machine, for a handshake that happens twice a day and
has a persistent local peer already connected.

**Rejected: reacting to resume without a pre-suspend handshake**, detecting the discontinuity by
comparing clocks or by a `/dev/rtc` wakeup. Cheaper, requires no peer, and it is exactly the design
that produces the flash of unlocked desktop, because by the time the discontinuity is observable the
frames have already been scanned out.

**Rejected: treating suspend as device pause via `ISession`.** It is the same interface shape, and
it conflates a device that went away with a machine that stopped. Device pause is decision 41's
territory and is answered by migration; suspend is a global time discontinuity that every clock,
every spring, and every output must be told about.

### 69. Settling answers with a wake; idleness folds a monoid, not an OR

*(Decided 2026-08-16, against the spring solver. Revises 58 and annotates 11 and 16.)*

Decision 58 makes *doing nothing costs nothing* a hard invariant, and decision 11's analytic settle
is what makes it exactly answerable. What neither settled is the **shape of the per-channel answer**,
and Animation.md carried `bool IsSettled(Instant t)` on `Animatable` by default rather than by
argument. The invariant is a fold over every animating channel, every pending timeout, and every
retiring entity; with a boolean, that fold is an OR.

**An OR has a term for resting and a term for moving and none for *later*.** So it excludes every
motion that is periodic rather than convergent — an indeterminate spinner, a marquee, a breathing
focus ring, and the blinking cursor decision 38's console owes. The closed form is indifferent: an
undamped oscillator is O(1) and exact at a predicted time like any other spring, so the frame path
does not care. What is missing is a way for such a channel to enter the ladder at all, and *one node
is pulsing* currently means the whole output can never fold to idle. That cost is paid invisibly by
systems with a mutable timeline; decision 58 makes it explicit, and until this decision it was
unpriced.

**The answer is a three-case `Wake`, reduced by `Sooner`, a commutative monoid with *settled* as its
identity.** `Settled` is nothing further ever; `Timed(when)` is one frame at an instant; and
`Continuous(when, interval)` is a frame at an instant and another every interval, without end. It
lives in `Core`. Architecture.md's invariant is restated from *no timer armed* to **at most one timer
armed per output, and its instant is the fold**, which is a stronger claim and a more testable one:
the dim and blank timeouts, the gesture-stop republish of decision 65, and retirement expiry become
contributions to one reduction rather than three separate arms.

*(Extended 2026-08-17.)* Clients contribute too, which this decision did not anticipate and the shape
absorbs without change.
[Decision 76](#76-cadence-authority-follows-predictability-not-foreground) reads a
`wp_commit_timing_v1` timestamp as `Timed` and a standing `wp_fifo_v1` pairing as `Continuous` at the
refresh period, so a surface with an outstanding commitment joins the animating channels, the pending
timeouts, and the retiring entities as a term in the same reduction. Worth marking here because the
contributor list above reads as complete and was not: it covered gyro-authored motion only, and an
output showing nothing but a video folded to *settled* between frames. That every case a client can
state maps onto an existing constructor, and that the one-frame horizon of a timestamp is honestly a
`Timed` rather than a defective `Continuous`, is the argument in this decision holding up against a
contributor it was not designed for.

**Rejected: `bool IsSettled(Instant)`** — the position this decision replaces. It is the ergonomic
signature, it costs the same as the alternative today, and its defect never presents as a bug. It
presents years later as a feature request that cannot be served without changing the fold, admission
control, and the wake path together. Kept here because *the boolean was adequate for everything we
had* is exactly the reasoning that makes a vocabulary permanent.

**Rejected: `std::optional<Instant> NextInteresting(Instant)`**, with `nullopt` for never. The
obvious repair — an any-reduce becomes a min-reduce at identical code volume — and it fails on the
commonest channel in the system. A spring in flight cannot name a next interesting instant, because
every instant between now and settling is one; it has to answer *now*, which the reduction cannot
distinguish from a one-shot that is merely overdue. That distinction is where a standing commitment
gets priced by decisions 29 and 31, so collapsing it reintroduces the invisibility this decision
exists to remove. It also splits the excluded motions wrongly: a blink is discrete and wants a wake
per edge, a marquee is continuous and wants every frame, and `t + period` describes only the first.

**And it collides with a sentinel the solver had already chosen.** `SettlesAt` returns a saturated
instant for a spring that never settles, deliberately, so that no frame reaches it. Under the
optional, the natural adapter spells that value as `nullopt` — *never interesting again* — and the
one motion in the design that genuinely never stops reports as idle. The correct adapter is one
keystroke away and looks equally reasonable. Under the three-case form the same comparison is false
forever and the answer is `Continuous`, so the composition is safe by construction rather than by
review. **A representation in which two opposite facts share a spelling is the wrong representation**
even when every current caller happens to write the right one.

**Rejected: spelling `Continuous` as `Timed(now)`.** Operationally identical for the scheduler's next
step and it discards the standing-ness, which is the only part admission control and the VRR servo
can act on. A throb that re-arms a one-shot every edge is indistinguishable from a cursor blink, so
the panel cannot be held at the throb's rate and the commitment cannot be charged.

**Rejected: `Continuous` without an interval.** Every frame is the conservative default and it is the
wrong ceiling for authored periodic motion: a 30 Hz throb on a 144 Hz panel is a quarter of the
composites and a rate the servo can hold. One defaulted field now, zero meaning every frame, against
a field that cannot be added later without revisiting every contributor.

**Rejected: `Wake` in `Animation`.** It reads as animation content and its commonest contributor is a
spring. `Console` depends on `Core`, `Geometry`, and `Seam` alone, and the recovery console's blink
is precisely a contribution the fold must accept — so the placement would force a second vocabulary
beside the first, and `CheckLayering.cmake` would say so. The idle ladder's own timeouts make the
same argument.

**What the monoid buys beyond the vocabulary**, and the reason to do this before `Animatable` exists
rather than after: associativity is what allows the fold to be partitioned per output, which decision
58's *a blinking cursor on one output must not wake the other* requires and which a scene-wide
reduction would undo; and it is what allows the result to be cached per subtree and recomputed along
the dirty path, instead of swept over every node whenever anything moves. An OR is also a monoid, and
a useless one. Retrofitting the algebra is more expensive than retrofitting the signature.

**Two obligations are on contributors and cannot be checked by the fold.** A timed instant is
strictly after the instant it was computed for, or the scheduler spins at full rate on a wake it has
already served. And a periodic contributor computes its next edge from its own origin, never from its
last wake — a wake is served at the following vblank, and adding to a rounded value accumulates
exactly the drift decision 11 exists to avoid, reappearing in the timer instead of in the trajectory.

## Rendering devices

Detail in [Architecture.md](Architecture.md#rendering-devices). The axis these decisions introduce:
*which physical device gyro renders on* is independent of *which backend it presents through*. gyro
may be on a GPU, on `simpledrm` before the real driver has loaded, or on software, and the frame
loop does not change. Decision 37 makes that unavoidable rather than merely tidy — starting before
the GPU driver means starting on something else.

### 40. Software rendering is a device, not a backend, and it is the floor tier

lavapipe — Mesa's software Vulkan driver, shipped in `mesa-vulkan-drivers` and therefore present
wherever Mesa is — advertises Vulkan 1.4 with `VK_EXT_external_memory_dma_buf`,
`VK_EXT_image_drm_format_modifier`, `VK_KHR_external_memory_fd`, and `VK_KHR_timeline_semaphore`.
That is the complete set gyro asks of a device. So the software path is **a physical device
selection, not a parallel code path**: no second renderer, no `#ifdef`, no backend.

The frame loop is unchanged. lavapipe executes command buffers on llvmpipe's rasterizer pool and
signals from a queue thread, so from the frame thread's side it is an asynchronous device that work
is submitted to — indistinguishable in structure from a GPU. Record, submit, wait on a timeline
semaphore, present.

**It is the permanent occupant of decision 34's floor tier.** Not a special mode with its own policy
— the quality floor, reached by a different physical device. `C_min` as a design target already
covers it, and it inherits the argument that the floor tier is a first-class exercised path.

Three riders:

- **Starvation is not what `SCHED_FIFO` risks here.** The frame thread only starves others while it
  is *runnable*, and it is blocked on a semaphore for most of a frame — provided it never busy-waits
  on a fence. The real hazard is priority inversion: the frame thread blocks in `vkQueueSubmit` or
  `vkWaitSemaphores` on a driver lock held by a lavapipe worker, that worker is preempted by any
  `SCHED_OTHER` process, and the composite waits on unrelated system load. So the frame thread stays
  `SCHED_FIFO` on this path, at a lower level than on the accelerated one, and the pool goes below
  it.
- **The rasterizer pool runs at `SCHED_FIFO`, one priority below the frame thread** — not at
  `SCHED_OTHER`, which would leave the composite at the mercy of every process on the machine. We do
  not own those threads, but they are in our address space: after device creation, walk
  `/proc/self/task/`, match `comm` against llvmpipe's thread names, and set policy. This works under
  `LimitRTPRIO=` alone and needs no capability — notably not `CAP_SYS_NICE`, which buys nothing here
  because there is no GPU queue to prioritize.
- **Interference is bounded by capping the pool size, not the CPU time.** `LP_NUM_THREADS` set to
  leave cores structurally reserved.

This is a genuine in-process priority inversion, and it was briefly offered as the repair for
[decision 2](#2-gyro-owns-the-protocol-seam-libwayland-implements-the-server-codec)'s broken rationale. It is
not one. A lavapipe worker holding a driver lock exists whether or not gyro owns the wire protocol,
and the fix is the priority ladder above rather than anything about Wayland — what it establishes is
that in-process inversion is real on this project, not that owning the protocol addresses it.
Decision 2 was re-argued on other grounds instead.

The ladder this rider sets up is completed by
[decision 45](#45-protocol-dispatch-is-a-thread-not-a-task), which adds the dispatch thread beneath
these workers on the same principle read in the other direction: the frame thread waits on the
rasterizer pool, so the pool must outrank everything below it; the frame thread never waits on
dispatch, so dispatch may go last.

**Rejected: a residency cap expressed as a percentage.** The obvious mechanisms do not fit. cgroup
v2's cpu controller does not do RT bandwidth and refuses RT tasks in a non-root cgroup without
separate configuration, so `CPUQuota=` bounds `SCHED_OTHER` threads cleanly and `SCHED_FIFO` threads
badly. `SCHED_DEADLINE` expresses it exactly — `sched_setattr(runtime, deadline, period)`,
kernel-enforced, with runtime/period being literally the residency fraction — and is worth noting
for the resemblance:
[decision 29](#29-outputs-are-periodic-real-time-tasks-the-test-allocates-effect-budget) applies the
processor demand criterion to periodic tasks, and `SCHED_DEADLINE` is the kernel doing EDF with
admission control over the same `(C, D, P)`. It does not replace decision 29, because one frame
thread serves many outputs at different periods and one DL task cannot express that. Capping the
pool is preferred over both: a tile cannot be preempted mid-raster anyway, so bounding *concurrency*
bounds interference more predictably than bounding *aggregate time*, and it degrades into a longer
frame rather than a stall waiting for a throttle window to refill.

**Separately, and on every path: `RLIMIT_RTTIME` on every real-time thread.** Not a residency cap —
a watchdog that `SIGXCPU` s then `SIGKILL` s a `SCHED_FIFO` thread running continuously without
blocking. Cheap insurance against a runaway frame loop hard-locking a machine that decision 37 has
deliberately left with no VT to escape to.

**This is gyro choosing deliberate death for itself, and the test it passes is worth stating**, since
[decision 2](#2-gyro-owns-the-protocol-seam-libwayland-implements-the-server-codec) spent three
rationales on a library doing the same thing. *(Added 2026-08-16.)* The distinction is not that one
unilateral termination is acceptable and the other is not. It is *who decides, against what
condition, and with what alternative*. The watchdog fires on a condition gyro named, at a threshold
gyro set, against the one outcome strictly worse than dying: a `SCHED_FIFO` thread spinning on a
VT-less machine is unrecoverable without a power cycle, whereas a dead gyro is recovered by decision
37's `OnFailure=` unit at the cost
[decision 27](#27-resource-accounting-is-attribution-not-per-user-fairness) now states precisely.

*(Revised later the same day.)* This paragraph used to end by contrasting that with what decision 2
objected to — termination at a moment a third party chose, for a condition gyro did not name, where
the alternative was simply not dying. The contrast is retired, because the reading behind decision 2's
reversal found that libwayland's remaining aborts pass this same test: the conditions are gyro's own
API misuse, and the alternative to aborting is a corrupted refcount or a NULL dereference rather than
not dying. The test was the right one. It just does not discriminate the way this decision assumed
when it was written.

**And it does not cover the case it most resembles.** A frame thread deadlocked on a driver lock —
the inversion named at the top of this decision — is *blocked*, not running, so it accrues no
real-time budget and trips nothing. Nor does a livelock that yields. `RLIMIT_RTTIME` catches a spin,
which is one failure mode of several, and the rest is why `sysrq` is a deployment requirement
alongside `pstore` rather than an afterthought.

### 41. Device migration is exercised on every boot

`simpledrm` binds the EFI framebuffer before the real GPU driver loads, and the real driver replaces
it. Rather than waiting for the final device, gyro starts on whatever is present and **migrates** —
which means the device-loss recovery path in
[decision 22](#22-gyro-runs-as-a-dedicated-unprivileged-uid-with-cap_sys_nice-and-nothing-else) 's
open list runs on every single boot instead of rotting until a rare hotplug finds its bugs. Same
argument as decisions 34 and 38: the path needed when things break must not be the path nobody has
executed.

**Migration is free, because KMS holds the last frame.** Nothing needs to be rendered during the
transition — the current scanout buffer stays on screen at zero cost while the new device is brought
up, and then gyro flips. At boot the content is a static logo, so the transition is invisible.
Mid-session GPU loss becomes "the screen holds, then resumes", which is honest and far better than
black.

What it forces at line zero, none of which is retrofittable:

- **No GPU resource may be the only copy of anything.** Every texture, atlas, and buffer gyro owns
  must be re-creatable from CPU-side truth; "upload and forget" is banned. Same discipline class as
  no allocation on the frame path — free if adopted now, a rewrite later.
- **`wp_linux_dmabuf_feedback_v1` from the start, never static format advertisement.** lavapipe
  advertises `VK_EXT_image_drm_format_modifier` but will only accept `DRM_FORMAT_MOD_LINEAR`, so a
  buffer a client allocated tiled for amdgpu will not import into it, and the reverse. Changing
  device changes the modifier set, which means clients must re-allocate. Wayland has exactly this
  mechanism; building the older static path forecloses migration entirely.
- **All Vulkan handles behind a unit that tears down and rebuilds whole.** No global `VkDevice`, no
  static pipeline handles.

Runtime shader recompilation comes free from
[decision 9](#9-volk-and-runtime-shader-compilation-in-vma-and-a-test-framework-out).

**Exit snapshots are the one admitted exception to the first rule.** *(Added 2026-08-16.)* Their
source pixels belong to a client that may already be gone, so there is no CPU-side truth to rebuild
them from. They are not made re-creatable; they are **permitted to be lost**, and the loss path is
the eviction path
[decision 46](#46-exit-snapshots-come-from-a-pre-reserved-per-output-atlas-exhaustion-finishes-exits-early)
needs for memory pressure anyway. Recorded as an exception rather than left to be discovered, since
the two ways of discovering it are building an unnecessary readback to satisfy the rule, and
assuming the rule holds and shipping a use-after-destroy.

Surprise removal is what makes that exception safe to take and the rule itself load-bearing. Unplug
a Thunderbolt GPU and there is no notice and no opportunity to read anything back, so every
reclamation path has to work with no GPU at all — which is an argument for the discipline everywhere
it can be met, and for permitting loss in the one place it cannot.

**Migration is an admitted multi-frame stall**, and
[decision 35](#35-a-miss-costs-one-frame-bounded-by-the-floor-composite) does not cover it. Stated
rather than left to be inferred: the promise is that the last frame remains on glass throughout, not
that no frame is missed.

**Rejected: waiting for the real GPU driver before starting.** Simpler, and it discards the reason
to start early at all — the firmware logo would persist correctly, but verbose boot, the passphrase
prompt, and any failure before the driver loads would all have nowhere to draw. It also leaves
device migration untested until it matters.

## Login

Detail in [Architecture.md](Architecture.md#the-login-agent). Decision 24 established that gyro
learns about sessions because a listener is offered to it. These decide who does the offering, and
what the user sees at the two moments that are hardest to make feel deliberate — first login, and
coming back to a locked machine.

### 42. The login agent speaks the greetd protocol, but is not greetd

greetd's IPC is exactly right for the part nobody enjoys writing: a u32 length prefix plus JSON over
a socket named by `GREETD_SOCK`, and a genuine PAM conversation proxy — `auth_message` typed as
`visible` / `secret` / `info` / `error`, answered by `post_auth_message_response`. That shape
handles fingerprint, 2FA, expired-password change, and "touch your key now" structurally rather than
as special cases, which is the difference between a login stack that can grow and one that cannot.

So **gyro's login agent implements the greetd protocol.** That is a protocol we speak, not a library
we link, so it adds no dependency. The payoff is disproportionate: every existing greetd greeter —
`agreety`, `tuigreet`, `gtkgreet`, `ReGreet`, `wlgreet` — works against gyro unmodified, which
means **the entire boot chain can be brought up end to end before one line of greeter UI is
written.**

**Rejected: using the greetd daemon.** Packaged in Fedora, so decision 8's build-complexity bar is
cleared, and it was the expected answer. Three structural conflicts, each independently fatal:

- **It is a single-session state machine.** `start_session` executes *after the greeter process
  terminates*, and the greeter restarts only once a user session ends. That is the exact inverse of
  [decision 21](#21-many-sessions-connected-one-presented-locally).
- **The greeter dies to start a session.** gyro's greeter session is permanent from boot, and its
  permanence is what makes unlock instant under
  [decision 43](#43-lock-and-greeter-are-one-ui-locking-is-an-output-reassignment).
- **It is VT-centric** — `[terminal] vt = …` with switching. Decision 37 has no VTs.

A useful accident of the protocol carrying no session identity, because stock greetd only ever has
one: it does not need one. When someone authenticates as a user who already has a live session, the
agent authenticates and then *switches* rather than spawning. The greeter never learns the
difference, so unmodified greeters get fast user switching with no protocol extension.

[Decision 44](#44-a-session-is-a-user-identity-is-the-uid) is what makes that a rule rather than a
guess: with one session per user, "the user's session" is unique, so switch-or-spawn has no third
case and needs nothing from the protocol that greetd does not already carry.

### 43. Lock and greeter are one UI; locking is an output reassignment

Locking moves the output to the greeter's session. There is no separate lock screen, no separate
lock client, and no lock surface belonging to the locked session. This is
[Architecture.md](Architecture.md#a-session-is-not-a-display) 's decomposition followed through
rather than a new position.

**The decisive argument is that spoofing stops being a policy and becomes structural.** Under
`ext-session-lock-v1`, the model every other Wayland compositor uses, the lock surface is a client
of the user's *own* session, so "which client may be the lock screen" is a rule gyro must enforce
correctly, forever, against every client in that session. Here, when an output is locked gyro has
assigned it to the greeter session and the user's session is not composited **at all**. A malicious
client cannot draw a false password prompt because it cannot draw. That is not a check to be
bypassed; it is a different uid on a different session.

Unlock is instant because the greeter session exists from boot — an output reassignment with no
process start. That property is load-bearing, not incidental: a greeter spawned on demand would put
process startup latency in front of every unlock.

**Cost accepted: the locked screen cannot show user-owned content.** Notifications, media controls,
and lock-screen widgets are all the same problem — they live in the user's session, at the user's
uid, and the greeter is neither.

Wallpaper came off that list on 2026-08-16, and by a route worth noting because it is the shape any
further exception has to take. Under
[decision 51](#51-the-shell-is-a-per-session-client-gyro-owns-mechanism) gyro owns the background
and holds a persisted copy of it, so a locked output can show the wallpaper of the user who locked
it with **gyro** compositing the image and the greeter never receiving it. That is not a hole in the
isolation; it works precisely because gyro, not the greeter, is the party that already has the
pixels. The same decision refuses the neighbouring move — compositing a user's background before
that user has authenticated — for the reason this decision exists.

The designed answer, **not built initially**, is a **lock-screen content surface**: a locked session
may present one non-interactive surface, which gyro composites below the greeter's UI on the output
that session was locked out of. It works because the privacy decision ("hide previews when locked")
is then made inside the user's own session by the user's own software, and gyro never inspects the
content; because non-interactive is the safety property, so keystrokes meant for the password field
can never reach a client of the locked session (which puts notification *actions* out of scope); and
because it is scoped by construction, so "user A's notifications while user B is at the greeter" is
impossible rather than forbidden. Deferring it is safe precisely because unification does not
foreclose it.

**Requirement this creates:** the greeter is now on the critical path for getting back into the
machine, and one greeter serves every session, so a greeter crash locks out everybody. The login
agent must respawn it, and gyro's lock state must be entirely independent of the greeter's liveness.
Decision 25 already says gyro owns lock state and not lock UI; that is now load-bearing rather than
tidy.

**And a locked output is never blank.** *(Amended 2026-08-16; this entry previously said the output
stays black across a respawn.)* Decision 51 gives gyro the background and a persisted copy per uid,
so the locked state gyro composites is available with no client running — locking is immediate, the
greeter's UI arrives over the wallpaper when it is ready, and a greeter crash costs the password
prompt rather than the picture. Decision 51's session-ready gate does **not** apply to lock: waiting
for the greeter would hold the outgoing session's pixels on a screen that is supposed to be locked,
which is the failure decision 59 exists to prevent, at a seam that recurs many times a day rather
than once. **Lock is the one output reassignment that may not wait.**

The general form is worth stating, because the first reading of this went the other way: a conflict
between isolation and the experience is a defect in the architecture, not a trade to be settled by
ranking them. Here the second look found that gyro already held the pixels, and the conflict was
never real. See [Experience.md](Experience.md#the-six-promises).

**Rejected: `ext-session-lock-v1` 's model**, with the lock surface as a client of the locked
session. It is the ecosystem-standard approach and it solves the notification problem for free. It
loses the structural anti-spoofing property, it needs a correct and permanent policy about which
client may claim the lock role, and it means lock and switch-user are two transitions instead of
one.

---

## Color

Recorded 2026-08-16, last of the pre-implementation decisions and the only section prompted by an
absence rather than by a question. Color appeared nowhere in this log until it was noticed that
three decisions already recorded — 13, 18, and 34 — depend on an answer it had never given. It
belongs with device migration and the publication boundary in the class of things that are free at
line zero and a rewrite afterwards.

### 47. Compositing happens in linear light at wide primaries

The composite space is **linear, Rec.2020 primaries, brightness-relative with 1.0 as SDR reference
white.** Every surface carries a color state — primaries, transfer function, alpha mode, reference
luminance — from line zero. Untagged content is sRGB *by rule*, never by inspection.

**One rule forces the rest.** Blending, scaling, mipmapping, and blur are all weighted sums of
light, so they are correct only in a space proportional to light. Everything else in color
management is appearance matching, which is negotiable and taste-laden; this part is arithmetic.
gyro does all four constantly and blur is the feature, so there is no version of this project in
which the rule is ignored cheaply.

**Alpha is un-premultiplied before linearisation, once, at import.** Wayland's `ARGB8888` is
premultiplied and the client computed `S = encode(C)·α`, so a hardware sRGB sampler returns
`EOTF(C·α)` where the wanted value is `EOTF(C)·α`. The error factor is `α^1.2` — about 13% too dark
at `α = 0.5` — and it lands on every soft edge in the system. This is the one place where doing
color half-way is worse than not doing it: a compositor blending in encoded space is wrong but
self-consistent, whereas one that linearises without un-premultiplying is wrong at every partially
transparent pixel, by an amount that varies with alpha.

**Precision is per-target, and the blur chain is the one that mattered.** Linear light at 8 bits
bands unacceptably in the shadows, which appeared at first to put a 16-bit float format on the pass
chain that [decision 29](#29-outputs-are-periodic-real-time-tasks-the-test-allocates-effect-budget)
identifies as the dominant term in `C` — a doubling of bandwidth on precisely the wrong pass. It
does not. **A backdrop blur chain carries no alpha**, because a backdrop is opaque by construction,
so `VK_FORMAT_B10G11R11_UFLOAT_PACK32` holds linear light at 32 bits per pixel: the same bandwidth
as the `RGBA8` the chain would have used anyway. Correct blur is therefore bandwidth-neutral against
the budget rather than a claim on it. The alpha-bearing composite target is 16-bit float and there
is one per output. Rec.2020 is what makes the packed format usable at all — it has no sign bit, so
it needs a primary set wide enough that ordinary content does not go negative.

**Brightness is relative, not absolute.** 1.0 is SDR reference white and HDR headroom lives above
it, as a multiple that varies with display brightness. Under the alternative — absolute nits
throughout — every SDR-beside-HDR case becomes a policy question answered separately, and the
policies interact. Apple's EDR is this model and it is the part of macOS worth copying outright. The
brightness control then changes available headroom rather than scaling pixels, which is an
architectural consequence rather than a UI choice.

**Direct scanout requires transform equivalence.** A client buffer flipped straight to a plane
bypasses the composite pass, so every transform the composite would have applied must be expressible
in the KMS color pipeline — per-plane degamma, CTM, gamma, or the newer pipeline properties — or
the picture changes when the fast path is taken, which is a visible flash at the moment a fullscreen
client is promoted. Scanout is therefore conditional on the hardware expressing the identical
transform and composites otherwise. That is a constraint on `IPresenter` and on plane assignment,
and it is far cheaper to state now than to retrofit into
[decision 5](#5-the-drm-backend-is-designed-from-specification-with-no-hardware-spike)'s
deliberately over-provisioned presenter after it is written.

**Three recorded decisions were already depending on this**, and none of them said so — which is the
argument for settling it now rather than when HDR arrives:

| Decision | Operation | What encoded-space compositing does to it |
| -------- | --------- | ----------------------------------------- |
| [34](#34-effect-quality-is-a-tier-gyro-chooses-and-the-floor-tier-is-the-recovery-path) | chain internal resolution, a downscale | darkens, so the "very nearly invisible" first rung shifts brightness at every tier step |
| [13](#13-a-closed-motion-vocabulary-with-runtime-configuration-exposing-only-that-vocabulary) | reduced motion replaces movement with cross-fades | luminance dips mid-fade, on the accessibility path, at every transition |
| [18](#18-matched-geometry-is-in-the-first-cut) | window to overview thumbnail, a scale | the thumbnail arrives dimmer than the window it came from |

**Rejected: sRGB-encoded compositing**, which is what essentially every Wayland compositor does
today. Cheap, self-consistent, and what the ecosystem's content was tuned against — see
[decision 48](#48-linear-blending-is-a-visible-ecosystem-change-and-gyro-takes-it), where that cost
is paid rather than dodged. Rejected because it turns the three rows above from operations into
artefacts, and because it forecloses HDR at the format level rather than at the feature level.

**Rejected: scRGB** — linear with sRGB primaries and values permitted outside `[0,1]`, which is what
DWM composites in. It reaches equivalent gamut coverage through negative values instead of wide
primaries, and negative values need a signed format, which costs the packed blur chain above. The
same destination, with strictly more bandwidth on the one pass that cannot afford it.

**Rejected: absolute-nits composition.** Easier to reason about for HDR video in isolation and worse
at everything else. Windows is the demonstration: SDR content mapped to a slider-chosen nit value
rather than to a definitional reference white is the origin of "HDR makes everything look washed
out", and years of policy have gone into partly repairing what the relative model gets right by
construction.

**Rejected: deciding color when HDR arrives.** The tempting position, since HDR output is out of
scope initially and nothing here is needed to light a pixel. It fails the same test as
[decision 41](#41-device-migration-is-exercised-on-every-boot)'s "no GPU resource is the only copy
of anything": composite space, target formats, and per-surface color state are structural, they are
chosen the day the renderer is written, and every effect and animation authored against the wrong
one is re-authored. The *features* — tone mapping, gamut mapping, output characterisation, the
color-management protocol itself — are additive on top and genuinely deferrable, and are left in
the open list as such.

**Cost accepted:** one 16-bit-float composite target per output; color becomes a dimension of every
golden image, so [decision 4](#4-all-three-backends-are-in-scope-nested-headless-drm)'s headless
comparisons must pin the whole pipeline or they are brittle in a way that is miserable to diagnose;
and the material vocabulary in
[decision 33](#33-effects-are-named-materials-not-parameterized-filter-calls) must now be designed
against a defined space, which is a fresh constraint on a vocabulary that does not exist yet.

### 48. Linear blending is a visible ecosystem change, and gyro takes it

[Decision 47](#47-compositing-happens-in-linear-light-at-wide-primaries) settles the arithmetic.
This settles what the arithmetic does to content nobody here wrote — because the answer is
*something*, and meeting it after the renderer exists would present as a bug in gyro.

**The exposure is one thing, not everything.** Most of a desktop involves no compositor-level
blending at all: an opaque window over an opaque window is a copy or an occlusion cull. What is
blended is client-drawn CSD drop shadows, dominant by area; client-drawn rounded corners and their
antialiased edges; translucent surfaces; and subsurfaces. gyro's own shadows, blur, dimming, and
cross-fades are gyro's to tune and are correct by construction. So the question reduces to: what
happens to GTK and Qt shadows.

**They lose about half their depth on a light background.** For a black shadow the composited result
is `B·(1−α)` in whichever space the blend happens, so the encoded outputs differ by a factor of
`(1−α)^(1/2.2)`, which grows with `α`. At a background of level 235 with a peak shadow alpha of
0.35, the darkest point of the shadow moves from level 153 to level 194 — 41 levels of depth where
there were 82. On a dark background the same shadow moves from 26 to 31 and nobody will ever see it.
Because the factor grows with `α`, the shadow does not lighten uniformly: its internal gradient
compresses, so it reads as diffuse and ungrounded rather than as subtle.

**This is a mistuning, not an inferiority**, and the distinction decides what to do about it. The
alpha in a CSD shadow is not physical opacity — there is no occluder. It is a designer's dial,
turned until it looked right against the blending behaviour that designer's system actually had.
GTK's was turned against encoded-space blending because every Linux compositor blends that way.
Apple's was turned against linear, because `WindowServer` composites in linear, and macOS shadows
look excellent. Nothing about correct blending makes shadows look bad. It makes shadows tuned for
the other model look wrong.

**The answer is that gyro draws the shadows.** Server-side decorations are where this design already
points — a closed vocabulary, gyro deciding what things look like, call sites naming no parameters,
exactly as in
[decisions 13](#13-a-closed-motion-vocabulary-with-runtime-configuration-exposing-only-that-vocabulary)
and [33](#33-effects-are-named-materials-not-parameterized-filter-calls). Under SSD the shadow is
gyro's, tuned in linear, and right. That reduces the exposure to clients insisting on CSD rather
than eliminating it, GTK being the stubborn case, and that residue is what this decision accepts.

**Rejected: the legacy alpha remap.** Apply `α' = 1 − (1−α)^2.2` at import to any surface that has
not declared a color state, on the reading that untagged means authored against encoded-space
blending. Genuinely tempting: for a pure black source the correction is *exact* and independent of
the background, which is precisely what a drop shadow is, so it fixes the dominant case perfectly —
and it self-retires, since a client adopting the color-management protocol gets straight linear.

Rejected because it delivers compatibility with other compositors, which is not the goal, in place
of cohesion, which is. A remapped GTK shadow beside gyro's correctly blended one looks different
from it — the same two-blending-models-in-one-frame problem, relocated rather than solved, and
[Animation.md](Animation.md#priorities) puts cohesion first. It is also exact only for black:
colored translucency, which is most of a transparent terminal, degrades. And a compatibility remap
in the composite path is permanent once shipped, because the content it compensates for never stops
arriving.

**Rejected: blending in encoded space to match the ecosystem.** The honest form of the same impulse,
and it fails on decision 47's table. It would not merely leave GTK's shadows where their designer
put them; it would turn gyro's own tier steps, cross-fades, and matched-geometry scales into
artefacts. Two blending models cannot coexist in one frame, so the only question is which one, and
just one of them is correct and extends to HDR.

**Cost accepted:** every GTK and Qt window using client-side decorations has a lighter, flatter
shadow under gyro than under any other compositor, most visibly on light backgrounds, until it
adopts server-side decorations. This will be reported as a bug. It is not one, and this entry is the
reply.

**The exit is not server-side decoration adoption.** *(Revised 2026-08-22.)*
[Decision 96](#96-the-frame-is-the-compositors-and-the-header-is-the-apps) reads the cost-accepted
paragraph above against what GTK has actually said, which is that it will not adopt server-side
decorations. So *until it adopts server-side decorations* is not a horizon. It is a permanent cost on
most of what an ordinary person runs, which makes this a different entry from the one it was written
as.

What that decision changes is the shape of the ask rather than the arithmetic. This entry treats
client-side decoration as one refusal and it is not: a toolkit wants its own header bar, has no
position at all on the shadow, and already publishes that shadow's extent to every compositor on the
wire through `xdg_surface.set_window_geometry`. The exit is a hint that lets a client say it has
stopped drawing one — after which gyro's own shadow, derived from the window's alpha silhouette,
needs no further cooperation. The residue stands until then, and what it is waiting on changes from a
toolkit's decoration policy to a protocol nobody has written.

**To be confirmed by looking, not by arguing.** The composite pass is one place and the toggle is a
build flag, and [decision 4](#4-all-three-backends-are-in-scope-nested-headless-drm) makes nested
the daily driver, so this needs no hardware and no DRM backend: a GTK application on a light
wallpaper, flipped between the two. Recorded as a decision rather than as an open question because
the direction does not depend on the result — only the urgency of the mitigation does.

---

## Geometry

Recorded 2026-08-16, immediately after color and prompted the same way: the word "coordinate"
appeared nowhere in this log, and three decisions already recorded — 17, 18, and 28 — turn out to
rest on an answer it had never given. Scaling is where compositors are most visibly bad, and the
reason the bugs are so hard to attribute afterwards is that almost every one of them is a value that
got rounded once and then stored.

### 52. Coordinate spaces are three, and quantization belongs to the output

There is one **global layout space** — continuous, real-valued, output-independent, Y-down — and it
is where the model lives. Below it is **buffer space**, integer texels belonging to the client.
Above it is **output device space**, one per output, integer only at the boundary. Two adapters
connect them and both are exact rather than inferred: the *surface* adapter is whatever
`buffer_transform`, `buffer_scale`, and the viewport's `src` and `dst` say it is, and the *panel*
adapter is an integer rotation and flip that KMS may execute or the composite may.

**Those four inputs produce a scale per axis, not one number.** *(Corrected 2026-08-16.)* `src` and
`dst` may disagree in aspect ratio — which is how anamorphic video reaches a compositor — so the
sentence above reads as though the surface adapter were a single rational when it is two. The
restricted transform stays exactly closed under the widening, since a quarter turn merely exchanges
the two factors, which is what lets one type serve all three adapters rather than the surface one
needing an algebra of its own that would have to agree with theirs.

The rule that makes this a design rather than a diagram: **no integer ever flows backwards into the
model.** Every rounding is a pure function of `(node, output, frame)` and is discarded with the
frame that computed it.

This is the third instance of a seam the design already has. Model versus presentation separates
what was set from what is drawn;
[decision 28](#28-the-frame-clock-is-per-output) separates one scene into per-output evaluations at
different *times*; this separates it into per-output evaluations on different *grids*. A frame was
already `(output, predicted presentation time)`. It is also `(output, device grid)`, and the second
half is close to free because the first half already forced the traversal to be per output.

The basis unit of global space is "one logical pixel at scale 1", chosen for wire compatibility and
for nothing else. It carries no implication of integer alignment, and there is no stored logical
size of a window anywhere in the system — there is a real-valued rectangle and a rounding of it per
output.

**Three places an integer is unavoidable, and each is a boundary rather than a store:**

- **`xdg_toplevel.configure`**, which is integer logical and does not divide evenly at fractional
  scale. Layout is computed in device pixels for the output the window is on, the logical size is
  rounded for the wire, and gyro **absorbs the remainder into its own gap** rather than letting it
  fall between two windows. A one-pixel seam showing the background between tiled windows is the
  signature artefact of fractional scaling, and it is this remainder placed wrongly. A configure is
  also a *request*: the client may answer with something else, and nothing may assume it did not.
- **The cursor plane, and surface-local pointer delivery.** The pointer is real-valued in global
  space, accumulated from libinput's own doubles and constrained in global space. It is rounded
  twice — once into the plane's device position, once into `wl_fixed` for delivery — and never
  round-tripped back through either. Quantizing it to logical pixels would leave a 3840-wide output
  at 1.5 with 2560 addressable columns, which is a mouse that physically cannot reach a third of the
  display.
- **Damage and scissor rectangles**, which are the enclosing integer rectangle *plus the resampling
  filter's support radius in output pixels*. Omitting the kernel footprint leaves one-pixel trails
  behind moving content: intermittent, absent from screenshots, and expensive to find later.

**Positions cross the publication boundary at double precision** and everything else at single.
Single gives 1/256 of a pixel around ±32768, which is exactly `wl_fixed`'s resolution and too close
to the floor for a large arrangement of outputs. Four bytes per position, on a structure whose cost
is dominated by cache lines, buys out a precision budget that would otherwise be re-verified every
time somebody adds a monitor.

Virtual outputs are not an exception.
[Decision 26](#26-remote-presentation-is-a-virtual-output-with-client-supplied-targets) makes them
ordinary outputs, so they carry a scale and a grid like any other, and a capture spanning two
outputs is a resample that has to be declared as one rather than discovered.

**Rejected: a quantized logical space.** Storing integer logical geometry and converting to device
pixels at the end is the near-universal design, and it is where the artefacts come from — a value
rounded once, stored, then rounded again elsewhere against a different grid. The rounding is not the
bug. The storing is.

**Rejected: physical units as the basis** — millimetres, or points at a reference viewing distance.
More principled-sounding, and it buys nothing. Two outputs at 1.0 and 1.5 have different sets of
well-aligned positions whatever the basis is called, so the basis cannot fix alignment and only
[decision 54](#54-settled-geometry-snaps-to-the-outputs-device-grid) can. It costs a conversion at
every protocol boundary, and it misdescribes what scale is: a television at three metres and a
monitor at sixty centimetres can have identical pixel density and want different scales, because
scale is a preference and not a derivation.

**Rejected: device pixels of a reference output.** Adding a monitor could then change the basis of
the entire layout, and which output is the reference is a question with no good answer at hotplug
time.

**Rejected: no global space, only per-output scenes.** It makes a window straddling a seam
inexpressible, which is the configuration [decision 28](#28-the-frame-clock-is-per-output) exists to
serve.

### 53. Scale is an exact rational

`wp_fractional_scale_v1` speaks 120ths, so the numerator is what is stored and size arithmetic is
integer. The reason is one line long:

```
1000 × 1.1        = 1100.0000000000001  → ceil → 1101
1000 × 132 / 120  = 1100                            exactly
```

The compositor and the client have to arrive independently at the same integer or there is a gap, an
overlap, or a protocol error. Making that agreement arithmetic rather than probabilistic costs one
type.

What makes it worth a decision rather than a code comment is that it is *nearly* never wrong. 1.25,
1.5, 1.75, and 2.0 are all exact in binary, so the ordinary settings-menu ladder round-trips
correctly and nothing ever appears. It appears at 110% and 133% — the arbitrary-percentage sliders
users actually reach for — at large surface sizes, on one output, intermittently. That is the
failure profile of something that ships.

**Rejected: float scale with an epsilon.** The epsilon has to be chosen against the largest surface
the system will ever round, so it is a function of resolution and it is wrong on the next monitor.

**Rejected: restricting scale to binary fractions.** Since 1.25, 1.5, and 1.75 are exact, confining
the settings UI to them makes the problem disappear. It is an interface restriction defended by an
implementation detail, and 120ths already express every fraction such a ladder would contain — plus
the awkward ones, correctly.

### 54. Settled geometry snaps to the output's device grid

A surface sampling one-to-one at a half-device-pixel offset is soft across its entire area, and that
is the most reported complaint about fractional scaling anywhere. So settled content snaps to the
grid of the output it is being evaluated for.

Snapping *during* motion is worse than not snapping, because it turns smooth translation into
stair-stepping. So it happens exactly at the transition to settled — where the residual is below the
settling threshold by construction, the snap is sub-pixel, and there is nothing to see. The spring
system already pays for that property; this is the first thing that spends it.

It cannot live in the model. One model position on a 1× output and a 1.5× output must yield two
different snapped positions, so the only place it can go is per-output evaluation — which
[decision 28](#28-the-frame-clock-is-per-output) built for the time half and which
[decision 52](#52-coordinate-spaces-are-three-and-quantization-belongs-to-the-output) forbids
writing back.

**The settling threshold acquires an output.** Animation.md expresses geometric thresholds in output
pixels, which is ambiguous the moment a node is on two outputs at different densities. It resolves
as the **finest** grid the node currently intersects: settle against the coarse one and the fine one
shows residual crawl. That is conservative, it costs a slightly longer settle, and it is a
dispatch-side query, which
[decision 50](#50-the-world-is-authored-on-the-dispatch-thread-the-snapshot-carries-coefficients)
already has the information for. Angular and scale channels convert through the node's bounding
radius — a rotation residual of ε displaces a corner by roughly ε·r — or rotation settles against a
criterion unrelated to anything anyone can see.

**Rejected: quantizing the model at layout time.** Cheaper, and strictly better where it works,
since the spring then converges to an aligned target and no snap is needed at all. It has no answer
for a window on two grids, and picking one output's grid to quantize against writes an output into
the model, which is the thing decision 52 exists to prevent.

**Rejected: snapping every frame.** Stair-steps, and it corrupts the settle test as well — a
quantized presentation value stops changing before the spring has, so slow springs read as settled
early.

**Rejected: never snapping.** Simple, honest, and it leaves every window on a fractional output soft
all of the time. The entire point is that the common case is crisp; a compositor that resamples
unconditionally has implemented fractional scaling in the sense that the number is respected and in
no other sense.

**Whether this reaches a node that is not sampling one-to-one was left open here and is settled by
[decision 67](#67-the-settled-snap-is-unconditional).** *(Annotated 2026-08-16.)* It does, and the
reason is that most of what the snap buys — the node's edge, its decorations, and its abutment with
its neighbours — is one-to-one even where its content is not. The wording above reads as though the
snap were for the sampled interior alone, which is what made the question look open.

### 55. Transforms are 3D; the scene is a painter's algorithm

*(Revises [decision 17](#17-transforms-are-decomposed-into-trs-with-per-channel-springs).)*

Nodes carry a full 3D affine with **node-local perspective and no camera**, and composition is
strict tree order with **no depth buffer**. Intersecting geometry therefore does not render
correctly, and that is a stated limit rather than an oversight. This is CoreAnimation's shape and
has been for fifteen years.

**The cost of 3D is not the matrix.** A 4×4 through a pipeline that is already a 3D pipeline is
free. The cost is in what a transform destroys, and that list is shorter than it first looks: damage
becomes the axis-aligned bound of a projected quad, which is computable; scanout eligibility is the
existing predicate returning false rather than a new class of problem; hit-testing becomes
ray-versus-plane and is barely exercised, because hit-testing reads model and settled model
transforms are axis-aligned. Even the blur backdrop survives — blur the screen-space bound at a
screen-space radius and sample it through the same projection, and
[decision 29](#29-outputs-are-periodic-real-time-tasks-the-test-allocates-effect-budget)'s admission
test still has an area to key on.

What does not survive is depth-buffered intersection with transparency. Painter's algorithm fails on
intersecting geometry, a depth buffer does not fix order-dependent transparency, and every surface
here has alpha with a backdrop reading through it. That is one of the two things a strict-tree-order
composite gives up; the other is group opacity, and it is
[decision 60](#60-group-opacity-requires-flattening-per-node-alpha-is-not-a-group-fade). Excluding
the first and paying for the second is what makes all the rest affordable.

**There is no camera**, which is the non-obvious half. Perspective is a property of a node's own
transform, applied in its parent's space, rather than a frustum belonging to an output. A per-output
camera would project a straddling window differently on each output, so the window would visibly
change shape across the seam — on precisely the configuration
[decision 28](#28-the-frame-clock-is-per-output) exists to serve.

**Rotation is a quaternion sprung in the log map.** Decision 17's per-channel intent survives and
gets more honest with it: rotation is one channel with one spring, not three axes pretending to be
independent. Two guards come along — perspective is bounded so the near plane never crosses the
quad, and back faces cull, because a window flipped past ninety degrees showing mirrored text reads
as a bug. A card flip is two nodes and a catalog transition, which is where it belonged anyway.

**Both guards were stated wrongly here and are corrected against the implementation.**
*(2026-08-16.)* The first read "the perspective distance is clamped", which is not something a
transform can do: a distance and a quad extent are independent, and scale is an animatable channel,
so a node growing while its stored distance stayed put walks its own far corner through the near
plane mid-transition — the frame that divides by zero and makes the damage bound a NaN. Perspective
is therefore held as a dimensionless strength in the node's own bounding radii, where the guarantee
is unconditional at every scale, and which is additionally the only form [decision
13](#13-a-closed-motion-vocabulary-with-runtime-configuration-exposing-only-that-vocabulary)'s
catalog can author — a distance in pixels is a different amount of perspective on a thumbnail than
on a full window. The second read "cull *by default*", implying an override whose only use is the
double-sided quad this same paragraph refuses; culling is unconditional, and a node that must be
visible from behind is asking for a material rather than for a geometry flag.

**"A full 3D affine" also overstates what is stored.** A decomposed TRS with an anchor cannot
express a shear, and non-uniform scale composes only in the node's own frame. That is the right
restriction rather than a shortfall — a shear has no meaningful spring, and decomposition is what
[decision 17](#17-transforms-are-decomposed-into-trs-with-per-channel-springs) is about — but
"affine" names a larger set than anything here can build.

**Anchor point becomes explicit**, and it was under-specified in decision 17 even in two dimensions.
Scaling from a corner rather than from the centre is most of what makes a transition read as
intentional.

**Rejected: two-dimensional affine only.** The position held until 2026-08-16, on the grounds that
3D would destroy damage tracking, occlusion culling, scanout eligibility, and the effect cost model.
Only the last was ever close and it does not hold: a projected quad has a screen-space bound and
therefore an area, which is all decision 29 needs. The restriction bought nothing and foreclosed
perspective overviews, card flips, and depth cues — a large expressive loss for a system whose first
stated priority is cohesion.

**Rejected: a depth buffer and correct intersection.** It would render intersecting layers correctly
and leave transparency order-dependent, which is the worse of the two failures, because every
surface in this system is translucent somewhere and `Material::Glass` samples what is behind it.

**Rejected: Euler angles.** Gimbal lock, and interpolation that does not take the shortest path.
Both present as a window taking a visibly strange route through a rotation, which is the failure
mode hardest to attribute to its cause.

**Considered: Fuchsia's retreat from a 3D scene graph, which is the field's one real datum here and
does not bind.** Scenic shipped `fuchsia.ui.gfx`, a 3D scene graph, and replaced it with Flatland, a
strictly 2D one. Three reasons are recorded and the first two do not transfer. Their clients were 2D
products that *declared* the 3D themselves, so the expressiveness was a mismatch with the people
using it; gyro's clients declare no transform at all — a shell names a catalog transition and gyro
owns the matrix, per [decision 51](#51-the-shell-is-a-per-session-client-gyro-owns-mechanism), so
there is nobody to mismatch. And their display controllers take strictly 2D rectangular layers, so a
client-declared 3D transform foreclosed plane promotion at arbitrary times and for arbitrarily long;
here a non-axis-aligned transform exists only while something is in flight, and
[decision 54](#54-settled-geometry-snaps-to-the-outputs-device-grid) returns the settled state to
the device grid — so the frames that lose scanout eligibility are the frames that were compositing
anyway. Their third reason is group opacity, and that one transfers in full; it is decision 60.

The corroborating detail is worth more than the argument. Flatland's entire client-facing transform
vocabulary is a ninety-degree-only orientation enum, a float scale, and an **integer** translation —
which is the restricted transform type
[Architecture.md](Architecture.md#resample-once-and-know-when-it-is-zero) asks for, promoted to
being the whole API. Two designs arriving independently at the same predicate is evidence the seam
is real rather than convenient. The integer translation is where they could afford to stop and gyro
cannot: it forecloses sub-pixel motion, which is the product.

### 60. Group opacity requires flattening; per-node alpha is not a group fade

*(Revises [decision 55](#55-transforms-are-3d-the-scene-is-a-painters-algorithm).)*

**Fading a subtree is not the same operation as fading each node in it, and the difference is
visible at every value except the two endpoints.** A subtree faded per node is transparent *to
itself*: an occluded window shows through the one in front of it, and the backdrop is weighted twice
where they overlap. Both artefacts vanish at `g = 0` and `g = 1`, which is precisely why this
survives review — it is correct in every screenshot anyone thinks to take.

The arithmetic, in an overlap region where the front node `a` and the occluded node `b` are both
fully covering, over a backdrop `D`, at group opacity `g`:

```
	flattened     g·a + (1−g)·D                        a alone, blended once
	per-node      g·a + g(1−g)·b + (1−g)²·D            b leaks in
	error         g(1−g)·(b − D)                       maximal at g = ½
```

So at the midpoint of every fade, a quarter of the difference between the hidden window and the
backdrop is added to the picture. It is not subtle, it is not confined to translucent content, and
it gets worse rather than better as the content becomes more ordinary — two opaque overlapping
windows is the common case, not the exotic one.

**Reduced motion is what makes this unavoidable rather than occasional.**
[Decision 13](#13-a-closed-motion-vocabulary-with-runtime-configuration-exposing-only-that-vocabulary)
replaces movement with cross-fades and drops parallax and scale entirely, so on the accessibility
path *every* transition in the system is a group fade. The same shape as
[decision 48](#48-linear-blending-is-a-visible-ecosystem-change-and-gyro-takes-it)'s luminance dip,
which lands in the same place for the same reason: the path taken by the people least able to
tolerate a defect is the path with the most fades on it. Workspace switching, overview dismissal, a
shell restart, and the greeter cross-fade in
[decision 43](#43-lock-and-greeter-are-one-ui-locking-is-an-output-reassignment) are the everyday
cases underneath that.

*(Corrected 2026-08-17.)* "*Every* transition" overstates it, and the correction is worth making
because it is the sentence someone would cost this against. [Decision
71](#71-reduced-motion-is-three-named-forms-not-a-per-bundle-reduced-table) gives reduced motion a
form that fades nothing, and a single window fading out is one object rather than a group — [exit
pixels](Animation.md#exit-pixels) even hand it a snapshot that is already flat. The claim that
survives is the one that matters here: what the reduced path converts to fades is exactly the
transitions that move whole stacks of windows, so subtree fades stop being occasional. Which bundles
declare a group is [open](Open.md).

**An opacity group is a node property, and it flattens.** The subtree renders to an offscreen at its
screen-space bound, opaque to itself, and is composited once at `g`. The extent is the bound
[decision 29](#29-outputs-are-periodic-real-time-tasks-the-test-allocates-effect-budget) already
keys effect cost on, so the cost enters `C` as an area like everything else, and the storage
reservation follows the discipline of
[decision 46](#46-exit-snapshots-come-from-a-pre-reserved-per-output-atlas-exhaustion-finishes-exits-early)
— reserved at output configuration, never allocated at the moment a transition starts. Only nodes
that declare a group pay, which keeps the ordinary case free.

**Backdrop materials inside a group are resolved rather than forbidden.** `Material::Glass` inside a
flattening subtree samples the composite *as of the group's base* — the scene below the group,
which is already rendered when the group begins. That is the same backdrop it would have sampled
unflattened, so the rule costs nothing and the alternative, prohibiting glass inside anything that
fades, would be unenforceable in a vocabulary a shell composes freely.

**Rejected: per-node alpha, and state the limitation.** This was the position implied by decision 55
until 2026-08-16. It is the cheapest option and the only one with no render target, and it fails the
test [Experience.md](Experience.md#the-picture-is-correct) sets — light behaving like light — at the
moment the system is most watched. A limitation that is invisible in stills and obvious in motion is
the worst kind to admit deliberately.

**Rejected: forbidding overlap inside a fading group.** The catalog could be designed so that no
transition fades a subtree containing overlapping content, which would make per-node alpha exact.
The constraint is not satisfiable: a workspace is a stack of overlapping windows, and fading one out
is the transition this rule would have to forbid first.

**Rejected: a depth buffer.** For the same reason decision 55 rejects it. This is a
compositing-order problem, not a visibility problem, and depth does not touch it.

### 56. Clients render at the ceiling and gyro downscales

A surface's preferred scale is the **maximum** over the outputs it intersects, and a client that
understands only integer `wl_output.scale` is given the ceiling of that. Minification loses detail
gracefully; magnification loses it visibly. The bill is the client's memory and its GPU time, plus a
sampling cost here, and it is accepted deliberately.

**Scale changes are asymmetric.** Raised immediately on any overlap, lowered only once the
manipulation settles and a debounce elapses. The symmetric rule drives a buffer reallocation storm
in the client while a window is dragged along a monitor boundary. Same shape as
[decision 34](#34-effect-quality-is-a-tier-gyro-chooses-and-the-floor-tier-is-the-recovery-path)'s
sticky tiers, and the same shape as
[decision 32](#32-a-surfaces-frame-cadence-follows-its-fastest-output): a surface follows the most
demanding output it is on, in density as in rate.

**This makes mip chains load-bearing rather than optional**, which is the part with a real cost. At
1.25 the ceiling is 2, so a legacy client's buffer is minified by 1.6 on every frame, and without
mip levels that aliases along every high-frequency edge — which is most of a user interface. The
chain is built from the linearised copy
[decision 47](#47-compositing-happens-in-linear-light-at-wide-primaries) already keeps at import,
whose one rule names mipmapping explicitly. Perspective under decision 55 wants anisotropic sampling
on top of it, free where the hardware has it and absent on the software floor tier — where decision
34 has already dropped effects.

**`wp_viewporter` and `wp_fractional_scale_v1` are the main path**, and
`wl_surface.set_buffer_scale` is correct but second-class. It cannot be *required*, since a client
may simply not bind viewporter, so legacy is deprecated by being visibly worse rather than by being
refused. What it buys structurally is that the buffer-to-surface adapter becomes **declared rather
than inferred** — `src` is `wl_fixed` and `dst` is integer, so the mapping is an exact rational per
axis (see decision 52; `src` and `dst` may disagree in aspect ratio) and gyro never derives a
logical size from buffer dimensions and a floating-point scale. Decision 53's rule gets a structural
home instead of remaining a discipline somebody has to remember.

X11 clients have no notion of scale, so Xwayland surfaces live at one scale and are resampled
everywhere else. A visible consequence, recorded rather than discovered.

**Rejected: rounding to the nearest integer scale.** Half of the fractional ladder rounds down, and
rounding down magnifies.

**Rejected: preferred scale from the largest intersecting area.** Looks steadier, and it makes the
majority of a straddling window correct. It flips at the halfway line, which is the middle of a
drag, which is the worst moment available — and it makes the leading edge of a window entering a
denser output the part that looks worst.

**Rejected: compositing at an integer scale and downscaling the result.** Early GNOME's answer. It
double-resamples every client including the ones that did everything right, and it is most of why
fractional scaling has the reputation it has.

**Rejected: refusing clients that do not bind viewporter.** The system layer does not get to
excommunicate toolkits, and ceiling-and-downscale is a real answer for them rather than a
placeholder.

### 64. The scene is instantiable; an instance has identity, a clock, and a permission

*(Revises [decision 19](#19-hierarchical-time-is-a-per-subtree-timescale-only).)*

**"Render this subtree, under this transform, to this target" is one primitive with several
consumers**: an output, a virtual output
([decision 26](#26-remote-presentation-is-a-virtual-output-with-client-supplied-targets)), an
overview thumbnail, a switcher tile, a screen capture, and the flattening offscreen of an opacity
group ([decision 60](#60-group-opacity-requires-flattening-per-node-alpha-is-not-a-group-fade)).
They are the same operation. The reason to say so once is that the alternative — a path per consumer
— is how a live thumbnail quietly becomes a stale one, and how a switcher tile and an overview
thumbnail drift into being two different renderings of one window.

**An instance is an object with identity, not a render parameter.**
[Decision 18](#18-matched-geometry-is-in-the-first-cut)'s matched geometry between a tile and a
thumbnail requires them to be the *same* thing rather than two renderings that agree, because
interruption mid-gesture has to retarget one object. Identity is
[decision 15](#15-identity-is-a-generational-handle)'s generational handle, as everywhere else.

**Each instance samples its own time, and closed-form springs are what make that free.**
[Decision 28](#28-the-frame-clock-is-per-output) gives every output its own clock, so an instance on
a 30 Hz virtual output and one on a 144 Hz panel cannot read a single global animation time — and
that is the everyday configuration rather than an exotic one.
[Decision 11](#11-springs-are-closed-form-not-numerically-integrated) makes evaluating one animation
at two instants exact and nearly free; a numerically integrated spring would have to be stepped once
per instance and would drift between them, which is the same argument decision 11 already won on
different grounds. This *extends* decision 19 rather than reversing it: `TimeScale` remains the only
hierarchical time, and what is per instance is the instant the world is sampled at, not a second
timing model. Decision 19 was right that most of `CAMediaTiming` is redundant once springs retarget;
this is the part of it that turns out to be load-bearing, and it arrives from per-output clocks
rather than from animation.

**Instantiation is the capture primitive, so the permission belongs on it.** A thumbnail, a
screenshot, a recording, and a remote output are one mechanism, and a permission attached to
consumers is bypassed by whichever consumer is added in three years. The primitive takes the
requesting connection's trust tier and the subtree's owning session and refuses across the boundary.
That is what keeps
[decision 43](#43-lock-and-greeter-are-one-ui-locking-is-an-output-reassignment)'s locking a
property of the system rather than a rule the system tries to enforce — that phrasing is
[Experience.md](Experience.md#the-machine-holds-several-people)'s, and this primitive is where it is
either true or merely intended.

**Cost enters admission control per instance.**
[Decision 29](#29-outputs-are-periodic-real-time-tasks-the-test-allocates-effect-budget) keys `C` on
area and an instance has one, so no new machinery is needed — but the *count* is a new axis, and
overview entry instantiates a subtree per window in the frame least able to afford it. Decision 34's
cut point at commit time is already the right shape, because
[decision 14](#14-declarative-commits-with-dirty-tracking) means gyro knows the instance set before
it draws.

**Damage crosses instances.** One surface changing dirties every instance containing it, each in its
own space, and the mapped bound is
[decision 55](#55-transforms-are-3d-the-scene-is-a-painters-algorithm)'s projected quad under
[decision 63](#63-effects-declare-their-kind-and-their-damage-the-verifier-keeps-them-honest)'s
expansion. This is the cost the primitive actually carries, and it is why an off-screen thumbnail of
a playing video is not free.

**Rejected: snapshot-backed overviews.** Cheaper, steadier, and it makes the overview a picture of
the desktop rather than the desktop. A window that stops updating when it becomes a thumbnail is a
cross-fade wearing a transform, which is decision 18's failure mode arriving by another route.
[Decision 20](#20-exit-animations-use-full-resolution-snapshots)'s snapshots are for content that no
longer exists; a live window is not that.

**Rejected: a path per consumer**, which is the ordinary arrangement — thumbnails, capture, and
remote presentation each with their own renderer. It is how the primitive would emerge anyway, three
times, with three answers to instance timing and three places to forget the permission check.

**Rejected: instances without identity** — a transform passed to a render call, nothing retained. It
is the cheapest version and it is sufficient for capture and for thumbnails taken one at a time. It
cannot express two presentations of one window being the same object, so matched geometry between
them is not statable, and decision 18 is in the first cut precisely so that this is not discovered
later.

**Cost accepted:** instance count becomes a dimension of the budget and of
[decision 46](#46-exit-snapshots-come-from-a-pre-reserved-per-output-atlas-exhaustion-finishes-exits-early)'s
atlas sizing, and instances acquire lifetime rules that nothing previously needed.

**Prior art.** `CAReplicatorLayer` is this primitive exposed publicly, and macOS's Exposé has shown
live windows rather than snapshots since 2003; Compiz's `paintOutput` took a transform for the same
reason. It is the least-copied of the three ideas recorded here and the one with the longest
uninterrupted track record.

**The instance is a node.** *(Revised 2026-08-22.)*
[Decision 88](#88-an-instance-is-a-node-the-published-scene-is-a-dag) keeps this decision's claim —
one primitive with several consumers, and identity on it rather than a render parameter — and
changes what the primitive is made of: a reference node in
[decision 86](#86-the-published-scene-is-a-preorder-tree-model-values-inline-coefficients-by-reference)'s
published tree, expanded inline by the frame thread, rather than a retained object rendering into a
target of its own. Two of the six consumers listed above had already left the list by the time it
was read — [decision 82](#82-the-renderer-is-handed-an-evaluated-draw-list-not-a-scene) made a
group's flattening offscreen a draw item, and decision 26 makes a virtual output an output — and
what remained composites into the target of whatever references it.

Three paragraphs above narrow with it. *Each instance samples its own time* becomes the host
output's presentation instant, which is [decision 28](#28-the-frame-clock-is-per-output) and needs
nothing further. *Cost enters admission control per instance* holds in its first half and the count
is **withdrawn** as a new axis: a reference node is area, which `Admit` already prices. And the
permission is checked dispatch-side at commit, where the reference is authored, rather than by the
side that draws — which is where it was always going to land, since that is the side that holds the
tree.

### 67. The settled snap is unconditional

[Decision 54](#54-settled-geometry-snaps-to-the-outputs-device-grid) left open whether the snap
applies to a node that is not sampling one-to-one. It does. Settled geometry snaps to the grid of
the output being evaluated for, whatever its content is doing.

The question reads as *a resampled node gains nothing, so why move it*, and that is true of the
node's **content** and false of everything else about it. Three things about a node are geometry
gyro draws rather than pixels it samples, and all three are one-to-one no matter what the interior
does:

- **The node's own edge.** A rect whose boundary lands at a non-integer device position has
  partially covered pixels along its entire perimeter, so the composite blends it against whatever
  is behind it. On a dark desktop under a light window that is a visible halo.
- **Decorations.** [Decision
  48](#48-linear-blending-is-a-visible-ecosystem-change-and-gyro-takes-it) pushes gyro toward
  drawing borders and shadows itself, and their crispness is entirely a function of the node rect
  being on the grid.
- **Adjacency.** Two rects abut exactly when both are on the grid, and not otherwise.

Adjacency decides it, because [Experience.md](Experience.md#the-picture-is-correct) states the
tiling promise unconditionally — *two tiled windows meet with no line of background showing between
them* — and a rule covering only one-to-one nodes cannot keep it. The configure remainder does not
rescue that: [decision 52](#52-coordinate-spaces-are-three-and-quantization-belongs-to-the-output)
absorbs the remainder into gyro's own gap and thereby governs device *extent*, while abutment
additionally requires the *position* to be on the grid — and positions arrive continuously, from a
drag released, a window animated into place, or a popup anchored to a control.

The case that settles it is ordinary rather than exotic. A 1.5× panel with two windows tiled side by
side, both Xwayland or integer-scale-only clients: [decision
56](#56-clients-render-at-the-ceiling-and-gyro-downscales) has them render at 2× and be minified to
0.75, so neither samples one-to-one, so under the narrower rule neither snaps, their shared boundary
lands at an arbitrary sub-device-pixel offset, and a hairline of wallpaper shows between two windows
that are supposed to be touching.

The converse costs nothing. An overview thumbnail settled at scale 0.25 is snapped, gains nothing
for its interior, loses nothing, and has a crisp edge. A node settled at a rotation that is not a
multiple of ninety degrees gets a snap that is meaningless and harmless.

**Consequence: the transform classification predicate is not a placement input.** The resample-once
rule needs to know whether a node samples one-to-one; the snap does not ask. The predicate is
diagnostic, and it drives plane promotion and effect quality. Wiring it into the snap path is how
the second rule gets back in, so the type that answers it says as much.

**Rejected: snapping only nodes that sample one-to-one.** Snap exactly where it buys crispness and
skip where it cannot. It is the more precise-sounding rule, it fails the tiling promise on the most
common fractional configuration there is, and it makes the settle path branch on a classification it
otherwise has no reason to consult.

**Rejected: "it keeps one rule instead of two."** The argument this decision was expected to rest
on, and too weak to carry it — tidiness is not a reason to move a window. Recorded because the real
argument was found only by asking what the snap buys a node whose content cannot benefit, and the
answer turned out to be *everything except the content*.

### 68. A subsurface snaps like any other settled node

`wl_subsurface.set_position` is integer surface-local, so a subsurface cannot be device-aligned on a
fractional output whatever gyro does. Decision 52 records that the limitation is the protocol's;
what was open is whether gyro compounds it by rounding a second time. It does not compound it —
because the misalignment is not gyro's to introduce. It is already there, inside the client, before
gyro is involved.

Work the arithmetic on the ordinary case: a 1.5× output, a client that speaks
`wp_fractional_scale_v1` correctly, a video player drawing a frame around a video in a subsurface at
surface-local offset (1, 1).

- The client renders its parent buffer at 1.5×, so it rasterizes the hole for the video at buffer
  texel `round(1 × 1.5) = 2`. Cairo and Skia both round to device pixels, and neither can draw a
  hole at texel 1.5. - The client declares the subsurface position as the integer `1`, because that
  is all `set_position` can express. In device space that is 1.5.

**So the client has put its hole at device offset 2 and declared its child at 1.5.** The half-pixel
disagreement exists in the client's own output before gyro touches anything, which means the
candidates are not *exact versus approximate*. They are *which of the client's two contradictory
statements do we believe*, and snapping believes the rasterization — which is where the pixels
actually are.

**Rejected: leaving the sub-pixel offset in place.** Believes the declaration, places the child at
1.5, and lands it half a pixel from the hole the client drew, so the result is a soft child *and* a
hairline at its boundary. It was the intuitive answer and it is the worst of the three, which is
worth recording: it is what *do not introduce error* looks like when the error is already there.

**Rejected: absorbing the remainder** the way decision 52 absorbs the configure remainder. There is
no gap to absorb into, since a subsurface sits on its parent rather than beside it. The only
coherent reading is choosing the *parent's* snap phase to favour its child, which trades the
parent's crispness for the child's, cannot satisfy two subsurfaces at once, and needs an ordering
rule that changes whenever a client attaches a differently scaled buffer. Three rules to serve a
case that one rule already serves.

**Rejected: ceiling the child's extent outward** so that a gap becomes an invisible overlap instead.
It sounds free and it is not: ceiling the extent moves the child's sampling ratio off exactly one,
so it resamples a child that was about to be crisp and defeats the thing the snap was for. Snap the
position; leave the extent exact.

**This rests on a claim about other people's code**, so it is worth being explicit that it is a bet
rather than a proof. It assumes clients round their internal geometry to device pixels, at the scale
they were told, to nearest. That is true of Cairo and Skia and of anything doing ordinary
fractional-scale layout, and it is not guaranteed. A client that floored internally would put its
hole at texel 1 while gyro snapped the child to 2 — a full device pixel of disagreement, and worse
than leaving the offset alone. It is measurable rather than arguable, and the measurement is carried
in [Open.md](Open.md).

### 85. The headless backend is portable, and the instrument is the reason

*(Decided 2026-08-21, on building `Headless` against the loop that had just landed.
[Structure.md](Structure.md#the-modules)'s table had already placed it, and placing it turned out to
have been an assumption rather than a reading.)*

**`Headless` is in the portable tier: ISO C++ and POSIX, enrolled in
`CMake/CheckPortability.cmake` beside `Core`, `Geometry`, `Animation`, `Publication`, `Seam`, and
`Frame`.** The table had it as platform code alongside `Nested` and `Drm`, on the entirely reasonable
ground that a backend is where the platform lives. Written out, it has no platform in it. A simulated
panel is arithmetic over [Core/Time.h](../Source/Core/Time.h) — a phase, a period, and which vblank a
commit at a given instant makes. Its images are heap pages rather than dumb buffers, so
`RenderTarget`'s `MappedImage` is satisfied by an allocation. And the one place a backend would
ordinarily reach for a Linux header is a descriptor to wake on, which
[Seam/EventSource.h](../Source/Seam/EventSource.h) had already settled in the other direction: a
headless source reports an *invalid* descriptor, because its flips are a function of the clock the
test drives and no file becomes readable when one falls due. There is nothing to open.

**The tier is not a label on that observation; it is what holds it.** `Headless` is the instrument
[decision 29](#29-outputs-are-periodic-real-time-tasks-the-test-allocates-effect-budget)'s
schedulability claim is falsified with — the fake clock that takes arbitrary rates and *phases*, the
deliberately injected miss, the panel that does not run at its nominal rate — and
[Structure.md](Structure.md#frame-is-portable) already turns on that sweep running "on a machine with
no GPU, no seat, and no compositor". A module that merely *happens* to be portable acquires a platform
dependency the first time one is convenient, and the failure is silent: the sweep stops running in the
environment it was built for, and nobody notices until it is the environment CI has. Declaring the
tier makes that a build failure naming the include.

**Rejected: platform, for consistency with `Nested` and `Drm`.** The row would read more evenly and
the check exists to catch a dependency nobody meant to add — so declining to enrol the one module
whose entire job is to run where there is no hardware gives up the rule exactly where it pays. The two
neighbours are platform because they *are*: a Wayland connection and a DRM file. Sharing a table row
is not sharing a reason.

**Rejected: a self-notification descriptor, which would have decided the tier by itself.** The
question was `eventfd` against `pipe` — one fd against two, kernel-side coalescing against a drain
that reads to empty, a counter that cannot fill against a 64 KiB buffer that can, and `pipe2`'s flags
being Linux's while `pipe`'s are POSIX's. It is moot here and stays decided where it actually lands:
[decision 83](#83-dispatchs-publication-is-an-event-source) has dispatch's nudge as an `eventfd`, in a
module that is platform code for other reasons. Headless needs neither, and reaching for one to look
like the other backends would have bought the platform tier for a descriptor nothing waits on.

**The simulated renderer lives here too, and it is the same argument one seam over.** The sweep needs
"a null renderer that merely charges a simulated `C`", and the obvious home is `Render` — which is
platform, holds Vulkan, and would drag the tier back. It is not a Vulkan device with the drawing
removed; it is the second thing headless simulates, and it belongs beside the panel. What it produces
is durations, which is the whole of what decision 29's test consumes, and its `SyncPoint`s are
immediate because [Seam/SyncPoint.h](../Source/Seam/SyncPoint.h)'s null point already means *finished
on the CPU before the call* — a fabricated descriptor number would be a lie a presenter written to
honour it could hand to a real syscall.

**What would overturn this, and what it would cost.** Two things headless is expected to grow reach
for the platform: frame dumps that want a mapping rather than a copy, and scripted input shaped like
what `libinput` delivers. The first is additive and can stay a copy; the second is dispatch-side and
arrives with the input seam, at which point the module gains a `DISPATCH_HALF` and the question is
asked again — for that half, and not for the panel. Either way the check reports it as a build failure
with the include named, which is the whole reason to declare the tier while the answer is still cheap.

### 88. An instance is a node; the published scene is a DAG

*(Decided 2026-08-22. Revises [decision 64](#64-the-scene-is-instantiable-an-instance-has-identity-a-clock-and-a-permission),
whose claim survives and whose mechanism does not.)*

**A node may reference another subtree, and the frame thread expands the reference inline during its
preorder walk.** Decision 64's primitive — *render this subtree, under this transform, to this target*
— stays one primitive with one identity. What changes is that it is a node in
[decision 86](#86-the-published-scene-is-a-preorder-tree-model-values-inline-coefficients-by-reference)'s
published tree rather than a retained object with a target of its own, so nothing new is enumerated,
`EvaluateRequest` is unchanged, and decision 84's per-output indexing is untouched.

**Two of decision 64's six consumers had already left the list, and noticing that is most of the
argument.** Decision 82 made a group's flattening offscreen a `DrawGroup` in the draw list, managed by
the renderer. And
[decision 26](#26-remote-presentation-is-a-virtual-output-with-client-supplied-targets) makes a
virtual output an *output* — its own frame clock, its own damage, its own row in the admission set —
which covers remote presentation and the screen recorder wanting per-output frames alike. What was
left needing a mechanism is the overview thumbnail, the switcher tile, and the one-shot still.

**The transition it has to serve is window-to-thumbnail, and a reference node is the only shape that
resamples it once.** [Decision 56](#56-clients-render-at-the-ceiling-and-gyro-downscales) and
[decision 52](#52-coordinate-spaces-are-three-and-quantization-belongs-to-the-output) between them
forbid resampling an already-resampled image. Under a reference node the transform chain composes and
the client's buffer is sampled once at whatever scale the moment requires, with the mip chain off the
import path where [Animation.md](Animation.md#matched-geometry) already puts it. Under an instance
with its own target, the subtree renders at the target's size and is then drawn at the animating size
— two resamples for the length of the transition, on
[decision 18](#18-matched-geometry-is-in-the-first-cut)'s worked example and
[Experience.md](Experience.md#one-hand-made-all-of-it)'s most-watched motion. It is the failure the
[two-output atlas](Open.md) entry describes, reached through storage rather than through geometry.

**Identity comes out stronger than the mechanism it replaces.** A referenced subtree is *literally the
same nodes with the same springs*, so a switcher tile and an overview thumbnail are one object by
construction rather than two the system keeps in agreement — which is what
[decision 15](#15-identity-is-a-generational-handle) was wanted for here, obtained without a second
kind of thing to have identity.

**It keeps the schedule and the fold where they are.** Instance count would have been a new axis of
[decision 29](#29-outputs-are-periodic-real-time-tasks-the-test-allocates-effect-budget)'s admission
set, and decision 64 said so; as a node it is area, which `Admit` already prices, on the frame
decision 64 correctly named as the one least able to afford a surprise. A reference node's springs are
the host output's springs, so decision 69's per-output fold covers it unchanged instead of
partitioning per instance — and every instance that could arm its own wake is another way to fail
[doing nothing costs nothing](Experience.md#doing-nothing-costs-nothing).

**And it keeps decision 84 from firing on the transition it must not disturb.** An instance run would
have made the header's generation an *instance*-set generation, and thirty thumbnails coming into
being renumbers the set at overview entry — where the guard's correct behaviour is to contribute
nothing for an iteration. That is a dropped frame at the start of the most demanding transition in the
system, against [every frame](Experience.md#every-frame). Avoiding it means matching instances by
handle rather than indexing them, which is decision 84's rejected alternative; 84 named the condition
for reopening it — *if the run ever needs to be sparse* — and a volatile instance set is that
condition. Better not to create it.

**Two obligations fall on `Scene`, and they are the price.** A reference cycle is an unbounded
traversal inside the frame section and the frame thread cannot afford to detect one, so **acyclicity
and a bounded reference depth are guaranteed dispatch-side**, at commit, like every other invariant
the authoring side owes the frame side. And **a reference is not a second identity**: the referenced
subtree keeps its own `EntityId`s and the reference node has its own for its own transform, or matched
geometry has two objects to reconcile and the property above is given away again.

**The frame thread checks anyway.** *(Revised 2026-08-22.)* The obligation above stands and stops
being the only thing between a dispatch-side bug and the machine.
[Decision 90](#90-the-snapshots-runs-are-one-per-channel-and-the-frame-side-validates-the-tree-it-walks)
has the walk carry a depth counter and check each subtree length against what remains — one comparison
per node, against an unbounded traversal at `SCHED_FIFO` in the only compositor the machine has, whose
survivable outcome is `RLIMIT_RTTIME` killing every session's UI at once. *The frame thread cannot
afford to detect one* was read as a reason to trust the producer; it is a reason to bound the work,
which is cheaper than detection and does not require trust.

**Rejected: an instance run with per-instance targets and clocks** — decision 64 read literally. It is
the more general mechanism and every part of the generality costs something here: a pool of targets
reserved at configuration, because `BindTargets` allocates and may not run in the frame section; an
eviction policy over that pool, whose visible form is a thumbnail showing a frame from a moment ago,
which is decision 64's own rejected snapshot-backed overview arriving through the back door under
exactly the load where somebody is looking at it; and a dependency order among instances inside one
iteration. It stays available for anything that genuinely needs its own clock *and* its own target,
and the still capture is where it would first arrive.

**Rejected: rendering a thumbnail once and sharing it across outputs**, which is the efficiency
argument for the instance run and the reason it looks like the scalable answer. Two outputs showing an
overview are at two densities, so the shared render is the one that would be *wrong* — the same
reasoning the two-output atlas entry already carries. The sharing on offer is sharing that would have
to be refused.

**What would overturn this.** A thumbnail that must run at a rate other than the output it is drawn
on. A reference node inherits its host's clock by construction and cannot express that, and the
promotion path — turning one into a target-owning instance under load — would be a visible rate change
in something being watched, which
[quality does not visibly fluctuate](Experience.md#the-picture-is-correct) is close enough to forbid.
Nothing in the design wants it today, and it is the load-bearing assumption rather than a detail.

### 92. The transform chain composes into one matrix; the perspective clamp was the artefact it guarded against

*(Decided 2026-08-22, on trying to produce a `Quad`.
[Decision 86](#86-the-published-scene-is-a-preorder-tree-model-values-inline-coefficients-by-reference)
says the frame side's walk carries "an explicit transform stack" and nothing said what was in it.
Corrects a claim this log made in [decision 55](#55-transforms-are-3d-the-scene-is-a-painters-algorithm)'s
implementation and a citation of [decision 29](#29-outputs-are-periodic-real-time-tasks-the-test-allocates-effect-budget).)*

**The stack holds one composed 4x4 per level, and the accumulated weight is its W component.** Not the
ancestors' `NodeTransform`s with each node's corners pushed up through all of them — that reading was
the obvious one and it is four walks of the ancestor chain per node, on the arithmetic that runs for
every node of every frame. One matrix multiply on push, four matrix-vector products per node, constant
in nesting depth. [Seam/Renderer.h](../Source/Seam/Renderer.h)'s "accumulated divisor for that corner,
the product along the chain" needs no accumulating: it falls out of the bottom row.

**Why the chain folds at all is not obvious, and the reason it was missed is worth keeping.**
`NodeTransform::Apply` divides by the perspective weight *before* handing the point to the parent, so
the parent rotates an already-divided point and this is not matrix composition on its face. It works
because each node's weight is affine in the point that node is handed — the projection is the ordinary
homogeneous one with `1/d` in the bottom row — so the divides telescope. Checked numerically over a
four-deep chain projecting at three levels before it was built on: composed and hand-walked agree to
6e-8 on position and 1.3e-8 on weight, which is float rounding.

**The one thing that could have broken it was the clamp, and the clamp was also the bug.**
`Perspective::Weight` saturated the depth against the node's bounding radius, on a stated contract
that `|depth| <= boundingRadius` and that a caller breaking it "has handed over a radius belonging to
some other node." The walk breaks it as ordinary business: an ancestor's radius is computed from the
ancestor's own extent and anchor, and a descendant's corner lands where it lands. **A piecewise weight
is not a matrix at any depth**, so removing the clamp is what makes the walk constant per node — the
correctness fix and the cost fix are one edit.

Two things it did on screen, and the second is the artefact the weights exist to remove:

- **A window pulled out of a workspace stopped growing partway through.** Past the *workspace's*
  radius — a number with nothing to do with the window — the weight froze while the translation kept
  running. It reads as the window hitting a pane of glass with the animation still visibly in flight.
- **A quad with some corners saturated and some not stopped being a projective image of a rectangle**,
  so its content swam across it as it turned. Measured at 7% relative against 3e-7 unclamped. That is
  precisely the warp `Quad::Weights` was introduced to prevent, reintroduced by the guard next to it.

Neither is an edge case, because **with no camera a container is the only thing that can give its
children a shared vanishing point.** Nine workspace cards each carrying their own node-local
perspective each recede about their own centre, which reads as a collage rather than as a space. So
the overview — the reason decision 55 wanted three dimensions — puts the projection on an ancestor and
makes every window in it a descendant, and the descendant case was the one that did not work.

**The eye is resolved once per node rather than per point.** `Perspective::InverseEyeDistance(radius)`
is `strength / radius`, computed where the radius is unambiguously the node's own, and every point
above it costs a multiply-add instead of a divide. It also makes the mismatch unspellable: after the
push there is no radius left to pair with the wrong depth.

**The only cull is the geometric one.** A weight at or below zero means the point has passed through
the eye of some node above it and is behind the viewer. That is unrenderable in the same way a back
face is, and decision 55 already culls those unconditionally with no per-node override, so this joins
them rather than inventing a policy. The floor sits at a magnification of 1024 — four orders of
magnitude past anything the motion catalog could author — and exists so the *shape* of a mistake is a
disappearance rather than a NaN in a damage bound, which spreads to everything the output composites.

**Rejected: a magnification budget as the cull threshold**, on the reasoning that removing the clamp
removed the bound `MinimumRadii` claimed to provide. That claim cited decision 29 in the form decision
29 rejects. `C` is a budget gyro *enforces*, not a cost it observes — the 2026-08-15 revision — so
nothing consumes a per-node projected-area bound and nothing ever did; `grep -r Area Source/` returns
nothing. The bound was also already false, since per-node weights in `[0.5, 1.5]` multiply along a
chain. Four times magnification was the value under discussion and is the worst available: a tilted
deck holding a flipping card reaches it legitimately, so it would fire on real content, which is the
same character as the clamp it replaced. It is also out of key with
[decision 30](#30-budget-shortfalls-are-answered-by-spending-less-chunking-and-early-rendering-are-contingencies)
— a shortfall is answered by spending less, and `Admit()` returns a degraded plan and never a refusal.
A window vanishing because it grew is a refusal.

**Rejected: a subtree bounding radius**, so every descendant lies inside the radius its ancestor's
projection is stated against. It is the exact fix and it makes a parent's foreshortening a function of
its children: dragging a window inside an overview would visibly change the overview's shape.

**Rejected: near-plane clipping**, which is what a general 3D pipeline does with this. It turns a quad
into a three-to-five-gon and costs
[decision 82](#82-the-renderer-is-handed-an-evaluated-draw-list-not-a-scene)'s flat draw list its four
corners, to rescue geometry the viewer is standing behind.

**Rejected: storing the matrix on the node.** Unchanged from
[decision 17](#17-transforms-are-decomposed-into-trs-with-per-channel-springs) — a stored matrix is one
somebody eventually interpolates, and lerping two of them shears the result through configurations
that are not rotations. `ComposedTransform` is composed on the render path and holds nothing between
frames, which is what `Apply`'s own comment always asked for.

**A correction to something claimed while reaching this.** The four-corners-times-depth reading was
offered as a second reason for
[decision 90](#90-the-snapshots-runs-are-one-per-channel-and-the-frame-side-validates-the-tree-it-walks)'s
depth cap, one that was not "don't hang the machine." It does not survive: the walk is constant per
node, so the cap gains nothing here. What it bounds instead is the stack itself, at 128 bytes per
level inside a section that may not allocate.

**What the tests had to be taught.** The first versions of all three passed against the clamp,
because a window at rest sits inside its workspace's own extent and never reaches the saturation —
the defect exists only during the gesture, which is exactly when someone is looking at the window
closely. They now sweep a lifted window and assert through the walk rather than through the matrix,
since the walk is what the transforms *mean* and a matrix built over a projection that was not
projective would satisfy a test of its own internal consistency while the screen disagreed.
### 94. A frame's cost has a part no tier reduces, and the walk is it

*(Decided 2026-08-22, on asking where the real evaluator's time goes before writing it.
[Decision 82](#82-the-renderer-is-handed-an-evaluated-draw-list-not-a-scene) put a walk on the frame
thread, [decision 86](#86-the-published-scene-is-a-preorder-tree-model-values-inline-coefficients-by-reference)
gave it something to walk, and decision 93 builds it; none of the three says what it costs.)*

**`Budget` carries a third figure on the CPU device, and `Timing` adds it to both tiers.**
[Budget.h](../Source/Frame/Budget.h) held exactly two terms — the CPU record cost a renderer reports
and the GPU execution cost it resolves late — and
[decision 35](#35-a-miss-costs-one-frame-bounded-by-the-floor-composite)'s check composes them into a
verdict. Between that verdict and the record, the frame thread turns a snapshot into a draw list.
[Loop.h](../Source/Frame/Loop.h) calls it, nothing times it, and while `NullEvaluator` is what is
bound that is exactly right. The moment a real evaluator is bound it stops being right, and the
failure is silent: the check admits a frame on figures that describe only the composite, the walk
runs anyway, and the frame lands late with no term in the model that moved.

**It is not a bigger CPU figure, because the ladder cannot step it down.**
[Decision 34](#34-effect-quality-is-a-tier-gyro-chooses-and-the-floor-tier-is-the-recovery-path)'s tiers and decision 35's
floor composite reduce what a frame *draws*. They do not reduce what it walks: the floor tier is a
cheaper shader over the same items, so the same tree is traversed and the same springs are evaluated
whichever verdict came back. Folding the walk into `FloorCpu` would therefore make the floor target
scene-dependent — and that target is set at configuration change, before the session has the windows
that make the walk expensive, so it is the one figure in the record that cannot be allowed to depend
on the scene. Decision 35's second promise, that an overrun costs exactly one frame, rests on the
floor composite being a cost the machine can always afford; a floor whose real cost grows as the
user opens windows is not that.

Stated as a property rather than as a name, the axis is **what varies with mode**. Pixels and effects
size the two figures that were already there; how many nodes exist sizes this one. Adding it to both
tiers is what makes it irreducible in the only sense the schedule can act on.

**The evaluator measures itself, and the measurement comes back in the `DrawList`.** That is
`Submission::RecordCost` one step earlier and for the same reason
[Seam/Renderer.h](../Source/Seam/Renderer.h) gives: the party that knows where the work started and
stopped is the party that did it. The alternative is the loop bracketing the call with two clock
reads per output, where [Core/Clock.h](../Source/Core/Clock.h) asks for one per iteration — and a
frame thread that reads the timebase per output is the shape decision 57 exists to prevent, arriving
in the one part of the loop the schedulability sweep drives hardest.

**Measured and windowed, like the planned marks and unlike the floor's.** A figure that sizes a
reservation must forget, or a workspace that was crowded once reserves for the crowd forever; a
figure whose job is to contradict a target must not, which is why `MeasuredFloorCpu` accumulates
differently. This one sizes a reservation. It seeds from `InitialIrreducibleCpu` and clears on
`Invalidate()`, since a mode set changes the grid every node is projected onto and the last walk
described a different frame.

**Rejected: restructuring the two mode figures into a fixed part plus a reducible part**, which is
the tidier shape and is what the paragraphs above sound like they are arguing for. It cannot be
measured. [Decision 62](#62-effect-composition-is-an-optimization-and-the-unfused-path-is-the-reference)'s fusion means a planned
composite contains no separable base-composite span, so the reducible part is obtainable only by
subtracting the floor mark from the planned one — two high-water marks taken over different
populations at different times, whose difference can come out negative. The record currently never
subtracts, and that is worth more than the symmetry.

**Rejected: charging the walk to whichever mode followed it**, the one-line version that needs no new
field. It files one population into two, and it fails in the direction that compounds: a machine in
trouble renders the floor tier often, so the walk's cost would go mostly into a population nothing
sizes with, while the planned mark came to describe frames that never had to build their own list.
It is the same objection this record already makes to letting floor frames size the planned mark.

**Rejected: a symmetric GPU term.** Nothing irreducible occupies the GPU today — the walk is pure
CPU work and there is no second device between the snapshot and the record. The field can arrive
when something needs it, and a field with no writer is a field whose meaning is decided by whoever
first guesses at it.

**Consequences.** `Timing::Cpu` is a sum where it was a selection, so `Reserve`, `Finish`, and
`Project` all carry the term without knowing it exists. `BudgetPolicy` gains a third `// SPEC:`
number, and [Open.md](Open.md)'s *the capability probe* is where it lands — as the one figure that
probe can only partly supply, since it measures the machine and this one belongs to the scene. A
probe can seed an empty session; everything after that is measurement. The
schedulability sweep gains a real number here the day the real evaluator is bound, which is the
first figure in that sweep that is not one a test chose.

### 93. The quad is assembled in `Frame`, and the back face is the signed area

*(Decided 2026-08-22, producing the `Quad` that
[decision 92](#92-the-transform-chain-composes-into-one-matrix-the-perspective-clamp-was-the-artefact-it-guarded-against)
composed the chain for. Amends a sentence in
[decision 86](#86-the-published-scene-is-a-preorder-tree-model-values-inline-coefficients-by-reference)
and one in [Seam/Renderer.h](../Source/Seam/Renderer.h).)*

**Turning a composed chain into four corners lives in `Frame`, as
[Projection.h](../Source/Frame/Projection.h).** `Quad` is in `Seam` and `ComposedTransform` is in
`Geometry`, and `Geometry` may not say `Seam`, so it cannot sit with either type it joins. `Seam` is
wrong for it on that waist's own terms — "every interface with more than one implementation and the
data crossing it" — and this is one caller, one implementation, and no backend on the far end.
[Decision 82](#82-the-renderer-is-handed-an-evaluated-draw-list-not-a-scene) already has `Frame`
building the draw list into its own arena; this is the arithmetic that fills one item of it.

**The back-face test is the sign of the projected quad's area, not `NodeTransform::FacesViewer`.**
Decision 86 said back faces go by that predicate "over the composed chain", and there is no composed
chain to ask: it reads one node's rotation and scale signs, and **the answers do not multiply**. Two
nodes each turned eighty degrees about the same axis both face front; their composition, at a hundred
and sixty, does not. The shoelace sum over the four corners already computed is six multiplies, needs
no transform, and accounts for the perspective as well — which a decomposed answer cannot see at all.
The predicate stays as the one-node statement of decision 55's rule and has no caller.

**A mirrored output is a configuration and not a screen of back faces.** The output adapter carries
[decision 52](#52-coordinate-spaces-are-three-and-quantization-belongs-to-the-output)'s eight
orientations and four of them are reflections, so on a mirrored panel every quad's signed area is
negative and a fixed comparison would cull the entire desktop rather than the two or three nodes
anybody meant. `OutputView` reads the sign a front face carries off the seed matrix's own
determinant, which also keeps the fact from coming loose from the transform it describes. What the
correction is *not* is a blanket flip: a node with a negative scale on one axis is still showing its
back on a mirrored output, because the two mirrors cancelling is exactly the reading decision 55
refuses.

**The output's origin folds inside the matrix, not into the corners.** The walk is seeded with
`ComposedTransform::ForView(adapter)`, so the first push subtracts the output's origin from a global
translation once, in double. Projecting to global and converting four corners instead loses 1/256 of
a logical pixel each at the far edge of a large desk, which is precisely `wl_fixed`'s resolution —
so a client's stated subpixel position would not survive the trip, on the arrangement
[Geometry/Space.h](../Source/Geometry/Space.h) makes global space double for. It is
[Seam/Renderer.h](../Source/Seam/Renderer.h)'s "the precision at which translation folds against the
output origin belongs to the render path", made a property of the seed rather than a subtraction
somebody remembers to write.

**All three culls are all-or-nothing, and absence is the return value.** A corner behind an
ancestor's eye takes the node (decision 92 rejects near-plane clipping); a back face takes the node
(decision 55, no per-node override); a quad that misses the target takes the node. `Project` returns
`std::optional<Quad>`, because a quad beside a separate flag is a quad a caller can emit without
consulting the flag.

**The placement arrives as the adapter that already exists**, `AxisTransform<GlobalSpace,
DeviceSpace>` — decision 52's output adapter, "global onto an output's device grid, from output
configuration". No output-layout record was invented for it: what an output *is* moves at hotplug
rate and is dispatch-side, which is
[decision 87](#87-a-type-both-halves-of-the-world-name-lives-below-both-waists-not-in-seam)'s axis
exactly. `OutputConfiguration` supplies the other half, `Resolution`, and still deliberately carries
no scale.

**Rejected: the target cull left to the caller.** It is one rectangle overlap and it looked like the
walk's business rather than the quad's. But [decision 82](#82-the-renderer-is-handed-an-evaluated-draw-list-not-a-scene)'s
`DrawItem` states the three culls as one contract — "back faces are culled, and anything entirely
outside the target is gone" — and splitting them across two places is how a caller comes to apply
two of the three.

**Rejected: rounding the bound outward before testing it.** `Quad::PixelBounds` exists and is the
obvious thing to reach for. Its outward rounding is for damage, where keeping too much is the safe
direction; here it would keep a quad that misses the target by a third of a pixel, which is the
opposite. The exact real bound is what the test wants, and `Quad::Bounds` is already the unrounded
one for this reason.

**Rejected: carrying the view's handedness as a parameter.** One `bool` at the call site, passed by
whoever built the seed. It is the same fact in two places and the disagreement is silent — a screen
with nothing on it, on the one configuration nobody has to hand to notice.

**Left open: `DrawItem::Sampling`.** [Seam/Renderer.h](../Source/Seam/Renderer.h) insists the
producer derives the `TransformClass` because a renderer cannot recover it from four floats, and that
is still owed. Nothing here forecloses it: the reduction reads the *chain*, which `Project` takes by
reference and does not consume, and it needs the surface adapter that does not exist yet. It is a
function beside this one when there is something to reduce.

### 97. An output's placement is published; the mode's half of the view meets it in the walk

*(Decided 2026-08-22, on writing the evaluator and finding it had no way to know where an output
sits. [Frame/Projection.h](../Source/Frame/Projection.h) took the composed adapter and said the
carrier was unsettled; this settles it.)*

**The placement crosses in the snapshot, as one adapter per output beside the wake schedule.**
[Decision 52](#52-coordinate-spaces-are-three-and-quantization-belongs-to-the-output)'s output
adapter — global space onto one output's device grid — is authored by the same side that authors the
scene and read by the same side that walks it, so it travels the way everything else in that
direction travels. [Decision 84](#84-the-snapshots-per-output-run-is-indexed-under-a-set-generation)
governs it unchanged: it crosses positionally, and a run whose length is not the output set's is no
information rather than partial information.

**Rejected: `OutputConfiguration`.** It is the obvious home — the mode is already there, and an
output's placement feels like part of how it is configured. It is the wrong one twice over. That
record is what the *hardware is programmed to*, and it travels through
[decision 73](#73-the-frame-thread-initiates-reconfiguration-and-never-performs-it)'s reconfigure
path, which completes as an event some milliseconds later; an origin that moves at pointer rate for
as long as somebody drags a monitor around a settings panel would then be routed through a verb whose
own comment says it must never enter the frame thread's non-preemptible chunk. And the scale half is
layout policy that no backend programs at all, which is the argument that file already makes for
keeping `Scale` out of it.

**The two halves of a view are authored at rates far apart, so the walk composes them rather than
either side carrying both.** The extent is the achieved mode's and is the frame side's; the placement
is the world's and is dispatch's. `Frame/Projection.h` takes them already composed and declines to
know which came from where, which is what leaves this decision free to put each one where it is
actually produced — the evaluator holds them together for the length of one call and nothing stores
the pair.

**Consequence.** The snapshot header gains three named entries rather than one: `Views` beside
`Wakes`, and `Images` and `Solids` for
[decision 95](#95-the-scene-vocabulary-is-four-kinds-a-material-is-a-field-not-a-kind)'s per-kind
content runs, which had nowhere to live either. All three are resolved through templates, so
`Publication` still names neither a coordinate nor a world record and a field added to any of them
does not touch the waist. The version does not move, for the reason
[decision 45](#45-protocol-dispatch-is-a-thread-not-a-task)'s
element size and alignment already give.

### 98. A node is at rest when it names no coefficient

*(Decided 2026-08-22, on asking how the frame thread knows a spring has settled, expecting to have
to publish thresholds and finding they are already unnecessary.)*

**The frame side needs no settling thresholds, because a settled channel does not cross as a
spring.** [Decision 86](#86-the-published-scene-is-a-preorder-tree-model-values-inline-coefficients-by-reference)
has a node carry its model value inline and a coefficient by reference, and
[decision 90](#90-the-snapshots-runs-are-one-per-channel-and-the-frame-side-validates-the-tree-it-walks)
states the rule as *a node carries whatever reconstitutes its value*. Dispatch drops the reference
when the spring comes to rest. So `TranslationSpring == NoCoefficient` **is** the statement that the
channel is at rest, and the walk reads it as one — no thresholds crossed the waist, and
[Animation/Solve/Spring.h](../Source/Animation/Solve/Spring.h)'s insistence that neither half of a
`SettleThresholds` is its own module's to know stays true on both sides of the boundary.

**That is what [decision 67](#67-the-settled-snap-is-unconditional)'s snap tests**, and it makes the
snap a comparison against a sentinel rather than an evaluation. A node is snapped when it and every
ancestor name no moving channel, and the correction goes into the composed chain's translation
column — the position and not the extent, which is what the tiling promise actually rests on.

**It lags by the republication and that is the honest cost.** Between the instant a spring settles and
the commit that inlines its value, the node is still animating as far as the walk can see, so the snap
arrives a frame or two after the motion stops. What lands on screen is a window that stops and then
crisps, where the alternative — the frame side evaluating a settle predicate per node per frame — is
the per-node cost decision 90 refused, to buy an instant nobody watching can distinguish.

**Rejected: publishing the settle instant per channel.** It is exact, it is one `Instant` per active
coefficient, and it makes the frame side's test a comparison instead of a sentinel. It also puts a
second answer to *is this channel moving* in the record beside the first, and the two disagree exactly
when dispatch is late — which is the case the field was added for. One fact, one spelling.

**A driven ramp counts as moving and nothing more.** [Decision 72](#72-the-driven-regime-is-a-distinct-record-the-snapshots-arrays-stay-homogeneous)'s
regime says which channel it drives and the regime is not built, so the walk knows something is in
flight and cannot yet know what it moves. That is enough for the snap and for the damage rule, and it
is not enough to place anything, which is where it stops.

### 99. A dressing draws over the node's own extent, whatever the node's kind

*(Decided 2026-08-22, answering [Open.md](Open.md)'s *what a dressing means on a reference node* the
way that entry asked for — with the walk, since that is the code obliged to have an answer.)*

**Every kind is dressed the same way: the item is placed on the node's own extent, and the dressing
is not inherited by anything.** A dressed image is its texture and its material. A dressed container
is its material alone, which is the case
[decision 95](#95-the-scene-vocabulary-is-four-kinds-a-material-is-a-field-not-a-kind) makes a
material a field for. A dressed reference is its material over the reference's extent, drawn *before*
the expansion because preorder is the painter's order — so an overview thumbnail with a shadow is the
tile's shadow with the window on top of it, which is the thing a shell was going to ask for.

**Rejected: the dressing applies to the expansion.** It reads as the generous interpretation and it
is the one that cannot be drawn. The expansion is a subtree with a screen-space bound, so dressing it
means either an offscreen — a render target on every thumbnail in an overview — or dressing each
drawn node inside it, which is one shadow per subsurface and a seam down the middle of every window
that has one.

**Rejected: a dressed reference is malformed.** The encoding permits it, a shell will write it, and a
frame is not where that gets reported. Refusing it in the walk means an overview tile that silently
loses its shadow on the day somebody adds one.

**So an item can have no content, and the draw list says so rather than encoding it.**
[Seam/Renderer.h](../Source/Seam/Renderer.h) gains a `DrawDressing` alternative: a quad, an extent, an
opacity, and a material, with nothing of the node's own underneath. **Rejected: a fully transparent
`DrawSolid`**, which is the same picture and a worse contract — a renderer would be inferring *this
item is only its dressing* from an alpha of zero, which is also what a solid animating to invisible
looks like on the frame before it is dropped. One of those wants the effect pass and the other wants
to be skipped. It takes the variant's first position so that a default-constructed item draws nothing
rather than opaque black, which is the direction [World/Node.h](../Source/World/Node.h) defaults every
field in.

**A group takes the node's dressing along with its opacity**, which follows from the same place group
opacity does: both belong to the flattened result. A glass window that declares a group blurs what is
behind the *group*, and leaving the material on the member as well blurs it twice — visibly, at the
one moment the window is also fading, which is the only moment a group exists for.

**`DrawItem` gains `Elevation` beside `Material`**, for the reason
[World/Elevation.h](../Source/World/Elevation.h) gives about the node: a glass panel casts a shadow
too, so it is a second field rather than more enumerators in the first. Both enums carry one
enumerator today, so what is settled here is where a dressing lands rather than what any of them look
like — which is still [Open.md](Open.md)'s review with a screen.

### 100. The walk drops a subtree rather than a frame, and it does so three times over

*(Decided 2026-08-22, writing the evaluator's three bounded failures. Each one is a rule about what a
person sees when the walk cannot finish, which is the only reading that separates them.)*

**A malformed subtree length abandons its level.**
[Decision 90](#90-the-snapshots-runs-are-one-per-channel-and-the-frame-side-validates-the-tree-it-walks)
puts the check on the walk; what it does after the check is this decision. A length that overruns the
run has left the walk with no way to find that subtree's end, so the level stops and its outer levels
close normally: the siblings already emitted survive, and what is lost is the tail of one run rather
than the screen.

**Past the depth cap the subtree is dropped whole.** References nest
([decision 88](#88-an-instance-is-a-node-the-published-scene-is-a-dag)), the backwards rule makes a
cycle unrepresentable but not a deep tree, and the walk is on a `SCHED_FIFO` thread where an unbounded
traversal ends with `RLIMIT_RTTIME` taking every session's UI at once. A `// SPEC:` thirty-two is an
order of magnitude past any arrangement a shell has been asked for.

**Exhausting the item arena rolls back to the last top-level subtree.** This is the one that is not
obvious. Truncating where the arena happens to end leaves a group whose offscreen is missing half its
members — a window drawn with its menu gone and the fade applied to what is left — where dropping the
whole subtree loses one window and leaves everything else exactly right. Neither is good; only one of
them is legible, and only one of them is a picture somebody can report. The rollback is a mark taken
at each top-level node and restored on exhaustion, which costs one assignment per window.

**All three are silent to the loop and visible to a sweep.** A frame is not where a scene bug gets
reported ([decision 45](#45-protocol-dispatch-is-a-thread-not-a-task)'s
rule, one level up), so none of them refuses a frame or returns an error; the evaluator reports
truncation as a flag a log line or the schedulability sweep can read.

**Rejected: growing the arena.** It is the natural answer and it is unavailable —
[decision 36](#36-frame-path-discipline-is-enforced-mechanically-not-by-review) forbids allocating
inside the frame section, and a capacity chosen at a configuration change is what
[decision 82](#82-the-renderer-is-handed-an-evaluated-draw-list-not-a-scene) already asks for. The
number is a `// SPEC:` and it wants measuring against a real desktop rather than arguing about here.

### 101. Damage is the whole output while anything moves, and per-node damage needs an identity the record does not carry

*(Decided 2026-08-22, on the evaluator having to report something for `DrawList::Damage` and finding
that the honest answer is a coarse one.)*

**An output owes a whole frame when anything in the walk was moving, or when the snapshot it was
drawn from is newer than the one that output last drew from. Otherwise it owes nothing.** Both
conditions are cheap, both are exact in the conservative direction, and together they keep
[Architecture.md](Architecture.md#doing-nothing-must-cost-nothing)'s promise from the evaluator's
side: a still desktop on an unchanged publication reports an empty region and the compositor idles.

**Per-node damage is what everyone wants and it is not derivable from what crosses.** Damaging where a
node *was* and where it *is* requires the two frames' nodes to be the same node — a published identity
rather than a position in a run dispatch is free to reorder between publications. `Core/Handle.h`
exists and the node record does not carry one. Client surface damage is the other half and has no
carrier either, since there is no protocol layer to mint it. Both are worth having; neither is
inventable from the wire as it stands, so this reports the honest bound rather than a plausible one.
[Open.md](Open.md) carries what it would take.

**Rejected: the union of the moving nodes' bounds, this frame and last.** It is the version that needs
no identity — keep the previous evaluation's union per output, report it with the current one, and a
node that stops contributes nothing. It fails on the frame a node settles: the settled snap moves it
by up to half a pixel *after* it stopped being counted as moving, so the last position it occupied can
sit a pixel outside everything the rule reported. A trail one pixel wide, on exactly the frame the
window came to rest, is the artefact this whole area exists to prevent.

**What it costs today is bandwidth on animating frames and nothing on still ones**, which is the trade
worth taking while the scissor is the only consumer of the region. It stops being worth taking when
there is a partial-composite path to feed, and the entry above is what has to land first.

### 102. A virtual output allocates the buffers it hands out, and that is what stands the renderer up

*(Decided 2026-08-22, on asking what a Vulkan renderer would bind and finding that nothing in the
tree hands out a dmabuf.)*

**`Virtual` is the third backend: an `IPresenter` whose consumer is a file, an encoder, or a test
rather than a panel, which allocates its own dmabufs and hands them out as `RenderTarget`s.** It is
what screen recording and remote desktop are eventually built on, and it is what the Vulkan renderer
is exercised against in the meantime.

**It is not a new concept, and [Seam/RenderTarget.h](../Source/Seam/RenderTarget.h) already contains
the sentence it falls out of** — *a virtual output renders into dmabufs a client owns and a local
output renders into dmabufs gyro allocated; only the source of the constraint differs*. A local
output's constraints are the display plane's. A client-registered virtual output's are the client
device's. A file's are *empty*, which makes this the degenerate case of the rule rather than an
exception to it: one presenter, with the allocator swapped. Building the empty case as a special
fixture is exactly what that paragraph says makes remote desktop a fork instead of an output.

**What forced it now is that the renderer has nothing legal to bind.** [Decision
85](#85-the-headless-backend-is-portable-and-the-instrument-is-the-reason) keeps `Headless` in the
portable tier, so its images are heap pages behind `MappedImage` and must stay so; the DRM and nested
backends do not exist. A Vulkan renderer written against that binds nothing, and the import path —
the half that actually breaks — goes unexercised.

**udmabuf makes the whole path run on a machine with no GPU, and the reading confirmed it rather than
assuming it.** A `memfd` sealed with `F_SEAL_SHRINK` becomes a dmabuf through `UDMABUF_CREATE`; the
size must be page-aligned or the ioctl is `EINVAL`; the memfd may then be *closed*, because the
driver pins the pages, so a target costs one descriptor rather than two; the resulting dmabuf is
directly `mmap`-able and coherent with the memfd's own mapping, so a test inspects composited pixels
with no readback and no staging buffer; and `DMA_BUF_IOCTL_SYNC` is accepted on it, so the CPU-access
bracket is real rather than skipped. What it can produce is `DRM_FORMAT_MOD_LINEAR` and nothing else
— which is exactly the only modifier lavapipe accepts, per [decision
40](#40-software-rendering-is-a-device-not-a-backend-and-it-is-the-floor-tier). The GPU-free path is
therefore complete rather than approximate: real dmabufs, really imported, by the driver that is the
permanently occupied floor tier.

**The permission story points the opposite way from the capability story, which is worth knowing
before it is discovered in CI.** `/dev/udmabuf` is `0600 root:kvm` and is reachable on a developer's
machine only because logind puts a `uaccess` ACL on it for the seat-local user — over SSH with no
session, or in a container, it needs a udev rule or root. `/dev/dri/renderD128` is `0666` by udev
default and needs neither. So udmabuf is the more portable *capability* and the less portable
*permission*, and a build that assumed either one would be wrong in a different environment.

**Rejected: exporting the targets from the Vulkan device.** The chip that scans out is not always the
chip that draws, and the modifier set is an intersection with the display plane's while GBM's scanout
usage is what makes an allocation eligible for a framebuffer at all — none of which a `VkImage`
exported from the render device can carry, and nested there is nothing to export into, since
[decision 1](#1-the-nested-backend-drives-raw-wayland-protocol-not-vulkan-wsi) refuses WSI and a
target is a `wl_buffer` the host handed back. The narrower objection is the one that matters for
testing: a renderer verified against images it allocated for itself is verified against the one
configuration that cannot fail.

*(Revised 2026-08-22 by [decision 120](#120-a-nested-outputs-targets-are-exported-from-the-vulkan-device-and-the-allocator-moves-to-seam).
This rejection holds for a virtual output and does not survive nested, where every clause of it is
about scanout and nothing scans out. The nested sentence is also wrong on the protocol:
`zwp_linux_dmabuf_v1` has no allocation request, `zwp_linux_buffer_params_v1.add` takes an fd the
client already owns, and the `wl_buffer` the host hands back is a handle to gyro's own memory — so
there is something to export into and exporting is the only way to fill it. The testing objection
reverses there too, because the importer is a host compositor gyro did not write.
`IDmabufAllocator` moves to `Seam` with that decision; the argument in
[Virtual/Allocator.h](../Source/Virtual/Allocator.h) for keeping it out was a statement of fact
about there being one caller, and there are now two.)*

**Rejected: giving `HeadlessOutput` a dmabuf provider, which looks like much the smaller change.** The
two rings retire on different events, and that is the one part that is not shared. A headless target
is held until the *next* flip, because a plane is scanning it out — the bound
[Frame/Loop.h](../Source/Frame/Loop.h) leans on when it declines to treat a refused `AcquireTarget`
as an error. A virtual target is held until the consumer lets go, which is the backpressure
[Seam/Presenter.h](../Source/Seam/Presenter.h) already describes as the ordinary reason `AcquireTarget`
answers nothing. What looked like duplication is the mechanism. Decision 85's second argument applies
on top: the instrument should not acquire a shipping backend's requirements.

**What *is* reused is `VblankTimeline`, and that is not a compromise.** A virtual output's cadence is
a period and a phase — a recording at 60 Hz wants vblanks at exact intervals — which is what that
class computes, in arithmetic over `Core/Time.h` with nothing test-only in it. A platform module
depending down onto a portable one is the direction the graph already allows. If it ever grows
behaviour that only a sweep wants, that is the moment it moves rather than a reason to copy it now.

**Deferred: the GBM provider, and the reason is that it costs a dependency for something the DRM
backend owns anyway.** `libgbm` is a new pkg-config entry, and what it buys over udmabuf is tiled
modifiers from a real driver — which is worth exercising against a real *display* plane, not against
a file. The allocator interface is written so the provider drops in; the entry in
[Architecture.md](Architecture.md#dependencies)'s table is not taken until it does.

**Deferred: a consumer interface.** There is one consumer — a test that reads the mapping — so the
output exposes the presented frame and a release verb, the way `HeadlessOutput` exposes `Scanout()`.
An `IFrameSink` with a file writer and an encoder behind it is a second implementation away, and
[Structure.md](Structure.md#the-modules)'s standing rule is that an interface exists where there is a
fake. Encoding in particular stays out: this log already records that Vulkan Video encode coverage is
uneven, and a presenter that hands out buffers and takes them back does not care what reads them.

**What this deliberately does not test, said plainly rather than papered over.** A virtual output
never scans out, so the constraint that actually differs between a buffer that works and one that
does not — a modifier the display plane will accept, and the scanout usage that makes the allocation
eligible — is untested until the DRM backend lands. That failure should arrive there honestly rather
than appear to have been covered by a fixture.

### 107. Vulkan arrives through CPM and gyro never links the loader

*(Decided 2026-08-22, on going to add the pkg-config rows for the renderer and finding they buy
nothing. Numbered 107 rather than 103 on the same day: two changes landed within the hour and both
claimed 103 and 104, and the dressing set — [decision 103](#103-a-dressing-is-named-by-what-it-does-to-light-the-material-set-is-glass-and-smoke)
and [decision 104](#104-an-elevation-is-a-height-under-one-light-and-the-shadow-is-analytic) — went
in first and kept them. Anything citing "decision 103" about the loader means this.)*

**`Vulkan-Headers` and `volk` are CPM packages pinned to the same SDK, and there is no `vulkan.pc`
entry, no `libvulkan` link line, and no `vulkan-loader-devel` in the setup instructions.** volk
resolves Vulkan with `dlopen("libvulkan.so.1")` inside `volkInitialize()`, so the only build input is
a directory of headers — which is a git tag rather than a distribution package.

**The reason that is better rather than merely equivalent is what gyro is.** A linked loader makes a
missing or broken `libvulkan.so.1` a failure of `execve`: the dynamic linker refuses, the process
never runs a line, and nothing reaches a screen. On a login-session compositor that is an error
message in a terminal somebody already has. On a boot service that has subsumed the splash and
[removed the VTs](#37-gyro-owns-the-display-from-firmware-handoff-onward-there-are-no-vts), it is a
black machine with no way in. `volkInitialize()` returning `VK_ERROR_INITIALIZATION_FAILED` is a
condition gyro can *report*, and [decision 79](#79-the-console-is-a-renderer-not-a-presenter)'s
console renderer is what it falls back to — the same path the boot splash already draws through, so
the fallback is a path that runs rather than one that is argued about.

The second-order benefit is that this was found by trying to build on a machine that had
`vulkan-loader` and `mesa-vulkan-drivers` installed and neither `vulkan-headers` nor a
`libvulkan.so` symlink, which is the ordinary state of a Fedora workstation that has never had a
Vulkan SDK on it. That configuration is not a broken machine — it is what a *deployment* target looks
like, and the build should not have needed more than it.

**Rejected: pkg-config for the headers and the loader, which is what
[Architecture.md](Architecture.md#dependencies)'s table said until today.** *(This decision reverses
that row.)* The stated rule is that the platform stack comes through pkg-config and CPM is for C++
libraries, and Vulkan looks like the platform stack. It is not: the loader is a runtime artefact
gyro deliberately does not depend on at link time, and the headers are a version-pinned interface
definition rather than something the distribution owns. `libdrm`, `libinput` and `wayland-server` all
stay on pkg-config, because gyro genuinely links those and a distribution's copy is the right one.

**Rejected: taking the distribution's headers where they exist and CPM's otherwise.** That is one
build that compiles against two different `vulkan_core.h` revisions depending on the machine, and
volk's dispatch table is *generated* from a header revision — a table built against one and used over
another is a null pointer in the middle of a struct, which is a crash with no symbol on it. The
build sets `VULKAN_HEADERS_INSTALL_DIR` explicitly for exactly this reason, because volk's own search
tries `FindVulkan` first and would silently prefer the system copy.

**Deferred, and it is the interesting half: `shaderc`.** The first renderer commit draws with
`vkCmdClearAttachments` and has no shader in it, so the question has not been forced. When it is, the
options are worse than this one: `shaderc` through CPM drags in glslang and SPIRV-Tools and roughly
doubles a clean build, and the alternative — SPIR-V compiled offline and checked in — needs a
compiler nobody has on the machine and makes a shader edit a two-step ritual.
[Decision 9](#9-volk-and-runtime-shader-compilation-in-vma-and-a-test-framework-out) took runtime
compilation for hot reload and that argument still holds; what has changed is that the cost is now
known to be a build-time one rather than a dependency-availability one, which is a different
trade to make and worth making when there is a shader to compile.

### 108. A device that cannot export a timeline finishes the frame inside `Record`

*(Decided 2026-08-22, on writing the Vulkan renderer's first commit and discovering that the floor
tier cannot do what [Seam/SyncPoint.h](../Source/Seam/SyncPoint.h) says every device does. Numbered
108 rather than 104 for the reason [decision 107](#107-vulkan-arrives-through-cpm-and-gyro-never-links-the-loader)
gives; anything citing "decision 104" about a `SyncPoint` means this, and
[decision 104](#104-an-elevation-is-a-height-under-one-light-and-the-shadow-is-analytic) itself is
about shadows.)*

**lavapipe cannot create an exportable semaphore of any kind, so a renderer on it has no descriptor
to put in a `SyncPoint` — and rather than lie about that, it waits for its own submission and returns
`SyncPoint::Immediate()`.** Whether a device can export is asked once at device creation with
`vkGetPhysicalDeviceExternalSemaphoreProperties` and stored on `DeviceDescription`; it is a device
property, not a per-frame branch.

**What the measurement was, because the entry it corrects rested on a reading nobody had done.**
`Tools/VulkanProbe.cpp` was written to settle
[decision 40](#40-software-rendering-is-a-device-not-a-backend-and-it-is-the-floor-tier)'s extension
claim, which turned out to be correct — lavapipe advertises all four, and linear is the only modifier
it accepts, so the udmabuf pairing
[decision 102](#102-a-virtual-output-allocates-the-buffers-it-hands-out-and-that-is-what-stands-the-renderer-up)
rests on is exact. What did not survive is one sentence in `Seam/SyncPoint.h` — *DRM syncobj
timelines throughout, exported from Vulkan timeline semaphores*:

| | timeline opaque_fd | binary opaque_fd | binary sync_fd |
| --- | --- | --- | --- |
| anv (Intel, real driver) | exports, `anon_inode:syncobj_file` | exports | not exportable |
| lavapipe | `VK_ERROR_INVALID_EXTERNAL_HANDLE` at *create* | same | advertised, see below |

So the premise is **confirmed where it matters and false on the tier that is permanently occupied**.
That is a gap between two files written apart rather than an error in either: decision 40 never
claimed export, and `SyncPoint.h` never asked which device it was describing.

**Immediate is the accurate description rather than a workaround, and that is the whole argument.** A
sync point exists so that *two* devices can overlap — the frame thread issues a present against work
the GPU has not finished. On lavapipe there is one device, the CPU, and it is the same one that would
be doing the waiting; there is nothing to overlap with, so "already finished" is simply true. This is
the case `SyncPoint.h` already names as a legitimate producer of the null point — *content that was
finished on the CPU before the call* — and it is why `Blit` and `SimulatedRenderer` answer the same
way. Nothing at the seam changes: KMS gets no `IN_FENCE_FD` and needs none, nested sends no
`wp_linux_drm_syncobj_v1` and needs none, and a virtual output ignores the point entirely.

**What it costs, stated rather than buried.** `IRenderer::Record`'s *records and submits and does not
wait for the GPU* becomes *does not wait where the device can be waited on separately*, and
`Submission::RecordCost` on such a device includes rasterization. Both are more accurate than the
text they replace. It also makes decision 40's priority ladder load-bearing rather than
precautionary: the rasterizer pool runs `SCHED_FIFO` one below the frame thread precisely so that the
frame thread can block on llvmpipe without inheriting every `SCHED_OTHER` process on the machine, and
until now nothing in the tree ever blocked on it.

**Rejected: exporting a binary `sync_fd` per frame, which is the one that looks like it works.**
lavapipe advertises it, and KMS `IN_FENCE_FD` takes a sync_file rather than a syncobj, so on paper
the floor tier could hand a real fence to a real display plane. It cannot. Exporting a `sync_fd`
requires the semaphore to have a pending signal operation, so the descriptor can only be asked for
*after* submission — which is the round trip inside the frame section that `SyncPoint.h` chose a
timeline over a binary fence to avoid, now measured rather than argued. Asking before there is a
pending signal does not return an error; it **hangs**, which is how this was found. It also serves
nested not at all, since `wp_linux_drm_syncobj_v1` wants a timeline, and an owned per-frame descriptor
breaks `RawFd Timeline`'s contract that the timeline outlives every point on it.

**Rejected: a third `SyncPoint` state meaning "not immediate, poll the renderer".** `SyncPoint.h`
already warns that the two producers of the null point must not be conflated, and a third state makes
that worse rather than better — the type is a trivially copyable value in a per-frame layer list, and
every presenter would grow a case. Worse, the presenter that most needs the descriptor is the DRM one,
and handing it "poll the renderer" leaves it blocking anyway, at a point where it does not own the
semaphore. That is this decision relocated to the party least able to act on it.

**Rejected: making the syncobj ourselves.** Open a render node, `drmSyncobjCreate`, and signal it from
a helper thread when the Vulkan timeline advances. This does produce a genuine syncobj on lavapipe.
It costs a thread, a syscall per frame, `libdrm`, and a render node — which a machine with a display
and no GPU driver may not have at all, that being the machine the floor tier exists for.

**What is still open, and it belongs to the DRM backend rather than here.** A floor-tier machine with
a real panel pays software rasterization inside the frame section, and whether the frame clock's
prediction absorbs that gracefully is a question for the first time gyro drives a real display on
lavapipe. Nothing is blocked meanwhile: the virtual output never waits on a point, so the path is
exercised end to end without a panel.

### 109. Shaders compile at build time; runtime compilation is a development option

*(Decided 2026-08-22, on writing the quad pipeline —
[decision 107](#107-vulkan-arrives-through-cpm-and-gyro-never-links-the-loader) deferred this until
there was a shader to compile, and now there is. Revises
[decision 9](#9-volk-and-runtime-shader-compilation-in-vma-and-a-test-framework-out)'s 2026-08-16
addition and sharpens what
[decision 62](#62-effect-composition-is-an-optimization-and-the-unfused-path-is-the-reference) means
by "compiled off the frame path".)*

**Every SPIR-V module gyro can ever need exists before it boots, embedded in the binary. Nothing in
the shipped process turns text into SPIR-V.** What happens at runtime is
`vkCreateGraphicsPipelines` against a module that is already there.

**The two readings of decision 62 are not the same thing, and only one of them needs a compiler in
the address space.** That entry says "pipeline creation is milliseconds of CPU", "the lattice is
built at startup and persisted", and "a variant that is nevertheless absent is drawn unfused that
frame and compiled off the frame path". Every one of those is satisfied by creating a pipeline from
an existing module with the run selected by specialization constants. Decision 9's addition reads
the same sentence as glslang running in the compositor, and that is the reading this entry drops.

**What makes it possible is that the set is closed.** Decision 103 counts the lattice — four ordered
pointwise elements make ten contiguous runs, a gather splits a chain into at most two of them, and
there is at most one gather per item because `Dress` is one field
*(the arithmetic is corrected by [decision 116](#116-the-pointwise-lattice-is-two-run-bits-and-a-conversion-selector-and-the-outputs-colour-state-moves-to-the-binding);
what this entry needs from it — that the set is finite and known before boot — is what survived)* — over a vocabulary
[decision 33](#33-effects-are-named-materials-not-parameterized-filter-calls) closed for cohesion.
An enumerable set can be compiled before boot. Compiz is the instructive contrast and the reason the
comparison keeps coming up: it composed plugin-supplied fragment snippets into one program in 2007
because *any* plugin could contribute one, so the set of combinations was unknowable until the user's
plugin list loaded and the only place left to assemble a program was at runtime. gyro gets the same
fusion out of a table, and it gets it because of a decision taken for an unrelated reason.

**What reopens this is the vocabulary opening.** If a shell ever supplies its own material, the set
stops being enumerable and Compiz's problem returns whole. Decision 33 forbids exactly that, so the
two decisions stand or fall together — worth saying out loud, because a proposal for shell-authored
effects is also a proposal for glslang in a `SCHED_FIFO` process, and it will not arrive announcing
itself that way.

#### glslang rather than shaderc, and the difference is three dependencies

shaderc is the incumbent name in [Architecture.md](Architecture.md#dependencies)'s table and in
decision 9. It is a *library* wrapper whose value is the runtime compilation above, and taking it
costs SPIRV-Tools, SPIRV-Headers and glslang underneath it. Asked for the one thing wanted here — a
`.vert` in and a header out — `glslang -V` is the same SPIR-V from one CPM package. `ENABLE_OPT=OFF`
is what keeps SPIRV-Tools out; HLSL and the SPIR-V remapper are off for the same reason, and what is
left builds in about twenty-five seconds. It is pinned to the SDK Vulkan-Headers and volk are pinned
to, for decision 107's reason: a front end generating for one header revision and a dispatch table
built from another disagree about what a structure contains.

`--vn` emits the header directly, so there is no `bin2c` step and no intermediate `.spv` in the
build. `CMake/Shaders.cmake` is the rule.

#### Rejected: linking shaderc into gyro, which decision 9 had already taken

gyro calls `mlockall(MCL_CURRENT | MCL_FUTURE)`, so glslang and SPIRV-Tools become roughly ten
megabytes of compiler text pinned into RAM that never comes back — on every machine, including the
sc7180 tablet [decision 29](#29-outputs-are-periodic-real-time-tasks-the-test-allocates-effect-budget)
protects, so that one person can tune a blur once. It also buys no machinery: compilation and
pipeline creation both allocate and both take tens of milliseconds, so either way they need a worker
thread and an atomic swap at a frame boundary, which decision 62 already requires.

**The development case decision 9 took it for survives without it: watch the `.spv`, not the GLSL.**
A single shader recompiles in tens of milliseconds, so the file has already changed by the time the
eye is back on the screen — the same loop, with the compiler outside the process. The embedded array
stays the fallback, which matters because
[decision 41](#41-device-migration-is-exercised-on-every-boot) has gyro running before the real
driver has loaded: a shader that must be *found* on a filesystem is a black screen with a path in a
log nobody can read yet.

**And most of what "tune a blur by looking at it" means is not a source edit at all.** Decisions 33
and 103 make the radius, the tint, `Smoke`'s contrast floor and the shadow's falloff constants *of
the material* rather than parameters of a call site, so a live loop for those is a reloadable
constant block and no recompile of anything. Source edits — kernel shape, sample pattern — are the
rarer half and are what the `.spv` watch covers.

#### Rejected: SPIR-V compiled offline and checked in

Decision 107 named this as the alternative and it is worse than it looks. Nobody reviewing a diff can
tell whether the binary matches the source sitting beside it, and the failure is silent in the
direction that matters: a shader edit that was never recompiled ships the old picture.

#### Rejected: `find_program(glslangValidator)`

No dependency at all, and it makes the SPIR-V depend on whichever SDK the build machine happens to
have — which is decision 107's rejected *distribution headers where they exist* arriving one tool
over, with the same consequence that a build is reproducible only by accident.

### 110. `Blit` never reads its target, and nothing moves under it until there is a real flip

*(Decided 2026-08-22, on asking whether the `SketchRenderer` in
[VirtualComposite.Test.cpp](../Source/Integration/VirtualComposite.Test.cpp) should become the console
renderer [decision 79](#79-the-console-is-a-renderer-not-a-presenter) names. It should, and most of
this entry is not about that: the question was settled by reading `drivers/gpu/drm/sysfb/`, which
found that the boot path has no page flip and no vblank — and two paragraphs written elsewhere had
assumed both.)*

**The pivot is the shell rather than the file.** The sketch's seam-facing half is carried over whole,
because it is the half where being wrong is invisible and it has already been read: it refuses a
dmabuf rather than casting, it takes `LOAD` semantics so what lies outside the damage is what was
there, it paints in list order because
[decision 55](#55-transforms-are-3d-the-scene-is-a-painters-algorithm) makes preorder the painter's
order, it answers with an immediate point for
[decision 108](#108-a-device-that-cannot-export-a-timeline-finishes-the-frame-inside-record)'s
reason, and it charges no GPU cost. The inner loop is thrown away. Filling each item clipped to each
damage rectangle is full overdraw, which is the one thing a CPU compositor cannot afford at a panel's
resolution, and what replaces it is spans.

**The boot scene is authored to what `Blit` draws, rather than `Blit` grown to meet a scene it never
sees.** No material, no elevation, no `Reference`, no corner radius *(added 2026-08-22 — the coverage
arithmetic a subpixel edge already needs is most of what an analytic corner wants, so this one is
refused for now rather than foreclosed)*, and quads that are axis-aligned with subpixel edges — a scaled and translated logo needs coverage on the boundary spans and nothing more, so a
rotated or projective quad is refused. `DrawGroup` is kept, because
[decision 60](#60-group-opacity-requires-flattening-per-node-alpha-is-not-a-group-fade)'s offscreen
is the one thing a flat list cannot be widened into later and because the scratch below supplies it
for free. `DrawTexture` is required and is not the console's half of the vocabulary: the firmware
logo is an image, and [Architecture.md](Architecture.md#from-firmware-to-gyro) makes its reproduction
a scale-and-place where getting it wrong is a visible jump at the one moment the design exists to
make seamless. Both texture consumers are gyro's own CPU images, so there is no import path and no
lifetime problem — a far smaller table than the Vulkan renderer's.

**Refusing is safe here in a way it is not one tier up, and only for one reason.**
[Seam/Renderer.h](../Source/Seam/Renderer.h)'s *a renderer refuses what it cannot express* rests on
there being something below to fall to. Below `Blit` there is nothing: it is what runs when Vulkan
will not initialize at all, so a refusal is a dark machine that cannot say why. What makes it safe is
that `Blit` only ever composites gyro's own scene — the console's, never a client's — and that has to
stay true by construction rather than by habit.

**`Blit` never reads its target, and the reason does not differ by driver.** *(Revised 2026-08-22,
on writing it.)* A dumb buffer from a real KMS driver is write-combined: writes stream, reads are one
to two orders slower, and blending a translucent item over what is beneath it is a read-modify-write
of the framebuffer. So the composite happens in cached memory and the damage is copied out. That much
holds. What does not is the driver split this entry originally drew — that on `simpledrm` the mapping
is `drm_gem_shmem` and therefore cached, so the composite could happen in place, and that
`RenderTarget`'s `MappedImage` should carry a field saying which. **There is nothing for that field to
enable.** *What is beneath* a translucent item inside the damage region is always something `Blit`
itself just wrote — the bottom of every composite is an opaque clear — and outside the damage region
nothing is written at all, which is the whole of what `LOAD` semantics ask for. So the backdrop lives
in the scratch on every driver and the target is write-only on every driver. The only thing the field
could have bought is skipping the scratch-to-target copy on `simpledrm`, and it cannot buy that
either: compositing straight into the target means blending in the target's own encoding, which is
[decision 47](#47-compositing-happens-in-linear-light-at-wide-primaries)'s linear-light blend given
up. Keep the scratch and the copy comes back; drop it and the boot logo's antialiased edge is where it
shows — half the light encodes to sRGB 188 and a blend in the encoding puts 128 there, a dark rim
around everything `Blit` draws.

**The scratch is a band rather than a second framebuffer**, which this entry did not say and should
have. Full-size is 66 MB per 4K target and drags every pixel through the cache twice; a few dozen rows
sized to a core's private cache keeps the composite in cache, makes the copy out the sequential write
that write-combining wants, and keeps the size of the panel out of the renderer's memory footprint
entirely. It holds nothing between frames — for the paragraph above's reason — so there is one for the
whole target set rather than one per target.

#### What the reading found

`drivers/gpu/drm/sysfb/`, which is where `simpledrm` now lives:

- **There is no page flip.** `drm_sysfb_plane_helper_atomic_update` walks the damage clips and
  `memcpy`s from the shadow buffer into `sysfb->fb_addr`, which `simpledrm.c` obtains through
  `devm_ioremap_wc`. There is one buffer, the display is scanning it, and nothing is synchronized to
  the raster.
- **There is no vblank.** Nothing in `sysfb` calls `drm_vblank_init`, so
  `drm_atomic_helper_check_modeset` sets `no_vblank = true` and `drm_atomic_helper_fake_vblank`
  delivers the completion event at commit time. The commit is synchronous and the driver's copy is
  charged to whoever called it.
- **There is one mode and its refresh rate is fiction.** `drm_sysfb_mode()` builds a 60 Hz mode with
  no relation to what the panel does, and `drm_connector_helper_get_modes_fixed` offers only that.

**So tearing at boot is real and is not gyro's to fix — which makes it a scheduling constraint rather
than a mechanism.** Nothing moves under `Blit`. The logo is static, boot output is damage-clipped
text, and both tear invisibly. The animation into the greeter is the first moving thing and it runs
after the migration [decision 41](#41-device-migration-is-exercised-on-every-boot) already sequences
on every boot, on a driver with a real flip.
[Experience.md](Experience.md#one-continuous-image)'s *the whole boot is one picture* is kept by that
ordering rather than despite it.

**One target, not three.** A flip queue buys nothing where there is no flip; a second dumb buffer is
a buffer the driver copies out of. On a no-vblank device the presenter binds one, `AcquireTarget`
never stalls, and the loop's stall path is dead there.

**The frame clock synthesizing a period is not new.**
[Virtual/Output.h](../Source/Virtual/Output.h) already includes `Headless/Vblank.h`, so a platform
presenter borrowing the synthetic timeline is the shape that is in the tree. What is new is that a
real panel is scanning at a rate nobody reports while the mode claims 60. It does not matter, for the
paragraph above's reason: nothing is moving, so a wandering tear line has nothing to tear.

**The console publishes one `DrawTexture` and not one item per cell.** A 4K panel at a 16×32 cell is
about sixteen thousand cells, so per-cell items is a megabyte of draw list republished through the
ring every frame and sixteen thousand walk iterations to report that one line changed. One texture
keeps the grid in the console's own image, where a printed line is a few hundred kilobytes of damage
and a scroll is a `memmove` in cached memory. The scene is the wrong place to hold a text grid.

**Rejected: deleting the sketch and substituting `Blit` into the integration test**, which is what
that file's own comment says will happen. It would turn four tests about *frame delivery* —
acquisition, presentation, the ring turning, damage outliving a stall — into tests about a renderer,
so a coverage-antialiased edge or a linear-light blend would fail assertions that are about neither,
and one `Blit` bug would fail all of them at once. The flat painter stays where it is; `Blit` gets
its own tests and one `Integration` file pairing it with `Virtual`, the way
[RenderImport.Test.cpp](../Source/Integration/RenderImport.Test.cpp) pairs the Vulkan one.

**Rejected: `Blit` as a `DeviceClass` under `Render`.**
[Decision 40](#40-software-rendering-is-a-device-not-a-backend-and-it-is-the-floor-tier) makes
software rendering a device selection, which reads as though lavapipe already covers this. It does
not: `Blit` runs before the real driver has loaded on every boot, and permanently on a machine where
Vulkan will not initialize at all. It is the floor beneath the floor tier, which is what decision 79
already said and what this entry declines to re-open.

**Rejected: a general edge-function rasterizer, so that `Blit` draws every quad the walk can
produce.** It is a rasterizer's worth of code in service of content that does not exist — the boot
scene is gyro's own and can be authored not to rotate. The refusal costs a branch and is caught by a
test rather than by a user.

**Rejected: double-buffering the dumb buffer on a no-vblank device**, for symmetry with the real
driver. It costs a full-screen copy per frame and buys nothing: the commit returns after the driver's
`memcpy`, so the buffer is free the instant the ioctl is.

**Rejected: compositing directly into the target on every driver**, which is what the sketch does and
what is correct on `simpledrm`. On a real driver it is the read-modify-write above, on the memory
that is worst at it.

*(Not verified: what the `simpledrm`-to-real-driver handoff itself costs on screen. The real driver
displaces `simpledrm` through the aperture helpers and brings the pipe up its own way, before gyro
holds an fd on it, so whether the picture survives is a property of that driver's probe — i915
fastboot reads hardware state back and can skip the modeset, and amdgpu is believed not to. Neither
was read. It is the same class of exposure as the mode difference and the wire colorimetry below, and
all three want one entry.)*

**What this deliberately does not settle**, each of which is its own question: which formats `Blit`
encodes, now that *XR24 only* has been rejected as an accident of what dumb buffers usually are; the
wire colorimetry, which is the one seam at the handoff that genuinely blanks a panel; and the
per-target damage accumulation
[VirtualComposite.Test.cpp](../Source/Integration/VirtualComposite.Test.cpp) characterises, which
`Blit` is what finally makes worth closing —
[decision 101](#101-damage-is-the-whole-output-while-anything-moves-and-per-node-damage-needs-an-identity-the-record-does-not-carry)
reports the whole output while anything moves, and a whole-output CPU composite is the expensive
case by a wide margin.

### 116. The pointwise lattice is two run bits and a conversion selector, and the output's colour state moves to the binding

*(Decided 2026-08-22, on building
[decision 103](#103-a-dressing-is-named-by-what-it-does-to-light-the-material-set-is-glass-and-smoke)'s
chain into the quad pipeline. Corrects that entry's arithmetic, below, and settles what
[decision 109](#109-shaders-compile-at-build-time-runtime-compilation-is-a-development-option) meant
by a run mask.)*

**Decision 103's conclusion holds and its count does not.** That entry needed the variant set to be
enumerable at build time, and it is — which is the property decision 109 rests on and it is
untouched. What it got wrong is the number, and the three mistakes are each worth more than the
figure they produced.

#### Counted against code

Decision 103 reads *four ordered pointwise elements — the colour-state conversion, the corner-radius
mask, per-node opacity, and decision 62's dim — so contiguous runs number ten*. Three of the four
premises fail.

**Opacity and the dim are one multiply, and it is data rather than code.** Both are a scalar on
premultiplied components, so they commute with each other and the renderer folds them into one
number per item. There is nothing for a variant to switch on: eliding a multiply by a push constant
saves one instruction and costs a whole pipeline. Two of the four elements are not axes at all, and
counting them as two was counting a name rather than an operation.

**The conversion is not one element but three stages, and its presence bit is not separate from
its selector.** `TransferFunction::Linear` *is* the absent decode; matching `ColorPrimaries` *is*
the absent matrix. So there is no mask bit to add beside the selector — the selector is the mask,
and what the conversion costs the lattice is a product over two closed enumerations rather than a
factor of two.

**Nothing splits this chain, so the contiguous-runs formula counts a shape the vocabulary cannot
express.** [Decision 62](#62-effect-composition-is-an-optimization-and-the-unfused-path-is-the-reference)
counts runs because a gather forces materialisation *in the middle* of a chain, and the ten follows
from a gather being able to land at any of the positions. Here it cannot. There is at most one
gather per item because `Dress` is one field, and
[decision 99](#99-a-dressing-draws-over-the-nodes-own-extent-whatever-the-nodes-kind) places the
dressing over the node's own content — so the gather lands at exactly one position, after the
content's conversion and before the mask and the scalar. **One fixed split point admits two runs,
not ten**, and the eight the formula adds are chain shapes no `DrawItem` can encode.

**So the run mask is two bits** — the corner mask and the conversion — and the lattice's size is the
conversion's selector rather than the mask. `Prepare` builds twenty pipelines per binding: the two
corner variants that convert nothing, and a converting pair for each of the nine source colour
states. About twenty-five milliseconds of pipeline creation for the set, measured on lavapipe, at a
reconfiguration and nothing at all inside a frame.

*(That figure is measured rather than derived, and the first version of this paragraph got it wrong
in exactly the way this entry is about. A plain variant takes six hundred microseconds and a
converting one roughly 1.3 milliseconds, so twenty of them is twenty-five rather than the twelve that
multiplying by the cheap one gives. [Pipeline.Test.cpp](../Source/Render/Pipeline.Test.cpp) prints it
rather than asserting it, so the next person to change the variant count sees what it did.)*

**Twenty-five milliseconds is paid at a binding, which includes the first one**, so it is that much
before the first composite on a boot service whose whole point is being early. That is acceptable and
it is not free, and the lever if it stops being acceptable is the pipeline cache decision 62 already
asks for and nothing here builds — not a smaller set, because a smaller set is a frame that cannot be
drawn.

**The corner mask survives as a real axis and it is the only one of the three that does.** It is two
hardware derivatives, a length and a divide, and an unrounded node — the wallpaper, a tiled window,
[decision 106](#106-an-x11-client-has-no-window-geometry-gyros-window-manager-computes-the-frame-rect)'s
X11 client — pays all of it to be multiplied by one. Its elision is also exact rather than close: at
a radius of zero the field is identically one everywhere the rasterizer covers, so the two variants
are the same picture bit for bit.

**What a person sees is nothing, and that is the point of writing it down.** No frame looks different
for this. What changes is that the number decision 109 cited as evidence the set is enumerable was
arithmetic nobody had run, and the next question resting on it — the first gathering material, which
is where a chain genuinely does split — would have inherited it.

#### The output's colour state moves from `RecordRequest` to `BindTargets`

**A renderer is per output, so what the composite is encoded to is a property of a binding rather
than of a frame.** [Compositor.cpp](../Source/Compositor/Compositor.cpp) already says so in as many
words — a writer is bound to one presenter's target set for as long as that set exists — so stating
the colour state on every `RecordRequest` was a per-frame restatement of a per-binding fact.

What forced the move is that **the target end of a conversion decides which pipelines have to
exist.** Decision 62 forbids a frame blocking on compilation, so a variant an item wants and does not
find is a refused frame; `For` therefore has to be total, which means `Prepare` has to have
enumerated everything reachable, which means the target end has to be known at the only call the seam
allows to be slow. It was not. The renderer learned it one call too late, every frame, forever.

**Rejected: enumerating the target end as well.** It needs no seam change and it is what the code
would have done by default. Nine target states against nine source states and two corner variants is
a hundred and sixty-four pipelines per format — over two hundred milliseconds of pipeline creation at
every binding, on a boot service whose first pixel is the firmware logo continuing, and twice that
where two formats are bound. Paid on every hotplug,
to discover at runtime a fact the composition root held all along.

**Rejected: the colour state on `RenderTarget`.** It scales to several outputs on one renderer
without a second thought, and [RenderTarget.h](../Source/Seam/RenderTarget.h) rejects it in a comment
that is still right: a copy on every target is a second place to be wrong when a reconfiguration
changes one and not the others. One per call has the same staleness window and one copy in it.

**Rejected: building the missing variant on the first frame that names it.** It is decision 62's own
*a variant that is nevertheless absent is drawn unfused that frame*, and the unfused path does not
exist yet — so what it actually means today is dropping the first frame after a client posts content
in a new colour state. That is a stutter exactly when something new appears on screen, which is the
worst available moment for one.

#### HLG is refused by name, and the gamut clip is named rather than discovered

**Nothing converts to or from `TransferFunction::Hlg`.** BT.2100's scene-to-display step needs the
display's peak luminance and [ColorState.h](../Source/Core/ColorState.h) carries a reference white
instead. Every implementation without a peak has silently picked one; the failure is a film that is
subtly the wrong contrast on one panel and right on another, which nobody traces to a shader. So it
is `EINVAL` at the binding and at the item, and the peak is an [Open.md](Open.md) entry rather than a
constant somebody guessed.

**A conversion into narrower primaries is clipped at zero, and that is a gamut clip.** Bt2020's red
is a long way outside Bt709's, so the matrix produces negative components and there is no encoding
for negative light — left alone it is `pow` of a negative, which GLSL leaves undefined and drivers
answer with a NaN that blends across the whole quad. The clip desaturates rather than losing the
colour, and every alternative is a gamut-mapping curve with a perceptual argument behind it, which is
a number wanting a screen. What must not happen is the picture depending on which driver the machine
has.

#### The fused and unfused paths are built from one set of GLSL functions

Decision 62 requires the unfused path to exist as separate passes and to be the reference.
[Chain.glsl](../Source/Render/Shaders/Chain.glsl) is one function per element and both forms compose
it, rather than each form spelling the elements itself.

**Sharing strengthens the oracle decision 62 actually described and weakens one it did not.** That
entry names the hard part as precision — a fused chain keeps intermediates in registers while
separate passes round at every target boundary — and that comparison is only readable if the
arithmetic on both sides is the *same* arithmetic. With two sources, a disagreement is ambiguous
between a fusion bug and two sRGB curves differing in the last bit, and the ambiguous reading is the
one a tired person takes.

**What is given up, named:** an element whose arithmetic is wrong is wrong identically on both sides,
so the oracle cannot see it. That was never what it was for, and two hand-written copies would fail
the test they look like they pass — both get written the same afternoon from the same paragraph of
the same standard, so a misreading goes into both and the oracle reports agreement. Element
correctness is a value against a published curve, and
[RenderImport.Test.cpp](../Source/Integration/RenderImport.Test.cpp) checks it that way: the sRGB
encode of one half, Bt709's red through the Bt2020 matrix, and a reference-white ratio, each
isolating one stage.

**And the pass-based form is not built yet, deliberately.** A pass boundary exists because a gather
cannot consume a value that has not been written, and no gather is expressible — every `Material` is
refused. Building one now would be a render target introduced so that a chain with no split could be
split, whose only distinguishing property is the round-trip rounding decision 62 calls a difficulty.
The comparison that is worth running today is the elided variant against the unelided one, which
needs no target and tests the thing this change actually introduces: whether the run mask elides
something that was not a no-op.

### 117. A gather reads the target it is drawing into; the numbers live in `Seam` and the tier rides the request

*(Decided 2026-08-22, on building
[decision 103](#103-a-dressing-is-named-by-what-it-does-to-light-the-material-set-is-glass-and-smoke)'s
first gathering material. Four questions were open at once and each of the four had an obvious wrong
answer, which is why they are one entry.)*

#### `Material::Glass` gets the offscreen, and decision 60's group will not want the same one

The two look like one mechanism — *a gather needs a backdrop, so the renderer allocates an offscreen*
— and they read in opposite directions.

**A group *writes* an offscreen.** Decision 60 renders a subtree into a cleared, full-resolution,
alpha-carrying target so that it is opaque to itself, then composites it once at `g`.

**A material *reads the attachment it is already drawing into*.** It needs no target of its own for
the backdrop at all. Decision 60's own rule is what collapses the two cases: glass inside a
flattening subtree samples the composite as of the group's base, which is exactly *whatever
attachment is bound right now, as it stands*. So a material never learns what a group is, and a group
never learns what a material is — and the sentence in decision 60 that reads as a special case turns
out to be the thing that makes them independent.

What is genuinely shared, and is what would otherwise get built twice, is the **reservation**:
decision 46's discipline of storage taken when an output is configured and never allocated at the
moment a transition starts. [Backdrop.h](../Source/Render/Backdrop.h) is that reservation with the
chain on top of it, and flattening takes its full-resolution images from the same call.

**What the split costs, stated rather than discovered.** Vulkan cannot sample the colour attachment
it is rendering into with a neighbourhood read — an input attachment reads one fragment's own
coordinate and a blur reads its neighbours — so a dressed item ends the render pass, barriers the
target to a shader read, runs the chain, barriers back, and resumes with `LOAD_OP_LOAD`. One split
per dressed item. Decision 116 said the pass-based form was not built because no gather was
expressible; this is the gather that made it expressible.

**Rejected: batching the non-overlapping dressed items into one split.** Two glass panels that do not
overlap could share an extract, and on a screen with a dock and a top bar that is every frame. It is
an optimization on top of a correct path, which is decision 62's own shape, and taking it first would
make the reference the thing nobody runs.

**Rejected: extracting into a second full-resolution copy of the target.** It avoids the pass split
by copying once at the top of the frame, and it spends a full-resolution write plus a full-resolution
read on every frame with any glass on it — against a split whose cost is a tile flush, on a panel's
own area. It also samples a backdrop as of *before the frame*, not as of below the item, which is a
different picture wherever anything below a panel moved.

#### The damage path knows nothing about expansion, and this does not compensate for that

[Decision 63](#63-effects-declare-their-kind-and-their-damage-the-verifier-keeps-them-honest) makes
expansion a declared property and spends it three times. Read against the tree, the second of those
three has nowhere to land: [Evaluator.h](../Source/Frame/Evaluator.h) reports the whole output or
nothing under
[decision 101](#101-damage-is-the-whole-output-while-anything-moves-and-per-node-damage-needs-an-identity-the-record-does-not-carry),
[Loop.h](../Source/Frame/Loop.h) unions that per output, and `Region::Expand` has no caller anywhere
near either. So the expansion is **satisfied vacuously**: a whole-output region already contains
every neighbourhood a chain can read.

**The number is declared anyway and it is not idle.** It has a consumer today and it is the renderer's
own read: a dressed item's chain extracts its bound *grown by exactly that figure*, because a pixel at
the item's edge needs a whole neighbourhood behind it. Same number, two jobs — which is decision 63's
*three uses of one declaration* showing up one use earlier than expected. What is owed is the damage
job, and it arrives with per-node damage rather than before it.

**Rejected: clamping the chain's sampling to the damage region instead.** It would make the renderer
correct under a narrower damage rule without anybody writing that rule, and it would put a
correctness invariant in the one file nobody would look in for it. When per-node damage lands and the
expansion is forgotten, the failure should be a visible fringe at a panel's edge — attributable, and
fixed — rather than a renderer silently absorbing it.

**A box chain is what makes the declaration exact.** A Gaussian has infinite support, so declaring a
bound for one means choosing a number of standard deviations and accepting a fringe below it, which is
the under-declaration decision 63 says leaves a trail. `passes` boxes of half-width `w` reach exactly
`passes · w` and not one texel further. That is an argument for boxes beyond their cost, and it was
not the reason they were picked.

#### The numbers live in `Seam/Dressing.h`, in two tables that share no column

**Not `World/Material.h`.** Decision 33 forbids the call site naming a radius, and that header is what
the authoring side includes in order to say `Material::Glass` — so a radius in it is decision 33 lost
by proximity rather than by argument, one `#include` from the thing it exists to prevent.

**Not `Render`.** Decision 62's oracle draws one frame two ways and asserts they agree, and decision
40 makes software rendering a device rather than a backend — so `Blit` and the Vulkan renderer are two
implementations of one look, and a number in either is a number the other can disagree with.

`Seam` is *every interface with more than one implementation and the data crossing it*, and this is
the second half of that sentence. `MaterialTable` holds the sigma, the tint, `Smoke`'s contrast floor
and decision 63's kind; `TierTable` holds decision 34's two rungs. **The per-pass half-width is
solved rather than stored**, from the material's sigma and the tier's structure, so *the tier changes
the pass structure and never the look* is an assertion a test makes rather than a promise a comment
makes.

**The obvious implementation is the one that breaks decision 34.** A dual-Kawase chain — what KWin and
Hyprland use — halves the resolution *per pass*, so rungs one and two become one lever and the sigma
falls out of the structure instead of being an input to it. Decision 34 cannot survive that, and it
is what a first cut written from the prior art would have been.

**Two things the arithmetic said that the entries did not.** *Equal sigma is not an identical kernel*
— three boxes make a near-Gaussian and two make a triangle, so rung two has the same spread with a
harder falloff, which is why decision 34's order is right and not merely conventional. And *the
ladder bottoms out*: the extract reads the panel's area once at full resolution and the dressing
writes it once, so about two passes over that area are owed whatever the tier is, and below a divisor
of eight the blur itself is a fraction of one. Rung one buys most of what there is, rung two buys a
little, and what buys the rest is the third rung.

#### Decision 34's third rung is `RenderMode::Floor`, and this is the first behaviour either renderer
attaches to it

That entry ends its ladder at *the material is not rendered — an opaque or simply tinted fill*, and
decision 35's record-time check already picks a mode per frame. A tier enumerator meaning the same
thing would be a second spelling of one fact. So a **tier** says how a chain is built and a **mode**
says whether there is one, and at `Floor` every material paints its tint alone: no offscreen, no read
of the backdrop, nothing a floored frame cannot afford.

Both renderers carried `RecordRequest::Mode` and ignored it until now; only the simulated headless one
charged a different cost for it. A gather is what gives it a picture.

**The same rung catches what a device will not do.** A machine whose driver has no floating-point
offscreen, or whose target modifier does not list `SAMPLED_IMAGE`, draws its materials as tints and
everything else exactly as before. Refusing the bind instead would turn a look into a black screen.

#### The tier rides `RecordRequest`, beside the mode

[Decision 116](#116-the-pointwise-lattice-is-two-run-bits-and-a-conversion-selector-and-the-outputs-colour-state-moves-to-the-binding)
moved the output's colour state to `BindTargets` because the target end of a conversion decides which
pipelines must exist and a frame may not compile. **That argument does not reach a tier**, which
decides a loop count and an image extent and opens no pipeline — every one it can name is built at the
binding.

What decides it instead is decision 63: **the party that expands damage for a material must be using
the same tier as the party that draws it**, and the only arrangement that guarantees that is for the
tier to travel on the request whose damage was expanded. Decision 34's stickiness is a rule on
whatever *chooses* a tier, which is a startup probe nobody has written; until then it is `High` from
one end of a session to the other.

**Rejected: a third verb on `IRenderer`.** It matches the seam's existing contract split and it costs
a rebind's worth of ceremony for a byte, and it puts the tier somewhere the damage expansion cannot
see it.

#### Two Open.md entries take defaults, and both are forced rather than chosen

**Blur order against tone mapping** — blur in linear, and there is no tone map to order against.
Nothing in the tree tone maps; [Chain.glsl](../Source/Render/Shaders/Chain.glsl) is a decode, a
matrix, a gamut clip and an encode. The chain decodes the target to linear, blurs, and re-encodes.
A tone map lands after this when it arrives, and the entry stays open.

**Blur across colour-state boundaries** — the physically correct one, by omission. The backdrop is the
composite target, and every item was converted into the output's state on the way in, so there are no
colour-state boundaries left in what is sampled: a 1000-nit highlight bleeds through the glass exactly
as far as linear arithmetic says. The entry now has a price attached, which is the useful half —
answering it *no* costs a per-pixel provenance channel the composite does not carry.

**Neither is an answer and both are recorded as defaults**, because either could be settled the other
way by a screen and neither is settled by this.

#### The chain does not convert primaries, and that is a saving rather than a shortcut

Decision 47 fixes the composite space as linear at wide primaries, and the chain is neither: it holds
the *output's* primaries, made linear. A blur is a weighted sum, a primaries change is a matrix, and a
matrix commutes with a weighted sum — so blurring in the output's own primaries and blurring in
Rec.2020 are the same picture, and the two matrices this would otherwise spend are two that cancel.
It also retires the reason [Architecture.md](Architecture.md#precision-and-why-the-blur-chain-is-affordable)
gives for wanting Rec.2020 in the packed float: a value decoded out of the target's own encoding is
non-negative before any matrix touches it.

#### What is left undone, named

**`Blit` still refuses a material**, so decision 35's floor composite cannot yet draw a scene with
glass in it on the CPU renderer. The rung it needs is the tint alone and it is small; it was left out
because `Blit` was being rewritten in the tree at the same time and adding to it would have collided.
It is the next thing owed here.

**Decision 63's verifier is not written**, because with whole-output damage it has nothing to
contradict. It arrives with per-node damage, alongside the expansion's second consumer.

**Nothing probes.** Decision 34's startup capability probe is still the thing that should choose a
tier, and until it exists every output runs at the top of the ladder.

### 118. The unfused chain's intermediate is a half float, and the oracle asserts one eight-bit code point

*(Decided 2026-08-22, on writing decision 62's oracle. Answers [Open.md](Open.md)'s "the intermediate
format for unfused effect passes", which asked for a measurement with a tolerance attached and said
the tolerance was the harder half. It was, and not for the reason that entry expected.)*

#### The reference had to be built before it could be the reference

[Decision 62](#62-effect-composition-is-an-optimization-and-the-unfused-path-is-the-reference) has
said since it was written that the fused form is never required for correctness and that the
separate-pass form is the oracle. Nothing unfused existed. The lattice was built whole at every
binding and every item found its variant, so the sentence was aspirational — which is precisely the
failure [decision 34](#34-effect-quality-is-a-tier-gyro-chooses-and-the-floor-tier-is-the-recovery-path)'s
floor-tier argument names, an alternative path that has never run.

[Unfused.h](../Source/Render/Unfused.h) is that path. Per item: the fill into an offscreen, then the
colour-state conversion, then the corner mask, then the scalar — each rasterizing *the same six
vertices through the same placement function* as the fused program, each reading its own fragment
coordinate out of the offscreen the element before it wrote, and a final pass compositing the result
`over` the target. Four rasterizations and three round trips where the lattice does one draw and
keeps everything in registers.

**Five pipelines rather than thirteen, and the selectors are the difference.** The fused set
enumerates nine source colour states per binding because a specialization constant has to exist
before a frame needs it; a separate pass carries the same four selectors as data, in the sixteen
bytes `QuadConstants` had left of the guaranteed push constant block, so one convert pipeline serves
every source state. That is decision 62's shape read from the other end — the fused set counts
contiguous runs and the unfused set counts *effects* — and it is what makes keeping the reference
available cheap rather than something to compile out.
[Decision 109](#109-shaders-compile-at-build-time-runtime-compilation-is-a-development-option)'s
argument against selectors as data does not reach it: nine multiplies per fragment on the identity is
a real cost on every ordinary frame and is not a cost on a path that runs in a test.

**A renderer that cannot build the reference refuses at the binding rather than falling back.**
Everywhere else in the renderer a failure to reserve is a tier — decision 34's third rung, the
material drawn as its tint — because the picture is what matters. Here the picture is not what
matters: a reference that quietly fell back to the lattice would make the oracle compare the fused
execution against itself and report agreement, which is the one failure this whole path must not
have. So `Fusion` is fixed at construction, and a `Fusion::Separate` renderer whose intermediates did
not come up fails `BindTargets`.

#### The format is `R16G16B16A16_SFLOAT`

The whole of what the two paths can disagree by is what the intermediate rounds to. Four
requirements, and each of them kills at least one candidate:

- **Finer than the output, by enough that several boundaries are still finer.** A half rounds at
  2⁻¹¹ relative — about a quarter of an eight-bit code point at the top of its range and far less
  below it — so a chain of four stays inside one code point.
- **Alpha at colour's depth**, because what travels here is a premultiplied item mid-fade.
- **Unbounded above**, so [decision 47](#47-compositing-happens-in-linear-light-at-wide-primaries)'s
  HDR headroom over 1.0 survives a round trip.
- **Present on every device**, and in particular on the floor tier, which is the machine that most
  has to be able to run the reference.

**Rejected: `R8G8B8A8_UNORM`.** The same depth as the output, so every boundary costs a whole code
point and the disagreement is the pass count. This is the one that was measured rather than argued:
swapping it in takes the worst pixel from one code point to **forty-eight** and puts 268 of 2048
pixels past the threshold. That number is the evidence that the oracle bites rather than passing by
construction, which is a property every comparison test owes and few can show.

**Rejected: `A2B10G10R10_UNORM_PACK32`.** Two bits of alpha is a fade in four steps.

**Rejected: `R16G16B16A16_UNORM`,** and it is the tempting one: over `[0, 1]` it is finer than a half
everywhere, by sixty-four times at the top of the range. It clamps at 1.0, which throws away the
headroom above; and Vulkan does not require it of a colour attachment, so the reference could be
missing on exactly the device somebody is trying to explain a picture on.

**Rejected: `B10G11R11_UFLOAT_PACK32`,** which
[decision 117](#117-a-gather-reads-the-target-it-is-drawing-into-the-numbers-live-in-seam-and-the-tier-rides-the-request)
gives the blur chain. No alpha at all, and six mantissa bits is *coarser* than the output it is
supposed to be finer than. The two chains want opposite things for one reason: a backdrop is opaque
and an item is not.

**Rejected, and this is the one that decides the entry: `R32G32B32A32_SFLOAT`.** It would make the
oracle pass with room to spare and prove nothing. A reference at a precision no production frame
would ever use is a comparison against arithmetic rather than against an implementation, and the
first real unfused frame would be the first time anybody found out what the format cost. The
reference has to be a path the machine would actually run.

Vulkan's required-format table lists the chosen format for `COLOR_ATTACHMENT` and `SAMPLED_IMAGE`,
which is why it should be present everywhere; `Unfused::Reserve` asks the device anyway, because a
renderer that read a table instead of the driver in front of it would report a driver's gap as a
corrupted picture.

#### The tolerance is one eight-bit code point, and the margin is the count rather than the worst

[Experience.md](Experience.md#the-picture-is-correct) requires that the image not change when the
machine changes how it draws it, and decision 62 says that promise now covers the fusion decision
itself. An eight-bit sRGB step is about where a difference stops being visible in a gradient — which
is why eight bits is marginal and ten fixes it — so *within one step of the output's own encoding* is
a claim about vision rather than about a format, and it holds whatever depth an output is configured
for.

**The worst pixel saturates and the count does not**, which was not obvious until the test ran. Two
executions whose values drift by a fifth of a code point land on a different code at about a fifth of
their pixels and on the same code everywhere else — so the worst figure reads exactly 1.00 the moment
any pixel sits near a rounding boundary, and stays at 1.00 whether the drift underneath is a fifth of
a step or nine tenths of one. What keeps moving is how many pixels differ at all, so
`ImageDifference` reports both and the test prints both. A threshold test whose only instrument is
its worst pixel has no way to see its own margin eroding.

**Measured, on a scene holding all four chain shapes at once: six pixels of 2048 differ, each by one
code point, none beyond the threshold — identically on lavapipe and on Intel hardware.** Six of 2048
puts the drift near three thousandths of a code point, two orders of magnitude inside the threshold
rather than the factor of four the arithmetic alone predicts. The arithmetic is not wrong; the scene
is mostly uniform fills, where a whole fill lands on one side of a boundary or the other together,
and what is left is the corner arcs.

**Rejected: a golden image.** A checked-in frame states every pixel, so it fails on the ones nobody
meant to promise — a driver that rounds an arc one bit differently — and it goes stale silently. The
reference here is the other execution of the same scene, on the same device, in the same run: it
cannot go stale, and it asserts exactly the property decision 62 needs and nothing else.
[Pixels.h](../Source/Virtual/Pixels.h) already made this argument for the predicates; this is the
same argument where the expected value is a second rendering rather than a claim a test wrote down.

**Rejected: reimplementing the chain on the CPU and comparing against that.** It is two hand-written
copies of the same paragraph of the same standard, written the same afternoon, so a misreading goes
into both and the oracle reports agreement.
[Chain.glsl](../Source/Render/Shaders/Chain.glsl) is the answer to that: one implementation of every
element, shared by both paths, so what is compared is composition and precision rather than two
spellings. What that gives up is real — an element whose arithmetic is wrong is wrong identically on
both sides — and it was never what this was for. Element correctness is a value against a curve in a
standard, which is checkable without a second implementation; composition and precision are not.

#### What is left undone, named

**A gather has no second execution and is not compared.** Decision 62 segments a chain at every
gathering effect, so decision 117's blur chain is separate passes by construction and there is no
fused form to hold it against. The oracle covers exactly the run the lattice fuses.

**Nothing selects unfused in production.** The lattice is built whole at every binding and no frame
has ever missed a variant, so decision 62's *a variant that is absent is drawn unfused that frame*
has no trigger yet. The path is built, exercised every run, and reachable only by asking for it. That
is the right order — it is available before it is needed rather than written at the moment a cold
cache first misses.

**The comparison has not been run under validation layers**, which are not installed on the machine
this was written on. Every barrier and layout transition here is unverified by anything except two
drivers not complaining.

### 119. The Wayland wire codec is its own module, `Wire`, and it is portable

*(Decided 2026-08-22, on asking where the client codec
[decision 1](#1-the-nested-backend-drives-raw-wayland-protocol-not-vulkan-wsi) requires actually
lands, given [decision 81](#81-a-source-is-pumped-by-one-thread-nested-opens-one-connection-pumped-by-the-frame-thread)'s
revision putting a host connection on the frame thread.)*

**The wire codec is a module of its own, `Wire`, depending on `Core` alone, sited below both waists,
and in the portable tier.** *(Revised 2026-08-23: it depends on `Core` and `Seam`. A connection is
drained by the frame loop alongside a DRM device and a simulated vblank, so it implements
`IEventSource` rather than being something `Frame/Loop.h` learns to poll specially — an edge this
entry did not have to consider because decision 81's revision had not yet put a host connection on
the frame thread in a form anything polled. The edge is `IEventSource` and nothing else, and it is
confined to `Wire/Connection.h`: `Wire/Writer.h` forward-declares `Connection` and the one
constructor needing it complete lives in `Wire/Writer.cpp`, so marshalling a request — which is what
every generated call site does — does not include the control waist. Below both waists still holds
in the sense the entry meant it: nothing here names `Scene` or `Protocol`, and `CheckLayering.cmake`
still refuses the edge that was the reason for the module.)* A connection instance is pumped by one
thread for the whole of its life, which is decision 81's rule applying per object rather than per
module — so the module itself has no thread affinity, the way `Core` and `Geometry` have none.

**What forced it out of `Nested` is a layering edge that cannot exist.**
[Structure.md](Structure.md#the-modules) has `Protocol` depending on `Scene`, and `Nested` split
across both halves. `Nested` reaching `Protocol` for a codec would make `Scene` reachable from the
frame side, which that document names as *the one thing worth enforcing rather than describing* and
which `CheckLayering.cmake` refuses. So the codec cannot be borrowed from the server side, and the
question is only whether it lives inside `Nested` or below it.

**It does not serve `Protocol` today, and saying so is worth more than the symmetry.** The obvious
argument for the module — *one codec, two callers, so it belongs below both* — is not available,
because [decision 2](#2-gyro-owns-the-protocol-seam-libwayland-implements-the-server-codec) gives the
server codec to `libwayland-server`. The demarshaller, the socket manager, id allocation and
`delete_id`, fd buffering against the 28-fd send limit: all inherited, none of it gyro's to write, and
what the build-time bindings generate for the server half *wraps* libwayland rather than sitting on a
gyro codec. `Wire` has exactly one caller. The module has to be justified without the second one, and
two arguments carry it.

**It is testable against a scripted peer instead of a compositor.** A codec inside `Nested` is
reachable only through a backend whose far end is a running host — so a marshalling bug is found by
mutter closing the connection, with a protocol error string and no frame of reference. A codec in its
own module is exercised over a `socketpair` with the peer written by the test: a truncated header, an
fd arriving one message early, a `new_id` reused inside the `delete_id` window, an argument array
that runs off the end. Those are the cases that matter and none of them is reproducible by asking a
host nicely. *(2026-08-23: the `new_id`-inside-`delete_id` case named here is where
[decision 123](#123-a-destroyed-proxy-keeps-its-dispatcher-and-loses-its-object) came from — the
runtime as first written ended the connection on it, which is what a scripted peer found and a real
host would only have shown as an unexplained disconnect.)*

**Which is also why it is portable rather than platform**, and that is a correction to the obvious
reading. `sendmsg` with `SCM_RIGHTS` over `AF_UNIX` is POSIX — `<sys/socket.h>` and `<sys/un.h>` are
not on `CheckPortability.cmake`'s forbidden list, and they are not on it because they should not be.
Portable here means what [decision 6](#6-no-macos-port-development-continues-over-ssh) means — the
tests build and run on a machine with no GPU, no seat, and no compositor — and a wire codec talking
to a socket the test created satisfies that exactly. The one non-POSIX spelling in the neighbourhood
is `MSG_CMSG_CLOEXEC`, which is a flag rather than a header and has an `fcntl` fallback if it ever
needs one. `Nested` stays platform for its own reasons; the codec underneath it does not have to.

**The second argument is decision 2's standing swap.** That entry records three conditions that
reopen the in-tree server half and deliberately builds nothing on libwayland that would have to be
unbuilt. If one of them fires, the server codec's home is this module — already below both waists,
already portable, already exercised by a peer that is not a compositor. Siting the client codec
inside `Nested` would make that swap a module extraction on top of everything else it already is,
which is exactly the cost `Wire` is cheap enough to buy off now. This is
[decision 69](#69-settling-answers-with-a-wake-idleness-folds-a-monoid-not-an-or)'s trigger rather than a prediction: the
type has one caller, so the move costs nothing today.

**Rejected: the codec inside `Nested`, extracted if `Protocol` ever wants it.** The honest version of
the position above, and it loses on the test argument alone. It also gets the tier wrong by
association — a codec inside a platform module is a platform module, and it would stop being run on
the machines where it is cheapest to run it.

**Rejected: `Seam`.** It is an interface with more than one implementation only in the sense that a
socket has two ends, and nothing crosses the control waist here — `Frame` never names a connection,
and the composition root never hands one to a renderer. `Seam`'s rule is a test, not a home for
anything shared.

**Deferred: whether the generated bindings live here.** The build-time generator is a host tool and
its output is per protocol; whether the descriptors it emits are `Wire`'s types or a generated
module's is a question about the generator, and there is no generator yet. What is settled is where
the *runtime* codec lives, because that is what decision 81's revision needs a home for.

### 120. A nested output's targets are exported from the Vulkan device, and the allocator moves to `Seam`

*(Decided 2026-08-22, on asking what stands a nested output's target ring up, given that unlike KMS
there is no GBM device handed over and unlike WSI nothing allocates on gyro's behalf.)*

**The Vulkan device exports a nested output's dmabufs, and `IDmabufAllocator` moves from
[Virtual/Allocator.h](../Source/Virtual/Allocator.h) up to `Seam`.** Two answers because the second
is the only way to spend the first: a presenter owns its targets, `Nested` may not name `Render`, and
the composition root is the only thing that knows both.

**Export reuses machinery that already exists, which is most of the argument.**
[Source/Render/Device.cpp](../Source/Render/Device.cpp) already requires
`VK_KHR_external_memory_fd`, `VK_EXT_external_memory_dma_buf`, and
`VK_EXT_image_drm_format_modifier`, and already queries `VkDrmFormatModifierPropertiesListEXT` for
what a format can be tiled as. What export adds over the import path the renderer runs today is
`VkExportMemoryAllocateInfo` on the allocation and one `vkGetMemoryFdKHR` — the same image, the same
modifier query, the fd going out instead of coming in.

**[Decision 102](#102-a-virtual-output-allocates-the-buffers-it-hands-out-and-that-is-what-stands-the-renderer-up)
rejected exactly this, and every clause of that rejection is about scanout.** It reads: the chip that
scans out is not always the chip that draws, the modifier set is an intersection with the display
plane's, and GBM's scanout usage is what makes an allocation eligible for a framebuffer at all. A
nested output never scans out. There is no plane, no `AddFB2`, and no second chip in the path — the
consumer is a compositor importing a buffer, which is the same thing every Wayland client on the
machine already does. What constrains the modifier instead is `zwp_linux_dmabuf_v1`'s feedback: the
host sends a format table and per-tranche target devices, so gyro is *told* the constraint rather
than having to infer it. Decision 102's objection was that a `VkImage` cannot carry a constraint
nobody stated; here it is stated on the wire.

**One clause of that rejection is wrong on the protocol and is marked there.** It says "nested there
is nothing to export into, since a target is a `wl_buffer` the host handed back". `zwp_linux_dmabuf_v1`
has no allocation request. `zwp_linux_buffer_params_v1.add` takes an fd the *client* already owns and
`create` / `create_immed` turn those fds into a `wl_buffer`, so what the host hands back is a handle
to gyro's own memory. There is something to export into, and exporting is the only way to fill it.

**Decision 102's narrower objection reverses under nested, which is the part worth having.** It said a
renderer verified against images it allocated for itself is verified against the one configuration
that cannot fail. Under nested the importer is the host — mutter, then a wlroots compositor, neither
of them gyro's code — so an exported image with a modifier the host will not take fails visibly and
immediately rather than passing a fixture. This is the first target set in the tree whose consumer
gyro did not write.

**The tension this creates is real and is resolved by moving the interface up rather than by routing
around it.** [Virtual/Allocator.h](../Source/Virtual/Allocator.h) argues an allocator must not appear
in `Seam` because *nothing outside this module ever names one*. That was a statement of fact, and it
has stopped being true — which is the rule working rather than eroding.
[Structure.md](Structure.md#the-modules)'s test for the control waist is *more than one
implementation and the data crossing it*, and both clauses now hold: two implementations, `udmabuf`
and the Vulkan export; two consumers in different modules, `Virtual` and `Nested`, neither of which
owns either implementation; and the composition root the only thing that knows both sides, which is
what [orchestration](Structure.md#orchestration) says a seam is for.

**The waist is already carrying the description, so what moves is small.**
[Seam/RenderTarget.h](../Source/Seam/RenderTarget.h) already holds `PixelFormat` with a DRM modifier,
`DmabufPlane`, and `DmabufImage`, in the portable tier. What comes up with the interface is the
*owning* half — `DmabufBuffer` and `Mapping` — which is a `Core::Fd`, a pointer, and a length, and
names no platform header. Constructing one stays a platform module's business, because only a
platform module implements the interface.

**Rejected: the composition root allocates and hands the buffers down.** The alternative that keeps
`Seam` narrow, and it puts a window resize on the wrong thread. A nested output's target set is
reallocated on `xdg_toplevel.configure`, which arrives on the host connection that
[decision 81](#81-a-source-is-pumped-by-one-thread-nested-opens-one-connection-pumped-by-the-frame-thread)
now has the frame thread pumping;
[Seam/Presenter.h](../Source/Seam/Presenter.h) already says the backend allocates or imports, and
`Targets()` is empty between `TargetsInvalidated` and `Reconfigured`. Routing the reallocation
through the root means that window is held open until a thread that owes no deadline is scheduled —
so dragging the nested window's edge leaves the contents blank for as long as that takes, on the
backend that is the daily driver. It also costs module edges the other answer does not: `Render`
and `Nested` both already depend on `Seam`, so moving the interface up adds none, while
root-allocates needs the root to grow a per-resize path and `Nested` an inbound verb it otherwise
has no use for.

**Rejected: GBM.** Decision 102 deferred it because it costs a pkg-config entry for tiled modifiers
worth exercising against a real display plane, and nested has no display plane — so under this
backend it buys nothing at all and the deferral gets easier rather than harder. It comes back with
the DRM backend or not at all.

**Rejected: udmabuf.** It is what `Virtual` uses and it is linear host memory, which a discrete GPU
may refuse to import outright; where the host accepts it, it accepts it by copying, so the daily
driver would run every frame through a detour that exists nowhere else. Decision 102's permission
note points the same way: `/dev/udmabuf` is `0600 root:kvm` and reachable only through a seat-local
ACL, which is precisely the environment a nested session may not have.

**What must be checked at bind rather than assumed.** The host's dmabuf feedback names a main device
and per-tranche target devices, and nothing guarantees they are the device
[Device.cpp](../Source/Render/Device.cpp)'s ranking selected — a laptop with two GPUs is the ordinary
case, not the exotic one. The tranche's device is the constraint, and a mismatch is a condition to
report at `BindTargets` with both device names in the line, not one to discover as a host protocol
error three frames later.

### 121. `--backend=dump` is a backend, and the frame it writes crosses to a writer thread as a copy

*(Decided 2026-08-22, on wanting something that paces frames and shows what was drawn before there is
a panel, a protocol, or a scene author. The chicken-and-egg is the reason it comes first: the thing
that will animate needs a monitor to animate against, and this is the monitor.)*

**A fourth backend, wiring `Virtual` to `Blit` with [Virtual/Dump.h](../Source/Virtual/Dump.h) as the
consumer: one PAM per presented frame, named for the frame's own sequence.** The presenter allocates
through `HeapAllocator` and hands out mapped faces, the CPU renderer composites into them, and a sink
copies each frame out and posts it to a writer thread. Nothing on the path needs a GPU, a seat, a
Vulkan ICD or `/dev/udmabuf` — which matters because the machine where somebody most wants a picture
of what gyro drew is usually the one with no screen attached to it.

**It paces, and that is the half that is not about pictures.** A virtual output has a period and a
phase and retires on release, so the frame loop meets real backpressure at a real cadence rather than
a simulation of one. Two outputs at 60 and 30 produce frames in a 2:1 ratio without anything being
told to; a 60 Hz single output runs 39 frames in 660 ms of wall clock. Headless cannot do this job —
its renderer charges a cost and draws nothing, which is exactly right for the schedulability sweep and
useless for seeing whether a fold looks correct.

**The handoff is a copy, and that is what makes it admissible now.**
[Virtual/Sink.h](../Source/Virtual/Sink.h) had already named *a sink that hands a frame to another
thread* as deliberately not built, and the reason it gave was that a real recording consumer wants the
**descriptor**, decoded on its own thread — which races the frame thread's `AcquireTarget` on the same
ring. A copy races nothing: the image goes back to the output inside `OnFrame`, exactly as
`CapturingSink` returns it, and what the writer owns afterwards is bytes in the sink's own slab. So
the deferral stands for the case it was written about, and this is a different case that happens to
sit next to it. The queue is a single-producer single-consumer ring of monotone counts over a
preallocated slab — a release store on one side, an acquire load on the other, no read-modify-write
between them, and no allocation after `Open`.

**A full queue drops the newest frame and counts it, rather than making the frame thread wait.** The
alternative turns a slow disk into a stutter, and an instrument that reads its own weight is worse
than one with holes in it — especially since the holes are *visible*: a file is named for the frame's
sequence, so a gap in the numbers is a gap in the run, and the root warns with the count and names the
remedy. The remedy is a slower output rather than a deeper queue, because a queue absorbs a burst and
this is a rate: 1080p60 is 498 MB/s, which the page cache swallows and a 4K panel would not.

**Rejected: writing from the frame thread.** The dump forces `SCHED_FIFO` off anyway, so nothing
hard-breaks — but [Open.md](Open.md)'s *spdlog async sink* entry is the same hazard, a file write from
the frame path punts to io-wq and comes back as a frame gyro missed, and the one backend whose job is
to show what the timing produced should not be the one that perturbs it.

**Rejected: `--backend=virtual --dump=DIR`, naming the backend for the module.** It composes better on
paper — the destination picks the sink, and `--record=out.mp4` needs no second backend name. It is the
wrong axis: screen sharing and recording will each want their own target ring depth, their own format,
and their own answer to backpressure, so they are separate backends over a shared presenter rather
than sinks behind one name.

**Rejected: a lossless mode that blocks until the writer catches up.** It would give a scene author a
guaranteed-complete record, which is the thing the next consumer of this actually wants. It is not
built because the cadence is already the knob — `--output=1920x1080@10` is 83 MB/s and any disk keeps
up — and a mode that stops the compositor pacing is a different instrument wearing this one's name.
`FrameDump::Flush` is the barrier if that turns out to be wrong; making it a policy is a line.

**Rejected: a third verb on `IEventSource` so the root need not know which backend it built.** This is
[Open.md](Open.md)'s *whether a source can answer when it will next have something*, and a second
backend is not what settles it — `VirtualDevice` is a clock-driven fake exactly as `HeadlessDevice`
is, so the verb would still exist for the fakes alone, which is
[the test for whether a seam is real](Structure.md#orchestration) run in reverse. The knowledge lives
on a root-local `IBackend` instead, one virtual call wide. That entry settles when nested lands, since
a host's frame callback is the first *real* source with a genuine answer.

**What it draws today is one black frame, and that is the correct answer rather than a defect.** There
is no producer on the snapshot ring, so the evaluator walks an empty scene, `Blit` clears the first
frame's whole-output damage, and nothing damages anything after that — so the loop settles and stops
presenting, which is
[doing nothing must cost nothing](Architecture.md#doing-nothing-must-cost-nothing) reached. Every
frame after the first arrives when there is something to author.

### 122. The wake fold is scene-wide and replicated per output; a settled channel retires where it is published

*(Decided 2026-08-23, against the serializer and the frame loop at once. Answers the geometric half of
[Open.md](Open.md)'s *settling thresholds* with numbers rather than a form, narrows the non-geometric
half, and spends part of [decision 69](#69-settling-answers-with-a-wake-idleness-folds-a-monoid-not-an-or)'s
partition on purpose.)*

**Nothing gyro published was ever drawn, and it took three absences to make that true.**
`SceneSerializer` staged every run except the wake schedule; `FrameLoop::SceneWake` reads a schedule
whose length does not match the output count as `Never()`; `Never()` is `Settled`; and `Wants` gates
*evaluation* rather than presentation. So an animating scene drew whatever damage an invalidated target
happened to buy and then sat still while the springs it had published ran to completion unseen. Each of
the three is defensible alone — decision 84's rule about run lengths is right, and `Wants` gating
evaluation is decision 94's whole point — which is why the failure survived every unit test in the
tree. It is the shape [decision 76](#76-cadence-authority-follows-predictability-not-foreground) already
warned about from the other side: a fold whose contributor list is incomplete answers *settled* and is
believed.

**The fold is over the scene and the answer is replicated to every output.** Decision 69 makes `Sooner`
a monoid so the fold *may* be partitioned per output, and the example both it and
[Animation.md](Animation.md#settling-answers-with-a-wake-not-a-boolean) give is a cursor blinking on one
panel and not the other. That example is a contributor attached to an **output**, and every such
contributor still partitions exactly: an idle timeout, a `wp_fifo_v1` pairing, the recovery console's
blink. The contributor that exists today is attached to a **node**, and partitioning it means knowing
which outputs a node reaches *while it moves* — a swept screen-space bound, composed through the
transform chain, per node, on the dispatch thread. That is the walk
[Frame/Evaluator.h](../Source/Frame/Evaluator.h) performs a few milliseconds later with the output's own
placement and its predicted presentation time in hand, run a second time on the side that is called at
input rate rather than at frame rate — which is the wrong side of the trade
[Open.md](Open.md)'s *publication pacing* entry is about.

**What that costs is a whole composite on a panel with nothing moving on it, for the length of every
animation on the panel beside it, and it is worse than a wasted watt.** Two outputs share one GPU queue,
and [decision 29](#29-outputs-are-periodic-real-time-tasks-the-test-allocates-effect-budget)'s
`deviceFreeAt` threads that serialisation through the loop — so the frame nobody asked for on the idle
panel pushes back the frame somebody is watching on the active one. This is stated rather than softened
because it is the reason the entry stays open: the honest version of this decision is *not yet*, not
*never*.

**Rejected: computing the bound dispatch-side anyway.** It is not merely expensive, it is expensive and
still approximate — the bound that matters is swept over the spring's whole trajectory, since a window
crossing the seam intersects the output it is leaving and the one it is arriving at, and neither the
model value nor the presentation value at the instant of publication is that set. So the price is a
second transform walk per commit *plus* an envelope derivation per channel, to compute conservatively
what the frame side computes exactly for free.

**Rejected: narrowing after the fact through the return channel.** The frame walk already knows, per
output, whether anything it drew was moving; `FrameReport` has a spare word and
[decision 83](#83-dispatchs-publication-is-an-event-source)'s channel could carry it back. It is the
cheapest correct mechanism visible and it is a feedback loop: dispatch would narrow an output's wake on
the strength of a frame that has already been drawn, which is one frame stale in the direction that
freezes a motion rather than the direction that wastes one. Worth building when there is a second
observation to justify the loop; not worth building for its first.

**A channel that has settled is retired in the serializer, on the visit that decides whether it
crosses.** [Animatable.h](../Source/Animation/Author/Animatable.h) deferred this to *the publisher that
owns the array* on the grounds that writing the hook before the array existed would fix the interface
from the wrong end; the array is now the serializer's runs. The two questions turn out to be one
question — *is this channel at rest* decides whether a coefficient crosses, and *has it settled* is the
same question asked with the thresholds in hand — so they are asked together, per channel, in one
branch. That is what makes the two bad states unwritable rather than checked: a coefficient published
with a `Settled` wake is a scene that never idles, and a wake published for a coefficient that was
dropped is a scene that stops mid-motion, and neither is reachable when one branch decides both.

**What it costs is `Serialize` taking a non-const store**, which was the real question rather than a
line to add. The alternative is a pass of its own over the entity array, which touches every entity
twice per publication and, worse, leaves a caller able to run one without the other. `SceneStore`
therefore has a second friend, and the reason it is admissible is that
[decision 89](#89-a-commit-resolves-in-two-phases-a-change-becomes-motion-where-its-inputs-are-complete)'s
objection to a writable entity is that a caller could *start* a motion outside a transaction: what the
serializer does is end one that ended by itself, and `Animatable` gives it no verb for anything else.

**Retirement is invisible, and that is an arithmetic claim rather than a hope.** `Animatable::Settle`
puts the property on its model value, and the property is inside the position threshold by construction
at the moment it is called — so the published value moves by less than
[decision 54](#54-settled-geometry-snaps-to-the-outputs-device-grid)'s snap to the device grid moves it
on the very next frame. The two happen at the same instant for the same reason.

**Without a third piece the change swaps one broken invariant for its mirror image.** A scene is
published when a commit resolves, and a free-running animation commits once. So the snapshot the frame
thread holds says *every frame, forever*, nothing ever contradicts it, and the compositor that used to
draw one frame and stop would instead draw every frame and never stop. `SceneSerializer::Republish` is
the missing number: the earliest instant at which some active channel comes to rest, folded by the same
monoid and answered as `Timed` rather than `Continuous`, because dispatch has exactly one thing to do
while an animation runs and nothing to do between. It is [Animation.md](Animation.md#storage)'s *the
settle time is analytic, so nothing has to observe an evaluation* given a carrier. **Nothing arms it
yet** — there is no dispatch event loop in the tree, and inventing one here would fix its shape from a
serializer — so what exists is the fold and not the timer.
`Source/Integration/SceneIdle.Test.cpp` stands in for the loop in two lines, and stands in honestly:
it re-serialises at the instant the serializer named and never on a frame boundary.

**The settling thresholds, as numbers, because both halves above need them and neither can proceed
without.** They live in [Scene/Settle.h](../Source/Scene/Settle.h) as policy fields rather than
constants, for the reason `BudgetPolicy`'s are: every one is chosen rather than measured, and the entry
that replaces them should replace them in one place.

- **A sixteenth of a device pixel, and one device pixel per second.** The position figure is what bounds
  decision 54's snap, and the whole argument for snapping at settle rather than every frame is that the
  jump is invisible: an eighth of a pixel shifts an antialiased edge's coverage by an eighth, which on a
  black-on-white window border is a visible twitch, and a sixteenth is half of that. It costs one halving
  of the exponential — under a tenth of a second on a standard response, at the tail of a transition. The
  velocity figure is legible rather than derived: at one pixel per second the fastest panel gyro will
  drive moves a node by a hundred and forty-fourth of a pixel between frames, and on the catalog's own
  responses the two crossings land within a tenth of a second of each other, which is the sign that
  neither is doing all the work.
- **Half an eight-bit code point for opacity, and one code point per second.** Open.md is right that
  opacity has no output pixel to be expressed in, and this deliberately does not pretend otherwise: it is
  a *representability* argument, not a perceptual one. Below half a code point two values cannot be the
  difference between two pixels that leave the machine on an ordinary wire. The perceptual half stays
  open and belongs to the same review with a screen in front of it that the dressing numbers want.
- **The finest grid in the output set, rather than the finest grid a node intersects.** Decision 54 asks
  for the second and it is the same screen-space question the fold could not answer either. The
  substitution is conservative — a superset of outputs can only make the grid finer, so a node settles no
  earlier — and it costs a node on a 1× panel settling to the tolerance of the 2× panel beside it.
- **A dimensionless residual is judged at the screen's half-diagonal.** Scale and a log-map rotation
  displace a point at radius `r` by about `ε·r`, and the honest radius is the node's own composed through
  every ancestor's scale, because an overview magnifying a thumbnail magnifies its residual too. Judging
  every node at the largest radius anything can be drawn at cannot be wrong in the direction that
  freezes. It costs about three tenths of a second of extra tail on a small node that scales, which is
  the largest single price in this list and the first thing a per-node radius buys back.

**Every number errs long, and that is one rule rather than four choices.** A threshold that is too small
costs redundant composites at the tail of an animation; one that is too large freezes a motion somebody
is watching. It is the same asymmetry
[decision 11](#11-springs-are-closed-form-not-numerically-integrated)'s envelopes are built on, applied one level up,
and it is why nothing here is tuned toward the shorter tail.

**What is now asserted rather than argued.** `Source/Integration/SceneIdle.Test.cpp` runs a real store, a
real serializer, a real ring, and a real `SceneEvaluator` against a headless panel: a still scene draws
nothing and arms nothing, damage from outside buys exactly one frame and no second one, and an animating
scene is drawn every frame for about a second and then stops — with the store's springs at rest on the
model values a commit set. `Source/Integration/Schedulability.Test.cpp`'s
*ASettledOutputIsNotWokenByTheOneAnimatingBesideIt* still passes and is now the one claim in the tree
that the authoring side cannot produce: it drives a per-output schedule a test wrote. That is the cost of
this decision with a test's name on it.

### 123. A destroyed proxy keeps its dispatcher and loses its object

*(Decided 2026-08-23, on reading [decision 119](#119-the-wayland-wire-codec-is-its-own-module-wire-and-it-is-portable)'s
module into the tree and finding that an event for an object the client had already destroyed ended
the connection.)*

**An unbound id keeps its dispatcher until `delete_id` frees it, and the runtime calls that dispatcher
with a null `self`.** The arguments are read and thrown away; only the call into the proxy is skipped.

**What this costs a person is a nested output going black in the middle of a session, with nothing
in front of it to explain why.** Both ends of a connection talk at once, so the host has an event for
an object on the wire before it has read the request destroying that object — every time, for one
round trip. Treating that as a protocol error means the ordinary lifecycle of a `wl_callback`, a
presentation feedback, or a buffer release ends the connection, at a moment that depends on how the
two processes were scheduled. It is the failure this project cannot absorb: it ships as a window that
stops updating and a log line naming the *symptom*.

**Skipping the message is not available, and that is the whole difficulty.** A Wayland message frames
its own bytes — the header's size word says where the next one starts — so bytes can always be
stepped over. Descriptors cannot. They arrive out of band on `SCM_RIGHTS`, attached to whichever
`sendmsg` carried the first byte of their message, and they come off a queue in the order the
receiver's demarshalling asks for them. Nothing in that queue says which message an entry belongs to.
So a message whose arguments go unread leaves the queue one entry out of step, and the *next* message
takes a descriptor belonging to the one thrown away — a real, open descriptor for the wrong buffer.
The compositor draws somebody else's window contents and no check anywhere fires. Reading the
arguments is what takes the descriptors off the queue and closes them, so the message has to be read
whether or not there is anything left to hand it to.

**The dispatcher is the only thing that knows how**, which is
[decision 2](#2-gyro-owns-the-protocol-seam-libwayland-implements-the-server-codec)'s split showing up
as a constraint rather than a convenience. `Wire` has no signatures by construction — it cannot know
that opcode 3 on this object takes a descriptor — so the decode has to come from the generated
bindings, and the generated bindings are a function pointer and a `void*`. Keeping the pointer and
dropping the object is the smallest thing that keeps the decode reachable after the object is gone.

The obligation this puts on the generator is one line per event. The emitted code already reads its
arguments into locals and then makes one call, so the guard goes around the call:

    const std::uint32_t name = wireReader.GetUint();
    const std::string_view interface = wireReader.GetString();

    if (wireListener != nullptr)
    {
        wireListener->OnGlobal(name, interface);
    }

**Rejected: a second generated function that reads and discards.** The honest version — `Unbind`
takes an explicit skip dispatcher, and nothing is ever handed a nullable `self`. It states the
contract in the type system, which is the better property, and it loses anyway: two decoders for one
interface drift apart the first time an argument is added to one of them, and the symptom of that
drift is the misaligned descriptor queue this entry exists to prevent — silent, delayed, and
attributed to the wrong message. One decoder and one branch cannot drift. What the rejected version
would have bought is a null dereference becoming impossible; what it costs is that the failure it
replaces is loud and immediate while the one it introduces is neither.

**Rejected: discarding the message, as libwayland does.** libwayland can, because it holds every
interface's signature and closes exactly the descriptors the discarded message carried. Reproducing
that here means putting the signatures in `Wire`, which is the thing decision 2 spent an entry not
doing.

**Rejected: never unbinding a proxy the host may still be eventing.** Pushes an unanswerable question
onto every call site — no client knows what is in flight — and the answer would have to be "keep
every proxy alive for one round trip", which is the zombie this decision already is, with the
bookkeeping moved somewhere it cannot be done correctly.

**A message may carry at most twenty-eight descriptors, refused where it is marshalled.** The same
invariant from the sending side. `SCM_RIGHTS` is limited to what one `sendmsg` carries, so a burst of
requests holding more than that has to be split — and the split has to fall on a message boundary,
because a message reaching the far end ahead of its descriptors is the same queue misalignment read
backwards. A single message with twenty-nine descriptors of its own offers no boundary to split at:
it would go out whole with twenty-eight behind it. `MessageWriter::Send` refuses it, latching on the
buffer the way the 16-bit size cap already does, so the batching clamp is total rather than true in
practice. No protocol in the set gyro speaks comes close — four planes is the most any request asks
for — and the point is that the clamp is now provable rather than lucky.

**Two things were wrong underneath this and are worth recording, because both were invisible.** A
descriptor's message boundary was doubling as a *committed* flag, with zero meaning "not yet"; the
offset rebasing that happens when a sent prefix is reclaimed could land a live boundary back on zero,
at which point the next message adopted the descriptor and sent it one message late. Committed is a
count now, which cannot be rebased into meaning something else. Separately, an id that was allocated
and never bound was retired rather than freed — but nothing on the wire had ever named it, so no
`delete_id` was ever coming and the id was stranded for the life of the connection. It frees
immediately now, which rests on the rule that a proxy is bound before its id is marshalled; that rule
is stated in `Wire/Connection.h` rather than enforced, because the request that would break it is
generated code this module never sees.

**What is now asserted rather than argued.** `Source/Wire/Connection.Test.cpp`'s
*AnEventForARetiredProxyIsReadAndDropped* sends two events each carrying a descriptor, the first for
an object already unbound, and checks that the *second* object received its own descriptor — which is
only true if the dead object's came off the queue first. *DeleteIdClearsTheRetiredDispatcher* checks
that the zombie window closes when the host says so, and that an event after it is a connection error
again. `Source/Wire/Codec.Test.cpp`'s *RefusesAMessageCarryingMoreDescriptorsThanOneSend* and
*TwentyEightDescriptorsInOneMessageStillGo* are the send limit as a boundary rather than a ceiling
somebody guessed at.
