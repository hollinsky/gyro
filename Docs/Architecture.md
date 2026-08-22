# Platform Architecture

How gyro reaches the screen, where its input comes from, and how the frame loop is driven.

Companion documents: [Animation.md](Animation.md) for the animation system,
[Decisions.md](Decisions.md) for the decision log including rejected alternatives.

## Constraints this design serves

gyro is a system layer, not a session component — one compositor per machine. It runs `SCHED_FIFO`
with the goal of hitting every frame, and it drives animations and per-pixel effects like blur. It
uses no VTs at all, and it has no session dependencies — device access comes from udev rules, DRM
master from first-open, and session lifecycle from a handover it is offered rather than a bus it
subscribes to. That is what makes it a boot service rather than a login one; see
[Boot](#boot-and-the-display-lifetime).

Three consequences shape everything below:

- **Frame pacing is the product.** A design that renders correctly but misses deadlines has failed.
  Scheduling is therefore a first-class subsystem, not a loop that fell out of the renderer.
- **Real-time means no surprises.** No allocation on the frame path, no blocking syscalls, and no
  lock held against the frame thread by anything it outranks.
- **The frame thread does one thing.** Evaluate, record, submit, present. Everything whose cost a
  client can influence — the wire protocol, buffer import, input — lives on the far side of
  [a publication boundary](#threads), on another thread. This is what makes the other two
  enforceable rather than aspirational.

## The seam

"Backend" usually means "where pixels go". That conflation is the mistake worth avoiding, because
the platform is really four independent concerns:

| Concern          | Native              | Nested Wayland          | Headless            |
| ---------------- | ------------------- | ----------------------- | ------------------- |
| **Session**      | udev rules, first-open master | no-op         | no-op               |
| **Presentation** | atomic KMS flip     | `wl_surface` + dmabuf   | dmabuf → dump       |
| **Input**        | libinput + udev     | `wl_seat`               | scripted injection  |
| **Outputs**      | connectors / CRTCs  | host windows            | virtual             |

Keeping them independent is what lets them mix: nested presentation with scripted input for
reproducible tests, headless presentation with real libinput for perf harnesses. Fusing them into
one `IBackend` loses that, and the fusing is what's hard to undo later.

**Design principle: the seam is designed to the constrained backend and implemented first by the
convenient one.** KMS decides the shape of the interfaces; nested is what first proves them.

### Presentation

The framing that makes one interface serve all three: **KMS is a swapchain.** A small ring of
dmabuf-backed images, FIFO-presented, with an unusually precise clock.

```cpp
class IPresenter
{
public:
	// The backend owns the images. Both KMS and Wayland have modifier and device
	// constraints the renderer cannot know, so neither can accept foreign buffers.
	virtual std::span<const RenderTarget> Targets() const = 0;
	virtual std::optional<uint32_t>       AcquireTarget()  = 0;

	// May never block: no device-wide lock, no wait on another output's commit. Its cost is
	// bounded by the composite, because it is decision 29's B(L). Layers are ordered bottom
	// first and the composited remainder is one of them; the Result is how a commit that
	// failed without blocking reaches a caller that can still act. See decision 78.
	virtual Result<void> Present(std::span<const PresentLayer> layers) = 0;

	// Initiated on the frame thread, performed elsewhere. Returns before the hardware is
	// programmed; completion arrives as Reconfigured. Unbounded by contract even where a
	// given driver is fast. Adoption is not a second entry point — Reconfigure() to the
	// mode already set is an adoption, and the backend commits without ALLOW_MODESET first
	// so that the kernel is the one deciding whether it was. See decision 73.
	virtual void Reconfigure(const OutputConfiguration& wanted) = 0;

	Signal<const PresentationInfo&>    Presented;           // frame reached glass
	Signal<const OutputConfiguration&> Reconfigured;        // what was achieved; targets valid again
	Signal<>                           TargetsInvalidated;  // resize, mode set, modifier renegotiation
};
```

**A frame is a list of layers, not an image.** The arrangement worth having is not one client filling
the screen — it is several layers on several planes with the GPU never waking, which is what a tablet
spends most of its life able to do. So the composited remainder is one entry in the list rather than
a privileged concept, and promotion is a partition rather than a fullscreen special case. A layer
carries a source, an acquire point, damage, a source crop, a destination rectangle, a blend mode and a
color state; z is the list order. One element is what nested and headless present today, so nothing
is given up by saying it this way, and what is bought is that the modules written next — damage
accumulating per *plane*, `C` as the cost of what was not offloaded, a scanout hold crossing the
return channel — are not written on the assumption that a frame is one image. See
[decision 78](Decisions.md#78-present-takes-a-layer-list-and-the-composite-is-one-member-of-it),
which also records the one thing the list cannot yet express: a layer whose source is a client's
buffer rather than one of the output's own targets.

**The completions arrive on a source, and the source is the device's rather than the output's.**
Every one of those three signals begins as a readable file: page-flip events on the DRM device,
`wp_presentation_feedback` on the host connection. A presenter is one output's, and that descriptor
is not — one DRM file serves every CRTC on the device, one connection every window nested opened. So
the drain sits beside `IPresenter` rather than on it, and the presenters are what it emits into.

```cpp
class IEventSource
{
public:
	// What the loop waits on, or an invalid descriptor where there is nothing to wait on —
	// headless, whose flips are a function of the fake clock and make no file readable.
	virtual RawFd Descriptor() const noexcept = 0;

	// Read what has arrived and emit it, until nothing is left. Drained every iteration
	// whatever woke the loop, so finding nothing is the ordinary result and is success.
	// Present()'s failure vocabulary, because it is the same file failing the same ways.
	virtual Result<void> Drain() = 0;
};
```

Exactly one thread pumps a source for the whole of its life, which is what lets `Drain()` hold no
lock — and which is why nested opens two connections to the host rather than partitioning one by
event queue. See [decision 80](Decisions.md#80-the-frame-loop-is-a-step-the-composition-root-owns-the-wait)
and [decision 81](Decisions.md#81-a-source-is-pumped-by-one-thread-nested-opens-two-connections).

**The completion carries what was achieved rather than merely that it finished.** The
variable-refresh range is derived from the mode and cannot be asked for, so a bare signal would need
a query path beside it; carrying the configuration answers that and makes a request the hardware
could not honour report itself by disagreeing with what was asked.

**The two verbs carry opposite contracts, and that is the point of their being two.** The kernel's
atomic API has one entry point for both, distinguished by a flag, which is why its real-time
behaviour is set by the slower of the two workloads — the reasoning is in
[KernelWishlist.md](KernelWishlist.md). Splitting them here is what lets an output be
mid-reconfiguration while every other output keeps presenting, and it is what keeps a mode set from
becoming a term in [admission control](#admission-control)'s blocking bound.

The consequence that matters: **the renderer never sees a `VkSwapchainKHR`.** It renders into
dmabuf-backed `VkImage`s and produces a `SyncPoint`. That is exactly what KMS needs, and it is
achievable nested by driving `zwp_linux_dmabuf_v1` directly rather than going through Vulkan WSI.

Sync is uniform: DRM syncobj timelines throughout, exported from Vulkan timeline semaphores.
On KMS that becomes `IN_FENCE_FD` / `OUT_FENCE_PTR`; nested it becomes `wp_linux_drm_syncobj_v1`.
Damage is in the interface from the start because both paths want it — `FB_DAMAGE_CLIPS` on KMS,
`wl_surface.damage_buffer` nested.

> Written from specification rather than from a hardware prototype. The interface is deliberately
> over-provisioned — plane assignment, per-plane fencing — so that the DRM backend has room to fit
> without a redesign. Expect these spots to need revisiting when the DRM backend lands; they are
> marked `// SPEC:` in the headers.
>
> The mode-set-versus-page-flip split was in that list until 2026-08-17, when it stopped being a
> provision and became [decision 73](Decisions.md#73-the-frame-thread-initiates-reconfiguration-and-never-performs-it).
> It is the one place the over-provisioning turned out to be aimed at the wrong thing: the seam had
> room for planes and fencing, which are frame-side, and none for mode setting, which is not. The
> layer list left the same day, by being taken.
>
> The drain those signals arrive on is a plainer failure than either, and it is recorded beside them
> because the three are easy to conflate. It was not provisioning aimed at the wrong thing; it was
> an omission. This interface promised that `Reconfigured` is emitted "from the loop's own drain"
> and described no drain, while a fake presenter with a `Flip()` a test calls stood in for one.
> `IEventSource` closed it on 2026-08-17, and the lesson is about the double rather than about the
> interface: **a fake that supplies a missing half will not report it missing.**
>
> The hole that lesson predicts is still open, and it is the cursor.
> [Admission control](#admission-control) exempts the cursor plane from the budget *because it
> updates independently of the composite*, which says there is a commit that is not a frame — and
> this interface has no room for one. Recorded in [Open.md](Open.md) rather than provisioned for,
> since the shape depends on measurements nobody has taken.

### The frame clock

The scheduler never talks to KMS directly. **There is one clock per output, never one for the
machine** — see [Presentation timing](#presentation-timing) for why that is the load-bearing choice
and not an implementation detail.

```cpp
class FrameClock
{
public:
	void Configure(const OutputConfiguration& achieved);  // nominal period, learned VRR range
	void Observe(const PresentationInfo& info);   // presented-at, period, sequence, flags
	void Invalidate();                            // resume, mode set, device migration, VRR idle
	void Command(Duration target);                // VRR servo; no-op on a fixed output

	// Keyed by sequence, and Unscheduled until an Observe() re-seeds the clock after an
	// Invalidate(); IsValid() is how the loop asks.
	Instant PresentationAt(uint64_t sequence) const;  // when that frame reaches glass
	Instant DeadlineAt(uint64_t sequence) const;      // when its commit must land
	Instant WakeupAt(uint64_t sequence, Duration reserve) const;

	uint64_t LastSequence() const;                // the frame the anchor describes
	uint64_t SequenceAfter(Instant notBefore) const;  // the frame that instant can still make

	Instant NextPresentation() const;             // == PresentationAt(LastSequence() + 1)
	Instant NextDeadline() const;
	Instant NextWakeup(Duration reserve) const;   // deadline - renderBudget - safety

	Duration        Period() const;               // what the panel did
	Duration        CommandedPeriod() const;      // what gyro is asking it for
	Duration        TargetPeriod() const;         // where the servo is heading
	VariableRefresh PeriodRange() const;          // VRR window; degenerate when fixed
	bool            IsValid() const;              // observed since the last invalidation
	bool            IsVariable() const;           // deadline may be deliberately deferred
	bool            IsPrecise() const;            // hardware clock, or best-effort
};
```

**It is keyed by sequence and not by now.** The whole state is an anchor — frame `S` reached the
glass at `T`, the output is running at period `P` — so the next frame is `S + 1` at `T + P`, and the
frame [speculative early rendering](#speculative-early-rendering) wants is `S + k`. `PresentationAt()`
answering for an arbitrary sequence is what that needs, and it is only answerable because animation is
a pure function of time. Keying the whole interface that way is
[decision 36](Decisions.md#36-frame-path-discipline-is-enforced-mechanically-not-by-review)'s
discipline applied one level below the render path: not merely that animation is never evaluated
against an ambient now, but that the thing computing the deadlines does not read one either.

**`SequenceAfter()` is the one question that cannot be answered that way, and it is the recovery
path.** When the anchor is several periods old — the output was idle, or a stall ate frames — which
frame is next depends on an instant, and that is
[decision 35](Decisions.md#35-a-miss-costs-one-frame-bounded-by-the-floor-composite)'s
`⌈overrun / P⌉` rather than an incidental. Refusing the question does not remove it: a loop coming out
of idle would ask for `S + 1`, be handed a deadline in the deep past, fail both branches of the timing
policy, skip, ask for `S + 2`, and skip forever. So the instant enters here once, at the call where it
means something. The loop already reads `Now()` once per iteration and hands it on, and
`SequenceAfter(now + C)` reads as *the frame I can still make if I finish then* — which is the timing
policy's own question with the clock's arithmetic behind it.

**Everything the clock knows, it learned from the output.** Observations arrive from the backend,
the nominal period and the variable-refresh window from the configuration a reconfiguration achieved,
and nothing in it is a fact about gyro. That is why `WakeupAt()` takes the reserve rather than holding
it: `renderBudget` is a high-water mark of gyro's own CPU and GPU time and `safety` is a margin on
gyro's own wakeups, both of which [the frame loop](#the-frame-loop) already holds for its record-time
check. A copy on the clock would be a second place the same number can be wrong, written by admission
control while the backend writes everything else in the object, and the two would disagree exactly
when a budget moved. *Per-output budget as real data* is about that data existing, not about the clock
being where it lives.

**A deadline is a presentation minus the latch lead**, which is how early the driver needs a commit
programmed for the vblank that latches it. The two instants are separate questions because the
hardware makes them separate; nested and headless pass zero, which makes them equal there without
making the distinction something a backend opts into.

**Three periods, and conflating any two of them is a servo reporting a rate nothing achieved.**
`Period()` is what the panel last did, `CommandedPeriod()` is what gyro is presently asking for, and
`TargetPeriod()` is where [the servo](#vrr-as-a-scheduling-degree-of-freedom) is heading; they
disagree for the length of every ramp and permanently on a panel that will not comply. `Command()` is
the whole of the servo's reach into the rest of the system, and it states a target rather than a
value: each observation moves the commanded period toward it by a bounded step, clamped into the
window the panel reported and held clear of the bottom so low-framerate compensation never engages
underneath it. Convergence happens inside `Observe()` because an observation is the only moment new
evidence exists — [decision 31](Decisions.md#31-vrr-is-a-scheduling-degree-of-freedom-not-only-a-latency-feature)
requires the loop to close on one rather than on an acknowledgement, since panels misreport their
ranges. Prediction takes the commanded period on a variable output, where the vblank happens when gyro
submits, and the observed one on a fixed output, where gyro's intention is not an input.

A window that cannot be true — disabled, inverted, non-positive — is a backend saying nothing rather
than saying something to distrust, so the servo is refused outright and
[admission control](#admission-control) falls through to the next rung. That is the direction that
cannot produce a period no panel agreed to.

**`Invalidate()` and `IsValid()` exist because a clock can become a liar.** After a system resume,
after a mode set, and after [device migration](#device-migration), the last observation describes a
world that no longer exists — and a stale observation is worse than none, because `NextDeadline()`
then returns an instant in the deep past and [the frame loop](#the-frame-loop)'s timing policy fails
every branch forever rather than falling to the floor tier. An invalid clock owes no frame and arms
no timer until an observation re-seeds it.

What it answers meanwhile is chosen rather than convenient. Saturating forward — `Unscheduled` —
inverts both halves of that failure into the behaviour the design already asks for: nothing is ever
due, so no timer is armed and an idle output costs nothing, while any frame damage demands passes the
record-time check immediately and presents as soon as it is ready. That is
[decision 31](Decisions.md#31-vrr-is-a-scheduling-degree-of-freedom-not-only-a-latency-feature)'s
account of the first frame after an idle VRR output, falling out of the sentinel rather than needing a
mode. What an invalidation drops is the anchor and not the rate: what a resume falsifies is where in
the cadence the output is, and admission control still has to size an output that has not presented
since.

Sources per backend:

- **DRM** — page-flip event timestamps, with `DRM_CAP_TIMESTAMP_MONOTONIC`.
- **Nested** — `wp_presentation_feedback.presented`, which carries presentation time, refresh
  period, sequence, and flags including `VSYNC` / `HW_CLOCK` / `ZERO_COPY`. Structurally identical
  to KMS.
- **Headless** — a fake clock the tests drive, including deliberately missed deadlines and
  arbitrary mixed-refresh combinations.

A backend that reports no period at all is answered by a fallback ladder rather than by a fabricated
one: difference the anchor, which divides by the sequence advance and is therefore right across
skipped frames, and fall back to the configured nominal when even that is unavailable. There is no
filter over the observed period, deliberately — every observation re-anchors, so prediction error is
bounded by one period of extrapolation rather than by however long ago the estimate was formed, and a
filter would buy accuracy in a term already re-measured every frame at the cost of lag in the one
quantity the servo closes its loop on.

**`Instant` and `Duration` are different types**, and the distinction is load-bearing rather than
decorative — see [the timebase](#the-timebase). `Instant - Instant → Duration`,
`Instant + Duration → Instant`, `Instant + Instant` ill-formed.

`renderBudget` is a high-water mark of gyro's own CPU and GPU time for **that output**, measurable
everywhere. Resolution and effect load differ per output, so one figure for the machine is not
meaningful. It is a worst case rather than a percentile, for the reason given under
[Budgets](#budgets).

`IsPrecise()` exists because a host compositor may not implement presentation feedback, and it is
per clock rather than global — a remote output's clock is derived from flow control and is never
precise, while a local panel's is. The fallback is `wl_surface.frame` plus an assumed refresh, and
the compositor needs to *know* which of its clocks are mush so it can downgrade log severity and
skip latency assertions rather than reporting nonsense.

This is why nested is worth building: the real deadline scheduler runs unmodified against it.

### Session

Device acquisition and revocation only — no VT handling.

```cpp
class ISession
{
public:
	virtual Result<Fd> OpenDevice(std::string_view path) = 0;
	virtual void       CloseDevice(Fd fd)                = 0;

	Signal<RawFd> DevicePaused;
	Signal<RawFd> DeviceResumed;
	Signal<bool>  ActiveChanged;
};
```

**The signals carry a borrowed descriptor and the calls carry an owning one, and the asymmetry is
forced.** `Fd` owns and is move-only, which is what `OpenDevice` returning one and `CloseDevice`
consuming one mean. A broadcast cannot transfer ownership to anyone — with N observers at most one
could take the descriptor and nothing in the signature says which — so what a signal delivers is the
descriptor's *identity*, which the receiver matches against the `Fd` it already holds. See
[decision 77](Decisions.md#77-a-signals-observers-are-links-the-observers-own).

Nested and headless implement this as three functions that always succeed. The real implementation
is deferred until the DRM backend needs it — see
[Decisions.md](Decisions.md#7-session-claiming-is-deferred-basu-rejected).

Note that `ISession` is about *devices*. gyro's own notion of a user session is a separate concept
that does not pass through this interface at all; see [Sessions and users](#sessions-and-users).

## Backends

### Nested Wayland

The daily driver. Presents into a window hosted by another compositor.

Beyond convenience it unlocks RenderDoc, Vulkan validation layers, ASan/UBSan, and gdb — all of
which range from awkward to impossible against a compositor holding DRM master. That alone roughly
pays for the backend.

Two features to build deliberately rather than let emerge:

- **`--outputs N`** — one host window per virtual output. Multi-monitor layout, cross-output window
  drags, and per-output scale — including mixed *fractional* scales, which is where
  [Geometry](#geometry) is hardest — become testable without owning three monitors.
- **Dynamic modes** — a window resize *is* a mode change. Handling that from day one means real
  hotplug and mode-setting work when the DRM backend arrives instead of being a rewrite.
- **Two connections to the host**, not one. Presentation feedback is frame-side and `wl_seat` input
  is dispatch-side, so a single connection is one socket read by two threads — and partitioning it
  by event queue, which is what libwayland-client does, costs a lock that the frame thread meets and
  the dispatch thread holds. Each connection binds only its own half's globals and is pumped by
  exactly one thread. See [decision 81](Decisions.md#81-a-source-is-pumped-by-one-thread-nested-opens-two-connections).

Safety rules, enforced by the backend rather than by convention. Nested and headless force off
`SCHED_FIFO`, `mlockall`, session claiming, and DRM master unless explicitly overridden. A real-time
thread nested inside a normal-priority host is an effective way to hard-lock a desktop. gyro also
advertises its own `WAYLAND_DISPLAY` socket without disturbing the host's.

### Headless

Virtual outputs, scripted input, a fake clock, and frame dumps. This is what makes golden-image
tests of blur and animation possible in CI, and it is where deliberately missed deadlines get
injected to prove the scheduler recovers monotonically.

It is also the only place the [presentation timing](#presentation-timing) work can be tested
precisely. Fake clocks take arbitrary rates and phases, which is what makes the schedulability claim
falsifiable: an admitted task set can be swept across every phase relationship and asserted to miss
nothing, including on rate combinations nobody has on a desk.

### DRM / KMS

Atomic modesetting, plane assignment, hardware cursor, explicit fencing, VRR, and the
[color](#color) pipeline — which plane assignment depends on, since direct scanout is conditional
on the hardware expressing the transform the composite would have applied. Built third, designed for
from the start.

### Selection

Configure-time `GYRO_BACKEND_DRM` / `_NESTED` / `_HEADLESS`, all `ON` by default. Runtime
`--backend=auto|drm|nested|headless`, where `auto` picks nested when `WAYLAND_DISPLAY` is set and
DRM otherwise.

Note that the *rendering device* is a separate axis from the backend. Software rendering is a
physical device gyro may select under any of the three, not a fourth backend — which is what makes
it testable nested. See [Rendering devices](#rendering-devices).

## Boot and the display lifetime

gyro waits on two things: a DRM device with its driver's firmware loaded, and `/dev/input/*` with
udev rules applied. Nothing else — no D-Bus, no network, no user, no VT, no `XDG_RUNTIME_DIR`. So
`gyro.service` is udev-triggered on the DRM device and starts before nearly everything.

That is not merely early; it is early enough to displace what normally renders first, and the design
takes that seriously. **gyro is the only thing that ever writes to the display**, from the firmware
handoff to shutdown.

### From firmware to gyro

With `fbcon=off`, no Plymouth, and no initramfs output, nothing binds a console to the EFI
framebuffer, so **the firmware's BGRT logo simply stays on screen until gyro's first modeset.**
"Zero output until gyro" is not arranged; it is what remains once everything else is removed.

gyro then reads `/sys/firmware/acpi/bgrt/{image,xoffset,yoffset}` and reproduces the logo in its own
mode at the corresponding position. The offsets are in the *firmware's* mode, so this is a
scale-and-place computation rather than a memcpy — get it wrong and there is a visible jump at
exactly the moment the design exists to make seamless. From there it animates into the greeter, so
the entire boot is one continuous image.

Fedora ships `CONFIG_VT=y`, `CONFIG_VT_CONSOLE=y`, and `CONFIG_FRAMEBUFFER_CONSOLE=y`, which is
better than compiling them out: `fbcon=off` achieves the visible result with no custom kernel, and
the VT machinery survives underneath as somewhere for a panic to go.

Three things follow that gyro cannot solve and that become deployment requirements:

- **`CONFIG_DRM_PANIC` must be enabled, with `pstore` behind it.** A kernel panic now prints where
  nobody is looking. The DRM panic handler draws it straight onto the framebuffer with no VT and no
  fbcon — the arrangement its own Kconfig help text names — and `drm.panic_screen=kmsg` makes it the
  tail of the log. Every driver on gyro's path implements the hook, `simpledrm` included. `pstore`
  stays enabled for what the handler cannot draw.
- **A systemd `OnFailure=` unit runs `gyro --console`.** gyro failing to start otherwise leaves no
  local way into the machine at all — worse than "no UI", because there is no VT to fall back to.
- **`sysrq` must be enabled.** `RLIMIT_RTTIME` catches a real-time thread that spins; a frame thread
  blocked on a driver lock accrues no real-time budget and trips nothing. With no VT, the keyboard
  is the only remaining way in — and `SysRq-V` is the half of it that restores a picture rather than
  ending the process holding one.

### Restart

gyro can die, and what that costs is settled in
[decision 49](Decisions.md#49-the-restart-boundary-is-made-cheap-where-it-can-be-and-stated-where-it-cannot).
Three things are separable and only the last is forced:

| | Survives a restart? | How |
| --- | --- | --- |
| The display | yes | the service manager's fd store holds the DRM fd open, so the kernel never releases it; the mode is adopted rather than re-set |
| The listeners | yes | session agents re-offer, which is why the [handover](#listener-handover) is a stable ABI |
| The clients | **no** | a connection is an fd, and every toolkit treats its closure as fatal |

So a restart reads as *the last frame holds, then a greeter fades in*, rather than as the machine
going black — and everyone loses everything they had open. The first two rows are why the phrase
"gyro has no restart boundary" is retired throughout this document in favour of the accurate one:
the boundary is ruinously expensive, not absent.

**That first row depends on the fd store entirely, not partially.** The kernel disables any plane
still scanning a framebuffer out as part of releasing the file that created it, so a DRM fd that
genuinely closes takes the picture with it — there is no state in which the CRTC quietly carries on.
What the store buys is that the fd never closes at all: `SCM_RIGHTS` shares a reference to the same
open file description rather than copying it. Hence `FDSTORE=1` goes out immediately after
first-open master and before anything else that can fail, since the gap between the two is the whole
of gyro's exposure to a black screen. This is the same property that makes
[re-exec](#the-pre-vulkan-console) seamless, and the reading behind
[decision 49](Decisions.md#49-the-restart-boundary-is-made-cheap-where-it-can-be-and-stated-where-it-cannot)
is what established they are one mechanism rather than two.

**Most failures must never reach here.** Parse failures, resource exhaustion, and protocol
violations are client-fatal by construction, per [resource accounting](#resource-accounting).
Restart is for the bugs that escape that, it is rate-limited, and exhausting the limit falls through
to the `OnFailure=` console above — because a crash loop with the last frame frozen on glass is
worse than a black screen, and it is the shape a malformed message replayed on reconnect would take.

**The one lever that would shrink a restart is dispatch as a process per session**, containing a
demarshaller crash to one user. It is deferred, not foreclosed, and the affordance that keeps it
available is a representation choice on [the publication boundary](#the-publication-boundary).

### The pre-Vulkan console

DRM dumb buffers, CPU blits, an embedded bitmap font, a fixed glyph grid. No Vulkan, no GPU driver,
no text shaping. It is a permanent subsystem rather than a bootstrap, because four requirements turn
out to be one piece of code: BGRT continuation, verbose boot output, the recovery console that
replaces VTs, and the failure display when Vulkan will not initialize.

This is not the in-process UI that [Locking](#locking) refuses. That refusal is about UI with a
*design* — text shaping, layout, styling. A fixed grid of pre-rendered glyphs is what a recovery
console should be, precisely because it must work when nothing else does.

It also runs on every boot rather than only in failure, which is the same argument
[the floor tier](#the-floor-tier) makes: the path needed when things are broken must not be the path
nobody has executed since it was written.

> **Deferred: running from the initramfs.** The intended end state is gyro starting inside the
> initramfs and re-executing from the real root after switch-root, the way systemd does on itself,
> which is what gives full-disk encryption somewhere to prompt. It is easy for gyro in a way it is
> not for systemd, because at switch-root there are no clients — only the DRM fd, the mode, the
> buffer on screen, and the input fds. **DRM master is a property of the open file description, so
> it survives `execve` with `FD_CLOEXEC` cleared**: master is never released, no mode is reset,
> nothing flickers. Out of scope initially; the affordances that keep it possible are in
> [what to build before it is needed](#what-to-build-before-it-is-needed).

## Rendering devices

`VkPhysicalDevice` selection, not a backend. gyro may be running on a GPU, on `simpledrm` before the
real driver has loaded, or on software — and the frame loop is identical in all three.

### Software rendering is the floor tier

lavapipe ships in `mesa-vulkan-drivers`, so it is present wherever Mesa is, and it advertises
Vulkan 1.4 with `VK_EXT_external_memory_dma_buf`, `VK_EXT_image_drm_format_modifier`,
`VK_KHR_external_memory_fd`, and `VK_KHR_timeline_semaphore` — the complete set gyro asks of a
device. It executes command buffers on llvmpipe's rasterizer pool and signals from a queue thread,
so from the frame thread's side it is an asynchronous device that work is submitted to, structurally
indistinguishable from a GPU.

So it needs no policy of its own. It is [the floor tier](#the-floor-tier), permanently occupied,
which means a machine with a broken GPU driver runs the same render mode the tests and golden images
already exercise.

Thread priorities are the one thing that changes with the device — the [dispatch
thread](#threads) sits below both columns either way:

| | Accelerated | Software |
| --- | --- | --- |
| Frame thread | `SCHED_FIFO` | `SCHED_FIFO`, lower priority |
| Device workers | on the GPU | `SCHED_FIFO`, one below the frame thread |
| Contention lever | `VK_QUEUE_GLOBAL_PRIORITY_HIGH` | `LP_NUM_THREADS` reserving cores |

The workers go on `SCHED_FIFO` rather than `SCHED_OTHER` deliberately. Starvation is not the hazard
— a `SCHED_FIFO` thread only starves others while runnable, and the frame thread is blocked on a
semaphore for most of a frame, provided it never busy-waits on a fence. The hazard is **priority
inversion**: the frame thread blocks in `vkQueueSubmit` or `vkWaitSemaphores` on a driver lock held
by a worker, that worker is preempted by any `SCHED_OTHER` process, and the composite waits on
unrelated system load. Putting the pool one priority below the frame thread makes that impossible.
We do not own those threads, but they are in our address space: after device creation, walk
`/proc/self/task/`, match `comm`, and set policy — which works under `LimitRTPRIO=` alone and
needs no capability.

Interference is bounded by capping the pool *size*, not CPU time. A tile cannot be preempted
mid-raster anyway, so bounding concurrency bounds interference more predictably than bounding
aggregate time, and it degrades into a longer frame rather than a stall waiting for a throttle
window to refill.

Independently of path: **`RLIMIT_RTTIME` on every real-time thread.** Not a residency cap — a
watchdog against a `SCHED_FIFO` thread running continuously without blocking. That is a hard lock
where `sched_rt_runtime_us` has been set to `-1`, as a tuned system often does, and on a defaulted
system it leaves `SCHED_OTHER` the 5% the throttle reserves — a machine with a frozen display, no
VT, and a twentieth of a CPU, which is the same outcome by a slower route. See
[decision 61](Decisions.md#61-the-frame-thread-is-sched_fifo-the-earliest-deadline-first-schedule-is-gyros-not-the-kernels).

### Device migration

`simpledrm` binds the EFI framebuffer before the real driver loads and is replaced by it. gyro
starts on whatever is present and migrates, which means **device-loss recovery runs on every boot**
instead of rotting until a rare hotplug finds its bugs.

Migration is free, because KMS holds the last frame. Nothing is rendered during the transition — the
current scanout buffer stays on screen at zero cost while the new device comes up, then gyro flips.
At boot the content is a static logo, so it is invisible; mid-session GPU loss becomes "the screen
holds, then resumes". It is an **admitted multi-frame stall**, outside the promises in
[decision 35](Decisions.md#35-a-miss-costs-one-frame-bounded-by-the-floor-composite), and what is
guaranteed instead is that the last frame stays on glass throughout.

Three things it forces, none retrofittable:

- **No GPU resource may be the only copy of anything.** Every texture, atlas, and buffer gyro owns
  must be re-creatable from CPU-side truth. "Upload and forget" is banned — the same discipline
  class as no allocation on the frame path.
- **`wp_linux_dmabuf_feedback_v1` from the start**, never static format advertisement. lavapipe
  accepts only `DRM_FORMAT_MOD_LINEAR`, so a buffer a client allocated tiled for amdgpu will not
  import into it and the reverse; changing device changes the modifier set and clients must
  re-allocate. Wayland has exactly this mechanism, and building the older static path forecloses
  migration.
- **All Vulkan handles behind a unit that tears down and rebuilds whole.** No global `VkDevice`, no
static pipeline handles.

**[Exit snapshots](Animation.md#exit-pixels) are the one admitted exception to the first rule**,
because their source pixels belong to a client that may already be gone and there is no CPU-side
truth to rebuild them from. They are not made re-creatable — they are *permitted to be lost*, and
the loss path is the eviction the snapshot atlas needs for memory pressure regardless. Surprise
removal is what settles this: unplug a Thunderbolt GPU and there is no notice and no chance to read
anything back, so every reclamation path must work with no GPU at all. Which is an argument for the
discipline everywhere it can be met, and for permitting loss in the one place it cannot.

## Threads

Three of them carry the design, and the boundaries between them are it. A fourth exists to hold work
that has no deadline at all.

| Thread | Runs | May allocate |
| --- | --- | --- |
| Frame | Evaluate, record, submit, present | No, inside the frame section |
| Device workers | llvmpipe rasterization, [software path only](#software-rendering-is-the-floor-tier) | Not ours to say |
| Dispatch | Wire protocol, input, buffer import, **the world** | Yes, and it must |
| Helper | Configuration parsing, background persistence | Yes, and it blocks freely |

**The world is on the dispatch thread**, and this is the part that is easy to leave unstated until
it is wrong. Window-management mechanism, the entity graph, the differ, and every `Animatable` live
beside the wire protocol and input, because the chain from an input event to a commit is one causal
sequence and its front end is already here. The frame thread receives the result through [the
publication boundary](#the-publication-boundary) and authors none of it. Policy above that mechanism
is not here at all — it is a client, per [The shell](#the-shell).

The helper thread is not part of the argument; it is where multi-megabyte writes and configuration
parses go so that they are not on the dispatch thread, whose latency a user feels through input even
though it owes no frame.

**The priority order follows from what blocks on what**, and is the same on every device: frame
thread highest, device workers below it, dispatch last. The frame thread *waits* on device workers —
a `vkQueueSubmit` or `vkWaitSemaphores` against llvmpipe is a wait on threads in our own address
space — so those must outrank everything beneath them, or a preempted worker stalls a composite. The
frame thread never waits on dispatch, so dispatch goes last. Absolute levels differ between the
accelerated and software paths; the order does not. Nested and headless are exempt from `SCHED_FIFO`
altogether, consistently with their exemption from `mlockall` and DRM master.

The policy is `SCHED_FIFO` and not `SCHED_DEADLINE`, which matters enough to state where the
priorities are: the [earliest-deadline-first schedule](#outputs-are-independent-periodic-tasks) is
gyro's, run in userspace over a task set no single kernel reservation can express, and whose binding
term is GPU blocking the CPU scheduler cannot observe. See
[decision 61](Decisions.md#61-the-frame-thread-is-sched_fifo-the-earliest-deadline-first-schedule-is-gyros-not-the-kernels).

### The publication boundary

The frame thread reads the world through a single-producer / single-consumer publication. It is the
*only* path into the frame thread. A return path exists and is described below, but it carries no
state anything renders from. Four of the forward channel's properties are requirements rather than
optimizations:

- **Wait-free on the reader.** The frame thread swaps in the newest published snapshot. It never
  takes a lock the dispatch thread can hold, because a lock shared with a thread it outranks is
  precisely the priority inversion the ordering above exists to prevent.
- **The unit is a commit, not a message.** [Declarative
  commits](Decisions.md#14-declarative-commits-with-dirty-tracking) already make an atomic set of
  surface state the natural granularity, so a surface and its subsurfaces cross together or not at
  all. Publishing per message would let a frame observe half a transaction.
- **Animation crosses as coefficients, never as values.** A published spring is
  `(u₀, v₀, t₀, ω, ζ, target)`, and the frame thread evaluates it once per output at that output's
  own predicted presentation time. Publishing evaluated values would be cheaper and would silently
  collapse the scene back to a single timeline — the defect
  [Presentation timing](#presentation-timing) exists to avoid, reintroduced at the boundary instead
  of in the solver, and invisible until a second monitor is plugged in.
- **Reclamation is deferred and one-way.** The frame thread publishes the sequence it last consumed;
  the dispatch thread frees below that. Shared ownership across the boundary puts `free` on the
  frame path wearing a destructor's clothes, where the debug allocator will not catch it.

**The reader takes the newest and skips the rest, and a full ring defers rather than drops.** A
snapshot is complete scene state, so one nobody read costs nothing — which makes skipping free and
makes one thing forbidden: *nothing may be owed once per published snapshot, only once per rendered
frame.* On the writer's side, the slot a snapshot lands in is never reused until the watermark has
passed it, which is what keeps the reader's acquire a single indexed read with no retry; and when
every slot is still live, dispatch retains the snapshot and retries rather than discarding it, because
a discarded *last* publish before the scene quiesces is never re-sent. See
[decision 74](Decisions.md#74-the-forward-ring-recycles-only-below-the-watermark-and-a-full-ring-defers).

Publishing coefficients is only available because animation is [closed
form](Animation.md#closed-form-not-integrated). An integrated spring must be *advanced* by whoever
evaluates it, so the frame thread would write to what it reads and the boundary would need to be
bidirectional and stateful — a lock, across exactly the priority ordering above. This is the largest
thing the closed-form solver buys that is not visible from the solver.

Two consequences follow for free. The active-spring array is not a mirror maintained alongside the
nodes but the **serialization the publisher emits**, so it is contiguous by construction and cannot
disagree with anything. And settling needs no coordination: settling time is analytic, so the
dispatch thread knows when each spring finishes without evaluating it, and schedules entity
destruction, retiring-set drainage, and [atlas](Animation.md#where-snapshots-live) release on its
own timer — all on the side that is permitted to allocate.

**A commit lands in the frame whose record point it beats, and otherwise in the next one.** Wayland
permits this unconditionally — clients are paced by frame callbacks and have no say in when a
compositor reads them — so nothing is given up to get it. See
[decision 45](Decisions.md#45-protocol-dispatch-is-a-thread-not-a-task).

**What is deliberately on the far side:** per-message allocation, `wl_shm` mapping and the SIGBUS
window it opens, dmabuf import, Vulkan resource creation from client buffers, and libinput. Every
one of them is unbounded, and every one of them was implicitly on the frame path before the split.

**The return path is a second channel, and it is designed rather than incidental.** Some things are
frame-thread knowledge that the dispatch thread needs: `wl_surface.frame` callbacks,
`wp_presentation_feedback` with the timestamp the flip actually landed at, `wl_buffer.release`, the
per-buffer hold below, and the measured costs that feed
[budgets](#budgets) and admission control. None of it is state anything renders from, which is what
keeps the forward channel's exclusivity meaningful — but it is real traffic, and leaving it
undescribed only scatters it across a handful of ad-hoc mechanisms with a handful of lifetimes.

So: a second SPSC queue, frame → dispatch, carrying small POD records. It is wait-free on the
**writer** this time, which means bounded, and the failure mode to design against is a stall on the
frame thread, not a dropped callback. The consumed-sequence watermark rides the same channel.

**The record is a per-frame summary, and that is what removes the drop policy.** Dropping looked
acceptable until the cost was named: a lost `wl_surface.frame` callback is a hang rather than a
hitch, because a client waiting on it never draws again. But dispatch *authored* the snapshot, so a
report saying which sequence was presented lets it derive the callbacks and the buffer releases
itself — the forward channel's publish-and-derive run backwards — which collapses the traffic from
outputs × surfaces to one fixed-size record per frame. The bound then follows from something already
true: the frame thread cannot allocate, so every set it holds is fixed-capacity, and a record sized
from those ceilings has nothing left to drop. A full queue is merged into the writer's own staged
report, and a hold that does not fit is simply not released this frame. See
[decision 75](Decisions.md#75-the-return-channel-is-one-report-per-frame-per-surface-facts-are-derived-not-sent).

**Cross-thread lifetime is the cost.** A client may destroy a surface while the frame thread holds
it. [Generational handles](Decisions.md#15-identity-is-a-generational-handle) supply the detection
and [exit snapshots](Decisions.md#20-exit-animations-use-full-resolution-snapshots) already
establish that an entity outlives its protocol object, so the mechanisms exist — but the discipline
is new, and it is the one part of this split that is not free.

**Reclamation needs a per-buffer hold as well as the watermark.** The exit path defers its snapshot
blit by a frame or two to keep it off the budget of the frame where the surface died
([Animation.md](Animation.md#exit-pixels)), which means the frame thread holds one client buffer
past the commit that carried it. The consumed-sequence watermark cannot express that — withholding
it would stall reclamation of every unrelated commit below — so an individual hold, released over
the return path when the blit lands, sits alongside the watermark rather than being encoded in it.
It is the only reader-side hold in the design, and it should stay that way.

## Presentation timing

Mixed-refresh multi-monitor is where compositors are worst today and where gyro should be decisively
best. It is also the everyday case: a 144 Hz laptop panel and a 60 Hz projector is a Tuesday, not an
exotic configuration.

The symptom everyone has shipped: **plug a 60 Hz external into a 144 Hz laptop and the laptop
panel's animations drop to 60.** Alongside it, dragging a window across the seam judders on one side
or both, and video playing on the external hitches the internal.

The cause is a single global frame clock driving animation state that advances by `Tick(delta)`.
With one mutable timeline there is one delta per iteration, so the compositor either runs everything
at the slowest output's rate or shows the slow output stale frames. Per-output frame clocks — which
Mutter and KWin both adopted years ago — fix the *scheduling* half, but the animation state
underneath remains one timeline advanced by one delta, so the visible problem survives the fix.

### The timebase

Before any of that: **there is no global clock, and there is exactly one timebase.** Those are
different statements and both are needed. The first says outputs do not share a schedule and is the
whole of this section. The second says every timestamp in the process is comparable — monotonic
nanoseconds, converted once at ingest and never observed anywhere else. See
[decision 57](Decisions.md#57-one-timebase-clock_monotonic-converted-at-ingest-and-nowhere-else).

gyro does not choose the clock; everything it speaks chose already. libinput reports
`CLOCK_MONOTONIC` in microseconds, DRM page flips report it only when
`DRM_CAP_TIMESTAMP_MONOTONIC` says so, io_uring times out against it by default, and
`wp_presentation` makes gyro *advertise* a `clock_id` to clients — once, at bind, for the life of
the connection. That last one is why this is fixed by fiat rather than adopted per device: outputs
are hotpluggable and the advertisement is not, so a device whose driver disagrees is converted at
its own ingest.

Two clocks are refused, and one of them is refused for a reason that is invisible until it is
expensive. `CLOCK_REALTIME` steps, and a stepped clock on the frame path schedules a frame into last
Tuesday. `CLOCK_BOOTTIME` looks correct for idle timers and is worse, because **Linux's
`CLOCK_MONOTONIC` does not advance across suspend and `CLOCK_BOOTTIME` does** — so mixing the two
puts the frame ring's timers and every `FrameClock` observation in domains separated by the suspend
duration, exactly once, on the first lid-open. What replaces it is an explicit resume event, which
[Idle and power](#idle-and-power) needs to exist regardless.

The consequence for everything below is that a `FrameClock` reading, an input event's `t₀`, a
`renderBudget` high-water mark, and a page-flip timestamp are all in one domain — so
**input-to-photon is a subtraction rather than an estimate**, per output, and can be asserted on
rather than described.

### Per-output evaluation

gyro cannot have that bug, and the reason is
[decision 11](Decisions.md#11-springs-are-closed-form-not-numerically-integrated). Animation is a
pure function of time, so **the same scene can be evaluated at two different presentation times in
the same iteration and both results are exact.**

A window dragged across the seam is evaluated at the panel's next vblank for the panel and at the
projector's next vblank for the projector. Both correct, simultaneously, with no extrapolation and
no compromise rate. This is structurally unavailable to anything that integrates numerically, and it
is the largest single thing separating gyro from the field on this problem.

The consequence, which everything below assumes: **a frame is `(output, predicted presentation
time)`, not a tick.** Animation evaluation is per presentation target. There is no global "now" on
the render path.

The traversal that makes this possible carries a second per-output property nearly for free. A frame
is also `(output, device grid)`, and [Geometry](#quantization-belongs-to-the-output) puts rounding
there for the same reason evaluation is there: one model value, two correct answers, and no way to
express either by storing a single result.

### Outputs are independent periodic tasks

Two outputs are genuinely independent. **The only thing coupling them is that they share one frame
thread and one GPU queue.** There is no other channel, and modelling this as anything more
complicated than resource contention is how the wrong design gets built.

So this is the classic periodic real-time task problem, and it should be treated as exactly that:

- Each output is a task with a period `P`, a measured execution time `C`, and a deadline equal to
  its period.
- The frame thread is a uniprocessor running them earliest-deadline-first.
- Composites are **non-preemptive** — once GPU work is submitted it runs to completion.

`P` is **the shortest interval in which the output may demand a frame**, which on a fixed-refresh
output is its measured period and nothing more interesting. The distinction earns its keep only on a
variable-refresh one, where the two genuinely differ and where taking the observed period would
quietly void the guarantee below — see [VRR](#vrr-as-a-scheduling-degree-of-freedom).
Processor-demand analysis holds for sporadic tasks under a minimum separation exactly as it does for
periodic ones, so this changes no symbol in the test.

The property that matters: **the schedulability test does not take phase as an input.** It is a
processor-demand test over interval lengths rather than a simulation of a schedule, so it holds at
every phase relationship by construction. Which gives the design its goal state:

> If the task set passes the test, every deadline is met at every phase relationship, and the
> outputs never influence each other at all. If it fails, no amount of phase modelling saves it.

The test is demand plus blocking, checked over every interval length that can bind:

```
	U    = Σᵢ Cᵢ / Pᵢ

	h(L) = Σᵢ ⌊L / Pᵢ⌋ · Cᵢ            work with a deadline at or before L
	B(L) = max{ qₖ : Pₖ > L }          the longest non-preemptible chunk of any
	                                    output whose deadline falls after L

	feasible  ⟺  U ≤ 1  and  ∀ L ∈ 𝓛 :  h(L) + B(L) ≤ L
```

`qₖ` is output `k`'s largest non-preemptible chunk — equal to `Cₖ` unless it is
[chunked](#chunking), which is the one and only way chunking enters the test. `𝓛` is every multiple
of every `Pᵢ` up to the synchronous busy period `L* = Σ Cᵢ / (1 − U)`, capped by the hyperperiod.
Because `L*` diverges as `U → 1`, sets above `U = 0.95` are rejected before the loop runs. Time is
integer nanoseconds, so the floors are exact rather than a discretization of a continuous model.

With two outputs this reduces to the simple form: at `L = P_fast` the demand is `C_fast` and the
blocking is `C_slow`, so `C_fast + C_slow ≤ P_fast`. On a 144 Hz panel beside a 60 Hz projector,
with a 4 ms panel composite, **the projector's entire composite must fit in under 2.9 ms.** That is
the number the configuration is quoted against.

Note what it is not sensitive to. Utilization here is `4/6.944 + 5/16.667 = 0.88`, comfortably below
1 — the set is utilization-feasible and non-preemptively infeasible. The slack exists; it is in the
wrong place. That distinction is what the mechanisms below act on.

The general form is needed as soon as there are three displays, because the simple one accounts for
a single fast job plus blocking rather than for accumulated demand. Two 144 Hz panels at 3 ms beside
a 60 Hz projector at 2 ms satisfies `C_fast + max(C_other) ≤ P_fast` and still misses: with the
projector's job in flight at the critical instant, the second panel finishes at 8 ms against a 6.944
ms deadline. `h(P_fast) + B(P_fast) = (3 + 3) + 2 = 8 > 6.944` catches it.

**Protocol dispatch and input are not in the set.** They run on [their own thread](#threads), which
the frame thread outranks and never blocks on, so they are preempted rather than accumulated and
enter neither `h` nor `B`. The set is the outputs, and only the outputs.

The general rule, since "it runs on another thread" is not by itself the reason: **work belongs in
the test exactly when the frame thread waits on it.** Device workers on the software path are inside
`C` by that rule despite being separate threads; dispatch is outside it because nothing above it
ever waits. See [decision 45](Decisions.md#45-protocol-dispatch-is-a-thread-not-a-task).

### Phase drift is not something to track

144 Hz and 60 Hz have periods of 6.944 ms and 16.667 ms, a clean 12:5 ratio realigning every
83.33 ms. That is a useful intuition and a trap, because real panels do not run at nominal rates — a
"144 Hz" panel might be 143.98 and a "60 Hz" projector 59.94. The phase relationship slides, and two
nominally identical 60 Hz monitors drift past each other over seconds.

Drift's role in the design is entirely negative: **it guarantees that every phase relationship will
eventually occur.** A scheduler that observes a favourable phase and relies on it is therefore
wrong, not unlucky. That is the whole reason to design against the critical instant rather than
against the observed schedule, and it is why a rolling collision forecast is the wrong shape — it
solves reactively, per collision, a problem that admits a static answer.

Measured periods still matter, which is what `FrameClock::Observe` is for. They are where `P` comes
from on a fixed output, and where the servo's plant estimate comes from on a variable one — with `C`
they are the whole of what the test above is computed from. Phase does not feed anything.

### Admission control

The test runs when an input to it changes — an output added or removed, a mode set, a measured cost
moving the floor, a variable-refresh output's refresh passing to or from a client — not every frame.
Stating the trigger as *any input to the test* rather than as a list of events is deliberate: the
list is what a reader checks against, and the one that was missing is the subject of
[VRR](#vrr-as-a-scheduling-degree-of-freedom) below. It is solved for `C`, so its output is an
**allocation**: how much each output may spend per frame. See
[decision 29](Decisions.md#29-outputs-are-periodic-real-time-tasks-the-test-allocates-effect-budget)
for why `C` is a budget gyro enforces rather than a cost it observes — briefly, `C` is a property of
the scene as much as of the output, and the peak is an overview transition nobody can predict at
plug-in time.

Where the allocation is smaller than the current scene wants, the ladder is:

1. **Lengthen a VRR output's period.** Free — no visual cost. Available only when one of the outputs
   is variable-refresh, its refresh is gyro's to set rather than a client's, and the result stays
   inside the panel's supported range. See [VRR](#vrr-as-a-scheduling-degree-of-freedom).
2. **Step the effect quality tier down** on the output that can least afford it. Nearly free at the
   first rung, and this is expected to be the answer almost every time. See
   [Effects and quality](#effects-and-quality).
3. **Chunk the slow output's work**, so the blocking term becomes the largest chunk rather than the
   largest job. *Contingency — gated on measurement, not first-cut work.*
4. **Render the slow output early**, so its work may be placed in slack elsewhere in the cycle.
   *Contingency — same gate.*
5. **Accept a known drop pattern**, repeating frames on the losing output.

Which output loses at step 5, and which is chosen at steps 2 through 4: **the one without input
focus, ties broken toward the slower refresh.** The panel under someone's hands is where latency is
perceived; a sixtieth of a second of staleness on a projector is close to invisible, and the same
hitch on a 144 Hz panel being touched is not.

Steps 3 and 4 are described below because the interfaces have to leave room for them, not because
they are early work. A configuration that cannot be rescued by step 2 has to be observed before
either is worth building.

**Phase is not a rung and cannot become one.** Aligning two outputs' vblanks changes no input to the
test, so it cannot move a set from infeasible to feasible, and
[phase drift](#phase-drift-is-not-something-to-track) is the whole of the argument. Variable refresh
does not exempt itself from it by making phase controllable: a servo can *hold* a relationship that
drift would otherwise walk through, but a set admitted on that basis depends on keeping the lock,
and the lock is lost exactly when it is least convenient — the panel's range runs out, or a client
takes the refresh rate. So phase alignment may improve an already-feasible set and may never be the
reason one is admitted. Rung 1 is the period, and only the period.

The hardware cursor plane is exempt from all of this. It updates per CRTC independently of the
composite, so even a dropped composite on a 144 Hz panel leaves the cursor moving at 144 Hz.

### Chunking

Splitting an output's work at render-pass and submission boundaries reduces the blocking term from
`max(C_other)` to `max(chunk)`. Blur pass chains split naturally, which is convenient, because blur
is most of what makes `C` large in the first place.

Chunking and early rendering compose, and together they are the general answer: chunk the slow
output's work into pieces that fit the gaps between fast-output composites, spread them across
several fast periods, and hold the target until its flip. As chunk size falls this approaches the
preemptive EDF bound of `U ≤ 1`, which is the uniprocessor optimum.

> Written from specification. A submission boundary is a *scheduling opportunity* for the GPU, not a
> guaranteed preemption point, and how well this works is hardware-dependent. Marked `// SPEC:`, and
> on the list for the DRM hardware spike to measure.

### Speculative early rendering

Producing output X's frame for presentation time T at wall-clock `T − k·period(X) − budget` rather
than at `T − budget`.

What it buys, in scheduling terms, is a **relaxed release-time constraint**. Instead of "this job
must run in the window immediately before its own deadline", it becomes "this job may run in any
window within the period preceding its deadline" — which lets work move into slack that exists
elsewhere in the cycle. That is precisely the failure mode identified above, where utilization is
below 1 but the slack is in the wrong place.

It is not a universal solvent. In the 144 + 60 example the gap between consecutive panel composites
is `6.944 − 4 = 2.944 ms`, so a contiguous 5 ms projector composite fits in no gap at all, early or
otherwise. Early rendering fixes cases where the slow job fits *somewhere*; chunking is what fixes
the rest, and the two are meant to be used together.

The content trade is exact, and it is the whole reason this is safe:

- **Animation state is correct**, because `Evaluate(T)` is exact for any future T. Nothing is
  extrapolated and nothing is stale.
- **Client content is what you pay.** The recorded frame samples whatever buffer the client had
  committed at record time, so a client committing between record and present is shown one frame
  late.

Which gives the policy directly: **early rendering is nearly free when an output's content is
compositor-driven and expensive when it is client-driven.** A workspace switch, an overview
transition, or a window animating out is pure animation and loses nothing. A fullscreen video or a
game loses a frame of latency. So the decision is per output, gated on whether any surface there
committed recently — and, per the rule above, never applied to the input-focused output.

Two mechanics are easy to miss and both constrain the frame loop:

- **The record moves and the commit does not.** Issuing the commit at the early record point flips
  the frame at the *previous* frame's vblank — the early frame lands a period early and the one
  before it never lands at all. So the output still wakes in the frame's own commit window, and what
  that wake costs is a `Present` rather than a composite. An output at lead `k` contributes the
  earlier of two instants to [the wake fold](#doing-nothing-must-cost-nothing): the placed record
  point of the frame it is producing, and the commit point of the frame it already holds.
- **What bounds an early frame is its placement, not its deadline.** That deadline is `k` periods
  away, so the [record-time check](#the-frame-loop) would admit the work however long it ran; what
  has to hold is the end of the gap it was placed in, since overrunning that is what blocks the fast
  output. The binding deadline is the earlier of the two, and at lead 0 the placement is the
  deadline — which is why that check reads as one number.

Two refinements make it better than the naive version:

- **Pull the frame callback forward too.** Frame callbacks are the compositor's lever on when
  clients produce, and because the schedule is known statically, the clients on that output can be
  asked for their content earlier. For clients that use `wp_presentation_feedback` and render for a
  stated target time this recovers the freshness entirely; for clients that simply draw on callback
  it is neutral. Nobody uses frame callbacks predictively today.
- **Bound the lead to one period, and require a free target.** Rendering one frame ahead means three
  targets in flight on that output — one scanning out, one pending flip, one being recorded. The
  ring depth is also what enforces the bound: `AcquireTarget()` answers nothing while every target
  is held, so a double-buffered output cannot run ahead whatever else is true, and a lead is
  something the admitted plan grants rather than something the loop falls into.

That last point is **not** triple buffering in the sense that costs latency. The conventional
version pipelines every frame through a three-deep queue and pays a full period on all of them.
Here the third target is *allocated*, not *occupied*: pipeline depth stays at one, gyro still
records late by construction at `deadline − renderBudget − safety`, and the extra period is paid
only on frames actually rendered early.

| | Cost |
| --- | --- |
| VRAM | one extra target on outputs that may render early |
| Steady-state latency | none — pipeline depth is unchanged |
| Latency on an early-rendered frame | one period, on that output only, and only if its content is client-driven |

Because the input-focused output is never chosen for early rendering, the panel under someone's
hands keeps late-latched, double-buffered latency unconditionally.

### VRR as a scheduling degree of freedom

Variable refresh is treated everywhere as a latency feature for games: present when content is
ready. It is also the only control gyro has over *when* an output's vblank happens, and a laptop
with a VRR panel beside a fixed external is a very common configuration.

So when admission control finds a configuration infeasible, a VRR output's period is the first thing
adjusted — lengthening `P` for that output changes the test's inputs, and it is the only input that
can be changed without giving anything up.

**But `P` is gyro's to change only while gyro decides when frames are demanded.** A client whose
arrivals gyro cannot predict takes that decision away, and the conflict is resolved in the client's
favour: **content-driven VRR wins over scheduling-driven VRR.** gyro does not servo against it, and
admission control falls through to the next rung.

**What takes it is unpredictability, not foreground.** An output is client-paced when it carries an
active producer whose next arrival gyro cannot predict — one that commits and never says when it
will commit next. A client that posts a `wp_commit_timing_v1` timestamp has named the instant, and a
client pairing `wp_fifo_v1` barriers has asked to be paced at the refresh cycle; in both cases gyro
keeps authority and servoes *to* the stated cadence rather than against it. Being fullscreen grants
nothing. It remains a real fact about *cost*, since it is what makes direct scanout available and
therefore what makes a high rate affordable, and cost is [admission control](#admission-control)'s
subject rather than this one's. See
[decision 76](Decisions.md#76-cadence-authority-follows-predictability-not-foreground).

What follows is then not a matter of policy, because `P` is the shortest interval in which an output
may demand a frame:

| Who controls arrivals | `P` |
| --- | --- |
| Fixed-refresh output | its measured period |
| VRR, gyro's | the commanded period — and the servo may not command shorter than was admitted |
| VRR, a client's stated cadence | the commanded period, servoed to what the client asked for |
| VRR, a client free-running | the panel's minimum period, whatever the client happens to be doing |

The last row is the one that costs something. A client presenting at 60 Hz on a 144 Hz panel is
budgeted at 6.944 ms all the same, because nothing stops it presenting faster on the very next frame
and the guarantee is over every interval rather than over the observed one. That is pessimistic, and
it is not negotiable without giving up the property that makes the test worth having.

The row above it is the one that used to be folded into it, and separating them is most of what
[decision 76](Decisions.md#76-cadence-authority-follows-predictability-not-foreground) buys. A client
that has stated its cadence is budgeted at the period it stated, because gyro is the one commanding
that period and can hold it. Video is the overwhelming majority of what plays fullscreen, so the
common case moves from the bottom row to this one and stops costing a second output anything at all.

**Control changing hands is a configuration change.** This is the price of using VRR as a lever at
all, and it is worth putting as a failure rather than as a rule, because the rule reads like
bookkeeping and the failure does not. Suppose gyro servoes a 144 Hz panel down to 100 Hz to fit a
projector beside it, and admits the set. A game goes fullscreen on the panel and takes the refresh
rate. `P` returns to 6.944 ms, demand rises, and the projector starts missing — quietly, because
[the frame loop](#the-frame-loop) contains a miss by falling to the floor tier rather than by
reporting one. Nothing changed that an event-shaped trigger list would have recognised, so admission
control never re-ran and never stepped the projector's quality tier, which was sitting there
available the whole time. That is why the trigger is stated as any input to the test.

Re-solving on that transition is not work for the frame thread — `𝓛` is not a usefully bounded loop.
It does not have to be. Control of each VRR output is binary, so a configuration with `n` of them
has `2ⁿ` plans, `n` is one or two on real hardware, and admission control solves all of them when
the configuration changes. The transition is then a plan swap rather than a solve, which preserves
the property that [the frame loop](#the-frame-loop) runs a schedule it was handed and decides nothing
about timing itself. Where `2ⁿ` stops being small, the surplus outputs are planned as
client-controlled — the pessimistic direction, and the one that cannot produce a miss.

**Detection is deliberately asymmetric.** Reading a client as in control when it is not costs a
rung; reading it as not in control when it is costs missed deadlines on another output. So the
client-controlled reading is entered readily and left only on hysteresis — the same shape [client
cadence](#client-cadence-on-multiple-outputs) needs, for the same reason. That asymmetry is also why
silence reads as control: a client that has stated no cadence is assumed to have taken the rate. The
hysteresis applies to that inference and not to a client that has spoken, since a protocol binding is
stable where an observed rate flickers frame to frame.

The adjustment is constrained three ways, all pushing the same direction:

- The resulting interval must stay inside the panel's supported range, clear of low-framerate
  compensation at the bottom.
- Abrupt refresh changes cause visible brightness flicker on many panels.
- A large step is perceptible as a stutter even when every deadline is met.

All three are answered by rate-limiting the adjustment — a small bound per frame, converging on the
target period over several frames. It is a servo, not a jump. It converges on a period and never on
a phase relationship, per [admission control](#admission-control) above.

**The servo is a property of the clock, not of the present.** It commands a period on the output's
[`FrameClock`](#the-frame-clock) and the existing machinery carries it from there: `NextDeadline()`
moves, the frame loop arms its timer against the new value, and on KMS with variable refresh enabled
a flip presents at or after the commit — so shaping the clock is shaping presentation, and nothing
is added to `IPresenter` for the servo. The one thing that does reach the backend is enabling
variable refresh on the CRTC at all, which is set when a mode is set rather than per frame, and which
therefore belongs to [`Reconfigure()`](#presentation) rather than to the servo.

That division is sharper than it first looked. Once variable refresh is active the effective interval
is carried entirely by *when the flip is submitted* — there is no per-frame property to set, on
either of the drivers read — so the servo's period command reaching the panel through the clock is
not one mechanism among several but the only one there is. Enabling is configuration; cadence is the
lever. The cost is that `[vmin, vmax]` is derived from the mode and the panel's reported range and
cannot be asked for, so the servo's authority is bounded by a number gyro learns only after setting a
mode. See [decision 31](Decisions.md#31-vrr-is-a-scheduling-degree-of-freedom-not-only-a-latency-feature).

Putting it on the clock also keeps the servo honest about what it knows. It closes on an observation
rather than on an acknowledgement: it commands a period and learns from the next `Observe()` what
the panel actually did. Panels misreport their ranges, so a backend that validated a requested
period would be validating against the same documentation the servo already has reason to distrust.

**An idle VRR output gets no keepalive commit.** [Doing nothing must cost
nothing](#doing-nothing-must-cost-nothing) is a hard invariant, and a timer armed to hold a refresh
rate would break it on every VRR laptop, in precisely the way that section exists to prevent. None
is needed: the panel holds the last buffer scanned out to it without gyro's help, the same property
[resume](#suspend-and-resume) relies on. What does go stale is the clock, and here the variable case
differs from the fixed one in kind rather than in degree — an idle fixed panel keeps its period and
its clock merely accumulates drift, while an idle VRR panel has genuinely changed period, so its
last observation is not stale but wrong. Going idle therefore invalidates it. The first frame
afterwards is driven by damage rather than by a timer, presents as soon as it is ready, and re-seeds
the clock through `Observe()` — which is variable refresh behaving as variable refresh.

> Written from specification. Panel range behaviour and flicker thresholds are from documentation
> rather than from hardware, as is the interaction between cursor-plane commits and the refresh
> timer — [admission control](#admission-control) exempts the cursor plane because it updates
> independently of the composite, which is a weaker claim on a VRR panel than on a fixed one. All
> marked `// SPEC:` where they land in code.

### Client cadence on multiple outputs

A surface visible on a 144 Hz output and a 60 Hz output still has exactly one `wl_surface.frame`
cadence. The policy:

- **Drive at the fastest output the surface touches.** Content is then never stale on either.
- **Apply hysteresis**, so a drag across the seam does not oscillate the cadence.
- **Never switch cadence mid-animation.** A client changing its render rate mid-gesture hitches
  visibly, and users blame the compositor for it.

That last rule is precise rather than heuristic, because settling time is analytic — gyro knows the
exact moment a surface's own animation completes and can defer the switch to it.

### Budgets

`renderBudget` is per output, since resolution and effect load differ. It supplies `C` in the
schedulability test, which means the figure that matters is a **worst case, not a percentile.** A
p99 admits one frame in a hundred over budget, and since drift guarantees every phase relationship
eventually occurs, those frames will eventually land on the critical instant. Sizing to a percentile
looks healthy in aggregate and misses periodically forever.

Budgets are therefore tracked as a high-water mark over a window, and a change in that mark is a
configuration change — it re-runs admission control.

GPU work from two outputs serializes on the queue regardless of how it was recorded, which is why
parallel command recording does not make an infeasible set feasible — see
[decision 29](Decisions.md#29-outputs-are-periodic-real-time-tasks-the-test-allocates-effect-budget).

## Idle and power

Everything above is about hitting deadlines. This is about the frames gyro does not draw, and it is
as much the product as the other kind — a compositor that never misses and costs two hours of
battery has not succeeded at anything.

[Boot](#boot-and-the-display-lifetime) covers the two ends of the display's lifetime. This is the
middle, and it is the part a user meets several times a day. Rationale and rejected alternatives in
[decisions 58 and 59](Decisions.md#idle-and-power).

### Doing nothing must cost nothing

The invariant is hard: **when nothing is animating and nothing has committed, no timer is armed and
the frame thread blocks indefinitely.**

This is only exactly answerable because settling time is
[analytic](Animation.md#closed-form-not-integrated). The dispatch thread knows the instant the last
spring settles without evaluating anything, so it knows when there is nothing to wake for — an
integrated spring would have to be woken to discover it had nothing to do, which is the same defect
[Per-output evaluation](#per-output-evaluation) rejects, showing up on the power bill instead of on
the seam between two monitors.

**The invariant is a fold, and what it folds is a `Wake` rather than a boolean.** *(Written against
the implementation, 2026-08-16; client contributions added 2026-08-17.)* Every animating channel,
every pending timeout, every retiring entity, and every surface holding an outstanding commitment
contributes one; `Sooner` reduces them to the schedule, with *settled* as the identity, so a scene in
which nothing contributes arms nothing. The last of those is a client's own statement about when it
will next produce — a `wp_commit_timing_v1` timestamp is `Timed`, a standing `wp_fifo_v1` pairing is
`Continuous` at the refresh period — and without it an output showing nothing but a video folds to
*settled* between frames. See
[decision 76](Decisions.md#76-cadence-authority-follows-predictability-not-foreground). Stating it that way strengthens the invariant rather
than restating it: *no timer armed* becomes **at most one timer armed per output, and its instant is
the fold** — which absorbs the dim and blank timeouts below, the gesture-stop republish in
[Animation.md](Animation.md#it-crosses-the-boundary-as-coefficients-like-everything-else), and
retirement expiry into one reduction instead of leaving each of them a separate arm. It is also the
only thing that gives periodic motion a way into this ladder at all, which the recovery console's
[blinking cursor](#the-pre-vulkan-console) makes a shipping requirement rather than a nicety. The
argument for the shape, including why an optional instant is not it, is in
[Animation.md](Animation.md#settling-answers-with-a-wake-not-a-boolean).

Three things follow that are easy to get wrong by omission. Cursor motion over an otherwise static
screen updates the cursor plane and triggers no composite — the plane is already exempt from
[admission control](#admission-control), and this is the other half of that exemption. A blinking
text cursor on one output must not wake the other, which per-output damage already gives, and which
the fold above is partitioned by for the same reason: it is associative, so it may be reduced over
the contributors to one output alone. And a variable-refresh panel needs no keepalive commit to hold
its rate, which is the omission-error
[the VRR servo](#vrr-as-a-scheduling-degree-of-freedom) is most likely to introduce and the one that
would cost the most, since it would arm a timer on the machine most likely to be running on a
battery.

It is falsifiable, so it is a test rather than an aspiration: a static scene in the headless
harness, N seconds, assert zero composites and no armed timer. That belongs beside the schedulability
sweep, and it is the same argument [the floor tier](#the-floor-tier) makes — a property nobody
exercises is a property nobody has. *(Written, 2026-08-22, and in two places because the invariant has
two halves.)* `Source/Integration/Schedulability.Test.cpp` holds the fold's: an idle scene arms nothing
across sixty-six seconds of virtual time, damage settles back to idle in a fixed number of iterations
that does not grow with how long the run has been going, and a settled output declines every one of the
hundreds of iterations its animating neighbour wakes the loop for — which is the partition above,
asserted rather than argued. `Source/Compositor/Uring.Test.cpp` holds the shim's, which needs a kernel
rather than a `ManualClock`: `Wake::Never()` arms no timeout, and the wait ends only when a watched
descriptor says so. Neither of them reads a context-switch count, deliberately — an OS-level symptom
turns an invariant into a threshold, and the structural facts are available.

### The ladder

gyro owns the rungs and does not own the timeouts. Each rung trades wake latency for power, in the
same shape as the [effect quality](#quality-tiers) ladder and closed for the same reason:

| Rung | Mechanism | Wake latency |
| --- | --- | --- |
| Dim | backlight, animated | instant |
| Backlight off | connector property, CRTC still live | instant |
| Display off | atomic commit, `ACTIVE=0` | ~100 ms |
| Lock | [output reassignment](#locking) | — |
| Suspend | **not gyro's** — see [Suspend and resume](#suspend-and-resume) | seconds |

**Dimming is the backlight, not an overlay.** An alpha overlay cannot go below the panel's black and
changes color rendition on the way down. So gyro drives `/sys/class/backlight`, which adds a udev
rule to the table under [Privilege](#privilege) alongside the DRM and input ones. It also means the
idle dim and the user's brightness key are **one mechanism**: [Color](#the-composite-space) makes
brightness change available HDR headroom rather than scaling pixels, so a dim that overwrote the
user's setting instead of composing with it would silently change headroom, and one that failed to
restore it exactly would leave the display wrong after a keypress.

**The dim ramp is animated**, and it is probably the most-watched animation in the system — more
often seen than any window transition, and animated by nobody. It costs nothing here: the dim level
is a compositor-owned `Animatable`, so it is a catalog motion, and undimming on input is
interruption by retargeting, which springs do natively.

**Timeouts are configuration; the lock rung has an envelope.** Dim and blank are comfort and
battery, and a shell sets them freely. Lock is the one rung with a security consequence, so the
system configures a maximum and a shell may only be stricter — permissive by default on a laptop,
because "never lock" is a legitimate thing to want at home, and present at all because its absence
is what makes a system undeployable somewhere with a policy. Overrides are **state in gyro, not a
live subscription**: they persist when a shell dies, and a hung shell can neither change the ladder
nor hold the screen unlocked. A shell names rungs and cannot invent them, which is the same closure
[materials](#materials-not-filter-calls) and the motion catalog already have.

**Battery versus AC is a ladder selector, not a ladder.** Both profiles are gyro's configuration and
something above chooses between them. gyro does not read `/sys/class/power_supply` — power source is
not a display concern, and sampling it is exactly the kind of drift from
[declare, do not drive](#declare-do-not-drive) that starts as one exception.

### Idleness is a fold

Every other compositor's idle state is per session because the process is. gyro's is a fold over
seats, sessions, and consumers, and the terms are unfamiliar:

- **Activity belongs to a seat**, not a session. It is input activity, and it is the only thing that
  undims a panel.
- **An output's power state follows the session currently assigned to it.** A locked panel has been
  reassigned to the greeter ([Locking](#locking)), so the greeter's ladder governs it and an
  inhibitor held by the locked user's clients must not keep it lit.
- **A session with no local output but a live consumer of a
  [virtual output](#virtual-outputs-and-why-remote-desktop-is-one) is not idle**, and the machine
  must not suspend under it. [A session is not a display](#a-session-is-not-a-display) already says
  a session with no output assigned is not suspended; this is that sentence's power consequence.

**Injected input does not wake a local panel.** Injected input enters the same pipeline as local
input so that `t₀` is the event time, which is right for animation and wrong for display power —
otherwise someone remoting into a locked machine on a desk lights up its screen in a room they
cannot see. Activity is attributed to a seat, and a virtual output's input drives that output only.

**The lid is an input event.** `SW_LID` arrives through libinput like anything else, so gyro sees it
with no bus and no agent. Lid close with an external attached is an output reconfiguration, which is
gyro's; the suspend consequence, if any, is not.

### Inhibition

`zwp_idle_inhibit_v1` is [session-scoped](#filtered-globals) and is mechanism. Three details are
where implementations usually go wrong:

- **An inhibitor is effective only while its surface is genuinely presented** — mapped, on an
  output, on an output that is on. gyro is uniquely able to evaluate that, and the common
  approximation of "mapped" is why a backgrounded video tab keeps a laptop awake.
- **Inhibitors are attributed** under [resource accounting](#resource-accounting), because an
  inhibitor is how a client spends the user's battery. The attribution is a feature rather than
  bookkeeping: *what is keeping this machine awake* is a question users ask and that no Linux
  desktop answers well.
- **"Nobody has touched anything" and "the system may go idle" are different facts.**
  `ext-idle-notify-v1` grew an input-only notification because conflating them breaks activity
  tracking, and both are answerable here.

### Suspend and resume

**gyro still holds no bus connection.** The handshake rides the control connection that
[listener handover](#listener-handover) already established, with the greeter's session agent —
persistent from boot, already running a PAM stack — as the machine-level peer. It takes logind's
`delay` inhibitor, sends `suspending`, waits for gyro inside the delay window, releases.

The handshake exists because two things must happen *before* the machine stops, neither of which is
expressible as a reaction afterwards:

- **The locked state must be presented**, not merely entered. Otherwise the machine wakes showing
  the desktop for several frames while the lock screen paints — which the user experiences as their
  session being briefly readable by whoever opened the lid.
- **The frame loop must quiesce**, because it is about to schedule against a clock that stops.

**Resume is a modeset, not a wakeup.** The checklist:

| | Why |
| --- | --- |
| Invalidate every `FrameClock` | its last observation predates the suspend, and its mode may be gone |
| Reconcile outputs, re-run admission control | the machine may have been docked with the lid shut |
| Hard-settle every spring, drain the retiring set | nobody should resume into the middle of yesterday's transition |
| Expect `VK_ERROR_DEVICE_LOST` | a discrete GPU may have powered off; [migration](#device-migration) is this path |
| Absorb the client burst | every frame callback that did not fire for nine hours fires at once |

The first row is the one that bites hardest if it is forgotten: a stale clock returns a deadline in
the deep past, and [the frame loop](#the-frame-loop)'s timing policy then fails every branch forever
instead of falling to the floor tier.

**And the last frame is the resume image.** KMS holds the scanout buffer across suspend — the same
property [device migration](#device-migration) relies on — so if the locked state was presented
before suspending, resume is *the panel lighting up already showing the correct final image*. No
black frame, no flash of pre-suspend content, no repaint race. That is
[the boot seam's](#from-firmware-to-gyro) continuity argument at a seam that recurs ten times a day
rather than once, and it is what this whole section is for.

Hibernate is the same shape: the monotonic clock does not advance, KMS state is rebuilt by the
kernel, and the checklist is unchanged.

## Geometry

Scaling is the most visible thing a compositor gets wrong and the least diagnosable afterwards,
because nearly every artefact of it is a value that got rounded once and then stored. One rule fixes
most of what follows: **quantization is a property of an output, never of the world.** Rationale and
rejected alternatives in [decisions 52–56](Decisions.md#geometry).

### The spaces

| Space | Extent | Type | Determined by |
| ----- | ------ | ---- | ------------- |
| Buffer | one per attached buffer | integer texels | the client |
| *surface adapter* | one per surface | exact rational **per axis** | `buffer_transform`, `buffer_scale`, viewport `src` and `dst` |
| Global | exactly one, continuous | real, Y-down | gyro's model |
| *output adapter* | one per output | exact rational plus an integer rotation | output configuration |
| Output device | one per output | integer at the boundary only | the composite target |
| *panel adapter* | one per output | integer rotation and flip | KMS, or the composite |

The adapters are called out separately because both are **exact and declared rather than inferred.**
The surface adapter is whatever the client's protocol state says it is, never recomputed from a
scale factor. The panel adapter is distinct from output device space because a ninety-degree
rotation may be executed by a KMS plane or by us, and
[direct scanout](#direct-scanout-is-conditional) already depends on knowing which.

**All three adapters are one kind of thing**, and the table's differing descriptions of them are
constraints rather than types: a rotation, a flip, a scale per axis, and a translation, closed under
composition. What varies is which values each is held to — the panel adapter's translation is
integral, and its scale is one *unless a plane's scaler is doing the work*, which is a property of
the hardware rather than of the adapter. Reading the rows as three types costs the thing the closure
buys, which is that a chain of them composes exactly and in any grouping.

Global space is continuous, real-valued, output-independent, and Y-down, which matches Wayland's
convention and Vulkan's clip space alike. Its basis unit is "one logical pixel at scale 1", for wire
compatibility and for nothing else — nothing assumes integer alignment in it, and no logical size of
a window is stored anywhere. Positions cross [the publication boundary](#the-publication-boundary)
at double precision and everything else at single, because single gives 1/256 of a pixel around
±32768, which is exactly `wl_fixed`'s resolution and too near the floor for a large arrangement of
outputs.

### Quantization belongs to the output

No integer flows backwards into the model. Every rounding is a pure function of
`(node, output, frame)` and dies with the frame that computed it.

This is the same seam twice more. [Per-output evaluation](#per-output-evaluation) already splits one
scene into evaluations at different *times*; this splits it into evaluations on different *grids*. A
frame was `(output, predicted presentation time)`; it is also `(output, device grid)`, and the
second half is nearly free because the first already forced the traversal to be per output.

**Settled content snaps to the grid of the output being evaluated for.** A surface sampling
one-to-one at a half-device-pixel offset is soft across its whole area, which is what gets reported
as blurry text. Snapping *during* motion is worse than not snapping — it stair-steps — so it happens
exactly at the transition to settled, where the residual is below the settling threshold by
construction and the snap is therefore sub-pixel and invisible.

The snap cannot live in the model, because one model position on a 1× and a 1.5× output has to
produce two different snapped positions. It also makes the settling threshold acquire an output:
geometric thresholds are in device pixels of the **finest** grid the node currently intersects,
since settling against the coarse one leaves the fine one crawling. Angular and scale channels
convert through the node's bounding radius. See [Animation.md](Animation.md#implementation-notes).

### Scale is an exact rational

`wp_fractional_scale_v1` speaks 120ths. Store the numerator and do size arithmetic in integers,
because gyro and the client must arrive independently at the same integer or there is a gap, an
overlap, or a protocol error:

```
1000 × 1.1        = 1100.0000000000001  → ceil → 1101
1000 × 132 / 120  = 1100                            exactly
```

1.25, 1.5, 1.75, and 2.0 are all exact in binary, so the ordinary settings ladder never shows this
and 110% at a large surface size does — on one output, intermittently. That failure profile is why
it is a type rather than a code comment.

### Resample once, and know when it is zero

The composite pass samples each client buffer directly, through the fully composed buffer-to-output
transform. There is no per-surface intermediate, and no logical-space intermediate that is then
scaled to the mode.

This wants a first-class classification of the composed transform, and the thing to get right is
that its three consumers are not asking quite the same question. Damage rectangle mapping and
sharpness ask *is this a no-op* — axis-aligned, unit-scale, and at an integer device offset. [Plane
promotion](#direct-scanout-is-conditional) asks *can this particular hardware plane express it*, and
a plane with a scaler and a color-space converter expresses a great deal more than a no-op.
Answering the second question with the first forecloses promotion on every fractionally scaled
output, which [decision 56](Decisions.md#56-clients-render-at-the-ceiling-and-gyro-downscales) makes
the common case rather than the exception.

So the classification returns a **set of independent facts** rather than a boolean, and each
consumer takes the conjunction it can afford: *is it axis-aligned, is it upright, is the scale
exactly one, does the translation land on the destination's integer grid?* That is one function with
three readers rather than three predicates that agree until they do not.

*(Corrected 2026-08-16, against the implementation.)* This paragraph previously described a **rung**
— identity, integer translation, ninety-degree rotation and flip, positive scale, general affine —
with each consumer testing against the rung it could afford. A total order over five rungs cannot
carry four independent facts, and it fails twice. A quarter turn at unit scale on the grid is
resample-free, since it permutes texels and filters nothing, and yet it sits *below* scale on that
ladder — so a sharpness reader testing "identity or integer translation" rejects a transform that is
exactly sharp. Worse, a sub-pixel translation has no rung at all: it is not identity, not integer
translation, not a rotation, not a scale. That is the case this section cares about most, because
removing it is precisely what [the settled snap](#quantization-belongs-to-the-output) is for.

It still argues for a restricted transform type representing rotation, flip, per-axis scale, and
translation exactly, widening to a general affine only where an animation demands it.

A scaling plane does not break this section's rule. It is still one resample, executed in fixed
function instead of in the composite pass — though *which* resample it is turns out to matter, and
that is [an open question](Open.md) rather than a settled one.

**Minification needs mip levels, and they are built in linear light.** [Color](#color)'s one rule
already names mipmapping among the weighted sums of light, and gyro already holds a linearised copy
of every surface from import, so the chain has a correct source — building it from the encoded
buffer would darken every level and compound down the chain.
[Perspective](#transforms-are-3d-the-scene-is-not) wants anisotropic sampling on top of that, free
where the hardware has it and absent on the [floor tier](#the-floor-tier), which has already dropped
effects by the time it matters.

The one sanctioned second resample is the [exit snapshot](Animation.md#exit-pixels), captured at
output device resolution and then scaled by the animation. It is transient, and the per-output atlas
means the capture happens at the right density on each output a retiring window straddles.

### Transforms are 3D; the scene is not

Nodes carry a full 3D affine with node-local perspective and no camera. Composition is strict tree
order with no depth buffer, so intersecting geometry does not render correctly. That is excluded,
and excluding it is what makes the rest affordable: the failure is order-dependent transparency, a
depth buffer does not fix it, and every surface here has alpha with a backdrop reading through it.

**Group opacity is the second thing strict tree order gives up, and it is paid for rather than
excluded.** Applying a group's alpha per node is not a group fade — an occluded window shows through
the one in front of it and the backdrop is weighted twice, by `g(1−g)·(b − D)` in the overlap, which
peaks at the midpoint of every fade. So a node may declare itself an **opacity group**, and its
subtree renders to an offscreen at its screen-space bound and is composited once. The bound is the
area [admission control](#admission-control) already keys on and the storage follows the [exit
atlas](Animation.md#where-snapshots-live)'s reservation discipline, so nothing new appears on the
frame path. `Material::Glass` inside a group samples the composite as of the group's base, which is
the backdrop it would have sampled unflattened. See
[decision 60](Decisions.md#60-group-opacity-requires-flattening-per-node-alpha-is-not-a-group-fade).

Nothing else in the pipeline is destroyed by a transform. Damage becomes the axis-aligned bound of a
projected quad. Scanout eligibility is the predicate above returning false. Hit-testing becomes
ray-versus-plane and is barely exercised, since it reads model and settled model transforms are
axis-aligned. A blur backdrop is the screen-space bound at a screen-space radius, which still has an
area for [admission control](#admission-control) to key on.

**There is no camera**, because a per-output frustum would project a straddling window differently
on each output and change its shape across the seam. Perspective is a property of a node's own
transform, applied in its parent's space.

Two guards. **Perspective is held as a strength relative to the node's own extent, not as a distance
in local units**, which is what makes "the near plane never crosses the quad" an invariant rather
than a hope: scale is an animatable channel, so a node that grows while a stored distance stayed put
would walk its own far corner through the near plane mid-transition — the frame that divides by zero
and turns the damage bound into a NaN. Expressed in bounding radii the guarantee holds at every
scale, and the strength is additionally the only form [the motion
catalog](Animation.md#the-motion-catalog) can author, since a distance in pixels reads as different
amounts of perspective on a thumbnail and on a full window. *(Corrected 2026-08-16: this previously
read "the perspective distance is clamped", which a transform cannot do, because it does not know
the quad.)*

**Back faces cull, always** — not by default, since the only thing an override would buy is the
double-sided quad the next sentence refuses. A card flip is two nodes and a catalog transition. If a
node ever needs to be visible from behind, that belongs to its material rather than to its geometry.

### What clients are asked for

**`wp_viewporter` and `wp_fractional_scale_v1` are the main path.** `wl_surface.set_buffer_scale` is
correct and second-class; it cannot be required, so legacy is deprecated by being visibly worse
rather than by being refused. The structural gain is that the buffer-to-surface adapter is
*declared* — `src` in `wl_fixed`, `dst` in integers, an exact rational — so no logical size is ever
derived from buffer dimensions and a float.

**That rational is per axis, and the protocol means it.** `src` and `dst` may disagree in aspect
ratio, which is how anamorphic video reaches a compositor at all, so the buffer-to-surface adapter
carries two scale factors rather than one. The restricted transform stays exactly closed under that
widening — a quarter turn merely exchanges the two factors — which is what lets one type serve this
adapter, the output adapter, and the panel adapter instead of the surface adapter needing an algebra
of its own that would have to agree with theirs. *(Corrected 2026-08-16; this sentence and the table
above both said "an exact rational" as though it were one number.)*

**Preferred scale is the maximum over the outputs a surface intersects**, and clients that speak
only integer scale get the ceiling of that. Minification degrades gracefully and magnification does
not, so the trade is accepted along with its bill: the client's memory and GPU time, and a sampling
cost here.

**Changes are asymmetric** — raised immediately on any overlap, lowered only after the manipulation
settles and a debounce elapses. The symmetric rule turns dragging a window along a monitor boundary
into a buffer reallocation storm inside the client. Same shape as [sticky quality
tiers](#quality-tiers) and as [client cadence](#client-cadence-on-multiple-outputs): a surface
follows the most demanding output it is on, in density as in rate.

X11 clients have no notion of scale, so Xwayland surfaces live at one scale and are resampled
everywhere else.

### Where the integers are

- **`xdg_toplevel.configure`** is integer logical and does not divide evenly at fractional scale.
  Layout is computed in device pixels for the output the window is on, the logical size is rounded
  for the wire, and **gyro absorbs the remainder into its own gap**. A one-pixel seam showing the
  background between two tiled windows is that remainder, placed wrongly. A configure is also a
  *request*: the client may answer with something else, and nothing may assume it did not.
- **Pointer position** is real-valued in global space, accumulated from libinput's own doubles and
  constrained in global space. It is rounded once into the cursor plane's device position and once
  into `wl_fixed` for delivery, and never round-tripped back through either. Quantized to logical
  pixels, a 3840-wide output at 1.5 would have 2560 addressable columns — a mouse that physically
  cannot reach a third of the display.
- **Damage and scissor rectangles** are the enclosing integer rectangle plus the resampling filter's
  support radius in output pixels. Without the kernel footprint, moving content leaves one-pixel
  trails that never show up in a screenshot.
- **`wl_subsurface.set_position`** is integer surface-local, so a subsurface cannot be
  device-aligned on a fractional output at all. That one belongs to the protocol and is not gyro's
  to fix; what gyro controls is whether it compounds it by rounding a second time, and
  [decision 68](Decisions.md#68-a-subsurface-snaps-like-any-other-settled-node) settles that it does
  not — a settled subsurface snaps to the device grid like any other node. What stays
  [open](Open.md) is narrower: whether clients round their interior the way that snap assumes.

## Effects and quality

Effects are where `C` comes from, so they are also where it is controlled. Plain composition has not
been a scheduling problem for a decade; blur is, and blur is the feature.

### Materials, not filter calls

A surface declares a **material** from a closed vocabulary — `Material::Glass` and
`Material::Smoke` — and gyro decides what that means this frame. The shell names no radius, no pass
count, no chain resolution, exactly as it names no spring parameters.

The set is named by what each material does to light rather than by the role it is put to, which is
what keeps `Sidebar` and `Titlebar` out of it: the moment the vocabulary knows what a sidebar is, the
arrangements that are not sidebars stop being sayable about, and that is
[decision 51](Decisions.md#51-the-shell-composes-gyro-animates)'s rule one field over from the node
kinds. `Glass` sits over content the user arranged, so it can be thin. `Smoke` sits over content gyro
did not choose — a film, a white page — so its opacity comes from a worst-case contrast floor rather
than from taste. See
[decision 103](Decisions.md#103-a-dressing-is-named-by-what-it-does-to-light-the-material-set-is-glass-and-smoke).

This is forced twice over. Structurally, only the compositor has the backdrop: a client cannot blur
what is behind its own window, because that content belongs to processes it cannot see. And for
cohesion, a parameterized effect call bakes quality into the call site, so degrading it later means
overriding what a caller explicitly asked for. macOS lands in the same place for the same reasons —
`NSVisualEffectMaterial` names a material and the render server inside `WindowServer` renders it.

### Quality tiers

A material renders at a tier. For blur the ladder is **chain internal resolution → pass count → do
not render the material**, in that order. The first rung is close to invisible, since the result is
about to be blurred anyway. Radius is not on the ladder; it is a design property, and moving it
changes what the system looks like rather than what that look costs.

A **startup capability probe** runs the real pass chain at two or three sizes and picks a tier,
which is then stable. Stability is the point: a quality level that drifts with load reads as cheap
even when no frame is missed. Measured cost — keyed on `(material, region area, pass count)`,
recorded from GPU timestamps rather than modelled — exists to catch what the probe cannot predict:
thermal throttling, a client saturating the GPU, an unusually expensive scene. Tier changes are
sticky and asymmetric, down quickly and up slowly, and never during an animation.

The cut point is computed at commit time, not per frame. Effects are recorded in declared priority
order against a running cost sum, and where that sum crosses the allocation is where optional work
stops.

Region area is the node's screen-space bound, which under a
[3D transform](#transforms-are-3d-the-scene-is-not) is the bound of the projected quad. Still an
area, which is what keeps the cost model intact when a material is on something in flight.

### The floor tier

The floor composite — no effects, base composite only — is a first-class render mode used by tests
and headless golden images, not an emergency fallback. A recovery path that has never run is broken
when it is needed.

Its cost `C_min` is a design target rather than a residue, because it bounds what the system can
absorb: an overrun is recoverable in one frame only if `t_done + C_min ≤ deadline`. A cheap floor
composite is what buys the promise in
[decision 35](Decisions.md#35-a-miss-costs-one-frame-bounded-by-the-floor-composite).

## Color

Blending, scaling, mipmapping, and blur are weighted sums of light, and are correct only in a space
proportional to light. That single rule fixes most of what follows; the rest of color management is
appearance matching, which is policy. Rationale and rejected alternatives in
[decisions 47 and 48](Decisions.md#color).

### The composite space

**Linear, Rec.2020 primaries, brightness-relative** — 1.0 is SDR reference white and HDR headroom
lives above it as a multiple that varies with display brightness. Every surface carries a color
state: primaries, transfer function, alpha mode, reference luminance. Untagged content is sRGB by
rule and never by inspection.

Brightness-relative is the consequential half. It makes SDR-beside-HDR definitional rather than a
policy question answered separately in each case, and it makes the brightness control change
available headroom rather than scale pixels.

### Precision, and why the blur chain is affordable

| Target | Format | Why |
| ------ | ------ | --- |
| Blur pass chain | `B10G11R11_UFLOAT_PACK32` | a backdrop is opaque, so no alpha is needed — 32 bpp, the same bandwidth as the `RGBA8` it replaces |
| Composite target | 16-bit float, one per output | carries alpha, so it pays the width |
| Scanout | output's own format | 8-bit sRGB or 10-bit PQ depending on the mode |

Linear light at 8 bits bands unacceptably in the shadows, which looks at first like a doubling of
bandwidth on the pass chain [Effects and quality](#effects-and-quality) identifies as the dominant
term in `C`. It is not, because the chain needs no alpha. Rec.2020 is what makes the packed float
usable — it has no sign bit, so it wants a primary set wide enough that ordinary content stays
non-negative.

### Premultiplied alpha is the sharp edge

`ARGB8888` is premultiplied and the client computed `S = encode(C)·α`, so a hardware sRGB sampler
returns `EOTF(C·α)` where the wanted value is `EOTF(C)·α` — an error factor of `α^1.2`, about 13% at
`α = 0.5`, on every soft edge in the system. Surfaces are un-premultiplied, linearised, and
re-premultiplied once at import on the [dispatch thread](#threads), never per sample.

Half-correct is worse than consistently wrong here: encoded-space blending is wrong but
self-consistent, while linearising without un-premultiplying is wrong by an amount that varies with
alpha.

### What it costs the ecosystem

Client-drawn CSD shadows were tuned against encoded-space blending, because that is what every
Wayland compositor does. Under linear blending they lose roughly half their depth on a light
background and flatten as well as lighten. This is a mistuning rather than a defect — macOS
composites in linear and its shadows look excellent, because they were tuned against it — and the
answer is server-side decorations, which is where the closed-vocabulary argument in
[Materials, not filter calls](#materials-not-filter-calls) already points.

### Direct scanout is conditional

A client buffer flipped straight to a plane bypasses the composite pass, so every transform the
composite would have applied must be expressible in the KMS color pipeline — per-plane degamma,
CTM, gamma, or the newer pipeline properties. Where it is not expressible, gyro composites instead.
Otherwise the picture changes at the moment a client is promoted to a plane, which is a visible
flash and a correctness bug wearing an optimization's clothes. This is a constraint on `IPresenter`
and on plane assignment, and it is why it appears in
[what to build before it is needed](#what-to-build-before-it-is-needed).

[Geometry](#resample-once-and-know-when-it-is-zero) asks the identical question about the *spatial*
transform and answers it with the same classification, which damage mapping and the sharpness path
also consume. Promotion is admissible only where both halves say yes.

**The spatial half is a predicate plus a per-plane intersection, and not a predicate alone.**
*(Clarified 2026-08-16.)* What the transform can answer on its own is that it is axis-aligned and
that the destination offset is integral — a layer at a half-pixel offset cannot be handed to a plane
without moving it, and moving it is the visible flash this section refuses. Everything remaining
needs what the transform does not have: whether *this* plane has a rotation property, whether it has
a scaler, and the source extent, since `CRTC_W`/`CRTC_H` are integers and a 1.1× scale of a
101-pixel source has no whole destination. So the classification hands the plane assigner the
separate facts — upright, unit scale — and the assigner intersects them with the plane's own
capabilities. Note what is deliberately *not* required: a fractional scale is promotable, because
[decision 56](Decisions.md#56-clients-render-at-the-ceiling-and-gyro-downscales) makes minification
the common case, and answering this with the sharpness reading would foreclose promotion on every
fractionally scaled output.

**Promotion is a partition, not a fullscreen special case.** The arrangement worth having is not one
client filling the screen — it is several layers on several planes with the GPU never waking at all,
which is what SurfaceFlinger has done on mobile hardware for a decade. Adopting that framing now
costs nothing and is what the layer-list `IPresenter` in
[what to build](#what-to-build-before-it-is-needed) exists for; adopting it later means widening the
presentation interface after three backends implement it.

**But the hardware blends where gyro does not.** [The composite space](#the-composite-space) is
linear light at wide primaries, and
[decision 48](Decisions.md#48-linear-blending-is-a-visible-ecosystem-change-and-gyro-takes-it) takes
the visible ecosystem cost of that deliberately. A display controller's mixer blends in its own
space, which is neither linear nor gyro's, so two overlapping translucent layers on two planes do
not produce the composite's result. The rule that falls out is narrow and worth stating plainly:
**layers are promoted only where they are opaque and do not overlap.** Anything else is the defect
this section opens with, arriving a second time in the blend domain rather than the transfer domain,
and it is refused for the same reason — the only promotion worth having is one nobody can see.

That is less limiting than it sounds. Opaque and non-overlapping describes a fullscreen application,
the cursor, and an opaque panel, which is the arrangement a tablet spends most of its life in and
the one where the power saved is worth the most.

**And the plane count is small.** The mental image of a dozen overlays is a phone SoC's, not a
general one. An sc7180 tablet exposes four source pipes, of which exactly one scales or converts
YUV; the rest are flat RGB, and one of those is the cursor. So the reachable win there is a
fullscreen video or application on the scaling pipe with the GPU powered down, and not a partition
worth searching for. gyro therefore builds the *seams* for a partition and a plane assigner that
proposes the obvious arrangement and accepts refusal — never a combinatorial search over plane
assignments, which is machinery sized for hardware gyro is unlikely to meet and which would spend
deadline time to find it.

Collected, because it is otherwise four conditions spread over five paragraphs, a layer is promoted
only where **the KMS pipeline expresses its color transform**, **the plane expresses its spatial
rung**, **it is opaque and overlaps nothing**, and **its client can spare the held buffer**. The
last of those is in [what to build](#what-to-build-before-it-is-needed) with the reason; the
sharpness question the second one raises where the rung is a scale is [open](Open.md).

### Deferred

Tone mapping, gamut mapping, per-output characterisation and ICC profiles, and the color-management
protocol itself are all additive on the structure above and are carried in decision 47's open items.
The structure is not additive, which is the whole reason it is here this early.

## Event loop

`io_uring` with `IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN`, via liburing. There are
two loops on two [threads](#threads), and `SINGLE_ISSUER` — one submitting thread per ring — means a
ring each.

### The frame loop

**Timer-first, not event-first**, and it now has no client traffic to fit around: the only fds on
this ring are timers and KMS.

**`Frame` is one iteration of it, and the `while` is the composition root's.** The step below
returns the [`Wake`](#doing-nothing-must-cost-nothing) the next one is owed at; a shim in
`Compositor` arms the timer and does the waiting, because
[the root already constructs the threads](Structure.md#orchestration) and because a portable module
cannot call `io_uring_enter`. The shim decides nothing — its whole contract is *wake at or after
this instant, or earlier when a registered descriptor is readable*, and it is permitted to wake
spuriously. See [decision 80](Decisions.md#80-the-frame-loop-is-a-step-the-composition-root-owns-the-wait).

```
	// the shim: arm an absolute IORING_OP_TIMEOUT for the returned Wake, wait, call Step

	drain every event source                     // whatever woke us; see below

	acquire the newest published client state    // wait-free, once per iteration

	due = outputs owing a frame, earliest deadline first

	for each output in due:                    // earliest deadline first
		target = the frame after the last one committed, never the last one presented
		if previous frame still executing or now + C_planned > deadline(target):
			fall to the floor tier, or skip this output entirely   // see below
		evaluate animations at Clock(output).PresentationAt(target)
		record and submit per the admitted plan
		Presenter(output).Present(...)

	for each output the admitted schedule places early here:
		evaluate animations at Clock(output).PresentationAt(sequence)
		record and submit; the commit waits for that frame's own window above

	publish the consumed snapshot sequence      // releases the dispatch thread to reclaim

	return Sooner over every output's Wake       // Wake::Never() arms nothing
```

**The drain is first, and it is first for correctness rather than tidiness.** A `Presented` is the
sole input to every deadline, so one that lands after `FrameClock` was read leaves the whole
iteration scheduled against a prediction one frame stale. Every source is drained on every iteration
and the loop never asks which was ready — putting that question in the shim would put the ordering
in the one part of this that the [schedulability sweep](#outputs-are-independent-periodic-tasks)
does not run.

Acquisition is once per iteration rather than once per output. Per-output acquisition would be
fresher and is expressible — the boundary is wait-free, so a second acquire costs nothing — but a
window straddling two outputs would then show two different client frames in the same iteration,
which is visible on precisely the configuration this section exists to serve. Staleness is bounded
by one iteration; the seam artefact is bounded by nothing.

The plan this executes is static, computed by admission control at configuration change. The loop
does not decide anything about timing relationships; it runs the schedule it was given.

**The target is the frame after the last one committed, and the distinction is not pedantic.** The
clock's anchor is the last frame *presented*, and between a submit and its vblank the two differ —
on a 144 + 60 configuration the loop visits the 60 Hz output two or three times inside that
interval, and a target derived from the anchor would name a frame already drawn and spend three
allocations on it. It is also the one number [early rendering](#speculative-early-rendering) moves:
a pipeline `k + 1` deep is `committed = anchor + k`, and nothing else in the check changes.

The one exception is that first `if`, which is the whole of gyro's runtime timing policy:

```
	now + C_planned ≤ deadline   →  render the planned tier
	now + C_min     ≤ deadline   →  render the floor tier
	otherwise                    →  skip this output's frame, target the next deadline
```

Completion of the previous frame is polled non-blockingly through its timeline semaphore, so this
costs two comparisons and no stall. The third case is not a failure path but the mechanism that
stops a cascade: submitting work that will also be late keeps the GPU busy and makes the next frame
late too. **This is why damage must accumulate per output since its last successful present, never
per frame** — a skipped frame would otherwise lose damage and corrupt the next one. See
[decision 35](Decisions.md#35-a-miss-costs-one-frame-bounded-by-the-floor-composite).

**Read it as a fixpoint and not as one pass.** *(Added 2026-08-22.)* The third line names a new
deadline, and the check runs again against that one, so frames are dropped until the target is a
frame the work fits before — which is decision 35's `⌈overrun / P⌉` and not an extra rung beside it.
Applying the three lines once instead leaves an output that falls a whole period behind unable to
recover at all, because the deadline is derived from the frame clock's anchor and only a flip
advances the anchor: render nothing, present nothing, flip nothing, and be judged against the same
anchor forever. Every output is in that state coming out of idle.

Note that the evaluation step is inside the per-output loop. That is the whole of
[Presentation timing](#presentation-timing) expressed in one line of pseudocode: there is no global
predicted presentation time, because there is no global clock.

### The dispatch loop

Its own ring, its own thread, and no deadline of its own:

```
	wait for completions            // client sockets, evdev, the control socket

	drain input first, always
	for each ready connection, round-robin:
		demarshal under a per-client message budget
		import buffers, map shm, build pending state

	publish completed commits
	reclaim snapshots the frame thread has released
```

Input is drained ahead of client traffic because it is the only thing here whose latency the user
feels directly — and it is drained before `wl_event_loop_dispatch` is called at all, so the ordering
is a property of this loop rather than of the codec beneath it. The per-client budget is what stops
one chatty connection delaying twenty others. A message-granular budget is not available under
[decision 2](Decisions.md#2-gyro-owns-the-protocol-seam-libwayland-implements-the-server-codec)'s
server codec, so the lever is the coarser one: unsubscribe a client's socket and let kernel
buffering push back.

### Why io_uring

**Determinism, not throughput.** Twenty clients on a frame is perhaps forty syscalls collapsing to
one `io_uring_enter` — on the order of tens of microseconds against a 16.6 ms budget, and frames are
dominated by GPU work regardless. What earns its place is `DEFER_TASKRUN`: kernel completion work
runs where we ask for it rather than preempting mid-render, and batching reduces the number of
preemption points per frame. Jitter, not bandwidth.

That argument is now almost entirely about the *frame* ring, whose completions are timers and flips.
On the dispatch ring io_uring is a convenience and plain epoll would be defensible.

Three hazards that follow directly:

- **No lock may span the boundary.** The frame thread outranks the dispatch thread, so any mutex
  they share is a priority inversion — the [publication boundary](#the-publication-boundary) is
  wait-free on the reader for this reason and is the only permitted channel. This is the one hazard
  here that cannot be diagnosed from a profile after the fact.
- **Blocking operations poison the frame ring.** With `DEFER_TASKRUN`, pollable fds complete inline;
  anything that cannot gets punted to an io-wq worker. Regular file I/O is the obvious one, which
  means **spdlog must be on an async sink with a ring buffer** and must never write from the frame
  thread. Cheap to get right now, miserable to diagnose later as unexplained jitter. The dispatch
  thread is under no such constraint — blocking there is what it is for.
- **Kernel floor, expressed as a capability rather than a version.** The frame ring needs
  `SINGLE_ISSUER` and `DEFER_TASKRUN`, and `DEFER_TASKRUN` is rejected without `SINGLE_ISSUER`, so
  the requirement is one ring configuration rather than a list of flags. A kernel version is the
  wrong thing to test: distributions backport flags into kernels that predate them, and
  `io_uring_disabled` — 1 for privileged-only, 2 for off entirely — subtracts them from kernels that
  have them. So gyro attempts the configuration at startup and reports which flag failed. Multishot
  `recvmsg` was on this list until [decision 2](Decisions.md#2-gyro-owns-the-protocol-seam-libwayland-implements-the-server-codec)
  reversed; libwayland does its own buffered `recvmsg` and `sendmsg`, so gyro never issues one.
- **`DEFER_TASKRUN` defers only if the ring is reaped through `io_uring_enter`** with
  `IORING_ENTER_GETEVENTS`. Peeking the completion tail directly does not fail — it silently stops
  deferring, which withdraws the entire property the flag is being bought for, invisibly and
  permanently. A constraint on the frame loop's reap site rather than on the kernel.

## Sessions and users

One compositor per machine means the isolation a per-session compositor gets for free from the
kernel — separate processes, separate uids, separate sockets — is gyro's own job. This section is
what replaces it.

The model is **many sessions connected, one presented locally**. A session's clients stay alive and
warm while another session is on screen; switching users is not a teardown. Multi-seat — two people
on two monitors with two keyboards simultaneously — is out of scope, though seats are plural in the
interfaces regardless because multi-output demands it.

The precedent is macOS. `WindowServer` runs as an unprivileged uid, one instance per machine,
serving every fast-user-switched session, with the login and lock UI in a separate process that is
an ordinary client. Clients reach it by resolving a Mach service name through launchd's bootstrap
namespace, where **the namespace is per-session** — the same name resolves differently depending on
which session the looking process is in — and every message carries a kernel-supplied audit token
naming the sending uid and audit session. The part that transfers is that identity is *established
by the kernel rather than asserted by the client*; the audit session half does not, because
[a session here is a user](#connection-identity). The design below is that shape with the filesystem
in place of bootstrap.

### A session is not a display

The decomposition that makes fast user switching worth building:

- A **session** owns clients, a seat, a lock state, and a focus stack. It is a user, one to one.
- An **output** is assigned to at most one session at a time.
- **Presentation** is the mapping between them, and it is not required to be onto.

A session with no output assigned is not suspended. It renders if something is consuming its frames.

This is what makes the laptop case work: locking the lid-side panel is an *output reassignment*, not
a session state change. The panel goes to the greeter's session; the locked session keeps its remote
output and keeps running. Lock state and output assignment are independent, and remote access to a
locally-locked machine falls out of the model instead of being special-cased into it.

### Virtual outputs, and why remote desktop is one

A `System`-tier client registers an output — size, format, and a ring of dmabufs it owns — and gyro
treats it as an output like any other: its own frame clock, its own place in the scene graph, its
own damage, its own row in the admission set. gyro renders into the client's buffers and knows
nothing about what happens next.

This is the difference between remote desktop working and remote desktop being tolerated. The usual
stack composites for a display nobody is looking at, copies the result out after the fact, and
delivers it at a cadence unrelated to how it was produced — damage information already discarded,
input arriving through a side channel with no relationship to the frame clock. Here the session
renders *for* that consumer at its cadence and its resolution, with damage intact, zero-copy into
memory the consumer already owns. Decision 1 buys it: because the renderer never sees a
`VkSwapchainKHR`, a virtual output is another presenter rather than a fork.

**gyro contains no encoder and no network stack.** Putting them here would repeat, in a worse form,
the mistake [locking](#locking) refuses — a network stack is a *remote* attack surface on the one
process whose death takes every session on the machine, where the local wire protocol at least
requires an account on it first. So remote desktop is a client, a screen recorder wanting per-output
pre-composite frames is a client, and a tablet used as a second display is a client. The word
"remote" does not appear in gyro.

What the shape obliges:

- **Targets are imported, and dmabuf only.** The rule that the backend owns the images still holds —
  an encoder has format and modifier constraints the renderer cannot know — but the
  constraint-holder is now a client, so gyro renders into foreign memory. `wl_shm` is refused: a
  dmabuf's size is fixed at allocation, so the truncate-and-fault hazard that forces a SIGBUS
  handler for shm has no equivalent here, and allowing shm would import it.
- **A virtual output's frames are dropped, never deferred.** It sits in the admission set because it
  consumes real GPU time and blocks local outputs, but its period comes from flow control and a
  stall would otherwise make it unbounded. It must never be able to make a local output miss.
- **Its allocation is negotiated.** A client asking for 4K at 120 Hz is answered with what
  [admission control](#admission-control) says it can afford. This is the only place a client
  influences the admission set at all.
- **Injected input carries timestamps** and enters the same pipeline as local input, so `t₀` is the
  event time rather than the handling time — which is the one claim about input the out-of-process
  split still supports, and it is the one that matters.

> **Consequence.** Concurrent presentation targets means concurrent frame clocks — a 120 Hz panel
> and a 60 Hz network consumer, on one `SCHED_FIFO` frame thread. Multi-output already implied this;
> virtual outputs make it unavoidable, because their clocks are flow control rather than vblank and
> `IsPrecise()` is false by construction. The scheduler's handling of multiple clocks is unresolved,
> and it is the first thing the headless backend's fake clock should be pointed at.

### Connection identity

**A session is a user.** A user has at most one session, so the uid names it completely — gyro mints
no session identifier, the session agent asserts none, and there is no name anywhere in the system
that a user could choose, forge, or collide with. See
[decision 44](Decisions.md#44-a-session-is-a-user-identity-is-the-uid).

One listening socket per user, at `$XDG_RUNTIME_DIR/wayland-0`, with `WAYLAND_DISPLAY` set in the
session's environment and inherited by everything the session leader spawns. Keeping the socket
inside the runtime directory is what lets unmodified clients and flatpak find it with no
configuration.

**The listener is the credential.** A connection's session is known before its first byte, with no
per-message checks and no runtime cost on the dispatch path. `SO_PEERCRED` is verified at accept as
well — one syscall per connection, and under [listener handover](#listener-handover) it is what
prevents a user from laundering another user's client into their session.

The case that would have demanded more than a uid is the same user holding two simultaneous
independent desktops, and it has no occupant. Logging in over SSH to launch something onto the
physical screen means joining the running session, not forking a second one. Wanting the desktop on
a tablet at a different resolution while the desk monitor shows something else means two *outputs*,
which [virtual outputs](#virtual-outputs-and-why-remote-desktop-is-one) already deliver and which
[a session is not a display](#a-session-is-not-a-display) already permits. And a boundary between
two same-uid sessions would be one the kernel does not draw — either could `ptrace` the other's
clients — so gyro could not enforce it even if it wanted to.

This is a generalization of `wp_security_context_v1`, which attaches metadata to a listening socket
and lets connections inherit it. Sandbox contexts therefore compose on top later rather than
conflicting.

### Privilege

gyro runs as a dedicated unprivileged uid, with exactly one capability:

| Need                        | Mechanism                                                   |
| --------------------------- | ----------------------------------------------------------- |
| `SCHED_FIFO`                | `LimitRTPRIO=` in the unit, not `CAP_SYS_NICE`               |
| `mlockall`                  | `LimitMEMLOCK=infinity`, not `CAP_IPC_LOCK`                  |
| `/dev/dri/*`, `/dev/input/*`| udev rules granting the gyro uid                             |
| DRM master                  | implicit on opening a device with no current master          |
| High-priority GPU queue     | `CAP_SYS_NICE` — the one capability, see below               |

DRM master deserves a note. `drmSetMaster()` requires `CAP_SYS_ADMIN`, which is effectively root and
would defeat the exercise — but it is not needed. Opening a DRM device that has no current master
confers master implicitly, and since gyro uses no VTs and never hands off, it takes master once at
boot and holds it for the life of the machine. The wrinkle is Plymouth, which holds master during
boot, so this is unit ordering. **Needing `CAP_SYS_ADMIN` should be read as an ordering bug, not as
a requirement.**

### GPU priority

Everything else in this document defends the frame budget on the CPU. The resource that actually
blocks a composite is the GPU queue, and there gyro is one submitter among all clients: they reach
the GPU through render nodes, which need neither DRM master nor gyro's cooperation. A client's 10 ms
batch delays gyro's composite by 10 ms no matter what priority gyro's threads run at.

Presentation is not where this leaks. gyro holds DRM master permanently, so every pixel reaches a
connector through gyro's atomic commit — there is no exclusive-fullscreen bypass on Linux, and
direct scanout of a client buffer is still gyro flipping it. `wp_drm_lease_v1` is the sole exception
and gyro decides whether to grant one. Direct scanout in fact sharpens the problem: on the frame
where a fullscreen game is scanned out directly, gyro's own GPU work for that output is near zero
while the *other* output's composite queues behind that game's batch.

So gyro requests `VK_QUEUE_GLOBAL_PRIORITY_HIGH` on its queues, which requires `CAP_SYS_NICE` on
i915 and xe and is permitted to a DRM master on amdgpu. `REALTIME` is not requested. The priority is
queried with `VK_EXT_global_priority_query` and `VK_ERROR_NOT_PERMITTED_KHR` is a warning rather
than a fatal error; nested and headless never request it at all. High priority reduces blocking and
does not eliminate it — preemption granularity is hardware-dependent — so `C` is still measured and
the frame-drop backstop still exists. See
[decision 22](Decisions.md#22-gyro-runs-as-a-dedicated-unprivileged-uid-with-cap_sys_nice-and-nothing-else).

Socket creation is the one thing gyro cannot do unaided — it cannot `chown` a socket to a user
without `CAP_CHOWN` — so the handover is inverted. See below.

### Listener handover

A small session agent, running as the user, creates the listening socket in its own runtime
directory and passes the fd to gyro over a world-writable control socket. gyro verifies the offering
process with `SO_PEERCRED` and binds the listener to that uid's session, creating it if this is the
first offer.

The capability saving is secondary. What earns this shape is that **the handover is the
session-start event.** gyro learns a session exists because a listener arrived, not because it
subscribed to a signal, and the helper's control connection stays open for the session's lifetime so
its EOF is the session-end event. Together with permanent DRM master and no VTs, this is why gyro
likely needs no D-Bus client at all — the residual dependency on logind is `pam_systemd` creating
`XDG_RUNTIME_DIR`, which is a deployment dependency rather than a runtime coupling.

What the shape obliges:

- **The per-connection `SO_PEERCRED` check is load-bearing, not defence in depth.** Because the user
  creates the socket, a user can create a permissive one; without a per-connection check, another
  user connecting through it would be attributed to the offering user's session, landing inside
  their clipboard and their surfaces. **gyro rejects any connection whose peer uid does not match
  the session's user.** This check is redundant only under the design that was not chosen, and it
  must not be optimized off the accept path.
- **The control socket is an unauthenticated entry point** into the one process whose death takes
  every session on the machine. Bounded by `SO_PEERCRED` on the offer, validation that the offered
  fd is a listening `AF_UNIX` stream socket bound inside the offering uid's runtime directory, a
  per-uid offer cap, and one accepted listener per session.
- **The handshake is a stable ABI.** It must survive gyro restarting — every listener is lost, so
  helpers re-offer — and version skew across upgrades.
- **`WAYLAND_DISPLAY` must be in the session environment before the first client starts.** Ordering
  mistakes here present as "sometimes applications cannot find the display".
- **The greeter has no user session**, so the privileged login agent performs the handover for the
  greeter's dedicated uid, making the login agent a hard dependency.
- **Nested and headless bind their own listener** with no helper, consistent with the existing rule
  that development backends are exempt from `SCHED_FIFO`, `mlockall`, and DRM master.

### The login agent

Structurally a display manager with the compositor removed: a PAM conversation over a socket, plus
`setuid`-and-spawn. It runs as root, because authentication, creating `XDG_RUNTIME_DIR`, and
starting processes as other users all require it — and because that privilege is exactly what gyro
refuses to hold.

```
systemd (pid 1)
  └─ gyro.service                 uid gyro, SCHED_FIFO, CAP_SYS_NICE
     · DRM master by first-open, input via udev rules
     · binds /run/gyro/control          the offer socket
  └─ gyro-login.service           uid root
     · fork → setuid(greeter) → session agent → binds a listener, offers it
     · gyro assigns the local output to the greeter session

   greeter ──credentials──▶ login agent          never through gyro
                             · PAM conversation in a forked child
                             · on success, session stack as the user
                                 → pam_systemd: XDG_RUNTIME_DIR, systemd --user
                             · fork → setuid(user) → session agent
                                   · bind, offer, export WAYLAND_DISPLAY
                                   · start graphical-session.target
                                   · idle, holding the control connection
   gyro shows the user's background, waits for their shell to present,
   then reassigns the output: greeter ──cross-fade──▶ user session
```

The wait is [required](#an-output-waits-for-its-sessions-shell), not an optimization: reassigning as
soon as the listener arrives puts a blank screen between the greeter and the shell's first frame.

**gyro's independence from logind is gyro's alone.** The login agent runs an ordinary PAM stack, and
`pam_systemd` is in it — which is how `XDG_RUNTIME_DIR`, `systemd --user`, and therefore portals
and pipewire come to exist at all. The coupling is real; it is simply not on gyro's side of the
boundary.

**The agent speaks the greetd protocol** — a u32 length prefix and JSON over a socket named by
`GREETD_SOCK`, with `auth_message` typed as `visible` / `secret` / `info` / `error`. A protocol we
speak rather than a library we link, so it adds no dependency, and every existing greetd greeter
works against gyro unmodified. That is what lets the whole boot chain be brought up before any
greeter UI is written. The greetd *daemon* is not used: it is a single-session state machine that
terminates the greeter to start a session, and it is VT-centric.

The agent is not a one-shot fd donor. Two jobs keep it alive for the session's lifetime — which is
also what makes "control-connection EOF means the session ended" true by construction rather than by
hope:

- **It performs the listener handover** described above.
- **It forks Xwayland**, because Xwayland must run as the user and gyro cannot `setuid`. gyro owns
  the X sockets — `/tmp/.X11-unix` is world-writable, unlike the user's runtime directory — so gyro
  listens, detects the first X client, allocates a display number from a machine-global namespace
  that only a machine-global compositor can properly arbitrate, and asks the agent to do the fork.

### Filtered globals

`wl_registry` advertisement is a function of the connection, not a static table. This is the
structural commitment — a small amount of code before the server half exists, and a rewrite of every
global's bind path afterwards. The tiers it enforces are policy and can move:

- **Shared** — `wl_compositor`, `wl_shm`, dmabuf, `xdg_wm_base`, viewporter, fractional-scale,
  presentation-time, `wp_fifo_v1`, `wp_commit_timing_v1`.
- **Session-scoped** — data device and primary selection, `xdg_activation`, `xdg_foreign`,
  text-input and input-method, idle-inhibit.
- **System tier** — screencopy and screencast, [virtual output
  registration](#virtual-outputs-and-why-remote-desktop-is-one), foreign-toplevel management,
  layer-shell, output configuration, the lock protocol below, and [the shell's](#the-shell) own
  scene, policy, and background protocols.

The last two in the shared tier are there for a reason worth recording, because it is not the usual
one. `wp_fifo_v1` and `wp_commit_timing_v1` are not advertised to give clients a throttling
primitive; they are advertised because what a client says through them is what lets gyro keep
scheduling authority over a variable-refresh output and keep its promises to every other output
attached — see
[decision 76](Decisions.md#76-cadence-authority-follows-predictability-not-foreground). Neither is a
prerequisite: a client that states nothing is read pessimistically and gyro still works. They are the
path by which a well-behaved client costs its neighbours nothing.

Connections carry a trust level of `User` or `System`. The enum is what is expensive to add later;
its membership is not.

Trust is a property of the *listener*, since that is where connection identity comes from — so a
`System` connection cannot arrive on the ordinary per-user socket, and the shell needs a second
listener with different permissions. Who creates it and how a process is judged worthy of it is
[open](Open.md), and the shell is what makes it urgent rather than theoretical.

### Locking

**The compositor knows whether a session is locked. It does not know how to draw a lock screen.**

gyro owns the lock *state* and everything that state implies — which surfaces may be presented,
which client receives input, what happens on a hotplug or a new output arriving while locked, and
the fact that a session stays locked rather than falling open when the client drawing the lock dies.
It owns none of the pixels.

The split is load-bearing in both directions. Drawing the lock in-process would drag text shaping
and UI layout into a `SCHED_FIFO` process whose thesis is that it does one thing perfectly. Leaving
lock state to client policy would mean a client crash exposes the desktop, which is the failure mode
this arrangement exists to make impossible.

This is also what keeps authentication out of gyro entirely. PAM runs in the privileged login agent
that owns session creation; the greeter is its face, and gyro never sees a credential.

**Locking is an output reassignment, and the greeter is the lock screen.** There is no separate lock
client and no lock surface belonging to the locked session — the output moves to the greeter's
session, which exists from boot and is therefore always ready. That follows
[A session is not a display](#a-session-is-not-a-display) rather than adding to it, and it buys a
property no other arrangement has: **spoofing becomes structural rather than a policy.** Under
`ext-session-lock-v1`, the lock surface belongs to the user's own session, so "which client may be
the lock screen" is a rule gyro must enforce correctly forever. Here, a locked output has been
assigned elsewhere and the user's session is not composited at all — a malicious client cannot draw
a false password prompt because it cannot draw.

The cost is that the locked screen cannot show user-owned content: notifications, media controls,
and widgets are all the same problem, since they live at the user's uid and the greeter is not it.

Wallpaper is the exception, and the route it takes is the one any further exception has to take.
[gyro owns the background](#the-background) and holds a persisted copy, so a locked output shows the
wallpaper of the user who locked it with gyro compositing the image and the greeter never receiving
it. The isolation is not relaxed to allow this; it works because gyro, rather than the greeter, is
already the party holding the pixels.

The designed answer for the rest, deliberately not built initially, is a **lock-screen content
surface** — one non-interactive surface a locked session may present, composited below the greeter's
UI on the output it was locked out of. Non-interactivity is the safety property: keystrokes meant
for the password field can never reach a client of the locked session, which also puts notification
*actions* out of scope. It is safe to defer because unification does not foreclose it.

One requirement falls out. The greeter is now on the critical path for getting back into the machine
and one greeter serves every session, so its crash locks out everybody. The login agent must respawn
it, and gyro's lock state must be entirely independent of the greeter's liveness.

**The greeter is not on the path to being locked, and a locked output is never blank.** Because
[gyro owns the background](#the-background) and holds a persisted copy per uid, the locked state
gyro composites — the wallpaper of the user who locked the output — is available with no client
running at all. So locking is immediate, the greeter's UI arrives over that background whenever it
is ready, and a greeter crash mid-lock costs the password prompt rather than the picture. The
[session-ready gate](#an-output-waits-for-its-sessions-shell) deliberately does **not** apply here:
waiting for the greeter to present would hold the *outgoing* session's pixels on a screen that is
supposed to be locked, which is the same failure
[the suspend handshake](#suspend-and-resume) exists to prevent, at a seam that recurs many times a
day rather than once. Lock is the one output reassignment that may not wait.

### Activity, input, and frame callbacks

A session is **presented** if it has at least one live presentation target, local or remote.

- Inactive sessions receive no `wl_seat` events. Not events they are expected to ignore — no
  capability, no focus, no keys. Getting this wrong means one user typing a password into another
  user's terminal.
- Inactive sessions receive no `wl_surface.frame` callbacks. This is the only throttle Wayland
  offers, and without it every logged-in user's clients render at full rate into surfaces nobody is
  looking at. A per-session compositor gets this free because the whole process is descheduled; gyro
  does not. What pays is the [GPU queue](#gpu-priority) gyro shares with them and the [dispatch
  thread](#threads) draining their commits — not the frame thread, which never sees the traffic at
  all. The throttle still matters, because the GPU contention it prevents is the one interference
  source gyro's scheduling cannot defend against.

It also gives the scheduler a clean answer to "is anything still animating": only presented
sessions count.

### Resource accounting

**Attribution always; policy sized to survive, not to ration.**

Every fd, shm mapping, and dmabuf import is attributed to the connection that caused it. That is a
counter increment, it is the first thing wanted when diagnosing where memory went, and it is the
part that cannot be retrofitted through a wire implementation that assumed a single tenant.

Policy is deliberately thin. Per-user fairness budgets would be solving a problem a laptop does not
have, and every threshold becomes a ceiling some legitimate workload eventually hits. The reason any
limit exists is narrower: **gyro's restart boundary is ruinously expensive.** A session compositor
that dies loses one login and is restarted by its greeter. gyro dying takes every client belonging
to every user on the machine at once. So limits are backstops an order of magnitude above anything
real — total mapped shm bytes, total imported dmabuf bytes, per-connection object count, with
`LimitNOFILE` raised well past where it can bind — and the response is a protocol error killing the
offending client, which the protocol already sanctions, rather than throttling anyone.

**Restart is possible, and it is worth being exact about that**, because several arguments in this
document lean on the phrase above. gyro comes back, the display holds across it, and the listeners
return — see [Restart](#restart) for the contract and for what does not come back, which is every
client's state for everyone logged in. The boundary is ruinously expensive rather than absent, and
since it is a real recovery path it earns the same treatment as the others here: exercised rather
than assumed.

That is also what limits this section. Backstops are sized against compositor-selected resources
because a client-selected one behind a survival backstop is a client choosing when gyro dies, and
the restart it would trigger is expensive rather than free.

One thing that is not policy at all: a client that stops reading its socket. Unbounded send
buffering is a memory bug, so something must be decided regardless of how many users exist. That
belongs with backpressure in the protocol layer, not here.

`mlockall` is a related trap and is recorded under
[decision 22](Decisions.md#22-gyro-runs-as-a-dedicated-unprivileged-uid-with-cap_sys_nice-and-nothing-else):
locking the whole process pins every `wl_shm` pool gyro maps, whose size and count a client chooses,
which is a client-selected resource sitting behind a backstop meant for compositor-selected ones.
Residency is for the frame thread's working set, not for the process.

### Consequences for the protocol layer

The wire implementation now terminates untrusted input from every account on the machine. Under
[decision 2](Decisions.md#2-gyro-owns-the-protocol-seam-libwayland-implements-the-server-codec) that
implementation is libwayland's, so the fuzzing it warrants is upstream's rather than a line in gyro's
cost column — but the exposure is unchanged, because the code runs in gyro's address space beside DRM
master either way. What is gyro's is everything layered on top: attribution, filtered globals, and
the shadow object model's lifetime rules, none of which libwayland has an opinion about and all of
which are reached by the same untrusted input.

## The shell

gyro is a system layer, so it does not contain a desktop. Window-management policy and every piece
of shell chrome run as clients at the user's own uid; gyro owns the mechanism underneath them.
Rationale and rejected alternatives in
[decision 51](Decisions.md#51-the-shell-is-a-per-session-client-gyro-owns-mechanism).

| | gyro | The shell |
| --- | --- | --- |
| **Composition** | z-order, the entity graph, transitions, materials, the background | which entities exist and how they are arranged |
| **Input** | routing, hit-testing, grabs, cursor | the focus *model*, declared in advance |
| **Manipulation** | drag, resize, swipe, at device rate | the constraints those run under |
| **Cross-session** | outputs, session assignment, lock state | nothing — it is inside one session |

The last row is not a boundary of convenience. Session switching, locking, and output assignment
span sessions by definition, so no per-session client can own them; the shell operates on a subtree
of a scene whose top belongs to gyro.

A shell reaches all of this through the `System` tier of [filtered globals](#filtered-globals), and
trust is a property of the listener rather than of the connection — so being the shell is something
a process is granted at the socket it connects to, not something it claims. Which listener that is,
and who decides who may use it, is [open](Open.md).

### Declare, do not drive

**The shell is never in a per-event loop.** It is the same rule
[materials](#materials-not-filter-calls) and [the motion catalog](Animation.md#the-motion-catalog)
already follow — name a material and gyro decides what it costs, name a transition and gyro owns the
springs — applied to interaction. A window drag is not motion events forwarded to a client and
positions sent back; it is a declaration that an entity tracks the pointer until release, executed
by gyro at input rate with no process in the loop. Tracking is
[driven](Animation.md#interactive-transitions) rather than sprung, and no spring is interposed
between the pointer and the pixels — which is what `Motion::Interactive` would have meant here, and
is what [decision
65](Decisions.md#65-interactive-transitions-are-driven-by-a-progress-parameter-not-by-a-moving-target)
rejects.

The line between what may round trip and what may not is the *shape* of the interaction rather than
the subsystem it belongs to:

- **Continuous manipulation is gyro's.** Resize is already a round trip through the client, which is
  why it rubber-bands everywhere; adding a second one through the shell would make gyro worse than
  the field at the interaction users judge hardest.
- **Discrete state changes may round trip.** Maximize, tile, move-to-workspace, and placement of a
  new window all animate compositor-side while the client's pixels catch up, so the felt latency is
  when the animation starts, not when the client renders. A hop costs one frame against no
  reference.

Swipe is the case that tests the rule hardest, because a gesture scrubbing a transition looks like
it needs a per-event channel and turns out not to. The shell binds a gesture to a named transition
and its endpoints once; gyro owns the recognizer, the map from displacement to progress, and the
release, and reports only that the gesture began, committed, or was cancelled. See [interactive
transitions](Animation.md#interactive-transitions) and [decision
65](Decisions.md#65-interactive-transitions-are-driven-by-a-progress-parameter-not-by-a-moving-target).

Consistency matters more than speed here: a configure that takes 8 ms every time reads as a physical
response, and one varying between 1 and 20 ms reads as unreliability.

### One policy client, several chrome clients

Panel, launcher, overview, and notifications are separate clients from the policy client and from
each other, because a bug in a launcher's search should not take window management with it.

The reason that decomposition is safe is [`t₀`](Animation.md#timing-and-rates): a commit's origin is
the input event's timestamp rather than the moment of handling, so several processes reacting to one
event produce one gesture with one origin, and a commit that arrives a frame late renders *already
in progress* rather than starting late. Cohesion across processes costs nothing here and would be
impossible in a system that integrated its animations.

Login is the case that does not inherit an origin, since nothing about it is input-driven. That is
what the session-ready gate below is for.

### The scene vocabulary is closed; composition is not

The shell builds scenes from a closed set of node kinds — surface reference, snapshot reference,
solid, effect layer — into arbitrary trees, and moves them only with catalog transitions. **Cohesion
lives in the motion, not in the arrangement.** A shell may invent any idiom it likes and cannot
invent a spring, which is what lets two desktops on gyro look nothing alike and still feel like the
same machine. The test that keeps the line honest: if a shell can produce motion that does not match
the catalog, the vocabulary is wrong.

The closed set is also what makes a shell replaceable, because gyro can go on presenting windows
under default policy while one restarts. Anonymous nodes would leave gyro holding a scene it cannot
interpret, with nothing to do but freeze or drop it. That default policy is the same
[floor tier](#the-floor-tier) argument in a second place: the behaviour gyro falls back to is the
behaviour it uses to come up before any shell exists.

### The background

The background belongs to gyro. Three things follow that would otherwise each need answering
separately: [`Material::Glass`](#materials-not-filter-calls) always has a backdrop to sample, so the
effect path has no degenerate case; [the firmware handoff](#from-firmware-to-gyro) reaches a
gyro-owned image with no client in the path, so one continuous picture from BGRT onward does not
depend on anything having started; and both a shell restart and the gate below have something to
show that is not black.

The shell supplies it as a **buffer**, never a path, and gyro persists a copy so it is available
before any shell is running:

- **A raw dump plus a header**, not an encoded image — the whole point is that gyro contains no
  decoder. The header carries the surface's [color state](#color), stored as delivered so that
  loading it reuses the client-buffer import path rather than adding a second one.
- **Written by the [helper thread](#threads)**, debounced so a rotating-wallpaper slideshow does not
  write to disk every thirty seconds, and kept in gyro's own state directory keyed by uid.
- **Scaled and placed** when the mode it is shown at differs from the mode it was captured at, which
  is the computation [BGRT continuation](#from-firmware-to-gyro) already needs.
- **Never composited before that user has authenticated.** Showing a selected user's wallpaper on
  the login screen is tempting and would put user-controlled pixels where another user may be about
  to type a password. After authentication the question dissolves, since the wallpaper on a locked
  output belongs to the uid that supplied it.

That last rule is what lets [locking](#locking) show a user's own wallpaper at all: gyro composites
the image and the greeter never receives it, so the isolation is not weakened to get it.

Whether a background is one image per session or one per output is deliberately
[open](Open.md). The cache is written and read at moments when no per-output intent has
been expressed — before the shell exists, and at the greeter — so the live case and the cached case
may not want the same answer, and nothing here depends on which way it goes.

### An output waits for its session's shell

**An output is not reassigned to a session until that session's shell has presented.** Between
`graphical-session.target` and a shell's first frame is a second or more, and without the gate login
reads as greeter, blank, shell — three beats where the design promises one continuous image. With
it, and with the background above, it is greeter, then that user's background, then the shell
arriving over it.

## Wayland protocol layer

**gyro owns the seam and the shadow object model; `libwayland-server` implements the server codec
behind them; the client codec is gyro's own.** The split is not a compromise between two positions —
each half sits where the argument for it actually is. Rationale and the reading that settled it in
[decision 2](Decisions.md#2-gyro-owns-the-protocol-seam-libwayland-implements-the-server-codec).

**Why the shadow object model is gyro's regardless.** A `wl_resource` dies synchronously on the
dispatch thread when its client goes away, and [the frame thread](#threads) may be holding what it
described. So a `wl_resource` can never be the thing the frame thread holds, whatever codec produced
it, and the scene reads a snapshot rather than protocol objects. That is a seam by construction, and
a seam is what makes the codec underneath replaceable rather than foundational.

**Why the client codec is gyro's.** [Decision 1](Decisions.md#1-the-nested-backend-drives-raw-wayland-protocol-not-vulkan-wsi)
has the nested backend drive `zwp_linux_dmabuf_v1`, `presentation-time`, and
`wp_linux_drm_syncobj_v1` directly rather than going through Vulkan WSI, so the renderer never
sees a `VkSwapchainKHR`. That needs a Wayland client, it is the smaller half, and it runs against a
known-good peer — mutter first, strict and loud about malformed protocol, then a wlroots compositor,
since the two disagree in exactly the corners that are easy to get wrong.

**Why the server codec is not.** The case for writing it rested on libwayland resolving ordinary
failure by calling `wl_abort()` in a process whose death takes every client belonging to every user
on the machine. Read against 1.26, that is false: allocation failure never aborts anywhere in the
server library, there are no reachable assertions, and the two sites a client can drive both require
a gyro bug in front of them. What survives is a preference about dispatch granularity, an even trade
on SIGBUS, and one real argument — no `wl_list` / `wl_listener` object model at the boundary, where
destroy-listener lifetime bugs are the best-known failure family in compositors built this way. That
last one is satisfied by the shadow object model and the generated bindings, which are being built
either way.

**Bindings** are generated at build time by a host tool, with a hand-rolled parser for the XML
subset the protocols use. No scripting-language dependency, no third-party XML library, no generated
code in the tree. They serve the client codec directly and wrap libwayland on the server side, and
the wrapping is not only ergonomic: **a generated dispatch table cannot have a hole, and a generated
resource constructor cannot publish an id before an implementation is set.** Those are the two
`wl_abort` sites in `wl_closure_invoke` that a client can reach, closed by construction rather than
by remembering.

**What is still gyro's on the server side**, and it is the part that was always going to be the
real work:

- **Cross-thread object lifetime.** [Generational handles](Decisions.md#15-identity-is-a-generational-handle)
  supply the detection and [exit snapshots](Decisions.md#20-exit-animations-use-full-resolution-snapshots)
  already establish that an entity outlives its protocol object. The discipline is new and is not
  retrofittable — this is the cost
  [decision 45](Decisions.md#45-protocol-dispatch-is-a-thread-not-a-task) accepts.
- **Dispatch ordering and backpressure policy.** Input is drained before `wl_event_loop_dispatch` is
  called at all, so the ordering in [the dispatch loop](#the-dispatch-loop) is a property of that
  loop rather than of the codec. What libwayland does not offer is a *message-granular* per-client
  budget, so the lever is the coarse one: drop a client's event source and let the kernel socket
  buffer push back.
- **Resource attribution.** Every fd, shm mapping, and dmabuf import is attributed to the connection
  that caused it, per [resource accounting](#resource-accounting). libwayland tracks none of this,
  and it is the part that cannot be retrofitted through code that assumed a single tenant.
- **Filtered globals.** `wl_registry` advertisement as a function of the connection rather than a
  static table, which libwayland supports through `wl_global_create`'s filter but does not decide.

**What is now inherited rather than written.** The demarshaller and socket manager, id allocation
and `delete_id` semantics with the zombie window a client can send into, fd buffering against message
order and the 28-fd send limit, and send-buffer growth. Those were the listed hard parts and they are
upstream's — including the fuzzing, on code that is the most-exercised in the ecosystem.

**SIGBUS on `wl_shm` is inherited too, and the exposure is unchanged.** A client may truncate the fd
backing a pool while it is mapped, faulting us on access; libwayland guards compositor access with a
process-global signal handler and a `sigsetjmp` trampoline. [Threads](#threads) moves the fault onto
the dispatch thread, which downgrades it from a missed frame to a client error. The exposure is
confined to the upload of a *live* surface: shm content is copied into a compositor-owned image at
commit in order to be sampled at all, so nothing downstream — [exit
snapshots](Animation.md#exit-pixels) included — ever touches a client mapping. Importing the mapping
instead, via `VK_EXT_external_memory_host`, is refused for exactly this reason; it would trade a
SIGBUS someone can catch for a GPU fault nobody can.

**The swap stays available.** Nothing is built on libwayland that would have to be unbuilt, which is
the whole point of putting the seam first. Decision 2 records the three conditions that would reopen
the in-tree server half, and the abort inventory is a snapshot of one version rather than a property
of the library — so it wants re-reading on major bumps.

Ecosystem risk is lower than it appears in both directions: Xwayland is a *client* and links
libwayland-client, which is its own business, and the protocol extensions we want are XML only.

## Dependencies

The platform stack is consumed through pkg-config; CPM is for C++ libraries.

The distinction that matters is **not** meson versus CMake — wayland, libdrm, libinput, and
libxkbcommon are all meson or autotools, and that is entirely invisible because distributions
package them. The distinction is whether we ever have to *build* it. A meson project the distro does
not ship means `ExternalProject_Add` driving meson from CMake, which breaks the single-configure
flow. That is what disqualified basu.

| Dependency                     | Source     | For                                    |
| ------------------------------ | ---------- | -------------------------------------- |
| `wayland-protocols`            | pkg-config | XML only, build time                   |
| `wayland-server`               | pkg-config | server codec, behind gyro's seam       |
| `libxkbcommon`                 | pkg-config | keymaps, both directions               |
| `libdrm`                       | pkg-config | atomic KMS, dmabuf                     |
| `libinput`, `libudev`          | pkg-config | native input                           |
| `liburing`                     | pkg-config | event loop                             |
| spdlog                         | CPM        | logging — async sink only              |
| `Vulkan-Headers`               | CPM        | renderer — headers only, version pinned |
| volk                           | CPM        | Vulkan meta-loader; `dlopen`s the loader |
| shaderc / glslang              | CPM        | runtime shader compilation, hot reload — not taken yet |

Not used: `libwayland-client` (the client codec is gyro's, per
[decision 1](Decisions.md#1-the-nested-backend-drives-raw-wayland-protocol-not-vulkan-wsi)),
VulkanMemoryAllocator, libseat, basu, greetd (the [protocol is implemented](#the-login-agent), the
daemon is not used).

`wayland-server` is the one entry here that is a *replaceable* implementation rather than a
dependency the design leans on: it sits behind gyro's own seam and shadow object model, and
[decision 2](Decisions.md#2-gyro-owns-the-protocol-seam-libwayland-implements-the-server-codec)
records what would replace it.

Fedora setup:

```bash
sudo dnf install wayland-devel wayland-protocols-devel libdrm-devel libinput-devel libxkbcommon-devel liburing-devel
```

**Vulkan is deliberately absent from that line**, and its absence is the content of
[decision 103](Decisions.md#103-vulkan-arrives-through-cpm-and-gyro-never-links-the-loader): gyro
does not link the loader. volk resolves it with `dlopen("libvulkan.so.1")` at startup, so the headers
are the only build input and they arrive pinned through CPM. What that buys is not convenience — it
is that a machine with a broken or missing Vulkan install answers `volkInitialize()` with a failure
gyro can *report* and fall back from, where a linked loader would make it a refusal to `execve`. On a
boot service that has [subsumed the splash](#from-firmware-to-gyro) and removed the VTs, that
difference is a legible error against a black machine with no way in.

`Tools/VulkanProbe.cpp` is what answers "what does this machine actually have" — it links the headers
only, `dlopen`s the loader itself, and runs where there is no ICD at all. Run it before diagnosing a
renderer that will not come up.

`systemd-devel` supplies `libudev` and is typically already present.

Runtime requirements are a separate list, and they are deployment facts rather than build inputs:

| Requirement                       | For                                                       |
| --------------------------------- | --------------------------------------------------------- |
| `mesa-vulkan-drivers`             | lavapipe — [software rendering](#software-rendering-is-the-floor-tier) |
| `fbcon=off`, no Plymouth          | [firmware handoff](#from-firmware-to-gyro)                 |
| `CONFIG_DRM_PANIC`, `pstore`      | kernel panics with no console to print to                  |
| `sysrq` enabled                   | a wedged gyro that `RLIMIT_RTTIME` cannot catch; `SysRq-V` to restore a picture |
| `pam_systemd` in the PAM stack    | `XDG_RUNTIME_DIR` and `systemd --user`, via the login agent |
| udev rules for `/dev/dri`, `/dev/input` | device access without capabilities                    |
| `LimitRTPRIO=`, `LimitMEMLOCK=`   | `SCHED_FIFO` and page residency without capabilities        |
| io_uring with `SINGLE_ISSUER` and `DEFER_TASKRUN` | [the frame ring](#why-io_uring); probed at startup, not inferred from a version |

**gyro does not carry a fallback for that last row**, and the omission is deliberate. An epoll frame
loop is easy to write and would be exercised by nobody, which is the argument
[the floor tier](#the-floor-tier) makes about render modes and
[the pre-Vulkan console](#the-pre-vulkan-console) makes about recovery paths, pointing the other way
for once: a path that never runs is not insurance. What gyro owes instead is a legible failure — the
startup probe names the flag and the `io_uring_disabled` sysctl on the console, rather than leaving
an `EINVAL` from `io_uring_setup` as the only evidence on a machine with
[no VT](#boot-and-the-display-lifetime) to read it from.

`sysrq` deserves its place, and it has two halves. `RLIMIT_RTTIME` catches a real-time thread that
spins, which is one failure mode; a frame thread deadlocked on a driver lock is *blocked*, accrues
no real-time budget, and trips nothing. On a machine
[decision 37](Decisions.md#37-gyro-owns-the-display-from-firmware-handoff-onward-there-are-no-vts)
has deliberately left with no VT, that leaves the keyboard as the only way in. The other half is
`SysRq-V`, which the DRM core registers for every device and which forces the in-kernel client to
restore — the only key that puts a picture back rather than ending the process holding one. What it
restores under `fbcon=off` is [open](Open.md).

## What nested can and cannot prove

Testable nested — the majority of the codebase:

Protocol implementation and client compatibility · surface lifecycle and damage tracking · **client
dmabuf import** (clients connecting to gyro are ordinary Linux clients handing over real dmabufs, so
this path is fully live) · Xwayland · scene graph and transforms · animations and effect shaders ·
input routing and xkb · window management policy · multi-output layout · **[geometry](#geometry)** —
`--outputs N` with a different fractional scale per host window exercises snapping, rational scale,
the straddle case, and the resample-once rule, and golden images make the crispness claim
falsifiable rather than felt · **the session model** — nested can fake several sessions and hand
each a listener, which makes session switching, per-session registry filtering, lock state, and
output reassignment testable without a second user or a second machine · **the composite half of
[color](#color)** — linear
blending, premultiplied-alpha handling, blur in linear light, and the client-shadow question in
[decision 48](Decisions.md#48-linear-blending-is-a-visible-ecosystem-change-and-gyro-takes-it) are
all pixels in a buffer and need no display to settle · **[the shell split](#the-shell)** — a shell
is an ordinary client, so the scene vocabulary, the declare-don't-drive rule, gesture cohesion
across several chrome processes, the background cache, and what a shell restart looks like against
the floor policy are all exercisable nested, and the session model above already fakes the sessions
to hang them on.

Not testable nested:

Real vblank pacing and latency budgets · `SCHED_FIFO` behaviour under contention · atomic
modesetting, plane assignment, hardware cursor · **direct scanout of client buffers**, and with it
whether the KMS color pipeline can express what the composite would have done · VRR panel
response · **the display half of color** — HDR output modes, gamma LUTs, EDID and panel characterisation · tearing control ·
multi-GPU · DRM master loss, device pause/resume · real hotplug and DPMS · **the boot path** — BGRT
reproduction, the firmware-mode handoff, and `simpledrm` → real-driver
[migration](#device-migration), none of which have a nested equivalent.

The second list is largely the DRM backend's own code, which is work done in front of real hardware
regardless.

One thing that *looks* like it belongs in the second list and does not: **software rendering**.
Because it is a device selection rather than a backend, nested with lavapipe forced exercises the
same floor tier the DRM backend falls back to, thread priorities included.

And one entry on the second list belongs there only in part: **plane assignment.** Whether a given
controller accepts a given partition is hardware, and is not answerable anywhere else. But the
assigner that proposes partitions, the fallback when one is refused, and the frame loop's behaviour
when a refusal lands after the budget was planned are all ordinary logic, and the synthetic plane
catalog in [what to build](#what-to-build-before-it-is-needed) makes them headless tests. The
distinction is worth holding onto: what cannot be proven without hardware is the driver's answer,
not gyro's question.

**VRR** divides on that same line, and it moved once the servo became a property of the clock rather
than of the present. What needs a panel is the panel's answer — whether a commanded period is
honoured, where the flicker threshold sits, what the advertised range is worth, and whether a
cursor-plane commit disturbs the refresh timer. What does not is everything above that: the servo
commands a period and reads back an observation, and the headless fake clock can lie about that
observation in every way a real panel can, refusal included. So the rate limit, the
[arrival transition](#vrr-as-a-scheduling-degree-of-freedom), and the plan swap are headless tests
rather than hardware ones — which matters, because they are the half that can silently cost another
output its deadlines.

## What to build before it is needed

The analysis in [Presentation timing](#presentation-timing) and [Effects](#effects-and-quality) is
deferrable — `Admit()`, the cost table, tier selection, chunking, early rendering, the VRR servo are
all pure additions on top of what is below. The *seams* they need are not deferrable, and most of
them are close to free to build now and expensive to retrofit.

**Ship with policy hardcoded to full quality, always. Build the affordances anyway.**

In the animation system, as it is written:

- **`Evaluate(presentationTime)` and nothing else.** No ambient clock, no `deltaTime`, no global
  "now" reachable from the render path. Enforce it mechanically rather than by review — if a global
  now is reachable, something eventually reaches it.
- **One reachable time source, and `Instant` distinct from `Duration`.** The stronger form of the
  point above: not merely no ambient now on the render path, but one place in the process that calls
  `clock_gettime` at all, injectable so the headless fake clock is real rather than a fiction. The
  type split is what stops a cross-domain subtraction from compiling, and retrofitting it means
  touching every timestamp in the system at once. See
  [the timebase](#the-timebase).
- **A debug allocator that aborts.** Global `operator new` overridden in debug builds against a
  thread-local flag set around the frame section. Thirty lines before there is anything to catch;
  archaeology afterwards. Thread-local is the operative word — the dispatch thread allocates and
  must, so this is a property of the frame thread's frame section and never of the process.
- **Scoped CPU timing that knows which output it is in.** Nothing reads it but a log at first. The
  retrofit cost is not the timer, it is threading *which output am I* through code that never needed
  to know.
- **Per-output budget as real data**, even while it is a constant. `NextWakeup()` already implies
  it.
- **A bundle can be driven by a parameter, from the first bundle.** Whether a transition is watched
  or made is a dimension of its definition the way reduced motion is, so adding it later means
  revisiting the catalog rather than extending it — and the parameter reaches the frame thread as
  coefficients from the start, for exactly the reason springs do. See
  [interactive transitions](Animation.md#interactive-transitions).

For [the publication boundary](#the-publication-boundary), which is a shape rather than a feature
and is the least retrofittable thing here:

- **Client state reaches the frame thread through exactly one channel, from line zero.** Not "a
  mutex for now, a ring later" — the second reader of a shared structure is the one that never gets
  removed, and every lock added across this boundary is invisible until it is a jitter bug.
- **No shared ownership across the boundary.** Deferred reclamation against a consumed-sequence
  counter, so `free` never runs on the frame thread. A `shared_ptr` crossing the boundary is an
  allocator call on the frame path that the debug allocator cannot catch, because the destructor
  runs wherever the last reference happens to die.
- **The published snapshot is offset-addressed POD, and the frame side bounds-checks what it
  resolves.** Pointers and standard-library containers are the obvious choice and they foreclose
  moving the snapshot into a shared mapping, which is what
  [decision 49](Decisions.md#49-the-restart-boundary-is-made-cheap-where-it-can-be-and-stated-where-it-cannot)
  needs if dispatch ever becomes a process rather than a thread. Base addresses differ between two
  processes; offsets survive that and pointers do not. Generational handles already supply most of
  the checking half.
- **The scene reads a snapshot, not the protocol objects.** The shadow object model is needed
  whether or not the wire protocol is gyro's own, so writing the scene against `wl_resource`-shaped
  types would be wrong even under libwayland.
- **Springs cross as coefficients from the first published spring.** Evaluated values are cheaper,
  correct on one monitor, and wrong in a way that only appears on the configuration
  [Presentation timing](#presentation-timing) exists to serve.
- **The return path is a channel, not four exceptions.** Frame callbacks, presentation feedback,
  buffer release, and the exit-blit hold all flow frame → dispatch whether or not anything is built
  to carry them. Building the queue first is a few dozen lines; discovering it afterwards means
  finding four ad-hoc mechanisms with four different lifetimes.

For [the shell](#the-shell), which is a boundary rather than a feature and will have exactly one
implementation for a long time:

- **gyro's built-in policy speaks the same protocol a shell would.** The default window management
  that brings the system up is not a privileged internal path with a client interface added later —
  it is the first consumer of the seam. That is what keeps the floor policy honest, and it is the
  same argument [the floor tier](#the-floor-tier) makes about render modes.
- **The node vocabulary is closed from the first node kind.** Adding kinds is easy; removing the
  ability to describe arbitrary ones is not, and an open vocabulary loses cohesion silently.
- **Output assignment goes through a gate that can wait**, even while the only thing it waits for is
  trivially ready. Retrofitting the wait means retrofitting it into every path that assigns an
  output, including lock and session switch.
- **The background is gyro's from line zero**, because the effect path, the boot path, and every
  fallback screen assume something is behind them.

For [color](#color), which is structural in the same way and for the same reason — it is chosen
the day the renderer is written, and everything authored against the wrong answer is re-authored:

- **The composite space is linear, wide, and brightness-relative**, and it is not the output space.
  The moment it is the output space, two monitors with different gamuts force two composites and a
  surface's appearance depends on which one it is on.
- **A color state on every surface from line zero**, defaulting to sRGB by rule. It is a field on
  the shadow object model, which [the publication boundary](#the-publication-boundary) requires
  building regardless.
- **Alpha is un-premultiplied before linearisation, once, at import.** Retrofitting this means
  finding every sample site rather than one import path.
- **The composite target format is stated, and the blur chain carries no alpha.** Both are trivial
  now and are a renderer rewrite once passes are written against them.
- **Direct scanout is conditional on the KMS pipeline expressing the same transform.** A rule
  attached to plane assignment before plane assignment exists, rather than a special case added to
  it afterwards.

For [geometry](#geometry), which is chosen the day the scene graph is written and re-authored
afterwards if it is chosen wrongly:

- **Scale is a rational type from the first surface**, never a float that a rational replaces later.
  The replacement means auditing every size computation in the system to find the one that still
  rounds, and that one is on somebody else's monitor.
- **Nothing stores a rounded coordinate.** Rounding is a function of `(node, output, frame)` and the
  result is never written back. A discipline rather than a feature, free now, and the retrofit is
  finding every place a logical integer was cached.
- **The transform classification exists before it has a second caller, and it returns independent
  facts rather than a boolean or a rung.** Plane promotion, damage mapping, and the sharpness path
  ask *related* questions and not identical ones — the first asks what a given plane can express, the
  other two ask whether the transform is a no-op — and three independently derived answers is how
  they drift apart. The properties do not order, so no ladder expresses them; see
  [resample once](#resample-once-and-know-when-it-is-zero).
- **Transforms are 3D from the first node**, with an explicit anchor point and rotation carried as a
  quaternion. Widening a 2D transform afterwards means revisiting every spring, every hit test, and
  every damage bound.
- **Geometry crosses the publication boundary at double precision.** Four bytes, decided once.
- **The mip chain is built in linear light**, off the same import path that already
  un-premultiplies. Built from the encoded buffer it looks right in one screenshot and is wrong at
  every level.

Recorded now, honoured when the renderer lands:

- **A quality parameter on every effect** — chain internal resolution and pass count — plumbed from
  the first pass and pinned to maximum. Varying it later is then choosing an argument rather than
  rewriting a shader pipeline.
- **The floor tier as a supported, exercised render mode**, not a fallback.
- **Damage accumulates per output since its last successful present** — and per *plane* once there
  is more than one, since `FB_DAMAGE_CLIPS` is a plane property and offloaded layers accumulate
  separately even though they retire together at the flip. Modelled per frame, a skipped frame
  becomes a correctness bug; modelled per output only, the first thing ever promoted has the wrong
  damage.
- **The frame loop tolerates an output not rendering this iteration.** Not the check itself, which
  is three lines — the loop shape that permits it.
- **`FrameClock` separates the period it commands from the period it observed**, and `P` is a
  minimum inter-arrival everywhere it is written down, even while every output is fixed-refresh and
  the two are the same number. Both are free to say now. The retrofit is not the field, it is that
  [VRR](#vrr-as-a-scheduling-degree-of-freedom) makes them differ on the one configuration nobody
  develops against, so the code that conflated them is correct on every machine it was written on.
- **GPU timestamp queries around each pass**, for the same reason as the CPU timers.
- **`IPresenter` treats backend-allocated and imported targets as one concept**, not as a base case
  with imported targets bolted on later for [virtual
  outputs](#virtual-outputs-and-why-remote-desktop-is-one). Nested and virtual outputs are the same
  shape — present into buffers someone else owns, receive feedback back — and only the source of the
  constraint differs. Formats that are not RGB, and `AcquireTarget()` legitimately yielding nothing
  under backpressure, fall out of the same generalization.

Timeline semaphores are already the sync primitive throughout, so the non-blocking "is the previous
frame still in flight" query needs nothing new.

For [plane offload](#direct-scanout-is-conditional), where the interface is the half that cannot be
widened afterwards and the policy is the half that can wait:

- **`Present()` takes a layer list, not a target index.** *(Taken 2026-08-17;
  [decision 78](Decisions.md#78-present-takes-a-layer-list-and-the-composite-is-one-member-of-it).)*
  Each layer carries a source, an acquire point, damage, a source crop, a destination rect, a blend
  mode, and a color state, with z as the list order, and the GPU-composited remainder is one more
  layer in that list rather than a separate concept. This is the `IPresenter` bullet above about
  imported targets, seen from another side: `AcquireTarget()` supplies the composited layer and a
  client's dmabuf supplies an offloaded one — and the second of those is the half still open, since
  turning a client dmabuf into a scanout framebuffer is a kernel allocation that cannot happen in
  the frame section while the presenter is frame-side. A one-element list is what nested and
  headless present today, so nothing was given up to say it this way.
- **Headless carries a synthetic plane catalog.** N pipes with declared capabilities and a scripted
  refusal policy. It is a few dozen lines, and it is the only way the assignment path, the fallback
  to compositing, and a refusal arriving *after* the frame's budget was planned are exercised before
  the DRM backend exists. Without it the multi-layer path is code that has never run, which widens
  [decision 5](Decisions.md#5-the-drm-backend-is-designed-from-specification-with-no-hardware-spike)'s
  exposure rather than paying it down.
- **A capability descriptor filters; the atomic test decides.** Real controllers constrain on
  per-CRTC bandwidth, core clock, line width, scaler ratios that vary with format, and pipes that
  serve two rectangles when both are small enough. None of that is a per-plane flag, so a descriptor
  rich enough to be an oracle is a descriptor that lies. It exists to eliminate the obviously
  impossible cheaply; `TEST_ONLY` is the only authority, and the assigner is written to expect
  refusal rather than to avoid it.
- **A surface may exist with no linearised copy and no mip chain.** The largest single offload win
  is video — NV12 or P010 straight to a plane with fixed-function conversion, the GPU untouched and
  the [composite target](#the-composite-space) never written — and both of those are precisely the
  costs [color](#color)'s import path pays for every surface unconditionally. So the import is a
  property of *how a surface is being used this frame* rather than of the surface. What a refused
  promotion then costs is [open](Open.md).
- **A client is not promoted until it can afford the hold.** A buffer on a plane is held until the
  next flip retires it, so promotion costs a double-buffered client one buffer and halves its rate.
  A promotion that makes a client visibly worse is the temporal form of the defect [direct
  scanout](#direct-scanout-is-conditional) exists to prevent, so the predicate carries a precondition
  on observed buffer rotation, and [resource accounting](#resource-accounting) attributes a held
  scanout buffer rather than losing track of it.

For [device migration](#device-migration), which is not deferrable in the same way — it runs on the
first boot the DRM backend ever completes:

- **No GPU resource is the only copy of anything.** Every texture, atlas, and buffer is re-creatable
  from CPU-side truth. This is a discipline, not a feature, and it is free at line zero. [Exit
  snapshots](#device-migration) are the single exception, and they are permitted to be lost rather
  than exempted from the rule.
- **All Vulkan handles live behind a unit that tears down and rebuilds whole.** No global `VkDevice`,
  no static pipeline handles, no handle outliving the device that made it.
- **Snapshot storage is reserved at output configuration, never allocated at close time.** The
  reservation is what keeps `vkCreateImage` off the frame path and what bounds how long a client
  buffer can be held. Retrofitting it means retrofitting an eviction policy into code written on the
  assumption that a snapshot always succeeds — see [exit pixels](Animation.md#exit-pixels).
- **`wp_linux_dmabuf_feedback_v1`, never static format advertisement.** The modifier set changes
  when the device changes; the static path cannot express that.

And for keeping the picture on the glass across gyro's own lifetime — [restart](#restart) and
[running from the initramfs](#the-pre-vulkan-console), which the kernel reading turned into one
mechanism, since both work by never letting the DRM file description be released. The second is
deferred; neither should be foreclosed:

- **The platform seam takes an already-open DRM fd as its primary path**, with "open one yourself"
  as the default provider rather than the only one. gyro already treats receiving an fd from
  elsewhere as first-class, from the [listener handover](#listener-handover).
- **gyro adopts an existing mode rather than unconditionally modesetting.** Read the current CRTC
  state and modeset only on difference. Four callers want this: re-exec, `simpledrm` → real driver,
  recovery after a crash, and any Plymouth takeover we are forced into.
- **`FDSTORE=1` goes out as soon as master is taken** — before Vulkan, before outputs, before
  anything else that can fail. The kernel blanks a plane whose framebuffer's creating file is
  released, so the interval between taking master and populating the store is exactly the interval
  in which a crash costs the picture. Ordering rather than machinery, and invisible once the startup
  path has grown.
- **`RLIMIT_RTTIME` on every real-time thread**, from the first one. It is two lines, and the
  machine it protects has no VT to escape to.
