/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/Mesh/SkeletalMeshImporter.h"
#include "Rendering/SkeletalMeshLODImporterData.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Engine/AssetCompatibility.h"
#include "Engine/AssetUserData.h"

/* Baking a DNA into poses is the DNA half's, but this is where the setting is acted on */
#if REFLECTION_RIG_LOGIC
#include "Animation/PoseAsset.h"
#endif

#include "Animation/PoseAsset.h"
#include "Engine/EngineUtilities.h"
#include "Utilities/JsonHelpers.h"

#include "Modules/Cloud/Cloud.h"
#include "Modules/Cloud/Tools/MeshGeometry.h"
#include "Modules/Cloud/Tools/SkeletalMeshData.h"

#include "Engine/SkeletalMesh.h"
#include "Animation/Skeleton.h"
#include "Rendering/SkeletalMeshModel.h"
#include "Rendering/SkeletalMeshLODModel.h"

/* FSkeletalMaterial moved out of SkeletalMesh.h in 5.1, the same way the geometry tool takes it */
#if UE5_1_BEYOND
#include "Engine/SkinnedAssetCommon.h"
#endif

/* 4.25 put the mesh's reference skeleton, its materials and its imported data behind accessors.
 * Before that they are members, and the imported data belongs to the LOD model rather than to the
 * mesh, so the same questions are asked of a different object. */
namespace {
#if UE4_25_BELOW
	void SetMeshRefSkeleton(USkeletalMesh* Mesh, const FReferenceSkeleton& Value) { Mesh->RefSkeleton = Value; }
	void SetMeshMaterials(USkeletalMesh* Mesh, const TArray<FSkeletalMaterial>& Value) { Mesh->Materials = Value; }
	void SetMeshSkeleton(USkeletalMesh* Mesh, USkeleton* Value) { Mesh->Skeleton = Value; }
	void SetMeshMinLod(USkeletalMesh* Mesh, const FPerPlatformInt& Value) { Mesh->MinLod = Value; }
	const TArray<UMorphTarget*>& MeshMorphTargets(const USkeletalMesh* Mesh) { return Mesh->MorphTargets; }

	FSkeletalMeshLODModel* MeshLodModel(const USkeletalMesh* Mesh, const int32 Lod) {
		FSkeletalMeshModel* Model = Mesh->GetImportedModel();

		return Model != nullptr && Model->LODModels.IsValidIndex(Lod) ? &Model->LODModels[Lod] : nullptr;
	}

	bool IsMeshLodImportedDataEmpty(const USkeletalMesh* Mesh, const int32 Lod) {
		const FSkeletalMeshLODModel* LodModel = MeshLodModel(Mesh, Lod);

		return LodModel == nullptr || LodModel->RawSkeletalMeshBulkData.IsEmpty();
	}

	void LoadMeshLodImportedData(const USkeletalMesh* Mesh, const int32 Lod, FSkeletalMeshImportData& Out) {
		if (FSkeletalMeshLODModel* LodModel = MeshLodModel(Mesh, Lod)) LodModel->RawSkeletalMeshBulkData.LoadRawMesh(Out);
	}

	void SaveMeshLodImportedData(const USkeletalMesh* Mesh, const int32 Lod, FSkeletalMeshImportData& In) {
		if (FSkeletalMeshLODModel* LodModel = MeshLodModel(Mesh, Lod)) LodModel->RawSkeletalMeshBulkData.SaveRawMesh(In);
	}
#else
	void SetMeshRefSkeleton(USkeletalMesh* Mesh, const FReferenceSkeleton& Value) { Mesh->SetRefSkeleton(Value); }
	void SetMeshMaterials(USkeletalMesh* Mesh, const TArray<FSkeletalMaterial>& Value) { Mesh->SetMaterials(Value); }
	void SetMeshSkeleton(USkeletalMesh* Mesh, USkeleton* Value) { Mesh->SetSkeleton(Value); }
	void SetMeshMinLod(USkeletalMesh* Mesh, const FPerPlatformInt& Value) { Mesh->SetMinLod(Value); }
	const TArray<TObjectPtr<UMorphTarget>>& MeshMorphTargets(const USkeletalMesh* Mesh) { return Mesh->GetMorphTargets(); }

