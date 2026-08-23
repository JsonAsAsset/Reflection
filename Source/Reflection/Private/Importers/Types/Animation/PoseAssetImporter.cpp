/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/Animation/PoseAssetImporter.h"
#include "Animation/AnimSequence.h"
#include "Animation/PoseAsset.h"
#include "Engine/EngineUtilities.h"

/* AssetRegistryModule.h only moved under an AssetRegistry/ folder later on */
#if UE4_25_BELOW
#include "AssetRegistryModule.h"
#else
#include "AssetRegistry/AssetRegistryModule.h"
#endif

#if ENGINE_UE5 && ENGINE_MINOR_VERSION >= 2
#include "Animation/AnimData/IAnimationDataController.h"
#endif

#include "Modules/Cloud/Cloud.h"
#include "Modules/Cloud/Remote.h"
#include "Utilities/JsonHelpers.h"

UObject* IPoseAssetImporter::CreateAsset(UObject* CreatedAsset) {
	return IImporter::CreateAsset(NewObject<UPoseAsset>(GetPackage(), UPoseAsset::StaticClass(), *GetAssetName(), RF_Standalone | RF_Public));
}

bool IPoseAssetImporter::Import() {
	PoseAsset = Create<UPoseAsset>();

	/* Set Skeleton, so we can use it in the de-cooking process */
	GetObjectSerializer()->DeserializeObjectProperties(KeepPropertiesShared(GetAssetData(),
	{
		"Skeleton",
		"bAdditivePose"
	}), PoseAsset);

	/* Reverse LocalSpacePose (cooked data) back to source data */
	ReverseCookLocalSpacePose(PoseAsset->GetSkeleton());

	/* Final operation to set properties */
	GetObjectSerializer()->DeserializeObjectProperties(GetAssetData(), PoseAsset);

	/* If the user wants to specify a pose asset animation */
	if (UAnimSequence* OptionalAnimationSequence = GetSelectedAsset<UAnimSequence>(true)) {
		PoseAsset->SourceAnimation = OptionalAnimationSequence;
	}
	
	return OnAssetCreation(PoseAsset);
}

