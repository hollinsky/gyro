cmake_minimum_required(VERSION 3.25)

project(gyro
	VERSION 0.0.0
	DESCRIPTION "Gyro Compositor"
	LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# ISO rather than GNU. Core/Clock.cpp asks for POSIX by defining _POSIX_C_SOURCE where it needs it,
# which is the portable tier's rule spelled out: what is wanted from the platform is named.
set(CMAKE_CXX_EXTENSIONS OFF)
# Lock C standard before any CPM dep can override it (libgit2 sets C90 in cache)
set(CMAKE_C_STANDARD 23 CACHE STRING "The C standard to compile against" FORCE)
set(FETCHCONTENT_QUIET OFF)

set(GYRO_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/Source)

# Setup CPM
set(CPM_SOURCE_CACHE "${CMAKE_SOURCE_DIR}/.cpm-cache" CACHE PATH "CPM cache directory")
include(CMake/CPM.cmake)

include(CMake/BuildFlags.cmake)
include(CMake/Module.cmake)
include(CMake/Shaders.cmake)
include(CMake/Bindings.cmake)

enable_testing()

find_package(Git)

add_custom_target(AppVersion
	COMMAND ${CMAKE_COMMAND}
		-D GIT_EXECUTABLE=${GIT_EXECUTABLE}
		-D GIT_WORKING_DIR=${CMAKE_CURRENT_SOURCE_DIR}
		-D INPUT_FILE=${CMAKE_CURRENT_SOURCE_DIR}/Source/Version.h.in
		-D OUTPUT_FILE=${CMAKE_CURRENT_BINARY_DIR}/Generated/Version.h
		-D PROJECT_NAME=${PROJECT_DESCRIPTION}
		-D VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
		-D VERSION_MINOR=${PROJECT_VERSION_MINOR}
		-D VERSION_PATCH=${PROJECT_VERSION_PATCH}
		-P ${CMAKE_CURRENT_SOURCE_DIR}/CMake/Version.cmake
	DEPENDS
		${CMAKE_CURRENT_SOURCE_DIR}/Source/Version.h.in
	BYPRODUCTS
		${CMAKE_CURRENT_BINARY_DIR}/Generated/Version.h
)

# The timebase has exactly one reader; checked on every build. ALL rather than a dependency of gyro,
# so the rule covers the test binaries and anything else added later without being remembered.
add_custom_target(ClockDiscipline ALL
	COMMAND ${CMAKE_COMMAND}
		-D SOURCE_DIR=${GYRO_SOURCE_DIR}
		-P ${CMAKE_CURRENT_SOURCE_DIR}/CMake/CheckClockDiscipline.cmake
	VERBATIM
)

# The platform stack, consumed through pkg-config; CPM is for C++ libraries. See
# Docs/Architecture.md#dependencies, where this row already sits.
#
# liburing is the first of them, and it arrives with the frame ring rather than with the event loop in
# general: the dispatch ring is a convenience where plain epoll would be defensible, and the frame
# ring is where DEFER_TASKRUN earns its place. Docs/Architecture.md#why-io_uring has the argument, and
# Source/Compositor/Uring.h has the one reap-site constraint that follows from it.
#
# Named REQUIRED with a sentence rather than left to pkg-config's own message, because the machine
# that hits this is a machine somebody is setting up and the fix is one package away.
find_package(PkgConfig REQUIRED)
pkg_check_modules(liburing IMPORTED_TARGET liburing)

if(NOT liburing_FOUND)
	message(FATAL_ERROR
		"liburing was not found. gyro's frame ring is io_uring and carries no epoll fallback, "
		"deliberately; see Docs/Architecture.md#dependencies. Install liburing-devel (Fedora) or "
		"liburing-dev (Debian)."
	)
endif()

# The frame thread is a thread, and the composition root is what starts it. Declared here with the rest
# of the platform stack rather than beside the one target that used to want it, because it is now two:
# Compositor spawns the frame thread and drives its ring from a second one under test, and Integration
# runs the publication soak.
find_package(Threads REQUIRED)

# spdlog, on std::format rather than on its bundled fmt.
#
# This codebase writes std::formatter specializations — Wake, FrameDecision, OutputConfiguration,
# Budget, Plan — and writes them precisely so that a log line can print one. Left on bundled fmt,
# spdlog would accept none of them and every log site would hand-expand the fields, which is how a
# formatter and the line that prints it drift apart. One vocabulary, decided here.
set(SPDLOG_USE_STD_FORMAT ON CACHE BOOL "spdlog formats with std::format" FORCE)

CPMAddPackage(
	NAME spdlog
	GITHUB_REPOSITORY gabime/spdlog
	GIT_TAG v1.17.0
	SYSTEM YES
	EXCLUDE_FROM_ALL YES
)

# Vulkan, and every part of it arrives through CPM rather than pkg-config. See
# Docs/Decisions.md decision 107; the short of it is that gyro never links the loader. volk resolves
# it with `dlopen("libvulkan.so.1")` at startup, so a machine with a broken Vulkan install answers
# `volkInitialize()` with a failure gyro can report and fall back from, rather than refusing to exec
# a boot service. The headers are the only build input, and they are headers.
#
# The two are pinned to the same SDK on purpose: volk's dispatch table is generated from a header
# revision, and a table built against one revision over a `vulkan_core.h` from another is a null
# entry in the middle of a struct.
set(VULKAN_HEADERS_ENABLE_MODULE OFF CACHE BOOL "No Vulkan-Hpp C++20 module; gyro is not on Vulkan-Hpp" FORCE)

CPMAddPackage(
	NAME Vulkan-Headers
	GITHUB_REPOSITORY KhronosGroup/Vulkan-Headers
	GIT_TAG v1.4.357
	SYSTEM YES
	EXCLUDE_FROM_ALL YES
)

# Named explicitly rather than left to volk's own search, which tries FindVulkan first and would
# quietly prefer a distribution's headers over the pinned ones on a machine that has both.
set(VULKAN_HEADERS_INSTALL_DIR "${Vulkan-Headers_SOURCE_DIR}" CACHE PATH "The headers volk generates against" FORCE)

CPMAddPackage(
	NAME volk
	GITHUB_REPOSITORY zeux/volk
	GIT_TAG vulkan-sdk-1.4.357.0
	SYSTEM YES
	EXCLUDE_FROM_ALL YES
)

# The GLSL front end, and it is a build tool rather than a library gyro links. CMake/Shaders.cmake is
# the argument; the short of it is that decision 103's variant lattice is enumerable, so every module
# exists before gyro boots and nothing at runtime turns text into SPIR-V. Pinned to the same SDK as
# the two above for their reason.
#
# Three options turn off three quarters of the build. ENABLE_OPT is the one that matters: it is what
# pulls in SPIRV-Tools, and the optimizer is not wanted on shaders this small. HLSL is a front end
# gyro will never speak and is deprecated upstream; the SPIR-V remapper is a size tool for platforms
# that ship uncompressed shader blobs.
set(ENABLE_OPT OFF CACHE BOOL "No SPIRV-Tools; gyro does not optimize its own shaders" FORCE)
set(ENABLE_HLSL OFF CACHE BOOL "gyro writes GLSL" FORCE)
set(ENABLE_SPVREMAPPER OFF CACHE BOOL "Nothing here remaps SPIR-V" FORCE)
set(GLSLANG_TESTS OFF CACHE BOOL "glslang's own suite is not gyro's" FORCE)
set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "Nothing here is installed" FORCE)

