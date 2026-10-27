/* Copyright Reflection Contributors 2024-2026 */

/* Overrides a parent event on a blueprint already on disk so it calls the parent and then plays a gameplay cue held in a
 * new tag variable, the shape a pawn's death effects take, and touches nothing else on it:
 *
 *     -run=BlueprintAddEventCue -path=/Game/Some/Blueprint -event=OnDeathPlayEffects -variable="Some Cue" -tag=GameplayCue.Some.Cue
 *
 * -noparent leaves out the parent call, for an event the blueprint only implements. -hidemesh=<seconds> hides the mesh and
 * its children, waits, and shows them again before the cue plays, which is how a pawn teleports in. */

#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "BlueprintAddEventCueCommandlet.generated.h"

UCLASS()
class UBlueprintAddEventCueCommandlet : public UCommandlet {
	GENERATED_BODY()

public:
	virtual int32 Main(const FString& Params) override;
};
