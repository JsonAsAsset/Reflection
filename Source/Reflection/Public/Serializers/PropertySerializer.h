/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "ObjectSerializer.h"
#include "Containers/ExportContainer.h"
#include "Dom/JsonObject.h"
#include "Structs/StructSerializer.h"
#include "UObject/Object.h"
#include "UObject/UnrealType.h"
#include "Engine/Compatibility.h"
#include "PropertySerializer.generated.h"

class IImporter;

UCLASS()
class REFLECTION_API UPropertySerializer : public UObject
{
	GENERATED_BODY()
public:
	UPropertySerializer();

	/** Disables property serialization entirely */
	void DisablePropertySerialization(const UStruct* Struct, FName PropertyName);
	void AddStructSerializer(UScriptStruct* Struct, const TSharedPtr<FStructSerializer>& Serializer);

	/** Checks whenever we should serialize property in question at all */
	bool ShouldDeserializeProperty(FProperty* Property) const;

	void DeserializePropertyValue(FProperty* Property, const TSharedRef<FJsonValue>& Value, void* OutValue, UObject* OptionalOuter = nullptr);
	void DeserializeStruct(UScriptStruct* Struct, const TSharedRef<FJsonObject>& Value, void* OutValue, UObject* OptionalOuter = nullptr) const;
private:
	FStructSerializer* GetStructSerializer(const UScriptStruct* Struct) const;
	
public:
	friend class UObjectSerializer;

	UPROPERTY()
	UObjectSerializer* ObjectSerializer;

	IImporter* Importer;
	
	TArray<FProperty*> BlacklistedProperties;
	TSharedPtr<FStructSerializer> FallbackStructSerializer;
	TMap<UScriptStruct*, TSharedPtr<FStructSerializer>> StructSerializers;

	bool FallbackToParentTrace = true;

	FUObjectExportContainer* ExportsContainer;
	TArray<FString> BlacklistedPropertyNames;
};

/* Use to handle differentiating formats produced by UEParse */
inline bool PassthroughPropertyHandler(FProperty* Property, const FString& PropertyName, void* PropertyValue, const TSharedPtr<FJsonObject>& Properties, UPropertySerializer* PropertySerializer) {
	/* Handles static arrays in the format of: PropertyName[Index] */
	if (Property->ArrayDim != 1) {
		TArray<TSharedPtr<FJsonValue>> ArrayElements;

		/* Finds array elements with the format: PropertyName[Index] and sets them properly into an array */
		for (const auto& Pair : Properties->Values) {
			const FString& Key = Pair.Key;
			const TSharedPtr<FJsonValue> Value = Pair.Value;

			/* If it doesn't start with the same property name */
			if (!Key.StartsWith(PropertyName)) continue;

			/* By default, it should be 0 */
			int32 CurrentArrayIndex = 0;

			/* If it is formatted like PropertyName[Index] */
			if (Key.Contains("[") && Key.Contains("]")) {
				int32 OpenBracketPos, CloseBracketPos;

				/* Find the index in PropertyName[Index] (integer) */
				if (Key.FindChar('[', OpenBracketPos) && Key.FindChar(']', CloseBracketPos) && CloseBracketPos > OpenBracketPos) {
					FString IndexStr = Key.Mid(OpenBracketPos + 1, CloseBracketPos - OpenBracketPos - 1);
					
					CurrentArrayIndex = FCString::Atoi(*IndexStr);
				}
			}

			/* Fail-save? */
			if (CurrentArrayIndex >= ArrayElements.Num()) {
				ArrayElements.SetNum(CurrentArrayIndex + 1);
			}
			
			ArrayElements[CurrentArrayIndex] = Value;
		}

		/* Array elements is filled up, now we set them in the property value */
		for (int32 ArrayIndex = 0; ArrayIndex < ArrayElements.Num(); ArrayIndex++) {
			uint8* ArrayPropertyValue = static_cast<uint8*>(PropertyValue) + GetElementSize(Property) * ArrayIndex;

			if (!ArrayElements.IsValidIndex(ArrayIndex)) continue;

			TSharedPtr<FJsonValue> ArrayJsonElement = ArrayElements[ArrayIndex];

			/* Check to see if it's null */
			if (ArrayJsonElement == nullptr || ArrayJsonElement->IsNull()) continue;
			
			const TSharedRef<FJsonValue> ArrayJsonValue = ArrayJsonElement.ToSharedRef();

			PropertySerializer->DeserializePropertyValue(Property, ArrayJsonValue, ArrayPropertyValue);
		}

		return true;
	}

	return false;
}