CPMAddPackage(
	NAME glslang
	GITHUB_REPOSITORY KhronosGroup/glslang
	GIT_TAG vulkan-sdk-1.4.357.0
	SYSTEM YES
	EXCLUDE_FROM_ALL YES
)

# The test harness, declared before the modules whose tests link it.
gyro_add_module(Testing PORTABLE OBJECT
	SOURCES
		Main.cpp
		Test.cpp
)

# OBJECT for Testing's reason, one step less obvious. DebugAllocator.cpp defines the replaceable
# operator new/delete set and nothing references a symbol in it, so as a static archive member it
# would never be extracted and the check would be silently absent from every binary that links Core.
# An object library has no shelf to leave it on.
#
# The consequence to know about: CMake adds an object library's files only to the targets that name
# it directly, so reaching Core through another module's INTERFACE library drops them. That is why
# gyro_add_module names Core on every test executable rather than relying on the graph, and why the
# gyro target below already names it.
gyro_add_module(Core PORTABLE OBJECT
	SOURCES
		Clock.cpp
		DebugAllocator.cpp
		Fd.cpp
		Signal.cpp
	TESTS
		Clock.Test.cpp
		ColorState.Test.cpp
		Fd.Test.cpp
		FrameSection.Test.cpp
		Handle.Test.cpp
		Quiet.Test.cpp
		Result.Test.cpp
		Signal.Test.cpp
		SlotAllocator.Test.cpp
		Time.Test.cpp
		Wake.Test.cpp
)

# Header-only, so the tests are the only thing that compiles it. See Docs/Structure.md, both for why
# Geometry is not part of Core and for what that arrangement makes true.
gyro_add_module(Geometry PORTABLE
	DEPENDS
		Core
	TESTS
		AxisTransform.Test.cpp
		NodeTransform.Test.cpp
		Region.Test.cpp
		Scale.Test.cpp
		Space.Test.cpp
)

