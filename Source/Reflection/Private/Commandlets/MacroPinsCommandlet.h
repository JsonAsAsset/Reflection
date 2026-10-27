/* Copyright Reflection Contributors 2024-2026 */

/* Prints a StandardMacros graph's tunnel pins and nodes, which is what a macro pattern has to match against:
 *
 *     -run=MacroPins -macro=DecrementInt */

#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "MacroPinsCommandlet.generated.h"

UCLASS()
class UMacroPinsCommandlet : public UCommandlet {
	GENERATED_BODY()

public:
	virtual int32 Main(const FString& Params) override;
};
