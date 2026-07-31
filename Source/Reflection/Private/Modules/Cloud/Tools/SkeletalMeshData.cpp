/* Copyright Reflection Contributors 2024-2026 */

#include "Modules/Cloud/Tools/SkeletalMeshData.h"

#include "Engine/EngineUtilities.h"

/* The cloth runtime was a single ClothingSystemRuntime module until 4.26 broke it apart, and
 * neither of these headers exists before that. Everything below that touches clothing is
 * already 4.27 and up only, so there is nothing to include in their place. */
#if !UE4_25_BELOW
#include "ClothingAssetBase.h"
#endif

#include "Dom/JsonObject.h"
#include "Animation/AnimSequence.h"

#if ENGINE_UE5 && ENGINE_MINOR_VERSION >= 3
#include "ClothingAsset.h"
#include "ClothLODData.h"
#elif !UE4_25_BELOW
#include "ClothingSystemRuntimeCommon/Public/ClothingAsset.h"
#endif

#include "Engine/SkeletalMeshSocket.h"

#if ENGINE_UE5
#include "Engine/SkinnedAssetCommon.h"
#endif

#include "EditorFramework/AssetImportData.h"
#include "Importers/Constructor/Importer.h"
#include "Utilities/JsonHelpers.h"

#if ENGINE_UE5
#include "Animation/AnimData/IAnimationDataController.h"
#if ENGINE_MINOR_VERSION >= 4
#include "Animation/AnimData/IAnimationDataModel.h"
#endif
#include "AnimDataController.h"
#endif

class UClothingAssetCommon;