# The world's own vocabulary: what a node is. It exists because Scene writes the published node
# record and Frame walks it, and Frame may not depend on Scene — so the record belongs below both
# waists rather than in either half that names it. Decision 91, and decision 87 for the rule.
#
# DEPENDS Geometry alone, and the omission is the point rather than an oversight. The record names a
# transform and an extent and nothing of Core's, and CheckLayering denies what is not declared — so
# the narrower declaration is the stronger rule, exactly as Frame's below says. Core still reaches the
# test binary, because gyro_add_module names it on every one.
#
# No DISPATCH_HALF. This is declarations both sides read, like Seam: the world authors these records
# and the frame thread reconstitutes them, and neither half owns the type.
gyro_add_module(World PORTABLE
	DEPENDS
		Core
		Geometry
	TESTS
		Node.Test.cpp
		Content.Test.cpp
)

# Split by direction rather than by tier: Solve consumes spring coefficients and is what the frame
# thread may call, Author produces them and is dispatch-side.
#
# The Geometry edge is the catalog's and only the catalog's, and it is worth knowing which half of it
# drew the edge. Not the springs: a spring's parameters are scalar whatever its channel's value type
# is, so the bundle table names channels without naming their types. Not the settling thresholds
# either — those arrive as arguments precisely so that neither the geometric ones nor the unresolved
# non-geometric ones have to be known here. It is the gesture mapping: a travel distance is a
# displacement in global space, and typing it is what keeps a device-pixel distance from being
# written where an output-independent one belongs.
#
# FRAME_DEPENDS is what makes that a rule rather than a description. The edge arrived with authoring,
# so it stops at the publication boundary: Solve is scalar throughout and reaches Core only, and a
# geometric type appearing in the half the frame thread calls is a build failure rather than something
# a reader has to notice.
gyro_add_module(Animation PORTABLE
	DISPATCH_HALF
		Author
	DEPENDS
		Core
		Geometry
	FRAME_DEPENDS
		Core
	TESTS
		Solve/Ramp.Test.cpp
		Solve/Spring.Test.cpp
		Author/Animatable.Test.cpp
		Author/Bundle.Test.cpp
		Author/Catalog.Test.cpp
		Author/Drive.Test.cpp
		Author/Motion.Test.cpp
		Author/Retarget.Test.cpp
)

# The data waist: what crosses between the two threads — the snapshot representation and its wait-free
# reader, the newest-wins forward ring, the return channel carrying the consumed-sequence watermark
# and the per-buffer hold, and the deferred reclamation that hangs off it. Split by direction like
# Animation is: Reader is the frame half and is the default, Publisher is the dispatch half that
# serialises, owns the buffers, and allocates.
#
# FRAME_DEPENDS Core narrows the frame half to Core alone. The waist rests on Core and Geometry
# (Docs/Structure.md), but Geometry is authoring-side content — the coordinate spaces a position is
# expressed in before it is serialised — so it stops at the boundary the same way Animation's does. A
# geometric type reaching the reader would be a build failure with the reason attached rather than
# something a reader has to notice.
gyro_add_module(Publication PORTABLE
	DISPATCH_HALF
		Publisher
	DEPENDS
		Core
		Geometry
	FRAME_DEPENDS
		Core
	TESTS
		Return.Test.cpp
		Ring.Test.cpp
		Snapshot.Test.cpp
		Reader/Reader.Test.cpp
		Publisher/Outbox.Test.cpp
		Publisher/Publisher.Test.cpp
)

# The world, as the dispatch thread holds it: the entity store, and the serializer that turns it into
# a snapshot. Decision 111 for the shape — an entity is a node's authoring side and the store is one
# tree — and decision 86 for what comes out the other end.
#
# DISPATCH whole rather than a straddler, which is what makes the frame side's inability to reach it a
# graph property instead of a directory one. Nothing here runs on the frame thread: the walk that
# consumes what this publishes is Frame's, and the two never meet.
#
# DEPENDS is the row Docs/Structure.md already carried before there was a line of this module, which is
# the check that the placements it rests on are right. The store needs SlotAllocator and Handle; the
# entity holds Animatables; the serializer stages into Publication's builder and writes World's record;
# and the output model needs Scale and the adapter. Seam is not among them and must not become one —
# CheckLayering denies what is not declared, and its report would name the missing edge rather than
# decision 87.
gyro_add_module(Scene PORTABLE DISPATCH
	DEPENDS
		Core
		Geometry
		World
		Animation
		Publication
	TESTS
		Commit.Test.cpp
		Entity.Test.cpp
		Output.Test.cpp
		Serializer.Test.cpp
		Settle.Test.cpp
		Store.Test.cpp
)

