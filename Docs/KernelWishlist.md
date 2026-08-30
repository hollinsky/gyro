# Kernel Wishlist

What gyro wants from the kernel that the kernel does not currently offer, and what it does instead
in the meantime. Cross-cutting, like [Decisions.md](Decisions.md) and [Open.md](Open.md), and
distinct from both: a decision records a choice gyro made, an open item records a question gyro must
answer, and an entry here records a constraint gyro did not choose and cannot answer alone.

**Volatility: this document changes when the kernel does.** Every claim below is a reading of a
specific tree at a specific commit, cited to file and line, and every one of them can be falsified
by an upstream commit — which is the point of recording where they came from rather than what they
concluded. An entry leaves this file by the kernel changing, not by gyro deciding something.

**Why keep it at all**, given that gyro cannot fix any of it. Three reasons, and the second is the
load-bearing one:

- **A workaround whose cause is written down can be removed.** Behaviour that gets absorbed silently
  into a design becomes indistinguishable from a design choice within a year, and then survives the
  bug it was compensating for. Each entry below names what would let gyro delete code.
- **The seam should express the contract, not the workaround.** Where a driver's behaviour forces a
  shape on gyro, the interface records the *contract* — this call may block, that one may not — and
  the driver's behaviour is one implementation living inside it. A seam that instead encodes *the
  helper thread issues the commit because amdgpu takes a global lock* has carved a vendor's `TODO`
  comment into the architecture, and it will be load-bearing long after the comment is gone.
- **The constraint should be legible from outside.** gyro is an unusual consumer — a hard real-time
  frame thread servicing N displays through one submission path — and it is exactly the case that
  makes an interface's real-time contract matter. An artefact saying *on this driver a mode change
  costs N frames on unrelated outputs* is worth more upstream than an opinion.

**Source basis.** Everything below was read against `linux-next` at `4477a78374a5`, tagged
`next-20260814`. Line numbers are against that tree. Where a reading did not settle a question, the
entry says so rather than filling the gap.

## The defect underneath most of this

The atomic modesetting API has **one entry point serving two workloads with opposite contracts.**

Presentation is per-CRTC, sub-millisecond, and happens sixty to a hundred and forty times a second
on every output. Reconfiguration is device-wide, tens of milliseconds, and happens when somebody
plugs in a monitor. They are distinguished today by a *flag on the same ioctl* —
`DRM_MODE_ATOMIC_ALLOW_MODESET` — which means the ioctl's real-time contract must be the weaker of
the two, and drivers have reasonably taken the latitude that gives them.

This framing matters because it decides who is at fault, and therefore what fixing it looks like.
amdgpu's device-wide stall is not a bug against the interface; it is *permitted* by the interface.
So the fix is not a driver patch, and gyro should not design as though the next amdgpu release
removes the problem.

The three entries that follow are all this defect seen from different sides.

### `atomic_check` runs on the caller's thread, unbounded

`drm_atomic_nonblocking_commit()` (`drm_atomic.c:1887`) runs the full check — including the
driver's own `atomic_check` hook — synchronously before it queues anything. The uapi documents this
as a feature (`include/uapi/drm/drm_mode.h:1290-1296`: *"the driver will still check that the update
can be applied before returning"*), and for a normal client it is one: an invalid configuration
comes back as `-EINVAL` rather than as an event nobody is listening for.

For gyro it means `DRM_MODE_ATOMIC_NONBLOCK` does not do what its name promises. The flag defers
*commitment*; it does not defer *validation*, and validation is where the cost turned out to be.

**What amdgpu does with that latitude.** `amdgpu_dm_atomic_check()` (`amdgpu_dm.c:6083`) calls
`do_aquire_global_lock()` (`amdgpu_dm.c:5306`), which takes every modeset lock on the device via
`drm_modeset_lock_all_ctx()` and then walks every CRTC waiting on
`wait_for_completion_interruptible_timeout(&commit->hw_done, 10*HZ)` for each one's outstanding
commit — on the caller's thread, bounded in practice by outstanding commit completion (order of a
frame period) with a ten-second backstop. It fires for any bandwidth-affecting change, not only mode
changes. The driver's own comments call it a stopgap (`amdgpu_dm.c:6450-6459`: *"We have to
currently stall out here in atomic_check for outstanding commits to finish... TODO: Remove this
stall"*).

