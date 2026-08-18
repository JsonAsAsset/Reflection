/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/Mesh/SkeletalMeshImporter.h"
#include "Rendering/SkeletalMeshLODImporterData.h"
#include "Engine/AssetUserData.h"

#if REFLECTION_RIG_LOGIC
#include "DNAAsset.h"
#include "DNAUtils.h"
#include "RigLogic.h"
#include "RigInstance.h"
#include "SkelMeshDNAUtils.h"
#include "Animation/AnimSequence.h"
#include "Animation/PoseAsset.h"
#include "Animation/AnimData/IAnimationDataController.h"
#endif

#include "Engine/EngineUtilities.h"
#include "Utilities/JsonHelpers.h"

#include "Modules/Cloud/Cloud.h"
#include "Modules/Cloud/Tools/MeshGeometry.h"
#include "Modules/Cloud/Tools/SkeletalMeshData.h"

#include "Engine/SkeletalMesh.h"
#include "Animation/Skeleton.h"
#include "Animation/MorphTarget.h"
#include "Rendering/SkeletalMeshModel.h"
#include "Rendering/SkeletalMeshLODModel.h"

/* FSkeletalMaterial moved out of SkeletalMesh.h in 5.1, the same way the geometry tool takes it */
#if UE5_1_BEYOND
#include "Engine/SkinnedAssetCommon.h"
#endif

UObject* ISkeletalMeshImporter::CreateAsset(UObject* CreatedAsset) {
	return IImporter::CreateAsset(NewObject<USkeletalMesh>(GetPackage(), USkeletalMesh::StaticClass(), *GetAssetName(), RF_Public | RF_Standalone));
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

		if (SlotName.IsEmpty()) {
			SlotName = FString::Printf(TEXT("Material_%d"), SlotIndex);
		}

		FSkeletalMaterial& Material = Materials.AddDefaulted_GetRef();

		Material.MaterialSlotName = FName(*SlotName);
		Material.ImportedMaterialSlotName = Material.MaterialSlotName;
	}

	SkeletalMesh->SetMaterials(Materials);
}

int32 ISkeletalMeshImporter::BuildMorphTargets(USkeletalMesh* SkeletalMesh, const TSharedPtr<FJsonObject>& Payload) {
	const TArray<TSharedPtr<FJsonValue>>* Morphs;

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
		if (!SkeletalMesh->HasMeshDescription(LodIndex)) continue;

		FSkeletalMeshImportData ImportData;
		SkeletalMesh->LoadLODImportedData(LodIndex, ImportData);

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

		SkeletalMesh->SaveLODImportedData(LodIndex, ImportData);
	}

	return Written.Num();
}

bool ISkeletalMeshImporter::ExportNamesDna() {
	const TArray<TSharedPtr<FJsonValue>>* UserData;

	if (!GetAssetData()->TryGetArrayField(TEXT("AssetUserData"), UserData)) {
		return false;
	}

	for (const TSharedPtr<FJsonValue>& Entry : *UserData) {
		const TSharedPtr<FJsonObject> Reference = Entry.IsValid() ? Entry->AsObject() : nullptr;
		if (!Reference.IsValid()) continue;

		FString ObjectName;

		if (Reference->TryGetStringField(TEXT("ObjectName"), ObjectName) && ObjectName.StartsWith(TEXT("DNAAsset"))) {
			return true;
		}
	}

	return false;
}

bool ISkeletalMeshImporter::ApplyDna(USkeletalMesh* SkeletalMesh, const FString& FetchPath) {
#if REFLECTION_RIG_LOGIC
	TArray<uint8> Dna = Cloud::Export::GetDnaBlocking(FetchPath);

	if (Dna.Num() == 0) {
		return false;
	}

	/* Behavior is what RigLogic runs a face with, geometry is what the editor updates the mesh
	 * from. A cook keeps the first and leaves an empty stream where the second was, so the mesh
	 * animates and the design time half is simply not there to rebuild. */
	const TSharedPtr<IDNAReader> Behavior = ReadDNAFromBuffer(&Dna, EDNADataLayer::Behavior | EDNADataLayer::MachineLearnedBehavior, 0u);

	if (!Behavior.IsValid()) {
		return false;
	}

	UDNAAsset* DNAAsset = nullptr;

	/* The mesh's AssetUserData names the DNA asset, so deserializing the properties has usually
	 * made an empty one already. Filling that one keeps the mesh pointing at a single DNA. */
	if (const TArray<UAssetUserData*>* UserData = SkeletalMesh->GetAssetUserDataArray()) {
		for (UAssetUserData* Entry : *UserData) {
			if (UDNAAsset* Existing = Cast<UDNAAsset>(Entry)) {
				DNAAsset = Existing;

				break;
			}
		}
	}

	if (DNAAsset == nullptr) {
		DNAAsset = NewObject<UDNAAsset>(SkeletalMesh, TEXT("DNAAsset"), RF_Public);

		SkeletalMesh->AddAssetUserData(DNAAsset);
	}

	DNAAsset->DnaFileName = SkeletalMesh->GetName() + TEXT(".dna");
	DNAAsset->SetBehaviorReader(Behavior);

	if (const TSharedPtr<IDNAReader> Geometry = ReadDNAFromBuffer(&Dna, EDNADataLayer::Geometry, 0u)) {
		DNAAsset->SetGeometryReader(Geometry);
	}

	return true;
#else
	return false;
#endif
}

