gyro is a compositor for Linux / Wayland that is an entire system layer: one compositor per system,
not per session. It runs `SCHED_FIFO` with the goal of hitting every frame, and it handles animation
and visual effects itself.

It uses no VTs and has no session dependencies — device access comes from udev rules, DRM master from
first-open, and session lifecycle from a listener handed to it by a per-session agent. That makes it a
boot service rather than a login one: it subsumes the boot splash, continuing the firmware's BGRT
logo, and provides the recovery console that replaces VTs.

Stack: C++23, CMake with CPM, Vulkan, spdlog. Nothing commercial.

## Working model

**Never commit.** Stage the change, write the commit message, and hand both to Paul for review. He
commits. When a change is ready, check that there are no currently staged changes, then if not you can
`git add` it, hunkwise if necessary to avoid accidentally pulling another agent's work in. In any case
put the message in the reply — subject on one line, blank line, then the body.

- **Ask before implementing** where an answer would make the implementation better, and ask when you
  think extra effort would make the code better. Code quality is the point.
- **Ask before adding a dependency**, and justify it.
- **Build and test before saying a change is done**: `cmake -S . -B build -G Ninja && ninja -C build
  && ctest --test-dir build`. The disciplines below are `ALL` targets, so an ordinary build runs them.
- **Format C++ with the project's `.clang-format`** before finalizing.
- No `Co-Authored-By:` or other attribution footers in commit messages.

### Explaining your work

**Never cite a decision number without saying what it is.** "Decision 87" tells Paul nothing and
costs him a follow-up question. Write "the rule that keeps the world-authoring types below both
waists (87)" — the number is a pointer for the next agent, the words are for the reader.

**Justify a choice by what a person using the compositor would perceive**, not by the codebase's own
vocabulary. A waist, a run, a watermark, a floor composite: these are internal names, and reaching
for them to explain *why* something is right is almost always a way of avoiding the real argument.
Say what breaks on screen, what stutters, what a user waits for.

**Lead with the conclusion, then name where to dig.** A short summary and a list of two or three
places worth opening beats paragraphs of justification. Paul is usually already onboard with the
part you are about to explain at length. Let him ask for depth rather than pre-empting it.

**But always name the alternative you rejected, in one sentence.** "Not TCP — the delayed-ack stall
would land as input lag." That is enough for him to pull the thread or say "ah yeah" and move on.
Leaving it out makes him ask; arguing it over three paragraphs is the thing he does not want. This
is how [Decisions.md](Docs/Decisions.md) is already written — the rejected alternative is the point
of an entry — so speak the way the log reads.

### Questioning

**Question anything that looks wrong, and say so.** A decision in the log, a paragraph in the docs, a
line already in the tree: none of them earn deference by existing. The docs have been wrong before —
decision 2's conclusion was *reversed* by an afternoon spent reading libwayland, and that reversal is
the best thing in the log. Silent compliance with something you think is mistaken is the one failure
mode this project cannot absorb, because it ships as a frame drop nobody can trace.

**Re-litigating is wanted, and the bar is the argument rather than permission.** Read what the entry
already rejected and engage with it — a proposal that restates the alternative the log dismissed
wastes a round trip. A proposal that breaks a premise the log rested on is exactly what should happen.

**Try to kill it yourself first, and bring what survives.** If the question can be settled by reading
the source, working the arithmetic, or writing the test, do that and bring the conclusion instead of
the question — including when the conclusion is "I was wrong, never mind," which is a better outcome
than an interruption. What earns an interruption is a disagreement that survived your own attempt to
refute it, or a choice where you genuinely cannot know Paul's preference.

**This applies to the task, not only the code.** If the thing being asked for looks like the wrong
thing to build, say that before building it.

Nothing here is sacred and there is no "mine" versus "yours" — we are building this together, and the
world outside the repo is malleable too. If the right answer is a kernel change or a new userspace
standard, say so and we will talk about it.

## Where to look

| Question | Read |
| --- | --- |
| What does a person perceive? What is promised? | [Docs/Experience.md](Docs/Experience.md) |
| How does a mechanism work — seam, backends, timing, color, sessions, protocol? | [Docs/Architecture.md](Docs/Architecture.md) |
| How does animation work — springs, catalog, commits, transforms, identity? | [Docs/Animation.md](Docs/Animation.md) |
| Where does code live, what may depend on what, which thread runs it? | [Docs/Structure.md](Docs/Structure.md) |
| Why not X? What was rejected? | [Docs/Decisions.md](Docs/Decisions.md), 121 entries, anchored `### N.` |
| What is still unsettled? | [Docs/Open.md](Docs/Open.md) |
| What does gyro want from the kernel and cannot have? | [Docs/KernelWishlist.md](Docs/KernelWishlist.md) |