void IPoseAssetImporter::ReverseCookLocalSpacePose(USkeleton* Skeleton) {
	/* If PoseContainer or Tracks don't exist, no need to perform any operations */
	if (
		!GetAssetData()->HasField(TEXT("PoseContainer")) ||
		!GetAssetData()->GetObjectField(TEXT("PoseContainer"))->HasField(TEXT("Tracks"))
	) {
		return;
	}
	
	const TSharedPtr<FJsonObject> PoseContainer = GetAssetData()->GetObjectField(TEXT("PoseContainer"));
	const TArray<TSharedPtr<FJsonValue>> TracksJson = PoseContainer->GetArrayField(TEXT("Tracks"));

	/* Guarded rather than assumed, GetArrayField is fatal on a missing field */
	TArray<TSharedPtr<FJsonValue>> PosesJson; {
		if (PoseContainer->HasField(TEXT("Poses"))) {
			PosesJson = PoseContainer->GetArrayField(TEXT("Poses"));
		}
	}

	const int32 NumTracks = TracksJson.Num();

	/* Which pose holds which track, the way 5.x cooks it.
	 *
	 * TrackPoseInfluenceIndices[Track] lists every pose that moves that track, as a pose index
	 * paired with the slot the transform sits in inside that pose's LocalSpacePose. Older data
	 * carried the same mapping the other way round, per pose, as TrackToBufferIndex. */
	TMap<int32, TMap<int32, int32>> PoseToTrackBuffer; {
		if (PoseContainer->HasField(TEXT("TrackPoseInfluenceIndices"))) {
			const TArray<TSharedPtr<FJsonValue>> InfluenceIndicesJson = PoseContainer->GetArrayField(TEXT("TrackPoseInfluenceIndices"));

			for (int32 TrackIndex = 0; TrackIndex < InfluenceIndicesJson.Num(); TrackIndex++) {
				const TSharedPtr<FJsonObject> InfluencesObject = InfluenceIndicesJson[TrackIndex]->AsObject();
				if (!InfluencesObject.IsValid() || !InfluencesObject->HasField(TEXT("Influences"))) continue;

				for (const TSharedPtr<FJsonValue>& InfluenceValue : InfluencesObject->GetArrayField(TEXT("Influences"))) {
					const TSharedPtr<FJsonObject> Influence = InfluenceValue->AsObject();
					if (!Influence.IsValid()) continue;

					int32 PoseIndex = INDEX_NONE;
					int32 BoneTransformIndex = INDEX_NONE;

					if (!Influence->TryGetNumberField(TEXT("PoseIndex"), PoseIndex)) continue;
					if (!Influence->TryGetNumberField(TEXT("BoneTransformIndex"), BoneTransformIndex)) continue;

					PoseToTrackBuffer.FindOrAdd(PoseIndex).Add(TrackIndex, BoneTransformIndex);
				}
			}
		}
	}

	/* What the poses were actually taken against, best first */
	const TMap<FName, FTransform> MeshBindPose = GetSourceMeshBindPose();
	const TMap<FName, FTransform> CookedReferencePose = GetCookedReferencePose();

	/* Curves have a cooked and a source side just like the transforms do, and the engine reads the
	 * source one back when it rebuilds an additive pose:
	 *
	 *     GetBasePoseTransform  -> OutCurve = Poses[BasePoseIndex].SourceCurveData
	 *     ConvertToAdditivePose -> check(InBaseCurve.Num() == Curves.Num())
	 *
	 * Leaving it empty trips that check during PostLoad, so it is filled from the cooked side. */
	int32 NumCurves = 0; {
		if (PoseContainer->HasField(TEXT("Curves"))) {
			NumCurves = PoseContainer->GetArrayField(TEXT("Curves")).Num();
		}
	}

	for (int32 PoseIndex = 0; PoseIndex < PosesJson.Num(); PoseIndex++) {
		TSharedPtr<FJsonObject> Pose = PosesJson[PoseIndex]->AsObject();

		if (!Pose.IsValid()) {
			continue;
		}

		/* Read the optimized LocalSpacePose array */
		TArray<TSharedPtr<FJsonValue>> LocalSpacePoseJson; {
			if (Pose->HasField(TEXT("LocalSpacePose"))) {
				LocalSpacePoseJson = Pose->GetArrayField(TEXT("LocalSpacePose"));
			}
		}

		/* Build a mapping from track index (as int32) to index into LocalSpacePose. */
		TMap<int32, int32> TrackToBufferIndex; {
			if (const TMap<int32, int32>* Influences = PoseToTrackBuffer.Find(PoseIndex)) {
				TrackToBufferIndex = *Influences;
			}
			else if (Pose->HasField(TEXT("TrackToBufferIndex"))) {
				const TArray<TSharedPtr<FJsonValue>> TrackToBufferJson = Pose->GetArrayField(TEXT("TrackToBufferIndex"));

				for (TSharedPtr<FJsonValue> TrackToBuffer : TrackToBufferJson) {
					const TSharedPtr<FJsonObject> TrackToBufferObject = TrackToBuffer->AsObject();

					if (TrackToBufferObject.IsValid()) {
						int32 Key = FCString::Atoi(*TrackToBufferObject->GetStringField(TEXT("Key")));
						int32 Value = FCString::Atoi(*TrackToBufferObject->GetStringField(TEXT("Value")));

						TrackToBufferIndex.Add(Key, Value);
					}
				}
			}
		}

		/* We take the cooked pose data [LocalSpacePoseJson] and convert it back to SourceLocalSpacePose */
		TArray<TSharedPtr<FJsonValue>> SourceLocalSpacePose;
		SourceLocalSpacePose.SetNum(NumTracks);

		for (int32 i = 0; i < NumTracks; i++) {
			const FName TrackName = FName(*TracksJson[i]->AsString());

			/* DefaultTransform can either be default, or extracted from the base skeleton */
			FTransform DefaultTransform = FTransform::Identity; {
				if (const FTransform* BoundTransform = MeshBindPose.Find(TrackName)) {
					DefaultTransform = *BoundTransform;
				}
				else if (const FTransform* CookedTransform = CookedReferencePose.Find(TrackName)) {
					DefaultTransform = *CookedTransform;
				}
				else if (Skeleton) {
					/* Cloud had nothing to say about this bone, so the project's skeleton stands in */
					const FReferenceSkeleton& ReferenceSkeleton = Skeleton->GetReferenceSkeleton();
					const int32 BoneIndex = ReferenceSkeleton.FindBoneIndex(TrackName);

					if (BoneIndex != INDEX_NONE) {
						const TArray<FTransform> ReferencePose = Skeleton->GetRefLocalPoses();

						if (ReferencePose.IsValidIndex(BoneIndex)) {
							DefaultTransform = ReferencePose[BoneIndex];
						}
					}
				}
			}

			/* If a track is found, combine the transform data together (the engine cooks only the difference between the reference skeleton and the pose) */
			if (TrackToBufferIndex.Contains(i)) {
				const int32 LocalIndex = TrackToBufferIndex[i];

				if (LocalSpacePoseJson.IsValidIndex(LocalIndex)) {
					/* Falls through to the reference transform below rather than skipping the
					 * track, a hole here is a null entry that whoever reads the array walks into */
					const TSharedPtr<FJsonObject> AdditiveJson = LocalSpacePoseJson[LocalIndex]->AsObject();
					if (!AdditiveJson.IsValid()) {
						SourceLocalSpacePose[i] = MakeShareable(new FJsonValueObject(GetTransformJson(DefaultTransform)));
						continue;
					}

					const FTransform AdditiveTransform = GetTransformFromJson(AdditiveJson);

					FTransform FullTransform = DefaultTransform; {
						FullTransform.SetRotation(AdditiveTransform.GetRotation() * DefaultTransform.GetRotation());
						FullTransform.SetTranslation(DefaultTransform.GetTranslation() + AdditiveTransform.GetTranslation());
						FullTransform.SetScale3D(DefaultTransform.GetScale3D() * (AdditiveTransform.GetScale3D() + FVector::OneVector));

						if (!PoseAsset->IsValidAdditive()) {
							FullTransform.SetRotation(AdditiveTransform.GetRotation());
							FullTransform.SetTranslation(AdditiveTransform.GetTranslation());
							FullTransform.SetScale3D(AdditiveTransform.GetScale3D());
						}
						
						FullTransform.NormalizeRotation();
					}

					SourceLocalSpacePose[i] = MakeShareable(new FJsonValueObject(GetTransformJson(FullTransform)));
					continue;
				}
			}

			SourceLocalSpacePose[i] = MakeShareable(new FJsonValueObject(GetTransformJson(DefaultTransform)));
		}

		/* Update the Pose with SourceLocalSpacePose */
		Pose->SetArrayField(TEXT("SourceLocalSpacePose"), SourceLocalSpacePose);

		/* And the curves alongside them, sized to the container's curve list either way */
		TArray<TSharedPtr<FJsonValue>> SourceCurveData;
		SourceCurveData.Reserve(NumCurves);

		if (Pose->HasField(TEXT("CurveData"))) {
			SourceCurveData = Pose->GetArrayField(TEXT("CurveData"));
		}

		if (SourceCurveData.Num() != NumCurves) {
			UE_LOG(LogReflection, Warning, TEXT("\"%s\" pose %d carries %d curve values for %d curves, padding to match"), *GetAssetName(), PoseIndex, SourceCurveData.Num(), NumCurves);

			while (SourceCurveData.Num() < NumCurves) {
				SourceCurveData.Add(MakeShareable(new FJsonValueNumber(0.0)));
			}

			SourceCurveData.SetNum(NumCurves);
		}

		Pose->SetArrayField(TEXT("SourceCurveData"), SourceCurveData);
	}

	FString CleanName = GetAssetName();

	const FString PoseAssetPackagePath = GetPackage()->GetName();
	const FString ParentPath = FPackageName::GetLongPackagePath(PoseAssetPackagePath);

	if (GetAssetName().EndsWith(TEXT("_PoseAsset"))) {
		CleanName.RemoveFromEnd(TEXT("_PoseAsset"));
	} else {
		CleanName = GetAssetName() + "_Pose_Export";
	}

	const FString PotentialAnimSequencePath = ParentPath / CleanName;
	if (FPackageName::DoesPackageExist(PotentialAnimSequencePath)) {
		CleanName = GetAssetName() + "_Pose_Export";
	}
	
	const FString AnimSequencePackagePath = ParentPath / CleanName;

	UPackage* AnimPackage = CreatePackage(
		/* 4.25, 4.26.0 and below need an Outer */
#if UE4_25_BELOW || (UE4_26_0)
		nullptr,
#endif
		*AnimSequencePackagePath);

	if (UAnimSequence* AnimSequence = CreateAnimSequenceFromPose(Skeleton, CleanName, PoseContainer, AnimPackage)) {
		PoseAsset->SourceAnimation = AnimSequence;
	}
}

