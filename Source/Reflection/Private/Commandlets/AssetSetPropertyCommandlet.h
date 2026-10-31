/* Copyright Reflection Contributors 2024-2026 */

/* Sets properties on an asset already on disk and saves it, touching nothing else on it:
 *
 *     -run=AssetSetProperty -path=/Game/Some/Asset -set=SomeProperty=Value -set=Other=/Game/Some/Other.Other
 *
 * Each value is read the way the editor reads a pasted property, so an object is named by its
 * path and a struct is written out as it would be copied. Every property set is read back and
 * said, and nothing is saved if any of them could not be set. */

#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "AssetSetPropertyCommandlet.generated.h"

UCLASS()
class UAssetSetPropertyCommandlet : public UCommandlet {
	GENERATED_BODY()

public:
	virtual int32 Main(const FString& Params) override;
};