# The scenes gyro authors for itself, with no protocol behind them: a handful of nodes moving under
# springs, as the instrument for looking at what the compositor actually draws.
#
# A module rather than files under `Compositor`, and the invariant is what earns it. A gym reaches
# nothing of the root's vocabulary — no ring, no SCHED_FIFO, no backend, no output negotiation — so
# `PORTABLE` holds it to a scene that is authored the way a shell would author one, and the build says
# so the first time something here wants a platform header. Left in the root it would be portable by
# accident, since the root is the one module that is not. The boot splash is the second scene with
# exactly this shape.
#
# `DISPATCH` whole for `Scene`'s reason, and it is the same edge being protected: a gym holds a
# `SceneStore` and opens `SceneCommit`s, so the frame side's inability to reach it has to be a graph
# property rather than a directory one.
#
# DEPENDS is what the code has. `Scene` for the store and the commit, `Animation` for the named motions
# a lane is driven under, `World` for the fills a solid carries and the dressings the materials gym
# adds, `Geometry` for the output rectangle a layout is a fraction of, and `Core` for the result, the
# clock and the wake. `Publication` is deliberately absent — a gym authors and never publishes, which
# is the composition root's half of the loop around it.
gyro_add_module(Gym PORTABLE DISPATCH
	DEPENDS
		Core
		Geometry
		World
		Animation
		Scene
	SOURCES
		Gym.cpp
		Lanes.cpp
	TESTS
		Gym.Test.cpp
		Lanes.Test.cpp
)

# The dispatch thread's iteration, with the wait left to the composition root — decision 80's shape,
# on the other side of the boundary and for the same reason.
#
# A module rather than files under `Compositor`, and CheckLayering is what forces it rather than taste:
# the outbox lives in `Publication/Publisher`, which is a dispatch half, so a file in the composition
# root cannot name one. The root is neither thread — it constructs both — and the check reads anything
# outside a dispatch half as the side being protected. Putting the step here makes the boundary a graph
# property, and PORTABLE keeps the producer side of the publication boundary runnable on a machine with
# no GPU, no seat, and no compositor.
#
# DEPENDS is what the code has. `Scene` for the store and the serializer, `Publication` for the ring,
# the return channel and the outbox that owns a published snapshot's bytes, `Gym` because the scenes
# gyro authors for itself are the only author there is until there is a protocol, and `Core` for the
# clock, the result and the wake.
gyro_add_module(Dispatch PORTABLE DISPATCH
	DEPENDS
		Core
		Publication
		Scene
		Gym
	TESTS
		Loop.Test.cpp
)

# The control waist: every interface with more than one implementation, and the plain data that
# crosses them. This is the presentation half — IPresenter and what its two verbs take and report.
# IRenderer, ISession and IInput join it where they are written.
#
# Portable, and it is worth saying why an interface to KMS can be. Nothing here names a Linux header:
# a format is a four-character code and a modifier is a number, both reproduced from stable kernel ABI
# rather than included, and a descriptor is Core/Fd.h's. That is what lets the frame loop's
# schedulability sweep run against a fake presenter on a machine with no GPU, which is the first point
# at which decision 29 becomes falsifiable rather than argued.
#
# No DISPATCH_HALF. Both threads touch these types and neither owns them — the frame thread calls both
# verbs, dispatch authors the configuration one of them takes — so there is no half to name and the
# module is deliberately the same on both sides of the boundary.
#
# The World edge is the dressing a draw item carries. A material is named by the shell and consumed by
# the renderer, so decision 87 puts it below both waists rather than here, and the seam reaches down
# for it — which is the direction that stays legal as the scene vocabulary grows.
gyro_add_module(Seam PORTABLE
	SOURCES
		Buffer.cpp
	DEPENDS
		Core
		Geometry
		World
	TESTS
		Buffer.Test.cpp
		Dressing.Test.cpp
		EventSource.Test.cpp
		Importer.Test.cpp
		OutputConfiguration.Test.cpp
		Pixel.Test.cpp
		PresentationInfo.Test.cpp
		Presenter.Test.cpp
		RenderTarget.Test.cpp
		Renderer.Test.cpp
		SyncPoint.Test.cpp
)

# The Wayland wire codec: framing, the argument vocabulary, fd passing over SCM_RIGHTS, and the
# per-connection object map. Docs/Decisions.md decision 119 for why it is a module rather than part of
# the nested backend, and Docs/Structure.md for the row.
#
# PORTABLE, and that is decision 119's correction to the obvious reading rather than an oversight.
# `sendmsg` with `SCM_RIGHTS` over `AF_UNIX` is POSIX — `<sys/socket.h>` and `<sys/un.h>` are not on
# CheckPortability.cmake's list and should not be — so the tier costs nothing and buys what decision 6
# is about: the codec is exercised over a socketpair by a peer the test wrote, on a machine with no
# GPU, no seat, and no compositor. `Nested` stays platform for its own reasons.
#
# The Seam edge is `IEventSource` and nothing else, and it is what decision 119 did not have to
# consider: a connection is drained by the frame loop alongside a DRM device and a simulated vblank,
# so it is a source like any other rather than something Frame/Loop.h learns to poll specially. It is
# declared here after Seam because target_link_libraries would otherwise take the name for a system
# library rather than a target; the graph itself has Wire below both waists, which is where
# Docs/Structure.md's table puts it.
#
# No DISPATCH_HALF. A connection is pumped by one thread for the whole of its life, which is decision
# 81's rule per object rather than per module — so the module has no thread affinity, the way Core and
# Geometry have none, and which thread a given connection belongs to is settled by whoever constructs
# it.
gyro_add_module(Wire PORTABLE
	DEPENDS
		Core
		Seam
	SOURCES
		Connection.cpp
		Writer.cpp
	TESTS
		Buffer.Test.cpp
		Codec.Test.cpp
		Connection.Test.cpp
		Live.Test.cpp
		Message.Test.cpp
)

