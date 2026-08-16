# Decision Log

Design decisions with their rationale and, more usefully, the alternatives that were rejected and
why. Decisions 1–20 recorded 2026-08-10; the session model and presentation timing 2026-08-15;
effects and frame integrity later the same day, together with revisions to 22, 29, and 30. Boot,
rendering devices, and login 2026-08-16; session identity later the same day, revising 23; the
thread split (45) later still, revising 2, 3, 29, 36, and 40; snapshot storage (46) last, revising
20 and 41. All of it before implementation.

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

### 2. The Wayland wire protocol is implemented in-tree

Both halves, with typed C++23 bindings generated at build time.

**The decisive argument is that gyro has no restart boundary.** *(Revised 2026-08-16; this decision
has now outlived two rationales, both kept below.)* libwayland resolves allocation failure and
internal invariant violations by calling `wl_abort()`, and compiles in assertions whose disposition
depends on how the distribution built it. For a session compositor that is a crash, one lost login,
and a greeter that restarts it. For gyro it is the machine's display going out at a moment a linked
library chose, with no way to intercept, degrade, or contain it — on a system that
[decision 37](#37-gyro-owns-the-display-from-firmware-handoff-onward-there-are-no-vts) has
deliberately left with no VT to fall back to, and whose recovery console is gyro itself. Every other
decision about failure here points the same way:
[decision 27](#27-resource-accounting-is-attribution-not-per-user-fairness) sizes limits to survive
rather than to ration, and [decision 41](#41-device-migration-is-exercised-on-every-boot) keeps the
last frame on glass through device loss. Linking a library that terminates the process unilaterally
contradicts all of it.

Three supporting arguments, none sufficient alone:

- **A per-client dispatch budget.** libwayland offers no lever for "process at most N messages from
  this client". Under [decision 45](#45-protocol-dispatch-is-a-thread-not-a-task) one thread serves
  every client on the machine, so fairness between them is gyro's problem and it needs a lever that
  does not exist.
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

**Cost accepted:** 3–6 weeks to parity plus an interop tail; SIGBUS handling for `wl_shm` pools
becomes ours; and continuous fuzzing of the demarshaller becomes a requirement rather than a nicety,
because this is where untrusted input from every account on the machine terminates. Staged
client-half-first against mutter, with wlroots as a second target.

**Open: the reachability of libwayland's abort paths is unverified**, and the decisive argument
rests on it. What is wanted is a count of `wl_abort()` and assertion sites in `wayland-server`
reachable from ordinary operation rather than from programmer error, and whether any is reachable
from client input rather than only from allocation failure. If the answer is "allocation failure
only, and gyro is dead in that case regardless", this falls back to the three supporting arguments,
and the margin over libwayland is then thin enough that the decision should be re-opened rather than
assumed.

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
[decision 2](#2-the-wayland-wire-protocol-is-implemented-in-tree) was originally written about. A
hostile or merely chatty client now cannot reach the frame thread at all, which is a stronger
property than bounding what it costs us.

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

**Cross-thread lifetime is the real price.** A client may destroy a surface while the frame thread
holds it. The single-threaded design gets that right for nothing and this one has to build it:
[decision 15](#15-identity-is-a-generational-handle)'s generational handles already supply the
detection, and [decision 20](#20-exit-animations-use-full-resolution-snapshots) already establishes
that an entity outlives its protocol object, so the shape exists — but the discipline is new, and it
is not retrofittable.

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

### 17. Transforms are decomposed into TRS with per-channel springs

Matrices are never interpolated; lerping them shears and collapses. Per-channel springs let position
be snappier than scale, which is what makes motion feel designed rather than mechanical.

### 18. Matched geometry is in the first cut

Match keys plus the differ's exit/enter pairing pass. Without it, window-to-overview and similar
transitions are cross-fades, which is what "not cohesive" looks like in practice. Retrofitting means
revisiting every transition that already exists.

Developed against one real transition — window to overview thumbnail — rather than a synthetic test,
since that exercises mismatched aspect ratios, cross-tree parenting, and mid-gesture interruption at
once.

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
machine, using a hand-written codec, with no restart boundary underneath it. That is the worst
available candidate for ambient privilege. `CAP_SYS_NICE` is not a step onto that path — it is one
bounded capability with one named use, and the rest of the set stays refused.

**Note:** `drmSetMaster()` wants `CAP_SYS_ADMIN`, which is effectively root. It is not needed —
first-open confers master, gyro never hands off, and Plymouth is a unit ordering problem. Needing
that capability remains an ordering bug, not a requirement.

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
- **The control socket is an unauthenticated entry point** into the one process on the machine with
  no restart boundary. Bounded by `SO_PEERCRED` on the offer, a per-uid offer cap, and one accepted
  listener per session.
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
with no restart boundary, where the local wire protocol at least requires an account on the machine.
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
it is that gyro has no restart boundary. A session compositor that dies loses one login and is
restarted by its greeter; gyro dying takes the machine's UI to black with nothing underneath.

Attribution is kept unconditionally because it costs a counter increment, it is the first thing
wanted when diagnosing memory, and it is the part that cannot be retrofitted afterwards.

Client send-buffer backpressure is *not* filed here. Unbounded buffering is a memory bug at one user
as much as at ten, so it belongs with the protocol layer's backpressure work.

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
system looks like rather than what it costs to look that way.

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
[decision 2](#2-the-wayland-wire-protocol-is-implemented-in-tree). Owning the wire protocol makes
message handling cheap and bounded, which is worth having and is not what protects the frame. The
distinction is worth stating because the two were conflated for this design's entire first pass: the
frame is protected by what is *not on the thread*, not by what is fast.

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
  state and modeset only on difference. This is needed three separate times: re-exec, the
  `simpledrm` → real-driver migration in
  [decision 41](#41-device-migration-is-exercised-on-every-boot), and any Plymouth takeover we are
  ever forced into.

**Rejected: no initramfs at all.** It is the cleanest version of decision 37 and it makes encrypted
root impossible, since a passphrase prompt has nowhere to live. TPM2-backed unlock covers the happy
path, but a firmware update that changes the PCRs then leaves a machine that boots to a black screen
with no way to enter a recovery key. The initramfs should exist and be silent, producing output only
when it genuinely needs input.

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
[decision 2](#2-the-wayland-wire-protocol-is-implemented-in-tree)'s broken rationale. It is not one.
A lavapipe worker holding a driver lock exists whether or not gyro owns the wire protocol, and the
fix is the priority ladder above rather than anything about Wayland — what it establishes is that
in-process inversion is real on this project, not that owning the protocol addresses it. Decision 2
was re-argued on other grounds instead.

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
per-user lock wallpaper, and lock-screen widgets are all the same problem — they live in the user's
session, at the user's uid, and the greeter is neither.

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

## Open

Carried forward, roughly in the order they will bite:

- **libwayland's abort reachability.** Decision 2's decisive argument rests on it and nothing else
  does, so it should be checked first, and it is an afternoon's reading. Count the `wl_abort()` and
  assertion sites in `wayland-server` reachable from ordinary operation rather than from programmer
  error, and establish whether any is reachable from client input rather than only from allocation
  failure. A weak answer re-opens decision 2 rather than adjusting it.
- **Publication granularity across outputs.** Decision 45's boundary is wait-free, so the frame
  thread could acquire client state once per iteration or once per output. Architecture.md takes
  once per iteration, on the grounds that a window straddling two outputs would otherwise show two
  different client frames in one iteration — visible on exactly the configuration decision 28
  exists to serve. Per-output acquisition is strictly fresher, and the trade has been reasoned
  rather than measured.
- **Colour format for virtual outputs.** Encoders want NV12 or P010, not RGBA. The agent can convert
  (an extra full-frame pass and its bandwidth), or gyro can fold RGB→YUV into its final composite
  pass (much cheaper, but the renderer grows a YUV output path it otherwise would not have), or both
  can be offered. HDR sharpens it — P010 and transfer functions. This is the one part of decision 26
  with a real performance number attached and it should not be decided from the armchair.
- **Clock offset for injected input.** Decision 26 keeps the claim that `t₀` is the event timestamp,
  but a remote client's timestamps come from another machine's clock. Used naively they start
  animations in the past or the future. Needs an offset estimate, and it is the kind of thing that
  ships subtly wrong and presents forever as "remote feels strange".
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
  where it lives, and how a client is judged worthy of it are all unspecified.
- **Xwayland has no owner.** It must run as the user, so gyro cannot spawn it, which means decision
  24's session agent is a persistent agent rather than a one-shot fd donor. gyro can own the X
  sockets — `/tmp/.X11-unix` is world-writable — and allocate display numbers, which a
  machine-global compositor is uniquely placed to arbitrate; the fork must be delegated.
- **`WAYLAND_DISPLAY` versus `systemd --user` start order.** `pam_systemd` starts the user manager
  before the session agent binds a socket, so user units do not inherit the variable. The agent must
  import it before starting `graphical-session.target`, and everything graphical must be ordered
  after that target. Decision 24 predicts this bug class; this is its concrete form.
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
- **Greeter respawn policy.** One greeter serves every session under decision 43, so its crash locks
  out the machine. Restart rate limiting, and what the output shows meanwhile, are undecided.
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
- **Scheduling policy constants** — the VRR servo's per-frame bound, and how recently a surface must
  have committed to disqualify its output from early rendering.
- **The imperative escape hatch** for event-driven one-shots — shape and boundary.
- **Colour interpolation space** — Oklab proposed over sRGB.
- **Configuration format** — a hand-rolled flat key-value parser is proposed, consistent with the
  hand-rolled XML parse in the protocol generator.
- **Settling thresholds** for non-geometric properties.
- **io_uring kernel floor** — 6.0+ for `SINGLE_ISSUER`, `DEFER_TASKRUN`, and multishot `recvmsg`.
  Needs to be stated and checked, not assumed.
- **spdlog async sink** — file I/O from the frame thread punts to io-wq and surfaces as jitter.
