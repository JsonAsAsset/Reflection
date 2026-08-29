/* Copyright Reflection Contributors 2024-2026 */

/* Headless import of whatever the cloud has at a path, so what an importer makes of an asset can be
 * looked at without anyone sat in front of the editor:
 *
 *     -run=ReflectImport -path=<cloud asset path>
 *
 * It imports and saves, and says what came out: the objects the package ended up holding, and for
 * anything carrying a graph, how much of that graph is there. Enough to tell an import that worked
 * from one that made the objects and none of the wiring. */

#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "ReflectImportCommandlet.generated.h"

UCLASS()
class UReflectImportCommandlet : public UCommandlet {
	GENERATED_BODY()

public:
	virtual int32 Main(const FString& Params) override;
};