**What gyro does instead.** Never issues a reconfiguring commit from the frame thread. The frame
thread's non-preemptible section is bounded by composite cost — this is
[decision 29](Decisions.md#29-outputs-are-periodic-real-time-tasks-the-test-allocates-effect-budget)'s
`B(L)` term and it is the term the whole schedulability test turns on — and a synchronous
device-wide wait inside it is a `B` of a frame period against a test that quotes a 60 Hz projector
2.9 ms.

**What would let gyro delete it.** Either a bound on `atomic_check` the core enforces, or a way to
separate validation from commitment — the next entry.

### Validation cannot be separated from commitment

`DRM_MODE_ATOMIC_TEST_ONLY` exists and validates without committing. It buys nothing here, because a
state validated at T carries no token asserting it is still valid at T+ε, so the real commit
re-validates from scratch and pays the cost again.

**What gyro wants.** A `PRECHECKED` commit carrying a validation cookie, invalidated by any
intervening state change on the affected objects. Expensive checking happens off the real-time path,
on whatever thread can afford to block; what reaches the frame thread is a commit guaranteed cheap.
This is the most incremental of the three — it disturbs neither the commit model nor the locking —
and it is the one that would generalise beyond gyro, since every compositor that wants to modeset
without dropping a frame has the same problem.

**What it would not fix.** amdgpu's wait is not validation cost; it is an ordering requirement
arising from its IRQ handlers referencing DRM state directly. Prechecking removes the general
defect and leaves that specific one, which is why this entry does not supersede the one above it.

### Hardware coupling between CRTCs is invisible to userspace

The kernel knows which CRTCs share a bandwidth pool, a PLL, or a link. Userspace does not, and can
only discover coupling by being stalled by it.

This is the entry with the most to gain, because it is information gyro cannot obtain any other way.
[Decision 29](Decisions.md#29-outputs-are-periodic-real-time-tasks-the-test-allocates-effect-budget)
models outputs as independent periodic tasks coupled solely by one frame thread and one GPU queue —
not because hardware coupling does not exist, but because there is no interface through which to
learn about it. A **commit domain** exposed as the set of CRTCs that genuinely must serialize would
let the schedulability test carry hardware coupling as a real term instead of assuming it away.

Absent it, gyro's test is optimistic in a way it cannot detect, and the first symptom is a
configuration that passes admission and misses deadlines on hardware.

### VRR range is discovered, never negotiated

Once VRR is active, the effective refresh interval is controlled entirely by **when a flip is
submitted** — `intel_vrr_send_push()` writing `TRANS_PUSH_SEND` from `intel_pipe_update_end()`
(`intel_vrr.c:773-790`, reached from `intel_display.c:6949-6962` on every ordinary commit), and on
amdgpu `dc_stream_adjust_vmin_vmax()` reached from the per-flip surface-address path
(`amdgpu_dm.c:3974-3980`). No property is set, no modeset occurs, and no kernel-imposed rate limit
was found within the window. This is the primary interface rather than a workaround, and it is what
makes [decision 31](Decisions.md#31-vrr-is-a-scheduling-degree-of-freedom-not-only-a-latency-feature)'s
lever real and cheap.

What gyro cannot do is *ask* for the window. `intel_vrr_compute_vmin()` / `intel_vrr_compute_vmax()`
(`intel_vrr.c:397-422`) derive `[vmin, vmax]` from the current mode's `crtc_vtotal` and `crtc_clock`
together with the connector's EDID-reported `display_info.monitor_range`. There is no
userspace-settable property for it on i915 at all — the window is fixed once per mode, and gyro
discovers what it was given.

For a compositor scheduling *against* VRR that is the difference between a scheduling parameter and
a fact of the panel. gyro would negotiate the range at modeset time, as an input to admission
control rather than a constraint discovered afterwards.

*(The rate-limit search was not exhaustive: AMD's static-screen ramp and below-range compensation
hysteresis, and i915's DC-balance correction on `DISPLAY_VER >= 30`, are scenario-specific
smoothing rather than general clamps. No panel-firmware-side backpressure path was audited.)*

## Behaviours gyro works around, with the workaround stated

Distinct from the wishlist above: these are not interface defects but driver behaviour gyro must
route around today. Each names the check that would let the workaround go.

### `ALLOW_MODESET` is not free to set defensively

On AMD silicon below `IP_VERSION(3, 2, 0)`, `should_reset_plane()` (`amdgpu_dm.c:5647-5649`) returns
true for **every plane on the CRTC** for any commit carrying `ALLOW_MODESET`, regardless of what
actually changed — routing an ordinary flip into the full `do_aquire_global_lock()` stall above.

**Rule: the flag tracks whether the commit is genuinely a modeset, and is never a constant.** This
is worth stating explicitly because the natural reading of the fast path below is that setting it
always is harmless, and on this hardware it is catastrophic.

The cutoff is an internal `DCE_HWIP` / `IP_VERSION` constant rather than a marketing generation; it
was not resolved to product names and should be checked against actual supported hardware rather
than against a guess at which cards are affected.

### Adopting an existing mode is genuinely free, and the kernel will say so

`drm_atomic_helper_commit_crtc_enable()` (`drm_atomic_helper.c:1677-1678`) skips the driver's
hardware enable hook entirely when `!drm_atomic_crtc_needs_modeset()`, and `drm_mode_equal()`
(`drm_modes.c:1598-1608`) compares real timings rather than mode names, so a matching mode does not
reprogram. amdgpu respects the same signal independently at the DC layer (`amdgpu_dm.c:5402`).

Combined with the rule above this produces a better shape than *query the current mode, then decide*:
`drm_atomic_check_only()` (`drm_atomic.c:1801-1809`) demands `ALLOW_MODESET` only when
`drm_atomic_crtc_needs_modeset()` is true, so **committing without the flag is the assertion that
this is an adoption, and `-EINVAL` is the kernel answering that it is not.** Try bare, escalate on
rejection. No separate query path, no reading back hardware state, and the check belongs to the
party that actually knows.

This is what makes *adopt an existing mode* an argument to one call rather than a second path, which
[Open.md](Open.md) wanted for re-exec, the `simpledrm` handoff, and crash recovery.

*(Not verified: whether a driver's `mode_fixup()` can rewrite the stored current mode such that a
faithful round-trip fails `drm_mode_equal`. If it can, adoption would spuriously escalate to a real
modeset on that driver — the failure is a wasted modeset rather than a wrong picture, but it would
be invisible without looking for it.)*

### The two vendors are expensive in opposite places

Worth stating as its own entry, because it is the argument against a per-driver policy rather than a
fact about either driver.

| | i915 / xe | amdgpu |
| --- | --- | --- |
| Nonblocking modeset vs. unrelated flips | Decoupled — separate workqueues (`intel_display_driver.c:248-255`) | Stalls device-wide in `atomic_check` |
| Concurrent modesets | Serialized on one ordered workqueue | — |
| `VRR_ENABLED` toggle | Forced modeset every time (`intel_vrr.c:104-115`) | Cheap; does not reach the global lock |
| `ALLOW_MODESET` on a non-modeset | Harmless | Catastrophic below DCN 3.2 |

Neither vendor is the bad one. They are bad at different things, which means adapting per driver
means carrying two divergent quirk models rather than one — and the divergence is in behaviour that
is acknowledged technical debt on one side and may move on the other.

Core DRM's own locking is per-object and would support what gyro wants: `crtc->mutex` and
`plane->mutex` are per-object (`drm_crtc.h:998`, `drm_plane.h:682`), the atomic path never takes
`mode_config.mutex` at all, and everything after `drm_atomic_helper_swap_state()` runs without
modeset locks by design (`drm_atomic_helper.c:2294-2312`). The one genuine device-wide term is
`connection_mutex`, taken by any commit touching connector state — which
`drm_atomic_helper_check_modeset()` makes *every real modeset* do, via
`drm_atomic_add_affected_connectors()` (`drm_atomic_helper.c:776`).

### Hotplug probing contends with modesetting

`drm_helper_probe_single_connector_modes()` (`drm_probe_helper.c:559-698`) holds `connection_mutex`
across the entire probe including the EDID transaction, and `drm_do_probe_ddc_edid()`
(`drm_edid.c:2165-2215`) retries a failing block read five times with no DRM-imposed delay between
attempts. i915's GMBUS backend allows 50 ms per wait (`intel_gmbus.c:429-431`), so a failing read
can plausibly hold the lock for hundreds of milliseconds.

Since every real modeset also needs `connection_mutex`, **probing and modesetting contend — and
plugging in a monitor is exactly when both happen.** An ordinary flip touching no connector is
unaffected.

This is the entry most likely to be the real-world cost, ahead of the modeset stall itself, because
it compounds with it at the one moment both occur. The AUX/DDC-over-DisplayPort paths were not
measured and the 50 ms figure is i915 GMBUS-specific.

### The commit that must beat the vblank runs at a priority gyro cannot raise

A non-blocking atomic commit returns from the ioctl having queued the work rather than done it.
`intel_atomic_commit` hands the state to a worker — `INIT_WORK(&state->base.commit_work,
intel_atomic_commit_work)` at
[`intel_display.c:7889`](https://git.kernel.org/linus/4477a78374a5) and `queue_work(display->wq.flip,
&state->base.commit_work)` at line 7894 — and that worker is what waits on the in-fence, evades the
vblank, and writes the registers that arm the flip. `wq.flip` is `alloc_workqueue("i915_flip",
WQ_HIGHPRI | ...)` at [`intel_display_driver.c:254`](https://git.kernel.org/linus/4477a78374a5).

`WQ_HIGHPRI` is nice -20. It is not a real-time priority, and there is no interface that would make
it one. So **gyro's real-time guarantee ends at the ioctl boundary**: the frame thread is `SCHED_FIFO`
with `RLIMIT_RTTIME` and `mlockall`, and the last few hundred microseconds of work standing between a
composited frame and the glass are done by a `SCHED_OTHER` kworker competing with everything else on
the machine. gyro has no way to raise it, no way to pin it, and no way to ask how late it was.

The inversion is worse than it first reads. A `SCHED_FIFO` thread *always* preempts a nice -20
kworker, so on a machine short of a runnable CPU gyro can delay the very worker that arms its own
flip — a compositor's real-time priority working against the compositor.

**Measured.** On an i915 Tiger Lake laptop the commit path costs 288 to 387 microseconds on an idle
machine, and gyro budgets 500 for it
([decision 159](Decisions.md#159-the-latch-lead-is-the-modes-blanking-interval-plus-the-drivers-commit-path-and-it-belongs-to-the-output-rather-than-to-policy)).
Under a parallel kernel build on the same machine, 2.67% of commits missed the vblank they were aimed
at, in bursts, with the composite's fence signalled 1.6 to 2.6 milliseconds ahead of the deadline —
four to seven times the idle cost of the path, and time gyro had already spent waiting for. On a
compositor that is the system's only one, "the machine is compiling" is Tuesday rather than a stress
test, and what a person sees is a pointer that stutters whenever they build something.

**What would let gyro delete code.** Any of three, weakest first. A way to read how long the path
actually took, so the figure below could be measured rather than inferred from failures. A way to ask
that the commit worker inherit the committing thread's scheduling class, which is the ordinary answer
to priority inversion and which the kernel already does for mutexes. Or a commit that does the work on
the caller's thread when it asks — the caller is already `SCHED_FIFO`, already has its own deadline,
and already pays for the wait; handing the work to a lower-priority thread on its behalf is the one
part of this it did not choose.

**What gyro does meanwhile.** Budgets a fixed 500 microseconds for the path and, because that is a
figure about someone else's load, ratchets it upward on an observed miss — Open.md's *the latch lead
wants to be a ratchet*. That is a compositor inferring a scheduling latency from dropped frames,
which is the shape this entry exists to record: the workaround is the only instrument available, and
it costs a person pointer latency for as long as the machine stays busy.

## What the modesetting entries add up to for gyro's own shape

The design forced by today's kernel and the design wanted against an ideal one **agree**, which is
the finding that matters most here and the reason none of the above is a case for waiting.

Against a perfect kernel gyro would still want reconfiguration decided dispatch-side (it is policy,
not frame-loop business), issued off the frame thread, completing by event, with the frame thread
never blocking and simply not presenting an output mid-transition. That is precisely what amdgpu's
stall forces today. What a better kernel changes is how long the stall lasts and how many outputs it
touches — not one seam.

So the contract to write down, and the one that absorbs every improvement above for free:

- **`Present()` may never block.** No device-wide lock, no `ALLOW_MODESET`, no wait on another
  output's commit.
- **`Reconfigure()` may take arbitrarily long and completes by event.** Its cost is unbounded by
  contract even where a given driver happens to be fast.
- **An output mid-reconfiguration is not presenting, and the loop tolerates that** — the same
  admitted multi-frame stall
  [decision 41](Decisions.md#41-device-migration-is-exercised-on-every-boot) already takes for device
  migration, with the same guarantee: the last frame stays on glass.

## The frequency governor cannot see a deadline

### The deadline hint exists and reaches every driver but msm

`dma_fence_set_deadline()` (`dma-fence.c`) is the interface for this and it has been upstream since
6.5. Its own documentation names gyro's case as the motivating one — the hint carries *"the vblank
based deadline for page-flipping, or the start of a compositor's composition cycle"*, and the
signaling driver "may react by increasing frequency."

**The userspace path costs gyro nothing**, which is what makes the driver gap the whole of the
problem. `drm_syncobj_array_wait_timeout()` (`drm_syncobj.c:1126`) calls `dma_fence_set_deadline()`
**before** entering the wait loop, and the loop returns `-ETIME` immediately when the timeout is zero
(`drm_syncobj.c:1164-1165`). So a `DRM_IOCTL_SYNCOBJ_WAIT` carrying
`DRM_SYNCOBJ_WAIT_FLAGS_WAIT_DEADLINE` at zero timeout is a **non-blocking poll that states a
deadline** — exactly the shape
[decision 29](Decisions.md#29-outputs-are-periodic-real-time-tasks-the-test-allocates-effect-budget)'s
frame thread needs, since blocking would put the GPU's schedule on a `SCHED_FIFO` thread.

What is missing is entirely the driver end.

| Implementer | `->set_deadline` | Reaches frequency control |
| --- | --- | --- |
| msm (`msm_fence.c:171`) | yes | **yes** — hrtimer 3 ms before the deadline, then `msm_devfreq_boost(gpu, 2)` |
| `drm_sched` (`sched_fence.c:193`) | yes | no — records it, forwards to the hardware fence |
| amdgpu (`amdgpu_fence_ops`) | **no** | forwarded out of `drm_sched` into a hole |
| xe (`xe_hw_fence.c:178`) | **no** | same |
| i915 | **no plumbing at all** | and it does not use `drm_sched` either |

`dma-fence-chain`, `dma-fence-array` and `sw_sync` also implement the op, and all three only
propagate it. **So on every part gyro runs on but msm, the hint is a no-op.** msm is the one driver
that wires the hint to frequency — the `msm_fence_set_deadline` timer fires `msm_devfreq_boost` three
milliseconds before the deadline — and it is the part this tree is now being built against: the
startup probe measures the response on the machine rather than trusting this table, and the deadline
gyro states per frame is the msm frequency policy.

**The irony worth recording.** `drm_atomic_helper.c:1815` already calls `dma_fence_set_deadline()` on
plane in-fences with the next vblank time. The kernel therefore already says *this buffer is needed by
vblank* about a **client's** buffer that gyro is scanning out, and says nothing at all about gyro's own
composite — because there gyro is the waiter, and has no driver willing to listen.

**Why the utilization governors do not substitute.** Stated because it is what makes this an interface
gap rather than a tuning problem. i915's `rps_up_threshold_pct` defaults to 95 (`intel_rps.c:2040`) and
its evaluation-interval worker compares busy time against it (`intel_rps.c:1811`). A composite
occupying 95% of a refresh is a frame with no headroom left, so the threshold is unreachable by any
workload that is meeting its deadlines. And on gyro's traffic the worker does not run at all:
`intel_rps_park`'s own comment says a caller that parks and unparks faster than the worker "will not
respond to any EI and never see a change in frequency", which is a description of a compositor. A
deadline is information that no occupancy measurement contains.

**What gyro does instead.** Commands `rps_min_freq_mhz`, or the per-driver equivalent, on the systems
where a startup probe shows the deadline does not move the clock —
[decision 142](Decisions.md#142-gyro-states-the-deadline-and-commands-the-clock-only-where-the-deadline-does-not-reach-it)
carries the argument and the probe. The cost is that gyro sets a power policy on the whole machine's
behalf in order to compensate for a hint it is already sending correctly.

**What would let gyro delete it.** `->set_deadline` on the i915, xe and amdgpu hardware fences, wired
to RPS or DPM, with msm as a working reference for both halves. Nothing in gyro's userspace changes:
it already sends the hint, and the probe simply stops selecting the floor.

## There is no way to ask for a buffer several devices can all use

A scanout target has to satisfy three parties at once: the display engine that will scan it out, the
renderer that will draw into it, and whatever allocator actually produced the pages. Nothing in the
kernel takes those three constraint sets and returns a buffer, so every compositor on Linux hand-rolls
a chain of allocators and tries each one until something works — which is a negotiation performed by
trial and error, in userspace, with no way to ask a question in advance.

### dma-heaps allocate but do not negotiate

`DMA_HEAP_IOCTL_ALLOC` takes a length and a set of flags. It does not take a set of devices, a format,
a modifier, or an alignment, and it returns a dmabuf whose suitability for any particular device is
discovered by attempting the import. `dma_buf_attach` is where a constraint would be expressed and it
is the wrong end of the operation — the pages already exist by then, so an attach that fails is an
allocation that has to be thrown away and retried somewhere else.

The information exists on both sides and never meets. A DRM plane publishes `IN_FORMATS`, which is a
format and modifier table. A Vulkan device answers
`vkGetPhysicalDeviceImageFormatProperties2` with what it can render into and whether it can export.
What is missing is the intersection, and a heap that could allocate from it.

**What gyro does instead.** An ordered chain of providers behind `Seam/Allocator.h`, walked until one
succeeds:
[decision 151](Decisions.md#151-the-panel-allocates-its-own-targets-where-the-render-device-will-not)
carries the argument. The chain is the workaround, and its rungs are each a different party guessing
at what the other two will accept.

**What this costs when the guess is wrong.** It is not a failed allocation — it is a working one that
is slower than it needed to be. Handing a compositor's targets to whichever provider answered first
produced a linear scanout buffer on hardware that renders two and a half times faster into a tiled
one, which reached a person as a blur that switched itself off and reached the log as nothing;
[decision 138](Decisions.md#138-the-parent-compositor-says-what-it-can-import-the-device-says-what-it-wants-to-draw-into)
is that measurement. A negotiated allocation is the difference between a picture and a fast picture,
which is why trial and error is not good enough here.

**What would let gyro delete it.** An allocation interface that takes a set of device constraints —
the devices themselves, or the format-and-modifier tables they publish — and returns a buffer
satisfying all of them, or says that none exists. This is the unix device memory allocator problem;
it has been open for a decade, and every attempt so far has foundered on there being no common
vocabulary for a constraint. gyro does not need the general solution. It needs the two-party case:
*this DRM device will scan it out, this Vulkan device will render into it, give me the best layout
they share.*
