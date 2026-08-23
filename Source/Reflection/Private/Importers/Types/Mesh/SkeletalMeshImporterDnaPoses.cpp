/* Copyright Reflection Contributors 2024-2026 */

/* A MetaHuman face for an engine that cannot read one.
 *
 * RigLogic is what turns a head's controls into joints, and it ships with Unreal Engine 5. Older
 * engines have nothing that reads a DNA at all, so the face is flattened into a pose asset instead:
 * one pose per control, holding the joints that control moves. An animation graph drives those
 * poses off the same curves the rig would have read, and the head moves without the rig.
 *
 * The numbers come from the Cloud already worked out, which is what makes this possible here: the
 * arithmetic that needs RigLogic happens where RigLogic is, and what arrives is a list of joints. */

#include "Importers/Types/Mesh/SkeletalMeshImporter.h"

#include "Animation/AnimSequence.h"
#include "Animation/PoseAsset.h"
#include "Animation/Skeleton.h"
#include "Engine/EngineUtilities.h"
#include "Engine/SkeletalMesh.h"

#if ENGINE_UE5 && ENGINE_MINOR_VERSION >= 2
#include "Animation/AnimData/IAnimationDataController.h"
#endif

#include "Modules/Cloud/Cloud.h"
#include "Settings/Types/DNASettings.h"
#include "Utilities/JsonHelpers.h"

namespace {
	/* 4.26 is where the mesh's members went behind accessors */
	USkeleton* PoseMeshSkeleton(USkeletalMesh* Mesh) {
#if UE4_25_BELOW
		return Mesh->Skeleton;
#else
		return Mesh->GetSkeleton();
#endif
	}

	/* The mesh's own bind pose, which is not the one its skeleton carries: a skeleton is shared
	 * across a family of heads and rests where none of them quite does, and the difference is
	 * centimetres. The mesh is what the face was skinned against. */
	const FReferenceSkeleton& PoseMeshRefSkeleton(const USkeletalMesh* Mesh) {
#if UE4_25_BELOW
		return Mesh->RefSkeleton;
#else
		return Mesh->GetRefSkeleton();
#endif
	}

	/* What a control drives, put into the axes the neutral is read in.
	 *
	 * A DNA's neutral and its deltas do not survive the same reading. The RigLogic anim node reads
	 * a joint as (x, -y, z) and a rotator of (-ry, -rz, rx), and the importer that feeds it rewrites
	 * the DNA to suit: the neutral it swaps out for the pose the mesh is bound at, and the deltas it
	 * negates on translation Y, rotation X and rotation Z. Only the second half applies here, since
	 * the neutral being read is the DNA's own and already lands where it should.
	 *
	 * So the same three attributes are negated here, and the reading below is left alone. Doing it
	 * to both instead mirrors every bone in the face. */
	void ToAnimNodeAxes(TArray<float>& Delta) {
		for (const int32 Attribute : { 1, 3, 5 }) {
			if (Delta.IsValidIndex(Attribute)) {
				Delta[Attribute] = -Delta[Attribute];
			}
		}
	}

	/* A joint's local transform: the pose the mesh rests at, then whatever a control did on top.
	 *
	 * Rested at the mesh's own bind pose rather than the DNA's neutral, because those two disagree
	 * and the mesh is the one that is right: a DNA and the skeleton it ends up on differ over the
	 * body joints by more than a centimetre each, and the importer that feeds RigLogic deals with
	 * that the same way, writing the bind pose over the DNA's neutral before the rig ever runs.
	 *
	 * It only shows in the animation. The pose asset built from it is made additive against its
	 * first frame, so whatever the rest pose is cancels there and a wrong one is invisible; the
	 * animation keeps it, and every frame of it is off by however wrong that pose was.
	 *
	 * Nine attributes a joint. A head whose rig writes rotations as quaternions spreads a joint over
	 * ten instead, but the Cloud puts those back before sending them, so what arrives is always
	 * translation, euler rotation and scale, three each. */
	FTransform ComposeDnaPoseJoint(const FTransform& Bind, const TArray<float>& Delta) {
		const auto Read = [&Delta](const int32 Index) {
			return Delta.IsValidIndex(Index) ? Delta[Index] : 0.0f;
		};

		const FVector Translation = Bind.GetTranslation() + FVector(Read(0), -Read(1), Read(2));

		const FQuat Rotation = Bind.GetRotation() * FQuat(FRotator(-Read(4), -Read(5), Read(3)));

		const FVector Scale = Bind.GetScale3D() + FVector(Read(6), Read(7), Read(8));

		return FTransform(Rotation, Translation, Scale);
	}

