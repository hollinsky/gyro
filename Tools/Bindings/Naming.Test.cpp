#include "Naming.h"

#include <string>
#include <string_view>

#include "Testing/Test.h"

// The spelling rules, and every case here is one a published protocol actually contains. There are
// no invented names: a rule this file made up would be a rule nothing holds it to.

GYRO_TEST(Naming, InterfaceKeepsItsPrefix)
{
	GYRO_CHECK_EQ(Pascal("wl_surface"), std::string{ "WlSurface" });
	GYRO_CHECK_EQ(Pascal("xdg_toplevel"), std::string{ "XdgToplevel" });

	// The two shapes that would break a naive split: a trailing version segment, and a leading `z`
	// that is part of the prefix rather than a word.
	GYRO_CHECK_EQ(Pascal("zwp_linux_dmabuf_v1"), std::string{ "ZwpLinuxDmabufV1" });
	GYRO_CHECK_EQ(Pascal("zxdg_toplevel_decoration_v1"), std::string{ "ZxdgToplevelDecorationV1" });
	GYRO_CHECK_EQ(Pascal("wp_presentation_feedback"), std::string{ "WpPresentationFeedback" });
}

GYRO_TEST(Naming, DigitsAreNotAcronyms)
{
	// `wl_shm.format` is DRM fourccs spelled out. Uppercasing the run would give `ARGB8888`, which
	// requires a table of acronyms that has to be maintained against protocols nobody has written
	// yet — so the first character and nothing else.
	GYRO_CHECK_EQ(Pascal("argb8888"), std::string{ "Argb8888" });
	GYRO_CHECK_EQ(Pascal("xrgb2101010"), std::string{ "Xrgb2101010" });
}

GYRO_TEST(Naming, LeadingDigitTakesAnUnderscore)
{
	// `wl_output.transform` really does have entries named `90`, `180` and `270`. `_90` is a legal
	// enumerator of a scoped enum — the leading-underscore reservation covers the global namespace,
	// which an enumerator is not in — and inventing `Rotate90` would be a name no reader can find in
	// the XML.
	GYRO_CHECK_EQ(Pascal("90"), std::string{ "_90" });
	GYRO_CHECK_EQ(Pascal("flipped_90"), std::string{ "Flipped90" });
	GYRO_CHECK_EQ(Pascal("normal"), std::string{ "Normal" });
}

GYRO_TEST(Naming, AFileNameIsAUnitName)
{
	// The generated header is named after the file the protocol was read from, so that the build knows
	// an output path from an input path. A hyphen has to separate for that to work.
	GYRO_CHECK_EQ(Pascal("xdg-shell"), std::string{ "XdgShell" });
	GYRO_CHECK_EQ(Pascal("linux-dmabuf-v1"), std::string{ "LinuxDmabufV1" });
	GYRO_CHECK_EQ(Pascal("xdg-decoration-unstable-v1"), std::string{ "XdgDecorationUnstableV1" });
	GYRO_CHECK_EQ(Pascal("wayland"), std::string{ "Wayland" });
}

GYRO_TEST(Naming, ParametersAreLowerCamel)
{
	GYRO_CHECK_EQ(Parameter("plane_idx"), std::string{ "planeIdx" });
	GYRO_CHECK_EQ(Parameter("modifier_hi"), std::string{ "modifierHi" });
	GYRO_CHECK_EQ(Parameter("surface"), std::string{ "surface" });
}

GYRO_TEST(Naming, KeywordGuardIsIdempotent)
{
	// `wl_shell_surface.set_class` already spells its argument `class_`, which is this rule arrived at
	// independently by the protocol's author. Running over it again has to leave it alone, or the
	// generated parameter drifts from the one the protocol documents.
	GYRO_CHECK_EQ(Parameter("class_"), std::string{ "class_" });
	GYRO_CHECK_EQ(Parameter("class"), std::string{ "class_" });
	GYRO_CHECK_EQ(Parameter("default"), std::string{ "default_" });
	GYRO_CHECK_EQ(Parameter("template"), std::string{ "template_" });
}

GYRO_TEST(Naming, ReservedMembersAreTheOnesEveryProxyDeclares)
{
	// The set exists so that a protocol naming a request `version` fails the build at the generator
	// rather than at a redefinition error in a file nobody wrote.
	GYRO_CHECK(IsReservedProxyMember("Version"));
	GYRO_CHECK(IsReservedProxyMember("Listener"));
	GYRO_CHECK(IsReservedProxyMember("Dispatch"));
	GYRO_CHECK(IsReservedProxyMember("Id"));

	GYRO_CHECK(!IsReservedProxyMember("Attach"));
	GYRO_CHECK(!IsReservedProxyMember("Commit"));
}

GYRO_TEST(Naming, TheTwoDirectionsReserveDifferentNames)
{
	// The two lists are not one union, and this is what says so: a resource has no `Listen` and a
	// proxy has no `Create`, so a protocol with an event named `listen` is emittable on the server
	// arm and a request named `create` is emittable on the client one. Refusing either against the
	// other list would be the generator declining a protocol over a class it never puts it in.
	GYRO_CHECK(IsReservedResourceMember("Create"));
	GYRO_CHECK(IsReservedResourceMember("PostError"));
	GYRO_CHECK(IsReservedResourceMember("WireResource"));

	GYRO_CHECK(!IsReservedResourceMember("Listen"));
	GYRO_CHECK(!IsReservedProxyMember("Create"));

	// The four that are the same question in both classes.
	for (const std::string_view shared : { "Version", "IsValid", "WireName", "WireVersion" })
	{
		GYRO_CHECK(IsReservedProxyMember(shared));
		GYRO_CHECK(IsReservedResourceMember(shared));
	}
}
