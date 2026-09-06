# The Experience

What a person perceives when they use a machine running gyro, stated independently of how any of it
is achieved.

This is the first tier of the documentation. [Architecture.md](Architecture.md) is the second and
describes mechanism; [Structure.md](Structure.md) is the third and describes where that mechanism
lives. The relationship between them is directional and it is the point of the arrangement:

- **Citations point up; dependencies point down.** Architecture cites this document as
  justification. This document may not require Architecture in order to be understood. If an
  assertion here can only be stated by naming a mechanism, either the assertion is wrong or the
  mechanism has leaked upward.
- **Every assertion here is observable.** Someone can sit in front of the machine, or point an
  instrument at it, and find out whether the claim is true. An assertion that cannot fail is not an
  assertion.
- **Nothing here is true of gyro in particular.** Every promise below could in principle be kept
  by a different implementation. That is the test for whether a sentence belongs.

Each section closes with a *Mechanism* line naming where the how is kept. Those pointers are for
navigation and are never load-bearing.

**Volatility: this document changes when the product changes.** It does not change because hardware
measured differently, because an interface was reshaped, or because something turned out to be
harder than expected. Those change [Architecture.md](Architecture.md). If a measurement makes a
promise here unkeepable, the promise is withdrawn deliberately and the withdrawal is recorded — it
does not quietly become aspirational.

