#pragma once

#include <chrono>
#include <cstdint>
#include <utility>
#include <vector>

#include "Animation/Author/Bundle.h"
#include "Core/ColorState.h"
#include "Core/Pam.h"
#include "Core/Result.h"
#include "Core/Texture.h"
#include "Core/Time.h"
#include "Core/Wake.h"
#include "Geometry/Space.h"
#include "Scene/Commit.h"
#include "Scene/Output.h"
#include "Scene/Store.h"
#include "Scene/Textures.h"
#include "World/Content.h"
#include "World/Node.h"

// The system background: what is behind the greeter, behind the lock screen, behind a person's
// windows, and what the boot animation fades up onto.
//
// **It is gyro's rather than a session's, which is the whole reason it is a thing at all.** A
// background drawn by a shell is a background that is absent before anybody logs in, absent between
// one session and the next, and absent on the recovery console — so the three moments a machine looks
// most like it has crashed are the three it has nothing behind it. `SessionId::None` is the root kind
// that already means *gyro's own, shown on every output*, and this is its second member after the
// pointer glyph.
//
// **The container is authored before any author opens, and that is what makes it backmost.** A root
// created later lands in front of the roots already there (decision 55), and a background that
// arrives from a shell hours into a session must not appear over the windows it is behind. So the
// container is created once at `Open`, empty, before `ISceneAuthor::Open` builds anything — the exact
// inverse of `Scene/Cursor.h`, which is the *last* root and re-raises itself every iteration to stay
// there. Images are children of it, so a background arriving at any later moment changes nothing
// about where the world is stacked.
//
// **An image is shown only where it fits the panel exactly, and nowhere else.** There is no fit
// policy here: no cover-crop, no stretch, no letterbox. A background is *scaled by whoever produced
// it* — the shell knows the panels because gyro tells it, and it hands over an image per size — and
// an output with nothing that fits draws black, which is what it drew before anybody asked for a
// background at all. The alternative is a resampler in the compositor and a wallpaper that is subtly
// soft on the one panel a person spends the day in front of, in exchange for never showing black on a
// monitor that was plugged in ten seconds ago.
//
// **The first background waits before it fades in, and a replacement does not.** A machine coming up
// has the firmware's logo on the glass and then gyro's own splash over it, and a wallpaper that
// appeared the instant the compositor got far enough to read a file would be a picture racing the boot
// it is meant to arrive after. So the first image gyro is handed holds off for `BackgroundDelay` and
// then fades up from black. A *replacement* starts immediately, because the picture it is fading from
// is already on screen: delaying that would be a black gap between two wallpapers, on the one
// interaction — a person changing their background — where the answer is supposed to be instant.
//
// **What replaces a background is a cross-fade, and it is one commit.** The outgoing nodes fade to
// nothing and are retired in the same scope; decision 114 keeps a retired subtree published for as
// long as anything on it is still moving, so the fade *is* the exit and nothing here has to remember
// to sweep. The incoming nodes are created transparent and fade up, so the first background on a
// machine — the one the boot animation hands over to — fades in from black by the same path rather
// than by a second one.
//
// **The texture outlives the fade and this is what holds it.** `ITextures::Retire` promises an id to
// the frames already published, and a node still fading out is a frame not yet published — so the
// outgoing image is given up when the last node naming it has left the world, which is what a sheet
// below is counting.

// SPEC: how long the first background waits before it begins to fade up.
//
// Two seconds is a boot's worth of everything else — the splash gyro draws over the firmware's logo,
// and the session coming up behind it — and it is a number about the *machine* rather than about the
// picture, which is why it is stated here rather than passed in. It applies only where there is
// nothing to fade from; see above.
inline constexpr Duration BackgroundDelay = std::chrono::milliseconds{ 500 };

// One adopted image and the nodes drawing it.
//
// **A record per image rather than a current-and-previous pair**, because two backgrounds can be
// replaced faster than one fade finishes and the cost of getting that wrong is a texture reclaimed
// while a panel is still sampling it. Only one sheet is ever current; the rest are on their way out.
struct BackgroundSheet
{
	TextureId Texture{};

	// The device pixels this image is, which is what an output's own extent is compared against.
	PixelSize<BufferSpace> Size{};