	bool IsMeshLodImportedDataEmpty(const USkeletalMesh* Mesh, const int32 Lod) { return Mesh->IsLODImportedDataEmpty(Lod); }
	void LoadMeshLodImportedData(const USkeletalMesh* Mesh, const int32 Lod, FSkeletalMeshImportData& Out) { Mesh->LoadLODImportedData(Lod, Out); }
	void SaveMeshLodImportedData(USkeletalMesh* Mesh, const int32 Lod, FSkeletalMeshImportData& In) { Mesh->SaveLODImportedData(Lod, In); }
#endif
}

UObject* ISkeletalMeshImporter::CreateAsset(UObject* CreatedAsset) {
	/* Reflecting the same mesh twice lands on the package the first import wrote, and making an
	 * object over one already sitting there destroys the one already sitting there: the name is
	 * taken, so the engine tears the old mesh down where it stands and builds the new one in its
	 * place. The Content Browser draws that mesh through a component whose mesh object holds its
	 * render data by pointer, and nothing tells the pointer. The vertex factories that object made
	 * are then released against freed memory, the release is skipped, and deleting the object
	 * afterward stops the editor on a render resource that was never released.
	 *
	 * Moved aside rather than destroyed: whatever is still drawing the old mesh keeps a mesh that
	 * is still there to draw, and it goes on its own once nothing is. */
	if (UObject* Existing = StaticFindObjectFast(USkeletalMesh::StaticClass(), GetPackage(), StringToName(GetAssetName()))) {
		Existing->Rename(
			nullptr,
			GetTransientPackage(),
			REN_ForceNoResetLoaders | REN_DoNotDirty | REN_DontCreateRedirectors | REN_NonTransactional
		);
	}

	return IImporter::CreateAsset(NewObject<USkeletalMesh>(GetPackage(), USkeletalMesh::StaticClass(), *GetAssetName(), RF_Public | RF_Standalone));
}

/* A half built mesh is still drawn.
 *
 * The Content Browser renders a thumbnail of whatever its package holds, and an import that waits
 * on the Cloud keeps the editor ticking while it waits, so the mesh is posed and handed to the
 * renderer long before anything reads the failure this is called for. Neither half of one survives
 * being drawn: bones with no LOD to name the ones the mesh needs walk off the end of a required
 * bone list the engine never filled in, and an empty LOD asks the renderer for the inverse bind
 * matrices the bones would have carried. Both are emptied here, and the export lets go of the mesh
 * so the package it was made in is dropped along with it. */
void ISkeletalMeshImporter::Abandon(USkeletalMesh* SkeletalMesh) {
	if (SkeletalMesh == nullptr) return;

	/* Nothing to pose */
	SetMeshRefSkeleton(SkeletalMesh, FReferenceSkeleton());
	SkeletalMesh->CalculateInvRefMatrices();

	/* Nothing to draw: the renderer is only asked to update a LOD the mesh says it has */
	SkeletalMesh->GetLODInfoArray().Empty();

	SkeletalMesh->ClearFlags(RF_Public | RF_Standalone);

	/* Not marked for destruction, however tempting: the Content Browser draws thumbnails through a
	 * component that keeps a mesh object of its own, and that object holds the mesh's render data
	 * by pointer. Marking the mesh clears the component's reference to it out from under that
	 * pointer on the next collection, and the vertex factories the mesh object made are then
	 * released against render data that is gone which the engine finds out about by deleting a
	 * render resource that was never released, and stops the editor over. Emptied of everything
	 * that can be drawn, it is collected on its own once the package below lets go of it. */

	/* What the reader watches to decide whether the import built anything worth keeping */
	SetAsset(nullptr);
}

