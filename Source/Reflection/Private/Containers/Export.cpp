/* Copyright Reflection Contributors 2024-2026 */

#include "Containers/Export.h"
#include "Engine/EngineUtilities.h"
#include "Settings/ReflectionSettings.h"
#include "Utilities/BlueprintUtilities.h"

FString ReadPathFromObject(const FUObjectJsonValueExport& PackageIndex) {
	FString ObjectType, ObjectName, ObjectPath, Outer;
	PackageIndex.GetString("ObjectName").Split("'", &ObjectType, &ObjectName);

	ObjectPath = PackageIndex.GetString("ObjectPath");
	ObjectPath.Split(".", &ObjectPath, nullptr);

	const UReflectionSettings* Settings = GetSettings();

	if (!Settings->AssetSettings.ProjectName.IsEmpty()) {
		ObjectPath = ObjectPath.Replace(*(Settings->AssetSettings.ProjectName + "/Content"), TEXT("/Game"));
	}

	ObjectPath = ObjectPath.Replace(TEXT("Engine/Content"), TEXT("/Engine"));
	ObjectName = ObjectName.Replace(TEXT("'"), TEXT(""));

	if (ObjectName.Contains(".")) {
		ObjectName.Split(".", nullptr, &ObjectName);
	}

	if (ObjectName.Contains(".")) {
		ObjectName.Split(".", &Outer, &ObjectName);
	}

	return ObjectPath + "." + ObjectName;
}

UClass* FUObjectExport::GetClass() {
	if (Class) return Class;
	
	FString ClassName = GetString("Class");

	if (Has("Template")) {
		ClassName = ReadPathFromObject(GetObject("Template")).Replace(TEXT("Default__"), TEXT(""));
	}

	if (ClassName.Contains("'")) {
		ClassName.Split("'", nullptr, &ClassName, ESearchCase::IgnoreCase, ESearchDir::FromStart);
		ClassName.Split("'", &ClassName, nullptr, ESearchCase::IgnoreCase, ESearchDir::FromStart);
	}

	UClass* OutClass = FindClassByType(ClassName);
	if (!OutClass) {
		OutClass = FindClassByType(GetType().ToString());
	}

	if (Has("Next") || Has("SuperStruct")) {
		OutClass = LoadClass(GetSuperStructJsonObject(GetProperties()));
	}

	if (!OutClass) return nullptr;

	Class = OutClass;
	return Class;
}
