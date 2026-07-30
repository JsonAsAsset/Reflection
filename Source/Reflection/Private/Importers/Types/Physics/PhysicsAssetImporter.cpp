/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/Physics/PhysicsAssetImporter.h"
#include "Engine/EngineUtilities.h"

#include "PhysicsEngine/PhysicsConstraintTemplate.h"
#include "Settings/Runtime.h"
#include "Utilities/JsonHelpers.h"

UObject* IPhysicsAssetImporter::CreateAsset(UObject* CreatedAsset) {
	return IImporter::CreateAsset(NewObject<UPhysicsAsset>(GetPackage(), UPhysicsAsset::StaticClass(), *GetAssetName(), RF_Public | RF_Standalone));
}

bool IPhysicsAssetImporter::Import() {
	/* CollisionDisableTable is required to port physics assets */
	if (!GetAssetData()->HasField(TEXT("CollisionDisableTable"))) {
		SpawnPrompt("Missing CollisionDisableTable", "The provided physics asset json file is missing the 'CollisionDisableTable' property. This property is required.");

		return false;
	}

	UPhysicsAsset* PhysicsAsset = Create<UPhysicsAsset>();

	DeserializeExports(PhysicsAsset, false);
	FUObjectExportContainer* ExportContainer = GetContainer();

	/* SkeletalBodySetups ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
	const FString SkeletalBodySetupsName = !GReflectionRuntime.IsOlderUE4Target() ? "SkeletalBodySetups" : "BodySetup";

	ProcessJsonArrayField(GetAssetData(), SkeletalBodySetupsName, [&](const TSharedPtr<FJsonObject>& ObjectField) {
		const FName ExportName = GetExportNameOfSubobject(ObjectField->GetStringField(TEXT("ObjectName")));
		const TSharedPtr<FJsonObject> ExportJson = ExportContainer->Find(ExportName)->JsonObject;

		const TSharedPtr<FJsonObject> ExportProperties = ExportJson->GetObjectField(TEXT("Properties"));
		const FName BoneName = FName(*ExportProperties->GetStringField(TEXT("BoneName")));
		
		USkeletalBodySetup* BodySetup = CreateNewBody(PhysicsAsset, ExportName, BoneName);

		GetObjectSerializer()->DeserializeObjectProperties(ExportProperties, BodySetup);
	});

	/* For caching. IMPORTANT! DO NOT REMOVE! */
	PhysicsAsset->UpdateBodySetupIndexMap();
	PhysicsAsset->UpdateBoundsBodiesArray();

	/* CollisionDisableTable ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
	TArray<TSharedPtr<FJsonValue>> CollisionDisableTable = GetAssetData()->GetArrayField(TEXT("CollisionDisableTable"));

	for (const auto& TableJSONElement : CollisionDisableTable) {
		const TSharedPtr<FJsonObject> TableObjectElement = TableJSONElement->AsObject();

		bool MapValue = TableObjectElement->GetBoolField(TEXT("Value"));
		TArray<TSharedPtr<FJsonValue>> Indices = TableObjectElement->GetObjectField(TEXT("Key"))->GetArrayField(TEXT("Indices"));

		const int32 BodyIndexA = Indices[0]->AsNumber();
		const int32 BodyIndexB = Indices[1]->AsNumber();

		/* Add to the CollisionDisableTable */
		PhysicsAsset->CollisionDisableTable.Add(FRigidBodyIndexPair(BodyIndexA, BodyIndexB), MapValue);
	}

	/* ConstraintSetup ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
	ProcessJsonArrayField(GetAssetData(), TEXT("ConstraintSetup"), [&](const TSharedPtr<FJsonObject>& ObjectField) {
		const FName ExportName = GetExportNameOfSubobject(ObjectField->GetStringField(TEXT("ObjectName")));
		const TSharedPtr<FJsonObject> ExportJson = ExportContainer->Find(ExportName)->JsonObject;

		const TSharedPtr<FJsonObject> ExportProperties = ExportJson->GetObjectField(TEXT("Properties"));
		UPhysicsConstraintTemplate* PhysicsConstraintTemplate = CreateNewConstraint(PhysicsAsset, ExportName);
		
		GetObjectSerializer()->DeserializeObjectProperties(ExportProperties, PhysicsConstraintTemplate);

		/* For caching. IMPORTANT! DO NOT REMOVE! */
		PhysicsConstraintTemplate->UpdateProfileInstance();
	});

	/* Simple data at end ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
	GetObjectSerializer()->DeserializeObjectProperties(RemovePropertiesShared(GetAssetData(),
	{
		"SkeletalBodySetups",
		"ConstraintSetup",
		"BoundsBodies",
		"ThumbnailInfo",
		"CollisionDisableTable"
	}), PhysicsAsset);

	/* If the user selected a skeletal mesh in the browser, set it in the physics asset */
	const USkeletalMesh* SkeletalMesh = GetSelectedAsset<USkeletalMesh>(true);

	/* Otherwise, fallback to any skeletal mesh sitting in the same folder as the physics asset */
	if (!SkeletalMesh) {
		const FString SearchPath = FPackageName::GetLongPackagePath(GetPackage()->GetName());

		const FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
		AssetRegistryModule.Get().ScanPathsSynchronous({ SearchPath }, false);

		TArray<FAssetData> AssetDataList;
		AssetRegistryModule.Get().GetAssetsByPath(FName(*SearchPath), AssetDataList, false);

		for (const FAssetData& AssetData : AssetDataList) {
			const UClass* AssetClass = AssetData.GetClass();

			if (!AssetClass || !AssetClass->IsChildOf(USkeletalMesh::StaticClass())) {
				continue;
			}

			SkeletalMesh = Cast<USkeletalMesh>(AssetData.GetAsset());

			if (SkeletalMesh) {
				break;
			}
		}
	}
	
	if (SkeletalMesh) {
		PhysicsAsset->PreviewSkeletalMesh = SkeletalMesh;
		PhysicsAsset->PostEditChange();
	}

	/* Finalize ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
	PhysicsAsset->Modify();
	PhysicsAsset->MarkPackageDirty();
	PhysicsAsset->UpdateBoundsBodiesArray();
	
	return OnAssetCreation(PhysicsAsset);
}

USkeletalBodySetup* IPhysicsAssetImporter::CreateNewBody(UPhysicsAsset* PhysAsset, const FName ExportName, const FName BoneName) {
	USkeletalBodySetup* NewBodySetup = NewObject<USkeletalBodySetup>(PhysAsset, ExportName, RF_Transactional);
	NewBodySetup->BoneName = BoneName;

	PhysAsset->SkeletalBodySetups.Add(NewBodySetup);

	return NewBodySetup;
}

UPhysicsConstraintTemplate* IPhysicsAssetImporter::CreateNewConstraint(UPhysicsAsset* PhysAsset, const FName ExportName) {
	UPhysicsConstraintTemplate* NewConstraintSetup = NewObject<UPhysicsConstraintTemplate>(PhysAsset, ExportName, RF_Transactional);
	PhysAsset->ConstraintSetup.Add(NewConstraintSetup);

	return NewConstraintSetup;
}