#if REFLECTION_RIG_LOGIC
/* What RigLogic hands back per joint: translation, rotation and scale, three floats each */
static constexpr int32 GDnaJointAttributes = 9;

namespace {
	/* What a control did to a joint, on its own, in the axes the engine poses a bone with.
	 *
	 * The mapping is not the one the RigLogic anim node uses: that one is written for a DNA in
	 * MetaHuman's coordinate system, and this data is not in it. These signs are the ones that
	 * reproduce the skeleton's own reference pose out of the DNA's neutral, to within a fifth of
	 * a degree across every facial joint. */
	FTransform ComposeDnaDelta(const TArrayView<const float>& Delta, const int32 Attribute) {
		const auto Read = [&Delta](const int32 Index) {
			return Delta.IsValidIndex(Index) ? Delta[Index] : 0.0f;
		};

		return FTransform(
			FQuat(FRotator(-Read(Attribute + 4), Read(Attribute + 5), -Read(Attribute + 3))),
			FVector(Read(Attribute + 0), Read(Attribute + 1), Read(Attribute + 2)),
			FVector(Read(Attribute + 6), Read(Attribute + 7), Read(Attribute + 8))
		);
	}
}
#endif

UPoseAsset* ISkeletalMeshImporter::BakeDnaPoseAsset(USkeletalMesh* SkeletalMesh) {
#if REFLECTION_RIG_LOGIC
	UDNAAsset* DNAAsset = USkelMeshDNAUtils::GetMeshDNA(SkeletalMesh);
	if (DNAAsset == nullptr) return nullptr;

	const TSharedPtr<IDNAReader> Behavior = DNAAsset->GetBehaviorReader();
	if (!Behavior.IsValid()) return nullptr;

	USkeleton* Skeleton = SkeletalMesh->GetSkeleton();
	if (Skeleton == nullptr) return nullptr;

	const int32 ControlCount = Behavior->GetRawControlCount();
	if (ControlCount == 0) return nullptr;

	FRigLogic RigLogic(Behavior.Get());
	FRigInstance Instance(&RigLogic);

	/* A DNA names the joints it drives, and the mesh knows them as bones. Only those get a track:
	 * everything else stays wherever the pose it is played over left it. */
	const FReferenceSkeleton& RefSkeleton = Skeleton->GetReferenceSkeleton();

	TArray<FName> BoneNames;
	TArray<int32> BoneJoints;
	TArray<FTransform> BoneRestPose;

	for (uint16 Joint = 0; Joint < Behavior->GetJointCount(); ++Joint) {
		const FName BoneName(*Behavior->GetJointName(Joint));

		const int32 Bone = RefSkeleton.FindBoneIndex(BoneName);
		if (Bone == INDEX_NONE) continue;

		BoneNames.Add(BoneName);
		BoneJoints.Add(Joint);
		BoneRestPose.Add(RefSkeleton.GetRefBonePose()[Bone]);
	}

	if (BoneNames.Num() == 0) {
		FImportIssues::Report(
			EImportIssue::Data,
			TEXT("The DNA's joints aren't the mesh's bones"),
			TEXT("None of the joints the DNA drives are named in the skeleton, so a pose built out of it would move nothing.")
		);

		return nullptr;
	}

	/* One frame per control, and a first frame with the rig standing still for the rest of them to
	 * be measured against */
	const int32 FrameCount = ControlCount + 1;

	TArray<FName> PoseNames;
	PoseNames.Reserve(FrameCount);

	TArray<TArray<FVector>> PositionKeys;
	TArray<TArray<FQuat>> RotationKeys;
	TArray<TArray<FVector>> ScaleKeys;

	PositionKeys.SetNum(BoneNames.Num());
	RotationKeys.SetNum(BoneNames.Num());
	ScaleKeys.SetNum(BoneNames.Num());

	/* A pose is the bone where the skeleton put it, moved by what the control did. Writing the
	 * DNA's own neutral instead would drag every bone to wherever that rig had it, which is not
	 * where this skeleton is built or where the mesh is bound. */
	const auto WriteFrame = [&](const TArrayView<const float>& Delta) {
		for (int32 Bone = 0; Bone < BoneNames.Num(); ++Bone) {
			const FTransform Rest = BoneRestPose[Bone];
			const FTransform Moved = ComposeDnaDelta(Delta, BoneJoints[Bone] * GDnaJointAttributes);

			PositionKeys[Bone].Add(Rest.GetTranslation() + Moved.GetTranslation());
			RotationKeys[Bone].Add(Rest.GetRotation() * Moved.GetRotation());
			ScaleKeys[Bone].Add(Rest.GetScale3D() + Moved.GetScale3D());
		}
	};

	WriteFrame({});
	PoseNames.Add(TEXT("Neutral"));

	for (int32 Control = 0; Control < ControlCount; ++Control) {
		for (int32 Reset = 0; Reset < ControlCount; ++Reset) {
			Instance.SetRawControl(static_cast<uint16>(Reset), 0.0f);
		}

		Instance.SetRawControl(static_cast<uint16>(Control), 1.0f);

		RigLogic.Calculate(&Instance);

		WriteFrame(Instance.GetRawJointOutputs());

		/* A control is named with a dot between its group and itself, which reads as a path
		 * everywhere a curve name is typed */
		PoseNames.Add(FName(*Behavior->GetRawControlName(static_cast<uint16>(Control)).Replace(TEXT("."), TEXT("_"))));
	}

	/* A pose asset is built out of an animation, one pose per frame. The animation is a means to
	 * an end here, so it is left in the transient package and the link to it dropped below. */
	UAnimSequence* Sequence = NewObject<UAnimSequence>(GetTransientPackage(), NAME_None, RF_Transient);
	Sequence->SetSkeleton(Skeleton);

	{
		IAnimationDataController& Controller = Sequence->GetController();

		Controller.OpenBracket(NSLOCTEXT("Reflection", "BuildDnaPoses", "Building poses from DNA"), false);
		Controller.InitializeModel();

		Controller.SetFrameRate(FFrameRate(30, 1), false);
		Controller.SetNumberOfFrames(FFrameNumber(FMath::Max(1, FrameCount - 1)), false);

		for (int32 Bone = 0; Bone < BoneNames.Num(); ++Bone) {
			Controller.AddBoneCurve(BoneNames[Bone], false);
			Controller.SetBoneTrackKeys(BoneNames[Bone], PositionKeys[Bone], RotationKeys[Bone], ScaleKeys[Bone], false);
		}

		Controller.NotifyPopulated();
		Controller.CloseBracket(false);
	}

	const FString PoseAssetName = GetAssetName() + TEXT("_PoseAsset");
	const FString PoseAssetPath = FPackageName::GetLongPackagePath(GetPackage()->GetName()) / PoseAssetName;

	UPackage* PosePackage = CreatePackage(*PoseAssetPath);
	if (PosePackage == nullptr) return nullptr;

	UPoseAsset* PoseAsset = NewObject<UPoseAsset>(PosePackage, FName(*PoseAssetName), RF_Public | RF_Standalone);

	PoseAsset->SetSkeleton(Skeleton);
	PoseAsset->SetPreviewMesh(SkeletalMesh);
	PoseAsset->CreatePoseFromAnimation(Sequence, &PoseNames);

	/* Additive against the neutral frame, so the controls a face is driven by add up the way they
	 * do in the rig rather than each replacing the last */
	PoseAsset->ConvertSpace(true, 0);

	/* Built from the DNA, not from an animation that outlives this import */
	PoseAsset->SourceAnimation = nullptr;

	HandleAssetCreation(PoseAsset, PosePackage);

	if (GetSettings()->AssetSettings.SaveAssets) {
		SavePackage(PosePackage);
	}

	return PoseAsset;
#else
	return nullptr;
#endif
}

