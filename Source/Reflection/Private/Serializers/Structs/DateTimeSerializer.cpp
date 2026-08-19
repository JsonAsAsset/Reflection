/* Copyright Reflection Contributors 2024-2026 */

#include "Serializers/Structs/DateTimeSerializer.h"

#include "Dom/JsonObject.h"

void FDateTimeSerializer::Deserialize(UScriptStruct* Struct, void* StructData, const TSharedPtr<FJsonObject> JsonValue, UObject* OptionalOuter) {
	FDateTime* DateTime = static_cast<FDateTime*>(StructData);
	const int64 Ticks = FCString::Atoi64(*JsonValue->GetStringField(TEXT("Ticks")));
	*DateTime = FDateTime(Ticks);
}