USkeleton* ISkeletalMeshImporter::BuildSkeletonFromMesh(USkeletalMesh* SkeletalMesh) const {
	if (SkeletalMesh == nullptr || MeshRefSkeleton(SkeletalMesh).GetNum() == 0) return nullptr;

	const FString SkeletonName = GetAssetName() + TEXT("_Skeleton");
	const FString Folder = FPackageName::GetLongPackagePath(GetPackage()->GetName());

	UPackage* SkeletonPackage = FAssetUtilities::CreateAssetPackage(Folder / SkeletonName);
	if (SkeletonPackage == nullptr) return nullptr;

	/* Re-importing lands on the package the last import wrote, and a package read back off disk
	 * comes in on demand: saving one that was never finished reading is fatal */
	SkeletonPackage->FullyLoad();

	USkeleton* Skeleton = NewObject<USkeleton>(SkeletonPackage, StringToName(SkeletonName), RF_Public | RF_Standalone);

	/* Builds the bone tree out of the mesh's reference skeleton, which is the same thing the
	 * editor does for a mesh imported without one */
	if (!Skeleton->MergeAllBonesToBoneTree(SkeletalMesh)) return nullptr;

	HandleAssetCreation(Skeleton, SkeletonPackage);

	if (GetSettings()->AssetSettings.SaveAssets) {
		SavePackage(SkeletonPackage);
	}

	UE_LOG(LogReflection, Display, TEXT("\"%s\" named no skeleton, so \"%s\" was made from its %d bone(s)"), *GetAssetName(), *SkeletonName, MeshRefSkeleton(SkeletalMesh).GetNum());

	return Skeleton;
}

USkeleton* ISkeletalMeshImporter::ResolveSkeleton() {
	const FUObjectJsonValueExport SkeletonReference = GetAssetDataAsValue().GetObject(TEXT("Skeleton"));

	if (!SkeletonReference.JsonObject.IsValid() || !SkeletonReference.Has(TEXT("ObjectName"))) {
		return nullptr;
	}

	/* Loads it out of the project, and asks the Cloud for it when it isn't there yet */
	TObjectPtr<USkeleton> Skeleton;
	LoadExport<USkeleton>(&SkeletonReference.JsonObject, Skeleton);

	return Skeleton.Get();
}

void ISkeletalMeshImporter::BuildMaterialSlots(USkeletalMesh* SkeletalMesh, const TArray<TSharedPtr<FJsonValue>>& Slots) {
	TArray<FSkeletalMaterial> Materials;
	Materials.Reserve(Slots.Num());

	for (int32 SlotIndex = 0; SlotIndex < Slots.Num(); ++SlotIndex) {
		const TSharedPtr<FJsonObject> Slot = Slots[SlotIndex].IsValid() ? Slots[SlotIndex]->AsObject() : nullptr;

		FString SlotName;

		if (Slot.IsValid()) {
			Slot->TryGetStringField(TEXT("MaterialSlotName"), SlotName);
		}

		/* A cook keeps the slots and drops the names they were imported under, so every one of them
		 * comes back called None. The build hands a face to a slot by matching that name, and a
		 * name every slot answers to sends every section to the first of them: the mesh draws with
		 * one material and the rest go unused. Named by their place instead, which is the same
		 * thing the static mesh import does with slots a cook left unnamed. */
		if (SlotName.IsEmpty() || SlotName == TEXT("None")) {
			SlotName = FString::Printf(TEXT("Material_%d"), SlotIndex);
		}

		FSkeletalMaterial& Material = Materials.AddDefaulted_GetRef();

		Material.MaterialSlotName = FName(*SlotName);
		Material.ImportedMaterialSlotName = Material.MaterialSlotName;
	}

	SetMeshMaterials(SkeletalMesh, Materials);
}