bool ISkeletalMeshImporter::Import() {
#if UE4_27_AND_UE5
	USkeletalMesh* SkeletalMesh = Create<USkeletalMesh>();
	if (SkeletalMesh == nullptr) return false;

	/* The reference pose comes with the skeleton, and everything below is skinned against it:
	 * the bone map a section names, the influences a vertex carries, the inverse matrices the
	 * renderer poses with. Without one there is no mesh to build. */
	USkeleton* Skeleton = ResolveSkeleton();

	if (Skeleton == nullptr) {
		FImportIssues::Report(
			EImportIssue::MissingAsset,
			TEXT("The mesh's skeleton isn't in this project"),
			TEXT("A skeletal mesh is skinned to its skeleton's reference pose, so the import stops here rather than building a mesh with no bones. Reflect the skeleton first.")
		);

		return false;
	}

	SkeletalMesh->SetSkeleton(Skeleton);
	SkeletalMesh->SetRefSkeleton(Skeleton->GetReferenceSkeleton());
	SkeletalMesh->CalculateInvRefMatrices();

	/* Before the geometry: a wedge names the slot it belongs to */
	const TArray<TSharedPtr<FJsonValue>>* Slots;

	if (GetAssetExport()->TryGetArrayField(TEXT("SkeletalMaterials"), Slots)) {
		BuildMaterialSlots(SkeletalMesh, *Slots);
	}

	/* Everything the mesh is besides its geometry: the LOD it starts drawing at, whether it was
	 * cooked with vertex colours, the physics it collides with. What this import builds itself, or
	 * what the tool below owns, is left out rather than written over. */
	GetObjectSerializer()->DeserializeObjectProperties(RemovePropertiesShared(GetAssetData(), {
		/* Built here, out of the geometry */
		"LODModels",
		"NaniteResources",
		"SourceModels",
		"SamplingInfo",

		/* Already set, and the reference in the export points at the cooked package */
		"Skeleton",

		/* The skeletal mesh data tool's, once the mesh exists */
		"Sockets",
		"MorphTargets",
		"SkinWeightProfiles",
	}), SkeletalMesh);

	/* Geometry is not in the export, so it is asked for by the path the game cooked it under.
	 * The export names that itself; only when it doesn't is the path the asset landed at turned
	 * back into one, which is a guess the moment an import is redirected somewhere else. */
	FString FetchPath = GetAssetExport()->HasField(TEXT("Package"))
		? GetAssetExport()->GetStringField(TEXT("Package"))
		: FString();

	if (FetchPath.IsEmpty()) {
		FetchPath = GetPackage()->GetPathName();

		FRRedirects::Reverse(FetchPath);
	}

	const TSharedPtr<FJsonObject> Geometry = Cloud::Export::GetLodModelBlocking(FetchPath);

	if (!Geometry.IsValid()) {
		FImportIssues::Report(
			EImportIssue::Failed,
			TEXT("No geometry from the Cloud"),
			TEXT("The Cloud has to be running, and the mesh needs cooked render data to read.")
		);

		return false;
	}

	const int32 BuiltLods = TMeshGeometry::RebuildLodModels(SkeletalMesh, Geometry);

	if (BuiltLods == 0) {
		FImportIssues::Report(
			EImportIssue::Failed,
			TEXT("The mesh has no LOD to build"),
			TEXT("Every LOD the payload carries was missing the vertices or the sections to describe it.")
		);

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

	/* Morph deltas come down the way the geometry does -- a cook quantizes them into the render
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
		if (ApplyDna(SkeletalMesh, FetchPath)) {
			/* Flattening the rig into poses is the setting's job */
			if (GetSettings()->AssetSettings.SkeletalMesh.BakeDnaToPoseAsset) {
				if (const UPoseAsset* PoseAsset = BakeDnaPoseAsset(SkeletalMesh)) {
					UE_LOG(LogReflection, Display, TEXT("\"%s\" baked %d pose(s) out of its DNA into \"%s\""),
						*GetAssetName(), PoseAsset->GetNumPoses(), *PoseAsset->GetName());
				}
			}
		} else {
			FImportIssues::Report(
				EImportIssue::Data,
				TEXT("The mesh's DNA didn't come back"),
				TEXT("The export hangs a DNA off this mesh, so it is a rigged head. Without the stream the mesh imports, but nothing drives its face.")
			);
		}
	}

	SkeletalMesh->Build();

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

	SkeletalMesh->CalculateInvRefMatrices();
	SkeletalMesh->PostEditChange();

	UE_LOG(LogReflection, Display, TEXT("\"%s\" built %d LOD(s) and %d morph target(s) against skeleton \"%s\""),
		*GetAssetName(), BuiltLods, SkeletalMesh->GetMorphTargets().Num(), *Skeleton->GetName());

	return OnAssetCreation(SkeletalMesh);
#else
	FImportIssues::Report(
		EImportIssue::Failed,
		TEXT("Skeletal meshes need 4.27 or newer"),
		TEXT("The imported model this builds into is not what earlier engines keep their geometry in.")
	);

	return false;
#endif
}
