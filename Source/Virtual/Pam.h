#pragma once

#include <cstdint>
#include <string_view>

#include "Core/Result.h"
#include "Virtual/Pixels.h"

// A frame on disk, for a person to look at.
//
// **Nothing in the tree ever reads one back, and that is the design.** Virtual/Pixels.h carries the
// argument against golden images in full — a reference frame asserts every pixel, including the ones
// nobody meant to promise, so it fails on a driver that rounds an edge differently. What is left for
// a file to do is the thing a predicate is bad at: showing a human *what* went wrong when
// `BoundsOfDiffering` came back forty pixels to the left of where it should have been. So a dump is
// a diagnostic, it is written on failure and on request, and no test ever compares against one.
//
// **PAM rather than PNG, and the trade is deliberate.** PAM is netpbm's one format that carries an
// alpha channel and more than eight bits per sample, which are exactly the two things a composited
// frame has and PPM does not. Writing it costs a header and the rows; PNG costs either a dependency
// or a deflate implementation, in return for compression that a diagnostic nobody archives does not
// need. Every image viewer and every ImageMagick invocation on a Linux box opens a `.pam`. The one
// place this would flip is a browser, which opens neither — and if a dump ever needs to land in a CI
// artefact somebody clicks on, a stored-block PNG encoder is about eighty lines and goes beside this
// file rather than replacing it.
//
// **This is not frame-thread code and must never be called from there.** It opens, writes, renames,
// and allocates a row buffer. Docs/Structure.md puts the frame thread under `SCHED_FIFO` and
// Docs/Open.md's *spdlog async sink* entry is the same hazard stated for logging: a write from the
// frame thread punts to io-wq and surfaces as jitter. Virtual/Sink.h is written so that this is
// structurally hard — a sink copies pixels and a caller writes files, on its own time.

// One image, written to `path`.
//
// **Written through a temporary and renamed, because a dump directory is something people watch.**
// A viewer set to reload, or an `inotify` loop turning frames into a video, will open a file the
// moment it appears; a frame written in place is visible half-drawn for as long as the write takes,
// which reads as a corrupt compositor rather than as a partial file. The rename is atomic within a
// filesystem, so a reader sees the whole frame or no file at all.
//
// The sample depth follows the source: `MAXVAL 255` for an eight-bit format and `MAXVAL 65535`,
// big-endian per the netpbm specification, for a ten-bit one. Widening everything to sixteen bits
// would double a dump for no information, and narrowing everything to eight would throw away exactly
// the precision a ten-bit target was chosen for.
[[nodiscard]] Result<void> WritePam(const ImageView& image, std::string_view path);

// The same, into a directory, named by the frame's sequence.
//
// The directory is created if it is not there, which is the one convenience worth having: a dump
// path is typed on a command line or pulled from an environment variable, and failing because
// nobody ran `mkdir` first is a diagnostic that failed to diagnose. Anything past one level is the
// caller's to create, since a missing parent is a typo rather than an omission.
//
// Names are `frame-00000042.pam`, zero-padded so that a lexical sort is a temporal one — which is
// what `ffmpeg -i frame-%08d.pam` and every file manager assume.
[[nodiscard]] Result<void> DumpFrame(const ImageView& image, std::string_view directory, std::uint64_t sequence);

// Where a test should put a dump, or nothing where the user has not asked for one.
//
// Reads `GYRO_FRAME_DUMP`. It is an environment variable rather than a build option because the
// question it answers is *show me this run*, which is asked after a test has already failed — a
// rebuild to see a picture is a rebuild that loses the state that produced it.
[[nodiscard]] std::string_view FrameDumpDirectory() noexcept;
