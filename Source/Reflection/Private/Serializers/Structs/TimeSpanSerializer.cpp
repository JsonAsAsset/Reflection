/* Copyright Reflection Contributors 2024-2026 */

#include "Serializers/Structs/TimeSpanSerializer.h"

void FTimeSpanSerializer::Deserialize(UScriptStruct* Struct, void* StructData, const TSharedPtr<FJsonObject> JsonValue, UObject* OptionalOuter) {
	FTimespan* Timespan = static_cast<FTimespan*>(StructData);
	const int64 Ticks = FCString::Atoi64(*JsonValue->GetStringField(TEXT("Ticks")));
	*Timespan = FTimespan(Ticks);
}