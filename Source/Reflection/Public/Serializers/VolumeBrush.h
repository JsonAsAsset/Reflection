/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"

class ABrush;
class UBrushComponent;
class UPropertySerializer;

/* Gives a volume back the shape the cook took off it.
 *
 * A brush keeps its shape in UModel::Polys, which is editor only and so never reaches a cooked map.
 * What does reach one is the collision the editor built out of that shape: a body setup holding the
 * same volume as boxes and convex hulls, kept because a volume asks what it contains through its
 * collision. The shape reads back out of that.
 *
 * Without it UBrushComponent::CalcBounds has nothing to measure and answers with a point at the
 * actor, so the volume covers nothing, draws nothing, and nothing that asks what it contains ever
 * hears about it. */
class REFLECTION_API FVolumeBrush {
public:
	/* Builds the brush for Component's volume, where it has none and something to build one from.
	 *
	 * Properties are the component's own, and are read when the body setup itself never arrived: a
	 * map import spawns actors and leaves the objects hanging off their components alone, so the
	 * shape is taken out of the export the component names rather than off an object. */
	static void Rebuild(UBrushComponent* Component, const TSharedPtr<FJsonObject>& Properties = nullptr, const UPropertySerializer* Serializer = nullptr);

	/* The same, reached from the actor, for one whose body setup is already standing */
	static void Rebuild(ABrush* Brush);
};
