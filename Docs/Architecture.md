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

	virtual void Present(uint32_t target, SyncPoint renderFinished, const Region& damage) = 0;

	Signal<const PresentationInfo&> Presented;           // frame reached glass
	Signal<>                        TargetsInvalidated;  // resize, mode set, modifier renegotiation
};
```

The consequence that matters: **the renderer never sees a `VkSwapchainKHR`.** It renders into
dmabuf-backed `VkImage`s and produces a `SyncPoint`. That is exactly what KMS needs, and it is
achievable nested by driving `zwp_linux_dmabuf_v1` directly rather than going through Vulkan WSI.

Sync is uniform: DRM syncobj timelines throughout, exported from Vulkan timeline semaphores.
On KMS that becomes `IN_FENCE_FD` / `OUT_FENCE_PTR`; nested it becomes `wp_linux_drm_syncobj_v1`.
Damage is in the interface from the start because both paths want it — `FB_DAMAGE_CLIPS` on KMS,
`wl_surface.damage_buffer` nested.

> Written from specification rather than from a hardware prototype. The interface is deliberately
> over-provisioned — plane assignment, per-plane fencing, mode-set versus page-flip — so that the
> DRM backend has room to fit without a redesign. Expect these spots to need revisiting when the
> DRM backend lands; they are marked `// SPEC:` in the headers.

### The frame clock