# The typed client bindings, generated from the published XML into the build tree — decision 2's
# split, with the server codec left to libwayland and the client half gyro's because the nested
# backend needs it and there is no alternative that does not fork the renderer.
#
# The set is what a nested output actually speaks. `wayland` and `xdg-shell` are how a surface exists
# and gets a size; `linux-dmabuf-v1` is how the renderer's own allocation reaches the host without a
# copy, and `linux-drm-syncobj-v1` is how it says when that allocation is finished being drawn into;
# `presentation-time` is the host's vblank, which is the clock Frame/FrameClock.h predicts against
# when the panel belongs to somebody else; `viewporter` is scaling without a re-render; and
# `xdg-decoration` is how gyro says it draws its own, which it does.
gyro_add_bindings(WaylandProtocols
	PROTOCOLS
		wayland
		stable/xdg-shell/xdg-shell
		stable/presentation-time/presentation-time
		stable/linux-dmabuf/linux-dmabuf-v1
		staging/linux-drm-syncobj/linux-drm-syncobj-v1
		stable/viewporter/viewporter
		unstable/xdg-decoration/xdg-decoration-unstable-v1
)

# The CPU renderer, and PORTABLE is the load-bearing word here for a stronger reason than it is
# anywhere else.
#
# Decision 79 makes the pre-Vulkan console a renderer rather than a second presentation path, and
# decision 40 makes software rendering a device rather than a backend — so this is the floor beneath
# lavapipe, reached before any GPU driver has loaded on every boot and permanently on a machine where
# Vulkan will not initialize at all. A composite into a mapped pointer names no platform header, so
# CheckPortability.cmake can hold it there, and what that buys is that the renderer which runs when
# there is nothing else to run cannot acquire a platform dependency without the build saying so. Every
# assertion about where an edge landed runs on a machine with no GPU, which is the only kind of
# machine this code is for.
#
# It is not inside Console: the console is a text grid with its own thread, and this is an IRenderer
# on the frame thread that the boot splash reaches before any console exists. Console will depend on
# this rather than contain it. No DISPATCH_HALF — the whole module is frame-side.
gyro_add_module(Blit PORTABLE
	DEPENDS
		Core
		Geometry
		Seam
	SOURCES
		Band.cpp
		Blit.cpp
		Transfer.cpp
	TESTS
		Band.Test.cpp
		Blit.Test.cpp
		Transfer.Test.cpp
)

# The frame side: the per-output prediction every deadline in the system is derived from, the loop
# that steps it, admission control, the timing policy, and the walk that turns a published scene into
# a draw list.
#
# DEPENDS names what the code has rather than where the module is going. Declaring the eventual set
# early would spend the check that matters — CheckLayering denies what is not declared, so a narrower
# declaration is a stronger rule, and the edges arrive with the includes that need them. The World
# and Animation edges arrived with the evaluator, and both are the walk's: it reads the node record
# and the content records, and it evaluates the coefficients they name. Animation's frame half is
# what it reaches — Solve is the default and Author is a dispatch half, which CheckLayering denies
# from outside it without this file having to say so.
#
# Portable, and the clock is where that stops being an aspiration: it is fed by PresentationInfo and
# OutputConfiguration rather than by a drm_event, so decision 29's schedulability sweep can drive it
# at arbitrary mixed rates and phases on a machine with no GPU. No DISPATCH_HALF — the whole module
# is frame-side, which is the default and the reason the default is that way round.
gyro_add_module(Frame PORTABLE
	DEPENDS
		Core
		Geometry
		World
		Animation
		Publication
		Seam
	TESTS
		Admission.Test.cpp
		Budget.Test.cpp
		Evaluator.Test.cpp
		FrameClock.Test.cpp
		Loop.Test.cpp
		Projection.Test.cpp
		Timing.Test.cpp
)

# The instrument, and PORTABLE is the load-bearing word.
#
# Docs/Structure.md's table has the three backends as platform code, and this one turns out not to be:
# a simulated panel is arithmetic over Core/Time.h, its images are heap pages rather than dumb buffers,
# and Seam/EventSource.h already settles that a headless source has no descriptor at all — its flips
# are a function of the clock the test drives, so there is no file to poll and nothing to open. What
# the tier buys is that CheckPortability.cmake holds it there: the one place decision 29's
# schedulability claim is falsified cannot acquire a platform dependency without the build saying so,
# and it keeps running on a machine with no GPU, no seat, and no compositor.
#
# No DISPATCH_HALF yet. Scripted input is dispatch-side and arrives with the input seam; presentation
# is all that is here, and Docs/Structure.md's thread table already has that on the frame side.
gyro_add_module(Headless PORTABLE
	DEPENDS
		Core
		Geometry
		Seam
	TESTS
		Device.Test.cpp
		Output.Test.cpp
		Planes.Test.cpp
		Renderer.Test.cpp
		Vblank.Test.cpp
)

