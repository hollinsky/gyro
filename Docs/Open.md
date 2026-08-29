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
  [decision 97](Decisions.md#97-a-view-is-split-in-half-the-world-places-an-output-and-the-frame-side-carries-its-extent)
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
  gyro's.
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
  whether the first awkward case adds a per-event request and quietly undoes it. The gesture
  vocabulary itself — which gestures exist, and what each binds to — belongs with the scene and
  material vocabularies above and for the same reason: a gesture, the transition it drives, and the
  nodes it moves are one design problem seen three ways.
- **Color format for virtual outputs.** Encoders want NV12 or P010, not RGBA. The agent can convert
  (an extra full-frame pass and its bandwidth), or gyro can fold RGB→YUV into its final composite
  pass (much cheaper, but the renderer grows a YUV output path it otherwise would not have), or both
  can be offered. HDR sharpens it — P010 and transfer functions. This is the one part of decision 26
  with a real performance number attached and it should not be decided from the armchair.
- **Nested's frame-to-dispatch handoff for input.**
  [Decision 81](Decisions.md#81-a-source-is-pumped-by-one-thread-nested-opens-one-connection-pumped-by-the-frame-thread)
  puts the host connection on the frame thread, so `wl_seat` events are decoded there and have to
  reach dispatch — a third channel where
  [the design turns on there being two](Architecture.md#the-publication-boundary). What is settled is
  that it is nested's alone and not a general mechanism, and that the shape to reach for first is
  [decision 83](Decisions.md#83-dispatchs-publication-is-an-event-source)'s: a queue behind an
  `IEventSource` the dispatch thread already drains. What is not settled is the queue's discipline —
  input is the one stream where newest-wins is wrong, so it cannot be a `Ring`, and a bounded queue
  needs an answer for what a full one means. This does not bite until there is an input path at all;
  today the connection carries feedback and configure and nothing crosses.

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
  argument.
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
- **The System tier needs its own listener.** Also surfaced alongside decision 44. Trust level is
  per connection, connection identity comes from the listener (decision 23), so trust is a property
  of the listener — which means a `System` connection cannot arrive through the ordinary per-user
  socket. That implies a second listener per session with different permissions, and who creates it,
  where it lives, and how a client is judged worthy of it are all unspecified. Decision 51 promotes
  this from speculative to load-bearing: the shell is its motivating occupant, and "which process
  gets to be the shell" is exactly the judgement this listener has to encode.
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
  [decision 138](Decisions.md#138-the-device-picks-the-modifier-and-the-host-only-says-which-are-importable)
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
- **What a virtual output's cadence should be when nobody is asking for one.** Decision 102 gives it
  a `VblankTimeline`, which needs a period, and a recording at a fixed rate has an obvious one. A test
  stepping frame by frame does not, and neither does an encoder that wants to consume as fast as the
  compositor produces. Presenting at whatever rate the consumer releases buffers is the honest answer
  and it makes the output's period a function of backpressure, which is the one thing
  [FrameClock](../Source/Frame/FrameClock.h) is built to assume is stable. Small, and worth settling
  before a second consumer exists rather than after.
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

- **`Emit`'s promise of no syscall is a property of the machine's clocksource, not of the code.**
  [Trace.h](../Source/Core/Trace.h) says a record is four stores and a release with "no allocation,
  no lock, no syscall", and the always-armed ring makes that claim tens of times a frame on the
  `SCHED_FIFO` thread. The syscall half of it is not something the file can be correct enough to
  guarantee: `MonotonicClock::Now` calls `clock_gettime(CLOCK_MONOTONIC)`, and glibc only services
  that in userspace when the kernel's current clocksource has a vDSO mode. On `tsc` it does. On
  `acpi_pm` or `hpet` — an unstable TSC, some virtual machines, older parts — the vDSO falls through
  and every record becomes a real syscall on the thread that must not make one. Measured on this
  machine, which is `tsc` with enhanced IBRS: 13.3 ns through the vDSO against 118 ns forced through
  `syscall(SYS_clock_gettime)`, so about nine times, and worse on a machine paying for retpolines and
  PTI. What that actually costs is why this is an open item and not an alarm — at Trace.h's own
  "tens" of records it is roughly six microseconds a frame instead of one, which is a twentieth of a
  percent of a 60 Hz refresh and does not drop anything. The reason to carry it is that gyro is a boot
  service that reports cost figures, and on a machine where the vDSO is not servicing the clock every
  figure it reports is inflated by its own instrument by an amount nothing in the trace says. So what
  is open is whether the composition root should read
  `/sys/devices/system/clocksource/clocksource0/current_clocksource` once at startup and say what it
  found. Against doing it: that is a `/sys` read producing a number nothing decides against, and the
  rule that the timebase has one reader and no reachable now
  ([57](Decisions.md#57-one-timebase-clock_monotonic-converted-at-ingest-and-nowhere-else)) is there
  to keep exactly that kind of read from accumulating a consumer. A log line nobody reads is not an
  instrument either.

  What is *not* open is the virtual call. `IClock::Now` being virtual costs between 0.0 and 0.16 ns
  measured against the same 13.3 ns read — in the noise, and an order of magnitude better than
  [Clock.h](../Source/Core/Clock.h)'s own estimate of "roughly a tenth". Resolving the clock to a
  concrete type when a buffer is armed would buy nothing, and the interface is what lets the headless
  sweep place time at arbitrary phase.

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

What it needs is the shape [decision 73](Decisions.md#73-a-reconfiguration-is-initiated-on-the-frame-thread-and-performed-elsewhere)
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
way to hand over a descriptor in the first place. What is left here is the assignment itself: one
primary plane is driven and a second layer is refused, so `Frame/Assign.h`'s partition can promote at
most the top item and only onto the plane the composite would otherwise have used.

What that leaves open, now that a descriptor can arrive:

- **gyro advertises what the *renderer* can sample and not what the *panel* can scan out.** The two
  lists differ on real hardware — a compressed modifier a shader reads is not always one a display
  engine reads — and a client that picks a pair from the wrong half is simply composited, which is the
  behaviour every window already has. So it costs nothing today and it costs the whole mechanism the
  day promotion is what a tablet's battery depends on. The intersection is a query `Drm/Catalog.h`
  already has the data for; what has to be decided first is what a *second* panel with a different
  answer does to a list that is one per machine.

- **No `zwp_linux_dmabuf_v1` feedback, so a client cannot be told which device to allocate against.**
  Version 3's format and modifier events say what gyro will take and say nothing about *where* — and
  on a machine that composites on one card and scans out on another, that is the difference between a
  buffer that can be promoted and one that cannot. It is also the only way to tell a client its
  allocation stopped being scanout-capable, which is what a window being dragged between two monitors
  on two cards is. Decision 154 rejected advertising version 4 without honouring it; this is what
  honouring it means.

- **One plane per buffer.** `SamplingModifiers` filters to single-plane layouts and
  `VulkanDevice::ImportImage` describes one plane, so a client handing over NV12 — which is every
  hardware video decoder there is — falls back to whatever its toolkit does with shared memory. That
  is the case promotion exists for, and it is the one arrangement gyro cannot yet take.

- **No implicit modifier.** `DRM_FORMAT_MOD_INVALID` is refused wherever it appears, per decision
  150's rule that unknown is not linear. That is right for an allocation gyro makes and it turns away
  a legacy client whose buffer genuinely has no stated layout — which is what a GBM allocation without
  modifier support produces, and there are still drivers that do it.

## The arming is priced per output, and a device with two outputs on it is one queue

`Timing::Arming` composes one output's cost figures and subtracts the sum from that output's deadline.
That is what a record point is, and `FrameLoop::Serve` now holds a frame to it: a wake that came from
a flip, a socket or a key press finds the work fits from the instant the previous frame retires, and
recording there publishes a pointer position and a window's pixels a whole refresh before the glass
shows them.

The rule is only applied where an output has its device to itself, which is where the arithmetic is
true. Two outputs on one card share a queue: the second one's composite does not begin executing when
its own record starts, it begins where the first one's ended, so its own record point is not an instant
it can afford to wait for. Serving them back to back from wherever the loop happened to wake is what
made an admitted set meet its deadlines, and `Integration/Schedulability.Test.cpp`'s phase sweep sees
it go the moment each output is held to its own figure — the second output waits, is charged the
first's execution on top, and misses a frame the set was admitted to make.

What is missing is a reserve that prices the *batch*: every composite the device owes this refresh,
subtracted from the earliest deadline among them, which is decision 29's serialisation test read
backwards. It has to appear in the arming as well as in the check, or the alarm still names the instant
one output alone could start at and the output served second is late by exactly the other's cost. What
has to be decided is where it lives — `Timing` is written per output on purpose, and the party that
knows which outputs share a device is the loop.

Until then a second monitor on the same card keeps the old behaviour: it draws as soon as the work
fits, and its pointer is a refresh behind. A second monitor on a *second* card is unaffected, because
each is alone on its own queue.
