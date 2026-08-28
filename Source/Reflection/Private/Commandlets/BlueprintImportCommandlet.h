/* Copyright Reflection Contributors 2024-2026 */

/* Headless blueprint import, so the bytecode a rebuilt graph compiles to can be compared against
 * the bytecode the game shipped without anyone sat in front of the editor:
 *
 *     -run=BlueprintImport -path=<cloud asset path>
 *
 * It imports, compiles and saves, which leaves an asset on disk the same parser that read the
 * cooked one can read back. */

#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "BlueprintImportCommandlet.generated.h"

UCLASS()
class UBlueprintImportCommandlet : public UCommandlet {
	GENERATED_BODY()

public:
	virtual int32 Main(const FString& Params) override;
};
