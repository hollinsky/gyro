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

A fourth has now left by the other route the rule names. *What a dressing means on a reference node*
said to settle it with the walk, and
[decision 99](Decisions.md#99-a-dressing-draws-over-the-nodes-own-extent-whatever-the-nodes-kind) is
that walk having been written: the question turned out to have a cheap answer and two expensive ones,
which is legible from the code and was not legible from the entry.

A fifth left by the first route and took the entry's premise with it. *Whether Xwayland forwards
`_GTK_FRAME_EXTENTS` into window geometry* became
[decision 106](Decisions.md#106-an-x11-client-has-no-window-geometry-gyros-window-manager-computes-the-frame-rect):
the answer is no, because Xwayland gives an X11 client no window geometry for anything to be
forwarded into. An entry can name its source correctly and still ask after a mechanism that is not
there, and what settles that is enumerating everything the source *does* do — a grep that finds
nothing only proves the grep.

A sixth left by neither route: it named work rather than a question, and the work is written. *The
two client-reachable `wl_abort` sites* asked for a wrapper that cannot publish a resource id before
its implementation is set and a dispatch table that cannot have a hole, and both are now what
[Tools/Bindings](../Tools/Bindings/Emit.Server.cpp) emits — `Create` is the only door onto a
resource and its two calls are adjacent, and the table is declared and filled from one walk over the
XML. Writing it surfaced a third thing the entry did not know to ask for, and that one *was* settled
by reading: for the core protocol the `wl_interface` libwayland bounds an opcode against is
libwayland's own rather than the one the table was generated from, so the two agree by shipping in a
single package rather than by construction. The bindings now compare the counts before a resource of
that type can exist. An entry that names work can still be hiding a question, and the way to find out
is to do the work.

A seventh left by the route the triage rule names and no entry had used before: somebody watched it
happen. *The floor composite must not flicker* said in as many words that it would not yield to an
afternoon of reading, that it was a perceptual question, and that it wanted a blur, a scene that
animates, and somebody watching — and it got all three, on a run bar, as a blurred backdrop snapping
flat for one refresh and back. It became
[decision 191](Decisions.md#191-late-arrival-selects-a-frame-never-a-tier--the-record-time-check-is-planned-or-wait).
Two things about that are worth keeping. The entry was right that watching was required and wrong
about what watching would settle: the perceptual trade it posed — is a flicker worse than a dropped
frame — never had to be judged, because the trace taken at the same time priced the two composites
and the flat frame turned out to be buying 27%. And what the watching actually supplied was not a
verdict on the artefact but the *occasion* to look, which no amount of reading was going to produce
on its own. An entry that needs a user in front of it may still be settled by arithmetic once the
user is there.

An eighth left by growing the consumer it was faulted for lacking. *`Emit`'s promise of no syscall is
a property of the machine's clocksource* asked whether the composition root should read
`/sys/devices/system/clocksource/clocksource0/current_clocksource`, and argued against itself on the
grounds that this was a `/sys` read producing a number nothing decides against — with
[decision 57](Decisions.md#57-one-timebase-clock_monotonic-converted-at-ingest-and-nowhere-else)
standing behind the objection. It became
[decision 197](Decisions.md#197-a-trace-says-what-produced-it-and-the-log-runs-beside-the-frames)
once there was somewhere for the number to go: written into a trace's header it sits beside the very
cost figures it inflates, which is a reader rather than a decider. The entry was not wrong that a
number nothing consumes does not earn a syscall; what it could not see was that the consumer was one
change away. An entry that rests on *nothing would use this* is worth re-reading whenever something
new starts reporting.

The same reading measured one thing it left alone, kept here because it is a number and this is where
it was taken: `IClock::Now` being virtual costs between 0.0 and 0.16 ns against a 13.3 ns clock read,
in the noise and an order of magnitude better than [Clock.h](../Source/Core/Clock.h)'s own estimate of
"roughly a tenth". Resolving the clock to a concrete type when a buffer is armed would buy nothing,
and the interface is what lets the headless sweep place time at arbitrary phase.

- **Re-reading the abort inventory on major libwayland bumps.** The count went 6 → 18 across 1.23 to
  1.24, and the reading behind decision 2 is a snapshot of `1.26.0-9-ged0b9f1` rather than a
  property of the library. What wants watching specifically is a new site reachable from client
  input with no gyro bug in front of it, since that is the first of the three conditions decision 2
  names for reopening the in-tree server half. The two *client-reachable* sites are closed
  structurally by the generated bindings and no longer depend on the reading — what a bump can still
  move is the rest of the inventory, which is gyro's own API misuse and wants a person rather than a
  compare.
- **Tone mapping and gamut mapping policy**, in both directions, deferred by decision 47 as
  additive. The SDR-on-HDR direction is where every shipping system has gone wrong, and the rule is
  that SDR white maps to a reference — BT.2408 says 203 nits — or to a stated user preference, and
  never to display peak. HDR-on-SDR needs a curve chosen rather than inherited. Neither is urgent;
  both are easy to get subtly wrong and hard to notice afterwards.
- **Blur order against tone mapping.** Blurring in linear and tone mapping the result is correct and
  costs a full-resolution map after the chain; tone mapping first and blurring afterwards is cheaper
  and temporally steadier and is wrong. A real number attached to a real artefact, so it wants
  measuring rather than arguing — the same standing as the virtual-output color question below.

  **A default is now in the tree and it is the correct order, taken because there is nothing to order
  against.** *(2026-08-22, by
  [decision 117](Decisions.md#117-a-gather-reads-the-target-it-is-drawing-into-the-numbers-live-in-seam-and-the-tier-rides-the-request).)*
  [Render/Backdrop.h](../Source/Render/Backdrop.h)'s chain decodes the composite to linear, blurs, and
  re-encodes; nothing in the tree tone maps, so the cheap-and-wrong arrangement is not available to be
  chosen yet. What that fixes is where a tone map *lands* when it arrives — after this — and what it
  does not fix is whether that is affordable, which is still the measurement.
- **Blur across color-state boundaries.** `Material::Glass` samples a backdrop that may hold an HDR
  video window beside an SDR text editor, and physically correct linear blur bleeds a 1000-nit
  highlight through the glass into the region over the SDR window. Correct, and startling. There is
  no obviously right answer, which is what makes it a decision rather than an implementation detail.

  **The default is the correct-and-startling one, and building it put a price on the alternative.**
  *(2026-08-22, by
  [decision 117](Decisions.md#117-a-gather-reads-the-target-it-is-drawing-into-the-numbers-live-in-seam-and-the-tier-rides-the-request).)*
  The backdrop a gather reads is the composite target, into which every item has already been
  converted — so there are no colour-state boundaries left in what is sampled, and the bleed happens
  exactly as linear arithmetic says. That is the entry's own first branch taken by omission rather
  than by argument. What is new is the cost of the other branch: knowing which region of the backdrop
  came from which state means a per-pixel provenance channel the composite does not carry and which
  nothing else wants, so *soften the bleed* is not a shader change but a second attachment. Still a
  decision, and now one with a number on the far side of it.
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

  **The copy-on-write repair does not reach the node run, and what replaces it is more specific.**
  *(Narrowed 2026-08-22 by
  [decision 111](Decisions.md#111-an-entity-is-a-nodes-authoring-side-the-store-is-one-tree).)* An
  arena whose cost is the dirty set works for a per-entity record and not for a preorder run: an
  insertion renumbers every index above it, every enclosing subtree length, and every backward
  reference target, so there is no patch smaller than the run. What is left is the observation that
  the node run changes only when topology, flags, content, or the *active set* changes — a retarget
  inside an already-active channel changes none of them — so the saving on offer is copying the
  previous run's bytes rather than walking the store to rebuild them. Whether that copy beats the
  walk is a memcpy against a pointer chase over the same node count, which is the kind of thing to
  measure rather than argue.
- **The wake fold is not partitioned per output, and an idle panel pays for its neighbour's
  animation.** *(Raised 2026-08-23 by
  [decision 122](Decisions.md#122-the-wake-fold-is-scene-wide-and-replicated-per-output-a-settled-channel-retires-where-it-is-published),
  which takes the scene-wide answer deliberately and says why.)* Decision 69 made `Sooner` a monoid so
  the fold could be split by output; the split is unavailable for the one contributor that exists,
  because partitioning a node's contribution needs a swept screen-space bound the dispatch side has no
  way to compute cheaply. So every output is told the whole scene's answer, and a panel with nothing
  moving on it composites at full rate for the length of an animation happening on the panel beside
  it — on the same GPU queue, so it delays the frame somebody is actually watching. Two exits are
  visible and neither is free. Compute the bound dispatch-side, which is a second transform walk per
  commit plus a per-channel envelope, at input rate. Or let the frame walk report per output what it
  saw moving, through decision 83's return channel, and narrow on the strength of it — cheap, and a
  feedback loop that is one frame stale in the direction that freezes rather than the direction that
  wastes. What decides it is a number nobody has: how much of a second panel's budget this actually
  costs on a real desktop, which wants two monitors and a menu rather than an argument.
- **Whether the publication nudge should be conditional.**
  [Decision 83](Decisions.md#83-dispatchs-publication-is-an-event-source) has dispatch write its
  descriptor on every publication, which is correct and which wakes the frame thread ahead of its
  timer whenever both threads are busy. The saving is to write only when the frame thread last
  reported a `Settled` wake, which the return channel can carry — though not in a spare word any more
  *(2026-08-23)*, since the presented run took the one this entry was counting on and a flag would have
  to be its own field. What makes it a question rather than a patch is the lost wakeup it opens — dispatch may read
  *not idle*, publish, and decline to signal, all inside the window before the frame thread posts its
  report and sleeps — so the conditional form is correct only with the step re-checking the ring after
  posting and before it returns `Never()`. Settle it by measuring the waste on a busy system rather
  than before.

  **A different condition was taken and this one is untouched.** *(2026-08-23, by
  [decision 128](Decisions.md#128-the-publication-doorbell-rings-when-a-snapshot-crossed-not-on-every-step).)*
  The doorbell now rings when a snapshot actually crossed rather than on every step, which removes the
  wakeups a *refused* publish would have caused and none of the ones this entry is about. The two are
  not the same shape: that condition reads dispatch's own publication counter in its own thread, so
  there is no window to lose a wakeup in, while the saving proposed here reads the frame thread's state
  and races it. What still wants measuring is the waste on a busy system, where every publication is
  accepted and every one of them wakes a frame thread that was already running.
- **The return channel has no doorbell, so a deferred publish is retried on a timer.** *(Narrowed
  2026-08-25 by
  [decision 147](Decisions.md#147-the-return-channels-doorbell-is-the-frame-threads-and-it-rings-only-where-a-client-is-waiting),
  which wakes dispatch when a presented frame is owed to a client — the composition root's write on a
  condition dispatch publishes, rather than a descriptor on the channel. The deferred publish this
  entry is about is untouched: a refused publish is unblocked by the watermark moving, which the
  ledger that condition reads knows nothing about, so the poll stays.)*
  [Decision 74](Decisions.md#74-the-forward-ring-recycles-only-below-the-watermark-and-a-full-ring-defers)'s refused publish is retained
  and retried, and what unblocks it is the frame thread posting a `FrameReport` — but
  [Publication/Return.h](../Source/Publication/Return.h) carries no descriptor, so nothing wakes
  dispatch when that happens. [Dispatch/Loop.h](../Source/Dispatch/Loop.h) polls instead, at an
  interval sized to a panel period because a frame completing is the only thing that can unblock it.
  That is decision 83's problem with the threads reversed and without decision 83's answer. The
  obvious fix is an eventfd on the return channel, written by the frame thread, and it keeps decision
  83's bargain rather than inverting it: the post site is reached only when the frame thread steps,
  and an idle frame thread blocks indefinitely, so an idle machine writes nothing on this channel
  either. What it actually costs is a syscall per *rendered* frame from inside the `FrameSection`
  guard — non-blocking, and sub-microsecond beside the commit it would sit next to, and the post is
  the last statement before the guard closes, so it can move out of the section rather than have to
  be argued into it. The conditional form is available for the same reason decision 83 gives for a
  doorbell not being the third channel [the publication
  boundary](Architecture.md#the-publication-boundary) forbids: dispatch raising a flag on the forward
  ring when a publish is refused, and the frame thread testing it before it posts, carries nothing
  anything renders from — and the lost wakeup that opens is the double-check decision 83 already
  wrote down for the forward nudge. What is left as the argument for the poll is not its cost but its
  reach: settle this once the deferral path has been seen to happen at all, since it may never.
- **Decision 32's hysteresis has no input on the dispatch side.** *(Raised 2026-08-25 by
  [decision 146](Decisions.md#146-a-commit-is-owed-a-frame-and-a-ledger-of-entities-is-what-turns-a-presented-sequence-into-a-callback),
  which built the cadence and not the smoothing.)*
  [Decision 32](Decisions.md#32-a-surfaces-frame-cadence-follows-its-fastest-output) says a surface
  follows the fastest output it touches, *with hysteresis, and never switched mid-animation*. What is
  built answers on the first output to present the sequence, which is the same thing in the steady
  state and is re-decided from nothing on every commit.

  **What it costs is a client's own animation, at the moment somebody is watching that window.** Take
  a 60 Hz panel beside a 144 Hz one. A window straddling the seam is paced at 144 the instant one
  pixel crosses and at 60 the instant that pixel leaves, and a slow drag puts its edge over the
  boundary for long enough that the cadence flips every few frames. A toolkit paces its own animation
  off the interval between frame callbacks, so what a person sees is the *content* — a scrolling
  list, a caret, a spinner — stuttering inside a window that is gliding smoothly. gyro's animation is
  right and the client's is visibly wrong, which is the failure that decision says is worse than
  either rate. The cheaper half of it is the same crossing read as work: one pixel over the seam buys
  full 144 Hz pacing for a window almost entirely on the 60 Hz panel, so the client draws 2.4x the
  frames and two in three are composited into a panel that never shows them.

  **It costs nothing today.** There is no seat, so nothing drags a window; the Floorplanner centres a
  window on one output and it stays there, the reach mask is computed once and never changes, and
  every ledger entry clears against a single panel. The gap opens the day a window can move.

  Closing it wants a nominal refresh period on `SceneOutput`, which cuts against
  [decision 97](Decisions.md#97-an-outputs-placement-is-published-the-modes-half-of-the-view-meets-it-in-the-walk)
  giving the mode's extent to the frame side — the case for it being that a period used to *choose
  between* outputs is a policy input rather than a timing authority, and dispatch already holds worse
  ones. Not latching to the panel holding the window's centre instead: it carries no rate either, and
  a centre crossing a seam is a step function, so it moves the flip rather than removing it. What
  decides the shape is whether the switch is visible on a real drag across two panels at different
  rates, which wants the hardware rather than an argument.
- **Client damage is stored and never cleared.**
  [Decision 113](Decisions.md#113-client-damage-is-a-region-on-the-entity-in-buffer-space-and-it-is-cumulative)
  clears a surface's rectangles once *every* output showing it has presented the sequence that carried
  them, and
  [decision 146](Decisions.md#146-a-commit-is-owed-a-frame-and-a-ledger-of-entities-is-what-turns-a-presented-sequence-into-a-callback)'s
  ledger is exactly the fold it needs — the entry goes on clearing output bits after the frame
  callback has gone out for that reason. What is missing is the other end: `Protocol` mints no
  rectangles into the entity yet, so there is nothing to clear and no second signal. It lands with the
  damage run, and the ledger is not to grow a second mechanism when it does.
- **The shell's scene vocabulary.** *(Kinds answered 2026-08-22; the dressings remain.)*
  [Decision 95](Decisions.md#95-the-scene-vocabulary-is-four-kinds-a-material-is-a-field-not-a-kind)
  settles the node kinds — container, image, solid, reference — and finds decision 51's sketch wrong
  on its axis rather than merely short. What it deliberately does not settle is the *contents* of the
  two enums that dress a node, and both are still one design problem with the motion catalog, since a
  node, the material that dresses it, and the transition that reveals it are three views of one
  thing. The test named in decision 51 is still the constraint: a shell must not be able to produce
  motion that does not match the catalog.

  **What is left is the gesture vocabulary.** *(Dressings answered 2026-08-22.)* The material set and
  the elevation set were the review this entry asked for and are
  [decisions 103 to 105](Decisions.md#103-a-dressing-is-named-by-what-it-does-to-light-the-material-set-is-glass-and-smoke);
  designing them together is what produced the one rule both are named by, and what found that a
  shadow has to animate. A gesture, the transition it drives, and the nodes it moves are still one
  design problem seen three ways, and it still wants a screen rather than an argument.

  *(Annotated 2026-09-06.)* The rest of the vocabulary is now on the wire —
  [decision 198](Decisions.md#198-a-shell-says-where-the-windows-are-and-never-how-they-get-there-a-commit-names-a-transition-and-there-is-no-way-to-name-none)'s
  `gyro_scene_v1` carries containers, placement and the transition — and the gesture is the one part
  of it that was not specified, because three things are missing rather than one: `Scene/Entity.h`
  carries four sprung channels and no driven one, `Input/Devices.cpp` drops libinput's swipe and pinch
  for want of a recognizer, and the vocabulary itself still wants the screen this entry asks for.
  `Animation/Author/Drive.h` and `World/Node.h`'s `DrivenRamp` are the two ends that exist. **The
  order is now fixed even though the answer is not**: the driven channel is buildable today and does
  not need the screen, and enumerating the gestures does — so the channel lands first and the
  vocabulary is what a review with a panel in front of it settles.
- **The scene vocabulary closes arrangement and not trajectory.**
  [Decision 89](Decisions.md#89-a-commit-resolves-in-two-phases-a-change-becomes-motion-where-its-inputs-are-complete)
  makes setting a model value *be* a retarget, so a shell that republishes a node's position every
  frame gets a catalog spring chasing a moving target — which is
  [decision 65](Decisions.md#65-interactive-transitions-are-driven-by-a-progress-parameter-not-by-a-moving-target)'s
  rejected model reached from the authoring side, and reads as lag rather than as the catalog.
  Decision 95's closed set does not close it, because the hole is in *when* a value may change rather
  than in what may be said. The candidate rule is that every mutation names a `Transition`, which is
  roughly the shape a commit already has; what it costs is a shell that wants to place a window
  without animating it, and whether that case is real decides whether the rule is worth having. This
  is decision 51's falsifiable test with a mechanism attached, so it should be settled before a shell
  exists to violate it.

  **Half of it is answered and the answer is not the shell.** *(Narrowed 2026-08-22 by
  [decision 112](Decisions.md#112-a-commit-is-a-scope-with-an-origin-and-the-wire-says-when-it-closes).)*
  The cost this entry weighs the rule against — a caller that wants to place something without
  animating it — is not hypothetical and does not belong to a shell: every `wl_surface.commit` is a
  mutation that must not animate, since nothing a client authors is a sprung channel. So a transition
  meaning *none* has to exist whatever is decided here, and what stays open is only whether a
  **shell** may name it freely, which is the half that decides whether the catalog is enforceable.

  **Answered 2026-09-06: a shell may not name it, and the cost is paid at creation.**
  [Decision 198](Decisions.md#198-a-shell-says-where-the-windows-are-and-never-how-they-get-there-a-commit-names-a-transition-and-there-is-no-way-to-name-none)
  makes `gyro_scene_v1.commit` carry the catalog minus `None`, so every change a shell makes to
  something on screen animates. What the rule was weighed against turned out to be a coordinate
  stated before there is anything to move — a container being made has nowhere to have come from, and
  a window being placed for the first time has never been anywhere — so both carry their position on
  the request that creates the state rather than needing a transition that does not. The whole of this
  entry is now closed.
- **Per-node damage, which needs an identity the node record does not carry.**
  [Decision 101](Decisions.md#101-damage-is-the-whole-output-while-anything-moves-and-per-node-damage-needs-an-identity-the-record-does-not-carry)
  reports the whole output while anything is moving, because damaging where a node *was* against
  where it *is* requires the two frames' nodes to be the same node — a published `Handle` rather than
  a position in a run dispatch may reorder between publications. Client surface damage is the other
  half and now has a carrier —
  [decision 113](Decisions.md#113-client-damage-is-a-region-on-the-entity-in-buffer-space-and-it-is-cumulative)
  puts it on the image entity as a `Region<BufferSpace>`, cumulative because the ring may skip, and
  gives it a run of its own beside the content runs *(2026-08-22)*. What decides the shape of the
  node half is whether identity is a field on the node or a parallel run, and whether a node that
  gains one still costs 128 bytes. It is worth doing when there is a partial-composite path to feed; until then the
  coarse rule costs bandwidth on animating frames and nothing on still ones.
- **The corner radius and the layout state that zeroes it.** *(Carrier answered 2026-08-22; the
  number remains, below.)*
  [Decision 105](Decisions.md#105-relief-is-one-scalar-the-corner-radius-and-the-shadow-move-together)
  makes the radius and the shadow height one animated scalar — the shell holds the fact that a window
  is framed, gyro holds both numbers — which is the split this entry said the fullscreen case pointed
  at. It was forced by the node record rather than chosen: the reserved tail holds one more
  coefficient slot at 128 bytes and two would take it to a third cache line, so the two could not both
  be independent channels.
- **The buffer-to-surface adapter is not published, so nothing can classify a resample.**
  [Seam/Renderer.h](../Source/Seam/Renderer.h) requires the producer to derive `DrawItem::Sampling`
  because a renderer cannot recover it from four floats, and deriving it means composing the surface
  adapter — buffer scale, buffer transform, `wp_viewport` — with the node's chain. None of that
  crosses yet. Until it does, every item reports the all-false class, which is the honest default and
  which puts a still window on the resampling path that
  [decision 56](Decisions.md#56-clients-render-at-the-ceiling-and-gyro-downscales)'s sharpness rule
  exists to keep it off. It arrives with the protocol layer rather than being decidable now.
- **Clipping and masking.**
  [Decision 95](Decisions.md#95-the-scene-vocabulary-is-four-kinds-a-material-is-a-field-not-a-kind)
  leaves both out and says so rather than assuming them away. A node's children are not clipped to
  its extent, and a group is not a substitute — its offscreen sits at the subtree's own screen-space
  bound, so it contains the overflow rather than cutting it. What that costs is an overview tile that
  cannot crop a window with a popup hanging off it, and a shell's scrolling list of live surfaces
  spilling past its container. Both are recoverable by the shell clipping in its own surface, which
  is why this is an entry rather than a defect; what decides it is whether the recoverable version
  costs a round trip on something being dragged.
- **Where a client's entity is parented, before and without a shell.** *(Answered 2026-08-23 by
  [decision 141](Decisions.md#141-a-window-is-parented-into-gyros-floor-and-shown-when-placed-the-floorplanner-stands-in-for-an-absent-shell).)*
  A client's surface appears in the store the moment it commits, parented into its session's **floor** —
  the default container gyro authors, so it exists before the shell connects and survives the shell
  crashing — and it is *shown when placed* rather than at parent, so the window between commit and the
  shell's answer is invisible rather than flashing at the origin. Where no shell answers — development,
  or the restart gap — gyro's **Floorplanner** places it centered on the pointer's output. What stays
  open is layout below: the shell still declares and moves containers, and none of that policy is
  gyro's. *(Built 2026-09-06 by
  [decision 198](Decisions.md#198-a-shell-says-where-the-windows-are-and-never-how-they-get-there-a-commit-names-a-transition-and-there-is-no-way-to-name-none)
  — a shell declares a container by a name it mints, places windows into it, and claims placement so
  the Floorplanner stands down. One correction to this entry's own wording came out of writing it: a
  declared container is a **child of the floor** and not a root beside it, because decision 55 makes
  the sibling list the paint order and a later root is in front — a workspace authored as a root would
  draw over the shell's own panel.)*
- **A match key for a surface a client re-created.**
  [Decision 114](Decisions.md#114-retirement-is-the-author-going-away-and-resurrection-is-the-authors-alone)
  found that Animation.md's headline resurrection case is not resurrection: dismissing a menu destroys
  the `xdg_popup` and normally its `wl_surface`, so reopening it is a new entity and the smoothness has
  to come from decision 18's matching instead. Nobody mints the key that would make that work — a
  client does not declare match keys and the shell does not know a popup was reopened — and the
  obvious repair, gyro deriving one from the parent surface and the positioner, is a heuristic about
  what a toolkit meant. What decides it is whether the artefact is real: menus reopened fast enough
  to overlap their own dismissal are common, and the failure is a popup that pops rather than
  reverses. Wants a toolkit in front of it, not an argument.
- **What a restarted shell is told about what it left behind.** *(New 2026-09-06, from
  [decision 198](Decisions.md#198-a-shell-says-where-the-windows-are-and-never-how-they-get-there-a-commit-names-a-transition-and-there-is-no-way-to-name-none).)*
  A shell that comes back after a crash asks for its containers by name and is handed the ones it
  left, with the same windows still in them and still where they were — which is the property decision
  141 makes containers gyro's for. What it is not told is *which window is in which*, so it can put
  the workspaces back on screen and cannot draw the strip that says what is on each one. Three shapes
  are available and they are not equal: a `window` event per occupant on the container, which needs the
  foreign-toplevel identifier rather than the object because the two protocols' handles arrive
  asynchronously and a request that named an object would impose an order on binding them; a query on
  the window rather than on the container, which is the same information transposed and reads better
  for a taskbar than for a workspace strip; and nothing, on the grounds that a shell should persist its
  own arrangement and gyro's containers exist to keep the *windows* in place rather than to be a
  database. The third is the one to argue against first, because a shell's persisted file and gyro's
  live tree can disagree and the person is looking at gyro's.

- **Whether a shell's containers nest.** *(New 2026-09-06, same entry.)* A declared container is a
  child of the session's floor and nothing else, which is enough for a workspace and is not enough for
  a grid inside one. What decides it is whether the arrangements that want two levels are the ones a
  shell would build — an overview that scales a whole workspace is one level and a reference node, and
  a dock with a fisheye inside a panel is two — and the cheap half is already true, since
  `SceneStore::Reparent` refuses a parent inside the subtree being moved and so a nested container
  cannot make a cycle whatever the wire allows.

- **Decision 95's reference kind is not on the wire.** *(New 2026-09-06, same entry.)* An overview
  showing a live window in two places at once is what the kind exists for, and `gyro_scene_v1` has no
  request that mints one. What it needs stated is the backward-index rule — a reference's target must
  be authored before it, which is an ordering constraint on a protocol that otherwise has none — and
  the hiding of the originals, which decision 95 says is done by hiding their *parent* rather than each
  of them. Both are sayable; neither is obvious enough to guess at, and an overview is the thing that
  would settle them by being written.

- **Layout, and how little of it is gyro's.** Decision 89 puts layout in phase two and calls it a
  pass at close, which reads as though gyro has a layout engine. It should not: decision 51 makes
  window-management policy the shell's, so what runs at close is only *derived geometry* — an anchor
  coordinate resolved against a node's extent, and whatever else turns out to have the same shape.
  Tiling, stacking, snapping, and where a new window goes are the shell's, with gyro holding the
  constraints decision 51 says the shell declares ahead of time. Deliberately deferred: the boundary
  is stated here so that the first derived quantity does not quietly become the second, and settling
  it properly wants the constraint set that entry already owes and the gesture vocabulary below.
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
  whether the first awkward case adds a per-event request and quietly undoes it. *(Annotated
  2026-08-29.)* Drag is now built with that set **empty**, which is the honest reading of *the shell
  declares and there is no shell*: `xdg_toplevel.move` starts a gesture gyro runs at pointer rate,
  and the window goes exactly where the hand goes with nothing clamping it. What a person can do
  that they should not is drag a window off the top of a screen and have to drag it back — the
  smallest possible version of the missing declaration, and the one worth living with because every
  alternative is gyro picking the number. That the mechanism half works with no constraints at all
  is evidence for the split rather than against it; the question the enumeration still has to answer
  is resize, where a minimum and a maximum size are already on the wire and arrive from the *client*
  rather than from a shell. *(Annotated 2026-08-29.)* Resize is now built too, on
  [decision 166](Decisions.md#166-a-resize-is-a-request-the-client-owns-the-extent-gyro-owns-the-anchor),
  and it answers the half of this entry that was about to break the rule: the only bound it honours
  comes from the party being resized rather than from a policy, so nothing about it wanted a
  per-event request and the mechanism side stayed declarative. What is still unenumerated is the
  shell's own list — snap targets, tiling gravity, the edges a window may not cross — and the case
  that will settle whether the rule holds is now the first one that needs *two* of them at once,
  since a snap target and a minimum size can disagree and only the shell knows which wins. The gesture
  vocabulary itself — which gestures exist, and what each binds to — belongs with the scene and
  material vocabularies above and for the same reason: a gesture, the transition it drives, and the
  nodes it moves are one design problem seen three ways.
- **Color format for virtual outputs.** Encoders want NV12 or P010, not RGBA. The agent can convert
  (an extra full-frame pass and its bandwidth), or gyro can fold RGB→YUV into its final composite
  pass (much cheaper, but the renderer grows a YUV output path it otherwise would not have), or both
  can be offered. HDR sharpens it — P010 and transfer functions. This is the one part of decision 26
  with a real performance number attached and it should not be decided from the armchair.
  *(Annotated 2026-09-06.)* The first consumer converts, and that is a starting position rather than
  an answer: [decision 200](Decisions.md#200-a-client-registered-virtual-output-renders-into-a-ring-the-client-allocated-and-the-descriptors-reach-the-frame-thread-the-way-a-texture-does)
  puts the ring in the client's hands, so a client wanting NV12 today allocates NV12 and does the pass
  itself, where the cost is visible in its own numbers and not in gyro's. What settles the entry is
  that measurement — a real encoder, a real frame size — against the same composite ending in a YUV
  target, and the fused path is worth building exactly when the figure says so.
- **A nested session types gyro's layout rather than the surrounding session's.** *(What is left of
  this entry after
  [decision 173](Decisions.md#173-nested-gyro-takes-input-from-the-hosts-seat-and-a-host-window-is-an-absolute-device-bound-to-the-output-it-is)
  built the pointer and
  [175](Decisions.md#175-a-nested-keyboard-forwards-keycodes-and-nothing-else-and-focus-leaving-releases-what-was-held)
  built the keyboard. The handoff is a bounded oldest-wins ring behind an `IEventSource`, rung once
  per host drain, dropping at the tail because the producer runs where allocating is an abort; the
  keyboard rides it as keycodes with the host's keymap dropped.)* 175 settled the mechanism and left
  the consequence: gyro compiles its own layout from `XKB_DEFAULT_*`, so a person who selected Dvorak
  in their desktop and nests gyro inside it types QWERTY unless they set the variable too. That is
  correct for testing gyro's own keymap path and wrong for the person doing the testing, and the two
  candidate answers are opposite — read the host's keymap and forward it, which needs a channel from
  `Nested` to `Protocol` across the whole waist and puts a layout in the one path built to have none,
  or read the host's `XKB_DEFAULT_*` environment, which gyro already inherits and which is what a
  toolkit would use. The second is nearly free and may simply be what already happens; nobody has
  checked. Separately, the host's motion has already been through the host's acceleration curve, so
  gyro's own curve and warp path are unexercised under the backend everything is developed on;
  `zwp_relative_pointer_v1` with a lock is the fix and costs a pointer that cannot leave the window.

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

  **Answered: neither. The machine is a connection root holds on the socket that already exists.**
  *(2026-09-06, by [decision 202](Decisions.md#202-the-machine-is-a-second-kind-of-control-connection-held-by-root-and-it-states-a-request-gyro-satisfies-when-it-can).)*
  Decision 59's three reasons for the greeter's agent are all equally true of the login agent, and one
  thing separates them: the greeter takes keystrokes from whoever is standing at the machine, and
  assignment is what locking is built out of. So the peer is root, checked by the `SO_PEERCRED` the
  control socket already reads, and a second socket was rejected as a weaker gate than the one being
  made. **What is still open is the suspend half**, which that decision has crossing this connection —
  it is now the login agent's connection rather than the greeter agent's, and nothing is built.
- **The idle CI assertion.** Decision 58 makes "no timer armed when nothing is animating"
  falsifiable and therefore testable, but a static scene in a headless harness is not obviously the
  same static scene a real desktop presents — a clock in the shell's panel ticks once a second
  forever. What the test fixes as "idle" needs deciding before it is written, or it passes on a
  scene nobody runs.

  **Half of it is written and the half that is missing is exactly the half this entry names.**
  *(2026-08-23, by
  [decision 122](Decisions.md#122-the-wake-fold-is-scene-wide-and-replicated-per-output-a-settled-channel-retires-where-it-is-published).)*
  `Source/Integration/SceneIdle.Test.cpp` runs an authored scene through a real serializer and a real
  frame loop and asserts both ends: a still scene draws nothing and arms nothing, and an animating one
  is drawn every frame and then stops. What it cannot assert is that the scene it runs is a scene
  anybody has — it is one window and one commit — which is this entry unchanged. The mechanism it
  needed is no longer in the way, so what is left is choosing the scene.

  **And the assertion now runs against two threads and a real ring, which is where it will actually be
  made.** *(2026-08-23, by
  [decision 127](Decisions.md#127-idle-is-both-halves-at-rest-and-only-the-composition-root-sees-both).)*
  `--frames=N --gym=settle` authors once, animates to completion, and ends because the machine went
  quiet rather than because the counter ran out — the same claim as the integration test with a real
  boundary and a real dispatch thread between the halves. What that makes CI-ready is the *harness*.
  The scene is still one gyro authored for itself, so this entry is unchanged in the part that matters
  and there is now somewhere obvious to put the answer once it is chosen.
- **Backlight without a backlight.** Decision 58 dims via `/sys/class/backlight`, which external
  monitors do not have; DDC/CI is the usual answer and it is slow, unreliable, and needs I2C access.
  Whether an external output dims at all, and what the composite-side fallback is when it cannot, is
  open — and the fallback is the alpha overlay decision 58 rejects for internal panels, so admitting
  it here needs an argument this entry does not have.
- **Virtual outputs for a session that does not exist yet.** Remote login goes through the
  privileged login agent, as the greeter does, but the ordering against decision 24's listener
  handover is not worked out.
- **A display's peak luminance, which is what HLG needs and `ColorState` does not carry.**
  [Decision 116](Decisions.md#116-the-pointwise-lattice-is-two-run-bits-and-a-conversion-selector-and-the-outputs-colour-state-moves-to-the-binding)
  refuses `TransferFunction::Hlg` on both ends of a conversion rather than approximating it: BT.2100's
  scene-to-display step is `γ = 1.2 + 0.42·log10(Lw/1000)` and `Lw` is the display's peak, while a
  color state states a *reference white*. Deriving one from the other goes through BT.2408's
  conventions, which is a policy chain rather than an arithmetic one. What is open is whether the peak
  belongs on `ColorState`, on `OutputConfiguration` beside it, or in the per-output characterisation
  the primaries question below already wants — and the answer decides whether an HLG *source* and an
  HLG *output* are one question or two, since a client declares one and a panel reports the other.
- **Gamut mapping, where today there is a clip.** A conversion into narrower primaries produces
  negative components — Bt2020's red is a long way outside Bt709's — and decision 116 clips them at
  zero, which desaturates rather than losing the colour and is the same picture on every driver. What
  a clip cannot do is preserve the *relationship* between two out-of-gamut colours: a saturated red
  and a slightly more saturated one land on the same pixel value, so a gradient across the boundary
  bands. It wants a curve, a curve wants a perceptual argument, and the argument wants a wide-gamut
  panel to make it in front of. Same review as the dressing numbers below.
- **The dressing numbers, which are what the review with a screen is actually for.**
  [Decisions 103 and 104](Decisions.md#103-a-dressing-is-named-by-what-it-does-to-light-the-material-set-is-glass-and-smoke)
  fix which materials and which levels exist and what each is for, and deliberately fix no value:
  `Glass`'s radius and tint, `Smoke`'s radius and the worst-case contrast floor its opacity is derived
  from, the two heights, and the two constants the one light turns a height into —
  [decision 129](Decisions.md#129-a-height-is-the-offset-and-the-light-is-two-constants-its-size-and-its-weight)
  fixes what those two constants *are* and leaves every value here. The contrast floor
  is the only one of these that is not taste — it is a legibility threshold against a video frame,
  and it wants measuring rather than choosing. The rest want GTK, Qt, and an Xwayland application on
  a screen at once, which is the same sitting the entry below wants.
- **Whether a rotated node's shadow shears or rides its quad.** One light and a node turned out of
  the plane disagree: the physical answer shears the shadow, and the cheap one carries it on the
  quad, where a card mid-flip lights itself from the side. A card flip is the only arrangement that
  reaches it and it is a transition nobody has written, so this arrives with the first one that turns
  a node rather than before it. **It is the tilt alone**: a node *spun* in the plane is a photograph
  lying at an angle, its shadow falls down the screen like everything else's, and
  [decision 133](Decisions.md#133-a-node-spun-in-the-plane-is-not-a-node-tilted-out-of-it) draws it.
  What is left open is the card mid-flip, where the node is further from what it falls on at one end
  than the other — and the renderer refuses that rather than drawing one of the two answers, which is
  what keeps this open instead of settled by whichever was easier to write.
- **Where the minimum corner radius sits.** Decision 96 rounds every window to at least a floor, and
  the floor is a number nobody has looked at. Too high and it cuts inside toolkits that already round
  generously, which costs an antialiased edge and produces the corner artefact that entry names; too
  low and it does nothing for the square-cornered population it exists to serve. It wants a screen
  with GTK, Qt, and an Xwayland application on it at once.
- **The frame-parameters protocol, and what gyro proposes rather than works around.**
  [Decision 96](Decisions.md#96-the-frame-is-the-compositors-and-the-header-is-the-apps) says the
  exit from decision 48's residue is a hint that lets a client declare it has stopped drawing its own
  shadow, and publishes gyro's frame parameters the other way — radius, focus treatment, control set
  and order. Nothing about it is designed: not the interface, not whether it extends
  `xdg_decoration` or replaces it, and not whether it is worth proposing before gyro can demonstrate
  the result. The last of those is the real question, since a protocol argued from a design nobody
  has run is the kind that gets one implementation and no adopters.
- **Whether the crop comes back.** Decision 96 rejects cropping a client's buffer to its window
  geometry because the verification that would make it safe — refuse to crop a margin holding opaque
  texels — is a GPU readback on the dmabuf import path. Two things would reopen it: a client that has
  *said* it draws no shadow, at which point there is nothing to crop and the question dissolves; or a
  cheap way to characterise a dmabuf's margin at import, which is a rendering question rather than a
  protocol one. Worth re-reading when the import half of the renderer is written, since that is where
  the cost would actually land.
- **The damage verifier's waste threshold.** Decision 63 asserts that declared damage is not
  "grossly larger" than what changed, and grossly is not a number. Too tight and every gathering
  material fails on content that happens to be static; too loose and the silent direction stays
  silent, which is the whole reason the verifier exists. Probably a ratio with a floor area, and it
  should be calibrated against a real scene rather than chosen.
- **Damage region complexity cap.** The rectangle count past which decision 63 collapses a region to
  its enclosing bound. One of the few numbers in this list that is cheap to obtain, and it wants
  obtaining before the region implementation is tuned around a guess.
- **What may instantiate what.** Decision 64 puts the capture permission on the primitive and does
  not say how the check is expressed. *(Placed but not designed, 2026-08-22, and deliberately
  deferred with the rest of the scene's shape.)* Decision 88 moved the check to commit time on the
  dispatch side, and
  [decision 111](Decisions.md#111-an-entity-is-a-nodes-authoring-side-the-store-is-one-tree) gives it
  what it checks against — every entity has exactly one author, so a `Reference` whose target was
  authored by another session is a comparison rather than a lookup. What is still missing is how a
  connection's trust level is *expressed*, which is the System-tier listener question below rather
  than a scene one. It is the same question as the System tier's listener below —
  a trust level per connection deciding an operation — and the two want answering together, because
  a thumbnail of another session and an output moved between sessions are one bypass at two sizes.
- **Instance count in the atlas.** *(Halved 2026-08-22.)* Decision 64 made instances an axis of
  decision 29's allocation and of decision 46's sizing;
  [decision 88](Decisions.md#88-an-instance-is-a-node-the-published-scene-is-a-dag) **withdraws the
  budget half** — a reference node is area, which `Admit` already prices — and leaves the atlas half
  standing. Overview entry is still the worst case for it: a thumbnail per window, in a frame that
  may also be retiring surfaces. It belongs in the same derivation as the atlas multiple above rather
  than in one of its own.
- **`C_min` as a number, and whether the floor composite can earn its place.** Decision 35 made the
  floor composite's cost the bound on recoverable overrun, which made it a target rather than a
  measurement. [Decision 191](Decisions.md#191-late-arrival-selects-a-frame-never-a-tier--the-record-time-check-is-planned-or-wait)
  takes the frame path away from it, so `C_min` no longer bounds anything on that timescale and the
  question is now what it is worth as decision 34's bottom rung. The first measurement is not
  encouraging: on a nested output with one dressed panel, planned is 2.34 ms and floored is 1.71 ms,
  so `C_min` is **73% of `C_planned`** and the whole of rung 3 buys 27%. The base composite dominates
  and no effect change touches it. If that ratio holds on a real panel with a real scene, the ladder
  ends at rung 2 and the floor composite is a test and golden-image mode only — which is what
  [Architecture.md](Architecture.md#the-floor-tier) already says it primarily is, arrived at from the
  other end. What would change the ratio is the dressing composite, 0.64 ms of the 2.34, which is
  paid whether or not there is a chain under it.
- **Whether a planned composite may hold the previous frame's blur.** Decision 191 rejected this as a
  *floor* and named it an optimisation of the planned composite instead, on the grounds that a
  composite which looks the same is not a degradation rung. It is worth what flooring was worth —
  reusing the chain result saves the extract and the blur, 0.64 ms, within 0.01 ms of what flooring
  saved — for no change of appearance while the backdrop behind the glass is still. What is not known
  is the moving case: the held result is registered to the previous frame's backdrop, so under a
  window in motion the blur lags its own glass, and whether that reads as softness, as sliding, or as
  nothing at all wants the same thing the entry it came from wanted — a panel and somebody watching.
- **Scheduling policy constants.** The `// SPEC:` numbers in `FrameClockPolicy`, `TimingPolicy`, and
  `BudgetPolicy` cite this entry and it had never been written. They are the clearance the
  variable-refresh servo keeps from the panel's longest period, the servo's per-frame step bound, the
  safety margin held over a composed reserve, and the length of the window a budget's mark is the
  maximum over. What makes them one entry rather than four is their provenance: every one is read
  from documentation and driver source rather than from a panel, which is why they are policy fields
  a test can vary rather than constants compiled into the arithmetic.

  **The latch lead was the fifth and is
  [decision 159](Decisions.md#159-the-latch-lead-is-the-modes-blanking-interval-plus-the-drivers-commit-path-and-it-belongs-to-the-output-rather-than-to-policy),
  which broke two of this entry's premises and both are worth holding against what is left.** The
  first is that every unset value is the recoverable direction: for the latch lead it was not a
  margin but a cliff, with a latch rate of 0.0% below the threshold in every mode measured, and the
  only thing hiding a permanently floored output was a reserve planning for more work than the
  composite did. Ask of each remaining number whether being wrong is linear or total, rather than
  assuming the first. The second is that retiring these is "a bench with panels on it and not an
  afternoon of reading": it took one panel advertising two modes with different pixel clocks, which
  most panels do, because a second mode separates quantities that are proportional within one. That
  is the cheap instrument the other four should be tried against before they are deferred again.
- **The latch lead wants to be a ratchet.** Decision 159 leaves `Drm/Output.cpp`'s `CommitPath` a
  compiled-in 500 microseconds against a measurement of 288 to 387, and the part it is covering is a
  commit worker being scheduled — which grows under load, on a machine whose load gyro does not
  control. The shape is settled and the mechanism is not built: start high, step up on a miss. Never
  a servo, because the threshold is observable only by failing and the failure is every frame rather
  than a worse one. What it needs from the trace is already there — a commit's distance to the anchor
  it was aimed at, and whether the refresh that showed it was the one it named — and what it needs
  from the loop is a place to keep a per-output figure that a mode set drops, which is where the
  number already lives.

  **Coming down only on an invalidation is wrong, and measuring it is what showed why.** Decision 159
  proposed that, on the reading that the figure describes a panel. It does not: on a quiet machine
  the default misses 0 of 1449 commits at 60 Hz and 1 of 1426 at 40, while under a parallel build the
  same panel missed 2.67% in bursts — so what the ratchet tracks is *the machine's load*, by way of a
  kernel worker gyro cannot raise
  ([KernelWishlist.md](KernelWishlist.md#the-commit-that-must-beat-the-vblank-runs-at-a-priority-gyro-cannot-raise)).
  A disturbance lasting as long as a build must not cost a person pointer latency until the next mode
  set, which on a compositor that is a boot service may be days. So it wants a decay on the timescale
  of the disturbance rather than of the session, and choosing that timescale is the part that is
  actually open: too fast and it saws against a steady load, too slow and it is the invalidation rule
  again wearing a clock.
- **A miss that margin does not explain, at roughly one frame in fifteen hundred.** Distinct from the
  ratchet above and not answered by it: at an arming lead of 2.5 ms — a full millisecond past the
  point where every other commit in the capture latched — one frame of 1448 still landed a refresh
  late, with its composite's fence signalled 3.6 ms before the deadline. An earlier capture saw the
  same at 1 in 1923. Being unmoved by margin is what makes it a different defect, and what makes more
  lead the wrong instrument for it. The candidates worth separating are a vblank the kernel skipped, a
  completion delivered twice, and gyro's own sequence bookkeeping across a flip that reported nothing;
  the first two are visible in a capture recording the kernel's sequence against gyro's own on every
  event, which the ring now does. So this wants an accumulation of long quiet captures rather than an
  argument — and the catching is no longer by hand: the event recurs about as often as the ring is
  long, so waiting with a finger on `SIGUSR1` meant catching the right half-minute by luck.
  `--trace-on-miss` arms the loop to make the request itself the moment a flip lands past the refresh
  its frame named, once per run, and marks the vblank `landed late` so the frame is findable by name —
  including in a system trace merged over the file, which is where the kworker the wishlist entry
  blames would actually be seen.
- **The snapshot atlas multiple.** Decision 46 denominates capacity in output render-target
  equivalents and declines to guess the number. The derivation to check it against is the largest
  *legitimate* simultaneous retirement — closing an application with a menu open is a window plus
  two small surfaces; a nested menu chain is three or four small ones; closing a workspace's last
  window retires the workspace too. Logout retires everything, and should be one session-level
  transition rather than N window exits, which is a design constraint falling out of the same count.
  The number is then confirmed by instrumentation, not argument: per-output atlas high-water and
  eviction count, tracked the way decision 29 tracks the budget. An eviction outside a stress test
  means the multiple is wrong.
- **The last-good-frame for unresponsive clients.** Showing an application's last good frame while
  it is hung is a real feature and the same shape as an exit snapshot, but with an unbounded
  lifetime. It is the case that tests decision 46's admission rule, so it is worth deciding whether
  gyro wants it at all before the rule is bent to admit it.
- **The capability probe.** What it renders, at what sizes, how long it may take at startup, and how
  its result maps onto tiers. It runs on every boot of a system-layer process, so it has a latency
  budget. **Half of it now exists and has already spent that budget**:
  [Render/Governor.h](../Source/Render/Governor.h) is
  [decision 142](Decisions.md#142-gyro-states-the-deadline-and-commands-the-clock-only-where-the-deadline-does-not-reach-it)'s
  frequency probe, it runs the real pass chain off the frame path exactly as this entry wants, and it
  costs about half a second. So the open question is no longer only what a capability probe renders but
  whether the two are one probe — the frequency arms already produce a per-machine cost figure at a
  known clock, which is most of what seeding `BudgetPolicy` wants — and, separately, whether a boot
  service may remember either answer rather than re-measuring it every boot. **The floor composite belongs in what it renders**, at output resolution, because `C_min`
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

  **The assignment half is now expressed, and it is not a global at all.** *(2026-09-06, by
  [decision 202](Decisions.md#202-the-machine-is-a-second-kind-of-control-connection-held-by-root-and-it-states-a-request-gyro-satisfies-when-it-can).)*
  It is a message on the control socket that only a root connection may send, so it is unreachable
  from any client at any tier rather than reachable from a privileged one — which is a stronger split
  than this entry asked for. What is unchanged is the other half: output *configuration* is still
  owed a design, and putting it in the System tier is still the plan.
- **The System tier needs its own listener.** Also surfaced alongside decision 44. Trust level is
  per connection, connection identity comes from the listener (decision 23), so trust is a property
  of the listener — which means a `System` connection cannot arrive through the ordinary per-user
  socket. That implies a second listener per session with different permissions, and who creates it,
  where it lives, and how a client is judged worthy of it are all unspecified. Decision 51 promotes
  this from speculative to load-bearing: the shell is its motivating occupant, and "which process
  gets to be the shell" is exactly the judgement this listener has to encode.

  **The tier now has a global in it, and nothing can reach it.** *(Annotated 2026-08-30.)*
  `ext_foreign_toplevel_list_v1` is served, filtered and tested — a client admitted with
  `Trust::System` is told about the windows in its session, and an ordinary one is not offered the
  interface at all. What produced `Trust::System` was a test. So the entry has stopped being about a
  mechanism nothing needs and is now about a global that works and is unreachable in a real run, which
  is the state where the answer is cheapest to change and most obviously owed. The narrower question
  underneath it is the development case: a run under `--gym` or a bound socket has no agent, so
  whatever a session's System listener turns out to be, gyro also needs a way to be its own — and that
  half touches no ABI.

  **The development half is answered and the entry is now only about the session.** *(2026-09-05 by
  [decision 183](Decisions.md#183-gyro-binds-its-own-system-listener-beside-a-self-bound-socket-and-that-is-the-development-half-alone).)*
  A run that binds its own Wayland socket binds `gyro-system-N` beside it and grants every client that
  arrives there `Trust::System`, so a shell has somewhere to connect and the foreign-toplevel round
  trips run over it. A run under `--control` binds neither, which is the refusal that keeps the answer
  confined to the configuration where gyro, the shell and every client are one person anyway. What is
  left is the whole of what the entry was about — which listener carries System trust for a real
  session, who creates it, where it lives, and how a process is judged worthy of it — and it is still
  an ABI between gyro and the session agent rather than something gyro can settle alone.

  **Answered, and the entry closes.** *(2026-09-05 by
  [decision 189](Decisions.md#189-a-session-is-offered-whole--every-listener-it-has-each-with-the-role-it-plays--and-the-shell-is-handed-a-descriptor-rather-than-a-path).)*
  An agent offers its session whole — a listener per role, in one message — and the `Shell` role is
  the listener that carries `Trust::System`. Who creates it: the session agent, because trust belongs
  to the socket and gyro cannot put a file in a user's runtime directory. Where it lives: beside the
  display, named after it, with no lock and no name in any environment. How a client is judged worthy
  of it: the agent started it, and the socket's path is never told to anything else — the agent
  connects on the shell's behalf and hands over the descriptor. What that is worth is a boundary
  between users and none within one, so **the trust is per user rather than per program**; a judgement
  about the executable is a separate mechanism and is the entry below.
- **`Trust::System` is per user, and a judgement about the program is not built.** Left standing by
  decision 189. A process at the session's own uid that finds the shell listener's path can connect to
  it and be granted the run of the session; the path is never exported, so this takes looking rather
  than inheriting, and nothing stops it. It is not obviously worth closing — a process at that uid can
  `ptrace` the shell and use its connection directly — but the pieces exist if it is: `SO_PEERPIDFD`
  gives a peer's process a handle that cannot be recycled out from under the check, and what would be
  asked of it is an identity the kernel vouches for rather than a path the peer chose. What is missing
  first is a reason: a threat this closes that `ptrace` does not already open.
- **A shell cannot compose itself out of more than one process.** Surfaced reading decision 189 back
  rather than while writing it, which is why it is here instead of in the entry. The System listener is
  per *session* and accepts as many clients as connect — `Server::OnConnection` admits each one with
  the same session and trust, and nothing caps it — so several System clients per session is already
  the mechanism. What is missing is a way for a second one to arrive: the agent connects on the shell's
  behalf and the shell is never told the path, so only the agent can add anybody and only at session
  start. It cannot even be done crudely, since two clients sharing one `wl_display` share an object id
  space.

  That is a limitation decision 189 created without arguing for it, and the tree already assumes the
  opposite: [decision 187](Decisions.md#187-a-shell-declares-a-surface-to-be-chrome-once-and-that-one-word-takes-it-out-of-the-floor-the-walk-and-the-list)'s
  focus walk is written for "a panel and a launcher up at once", and a notification daemon that crashes
  should not take the panel with it.

  The candidate is **delegation on the wire**: a request on the shell's own System connection asking
  gyro for a fresh connection, answered with a descriptor the shell passes to a child it spawns. One
  connection per call and never a listener, since handing back a listener would put a path back in the
  world. It does not contradict trust belonging to the listener — an application cannot call it,
  the global being System tier, so what the shell does is *share* trust it holds rather than acquire
  trust it does not, which is delegation in the ordinary capability sense. The distinction is worth
  writing into [Protocol/Tier.h](../Source/Protocol/Tier.h) when it is built, because the two look
  alike at a glance. Rejected as the alternative: the agent starting every component from a repeatable
  `--shell`, which needs no mechanism and works today, but puts the composition of a desktop in the
  command line of a process that knows nothing about shells and cannot start anything in response to
  anything.

  Two things it does not fix, and neither is new: gyro cannot tell a delegated child from the shell
  afterwards — as it cannot tell the shell from the agent, which is the per-user entry above — and a
  delegated connection lives until it closes, there being no revocation. **Nothing is built and
  deferring costs nothing**, the whole of it being one request on a global that already exists: no ABI
  change and no change to the agent. What is missing first is a shell that wants to spawn something.
- **How a shell the agent did not start gets its socket.** Also left standing by decision 189.
  `gyro-session --shell` binds the second listener and starts the shell on a connection to it, which
  works because one process does both. The login agent's case is an agent with an empty command whose
  session manager starts the shell later, and it has no descriptor to hand anybody. Telling it the
  path is the obvious answer and is the one decision 189 rejected, since a path in an environment is
  a path every program the shell launches inherits.
- **When gyro reads an X11 client's frame extents, relative to the buffer they describe.**
  [Decision 106](Decisions.md#106-an-x11-client-has-no-window-geometry-gyros-window-manager-computes-the-frame-rect)
  found that no window geometry crosses from Xwayland at all, so gyro's own window manager reads
  `_GTK_FRAME_EXTENTS` off the X connection while the buffer arrives on the Wayland one, with
  nothing ordering the two. A resizing GTK window can therefore present a frame whose corners and
  shadow are cut at the previous extents — wrong for a frame or two, at the moment the eye is on the
  window edge. `_XWAYLAND_ALLOW_COMMITS` is the lever, since gyro is one of the two clients allowed
  to write it and clearing it holds Xwayland's commit until the property has been read; whether a
  stall per resize frame is worth paying is the question. Wants the X11 half to exist first.
- **Xwayland has no owner.** It must run as the user, so gyro cannot spawn it, which means decision
  24's session agent is a persistent agent rather than a one-shot fd donor. gyro can own the X
  sockets — `/tmp/.X11-unix` is world-writable — and allocate display numbers, which a
  machine-global compositor is uniquely placed to arbitrate; the fork must be delegated.
- **`WAYLAND_DISPLAY` versus `systemd --user` start order.** `pam_systemd` starts the user manager
  before the session agent binds a socket, so user units do not inherit the variable. The agent must
  import it before starting `graphical-session.target`, and everything graphical must be ordered
  after that target. Decision 24 predicts this bug class; this is its concrete form.

  **Confirmed on a running machine rather than predicted.** *(2026-09-06, bringing up
  `gyro-autologin@.service`.)* With gyro on the panel, an agent offering, and a shell connected and
  drawing, `systemctl --user show-environment` has no `WAYLAND_DISPLAY` and `graphical-session.target`
  is inactive. Nothing is broken today because the agent hands its shell a connected descriptor and
  the shell is the only client — which is exactly why this wants fixing before there is a second one,
  since the first symptom will be a portal or a notification daemon that cannot find the display. What
  is still open is the mechanism: the obvious one is the agent running `systemctl --user
  import-environment WAYLAND_DISPLAY` and then starting the target, which costs `gyro-session` an
  `exec` of a program it otherwise has no need of, and wants a flag so a development run does not
  reach into somebody's user manager.
- **The session-ready signal.** Decision 51 will not reassign an output until the incoming session's
  shell has presented, and the same signal is what stops login assembling in visible stages. What
  counts as presented, which client is authoritative when chrome is several clients, and what
  happens when a shell never presents at all are all unspecified — and the last is the one that
  matters, since the answer cannot be "the output never arrives".

  **The first of those is now answered, and the answer is that "presented" was the wrong word.**
  *(Narrowed 2026-09-06 by [decision 202](Decisions.md#202-the-machine-is-a-second-kind-of-control-connection-held-by-root-and-it-states-a-request-gyro-satisfies-when-it-can).)*
  A session on no output is not composited — decision 21 gives it no frame callbacks and
  `Protocol/Floor.h` does not place its windows — so a session waiting to be shown can never present,
  and a gate on presentation would never open. What is wanted is that **the shell has committed a
  buffer**, which is knowable for an unshown session and is what makes the reassignment not a blank.
  Two things are still open and both are as this entry left them: which client is authoritative once a
  shell is several of them, and what happens when one never draws at all. On the last, the shape that
  falls out of decision 202 is that the request simply waits and the greeter holds the screen, which is
  a machine that stays at a login prompt rather than one whose output never arrives — but nothing is
  built either way, so it is a proposal rather than an answer.

  **Nothing is built and login shows a beat of empty session until it is.** The mechanism it needs is a
  fact carried from `Protocol` to the composition root that does not exist: gyro's client host knows a
  surface committed, and nothing tells the root that a *session* now has something to draw.
- **The machine peer cannot enumerate outputs.** Decision 202 has a login agent naming a screen by its
  connector — `eDP-1`, `DP-7` — or naming none at all and meaning every one of them, and `Managing`
  carries no list of what the machine has. So a peer that wants to place two people on two monitors has
  to learn the names from somewhere that is not gyro, and a name that matches nothing waits for ever
  rather than being refused. The reason it is not a field is that it cannot be one: an enumeration that
  stays true is a message now and an event on every hotplug, which is a channel with an ordering
  problem against the assignment it is meant to inform. Wants designing alongside output
  *configuration*, which has the same shape and the same audience.
- **Lock and unlock are not reachable by the machine peer.** Decision 202 puts assignment on the
  control socket and stops there, so the only thing that locks a screen is the development chord and
  the only thing it can lock to is gyro's own scene. The verbs exist on the store and the messages
  append cleanly; what is missing is a greeter session to lock *to*, which is the login agent's half.
  Worth doing together with it rather than before, since a lock message with nothing behind it cannot
  be exercised.
- **Per-output wallpapers.** Users set different backgrounds per monitor, and decision 51's cache is
  written and read before the shell exists, when no per-output intent has been expressed. Keying the
  cache per output also means identifying outputs across boots from EDID, which is unreliable with
  two identical panels. Deliberately left entirely open: the live case and the cached case may want
  different answers, and nothing else waits on it.
- **The background cache's lifecycle.** gyro accumulates one persisted image per uid in its own
  state directory, outside the user's control. Deleted accounts leave images behind, a machine with
  many users accumulates a bounded but unstated amount of disk, and nothing says what the cap is or
  who prunes. Small, and the kind of thing that is never written down unless it is written down now.

  **The per-uid keying has lost one of its two consumers and the question is now whether it has
  one.** *(Narrowed 2026-09-05 by
  [decision 188](Decisions.md#188-a-session-transition-is-a-live-cross-fade-the-output-owns-and-the-cut-is-its-absence)'s
  companion revision to [decision 43](Decisions.md#43-lock-and-greeter-are-one-ui-locking-is-an-output-reassignment).)*
  Locking used to want the wallpaper of the user who locked the output and now shows the system's, so
  what is left asking for a per-uid image is login — [Experience.md](Experience.md#one-continuous-image)
  promises that after authentication the screen shows *that user's own background*, before their shell
  has handed one over. On a single-user machine one image keeps that promise by itself. On a shared
  one it does not, and the choice is between keeping the cache keyed for a case that only appears with
  a second user on the machine, or weakening the promise to *the machine's background, then yours*.
  Nothing is built either way, and the entry above is the same disk question at whichever cardinality
  this lands on.
- **Whether logind accepts a VT-less graphical session on `seat0` .** The login agent registers
  sessions through `pam_systemd`, and decision 37 has no VTs to give it. Needs testing, not
  assuming.

  **The thing that will test it now exists and has not been run.** *(2026-09-06.)*
  `Deploy/gyro-autologin@.service` reaches `pam_systemd` through `PAMName=`, so booting a machine into
  it is the experiment. It has not been performed: it needs root and a machine whose display is free,
  and the development machine it was written on has neither at once. Until it is run, *gyro works
  under a session agent started by hand* is what has been shown, and the PAM half is unexercised.
- **BGRT reproduction.** Scaling and placement from the firmware's mode into gyro's, and what to do
  when the firmware framebuffer and the native mode disagree about aspect ratio. *(Narrowed
  2026-08-22.)* `simpledrm` offers exactly one mode and it is the firmware's
  (`drm_connector_helper_get_modes_fixed`), so at the first modeset the reproduction is a blit at
  the recorded offsets with no scaling in it at all. The scale-and-place lands at the migration to
  the real driver instead — under Vulkan, where there is a device to do it. What is left of this
  entry is the aspect-ratio half, and the two entries below are where the cost of that migration
  actually shows.
- **The seams at the driver handoff that cannot be hidden.** Two of them, wanting one answer
  because the lever is the same. The `simpledrm`-to-real-driver migration reprograms the pipe
  during the new driver's probe, before gyro holds an fd on it; and where the firmware's mode is
  not the panel's native mode, adopting native re-locks the panel. Ramping the backlight down
  before the commit and up after hides both, and gyro owns the backlight because it is the system
  layer — but an external display has no lever fast enough to time against a modeset, so there the
  honest answer is a stated exception in [Experience.md](Experience.md#one-continuous-image)
  rather than a mechanism. Fading pixels to black is *not* the answer: nothing can be painted
  during the blank, so it hides only the entry into it and makes the dark period longer. Surfaced
  by [decision 110](Decisions.md#110-blit-never-reads-its-target-and-nothing-moves-under-it-until-there-is-a-real-flip), which records the probe half as unread.
- **The wire colorimetry, and when it is chosen.** A framebuffer format change flips freely and
  costs nothing; changing a connector's colorimetry and `HDR_OUTPUT_METADATA` makes the sink
  re-lock, which is a real black period and the one transition at the handoff that genuinely
  breaks [one continuous image](Experience.md#one-continuous-image). So it cannot be a login-time
  or preference-time event. The candidate answer is that colorimetry is a property of the panel,
  read from EDID at the first modeset and never a setting — an HDR-capable panel driven in PQ
  from the very first frame, logo included, with
  [decision 47](Decisions.md#47-compositing-happens-in-linear-light-at-wide-primaries)'s
  brightness-relative composite space making SDR content on that wire definitional rather than a
  conversion. Against it: some panels are genuinely worse in HDR mode — raised blacks, forced
  processing, backlight behaviour — which is per-panel characterisation and wants the same review
  with a screen the dressing numbers do. It also decides which formats `Blit` encodes, which
  [decision 110](Decisions.md#110-blit-never-reads-its-target-and-nothing-moves-under-it-until-there-is-a-real-flip) leaves open.
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

  **The shell's half now has a mechanism and needs a policy.** *(2026-08-22.)*
  [Decision 114](Decisions.md#114-retirement-is-the-author-going-away-and-resurrection-is-the-authors-alone)
  makes the shell disconnecting retire everything the shell authored and nothing any client did,
  which is decision 51's floor policy falling out of the lifetime rule rather than being built beside
  it. What it does not decide is whether those nodes run their exit transitions or simply vanish, and
  the pull is toward vanish: a desktop's whole chrome animating gracefully out is a statement about a
  crash the user should probably not be shown, and it spends a screen's worth of atlas at the moment
  the machine is least well. Where the client windows go is
  [answered](Decisions.md#141-a-window-is-parented-into-gyros-floor-and-shown-when-placed-the-floorplanner-stands-in-for-an-absent-shell):
  the container they were parented into is gyro's floor, not the shell's, so a shell restart retires the
  chrome and leaves every window in place. What is still open here is only whether the *chrome* exits or
  vanishes.
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

  **Answered by [decision 153](Decisions.md#153-a-promoted-layer-names-a-texture-id-and-a-scanout-framebuffer-is-a-second-importer-on-the-same-id-space),
  and none of the three shapes above is it.** *(2026-08-25.)* A promoted layer names the `TextureId`
  the draw item was already carrying, and a second importer beside `ITextureImporter` makes that same
  id name a framebuffer on a card. One id space, two importers, two devices that need not be the same
  one — so the frame thread performs no lookup and the composition root drives both from where it
  already drives one. What the entry adds that this one did not see is that the retirement rule is
  *not* decision 131's: a scanout framebuffer is still being read by the display engine after the
  sequence that named it went below the watermark, and `drm_framebuffer_remove` disables the planes
  using one, so releasing it on the watermark would blank a plane and take a modeset on the frame
  thread.
- **How a texture is minted, and who holds it.** *(Shape answered 2026-08-23; the minter answered the
  same day. The Vulkan arm remains.)* [Decision 82](Decisions.md#82-the-renderer-is-handed-an-evaluated-draw-list-not-a-scene)
  has a draw item name a `TextureId` and nothing in `IRenderer` creates one, on the grounds that a
  `wl_buffer` arrives on the dispatch thread and turning it into a device image must not happen inside
  the frame section — so import is the renderer's dispatch half, written when there is a protocol
  layer to call it. **The layering half was answered first, and was worse than it looked**:
  [decision 87](Decisions.md#87-a-type-both-halves-of-the-world-name-lives-below-both-waists-not-in-seam)
  found `TextureId` sitting in `Seam`, which neither `Protocol` nor `Scene` may name, so the import
  verb had no legal caller rather than merely no design — and moves the type to `Core`.

  **The shape is now
  [decision 131](Decisions.md#131-texture-import-is-a-second-interface-and-a-texture-retires-on-the-watermark),
  and one of the three things this entry called open turned out to be built already.** The release
  that has to be safe while the frame thread may still hold the id needs no mechanism: Return.h's
  watermark is the sequence the frame thread is rendering from, so a texture last named below it is
  one no frame can be sampling, and retirement rides beside the buffer releases dispatch already
  derives from that number. A failed import is a surface that is never published rather than a frame
  that draws nothing there. What also came out of writing it is that the entry had the *trigger*
  wrong — it reads as though a protocol layer is what forces the verb to the waist, and what actually
  forces it is device migration, since `Blit` had grown the verb privately and a private verb is one
  the composition root cannot call across a rebuild.

  **The minter is [decision 136](Decisions.md#136-the-texture-minter-is-dispatchs-own-and-a-retirement-is-sealed-with-the-sequence-that-stops-naming-it),
  and the entry's own observation is what decided it.** The snapshot atlas is minted from pixels that
  are about to stop existing, which is an import with no client on the other end — so the id space
  could not belong to whoever writes `Protocol`, and it went to the one module that can see minting,
  the waist and the watermark at once. What an author sees is two verbs and no `Seam`, because a gym
  stands where a client will stand and decision 87 forbids `Protocol` the waist; the party that adopts
  is the one that names a format. The `card` gym is what runs it, and it swaps its buffer forever so
  that `Forget` has a caller before there is a protocol.

  **What is left is the half with the device in it.** The Vulkan implementation is the dmabuf arm —
  an external-memory image, the format and modifier negotiation an import has to survive, and whether
  `wl_shm` wants a staging copy. Until it lands, `--gym=card` refuses to open on every backend but the
  CPU one, with `no renderer to import an image into` rather than four rectangles that draw nothing.
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

  **Narrowed to the late commit alone.** *(2026-08-25.)*
  [Decision 152](Decisions.md#152-promotion-is-a-partition-of-the-draw-list-computed-every-frame-and-a-node-is-promotable-when-its-resample-is-a-no-op-and-it-carries-no-dressing-on-itself)
  found that most of what this entry treats as missing is not: a plane's geometry is evaluated on the
  frame thread once per refresh and a client's content arrives when the client publishes, so a
  promoted plane that animates while its window redraws slowly needs no verb `IPresenter` does not
  have. The pointer is a node in the published scene like any other, and the dispatch thread wakes on
  the input event, so the newest position is in the ring before the frame thread reads it. What is
  still open is only the *late* commit — programming the cursor plane after the composite is
  recorded, against a position newer than the snapshot the frame was drawn from, which is a third
  channel across the publication boundary and wants both numbers below before it is worth one.
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

  **Numbers are now in the tree and three of the four are weaker than they look.** *(2026-08-23, by
  [decision 122](Decisions.md#122-the-wake-fold-is-scene-wide-and-replicated-per-output-a-settled-channel-retires-where-it-is-published).)*
  [Scene/Settle.h](../Source/Scene/Settle.h) carries a sixteenth of a device pixel and one device pixel
  per second for geometry, and half an eight-bit code point and one code point per second for opacity.
  The geometric pair is an argument about what decision 54's snap may move without being seen and is
  probably close to right. The opacity pair is deliberately a *representability* claim rather than a
  perceptual one — it settles what can be settled and leaves this entry's actual question open. What
  joins the entry are two substitutions the numbers rest on, both of which trade tail frames for safety
  and both of which a per-node answer buys back: the finest grid is taken over *every* output rather
  than the ones a node intersects, and a dimensionless residual is judged at the screen's half-diagonal
  rather than at the node's own bounding radius composed through its ancestors' scale. The second is
  the larger price — roughly three tenths of a second of extra tail on a small node that scales — and
  it is the one worth retiring first. Blur radius and corner radius still have no answer at all,
  because nothing animates them yet.
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
- **How the render tests are gated, and where the dmabuf comes from in CI.** *(Widened 2026-08-22:
  the renderer's own tests have landed and there are now two gates rather than one — a machine may
  lack `/dev/udmabuf`, and it may equally have no Vulkan ICD at all, which is the ordinary state of a
  minimal container. `Render/Device.Test.cpp` and `Integration/RenderImport.Test.cpp` both print a
  named skip line and pass. The question below is unchanged and is now the live one.)*
  [Decision 102](Decisions.md#102-a-virtual-output-allocates-the-buffers-it-hands-out-and-that-is-what-stands-the-renderer-up)
  puts the renderer's targets behind a udmabuf allocator and records the awkward half: `/dev/udmabuf`
  is `0600 root:kvm` and reachable on a workstation only through logind's `uaccess` ACL, so a
  container or an SSH session with no seat has the capability and not the permission. Three exits and
  none is obviously right — a udev rule shipped beside gyro's own, which is heavy for a test
  dependency; running the render tests as a member of a group CI grants; or accepting the skip and
  asserting at the CI level that they did not skip. The last is the current lean, and what makes it a
  question rather than a preference is that a green run which skipped everything is precisely the rot
  [decision 36](Decisions.md#36-frame-path-discipline-is-enforced-mechanically-not-by-review)'s
  build-time checks exist to prevent. That suite now exists and is exactly as hollow-able as
  predicted: on a machine with neither udmabuf nor an ICD, sixteen tests pass having asserted
  nothing.
- **Which rung of the allocator chain a target comes from, and whether GBM ever earns its place.**
  [Decision 151](Decisions.md#151-the-panel-allocates-its-own-targets-where-the-render-device-will-not)
  makes target allocation an ordered walk — the Vulkan export first, because it is the only rung that
  produces a tiled layout, and KMS dumb buffers last, because they are linear and always there. Two
  rungs are built and the middle one is not: GBM on the card node, which is what would produce a
  tiled buffer for a *pair* of devices rather than for one. Nothing needs it yet, and the case that
  will is multi-GPU — render on the discrete part, scan out on the integrated one, where the buffer
  has to be allocated somewhere both can reach and neither the renderer's exporter nor the display's
  dumb ioctl is that place. The question is whether that case arrives before something else does,
  because the answer decides whether GBM enters as a rung or as a dependency taken for one machine.
  [Decision 102](Decisions.md#102-a-virtual-output-allocates-the-buffers-it-hands-out-and-that-is-what-stands-the-renderer-up)
  declined the dependency once already and the reason still holds.
- **What the dumb rung costs on a working GPU, which nothing has measured.** It is selected only when
  the render device refuses to export, which today means lavapipe — and there the layout is linear
  either way, so the choice is free. On a machine where the GPU renders but its exporter is broken or
  disabled, the same rung would hand a real part a linear target, which
  [decision 138](Decisions.md#138-the-parent-compositor-says-what-it-can-import-the-device-says-what-it-wants-to-draw-into)
  measured at 7.4ms against 2.8ms for the same composite. That is a tier drop the frame clock will
  absorb silently. The log line names the provider, which is the whole of the instrumentation, and
  whether that is enough to notice is the open half.
- **Whether the frame clock absorbs a blocking `Record` on the floor tier.**
  [Decision 108](Decisions.md#108-a-device-that-cannot-export-a-timeline-finishes-the-frame-inside-record)
  has a renderer that cannot export a timeline wait for its own submission before returning, which is
  the accurate thing to do on a device with no second processor to overlap with — but it means that
  on a machine with a real panel and no GPU driver, software rasterization happens inside the frame
  section rather than beside it. Every input to
  [decision 35](Decisions.md#35-a-miss-costs-one-frame-bounded-by-the-floor-composite)'s record-time
  check is a duration, so the arithmetic is unchanged and the cost lands in the CPU half of `C` where
  it belongs; what is unknown is whether the *variance* of llvmpipe under load is something a
  prediction built for a GPU queue tracks well. It will not yield to reading — it wants a panel, a
  scene, and `LP_NUM_THREADS` turned down — and nothing is blocked meanwhile, because a virtual output
  never waits on a point and the path is exercised end to end without a display.
- **What a virtual output's cadence should be when nobody is asking for one.** *(Answered 2026-09-06
  by [decision 201](Decisions.md#201-a-virtual-outputs-cadence-is-its-consumers-and-the-client-states-the-ceiling-admission-needs),
  and the premise this entry rested on was wrong.)* Presenting at whatever rate the consumer releases
  is the honest answer and it does not make the period a function of backpressure, because there is no
  commanded period to make a function of anything: `FrameClock` answers `Unscheduled` for a clock with
  no live anchor, which arms no timer and lets damage present as soon as it is ready. What the client
  states instead is a ceiling — the shortest interval it may demand a frame in — which is what
  [Admission.h](../Source/Frame/Admission.h) means by `P` and what decision 66 already has a row for.
- **How long a held commit waits when there is no acquire point.**
  [Decision 125](Decisions.md#125-nesteds-release-timelines-come-from-a-drm-node-it-opens-itself)'s
  fallback polls `IRenderer::IsComplete` from the drain and asks to be looked at again in half a
  millisecond, because no file becomes readable when a GPU finishes. The number is a guess with the
  right shape — short enough not to be a visible fraction of a frame at 60 Hz, long enough not to be
  a spin, and armed only while a composite is actually outstanding so idle still costs nothing — and
  it is the one figure on that path nothing measures. It is also the reason the path's own timestamps
  are not to be trusted, which the log says. What would settle it is a machine with a host that has
  no syncobj protocol and a composite long enough to see: the answer is either a measured figure, or
  the observation that a GPU completion should be a descriptor and belongs in
  [KernelWishlist.md](KernelWishlist.md).
- **Whether decision 62's oracle should cover a textured item.** The unfused path has no sampling
  element: [Render/Unfused.h](../Source/Render/Unfused.h)'s chain begins at `Element::Emit`, which is
  where an item's *fill* enters, and a `VulkanRenderer` built `Fusion::Separate` therefore skips a
  `DrawTexture` rather than drawing one. Decision 62 says the separate-pass form is what correctness is
  defined against, so a content kind the reference cannot express is a content kind nothing checks the
  fusion of. It is small — one more enumerator, one branch in `Element.frag`, and the texture's
  descriptor bound for the emit pass instead of the intermediate nobody reads — and the reason it is
  an entry rather than done is that `ElementConstants` is at exactly 128 bytes, so the source
  rectangle has to travel in the field the fill would have used, which is the same overload the fused
  path already makes and worth deciding once for both rather than twice.
- **What a minified texture should be filtered with.** [Render/Textures.cpp](../Source/Render/Textures.cpp)
  mints one sampler, bilinear and clamped, because that is `Blit`'s answer and the two renderers have
  to produce one picture — and bilinear at unit scale is exact, so a still window is not softened by
  being composited. What it is not is *good* under minification: decision 99's overview shrinks every
  window to a tile, and bilinear with no mip chain aliases visibly on text at anything past about half
  scale. A mip chain is the obvious answer and its cost is the interesting part — it would be rebuilt
  on the dispatch thread at every client commit, which is where the trade actually is.
  [DrawItem::Sampling](../Source/Seam/Renderer.h) already carries decision 56's classification of
  whether the resample is a no-op, so the producer side of the question is answered and the renderer
  currently ignores it.
- **What a planar client buffer becomes.** `NV12` is a real client format and
  [Render/Textures.cpp](../Source/Render/Textures.cpp) refuses it: one plane, one view, one descriptor.
  A second plane needs a `VkSamplerYcbcrConversion`, which is an *immutable* sampler baked into a
  descriptor set layout — so it is a second set layout and a second lattice arm rather than a runtime
  branch, which is what makes it a decision rather than a gap. Nothing asks for it until there is a
  video client, and the refusal is loud meanwhile.
- **Whether the renderer should import a compression plane.**
  [decision 138](Decisions.md#138-the-parent-compositor-says-what-it-can-import-the-device-says-what-it-wants-to-draw-into)
  hands the driver the host's whole modifier set and takes what it picks, and then bounds that set by
  `Renderable` — one plane — because [Render/Device.cpp](../Source/Render/Device.cpp)'s import path has
  no second view to put an auxiliary plane in. On Tiger Lake the modifier that filter excludes is the
  Y-tiled CCS pair, which is the *fastest* thing the device offers and the one the driver reaches for
  first when it is allowed to. What that costs has not been measured, because the ring cannot currently
  be allocated under it long enough to time one; what is measured is the step below it, Y-tiled against
  linear, which was 2.6x. Lossless framebuffer compression is worth roughly its bandwidth share, so the
  question is whether a composite at this resolution is bandwidth-bound at all. *(Revised 2026-08-23.)*
  It was written here that it is not, on a figure of about a tenth of this machine's memory bandwidth;
  that figure counted each screen pixel once. [Decision 140](Decisions.md#140-a-composite-is-cut-at-its-barriers-and-counted-by-its-fragments)'s
  fragment counter says a plain scene shades 1.6 screens' worth per frame and every one of those
  fragments blends, which is a read and a write — about 18 GB/s at 1920x1048 on a part whose LPDDR4x
  delivers something in the twenties. So it is much closer to the ceiling than the old number claimed,
  and compression is correspondingly more likely to be worth its plane. Cheap to find out and expensive
  to assume.
- **What the trace ring costs when nothing is reading it.** Decision 139 spends sixteen mebibytes a
  thread and a handful of stores per slice on the grounds that both are far below anything the frame
  loop notices, and neither figure has been measured on a real panel. The instrument for the second
  is the one already built: `--trace-buffer=0` against the same gym and the same output set, with the
  CPU mark read off the report line. What would change the design is not a large number but a
  *variable* one — a store into a ring the frame thread has not touched for a frame is a cache miss,
  and a miss on the frame path is jitter rather than cost.
- **What counts as a driver that honours the fence deadline.**
  [Decision 142](Decisions.md#142-gyro-states-the-deadline-and-commands-the-clock-only-where-the-deadline-does-not-reach-it)
  probes for this at startup rather than inferring it from the driver's name, because a name predicts
  the wrong thing in both directions — amdgpu implements `->set_deadline` and drops it one layer down,
  and i915 gaining it should retire gyro's frequency floor without gyro shipping a new table. What the
  decision does not fix is the probe's numbers. The response is a matter of degree rather than a fact:
  msm, the only driver that wires the hint to frequency at all, answers it with `get_freq() * 2` on a
  timer three milliseconds out, which is a rescue heuristic and converges over several frames. So the
  probe has to ask *how much did the clock move, within how long*, and both thresholds are unchosen —
  as is how many samples it takes on a machine whose clock is also being moved by thermals and by
  whatever else is on the GPU at boot. The decision's tie-break is that an unclear probe takes the
  floor, which is the safe direction for frames and is also the direction that quietly spends power for
  the life of the session if the threshold is set badly.
- **Whether a known cost step needs a feedforward floor even where the deadline lands.** Decision 142
  says a driver holding the deadline plus its own history has both terms and gyro should stop at
  saying the deadline. The residue is one frame: gyro knows at commit time that a workspace switch is
  about to cost several times the last frame, and the driver learns it when the first expensive batch
  runs long. That is inside
  [decision 35](Decisions.md#35-a-miss-costs-one-frame-bounded-by-the-floor-composite)'s promise, so
  nothing is built for it — but the fix if it is ever wanted is small, because the floor writer exists
  for the other arm anyway: raise it at the commit that steps the cost, release it once the driver has
  seen one frame at the new cost. What holds it back is that it is speculative until a transition frame
  is *measured* missing, and that the failure mode of a release that does not fire is a machine held at
  high clock indefinitely — a worse bug than the one it fixes. The instrument is decision 140's spans
  across the first frames of a gym transition, which nothing has yet looked at.
- **The whole CPU side of the same question, which is unmeasured rather than answered.** Decision 142
  is GPU-only and deliberately so. `kernel.sched_util_clamp_min_rt_default` is 1024
  (`kernel/sched/core.c:1592`), so a `SCHED_FIFO` task requests the top of the frequency range — but
  that is schedutil's path, and a machine running `intel_pstate` in active mode with HWP, which is the
  default on the part this was measured on, does not take it. Whether the frame thread's core is
  actually at the clock the cost figures assume has never been checked, and the check is cheap:
  `scaling_cur_freq` beside a recorded `RecordCost`. big.LITTLE is a *placement* question rather than a
  frequency one — an RT thread pinned to an efficiency core is slow at any clock — and it belongs with
  the affinity constants
  [decision 61](Decisions.md#61-the-frame-thread-is-sched_fifo-the-earliest-deadline-first-schedule-is-gyros-not-the-kernels)
  already defers rather than with this. Nothing here has produced a symptom; it is carried because the
  GPU side did not produce one either until somebody read the clock.
- **Splitting the composite around every dressed panel costs more than the blur does.** At 1920x1048
  with two glass panels, [decision 140](Decisions.md#140-a-composite-is-cut-at-its-barriers-and-counted-by-its-fragments)'s
  spans put the frame at 3.0 ms in three composite segments, 1.6 ms in two extracts and 0.8 ms in
  twelve chain passes. The same scene with no material is 1.4 ms in one segment. So the effect adds
  about 2.6 million fragments, which at the rate the plain scene achieves is a little over a
  millisecond — and it adds four extra render pass boundaries, which is the other three. An extract is
  one draw and costs more than the six-pass chain that follows it, because the barrier in front of it
  flushes a full-screen render target the composite has just written; the chain passes are small and
  their cost barely moves with resolution, which is a fixed per-pass overhead rather than pixels. What
  would change it is drawing every material's extract in one pass instead of interleaving them into the
  painter's order — which is decision 60's group flattening arriving from the cost side rather than the
  correctness side, and is not obviously compatible with a material reading the target as of before its
  own item began.

- **The idle state the frame thread wakes from, which costs more than everything gyro does before the
  composite.** `TimingPolicy::Lead` is five hundred microseconds because that is what a capture
  measured: two hundred and eighty of them are the kernel getting the `SCHED_FIFO` frame thread onto a
  core after the ring's absolute timeout expires, and a hundred and fifty-four are the drain and the
  snapshot acquisition once it is there. The large half is not the scheduler. A thread that sleeps a
  whole refresh lets its core fall into a deep idle state, and a bare `clock_nanosleep` with no
  compositor near it reproduces the whole figure on this machine — 3.6 µs late after a 50 µs sleep,
  14 µs after 500 µs, 127 µs after 2 ms, 146 µs after 13 ms — which is the shape of `cpuidle` choosing
  a deeper state as the predicted sleep grows. This part is on the ACPI idle driver rather than
  `intel_idle`, whose `C2_ACPI` is documented at 152 µs to leave and whose `C3_ACPI` is documented at
  1034 µs, and `C3_ACPI` is the most-entered state on the machine by a factor of two.

  So what is open is whether gyro should hold a latency request against the idle governor, and at what
  value. The mechanism is PM QoS: `/dev/cpu_dma_latency` held open with a microsecond bound caps the
  state the governor will choose for the whole machine, and
  `/sys/devices/system/cpu/cpuN/power/pm_qos_resume_latency_us` does it for one core, which is the
  right shape for a thread that could be pinned. Either would take the wakeup from ~146 µs to the ~4 µs
  a shallow state costs, which would let the lead fall to the drain alone and give the rest back as
  latency. It fits gyro's existing model — the descriptor would come from a udev rule like every other
  device gyro is given — and it fits gyro's position, since a compositor that is the whole system layer
  is the party entitled to make a machine-wide latency claim rather than one process among many asking
  for a favour.

  Against holding it unconditionally: it is a power decision made in a scheduling file, and on a laptop
  a core that never idles deeply is a battery figure a user will notice long before they notice a
  composite tier. The shape that resolves both is holding it *while the world is animating* and
  releasing it when the world settles, which is
  [Architecture.md](Architecture.md#doing-nothing-must-cost-nothing) applied one level below where it
  currently reaches — gyro already knows the difference, since the fold answers `Wake::Never()` for
  exactly that state. What is unmeasured is the cost of the transition: a governor told mid-run that
  the bound has changed does not retroactively wake a core that is already deep, so the first frame out
  of idle pays the exit latency anyway, and whether that first frame is worth a tier is the question
  the entry actually turns on.

- **Where a setup is kept, on a compositor with nothing to keep anything in.**
  [Decision 164](Decisions.md#164-density-is-one-angular-preference-and-every-output-derives-its-scale-from-it-a-setup-supplies-the-distance-that-derivation-needs)
  makes a setup the seat's rather than a session's, which is the same side of the line
  [Scene/Pointer.h](../Source/Scene/Pointer.h) already puts the pointer's position on — and that is
  exactly what makes it homeless. gyro is a boot service: it lights panels before anything logs in,
  which is the whole reason the arrangement is right at first light, and it therefore cannot read the
  arrangement out of a person's session. This would be the first configuration gyro persists at all,
  so what is open is not the format but whether a compositor that owns no filesystem policy should be
  reading and writing one, or whether the per-session agent hands it over the way it already hands over
  a session lifetime. The units are not open: the arrangement is in logical pixels at the reference
  preference, which is what makes one stored setup readable by two people who want different text
  sizes. The angular preference sits on the other side and has the easier answer, being
  one scalar a session can supply.
- **Whether a pointer crossing between outputs is geometric or topological.**
  [Scene/Pointer.h](../Source/Scene/Pointer.h) confines to the union of the output rectangles and
  slides along an edge, which is the mitigation rather than the answer: with any vertical offset
  between two panels a crossing is not invertible — leave the right edge at one height, come back at
  another — and with a gap there are stretches of edge that cross nowhere. Mapping edge *segments* to
  segments on the neighbour makes every crossing reversible and removes the dead stretches, at the cost
  of a pointer whose path is not a straight line in global space, which is visible if anything ever
  draws a trail behind it. It matters more here than elsewhere because there is no settings panel to
  nudge an arrangement into alignment with, and decision 164 guarantees the offsets rather than
  avoiding them — twice over, since its centre-aligned default produces one for every pair of unequal
  panels and its snapping band produces a second by giving a snapped output a logical footprint that
  is not quite the angle it subtends. A segment mapping is the only proposal here that absorbs either.
  Wants two panels of different heights and a person, not an argument.
- **The width of the band decision 164 snaps a scale to an integer inside.** The asymmetry is settled —
  a resample is visible and a tenth of a stop of text size is not — and the number is now bounded
  rather than free. Too narrow and a 27-inch 1440p panel takes a fractional scale it does not need,
  which puts every window on the machine on the minification path
  [decision 56](Decisions.md#56-clients-render-at-the-ceiling-and-gyro-downscales) describes for no
  perceptible gain; too wide and a 4K laptop panel snaps to 2 from far enough away that text is
  visibly large. What widens the stakes past text size is that the same number bounds how far a
  snapped output's placement departs from the angle it subtends, so it is also the alignment error two
  panels sit at and feeds the crossing question above. **Writing the derivation settled the
  interval and not the number**: 164's own table takes a 1.13 to 1 and leaves a 3.48 where it is,
  which is a band of at least 11.8% and under 13.7%, and
  [Scene/Density.h](../Source/Scene/Density.h) takes one eighth. What is left is a couple of points
  inside that window — and the *shape*, because the whole of the win lives at 1× and 2× while a snap
  downward is the direction a person notices, which argues for a band that narrows as the scale
  grows. That is a second parameter rather than a correction to this one, and it wants the same two
  panels and the same person. Now with a number to disagree with.

## Keyboard sysrq, which the input grab takes away

[Decision 148](Decisions.md#148-input-is-a-source-the-dispatch-thread-drains-and-the-way-out-of-gyro-is-a-leader-chord)
takes every device that can type letters exclusively, with `EVIOCGRAB`, because nothing else stops
the kernel's own keyboard handler from delivering a person's keystrokes into whatever getty is on a
VT behind the screen. That handler is also where sysrq lives, so the grab shuts sysrq out along with
the getty — and sysrq is what the deployment requirements count on when gyro is wedged past the
point where its own chord can be read.

**The other mechanism keeps it, and gyro cannot use it yet.** Every other compositor closes the same
hole from the far end: logind puts the session's VT into `K_OFF` with `KDSKBMODE`, which silences
console translation while leaving the handler attached, so sysrq still works. It is not available
here for two reasons and only one of them is permanent. gyro has no VT to put into a mode — which is
the boot model rather than an omission — and, more importantly, `K_OFF` is *not* released when the
process dies: a gyro that crashes would leave a machine whose console keyboard is dead, which is
strictly worse than the console typing this exists to stop. The grab releases itself when the
descriptor closes.

**What would change the answer is a VT that gyro owns on purpose.** The moment it opens `/dev/tty0`
to take the console out of the picture properly, `KDSKBMODE` is available and is the better of the
two — at which point the grab can be dropped and keyboard sysrq comes back. What has to be settled
first is restoration on abnormal exit, which is the part logind is actually providing and which no
`atexit` covers: a `SIGSEGV` on the frame thread has to leave a keyboard behind.

Until then the trade is deliberate and asymmetric by hardware. On a board with no `KEY_SYSRQ` at all
— the one this is developed on — nothing is lost. On a board that has one, the rescue path is the
network, and `RLIMIT_RTTIME` remains what saves a spinning frame thread.

## The mode set the DRM backend cannot perform yet

`DrmOutput::Reconfigure` accepts a request and answers with the configuration it still has, which
`OutputConfiguration::SatisfiedBy` reads as *not honoured*. Nothing lies, and nothing changes a mode
after startup either — so a monitor's refresh rate cannot be changed, an output cannot be turned off
by the idle ladder, and hotplug is not connected to anything.

What it needs is the shape [decision 73](Decisions.md#73-the-frame-thread-initiates-reconfiguration-and-never-performs-it)
already fixes: a thread that owns the blocking commit, and a completion that arrives back through the
frame loop's own drain so that `Reconfigured` emits on the thread it is claimed by. The reason it is
not free is the reason that decision exists — `drm_atomic_nonblocking_commit` runs the driver's
`atomic_check` synchronously on the caller, and amdgpu uses that latitude to take every modeset lock
on the device and wait on every CRTC's outstanding commit, so the thread is not an optimisation.

What is genuinely open is whether that thread is per device or per process. Per device is the honest
granularity of the lock the driver takes; per process is one thread rather than one per card, and on
the machines gyro will actually run on there is one card. The second is what makes a two-GPU laptop's
external monitor wait behind the panel's mode set, and neither is measured.

## What the partition cannot express yet

*(2026-08-25, alongside the first multi-layer commit.)*
[Decision 152](Decisions.md#152-promotion-is-a-partition-of-the-draw-list-computed-every-frame-and-a-node-is-promotable-when-its-resample-is-a-no-op-and-it-carries-no-dressing-on-itself)
is built as a suffix of the draw list: gyro's composite sits on the primary plane and promoted layers
go above it. Three things follow that are deliberately not built, and each is a real arrangement
rather than an oversight.

- **A layer *below* the composite.** Some hardware puts an overlay under the primary, and the
  arrangement that wants it is a fullscreen video with a mostly empty interface over the top. Taking
  it means the composite has to be transparent where the promoted layer lands — an alpha channel on a
  scanout target, and a renderer that can be told to leave a hole — and it means the assigner may
  promote out of the middle of the list rather than off the end of it. `Drm/Output.cpp` skips such a
  plane rather than mis-assigning it, so the cost today is one overlay unused on the hardware that
  has one.
- **A promoted node keeping its shadow.** A shadow is drawn around an opaque quad and is separable in
  principle: it could stay in the composite while the quad goes to a plane. Doing it means re-emitting
  the item as a `DrawDressing` with no content of its own, which is the one thing `Seam/Renderer.h`
  already has a shape for — and it means the assigner rewrites the list it was handed rather than only
  partitioning it. Until then a lifted window is composited, which is most windows, and the promotion
  that pays is the fullscreen one that casts no shadow anyway.
- **Where the atomic test is actually cached.** `IPresenter::TestLayers` is the party that knows, and
  decision 152 says it is asked when an item crosses the promotion predicate rather than once a frame.
  Nothing yet holds that cache: `Frame/Loop.h` asks on every frame that promotes anything, which is an
  ioctl per frame on the arrangement promotion exists to make cheap. What the cache has to be keyed on
  is the partition's *shape* — which items, which planes, at what sizes — and not its positions, since
  a plane that merely moved is two integers in a commit that was happening anyway. A machine that
  promotes nothing pays nothing today, which is why this is a cost rather than a defect.
- **A frame with no composite at all.** `Assign` produces one — every item promoted, no render pass,
  no queue submission, the tablet playing video with the GPU asleep — and `Frame/Loop.h` demotes the
  bottom layer back into the composite rather than taking it. The reason is the loop's order: a target
  is acquired *before* the draw list is evaluated, and `IPresenter` has no verb that gives one back, so
  a frame that turned out to need no target has already taken one. The fix is to evaluate first and
  acquire only where the partition says a composite happens, which moves the buffer-age join and the
  damage backlog with it — worth doing once there is a client that can actually promote.

  **Answered by [decision 157](Decisions.md#157-the-frame-loop-acquires-a-target-only-where-the-partition-says-a-composite-happens),
  and it turned out to be blocking rather than worth doing later.** *(2026-08-29.)* The demotion hands
  back the *bottom* of the promoted suffix, which on a screen with one window and a pointer over it is
  the window — so the window was never in a set `IPresenter::TestLayers` was asked about, and a trace
  taken to find out why nothing promoted could only ever report on the cursor. The reorder is what the
  entry said it was: evaluate, partition, and acquire under `NeedsComposite()`. What it did not
  anticipate is that an empty draw list answers that predicate the same way a fully promoted one does
  and still owes the screen a clear, and that a fully promoted frame the driver then refuses has to ask
  for a target late.
- **Damaging only what changed sides.** A partition that differs from the last frame's repaints the
  whole output, because a composite that stopped drawing a window has to repaint where it was and
  nothing in the *scene* says so. The tight answer is the union of the quads that crossed the
  partition, which needs last frame's quads kept per output rather than only its shape. What the blunt
  answer costs is one full repaint on the frames a window starts or stops moving — decision 35's
  budget exactly, a handful of times a session.

## One plane per output, and what the catalog is already for

The DRM backend drives one primary plane and refuses a layer set with a second layer in it.
`Drm/Catalog.h` already decodes what every plane on the device accepts, because the target
allocation needs it; what is missing is the assignment — which layers can be promoted onto which
overlay, and what it costs to be wrong. [Seam/Presenter.h](../Source/Seam/Presenter.h) has carried
the multi-layer list from the start for exactly this, so the shape is not the question.

Where a *client* buffer becomes a scanout framebuffer is settled — decision 153 makes it a second
importer over the same id space, `Drm/Scanout.h` is that importer, and decision 154 gives a client a
way to hand over a descriptor in the first place. So is driving the planes: an output takes the
primary plus every overlay its CRTC can drive, up to `MaxLayers`, and commits them together.
*(Corrected 2026-08-29; this paragraph read "one primary plane is driven and a second layer is
refused" after that stopped being true.)*

What is left is the assignment *policy*. The planes are enumerated in the order the card reports
them and a promoted layer takes the slot at its own index, so which overlay a layer lands on is not
chosen — and the cost of being wrong is unmeasured, because a plane a driver will not accept for
this format at this scale is an atomic test failure that costs the whole frame's promotion. The
hardware's cursor plane is skipped by name for a version of that reason already.

What that leaves open, now that a descriptor can arrive:

- **gyro advertises what the *renderer* can sample and not what the *panel* can scan out.** The two
  lists differ on real hardware — a compressed modifier a shader reads is not always one a display
  engine reads — and a client that picks a pair from the wrong half is simply composited, which is the
  behaviour every window already has. So it costs nothing today and it costs the whole mechanism the
  day promotion is what a tablet's battery depends on. The intersection is a query `Drm/Catalog.h`
  already has the data for; what has to be decided first is what a *second* panel with a different
  answer does to a list that is one per machine.

- **Feedback is sent once and can never be re-sent, so nothing can tell a client its allocation
  stopped being the right one.** *(Narrowed 2026-08-30: the device half is built. Decision 154's
  reversal has `get_default_feedback` answering with a table, a main device and one tranche, which is
  what makes a GPU client something other than a software renderer.)* What is left is that every
  parameter is fixed at startup. The protocol's whole shape is that a compositor builds a *new* table
  and re-sends when the answer changes, and there are two things that change it: decision 41's device
  migration, where `simpledrm` is replaced by the real driver and the modifier set moves under every
  client on the machine, and a window dragged onto a monitor on another card. `DmabufFeedback` is one
  object owned by the global and every live feedback is served from it, so the re-send is a list of
  live objects and a second `Describe` — the mechanism is there and the *caller* is not, because
  neither migration nor a second card exists in this tree yet. Until then a client whose device
  changed under it is one allocating for a device that has gone, which today cannot happen and on the
  day migration lands is the first thing that will.

- **`get_surface_feedback` answers the default, so a per-window tranche says nothing.** The request
  exists so a compositor can say *this window is on that card, and here is what its display engine
  would take directly* — which is the same question the scanout tranche above cannot answer yet, asked
  per surface instead of per machine. gyro composites every window on one device, so the default is
  currently a true answer rather than a placeholder; it stops being true the moment either of the two
  entries above does.

- **One plane per buffer.** `SamplingModifiers` filters to single-plane layouts and
  `VulkanDevice::ImportImage` describes one plane, so a client handing over NV12 — which is every
  hardware video decoder there is — falls back to whatever its toolkit does with shared memory. That
  is the case promotion exists for, and it is the one arrangement gyro cannot yet take.

- **No implicit modifier.** `DRM_FORMAT_MOD_INVALID` is refused wherever it appears, per decision
  150's rule that unknown is not linear. That is right for an allocation gyro makes and it turns away
  a legacy client whose buffer genuinely has no stated layout — which is what a GBM allocation without
  modifier support produces, and there are still drivers that do it.

## Promotion does not check color, and the section that says it must is older than the promotion

[Architecture.md](Architecture.md#direct-scanout-is-conditional) states the rule: a client buffer
flipped straight to a plane bypasses the composite, so every transform the composite would have
applied has to be expressible in the KMS color pipeline, and where it is not gyro composites
instead. Otherwise the picture changes at the moment a window is promoted — a flash, and a
correctness bug wearing an optimization's clothes.

Decision 152's predicate does not implement it. It refuses a layer that would be *resampled* and is
silent about color; `Frame/Assign.h`'s `Promoted` stamps the layer with the output's own color state
and nothing compares that against the buffer's.

**It costs nothing today, which is exactly why it wants deciding now.** Every surface that can reach
a plane is sRGB: `wl_shm` and `zwp_linux_dmabuf_v1` are the only ways in and neither carries a color
description, so the stamp is accidentally correct on every client that exists. The first client to
say otherwise — `wp_color_management_v1`, or an HDR video player — makes it wrong, and it will be
wrong as a flash on promotion rather than as anything a test would fail on.

What has to be settled is which of two shapes it takes, and they are not the same cost:

- **A fourth `PromotionRefusal`**, computed the way the other three are: the item's color state
  against the output's, refuse where they differ. Cheap, correct, and it gives up the offload on
  precisely the content that most wants it — a fullscreen HDR video is the arrangement promotion
  exists for, and this refuses it by construction.
- **Ask the pipeline**, the way `TestLayers` already asks about the layer set. A plane's degamma,
  CTM and gamma are properties `Drm/Catalog.h` could read, and the newer `COLOR_PIPELINE` property
  describes the whole chain. Then the question is whether *this* conversion is expressible on *this*
  plane, which is the honest form, and it puts a color decision behind an ioctl the assigner already
  has a reason to cache.

The second is right and the first is what ships if nobody decides. Sizing: it blocks nothing until
there is a color protocol, and the color protocol is what makes it urgent, so the trigger is
whichever of the two lands first.

**Answered by [decision 161](Decisions.md#161-promotion-refuses-an-item-that-is-not-already-in-the-outputs-color-state-and-equality-is-the-question-rather-than-expressibility)
— the first, deliberately, and the second is what is left here.** *(2026-08-29.)* The refusal is in,
because it is four lines and because the alternative was a guard first reached in front of a person.
What stays open is the shape that does not give up the offload: asking a plane whether it can express
the conversion, which needs `Drm/Catalog.h` to read the colour properties and belongs behind
`TestLayers`. Its trigger is unchanged — a client that can be in another space — and its stakes went
up rather than down, since the conservative clause now refuses exactly the fullscreen HDR video that
promotion exists for.

## The batch reserve is folded from the frames outputs are owed *now*, and a scene wake is not one

`FrameLoop::Schedule` builds a device's batch from the outputs that are immediately owed a frame —
damage that has not reached the glass, a scene the output has not drawn, or a flip still outstanding.
An output whose only claim on a frame is a scene contributor naming an instant *later* is not in it, and
keeps the arming it composes for itself.

That is right for what the batch is about and it leaves a case unpriced. A window animating under a
spring on one panel wakes the loop through the retarget its commit performed, which is damage and is in
the fold; a contributor that says *a frame is wanted at T* and nothing else is not, so when T arrives
that output is served from its own record point with its neighbours' composites unaccounted. The cost is
the one this whole entry was about, on a narrower case: a frame served at an instant that did not price
the queue in front of it. Folding it needs the batch to be composed per *frame* rather than per refresh,
since two outputs wanting frames at different future instants are not one batch.

## A hold is refused for the whole device, where the honest unit is the interval

`FrameLoop::MayHold` compares a device's whole demand against its shortest member's period and turns the
record point off entirely when the two meet. That is exact for the case it was written against — a
queue with no idle time has nothing to hold a frame back *with* — and it is coarse: a card that
saturates only at the alignment where every panel's deadline coincides gives up the hold at every other
alignment too, and a card carrying one heavy output beside two cheap ones gives it up for all three.

What the rule is standing in for is a demand test over an interval rather than over a device. Decision
160 records why the coarse form is the one that landed: the failure it prevents is a frame dropped on a
saturated card, the cost of being coarse is latency on a card that is not, and only the first is
something a person cannot see coming.


## The first frame a window is promoted on lands a refresh late, and i915 asks for it

A window opens and its first frame is on screen for two refreshes instead of one. It is the moment a
person is most likely to be looking — the thing they just launched appearing — and it is not a rare
race: six captures out of six, five of them consecutive runs of the same script, every one missing at
the single `items 0→1` transition and nowhere else.

The frame is not late. Against a vblank grid fitted from the flip-event timestamps — 16.6528 ms with a
4.6 µs residual σ over 563 samples — the missed commit was issued 4.010 ms before its target in the
first capture and 3.184 ms in the instrumented one, against 2.926 ms for the frame immediately before
it that landed. Across the six the miss margins span 3.4–4.1 ms while hundreds of tighter frames make
their vblank; in one run the miss has the second-widest margin in the whole capture. Margin is not the
variable. Neither is the client's fence — the only `dma_fence_wait` near the commit is 28 µs, and
`Promoted` hands the layer `SyncPoint::Immediate()` anyway — and neither is scheduling: the commit
thread's state trace is identical in shape to every frame that succeeded.

What the merged system trace shows instead is a *two-phase* sleep. Every landing frame sleeps in `D`
and is woken by flip-done about 14 µs after its target vblank. The missed frame woke at its target
vblank +4 µs, ran for 10 µs, and went back to sleep for a further 16.476 ms. That is not a flip armed
too late; that is a `wait_for_next_vblank` inside the commit tail.

`drivers/gpu/drm/i915/display/intel_fbc.c` names it. Framebuffer compression is bound to one plane, and
while gyro scans out its own full-screen target on the primary that is the case FBC is for. Promoting
the window replaces that framebuffer with the client's, and `intel_fbc_can_flip_nuke` refuses the cheap
address swap when the format, the modifier, the plane stride or the cfb stride changed — the stride
certainly did, panel width to window width. So FBC is deactivated, and `__intel_fbc_pre_update` then
answers `need_vblank_wait` under **Display WA #1198**, *glk+*: an extra vblank between an FBC disable
and most plane updates, because touching the plane registers otherwise shows as corruption.
`intel_pre_plane_update` spends it.

gyro feels it because it drops `DRM_MODE_ATOMIC_NONBLOCK` and runs the ioctl on a per-output commit
thread — the trade `Drm/Commit.h` argues, that a non-blocking commit returns having only queued the
work and leaves a `SCHED_OTHER` kernel worker to arm the flip. The driver's extra vblank is therefore
spent inside gyro's `drmModeAtomicCommit` — and `CommitDepth` is one, so the loop is then locked out
until the flip, which is why one late latch reads as a duplicated frame rather than as slack absorbed.
The consistency check that makes the reading hard to dismiss is the transition that *does not* miss:
`1→2` enables an overlay and leaves the primary's framebuffer alone, and FBC binds to one plane, so
there is nothing to tear down and no extra vblank. Only the commit that rewrites the primary plane
pays.

**The cause is confirmed; what to do about it is not.** A boot with `i915.enable_fbc=0` was the
experiment this entry asked for, and it ran. The same client under the same script promoted at the same
transition and missed nothing: 371 frames, 371 on the refresh they were aimed at, against six captures
out of six before. The frame that used to miss took 3.076 ms from commit to flip event where the
median is 3.019 ms and the worst frame in the capture is 3.702 ms — where with FBC on the same frame
took 19.684 ms and was the slowest in its trace by a full refresh.

What makes one run enough is that the *mechanism* went with it rather than only the symptom. The
signature this entry was written around is the commit thread sleeping twice — woken at its target
vblank, then again for a whole refresh. With FBC off that frame sleeps once and is woken 0.011 ms after
its target vblank, which is where every ordinary landing frame is woken. There is nothing left to
attribute a miss to, rather than a miss that happened not to occur.

So the reading holds: promoting a window rewrites the primary plane's framebuffer, the stride change
costs the flip-nuke path, and i915 spends a vblank between the FBC disable and the plane update.
Disabling FBC is not the answer — it is a system-wide power cost paid to dodge a once-per-window
event, and gyro does not own that decision on a machine it is only one process on. Three mitigations,
none of them free. **Accept it** — one duplicated frame per window
map, spent on a driver workaround, and arguably the honest price. **Never promote onto the primary** —
keep the composite there and promote only onto overlays, so the primary's framebuffer never changes
shape and FBC keeps its flip-nuke path; this forfeits exactly the arrangement the partition exists for,
the fully-promoted screen with the GPU asleep. **Price it** — let the assigner know that rewriting the
primary plane's framebuffer costs a frame on this driver and schedule the transition a refresh early,
which keeps the promotion and turns a miss into a plan. The third is the only one that does not give
something up, and it is also the only one that puts a driver's name inside a portable module, which is
the reason it is a question rather than a change.

## The `planes` counter reads zero on a frame that committed a layer

`TraceCount("planes", partition.Layers())` at `Frame/Loop.h:1279` is the shape the frame actually
committed, and on an empty scene it is a lie. Where `TestLayers` refuses or nothing is promotable the
fallback rebuilds the partition as `Partition{ .Composited = list.Items.size() }` at `Loop.h:1248`; with
no items that is zero composited, so `NeedsComposite()` is false and `Layers()` is zero — while `count`
is one and the composite layer is what goes out. `DrmOutput::Expressible` refuses an empty layer set
with `EINVAL` at `Drm/Output.cpp:601`, which is the proof the commit was not empty: the flip was issued,
so a layer was there.

The cost is not the number, it is what a reader concludes from it. A `planes` of zero beside an `items`
of zero reads as *this frame configured no plane*, when what happened is *this frame put gyro's
composite on the primary*. During the Display WA #1198 hunt above it produced two wrong readings of the
same trace — first that a plane had been enabled at the transition, then that the primary had gone from
nothing to something — where the truth is that one layer was committed before and one after, and only
the *framebuffer* changed. That is the whole of the defect being chased, and the instrument pointed
away from it twice.

What the fix has to decide is what the counter is *for*, because the two answers differ here. If it
means the layers this commit carried, it is `count` and the empty scene reads one. If it means the
partition's shape — the reading `Loop.h`'s own comment takes, where one is a screen the GPU drew whole
and the interesting frame is the one where it climbs — then a scene with nothing in it is a third state
rather than a zero, since *the GPU drew a whole screen of nothing* and *no plane was configured* are
different frames and currently share a number.

## `C_min` is a target no part has been measured against, and the first one to be measured missed it

[Decision 168](Decisions.md#168-the-schedule-reserves-against-the-larger-of-the-floor-target-and-what-floor-frames-cost-and-it-arms-for-the-most-expensive-tier-still-reachable)
made the schedule tell the truth about a floor composite that overruns its target. It did not make the
composite cheaper, and on the first machine the figure was ever read against it is out by several
times: an Adreno 618 draws the materials gym's floor tier in 6.49 ms, inside a 16.67 ms refresh, for a
target that is supposed to leave room for the frame it is absorbing a shock on behalf of.

Two questions, and they are separable.

**What is `C_min` actually for, as a number?** Decision 35 sizes it as the shock the loop can absorb,
which makes it a fraction of a period rather than a constant — and the capability probe measures a
machine rather than a scene, while what a floor composite costs is mostly how many pixels are under
how many lifted items. A target that a busy scene cannot meet on any part is a target that reports a
defect on every machine and means nothing. Whether the answer is a fraction, a per-scene figure
resolved at admission, or a tier step that reaches `Budget::Invalidate` sooner is open.

**And what a shadow costs, which is what the 6.49 ms mostly is** — answered by
[decision 169](Decisions.md#169-the-shadows-normal-integral-is-a-polynomial-over-the-span-the-shadow-is-already-cut-at-because-what-it-costs-is-the-instruction-class-rather-than-the-operation-count),
which took the Adreno 618's composite from 10.58 ms to 8.79 ms by replacing `ShadowPhi`'s error
function with a polynomial and changed nothing measurable on a Tiger Lake. What that entry leaves here
is smaller and still real: the shadow is 7.7 times a plain fill per fragment rather than 12.6, and two
levers remain and one of them is already spent. `Seam/Dressing.h`'s `Expansion` sets the quad's *area*
at `Offset + 3σ`, which is a separate lever from the per-fragment cost — but it is at the right place
already: the alpha left outside the bound is 0.11 of a code point at three sigma, 0.51 at two and a
half and 1.86 at two, so tightening it trades a real cost for a visible edge around every shadow. What
is actually open is the other one. A fragment saturated on both axes, which
is most of a large panel's interior, still evaluates four full polynomials to answer one; an early out
there is the shape of win the deficit's own early exits already are, and nobody has measured how much
of the quad qualifies.

Worth stating what is *not* open: the corner quadrature decision 132 is mostly about has never
executed. Nothing in the tree sets `DrawItem::Radius` to anything but zero, so every quad gyro draws
today has square corners and `ShadowDeficit` returns at its first line. The measurement above is the
separable term alone, and the first rounded corner on screen adds to it rather than being included.

## The clients still on implicit sync are the stall explicit sync was built to remove

[Decision 174](Decisions.md#174-gyro-serves-wp_linux_drm_syncobj_v1-and-a-clients-fence-is-waited-on-before-the-commit-rather-than-inside-the-frame)
holds a commit until the client's acquire point signals, so no fence a client owns reaches gyro's queue
submission. That is true only of the clients that ask. Everything else — which is every dmabuf client
that has not adopted `wp_linux_drm_syncobj_v1`, and will remain most of them for years — is on implicit
synchronization, where the kernel attaches the client's fence to the buffer and gyro's own submission
waits on it at composite time. A late client is therefore still capable of holding up the composite for
every window on the machine, at a priority gyro asked for and cannot use, and the trace shows a long
GPU span rather than a wait on somebody else.

The shape of the fix is the same and it is not obviously worth what it costs. A dmabuf's implicit fence
is pollable: `poll(POLLIN)` on the descriptor answers *the writer has finished*, so a commit could be
held on that exactly as it is held on an acquire point, with no protocol involved and no client
cooperation. What is unmeasured is the cost of the poll on every commit of every software-paced
client — one syscall per commit is nothing, but the fences are per plane and a client with no fence at
all pays it to learn that.

Worse, the answer is not honest for a client that is *reusing* a buffer: the implicit fence says the
last writer finished, not that this frame's writer has started, so a toolkit that attaches a buffer it
has not begun drawing into yet would be published early. That is the case explicit sync exists to name
and implicit sync genuinely cannot. So the poll may be a strict improvement only for the single-buffer
case and a lie for the double-buffered one, which is the question to settle before building it.

## A held subsurface lands a beat after the arrangement it belongs to

Decision 174 holds a synchronized subsurface's *own* cached state on its acquire point rather than
holding the parent commit that would apply it. The exact reading of the protocol is the other one: the
cache is applied by whoever commits above it, so a parent whose child is not ready has not got a
complete arrangement to apply. What gyro does instead is let the parent apply everything that is ready
and let the late child arrive on its own, which is a video frame landing one beat away from the
controls drawn over it — precisely the tear `wl_subsurface`'s synchronized mode exists to prevent.

The reason it is this way round is that the exact reading has a worse failure: one late child freezes
every surface in the tree, including the ones that were ready, which is the hostage-taking decision 174
refuses at the top level. What is open is whether there is a third answer — hold the parent, but only
for a bounded number of refreshes, after which the ready surfaces go out without the late one. That
needs a deadline the dispatch thread does not currently have, and it needs somebody to say what the
bound is in terms of what a person sees.

## Nothing shows a person the clipboard, and nothing tells them it was read

Decision 176 has gyro holding the text of every selection and the sixteen before it, per session, and
recording the first read of each one by each application. Both halves are built and neither reaches a
person. The history has no reader at all; the read notice is an `info` line in a log, which is exactly
where a person who wanted to know that a background application had just taken what they copied will
not be looking.

What is missing in both cases is the same thing: a shell, and a protocol for it to hear about this on.
That is [decision 51](Decisions.md#51-the-shell-is-a-per-session-client-gyro-owns-mechanism)'s seam
and it does not exist yet, so what is open is not *whether* to surface these but whose vocabulary they
land in — a System-tier global a shell binds, with the history as an enumeration and the read as an
event, is the obvious shape and it prejudges nothing else. What has to be decided with it is whether
selecting an entry from the history is the compositor setting the selection on the shell's behalf, or
the shell holding a source of its own like every clipboard manager does today. The first is the reason
this is gyro's at all; the second is the one every toolkit is already written against.

A read notice also needs a policy the log line does not have: which reads are worth showing. A toolkit
reads the selection whenever a window gains focus, so *every first read per application per copy* — the
rule gyro records under — is still one line per application per Ctrl+C, which is a notification per
window a person alt-tabs through. Whether the honest unit is the read, the paste, or nothing at all
until a person asks is a question about what people do with the answer rather than about the mechanism.

## Drag-and-drop is not built, and a person can copy but not drag

Decision 176 is the selection half of `wl_data_device_manager`; `start_drag` is answered with
`wl_data_source.cancelled` and nothing happens. What that costs is dragging a file out of a file
manager, a tab out of a browser window, or a colour onto a swatch — none of which have any other path
in Wayland.

The work is not more clipboard. A drag needs an icon surface, which is a `wl_surface` role gyro does
not have and which is drawn under the pointer at input rate; a grab that supersedes the seat's implicit
one for the duration, which is the machinery
[Protocol/Drag.h](../Source/Protocol/Drag.h) already holds for moving a window and would have to admit
a second kind of gesture; and the action negotiation, which is the one part of this protocol where the
compositor is expected to have a *policy* — the modifier a person holds decides copy against move, and
there is no configuration and no shell to declare one. That last is the open question rather than the
first two: the mechanism is clear and the default behaviour is a choice nobody has made.

## Middle-click paste has no protocol behind it

`zwp_primary_selection_device_manager_v1` is a separate interface with the same shape as the selection
half of 176, and gyro advertises none — so selecting text in one window and middle-clicking in another
does nothing, which on X11 and on every other Wayland compositor it does. It is deliberately not folded
into 176: the primary selection is set by *selecting* rather than by a keystroke, so it changes
constantly and the eager fetch that makes the clipboard survive an application would mean asking a
source for its text on every drag of a mouse across a paragraph. Whether the primary selection is
cached at all, or is the one selection that lives and dies with its client, is the question to settle
before the global goes up.

## `Alt+Tab` has no switcher, so a person walks the windows blind

Decision 177 binds the gesture and moves focus, and the only feedback is the window it raises. Nothing
says how many windows there are, what they are called, or where in the list the walk has got to — so
stepping past four windows to reach the fifth is four full raises, each one a whole window appearing
and covering the screen, which is what everybody else replaced with a row of thumbnails for a reason.

What it needs exists: `Scene` authors nodes, `Text` draws a label, and a window's title is already held
for `ext_foreign_toplevel_list_v1`. What is not settled is what the thing *is*. A row of live
thumbnails needs a node that samples another subtree's pixels, which is decision 60's group flattening
pointed at a second consumer and is the expensive answer. A list of titles needs none of that and is
what a person actually reads. And either one is a piece of shell UI drawn by the compositor, which is
the thing 141, 162 and 177 are each careful to say they are only doing because nothing else can yet —
so the question is whether the switcher is the fourth stand-in or the point at which gyro stops adding
them and grows the surface a shell draws its own on.

**A shell can be summoned now and still cannot take this one.** *(Annotated 2026-09-05.)* Decision 186
gives a shell a chord and the press's own instant, which is what a launcher and an overview needed —
but not this: a walk needs the modifier's release to land on, and the binding protocol has no release
event. So of the three stand-ins, the two that fire on a press are displaceable and `Alt+Tab` is not,
which is recorded above under the entry that carries the missing half.

## A shell binding is matched against every session at once, and a held chord has no protocol

Decision 186 gives a shell a chord and the instant it was pressed, and leaves two things open that a
second session on the machine would find first.

**Every binding on the host is matched, whichever session claimed it.** Decision 21 serves every user
from one process, so two shells can hold `gyro_bindings_v1` at the same time — and the key path has no
notion of *which session is at the keyboard* to filter against, because there is one seat, one focus
and no switching between sessions yet. What leaks is small and real: a shell in one session learns that
somebody in another pressed a key, with no keystroke and no timing beyond that. What it costs when
session switching arrives is larger, because by then a chord claimed by a session nobody is looking at
would be swallowing keys from the session somebody is. The fix wants the same *which session has the
keyboard* that the seat will need, so this is one question rather than two, and answering it for the
seat answers it here.

**A chord a person holds has no protocol.** `pressed` is the only event; there is no release, and no
way to hear the modifier come up. That is what `Alt+Tab` is — decision 177's whole argument is that
cycling needs a held modifier to say *still choosing* and then *this one* — so the stand-in in
`Input/Chord.h` cannot be displaced by a shell until this grows. It is an event and a version bump
rather than a redesign, and the reason it is not built is that nothing wants it yet: the run bar and
the overview are both press-and-release-immediately. What is worth settling before writing it is
whether a held chord is the same object with two more events or a different request, since a shell that
claimed one of each on the same keys would want to know which fires.

## Nothing tells a shell which backgrounds to produce, and a panel with none draws black

Decision 179 makes the fit exact: a background is shown on an output whose device extent it matches
and nowhere else. That is right — the party that knows what a picture is *of* is the party that should
crop it — but it names an obligation and no channel for it. A shell has to know every panel's device
extent to produce an image per panel, and today a client learns a `wl_output`'s mode in the mode
event's own units, which is not the same number as the one this is compared against on a scaled or
rotated panel. So the first shell to try will produce an image that is refused, and what a person sees
is black behind their windows with nothing in the log about the size that was wanted.

The nearest existing answer is `wp_fractional_scale_v1` plus the output's mode, which reconstructs the
number the hard way and once per panel. The honest one is probably a protocol of gyro's own carrying
the extent this actually compares against — the same protocol that would let a shell hand a descriptor
over rather than a path — and the question is whether that is a background protocol or the first verb
of the shell interface decisions 141, 162 and 177 keep deferring.

Two smaller things wait behind it. **A background is one image, not one per output**, which is the
right shape while a shell is handing them over as attention moves and the wrong one if the answer above
turns out to be *here is the set, keep them all*. And **a background handed over as a dmabuf is not
built**: `PamImage` is the whole intake, so a shell that has already composited its wallpaper on the
GPU has to read it back and hand over bytes.

## A capture reads a client's buffer that another output may have on a plane

Decision 182 waits for a client's buffer to leave the planes before the frame thread reads it back, and
both halves of what it waits on are the output's own: the commits queued on this output, and the commit
this output last flipped. A window spanning two panels is on two outputs, and gyro composites it on the
one being captured while the other is scanning it out on an overlay. The acquire from
`VK_QUEUE_FAMILY_FOREIGN_EXT` is against the buffer rather than against an output, so the neighbouring
display engine is exactly as entitled to be mid-scanout as the local one was, and the wedge is the same
wedge.

It is not the failure that was found, because the machine it was found on drives one panel, and it is
not reachable by pressing the chord on a single-output session at all. What makes it worth writing down
now rather than after somebody's second reboot is that the shape of the answer is already in the tree
and is not a third bit: `Drm/Scanout.h` holds the scanout adoptions **per card** and releases one when
no commit could still be showing it, which is the same question asked of the same buffer by the party
that actually knows. A capture that asked the importer *is anything still scanning this out* would be
right for one output and for six, and would stop the loop carrying the fact at all.

What that costs is a verb across the waist on `IScanoutImporter`, answered by one backend and defaulted
to *no* by the rest — which is the shape decision 182 rejected for `IPresenter` and is a better fit
here, because this interface already exists to answer a question about one texture id rather than about
a frame.


## A shell can say a surface is chrome but not where it goes, so a panel has nowhere to be

Decision 187 gives a shell a surface that draws above every window, stays out of the switcher and can
be made of glass — and places it by centring it on the output holding the pointer, which is what the
Floorplanner does to every window because it is the one placement that takes no parameter (141).

For a launcher that is exactly right and is why the protocol shipped without an answer. For a panel it
is nothing like right: a status bar belongs along an edge of a named output, and the request that says
so does not exist. Nor does the other half of it — the space a maximised window must not cover — which
is what layer-shell calls an exclusive zone and is the reason it has one.

**The reason this is open rather than decided is that it is two questions wearing one coat.** *Where
does this surface go* is arithmetic the compositor can do from an anchor and a margin, and it is
roughly what `Positioner.h` already does for a popup against its parent. *What must not be covered* is
a constraint on every other window on the machine, which is window management and is decision 51's to
give to the shell — except that the shell cannot enforce it against windows it does not place, which is
all of them while the Floorplanner is the placer. So the second half probably waits for a shell that
declares a placement model at all, and the first half could land tomorrow.

What it costs today is that the only chrome worth writing is chrome that wants the middle of the
screen. A dock, a status bar and a notification stack are all unwritable, and the workaround — a
full-screen transparent surface with the panel painted at the top of it — is worse than nothing: it
would take the pointer across the whole screen, and it would ask the compositor to blur a panel-sized
region as though it were the size of the display.

## A chrome surface has a material but no shadow, so a launcher sits flat on the desktop

`Material` and `Elevation` are two named axes on the same entity, and decision 187 puts the first on
the wire and not the second. A launcher over a person's windows is exactly the case decision 104's
shadow was built for — it is what makes a floating thing read as floating rather than as a hole cut in
the screen — and a shell cannot ask for one.

The reason to leave it out was that a second enum on the wire before anything has drawn with the first
is guessing, and that is still the reason. What would settle it is looking at a run bar with glass and
no shadow under it on a real panel, which is what step three of the shell work exists to make possible.
Two outcomes are both plausible: that `Elevation` belongs beside `set_material` as a second named
request, or that a chrome surface should simply carry the elevation its material implies, which is a
table in `Seam/Dressing.h` rather than a word on the wire.

**Neither can be looked at on the CPU renderer.** `Blit` refuses a material and a shadow alike and
loses the whole frame, so this and everything else decision 187 made possible is visible only nested or
on DRM. That is decision 79's floor doing what it says, and it is worth naming here because it is now
the first thing between a person and the picture this protocol exists to produce.

## An exit that is drawing nothing is named and not ended, because the cut is on the other thread

`Frame/Evaluator.h` now counts the closing windows that put no items on a screen and marks the first
frame of each on the trace ring as `exit draws nothing`. What
[decision 46](Decisions.md#46-exit-snapshots-come-from-a-pre-reserved-per-output-atlas-exhaustion-finishes-exits-early)
actually asks for at that moment is the window's exit to *end* — a cut is a designed failure a person
reads as speed, and an empty rectangle fading for a third of a second is read as the application
breaking — and the count does not end anything.

The obstacle is a thread boundary rather than a policy. `FinishRetirement` is a dispatch verb over an
`EntityId`; the detection is on the frame thread and the only name it holds is a reservation count,
which is the number
[decision 46](Decisions.md#46-exit-snapshots-come-from-a-pre-reserved-per-output-atlas-exhaustion-finishes-exits-early)
mints precisely so that the two sides need not agree on identity. Closing it means a run of
reservations in `Publication/Return.h`'s `FrameReport` — a record whose padding is spelled because
[decision 49](Decisions.md#49-the-restart-boundary-is-made-cheap-where-it-can-be-and-stated-where-it-cannot)
holds open dispatch becoming a process — a merge rule for it beside the presented run's, and a reverse
lookup on the dispatch side from a reservation to the entity holding it, which nothing keeps today.
That is three new pieces of machinery on the boundary
[decision 83](Decisions.md#83-dispatchs-publication-is-an-event-source) and
[decision 147](Decisions.md#147-the-return-channels-doorbell-is-the-frame-threads-and-it-rings-only-where-a-client-is-waiting)
were careful about, to act on a condition that is a defect rather than a load.

**What would settle it is one occurrence.** The mark exists so that the next empty exit is visible in
a trace instead of taking five rounds of instrumentation to find, and the shape of what produced it is
what says whether the answer is the return channel, a repair further up, or an assertion that fails
the build. Building the channel first would be choosing the mechanism before the case.