/* The pose the mesh these poses were made for is bound at.
 *
 * A pose asset stores its differences against the mesh's neutral face, and cooking drops the pose
 * they were taken from. The skeleton's own is no substitute: heads share a skeleton and each is
 * built at its own proportions, so composing onto it moves every bone by however far this head sits
 * from whichever one the skeleton was saved at. The mesh sits beside the pose asset and is named by
 * it, so it can be asked for directly. */
TMap<FName, FTransform> IPoseAssetImporter::GetSourceMeshBindPose() {
	TMap<FName, FTransform> BindPose;

	FString FetchPath = GetAssetExport()->HasField(TEXT("Package"))
		? GetAssetExport()->GetStringField(TEXT("Package"))
		: FString();

	if (FetchPath.IsEmpty()) return BindPose;

	/* "<mesh>_Facial_Poses_PoseAsset" sits next to "<mesh>" */
	FString MeshPath = FetchPath;

	for (const TCHAR* Suffix : { TEXT("_Facial_Poses_PoseAsset"), TEXT("_PoseAsset") }) {
		if (MeshPath.EndsWith(Suffix)) {
			MeshPath = MeshPath.LeftChop(FCString::Strlen(Suffix));

			break;
		}
	}

	if (MeshPath == FetchPath) return BindPose;

	const FBlockingRequestScope BlockingScope(FText::Format(
		NSLOCTEXT("Reflection", "FetchingBindPose", "Reading bind pose from {0}"),
		FText::FromString(MeshPath)
	));

	const TSharedPtr<FJsonObject> Payload = Cloud::Export::GetReferenceSkeletonBlocking(MeshPath);

	const TArray<TSharedPtr<FJsonValue>>* Bones = nullptr;

	if (!Payload.IsValid() || !Payload->TryGetArrayField(TEXT("bones"), Bones)) {
		UE_LOG(LogReflection, Warning, TEXT("\"%s\" found no mesh at %s to read a bind pose from"), *GetAssetName(), *MeshPath);

		return BindPose;
	}

	const auto ReadNumber = [](const TArray<TSharedPtr<FJsonValue>>* Values, const int32 Index, const double Fallback) {
		return Values != nullptr && Values->IsValidIndex(Index) ? (*Values)[Index]->AsNumber() : Fallback;
	};

	for (const TSharedPtr<FJsonValue>& Value : *Bones) {
		const TSharedPtr<FJsonObject> Bone = Value.IsValid() ? Value->AsObject() : nullptr;
		if (!Bone.IsValid()) continue;

		FString Name;
		if (!Bone->TryGetStringField(TEXT("Name"), Name)) continue;

		const TArray<TSharedPtr<FJsonValue>>* Translation = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Rotation = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Scale = nullptr;

		Bone->TryGetArrayField(TEXT("Translation"), Translation);
		Bone->TryGetArrayField(TEXT("Rotation"), Rotation);
		Bone->TryGetArrayField(TEXT("Scale"), Scale);

		BindPose.Add(FName(*Name), FTransform(
			FQuat(ReadNumber(Rotation, 0, 0.0), ReadNumber(Rotation, 1, 0.0), ReadNumber(Rotation, 2, 0.0), ReadNumber(Rotation, 3, 1.0)),
			FVector(ReadNumber(Translation, 0, 0.0), ReadNumber(Translation, 1, 0.0), ReadNumber(Translation, 2, 0.0)),
			FVector(ReadNumber(Scale, 0, 1.0), ReadNumber(Scale, 1, 1.0), ReadNumber(Scale, 2, 1.0))
		));
	}

	UE_LOG(LogReflection, Display, TEXT("\"%s\" read a bind pose of %d bones from %s"), *GetAssetName(), BindPose.Num(), *MeshPath);

	return BindPose;
}