	// Whether this is the background gyro is holding. Exactly one sheet has it.
	bool Current = false;

	// The instant this image may start being drawn. Now for a replacement, and `BackgroundDelay` out
	// for the first one on a machine — which is what makes the wait *before* the fade rather than a
	// slower fade, the two being different pictures: a background that is faintly there for two seconds
	// is a smear over the splash, and one that is not there at all is a boot.
	Instant Due{};

	// One node per output showing this image, and the ones on their way out.
	struct Panel
	{
		// The output this node covers, or null once it is fading and belongs to nobody — which is what
		// keeps a mode change from re-authoring onto an output that already has a node leaving.
		OutputId Output{};

		EntityId Node{};
	};

	std::vector<Panel> Panels;
};

class SceneBackground
{
public:
	// Author the container. Called once, before the author builds its tree.
	//
	// False where the store would not take another entity, which is a scene at capacity before anything
	// has been authored into it — a wiring mistake rather than a state to serve.
	bool Open(SceneStore& scene)
	{
		if (!m_Container.IsNull())
		{
			return true;
		}

		// No extent and no content: the container is where the images hang, and a rectangle here would
		// be a black quad over every output on a machine that has no background — which is the same
		// picture, drawn at a cost, and one that would have to be culled again the moment a session
		// wanted its own colour behind it.
		const std::optional<EntityId> container = scene.CreateContainer(EntityId{}, {});

		if (!container)
		{
			return false;
		}

		m_Container = *container;

		return true;
	}

	// Take this image as the background, cross-fading from whatever gyro was holding.
	//
	// The pixels are read during the call and the registry holds them afterwards, which is
	// `Scene/Textures.h`'s contract — nothing here owns a texel past this line.
	//
	// The nodes are not authored here: `Step` is what puts an image onto the outputs it fits, so a
	// background handed over while a monitor is asleep lands on it when it comes back.
	//
	// `ENODEV` before `Open`; whatever the texture space answered where it refused the pixels.
	[[nodiscard]] Result<void> Set(SceneStore& scene, ITextures& textures, const PamImage& image)
	{
		if (m_Container.IsNull())
		{
			return Failure(ENODEV, "a background before the container was authored");
		}

		const PixelSize<BufferSpace> size{ image.Width(), image.Height() };

		const Result<TextureId> texture = textures.Adopt(
			size, image.Stride(), image.Bytes(), image.HasAlpha() ? TextureAlpha::Premultiplied : TextureAlpha::None
		);

		if (!texture)
		{
			return std::unexpected{ texture.error() };
		}

		// Read before the dismissal, because the dismissal is what takes them off screen: what decides
		// the wait is whether there is a picture to fade *from*, and after `Dismiss` every sheet is on
		// its way out whether or not it had ever been drawn.
		const bool showing = Showing();

		Dismiss(scene);

		m_Sheets.push_back(
			BackgroundSheet{ .Texture = *texture,
		                     .Size = size,
		                     .Current = true,
		                     .Due = showing ? scene.Now() : Advanced(scene.Now(), BackgroundDelay),
		                     .Panels = {} }
		);

		return {};
	}

	// Take the background away, fading out to black and holding nothing.
	//
	// This is a session ending on a machine whose shell has gone, and it is separate from `Set` because
	// there is no image to hand over — a caller that had to pass one would be inventing a picture in
	// order to say *no picture*.
	void Clear(SceneStore& scene) { Dismiss(scene); }

	// One dispatch iteration: put the current image on the outputs it fits, take it off the ones it
	// does not, and give up an image nothing is drawing any more.
	//
	// Silent about failure for `SceneCursor::Step`'s reason — a background that would not author is a
	// compositor that keeps running with black behind the windows, and there is nobody on this call to
	// tell.
	//
	// **It answers a wake, which the cursor does not have to.** A pointer is moved by a person and the
	// input that moves it is what wakes the loop; the first background is waiting on nothing but the
	// clock, so a world that has settled would sleep straight through the instant it was meant to
	// appear. `Wake::Never()` at every other moment, because the fade itself is a spring the serialiser
	// already folds.
	[[nodiscard]] Wake Step(SceneStore& scene, ITextures& textures)
	{
		if (m_Container.IsNull())
		{
			return Wake::Never();
		}

		Reclaim(scene, textures);

		Wake wake = Wake::Never();

		for (BackgroundSheet& sheet : m_Sheets)
		{
			if (sheet.Current)
			{
				wake = Sooner(wake, Reconcile(scene, sheet));
			}
		}

		return wake;
	}