int32 ISkeletalMeshImporter::BuildMorphTargets(USkeletalMesh* SkeletalMesh, const TSharedPtr<FJsonObject>& Payload) {
	const TArray<TSharedPtr<FJsonValue>>* Morphs = nullptr;

	if (!Payload.IsValid() || !Payload->TryGetArrayField(TEXT("morphs"), Morphs)) {
		return 0;
	}

	const FSkeletalMeshModel* ImportedModel = SkeletalMesh->GetImportedModel();
	if (ImportedModel == nullptr) return 0;

	/* The deltas go into the imported data rather than onto the mesh directly: a build makes the
	 * morph targets out of what the imported data names, and drops every one it doesn't find
	 * there. Anything written straight onto the mesh is gone the next time it is built. */
	TSet<FString> Written;

	for (int32 LodIndex = 0; LodIndex < ImportedModel->LODModels.Num(); ++LodIndex) {
		/* 5.4 renamed the question. Before it, the same thing is asked of the imported data. */
#if UE5_4_BEYOND
		if (!SkeletalMesh->HasMeshDescription(LodIndex)) continue;
#else
		if (IsMeshLodImportedDataEmpty(SkeletalMesh, LodIndex)) continue;
#endif

		FSkeletalMeshImportData ImportData;
		LoadMeshLodImportedData(SkeletalMesh, LodIndex, ImportData);

		if (ImportData.Points.Num() == 0) continue;

		ImportData.MorphTargetNames.Empty();
		ImportData.MorphTargets.Empty();
		ImportData.MorphTargetModifiedPoints.Empty();

		for (const TSharedPtr<FJsonValue>& MorphValue : *Morphs) {
			const TSharedPtr<FJsonObject> Morph = MorphValue.IsValid() ? MorphValue->AsObject() : nullptr;
			if (!Morph.IsValid()) continue;

			FString Name;
			const TArray<TSharedPtr<FJsonValue>>* Lods;

			if (!Morph->TryGetStringField(TEXT("Name"), Name) || Name.IsEmpty()) continue;
			if (!Morph->TryGetArrayField(TEXT("Lods"), Lods)) continue;

			for (const TSharedPtr<FJsonValue>& LodValue : *Lods) {
				const TSharedPtr<FJsonObject> Lod = LodValue.IsValid() ? LodValue->AsObject() : nullptr;
				if (!Lod.IsValid()) continue;
				if (Lod->GetIntegerField(TEXT("Index")) != LodIndex) continue;

				const TArray<TSharedPtr<FJsonValue>>* SourceIndices;
				const TArray<TSharedPtr<FJsonValue>>* PositionDeltas;

				if (!Lod->TryGetArrayField(TEXT("SourceIndices"), SourceIndices)) continue;
				if (!Lod->TryGetArrayField(TEXT("PositionDeltas"), PositionDeltas)) continue;

				/* A shape is the base points with the morph applied, named by the points it moved.
				 * The two are read side by side, so a point may only be named once. */
				FSkeletalMeshImportData Shape;
				TSet<uint32> ModifiedPoints;

				Shape.Points.Reserve(SourceIndices->Num());
				ModifiedPoints.Reserve(SourceIndices->Num());

				for (int32 Delta = 0; Delta < SourceIndices->Num(); ++Delta) {
					const int32 Point = static_cast<int32>((*SourceIndices)[Delta]->AsNumber());

					if (!ImportData.Points.IsValidIndex(Point)) continue;
					if (ModifiedPoints.Contains(static_cast<uint32>(Point))) continue;

					ModifiedPoints.Add(static_cast<uint32>(Point));

					Shape.Points.Add(ImportData.Points[Point] + FVector3f(
						ReadFloat(PositionDeltas, Delta * 3),
						ReadFloat(PositionDeltas, Delta * 3 + 1),
						ReadFloat(PositionDeltas, Delta * 3 + 2)
					));
				}

				if (Shape.Points.Num() == 0) continue;

				ImportData.MorphTargetNames.Add(Name);
				ImportData.MorphTargets.Add(MoveTemp(Shape));
				ImportData.MorphTargetModifiedPoints.Add(MoveTemp(ModifiedPoints));

				Written.Add(Name);
			}
		}

		SaveMeshLodImportedData(SkeletalMesh, LodIndex, ImportData);
	}

	return Written.Num();
}

