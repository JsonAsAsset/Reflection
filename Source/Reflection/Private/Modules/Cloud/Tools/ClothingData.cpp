/* Copyright Reflection Contributors 2024-2026 */

#include "Modules/Cloud/Tools/ClothingData.h"

#include "Engine/EngineUtilities.h"
#include "Utilities/JsonHelpers.h"

/* Cloth was one ClothingSystemRuntime module until 4.25, and this is 4.27+ only */
#if UE4_27_AND_UE5
#include "ClothingAssetBase.h"

#if UE5_3_BEYOND
#include "ClothingAsset.h"
#include "ClothLODData.h"
#else
#include "ClothingSystemRuntimeCommon/Public/ClothingAsset.h"
#endif

#include "Rendering/SkeletalMeshModel.h"
#include "Rendering/SkeletalMeshLODModel.h"
#include "SkeletalMeshTypes.h"
#endif

void TClothingData::Process(UObject* Object, const TArray<TSharedPtr<FJsonValue>>& Exports) {
#if UE4_27_AND_UE5
	USkeletalMesh* SkeletalMesh = Cast<USkeletalMesh>(Object);
	if (SkeletalMesh == nullptr) return;

	/* Says which cloth the mesh owns, in which order, and which sections they drive */
	TSharedPtr<FJsonObject> MeshExport;

	for (const TSharedPtr<FJsonValue>& Export : Exports) {
		if (!Export.IsValid() || !Export->AsObject().IsValid()) continue;

		const TSharedPtr<FJsonObject> JsonObject = Export->AsObject();
		if (!IsProperExportData(JsonObject)) continue;

		if (JsonObject->GetStringField(TEXT("Type")) != "SkeletalMesh") continue;
		if (JsonObject->GetStringField(TEXT("Name")) != Object->GetName()) continue;

		MeshExport = JsonObject;

		break;
	}

	if (!MeshExport.IsValid()) return;

	const TSharedPtr<FJsonObject> MeshProperties = MeshExport->GetObjectField(TEXT("Properties"));

	if (!MeshProperties->HasField(TEXT("MeshClothingAssets"))) {
		AppendNotification(
			FText::FromString("No Clothing Data: " + SkeletalMesh->GetName()),
			FText::FromString("The mesh was exported without any clothing assets."),
			3.5f,
			SNotificationItem::CS_Fail,
			false,
			310.0f
		);

		return;
	}

	FSkeletalMeshModel* ImportedModel = SkeletalMesh->GetImportedModel();

	/* A cooked mesh has no sections to bind to */
	if (ImportedModel == nullptr || ImportedModel->LODModels.Num() == 0) {
		SpawnPrompt("Missing Imported Model", "'" + SkeletalMesh->GetName() + "' has no imported model, so it has no sections to bind cloth to. Import the mesh itself first, then reflect its clothing.");

		return;
	}

	/* Binding writes a guid into the render sections, so anything already bound goes first */
	for (int32 Index = SkeletalMesh->GetMeshClothingAssets().Num() - 1; Index >= 0; --Index) {
		UClothingAssetBase* ClothingAsset = SkeletalMesh->GetMeshClothingAssets()[Index];
		if (ClothingAsset == nullptr) continue;

		ClothingAsset->Modify();
		ClothingAsset->UnbindFromSkeletalMesh(SkeletalMesh);
	}

	SkeletalMesh->GetMeshClothingAssets().Empty();

	/* Registering the mesh under the name the exports use as their outer is what lands the new
	 * cloth and its configs inside it. The shared sim config is outered to the mesh, not the cloth. */
	GetObjectSerializer()->ExportsToNotDeserialize.Empty();
	GetObjectSerializer()->SetExportForDeserialization(MeshExport, SkeletalMesh);
	GetObjectSerializer()->Parent = SkeletalMesh;

	/* Leaves the morph targets, sockets and curve metadata alone */
	GetObjectSerializer()->WhitelistedTypes = { TEXT("Cloth") };

	FUObjectExportContainer* Container = new FUObjectExportContainer(Exports);
	GetObjectSerializer()->DeserializeExports(Container);

	/* A section names its cloth by index into this list, so the export's order is kept */
	TArray<UClothingAssetCommon*> ClothingAssets;
	TArray<TArray<int32>> SourceLodMaps;

	ProcessJsonArrayField(MeshProperties, TEXT("MeshClothingAssets"), [&](const TSharedPtr<FJsonObject>& Reference) {
		const FName ExportName = GetExportNameOfSubobject(Reference->GetStringField(TEXT("ObjectName")));

		UClothingAssetCommon* ClothingAsset = Cast<UClothingAssetCommon>(Container->Find(ExportName)->Object);

		ClothingAssets.Add(ClothingAsset);
		SourceLodMaps.AddDefaulted();

		if (ClothingAsset == nullptr) {
			UE_LOG(LogReflection, Warning, TEXT("Clothing asset '%s' is missing from the exports of '%s'"), *ExportName.ToString(), *SkeletalMesh->GetName());

			return;
		}

		/* BindToSkeletalMesh writes LodMap itself and refuses a cloth LOD the map already claims,
		 * so the exported one is kept here and taken off the asset */
		SourceLodMaps.Last() = ClothingAsset->LodMap;
		ClothingAsset->LodMap.Empty();

		/* Two cloths sharing a guid is a mesh whose sections follow whichever answers first.
		 * Written through the property because only the factory is allowed to set it. */
		if (!ClothingAsset->GetAssetGuid().IsValid()) {
			if (const FStructProperty* AssetGuidProperty = CastField<FStructProperty>(UClothingAssetCommon::StaticClass()->FindPropertyByName(TEXT("AssetGuid")))) {
				*AssetGuidProperty->ContainerPtrToValuePtr<FGuid>(ClothingAsset) = FGuid::NewGuid();
			}
		}

		RebuildParameterMasks(ClothingAsset);

		/* Before the bind: BindToSkeletalMesh asserts when it cannot find itself in here */
		SkeletalMesh->GetMeshClothingAssets().Add(ClothingAsset);

		/* Bone indices are the source skeleton's; the names are what carries over */
		ClothingAsset->RefreshBoneMapping(SkeletalMesh);
	});

	/* Bind ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
	int32 BoundSections = 0;

	/* BindToSkeletalMesh opens one of these itself and writes the mapping after opening it, so its
	 * own scope rebuilds the sections on the way out and takes the mapping with it. Holding one
	 * open across the run makes the inner scope inert, which is what the editor's Apply Clothing
	 * does. */
	{
	FScopedSkeletalMeshPostEditChange ScopedPostEditChange(SkeletalMesh);

	const TArray<TSharedPtr<FJsonValue>>* LodModels;

	if (MeshExport->TryGetArrayField(TEXT("LODModels"), LodModels)) {
		for (int32 MeshLodIndex = 0; MeshLodIndex < LodModels->Num(); ++MeshLodIndex) {
			/* A mesh built with fewer LODs has nowhere to put the rest */
			if (!ImportedModel->LODModels.IsValidIndex(MeshLodIndex)) break;

			const TSharedPtr<FJsonObject> LodModel = (*LodModels)[MeshLodIndex]->AsObject();
			if (!LodModel.IsValid()) continue;

			const TArray<TSharedPtr<FJsonValue>>* Sections;
			if (!LodModel->TryGetArrayField(TEXT("Sections"), Sections)) continue;

			for (int32 SectionIndex = 0; SectionIndex < Sections->Num(); ++SectionIndex) {
				const TSharedPtr<FJsonObject> Section = (*Sections)[SectionIndex]->AsObject();
				if (!Section.IsValid()) continue;

				int32 AssetIndex;
				if (!Section->TryGetNumberField(TEXT("CorrespondClothAssetIndex"), AssetIndex)) continue;

				/* Everything that simulates nothing reads -1, which is most sections */
				if (!ClothingAssets.IsValidIndex(AssetIndex) || ClothingAssets[AssetIndex] == nullptr) continue;
				if (!ImportedModel->LODModels[MeshLodIndex].Sections.IsValidIndex(SectionIndex)) continue;

				const TArray<int32>& SourceLodMap = SourceLodMaps[AssetIndex];
				const int32 AssetLodIndex = SourceLodMap.IsValidIndex(MeshLodIndex) ? SourceLodMap[MeshLodIndex] : 0;

				/* Nothing was mapped to this mesh LOD */
				if (AssetLodIndex == INDEX_NONE) continue;

				UClothingAssetCommon* ClothingAsset = ClothingAssets[AssetIndex];
				if (!ClothingAsset->BindToSkeletalMesh(SkeletalMesh, MeshLodIndex, SectionIndex, AssetLodIndex)) continue;

				/* Bind only writes the section. UserSectionsData is the copy a rebuild syncs back
				 * over it, and unbinding clears both, so binding leaves this half to the caller. */
				FSkeletalMeshLODModel& TargetLodModel = ImportedModel->LODModels[MeshLodIndex];
				const FSkelMeshSection& BoundSection = TargetLodModel.Sections[SectionIndex];

				FSkelMeshSourceSectionUserData& SectionUserData = TargetLodModel.UserSectionsData.FindOrAdd(BoundSection.OriginalDataSectionIndex);

				int32 BoundAssetIndex = INDEX_NONE;
				SkeletalMesh->GetMeshClothingAssets().Find(ClothingAsset, BoundAssetIndex);

				SectionUserData.CorrespondClothAssetIndex = static_cast<int16>(BoundAssetIndex);
				SectionUserData.ClothingData.AssetGuid = ClothingAsset->GetAssetGuid();
				SectionUserData.ClothingData.AssetLodIndex = AssetLodIndex;

				/* @TEMP: mapping size is the only number that says whether the bind did anything */
				UE_LOG(LogReflection, Warning, TEXT("[Cloth] bound LOD%d section %d: %d render verts, %d sim verts, mapping %d x %d"),
					MeshLodIndex,
					SectionIndex,
					BoundSection.SoftVertices.Num(),
					ClothingAsset->LodData.IsValidIndex(AssetLodIndex) ? ClothingAsset->LodData[AssetLodIndex].PhysicalMeshData.Vertices.Num() : -1,
					BoundSection.ClothMappingDataLODs.Num(),
					BoundSection.ClothMappingDataLODs.Num() > 0 ? BoundSection.ClothMappingDataLODs[0].Num() : 0);

				BoundSections++;
			}
		}
	}

	/* Finalize ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
	for (UClothingAssetCommon* ClothingAsset : ClothingAssets) {
		if (ClothingAsset == nullptr) continue;

		/* Tethers and self collision are cooked out of the export, so they are rebuilt rather than
		 * trusted. Transition data is per LOD pair, so it waits until every LOD is bound. */
		ClothingAsset->InvalidateAllCachedData();
		ClothingAsset->BuildLodTransitionData();

		ClothingAsset->Modify();
	}

	SkeletalMesh->SetHasActiveClothingAssets(SkeletalMesh->ComputeActiveClothingAssets());

	SkeletalMesh->Modify();
	SkeletalMesh->MarkPackageDirty();

	/* The run's one rebuild happens here, on the way out */
	}

	/* @TEMP: read back after the rebuild, which is where the mapping goes missing */
	for (int32 MeshLodIndex = 0; MeshLodIndex < ImportedModel->LODModels.Num(); ++MeshLodIndex) {
		const FSkeletalMeshLODModel& LodModel = ImportedModel->LODModels[MeshLodIndex];

		for (int32 SectionIndex = 0; SectionIndex < LodModel.Sections.Num(); ++SectionIndex) {
			const FSkelMeshSection& Section = LodModel.Sections[SectionIndex];
			if (!Section.HasClothingData()) continue;

			UE_LOG(LogReflection, Warning, TEXT("[Cloth] end of run, LOD%d section %d: asset %d, mapping %d x %d, disabled %d"),
				MeshLodIndex,
				SectionIndex,
				Section.CorrespondClothAssetIndex,
				Section.ClothMappingDataLODs.Num(),
				Section.ClothMappingDataLODs.Num() > 0 ? Section.ClothMappingDataLODs[0].Num() : 0,
				Section.bDisabled ? 1 : 0);
		}
	}

	BrowseToWhenFinished(SkeletalMesh);

	AppendNotification(
		FText::FromString("Reflected Clothing Data: " + SkeletalMesh->GetName()),
		FText::FromString(FString::Printf(TEXT("%d asset(s), %d section(s) bound"), SkeletalMesh->GetMeshClothingAssets().Num(), BoundSections)),
		3.5f,
		FAppStyle::Get().GetBrush("PhysicsAssetEditor.EnableCollision.Small"),
		BoundSections > 0 ? SNotificationItem::CS_Success : SNotificationItem::CS_Fail,
		false,
		310.0f
	);
