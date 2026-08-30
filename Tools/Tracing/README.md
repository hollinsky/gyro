# Tracing

What to do when a frame missed and gyro's own trace says it should not have.

`Miss.cfg` is a Perfetto config for the kernel side. Everything here exists because gyro's trace has a
blind spot it cannot close from inside: the ring records the commit going out and the flip event
coming back, and a refresh lost between those two looks exactly like a frame that was issued late.
`Frame/Loop.h` marks the symptom as `landed late`; this is how you find the cause.

## Capture

Perfetto's `tracebox` is one static binary and carries `traced`, `traced_probes` and the CLI, so there
is no daemon to set up. It is deliberately not in the tree — see the `.gitignore` entry.

```
curl -LO https://get.perfetto.dev/tracebox && chmod +x tracebox
```

Nothing else may hold DRM master, or gyro exits with `EACCES` before it draws anything and the capture
records a machine with no compositor.

```
pkill -x gyro; sleep 2
sudo ./tracebox -c Tools/Tracing/Miss.cfg --txt -o system.pftrace &
sleep 3; ./build/gyro --trace=gyro.pftrace --trace-on-miss --trace-buffer=16M &
sleep 2; <the client that provokes it>
```

`--trace-on-miss` is the point. `landed late` calls `TraceTrigger`, so gyro writes the ring at the
instant of the miss rather than at exit, and you are not racing `SIGUSR1` against a ring that laps
about as often as the defect recurs. It lands as `gyro-1.pftrace` — the suffix is the snapshot index,
and *that* is the file with the miss in it, not the one named on the command line.

## Merge

A `.pftrace` is a concatenation of self-delimiting packets, and gyro emits a `ClockSnapshot` pairing
its monotonic timebase with `CLOCK_BOOTTIME` while declaring no `primary_trace_clock`. So the files
compose, in either order, with no tool in between:

```
sudo chown $USER system.pftrace
cat system.pftrace gyro-1.pftrace > merged.pftrace
```

Open `merged.pftrace` at `ui.perfetto.dev`, or query it — `.venv/bin/python -m
perfetto.trace_processor merged.pftrace` gives SQL over the same file. Note that `sched_switch`
arrives as `thread_state` and `sched_slice` rather than in `slice`, which is where the answer usually
is and is not where you will look first.

## Reading it

Find gyro's `landed late` mark and look at the same instant on the kernel rows. The four groups in
`Miss.cfg` are the four answers, and they are mutually exclusive:

- **A `dma_fence_wait` spanning the missed vblank.** The display engine was still waiting on pixels.
  For a promoted layer that means the client's buffer, since `Assign.h`'s `Promoted` hands the layer
  `SyncPoint::Immediate()` and gyro has no client fence to forward.
- **`intel_pipe_update_start` at frame N ending at N+1.** The driver could not land the register write
  in the frame it was aimed at. These sit behind `CONFIG_DRM_I915_LOW_LEVEL_TRACEPOINTS` on some
  kernels; check `available_events` before concluding their absence means anything.
- **The commit thread off-CPU across the window.** gyro's own problem, not the driver's.
- **A commit thread that sleeps twice** — woken at the target vblank, then again for a full refresh.
  That is a `wait_for_next_vblank` inside the commit tail, which is the driver spending a frame on
  purpose. It is not a flip armed late: a flip armed late never wakes at the first vblank at all.

The control that makes any of these an argument rather than a coincidence is the frame *before* the
miss. Every landing frame has a shape — for a blocking commit, sleep in `D` and wake on flip-done
about 14 µs past its vblank — and the useful question is always which part of that shape the missed
frame did differently.

## Cost

`sched_switch` on a busy machine is a large fraction of the file and tracing perturbs a `SCHED_FIFO`
frame thread. If the defect stops reproducing under capture, drop the `sched/*` events first: they
answer one of the four questions above and the other three do not need them. Check what else was
running, too — a concurrent test run with software rendering across every core is enough to change
what you are measuring, and it will not announce itself.
