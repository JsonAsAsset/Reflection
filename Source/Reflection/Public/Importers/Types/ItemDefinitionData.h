/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"

class UObjectSerializer;

/* The item definition's data list, read back onto the properties it stands for.
 *
 * Newer cooks keep an item definition's icons, tags, series and rarity in a DataList rather than on
 * the definition itself: an array whose entries each carry one thing and nothing else. Which entry
 * carries what is not fixed, so an entry is read by the member it has rather than by where it sits
 * in the array. */
struct REFLECTION_API FItemDefinitionData {
	/* Writes whatever the list carries onto Asset, leaving alone anything the export already said
	 * outright and anything this build has no property for. Says how many it wrote. */
	static int32 Apply(UObject* Asset, const TSharedPtr<FJsonObject>& Data, const UObjectSerializer* Serializer);
};
