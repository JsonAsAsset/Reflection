/* Copyright Reflection Contributors 2024-2026 */

/* Compares what the Cloud serves for an asset against the asset this project has, property by
 * property, and says where the two disagree:
 *
 *     -run=AssetCompare -path=/Game/Some/Asset
 *
 * The asset compared against is the one at the same path unless -asset= says otherwise. -from=
 * reads the export out of a file rather than asking the Cloud, -exports also compares every
 * subobject the export carries, and -all lists what agreed as well as what did not.
 *
 * Nothing is imported and nothing is written. */

#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "AssetCompareCommandlet.generated.h"

UCLASS()
class UAssetCompareCommandlet : public UCommandlet {
	GENERATED_BODY()

public:
	virtual int32 Main(const FString& Params) override;
};
