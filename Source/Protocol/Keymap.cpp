#include "Protocol/Keymap.h"

#include <xkbcommon/xkbcommon.h>

#include <cerrno>
#include <cstring>
#include <span>
#include <string>

#include "Protocol/Sealed.h"

namespace
{
// XKB numbers a key eight above the kernel's code, because the X11 protocol reserved the first eight
// for nothing. It is the one place in gyro that has to know, and it is here rather than at ingest so
// that `Core/Input.h`'s keycode stays the kernel's the whole way through — including into the escape
// chord, which must not be reading a number some other layer adjusted.
constexpr std::uint32_t XkbKeycodeOffset = 8;

} // namespace

Keymap::~Keymap()
{
	if (m_State != nullptr)
	{
		::xkb_state_unref(m_State);
	}

	if (m_Keymap != nullptr)
	{
		::xkb_keymap_unref(m_Keymap);
	}

	if (m_Context != nullptr)
	{
		::xkb_context_unref(m_Context);
	}
}

Result<void> Keymap::Open()
{
	if (IsOpen())
	{
		return Failure(EALREADY, "the keymap is already compiled");
	}

	m_Context = ::xkb_context_new(XKB_CONTEXT_NO_FLAGS);

	if (m_Context == nullptr)
	{
		return Failure(ENOMEM, "creating an xkb context");
	}

	// No names, which is xkbcommon's own spelling of *whatever the environment says, and `us` if it says
	// nothing*. The header carries why that is the right stand-in until there is a session to ask.
	m_Keymap = ::xkb_keymap_new_from_names(m_Context, nullptr, XKB_KEYMAP_COMPILE_NO_FLAGS);

	if (m_Keymap == nullptr)
	{
		return Failure(EINVAL, "compiling the keymap; is xkeyboard-config installed?");
	}

	m_State = ::xkb_state_new(m_Keymap);

	if (m_State == nullptr)
	{
		return Failure(ENOMEM, "creating the keymap state");
	}

	char* const text = ::xkb_keymap_get_as_string(m_Keymap, XKB_KEYMAP_FORMAT_TEXT_V1);

	if (text == nullptr)
	{
		return Failure(ENOMEM, "serialising the keymap");
	}

	const std::string owned{ text };

	::free(text);

	// The terminating byte is part of what the client maps: `wl_keyboard.keymap`'s size includes it and
	// xkbcommon parses the mapping as a C string.
	Result<Fd> sealed = SealedMemfd("gyro-keymap", std::as_bytes(std::span{ owned.c_str(), owned.size() + 1 }));

	if (!sealed)
	{
		return std::unexpected{ sealed.error() };
	}

	m_Fd = std::move(*sealed);
	m_Size = static_cast<std::uint32_t>(owned.size() + 1);

	return {};
}

Keymap::Modifiers Keymap::Update(std::uint32_t code, bool pressed) noexcept
{
	if (m_State != nullptr)
	{
		::xkb_state_update_key(m_State, code + XkbKeycodeOffset, pressed ? XKB_KEY_DOWN : XKB_KEY_UP);
	}

	return Current();
}

Keymap::Modifiers Keymap::Current() const noexcept
{
	if (m_State == nullptr)
	{
		return {};
	}

	return Modifiers{
		.Depressed = ::xkb_state_serialize_mods(m_State, XKB_STATE_MODS_DEPRESSED),
		.Latched = ::xkb_state_serialize_mods(m_State, XKB_STATE_MODS_LATCHED),
		.Locked = ::xkb_state_serialize_mods(m_State, XKB_STATE_MODS_LOCKED),
		.Group = ::xkb_state_serialize_layout(m_State, XKB_STATE_LAYOUT_EFFECTIVE),
	};
}