The scheduler never talks to KMS directly. **There is one clock per output, never one for the
machine** — see [Presentation timing](#presentation-timing) for why that is the load-bearing choice
and not an implementation detail.

```cpp
class FrameClock
{
public:
	void Observe(const PresentationInfo& info);   // presented-at, period, sequence, flags

	// The next frame this output owes.
	Nanoseconds NextDeadline() const;             // when the commit must land
	Nanoseconds NextPresentation() const;         // when that commit reaches glass
	Nanoseconds NextWakeup() const;               // deadline - renderBudget - safety

	// Any future frame. Speculative rendering needs this, and it is only answerable
	// because animation is a pure function of time.
	Nanoseconds PresentationAt(uint64_t sequence) const;

	Nanoseconds        Period() const;
	Range<Nanoseconds> PeriodRange() const;       // VRR window; degenerate when fixed
	bool               IsVariable() const;        // deadline may be deliberately deferred
	bool               IsPrecise() const;         // hardware clock, or best-effort
};
```

Sources per backend:

- **DRM** — page-flip event timestamps, with `DRM_CAP_TIMESTAMP_MONOTONIC`.
- **Nested** — `wp_presentation_feedback.presented`, which carries presentation time, refresh
  period, sequence, and flags including `VSYNC` / `HW_CLOCK` / `ZERO_COPY`. Structurally identical
  to KMS.
- **Headless** — a fake clock the tests drive, including deliberately missed deadlines and
  arbitrary mixed-refresh combinations.

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

	Signal<Fd>   DevicePaused;
	Signal<Fd>   DeviceResumed;
	Signal<bool> ActiveChanged;
};
```

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
  drags, and per-output scale become testable without owning three monitors.
- **Dynamic modes** — a window resize *is* a mode change. Handling that from day one means real
  hotplug and mode-setting work when the DRM backend arrives instead of being a rewrite.

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

Atomic modesetting, plane assignment, hardware cursor, explicit fencing, VRR, and the colour
pipeline. Built third, designed for from the start.

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

Two things follow that gyro cannot solve and that become deployment requirements:

- **`pstore` must be enabled.** A kernel panic now prints where nobody is looking, so the oops has
  to be readable on the next boot.
- **A systemd `OnFailure=` unit runs `gyro --console`.** gyro failing to start otherwise leaves no
  local way into the machine at all — worse than "no UI", because there is no VT to fall back to.

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
watchdog against a `SCHED_FIFO` thread running continuously without blocking, which on a machine
with no VT is a hard lock.

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

Three of them, and the boundaries between them are the design.

| Thread | Runs | May allocate |
| --- | --- | --- |
| Frame | Evaluate, record, submit, present | No, inside the frame section |
| Device workers | llvmpipe rasterization, [software path only](#software-rendering-is-the-floor-tier) | Not ours to say |
| Dispatch | Wire protocol, input, buffer import | Yes, and it must |

**The priority order follows from what blocks on what**, and is the same on every device: frame
thread highest, device workers below it, dispatch last. The frame thread *waits* on device workers —
a `vkQueueSubmit` or `vkWaitSemaphores` against llvmpipe is a wait on threads in our own address
space — so those must outrank everything beneath them, or a preempted worker stalls a composite. The
frame thread never waits on dispatch, so dispatch goes last. Absolute levels differ between the
accelerated and software paths; the order does not. Nested and headless are exempt from `SCHED_FIFO`
altogether, consistently with their exemption from `mlockall` and DRM master.

### The publication boundary

The frame thread reads client state through a one-way, single-producer / single-consumer
publication. It is the *only* channel between the two threads, and three of its properties are
requirements rather than optimizations:

- **Wait-free on the reader.** The frame thread swaps in the newest published snapshot. It never
  takes a lock the dispatch thread can hold, because a lock shared with a thread it outranks is
  precisely the priority inversion the ordering above exists to prevent.
- **The unit is a commit, not a message.** [Declarative
  commits](Decisions.md#14-declarative-commits-with-dirty-tracking) already make an atomic set of
  surface state the natural granularity, so a surface and its subsurfaces cross together or not at
  all. Publishing per message would let a frame observe half a transaction.
- **Reclamation is deferred and one-way.** The frame thread publishes the sequence it last consumed;
  the dispatch thread frees below that. Shared ownership across the boundary puts `free` on the
  frame path wearing a destructor's clothes, where the debug allocator will not catch it.

**A commit lands in the frame whose record point it beats, and otherwise in the next one.** Wayland
permits this unconditionally — clients are paced by frame callbacks and have no say in when a
compositor reads them — so nothing is given up to get it. See
[decision 45](Decisions.md#45-protocol-dispatch-is-a-thread-not-a-task).

**What is deliberately on the far side:** per-message allocation, `wl_shm` mapping and the SIGBUS
window it opens, dmabuf import, Vulkan resource creation from client buffers, and libinput. Every
one of them is unbounded, and every one of them was implicitly on the frame path before the split.

**Cross-thread lifetime is the cost.** A client may destroy a surface while the frame thread holds
it. [Generational handles](Decisions.md#15-identity-is-a-generational-handle) supply the detection
and [exit snapshots](Decisions.md#20-exit-animations-use-full-resolution-snapshots) already
establish that an entity outlives its protocol object, so the mechanisms exist — but the discipline
is new, and it is the one part of this split that is not free.

**Reclamation needs a per-buffer hold as well as the watermark.** The exit path defers its snapshot
blit by a frame or two to keep it off the budget of the frame where the surface died
([Animation.md](Animation.md#exit-pixels)), which means the frame thread holds one client buffer
past the commit that carried it. The consumed-sequence watermark cannot express that — withholding
it would stall reclamation of every unrelated commit below — so an individual hold, released when
the blit lands, sits alongside the watermark rather than being encoded in it. It is the only
reader-side hold in the design, and it should stay that way.

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

### Outputs are independent periodic tasks

Two outputs are genuinely independent. **The only thing coupling them is that they share one frame
thread and one GPU queue.** There is no other channel, and modelling this as anything more
complicated than resource contention is how the wrong design gets built.

So this is the classic periodic real-time task problem, and it should be treated as exactly that:

- Each output is a task with a measured period `P`, a measured execution time `C`, and a deadline
  equal to its period.
- The frame thread is a uniprocessor running them earliest-deadline-first.
- Composites are **non-preemptive** — once GPU work is submitted it runs to completion.

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

Measured periods still matter, which is what `FrameClock::Observe` is for. They feed `P` and `C` in
the test above. Phase does not feed anything.

### Admission control

The test runs when the configuration changes — an output added or removed, a mode set, a measured
cost moving the floor — not every frame. It is solved for `C`, so its output is an **allocation**:
how much each output may spend per frame. See
[decision 29](Decisions.md#29-outputs-are-periodic-real-time-tasks-the-test-allocates-effect-budget)
for why `C` is a budget gyro enforces rather than a cost it observes — briefly, `C` is a property of
the scene as much as of the output, and the peak is an overview transition nobody can predict at
plug-in time.

Where the allocation is smaller than the current scene wants, the ladder is:

1. **Shift a VRR output's phase and period.** Free — no visual cost. Available only when one of the
   outputs is variable-refresh and the shift stays inside its supported range.
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

Two refinements make it better than the naive version:

- **Pull the frame callback forward too.** Frame callbacks are the compositor's lever on when
  clients produce, and because the schedule is known statically, the clients on that output can be
  asked for their content earlier. For clients that use `wp_presentation_feedback` and render for a
  stated target time this recovers the freshness entirely; for clients that simply draw on callback
  it is neutral. Nobody uses frame callbacks predictively today.
- **Bound the lead to one period, and require a free target.** Rendering one frame ahead means three
  targets in flight on that output — one scanning out, one pending flip, one being recorded.

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
adjusted — changing `P` for that output changes the test's inputs, and it is the only input that can
be changed without giving anything up. Constraints, all of which push the same way:

- The resulting interval must stay inside the panel's supported range, clear of low-framerate
  compensation at the bottom.
- Abrupt refresh changes cause visible brightness flicker on many panels.
- A large phase step is perceptible as a stutter even when every deadline is met.

All three are answered by rate-limiting the adjustment — a small bound per frame, converging to the
target phase over several frames. It is a servo, not a jump.

One conflict has to be resolved explicitly: **content-driven VRR wins over scheduling-driven VRR.**
If a fullscreen client is driving the refresh rate for its own reasons, gyro does not servo against
it and admission control falls through to the next rung.

> Written from specification. VRR phase control, panel range behaviour, and flicker thresholds are
> from documentation rather than from hardware, and are marked `// SPEC:` where they land in code.

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