# The presenter that allocates. An output whose consumer is a file, an encoder, or a test rather than
# a panel, which is what gives the renderer a real dmabuf to bind before there is a DRM backend to
# hand it one. Docs/Decisions.md decision 102.
#
# Not PORTABLE, and the reason is one include: `linux/udmabuf.h`. Everything above it in this list
# either has no platform in it or is the composition root; this is the first backend that genuinely
# reaches for the kernel, which is the line CheckPortability.cmake exists to draw.
#
# The Headless edge is `VblankTimeline` and nothing else. A recording's cadence is a period and a
# phase exactly as a panel's is, and a platform module depending down onto a portable one is the
# direction the graph allows — see Docs/Structure.md#virtual-is-platform-and-it-is-the-one-that-
# allocates for what would change the answer.
#
# No DISPATCH_HALF. Presentation is frame-side, and the allocation is a phase rather than a thread:
# it runs at a target invalidation, which is where `IRenderer::BindTargets` runs, and nothing here is
# authored on dispatch.
gyro_add_module(Virtual
	DEPENDS
		Core
		Geometry
		Seam
		Headless
	SOURCES
		Buffer.cpp
		Dump.cpp
		Heap.cpp
		Pam.cpp
		Pixels.cpp
		Sink.cpp
		Udmabuf.cpp
	TESTS
		Buffer.Test.cpp
		Device.Test.cpp
		Dump.Test.cpp
		Output.Test.cpp
		Pam.Test.cpp
		Pixels.Test.cpp
		Sink.Test.cpp
		Udmabuf.Test.cpp
)

# Virtual/Dump.h runs a writer thread, which is the one thing in this module the C library does not
# supply on its own. PUBLIC so that both the module's own tests and the composition root inherit it.
target_link_libraries(Virtual
	PUBLIC
		Threads::Threads
)

# The Vulkan renderer. Platform code by definition — Docs/Structure.md's table has had this row since
# before the module existed, and volk, dmabufs and a device that can be lost are what it means.
#
# DEPENDS names what the code has rather than where the module is going, which is Frame's rule above.
# The eventual set adds `Publication` for the import half and `World` for the dressings; neither
# arrives until something includes them, because CheckLayering denies what is not declared and a
# narrower declaration is a stronger rule.
#
# No DISPATCH_HALF yet, and it is not an omission. Docs/Structure.md#threads-are-a-second-partition
# splits this module `Record` / `Import`, and `Import` is a directory that does not exist: a
# `wl_buffer` becomes a device image on the dispatch thread, and there is no protocol layer to hand
# one over. `gyro_add_module` refuses a half that names no directory, which is the right moment for
# the declaration to appear.
gyro_add_module(Render
	DEPENDS
		Core
		Geometry
		Seam
	SOURCES
		Allocator.cpp
		Backdrop.cpp
		Device.cpp
		Pipeline.cpp
		Renderer.cpp
		Unfused.cpp
	TESTS
		Backdrop.Test.cpp
		Device.Test.cpp
		Pipeline.Test.cpp
		Unfused.Test.cpp
)

gyro_add_shaders(Render
	SOURCES
		Quad.vert
		Quad.frag
		Backdrop.vert
		Extract.frag
		Blur.frag
		Dress.vert
		Dress.frag
		Element.vert
		Element.frag
		Shadow.vert
		Shadow.frag
)

# PUBLIC for Compositor's reason below: the module's own tests compile against volk's prototypes too,
# and Render's headers name Vulkan handles rather than hiding them behind an opaque pointer.
target_link_libraries(Render
	PUBLIC
		volk::volk
		spdlog::spdlog
)

# The nested backend: gyro as a client of another compositor, one host window per output.
#
# Docs/Architecture.md#nested-wayland calls it the daily driver, and the reason is not convenience. A
# compositor holding DRM master is somewhere between awkward and impossible to point RenderDoc, the
# Vulkan validation layers, ASan or gdb at; nested is where all four work. It is also where the
# deadline scheduler runs unmodified against a real cadence with real jitter, because the clock is
# somebody else's vblank arriving as `wp_presentation_feedback`.
#
# DEPENDS is the whole of what a presenter needs — a clock and a result from Core, a size and a damage
# region from Geometry, the presentation seam, and the client codec — and deliberately not `Render`.
# Decision 120 is why: the targets are exported from the Vulkan device, and the interface that says so
# is `Seam`'s, so the composition root is the only thing that knows both sides. The generated bindings
# are not a module and carry no edge; they are `Wire` plus text the build wrote, and the target below
# is what links them.
#
# No DISPATCH_HALF yet, and it is not an omission. Docs/Structure.md splits this module by thread and
# only the frame half exists: input is dispatch-side, decision 81 has the connection pumped by the
# frame thread and the input handed across, and there is no dispatch thread on the far end of that
# handoff. `gyro_add_module` refuses a half that names no directory, which is the right moment for the
# declaration to appear.
gyro_add_module(Nested
	DEPENDS
		Core
		Geometry
		Seam
		Wire
	SOURCES
		Feedback.cpp
		Host.cpp
		Output.cpp
		Peer.cpp
		Sync.cpp
	TESTS
		Feedback.Test.cpp
		Host.Test.cpp
		Output.Test.cpp
		Sync.Test.cpp
)

