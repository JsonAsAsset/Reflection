/* Copyright Reflection Contributors 2024-2026 */

#include "Commandlets/BlueprintAddVariablesCommandlet.h"

#include "Dom/JsonObject.h"
#include "Engine/Blueprint.h"
#include "Importers/Types/Blueprint/BlueprintGraphs.h"
#include "Importers/Types/Blueprint/BlueprintVariables.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Engine/Compatibility.h"
/* 5.0 gathered what a save is told into a header of its own */
#if ENGINE_UE5
#include "UObject/SavePackage.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogBlueprintAddVariables, Display, All);

int32 UBlueprintAddVariablesCommandlet::Main(const FString& Params) {
	FString Path, JsonFile, NameList;
	if (!FParse::Value(*Params, TEXT("path="), Path) || !FParse::Value(*Params, TEXT("json="), JsonFile) || !FParse::Value(*Params, TEXT("names="), NameList, false)) {
		UE_LOG(LogBlueprintAddVariables, Error, TEXT("usage: -run=BlueprintAddVariables -path=/Game/Some/Blueprint -json=<export.json> -names=A,B,C"));
		return 1;
	}

	TArray<FString> Names;
	NameList.ParseIntoArray(Names, TEXT(","), true);

	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *Path);
	if (!Blueprint) {
		UE_LOG(LogBlueprintAddVariables, Error, TEXT("no blueprint at %s"), *Path);
		return 1;
	}

	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *JsonFile)) {
		UE_LOG(LogBlueprintAddVariables, Error, TEXT("could not read %s"), *JsonFile);
		return 1;
	}

	TSharedPtr<FJsonObject> Root;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) || !Root.IsValid()) {
		UE_LOG(LogBlueprintAddVariables, Error, TEXT("%s is not json"), *JsonFile);
		return 1;
	}

	/* The generated class is the export that declares the blueprint's own variables */
	TSharedPtr<FJsonObject> ClassExport;

	for (const TSharedPtr<FJsonValue>& Value : Root->GetArrayField(TEXT("exports"))) {
		const TSharedPtr<FJsonObject> Export = Value->AsObject();

		if (Export.IsValid() && Export->GetStringField(TEXT("Type")) == TEXT("BlueprintGeneratedClass")) {
			ClassExport = Export;
			break;
		}
	}

	if (!ClassExport.IsValid()) {
		UE_LOG(LogBlueprintAddVariables, Error, TEXT("%s has no generated class export"), *JsonFile);
		return 1;
	}

	/* Only what was asked for, so the rest of the blueprint is left as it is on disk */
	TArray<TSharedPtr<FJsonValue>> Wanted;

	for (const TSharedPtr<FJsonValue>& Value : FBlueprintVariables::GetDeclared(ClassExport, nullptr)) {
		const TSharedPtr<FJsonObject> Property = Value->AsObject();

		if (Property.IsValid() && Names.Contains(Property->GetStringField(TEXT("Name")))) {
			Wanted.Add(Value);
		}
	}

	for (const FString& Name : Names) {
		if (!Wanted.ContainsByPredicate([&Name](const TSharedPtr<FJsonValue>& Value) { return Value->AsObject()->GetStringField(TEXT("Name")) == Name; })) {
			UE_LOG(LogBlueprintAddVariables, Warning, TEXT("%s declares no variable %s"), *JsonFile, *Name);
		}
	}

	/* A dispatcher is a variable and a signature graph, which only the graph builder makes. Its parameters come from the
	   signature function's export, so only a dispatcher that hands over nothing can be made from the property alone. */
	int32 Added = 0;

	for (int32 Index = Wanted.Num() - 1; Index >= 0; --Index) {
		const TSharedPtr<FJsonObject> Property = Wanted[Index]->AsObject();

		if (Property->GetStringField(TEXT("Type")) != TEXT("MulticastInlineDelegateProperty")) {
			continue;
		}

		const FString Name = Property->GetStringField(TEXT("Name"));
		const bool bExisted = Blueprint->DelegateSignatureGraphs.ContainsByPredicate([&Name](const UEdGraph* Graph) { return Graph && Graph->GetName() == Name; });

		if (FBlueprintGraphs::MakeDispatcher(Blueprint, Name, TArray<TSharedPtr<FJsonValue>>()) && !bExisted) {
			Added++;
		}

		Wanted.RemoveAt(Index);
	}

	Added += FBlueprintVariables::Construct(Blueprint, Wanted, nullptr);
	UE_LOG(LogBlueprintAddVariables, Display, TEXT("%s: %d of %d variable(s) added"), *Blueprint->GetName(), Added, Names.Num());

	if (Added == 0) {
		return 0;
	}

	FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipGarbageCollection);

	if (Blueprint->Status == BS_Error) {
		UE_LOG(LogBlueprintAddVariables, Error, TEXT("%s did not compile, nothing saved"), *Blueprint->GetName());
		return 1;
	}

	UPackage* Package = Blueprint->GetOutermost();
	const FString FileName = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());

	/* 5.0 gathered what a save is told into one struct. Before that it is told the same
	 * things one argument at a time. */
#if ENGINE_UE5
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;

	if (!UPackage::SavePackage(Package, nullptr, *FileName, SaveArgs)) {
#else
	if (!UPackage::SavePackage(Package, nullptr, RF_Public | RF_Standalone, *FileName)) {
#endif
		UE_LOG(LogBlueprintAddVariables, Error, TEXT("could not save %s"), *FileName);
		return 1;
	}

	UE_LOG(LogBlueprintAddVariables, Display, TEXT("saved %s"), *FileName);
	return 0;
}
