/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"
#include "MaterialSettings.generated.h"

/* Settings for materials */
USTRUCT()
struct FRMaterialSettings {
	GENERATED_BODY()
public:
	/* Creates stub versions of materials that have parameters (for Modding) */
	UPROPERTY(EditAnywhere, Config, Category = MaterialSettings)
	bool Stubs = false;
};