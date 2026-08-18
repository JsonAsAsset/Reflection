/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "SkeletalMeshSettings.generated.h"

/* Settings for skeletal meshes */
USTRUCT()
struct FRSkeletalMeshSettings {
	GENERATED_BODY()
public:
	/* A MetaHuman head animates through RigLogic, which reads the mesh's DNA and poses the face's
	 * joints. Turn this on to bake that rig down into a pose asset beside the mesh: every control
	 * the DNA names is evaluated on its own and the joints it moves are kept as a pose.
	 *
	 * The poses are additive, against the rig standing still, so they add up the way the controls
	 * do. What a control does on its own is exact; the corrective layers a DNA runs on top of the
	 * controls are not part of a pose and don't survive. */
	UPROPERTY(EditAnywhere, DisplayName = "Bake DNA to Pose Asset", Config, Category = SkeletalMeshSettings)
	bool BakeDnaToPoseAsset = false;
};
