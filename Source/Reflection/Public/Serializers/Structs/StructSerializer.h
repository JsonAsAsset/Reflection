/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "UObject/Object.h"

/** Handles struct serialization */
class REFLECTION_API FStructSerializer {
public:
	virtual ~FStructSerializer() = default;
	virtual void Deserialize(UScriptStruct* Struct, void* StructData, const TSharedPtr<FJsonObject> JsonValue, UObject* OptionalOuter) = 0;
};