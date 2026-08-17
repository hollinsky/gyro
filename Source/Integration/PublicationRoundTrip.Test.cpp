#include <array>
#include <cstddef>
#include <span>

#include "Animation/Author/Animatable.h"
#include "Animation/Solve/Spring.h"
#include "Core/FrameSection.h"
#include "Core/Time.h"
#include "Core/Wake.h"
#include "Publication/Publisher/Publisher.h"
#include "Publication/Reader/Reader.h"
#include "Testing/Test.h"

// The one test that stands on both sides of the publication waist.
//
// Publication does not depend on Animation — by the module graph, the snapshot carries springs as
// opaque bytes and knows nothing of Spring. So no module may both publish a real coefficient and
// reconstitute it through Animation/Solve, which is exactly the round trip Docs/Decisions.md decision
// 50 turns on: what crosses is coefficients, and the frame side evaluates them per output at that
// output's own predicted presentation time. This target links both halves to prove that the property
// survives the crossing, and it is a temporary home for the check until Scene and Frame exist to own
// their ends of it.
//
// **The assertion is exact equality, not nearness.** The coefficients cross byte for byte, and
// Spring::Evaluate is a pure function, so the frame-side evaluation of a reconstituted spring is
// bit-identical to the dispatch-side evaluation of the original. Anything less would mean the bytes
// did not survive, and an epsilon would hide it. This is the concrete form of decision 50's promise
// that an in-flight animation keeps producing exactly correct results across the boundary.

namespace
{
// The instants the round trip is checked at: the origin, a point mid-flight, and one well along.
// Mid-flight matters most — at the origin a spring is near its start and a late instant near its
// target, but in between is where a dropped coefficient would show as a value off by a visible amount.
constexpr std::array<Instant, 3> kProbes{ Monotonic::FromNanoseconds(0),
	                                      Monotonic::FromNanoseconds(120'000'000),
	                                      Monotonic::FromNanoseconds(600'000'000) };
} // namespace

GYRO_TEST(PublicationRoundTrip, SpringsEvaluateIdenticallyAcrossTheBoundary)
{
	// Two properties in flight, one per precision: a translation at double, an opacity at single, as
	// Docs/Architecture.md#the-spaces splits them. Authored through the ordinary path — a model value,
	// then a motion toward a new one — so what is published is what a commit would have published.
	Animatable<double> position{ 3.0 };
	position.AnimateTo(10.0, ParametersFromResponse(0.4, 1.0), Instant{});

	Animatable<float> opacity{ 0.0f };
	opacity.AnimateTo(1.0f, ParametersFromResponse(0.3f, 0.9f), Instant{});

	const Spring<double> positionCoefficients = position.Coefficients();
	const Spring<float> opacityCoefficients = opacity.Coefficients();

	const SnapshotBuffer buffer = SnapshotPublisher{}
	                                  .Sequence(1)
	                                  .Put<Spring<double>>(SnapshotRun::Positions, { &positionCoefficients, 1 })
	                                  .Put<Spring<float>>(SnapshotRun::Channels, { &opacityCoefficients, 1 })
	                                  .Build();

	const SnapshotReader reader{ buffer.Bytes() };
	GYRO_REQUIRE(reader.IsValid());

	const std::span<const Spring<double>> positions = reader.Run<Spring<double>>(SnapshotRun::Positions);
	const std::span<const Spring<float>> channels = reader.Run<Spring<float>>(SnapshotRun::Channels);
	GYRO_REQUIRE_EQ(positions.size(), std::size_t{ 1 });
	GYRO_REQUIRE_EQ(channels.size(), std::size_t{ 1 });

	for (const Instant probe : kProbes)
	{
		// Evaluate the reconstituted spring inside the frame section, so that the debug allocator of
		// decision 36 would abort if the frame-side read touched the heap. The comparisons are made
		// after the section closes, because a failed check formats its operands and that allocates.
		SpringState<double> fromSnapshot{};
		SpringState<float> channelFromSnapshot{};
		{
			const FrameSection guard;
			fromSnapshot = positions[0].Evaluate(probe);
			channelFromSnapshot = channels[0].Evaluate(probe);
		}

		const SpringState<double> fromModel = position.PresentationState(probe);
		GYRO_CHECK_EQ(fromSnapshot.Position, fromModel.Position);
		GYRO_CHECK_EQ(fromSnapshot.Velocity, fromModel.Velocity);

		const SpringState<float> channelFromModel = opacity.PresentationState(probe);
		GYRO_CHECK_EQ(channelFromSnapshot.Position, channelFromModel.Position);
		GYRO_CHECK_EQ(channelFromSnapshot.Velocity, channelFromModel.Velocity);
	}
}

GYRO_TEST(PublicationRoundTrip, AnAtRestSpringCrossesAndStaysAtRest)
{
	// A property that never moved publishes its coefficients like any other, and the far side must
	// agree it is at its value with zero velocity — the identically-zero solution the whole
	// derived-activity design rests on, checked through the bytes rather than in place.
	Animatable<double> resting{ 7.5 };
	const Spring<double> coefficients = resting.Coefficients();

	const SnapshotBuffer buffer =
		SnapshotPublisher{}.Put<Spring<double>>(SnapshotRun::Positions, { &coefficients, 1 }).Build();

	const SnapshotReader reader{ buffer.Bytes() };
	GYRO_REQUIRE(reader.IsValid());

	const std::span<const Spring<double>> positions = reader.Run<Spring<double>>(SnapshotRun::Positions);
	GYRO_REQUIRE_EQ(positions.size(), std::size_t{ 1 });

	const SpringState<double> state = positions[0].Evaluate(Monotonic::FromNanoseconds(1'000'000'000));
	GYRO_CHECK_EQ(state.Position, 7.5);
	GYRO_CHECK_EQ(state.Velocity, 0.0);
}
