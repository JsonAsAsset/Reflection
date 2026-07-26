/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/UserDefined/UserDefinedEnumImporter.h"

#include "Engine/UserDefinedEnum.h"
#include "Kismet2/EnumEditorUtils.h"

UObject* IUserDefinedEnumImporter::CreateAsset(UObject* CreatedAsset) {
	return IImporter::CreateAsset(NewObject<UUserDefinedEnum>(GetPackage(), *GetAssetName(), RF_Public | RF_Standalone));
}

bool IUserDefinedEnumImporter::Import() {
	/* Create the UserDefinedEnum */
	UUserDefinedEnum* UserDefinedEnum = Create<UUserDefinedEnum>();
	
	UserDefinedEnum->SetMetaData(TEXT("BlueprintType"), TEXT("true"));
	UserDefinedEnum->Modify();

	/* CppForm ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
	UEnum::ECppForm CppForm = UEnum::ECppForm::Regular;

	if (GetAssetData()->HasField(TEXT("CppForm"))) {
		const FString CppForm_String = GetAssetData()->GetStringField(TEXT("CppForm"));

		/*
		 * Selector based on text
		 * Seems like we can't use the normal EnumAsString because of some access error
		 */
		CppForm = CppForm_String == "Regular" ? UEnum::ECppForm::Regular : CppForm_String == "Namespaced" ? UEnum::ECppForm::Namespaced : UEnum::ECppForm::EnumClass;
	}

	/* EnumNames ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
	if (GetAssetData()->HasTypedField<EJson::Object>(TEXT("Names"))) {
		const TSharedPtr<FJsonObject> Names = GetAssetData()->GetObjectField(TEXT("Names"));

		/* Final EnumNames variable */
		TArray<TPair<FName, int64>> EnumNames;

		int32 EntryCount = Names->Values.Num();
		
		for (const auto& Pair : Names->Values) {
			/* Last entry is the _MAX name, automatically created by the engine */
			if (--EntryCount == 0) break;

			EnumNames.Emplace(FName(*Pair.Key), static_cast<int64>(Pair.Value->AsNumber()));
		}

		/* Update the enumeration with the enum names */
		UserDefinedEnum->SetEnums(EnumNames, CppForm
			#if ENGINE_UE5 || ((ENGINE_UE4 && ENGINE_MINOR_VERSION >= 26) && !(ENGINE_MINOR_VERSION == 26 && ENGINE_PATCH_VERSION == 0))
			, EEnumFlags::None, true
			#endif
		);
	}
	
	/* DisplayNameMap ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
	TArray<TSharedPtr<FJsonValue>> DisplayNameMap = GetAssetData()->GetArrayField(TEXT("DisplayNameMap"));
	TMap<FName, FText> DisplayNames;

	for (const TSharedPtr<FJsonValue>& DisplayEntry : DisplayNameMap) {
		if (!DisplayEntry.IsValid()) continue;

		TSharedPtr<FJsonObject> EntryObject = DisplayEntry->AsObject(); {
			if (!EntryObject.IsValid()) continue;
		}

		TSharedPtr<FJsonObject> ValueObject = EntryObject->GetObjectField(TEXT("Value")); {
			if (!ValueObject.IsValid()) continue;
		}

		/* Retrieve properties */
		FName EnumKey = *EntryObject->GetStringField(TEXT("Key"));
		FString TextNamespace = ValueObject->GetStringField(TEXT("Namespace"));
		FString UniqueKey = ValueObject->GetStringField(TEXT("Key"));
		FString SourceString = ValueObject->GetStringField(TEXT("SourceString"));

		if (ValueObject->HasField(TEXT("CultureInvariantString"))) {
			DisplayNames.Add(EnumKey, FText::FromString(*ValueObject->GetStringField(TEXT("CultureInvariantString"))));
		} else {
			/* TODO: Add LocalizedString */
			DisplayNames.Add(EnumKey, FInternationalization::ForUseOnlyByLocMacroAndGraphNodeTextLiterals_CreateText(*SourceString, *TextNamespace, *UniqueKey));
		}
	}

	/* Set Display Names in the UserDefinedEnum */
	for (const auto& Pair : DisplayNames) {
		UserDefinedEnum->DisplayNameMap.Add(Pair.Key, Pair.Value);
	}

	/* Finalization ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
	UserDefinedEnum->Modify();
	UserDefinedEnum->PostEditChange();

	FEnumEditorUtils::EnsureAllDisplayNamesExist(UserDefinedEnum);

	/* Handle edit changes, and add it to the content browser */
	return OnAssetCreation(UserDefinedEnum);
}
