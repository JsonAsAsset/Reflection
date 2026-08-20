/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/Mesh/SkeletalMeshImporter.h"
#include "Rendering/SkeletalMeshLODImporterData.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Engine/AssetUserData.h"

#if REFLECTION_RIG_LOGIC
#include "DNAAsset.h"
#include "DNAUtils.h"
#include "RigLogic.h"
#include "RigInstance.h"
#include "SkelMeshDNAUtils.h"
#include "FMemoryResource.h"
#include "riglogic/RigLogic.h"
#include "dna/BinaryStreamReader.h"
#include "dna/BinaryStreamWriter.h"
#include "trio/streams/MemoryStream.h"
#include "DNAToSkelMeshMap.h"
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
	/* Reflecting the same mesh twice lands on the package the first import wrote, and making an
	 * object over one already sitting there destroys the one already sitting there: the name is
	 * taken, so the engine tears the old mesh down where it stands and builds the new one in its
	 * place. The Content Browser draws that mesh through a component whose mesh object holds its
	 * render data by pointer, and nothing tells the pointer. The vertex factories that object made
	 * are then released against freed memory, the release is skipped, and deleting the object
	 * afterwards stops the editor on a render resource that was never released.
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
	SkeletalMesh->SetRefSkeleton(FReferenceSkeleton());
	SkeletalMesh->CalculateInvRefMatrices();

	/* Nothing to draw: the renderer is only asked to update a LOD the mesh says it has */
	SkeletalMesh->GetLODInfoArray().Empty();

	SkeletalMesh->ClearFlags(RF_Public | RF_Standalone);

	/* Not marked for destruction, however tempting: the Content Browser draws thumbnails through a
	 * component that keeps a mesh object of its own, and that object holds the mesh's render data
	 * by pointer. Marking the mesh clears the component's reference to it out from under that
	 * pointer on the next collection, and the vertex factories the mesh object made are then
	 * released against render data that is gone -- which the engine finds out about by deleting a
	 * render resource that was never released, and stops the editor over. Emptied of everything
	 * that can be drawn, it is collected on its own once the package below lets go of it. */

	/* What the reader watches to decide whether the import built anything worth keeping */
	SetAsset(nullptr);
}