bool ISkeletalMeshImporter::ApplyCookedBindPose(USkeletalMesh* SkeletalMesh, USkeleton* Skeleton, const FString& FetchPath) {
	const TSharedPtr<FJsonObject> Payload = Cloud::Export::GetReferenceSkeletonBlocking(FetchPath);

	const TArray<TSharedPtr<FJsonValue>>* Bones = nullptr;

	if (!Payload.IsValid() || !Payload->TryGetArrayField(TEXT("bones"), Bones) || Bones->Num() == 0) {
		return false;
	}

	/* The pose a mesh is bound at is the mesh's, not its skeleton's: characters share a skeleton
	 * and each is built at its own proportions. Everything the mesh is skinned with is measured
	 * against this pose, so binding to the skeleton's instead deforms the mesh the moment a bone
	 * moves away from it, which is what a rig does on its first evaluation. */
	FReferenceSkeleton BindPose;

	{
		FReferenceSkeletonModifier Modifier(BindPose, Skeleton);

		const auto ReadVector = [](const TArray<TSharedPtr<FJsonValue>>* Values, const int32 Offset) {
			return FVector(
				Values != nullptr && Values->IsValidIndex(Offset) ? (*Values)[Offset]->AsNumber() : 0.0,
				Values != nullptr && Values->IsValidIndex(Offset + 1) ? (*Values)[Offset + 1]->AsNumber() : 0.0,
				Values != nullptr && Values->IsValidIndex(Offset + 2) ? (*Values)[Offset + 2]->AsNumber() : 0.0
			);
		};

		for (const TSharedPtr<FJsonValue>& Value : *Bones) {
			const TSharedPtr<FJsonObject> Bone = Value.IsValid() ? Value->AsObject() : nullptr;
			if (!Bone.IsValid()) continue;

			FString Name;
			if (!Bone->TryGetStringField(TEXT("Name"), Name)) return false;

			int32 ParentIndex = INDEX_NONE;
			Bone->TryGetNumberField(TEXT("ParentIndex"), ParentIndex);

			const TArray<TSharedPtr<FJsonValue>>* Translation = nullptr;
			const TArray<TSharedPtr<FJsonValue>>* Rotation = nullptr;
			const TArray<TSharedPtr<FJsonValue>>* Scale = nullptr;

			Bone->TryGetArrayField(TEXT("Translation"), Translation);
			Bone->TryGetArrayField(TEXT("Rotation"), Rotation);
			Bone->TryGetArrayField(TEXT("Scale"), Scale);

			const FQuat BoneRotation(
				Rotation != nullptr && Rotation->IsValidIndex(0) ? (*Rotation)[0]->AsNumber() : 0.0,
				Rotation != nullptr && Rotation->IsValidIndex(1) ? (*Rotation)[1]->AsNumber() : 0.0,
				Rotation != nullptr && Rotation->IsValidIndex(2) ? (*Rotation)[2]->AsNumber() : 0.0,
				Rotation != nullptr && Rotation->IsValidIndex(3) ? (*Rotation)[3]->AsNumber() : 1.0
			);

			const FTransform Pose(BoneRotation.GetNormalized(), ReadVector(Translation, 0), ReadVector(Scale, 0));

			Modifier.Add(FMeshBoneInfo(FName(*Name), Name, ParentIndex), Pose);
		}
	}

	if (BindPose.GetNum() == 0) return false;

	SetMeshRefSkeleton(SkeletalMesh, BindPose);

	UE_LOG(LogReflection, Display, TEXT("\"%s\" bound at its own pose, %d bone(s)"), *GetAssetName(), BindPose.GetNum());

	return true;
}

