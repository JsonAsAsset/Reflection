/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"
#include "SkeletalMeshSettings.generated.h"

/* Settings for skeletal meshes */
USTRUCT()
struct FRSkeletalMeshSettings {
	GENERATED_BODY()
public:
	/* Meshes often ship with a minimum LOD, which hides their most detailed LODs on lower quality
	 * settings. Turn this on to show the best LOD the mesh has, or off to leave it as the game had
	 * it. */
	UPROPERTY(EditAnywhere, DisplayName = "Show Best LOD", Config, Category = SkeletalMeshSettings)
	bool IgnoreMinQualityLevelLODDefault = true;
};
