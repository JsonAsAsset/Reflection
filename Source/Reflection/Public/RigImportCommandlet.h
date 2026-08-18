/* Copyright Reflection Contributors 2024-2026 */

/* Headless rig import for verification: -run=RigImport -json=<file>, or -load=<asset path>
 * to cold-read a saved rig and report its graph */

#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "RigImportCommandlet.generated.h"

UCLASS()
class URigImportCommandlet : public UCommandlet {
	GENERATED_BODY()

public:
	virtual int32 Main(const FString& Params) override;
};
