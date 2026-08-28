/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"
#include "MaterialModdingSettings.generated.h"

/* What a material is opened up with, for building on top of rather than reading back. */
USTRUCT()
struct FRMaterialModdingSettings {
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, DisplayName = "Stubs", Config, Category = MaterialSettings)
	bool Stubs = false;
};