TMap<FName, FTransform> IPoseAssetImporter::GetCookedReferencePose() const {
	TMap<FName, FTransform> ReferencePose;

	const TSharedPtr<FJsonObject>* SkeletonReference;
	if (!GetAssetData()->TryGetObjectField(TEXT("Skeleton"), SkeletonReference)) {
		return ReferencePose;
	}

	FString SkeletonPath;
	if (!(*SkeletonReference)->TryGetStringField(TEXT("ObjectPath"), SkeletonPath) || SkeletonPath.IsEmpty()) {
		return ReferencePose;
	}

	/* Cloud cuts the export index off itself, but the path has to match what it indexes on */
	int32 Dot;
	if (SkeletonPath.FindLastChar(TEXT('.'), Dot)) {
		LeftInline(SkeletonPath, Dot);
	}

	const FBlockingRequestScope BlockingScope(FText::Format(
		NSLOCTEXT("Reflection", "FetchingSkeleton", "Reading reference pose from {0}"),
		FText::FromString(SkeletonPath)
	));

	const TSharedPtr<FJsonObject> Response = Cloud::Export::GetRawBlocking(SkeletonPath);
	if (Response == nullptr || !Response->HasField(TEXT("exports"))) {
		UE_LOG(LogReflection, Warning, TEXT("\"%s\" could not read \"%s\" from Cloud, falling back to the skeleton in the project"), *GetAssetName(), *SkeletonPath);

		return ReferencePose;
	}

	for (const TSharedPtr<FJsonValue>& ExportValue : Response->GetArrayField(TEXT("exports"))) {
		const TSharedPtr<FJsonObject> Export = ExportValue->AsObject();

		FString Type;
		if (!Export.IsValid() || !Export->TryGetStringField(TEXT("Type"), Type) || Type != TEXT("Skeleton")) {
			continue;
		}

		const TSharedPtr<FJsonObject>* Properties;
		if (!Export->TryGetObjectField(TEXT("Properties"), Properties)) continue;

		const TSharedPtr<FJsonObject>* ReferenceSkeleton;
		if (!(*Properties)->TryGetObjectField(TEXT("ReferenceSkeleton"), ReferenceSkeleton)) continue;

		const TArray<TSharedPtr<FJsonValue>>* BoneInfo;
		const TArray<TSharedPtr<FJsonValue>>* BonePose;

		if (!(*ReferenceSkeleton)->TryGetArrayField(TEXT("FinalRefBoneInfo"), BoneInfo)) continue;
		if (!(*ReferenceSkeleton)->TryGetArrayField(TEXT("FinalRefBonePose"), BonePose)) continue;

		/* Two parallel arrays, so a bone is only usable when both sides have it */
		for (int32 BoneIndex = 0; BoneIndex < BoneInfo->Num() && BoneIndex < BonePose->Num(); BoneIndex++) {
			const TSharedPtr<FJsonObject> Bone = (*BoneInfo)[BoneIndex]->AsObject();
			const TSharedPtr<FJsonObject> Transform = (*BonePose)[BoneIndex]->AsObject();

			if (!Bone.IsValid() || !Transform.IsValid()) continue;

			FString BoneName;
			if (!Bone->TryGetStringField(TEXT("Name"), BoneName)) continue;

			ReferencePose.Add(FName(*BoneName), GetTransformFromJson(Transform));
		}

		break;
	}

	return ReferencePose;
}