bool ISkeletalMeshImporter::Import() {
	/* The skeleton the mesh names, if it names one. Plenty of cooked meshes do not: the bones they
	 * are skinned to are written into the mesh itself, and the reference to a skeleton asset is
	 * something only the editor kept. One is made further down for those, out of the mesh's own
	 * pose, rather than the import stopping to ask for an asset that was never there. */
	USkeleton* Skeleton = ResolveSkeleton();

	/* Asked for by the path the game cooked it under. The export names that itself; only when it
	 * doesn't is the path the asset landed at turned back into one, which is a guess the moment an
	 * import is redirected somewhere else. */
	FString FetchPath = GetAssetExport()->HasField(TEXT("Package"))
		? GetAssetExport()->GetStringField(TEXT("Package"))
		: FString();

	if (FetchPath.IsEmpty()) {
		FetchPath = GetPackage()->GetPathName();

		FRRedirects::Reverse(FetchPath);
	}

	/* Asked for before the mesh is made rather than after it is half filled in. A mesh carrying the
	 * skeleton's bones with no LOD to say which of them it needs is still what the Content Browser
	 * draws a thumbnail from, and posing one for that thumbnail reads the first entry of a required
	 * bone list the engine never filled in: the editor goes down with it. Nothing is created until
	 * the geometry is in hand, so a Cloud that isn't running leaves the last import standing. */
	const TSharedPtr<FJsonObject> Geometry = Cloud::Export::GetLodModelBlocking(FetchPath);

	if (!Geometry.IsValid()) {
		FImportIssues::Report(
			EImportIssue::Failed,
			TEXT("No geometry from the Cloud"),
			TEXT("The Cloud has to be running, and the mesh needs cooked render data to read.")
		);

		return false;
	}

	USkeletalMesh* SkeletalMesh = Create<USkeletalMesh>();
	if (SkeletalMesh == nullptr) return false;

	if (Skeleton != nullptr) {
		SetMeshSkeleton(SkeletalMesh, Skeleton);
	}

	/* The mesh's own bind pose if the Cloud can serve it, and the skeleton's only as a fallback:
	 * the skeleton is shared and built at nobody's proportions in particular. */
	const bool bCookedBindPose = ApplyCookedBindPose(SkeletalMesh, Skeleton, FetchPath);

	if (!bCookedBindPose) {
		if (Skeleton == nullptr) {
			FImportIssues::Report(
				EImportIssue::Failed,
				TEXT("The mesh has no bones to skin to"),
				TEXT("The export names no skeleton, and the Cloud served no reference pose for the mesh either, so there is nothing to bind it against.")
			);

			Abandon(SkeletalMesh);

			return false;
		}

		SetMeshRefSkeleton(SkeletalMesh, Skeleton->GetReferenceSkeleton());
	}

	/* Named no skeleton and now holding the bones it was cooked with: the asset the editor would
	 * have pointed at is made here out of exactly those bones, and saved beside the mesh. */
	if (Skeleton == nullptr) {
		Skeleton = BuildSkeletonFromMesh(SkeletalMesh);

		if (Skeleton == nullptr) {
			FImportIssues::Report(
				EImportIssue::Failed,
				TEXT("A skeleton couldn't be made for this mesh"),
				TEXT("The export names none, so one is built out of the mesh's own bones, and the engine would not take them into a bone tree.")
			);

			Abandon(SkeletalMesh);

			return false;
		}

		SetMeshSkeleton(SkeletalMesh, Skeleton);
	}

	SkeletalMesh->CalculateInvRefMatrices();

	/* Before the geometry: a wedge names the slot it belongs to */
	const TArray<TSharedPtr<FJsonValue>>* Slots;

	if (GetAssetExport()->TryGetArrayField(TEXT("SkeletalMaterials"), Slots)) {
		BuildMaterialSlots(SkeletalMesh, *Slots);
	}

	/* Everything the mesh is besides its geometry: the LOD it starts drawing at, whether it was
	 * cooked with vertex colours, the physics it collides with. What this import builds itself, or
	 * what the tool below owns, is left out rather than written over. */
	/* The LOD list carries the screen sizes, hysteresis, material map and build settings the game
	 * shipped. Whichever name it arrives under, it is the same list, and this engine reads it as
	 * LODInfo. */
	if (!GetAssetData()->HasField(TEXT("LODInfo"))) {
		const TArray<TSharedPtr<FJsonValue>>* Models;

		if (GetAssetData()->TryGetArrayField(TEXT("SourceModels"), Models)) {
			GetAssetData()->SetArrayField(TEXT("LODInfo"), *Models);
		}
	}

	GetObjectSerializer()->DeserializeObjectProperties(RemovePropertiesShared(GetAssetData(), {
		/* Built here, out of the geometry */
		"LODModels",
		"NaniteResources",
		"SamplingInfo",

		/* Read as LODInfo below instead. A newer engine keeps the LOD list under this name, and
		 * this one keeps the mesh data the build owns under it, so deserializing it as written
		 * fills engine structures with somebody else's fields. */
		"SourceModels",

		/* Already set, and the reference in the export points at the cooked package */
		"Skeleton",

		/* The skeletal mesh data tool's, once the mesh exists */
		"Sockets",
		"MorphTargets",
		"SkinWeightProfiles",
	}), SkeletalMesh);

	const int32 BuiltLods = TMeshGeometry::RebuildLodModels(SkeletalMesh, Geometry);

	if (BuiltLods == 0) {
		FImportIssues::Report(
			EImportIssue::Failed,
			TEXT("The mesh has no LOD to build"),
			TEXT("Every LOD the payload carries was missing the vertices or the sections to describe it.")
		);

		Abandon(SkeletalMesh);

		return false;
	}

	/* The bounds the game cooked, rather than the ones a rebuild derives: a mesh whose bounds come
	 * out smaller than the game's is culled where the game would still draw it */
	const FUObjectJsonValueExport ImportedBounds = GetAssetAsValue().GetObject(TEXT("ImportedBounds"));

	if (ImportedBounds.JsonObject.IsValid() && ImportedBounds.Has(TEXT("SphereRadius"))) {
		/* Read out by hand: bounds are a vector type rather than a struct with a static one to
		 * deserialize against, and the three fields are all it is */
		auto ReadVector = [](const FUObjectJsonValueExport& Vector) {
			return FVector(
				Vector.Has(TEXT("X")) ? Vector.GetNumber(TEXT("X")) : 0.0,
				Vector.Has(TEXT("Y")) ? Vector.GetNumber(TEXT("Y")) : 0.0,
				Vector.Has(TEXT("Z")) ? Vector.GetNumber(TEXT("Z")) : 0.0
			);
		};

		FBoxSphereBounds Bounds;

		Bounds.Origin = ReadVector(ImportedBounds.GetObject(TEXT("Origin")));
		Bounds.BoxExtent = ReadVector(ImportedBounds.GetObject(TEXT("BoxExtent")));
		Bounds.SphereRadius = ImportedBounds.GetNumber(TEXT("SphereRadius"));

		SkeletalMesh->SetImportedBounds(Bounds);
	}

	/* Morph deltas come down the way the geometry does a cook quantizes them into the render
	 * buffers rather than keeping them where an import would look. */
	const TArray<TSharedPtr<FJsonValue>>* ExportedMorphTargets;

	const bool bHasMorphTargets = GetAssetData()->TryGetArrayField(TEXT("MorphTargets"), ExportedMorphTargets)
		&& ExportedMorphTargets->Num() > 0;

	const int32 WrittenMorphs = bHasMorphTargets
		? BuildMorphTargets(SkeletalMesh, Cloud::Export::GetMorphTargetsBlocking(FetchPath))
		: 0;

	/* A MetaHuman head keeps its face rig as a DNA hung off the mesh, and none of it is in the
	 * properties: the stream sits after them in the package the same way the geometry does. */
	if (ExportNamesDna()) {
		const bool bAppliedDna = ApplyDna(SkeletalMesh, FetchPath);

		if (bAppliedDna) {
			/* Only when the mesh's own bind pose could not be had: reconstructing it from the
			 * DNA is a guess at what the package already states outright */
			if (!bCookedBindPose) {
				AlignBindPoseToDna(SkeletalMesh);
			}
		}

		/* Flattening the rig into poses is the setting's job. Where the DNA never attached there
		 * is no rig here to flatten, and the poses are asked of the Cloud instead: that is the
		 * whole of the face on an engine that cannot read a DNA at all. */
		bool bBakedPoses = false;

		/* The faces as morph targets, which is the one shape that stacks the way the rig does */
		if (GetSettings()->AssetSettings.DNA.MorphTargets) {
			const FRDnaBackportSettings& Backport = GetSettings()->AssetSettings.DNA.Backport;

			const int32 Built = BuildMorphTargets(SkeletalMesh,
				Cloud::Export::GetDnaMorphsBlocking(FetchPath, Backport.BackportPoses ? Backport.CurveMapping : FString()));

			bBakedPoses = Built > 0;

			UE_LOG(LogReflection, Display, TEXT("\"%s\" built %d morph target(s) out of its DNA"), *GetAssetName(), Built);
		}

		if (GetSettings()->AssetSettings.DNA.BakeToPoseAsset) {
			const UPoseAsset* PoseAsset = bAppliedDna
				? BakeDnaPoseAsset(SkeletalMesh)
				: BakeDnaPoseAssetFromCloud(SkeletalMesh, FetchPath);

			bBakedPoses = PoseAsset != nullptr;

			if (bBakedPoses) {
				UE_LOG(LogReflection, Display, TEXT("\"%s\" baked %d pose(s) out of its DNA into \"%s\""),
					*GetAssetName(), PoseAsset->GetNumPoses(), *PoseAsset->GetName());
			}
		}

		/* Nothing to say where the poses stood in for the rig: the face is there either way, and
		 * only the thing that drives it differs */
		if (!bAppliedDna && !bBakedPoses) {
#if REFLECTION_RIG_LOGIC
			FImportIssues::Report(
				EImportIssue::Data,
				TEXT("The mesh's DNA didn't come back"),
				TEXT("The export hangs a DNA off this mesh, so it is a rigged head. Without the stream the mesh imports, but nothing drives its face.")
			);
#else
			/* Nothing was asked for: without RigLogic there is nothing here that reads a DNA, so
			 * saying the stream did not come back would be blaming the Cloud for the engine. */
			FImportIssues::Report(
				EImportIssue::MissingClass,
				TEXT("This engine has no RigLogic"),
				TEXT("The export hangs a DNA off this mesh, so it is a rigged head. RigLogic is what reads one and it ships with Unreal Engine 5, so the face has to arrive already worked out. Baking it to a pose asset is what does that, and it is off or did not run.")
			);
#endif
		}
	}

	/* Before 4.27 the LOD models were built as the geometry was read, so there is nothing left for
	 * the mesh to build out of imported data it doesn't keep. What turns those models into render
	 * data is the PostEditChange at the end of this function, and nothing may touch the mesh's
	 * resources before it: resources brought up against render data that has not been rebuilt yet
	 * describe a mesh that no longer exists, which the first draw finds out about. */
#if UE4_27_AND_UE5
	SkeletalMesh->Build();
#endif

	/* What the build made of it, which is not the same question as whether there was anything to
	 * build. A LOD whose influences all named bones the mesh hasn't got, or whose triangles the
	 * builder threw out, comes back with nothing on the other side, and a mesh carrying bones with
	 * no LOD to say which of them it needs is one the Content Browser crashes on when it draws the
	 * thumbnail. Reported and given up on here rather than left in the project to be drawn. */
	bool bBuilt = false;

	if (const FSkeletalMeshRenderData* RenderData = SkeletalMesh->GetResourceForRendering()) {
		for (int32 Lod = 0; Lod < RenderData->LODRenderData.Num(); ++Lod) {
			const FSkeletalMeshLODRenderData& LodData = RenderData->LODRenderData[Lod];

			if (LodData.GetNumVertices() > 0 && LodData.RequiredBones.Num() > 0) {
				bBuilt = true;

				break;
			}
		}
	}

	if (!bBuilt) {
		FImportIssues::Report(
			EImportIssue::Failed,
			TEXT("The build made nothing out of the geometry"),
			TEXT("The vertices arrived, and the mesh the engine built out of them is empty. A LOD whose influences name no bone the skeleton has is the usual reason.")
		);

		UE_LOG(LogReflection, Error, TEXT("\"%s\" built %d LOD(s) from the Cloud and the engine made an empty mesh out of them"), *GetAssetName(), BuiltLods);

		Abandon(SkeletalMesh);

		return false;
	}

	if (bHasMorphTargets && WrittenMorphs < ExportedMorphTargets->Num()) {
		FImportIssues::Report(
			EImportIssue::Data,
			TEXT("Some of the mesh's morph targets came back empty"),
			FString::Printf(
				TEXT("%d of %d were rebuilt. A morph whose deltas the cook stripped has nothing left to move the mesh with."),
				WrittenMorphs,
				ExportedMorphTargets->Num()
			)
		);
	}

	/* Materials, sockets, section flags, skin weight profiles and the rest of the properties are
	 * the same work the Skeletal Mesh Data tool does against a mesh that already exists */
	TSkeletalMeshData MeshData;
	MeshData.Process(SkeletalMesh, GetContainer()->JsonObjects);

	/* Dropped to the best LOD the mesh has, rather than wherever the game's quality settings
	 * started it drawing */
	if (GetSettings()->AssetSettings.SkeletalMesh.IgnoreMinQualityLevelLODDefault) {
		SetMeshMinLod(SkeletalMesh, FPerPlatformInt(0));

#if ENGINE_UE5
		FPerQualityLevelInt MinQualityLevelLod = SkeletalMesh->GetQualityLevelMinLod();

		MinQualityLevelLod.Default = 0;

		SkeletalMesh->SetQualityLevelMinLod(MinQualityLevelLod);
#endif
	}

	SkeletalMesh->CalculateInvRefMatrices();
	SkeletalMesh->PostEditChange();

	UE_LOG(LogReflection, Display, TEXT("\"%s\" built %d LOD(s) and %d morph target(s) against skeleton \"%s\""), *GetAssetName(), BuiltLods, MeshMorphTargets(SkeletalMesh).Num(), *Skeleton->GetName());

	return OnAssetCreation(SkeletalMesh);
}
