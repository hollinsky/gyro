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

Four invariants follow and are assumed everywhere below:

- **Animations are pure functions of time.** `Evaluate(presentationTime)`, never `Tick(delta)`.
- **Evaluation happens at predicted presentation time**, supplied by the `FrameClock` of the output
  being rendered. There is one clock per output and no global "now" — see
  [Architecture.md](Architecture.md#presentation-timing).
- **No allocation on the frame path.**
- **Authoring and evaluation are on different threads.** Everything in this document up to the point
  a spring is published happens on the dispatch thread; everything after it happens on the frame
  thread, which receives coefficients and never writes back. See
  [the publication boundary](Architecture.md#the-publication-boundary). It is the closed form that
  makes this a one-way relationship rather than a lock.

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
  what lets the compositor drop to idle cleanly. What shape that answer takes is
  [its own question](#settling-answers-with-a-wake-not-a-boolean), and "cleanly" is a hard invariant rather than a
  tidiness claim: no timer armed, the frame thread blocked indefinitely. See
  [Idle and power](Architecture.md#doing-nothing-must-cost-nothing). An integrated spring must be
  woken to discover it has nothing to do.

### Settling is a bound, not a solution

*(Written against the implementation, 2026-08-16.)* The formula above is the underdamped envelope
and it is the only one of the three that is quoted anywhere. The other two are not the same shape,
and the one that differs most is the one the catalog sits on.

**The error direction is what sets the whole design.** An answer later than the true settle costs a
few redundant composites at the tail of an animation. An answer earlier drops the compositor to idle
with something still moving, and the user watches an animation freeze. So each regime supplies an
envelope the trajectory provably sits inside, never an estimate of where it is, and each supplies one
for velocity as well — the answer is the later of the two crossings.

- **Underdamped.** Exact rather than merely conservative. With `A = √(u₀² + B²)`, position is
  bounded by `A·e^(−ζωt)` and velocity by `A·ω·e^(−ζωt)` — the velocity amplitude being
  `A·√(ζ²ω² + ω_d²)`, which collapses to `A·ω`. The only slack is the phase within the final cycle.
- **Overdamped.** The slower root bounds the sum, so `|u| ≤ (|C₁|+|C₂|)·e^(r₁t)` and
  `|u′| ≤ (|C₁r₁|+|C₂r₂|)·e^(r₁t)`. Slack is the triangle inequality alone. Note that `r₁` is the
  **slow** root under this document's sign convention, and that using the fast one is
  anti-conservative — the failure above, arrived at by a subscript.
- **Critical.** Transcendental, and the only regime that pays. `(u₀ + Ct)·e^(−ωt) < ε` has no closed
  solution, so the polynomial factor is absorbed into a slower exponential: `t·e^(−αωt) ≤ 1/(αωe)`
  for any `α ∈ (0,1)` bounds the whole expression by a pure exponential decaying at `(1−α)ω`.

`α = ½` keeps it to one logarithm and costs **up to twice the true settling time** — roughly six
tenths of a second of extra armed timers on a half-second response. That is the tail of an animation
rather than steady-state idle, so it does not touch
[the idle invariant](Architecture.md#doing-nothing-must-cost-nothing), and it is the accepted cost.
Tightening means optimizing `α` or taking a Newton step, and neither changes what is asserted.

**Two numerical hazards, both in the overdamped form.** `ζ − √(ζ²−1)` is a subtraction of
near-equals for large ζ and loses most of its digits; it is computed as `ω/(ζ + √(ζ²−1))`, which is
algebraically identical and has no cancellation. And the root span `r₁ − r₂ = 2ω√(ζ²−1)` vanishes as
ζ → 1 from above, which is why the band below is two-sided rather than a floor.

### Implementation notes

- **ζ near 1 is numerically hostile.** `ω_d → 0` in the underdamped form and `r₁ → r₂` in the
  overdamped one. Pick a threshold and fall into the critical form inside it.

  *(Annotated 2026-08-16.)* The threshold is a property of the stored precision rather than a
  constant. At `|1−ζ| = δ` the underdamped form's cancellation error grows like `ε/√δ` while the
  critical form's approximation error grows like `δ`, so balancing them gives `δ = ε^(2/3)` — with
  the exponent rounded down to keep the band exact in binary and to err toward the form that is well
  conditioned there. It is pinned by a trajectory-continuity test across both crossings rather than
  by the derivation. This is not a rare corner: "no bounce" is ζ ≥ 1, so the catalog is expected to
  sit on or beside the band.
- **Springs never arrive.** Settling needs both a position and a velocity threshold, or the system
  damages forever at sub-pixel amplitude. Geometric thresholds are in output pixels so they scale
  correctly with DPI — and on a node spanning outputs of different densities, in pixels of the
  **finest** grid it intersects, since settling against the coarse one leaves the fine one crawling.
  Angular and scale channels have no pixels of their own and convert through the node's bounding
  radius: a rotation residual of ε displaces a corner by roughly ε·r. Settling is also the moment a
  node's geometry snaps to the device grid, so the threshold has to be small enough that the snap is
  invisible — see [Architecture.md](Architecture.md#quantization-belongs-to-the-output).

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

Four floats, no allocation, exact velocity preservation. Retargeting subsumes the `speed = 0` plus
`timeOffset` model for *interruption*, which is what that model is usually reached for.

Exactness survives a channel whose coordinates move with its target — rotation is the one — but not
by this arithmetic. See [transforms](#transforms), where the subtraction above stops being the right
operation and the velocity is transported rather than copied.

It does not subsume gesture *driving*, and this paragraph claimed that it did until 2026-08-16 —
that the target could simply follow the finger, with the gesture's velocity handed off on release. A
spring whose target follows a finger still lags the finger, by `2v/ω`, and the claim survived
because nothing had yet asked what world state the moving target was supposed to be reading. See
[interactive transitions](#interactive-transitions), which keeps the retargeting above for what it
is good at and parameterizes the driven case by progress rather than by time. The error is worth
keeping because the conclusion it reached — no timeline model — was right, and the argument for it
was not.

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
	Motion::Interactive   the residual under a finger — pushback, snap, the settle on release
```

`Motion::Interactive` read *tracks input* until 2026-08-17, which was the moving-target model
[decision 65](Decisions.md#65-interactive-transitions-are-driven-by-a-progress-parameter-not-by-a-moving-target)
rejects. Nothing tracks a finger through a spring; the tracking is
[driven](#progress-is-an-ordinary-animatable), and this entry is the motion of what is left over.

Five to seven entries. Growth past that is cohesion leaking.

**The escape hatch is a call, not a sixth name.** It exists, and it should look like one: greppable
in a single command and obvious in review. But it cannot be an enumerator, because an enumerator is
resolved by looking it up in the table above — and the whole content of an escape hatch is that it
is *not* in the table. Giving it a row would make it a sixth motion that configuration retunes,
which is the opposite of what it is for. So it is a direct construction of spring parameters from a
response and a damping ratio, reachable only from gyro's own code and, unlike a name, not reachable
from a bundle at all.

For [the shell](Architecture.md#the-shell) the enforcement is stronger than a rule, because the
vocabulary is a protocol: a client naming a transition has no way to express a damping ratio at all.
The escape hatch is for gyro's own code, which is the only code that has one.

### Bundles are the real unit

Per-channel springs (see [transforms](#transforms) below) are what make motion feel designed rather
than mechanical — position wants to be snappier than scale, and opacity generally should not bounce
when geometry does. They are also where incohesion multiplies fastest if call sites tune channels
individually.

So the catalog's unit is not a spring but a **transition**: `Transition::WindowOpen` defines its
position, scale, and opacity springs *designed together*. The shell names the transition and never
the channels.

A bundle says four things and is not permitted a fifth:

- **What each channel does.** Three dispositions and not two — absent, immediate, or sprung under a
  named motion. The middle one is what makes reduced motion expressible: a slide-in that becomes a
  fade-in has a translation channel that is neither animated nor untouched, because the window has
  to *be* at its model position immediately, and that is a different statement from *this transition
  has no opinion about position*.
- **What it becomes when movement is not wanted**, which is
  [its own section](#reduced-motion-is-a-policy-not-a-parameter) below.
- **Where its scale and rotation are fixed** — a *policy* rather than a coordinate, because the
  coordinate is not knowable here. The anchor is in the node's own space and depends on its extent,
  and for the case that matters most it depends on something that does not exist until the gesture
  happens: a menu grows from where it was opened. So a bundle names the anchor's *source* and the
  scene resolves it.
- **How a gesture drives it**, if one can — [the mapping](#the-mapping-belongs-to-the-catalog)
  below. A bundle with no travel is one nobody can drive, so a transition that is only ever watched
  says nothing about gestures.

The last two sit beside the channels rather than inside them, and that placement is what makes
[reduced motion keeping the driven half](#interactive-transitions) structural: the substitution's
entire domain is the channel table, so it cannot reach the anchor or the mapping even by mistake.

**Four things said is not four fields, and the difference is the test rather than the count.** All
four above are motion design, which is what the refusal protects: a fifth *statement* is a fifth way
for two transitions to disagree about how the system moves. A bundle also carries whether the
transition needs [an opacity group](Decisions.md#60-group-opacity-requires-flattening-per-node-alpha-is-not-a-group-fade),
and that is not one of them — it declares what the transition costs to draw, and it is keyed by
transition only because the transition is what knows whether a subtree is underneath it. So the
question to ask of anything wanting to join the struct is which of the two it is. A member that
changes how the motion reads is refused by decision 13; a member that changes what the renderer must
allocate is the other kind, and has to win decision 60's argument instead. The group flag is
currently the only one, and [which entries set it](Open.md) is open.

**Staggering is not among them, and its absence is a decision rather than an omission.** Every
transition worth writing today is unstaggered, so a field defaulting to *no stagger* would change
nothing about any of them. What the design does need is already settled: the delay and its cap are
global modifiers, because an amount is tuning and must move the system together, while the *order*
items go in — list order, or outward from a focal point — is per transition, because it reads
completely differently and is motion design. Mechanically it costs nothing whenever it arrives, a
staggered entity being one whose spring carries an origin of `t₀ + i·delay`.

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
- **Never parse on the frame thread.** Parse on [the helper thread](Architecture.md#threads),
  validate, publish atomically, adopt at the next commit boundary.
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

**It is a closed vocabulary of forms, for the same reason the motions are.** A bundle names one of
three, and the three are what survived collapsing the obvious five:

```
	Fade          movement is replaced by a cross-fade — the ordinary case
	Cut           the change simply arrives — too structural to fade at all
	Unchanged     this was already pure opacity and is its own reduced form
```

Fade-in, fade-out, and cross-fade are not three forms. They differ only in what opacity is heading
*for*, and that is model state the differ already holds — an enter targets one, an exit targets zero
— so the direction is not the catalog's to know. What is left is the fade; the transition with
nothing to cross-fade, since there is no cross-fade of a *size* and a reduced resize simply arrives;
and the transition that was already opacity and would otherwise have to duplicate itself.

Two rules hold across all three. **Participation is inherited and disposition is not** — a reduced
form may snap a channel its transition already touched, and may never seize one it did not, because
forcing a rotation to land immediately would cancel whatever that rotation was doing for somebody
else. Opacity under `Fade` is the single deliberate exception, and it is the one the policy exists
for: a window that slides in with no opacity channel at all still has to fade in when its movement
is removed. And **reduced motion removes movement, not rhythm** — a fade standing in for a workspace
switch takes about as long as the movement it replaced, or the accessibility path runs at a tempo
the rest of the system does not share.

There is deliberately no per-bundle reduced *table*. It would be the rejected alternative arriving
one entry at a time, and it would put two independently authored fades on the path that gets the
least review, free to drift apart. A transition that needs something these three cannot say wants a
fourth **named** form — because a bespoke reduced shape that recurs is a form, and one that does not
is almost certainly a mistake.

It also loads one mechanism far harder than the default path does. If movement is replaced by
fading, then every reduced transition covering a *subtree* is a group fade, so the flattening in
[decision 60](Decisions.md#60-group-opacity-requires-flattening-per-node-alpha-is-not-a-group-fade)
is exercised constantly there and occasionally elsewhere. Not literally every transition — a `Cut`
fades nothing, and a single window fading out is one object rather than a group — but the
distinction does not soften the conclusion, because what the reduced path converts to fades is
precisely the transitions that move whole stacks of windows around. Reduced motion is not a cheaper
path and should never be costed as one.

## Properties

### Animatable

No keypaths, no boxing, no string lookup. A typed wrapper holding the model value plus inline spring
state — a handful of floats, evaluable without allocation.

```cpp
template <SpringValue T>
class Animatable
{
public:
	using Scalar = SpringScalar<T>;

	const T&       Model() const;
	T              Presentation(Instant t) const;       // pure; closed form
	SpringState<T> PresentationState(Instant t) const;  // (x, v); what a gesture takeover reads
	Wake           NextWake(Instant t, SettleThresholds<Scalar>) const;  // Core/Wake.h; see below
	bool           IsAtRest() const;                    // derived from the coefficients, not flagged

	void AnimateTo(const T& target, SpringParameters<Scalar> motion, Instant t0);
	void AnimateTo(const T& target, SpringParameters<Scalar> motion, Instant t0, const T& v₀);
	void SetImmediate(const T& value);
	void Settle();                                      // onto the model value, at rest, now
};
```

### Settling answers with a wake, not a boolean

*(Written against the implementation, 2026-08-16.)* The third method was `bool IsSettled(Instant t)`
until this section replaced it. The boolean is the ergonomic answer, and it is wrong in a way that
does not surface as a bug — it surfaces years later as a feature that cannot be added.

[Doing nothing must cost nothing](Architecture.md#doing-nothing-must-cost-nothing) is a **fold**:
every animating channel, every pending timeout, every retiring entity contributes an answer, and the
schedule is what they reduce to. With a boolean the reduction is an OR, and an OR has terms for
*resting* and *moving* and none for *something will happen later*. So it forecloses every motion that
is neither settled nor settling — an indeterminate spinner, a marquee for text that does not fit, a
breathing focus ring, and the blinking cursor the [recovery console](Architecture.md#the-pre-vulkan-console)
owes as a shipping requirement. Each is cheap on the frame path, since an undamped oscillator is O(1)
and exact at a predicted time like any other spring; none of them has a way into the idle ladder.
Adding the first one means changing the fold, admission control, and the wake path together.

**An instant alone is not the answer either**, which is the half that is easy to miss. The obvious
repair is `std::optional<Instant>` with `nullopt` for never, turning the any-reduce into a min-reduce
at identical code volume. It fails on the commonest case in the system: a spring in flight has no
next interesting instant to name, because every instant between now and settling is one. It must
answer *now*, which the reduction cannot tell from a one-shot that is merely overdue — and that is
exactly the distinction under which a standing commitment gets priced by
[admission control](Architecture.md#admission-control) and by
[the VRR servo](Architecture.md#vrr-as-a-scheduling-degree-of-freedom). The excluded motions are not
one kind either. A blink is discrete and wants a wake per edge with nothing drawn between them; a
marquee is continuous and wants every frame for as long as it lives. An optional fits the first and
has to spell the second as a lie.

`Wake` is therefore three cases. It lives in `Core` rather than in `Animation` because `Console` and
the idle ladder contribute to the same fold and neither may depend on the animation system — a
placement `CheckLayering.cmake` enforces rather than merely recommends.

| Case | Means | Contributed by |
| --- | --- | --- |
| `Settled` | nothing further, ever | a settled spring; an output with no pending timeout |
| `Timed(when)` | one frame, at `when` | a cursor blink; the [gesture-stop republish](#it-crosses-the-boundary-as-coefficients-like-everything-else); a dim or blank timeout |
| `Continuous(when, interval)` | a frame at `when`, and another every `interval`, without end | a spring in flight; a marquee; a throb |

`interval` zero is every frame the output offers, which is what a spring asks for. A non-zero one is
what makes a periodic motion **priced** rather than absorbed: a throb authored at 30 Hz is a quarter
of the composites on a 144 Hz panel and is a rate the VRR servo can hold, and neither statement is
expressible if the answer is a bare instant.

`Sooner` reduces two contributions to the more demanding, with `Settled` as its identity. That it is
a **commutative monoid** is the property worth having rather than an incidental one: associativity is
what lets the fold be partitioned per output — a blinking cursor on one panel must not wake the
other, and a scene-wide reduce would reintroduce the coupling per-output damage removed — and what
lets it be cached per subtree and recomputed along the dirty path, instead of swept over every node
each time anything moves.

*(Annotated 2026-08-23.)* **The partition is a property of the monoid and not yet a property of the
implementation**, and the distinction is worth stating where the claim is made rather than leaving the
paragraph above reading as a description.
[Decision 122](Decisions.md#122-the-wake-fold-is-scene-wide-and-replicated-per-output-a-settled-channel-retires-where-it-is-published)
folds the scene once and replicates the answer to every output, because the cursor in the example is a
contributor attached to an **output** and a spring is attached to a **node** — and splitting a node's
contribution means knowing which outputs it reaches while it moves, which is a screen-space question
the dispatch side cannot answer without redoing the frame walk. The carrier stays per output, so an
output-attached contributor partitions exactly when the first one arrives; what does not partition
today is the animating scene, and what that costs is in [Open.md](Open.md).

**The saturation the solver already chose composes with this in the safe direction.** `SettlesAt`
returns an instant no frame reaches for a spring that never settles, so the comparison against it is
false forever and an undamped oscillator contributes `Continuous` — correct, and visible to whatever
prices it. Under the optional, the same value reads as *never interesting again*: one keystroke from
dropping the compositor to idle with the one motion in the design that genuinely never stops. That is
[the error direction](#settling-is-a-bound-not-a-solution)'s failure arriving from the scheduler side
rather than from a subscript, and it is the argument for the third case rather than for the second.

Two obligations sit on contributors rather than on the fold, because nothing in the reduction can
check them. A timed instant is **strictly after** the instant it was computed for, or the scheduler
spins at full rate on a wake it has already served. And a periodic contributor computes its next edge
from its own origin, `t₀ + n·period`, **never from its last wake** — a wake is served at the
following vblank, and adding to a rounded value accumulates exactly the drift closed form exists to
avoid.

### Model versus presentation

Every animatable property carries two values: the **model** value the shell set, and the
**presentation** value drawn this frame.

- Layout and window-management policy read model.
- Rendering reads presentation.
- Hit-testing reads **model** by default, so clicking a window in flight hits it where it logically
  *is* rather than where it happens to be drawn. Presentation hit-testing is available where the
  opposite is wanted.

Presentation is also where the per-output work lands. One model value yields a different
presentation value on each output — evaluated at that output's predicted presentation time, and
rounded to that output's device grid once it settles — which is why neither result is ever written
back. See [Architecture.md](Architecture.md#quantization-belongs-to-the-output).

Omitting this split is cheap on day one and expensive at month six, because retrofitting it means
auditing every read of every property.

### Transforms

Matrices are never interpolated — lerping them shears and collapses. Transforms are stored
decomposed into translation, rotation, and scale about an explicit **anchor point**, each channel
driven by its own spring, and composed to a matrix at render time. Per-channel springs are what let
position be snappier than scale, which is a real expressive win for window transitions. The anchor
point is what makes a window grow out of the corner it was summoned from rather than out of its own
middle, and that is most of what makes a transition read as intentional rather than as a scale.

The transform is three-dimensional, with node-local perspective and no camera. That buys card flips,
perspective overviews, and depth cues for very little on the render path. What it is not allowed to
do is intersect — composition stays strict tree order with no depth buffer, because the alternative
is order-dependent transparency and every surface here has alpha. See
[Architecture.md](Architecture.md#transforms-are-3d-the-scene-is-not).

Strict tree order costs one more thing, and it is the opacity channel's. **A bundle that fades a
subtree fades it as one object, not node by node**, or the windows inside it show through each other
at every value between the endpoints. The transition declares the group and gyro flattens it; see
[decision 60](Decisions.md#60-group-opacity-requires-flattening-per-node-alpha-is-not-a-group-fade).

**Rotation is a quaternion, sprung in the log map.** Euler triples gimbal and take non-shortest
paths, and both present as a window travelling a visibly strange route, which is the failure mode
hardest to attribute to its cause. In the log map rotation stays one channel with one spring, which
is what [decision 17](Decisions.md#17-transforms-are-decomposed-into-trs-with-per-channel-springs)
meant by per-channel and what three Euler springs would quietly undo.

**The log map is full-angle**, so the channel is an axis times the rotation angle in radians and its
magnitude *is* that angle. This has to be said rather than left to be inferred, because roughly half
the literature means the other one — the textbook quaternion logarithm carries the half-angle — and
the two differ by a factor of two everywhere they meet. Two things here already depend silently on
the full-angle reading: the settling note below, where a residual of ε displaces a corner by ε·r
with no factor to remember, and any threshold in the catalog expressed in degrees. Canonicalizing
the double cover bounds the channel at π, which is what makes the shortest path structural rather
than a comparison somebody has to remember to write.

**One spring, not three, is a claim about settling rather than about the path.** *(Written against
the implementation, 2026-08-16.)* Three scalar springs sharing one motion produce an identical
trajectory, because the equation is decoupled and a shared `(ω, ζ, t₀)` gives every component the
same solution. What they cannot share is a settling criterion: three per-component thresholds is
axis-dependent, so the same rotation would finish at different moments depending on where its axis
pointed, and a channel could be two-thirds settled — which means nothing, and which is the moment
[the device-grid snap](Architecture.md#quantization-belongs-to-the-output) happens at. The threshold
above is on the magnitude, so the solver is generic over the channel's value type rather than
instantiated once per component.

**A chart-valued channel transports its velocity rather than copying it.** The log map is anchored
at the target, so retargeting a rotation is a change of chart and not a subtraction: the deviation
is recomputed against the new target, and the stored velocity — a tangent vector at the old base
point — is carried across through the right-Jacobian of the exponential map. Copying it instead is
wrong to *first* order in the change of deviation rather than second. Position stays continuous
either way, so the symptom is not a jump but an interruption that leaves along the wrong course — at
a large retarget, visibly turning about a different axis rather than a slightly wrong one.

**The linear term is not a bound in either direction**, and a figure quoted from it was wrong here
until 2026-08-16. `½[u′−u]×v` describes the error only while the change of deviation is small; past
about a radian the higher-order terms are the same size and may cancel it, so at `|Δu| = 1.83` the
linear estimate is three times the true error. The honest statement is that the correction is O(1)
in the change of deviation with a magnitude the linear term does not predict — which matters for one
reason: anything tempted to skip the transport when `|Δu|` looks small enough cannot use the linear
form to decide, and there is no cheaper test than doing it.

This is why [Retargeting](#retargeting)'s exactness claim holds for rotation rather than nearly
holding, and it is the reason the transport is one named call: the naive copy is the ergonomic path,
and [priority 2](#priorities) says that where the ergonomic path and the correct path diverge, the
design is at fault rather than the author. The cost is two Jacobians and a matrix-vector product on
the dispatch thread at event rate, so nothing about it is a trade.

### Storage

Nodes own their `Animatable<T>` members, which is how the code that mutates them wants to read and
write them. The per-frame evaluation pass instead walks a flat array of *active* springs, so it
touches only what is moving and stays cache-friendly.

The rule that makes the flat array safe: **it is derived, never maintained.** Registration happens
only inside `Animatable<T>`'s own methods, when a spring becomes active or settles. If that is the
sole path that can change activity state, the two representations cannot disagree, because no other
code is able to make them.

The two are not even on the same thread. Nodes live on the dispatch thread with the rest of the
world; the flat array is what the publisher **emits** into the snapshot the frame thread walks, so
it is a serialization rather than a mirror — contiguous by construction, and with no second live
copy that could drift. Settling is likewise a dispatch-side event: the settle time is analytic, so
nothing has to observe an evaluation to know a spring has finished, which is what keeps entity
destruction, [retirement](#lifetime), and [atlas](#where-snapshots-live) release off the frame
thread entirely. The [wake fold](#settling-answers-with-a-wake-not-a-boolean) runs on the same side
and for the same reason, and its result crosses in the snapshot header — one per output, beside the
coefficients — so the frame thread reads a schedule rather than deriving one.

*(Written against the implementation, 2026-08-23.)* **Emitting the array and retiring a settled spring
are one pass, and they have to be.** The serializer already asks each channel whether it is at rest in
order to decide whether a coefficient crosses; *has it settled* is the same question asked with the
thresholds in hand, so it is asked in the same branch — and the two states that would otherwise be
reachable are both fatal to the invariant above. A coefficient published with a `Settled` wake is a
scene that never idles; a wake published for a coefficient that was dropped is a scene that stops
mid-motion.
[Decision 122](Decisions.md#122-the-wake-fold-is-scene-wide-and-replicated-per-output-a-settled-channel-retires-where-it-is-published)
also names the piece this section had left implicit: a free-running animation commits **once**, so
something has to make dispatch look again, and *analytic settle* is the instant it looks at. The
serializer answers it as a `Timed` wake of its own — dispatch's, never published — and nothing arms it
yet, because there is no dispatch event loop to arm it with.

## Declarative commits

Nothing animates properties. Callers mutate world state inside a commit, and the system derives the
transitions:

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

The form above is gyro's own, and [the shell](Architecture.md#the-shell) is a client rather than a
caller — so most commits arrive over a protocol and are resolved on the dispatch thread. The model
is unchanged by that, deliberately: a commit is a transaction against world state whether it was
written as a lambda or demarshalled from a socket, and everything below holds either way.

Three things fall out:

- **Shared `t₀`.** Every transition in a commit starts at the same timestamp. Eight windows in a
  workspace switch move as one gesture rather than eight nearly-simultaneous ones. Hand-written
  calls desync the moment a commit straddles a frame boundary. Because `t₀` is the *event's*
  timestamp rather than the moment of handling, the sharing survives leaving the process: two shell
  clients reacting to one input event name the same origin, so a panel animating out and thumbnails
  animating in compose into a single gesture across a process boundary with no coordination between
  them.
- **Staggering.** "Each subsequent item starts 20 ms later" is only expressible against a shared
  origin, so it is a catalog-level policy that applies consistently or not at all — and it splits
  once it gets there, into a global delay and a per-transition order. See
  [bundles](#bundles-are-the-real-unit), where it is deferred rather than built.
- **Uniform interruption.** A commit landing mid-flight retargets every affected spring from its
  current `(x, v)`.

### Two phases, and no comparison pass

There is no world snapshot and nothing is diffed against anything. A commit resolves in two phases,
and the boundary between them is **whether a change's inputs are complete at the moment it is
written**.

**Phase one is every property change, resolved at the write.** Setting a property *is* retargeting
it: the model value is the spring's target, so there is no staged value and nothing to reconcile
later. The resolver has what it needs on the spot — the current presentation `(x, v)` from the
spring, the new value from the assignment, and the bundle and `t₀` the commit was opened with. It
costs four floats and no allocation, and writing one property twice inside a commit is exactly
idempotent, because the second retarget samples the first's spring at its own origin and reads back
the same `(x, v)`. Order does not matter and the last target wins.

**Phase two is everything whose inputs are the rest of the commit.** Enter and exit come from entity
creation and destruction, and neither can be resolved where it is written: a key-`K` exit becomes a
[move](#matched-geometry) rather than an exit if a key-`K` enter arrives later in the same commit, a
removal is a [resurrection](#lifetime) if the entity was retiring, an exit
[reserves atlas space](#when-the-blit-happens) that a move would not have wanted, and a
[derived](#bundles-are-the-real-unit) quantity like an anchor coordinate depends on an extent that
may still change. So phase one retargets channels and resolves nothing derived; layout, matching,
lifetime, and atlas reservation all run at close.

Neither phase walks the world. Phase one's cost is proportional to what was written and phase two's
to what was created, destroyed, or derived from those — never to world size. See
[decision 89](Decisions.md#89-a-commit-resolves-in-two-phases-a-change-becomes-motion-where-its-inputs-are-complete).

### Timing and rates

**`t₀` is the input event's timestamp, not the moment the event was handled.** This only means
anything because the event's clock and the presentation clock are the same clock — libinput's
timestamps and a page flip's are comparable by
[decision 57](Decisions.md#57-one-timebase-clock_monotonic-converted-at-ingest-and-nowhere-else)
rather than by luck, and the one place they are not is injected input from another machine, which is
converted at its ingest. Input events carry
timestamps, and animations are evaluated at predicted presentation time — so the first rendered
frame shows the animation *already in progress* by exactly the input-to-photon latency. Using `now`
instead silently adds a frame of lag to every gesture. This is most of what makes a system feel like
it is tracking a finger rather than following it.

The same property is what makes a *late* commit harmless. A commit delayed past a record point — by
a slow buffer import on the dispatch thread, or by a shell client that was not scheduled promptly —
still carries the event's timestamp, so when it is finally evaluated it renders already in progress
by exactly the elapsed amount rather than starting from zero. Lateness costs the first frame or two
of the animation, never its shape, and it never affects anything already in flight, which continues
to evaluate correctly from the coefficients already published. The degradation mode of the whole
authoring side is therefore input latency and not judder.

Commits happen at event rate; evaluation happens at frame rate. The declarative machinery therefore
never executes inside the frame budget. Gesture tracking is the case that looks like it needs pacing,
and it does not, twice over. [Decision 65](Decisions.md#65-interactive-transitions-are-driven-by-a-progress-parameter-not-by-a-moving-target)
removes the traffic rather than throttling it — a gesture is one commit and then a single progress
tuple republished, so a 1000 Hz device resolves one transition and not a thousand. And pacing would
be wrong even where the traffic is real: two commits inside one frame carry two `t₀`, so resolving
them together would discard the motion between two input events that genuinely happened. Retargeting
at the write keeps it, in the only place it needs to survive — the `(x, v)` the next retarget samples.

### Escape hatch

Genuinely event-driven one-shots — a ripple at a click point, a shake on failed authentication — are
not state changes and are not forced through the differ. They use a direct imperative API.

> **Open.** The exact shape of that API, and where the boundary sits, is undecided. It now has a
> second half: whichever shape it takes has to be expressible to
> [the shell](Architecture.md#the-shell) as well, without becoming the per-event channel that the
> declare-don't-drive rule exists to refuse.

## Interactive transitions

A transition the user is *making* is a different thing from one they are watching, and the
difference is not a matter of degree. Hold a three-finger swipe halfway and the overview stays
halfway, for as long as the fingers rest there. Reverse the swipe and it runs backwards under them.
Let go and it carries the speed it was let go at into whichever end it lands on. None of that is
interruption, which [retargeting](#retargeting) already answers, and none of it is reachable by
moving a spring's target.

### Why a moving target is not enough

The construction that changes nothing above is to let the gesture move the targets —
`Motion::Interactive`, retargeted per input event. It fails three ways, and only the first is about
feel.

- **A spring tracking a moving target lags it, by a computable amount.** Critically damped, the
  steady-state error against a target moving at `v` is `2v/ω`. At a 0.3 s response and a 1000 px/s
  scrub that is ~95 px of trail, and it inverts through zero on every reversal, which is perceived
  as the picture swimming rather than as latency. Direct manipulation requires the pixels to be
  *attached* to the fingers, and a spring is by construction attached to nothing.
- **There is no world state for the target to be.** [Declarative commits](#declarative-commits)
  derive transitions from a change in state, and the state here is `Workspace.Visible`, a boolean.
  *The overview is 47% open* is not a value that model holds, and adding one per transition is a
  timeline with extra steps and a worse name.
- **It would move the shape of the interpolation into the shell.** Something has to map finger
  displacement onto per-channel values every event, and if that something is the shell then the
  shell is authoring motion — [decision
  51](Decisions.md#51-the-shell-is-a-per-session-client-gyro-owns-mechanism)'s per-event loop and
  [the catalog](#the-motion-catalog)'s entire reason for existing, lost in one move. This objection
  stands even if the first two are answered.

### Progress is an ordinary animatable

An interactive transition is a bundle whose channels are functions of one scalar `p ∈ [0, 1]`, and
`p` is an `Animatable<float>` like any other — in its *free* regime; the *driven* regime is a
distinct coefficient record with its own snapshot array, not a spring, by [decision
72](Decisions.md#72-the-driven-regime-is-a-distinct-record-the-snapshots-arrays-stay-homogeneous). It
has two regimes and no third:

- **Driven.** `p` follows the gesture, at input rate, on the dispatch thread.
- **Free.** `p` is sprung toward an end — `0`, `1`, or a detent — with `v₀` the gesture's velocity
  in progress units and `t₀` the release event's timestamp, as everywhere else.

Release moves `p` from the first regime to the second. A cancelled gesture does the same thing with
the origin as its target, which is why cancellation needs no separate path.

What falls out is the whole of the behaviour above:

- **Held is settled.** Resting fingers produce no input events, so `p` stops changing and nothing in
  the scene is active. A held gesture arms no timer and draws no frames — [doing nothing costs
  nothing](Architecture.md#doing-nothing-must-cost-nothing) reaching a state no other system treats
  as idle.
- **Scrubbing is exact**, because nothing is converging on anything.
- **Taking over a release is one scalar.** A gesture beginning while `p` springs home reads `p`'s
  current `(x, v)` and re-enters the driven regime: the same retarget as everywhere else, performed
  once rather than per channel, and exact in both directions.
- **Reduced motion keeps the driven half.** [Replacing movement with
  fading](#reduced-motion-is-a-policy-not-a-parameter) is right for a transition being watched and
  wrong for one being made — a gesture that does not track is not reduced, it is broken. So reduced
  motion substitutes the bundle's channels and leaves the parameterization alone, which is only
  statable because they are separate things.
- **A driven entity is never a retiring one.** Swipe-to-dismiss does not remove anything from model
  state until the gesture resolves, so no exit is generated and no [snapshot](#where-snapshots-live)
  is reserved while a finger is down. That is what keeps the atlas admitting bounded-lifetime
  occupants only; an exit whose duration is under the user's control would be exactly the immortal
  occupant that rule refuses.

### It crosses the boundary as coefficients, like everything else

Publishing `p` as a number would be publishing an evaluated value, which [the publication
boundary](Architecture.md#the-publication-boundary) forbids, for a reason that binds here rather
than merely applying: the frame thread has to produce two correct answers for two outputs in one
iteration. A bare `p` is the same number on both, so a 144 Hz panel is pinned to the staleness of
the last input event instead of to its own presentation time, and the defect [Presentation
timing](Architecture.md#presentation-timing) exists to prevent is reintroduced at the boundary —
invisible until a second monitor is attached, which is the signature of that whole class.

So the driven regime publishes coefficients too — `(p₀, v₀, t₀, horizon)` — evaluated per output as

```
	p(T) = p₀ + v₀ · clamp(T − t₀, 0, horizon)
```

which is the first-order case of the shape a spring already has: an initial value, an initial
velocity, an origin, and a closed form read at the output's own predicted presentation time. The two
regimes become one mechanism carrying two coefficient sets, rather than a scalar path running beside
a spring path.

`(T − t₀)` is exactly the input-to-photon gap that
[`t₀` as the event timestamp](#timing-and-rates) makes measurable, so this is not prediction into
the future — it is undoing a latency the system already knows the size of, per output, because the
two outputs present at different instants. It is the same promise as *a gesture starts where your
finger is*, applied to the case where breaking it is cheapest.

**The `horizon` does two jobs and both are load-bearing.** It bounds the extrapolation, which is
what keeps a reversal from overshooting — a fitted velocity is good for about as long as a finger
holds one, and past that it is invention. And it is what makes *held is settled* true: past
`t₀ + horizon` the expression is constant, so `p` reports settled and the compositor drops to idle
with the gesture still in progress.

That bound has one honest cost. A finger that stops abruptly leaves a last event carrying full
velocity, so `p` runs on by up to `v₀ · horizon` and holds there. The correction is that **the
absence of an update is information**: a device reporting nothing for longer than its own interval
is a finger that is not moving, so dispatch republishes with `v₀ = 0` at the horizon. That is one
timer, once per gesture-stop, on the side that is allowed to allocate — the same analytic-settle
machinery [storage](#storage) already describes, rather than anything new.

### The mapping belongs to the catalog

Displacement to `p` is not arithmetic, it is motion design: how far the gesture must travel to mean
*all the way*, how it rubber-bands past the ends, where the detents are. It sits in the catalog with
the springs and for the same reason, and the shell names the binding without being able to express
the curve.

Drag is the degenerate case rather than a separate mechanism — the mapping is identity, `p` carries
a position rather than a fraction, and the driven regime is the whole of the tracking.
`Motion::Interactive` is then what it always should have been: the motion of the *residual* — the
pushback when a constraint is hit, the pull into a snap target — and never the motion of the finger
itself, which no spring should be interposed in.

### What the input side owes it

Three obligations, all cheap, and each of them the usual way this feels wrong.

- **Recognition costs no displacement.** When a recognizer completes past its threshold, `p` is
  computed from the gesture's origin rather than from the instant of recognition. Otherwise the
  transition either jumps at recognition or silently discards the travel that triggered it. The
  shape is the same as `t₀` being the event's timestamp and not the handler's.
- **Release velocity is a fit over a short window, not the last delta.** Fingers decelerate as they
  lift, so the final event is frequently near zero, and handing that off produces a flick that dies
  on release. libinput reports no momentum for swipe gestures — the hardware tells macOS when
  fingers leave and the system synthesizes the rest — so this estimator is gyro's, and its quality
  is felt directly.
- **Cancellation is an outcome, not an error.** libinput cancels gestures, and the free regime
  already expresses the result.

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
  `VK_ERROR_OUT_OF_DEVICE_MEMORY` in a process this expensive to restart cannot be handled at all.

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

It is also a large minification, which makes it the first thing in the system to need a mip chain —
built in linear light, off the import path. Without one it aliases, and it aliases worst while the
geometry is moving.

## Hierarchical time

A `TimeScale` per subtree, composed down the tree and resolved once per frame during traversal —
not per property.

This buys subtree slow-motion for debugging animations, which is wanted constantly, without the
per-frame cost of full `CAMediaTiming`. The rest of that model (`beginTime`, `timeOffset`, `speed`,
`repeatCount`, `autoreverses`, `fillMode`) is largely redundant once springs handle interruption by
retargeting.

## Open questions

- **Color interpolation space.** Oklab is proposed over sRGB; it matters most when cross-fading
  blurred backdrops, which gyro does constantly. Not yet decided.
- **The imperative escape hatch** for event-driven one-shots — shape and boundary undecided, in
  process and over the protocol both.
- **The node vocabulary the shell composes with**, which is the same design problem as the motion
  catalog and the material set and should be solved with them — a node, the material dressing it,
  and the transition revealing it are one thing seen three ways. See
  [Architecture.md](Architecture.md#the-scene-vocabulary-is-closed-composition-is-not).
- **Configuration format.** A hand-rolled parser for flat key-value float configuration keeps the
  dependency count at zero and is less code than wiring up a TOML library, consistent with the
  hand-rolled XML parse in the protocol generator. Not yet decided.
- **Settling thresholds** for non-geometric properties. The geometric case is settled — output
  pixels of the finest grid a node intersects — but opacity, blur radius, and corner radius have no
  output pixel to be expressed in, and the policy for them is unresolved. *(Numbers landed
  2026-08-23 by
  [decision 122](Decisions.md#122-the-wake-fold-is-scene-wide-and-replicated-per-output-a-settled-channel-retires-where-it-is-published);
  opacity's is a representability argument rather than a perceptual one, so this question is narrowed
  rather than answered. See [Open.md](Open.md).)* A progress parameter is
  the one non-geometric channel that escapes the problem rather than adding to it: its
  [mapping](#the-mapping-belongs-to-the-catalog) carries a distance, so a threshold on `p` converts
  to output pixels of travel like anything geometric.
- **Which periodic motions the catalog admits, and at what rate.**
  [The wake](#settling-answers-with-a-wake-not-a-boolean) gives periodic motion a way into the idle
  ladder; it does not decide that any belongs in [the catalog](#the-motion-catalog), whose five
  entries all converge. The recovery console's cursor needs no catalog entry, since it is
  [not a Vulkan path](Architecture.md#the-pre-vulkan-console) and its blink is a `Timed` contribution
  and nothing else. What is unresolved is whether the shell may name a pulsing or indeterminate
  motion at all, and if so what `interval` it is authored with — a rate is a permission to draw less,
  and picking one is motion design rather than scheduling, which puts it with the catalog and the
  vocabularies below.
- **Detents, and the flick threshold underneath them.** [The mapping](#the-mapping-belongs-to-the-catalog)
  owns where a release may land besides the two ends, and no transition has an interior stop today.
  What keeps this from being a field nobody sets is that a detent is not inert data: choosing
  between one and an end at release needs a threshold in progress per second, which interacts with
  the same velocity estimator whose quality is felt directly. Wants deciding alongside a transition
  that needs it.
- **The lead horizon for a driven gesture.** How far `p` may be carried toward predicted
  presentation time before the estimate stops being a correction for known latency and starts being
  a guess. One output period is the obvious first answer, since that is the gap being undone, and
  the failure it trades against — overshoot held after an abrupt stop — is bounded by `v₀ · horizon`
  and is a real artefact rather than a theoretical one. Wants measuring against a touchpad, with
  reversal as the case that decides it.
- **Snapshot atlas capacity.** The multiple of the output render target is deliberately not guessed.
  It wants a count of legitimate simultaneous retirements to size it and per-output high-water and
  eviction instrumentation to confirm it; an eviction outside a stress test means the number is
  wrong. See [Open.md](Open.md).
