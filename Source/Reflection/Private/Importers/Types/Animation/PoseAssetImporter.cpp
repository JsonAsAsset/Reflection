/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/Animation/PoseAssetImporter.h"
#include "Animation/PoseAsset.h"
#include "Utilities/JsonUtilities.h"

UObject* IPoseAssetImporter::CreateAsset(UObject* CreatedAsset) {
	return IImporter::CreateAsset(NewObject<UPoseAsset>(GetPackage(), UPoseAsset::StaticClass(), *GetAssetName(), RF_Standalone | RF_Public));
}

bool IPoseAssetImporter::Import() {
	PoseAsset = Create<UPoseAsset>();

	/* Set Skeleton, so we can use it in the decooking process */
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

void IPoseAssetImporter::ReverseCookLocalSpacePose(USkeleton* Skeleton) const {
	/* If PoseContainer or Tracks don't exist, no need to perform any operations */
	if (
		!GetAssetData()->HasField(TEXT("PoseContainer")) ||
		!GetAssetData()->GetObjectField(TEXT("PoseContainer"))->HasField(TEXT("Tracks"))
	) {
		return;
	}
	
	const TSharedPtr<FJsonObject> PoseContainer = GetAssetData()->GetObjectField(TEXT("PoseContainer"));
	const TArray<TSharedPtr<FJsonValue>> TracksJson = PoseContainer->GetArrayField(TEXT("Tracks"));
	const TArray<TSharedPtr<FJsonValue>> PosesJson = PoseContainer->GetArrayField(TEXT("Poses"));

	const int32 NumTracks = TracksJson.Num();

	for (TSharedPtr<FJsonValue> PoseValue : PosesJson) {
		TSharedPtr<FJsonObject> Pose = PoseValue->AsObject();
		
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
			if (Pose->HasField(TEXT("TrackToBufferIndex"))) {
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
			/* DefaultTransform can either be default, or extracted from the base skeleton */
			FTransform DefaultTransform = FTransform::Identity; {
				if (Skeleton) {
					const FReferenceSkeleton& ReferenceSkeleton = Skeleton->GetReferenceSkeleton();
					const int32 BoneIndex = ReferenceSkeleton.FindBoneIndex(FName(*TracksJson[i]->AsString()));
				
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
					const TSharedPtr<FJsonObject> AdditiveJson = LocalSpacePoseJson[LocalIndex]->AsObject();
					if (!AdditiveJson.IsValid()) continue;
					
					const FTransform AdditiveTransform = GetTransformFromJson(AdditiveJson);

					FTransform FullTransform = DefaultTransform; {
						FullTransform.SetRotation(AdditiveTransform.GetRotation() * DefaultTransform.GetRotation());
						FullTransform.SetTranslation(DefaultTransform.GetTranslation() + AdditiveTransform.GetTranslation());
						FullTransform.SetScale3D(DefaultTransform.GetScale3D() + AdditiveTransform.GetScale3D());

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

	UPackage* AnimPackage = CreatePackage(*AnimSequencePackagePath);

	if (UAnimSequence* AnimSequence = CreateAnimSequenceFromPose(Skeleton, CleanName, PoseContainer, AnimPackage)) {
		PoseAsset->SourceAnimation = AnimSequence;
	}
}

UAnimSequence* IPoseAssetImporter::CreateAnimSequenceFromPose(USkeleton* Skeleton, const FString& SequenceName, const TSharedPtr<FJsonObject>& PoseContainer, UPackage* Outer) {
#if ENGINE_UE4
	if (!Skeleton || !PoseContainer.IsValid()) {
		return nullptr;
	}

	const TArray<TSharedPtr<FJsonValue>> TracksJson = PoseContainer->GetArrayField(TEXT("Tracks"));
	const TArray<TSharedPtr<FJsonValue>> PosesJson = PoseContainer->GetArrayField(TEXT("Poses"));
	const int32 NumFrames = PosesJson.Num();
	const int32 NumTracks = TracksJson.Num();

	if (NumFrames == 0 || NumTracks == 0) {
		return nullptr;
	}

	UAnimSequence* AnimSequence = NewObject<UAnimSequence>(Outer, FName(*SequenceName), RF_Public | RF_Standalone);
	AnimSequence->SetSkeleton(Skeleton);

	AnimSequence->SetRawNumberOfFrame(NumFrames);
	AnimSequence->SequenceLength = (NumFrames > 1) ? static_cast<float>(NumFrames - 1) : 1.0f;
	
	TMap<FName, FRawAnimSequenceTrack> TrackMap; {
		for (int32 TrackIndex = 0; TrackIndex < NumTracks; ++TrackIndex) {
			const FString TrackName = TracksJson[TrackIndex]->AsString();

			if (const FName BoneName(*TrackName); Skeleton->GetReferenceSkeleton().FindBoneIndex(BoneName) != INDEX_NONE) {
				TrackMap.Add(BoneName, FRawAnimSequenceTrack());
			}
		}
	}

	for (int32 FrameIndex = 0; FrameIndex < NumFrames; ++FrameIndex) {
		const TSharedPtr<FJsonObject> Pose = PosesJson[FrameIndex]->AsObject();
		if (!Pose.IsValid()) continue;

		const TArray<TSharedPtr<FJsonValue>> SourceLocalSpacePose = Pose->GetArrayField(TEXT("SourceLocalSpacePose"));
		
		for (int32 TrackIndex = 0; TrackIndex < NumTracks; ++TrackIndex) {
			const FString TrackName = TracksJson[TrackIndex]->AsString();
			const FName BoneName(*TrackName);

			if (!TrackMap.Contains(BoneName)) continue;

			const TSharedPtr<FJsonObject> TransformJson = SourceLocalSpacePose[TrackIndex]->AsObject();
			if (!TransformJson.IsValid()) continue;

			const FTransform Transform = GetTransformFromJson(TransformJson);

			auto& [PosKeys, RotKeys, ScaleKeys] = TrackMap[BoneName];
			PosKeys.Add(Transform.GetTranslation());
			RotKeys.Add(Transform.GetRotation());
			ScaleKeys.Add(Transform.GetScale3D());
		}
	}

	for (TPair<FName, FRawAnimSequenceTrack>& Pair : TrackMap) {
		AnimSequence->AddNewRawTrack(Pair.Key, &Pair.Value);
	}

	AnimSequence->PostProcessSequence();
	
	return AnimSequence;
#else
	return nullptr;
#endif
}
