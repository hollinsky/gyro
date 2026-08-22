#pragma once

#include "Compositor/Options.h"
#include "Core/Result.h"

// The composition root, and the only thing in gyro that knows both sides of either waist.
//
// Docs/Structure.md#orchestration is the whole of this file's brief: one place constructs the clock,
// the backend, the rendering device, the publication channel, and the threads, and hands each
// subsystem what it needs. Nothing else names an implementation. What the header exposes is therefore
// one verb — everything else would be a reachable dependency, which is the singleton this design took
// out.
//
// It owns the `while` as well, per Docs/Decisions.md decision 80: `Frame` exposes one iteration and
// returns the `Wake` the next one is owed at, and the thread, the ring, and the loop above it are all
// the root's because the root is what constructs threads.

[[nodiscard]] Result<void> Run(const Options& options);