# The generated client bindings, and spdlog for the handful of lines that say which path a session
# came up on — which format and modifier the host and the device agreed on, and whether explicit sync
# is live. PUBLIC because the module's headers name `Wayland::` types: a presenter holds proxies.
target_link_libraries(Nested
	PUBLIC
		WaylandProtocols
		spdlog::spdlog
)

# The composition root, and the only module that knows both sides of either waist. Docs/Structure.md
# #orchestration is its whole brief: it constructs the clock, the backend, the rendering device, the
# publication channel, and the threads, and hands each subsystem what it needs.
#
# Not PORTABLE, and it is the first module in the tree that is not. That is the tier working rather
# than eroding — io_uring, SCHED_FIFO, mlockall, and RLIMIT_RTTIME all live here, and they live here
# *because* Frame and Headless may not say any of those words. Decision 80 is what put them on this
# side of the line: the loop is a step and the wait is the root's, so the platform collects at the
# top rather than seeping into the module the schedulability sweep runs.
#
# It depends on everything below it and always will, which is why it is the one module whose DEPENDS
# list is not an argument about layering. No DISPATCH_HALF: the root is what *constructs* both threads
# and runs on neither, so it belongs to the frame side by the default the halves are written around.
gyro_add_module(Compositor
	DEPENDS
		Core
		Geometry
		Publication
		Scene
		Seam
		Frame
		Headless
		Blit
		Virtual
		Render
		Nested
		Gym
		Dispatch
	SOURCES
		Compositor.cpp
		RealTime.cpp
		Uring.cpp
		Wait.cpp
	TESTS
		Options.Test.cpp
		Schedule.Test.cpp
		Uring.Test.cpp
)

# PUBLIC so that both the gyro binary and this module's own tests inherit them. gyro_add_module names
# only the module graph, which is the right thing for it to know: an external dependency is a property
# of the module rather than an edge in Docs/Structure.md's graph, and CheckLayering has nothing to say
# about either of these.
target_link_libraries(Compositor
	PUBLIC
		PkgConfig::liburing
		spdlog::spdlog
		Threads::Threads
)

# Declared after the modules, because the module list is what they check. ALL for the same reason as
# the clock check above.
#
# A semicolon inside a custom command's argument is split into separate arguments before the script
# sees it, so both checks are handed a separator of their own and split it themselves. VERBATIM is
# what keeps the graph's `|` from reaching the shell as a pipe.
get_property(GYRO_PORTABLE_MODULES GLOBAL PROPERTY GYRO_PORTABLE_MODULES)
list(JOIN GYRO_PORTABLE_MODULES "," GYRO_PORTABLE_MODULES)
add_custom_target(Portability ALL
	COMMAND ${CMAKE_COMMAND}
		-D SOURCE_DIR=${GYRO_SOURCE_DIR}
		-D MODULES=${GYRO_PORTABLE_MODULES}
		-P ${CMAKE_CURRENT_SOURCE_DIR}/CMake/CheckPortability.cmake
	VERBATIM
)

# The module graph, flattened to "Name=Dep,Dep|Name=Dep,Dep" because a -P script takes no maps.
get_property(GYRO_MODULES GLOBAL PROPERTY GYRO_MODULES)
set(GYRO_MODULE_GRAPH "")
foreach(MODULE IN LISTS GYRO_MODULES)
	get_property(MODULE_DEPENDS GLOBAL PROPERTY GYRO_MODULE_DEPENDS_${MODULE})
	list(JOIN MODULE_DEPENDS "," MODULE_DEPENDS)
	list(APPEND GYRO_MODULE_GRAPH "${MODULE}=${MODULE_DEPENDS}")
endforeach()
list(JOIN GYRO_MODULE_GRAPH "|" GYRO_MODULE_GRAPH)

# The second partition, which the graph cannot express: the halves of a straddling module, and the
# modules that are dispatch-side entire. Both are empty until a module says otherwise.
get_property(GYRO_DISPATCH_MODULES GLOBAL PROPERTY GYRO_DISPATCH_MODULES)
get_property(GYRO_DISPATCH_HALVES GLOBAL PROPERTY GYRO_DISPATCH_HALVES)
list(JOIN GYRO_DISPATCH_MODULES "," GYRO_DISPATCH_MODULES)
list(JOIN GYRO_DISPATCH_HALVES "," GYRO_DISPATCH_HALVES)

