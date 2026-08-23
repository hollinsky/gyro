#pragma once

#include <string>
#include <string_view>

// Wire spellings turned into C++ ones, and the only place in the generator that happens.
//
// Tools/Bindings/Protocol.h keeps the parser a transcription — `wl_surface`, `set_window_geometry`,
// `flipped_90` arrive exactly as the protocol author wrote them — precisely so that the opinion about
// what C++ should look like lives on this side of the seam. This file is that opinion, and it is
// small enough to state in full:
//
//   **An interface keeps its prefix.** `wl_output` is `WlOutput` and `zxdg_output_v1` is
//   `ZxdgOutputV1`, rather than the prefix being stripped for looks. Two reasons, and the second is
//   the one that decides it: the prefix is what makes the C++ name greppable back to the XML the
//   binding came from, and stripping it collides — `wl_output` and `zxdg_output_v1` would both be
//   `Output`, in one namespace, from two files nobody is looking at together.
//
//   **A segment's first character is uppercased and the rest is left alone.** `argb8888` becomes
//   `Argb8888` rather than `ARGB8888`, because there is no table that says which segments are
//   acronyms and inventing one means a name that is right for `rgb` and wrong for the next protocol
//   to land.
//
//   **A name that would start with a digit gets a leading underscore.** `wl_output.transform` really
//   does have an entry named `90`, and `_90` is the only mechanical answer — an invented `Rotate90`
//   reads better and is a name this file made up, which is the thing the transcription rule exists to
//   prevent. The underscore is safe because these are enumerators of a scoped enum rather than names
//   in the global namespace, which is the only scope the leading-underscore reservation covers.

// `wl_surface` to `WlSurface`, `flipped_90` to `Flipped90`, `90` to `_90`. A hyphen separates like an
// underscore, so `xdg-shell` is `XdgShell` and a file name is a unit name.
[[nodiscard]] std::string Pascal(std::string_view wire);

// A parameter or local: `window_geometry` to `windowGeometry`. Keywords take a trailing underscore,
// so a protocol that names an argument `class` produces `class_` rather than a build failure two
// thousand generated lines away from the XML that caused it.
[[nodiscard]] std::string Parameter(std::string_view wire);

// Whether a generated member name would land on one every proxy already declares. A protocol is free
// to name a request `version`, and the generator has to refuse rather than emit a class with two
// members of that name and a compiler error naming neither the protocol nor the request.
[[nodiscard]] bool IsReservedMember(std::string_view identifier) noexcept;
