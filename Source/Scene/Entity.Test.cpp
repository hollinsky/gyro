#include "Scene/Entity.h"

#include "Animation/Author/Retarget.h"
#include "Core/Time.h"
#include "Testing/Test.h"
#include "World/Node.h"

// The one-to-one of Docs/Decisions.md decision 111, asserted field by field: an entity is a node's
// authoring side, so what `Record` produces has to be the node the author described and nothing else.

namespace
{
constexpr NodeProperties Panel{ .Position = { 3.0, 4.0, 0.0 },
	                            .Extent = { 100.0F, 40.0F },
	                            .Opacity = 0.5F,
	                            .TimeScale = 2.0F,
	                            .Flags = Node::Hidden,
	                            .Dress = Material::Glass,
	                            .Lift = Elevation::Floating };
} // namespace

GYRO_TEST(Entity, AnAuthoredNodeIsTheRecordItPublishes)
{
	const Entity entity{ Panel };
	const Node node = entity.Record();

	GYRO_CHECK(node.Transform.Translation == Vector3<double>{ 3.0, 4.0, 0.0 });
	GYRO_CHECK_EQ(node.Extent, Size<SurfaceSpace, float>{ 100.0F, 40.0F });
	GYRO_CHECK_EQ(node.Opacity, 0.5F);
	GYRO_CHECK_EQ(node.TimeScale, 2.0F);
	GYRO_CHECK(node.IsHidden());
	GYRO_CHECK(node.Dress == Material::Glass);
	GYRO_CHECK(node.Lift == Elevation::Floating);
}

GYRO_TEST(Entity, TheRecordNamesNothingThatIsAPositionInASerialisation)
{
	const Node node = Entity{ Panel }.Record();

	// The three fields the serializer supplies, because a record cannot know them about itself: a
	// subtree length is the walk's, and a channel slot and a content index are positions in runs that
	// walk is still building.
	GYRO_CHECK_EQ(node.SubtreeLength, std::uint32_t{ 0 });
	GYRO_CHECK(!node.IsTranslating() && !node.IsScaling() && !node.IsRotating() && !node.IsFading());
	GYRO_CHECK_EQ(node.Content, NoContent);
}

GYRO_TEST(Entity, ANewEntityIsAtRestOnEveryChannel)
{
	const Entity entity{};

	GYRO_CHECK(entity.Translation.IsAtRest());
	GYRO_CHECK(entity.Scale.IsAtRest());
	GYRO_CHECK(entity.Turn.IsAtRest());
	GYRO_CHECK(entity.Opacity.IsAtRest());

	// Which is the state that makes the first commit against it an ordinary transition rather than a
	// case of its own — see Animation/Author/Animatable.h.
	const Node node = entity.Record();

	GYRO_CHECK_EQ(node.Opacity, 1.0F);
	GYRO_CHECK(node.Transform.Scale == Vector3<float>{ 1.0F, 1.0F, 1.0F });
	GYRO_CHECK(node.IsContainer());
}

GYRO_TEST(Entity, TheInlineValueIsTheModelWhetherOrNotTheChannelIsMoving)
{
	Entity entity{ Panel };

	entity.Translation.AnimateTo(Vector3<double>{ 40.0, 4.0, 0.0 }, ParametersFromResponse(0.4, 1.0), Instant{});

	// Decision 86 puts the model value inline and the coefficients by reference, so this is redundant
	// while the spring runs and is the whole answer the moment it settles (decision 98). Publishing it
	// unconditionally is what makes those one line rather than two cases — and what the walk on the far
	// side reads only when the node names no coefficient.
	GYRO_CHECK(!entity.Translation.IsAtRest());
	GYRO_CHECK(entity.Record().Transform.Translation == Vector3<double>{ 40.0, 4.0, 0.0 });
}

GYRO_TEST(Entity, TheOrientationCrossesWhetherOrNotAnythingIsTurning)
{
	const Quaternion turned = Quaternion::Exp(RotationVector{ 0.0F, 0.4F, 0.0F });

	Entity entity{ NodeProperties{ .Orientation = turned } };

	// Docs/Animation.md anchors the log map at the target, so the sprung channel is a *deviation* whose
	// target is zero and a settled one reads (0, 0, 0). The orientation is the chart's base point and
	// has to cross unconditionally, which is decision 90's rule — a node carries whatever reconstitutes
	// its value — with the one channel that is not inline-when-settled.
	GYRO_CHECK(entity.Turn.IsAtRest());
	GYRO_CHECK(entity.Record().Transform.Rotation == turned);

	entity.Turn.AnimateTo(
		RotationVector{}, ParametersFromResponse(0.4F, 1.0F), Instant{}, RotationVector{ 1.0F, 0.0F, 0.0F }
	);

	GYRO_CHECK(!entity.Turn.IsAtRest());
	GYRO_CHECK(entity.Record().Transform.Rotation == turned);
}
