/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "StructSerializer.h"

class FConnectivityCubeSerializer : public FStructSerializer {
public:
	virtual void Deserialize(UScriptStruct* Struct, void* StructData, const TSharedPtr<FJsonObject> JsonValue, UObject* OptionalOuter) override;
};
