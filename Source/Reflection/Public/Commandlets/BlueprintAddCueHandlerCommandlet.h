/* Copyright Reflection Contributors 2024-2026 */

/* Gives a blueprint already on disk a handler for one more gameplay cue, built in place beside what it already has, and
 * touches nothing else on it:
 *
 *     -run=BlueprintAddCueHandler -path=/Game/Some/Pawn -tag=GameplayCue.Some.Cue -like=GameplayCue.Other.Cue -spawn=SomeEffect -socket=root -skipdedicated
 *     -run=BlueprintAddCueHandler -path=/Game/Some/Pawn -tag=GameplayCue.Some.Cue -like=GameplayCue.Other.Cue -setbool=SomeFlag
 *
 * The handler is a custom event named after the tag, with the signature copied from the existing handler -like names, so
 * the cue interface finds it the same way. It runs only when the cue is executed. -spawn attaches the particle system held
 * in that variable to the mesh at -socket, and -skipdedicated leaves a dedicated server out. -setbool sets that variable.
 * -addvar=<name> -vardefault=<object path> adds the particle system variable first when the blueprint has not got it. */

#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "BlueprintAddCueHandlerCommandlet.generated.h"

UCLASS()
class UBlueprintAddCueHandlerCommandlet : public UCommandlet {
	GENERATED_BODY()

public:
	virtual int32 Main(const FString& Params) override;
};