UAnimSequence* IPoseAssetImporter::CreateAnimSequenceFromPose(USkeleton* Skeleton, const FString& SequenceName, const TSharedPtr<FJsonObject>& PoseContainer, UPackage* Outer) {
	if (!Skeleton || !PoseContainer.IsValid() || Outer == nullptr) {
		return nullptr;
	}

	const TArray<TSharedPtr<FJsonValue>>* TracksJson;
	const TArray<TSharedPtr<FJsonValue>>* PosesJson;

	if (!PoseContainer->TryGetArrayField(TEXT("Tracks"), TracksJson)) return nullptr;
	if (!PoseContainer->TryGetArrayField(TEXT("Poses"), PosesJson)) return nullptr;

	/* One frame per pose, one track per bone */
	const int32 NumFrames = PosesJson->Num();
	const int32 NumTracks = TracksJson->Num();

	if (NumFrames == 0 || NumTracks == 0) {
		return nullptr;
	}

	/* Every track gets a key on every frame, even where the pose had nothing to say about it.
	 * The keys are positional, so a track that skips a frame is not a track missing one value,
	 * it is a track whose every later key belongs to the wrong frame. */
	TArray<FName> BoneNames;
	TArray<TArray<FVector>> PositionKeys;
	TArray<TArray<FQuat>> RotationKeys;
	TArray<TArray<FVector>> ScaleKeys;

	TArray<int32> TrackToBone;
	TrackToBone.Init(INDEX_NONE, NumTracks);

	/* Which tracks any pose actually moves. The rest sit at the reference pose in every pose, and a
	 * track that never changes is not free: the animation writes it over whatever the mesh was
	 * bound at, which is not the skeleton's reference pose on a head. */
	TSet<int32> MovedTracks;

	{
		const TArray<TSharedPtr<FJsonValue>>* InfluenceIndices;

		if (PoseContainer->TryGetArrayField(TEXT("TrackPoseInfluenceIndices"), InfluenceIndices)) {
			for (int32 TrackIndex = 0; TrackIndex < InfluenceIndices->Num(); TrackIndex++) {
				const TSharedPtr<FJsonObject> Influences = (*InfluenceIndices)[TrackIndex]->AsObject();
				const TArray<TSharedPtr<FJsonValue>>* Entries;

				if (Influences.IsValid() && Influences->TryGetArrayField(TEXT("Influences"), Entries) && Entries->Num() > 0) {
					MovedTracks.Add(TrackIndex);
				}
			}
		} else {
			/* Older data says it the other way round, per pose */
			for (const TSharedPtr<FJsonValue>& PoseValue : *PosesJson) {
				const TSharedPtr<FJsonObject> Pose = PoseValue.IsValid() ? PoseValue->AsObject() : nullptr;
				const TArray<TSharedPtr<FJsonValue>>* Buffer = nullptr;

				if (!Pose.IsValid() || !Pose->TryGetArrayField(TEXT("TrackToBufferIndex"), Buffer)) continue;

				for (const TSharedPtr<FJsonValue>& Entry : *Buffer) {
					const TSharedPtr<FJsonObject> Pair = Entry.IsValid() ? Entry->AsObject() : nullptr;

					if (Pair.IsValid()) {
						MovedTracks.Add(FCString::Atoi(*Pair->GetStringField(TEXT("Key"))));
					}
				}
			}
		}
	}

	int32 SkippedTracks = 0;

	for (int32 TrackIndex = 0; TrackIndex < NumTracks; TrackIndex++) {
		const FName BoneName(*(*TracksJson)[TrackIndex]->AsString());

		/* Only what moves. With nothing to say about a bone, the animation leaves it alone. */
		if (MovedTracks.Num() > 0 && !MovedTracks.Contains(TrackIndex)) {
			continue;
		}

		/* A track the project's skeleton has no bone for has nowhere to go */
		if (Skeleton->GetReferenceSkeleton().FindBoneIndex(BoneName) == INDEX_NONE) {
			SkippedTracks++;
			continue;
		}

		TrackToBone[TrackIndex] = BoneNames.Add(BoneName);

		PositionKeys.AddDefaulted();
		RotationKeys.AddDefaulted();
		ScaleKeys.AddDefaulted();

		PositionKeys.Last().Reserve(NumFrames);
		RotationKeys.Last().Reserve(NumFrames);
		ScaleKeys.Last().Reserve(NumFrames);
	}

	if (BoneNames.Num() == 0) {
		UE_LOG(LogReflection, Warning, TEXT("\"%s\" has no track the skeleton \"%s\" recognises, skipping the animation"), *SequenceName, *Skeleton->GetName());

		return nullptr;
	}

	if (SkippedTracks > 0) {
		UE_LOG(LogReflection, Warning, TEXT("\"%s\" left out %d of %d tracks, \"%s\" has no bone by those names"), *SequenceName, SkippedTracks, NumTracks, *Skeleton->GetName());
	}

	UE_LOG(LogReflection, Display, TEXT("\"%s\" animates %d of %d tracks over %d frame(s)"), *SequenceName, BoneNames.Num(), NumTracks, NumFrames);

	for (int32 FrameIndex = 0; FrameIndex < NumFrames; FrameIndex++) {
		const TSharedPtr<FJsonObject> Pose = (*PosesJson)[FrameIndex]->AsObject();

		/* Written by ReverseCookLocalSpacePose, absent on any pose it couldn't read */
		const TArray<TSharedPtr<FJsonValue>>* SourceLocalSpacePose = nullptr;
		if (Pose.IsValid()) {
			Pose->TryGetArrayField(TEXT("SourceLocalSpacePose"), SourceLocalSpacePose);
		}

		for (int32 TrackIndex = 0; TrackIndex < NumTracks; TrackIndex++) {
			const int32 BoneIndex = TrackToBone[TrackIndex];
			if (BoneIndex == INDEX_NONE) continue;

			FTransform Transform = FTransform::Identity;

			if (SourceLocalSpacePose != nullptr && SourceLocalSpacePose->IsValidIndex(TrackIndex) && (*SourceLocalSpacePose)[TrackIndex].IsValid()) {
				if (const TSharedPtr<FJsonObject> TransformJson = (*SourceLocalSpacePose)[TrackIndex]->AsObject()) {
					Transform = GetTransformFromJson(TransformJson);
				}
			}

			PositionKeys[BoneIndex].Add(Transform.GetTranslation());
			RotationKeys[BoneIndex].Add(Transform.GetRotation());
			ScaleKeys[BoneIndex].Add(Transform.GetScale3D());
		}
	}

	UAnimSequence* AnimSequence = NewObject<UAnimSequence>(Outer, FName(*SequenceName), RF_Public | RF_Standalone | RF_Transactional);
	AnimSequence->SetSkeleton(Skeleton);

	/* A pose asset has no timing of its own, so the poses are laid out one per frame at a rate
	 * that makes each of them land on a whole frame */
	constexpr int32 FrameRate = 30;

/* 5.2 is where raw animation data moved behind the data model and its controller */
#if ENGINE_UE5 && ENGINE_MINOR_VERSION >= 2
	IAnimationDataController& Controller = AnimSequence->GetController();

	Controller.OpenBracket(NSLOCTEXT("Reflection", "BuildPoseAnimation", "Building pose animation"), false);
	Controller.InitializeModel();

	Controller.SetFrameRate(FFrameRate(FrameRate, 1), false);
	Controller.SetNumberOfFrames(FFrameNumber(FMath::Max(1, NumFrames - 1)), false);

	for (int32 BoneIndex = 0; BoneIndex < BoneNames.Num(); BoneIndex++) {
		Controller.AddBoneCurve(BoneNames[BoneIndex], false);
		Controller.SetBoneTrackKeys(BoneNames[BoneIndex], PositionKeys[BoneIndex], RotationKeys[BoneIndex], ScaleKeys[BoneIndex], false);
	}

	Controller.NotifyPopulated();
	Controller.CloseBracket(false);
#elif ENGINE_UE4
	AnimSequence->SetRawNumberOfFrame(NumFrames);
	AnimSequence->SequenceLength = NumFrames > 1 ? static_cast<float>(NumFrames - 1) / FrameRate : 1.0f;

	for (int32 BoneIndex = 0; BoneIndex < BoneNames.Num(); BoneIndex++) {
		FRawAnimSequenceTrack Track;

		Track.PosKeys = PositionKeys[BoneIndex];
		Track.RotKeys = RotationKeys[BoneIndex];
		Track.ScaleKeys = ScaleKeys[BoneIndex];

		AnimSequence->AddNewRawTrack(BoneNames[BoneIndex], &Track);
	}

	AnimSequence->PostProcessSequence();
#else
	/* 5.0 and 5.1 have the data model but not the controller surface used above */
	UE_LOG(LogReflection, Warning, TEXT("\"%s\" cannot be built on this engine version"), *SequenceName);

	return nullptr;
#endif

	/* Without this the sequence exists but never shows up in the Content Browser */
	FAssetRegistryModule::AssetCreated(AnimSequence);

	AnimSequence->PostEditChange();
	AnimSequence->MarkPackageDirty();

	Outer->FullyLoad();
	Outer->SetDirtyFlag(true);

	if (GetSettings()->AssetSettings.SaveAssets) {
		SavePackage(Outer);
	}

	UE_LOG(LogReflection, Log, TEXT("Built \"%s\" from %d poses over %d tracks"), *SequenceName, NumFrames, BoneNames.Num());

	return AnimSequence;
}
