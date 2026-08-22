# Open Items

Carried forward, decisions that need to be made, roughly in the order they will bite. Answering one
of these produces an entry in [Decisions.md](Decisions.md), which is where the reasoning goes; this
file holds only what has not been settled yet, so an item leaves it by being decided rather than by
being crossed off.

Three entries have left this list by being answered rather than deferred, and all three left the
same way — someone read the source the entry rested on. *libwayland's abort reachability* became
[decision 2](Decisions.md#2-gyro-owns-the-protocol-seam-libwayland-implements-the-server-codec),
*`IPresenter` has no mode-setting path* became
[decision 73](Decisions.md#73-the-frame-thread-initiates-reconfiguration-and-never-performs-it), and
*what signal says a client controls the refresh rate* became
[decision 76](Decisions.md#76-cadence-authority-follows-predictability-not-foreground).

That is the triage rule for everything below. **An entry that names the source its argument rests on
can be retired by an afternoon of reading; an entry that names none needs a frame loop, a panel, or
a user in front of it.** The third also showed that an entry can be well-formed and still
unanswerable — the signal it asked for does not exist — in which case the rule upstream of it is
what to suspect. [AGENTS.md](../AGENTS.md#how-decisions-get-made) carries these as working rules.

- **The two client-reachable `wl_abort` sites**, which
  [decision 2](Decisions.md#2-gyro-owns-the-protocol-seam-libwayland-implements-the-server-codec)
  closes with a wrapper that refuses to publish a resource id before its implementation is set, and
  with a generated dispatch table that cannot have a hole. Neither is written, both are small, and
  the failure they prevent is a machine-wide abort at a moment a client picks. The wrapper is the
  sort of thing that is obvious now and invisible once there are two hundred `wl_resource_create`
  calls.
- **Re-reading the abort inventory on major libwayland bumps.** The count went 6 → 18 across 1.23 to
  1.24, and the reading behind decision 2 is a snapshot of `1.26.0-9-ged0b9f1` rather than a
  property of the library. What wants watching specifically is a new site reachable from client
  input with no gyro bug in front of it, since that is the first of the three conditions decision 2
  names for reopening the in-tree server half.
- **Tone mapping and gamut mapping policy**, in both directions, deferred by decision 47 as
  additive. The SDR-on-HDR direction is where every shipping system has gone wrong, and the rule is
  that SDR white maps to a reference — BT.2408 says 203 nits — or to a stated user preference, and
  never to display peak. HDR-on-SDR needs a curve chosen rather than inherited. Neither is urgent;
  both are easy to get subtly wrong and hard to notice afterwards.
- **Blur order against tone mapping.** Blurring in linear and tone mapping the result is correct and
  costs a full-resolution map after the chain; tone mapping first and blurring afterwards is cheaper
  and temporally steadier and is wrong. A real number attached to a real artefact, so it wants
  measuring rather than arguing — the same standing as the virtual-output color question below.
- **Blur across color-state boundaries.** `Material::Glass` samples a backdrop that may hold an HDR
  video window beside an SDR text editor, and physically correct linear blur bleeds a 1000-nit
  highlight through the glass into the region over the SDR window. Correct, and startling. There is
  no obviously right answer, which is what makes it a decision rather than an implementation detail.
- **Mip generation's place in `C`.** Decision 56 makes minification the common case rather than the
  exception, so a mip chain is rebuilt for animating client content on the frames it changes — real
  bandwidth, roughly a third of the surface again, on the budget decision 29 defends and which does
  not know about it yet. It also wants a rule for when the chain is worth building at all, since a
  surface minified by 1.05 does not need one and a surface minified by 2 does. A measurement, not an
  argument.
- **Promotion quality for a minified surface.** The item above makes a mip chain, built in linear
  light, the answer to minification. A hardware plane scaler is not that chain and cannot read it —
  it is fixed function and its quality is the vendor's. So promoting a minified surface to a plane
  is a visible sharpness change, in a system whose fourth promise is that the picture does not
  change when the machine changes how it is drawing it — the argument in [direct scanout is
  conditional](Architecture.md#direct-scanout-is-conditional) arriving once more, in the sampling
  domain, and not obviously answerable the same way, because "composite instead" gives up the entire
  offload on precisely the outputs
  [decision 56](Decisions.md#56-clients-render-at-the-ceiling-and-gyro-downscales) makes ordinary.
  Sizing bounds the stakes: an sc7180 tablet has one scaling pipe against three flat ones, so
  *promote the minified layer* is a budget of one however the quality question lands. Wants a
  golden-image comparison against the mip path, not an argument.
- **Per-output characterisation.** The inverse of the display's measured behaviour belongs at the
  very end of the pipeline and preferably in KMS hardware. EDID routinely misdescribes the panel, so
  a user-supplied profile has to be possible, which implies a configuration surface and somewhere to
  put an ICC file — on a system-layer process where "the user" is not yet resolved at the time the
  first output lights up.
- **Publication granularity across outputs.** Decision 45's boundary is wait-free, so the frame
  thread could acquire client state once per iteration or once per output. Architecture.md takes
  once per iteration, on the grounds that a window straddling two outputs would otherwise show two
  different client frames in one iteration — visible on exactly the configuration decision 28 exists
  to serve. Per-output acquisition is strictly fresher, and the trade has been reasoned rather than
  measured.
- **Publication pacing**, which is the producer-side twin of the item above and is stated at the
  foot of
  [decision 50](Decisions.md#50-the-world-is-authored-on-the-dispatch-thread-the-snapshot-carries-coefficients).
  Eager per resolved commit serializes the scene once per input event; pacing to the fastest
  output's period spends up to a period of gesture latency, which is the worst currency available. A
  copy-on-write arena makes eager cost proportional to the dirty set and is the likely answer, but
  the first cut should be the naive one and the question should be settled by measurement. Decision
  65 removes the
  case that made this sharpest — a driven gesture republishes one coefficient tuple rather than
  re-serializing a scene per input event — which lowers the stakes without settling the question,
  since ordinary commits at device rate remain.
- **Whether the publication nudge should be conditional.**
  [Decision 83](Decisions.md#83-dispatchs-publication-is-an-event-source) has dispatch write its
  descriptor on every publication, which is correct and which wakes the frame thread ahead of its
  timer whenever both threads are busy. The saving is to write only when the frame thread last
  reported a `Settled` wake, which the return channel can carry and `FrameReport` has a spare word
  for. What makes it a question rather than a patch is the lost wakeup it opens — dispatch may read
  *not idle*, publish, and decline to signal, all inside the window before the frame thread posts its
  report and sleeps — so the conditional form is correct only with the step re-checking the ring after
  posting and before it returns `Never()`. Settle it by measuring the waste on a busy system rather
  than before.
- **The shell's scene vocabulary.** *(Narrowed 2026-08-22.)* Decision 51 commits to a closed set of
  node kinds — surface reference, snapshot reference, solid, effect layer — and that list is a sketch
  rather than a design. It is the same problem as the material vocabulary below and wants solving
  with it and with the motion catalog, since a node, the material that dresses it, and the transition
  that reveals it are one design problem seen three ways. The test named in decision 51 is the
  constraint: a shell must not be able to produce motion that does not match the catalog.

  **The sketch is known wrong rather than merely undesigned, which raises the priority.** Decision 88
  added a node kind the list does not contain — a node referencing another subtree — so anything
  built against the four would be built against a set already superseded once. Decision 91 is why
  this now blocks something concrete: `World/Node.h` exists and carries what the frame side's walk
  needs, and the fields that wait on this entry are absent from it. A node kind, a material, a
  texture, a color state, and the encoding of decision 88's reference all arrive together when this
  is settled, and `SnapshotVersion` is what absorbs them.
- **Which transitions declare an opacity group.** *(Narrowed 2026-08-17.)* Decision 60 settles that
  a group fade flattens and what it costs; it does not settle which bundles ask for one, and "every
  fade" is the wrong answer — paying for an offscreen on a single window fading out would put a
  render target on the most frequent transition in the system. Three of the seed entries now answer
  it locally and the reasoning is what generalizes rather than the verdicts: a menu dismissal
  declares one because a submenu can be open at dismissal and never at appearance, so the group is
  declared where a subtree *can* exist; a workspace switch declares one for its reduced path alone,
  since cross-fading a stack of windows needs flattening and sliding it does not; a window close
  declares none, because [exit pixels](Animation.md#exit-pixels) snapshot into a single texture that
  is already flat. What is left open is the general rule, and it still wants deciding with the scene
  vocabulary above, since whether a subtree is a group is a property of the transition and of the
  node kinds underneath it at once. The third case is the one to watch: it is a mechanism claim
  rather than a judgement, so an exit that ever runs off the live subtree takes the flag with it.
- **What the shell declares for continuous manipulation.** Decision 51 keeps drag, resize, and swipe
  inside gyro on the strength of the shell declaring constraints ahead of time — minimum and maximum
  sizes, snap targets, tiling gravity, and whatever else turns out to be needed. Decision 65 answers
  the half most likely to break the rule, since a swipe now declares a transition binding and
  nothing per event; what is left is narrower rather than closed. The constraint set for drag and
  resize is still not enumerated, and enumerating it is what decides whether the rule holds or
  whether the first awkward case adds a per-event request and quietly undoes it. The gesture
  vocabulary itself — which gestures exist, and what each binds to — belongs with the scene and
  material vocabularies above and for the same reason: a gesture, the transition it drives, and the
  nodes it moves are one design problem seen three ways.
- **Color format for virtual outputs.** Encoders want NV12 or P010, not RGBA. The agent can convert
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
- **Which materials are pointwise.** Decision 33 now carries a classification it does not populate,
  and the answer sizes decision 62's variant lattice. It wants doing *with* the vocabulary rather
  than after it: a set designed without the question in mind produces a gathering material where a
  pointwise one would have done, and each of those is a pass boundary that can never be fused away.
- **Where `Material` lives.** *(Answered 2026-08-22; what remains is the move.)*
  [Decision 87](Decisions.md#87-a-type-both-halves-of-the-world-name-lives-below-both-waists-not-in-seam)
  moved `TextureId` and `ColorState` out of `Seam` because the world authors them and the frame side
  consumes them, and could not place the third field of a `DrawItem` that has the same shape.
  [Decision 91](Decisions.md#91-the-worlds-vocabulary-is-a-module-of-its-own-below-both-waists)
  creates the fourth base module that entry said would resolve it, on the strength of the node record
  rather than of `Material`. So `Material` goes to `World` and `Seam` gains a `World` edge, and the
  two halves of this question come apart: *where* is settled, *what is in the set* is the entry above.
  It moves with the vocabulary rather than before it, because a move made while the set is one
  enumerator is a move nothing can check.
- **The intermediate format for unfused effect passes.** Decision 62 requires the fused and unfused
  paths to agree below the perceptual threshold, and the whole of the difference is rounding at pass
  boundaries — registers at full precision against whatever the intermediate stores. `fp16` is
  probably sufficient and 8-bit certainly is not, which is decision 47's precision argument on a
  smaller surface; but *probably sufficient* is the standing that argument rejects. A measurement
  with a tolerance attached, and the tolerance is the harder half.
- **The damage verifier's waste threshold.** Decision 63 asserts that declared damage is not
  "grossly larger" than what changed, and grossly is not a number. Too tight and every gathering
  material fails on content that happens to be static; too loose and the silent direction stays
  silent, which is the whole reason the verifier exists. Probably a ratio with a floor area, and it
  should be calibrated against a real scene rather than chosen.
- **Damage region complexity cap.** The rectangle count past which decision 63 collapses a region to
  its enclosing bound. One of the few numbers in this list that is cheap to obtain, and it wants
  obtaining before the region implementation is tuned around a guess.
- **What may instantiate what.** Decision 64 puts the capture permission on the primitive and does
  not say how the check is expressed. It is the same question as the System tier's listener below —
  a trust level per connection deciding an operation — and the two want answering together, because
  a thumbnail of another session and an output moved between sessions are one bypass at two sizes.
- **Instance count in the budget and in the atlas.** Decision 64 makes instances an axis of decision
  29's allocation and of decision 46's sizing, and overview entry is the worst case for both at once
  — a subtree instantiated per window, in a frame that may also be retiring surfaces. It belongs in
  the same derivation as the atlas multiple above rather than in one of its own.
- **The floor composite must not flicker, and the ladder as written says it will.** Decision 34's
  rung 3 is *the material is not rendered — an opaque or simply tinted fill*, and decision 35's
  record-time check picks the floor tier **per frame**. So a one-frame excursion is a blurred
  backdrop snapping flat and back inside a period. Decision 34 already names that failure and rejects
  it — *"a tier recomputed per frame makes effects flicker at the margin, which is a worse artefact
  than the dropped frame it avoids"* — and answers it with stickiness that governs the commit-time
  quality tier and says nothing about the record-time floor. Two mechanisms on one visual axis with
  opposite policies, and the correlation runs the wrong way: the record-time check fires most during
  animation, which is exactly when decision 34 says never to change tier. It also outruns what
  [Experience.md](Experience.md#how-it-degrades) promises: *effects give way before frames do*, and
  what is spent first is *"a small amount of quality in something that was about to be blurred
  anyway"* — which is rung 1. A per-frame excursion to rung 3 is not a small amount, and the same
  document's account of lateness is that work *"arrives late — it does not arrive wrong"*.

  The leading answer is that **the floor composite is defined to be visually continuous rather than
  absent** — a floored frame reuses the previous frame's blur result instead of dropping to a fill,
  which is stale by a frame, costs almost nothing, and reads as a held backdrop rather than a missing
  one. That makes this a constraint on what the floor composite *contains* rather than on when it is
  chosen, which is why it sits beside the entry below rather than inside decision 35.

  Three alternatives, kept because the first is not obviously right. Give the floor decision 34's own
  stickiness, so it is a step rather than a blink, at the cost of several mediocre frames instead of
  one bad one. Forbid flooring during an animation outright, which is decision 34's rule applied
  honestly and which leaves nothing but the frame drop in the case the floor tier exists for. Or take
  decision 34 at its word that the dropped frame is the *lesser* artefact, and remove the second
  branch of the record-time check for one-frame transients entirely.

  **This one will not yield to an afternoon of reading.** It is a perceptual question, and the third
  category the preamble names: it wants a blur, a scene that animates, and somebody watching. What it
  specifically must not be settled on is the strength of the argument above, since the entire claim is
  about what an eye notices. Nothing is blocked meanwhile — there are no effects yet, so today's floor
  composite draws the same nothing more cheaply — and it becomes real the day a blur exists.
- **`C_min` as a number.** Decision 35 makes the floor composite's cost the bound on recoverable
  overrun, which makes it a target rather than a measurement. What that target should be is
  undecided; the visibility half of *what the floor composite may contain* is the entry above, and
  the two constrain each other, since a floor that holds the previous blur is a different number from
  one that draws a flat fill.
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
  [Decision 32](Decisions.md#32-a-surfaces-frame-cadence-follows-its-fastest-output) makes
  multi-output surfaces first class, while decision 46's storage, capacity, attribution, and
  eviction locality are all per output. A window retiring while it straddles the seam is either in
  both
  atlases, doubling its cost
  on the configuration decision 28 exists to serve, or in one and sampled by the other, which breaks
  the argument that pressure is resolved against the slots that caused it. Surfaced by the same
  reading that produced decision 47, and it is a sizing question as much as a correctness one.
  Decision 52 shifts the trade rather than settling it: an atlas is at its output's density, so "in
  both" is the horn that is *correct* about density on both, and "in one, sampled by the other"
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
  budget. **The floor composite belongs in what it renders**, at output resolution, because `C_min`
  is a target rather than a measurement and this is the one place a per-machine figure to check it
  against can be obtained for free — the probe is already running the real pass chain off the frame
  path, and the alternative is rendering a floor composite nothing asked for on a machine where
  [decision 35](Decisions.md#35-a-miss-costs-one-frame-bounded-by-the-floor-composite)'s second
  branch never fires. Subtracting the effect passes from an ordinary frame is not that alternative:
  [decision 62](Decisions.md#62-effect-composition-is-an-optimization-and-the-unfused-path-is-the-reference)
  fuses pointwise effects into the composite pipeline, so a planned frame holds no separable
  base-composite span.

  **It also has to seed the irreducible term, and that half it cannot finish.**
  [Decision 94](Decisions.md#94-a-frames-cost-has-a-part-no-tier-reduces-and-the-walk-is-it) makes
  the scene walk a third figure in `BudgetPolicy`, and unlike the two beside it that figure is a
  property of the session rather than of the machine — a probe can measure the walk over an empty
  scene and nothing more, so what is open is whether an empty-scene seed is worth taking at all or
  whether the first window's worth of measurement arrives soon enough to start at zero.
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
- **`LP_NUM_THREADS` sizing, and the shader compilation worker with it.** Decision 40 bounds
  interference by reserving cores rather than capping time, which turns "how many" into a number
  that wants measuring on machines with 4, 8, and 16 cores. Decision 62's compilation worker is
  bounded the same way and is probably the same reservation rather than a second one, since the two
  do not contend at the same times — lavapipe is busy when there is no GPU, and variant compilation
  is busy when there is one.
- **The lock-screen content surface.** Decision 43 defers it with a design; the protocol, the
  surface role, and how it interacts with multiple outputs are unspecified.
- **Respawn policy, for the greeter, the shell, and gyro itself.** One greeter serves every session
  under decision 43, so its crash locks out the machine; decision 49 rate-limits gyro's own restarts
  for the same reason and falls through to `gyro --console`; decision 51 adds a per-session shell
  whose crash costs one user their desktop but not their clients. These are one problem at three
  radii — the thing that gets you back to work is itself crashing — and want one answer, covering
  the rate limits, what the output shows meanwhile (decision 51's floor policy and background, for
  the shell), and whether a crash loop should escalate the way gyro's own does.
- **What `SysRq-V` actually restores under `fbcon=off`.** Decision 37 now leans on it as the last
  key that can put a picture on a panel whose owner is wedged, and the DRM core registers it for
  every device — but it forces an *in-kernel client* to restore, so it does nothing unless one is on
  that device's client list. `fbcon=off` disables the console's binding rather than fbdev emulation
  itself, so the client is most likely still there; that is a configuration observation on the
  target rather than an argument, and it rides free on the first boot the DRM backend completes.
- **How a promoted client buffer is named at the seam.** [Decision 78](Decisions.md#78-present-takes-a-layer-list-and-the-composite-is-one-member-of-it)
  takes the layer list and can express every layer whose source is one of the output's own targets —
  the composite, a cursor image, a virtual output's imported ring. It cannot yet express the one that
  matters most, a client's buffer flipped straight to a plane, and the obstacle is a placement rather
  than a spelling. Turning a dmabuf into a scanout framebuffer is a kernel allocation, so it must not
  happen inside the frame section; the natural fix is an import verb whose result the layer names,
  and the presenter is frame-side, so dispatch cannot call it without a second thread touching the
  presenter. Three shapes are visible and none has been argued through: the import happens at
  configuration time for a bounded set of promotable surfaces, which bounds what may be promoted by
  something other than the assigner; the layer carries the dmabuf description inline and the backend
  caches framebuffers against it, which puts the cache's eviction policy on the frame path; or the
  composition root owns the import as it owns migration, which is a third party in a per-frame
  decision. This blocks nothing today and blocks plane assignment entirely, so it wants answering
  before that is written rather than during.
- **How a texture is minted, and who holds it.** [Decision 82](Decisions.md#82-the-renderer-is-handed-an-evaluated-draw-list-not-a-scene)
  has a draw item name a `TextureId` and nothing in `IRenderer` creates one, on the grounds that a
  `wl_buffer` arrives on the dispatch thread and turning it into a device image must not happen inside
  the frame section — so import is the renderer's dispatch half, written when there is a protocol
  layer to call it. **The layering half of this is now answered, and was worse than it looked**:
  [decision 87](Decisions.md#87-a-type-both-halves-of-the-world-name-lives-below-both-waists-not-in-seam)
  found `TextureId` sitting in `Seam`, which neither `Protocol` nor `Scene` may name, so the import
  verb had no legal caller rather than merely no design — and moves the type to `Core`. What stays
  open is the shape, and it is the promoted-buffer entry above wearing different clothes: the
  import verb, the release that has to be safe while the frame
  thread may still hold the id in a list it is recording from, and whether a failed import is a frame
  that draws nothing there or a surface that is refused at commit. The snapshot atlas is the case that
  says it cannot simply be deferred to whoever writes `Protocol` — an exit snapshot is minted by the
  *renderer* from pixels that are about to stop existing, which is an import with no client on the
  other end of it. It blocks nothing until the first surface is drawn, and blocks that entirely.
- **The cursor is a commit that is not a frame.** [Decision 29](Decisions.md#29-outputs-are-periodic-real-time-tasks-the-test-allocates-effect-budget)
  exempts the cursor plane from the budget on the grounds that it updates independently of the
  composite. That is a claim about the *rate* — the pointer moves at the input device's rate and not
  the output's — and it implies a commit path with no frame behind it, which `IPresenter` has no room
  for: `Present()` is a whole frame's layer list, and calling it to move a cursor would recomposite
  nothing at the cost of a full atomic commit's worth of state. This is the shape
  [decision 73](Decisions.md#73-the-frame-thread-initiates-reconfiguration-and-never-performs-it)
  caught once already — the seam provisioned for what is frame-side and had no room for what is not —
  and it is recorded rather than provisioned for because the answer depends on two things nobody has
  measured: how much of input-to-photon a coalesced cursor commit actually saves, and whether a
  cursor-plane commit disturbs the refresh timer on a VRR panel, which the presentation-timing entry
  below already wants a panel for.
- **What a refused promotion costs.** A surface bound for a plane can skip the linearised copy and
  the mip chain entirely, which is most of the win for video. If the atomic test then refuses the
  partition, gyro must composite that surface on the frame it had planned not to — and the import it
  skipped is an allocation and a conversion pass on the frame path, which is the one place nothing
  may allocate. Three answers, none free. Import speculatively on the dispatch thread, and spend the
  memory and bandwidth the offload existed to save. Treat a refusal as a miss and fall that output
  to the floor tier for one frame, which
  [decision 35](Decisions.md#35-a-miss-costs-one-frame-bounded-by-the-floor-composite) already
  sanctions and which costs nothing new. Or forbid the skip, and keep only the scanout half of the
  win. The middle answer
  looks right and is left here rather than written into a decision because it has not been measured
  against a real driver's refusal rate — a controller that refuses often turns it from a rarity into
  a stutter, and that number is not knowable from the specification.
- **Presentation timing needs hardware validation.** Decisions 28–32 and 66 are designed rather than
  measured. The scheduling half is testable headless with fake clocks at arbitrary mixed rates, and
  should be the first thing that harness is pointed at. *(Revised 2026-08-16: the VRR half divides
  rather than deferring whole.)* The servo's own logic belongs on the testable side now that
  decision 31 puts it on the clock — it commands a period and reads back an observation, and a fake
  clock can refuse a command as readily as a real panel can. What still needs a panel is the panel's
  answer: the flicker threshold, what the advertised range is worth in practice, and whether a
  cursor-plane commit disturbs the refresh timer — which matters more than it sounds, because
  decision 29 exempts the cursor plane from the budget on the grounds that it updates independently
  of the composite, and that is a weaker claim on a VRR panel than on a fixed one.
- **Chunk granularity and GPU preemption.** Chunking assumes a submission boundary is a scheduling
  opportunity for the GPU. It is not a guaranteed preemption point and the behaviour is
  hardware-dependent. Decision 30 now gates chunking on this being measured, so the question is no
  longer whether the assumption holds but whether the case that needs it ever arises.
- **GPU priority in practice.** Decision 22 takes `CAP_SYS_NICE` for a high-priority queue on the
  strength of driver source rather than observation. Whether `VK_QUEUE_GLOBAL_PRIORITY_HIGH` is
  granted, and how much blocking it actually removes against a saturating client, wants measuring on
  both i915/xe and amdgpu.
- **Whether clients round their internal geometry the way decision 68 assumes.** That decision snaps
  a subsurface to the device grid on the reasoning that the client already rasterized its own layout
  there, so gyro is agreeing with the pixels rather than moving them. It holds if clients round to
  device pixels, at the scale they were told, to nearest — true of Cairo and Skia, unguaranteed in
  general, and inverted for a client that floors, which would land a full device pixel out and be
  worse than not snapping. Measurable as soon as the nested backend exists: mpv, Firefox, and a GTK4
  video widget on a 1.5× virtual output, looking at the subsurface boundary. Cheap, and it decides
  whether 68 needs a per-client escape or none.
- **The real-time priority numbers.** Decision 61 settles the policy and requires the levels to be
  stated rather than discovered, and does not state them. Frame thread, device workers one below,
  dispatch below that, and `LimitRTPRIO=` above all three — chosen against what `rtkit` actually
  grants on a running system, which is a number to read rather than to reason about. It also wants
  confirming that a `SCHED_FIFO` frame thread is in fact not preempted by the audio stack in
  practice, since that is the one property decision 61 declines `SCHED_DEADLINE` in order to
  approximate.
- **Scheduling policy constants** — the VRR servo's per-frame bound, the clearance it holds above
  the bottom of the panel's range so that low-framerate compensation never engages underneath it,
  decision 66's hysteresis before an output is read as no longer client-controlled — which
  [decision 76](Decisions.md#76-cadence-authority-follows-predictability-not-foreground) confines to
  the case where a client has stated no cadence, so it is now a fallback's constant rather than the
  mechanism's — how recently a surface must have committed to disqualify its output from early
  rendering, and decision 56's debounce before a surface's preferred scale is lowered. The timing
  policy's safety margin joins them, and it is the one of these that is measurable rather than
  chosen: it covers the interval between the timer the composition root arms expiring and the first
  instruction of the record, so what sizes it is the wakeup latency of a `SCHED_FIFO` thread on an
  `io_uring` absolute timeout — a distribution to sample on the target machine, and a worst case
  rather than a percentile for the reason
  [Architecture.md](Architecture.md#budgets) gives about every other figure in the schedule.
- **The imperative escape hatch** for event-driven one-shots — shape and boundary.
- **Color interpolation space for animation** — Oklab proposed over sRGB. Distinct from
  [decision 47](Decisions.md#47-compositing-happens-in-linear-light-at-wide-primaries)'s composite
  space and easily conflated with it, so worth stating apart: 47 governs the space pixels are
  *combined* in and is
  settled by physics; this governs the path a single color takes while *animating* between two
  values, and is settled by perception. Linear light is right for the first and visibly wrong for
  the second, where it crushes the middle of a hue transition.
- **Configuration format** — a hand-rolled flat key-value parser is proposed, consistent with the
  hand-rolled XML parse in the protocol generator.
- **Settling thresholds** for non-geometric properties. The geometric half is settled by decision
  54; opacity, blur radius, and corner radius have no output pixel to be expressed in. Decision 65's
  progress parameter is the one such channel that escapes rather than joins them, since its mapping
  carries a travel distance and a threshold on it converts back to output pixels.
- **Detents, and the flick threshold underneath them.** Decision 13 names detent placement as part
  of the displacement mapping the catalog owns, and no seed transition has one — correct for all of
  them today, so the field is absent rather than defaulted. What keeps this from being a matter of
  adding one is that detents cannot be inert data: choosing between a detent and an end at release
  needs a threshold in progress per second, which is input-side tuning nobody has measured, and it
  interacts with the velocity estimator whose quality decision 65 already flags as felt directly.
  Wants deciding with a real transition that needs an interior stop, rather than ahead of one.
- **The lead horizon for a driven gesture.** Decision 65 carries progress toward predicted
  presentation time to undo the input-to-photon gap, bounded so that the correction does not become
  a guess. One output period is the obvious first answer, being the gap actually being undone. The
  bound trades against a real artefact rather than a theoretical one — an abrupt stop runs on by `v₀
  · horizon` until dispatch republishes at zero velocity — so it wants measuring against a touchpad,
  with direction reversal as the case that decides it.
- **io_uring's determinism claim.** *(Replaces the kernel-floor entry, 2026-08-16, which is settled
  — see [decision 3](Decisions.md#3-io_uring-event-loop-via-liburing): the floor is one probed ring
  configuration, multishot `recvmsg` is gone with decision 2, and there is no epoll fallback.)* What
  is left open is the premise underneath, which is that `DEFER_TASKRUN` measurably keeps kernel
  completion work off the render. Unmeasured, load-bearing, and the same shape as the argument
  decision 2 lost. Wants the headless harness, alongside the presentation-timing sweep.
- **Whether a source can answer when it will next have something.** *(Raised 2026-08-21, by the
  composition root running headless against a real clock.)*
  [`IEventSource`](../Source/Seam/EventSource.h) has a descriptor and a drain, and
  [decision 80](Decisions.md#80-the-frame-loop-is-a-step-the-composition-root-owns-the-wait) makes an
  invalid descriptor an ordinary answer — a headless flip is a function of the clock and no file
  becomes readable. That leaves one hole, and it is exactly one frame wide: before an output's first
  `Presented`, `FrameClock` has no anchor, so `Timing::WakeFor` contributes `Never()`, the loop arms
  nothing, and the flip that would have anchored the clock is never drained. The backend and the idle
  fold wait for each other. `Compositor` closes it by folding `HeadlessDevice::NextEvent()` into the
  wake it hands the shim, which is defensible — the root wired a clock-driven presenter to a real
  clock, so the consequence is the root's — and is also the root knowing which backend it built,
  which is the thing the seam exists to stop. The alternative is a third verb on `IEventSource`, and
  what makes it a question rather than a patch is that every *real* source would answer `Never()`:
  a DRM file cannot say when it will next be readable, so the verb would exist for the fake alone,
  which is [the test](Structure.md#orchestration) for whether a seam is real run in reverse. Settle
  it when nested lands, since a host connection is the second source with a genuine answer — a frame
  callback is a promise about the future in a way a page-flip event is not.
- **spdlog async sink** — file I/O from the frame thread punts to io-wq and surfaces as jitter.