# The same partition applied to the edges rather than to the includes: what a straddling module's
# frame half may reach, for the modules that narrow it. Same flattening as the graph above, because
# it is read the same way and against the same closure.
get_property(GYRO_FRAME_LIMITED_MODULES GLOBAL PROPERTY GYRO_FRAME_LIMITED_MODULES)
set(GYRO_FRAME_GRAPH "")
foreach(MODULE IN LISTS GYRO_FRAME_LIMITED_MODULES)
	get_property(FRAME_DEPENDS GLOBAL PROPERTY GYRO_MODULE_FRAME_DEPENDS_${MODULE})
	list(JOIN FRAME_DEPENDS "," FRAME_DEPENDS)
	list(APPEND GYRO_FRAME_GRAPH "${MODULE}=${FRAME_DEPENDS}")
endforeach()
list(JOIN GYRO_FRAME_GRAPH "|" GYRO_FRAME_GRAPH)

add_custom_target(Layering ALL
	COMMAND ${CMAKE_COMMAND}
		-D SOURCE_DIR=${GYRO_SOURCE_DIR}
		-D GRAPH=${GYRO_MODULE_GRAPH}
		-D FRAME_GRAPH=${GYRO_FRAME_GRAPH}
		-D DISPATCH_MODULES=${GYRO_DISPATCH_MODULES}
		-D DISPATCH_HALVES=${GYRO_DISPATCH_HALVES}
		-P ${CMAKE_CURRENT_SOURCE_DIR}/CMake/CheckLayering.cmake
	VERBATIM
)

add_executable(gyro
	Source/Main.cpp
)
add_dependencies(gyro AppVersion)
target_include_directories(gyro
	PRIVATE
		${CMAKE_CURRENT_BINARY_DIR}/Generated
)
# Core is named alongside Compositor for gyro_add_module's reason: it is an OBJECT library, so
# DebugAllocator.cpp's replacement of the global allocator reaches only the targets that name it
# directly. Reached through the graph it would be dropped, and decision 36's check would be silently
# absent from the one binary that has a real frame section in it.
target_link_libraries(gyro
	PRIVATE
		Compositor
		Core
)

# Standalone diagnostics. Not linked into gyro, and deliberately dependency-free — BuildFlags
# included — so they can be built and run on a target machine on their own.
add_executable(UringProbe
	Tools/UringProbe.cpp
)

# The same brief as UringProbe and the same shape: it answers a question about the machine before
# gyro runs on it, and it answers the questions Docs/Decisions.md decision 40 rested on for a year
# without being run. It links the *headers* and nothing else — the loader is `dlopen`ed exactly as
# volk does it, so this builds and runs on a machine with no Vulkan at all and says so.
add_executable(VulkanProbe
	Tools/VulkanProbe.cpp
)
target_link_libraries(VulkanProbe
	PRIVATE
		Vulkan::Headers
		${CMAKE_DL_LIBS}
)

# The tests that stand on both sides of the publication waist: they publish real spring coefficients
# through Publication and reconstitute them through Animation/Solve, which no single module may do —
# Publication does not depend on Animation, by the graph. So this is a bespoke executable rather than
# a gyro_add_module, outside the per-module layering the crossing is the whole point of, and it is a
# temporary home for the round trip until Scene and Frame own their ends of it.
#
# It runs two threads, which is what makes it the target to point GYRO_SANITIZE=thread at: the
# crossing is the thing whose memory ordering has to be proved, and the soak that proves it has
# nowhere else to live.
#
# Core and Testing are named directly for the reason gyro_add_module names them: they are OBJECT
# libraries, and an object library's members reach only the targets that name it.

add_executable(IntegrationTests
	Source/Integration/BlitPresent.Test.cpp
	Source/Integration/PublicationRoundTrip.Test.cpp
	Source/Integration/PublicationSoak.Test.cpp
	Source/Integration/RenderImport.Test.cpp
	Source/Integration/SceneCommit.Test.cpp
	Source/Integration/SceneIdle.Test.cpp
	Source/Integration/ScenePublication.Test.cpp
	Source/Integration/SceneRoundTrip.Test.cpp
	Source/Integration/Schedulability.Test.cpp
	Source/Integration/VirtualComposite.Test.cpp
	Source/Integration/WaylandBindings.Test.cpp
)
target_link_libraries(IntegrationTests
	PRIVATE
		Frame
		Headless
		Scene
		Publication
		Animation
		Render
		Blit
		Virtual
		WaylandProtocols
		Wire
		World
		Seam
		Geometry
		Core
		Testing
		Threads::Threads
)
add_test(NAME Integration COMMAND IntegrationTests)
