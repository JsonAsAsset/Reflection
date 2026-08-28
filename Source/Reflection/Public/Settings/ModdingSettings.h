/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"

/* Settings Substructures */
#include "Types/DNASettings.h"
#include "Types/MaterialModdingSettings.h"
#include "Types/CurveMappingSettings.h"

#include "ModdingSettings.generated.h"

USTRUCT()
struct FRModdingSettings {
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, Config, Category = Settings)
	FRMaterialModdingSettings Material;

	UPROPERTY(EditAnywhere, Config, Category = Settings)
	FRDnaSettings MetaHuman;

	/* What a curve mapping is brought in as. */
	UPROPERTY(EditAnywhere, DisplayName = "Curve Mapping", Config, Category = Settings)
	ERCurveMapping CurveMapping = ERCurveMapping::DataAsset;
};

UCLASS(Config = EditorPerProjectUserSettings, DefaultConfig)
class REFLECTION_API UReflectionModdingSettings : public UDeveloperSettings {
	GENERATED_BODY()
public:
	UReflectionModdingSettings();

	virtual FText GetSectionText() const override;

public:
	UPROPERTY(EditAnywhere, DisplayName = "Enable", Config, Category = Settings)
	bool Enabled = false;

	UPROPERTY(EditAnywhere, Config, Category = Settings, meta = (EditCondition = "Enabled", EditConditionHides))
	FRModdingSettings Settings;
};
