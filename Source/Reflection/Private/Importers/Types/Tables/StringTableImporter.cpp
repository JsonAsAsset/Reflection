/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/Tables/StringTableImporter.h"

#include "Internationalization/StringTable.h"
#include "Internationalization/StringTableCore.h"

UObject* IStringTableImporter::CreateAsset(UObject* CreatedAsset) {
	return IImporter::CreateAsset(NewObject<UStringTable>(GetPackage(), UStringTable::StaticClass(), *GetAssetName(), RF_Public | RF_Standalone));
}

bool IStringTableImporter::Import() {
	/* Create StringTable */
	UStringTable* StringTable = Create<UStringTable>();

	if (GetAssetData()->HasField(TEXT("StringTable"))) {
		const TSharedPtr<FJsonObject> StringTableData = GetAssetData()->GetObjectField(TEXT("StringTable"));
		const FStringTableRef MutableStringTable = StringTable->GetMutableStringTable();

		/* Set Table Namespace */
		MutableStringTable->SetNamespace(StringTableData->GetStringField(TEXT("TableNamespace")));

		/* Set "SourceStrings" from KeysToEntries */
		const TSharedPtr<FJsonObject> KeysToEntries = StringTableData->GetObjectField(TEXT("KeysToEntries"));
	
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : KeysToEntries->Values) {
			FString Key = Pair.Key;
			FString SourceString = Pair.Value->AsString();

			MutableStringTable->SetSourceString(Key, SourceString);
		}

		/* Set Metadata from KeysToMetaData */
		const TSharedPtr<FJsonObject> KeysToMetaData = StringTableData->GetObjectField(TEXT("KeysToMetaData"));

		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : KeysToMetaData->Values) {
			const FString TableKey = Pair.Key;
			const TSharedPtr<FJsonObject> MetadataObject = Pair.Value->AsObject();

			for (const TPair<FString, TSharedPtr<FJsonValue>>& MetadataPair : MetadataObject->Values) {
				const FName TextKey = *MetadataPair.Key;
				FString MetadataValue = MetadataPair.Value->AsString();
			
				MutableStringTable->SetMetaData(TableKey, TextKey, MetadataValue);
			}
		}
	}

	/* Handle edit changes, and add it to the content browser */
	return OnAssetCreation(StringTable);
}
