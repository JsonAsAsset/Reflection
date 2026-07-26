/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "Containers/ExportContainer.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonReader.h"
#include "Engine/EngineUtilities.h"
#include "Settings/ReflectionSettings.h"
#include "Serialization/JsonSerializer.h"

/* Conversion Functions ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
#if ENGINE_UE5
inline FVector3f ObjectToVector3F(const FJsonObject* Object) {
	return FVector3f(Object->GetNumberField(TEXT("X")), Object->GetNumberField(TEXT("Y")), Object->GetNumberField(TEXT("Z")));
}
#else

inline FVector ObjectToVector3F(const FJsonObject* Object) {
	return FVector(Object->GetNumberField(TEXT("X")), Object->GetNumberField(TEXT("Y")), Object->GetNumberField(TEXT("Z")));
}
#endif

inline FLinearColor ObjectToLinearColor(const FJsonObject* Object) {
	return FLinearColor(Object->GetNumberField(TEXT("R")), Object->GetNumberField(TEXT("G")), Object->GetNumberField(TEXT("B")), Object->GetNumberField(TEXT("A")));
}

inline FRichCurveKey ObjectToRichCurveKey(const TSharedPtr<FJsonObject>& Object) {
	const FString InterpMode = Object->GetStringField(TEXT("InterpMode"));
	
	return FRichCurveKey(Object->GetNumberField(TEXT("Time")), Object->GetNumberField(TEXT("Value")), Object->GetNumberField(TEXT("ArriveTangent")), Object->GetNumberField(TEXT("LeaveTangent")), static_cast<ERichCurveInterpMode>(StaticEnum<ERichCurveInterpMode>()->GetValueByNameString(InterpMode)));
}

/* Filter to remove */
inline TSharedPtr<FJsonObject> RemovePropertiesShared(const TSharedPtr<FJsonObject>& Input, const TArray<FString>& RemovedProperties) {
	TSharedPtr<FJsonObject> ClonedJsonObject = MakeShareable(new FJsonObject(*Input));
    
	for (const FString& Property : RemovedProperties) {
		if (ClonedJsonObject->HasField(Property)) {
			ClonedJsonObject->RemoveField(Property);
		}
	}
    
	return ClonedJsonObject;
}

/* Filter to whitelist */
inline TSharedPtr<FJsonObject> KeepPropertiesShared(const TSharedPtr<FJsonObject>& Input, TArray<FString> WhitelistProperties) {
	const TSharedPtr<FJsonObject> RawSharedPtrData = MakeShared<FJsonObject>();

	for (const FString& Property : WhitelistProperties) {
		if (Input->HasField(Property)) {
			RawSharedPtrData->SetField(Property, Input->TryGetField(Property));
		}
	}

	return RawSharedPtrData;
}

/* Simple handler for JsonArray */
inline auto ProcessJsonArrayField(const TSharedPtr<FJsonObject>& ObjectField, const FString& ArrayFieldName, const TFunction<void(const TSharedPtr<FJsonObject>&)>& ProcessObjectFunction) -> void {
	const TArray<TSharedPtr<FJsonValue>>* JsonArray;
	
	if (ObjectField->TryGetArrayField(ArrayFieldName, JsonArray)) {
		for (const auto& JsonValue : *JsonArray) {
			if (const TSharedPtr<FJsonObject> JsonItem = JsonValue->AsObject()) {
				ProcessObjectFunction(JsonItem);
			}
		}
	}
}

inline void ProcessObjects(const TSharedPtr<FJsonObject>& Parent, const TFunction<void(const FString& Name, const TSharedPtr<FJsonObject>& Object)>& ProcessObjectFunction) {
	for (const auto& Pair : Parent->Values) {
		if (!Pair.Value.IsValid()) continue;

		if (Pair.Value->Type == EJson::Object) {
			if (const TSharedPtr<FJsonObject> ChildObject = Pair.Value->AsObject()) {
				ProcessObjectFunction(Pair.Key, ChildObject);
			}
		}
	}
}

inline TSharedPtr<FJsonObject> GetVectorJson(const FVector& Vec) {
	TSharedPtr<FJsonObject> OutVec = MakeShareable(new FJsonObject);
	
	OutVec->SetNumberField(TEXT("X"), Vec.X);
	OutVec->SetNumberField(TEXT("Y"), Vec.Y);
	OutVec->SetNumberField(TEXT("Z"), Vec.Z);
	
	return OutVec;
}

inline TSharedPtr<FJsonObject> GetRotationJson(const FQuat& Quat) {
	TSharedPtr<FJsonObject> OutRot = MakeShareable(new FJsonObject);
	
	OutRot->SetNumberField(TEXT("X"), Quat.X);
	OutRot->SetNumberField(TEXT("Y"), Quat.Y);
	OutRot->SetNumberField(TEXT("Z"), Quat.Z);
	OutRot->SetNumberField(TEXT("W"), Quat.W);
	OutRot->SetBoolField(TEXT("IsNormalized"), true);
	OutRot->SetNumberField(TEXT("Size"), Quat.Size());
	OutRot->SetNumberField(TEXT("SizeSquared"), Quat.SizeSquared());
	
	return OutRot;
}