## Effects and quality

Effects are where `C` comes from, so they are also where it is controlled. Plain composition has not
been a scheduling problem for a decade; blur is, and blur is the feature.

### Materials, not filter calls

A surface declares a **material** from a closed vocabulary — `Material::Glass`, `Material::Sidebar`,
`Material::Hud` — and gyro decides what that means this frame. Shell code names no radius, no pass
count, no chain resolution, exactly as it names no spring parameters.

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

### The floor tier

The floor composite — no effects, base composite only — is a first-class render mode used by tests
and headless golden images, not an emergency fallback. A recovery path that has never run is broken
when it is needed.

Its cost `C_min` is a design target rather than a residue, because it bounds what the system can
absorb: an overrun is recoverable in one frame only if `t_done + C_min ≤ deadline`. A cheap floor
composite is what buys the promise in
[decision 35](Decisions.md#35-a-miss-costs-one-frame-bounded-by-the-floor-composite).

## Event loop

`io_uring` with `IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN`, via liburing. There are
two loops on two [threads](#threads), and `SINGLE_ISSUER` — one submitting thread per ring — means a
ring each.

### The frame loop

**Timer-first, not event-first**, and it now has no client traffic to fit around: the only fds on
this ring are timers and KMS.

```
	due    = outputs owing a frame, earliest deadline first
	wakeup = earliest NextWakeup() among them

	submit an absolute IORING_OP_TIMEOUT for wakeup
	wait for completions

	acquire the newest published client state    // wait-free, once per iteration

	for each output in due:                    // earliest deadline first
		if previous frame still in flight or now + C_planned > deadline:
			fall to the floor tier, or skip this output entirely   // see below
		evaluate animations at Clock(output).NextPresentation()
		record and submit per the admitted plan
		Presenter(output).Present(...)

	for each output the admitted schedule places early here:
		evaluate animations at Clock(output).PresentationAt(sequence)
		record and submit, holding the target until its flip

	publish the consumed snapshot sequence      // releases the dispatch thread to reclaim
```

Acquisition is once per iteration rather than once per output. Per-output acquisition would be
fresher and is expressible — the boundary is wait-free, so a second acquire costs nothing — but a
window straddling two outputs would then show two different client frames in the same iteration,
which is visible on precisely the configuration this section exists to serve. Staleness is bounded
by one iteration; the seam artefact is bounded by nothing.

The plan this executes is static, computed by admission control at configuration change. The loop
does not decide anything about timing relationships; it runs the schedule it was given.

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
feels directly. The per-client budget is what stops one chatty connection delaying twenty others,
and it is the lever [decision 2](Decisions.md#2-the-wayland-wire-protocol-is-implemented-in-tree)
notes libwayland does not offer.

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
- **Kernel floor.** `SINGLE_ISSUER`, `DEFER_TASKRUN`, and multishot `recvmsg` are Linux 6.0+. This
  must be a stated, checked minimum, not an accident of the development machine.

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
process with no restart boundary, where the local wire protocol at least requires an account on the
machine. So remote desktop is a client, a screen recorder wanting per-output pre-composite frames is
a client, and a tablet used as a second display is a client. The word "remote" does not appear in
gyro.

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
- **The control socket is an unauthenticated entry point** into the one process on the machine with
  no restart boundary. Bounded by `SO_PEERCRED` on the offer, a per-uid offer cap, and one accepted
  listener per session.
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
   gyro reassigns the output: greeter ──cross-fade──▶ user session
```

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
  presentation-time.
- **Session-scoped** — data device and primary selection, `xdg_activation`, `xdg_foreign`,
  text-input and input-method, idle-inhibit.
- **System tier** — screencopy and screencast, [virtual output
  registration](#virtual-outputs-and-why-remote-desktop-is-one), foreign-toplevel management,
  layer-shell, output configuration, and the lock protocol below.

Connections carry a trust level of `User` or `System`. The enum is what is expensive to add later;
its membership is not.

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
per-user wallpaper, and widgets are all the same problem, since they live at the user's uid and the
greeter is not it. The designed answer, deliberately not built initially, is a **lock-screen content
surface** — one non-interactive surface a locked session may present, composited below the greeter's
UI on the output it was locked out of. Non-interactivity is the safety property: keystrokes meant
for the password field can never reach a client of the locked session, which also puts notification
*actions* out of scope. It is safe to defer because unification does not foreclose it.

One requirement falls out. The greeter is now on the critical path for getting back into the machine
and one greeter serves every session, so its crash locks out everybody. The login agent must respawn
it, and gyro's lock state must be entirely independent of the greeter's liveness — the output stays
black across a respawn.

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
limit exists is narrower: **gyro has no restart boundary.** A session compositor that dies loses one
login and is restarted by its greeter. gyro dying takes the machine's UI to black with nothing
underneath. So limits are backstops an order of magnitude above anything real — total mapped shm
bytes, total imported dmabuf bytes, per-connection object count, with `LimitNOFILE` raised well past
where it can bind — and the response is a protocol error killing the offending client, which the
protocol already sanctions, rather than throttling anyone.

One thing that is not policy at all: a client that stops reading its socket. Unbounded send
buffering is a memory bug, so something must be decided regardless of how many users exist. That
belongs with backpressure in the protocol layer, not here.

### Consequences for the protocol layer

The wire implementation now terminates untrusted input from every account on the machine, and it is
hand-written. Continuous fuzzing of the demarshaller is a requirement rather than a nicety, and it
belongs in decision 2's cost column.

## Wayland protocol layer

gyro implements the Wayland wire protocol itself, on both sides, rather than using libwayland.

**Why — and the reason is not real-time.** [Threads](#threads) takes wire dispatch off the frame
thread, so libwayland's per-message allocation would land somewhere unbounded latency costs one
client some latency and costs the frame nothing. Nor is it io_uring: libwayland nests via
`wl_event_loop_get_fd()` at the price of a wakeup per dispatch, which on the dispatch thread is not
a price. What actually remains:

- **gyro has no restart boundary.** libwayland resolves allocation failure and internal invariant
  violations by calling `wl_abort()`. A session compositor that dies loses one login; gyro dying
  takes the machine's display, on a system with [no VT](#boot-and-the-display-lifetime) to fall back
  to and whose recovery console is gyro itself. This is the decisive argument, and it is the one
  still awaiting verification against libwayland's source — see decision 2's open item.
- **A per-client dispatch budget**, which libwayland does not offer and which one dispatch thread
  serving every client on the machine needs.
- **Typed C++23 bindings**, and no `wl_list` / `wl_listener` object model at the boundary, where
  destroy-listener lifetime bugs are the best-known failure family in compositors built this way.
- **Deterministic dispatch ordering.** Input, then client traffic under budget. epoll readiness
  order gives no such control.

The SIGBUS trade is even rather than favourable: libwayland guards compositor `wl_shm` access with a
process-global signal handler and a `sigsetjmp` trampoline, so owning the protocol replaces an
inherited mechanism with a chosen one rather than adding a burden that was not already there.

**Staging.** The client half comes first, as the proving ground: one connection instead of many, and
a known-good peer on the other end. mutter is the primary target — strict, actively maintained, and
loud about malformed protocol — with a wlroots compositor as a second target before the codec is
trusted, since the two disagree in exactly the corners that are easy to get wrong. The server half
then reuses the codec and generator, leaving object lifetime, id allocation, and multi-client
backpressure as the genuinely new work.

**Bindings** are generated at build time by a host tool, with a hand-rolled parser for the XML
subset the protocols use. No scripting-language dependency, no third-party XML library, no generated
code in the tree.

**The hard parts** — the wire format is not among them:

- **Object lifetime and id reuse.** `delete_id` semantics and the zombie window in which a client
  sends requests to an object it has destroyed but the server has not yet acknowledged. libwayland's
  exact behaviour is the de facto specification, and this is the largest source of interop bugs.
- **fd accounting.** Buffering received fds against message order, the 28-fd send limit, partial
  sends of messages carrying fds, closing unconsumed fds on error paths.
- **Backpressure.** Clients that stop reading, send buffers filling, and what happens next.
- **SIGBUS on `wl_shm`.** A client may truncate the fd backing a pool while we have it mapped,
  buggily or maliciously, faulting us on access. libwayland guards compositor shm access with a
  sigbus handler; owning the protocol means owning that too. [Threads](#threads) moves the fault off
  the frame thread and onto the dispatch thread, which downgrades it from a missed frame to a client
  error — but the handler is still process-global, so it must be written as though it can fire
  anywhere. The exposure is confined to the upload of a *live* surface: shm content is copied into a
  compositor-owned image at commit in order to be sampled at all, so nothing downstream — [exit
  snapshots](Animation.md#exit-pixels) included — ever touches a client mapping. Importing the
  mapping instead, via `VK_EXT_external_memory_host`, is refused for exactly this reason; it would
  trade a SIGBUS we can catch for a GPU fault we cannot.
- **Cross-thread object lifetime.** A `wl_resource` dies synchronously on the dispatch thread when
  its client goes away, and the frame thread may be holding what it described. This is the cost
  [decision 45](Decisions.md#45-protocol-dispatch-is-a-thread-not-a-task) accepts, and it is the
  reason the shadow object model is gyro's regardless of what sits underneath — which is what
  narrows the libwayland question to "do we write a demarshaller and a socket manager".

Ecosystem risk on the server side is lower than it appears: Xwayland is a *client* and links
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
| `libxkbcommon`                 | pkg-config | keymaps, both directions               |
| `libdrm`                       | pkg-config | atomic KMS, dmabuf                     |
| `libinput`, `libudev`          | pkg-config | native input                           |
| Vulkan headers + loader        | pkg-config | renderer                               |
| `liburing`                     | pkg-config | event loop                             |
| spdlog                         | CPM        | logging — async sink only              |
| volk                           | CPM        | Vulkan meta-loader, dispatch overhead  |
| shaderc / glslang              | CPM        | runtime shader compilation, hot reload |

Not used: libwayland (protocol implemented in-tree), VulkanMemoryAllocator, libseat, basu, greetd
(the [protocol is implemented](#the-login-agent), the daemon is not used).

Fedora setup:

```bash
sudo dnf install wayland-protocols-devel libdrm-devel libinput-devel libxkbcommon-devel vulkan-loader-devel vulkan-headers liburing-devel
```

`systemd-devel` supplies `libudev` and is typically already present.

Runtime requirements are a separate list, and they are deployment facts rather than build inputs:

| Requirement                       | For                                                       |
| --------------------------------- | --------------------------------------------------------- |
| `mesa-vulkan-drivers`             | lavapipe — [software rendering](#software-rendering-is-the-floor-tier) |
| `fbcon=off`, no Plymouth          | [firmware handoff](#from-firmware-to-gyro)                 |
| `pstore` enabled                  | kernel panics with no console to print to                  |
| `pam_systemd` in the PAM stack    | `XDG_RUNTIME_DIR` and `systemd --user`, via the login agent |
| udev rules for `/dev/dri`, `/dev/input` | device access without capabilities                    |
| `LimitRTPRIO=`, `LimitMEMLOCK=`   | `SCHED_FIFO` and `mlockall` without capabilities            |

## What nested can and cannot prove

Testable nested — the majority of the codebase:

Protocol implementation and client compatibility · surface lifecycle and damage tracking · **client
dmabuf import** (clients connecting to gyro are ordinary Linux clients handing over real dmabufs, so
this path is fully live) · Xwayland · scene graph and transforms · animations and effect shaders ·
input routing and xkb · window management policy · multi-output layout · fractional scale and DPI ·
**the session model** — nested can fake several sessions and hand each a listener, which makes
session switching, per-session registry filtering, lock state, and output reassignment testable
without a second user or a second machine.

Not testable nested:

Real vblank pacing and latency budgets · `SCHED_FIFO` behaviour under contention · atomic
modesetting, plane assignment, hardware cursor · **direct scanout of client buffers** · VRR ·
HDR, colour pipeline, gamma LUTs, EDID · tearing control · multi-GPU · DRM master
loss, device pause/resume · real hotplug and DPMS · **the boot path** — BGRT reproduction, the
firmware-mode handoff, and `simpledrm` → real-driver
[migration](#device-migration), none of which have a nested equivalent.

The second list is largely the DRM backend's own code, which is work done in front of real hardware
regardless.

One thing that *looks* like it belongs in the second list and does not: **software rendering**.
Because it is a device selection rather than a backend, nested with lavapipe forced exercises the
same floor tier the DRM backend falls back to, thread priorities included.

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
- **A debug allocator that aborts.** Global `operator new` overridden in debug builds against a
  thread-local flag set around the frame section. Thirty lines before there is anything to catch;
  archaeology afterwards. Thread-local is the operative word — the dispatch thread allocates and
  must, so this is a property of the frame thread's frame section and never of the process.
- **Scoped CPU timing that knows which output it is in.** Nothing reads it but a log at first. The
  retrofit cost is not the timer, it is threading *which output am I* through code that never needed
  to know.
- **Per-output budget as real data**, even while it is a constant. `NextWakeup()` already implies
  it.

For [the publication boundary](#the-publication-boundary), which is a shape rather than a feature
and is the least retrofittable thing here:

- **Client state reaches the frame thread through exactly one channel, from line zero.** Not "a
  mutex for now, a ring later" — the second reader of a shared structure is the one that never gets
  removed, and every lock added across this boundary is invisible until it is a jitter bug.
- **No shared ownership across the boundary.** Deferred reclamation against a consumed-sequence
  counter, so `free` never runs on the frame thread. A `shared_ptr` crossing the boundary is an
  allocator call on the frame path that the debug allocator cannot catch, because the destructor
  runs wherever the last reference happens to die.
- **The scene reads a snapshot, not the protocol objects.** The shadow object model is needed
  whether or not the wire protocol is gyro's own, so writing the scene against `wl_resource`-shaped
  types would be wrong even under libwayland.

Recorded now, honoured when the renderer lands:

- **A quality parameter on every effect** — chain internal resolution and pass count — plumbed from
  the first pass and pinned to maximum. Varying it later is then choosing an argument rather than
  rewriting a shader pipeline.
- **The floor tier as a supported, exercised render mode**, not a fallback.
- **Damage accumulates per output since its last successful present.** Modelled per frame, a skipped
  frame becomes a correctness bug.
- **The frame loop tolerates an output not rendering this iteration.** Not the check itself, which
  is three lines — the loop shape that permits it.
- **GPU timestamp queries around each pass**, for the same reason as the CPU timers.
- **`IPresenter` treats backend-allocated and imported targets as one concept**, not as a base case
  with imported targets bolted on later for [virtual
  outputs](#virtual-outputs-and-why-remote-desktop-is-one). Nested and virtual outputs are the same
  shape — present into buffers someone else owns, receive feedback back — and only the source of the
  constraint differs. Formats that are not RGB, and `AcquireTarget()` legitimately yielding nothing
  under backpressure, fall out of the same generalization.

Timeline semaphores are already the sync primitive throughout, so the non-blocking "is the previous
frame still in flight" query needs nothing new.

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

And for [running from the initramfs](#the-pre-vulkan-console), which is deferred but should not be
foreclosed:

- **The platform seam takes an already-open DRM fd as its primary path**, with "open one yourself"
  as the default provider rather than the only one. gyro already treats receiving an fd from
  elsewhere as first-class, from the [listener handover](#listener-handover).
- **gyro adopts an existing mode rather than unconditionally modesetting.** Read the current CRTC
  state and modeset only on difference. Three callers want this: re-exec, `simpledrm` → real driver,
  and any Plymouth takeover we are forced into.
- **`RLIMIT_RTTIME` on every real-time thread**, from the first one. It is two lines, and the
  machine it protects has no VT to escape to.
