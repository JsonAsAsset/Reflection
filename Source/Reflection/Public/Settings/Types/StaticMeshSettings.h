/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "StaticMeshSettings.generated.h"

/* Settings for static meshes */
USTRUCT()
struct FRStaticMeshSettings {
	GENERATED_BODY()
public:
	/* Meshes often ship with a minimum LOD, which hides their most detailed LODs on lower quality
	 * settings. Turn this on to show the best LOD the mesh has, or off to leave it as the game had
	 * it. Meshes that only have vertex colours on their later LODs still start from those, so they
	 * don't come out white. */
	UPROPERTY(EditAnywhere, DisplayName = "Show Best LOD", Config, Category = StaticMeshSettings)
	bool IgnoreMinQualityLevelLODDefault = true;
};
