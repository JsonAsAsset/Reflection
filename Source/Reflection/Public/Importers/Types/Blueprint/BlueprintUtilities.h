/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "Settings/ReflectionSettings.h"
#include "Settings/Runtime.h"
#include "Engine/EngineUtilities.h"
#include "Containers/ExportContainer.h"

/* 4.25 and below build this module without the engine's shared PCH (see Reflection.Build.cs),
 * which is where the blueprint function library type used to come in from */
#if UE4_25_BELOW
#include "Kismet/BlueprintFunctionLibrary.h"
#endif

inline TSubclassOf<UObject> LoadClassFromPath(const FString& ObjectName, const FString& ObjectPath) {
	const FString FullPath = ObjectPath + TEXT(".") + ObjectName;

	if (UObject* LoadedObject = LoadObjectByPath<UObject>(FullPath)) {
		if (UClass* LoadedClass = Cast<UClass>(LoadedObject)) {
			return LoadedClass;
		}
	}

	return nullptr;
}

inline TSubclassOf<UObject> LoadBlueprintClass(FString& ObjectPath) {
	const FString& ProjectName = GReflectionRuntime.Profile.ProjectName;

	if (!ProjectName.IsEmpty()) {
		ObjectPath = ObjectPath.Replace(*(ProjectName + "/Content"), TEXT("/Game"));
	}
	
	FString FullPath = ObjectPath; 
	if (FullPath.EndsWith(TEXT(".1"))) {
		FullPath = FullPath.LeftChop(2);
	}

	if (UObject* LoadedObject = LoadObjectByPath<UObject>(FullPath)) {
		const UBlueprint* LoadedBlueprint = Cast<UBlueprint>(LoadedObject);
		
		if (LoadedBlueprint && LoadedBlueprint->GeneratedClass) {
			return LoadedBlueprint->GeneratedClass;
		}
	}

	return nullptr;
}

inline UClass* LoadClass(const TSharedPtr<FJsonObject>& SuperStruct) {
	const FString ObjectName = SuperStruct->GetStringField(TEXT("ObjectName")).Replace(TEXT("Class'"), TEXT("")).Replace(TEXT("'"), TEXT(""));
	FString ObjectPath = SuperStruct->GetStringField(TEXT("ObjectPath"));

	/* It's a C++ class if it has Script in it */
	if (ObjectPath.Contains("/Script/")) {
		return LoadClassFromPath(ObjectName, ObjectPath);
	}
	
	ObjectPath.Split(".", &ObjectPath, nullptr);

	return LoadBlueprintClass(ObjectPath);
}

/* What a class or a struct says it comes from, under whichever name it is written as.
 *
 * A struct carries Next, a class made from C++ carries SuperStruct, and one made from another
 * blueprint carries Super. All three name the same thing, and a class read without finding it is
 * left parented to nothing at all, which is not something the compiler survives. */
inline TSharedPtr<FJsonObject> GetSuperStructJsonObject(const TSharedPtr<FJsonObject>& JsonObject) {
	if (!JsonObject.IsValid()) return nullptr;

	for (const TCHAR* Named : { TEXT("Next"), TEXT("SuperStruct"), TEXT("Super") }) {
		if (const TSharedPtr<FJsonObject>* Found = nullptr; JsonObject->TryGetObjectField(Named, Found)) {
			return *Found;
		}
	}

	return nullptr;
}

/* The kind of blueprint an asset was, as the asset itself says.
 *
 * A cooked class keeps what the editor knew it as in its tags, and that is the only place some of
 * them are said at all: an animation layer interface is a class like any other with an ordinary
 * parent, and read off the parent it comes back as a normal blueprint with its layers turned into
 * functions nobody can call. Where nothing says, the parent is still the best that can be done. */
inline EBlueprintType GetBlueprintTypeSaid(const TSharedPtr<FJsonObject>& AssetData, const UClass* Class);

inline EBlueprintType GetBlueprintType(const UClass* Class) {
	EBlueprintType BlueprintType = BPTYPE_Normal;

	if (Class->HasAnyClassFlags(CLASS_Const)) {
		BlueprintType = BPTYPE_Const;
	}
	
	if (Class == UBlueprintFunctionLibrary::StaticClass()) {
		BlueprintType = BPTYPE_FunctionLibrary;
	}
	
	if (Class == UInterface::StaticClass()) {
		BlueprintType = BPTYPE_Interface;
	}
	
	return BlueprintType;
}

inline EBlueprintType GetBlueprintTypeSaid(const TSharedPtr<FJsonObject>& AssetData, const UClass* Class) {
	const TSharedPtr<FJsonObject>* Tags = nullptr;

	if (AssetData.IsValid() && AssetData->TryGetObjectField(TEXT("EditorTags"), Tags)) {
		FString Says;

		if ((*Tags)->TryGetStringField(TEXT("BlueprintType"), Says) && !Says.IsEmpty()) {
			/* Spelled the way the engine spells it, so the engine is asked what it means */
			if (const UEnum* Kinds = StaticEnum<EBlueprintType>()) {
				const int64 Which = Kinds->GetValueByNameString(Says);

				if (Which != INDEX_NONE) return static_cast<EBlueprintType>(Which);
			}
		}
	}

	return GetBlueprintType(Class);
}

inline FUObjectExport* GetClassDefaultObject(FUObjectExportContainer* AssetContainer, const FUObjectJsonValueExport& JsonObject) {
	FUObjectExport* Export = AssetContainer->GetExportByObjectPath(JsonObject.GetObject("ClassDefaultObject"));
	if (!Export->IsJsonValid()) {
		Export = AssetContainer->GetExportStartingWith("Name", "Default__");
	}

	return Export;
}