# Animation

The animation system is built first, because animation quality *is* frame pacing — the two are not
separate subsystems that meet later. Starting here means the hardest scheduling question gets
attacked while the codebase is still small enough to restructure around the answer.

Companion documents: [Architecture.md](Architecture.md) for the platform seam and frame clock,
[Decisions.md](Decisions.md) for the decision log.

## Priorities

1. **Cohesion for the end user comes first.** Everything moves as one system. A window opening, a
   menu appearing, a workspace switching, and a notification arriving should feel like the same
   hand made them.
2. **The right way to write an animation is the easy way.** Where the ergonomic path and the correct
   path diverge, that is a design bug, not a discipline problem.
3. **Implementation complexity is acceptable** where it buys 1 or 2. It is not acceptable where it
   only buys development speed.

Three invariants follow and are assumed everywhere below:

- **Animations are pure functions of time.** `Evaluate(presentationTime)`, never `Tick(delta)`.
- **Evaluation happens at predicted presentation time**, supplied by the `FrameClock` of the output
  being rendered. There is one clock per output and no global "now" — see
  [Architecture.md](Architecture.md#presentation-timing).
- **No allocation on the frame path.**

## Springs

### Closed form, not integrated

The usual spring implementation integrates numerically — semi-implicit Euler or RK4, stepping state
forward by Δt each frame. Every game engine does this, and it is incompatible with all three
invariants above. It is stateful and timestep-dependent, so a dropped frame yields accumulated error
rather than the correct value; it cannot be evaluated at an arbitrary predicted time; and it is not
reproducible, which forecloses golden-image testing.

A damped harmonic oscillator has an exact analytic solution. With `u = x − target`:

```
	u″ + 2ζω·u′ + ω²·u = 0
```

Underdamped (ζ < 1), with `ω_d = ω·√(1 − ζ²)`:

```
	u(t) = e^(−ζωt)·[ u₀·cos(ω_d·t) + ((v₀ + ζω·u₀) / ω_d)·sin(ω_d·t) ]
```

Critically damped (ζ = 1):

```
	u(t) = (u₀ + (v₀ + ω·u₀)·t)·e^(−ωt)
```

Overdamped (ζ > 1), with roots `r₁,₂ = −ω·(ζ ∓ √(ζ² − 1))`:

```
	C₁ = (v₀ − r₂·u₀) / (r₁ − r₂)
	C₂ = (r₁·u₀ − v₀) / (r₁ − r₂)
	u(t) = C₁·e^(r₁·t) + C₂·e^(r₂·t)
```

Velocity comes from the same expressions in closed form. `Evaluate` is O(1), exact, stateless, and
reproducible. Everything downstream depends on this:

- A missed frame produces the *correct* value on the next frame, not a lurch.
- Evaluating at predicted presentation time is exact rather than an extrapolation.
- **Mixed-refresh multi-monitor works at all.** The same scene can be evaluated at two different
  presentation times in one iteration, both exact, so a window dragged between a 144 Hz panel and a
  60 Hz projector is correct on both simultaneously. A `Tick(delta)` system has one mutable timeline
  and one delta per iteration and therefore cannot express this — which is why every compositor that
  has one drops the fast panel to the slow panel's rate. This is the largest payoff of closed form
  and it is easy to miss when reading the list above as being about dropped frames.
- Headless tests with a fake clock are bit-reproducible, so golden images work.
- **Settling time is analytic.** For the underdamped envelope, `t = ln(A/ε) / (ζω)`. The scheduler
  can therefore answer "will anything still be animating at time T" without simulating, which is
  what lets the compositor drop to idle cleanly.

### Implementation notes

- **ζ near 1 is numerically hostile.** `ω_d → 0` in the underdamped form and `r₁ → r₂` in the
  overdamped one. Pick a threshold and fall into the critical form inside it.
- **Springs never arrive.** Settling needs both a position and a velocity threshold, or the system
  damages forever at sub-pixel amplitude. Thresholds should be expressed in output pixels where the
  property is geometric, so they scale correctly with DPI.

### Parameterization

Author in `(response, dampingRatio)`; store `(ω, ζ)`.

```
	ω = 2π / response
```

`response` is the natural period — how quickly it moves. `dampingRatio` is how much it bounces. The
two are legible and, crucially, independent. Physical `(stiffness, damping, mass)` is what
`CASpringAnimation` exposes and is miserable to tune because all three interact.

### Retargeting

Interruption is the whole game, and closed form makes it trivial and exact:

```
	read current (x, v) from the closed form at now
	u₀ = x − newTarget
	v₀ = v
	t₀ = now
```

Four floats, no allocation, exact velocity preservation. This is also why gyro does not need
timeline scrubbing for interactive transitions: the target follows the finger and the gesture's
velocity is handed off on release. Retargeting subsumes the `speed = 0` plus `timeOffset` model.

## The motion catalog

Cohesion is won or lost here, not in the solver. It fails when a window-open uses
`response 0.42, damping 0.83` and a menu-appear uses `0.45, 0.80` — both defensible in isolation,
and the system feels subtly incoherent forever after. Nobody catches it in review because both
diffs look reasonable.

**Call sites must not be able to name spring parameters.** Not by convention — enforced. Numeric
spring construction lives in the catalog and nowhere else. Everywhere else names a semantic intent
from a closed vocabulary:

```
	Motion::Standard      the workhorse — most state transitions
	Motion::Snappy        direct-manipulation feedback, minimal overshoot
	Motion::Gentle        ambient and background changes, no bounce
	Motion::Expressive    large or attention-drawing changes
	Motion::Interactive   tracks input, never overshoots under the finger
```

Five to seven entries. Growth past that is cohesion leaking. `Motion::Custom` exists as an escape
hatch and should look like one: greppable in a single command, and obvious in review.

### Bundles are the real unit

Per-channel springs (see [transforms](#transforms) below) are what make motion feel designed rather
than mechanical — position wants to be snappier than scale, and opacity generally should not bounce
when geometry does. They are also where incohesion multiplies fastest if call sites tune channels
individually.

So the catalog's unit is not a spring but a **transition**: `Transition::WindowOpen` defines its
position, scale, and opacity springs *designed together*. Shell code names the transition and never
the channels.

### Runtime configuration

The catalog is loaded from configuration in all builds, with the `constexpr` catalog as the fallback
that configuration overlays.

The constraint that keeps this from undermining cohesion is **granularity**: configuration exposes
the named vocabulary and a small set of global modifiers. It does *not* expose transitions. Retuning
`Motion::Standard` moves everything built on Standard together, so the system stays coherent under
any tuning. Exposing `Transition::WindowOpen` independently would destroy that immediately.

Practical requirements:

- **Validation.** A system-layer compositor cannot fail to start because someone fat-fingered a
  damping ratio. Clamp to sane ranges, warn on nonsense, fall back per key rather than wholesale.
- **Never parse on the frame thread.** Parse on a helper thread, validate, publish atomically, adopt
  at the next commit boundary.
- **Hot reload is clean by construction.** Because springs are closed form, applying new parameters
  mid-flight is just a retarget from the current `(x, v)` — live retuning produces no visible
  discontinuity even while things are moving.

### Reduced motion is a policy, not a parameter

This is routinely implemented wrong. Reduced motion does not mean faster or less bouncy springs. It
means **replacing movement with cross-fades** and dropping parallax and scale entirely — a window
that slides in should fade in instead.

That is a different transition, not a retuned one. Reduced motion is therefore a first-class
dimension of a bundle's definition from the start. Retrofitting it means revisiting every transition
in the catalog.

## Properties

### Animatable

No keypaths, no boxing, no string lookup. A typed wrapper holding the model value plus inline spring
state — a handful of floats, evaluable without allocation.

```cpp
template <Interpolatable T>
class Animatable
{
public:
	const T& Model() const;
	T        Presentation(Nanoseconds t) const;   // pure; closed form
	bool     IsSettled(Nanoseconds t) const;

	void AnimateTo(const T& target, MotionRef motion, Nanoseconds t0);
	void SetImmediate(const T& value);
};
```

### Model versus presentation

Every animatable property carries two values: the **model** value the shell set, and the
**presentation** value drawn this frame.

- Layout and window-management policy read model.
- Rendering reads presentation.
- Hit-testing reads **model** by default, so clicking a window in flight hits it where it logically
  *is* rather than where it happens to be drawn. Presentation hit-testing is available where the
  opposite is wanted.

Omitting this split is cheap on day one and expensive at month six, because retrofitting it means
auditing every read of every property.

### Transforms

Matrices are never interpolated — lerping them shears and collapses. Transforms are stored
decomposed into translation, rotation, and scale, **each driven by its own spring**, and composed to
a matrix at render time. Per-channel springs are what let position be snappier than scale, which is
a real expressive win for window transitions.

### Storage

Nodes own their `Animatable<T>` members, which is how shell code wants to read and write them. The
per-frame evaluation pass instead walks a flat array of *active* springs, so it touches only what is
moving and stays cache-friendly.

The rule that makes the mirror safe: **it is derived, never maintained.** Registration into the
active array happens only inside `Animatable<T>`'s own methods, when a spring becomes active or
settles. If that is the sole path that can change activity state, the two representations cannot
desync, because no other code is able to disagree with them.

## Declarative commits

Shell code does not animate properties. It mutates world state inside a commit, and the system
derives the transitions:

```cpp
Compositor.Commit(Transition::WorkspaceSwitch, [&](World& w)
{
	w.Workspace(1).Visible    = false;
	w.Workspace(2).Visible    = true;
	w.Window(focused).Focused = true;
});
```

**You cannot forget to animate a property, because you never animate properties.** Adding a new
animatable property means adding it to the state and to the catalog, after which it animates
correctly everywhere with no call-site changes. That retroactivity is what makes the machinery worth
its cost: cohesion becomes structural rather than maintained by discipline.

Three things fall out:

- **Shared `t₀`.** Every transition in a commit starts at the same timestamp. Eight windows in a
  workspace switch move as one gesture rather than eight nearly-simultaneous ones. Hand-written
  calls desync the moment a commit straddles a frame boundary.
- **Staggering.** "Each subsequent item starts 20 ms later" is only expressible against a shared
  origin, so it is a catalog-level policy that applies consistently or not at all.
- **Uniform interruption.** A commit landing mid-flight retargets every affected spring from its
  current `(x, v)`.

### Dirty tracking, not snapshot diffing

There is no world snapshot and no comparison pass. `SetModel` records the change as it happens, so a
commit closes by walking a dirty set whose size is proportional to what changed, not to world size.
Enter and exit come from entity creation and destruction recorded within the same commit. The
transition resolver has everything it needs: current presentation `(x, v)` from the spring, and the
new model value from the property.

### Timing and rates

**`t₀` is the input event's timestamp, not the moment the event was handled.** Input events carry
timestamps, and animations are evaluated at predicted presentation time — so the first rendered
frame shows the animation *already in progress* by exactly the input-to-photon latency. Using `now`
instead silently adds a frame of lag to every gesture. This is most of what makes a system feel like
it is tracking a finger rather than following it.

Commits happen at event rate; evaluation happens at frame rate. The declarative machinery therefore
never executes inside the frame budget. One case needs care: gesture tracking commits at input rate,
and a 1000 Hz mouse would otherwise diff a thousand times a second. Mark dirty on commit and resolve
at most once per frame.

### Escape hatch

Genuinely event-driven one-shots — a ripple at a click point, a shake on failed authentication — are
not state changes and are not forced through the differ. They use a direct imperative API.

> **Open.** The exact shape of that API, and where the boundary sits, is undecided.

## Identity

Identity has three jobs, and most designs conflate the first two and never build the third:

1. **Diffing** — is this the same entity as last commit? Transition, or enter, or exit.
2. **Lifetime** — an entity with an exit animation must outlive its removal from model state.
3. **Matching** — should two *different* entities transition into one another?

### Generational handles

```cpp
struct EntityId
{
	uint32_t Index;        // slot in the entity arrays
	uint32_t Generation;   // bumped on free; stale ids compare unequal
};
```

Allocated from a slot map at creation for everything animatable, protocol-backed or not — windows,
popups, layer surfaces, workspaces, overview thumbnails, switcher tiles, drag placeholders, effect
nodes. Reuse-safe, trivially comparable and hashable, and an array index, so evaluation is a linear
walk rather than a pointer chase.

Two load-bearing properties:

- **Independent of tree position.** A window moving between workspaces, going fullscreen, or
  becoming a tab in a stack keeps its id. Position is a property that animates; it is not part of
  what the thing *is*. Anything path-derived breaks all three cases.
- **Runtime only.** Session restore across compositor restarts is a different problem needing app-id
  and a persistence key. It must not contaminate this.

### Why not protocol identity

Keying windows off their `xdg_toplevel` resource fails on job 2: **when a client destroys its
toplevel the protocol object is gone, but the window still has to animate out.** Identity tied to
protocol lifetime cannot express an entity that outlives its protocol object. It also covers none of
the compositor-invented entities, which need the same machinery and must interoperate with windows
for matching.

Pointer identity is worse — addresses are reused after free, and the failure mode is a new entity
silently inheriting a dead one's animation state.

## Lifetime

- Model state removes the entity; the differ generates an exit transition.
- The entity moves to a **retiring** set — still evaluated and rendered, but invisible to layout,
  focus, and hit-testing.
- On settle it is destroyed and the slot's generation bumps.

**Re-adding while retiring resurrects, it does not re-create.** Dismiss a menu and immediately
reopen it, or hide and re-show a workspace: if that produced a new entity, the exit and enter
animations would fight and produce a visible hitch. Resurrection keeps the id live so the spring
retargets from its current `(x, v)` and smoothly reverses. Fast repeated actions feeling right
rather than janky is entirely this behaviour, and it requires retiring entities to stay in the
identity map rather than being freed on removal.

Snapshot pressure is the one thing that can take resurrection away. An entity evicted under pressure
([exit pixels](#exit-pixels)) is settled and destroyed, so reopening it enters fresh instead of
reversing — the cost is a hitch on exactly the fast repeated action this behaviour exists to serve.
It is bounded to the pressure case, and the converse works in our favour: resurrection returns a
rectangle to the atlas, so repeated open-close relieves the pressure it creates rather than
compounding it.

## Exit pixels

Animating a closed window out requires its last frame, and the client may be gone. gyro **snapshots
at full resolution** into a compositor-owned texture.

What makes this worth designing carefully is not the window closing. It is the menu. The common exit
animation is a popup or a tooltip destroyed inside a long-lived application, dozens of times a
minute, and it carries real information: the menu collapsing back toward the control it came from is
what tells the user which control they hit. So the bar is not "exit animations look good" — it is
that **an exit animation is never conditional on resource state.** One that animates most of the
time and pops the rest reads as a stutter, and the user blames the application that just closed.
Everything below follows from that.

### Why not hold the client's buffer

Holding a reference is genuinely zero-copy, and the memory does survive client death, since
importing a dmabuf takes a reference through the kernel's refcounting. Held **for the length of the
exit**, it was rejected on three grounds:

- **Surface destruction is not client destruction.** `wl_buffer.release` is how a client learns it
  may reuse a buffer, and most rotate two or three. Holding the last one for a 300 ms exit stalls a
  long-lived application's rotation, and the resulting hitch looks like the application's fault.
- **Heaviest sampling on a layout we did not choose.** Normal compositing samples a client buffer
  once per frame. An exit animation scales it and, with blur, samples it repeatedly across multiple
  passes. A buffer allocated with a compression modifier was optimized for the client's rendering or
  for scanout, not for a blur kernel's access pattern. A snapshot pays one blit and then gets every
  subsequent frame in exactly the layout the effect path wants — roughly 43 frames at 144 Hz over
  300 ms. Zero-copy is cheap at close time and potentially expensive on every frame after.
- **Pinning is client-controlled.** A 4K RGBA buffer is ~33 MB, and a client controls both how many
  it closes and how fast. Bounding it requires a fallback path, which is the snapshot path — at
  which point the zero-copy argument has eaten itself.

The first two are arguments about *duration*, and that distinction is load-bearing below: at one
frame rather than 43, the rotation stall is ~7 ms and the foreign layout is sampled exactly once,
which is the sampling the snapshot was going to do regardless. Only the third holds at every scale,
and a reservation answers it.

A fourth ground was recorded here until 2026-08-16 and is **withdrawn**: that holding `wl_shm`
buffers widens the window in which a client can truncate the backing fd and fault us. Vulkan cannot
sample a client's shm mapping, so every shm commit is already uploaded into a compositor-owned image
in order to be composited at all — on the dispatch thread, which is where
[Architecture.md](Architecture.md#the-publication-boundary) puts mapping and resource creation
alike. At close time those pixels are already ours and there is no client buffer to hold.
Snapshotting adds no shm SIGBUS exposure at all. The error is worth keeping because it inverted the
two buffer types: shm is the easy case, and dmabuf — where the pixels genuinely exist nowhere else —
is the hard one.

### Where snapshots live

A **per-output atlas, reserved at output configuration and never grown.** A snapshot is a rectangle
from a shelf packer plus a blit into a subregion, so nothing on the frame path creates a Vulkan
object. Three properties make the atlas the right shape rather than a generic allocator:

- **It is the layout the effect path wants.** Exit animations blur, and blurring the whole retiring
  set out of one image is one descriptor and one set of barriers rather than N.
- **Fragmentation needs no compaction**, because every occupant dies within one exit transition. The
  pool drains to empty on any idle moment, which is what makes shelf packing sufficient.
- **Failure is a packing miss, not a device OOM.** A packing miss can be handled deliberately. A
  `VK_ERROR_OUT_OF_DEVICE_MEMORY` in a process with no restart boundary cannot be handled at all.

Capacity is denominated in **output render-target equivalents, not bytes** — a byte count is right
only on the machine it was tuned on, while a multiple of the output's own render target scales with
resolution and monitor count and is expressed in a cost the frame budget already reasons about.

The atlas admits **bounded-lifetime occupants only**. A last-good-frame held for an unresponsive
client is the same shape with an unbounded lifetime, and one immortal occupant destroys the drain
property for everything else; such a consumer needs its own storage.

### When the blit happens

Not necessarily at close time. A full-screen snapshot is ~33 MB read and ~33 MB written, a
meaningful fraction of the frame budget on a laptop iGPU — landing on the frame where the user just
clicked something and the application behind it is at its busiest. An unbounded number of blits is
an unbounded term in the per-output cost the [frame clock](Architecture.md#presentation-timing) is
defending.

So the rectangle is reserved when the retirement is observed, and the blit is recorded in whichever
of the next frames has room. Two facts make late capture safe:

- **A destroyed surface cannot commit again**, so the pixels do not change while we wait.
- **Frame one of an exit can sample the client buffer directly**, exactly as a live surface does, so
  the snapshot only has to serve frames two onward.

Which means the deferral holds a client buffer for a frame or two — the narrow form of the rejected
alternative above, at a duration where two of its three objections do not apply and the third is
bounded by the reservation. Policy does not branch on buffer type: dmabuf holds the client's import,
shm holds the upload the compositor already owns. It does cost one piece of machinery at the
[publication boundary](Architecture.md#the-publication-boundary) — a per-buffer hold released when
the blit lands, since the consumed-sequence watermark cannot express it.

### When there is no room

**Older exits finish early**, hard-settling so their rectangles free. In a storm of closing popups
nobody is watching the twenty-ninth, so "old things finished fast" is imperceptible, while the
entity under the cursor still animates. Slots are attributed to the connection that caused them, and
pressure is resolved against the offender's own slots first, so a client destroying surfaces in a
loop degrades its own exits and nobody else's.

The alternative — downscaling under pressure — preserves the animation count at the cost of quality,
which sounds like the better trade until its landing point is checked: the cliff falls on whichever
window happens to close during the pressure, including the one being watched. Eviction degrades the
exits nobody is looking at.

The same eviction runs on device loss. Unplugging a Thunderbolt GPU gives no notice and no
opportunity to read anything back, so the reclamation path has to work with no GPU at all —
hard-settling springs and freeing rectangles is entirely CPU-side, so it does. Planned migration
then uses the same path rather than a second one that only ever runs during a hardware fault.
Snapshots are consequently the one GPU resource gyro permits itself to lose; see
[Architecture.md](Architecture.md#device-migration).

## Matched geometry

Identity answers "same entity". Matching answers "these are different entities, and the transition
between them should be continuous."

An entity may declare a **match key**. If one carrying key K exits in the same commit as one
carrying K enters, the differ emits a single move transition rather than an exit plus an enter.

This is what produces a window growing out of its overview thumbnail; an alt-tab tile and an
overview thumbnail staying continuous when modes change mid-gesture; a maximizing window flowing
from its restored geometry. Without it, all of those are cross-fades — and cross-fades are what
"not cohesive" looks like in practice.

Built in the first cut. The risk is designing against imagined requirements, so it is developed
against one real transition rather than a synthetic test: **window to overview thumbnail**, which
exercises mismatched aspect ratios, cross-tree parenting, and interruption mid-gesture at once.

## Hierarchical time

A `TimeScale` per subtree, composed down the tree and resolved once per frame during traversal —
not per property.

This buys subtree slow-motion for debugging animations, which is wanted constantly, without the
per-frame cost of full `CAMediaTiming`. The rest of that model (`beginTime`, `timeOffset`, `speed`,
`repeatCount`, `autoreverses`, `fillMode`) is largely redundant once springs handle interruption by
retargeting.

## Open questions

- **Colour interpolation space.** Oklab is proposed over sRGB; it matters most when cross-fading
  blurred backdrops, which gyro does constantly. Not yet decided.
- **The imperative escape hatch** for event-driven one-shots — shape and boundary undecided.
- **Configuration format.** A hand-rolled parser for flat key-value float configuration keeps the
  dependency count at zero and is less code than wiring up a TOML library, consistent with the
  hand-rolled XML parse in the protocol generator. Not yet decided.
- **Settling thresholds** expressed in output pixels for geometric properties — the exact policy for
  non-geometric properties (opacity, blur radius, corner radius) is unresolved.
- **Snapshot atlas capacity.** The multiple of the output render target is deliberately not guessed.
  It wants a count of legitimate simultaneous retirements to size it and per-output high-water and
  eviction instrumentation to confirm it; an eviction outside a stress test means the number is
  wrong. See [Decisions.md](Decisions.md#open).
