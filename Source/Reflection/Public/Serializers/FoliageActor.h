/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"

class AInstancedFoliageActor;
class UPropertySerializer;

/* Plants back the foliage an instanced foliage actor was carrying.
 *
 * The actor keeps none of it as properties. What it keeps is a map from the foliage type to the
 * component grown for it, and the component keeps a transform for every instance, both of them
 * sitting beside the properties rather than among them. Neither is anything the property serializer
 * walks, so the actor arrives with a root and nothing growing on it. */
class REFLECTION_API FFoliageActor {
public:
	/* Grows on Actor whatever its own export was carrying, which is looked up among the exports
	 * the serializer is working through */
	static void Rebuild(AInstancedFoliageActor* Actor, const UPropertySerializer* Serializer);
};
