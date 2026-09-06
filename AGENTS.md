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

**You are probably not alone in this tree.** Another agent is often editing it at the same time, and
the churn that causes is worth more care than it looks. Three habits, all of them earned:

- **Don't try to isolate your change unless you confirm with the user.** No stashing, no copying the tree somewhere clean, no reverting
  somebody else's files to get a quiet test run. It costs far more than it settles (clean builds take a while and contend with others on the same machine), and the answer it
  buys was usually available by reading. If in doubt, defer it to the end and the user will check.
- **A failure in a file somebody else has uncommitted changes to is theirs until shown otherwise.**
  `git status` first, then read the failing assertion: if it names a feature you did not touch, say so
  and carry on. Say it *out loud* rather than silently discounting it, so Paul can tell you are not
  reporting a green run over somebody else's red one.

- **Ask before implementing** where an answer would make the implementation better, and ask when you
  think extra effort would make the code better. Code quality is the point.
- **Ask before adding a dependency**, and justify it.
- **Build and test before saying a change is done, and do it through `Tools/Build.sh`** rather than
  by invoking ninja yourself. It configures if it has to, builds, runs `ctest`, and holds an
  exclusive lock across all three. Ninja takes no lock on its own `.ninja_deps` and `.ninja_log`, so
  two agents building at once leave caches the next run rejects — and a rejected deps log is a full
  rebuild, several minutes on this machine, on every edit after it until somebody notices. If
  another agent is building, the script says whose build it is waiting behind and then runs; it is
  not hung. `Tools/Build.sh --no-test` stops after the build, a bare argument is a ninja target, and
  `--wait <seconds>` bounds the wait. The disciplines below are `ALL` targets, so an ordinary build
  runs them.
- **Format C++ with the project's `.clang-format`** before finalizing.
- **A commit subject says what the commit did**, imperatively — `Render: Modulate the GPU frequency
  floor`, `Bindings: Split the emitter into a shared floor and two arms`. The declarative form
  belongs to `Docs:` commits, where the subject *is* the claim being recorded. It has leaked into a
  few code commits, so reading only the last handful of subjects gives the wrong pattern.
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
thing to build or should be split into smaller tasks say that before building it.

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
| Why not X? What was rejected? | [Docs/Decisions.md](Docs/Decisions.md), 189 entries, anchored `### N.` |
| What is still unsettled? | [Docs/Open.md](Docs/Open.md) |
| What does gyro want from the kernel and cannot have? | [Docs/KernelWishlist.md](Docs/KernelWishlist.md) |

Docs are tiered: Experience → Architecture and Animation → Structure. Citations point up, dependencies
point down. A tier-2 document may cite Experience as justification; Experience may never require a
mechanism document to be understood. Each doc states its own volatility at the top.

**Read the relevant docs before proposing an architectural change** — not for permission, but so the
argument you make is one the log has not already answered. See [Questioning](#questioning).

## The map

One line per module: which tier it is in, which thread runs it, and what it is. That is enough to
know where to look and what may not depend on what.

**What each module actually contains — file by file, with the invariant each one carries — is
[Structure.md's "What each module contains"](Docs/Structure.md#what-each-module-contains).** It used
to be in this table, which meant reading all of it on every query to answer a question about one
module. Read the row to find the module; read that section once you are in it. Structure.md also has
the dependency graph and the thread partition.

| Module | Tier | Thread | What it is |
| --- | --- | --- | --- |
| `Core` | portable | either | The timebase, `IClock`, `Signal`, `Result`, `Fd`, `Wake`, `Handle`, `SlotAllocator`, `FrameSection`, the always-armed trace ring, and the world-authoring types that must sit below both waists |
| `Geometry` | portable | either | `Scale`, `Space`, `Region` — the coordinate spaces and the damage set, with the integer grid kept off the world |
| `Wire` | portable | either | The Wayland wire codec: framing, the argument vocabulary, `SCM_RIGHTS`, the per-connection object map. One caller today (`Nested`), and below both waists because `Nested` is split |
| `World` | portable | both | What a node is — the record `Scene` writes and `Frame` walks, and neither may name the other — plus `Root`, the session partition beside it |
| `Text` | portable | both | Monospaced bitmap text, and a *producer of pixels* rather than a verb on `IRenderer`: face, size ladder, raster, label. Spleen, baked by `Tools/Fonts` |
| `Animation` | portable | both | Split by direction: `Solve/` is the closed forms the frame thread evaluates, `Author/` produces coefficients and is dispatch-side |
| `Publication` | portable | both | The data waist: `Snapshot`, `Ring`, `Return`, with `Reader/` frame-side and `Publisher/` dispatch-side |
| `Scene` | portable | dispatch | What writes a node: the entity store, the serializer, the settle thresholds, focus, the cursor, the background, the commit scope, and the return drain |
| `Gym` | portable | dispatch | The scenes gyro authors for itself with no protocol behind them — the instrument for looking at what the compositor actually draws |
| `Session` | platform | dispatch | The listener handover: how gyro learns a session exists, a session offered whole over a world-writable control socket |
| `Protocol` | platform | dispatch | gyro's Wayland server and the author with clients behind it: the globals, xdg-shell, the seat, the floor, the clipboard, and gyro's own two protocols |
| `Input` | platform | dispatch | Where a keystroke comes from: libinput behind `IInput`, the `Ctrl+Alt+Esc` chord and `Alt+Tab`, and the external trigger fifo |
| `Dispatch` | portable | dispatch | The dispatch thread's iteration — collect, retarget, serialise, publish, answer when to come back — and the texture minter |
| `Trace` | portable | own | The expensive half of tracing: Perfetto protobuf, the recorder, the `SIGUSR1` request, the writer thread, and the reader that checks what gyro wrote |
| `Seam` | portable | both | The control waist: every interface with more than one implementation, the data crossing it, and `Dressing`'s numbers |
| `Frame` | portable | frame | The frame thread's step: clock, budget, admission, the evaluator, the plane assignment, and the six trace rows per output |
| `Headless` | **portable** | split | Simulated vblanks and a renderer that charges a cost and draws nothing — the instrument the schedulability sweep runs against |
| `Blit` | **portable** | frame | The CPU renderer and the floor beneath the floor tier, with the mapping arm of the importer |
| `Render` | platform | frame | The Vulkan renderer: device by class, pipelines, backdrop, textures, the GPU frequency governor, and the unfused execution |
| `Virtual` | platform | frame, own | The presenter that *allocates*: an output whose consumer is a file, an encoder, or a test rather than a panel |
| `Nested` | platform | frame | The daily driver — gyro as a client of another compositor, one host window per output |
| `Drm` | platform | frame, own | The panel: the card node, an atomic-commit thread per output, planes, fences, and the scanout importer |
| `Compositor` | platform | constructs | The composition root and the first non-portable module — `io_uring`, `SCHED_FIFO`, `mlockall`, both threads' waits, and `IBackend` |
| `Integration` | portable | — | The tests that name two modules no module may |
| `Testing` | portable | — | The hand-rolled harness and every test binary's `main()` (9) |

`Source/Main.cpp` is a thin entry point, `Tools/` holds the probes and the trace dumper, and
`Protocols/` holds gyro's own protocol XML — all in
[Structure.md](Docs/Structure.md#tools-and-the-rest-of-the-tree). **`Tools/Build.sh` is not one of
them**: it is the build itself, serialised behind a `flock`, and Working model above is why that
matters.

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
