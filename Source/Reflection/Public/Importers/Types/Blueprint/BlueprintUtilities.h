/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "Settings/ReflectionSettings.h"
#include "Settings/Runtime.h"
#include "Engine/EngineUtilities.h"
#include "Containers/ExportContainer.h"
#include "Modules/Toolbar/Tools/ImportFromPath.h"

/* 4.25 and below build this module without the engine's shared PCH (see Reflection.Build.cs),
 * which is where the blueprint function library type used to come in from */
#if UE4_25_BELOW
#include "Kismet/BlueprintFunctionLibrary.h"
#endif

/* What a write into a struct member names, said either way round.
 *
 * A build spells the member as a way down to it and the thing it is in as a property of a property;
 * an older one spells the member as a reference to the property itself and the thing it is in as a
 * plain name. Read one way only, the other comes back naming nothing, and a write that names
 * nothing matches nothing. */
/* The struct a property is on and what it is called on it, said either way round.
 *
 * A build says the two apart the way down to the member, and the struct resolved beside it. An
 * older one says them together, as one reference to the property itself: the struct before the
 * colon and the member after it. Read the first way only, the older spelling answers nothing for
 * both, and a member of nothing on a struct of nothing is not something that can be laid out. */
inline void ReadPropertyReference(const FUObjectJsonValueExport& Property, FString& Owner, FString& Member) {
	if (!Property.JsonObject.IsValid()) return;

	if (Member.IsEmpty()) {
		Property.JsonObject->TryGetStringField(TEXT("Name"), Member);
	}

	if (Member.IsEmpty()) {
		if (const TArray<TSharedPtr<FJsonValue>>* Path = nullptr; Property.JsonObject->TryGetArrayField(TEXT("Path"), Path) && Path->Num() > 0) {
			Member = (*Path)[Path->Num() - 1]->AsString();
		}
	}

	if (!Owner.IsEmpty() && !Member.IsEmpty()) return;

	FString Spelled;

	if (!Property.JsonObject->TryGetStringField(TEXT("ObjectName"), Spelled)) return;

	/* FloatProperty'SomeStruct:Member' -> SomeStruct, Member */
	FString Held;

	if (!Spelled.Split(TEXT("'"), nullptr, &Held)) return;

	Held.RemoveFromEnd(TEXT("'"));

	FString Before, After;

	if (!Held.Split(TEXT(":"), &Before, &After)) return;

	if (Owner.IsEmpty()) Owner = Before;
	if (Member.IsEmpty()) Member = After;
}

inline void ReadStructMemberContext(const FUObjectJsonValueExport& Variable, FString& Owner, FString& Member) {
	Owner.Empty();
	Member.Empty();

	const FUObjectJsonValueExport Inside = Variable.GetObject(TEXT("StructExpression")).GetObject(TEXT("Variable"));

	if (Inside.JsonObject.IsValid() && !Inside.JsonObject->TryGetStringField(TEXT("Name"), Owner)) {
		if (const TSharedPtr<FJsonObject> Held = Inside.GetObject(TEXT("Property")).JsonObject; Held.IsValid()) {
			Held->TryGetStringField(TEXT("Name"), Owner);
		}
	}

	const TSharedPtr<FJsonObject> Named = Variable.GetObject(TEXT("Property")).JsonObject;

	if (!Named.IsValid()) return;

	if (const TArray<TSharedPtr<FJsonValue>>* Path = nullptr; Named->TryGetArrayField(TEXT("Path"), Path) && Path->Num() == 1) {
		Member = (*Path)[0]->AsString();

		return;
	}

	/* Spelled as a reference, of which the member is whatever comes after the class it is on */
	if (FString Spelled; Named->TryGetStringField(TEXT("ObjectName"), Spelled)) {
		Spelled.Split(TEXT(":"), nullptr, &Spelled);

		Member = Spelled.Replace(TEXT("'"), TEXT(""));
	}
}

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

	/* Named, since a package on its own answers with the package.
	 *
	 * What arrives here is the path to the asset with nothing after it, and asked for that way the
	 * loader hands back whatever lives at it. Where the asset is not in memory that is nothing and
	 * the load reads the file, which is the blueprint. Where it already is, it is the package the
	 * blueprint sits in, and a package is not a blueprint, so the answer is no class at all.
	 *
	 * It only ever is in memory when something has just brought it in, which is the parent an
	 * import fetched a moment ago. So the asset inside is named here, as anything else asking for
	 * one already does. */
	if (!FullPath.Contains(TEXT("."))) {
		FString Named = FullPath;

		if (Named.Split(TEXT("/"), nullptr, &Named, ESearchCase::CaseSensitive, ESearchDir::FromEnd) && !Named.IsEmpty()) {
			FullPath += TEXT(".") + Named;
		}
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

	if (UClass* Made = LoadBlueprintClass(ObjectPath)) {
		return Made;
	}

	/* The blueprint a class says it comes from, which the project has not got yet.
	 *
	 * A class read without finding its parent is left parented to nothing, and a blueprint in that
	 * state is one the editor can make nothing of: everything it inherited resolves to nothing and
	 * the asset opens broken. The parent is an asset like any other, so it is asked for, and the
	 * class it generates is the one this one comes from.
	 *
	 * It reaches back as far as it needs to. A parent that is itself a blueprint asks for its own
	 * parent as it is read, and the chain ends where it meets a class written in C++, which is
	 * already here and never asked for. */
	if (TToolImportFromPath::Import(ObjectPath)) {
		return LoadBlueprintClass(ObjectPath);
	}

	return nullptr;
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