> **This document is currently an extraction.** Everything below was already asserted somewhere in
> [Architecture.md](Architecture.md), [Animation.md](Animation.md), or
> [Decisions.md](Decisions.md), scattered through mechanism arguments as justification. Gathering it
> was the first pass. [What is not yet asserted](#what-is-not-yet-asserted) lists the holes that
> gathering exposed, and they are the second pass.

## The six promises

1. **[One continuous image](#one-continuous-image)** — from the firmware logo to shutdown, the
   screen is never blank, never flashes, and never shows a seam in time.
2. **[Every frame](#every-frame)** — on every display attached, at that display's own rate,
   simultaneously.
3. **[One hand made all of it](#one-hand-made-all-of-it)** — everything moves as one system, and
   the movement carries information rather than decoration.
4. **[The picture is correct](#the-picture-is-correct)** — sharp where it should be sharp, the
   right color, the right size, and it does not change when the machine changes how it is drawing
   it.
5. **[The machine holds several people](#the-machine-holds-several-people)** — and none of them
   can see, reach, or impersonate another.
6. **[Doing nothing costs nothing](#doing-nothing-costs-nothing)** — an idle machine draws no
   frames and spends no battery on the compositor.

They contend less than a list of six promises suggests, and where they do the order is not a matter
of taste.

**Isolation is a constraint, not a priority.** A priority is something a design trades against; a
constraint is something every candidate solution must satisfy. Promise 5 is the second kind, and
the difference is not pedantry — writing it as a priority licenses every future conflict to be
settled by citing it, which is exactly how a system accumulates a decade of *you cannot do that,
for security* with no replacement ever arriving.

So **where isolation appears to cost experience, that is a design defect and the conflict gets
removed rather than paid.** Locking is the worked example. Read as a ranking, "isolation wins" says
a locked screen may blank while the greeter starts. Read as a constraint, it says look again — and
the second look finds that gyro already holds the wallpaper, so the locked screen is instant,
survives the greeter dying, and gives up nothing. The conflict was in the architecture, not in the
requirements. This is [Animation.md's second priority](Animation.md#priorities) on a different axis:
where the safe path and the good path diverge, the architecture is wrong.

What isolation *does* decide alone is **absence**. A locked screen shows nothing that belongs to you
until something is designed that can show it safely. An absence is bounded, stated, and revisited;
it is not a degradation, and it is never an excuse for one.

**Continuity and pacing almost never compete for the same pixel.** Continuity is about the gap
between two distinct images; pacing is about the rate within motion. Nearly every case of continuity
— a graphics device being replaced, a machine resuming, an output waiting for a session to be ready —
holds a *static* picture, and holding a static picture is imperceptible because there is nothing
moving to judder. A held image is not a dropped frame, and no promise here trades one for the other.

**Changing the display configuration is the exception, and it is handled by removing the conflict
rather than by ranking the two.** It is the one case that happens while the machine is live, so the
picture being held may be one that was moving. The answer is to make the change when nothing is
moving, which is available far more often than it sounds — and where it is not available, to take
the hold, because a hold is still not a flash. What is given up is a small delay on an action the
person deliberately took, which is the cheapest thing in this document to spend.

**Pacing outranks cohesion and image quality**, in that order: spend effect quality first, spend
frames last. A transition rendered with a cheaper blur is better than a transition that stutters,
and this is the same ordering [How it degrades](#how-it-degrades) states from the other end.

**Quality is spent by being set lower and left there, never frame by frame.** The ordering says which
thing to give up on a machine that cannot keep up; it does not license the picture changing while
somebody is looking at it, which is the promise under [The picture is
correct](#the-picture-is-correct). A blur that is slightly cheaper for as long as you own the machine
is invisible. The same blur leaving and returning between two refreshes is not a cheaper transition,
it is a broken one — and it is worth saying here, because reading this ordering as a per-frame trade
is how a system arrives at the second while believing it chose the first.

**Cohesion and sharpness are separated in time rather than ranked.** Things are permitted to be soft
while they move and are required to be sharp once they stop, so the apparent conflict between a
transition looking smooth and the result looking crisp is resolved by when each claim applies.

## One continuous image

**The display is written by exactly one thing, from the firmware handoff until the machine stops.**
Every seam below is a moment where some other system shows a black frame, a flash, a resolution
change, or a flicker, and where gyro shows none.

**Boot.** The firmware's own logo remains on screen, in place, until gyro takes over — and gyro
reproduces it at its position and scale in gyro's own mode, so the takeover is invisible. From there
it animates into the greeter. There is no boot splash, because there is nothing to splash over: the
whole boot is one picture that starts as the manufacturer's logo and becomes a login screen.

**Login.** After authentication the screen shows that user's own background, and their desktop
arrives over it. It does not go through an intermediate blank, and the desktop does not assemble in
visible stages — panel, then wallpaper, then windows. Three beats become one.

**Locking.** Locking is immediate and never waits for anything. The screen becomes the system's own
background at once, and whatever asks for your password arrives over it — the same way your desktop
arrives over that background at login. What you were doing goes on running as it leaves, so a film
playing when you lock does not freeze on its way off the screen. It does not blank first, it is not
delayed by anything starting up, and it does not stop being locked or stop being drawn if the thing
asking for the password dies.

**Suspend and resume.** The machine goes to sleep already showing the locked screen, so it wakes
showing the locked screen. There is no moment on waking where the desktop is readable by whoever
opened the lid, no black frame, and no flash of what was there nine hours ago. Opening a laptop is
the panel lighting up already showing the correct final image.

**Hardware changing underneath.** When the graphics device is replaced during boot, or a GPU is
unplugged, the last frame stays on the glass until the new one is ready. The screen holds; it does
not go black. At boot the held image is a static logo, so nothing is perceptible at all.

**Changing how the displays are set up.** Plugging in a monitor, unplugging one, or changing a
display's resolution or refresh rate does not blank anything and does not flash. The display being
changed holds its last picture until the new one is ready, and on some hardware the others hold with
it for a moment — a held picture is not a dropped frame. Where such a change would interrupt
something already in motion, the system waits for the motion to end before making it, but only when
the end is a moment away. It never becomes an indefinite delay to something you asked for.

**The compositor itself failing.** If gyro dies and restarts, the last frame holds and a greeter
fades in over it. The machine does not go dark. Everything that was open is lost, which is the
subject of [what is deliberately not promised](#what-is-deliberately-not-promised).

**The desktop failing.** If the shell crashes, windows stay on screen and stay usable under default
behaviour while it comes back. A shell restart is not a black screen.

**When nothing else can run.** A machine that cannot start a desktop still shows something legible
and can still be typed into. There is no configuration of this system in which the display is a
mystery.

*Mechanism: [Boot and the display lifetime](Architecture.md#boot-and-the-display-lifetime),
[Suspend and resume](Architecture.md#suspend-and-resume), [Device
migration](Architecture.md#device-migration), [decision
37](Decisions.md#37-gyro-owns-the-display-from-firmware-handoff-onward-there-are-no-vts), [decision
49](Decisions.md#49-the-restart-boundary-is-made-cheap-where-it-can-be-and-stated-where-it-cannot),
[decision 73](Decisions.md#73-the-frame-thread-initiates-reconfiguration-and-never-performs-it).*

## Every frame

**Missing a frame is a defect, not a tolerance.** A machine that renders the right picture late has
produced the wrong picture.

**Every display runs at its own rate, at the same time.** Plugging a 60 Hz projector into a 144 Hz
laptop does not slow the laptop panel to 60. A window dragged across the boundary between two
displays is correct on both simultaneously — not compromised at some rate between them, and not
correct on one and juddering on the other. Video playing on one display does not hitch the other.
This is the everyday configuration, and it is where this system should be visibly better than any
other.

**A gesture starts where your finger is, not where it was.** The first frame drawn after you touch
something shows the movement already underway by exactly the time it took to reach the screen — so
the system reads as tracking your hand rather than following it.

**The pointer never stutters.** Whatever else the machine is doing, the cursor keeps moving at the
display's full rate. There is no state of the system in which the pointer is the thing that hitches.

**A missed frame costs one frame.** It does not cascade, it does not compound, and the frame after
it is correct rather than lurching to catch up. Animation in flight is unaffected by the machine
having been late.

**When work arrives late, it arrives late — it does not arrive wrong.** A desktop component that is
slow to respond costs you the first frame or two of a transition, never the shape of it. The way
this system degrades is latency, not judder.

*Mechanism: [Presentation timing](Architecture.md#presentation-timing), [decision
28](Decisions.md#28-the-frame-clock-is-per-output), [decision
35](Decisions.md#35-a-miss-costs-one-frame-bounded-by-the-floor-composite), [Timing and
rates](Animation.md#timing-and-rates).*

## One hand made all of it

**A window opening, a menu appearing, a workspace switching, and a notification arriving should feel
like the same hand made them.** This is the first priority of the whole system, and it is the one
most easily lost a little at a time — every individually reasonable choice about how one thing
should move is an opportunity to lose it.

**Movement is information.** A menu collapses back toward the control it came from, which is what
tells you which control you hit. A window grows out of the corner it was summoned from rather than
out of its own middle, which is what makes the transition read as intentional rather than as a thing
getting bigger.

**Things that turn into other things stay continuous.** A window becomes its overview thumbnail and
comes back out of it; a switcher tile and an overview thumbnail remain the same object when you
change your mind mid-gesture; a window maximising flows from where it was. The alternative to
continuity is a cross-fade, and a system full of cross-fades is exactly what "not cohesive" looks
like.

**Anything moving can be interrupted at any moment, and the interruption is smooth.** Changing your
mind halfway does not produce a jump, a restart, or a fight between two animations. Dismiss a menu
and immediately reopen it and it reverses out of where it had got to. Fast repeated actions feel
right rather than janky — that is a property of interruption, not of speed.

**A transition you are making with your hands is yours to hold.** Stop moving and it stops where you
stopped, for as long as you care to leave it there. Reverse, and it runs backwards. The picture
stays under your fingers rather than trailing after them, and letting go carries the speed you let
go at into wherever it settles. Driving a transition and starting one are different acts, and only
the second is one you hand to the machine to finish.

**Several parts of the desktop reacting to one action produce one gesture.** A panel sliding away
and thumbnails sliding in are the same movement with the same origin, even though nothing
coordinated them.

**An animation either always plays or never exists.** Nothing animates most of the time and pops the
rest. That inconsistency is perceived as a stutter and blamed on whichever application just closed.

**Clicking something in flight hits it where it is going.** You interact with the world as it
logically is, not with the picture of it mid-transition.

**A window never travels a strange route.** Nothing arcs, spins the long way round, or overshoots in
a direction it was not going.

**Reduced motion removes movement rather than shortening it.** Turning it on does not give you the
same animations faster or with less bounce; it replaces sliding with fading and removes parallax and
scaling entirely. Motion sensitivity is not impatience.

**Two desktops built on gyro may look nothing alike and will still feel like the same machine.**
Cohesion is a property of the movement, not of the arrangement — which is what makes it something
the system can guarantee rather than something each desktop has to earn.

*Mechanism: [Priorities](Animation.md#priorities), [The motion
catalog](Animation.md#the-motion-catalog), [Interactive
transitions](Animation.md#interactive-transitions), [Matched
geometry](Animation.md#matched-geometry), [Lifetime](Animation.md#lifetime), [Exit
pixels](Animation.md#exit-pixels), [decision
13](Decisions.md#13-a-closed-motion-vocabulary-with-runtime-configuration-exposing-only-that-vocabulary),
[decision 18](Decisions.md#18-matched-geometry-is-in-the-first-cut), [decision
65](Decisions.md#65-interactive-transitions-are-driven-by-a-progress-parameter-not-by-a-moving-target).*

## The picture is correct

**Text is sharp.** Content that is sitting still and being displayed at its natural size is not
soft, not shimmering, and not half a pixel off — on any display, at any scale setting, including the
fractional ones. Nothing is blurry because it came to rest in the wrong place.

**Nothing shimmers while it moves, and nothing snaps when it stops.** Sharpness is not bought with a
visible click into place at the end of every transition.

**Scaling is exact.** Two tiled windows meet with no line of background showing between them, and
nothing is one pixel too big or too small at 110%. Window edges do not disagree with each other.

**The pointer can reach every part of the screen.** There is no display configuration in which some
columns of pixels cannot be pointed at.

**Moving content leaves nothing behind.** No trails, no fringes, no one-pixel residue at the edge of
something that moved.

**Dragging a window between two displays of different densities does not make anything stall.** It
re-sharpens for the display it is on without the application having to rebuild itself repeatedly at
the boundary.

**Light behaves like light.** Translucency, blur, shadows, and scaled-down images are all computed
the way light actually combines, so a blurred backdrop is the color it should be and a soft edge is
not subtly wrong. Half of what people recognise as "looks cheap" is this arithmetic being done in
the wrong space.

**Quality does not visibly fluctuate.** Effects do not get coarser when the machine gets busy and
finer when it calms down. A blur that breathes with system load reads as cheap even when no frame is
ever missed, so the level is chosen once and held.

**The image does not change when the machine changes how it draws it.** Promoting a full-screen
application to a more efficient path, changing displays, or falling back to a slower renderer must
not produce a visible shift in color or a flash. There is no moment where the picture jumps because
something under it got faster.

*Mechanism: [Geometry](Architecture.md#geometry), [Color](Architecture.md#color),
[Effects and quality](Architecture.md#effects-and-quality), [decision
54](Decisions.md#54-settled-geometry-snaps-to-the-outputs-device-grid), [decision
47](Decisions.md#47-compositing-happens-in-linear-light-at-wide-primaries), [decision
56](Decisions.md#56-clients-render-at-the-ceiling-and-gyro-downscales), [decision
34](Decisions.md#34-effect-quality-is-a-tier-gyro-chooses-and-the-floor-tier-is-the-recovery-path).*

## The machine holds several people

**Switching users is not logging out.** Another person's session stays alive and warm while yours is
on screen. Coming back to it is not a restart — everything is where it was left, still running.

**A locked session cannot be seen, reached, or listened to.** While a session is not on screen its
applications receive no keystrokes, no pointer, and no keyboard focus of any kind. There is no
arrangement in which one person types a password into another person's window.

**The lock screen cannot be faked.** A malicious application cannot draw a false password prompt on
a locked machine, because on a locked machine it is not drawing at all. This is a property of the
system rather than a rule the system tries to enforce.

**A locked machine is not an anonymous slab.** The system's own background is behind the lock screen,
as it is behind the greeter and behind the recovery console, so a locked machine looks like a machine
rather than like a black screen with a box on it. Nothing of your session is behind it — that is the
point of it rather than a shortcoming, and it is what makes the promise above hold without an
exception in it.

**Locking one screen does not stop the session.** A laptop locked at the desk keeps running a
session you are connected to from somewhere else. Lock is about what is presented, not about what is
alive.

**Someone connecting remotely to a locked machine does not light up its screen.** A machine in an
empty room stays dark while it is being used from elsewhere.

**No one is locked out by something crashing.** The screen you type your password into is on the
critical path back into the machine, so its failure is recoverable and never leaves the machine
unusable.

**Remote access is a first-class display, not a copy of one.** A session presented to another device
is rendered for that device, at its resolution and its rate — not composited for a screen nobody is
looking at and copied out afterwards. That is the difference between screen sharing that works and
screen sharing that is tolerated.

**Nothing about your session survives on disk without your say.** The material gyro keeps in order
to draw your machine belongs to your account and is not shown to anyone who has not authenticated as
you.

*Mechanism: [Sessions and users](Architecture.md#sessions-and-users),
[Locking](Architecture.md#locking), [decision
21](Decisions.md#21-many-sessions-connected-one-presented-locally), [decision
43](Decisions.md#43-lock-and-greeter-are-one-ui-locking-is-an-output-reassignment), [decision
26](Decisions.md#26-remote-presentation-is-a-virtual-output-with-client-supplied-targets).*

## Doing nothing costs nothing

**An idle machine draws nothing.** When nothing is moving and nothing has changed, no frames are
produced at all — not cheap frames, not occasional frames. Nothing.

**A blinking cursor in one window does not wake the rest of the machine.** Small local activity has
local cost. Moving the pointer across a still screen does not redraw the screen.

**Dimming is a real dim.** The screen gets darker the way a screen gets darker, not by having a grey
sheet drawn over it — so blacks stay black and colors stay right on the way down. It fades rather
than steps, and it is probably the most frequently seen animation in the whole system.

**Touching anything undims instantly and smoothly**, from wherever the dim had got to, with no jump
and no delay.

**The brightness you set is the brightness you get back.** An automatic dim composes with your own
setting and restores it exactly; it never quietly overwrites it.

**Something playing full-screen keeps the screen awake, and something playing in a background tab
does not.** Preventing sleep requires actually being visible on a screen that is on. This is the
common failure that drains laptops, and it is answerable here rather than approximable.

**You can find out what is keeping the machine awake.** It is attributable to the thing responsible,
by name.

**Never locking is a supported choice.** A machine at home may be configured never to lock, and a
machine under a policy may be configured so that no desktop can weaken it. Both are ordinary
configurations rather than one being a workaround.

*Mechanism: [Idle and power](Architecture.md#idle-and-power),
[Inhibition](Architecture.md#inhibition), [decision
58](Decisions.md#58-idle-is-a-ladder-gyro-executes-and-does-not-choose), [decision
59](Decisions.md#59-suspend-is-a-handshake-on-the-control-connection-resume-is-a-modeset).*

## How it degrades

Every system fails under enough load. What distinguishes one is *where* the failure is placed, and
that is a product decision rather than a scheduling accident. The rule throughout:

**Degradation lands where it is least likely to be perceived, and never on the thing being
touched.**

- **The display you are using is protected first.** When two displays cannot both be served, the
  one without your attention gives way, and a tie goes to the slower one. The panel under your hands
  keeps the lowest latency the machine can produce, unconditionally.
- **Effects give way before frames do.** The first thing spent is a small amount of quality in
  something that was about to be blurred anyway. Frames are given up only after that. What is spent
  is then *held* — the level follows the machine's ability to keep up, which changes rarely, and
  never because a single frame ran late.
- **When too many things close at once, the ones you are watching finish properly.** The rest
  finish early. Nobody is watching the twenty-ninth menu to close.
- **A component that misbehaves degrades itself.** An application that abuses the system spends
  its own share, and its neighbours are unaffected.
- **The fallback is a path the machine uses every day.** The simplest way this system can draw is
  not an emergency mode that has never run; it is a real mode, exercised constantly, so it works
  when it is needed.

*Mechanism: [Admission control](Architecture.md#admission-control),
[The floor tier](Architecture.md#the-floor-tier), [When there is no
room](Animation.md#when-there-is-no-room),
[decision 27](Decisions.md#27-resource-accounting-is-attribution-not-per-user-fairness),
[decision 66](Decisions.md#66-arrival-control-is-an-input-to-admission-control).*

## What is deliberately not promised

Stated so that they are choices rather than disappointments.

| Not promised | Instead |
| --- | --- |
| Your open applications survive gyro restarting | The screen survives it; the applications do not. Everyone logged in loses everything open, at once |
| Two people using one machine at the same time on different screens | One person at the machine at a time, with other sessions alive and warm behind |
| A locked screen showing notifications, media controls, or widgets | The system's own background, and nothing of yours at all — for now. This is a deferred absence rather than a refusal: content a locked session may present is designed and unbuilt. Acting on it while locked is refused permanently, since a keystroke meant for the password field must never reach a locked application |
| Overlapping surfaces that intersect in three dimensions | Depth is a visual effect, not a geometry model. Things stack; they do not pass through each other |
| Applications drawn at exactly their own scale on every display | Whatever is being displayed smaller than it drew itself is scaled down, which is the direction that survives scaling well |
| X11 applications sharp on mixed-density displays | One density, resampled elsewhere. The limitation belongs to X11 |
| Client-drawn window shadows looking as their authors intended | They will look lighter and flatter than on other systems, because other systems compute them incorrectly. The system draws its own instead |
| Configuring how any individual transition moves | The vocabulary is tunable; individual transitions are not, because that is exactly how cohesion is lost |

*Mechanism: [decision 49](Decisions.md#49-the-restart-boundary-is-made-cheap-where-it-can-be-and-stated-where-it-cannot),
[decision 21](Decisions.md#21-many-sessions-connected-one-presented-locally),
[decision 43](Decisions.md#43-lock-and-greeter-are-one-ui-locking-is-an-output-reassignment),
[decision 55](Decisions.md#55-transforms-are-3d-the-scene-is-a-painters-algorithm),
[decision 56](Decisions.md#56-clients-render-at-the-ceiling-and-gyro-downscales),
[decision 48](Decisions.md#48-linear-blending-is-a-visible-ecosystem-change-and-gyro-takes-it),
[decision 13](Decisions.md#13-a-closed-motion-vocabulary-with-runtime-configuration-exposing-only-that-vocabulary).*

## What is not yet asserted

The gathering exposed these. Each is a place where the system currently has a mechanism opinion and
no product opinion, or no opinion at all — and the second tier cannot be written correctly against a
promise that does not exist.

- **Accessibility beyond reduced motion.** Magnification, color filters and high contrast, cursor
  size and visibility, focus visibility, on-screen keyboard, screen reader behaviour, and the input
  accommodations. Two of these are not additive — magnification is a scaling and resampling promise
  and belongs beside *the picture is correct*, and color filters are a color promise — so their
  absence here is not neutral.
- **What the system actually feels like to operate.** The catalogue of transitions, the materials,
  and the gestures are all declared closed and none is enumerated. Section [One hand made all of
  it](#one-hand-made-all-of-it) asserts that everything moves as one system without saying what any
  of it does. This is the largest hole and it is the one that most changes what gets built.
- **Latency, as a number.** *Feels like tracking your hand* is asserted; no figure is. There is a
  measurable input-to-photon time per display and it should be a promise with a value, since it is
  the one claim here that is trivially falsifiable and currently unfalsified.
- **What using a second machine as a display is like**, as distinct from the assertion that it is
  a real display. Latency, quality, and what happens when the link degrades.
- **Full-screen and game behaviour.** Whether tearing is ever permitted, what a full-screen
  application is promised about latency, and what happens to the rest of the machine while one is
  running.
- **Failure that the user sees.** What is shown when an application stops responding, when a
  display cannot be driven at its native mode, when the machine is thermally throttled, or when
  something cannot be authenticated. Currently only the total-failure console is described.
- **Hotplug as an experience.** Plugging in a display, unplugging one with windows on it, docking,
  and closing a lid with an external attached are all reconfigurations the user watches happen. What
  they look like, and where the windows go, is unstated.
- **Upgrading.** Updating this system restarts the one process every session on the machine
  depends on. What a person is promised about that — whether it can happen while they are working,
  and what they lose — is a product decision that has not been made.
- **The recovery console as a thing people use.** It is described as a subsystem and not as an
  experience: how it is summoned, what it can do, and what it requires of whoever summons it.
- **Sound, notifications, and the rest of the desktop**, which are not gyro's and are not promised
  here, but whose boundary should be stated so that the absence is deliberate.