USkeleton* ISkeletalMeshImporter::BuildSkeletonFromMesh(USkeletalMesh* SkeletalMesh) {
	if (SkeletalMesh == nullptr || SkeletalMesh->GetRefSkeleton().GetNum() == 0) return nullptr;

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

	UE_LOG(LogReflection, Display, TEXT("\"%s\" named no skeleton, so \"%s\" was made from its %d bone(s)"),
		*GetAssetName(), *SkeletonName, SkeletalMesh->GetRefSkeleton().GetNum());

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

	SkeletalMesh->SetMaterials(Materials);
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
		if (SkeletalMesh->IsLODImportedDataEmpty(LodIndex)) continue;
#endif

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

TArray<uint8> ISkeletalMeshImporter::RewriteDnaForAnimNode(USkeletalMesh* SkeletalMesh, const TArray<uint8>& Dna) {
#if REFLECTION_RIG_LOGIC
	/* A DNA asset is only ever read by the RigLogic anim node, and that node reads a joint's nine
	 * numbers in MetaHuman's axes: translation as (x, -y, z), rotation as a rotator of
	 * (-ry, -rz, rx). This data is not in those axes -- the pose the mesh is bound at is the same
	 * numbers read as (x, y, z) and (-ry, rz, -rx) -- so the node rests somewhere the mesh was
	 * never skinned and deforms the face.
	 *
	 * The node can't be changed, so the DNA is. Negating the three attributes the two readings
	 * disagree on cancels exactly: the node's own formula then lands on the pose the mesh is
	 * bound at. Translation Y, rotation X and rotation Z, wherever they appear -- in the neutral
	 * the rig rests at, and in the deltas every control drives. */
	static constexpr int32 AttributesPerJoint = 9;

	TArray<uint8> Source = Dna;

	auto InStream = rl4::makeScoped<trio::MemoryStream>();
	InStream->write(reinterpret_cast<const char*>(Source.GetData()), Source.Num());
	InStream->seek(0);

	auto Reader = rl4::makeScoped<dna::BinaryStreamReader>(
		InStream.get(), dna::DataLayer::All, dna::UnknownLayerPolicy::Preserve, 0u, FMemoryResource::Instance());

	Reader->read();

	if (!rl4::Status::isOk()) {
		UE_LOG(LogReflection, Warning, TEXT("Reading the DNA to rewrite it failed: %s"), ANSI_TO_TCHAR(rl4::Status::get().message));

		return Dna;
	}

	auto OutStream = rl4::makeScoped<trio::MemoryStream>();
	auto Writer = rl4::makeScoped<dna::BinaryStreamWriter>(OutStream.get(), FMemoryResource::Instance());

	Writer->setFrom(Reader.get(), dna::DataLayer::All, dna::UnknownLayerPolicy::Preserve, FMemoryResource::Instance());

	/* The pose the rig rests at. Not the DNA's own: the mesh's, taken from the package and written
	 * back in the node's axes, so the rig rests exactly where the mesh is bound for every joint it
	 * touches. The DNA and this skeleton disagree about the body -- arms, spine, neck -- and the
	 * node writes those joints too, so leaving them as the DNA had them drags the whole head. */
	const uint16 JointCount = Reader->getJointCount();

	const FReferenceSkeleton& RefSkeleton = SkeletalMesh->GetRefSkeleton();

	TArray<dna::Vector3> Translations;
	TArray<dna::Vector3> Rotations;

	Translations.Reserve(JointCount);
	Rotations.Reserve(JointCount);

	int32 Copied = 0;

	for (uint16 Joint = 0; Joint < JointCount; ++Joint) {
		const dna::StringView JointName = Reader->getJointName(Joint);

		const int32 Bone = RefSkeleton.FindBoneIndex(FName(FString(static_cast<int32>(JointName.size()), JointName.data())));

		if (Bone != INDEX_NONE) {
			const FTransform& Bound = RefSkeleton.GetRefBonePose()[Bone];
			const FRotator Rotation = Bound.GetRotation().Rotator();

			/* Inverted through the node's own reading: it takes translation as (x, -y, z) and
			 * rotation as a rotator of (-ry, -rz, rx), so these are the numbers that come back
			 * out of it as the pose the mesh is bound at */
			Translations.Add(dna::Vector3{
				static_cast<float>(Bound.GetTranslation().X),
				static_cast<float>(-Bound.GetTranslation().Y),
				static_cast<float>(Bound.GetTranslation().Z)
			});

			Rotations.Add(dna::Vector3{
				static_cast<float>(Rotation.Roll),
				static_cast<float>(-Rotation.Pitch),
				static_cast<float>(-Rotation.Yaw)
			});

			Copied++;

			continue;
		}

		/* A joint the skeleton has no bone for keeps what the DNA said, read the same way round */
		dna::Vector3 Translation = Reader->getNeutralJointTranslation(Joint);
		dna::Vector3 Rotation = Reader->getNeutralJointRotation(Joint);

		Translation.y = -Translation.y;

		Rotation.x = -Rotation.x;
		Rotation.z = -Rotation.z;

		Translations.Add(Translation);
		Rotations.Add(Rotation);
	}

	Writer->setNeutralJointTranslations(Translations.GetData(), JointCount);
	Writer->setNeutralJointRotations(Rotations.GetData(), JointCount);

	/* What the controls do on top of it. A joint group is a matrix of one row per attribute it
	 * drives, so a row is negated when the attribute it lands on is one of the three. */
	int32 NegatedRows = 0;

	for (uint16 Group = 0; Group < Reader->getJointGroupCount(); ++Group) {
		const auto Values = Reader->getJointGroupValues(Group);
		const auto Outputs = Reader->getJointGroupOutputIndices(Group);

		if (Values.size() == 0 || Outputs.size() == 0) continue;

		const int32 Columns = static_cast<int32>(Values.size() / Outputs.size());
		if (Columns == 0) continue;

		TArray<float> Patched;
		Patched.Append(Values.data(), static_cast<int32>(Values.size()));

		for (int32 Row = 0; Row < static_cast<int32>(Outputs.size()); ++Row) {
			const int32 Attribute = Outputs[Row] % AttributesPerJoint;

			if (Attribute != 1 && Attribute != 3 && Attribute != 5) continue;

			for (int32 Column = 0; Column < Columns; ++Column) {
				const int32 Index = Row * Columns + Column;

				if (Patched.IsValidIndex(Index)) {
					Patched[Index] = -Patched[Index];
				}
			}

			NegatedRows++;
		}

		Writer->setJointGroupValues(Group, Patched.GetData(), static_cast<uint32>(Patched.Num()));
	}

	Writer->write();

	if (!rl4::Status::isOk()) {
		UE_LOG(LogReflection, Warning, TEXT("Writing the rewritten DNA failed: %s"), ANSI_TO_TCHAR(rl4::Status::get().message));

		return Dna;
	}

	TArray<uint8> Rewritten;
	Rewritten.AddUninitialized(static_cast<int32>(OutStream->size()));

	OutStream->seek(0);
	OutStream->read(reinterpret_cast<char*>(Rewritten.GetData()), OutStream->size());

	UE_LOG(LogReflection, Display, TEXT("\"%s\" rewrote its DNA into the anim node's axes: %d joint(s), %d from the mesh's own bind pose, %d delta row(s)"),
		*GetAssetName(), JointCount, Copied, NegatedRows);

	return Rewritten;
#else
	return Dna;
#endif
}

bool ISkeletalMeshImporter::ApplyDna(USkeletalMesh* SkeletalMesh, const FString& FetchPath) {
#if REFLECTION_RIG_LOGIC
	TArray<uint8> Cooked = Cloud::Export::GetDnaBlocking(FetchPath);

	if (Cooked.Num() == 0) {
		return false;
	}

	/* A DNA kept in a package of its own has a few bytes of that package's own ahead of the stream,
	 * where one hung straight off the mesh starts at it. The reader wants the stream. */
	{
		int32 Start = INDEX_NONE;

		for (int32 Index = 0; Index + 2 < Cooked.Num() && Index < 64; ++Index) {
			if (Cooked[Index] == 'D' && Cooked[Index + 1] == 'N' && Cooked[Index + 2] == 'A') {
				Start = Index;

				break;
			}
		}

		if (Start == INDEX_NONE) {
			FImportIssues::Report(
				EImportIssue::Data,
				TEXT("The mesh's DNA isn't a DNA"),
				TEXT("What came back doesn't start with a DNA stream, so there is nothing RigLogic can read a face out of.")
			);

			return false;
		}

		if (Start > 0) {
			Cooked.RemoveAt(0, Start, EAllowShrinking::No);

			UE_LOG(LogReflection, Display, TEXT("\"%s\" trimmed %d byte(s) ahead of its DNA stream"), *GetAssetName(), Start);
		}
	}

	/* Rewritten into the axes the anim node reads, since that node is the only thing that ever
	 * reads a DNA asset */
	TArray<uint8> Dna = RewriteDnaForAnimNode(SkeletalMesh, Cooked);

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

	/* Some heads keep the DNA in a package of its own and only the definition with it: the names,
	 * the hierarchy and the pose, and a behavior layer that is a stub. There is a DNA on the mesh
	 * either way, and nothing in it for a rig to run. */
	if (Behavior->GetJointGroupCount() == 0) {
		FImportIssues::Report(
			EImportIssue::Data,
			TEXT("The mesh's DNA has no rig in it"),
			FString::Printf(
				TEXT("'%s' carries the face's joints and neutral pose but no behavior, so RigLogic has nothing to drive them with. The head imports and holds still."),
				*SkeletalMesh->GetName()
			)
		);
	}

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
	/* A joint's local transform: the DNA's neutral, then whatever the controls evaluated to on top.
	 *
	 * These are the signs FAnimNode_RigLogic uses, and they have to be: by the time anything reads
	 * a joint here the DNA has already been rewritten into that node's axes, so reading it any
	 * other way inverts every rotation and every sideways offset. */
	FTransform ComposeDnaJoint(const TArrayView<const float>& Neutral, const TArrayView<const float>& Delta, const int32 Attribute) {
		const auto Read = [](const TArrayView<const float>& Values, const int32 Index) {
			return Values.IsValidIndex(Index) ? Values[Index] : 0.0f;
		};

		const FVector Translation(
			Read(Neutral, Attribute + 0) + Read(Delta, Attribute + 0),
			-(Read(Neutral, Attribute + 1) + Read(Delta, Attribute + 1)),
			Read(Neutral, Attribute + 2) + Read(Delta, Attribute + 2)
		);

		const FQuat Rotation =
			FQuat(FRotator(-Read(Neutral, Attribute + 4), -Read(Neutral, Attribute + 5), Read(Neutral, Attribute + 3))) *
			FQuat(FRotator(-Read(Delta, Attribute + 4), -Read(Delta, Attribute + 5), Read(Delta, Attribute + 3)));

		const FVector Scale(
			Read(Neutral, Attribute + 6) + Read(Delta, Attribute + 6),
			Read(Neutral, Attribute + 7) + Read(Delta, Attribute + 7),
			Read(Neutral, Attribute + 8) + Read(Delta, Attribute + 8)
		);

		return FTransform(Rotation, Translation, Scale);
	}
}
#endif

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

	SkeletalMesh->SetRefSkeleton(BindPose);

	UE_LOG(LogReflection, Display, TEXT("\"%s\" bound at its own pose, %d bone(s)"), *GetAssetName(), BindPose.GetNum());

	return true;
}

