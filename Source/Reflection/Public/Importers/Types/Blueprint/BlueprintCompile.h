/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"
#include "Kismet2/KismetEditorUtilities.h"

class UBlueprint;

/* Compiling a blueprint that was read out of a game, without taking the editor with it */
REFLECTION_API bool CompileBlueprintGuarded(UBlueprint* Blueprint,
	EBlueprintCompileOptions Options = EBlueprintCompileOptions::None);

/* Making a blueprint, which compiles it before it hands it back and so can go the same way. */
REFLECTION_API UBlueprint* CreateBlueprintGuarded(UClass* ParentClass, UObject* Outer, FName Name,
	EBlueprintType BlueprintType, UClass* BlueprintClassType, UClass* BlueprintGeneratedClassType);
