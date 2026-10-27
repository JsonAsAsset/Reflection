/* Copyright Reflection Contributors 2024-2026 */

/* Adds named member variables to a blueprint already on disk, declared the way the importer declares them, and
 * touches nothing else on it:
 *
 *     -run=BlueprintAddVariables -path=/Game/Some/Blueprint -json=<export.json> -names=A,B,C */

#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "BlueprintAddVariablesCommandlet.generated.h"

UCLASS()
class UBlueprintAddVariablesCommandlet : public UCommandlet {
	GENERATED_BODY()

public:
	virtual int32 Main(const FString& Params) override;
};
