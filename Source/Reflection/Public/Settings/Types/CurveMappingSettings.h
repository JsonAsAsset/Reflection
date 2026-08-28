/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"
#include "CurveMappingSettings.generated.h"

/* What a curve mapping is brought in as.
 *
 * The asset says each of one head's curves is so much of this control plus so much of that one,
 * held between nothing and one. That is the same statement either way round; what differs is what
 * reads it.
 *
 * As the asset, it is the thing the game shipped, and the plugin that understands it drives the
 * curves. As a rig, the arithmetic is drawn out as nodes and runs in a forwards solve, which is a
 * thing you can open, read, wire something else into, and run on a head that has no such plugin.
 *
 * Only asked where both are possible. That plugin ships with Unreal Engine 5, and on an engine
 * without it there is no asset to make and the rig is what a mapping becomes either way. */
UENUM()
enum class ERCurveMapping : uint8 {
	/* The asset itself, expressions and all */
	DataAsset UMETA(DisplayName = "Data Asset"),

	/* The arithmetic, drawn as a rig that performs it */
	ControlRig UMETA(DisplayName = "Control Rig")
};
