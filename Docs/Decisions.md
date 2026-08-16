# Decision Log

Design decisions with their rationale and, more usefully, the alternatives that were rejected and
why. Decisions 1–20 recorded 2026-08-10; the session model and presentation timing 2026-08-15;
effects and frame integrity later the same day, together with revisions to 22, 29, and 30. Boot,
rendering devices, and login 2026-08-16; session identity later the same day, revising 23; the
thread split (45) later still, revising 2, 3, 29, 36, and 40; then snapshot storage (46), revising
20 and 41; then colour (47 and 48), revising 2, 22, 24, 27, 29, 34, and 40; then the restart
boundary (49), revising 2, 27, 39, and 45; then the ownership of the world (50) and of the shell
(51), revising 13, 14, 16, 25, 33, 43, and 45 again; then geometry (52–56), revising 17, 18, and
28; and the timebase (57) with idle and power (58 and 59) last, annotating 7, 11, and 26. All of it
before implementation.

Sections are thematic and numbering is chronological, so a later section is not a lower-level one —
it is simply where the argument had reached. Cross-references are the structure that matters.

Where a decision has been revised, the superseded position is kept as a rejected alternative rather
than deleted — the reasoning that led somewhere wrong is the most useful part of a decision log.

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

### 2. The wire protocol is implemented in-tree, but not first

Both halves, with typed C++23 bindings generated at build time — and **libwayland is an admissible
interim**, because nothing this decision settles has to be settled before the code that would depend
on it exists.

*(Revised twice on 2026-08-16: this decision has outlived two rationales, both kept below, and its
sequencing has since moved as well.)*

