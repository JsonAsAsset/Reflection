/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "KismetCompilerModule.h"
#include "Settings/ReflectionSettings.h"
#include "Engine/Compatibility.h"
#include "Engine/EngineUtilities.h"
#include "Containers/ExportContainer.h"

inline TSubclassOf<UObject> LoadClassFromPath(const FString& ObjectName, const FString& ObjectPath) {
	const FString FullPath = ObjectPath + TEXT(".") + ObjectName;

	if (UObject* LoadedObject = StaticLoadObject(UObject::StaticClass(), nullptr, *FullPath)) {
		if (UClass* LoadedClass = Cast<UClass>(LoadedObject)) {
			return LoadedClass;
		}
	}

	return nullptr;
}

inline TSubclassOf<UObject> LoadBlueprintClass(FString& ObjectPath) {
	const UReflectionSettings* Settings = GetSettings();
	
	if (!Settings->AssetSettings.ProjectName.IsEmpty()) {
		ObjectPath = ObjectPath.Replace(*(Settings->AssetSettings.ProjectName + "/Content"), TEXT("/Game"));
	}
	
	FString FullPath = ObjectPath; 
	if (FullPath.EndsWith(TEXT(".1"))) {
		FullPath = FullPath.LeftChop(2);
	}

	if (UObject* LoadedObject = StaticLoadObject(UObject::StaticClass(), nullptr, *FullPath)) {
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

inline TSharedPtr<FJsonObject> GetSuperStructJsonObject(const TSharedPtr<FJsonObject>& JsonObject) {
	if (JsonObject->HasField(TEXT("Next"))) {
		return JsonObject->GetObjectField(TEXT("Next"));
	}
	
	return JsonObject->GetObjectField(TEXT("SuperStruct"));
}

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

/*
 * Resolves the UBlueprint / UBlueprintGeneratedClass pair a cooked "<X>GeneratedClass" export belongs to.
 *
 * The Kismet compiler only answers for parent classes that were registered with it, and plenty of blueprint
 * families never register (Control Rig being one), so asking it alone hands back a plain UBlueprint and the
 * asset gets rebuilt as the wrong type. The export names its own class though, and the engine is consistent
 * about calling a generated class "<BlueprintClass>GeneratedClass", so the blueprint type can be looked up by
 * name and it then reports which generated class it expects. Nothing here is per-asset-type.
 */
inline void GetBlueprintTypesForExport(const FString& GeneratedClassType, UClass* ParentClass, UClass*& OutBlueprintClass, UClass*& OutGeneratedClass) {
	OutBlueprintClass = nullptr;
	OutGeneratedClass = nullptr;

	FModuleManager::LoadModuleChecked<IKismetCompilerInterface>("KismetCompiler")
		.GetBlueprintTypesForClass(ParentClass, OutBlueprintClass, OutGeneratedClass);

	/* The compiler recognized the parent class, it knows better than a name lookup would */
	if (OutBlueprintClass != nullptr && OutBlueprintClass != UBlueprint::StaticClass()) {
		return;
	}

	if (!IsGeneratedClassType(GeneratedClassType)) {
		return;
	}

	UClass* BlueprintClass = FindClassByType(GeneratedClassType.LeftChop(GeneratedClassSuffix.Len()));

	if (BlueprintClass == nullptr
		|| !BlueprintClass->IsChildOf(UBlueprint::StaticClass())
		|| BlueprintClass->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists)) {
		return;
	}

	const UBlueprint* BlueprintCDO = GetDefault<UBlueprint>(BlueprintClass);
	UClass* GeneratedClass = BlueprintCDO ? BlueprintCDO->GetBlueprintClass() : nullptr;

	if (GeneratedClass == nullptr || !GeneratedClass->IsChildOf(UBlueprintGeneratedClass::StaticClass())) {
		return;
	}

	OutBlueprintClass = BlueprintClass;
	OutGeneratedClass = GeneratedClass;
}

inline FUObjectExport* GetClassDefaultObject(FUObjectExportContainer* AssetContainer, const FUObjectJsonValueExport& JsonObject) {
	FUObjectExport* Export = AssetContainer->GetExportByObjectPath(JsonObject.GetObject("ClassDefaultObject"));
	if (!Export->IsJsonValid()) {
		Export = AssetContainer->GetExportStartingWith("Name", "Default__");
	}

	return Export;
}