bool ISkeletalMeshImporter::AlignBindPoseToDna(USkeletalMesh* SkeletalMesh) {
#if REFLECTION_RIG_LOGIC
	UDNAAsset* DNAAsset = USkelMeshDNAUtils::GetMeshDNA(SkeletalMesh);
	if (DNAAsset == nullptr) return false;

	const TSharedPtr<IDNAReader> Behavior = DNAAsset->GetBehaviorReader();
	if (!Behavior.IsValid()) return false;

	USkeleton* Skeleton = SkeletalMesh->GetSkeleton();
	if (Skeleton == nullptr) return false;

	/* RigLogic doesn't add to the pose a bone is already in, it puts the bone where the DNA's
	 * neutral says. A mesh bound anywhere else wears that difference as a deformation for as long
	 * as the rig runs, which is what a head bound to the shared skeleton rather than its own does.
	 *
	 * The engine has USkelMeshDNAUtils::UpdateJoints for this, and it can't be used here: it
	 * rebuilds the whole chain in component space off the DNA's own root, which is written in the
	 * DNA's axes, and this DNA is not in the ones that code was written for. It lands the skeleton
	 * on its side. Only the joints are taken, in the frame the skeleton already has them. */
	/* The same numbers the rig evaluates against, rather than the reader's own accessors: those
	 * two do not report a rotation the same way round, and this is the pair that has to agree. */
	FRigLogic RigLogic(Behavior.Get());

	const TArrayView<const float> Neutral = RigLogic.GetRawNeutralJointValues();

	int32 Aligned = 0;

	{
	FReferenceSkeletonModifier Modifier(SkeletalMesh->GetRefSkeleton(), Skeleton);

	for (uint16 Joint = 0; Joint < Behavior->GetJointCount(); ++Joint) {
		/* The DNA roots its hierarchy at a joint that is its own parent, and that one holds where
		 * the rig sits rather than how a bone is offset from its parent. Read as an offset it
		 * throws the skeleton clean out of its own mesh, so the game's placement is kept. */
		if (Behavior->GetJointParentIndex(Joint) == Joint) continue;

		const int32 Bone = SkeletalMesh->GetRefSkeleton().FindBoneIndex(FName(*Behavior->GetJointName(Joint)));
		if (Bone == INDEX_NONE) continue;

		/* Where the rig rests, which is where the mesh has to be bound */
		Modifier.UpdateRefPoseTransform(Bone, ComposeDnaJoint(Neutral, {}, Joint * GDnaJointAttributes));

		Aligned++;
	}

	}

	SkeletalMesh->GetRefBasesInvMatrix().Reset();

	UE_LOG(LogReflection, Display, TEXT("\"%s\" bound %d joint(s) to its DNA's neutral"), *GetAssetName(), Aligned);

	return Aligned > 0;
#else
	return false;
#endif
}

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

	/* Controls with no joint groups behind them evaluate to nothing, and asking the rig to run
	 * anyway walks off the end of tables it never built */
	if (Behavior->GetJointGroupCount() == 0 || Behavior->GetLODCount() == 0) {
		return nullptr;
	}

	FRigLogic RigLogic(Behavior.Get());
	FRigInstance Instance(&RigLogic);

	const TArrayView<const float> Neutral = RigLogic.GetRawNeutralJointValues();

	/* A DNA names the joints it drives, and the mesh knows them as bones. Only those get a track:
	 * everything else stays wherever the pose it is played over left it. */
	const FReferenceSkeleton& RefSkeleton = Skeleton->GetReferenceSkeleton();

	TArray<FName> BoneNames;
	TArray<int32> BoneJoints;

	for (uint16 Joint = 0; Joint < Behavior->GetJointCount(); ++Joint) {
		/* Skipped for the same reason the bind pose skips it: the DNA's root holds a placement,
		 * and a pose that wrote it as a bone offset would take the head with it */
		if (Behavior->GetJointParentIndex(Joint) == Joint) continue;

		const FName BoneName(*Behavior->GetJointName(Joint));

		const int32 Bone = RefSkeleton.FindBoneIndex(BoneName);
		if (Bone == INDEX_NONE) continue;

		BoneNames.Add(BoneName);
		BoneJoints.Add(Joint);
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

	/* The pose the rig itself would write for that control, so a pose and the rig agree bone for
	 * bone. The mesh is bound to the same neutral these are composed from. */
	const auto WriteFrame = [&](const TArrayView<const float>& Delta) {
		for (int32 Bone = 0; Bone < BoneNames.Num(); ++Bone) {
			const FTransform Local = ComposeDnaJoint(Neutral, Delta, BoneJoints[Bone] * GDnaJointAttributes);

			PositionKeys[Bone].Add(Local.GetTranslation());
			RotationKeys[Bone].Add(Local.GetRotation());
			ScaleKeys[Bone].Add(Local.GetScale3D());
		}
	};

	WriteFrame({});
	PoseNames.Add(TEXT("base_pose"));

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

	/* A pose asset is built out of an animation, one pose per frame, and keeps pointing at it: the
	 * poses can be rebuilt from the animation, and the animation is the only place the frames stay
	 * readable once the poses are additive. Both land beside the mesh. */
	const FString Folder = FPackageName::GetLongPackagePath(GetPackage()->GetName());

	const FString SequenceName = GetAssetName() + TEXT("_DNA_Facial_Pose_Export");
	const FString PoseAssetName = SequenceName + TEXT("_PoseAsset");

	UPackage* SequencePackage = CreatePackage(*(Folder / SequenceName));
	if (SequencePackage == nullptr) return nullptr;

	SequencePackage->FullyLoad();

	UAnimSequence* Sequence = NewObject<UAnimSequence>(SequencePackage, FName(*SequenceName), RF_Public | RF_Standalone);
	Sequence->SetSkeleton(Skeleton);
	Sequence->SetPreviewMesh(SkeletalMesh);

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

	UPackage* PosePackage = CreatePackage(*(Folder / PoseAssetName));
	if (PosePackage == nullptr) return nullptr;

	/* Re-importing lands on the package the last import wrote, and a package read back off disk
	 * comes in on demand: saving one that was never finished reading is fatal */
	PosePackage->FullyLoad();

	UPoseAsset* PoseAsset = NewObject<UPoseAsset>(PosePackage, FName(*PoseAssetName), RF_Public | RF_Standalone);

	PoseAsset->SetSkeleton(Skeleton);
	PoseAsset->SetPreviewMesh(SkeletalMesh);
	PoseAsset->CreatePoseFromAnimation(Sequence, &PoseNames);

	/* Additive against the base pose, so the controls a face is driven by add up the way they do in
	 * the rig rather than each replacing the last */
	PoseAsset->ConvertSpace(true, 0);

	PoseAsset->SourceAnimation = Sequence;

	HandleAssetCreation(Sequence, SequencePackage);
	HandleAssetCreation(PoseAsset, PosePackage);

	if (GetSettings()->AssetSettings.SaveAssets) {
		SavePackage(SequencePackage);
		SavePackage(PosePackage);
	}

	return PoseAsset;
#else
	return nullptr;
#endif
}

bool ISkeletalMeshImporter::Import() {
#if UE4_27_AND_UE5
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
		SkeletalMesh->SetSkeleton(Skeleton);
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

		SkeletalMesh->SetRefSkeleton(Skeleton->GetReferenceSkeleton());
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

		SkeletalMesh->SetSkeleton(Skeleton);
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
			/* Only when the mesh's own bind pose could not be had: reconstructing it from the
			 * DNA is a guess at what the package already states outright */
			if (!bCookedBindPose) {
				AlignBindPoseToDna(SkeletalMesh);
			}

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
		SkeletalMesh->SetMinLod(FPerPlatformInt(0));

#if ENGINE_UE5
		FPerQualityLevelInt MinQualityLevelLod = SkeletalMesh->GetQualityLevelMinLod();

		MinQualityLevelLod.Default = 0;

		SkeletalMesh->SetQualityLevelMinLod(MinQualityLevelLod);
#endif
	}

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