	// The container the images hang under, or null before `Open`. For a test, and for the day the hit
	// test has to know which node is not a window.
	[[nodiscard]] EntityId Container() const noexcept { return m_Container; }

	// The image gyro is holding, or null where it is holding none. What is still fading out is not it.
	[[nodiscard]] TextureId Texture() const noexcept
	{
		for (const BackgroundSheet& sheet : m_Sheets)
		{
			if (sheet.Current)
			{
				return sheet.Texture;
			}
		}

		return {};
	}

	// Every sheet, current and departing, for a test that asserts what is still on screen.
	[[nodiscard]] std::span<const BackgroundSheet> Sheets() const noexcept { return m_Sheets; }

	// The device extent an image has to be to be shown on this output.
	//
	// **The output's own logical extent in device pixels, rather than its mode.** The two are the same
	// number on an ordinary panel and are transposed on one stood on end: a mode is the glass and this
	// is the rectangle the world puts on it, and a wallpaper is authored in the space the world is laid
	// out in. Taking the mode instead would refuse every image on a rotated monitor and accept a
	// sideways one on the day somebody produced it.
	[[nodiscard]] static PixelSize<BufferSpace> FittingSize(const SceneOutput& output) noexcept
	{
		const auto extent = [](double logical, Scale density) {
			return density.DeviceFromLogical(static_cast<std::int32_t>(logical + 0.5), Rounding::Nearest);
		};

		return { extent(output.Bounds.Extent.Width, output.Density),
			     extent(output.Bounds.Extent.Height, output.Density) };
	}

private:
	// Everything current stops being current and starts leaving.
	void Dismiss(SceneStore& scene)
	{
		SceneCommit commit{ scene, CommitAuthor::Compositor, scene.Now(), Transition::BackgroundChange };

		for (BackgroundSheet& sheet : m_Sheets)
		{
			if (!sheet.Current)
			{
				continue;
			}

			sheet.Current = false;

			for (BackgroundSheet::Panel& panel : sheet.Panels)
			{
				panel.Output = {};

				Leave(commit, panel.Node);
			}
		}
	}

	// One node fades to nothing and is retired in the same scope.
	//
	// **The fade is written before the retire and the order is load-bearing.** Retirement takes a
	// subtree out of the authorable world (decision 114), so a fade written after one is a write to a
	// node that has stopped listening — and the picture that produces is a background that vanishes on
	// the frame it was asked to fade, which is the single most visible thing this file could get wrong.
	static void Leave(SceneCommit& commit, EntityId node)
	{
		static_cast<void>(commit.Fade(node, 0.0F));
		static_cast<void>(commit.Retire(node));
	}

	// Drop the nodes that have finished leaving, and give up an image once the last of them has.
	void Reclaim(SceneStore& scene, ITextures& textures)
	{
		for (BackgroundSheet& sheet : m_Sheets)
		{
			std::erase_if(sheet.Panels, [&scene](const BackgroundSheet::Panel& panel) {
				return !scene.IsLive(panel.Node);
			});
		}

		std::erase_if(m_Sheets, [&textures](const BackgroundSheet& sheet) {
			if (sheet.Current || !sheet.Panels.empty())
			{
				return false;
			}

			// **Given up here rather than at the dismissal**, which is the whole of why a sheet is a
			// record. The id was still being drawn for every frame of the fade, and `ITextures::Retire`
			// promises it only to the frames already published — so retiring it at the moment the
			// replacement arrived would be a wallpaper sampling memory the registry had reclaimed.
			textures.Retire(sheet.Texture);

			return true;
		});
	}

