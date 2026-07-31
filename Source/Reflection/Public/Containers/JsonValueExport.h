/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "Dom/JsonObject.h"
#include "UObject/Object.h"
#include "Engine/Compatibility.h"

struct REFLECTION_API FUObjectJsonValueExport {
	FUObjectJsonValueExport() {
		JsonObject = MakeShared<FJsonObject>();
		Value = MakeShared<FJsonValueObject>(JsonObject);
	}
	
	FUObjectJsonValueExport(const TSharedPtr<FJsonValue>& Value) : Value(Value) {
		if (Value.IsValid() && Value->Type == EJson::Object) {
			JsonObject = Value->AsObject();
		}
	}

	FUObjectJsonValueExport(const TSharedPtr<FJsonObject>& InJsonObject) {
		JsonObject = InJsonObject;

		/* Only create Value if needed */
		if (JsonObject.IsValid()) {
			Value = MakeShared<FJsonValueObject>(JsonObject);
		}
	}
	
	TSharedPtr<FJsonValue> Value;
	TSharedPtr<FJsonObject> JsonObject;
	
	/* Json Helpers */
	FString GetString(const FString& FieldName) const {
		return JsonObject->GetStringField(FieldName);
	}

	void SetString(const FString& FieldName, const FString& NewValue) const {
		JsonObject->SetStringField(FieldName, NewValue);
	}

	int32 GetInteger(const FString& FieldName) const {
		return JsonObject->GetIntegerField(FieldName);
	}

	int32 GetInteger(const FString& FieldName, const int Default) const {
		if (Has(FieldName)) {
			return JsonObject->GetIntegerField(FieldName);
		}
		
		return Default;
	}
	
	void SetInteger(const FString& FieldName, const int32& NewValue) const {
		JsonObject->SetNumberField(FieldName, NewValue);
	}

	bool GetBool(const FString& FieldName) const {
		return JsonObject->GetBoolField(FieldName);
	}

	bool GetBool(const FString& FieldName, const bool Default) const {
		if (Has(FieldName)) {
			return JsonObject->GetBoolField(FieldName);
		}
		
		return Default;
	}

	void SetBool(const FString& FieldName, const bool& NewValue) const {
		JsonObject->SetBoolField(FieldName, NewValue);
	}

	double GetNumber(const FString& FieldName) const {
		return JsonObject->GetNumberField(FieldName);
	}
		
	void SetNumber(const FString& FieldName, const double& NewValue) const {
		JsonObject->SetNumberField(FieldName, NewValue);
	}

	FUObjectJsonValueExport GetObject(const FString& FieldName) const {
		if (!JsonObject->HasField(FieldName)) {
			static const TSharedPtr<FJsonObject> EmptyObject = MakeShared<FJsonObject>();
			return EmptyObject;
		}

		/* TMap::FindByHash, which saves rehashing the key HasField already hashed, came later */
#if UE4_24_BELOW
		const TSharedPtr<FJsonValue>* Field = JsonObject->Values.Find(FieldName);
#else
		const TSharedPtr<FJsonValue>* Field = JsonObject->Values.FindByHash(GetTypeHash(FieldName), FieldName);
#endif
		if (Field == nullptr || !Field->IsValid()) {
			static const TSharedPtr<FJsonObject> EmptyObject = MakeShared<FJsonObject>();
			return EmptyObject;
		}

		if ((*Field)->Type != EJson::Object) {
			static const TSharedPtr<FJsonObject> EmptyObject = MakeShared<FJsonObject>();
			return EmptyObject;
		}
		
		return JsonObject->GetObjectField(FieldName);
	}
	
	void SetObject(const FString& FieldName, const TSharedPtr<FJsonObject>& NewValue) const {
		JsonObject->SetObjectField(FieldName, NewValue);
	}

	void SetObject(const FString& FieldName, const FUObjectJsonValueExport& NewValue) const {
    	JsonObject->SetObjectField(FieldName, NewValue.JsonObject);
    }

	void Remove(const FString& FieldName) const {
		JsonObject->RemoveField(FieldName);
	}

	TArray<FUObjectJsonValueExport> GetArray(const FString& FieldName) const {
		TArray<FUObjectJsonValueExport> Result;

		for (const TSharedPtr<FJsonValue>& ArrayValue : JsonObject->GetArrayField(FieldName)) {
			Result.Add(ArrayValue);
		}

		return Result;
	}

	void SetArray(const FString& FieldName, const TArray<FUObjectJsonValueExport>& NewValue) const {
		TArray<TSharedPtr<FJsonValue>> RawArray;
		RawArray.Reserve(NewValue.Num());

		for (const FUObjectJsonValueExport& Item : NewValue) {
			RawArray.Add(Item.Value);
		}

		JsonObject->SetArrayField(FieldName, RawArray);
	}

	void Set(const FString& FieldName, const TSharedPtr<FJsonValue>& NewValue) const {
		JsonObject->SetField(FieldName, NewValue);
	}

	bool Has(const FString& FieldName) const {
		return JsonObject->HasField(FieldName);
	}
};