Docs are tiered: Experience → Architecture and Animation → Structure. Citations point up, dependencies
point down. A tier-2 document may cite Experience as justification; Experience may never require a
mechanism document to be understood. Each doc states its own volatility at the top.

**Read the relevant docs before proposing an architectural change** — not for permission, but so the
argument you make is one the log has not already answered. See [Questioning](#questioning).

## The map

Source layout and the invariant each module carries. The *why* is in the cited decision;
[Structure.md](Docs/Structure.md) has the dependency graph, the thread partition, and the tier table.

| Module | Tier | Thread | What it is |
| --- | --- | --- | --- |
| `Core` | portable | either | Timebase (`Time.h`), `IClock`, `Signal`, `Result`, `Fd`, `Wake`, `Handle` the generational identity (15), `SlotAllocator`, `FrameSection`, `ColorState`, `Texture`. Types the world authors live here rather than `Seam`, because neither `Protocol` nor `Scene` may name `Seam` (87) — the ones that also need a coordinate go to `World` instead (91) |
| `Geometry` | portable | either | `Scale` the exact rational, `Space` the coordinate spaces, `Region` the damage set. The integer grid is kept off the world (52, 53) |
| `Wire` | portable | either | The Wayland wire codec: framing, the argument vocabulary, `SCM_RIGHTS` fd passing, the per-connection object map. Below both waists because `Nested` is split and reaching `Protocol` for a codec would put `Scene` on the frame side. One caller today — decision 2 leaves the server codec to libwayland — and what earns the module anyway is that a codec with no backend in front of it is testable against a `socketpair` peer the test wrote. Portable because `sendmsg` over `AF_UNIX` is POSIX; a connection is pumped by one thread, which is decision 81's rule per object rather than per module (2, 81, 119) |
| `World` | portable | both | What a node is. `Node` is the record the published scene is a preorder run of — `Scene` writes it, `Frame` walks it, and neither may name the other, so it lives below both waists rather than in either half (86, 90, 91). The node kind and `Material` join it when the scene vocabulary lands |
| `Animation` | portable | both | Split by direction: `Solve/` is the closed forms the frame thread evaluates, `Author/` produces coefficients and is dispatch-side (11, 12, 72) |
| `Publication` | portable | both | The data waist. `Snapshot` the offset-addressed layout — coefficient runs by channel, and the scene, the per-output wakes, the per-output placement, and the per-kind content runs by name — `Ring` the newest-wins forward channel, `Return` the per-frame report carrying the watermark; `Reader/` frame-side, `Publisher/` dispatch-side (45, 50, 74, 75, 86, 90, 97) |
| `Scene` | portable | dispatch | What writes a node. An entity is a `Node`'s authoring side and the two are one to one, so the store is one tree rather than entities owning subtrees — `SlotAllocator` for identity, parent / first child / next sibling held as handles, and a top level that is a list rather than a root (111). `Serializer` is decision 86's preorder run coming out the other end: model values inline, coefficients by reference, and a coefficient only where a channel is still owed a frame — which is also where a channel that has settled is *retired*, since the walk that decides whether a coefficient crosses is the walk that has the thresholds in hand (86, 90, 98, 122). `Settle` is those thresholds as numbers: device pixels of the finest grid for geometry, an eight-bit code point for opacity, and the two substitutions both rest on — every output rather than the ones a node intersects, and the screen's half-diagonal rather than a node's own radius (54, 122). `Output` is the output model decision 87 has it declare for itself rather than reach into `Seam` for. `Commit` is decision 112's scope — one open at a time, carrying an author and an origin, closing when the wire request that opened it does — and it is the only door onto a channel, because decision 89 makes setting a model value *be* a retarget under that shared origin. The wake schedule is folded here too, scene-wide and replicated per output, because partitioning a node's contribution needs a screen-space bound dispatch does not have — the cost is an idle panel compositing for its neighbour's animation, and it is an Open.md entry rather than a silence (69, 122). Phase two is not built: matching, lifetime, the atlas reservation, and every derived geometry resolve at close, where nothing runs yet (89, 112) |
| `Gym` | portable | dispatch | The scenes gyro authors for itself, with no protocol behind them: a handful of nodes moving under springs, as the instrument for looking at what the compositor actually draws. `Lanes` is the scene — one lane per sprung channel, each a still track and a marker that moves against it, so a reader can tell from one frame *which* channel is misbehaving. `Gym` is the four drivers and the vocabulary that names them: `lanes` retargets forever, which is the only way a scene keeps moving once settling is exact, and is therefore the traffic decision 89's eager path is measured at; `settle` authors once and answers `Never()`, which is the only instrument for *doing nothing costs nothing*; `turn` and `materials` want the Vulkan renderer, because `Blit` refuses a rotated quad, a material and an elevation alike and one refusal loses the whole frame — which is what `DrawsOnCpu` exists to say before the directory stays empty. A module rather than root files because `PORTABLE` is what keeps it authoring the way a shell would, and the boot splash is the second scene of this shape (13, 55, 79, 89, 112) |
| `Seam` | portable | both | The control waist: every interface with more than one implementation and the data crossing it. `IPresenter`, `IEventSource`, `IRenderer`, `ISession`, `IInput`, `IDmabufAllocator` — which moved up from `Virtual` when a second backend wanted one and a second module implemented it (120) — `RenderTarget`, `SyncPoint`, `PresentationInfo`, `OutputConfiguration`, `RenderMode`, and `Pixel` — what a fourcc means in memory, beside the fourccs, because `Blit` encodes through it and a test decodes through it. `Dressing` is that argument for numbers rather than types: the radius, the tint, `Smoke`'s contrast floor and decision 34's tier table, here because two renderers must produce one picture and because decision 33 keeps a radius out of the header a call site includes (33, 34, 63, 73, 78, 79, 80, 82, 117) |
| `Frame` | portable | frame | `FrameClock` the per-output prediction, `Budget` the cost figures, `Timing` the one runtime timing decision, `Loop` the step, `Admission` the processor-demand test read backwards, `Projection` the composed chain turned into a `Quad` — it is here because `Geometry` may not name `Seam` and a quad builder is not an interface (93). `Evaluator` the walk that turns a published scene into a draw list, whose interface is internal because no backend is ever on the far end of it (29, 30, 35, 61, 80, 97–101) |
| `Headless` | **portable** | split | The instrument the schedulability sweep runs against, so it must work on a machine with no GPU. Simulated vblanks, an `IPresenter` over them, one `IEventSource` for all of them, a synthetic plane catalog, a renderer that charges a cost and draws nothing (85) |
| `Blit` | **portable** | frame | The CPU renderer, and the floor beneath the floor tier: `Band` the linear premultiplied scratch a composite happens in, banded to stay in cache so the panel's size stays out of the footprint, `Transfer` the sRGB curve in the two forms two rates need, `Blit` the `IRenderer` itself. Portable because a composite into a mapped pointer names no platform header, and that is what keeps the renderer that runs when there is no GPU testable on a machine with none (79, 110) |
| `Render` | platform | frame | The Vulkan renderer. `Device` brings up an instance and a device by *class* — decision 40 makes software rendering a device selection rather than a backend, so the floor tier is `DeviceClass::Software` — and `Renderer` imports the presenter's dmabufs under an explicit DRM modifier and composites the damage region. It is also where a nested output's targets are *exported* from — the same modifier query, `VkExportMemoryAllocateInfo` on the allocation, and the fd going out instead of coming in (120). `Pipeline` is the quad pipeline and the first entry in decision 62's variant lattice, so a solid draws. `Backdrop` is the second and the first *gathering* one: it reserves the offscreens a blur chain ping-pongs through, splits the render pass so a material can read the target it is drawing into, and falls to the tint where the device will not have that — which is decision 34's third rung, and the first behaviour either renderer attaches to `RenderMode`. Glass owns it and decision 60's group flattening will not want it, because a group *writes* an offscreen and a material *reads the one it is already on*. A texture, a group or an elevation is still refused rather than silently dropped. `Unfused` is decision 62's *other* execution and the thing its oracle compares against: every element of a pointwise chain as a pass of its own through a half-float intermediate, five pipelines rather than the lattice's twenty because a separate pass carries its conversion selectors as data, and selected by a `Fusion` fixed at construction so both executions can be live at once against one device (103, 107, 108, 109, 116, 117, 118) |
| `Virtual` | platform | frame, own | The presenter that *allocates*: an output whose consumer is a file, an encoder, or a test rather than a panel. `udmabuf` turns a sealed `memfd` into a real dmabuf, so the renderer is exercised against real imports on a machine with no GPU. Targets retire on consumer release, not on a flip, which is why it is not `Headless` with a provider. It still owns the `udmabuf` provider; the interface it implements is `Seam`'s now. `Dump` is the consumer that is a file and the module's one thread: the sink copies a frame on the frame thread and a writer does the `open` and the `rename`, because the deferral in `Sink.h` was about handing over the *descriptor* — which races the ring — and a copy races nothing (102, 120, 121) |
| `Compositor` | platform | constructs | The composition root and the first non-portable module. `io_uring`, `SCHED_FIFO`, `mlockall`, `RLIMIT_RTTIME` live here *because* `Frame` and `Headless` may not say those words. `Uring`, `Schedule`, `RealTime`, `Options`. `IBackend` is the fork the second backend forced — a source, a `NextEvent`, and a presenter-plus-renderer per output — and it is root-local rather than a seam type on purpose, because two clock-driven fakes are not two implementations and `IEventSource` would gain a verb only they answer (80, 83, 121) |
| `Integration` | portable | — | The tests that name two modules no module may: `Publication` with `Animation`, and `Render` with `Virtual` — a renderer and a presenter, which only the composition root wires together |
| `Testing` | portable | — | The hand-rolled harness and every test binary's `main()`: `GYRO_TEST` / `GYRO_CHECK` / `GYRO_REQUIRE` (9) |

Also: `Source/Main.cpp` is a thin entry point; `Tools/UringProbe.cpp` is a standalone io_uring probe
with raw syscalls and no liburing, so it runs on a target machine before gyro does (3);
`Tools/VulkanProbe.cpp` is the same idea for Vulkan — headers only, `dlopen`s the loader, runs where
there is no ICD — and it is what retired decision 40's unverified extension claim and produced 108.

Tests live beside what they test as `<Unit>.Test.cpp`, and are listed in the module's `TESTS` rather
than compiled into it. A test for something in a dispatch half belongs in that half.

## Disciplines the build enforces

A discipline belongs here rather than in a style guide nobody greps. All of these fail the build
rather than waiting for review (36).

- `CheckClockDiscipline.cmake` — one reader of the timebase (57)
- `CheckPortability.cmake` — no platform headers in a `PORTABLE` module (6)
- `CheckLayering.cmake` — every `#include` lies on a declared edge, the graph is a DAG, nothing
  outside a dispatch half includes one, and a narrowed frame half reaches only its `FRAME_DEPENDS`
- `Core/DebugAllocator.cpp` — aborts on an allocation inside a `Core/FrameSection.h` guard. Not a
  build check; it is the one decision 36 actually names

`gyro_add_module()` in `CMake/Module.cmake` is where a module declares itself to all of them:
`PORTABLE`, `DEPENDS`, `DISPATCH_HALF` / `DISPATCH`, `FRAME_DEPENDS`, `SOURCES`, `TESTS`. Only the
dispatch half is ever named — the frame side is what is being protected, so it is the default.
`CMake/BuildFlags.cmake` holds the warning set, `GYRO_SANITIZE`, `GYRO_WERROR`, and `GYRO_LTO`.

## How decisions get made

Answering something in [Open.md](Docs/Open.md) produces an entry in
[Decisions.md](Docs/Decisions.md). Decisions.md is append-mostly: a revised decision keeps its
superseded position as a rejected alternative, because the reasoning that led somewhere wrong is the
most useful part of a log. Revisions are marked inline and dated at the paragraph they touch; there
is no global ledger of what revised what.

**Write the decision when the change is still cheap.** Decision 69 changed a type, and the argument
for making it then was that the type had one caller. The trigger is not the size of the question, it
is the size of what has been built on top of the current answer.

Five entries have been settled by going and reading the source an argument rested on, and they taught
four rules worth applying before the sixth:

- **An entry that names the source its argument rests on can be retired by an afternoon of reading.**
  An entry that names no source needs a frame loop, a panel, or a user in front of it.
- **Read even when you expect to be confirmed.** Decision 49's answer was right and its argument was
  wrong; that is the kind that survives review and fails in the field.
- **A question that resists the reading may be malformed rather than hard, and the thing to suspect
  is the rule upstream of it.** Decision 76's question was circular, and reading found an absence
  rather than an answer.
- **A *does X forward Y* question is settled by enumerating everything X does send, not by searching
  for Y.** Decision 106's answer was that the destination did not exist, which no amount of grepping
  for the source would have shown: a grep that finds nothing only proves the grep.

When a reading changes something, the narrative belongs in the decision it changed rather than in a
preamble that accretes one paragraph per event.