#endif
}

void TClothingData::RebuildParameterMasks(UClothingAssetCommon* ClothingAsset) {
#if UE4_27_AND_UE5 && WITH_EDITORONLY_DATA
	/* The simulation reads the weight maps, the cloth paint tool reads the masks, and only the
	 * weight maps are exported. Without these every painted map shows up empty. */
	const UEnum* TargetEnum = FindEnumByType(TEXT("EChaosWeightMapTarget"));

	if (TargetEnum == nullptr) {
		TargetEnum = FindEnumByType(TEXT("EWeightMapTargetCommon"));
	}

	for (FClothLODDataCommon& LodData : ClothingAsset->LodData) {
		LodData.PointWeightMaps.Empty();

		for (const TPair<uint32, FPointWeightMap>& WeightMap : LodData.PhysicalMeshData.WeightMaps) {
			if (WeightMap.Value.Values.Num() == 0) continue;

			FPointWeightMap& Mask = LodData.PointWeightMaps.Add_GetRef(WeightMap.Value);

			Mask.CurrentTarget = static_cast<uint8>(WeightMap.Key);
			Mask.bEnabled = true;

			/* Chaos declares its own targets on top of the common ones */
			const FString TargetName = TargetEnum != nullptr ? TargetEnum->GetNameStringByValue(WeightMap.Key) : FString();

			Mask.Name = TargetName.IsEmpty() ? FName(*FString::FromInt(WeightMap.Key)) : FName(*TargetName);
		}
	}
#endif
}
