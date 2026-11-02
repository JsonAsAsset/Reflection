/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"
#include "Kismet2/KismetEditorUtilities.h"

class UBlueprint;

/* Compiling a blueprint that was read out of a game, without taking the editor with it */
REFLECTION_API bool CompileBlueprintGuarded(UBlueprint* Blueprint,
	EBlueprintCompileOptions Options = EBlueprintCompileOptions::None);

/* Whether the class a blueprint ended up with is one the engine can make an object of.
 *
 * A compile that failed leaves the generated class half built: no constructor, nothing to be
 * within. The engine asserts on exactly that the moment anything asks the class for its default
 * object, and handing the blueprint over is enough to make something ask, so this is checked
 * before it is handed anywhere rather than after. */
REFLECTION_API bool IsBlueprintClassBuilt(const UBlueprint* Blueprint);

/* Making a blueprint, which compiles it before it hands it back and so can go the same way. */
REFLECTION_API UBlueprint* CreateBlueprintGuarded(UClass* ParentClass, UObject* Outer, FName Name,
	EBlueprintType BlueprintType, UClass* BlueprintClassType, UClass* BlueprintGeneratedClassType);