inline TSharedPtr<FJsonObject> GetTransformJson(const FTransform& Transform) {
	TSharedPtr<FJsonObject> OutTransform = MakeShareable(new FJsonObject);
	
	OutTransform->SetObjectField(TEXT("Rotation"), GetRotationJson(Transform.GetRotation()));
	OutTransform->SetObjectField(TEXT("Translation"), GetVectorJson(Transform.GetTranslation()));
	OutTransform->SetObjectField(TEXT("Scale3D"), GetVectorJson(Transform.GetScale3D()));
	
	return OutTransform;
}

inline FTransform GetTransformFromJson(const TSharedPtr<FJsonObject>& JsonObject) {
	FTransform OutTransform = FTransform::Identity;
	
	if (!JsonObject.IsValid()) {
		return OutTransform;
	}

	if (JsonObject->HasField(TEXT("Rotation"))) {
		const TSharedPtr<FJsonObject> QuatObject = JsonObject->GetObjectField(TEXT("Rotation"));
		
		FQuat Quat;
		Quat.X = QuatObject->GetNumberField(TEXT("X"));
		Quat.Y = QuatObject->GetNumberField(TEXT("Y"));
		Quat.Z = QuatObject->GetNumberField(TEXT("Z"));
		Quat.W = QuatObject->GetNumberField(TEXT("W"));
		
		OutTransform.SetRotation(Quat);
	}

	if (JsonObject->HasField(TEXT("Translation"))) {
		const TSharedPtr<FJsonObject> TranslationObject = JsonObject->GetObjectField(TEXT("Translation"));
		FVector Translation;
		
		Translation.X = TranslationObject->GetNumberField(TEXT("X"));
		Translation.Y = TranslationObject->GetNumberField(TEXT("Y"));
		Translation.Z = TranslationObject->GetNumberField(TEXT("Z"));
		
		OutTransform.SetTranslation(Translation);
	}

	if (JsonObject->HasField(TEXT("Scale3D"))) {
		const TSharedPtr<FJsonObject> ScaleObj = JsonObject->GetObjectField(TEXT("Scale3D"));
		FVector Scale;
		
		Scale.X = ScaleObj->GetNumberField(TEXT("X"));
		Scale.Y = ScaleObj->GetNumberField(TEXT("Y"));
		Scale.Z = ScaleObj->GetNumberField(TEXT("Z"));
		
		OutTransform.SetScale3D(Scale);
	}

	return OutTransform;
}

inline FJsonObject* EnsureObjectField(FJsonObject* Parent, const FString& FieldName) {
	if (!Parent->HasField(FieldName)) {
		Parent->SetObjectField(FieldName, MakeShareable(new FJsonObject()));
	}

	return Parent->GetObjectField(FieldName).Get();
}

inline FJsonObject* EnsureObjectField(const TSharedPtr<FJsonObject>& Parent, const FString& FieldName) {
	if (!Parent->HasField(FieldName)) {
		Parent->SetObjectField(FieldName, MakeShareable(new FJsonObject()));
	}

	return Parent->GetObjectField(FieldName).Get();
}

static void CollectObjectPackagesRecursively(const TSharedPtr<FJsonValue>& Value, FUObjectExportContainer* Container, TArray<FUObjectExport*>& Exports) {
	if (!Value.IsValid()) {
		return;
	}

	if (Value->Type == EJson::Object) {
		const TSharedPtr<FJsonObject>& Object = Value->AsObject();
		if (!Object.IsValid()) {
			return;
		}

		/* If it has ObjectName + ObjectPath, then resolve */
		if (Object->HasField(TEXT("ObjectName")) && Object->HasField(TEXT("ObjectPath"))) {
			FUObjectExport* Resolved = Container->GetExportByObjectPath(Object);
			
			Exports.Add(Resolved);
		}

		/* Recurse through fields */
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Object->Values) {
			CollectObjectPackagesRecursively(Pair.Value, Container, Exports);
		}
	}
	else if (Value->Type == EJson::Array) {
		for (const TSharedPtr<FJsonValue>& ArrayValue : Value->AsArray()) {
			CollectObjectPackagesRecursively(ArrayValue, Container, Exports);
		}
	}
}

inline TArray<FUObjectExport*> CollectObjectPackages(FUObjectExport* Export, FUObjectExportContainer* Container) {
	TArray<FUObjectExport*> Exports;

	if (!Export->IsJsonValid()) {
		return Exports;
	}

	CollectObjectPackagesRecursively(
		MakeShared<FJsonValueObject>(Export->JsonObject),
		Container,
		Exports
	);

	return Exports;
}