	TArray<float> ReadFloats(const TSharedPtr<FJsonValue>& Value) {
		TArray<float> Floats;

		const TArray<TSharedPtr<FJsonValue>>* Entries;

		if (Value.IsValid() && Value->TryGetArray(Entries)) {
			Floats.Reserve(Entries->Num());

			for (const TSharedPtr<FJsonValue>& Entry : *Entries) {
				Floats.Add(static_cast<float>(Entry->AsNumber()));
			}
		}

		return Floats;
	}
}

UPoseAsset* ISkeletalMeshImporter::BakeDnaPoseAssetFromCloud(USkeletalMesh* SkeletalMesh, const FString& FetchPath) {
	USkeleton* Skeleton = SkeletalMesh != nullptr ? PoseMeshSkeleton(SkeletalMesh) : nullptr;
	if (Skeleton == nullptr) return nullptr;

	/* Backporting names an older head's curves and says what each is made of, which the Cloud
	 * resolves against this rig: several controls at once is where the correctives fire, so those
	 * poses have to be evaluated there rather than added up from single control ones here. */
	const FRDnaBackportSettings& Backport = GetSettings()->AssetSettings.DNA.Backport;

	const FString Mapping = Backport.BackportPoses ? Backport.CurveMapping : FString();

	FString Strengths;

	if (!Mapping.IsEmpty() && Backport.AdjustPoseStrengths) {
		for (const TPair<FString, float>& Strength : Backport.PoseStrengths) {
			Strengths += FString::Printf(TEXT("%s%s:%g"), Strengths.IsEmpty() ? TEXT("") : TEXT(","), *Strength.Key, Strength.Value);
		}
	}

	const TSharedPtr<FJsonObject> Response = Cloud::Export::GetDnaPosesBlocking(FetchPath, Mapping, Strengths);

	if (!Response.IsValid()) {
		FImportIssues::Report(
			EImportIssue::Data,
			TEXT("The head's poses didn't come back"),
			TEXT("This engine has no RigLogic, so the face has to arrive already worked out, and the Cloud returned nothing for it. The mesh imports and its face stays still.")
		);

		return nullptr;
	}

	const TArray<TSharedPtr<FJsonValue>>* JointsJson = nullptr;
	const TArray<TSharedPtr<FJsonValue>>* PosesJson = nullptr;

	if (
		!Response->TryGetArrayField(TEXT("joints"), JointsJson) ||
		!Response->TryGetArrayField(TEXT("poses"), PosesJson) ||
		PosesJson->Num() == 0
	) {
		return nullptr;
	}

	/* A joint that hangs off itself is the DNA's root, and it holds a placement rather than
	 * anything the face moves. Written as a bone offset it would take the whole head with it. */
	const TArray<TSharedPtr<FJsonValue>>* ParentsJson = nullptr;
	Response->TryGetArrayField(TEXT("parents"), ParentsJson);

	/* Every pose, read before any track is made, because which joints the face actually drives is
	 * what decides which bones get one */
	TArray<FString> PoseLabels;
	TArray<TMap<int32, TArray<float>>> PoseDeltas;

	TSet<int32> DrivenJoints;

	for (const TSharedPtr<FJsonValue>& Entry : *PosesJson) {
		const TSharedPtr<FJsonObject> Pose = Entry->AsObject();
		if (!Pose.IsValid()) continue;

		TMap<int32, TArray<float>> Deltas;

		const TArray<TSharedPtr<FJsonValue>>* Moved = nullptr;

		if (Pose->TryGetArrayField(TEXT("joints"), Moved)) {
			for (const TSharedPtr<FJsonValue>& JointEntry : *Moved) {
				const TSharedPtr<FJsonObject> Joint = JointEntry->AsObject();
				if (!Joint.IsValid()) continue;

				TArray<float> Values = ReadFloats(Joint->TryGetField(TEXT("values")));

				ToAnimNodeAxes(Values);

				const int32 Index = Joint->GetIntegerField(TEXT("index"));

				DrivenJoints.Add(Index);
				Deltas.Add(Index, MoveTemp(Values));
			}
		}

		/* A control is named with a dot between its group and itself, which reads as a path
		 * everywhere a curve name is typed */
		PoseLabels.Add(Pose->GetStringField(TEXT("name")).Replace(TEXT("."), TEXT("_")));
		PoseDeltas.Add(MoveTemp(Deltas));
	}

	/* Only the joints the face actually moves get a track, and only where the mesh has a bone for
	 * them and the skeleton knows the name, or the track maps to nothing when the animation plays.
	 *
	 * A DNA names the body joints its facial hierarchy hangs off as well, and no control touches
	 * them. Giving those a track writes the head mesh's own idea of where the spine, clavicles and
	 * arms belong into every frame, which is nowhere near where the body has them: this head's
	 * spine sits over a metre from the DNA's. It never showed in the pose asset, because a bone
	 * holding one value in every frame cancels the moment the poses are made additive, and it is
	 * the animation underneath that ends up dragging the body around. */
	const FReferenceSkeleton& RefSkeleton = PoseMeshRefSkeleton(SkeletalMesh);
	const FReferenceSkeleton& SkeletonBones = Skeleton->GetReferenceSkeleton();

	TArray<FName> BoneNames;
	TArray<int32> BoneJoints;
	TArray<FTransform> BoneBindPose;

	int32 Untouched = 0;

	for (int32 Joint = 0; Joint < JointsJson->Num(); ++Joint) {
		if (!DrivenJoints.Contains(Joint)) {
			++Untouched;

			continue;
		}

		const FName BoneName(*(*JointsJson)[Joint]->AsString());

		const int32 Bone = RefSkeleton.FindBoneIndex(BoneName);

		if (Bone == INDEX_NONE || SkeletonBones.FindBoneIndex(BoneName) == INDEX_NONE) continue;

		BoneNames.Add(BoneName);
		BoneJoints.Add(Joint);
		BoneBindPose.Add(RefSkeleton.GetRefBonePose()[Bone]);
	}

	/* Asked for and not got, which is the mapping being wrong rather than the head */
	if (!Mapping.IsEmpty() && !Response->GetBoolField(TEXT("backported"))) {
		FImportIssues::Report(
			EImportIssue::Data,
			TEXT("The backport mapping didn't resolve"),
			FString::Printf(
				TEXT("'%s' is what says how this rig's controls line up with the older head's curves. Nothing in it named a control this head has, so its own controls are baked instead."),
				*Mapping)
		);
	}

	if (BoneNames.Num() == 0) {
		FImportIssues::Report(
			EImportIssue::Data,
			TEXT("The head's joints aren't the mesh's bones"),
			TEXT("None of the joints the face drives are named in the skeleton, so a pose built out of them would move nothing.")
		);

		return nullptr;
	}

	/* One frame a pose, and a first frame with nothing driven for the rest to be measured against */
	const int32 FrameCount = PoseDeltas.Num() + 1;

	TArray<FSmartName> PoseNames;
	PoseNames.Reserve(FrameCount);

	TArray<TArray<FVector>> PositionKeys;
	TArray<TArray<FQuat>> RotationKeys;
	TArray<TArray<FVector>> ScaleKeys;

	PositionKeys.SetNum(BoneNames.Num());
	RotationKeys.SetNum(BoneNames.Num());
	ScaleKeys.SetNum(BoneNames.Num());

	/* A pose name has to be on the skeleton before a pose can be called it */
	const auto NamePose = [Skeleton, &PoseNames](const FString& Name) {
		FSmartName Smart;

		Skeleton->AddSmartNameAndModify(USkeleton::AnimCurveMappingName, FName(*Name), Smart);
		PoseNames.Add(Smart);
	};

	const auto WriteFrame = [&](const TMap<int32, TArray<float>>& Deltas) {
		static const TArray<float> None;

		for (int32 Bone = 0; Bone < BoneNames.Num(); ++Bone) {
			const TArray<float>* Delta = Deltas.Find(BoneJoints[Bone]);

			const FTransform Local = ComposeDnaPoseJoint(BoneBindPose[Bone], Delta != nullptr ? *Delta : None);

			PositionKeys[Bone].Add(Local.GetTranslation());
			RotationKeys[Bone].Add(Local.GetRotation());
			ScaleKeys[Bone].Add(Local.GetScale3D());
		}
	};

	WriteFrame({});
	NamePose(TEXT("base_pose"));

	for (int32 Pose = 0; Pose < PoseDeltas.Num(); ++Pose) {
		WriteFrame(PoseDeltas[Pose]);
		NamePose(PoseLabels[Pose]);
	}

	/* A pose asset is built out of an animation, one pose a frame, and keeps pointing at it: the
	 * poses can be rebuilt from the animation, and the animation is the only place the frames stay
	 * readable once the poses are additive. Both land beside the mesh. */
	const FString Folder = FPackageName::GetLongPackagePath(GetPackage()->GetName());

	/* Whether the mapping actually resolved, rather than whether one was asked for: a mapping this
	 * head has nothing in common with falls back to the rig's own controls, and naming that a
	 * backport would put the wrong poses behind the right name. */
	const bool bBackported = Response->GetBoolField(TEXT("backported"));

	const FString SequenceName = GetAssetName() + (bBackported
		? TEXT("_DNA_Facial_Pose_Backport")
		: TEXT("_DNA_Facial_Pose_Export"));
	const FString PoseAssetName = SequenceName + TEXT("_PoseAsset");

	UPackage* SequencePackage = CreatePackage(
		/* 4.25, 4.26.0 and below need an Outer */
#if UE4_25_BELOW || (UE4_26_0)
		nullptr,
#endif
		*(Folder / SequenceName));
	if (SequencePackage == nullptr) return nullptr;

	/* Re-importing lands on the package the last import wrote, and a package read back off disk
	 * comes in on demand: saving one that was never finished reading is fatal */
	SequencePackage->FullyLoad();

	UAnimSequence* Sequence = NewObject<UAnimSequence>(SequencePackage, FName(*SequenceName), RF_Public | RF_Standalone);
	Sequence->SetSkeleton(Skeleton);
	Sequence->SetPreviewMesh(SkeletalMesh);

	constexpr int32 FrameRate = 30;

/* 5.2 is where raw animation data moved behind the data model and its controller */
#if ENGINE_UE5 && ENGINE_MINOR_VERSION >= 2
	{
		IAnimationDataController& Controller = Sequence->GetController();

		Controller.OpenBracket(NSLOCTEXT("Reflection", "BuildDnaPoses", "Building poses from DNA"), false);
		Controller.InitializeModel();

		Controller.SetFrameRate(FFrameRate(FrameRate, 1), false);
		Controller.SetNumberOfFrames(FFrameNumber(FMath::Max(1, FrameCount - 1)), false);

		for (int32 Bone = 0; Bone < BoneNames.Num(); ++Bone) {
			Controller.AddBoneCurve(BoneNames[Bone], false);
			Controller.SetBoneTrackKeys(BoneNames[Bone], PositionKeys[Bone], RotationKeys[Bone], ScaleKeys[Bone], false);
		}

		Controller.NotifyPopulated();
		Controller.CloseBracket(false);
	}
#elif ENGINE_UE4
	Sequence->SetRawNumberOfFrame(FrameCount);
	Sequence->SequenceLength = FrameCount > 1 ? static_cast<float>(FrameCount - 1) / FrameRate : 1.0f;

	for (int32 Bone = 0; Bone < BoneNames.Num(); ++Bone) {
		FRawAnimSequenceTrack Track;

		Track.PosKeys = PositionKeys[Bone];
		Track.RotKeys = RotationKeys[Bone];
		Track.ScaleKeys = ScaleKeys[Bone];

		Sequence->AddNewRawTrack(BoneNames[Bone], &Track);
	}

	Sequence->PostProcessSequence();
#else
	/* 5.0 and 5.1 have the data model but not the controller surface used above */
	UE_LOG(LogReflection, Warning, TEXT("\"%s\" cannot be built on this engine version"), *SequenceName);

	return nullptr;
#endif

	UPackage* PosePackage = CreatePackage(
		/* 4.25, 4.26.0 and below need an Outer */
#if UE4_25_BELOW || (UE4_26_0)
		nullptr,
#endif
		*(Folder / PoseAssetName));
	if (PosePackage == nullptr) return nullptr;

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

	UE_LOG(LogReflection, Display,
		TEXT("\"%s\" built %d pose(s) over %d bone(s) out of the Cloud's reading of its DNA%s"),
		*GetAssetName(), PoseDeltas.Num(), BoneNames.Num(),
		bBackported ? *FString::Printf(TEXT(", backported from \"%s\""), *FPaths::GetBaseFilename(Mapping)) : TEXT(""));

	/* Worth saying, since it is the difference between a head that animates and one that drags the
	 * body with it */
	if (Untouched > 0) {
		UE_LOG(LogReflection, Display,
			TEXT("\"%s\" left %d joint(s) untracked, no control of this face moves them"),
			*GetAssetName(), Untouched);
	}

	return PoseAsset;
}
