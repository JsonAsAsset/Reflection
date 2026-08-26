/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"

class UBlueprint;
struct FUObjectExportContainer;

/* What the editor knew about a blueprint, kept through the cook.
 *
 * A cooked class carries no editor data, with one exception: the names, categories and tooltips a
 * reader would have seen are written out beside it so the game can still show them. None of it
 * changes what runs, and all of it is the difference between the asset somebody wrote and a list
 * of variables sat in Default with no names on them. */
struct REFLECTION_API FBlueprintCookedMetaData {
	/* Says over the blueprint whatever the cook kept, and answers how much it said */
	static int32 Apply(UBlueprint* Blueprint, FUObjectExportContainer* Container);
};