**The decisive argument is that gyro's restart boundary is ruinously expensive.** libwayland
resolves allocation failure and internal invariant violations by calling `wl_abort()`, and compiles
in assertions whose disposition depends on how the distribution built it. For a session compositor
that is a crash, one lost login, and a greeter that restarts it. For gyro it is every client on the
machine dying at once, at a moment a linked library chose, with no way to intercept, degrade, or
contain it — on a system that
[decision 37](#37-gyro-owns-the-display-from-firmware-handoff-onward-there-are-no-vts) has
deliberately left with no VT to fall back to, and whose recovery console is gyro itself. The precise
form of that claim is
[decision 27](#27-resource-accounting-is-attribution-not-per-user-fairness), which is where this
document states it once and where it was overstated for this design's first pass. Every other
decision about failure points the same way: 27 sizes limits to survive rather than to ration, and
[decision 41](#41-device-migration-is-exercised-on-every-boot) keeps the last frame on glass through
device loss. Linking a library that terminates the process unilaterally contradicts all of it.

Three supporting arguments, none sufficient alone:

- **A per-client dispatch budget.** libwayland offers no lever for "process at most N messages from
  this client". Under [decision 45](#45-protocol-dispatch-is-a-thread-not-a-task) one thread serves
  every client on the machine, so fairness between them is gyro's problem. *(Weakened 2026-08-16.)*
  This once read "a lever that does not exist", which is too strong: removing a client's event
  source from the loop and letting the kernel socket buffer apply backpressure is a real lever. It
  is coarse — it bounds a client's share over many dispatches rather than the cost of any one
  message — and on a thread nothing above it waits on, coarse may well be enough. What survives is
  a preference, not a requirement.
- **No inherited process-global SIGBUS handler.** libwayland guards compositor `wl_shm` access with
  a `sigsetjmp` / `siglongjmp` trampoline behind a signal handler it installs process-wide on our
  behalf. Owning the protocol means owning shm truncation, which is a cost below — but the
  alternative is not free, it is the same hazard handled by a mechanism gyro inherited rather than
  chose, inside a multi-threaded real-time process.
- **No intrusive C object model at the boundary.** `wl_list`, `wl_signal`, and `wl_listener` with
  static trampolines put lifetime management in a linked list that every C++ type must remember to
  unlink itself from. Destroy-listener use-after-free is the best-known bug family in compositors
  built this way.

**What narrows the choice.** Decision 45 has the frame thread read published state, and a
`wl_resource` is destroyed synchronously on the dispatch thread when its client goes away — so a
`wl_resource` can never be the thing the frame thread holds. gyro needs its own shadow object model
with its own lifetime discipline whatever sits underneath. So this is not a choice between
libwayland's object model and ours. It is narrower: **do we write a demarshaller and a socket
manager?** That is the mechanical, well-specified part — and it is also where most of the cost
estimate lives.

**And what defers it.** That the shadow object model is required either way makes it a *seam*, and a
seam is exactly what turns libwayland from a foundation into a replaceable implementation of the two
mechanical pieces above. Nothing is built on it that would have to be unbuilt. Two supporting
positions had already collapsed on the same day and neither has been re-argued since:
[decision 3](#3-io_uring-event-loop-via-liburing) retracted the io_uring coupling, and decision 45
removed the real-time framing. What is left is a decision with no dependants, whose cost is
concentrated in the part that can be written last, and whose decisive argument is
[still unverified](#open). Committing up front buys nothing that committing later does not, and it
forecloses the outcome where the verification comes back weak.

So the sequencing is: the shadow object model and the seam from line zero; the client half next,
which the staging below already puts first and which
[decision 1](#1-the-nested-backend-drives-raw-wayland-protocol-not-vulkan-wsi) gives an immediate
consumer; and the server half when the abort reading says it is worth writing. A project that never
reaches the third step has lost an ambition and nothing else. One that commits first and reads
afterwards has spent the largest single line item in the estimate on a premise it did not check.

**Rejected: the priority-inversion rationale.** Recorded originally as the decisive argument: that
libwayland's per-message allocation exposed the `SCHED_FIFO` thread to a malloc arena lock held by a
normal-priority client thread. Clients are separate processes with separate glibc arenas, so that
mechanism cannot occur at all.

**Rejected: the repair, that client traffic drives allocation on our own real-time thread.** True as
stated, and the form the argument should always have taken — not *their* lock blocking us, but
*their* message volume deciding how often our frame thread enters the allocator. It was overtaken
within a day by [decision 45](#45-protocol-dispatch-is-a-thread-not-a-task), which takes dispatch
off that thread entirely; allocation on the dispatch thread costs one client some latency and costs
the frame nothing. Both failures are recorded at length because the pattern is the lesson: the
real-time framing was load-bearing for two rationales and was never what this decision was actually
about.

**Rejected: libwayland** — but on a materially narrower margin than this entry once claimed, and
the grounds are entirely above rather than anywhere in the real-time design. See
[decision 3](#3-io_uring-event-loop-via-liburing) for why the io_uring coupling — "the two decisions
are one decision" — does not survive either.

**Cost accepted, at the point it is accepted:** 3–6 weeks to parity plus an interop tail; SIGBUS
handling for `wl_shm` pools becomes ours; and continuous fuzzing of the demarshaller becomes a
requirement rather than a nicety, because this is where untrusted input from every account on the
machine terminates. Staged client-half-first against mutter, with wlroots as a second target. The
deferral above reduces none of it. It moves all of it behind a question that costs an afternoon to
answer.

**Open: the reachability of libwayland's abort paths is unverified**, and the decisive argument
rests on it. What is wanted is a count of `wl_abort()` and assertion sites in `wayland-server`
reachable from ordinary operation rather than from programmer error, and whether any is reachable
from client input rather than only from allocation failure. If the answer is "allocation failure
only, and gyro is dead in that case regardless", this falls back to the three supporting arguments —
now two and a preference — and the margin over libwayland is then thin enough that the interim
becomes the answer.

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

**Rejected: `wl_event_loop`.** Makes the frame deadline just another timer competing with client
traffic. **Rejected: own epoll loop.** Fine, and would deliver most of the real-time benefits via
decision 2 alone — but having decided to own the socket I/O, io_uring is the natural consequence.

**Rejected: "the two decisions are one decision."** Recorded in decision 2 originally, and the
mechanism is real: libwayland does its own buffered `recvmsg` / `sendmsg`, so nesting it inside
io_uring carries the complexity while forfeiting the benefit. The conclusion drawn from it was not.
libwayland nests perfectly well via `wl_event_loop_get_fd()` polled through io_uring, at the cost of
a wakeup per dispatch and the loss of batching on socket I/O — and on the dispatch thread, which is
where decision 45 puts that traffic, neither of those costs anything that can be measured against a
frame. The two decisions are independent, and decision 2 no longer draws support from this one.

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
[decision 2](#2-the-wire-protocol-is-implemented-in-tree-but-not-first) was originally written
about. A hostile or merely chatty client now cannot reach the frame thread at all, which is a
stronger property than bounding what it costs us.

**Input is on the dispatch thread too, drained first.** It arrives from evdev rather than from
clients, but it is the same kind of work — external data, unbounded library calls, a publication to
the frame thread — and it belongs on the same side of the boundary. Ordering within the thread is
input, then client traffic under a per-client budget, which is decision 2's deterministic-dispatch
argument finally landing somewhere it applies.

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

`Core/`, `Scene/`, and `Anim/` stay free of Linux headers regardless, so portable unit tests build
anywhere. That is free and worth doing on its own merits.

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
convention backed by review and a greppable `Motion::Custom`.
[Decision 51](#51-the-shell-is-a-per-session-client-gyro-owns-mechanism) puts the shell on the far
side of a protocol whose vocabulary has no way to express a damping ratio, so the closure is
structural for every caller outside gyro itself.

Reduced motion is a first-class dimension of each bundle, not a scalar — properly implemented it
replaces movement with cross-fades rather than making springs faster.

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
machine, using a hand-written codec, and its failure takes every session on that machine with it.
That is the worst available candidate for ambient privilege. `CAP_SYS_NICE` is not a step onto that
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
receive motion events and reply with positions, it declares that an entity tracks the pointer under
`Motion::Interactive` until release, and gyro runs that at device rate with no round trip at all.

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
sites cannot name spring parameters" is a convention backed by review and a greppable
`Motion::Custom`. Across a protocol, the shell has no way to express a damping ratio at all.

The second argument for the closed set is shell restart. With known node kinds gyro can keep showing
windows under default policy while the shell is upgraded or respawns, and the session stays coherent
through a hiccup; with anonymous nodes gyro holds a pile of things it cannot interpret and must
either freeze or drop them. **That closed set is the floor policy** — the vocabulary that lets a
shell be replaced is the same one that lets gyro run without one, which is
[decision 34](#34-effect-quality-is-a-tier-gyro-chooses-and-the-floor-tier-is-the-recovery-path)'s
argument arriving in a second place.

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
  the expensive restart, which is most of what decision 2 is about.
- **The header carries the colour state**, since every surface has one under
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
[open list](#open) carries against
[decision 26](#26-remote-presentation-is-a-virtual-output-with-client-supplied-targets) — a remote
machine's clock is a foreign domain, and with this decision it is converted by one estimator at one
ingest rather than leaking inward to be discovered later as "remote feels strange".

### 29. Outputs are periodic real-time tasks; the test allocates effect budget

Outputs are genuinely independent. The only thing coupling them is one frame thread and one GPU
queue, so this is the classic periodic task problem and is treated as exactly that: each output has
a measured period `P` and an execution time `C`, deadline equals period, scheduling is
earliest-deadline-first, and composites are non-preemptive.

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
outputs never influence each other. One that fails is not rescued by modelling phase.

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

**Chunking** splits an output's work at render-pass and submission boundaries, reducing the blocking
term from `max(C_other)` to `max(chunk)`. Blur pass chains split naturally, which is where `C` comes
from in the first place.

**Early rendering** produces a frame one period ahead of its normal record time and holds the target
for its flip. In scheduling terms it relaxes the release-time constraint: the job may run in any
window within the period preceding its deadline rather than only the one immediately before it.

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
pending flip, being recorded.

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
on abrupt refresh changes, and keeping the phase shift below perceptibility.

**Rejected: VRR as a latency feature only**, present-when-ready and nothing else. That is what
everyone does and it leaves the one available scheduling lever unused.

**Rejected: servoing against content-driven VRR.** If a fullscreen client is driving the refresh
rate for its own reasons it wins, and admission control falls through to the next rung.

Written from specification; VRR phase control, panel ranges, and flicker thresholds are marked
`// SPEC:` where they land in code.

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

---

## Effects

Recorded 2026-08-15, alongside the revision of decisions 29 and 30. These are what make `C` a number
gyro controls rather than one it discovers.

### 33. Effects are named materials, not parameterized filter calls

A surface declares a material from a closed vocabulary — `Material::Glass`, `Material::Sidebar`,
`Material::Hud` — and gyro decides what that means this frame. Shell code cannot name a blur radius,
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

### 34. Effect quality is a tier gyro chooses, and the floor tier is the recovery path

A material renders at a quality tier. The ladder, for blur:

1. **Internal resolution of the pass chain.** The cheapest lever and very nearly invisible — the
   result is about to be blurred anyway.
2. **Pass count.**
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
[decision 2](#2-the-wire-protocol-is-implemented-in-tree-but-not-first). Owning the wire protocol
makes message handling cheap and bounded, which is worth having and is not what protects the frame.
The distinction is worth stating because the two were conflated for this design's entire first pass:
the frame is protected by what is *not on the thread*, not by what is fast.

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
[decision 38](#38-the-pre-vulkan-console-is-a-permanent-subsystem-not-a-bootstrap). Two further
consequences are not gyro's to solve and become deployment requirements instead:

- **Kernel panics become invisible.** `pstore` must be enabled so the oops is readable on the next
  boot, and gyro should surface it.
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
seamless. A crash borrows the same property: the fd goes to the service manager's file descriptor
store and comes back on restart, via `sd_notify` with `FDSTORE=1` — an `AF_UNIX` datagram carrying
`SCM_RIGHTS`, roughly thirty lines, no libsystemd, and emphatically not D-Bus, so
[decision 7](#7-session-claiming-is-deferred-basu-rejected)'s position is untouched. The coupling is
to the service manager gyro already needs for `LimitRTPRIO=`, `LimitMEMLOCK=`, and `OnFailure=`.

The consumer already exists. Decision 39's rule that gyro adopts an existing mode rather than
unconditionally modesetting named three callers and this is the fourth, so the failure becomes *the
last frame holds, then a greeter fades in* rather than *the machine goes black*, at the cost of an
affordance already committed to.

**Whether the fd store is strictly required is a `// SPEC:` question**, and it is recorded as one
rather than assumed either way. With `fbcon=off` and no other master, nothing re-modesets when
gyro's fd closes, so the CRTC ought to keep scanning out — but whether the framebuffer survives
`drm_file` teardown on the strength of the plane's reference is exactly the class of behaviour
[decision 5](#5-the-drm-backend-is-designed-from-specification-with-no-hardware-spike) says to mark
rather than trust. The fd store turns *probably* into *by construction*, which on the one process
that owns the machine's display is worth thirty lines whichever way the reading comes out.

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
most catastrophic responsibility. Decision 2's own cost column names the demarshaller as the thing
needing continuous fuzzing, because it terminates untrusted input from every account on the machine
— and it currently shares an address space with DRM master. Split it, and a demarshaller crash costs
one user's session, which is precisely the failure mode decision 2 calls acceptable for a session
compositor.

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

**Consequence for decision 2, recorded rather than left to be met later.** Its decisive argument is
that `wl_abort()` puts the machine's display going out at a moment a linked library chose outside
gyro's control. With the display surviving a restart, that sentence is weaker: what a library-chosen
abort costs is every client on the machine, not the display itself. The margin over libwayland
narrows a second time, on top of the narrowing decision 2 already records. The conclusion does not
move — every client on the machine is a great deal to lose to somebody else's `assert` — but
decision 2 is now deferred rather than committed, and this is one more reason to do the abort
reading before a demarshaller is written rather than after.

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
panel's black, and it changes colour rendition on the way down. It also collides productively with
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
[decision 2](#2-the-wire-protocol-is-implemented-in-tree-but-not-first)'s broken rationale. It is
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

**This is gyro choosing for itself what decision 2 refuses to let a library choose for it**, and the
two should be read together rather than met later as a contradiction. *(Added 2026-08-16.)* The
distinction is not that one unilateral termination is acceptable and the other is not. It is *who
decides, against what condition, and with what alternative*. The watchdog fires on a condition gyro
named, at a threshold gyro set, against the one outcome strictly worse than dying: a `SCHED_FIFO`
thread spinning on a VT-less machine is unrecoverable without a power cycle, whereas a dead gyro is
recovered by decision 37's `OnFailure=` unit at the cost
[decision 27](#27-resource-accounting-is-attribution-not-per-user-fairness) now states precisely.
What [decision 2](#2-the-wire-protocol-is-implemented-in-tree-but-not-first) objects to is
termination at a moment a third party chose, for a condition gyro did not name and cannot intercept,
where the alternative was simply not dying.

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
agent must respawn it, and gyro's lock state must be entirely independent of the greeter's liveness
— the output stays black across a respawn. Decision 25 already says gyro owns lock state and not
lock UI; that is now load-bearing rather than tidy.

**Rejected: `ext-session-lock-v1` 's model**, with the lock surface as a client of the locked
session. It is the ecosystem-standard approach and it solves the notification problem for free. It
loses the structural anti-spoofing property, it needs a correct and permanent policy about which
client may claim the lock role, and it means lock and switch-user are two transitions instead of
one.

---

## Colour

Recorded 2026-08-16, last of the pre-implementation decisions and the only section prompted by an
absence rather than by a question. Colour appeared nowhere in this log until it was noticed that
three decisions already recorded — 13, 18, and 34 — depend on an answer it had never given. It
belongs with device migration and the publication boundary in the class of things that are free at
line zero and a rewrite afterwards.

### 47. Compositing happens in linear light at wide primaries

The composite space is **linear, Rec.2020 primaries, brightness-relative with 1.0 as SDR reference
white.** Every surface carries a colour state — primaries, transfer function, alpha mode, reference
luminance — from line zero. Untagged content is sRGB *by rule*, never by inspection.

**One rule forces the rest.** Blending, scaling, mipmapping, and blur are all weighted sums of
light, so they are correct only in a space proportional to light. Everything else in colour
management is appearance matching, which is negotiable and taste-laden; this part is arithmetic.
gyro does all four constantly and blur is the feature, so there is no version of this project in
which the rule is ignored cheaply.

**Alpha is un-premultiplied before linearisation, once, at import.** Wayland's `ARGB8888` is
premultiplied and the client computed `S = encode(C)·α`, so a hardware sRGB sampler returns
`EOTF(C·α)` where the wanted value is `EOTF(C)·α`. The error factor is `α^1.2` — about 13% too dark
at `α = 0.5` — and it lands on every soft edge in the system. This is the one place where doing
colour half-way is worse than not doing it: a compositor blending in encoded space is wrong but
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
in the KMS colour pipeline — per-plane degamma, CTM, gamma, or the newer pipeline properties — or
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

**Rejected: deciding colour when HDR arrives.** The tempting position, since HDR output is out of
scope initially and nothing here is needed to light a pixel. It fails the same test as
[decision 41](#41-device-migration-is-exercised-on-every-boot)'s "no GPU resource is the only copy
of anything": composite space, target formats, and per-surface colour state are structural, they are
chosen the day the renderer is written, and every effect and animation authored against the wrong
one is re-authored. The *features* — tone mapping, gamut mapping, output characterisation, the
colour-management protocol itself — are additive on top and genuinely deferrable, and are left in
the open list as such.

**Cost accepted:** one 16-bit-float composite target per output; colour becomes a dimension of every
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
not declared a colour state, on the reading that untagged means authored against encoded-space
blending. Genuinely tempting: for a pure black source the correction is *exact* and independent of
the background, which is precisely what a drop shadow is, so it fixes the dominant case perfectly —
and it self-retires, since a client adopting the colour-management protocol gets straight linear.

Rejected because it delivers compatibility with other compositors, which is not the goal, in place
of cohesion, which is. A remapped GTK shadow beside gyro's correctly blended one looks different
from it — the same two-blending-models-in-one-frame problem, relocated rather than solved, and
[Animation.md](Animation.md#priorities) puts cohesion first. It is also exact only for black:
coloured translucency, which is most of a transparent terminal, degrades. And a compatibility remap
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

**To be confirmed by looking, not by arguing.** The composite pass is one place and the toggle is a
build flag, and [decision 4](#4-all-three-backends-are-in-scope-nested-headless-drm) makes nested
the daily driver, so this needs no hardware and no DRM backend: a GTK application on a light
wallpaper, flipped between the two. Recorded as a decision rather than as an open question because
the direction does not depend on the result — only the urgency of the mitigation does.

---

## Geometry

Recorded 2026-08-16, immediately after colour and prompted the same way: the word "coordinate"
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
here has alpha with a backdrop reading through it. That is the one thing excluded, and excluding it
is what makes all the rest affordable.

**There is no camera**, which is the non-obvious half. Perspective is a property of a node's own
transform, applied in its parent's space, rather than a frustum belonging to an output. A per-output
camera would project a straddling window differently on each output, so the window would visibly
change shape across the seam — on precisely the configuration
[decision 28](#28-the-frame-clock-is-per-output) exists to serve.

**Rotation is a quaternion sprung in the log map.** Decision 17's per-channel intent survives and
gets more honest with it: rotation is one channel with one spring, not three axes pretending to be
independent. Two guards come along — the perspective distance is clamped so the near plane never
crosses the quad, and back faces cull by default, because a window flipped past ninety degrees
showing mirrored text reads as a bug. A card flip is two nodes and a catalog transition, which is
where it belonged anyway.

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
than inferred** — `src` is `wl_fixed` and `dst` is integer, so the mapping is an exact rational and
gyro never derives a logical size from buffer dimensions and a floating-point scale. Decision 53's
rule gets a structural home instead of remaining a discipline somebody has to remember.

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

---

## Open

Carried forward, roughly in the order they will bite:

- **libwayland's abort reachability.** Decision 2's decisive argument rests on it and nothing else
  does, so it should be checked first, and it is an afternoon's reading. Count the `wl_abort()` and
  assertion sites in `wayland-server` reachable from ordinary operation rather than from programmer
  error, and establish whether any is reachable from client input rather than only from allocation
  failure. Decision 2 now defers the commitment instead of resting on the answer, so this blocks
  nothing — but it is what decides whether the server half is ever written, and answering it late
  means answering it after the moment the answer was worth most. Decision 49 narrows the margin a
  second time, which raises rather than lowers the value of reading first.
- **Tone mapping and gamut mapping policy**, in both directions, deferred by decision 47 as
  additive. The SDR-on-HDR direction is where every shipping system has gone wrong, and the rule is
  that SDR white maps to a reference — BT.2408 says 203 nits — or to a stated user preference, and
  never to display peak. HDR-on-SDR needs a curve chosen rather than inherited. Neither is urgent;
  both are easy to get subtly wrong and hard to notice afterwards.
- **Blur order against tone mapping.** Blurring in linear and tone mapping the result is correct and
  costs a full-resolution map after the chain; tone mapping first and blurring afterwards is cheaper
  and temporally steadier and is wrong. A real number attached to a real artefact, so it wants
  measuring rather than arguing — the same standing as the virtual-output colour question below.
- **Blur across colour-state boundaries.** `Material::Glass` samples a backdrop that may hold an HDR
  video window beside an SDR text editor, and physically correct linear blur bleeds a 1000-nit
  highlight through the glass into the region over the SDR window. Correct, and startling. There is
  no obviously right answer, which is what makes it a decision rather than an implementation detail.
- **Mip generation's place in `C`.** Decision 56 makes minification the common case rather than the
  exception, so a mip chain is rebuilt for animating client content on the frames it changes — real
  bandwidth, roughly a third of the surface again, on the budget decision 29 defends and which does
  not know about it yet. It also wants a rule for when the chain is worth building at all, since a
  surface minified by 1.05 does not need one and a surface minified by 2 does. A measurement, not an
  argument.
- **Per-output characterisation.** The inverse of the display's measured behaviour belongs at the
  very end of the pipeline and preferably in KMS hardware. EDID routinely misdescribes the panel, so
  a user-supplied profile has to be possible, which implies a configuration surface and somewhere to
  put an ICC file — on a system-layer process where "the user" is not yet resolved at the time the
  first output lights up.
- **Publication granularity across outputs.** Decision 45's boundary is wait-free, so the frame
  thread could acquire client state once per iteration or once per output. Architecture.md takes
  once per iteration, on the grounds that a window straddling two outputs would otherwise show two
  different client frames in one iteration — visible on exactly the configuration decision 28
  exists to serve. Per-output acquisition is strictly fresher, and the trade has been reasoned
  rather than measured.
- **Publication pacing**, which is the producer-side twin of the item above and is stated at the
  foot of
  [decision 50](#50-the-world-is-authored-on-the-dispatch-thread-the-snapshot-carries-coefficients).
  Eager per resolved commit serializes the scene once per input event; pacing to the fastest
  output's period spends up to a period of gesture latency, which is the worst currency available. A
  copy-on-write arena makes eager cost proportional to the dirty set and is the likely answer, but
  the first cut should be the naive one and the question should be settled by measurement.
- **The shell's scene vocabulary.** Decision 51 commits to a closed set of node kinds — surface
  reference, snapshot reference, solid, effect layer — and that list is a sketch rather than a
  design. It is the same problem as the material vocabulary below and wants solving with it and with
  the motion catalog, since a node, the material that dresses it, and the transition that reveals it
  are one design problem seen three ways. The test named in decision 51 is the constraint: a shell
  must not be able to produce motion that does not match the catalog.
- **What the shell declares for continuous manipulation.** Decision 51 keeps drag, resize, and swipe
  inside gyro on the strength of the shell declaring constraints ahead of time — minimum and maximum
  sizes, snap targets, tiling gravity, and whatever else turns out to be needed. That set is not
  enumerated, and enumerating it is what decides whether the rule holds or whether the first awkward
  case adds a per-event request and quietly undoes it.
- **Colour format for virtual outputs.** Encoders want NV12 or P010, not RGBA. The agent can convert
  (an extra full-frame pass and its bandwidth), or gyro can fold RGB→YUV into its final composite
  pass (much cheaper, but the renderer grows a YUV output path it otherwise would not have), or both
  can be offered. HDR sharpens it — P010 and transfer functions. This is the one part of decision 26
  with a real performance number attached and it should not be decided from the armchair.
- **Clock offset for injected input.** Decision 26 keeps the claim that `t₀` is the event timestamp,
  but a remote client's timestamps come from another machine's clock. Used naively they start
  animations in the past or the future. Needs an offset estimate, and it is the kind of thing that
  ships subtly wrong and presents forever as "remote feels strange". Decision 57 does not answer it
  but does confine it: a foreign domain is converted by one estimator at one ingest, so what is open
  is the estimator's design rather than where the conversion belongs.
- **The idle ladder's numbers.** Decision 58 fixes the rungs and declines to fix the timeouts, the
  default lock envelope, or the shipped battery and AC profiles. Two of the three are taste; the
  envelope's default is not, since it is the difference between a laptop that behaves reasonably out
  of the box and one that cannot be deployed anywhere with a policy.
- **Wake latency per rung**, which is what makes decision 58's ladder a ladder rather than a list.
  Backlight-off against CRTC-off is a real trade of power for the delay between a keypress and a lit
  panel, and both numbers are hardware-dependent and unmeasured. `// SPEC:` territory, and it wants
  measuring alongside the DRM backend's other hardware questions.
- **What acknowledges a suspend, and what happens when nothing does.** Decision 59 has the greeter's
  agent hold logind's delay inhibitor and wait for gyro inside it, which is a few seconds. What gyro
  does if it cannot present the locked state in that window is unspecified, and "suspend anyway with
  the desktop on glass" is the answer that must not be reached by default. This is the same shape as
  decision 51's session-ready signal — a wait whose failure case decides the design.
- **Whether the greeter's agent is the right machine-level peer at all**, or whether a distinct
  system-tier control connection is cleaner. Decision 59 takes the former because it exists already;
  the latter separates "the machine" from "a session that happens to be permanent", which may matter
  more once the System-tier listener below is designed.
- **The idle CI assertion.** Decision 58 makes "no timer armed when nothing is animating"
  falsifiable and therefore testable, but a static scene in a headless harness is not obviously the
  same static scene a real desktop presents — a clock in the shell's panel ticks once a second
  forever. What the test fixes as "idle" needs deciding before it is written, or it passes on a
  scene nobody runs.
- **Backlight without a backlight.** Decision 58 dims via `/sys/class/backlight`, which external
  monitors do not have; DDC/CI is the usual answer and it is slow, unreliable, and needs I2C access.
  Whether an external output dims at all, and what the composite-side fallback is when it cannot, is
  open — and the fallback is the alpha overlay decision 58 rejects for internal panels, so admitting
  it here needs an argument this entry does not have.
- **Virtual outputs for a session that does not exist yet.** Remote login goes through the
  privileged login agent, as the greeter does, but the ordering against decision 24's listener
  handover is not worked out.
- **The material vocabulary.** Decision 33 commits to a closed set and does not say what is in it.
  It needs designing before the first effect is written, the same way the motion catalog does, and
  the two should be designed together — a material and the transitions that reveal it are one design
  problem.
- **`C_min` as a number.** Decision 35 makes the floor composite's cost the bound on recoverable
  overrun, which makes it a target rather than a measurement. What that target should be, and what
  the floor composite is allowed to contain, is undecided.
- **The snapshot atlas multiple.** Decision 46 denominates capacity in output render-target
  equivalents and declines to guess the number. The derivation to check it against is the largest
  *legitimate* simultaneous retirement — closing an application with a menu open is a window plus
  two small surfaces; a nested menu chain is three or four small ones; closing a workspace's last
  window retires the workspace too. Logout retires everything, and should be one session-level
  transition rather than N window exits, which is a design constraint falling out of the same count.
  The number is then confirmed by instrumentation, not argument: per-output atlas high-water and
  eviction count, tracked the way decision 29 tracks the budget. An eviction outside a stress test
  means the multiple is wrong.
- **The snapshot atlas has no home for a surface on two outputs.**
  [Decision 32](#32-a-surfaces-frame-cadence-follows-its-fastest-output) makes multi-output surfaces
  first class, while decision 46's storage, capacity, attribution, and eviction locality are all per
  output. A window retiring while it straddles the seam is either in both atlases, doubling its
  cost on the configuration decision 28 exists to serve, or in one and sampled by the other, which
  breaks the argument that pressure is resolved against the slots that caused it. Surfaced by the
  same reading that produced decision 47, and it is a sizing question as much as a correctness one.
  Decision 52 shifts the trade rather than settling it: an atlas is at its output's density, so
  "in both" is the horn that is *correct* about density on both, and "in one, sampled by the other"
  resamples an already-resampled snapshot — the one place the resample-once rule would be broken by
  storage rather than by geometry.
- **Whether session switch and lock want output-sized snapshots.** Decision 21 keeps unpresented
  sessions alive, so a crossfade *could* composite both live — at the cost of a second output's
  worth of `C` for the length of the transition, on the frame budget decision 29 defends. A snapshot
  of the outgoing session's last frame buys that back, and nobody can perceive that it froze during
  a 300 ms slide. If that is right, decision 43's locking wants the same thing, and both are one or
  two output-sized images rather than atlas slots — which changes sizing and probably means separate
  storage.
- **The last-good-frame for unresponsive clients.** Showing an application's last good frame while
  it is hung is a real feature and the same shape as an exit snapshot, but with an unbounded
  lifetime. It is the case that tests decision 46's admission rule, so it is worth deciding whether
  gyro wants it at all before the rule is bent to admit it.
- **The capability probe.** What it renders, at what sizes, how long it may take at startup, and how
  its result maps onto tiers. It runs on every boot of a system-layer process, so it has a latency
  budget.
- **Output-to-session assignment must not be client-reachable.** Surfaced while resolving decision
  44 and independent of it. `Filtered globals` puts *output configuration* in the System tier, and
  if a System-tier client could move an output between sessions that is a direct bypass of decision
  43's locking. Configuration — mode, scale, position — and session assignment have to be separate
  operations with separate reachability, and only the login agent may perform the latter. The split
  is not yet expressed anywhere.
- **The System tier needs its own listener.** Also surfaced alongside decision 44. Trust level is
  per connection, connection identity comes from the listener (decision 23), so trust is a property
  of the listener — which means a `System` connection cannot arrive through the ordinary per-user
  socket. That implies a second listener per session with different permissions, and who creates it,
  where it lives, and how a client is judged worthy of it are all unspecified. Decision 51 promotes
  this from speculative to load-bearing: the shell is its motivating occupant, and "which process
  gets to be the shell" is exactly the judgement this listener has to encode.
- **Xwayland has no owner.** It must run as the user, so gyro cannot spawn it, which means decision
  24's session agent is a persistent agent rather than a one-shot fd donor. gyro can own the X
  sockets — `/tmp/.X11-unix` is world-writable — and allocate display numbers, which a
  machine-global compositor is uniquely placed to arbitrate; the fork must be delegated.
- **`WAYLAND_DISPLAY` versus `systemd --user` start order.** `pam_systemd` starts the user manager
  before the session agent binds a socket, so user units do not inherit the variable. The agent must
  import it before starting `graphical-session.target`, and everything graphical must be ordered
  after that target. Decision 24 predicts this bug class; this is its concrete form.
- **The session-ready signal.** Decision 51 will not reassign an output until the incoming session's
  shell has presented, and the same signal is what stops login assembling in visible stages. What
  counts as presented, which client is authoritative when chrome is several clients, and what
  happens when a shell never presents at all are all unspecified — and the last is the one that
  matters, since the answer cannot be "the output never arrives".
- **Per-output wallpapers.** Users set different backgrounds per monitor, and decision 51's cache is
  written and read before the shell exists, when no per-output intent has been expressed. Keying the
  cache per output also means identifying outputs across boots from EDID, which is unreliable with
  two identical panels. Deliberately left entirely open: the live case and the cached case may want
  different answers, and nothing else waits on it.
- **The background cache's lifecycle.** gyro accumulates one persisted image per uid in its own
  state directory, outside the user's control. Deleted accounts leave images behind, a machine with
  many users accumulates a bounded but unstated amount of disk, and nothing says what the cap is or
  who prunes. Small, and the kind of thing that is never written down unless it is written down now.
- **Whether logind accepts a VT-less graphical session on `seat0` .** The login agent registers
  sessions through `pam_systemd`, and decision 37 has no VTs to give it. Needs testing, not
  assuming.
- **BGRT reproduction.** Scaling and placement from the firmware's mode into gyro's, and what to do
  when the firmware framebuffer and the native mode disagree about aspect ratio.
- **`LP_NUM_THREADS` sizing.** Decision 40 bounds interference by reserving cores rather than
  capping time, which turns "how many" into a number that wants measuring on machines with 4, 8, and
  16 cores.
- **The lock-screen content surface.** Decision 43 defers it with a design; the protocol, the
  surface role, and how it interacts with multiple outputs are unspecified.
- **Respawn policy, for the greeter, the shell, and gyro itself.** One greeter serves every session
  under decision 43, so its crash locks out the machine; decision 49 rate-limits gyro's own restarts
  for the same reason and falls through to `gyro --console`; decision 51 adds a per-session shell
  whose crash costs one user their desktop but not their clients. These are one problem at three
  radii — the thing that gets you back to work is itself crashing — and want one answer, covering
  the rate limits, what the output shows meanwhile (decision 51's floor policy and background, for
  the shell), and whether a crash loop should escalate the way gyro's own does.
- **Does the framebuffer survive `drm_file` teardown?** Decision 49 stores the DRM fd rather than
  relying on this, so nothing depends on the answer — but it decides whether the fd store is
  insurance or the mechanism, and it is one experiment on real hardware.
- **Presentation timing needs hardware validation.** Decisions 28–32 are designed rather than
  measured. The scheduling half is testable headless with fake clocks at arbitrary mixed rates, and
  should be the first thing that harness is pointed at. The VRR half is not testable without a
  panel, and the flicker and range behaviour is the part most likely to come back different.
- **Chunk granularity and GPU preemption.** Chunking assumes a submission boundary is a scheduling
  opportunity for the GPU. It is not a guaranteed preemption point and the behaviour is
  hardware-dependent. Decision 30 now gates chunking on this being measured, so the question is no
  longer whether the assumption holds but whether the case that needs it ever arises.
- **GPU priority in practice.** Decision 22 takes `CAP_SYS_NICE` for a high-priority queue on the
  strength of driver source rather than observation. Whether `VK_QUEUE_GLOBAL_PRIORITY_HIGH` is
  granted, and how much blocking it actually removes against a saturating client, wants measuring on
  both i915/xe and amdgpu.
- **Whether snapping applies to a node that is not sampling one-to-one.** Decision 54 exists to keep
  1:1 content crisp, and a node whose settled transform carries a non-unit scale or a rotation is
  resampled regardless — so the snap buys it nothing, though it also costs nothing and keeps one
  rule instead of two. Small, and worth deciding before two rules appear by accident.
- **Subsurface placement at fractional scale.** `wl_subsurface.set_position` is integer
  surface-local, so a subsurface cannot be device-aligned on a fractional output no matter what gyro
  does. The limitation is the protocol's and is not gyro's to fix; what is undecided is whether to
  compound it by re-rounding, absorb it the way decision 52 absorbs the configure remainder, or
  leave the sub-pixel offset in place and let the subsurface be the one soft thing on the screen.
- **Scheduling policy constants** — the VRR servo's per-frame bound, how recently a surface must
  have committed to disqualify its output from early rendering, and decision 56's debounce before a
  surface's preferred scale is lowered.
- **The imperative escape hatch** for event-driven one-shots — shape and boundary.
- **Colour interpolation space for animation** — Oklab proposed over sRGB. Distinct from
  [decision 47](#47-compositing-happens-in-linear-light-at-wide-primaries)'s composite space and
  easily conflated with it, so worth stating apart: 47 governs the space pixels are *combined* in
  and is settled by physics; this governs the path a single colour takes while *animating* between
  two values, and is settled by perception. Linear light is right for the first and visibly wrong
  for the second, where it crushes the middle of a hue transition.
- **Configuration format** — a hand-rolled flat key-value parser is proposed, consistent with the
  hand-rolled XML parse in the protocol generator.
- **Settling thresholds** for non-geometric properties. The geometric half is settled by decision
  54; opacity, blur radius, and corner radius have no output pixel to be expressed in.
- **io_uring kernel floor** — 6.0+ for `SINGLE_ISSUER`, `DEFER_TASKRUN`, and multishot `recvmsg`.
  Needs to be stated and checked, not assumed.
- **spdlog async sink** — file I/O from the frame thread punts to io-wq and surfaces as jitter.