void TSkeletalMeshData::Process(UObject* Object, const TArray<TSharedPtr<FJsonValue>>& Exports) {
	USkeletalMesh* SkeletalMesh = Cast<USkeletalMesh>(Object);
	if (SkeletalMesh == nullptr) return;

	for (const TSharedPtr<FJsonValue>& Export : Exports) {
		if (!Export.IsValid() || !Export->AsObject().IsValid()) {
			continue;
		}

		const TSharedPtr<FJsonObject> JsonObject = Export->AsObject();
		if (!IsProperExportData(JsonObject)) continue;

		TSharedPtr<FJsonObject> Properties = JsonObject->GetObjectField(TEXT("Properties"));
		const FString Type = JsonObject->GetStringField(TEXT("Type"));
		const FString Name = JsonObject->GetStringField(TEXT("Name"));

		if (Name != Object->GetName()) continue;

		if (Type == "SkeletalMesh") {
			bool UseClothingAssets = false;

#if UE4_27
			TArray<UClothingAssetBase*> ClothingAssets = SkeletalMesh->GetMeshClothingAssets();

			if (UseClothingAssets) {
				/* Empty all Clothing Assets */
				for (UClothingAssetBase* ClothingAsset : ClothingAssets) {
					ClothingAsset->Modify();
					ClothingAsset->UnbindFromSkeletalMesh(SkeletalMesh, 0);
					SkeletalMesh->GetMeshClothingAssets().Remove(ClothingAsset);
				}
			}
#endif

			TArray<TSharedPtr<FJsonValue>> SkeletalMaterials = JsonObject->GetArrayField(TEXT("SkeletalMaterials"));

			int SkeletalMaterialIndex = 0;
			
			for (const TSharedPtr<FJsonValue>& SkeletalMaterialExport : SkeletalMaterials) {
				if (!SkeletalMaterialExport.IsValid() || !SkeletalMaterialExport->AsObject().IsValid()) {
					continue;
				}

				const TSharedPtr<FJsonObject> SkeletalMaterialObject = SkeletalMaterialExport->AsObject();

				if (GetMaterials(SkeletalMesh).IsValidIndex(SkeletalMaterialIndex)) {
					FSkeletalMaterial& MaterialSlot = GetMaterials(SkeletalMesh)[SkeletalMaterialIndex];
					
					MaterialSlot.MaterialSlotName = FName(*SkeletalMaterialObject->GetStringField(TEXT("MaterialSlotName")));
					MaterialSlot.ImportedMaterialSlotName = MaterialSlot.MaterialSlotName;

					TSharedPtr<FJsonObject> SkeletalMaterial = SkeletalMaterialObject->GetObjectField(TEXT("Material"));

					IImporter* Importer = new IImporter();
					
					TObjectPtr<UObject> LoadedObject;
					Importer->LoadExport<UObject>(&SkeletalMaterial, LoadedObject);

					if (IsObjectPtrValid(LoadedObject)) MaterialSlot.MaterialInterface = Cast<UMaterialInterface>(LoadedObject.Get());
				} else break;

				SkeletalMaterialIndex++;
			}

			if (Properties->HasField(TEXT("LODInfo"))) {
				TArray<TSharedPtr<FJsonValue>> LODInfo = Properties->GetArrayField(TEXT("LODInfo"));

				for (const TSharedPtr<FJsonValue>& LOD : LODInfo) {
					if (!LOD.IsValid() || !LOD->AsObject().IsValid()) {
						continue;
					}

					const TSharedPtr<FJsonObject> LODObject = LOD->AsObject();
					if (!LODObject->HasField(TEXT("SourceImportFilename"))) continue;

					FString SourceImportFilename = LODObject->GetStringField(TEXT("SourceImportFilename"));
					if (SourceImportFilename.IsEmpty()) continue;

					SetAssetImportData(SkeletalMesh, NewObject<UAssetImportData>(SkeletalMesh, TEXT("AssetImportData")));
					GetAssetImportData(SkeletalMesh)->SourceData.SourceFiles.Add(SourceImportFilename);
				}
			}

			/* Create an object serializer */
			GetObjectSerializer()->ExportsToNotDeserialize.Empty();
			GetObjectSerializer()->SetExportForDeserialization(JsonObject, SkeletalMesh);
			GetObjectSerializer()->Parent = SkeletalMesh;
			
			SkeletalMesh->GetMeshOnlySocketList().Empty();

			FUObjectExportContainer* Container = new FUObjectExportContainer(Exports);
			GetObjectSerializer()->DeserializeExports(Container);

			for (const FUObjectExport* UObjectExport : GetObjectSerializer()->GetPropertySerializer()->ExportsContainer->Exports) {
				if (USkeletalMeshSocket* Socket = Cast<USkeletalMeshSocket>(UObjectExport->Object)) {
					SkeletalMesh->GetMeshOnlySocketList().Add(Socket);
				}
			}

			GetObjectSerializer()->DeserializeObjectProperties(KeepPropertiesShared(Properties, {
				// "MeshClothingAssets"
				"PhysicsAsset",
				"PostProcessAnimBlueprint",
				"ShadowPhysicsAsset",
				"PositiveBoundsExtension",
				"NegativeBoundsExtension",
				"MorphTargets",

				"Sockets",
				"SamplingInfo",
				"LODModels",
				"NaniteResources",
			}), SkeletalMesh);
			
			SkeletalMesh->Modify();
			
			if (UseClothingAssets) {
#if UE4_27
				ClothingAssets = SkeletalMesh->GetMeshClothingAssets();
			
				for (UClothingAssetBase* ClothingAssetBase : ClothingAssets) {
					ClothingAssetBase->Modify();

					if (UClothingAssetCommon* ClothingAsset = Cast<UClothingAssetCommon>(ClothingAssetBase)) {
						for (FClothLODDataCommon& LodData : ClothingAsset->LodData) {
							LodData.PointWeightMaps.Empty();

							for (TMap<uint32, FPointWeightMap>::TConstIterator Iterator(LodData.PhysicalMeshData.WeightMaps); Iterator; ++Iterator) {
								const uint32 Key = Iterator.Key();
								FPointWeightMap PointWeightMap = Iterator.Value();

								PointWeightMap.Name = FName(*FString::FromInt(Key));
								PointWeightMap.CurrentTarget = 1;
								LodData.PointWeightMaps.Add(PointWeightMap);
							}
						}
					}
				}
#endif
			}

			BrowseToWhenFinished(SkeletalMesh);

			/* Notification */
			AppendNotification(
				FText::FromString("Reflected Skeletal Mesh Data: " + SkeletalMesh->GetName()),
				FText::FromString(SkeletalMesh->GetName()),
				3.5f,
				FAppStyle::GetBrush("PhysicsAssetEditor.EnableCollision.Small"),
				SNotificationItem::CS_Success,
				false,
				310.0f
			);
		}
	}
}

TArray<FSkeletalMaterial> TSkeletalMeshData::GetMaterials(USkeletalMesh* Mesh) {
#if UE4_27 || ENGINE_UE5
	return Mesh->GetMaterials();
#else
	return Mesh->Materials;
#endif
}