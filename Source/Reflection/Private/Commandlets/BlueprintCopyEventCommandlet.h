/* Copyright Reflection Contributors 2024-2026 */

/* Copies what one event does from one blueprint into another already on disk, and touches nothing else on it:
 *
 *     -run=BlueprintCopyEvent -from=/Game/Scratch/Some/Blueprint -to=/Game/Some/Blueprint -event=OnSomething
 *
 * The source is usually a full import made somewhere out of the way with BlueprintImport -into=, when importing the
 * whole blueprint over the real one would throw away what it already does. Every node the event runs, and every node
 * those read from, comes across. The variables those nodes name have to be on the target already: BlueprintAddVariables
 * puts them there.
 *
 * -functions=A,B brings whole function graphs the event calls on the blueprint itself, when the target has none of that
 * name. */

#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "BlueprintCopyEventCommandlet.generated.h"

UCLASS()
class UBlueprintCopyEventCommandlet : public UCommandlet {
	GENERATED_BODY()

public:
	virtual int32 Main(const FString& Params) override;
};