	// The current sheet against the output set: one node per output it fits, and none anywhere else.
	//
	// Answers when to come back where it is holding an image that is not due yet.
	[[nodiscard]] Wake Reconcile(SceneStore& scene, BackgroundSheet& sheet)
	{
		const std::span<const SceneOutput> outputs = scene.Outputs();

		SceneCommit commit{ scene, CommitAuthor::Compositor, scene.Now(), Transition::BackgroundChange };

		// An output that has gone away, or whose extent no longer matches, loses its node. A monitor
		// unplugged takes its own with it; a mode change is the case worth having, because the image
		// that fitted a moment ago is now the wrong number of pixels and stretching it is the thing
		// this file declines to do.
		for (BackgroundSheet::Panel& panel : sheet.Panels)
		{
			if (panel.Output.IsNull())
			{
				continue;
			}

			const SceneOutput* const output = Find(outputs, panel.Output);

			if (output != nullptr && FittingSize(*output) == sheet.Size)
			{
				continue;
			}

			panel.Output = {};

			Leave(commit, panel.Node);
		}

		// **Nothing is authored before it is due, rather than authored transparent and left there.**
		// Nothing in the frame walk culls a fully faded node, so a wallpaper waiting at zero opacity is
		// a full-screen composite for every frame of the wait — which is `SceneCursor`'s finding about a
		// hidden pointer, on a quad the size of the screen.
		if (scene.Now() < sheet.Due)
		{
			return Wake::At(sheet.Due);
		}

		for (const SceneOutput& output : outputs)
		{
			if (FittingSize(output) != sheet.Size || Holds(sheet, output.Id))
			{
				continue;
			}

			const std::optional<EntityId> node = Author(scene, output, sheet);

			if (!node)
			{
				continue;
			}

			// Created transparent and faded up, in the same commit the departing nodes are fading down
			// in, so the two halves of a cross-fade share one origin and start on the same instant.
			static_cast<void>(commit.Fade(*node, 1.0F));

			sheet.Panels.push_back(BackgroundSheet::Panel{ .Output = output.Id, .Node = *node });
		}

		return Wake::Never();
	}

	// Whether anything is on screen: a sheet with a node on it, current or leaving.
	[[nodiscard]] bool Showing() const noexcept
	{
		for (const BackgroundSheet& sheet : m_Sheets)
		{
			if (!sheet.Panels.empty())
			{
				return true;
			}
		}

		return false;
	}

	// One image node covering an output, transparent until the commit that authored it fades it up.
	[[nodiscard]] std::optional<EntityId>
	Author(SceneStore& scene, const SceneOutput& output, const BackgroundSheet& sheet)
	{
		const NodeProperties properties{
			.Position = { output.Bounds.Origin.X, output.Bounds.Origin.Y, 0.0 },
			.Extent = { static_cast<float>(output.Bounds.Extent.Width),
			            static_cast<float>(output.Bounds.Extent.Height) },
			.Opacity = 0.0F,
			// Snapped for decision 52's reason and the pointer glyph's: the quad is one texel of the
			// image per pixel of the panel, and a background that landed on a half-pixel would be the
			// one image on screen where a resampler is guaranteed to run.
			.Flags = Node::Snap,
		};

		// The whole image, and sRGB because that is what a file with no tag on it is. A colour-managed
		// background is what `ColorState` is for and is not a thing anything hands over yet.
		const ImageContent content{
			.Texture = sheet.Texture,
			.Source = { {}, { static_cast<float>(sheet.Size.Width), static_cast<float>(sheet.Size.Height) } },
			.Frame = {},
			.Color = ColorState::Srgb()
		};

		return scene.CreateImage(m_Container, properties, content);
	}

	[[nodiscard]] static const SceneOutput* Find(std::span<const SceneOutput> outputs, OutputId id) noexcept
	{
		for (const SceneOutput& output : outputs)
		{
			if (output.Id == id)
			{
				return &output;
			}
		}

		return nullptr;
	}

	[[nodiscard]] static bool Holds(const BackgroundSheet& sheet, OutputId id) noexcept
	{
		for (const BackgroundSheet::Panel& panel : sheet.Panels)
		{
			if (panel.Output == id)
			{
				return true;
			}
		}

		return false;
	}

	EntityId m_Container{};

	// Rarely more than two: the one on screen and the one leaving it.
	std::vector<BackgroundSheet> m_Sheets;
};