inline bool IsProperExportData(const TSharedPtr<FJsonObject>& JsonObject) {
	/* Property checks */
	if (!JsonObject.IsValid() ||
		!JsonObject->HasField(TEXT("Type")) ||
		!JsonObject->HasField(TEXT("Name")) ||
		!JsonObject->HasField(TEXT("Properties"))
	) return false;

	return true;
}

inline bool DeserializeJSON(const FString& FilePath, TArray<TSharedPtr<FJsonValue>>& JsonParsed) {
	if (FPaths::FileExists(FilePath)) {
		FString ContentBefore;
		
		if (FFileHelper::LoadFileToString(ContentBefore, *FilePath)) {
			FString Content = FString(TEXT("{\"data\": "));
			Content.Append(ContentBefore);
			Content.Append(FString("}"));

			const TSharedRef<TJsonReader<>> JsonReader = TJsonReaderFactory<>::Create(Content);

			TSharedPtr<FJsonObject> JsonObject;
			if (FJsonSerializer::Deserialize(JsonReader, JsonObject)) {
				JsonParsed = JsonObject->GetArrayField(TEXT("data"));
			
				return true;
			}
		}
	}

	return false;
}

inline bool DeserializeJSONObject(const FString& String, TSharedPtr<FJsonObject>& JsonParsed) {
	FString Content = FString(TEXT("{\"data\": "));
	Content.Append(String);
	Content.Append(FString("}"));

	const TSharedRef<TJsonReader<>> JsonReader = TJsonReaderFactory<>::Create(Content);

	TSharedPtr<FJsonObject> JsonObject;
	if (FJsonSerializer::Deserialize(JsonReader, JsonObject)) {
		JsonParsed = JsonObject->GetObjectField(TEXT("data"));
	
		return true;
	}

	return false;
}

inline TSharedPtr<FJsonObject> GetExportStartingWith(const FString& Start, const FString& Property, TArray<TSharedPtr<FJsonValue>> JsonObjects, const bool ExportProperties = false) {
	for (const TSharedPtr<FJsonValue>& JsonObjectValue : JsonObjects) {
		if (JsonObjectValue->Type == EJson::Object) {
			TSharedPtr<FJsonObject> JsonObject = JsonObjectValue->AsObject();

			if (JsonObject.IsValid() && JsonObject->HasField(Property)) {
				const FString StringValue = JsonObject->GetStringField(Property);

				/* Check if the "Type" field starts with the specified string */
				if (StringValue.StartsWith(Start)) {
					if (ExportProperties) {
						if (JsonObject->HasField(TEXT("Properties"))) {
							return JsonObject->GetObjectField(TEXT("Properties"));
						}
					}
					return JsonObject;
				}
			}
		}
	}

	return TSharedPtr<FJsonObject>();
}

inline TSharedPtr<FJsonObject> GetExportMatchingWith(const FString& Match, const FString& Property, TArray<TSharedPtr<FJsonValue>> JsonObjects, const bool ExportProperties = false) {
	for (const TSharedPtr<FJsonValue>& JsonObjectValue : JsonObjects) {
		if (JsonObjectValue->Type == EJson::Object) {
			TSharedPtr<FJsonObject> JsonObject = JsonObjectValue->AsObject();

			if (JsonObject.IsValid() && JsonObject->HasField(Property)) {
				const FString StringValue = JsonObject->GetStringField(Property);

				/* Check if the "Name" field starts with the specified string */
				if (StringValue.Equals(Match)) {
					if (ExportProperties) {
						if (JsonObject->HasField(TEXT("Properties"))) {
							return JsonObject->GetObjectField(TEXT("Properties"));
						}
					}
					return JsonObject;
				}
			}
		}
	}

	return TSharedPtr<FJsonObject>();
}

inline TSharedPtr<FJsonObject> RequestObjectURL(const FString& URL) {
	FHttpModule* HttpModule = &FHttpModule::Get();

	const auto Request = HttpModule->CreateRequest();
			
	Request->SetURL(URL);
	Request->SetVerb(TEXT("GET"));

	const auto Response = FRemoteUtilities::ExecuteRequestSync(Request);
	if (!Response.IsValid()) return TSharedPtr<FJsonObject>();

	TSharedPtr<FJsonObject> DeserializedJSON;

	if (!DeserializeJSONObject(Response->GetContentAsString(), DeserializedJSON)) return TSharedPtr<FJsonObject>();
	return DeserializedJSON;
};

inline FName GetExportNameOfSubobject(const FString& PackageIndex) {
	FString Name; {
		PackageIndex.Split("'", nullptr, &Name);
		Name.Split(":", nullptr, &Name);
		Name = Name.Replace(TEXT("'"), TEXT(""));
	}
	
	return FName(Name);
}

template<typename K, typename V>
FORCEINLINE bool IsEmpty(const TMap<K, V>& Map) {
	return Map.Num() == 0;
}