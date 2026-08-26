/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/Animation/AnimSequenceImporter.h"
#include "Importers/Constructor/ImportJob.h"

#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "Importers/Constructor/ImportIssues.h"
#include "Modules/Cloud/Cloud.h"
#include "Modules/Cloud/Remote.h"
#include "Importers/Types/Animation/SAdditiveBaseDialog.h"
#include "Engine/EngineUtilities.h"
#include "Utilities/JsonHelpers.h"
#include "Modules/Cloud/Tools/AnimationData.h"

#if ENGINE_UE5 && ENGINE_MINOR_VERSION >= 2
#include "Animation/AnimData/IAnimationDataController.h"
#endif

/* Where the Cloud reads a sequence's keys back out of what the cook left of it */
static const FString AnimationURL = TEXT("/api/export/animation");

/* The bone tracks of an animation sequence, a key a frame, rebuilt into the animation itself when
 * the sequence is a difference from another. Null when the Cloud has no sequence at Path, or when
 * it was compressed by something it has no reader for.
 *
 * BasePath names what to rebuild a difference over, for a sequence that says nothing usable about
 * what it was built on. */
static TSharedPtr<FJsonObject> GetAnimation(const FString& Path, const FString& BasePath = FString()) {
	TMap<FString, FString> Parameters = { { TEXT("path"), Path } };

	if (!BasePath.IsEmpty()) {
		Parameters.Add(TEXT("base"), BasePath);
	}

	const TSharedPtr<FJsonObject> Response = Cloud::GetBlocking(AnimationURL, Parameters, {});

	if (!Response.IsValid() || !Response->HasField(TEXT("tracks"))) {
		return nullptr;
	}

	return Response;
}

/* One number out of a flat json array, since keys arrive as one array a channel rather than as an
 * object a key: three numbers a position, four a rotation, three a scale. */
static float ReadKeyFloat(const TArray<TSharedPtr<FJsonValue>>* Values, const int32 Index) {
	return Values != nullptr && Values->IsValidIndex(Index) ? static_cast<float>((*Values)[Index]->AsNumber()) : 0.0f;
}

UObject* IAnimSequenceImporter::CreateAsset(UObject* CreatedAsset) {
	/* Reflecting the same sequence twice lands on the package the first import wrote.
	 *
	 * The sequence already there is emptied and filled in again, rather than replaced. Making a
	 * second object over the name destroys the first where it stands, and moving the first aside
	 * instead leaves everything already pointing at it a blend space's samples, a montage's
	 * slots, a graph's play nodes holding an object that belongs to no package any more, which
	 * saves as nothing at all. Reflecting a folder does exactly that: the blend spaces come first
	 * by name, bind to the sequences beside them, and then those sequences are reflected in turn.
	 *
	 * Only the sequence's own keys and marks are cleared. What the import is about to write is
	 * written over anyway, and what it doesn't write is the asset's to keep. */
	if (UAnimSequence* Existing = Cast<UAnimSequence>(StaticFindObjectFast(UAnimSequence::StaticClass(), GetPackage(), StringToName(GetAssetName())))) {
#if ENGINE_UE4
		/* Compression started by the last import can still be running, and it reads the keys this
		 * is about to throw away */
		Existing->WaitOnExistingCompression(false);
		Existing->RemoveAllTracks();
#endif

		Existing->Notifies.Empty();
		Existing->AuthoredSyncMarkers.Empty();

		return IImporter::CreateAsset(Existing);
	}

	return IImporter::CreateAsset(NewObject<UAnimSequence>(GetPackage(), UAnimSequence::StaticClass(), StringToName(GetAssetName()), RF_Public | RF_Standalone | RF_Transactional));
}

USkeleton* IAnimSequenceImporter::ResolveSkeleton() {
	const FUObjectJsonValueExport SkeletonReference = GetAssetDataAsValue().GetObject(TEXT("Skeleton"));

	if (!SkeletonReference.JsonObject.IsValid() || !SkeletonReference.Has(TEXT("ObjectName"))) {
		return nullptr;
	}

	/* Loads it out of the project, and asks the Cloud for it when it isn't there yet */
	TObjectPtr<USkeleton> Skeleton;
	LoadExport<USkeleton>(&SkeletonReference.JsonObject, Skeleton);

	return Skeleton.Get();
}

bool IAnimSequenceImporter::Import() {
	/* A sequence is keys against a named skeleton's bones, and a bone is named rather than
	 * numbered on the way over. Without the skeleton there is nothing to write the keys onto. */
	USkeleton* Skeleton = ResolveSkeleton();

	if (Skeleton == nullptr) {
		FImportIssues::Report(
			EImportIssue::MissingAsset,
			TEXT("The sequence's skeleton isn't in this project"),
			TEXT("An animation is keys on the bones of the skeleton it names, so the import stops here rather than building one with nothing to play on.")
		);

		return false;
	}

	/* Asked for by the path the game cooked it under. The export names that itself; only when it
	 * doesn't is the path the asset landed at turned back into one. */
	FString FetchPath = GetAssetExport()->HasField(TEXT("Package"))
		? GetAssetExport()->GetStringField(TEXT("Package"))
		: FString();

	if (FetchPath.IsEmpty()) {
		FetchPath = GetPackage()->GetPathName();

		FRRedirects::Reverse(FetchPath);
	}

	const FBlockingRequestScope BlockingScope(FText::Format(
		NSLOCTEXT("Reflection", "FetchingAnimation", "Reading animation from {0}"),
		FText::FromString(FetchPath)
	));

	/* Asked for before the sequence is made rather than after: nothing is created for an animation
	 * whose keys cannot be had, so a Cloud that isn't running leaves the last import standing. */
	TSharedPtr<FJsonObject> Payload = GetAnimation(FetchPath);

	/* A difference with nothing to be put back over: it names a frame of itself, or names an
	 * animation that isn't there, or names none at all, which is what an aim offset pose is left as
	 * once it is cooked. The Cloud knows what a good few of them were built over and says so by
	 * answering with one; this is for the rest, and going without leaves it over the bind pose. */
	if (Payload.IsValid() && Payload->HasField(TEXT("needsBase")) && Payload->GetBoolField(TEXT("needsBase"))) {
		FString BasePath;

		/* Asked only of somebody who asked for this one animation.
		 *
		 * A run of folders is hundreds of these, and stopping on a dialog for each is not a
		 * question, it is a queue. Left unasked, the difference is built over the skeleton's own
		 * pose, which is what going without has always meant. */
		const bool bAsk = !FImportJob::IsRunning() && !IsRunningCommandlet();

		if (!bAsk) {
			UE_LOG(LogReflection, Display, TEXT("\"%s\" is a difference with nothing to put it over, and a run of assets is no place to ask what"), *GetAssetName());
		}

		if (bAsk && SAdditiveBaseDialog::Open(GetAssetName(), BasePath) && !BasePath.IsEmpty()) {
			if (const TSharedPtr<FJsonObject> Rebuilt = GetAnimation(FetchPath, BasePath)) {
				Payload = Rebuilt;

				UE_LOG(LogReflection, Display, TEXT("\"%s\" was built over \"%s\""), *GetAssetName(), *BasePath);
			} else {
				FImportIssues::Report(
					EImportIssue::Data,
					TEXT("The animation it was built over didn't come back"),
					FString::Printf(TEXT("\"%s\" is not something the Cloud could read, so the difference is built over the skeleton's own pose instead."), *BasePath)
				);
			}
		}
	}

	if (!Payload.IsValid()) {
		FImportIssues::Report(
			EImportIssue::Failed,
			TEXT("No animation from the Cloud"),
			TEXT("The Cloud has to be running, and the sequence's keys have to be in a format it can read back.")
		);

		return false;
	}

	UAnimSequence* AnimSequence = Create<UAnimSequence>();
	if (AnimSequence == nullptr) return false;

	AnimSequence->SetSkeleton(Skeleton);

	DeserializeExports(AnimSequence);

	/* Everything the sequence is besides its keys: how fast it plays, what it is additive against,
	 * its notifies and sync markers. What this import writes itself is left out. */
	GetObjectSerializer()->DeserializeObjectProperties(RemovePropertiesShared(GetAssetData(), {
		/* Written here, out of the Cloud's keys */
		"NumFrames",
		"SequenceLength",
		"RawAnimationData",
		"TrackToSkeletonMapTable",

		/* The cook's compressed stream, which is what the Cloud read to get those keys. Saving
		 * compresses the sequence again, so none of it belongs on the asset. */
		"CompressedDataStructure",
		"CompressedTrackToSkeletonMapTable",
		"CompressedRawDataSize",
		"CompressedCurveData",
		"CompressedCurveNames",
		"CompressedByteStream",
		"BoneCompressionSettings",
		"CurveCompressionSettings",

		/* Already set, and the reference in the export points at the cooked package */
		"Skeleton",
		"SkeletonGuid",
	}), AnimSequence);

	/* Held onto across the tracks below, which are what set the length the sequence plays for.
	 * The engine answers a length it considers grown by treating the difference as time inserted
	 * at the old end, and moves every notify and sync marker at or after that point along by it.
	 * The keys arrive before any length does, so the old end is the minimum a sequence starts at,
	 * every mark in the export sits at or after it, and each one is pushed a whole sequence to the
	 * right and clamped to the end. The engine's own import suppresses that while it populates,
	 * through a flag nothing outside the class can set. Put back afterwards instead, at the times
	 * the export gave them. */
	const TArray<FAnimSyncMarker> AuthoredMarkers = AnimSequence->AuthoredSyncMarkers;
	const TArray<FAnimNotifyEvent> AuthoredNotifies = AnimSequence->Notifies;

	const int32 WrittenTracks = BuildTracks(AnimSequence, Skeleton, Payload);

	if (WrittenTracks == 0) {
		FImportIssues::Report(
			EImportIssue::Failed,
			TEXT("None of the animation's bones are on this skeleton"),
			TEXT("Every track the Cloud sent names a bone the skeleton hasn't got, so there is nothing for the sequence to move.")
		);

		return false;
	}

	UE_LOG(LogReflection, Display, TEXT("\"%s\" built %d track(s) over %d frame(s) against skeleton \"%s\""),
		*GetAssetName(), WrittenTracks, static_cast<int32>(Payload->GetIntegerField(TEXT("numFrames"))), *Skeleton->GetName());

	AnimSequence->AuthoredSyncMarkers = AuthoredMarkers;
	AnimSequence->Notifies = AuthoredNotifies;

	/* Sorting is what puts the markers in the order the sync system walks them and works out the
	 * names it matches them against, which are derived from the markers rather than saved beside
	 * them. It is also what tells the blend spaces already bound to this sequence to look at its
	 * markers again: a blend space asked to sync against markers it cannot place brings the editor
	 * down inside the engine's own tick, where the import has nothing to catch. */
	AnimSequence->SortSyncMarkers();

	/* After the notifies are back, since the rows are laid out to hold them */
	BuildAnimNotifyTracks(AnimSequence);

	/* After the tracks: building them initializes the model, which takes the curves with it */
	if (!ReadAnimationCurves(this, AnimSequence)) {
		FImportIssues::Report(
			EImportIssue::Data,
			TEXT("The animation's curves weren't read"),
			TEXT("The sequence imports without them, so anything driven by a curve stays still.")
		);
	}

	AnimSequence->PostEditChange();

	return OnAssetCreation(AnimSequence);
}

int32 IAnimSequenceImporter::BuildTracks(UAnimSequence* AnimSequence, USkeleton* Skeleton, const TSharedPtr<FJsonObject>& Payload) const {
	const TArray<TSharedPtr<FJsonValue>>* TracksJson = nullptr;

	if (!Payload->TryGetArrayField(TEXT("tracks"), TracksJson)) return 0;

	const int32 NumFrames = FMath::Max(1, Payload->GetIntegerField(TEXT("numFrames")));

	const float SequenceLength = Payload->HasField(TEXT("sequenceLength"))
		? static_cast<float>(Payload->GetNumberField(TEXT("sequenceLength")))
		: 0.0f;

	const FReferenceSkeleton& RefSkeleton = Skeleton->GetReferenceSkeleton();

	TArray<FName> BoneNames;
	TArray<TArray<FVector>> PositionKeys;
	TArray<TArray<FQuat>> RotationKeys;
	TArray<TArray<FVector>> ScaleKeys;

	for (const TSharedPtr<FJsonValue>& TrackValue : *TracksJson) {
		const TSharedPtr<FJsonObject> TrackJson = TrackValue.IsValid() ? TrackValue->AsObject() : nullptr;
		if (!TrackJson.IsValid()) continue;

		FString BoneName;
		if (!TrackJson->TryGetStringField(TEXT("bone"), BoneName) || BoneName.IsEmpty()) continue;

		const int32 BoneIndex = RefSkeleton.FindBoneIndex(FName(*BoneName));
		if (BoneIndex == INDEX_NONE) continue;

		/* The Cloud sends an additive sequence as the animation rather than as the difference, the
		 * way the engine keeps its own, so a channel with no keys is the bind pose either way */
		const FTransform RefPose = RefSkeleton.GetRefBonePose()[BoneIndex];

		const TArray<TSharedPtr<FJsonValue>>* Positions = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Rotations = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Scales = nullptr;

		TrackJson->TryGetArrayField(TEXT("positions"), Positions);
		TrackJson->TryGetArrayField(TEXT("rotations"), Rotations);
		TrackJson->TryGetArrayField(TEXT("scales"), Scales);

		/* A channel the cook left out comes back with no keys, and the bone holds the pose it is
		 * played against rather than moving to nothing */
		const int32 PositionCount = Positions != nullptr ? Positions->Num() / 3 : 0;
		const int32 RotationCount = Rotations != nullptr ? Rotations->Num() / 4 : 0;
		const int32 ScaleCount = Scales != nullptr ? Scales->Num() / 3 : 0;

		TArray<FVector> TrackPositions;
		TArray<FQuat> TrackRotations;
		TArray<FVector> TrackScales;

		if (PositionCount == 0) {
			TrackPositions.Add(RefPose.GetTranslation());
		} else {
			TrackPositions.Reserve(PositionCount);

			for (int32 Key = 0; Key < PositionCount; ++Key) {
				TrackPositions.Add(FVector(
					ReadKeyFloat(Positions, Key * 3),
					ReadKeyFloat(Positions, Key * 3 + 1),
					ReadKeyFloat(Positions, Key * 3 + 2)
				));
			}
		}

		if (RotationCount == 0) {
			TrackRotations.Add(RefPose.GetRotation());
		} else {
			TrackRotations.Reserve(RotationCount);

			for (int32 Key = 0; Key < RotationCount; ++Key) {
				FQuat Rotation(
					ReadKeyFloat(Rotations, Key * 4),
					ReadKeyFloat(Rotations, Key * 4 + 1),
					ReadKeyFloat(Rotations, Key * 4 + 2),
					ReadKeyFloat(Rotations, Key * 4 + 3)
				);

				Rotation.Normalize();

				TrackRotations.Add(Rotation);
			}
		}

		if (ScaleCount == 0) {
			TrackScales.Add(RefPose.GetScale3D());
		} else {
			TrackScales.Reserve(ScaleCount);

			for (int32 Key = 0; Key < ScaleCount; ++Key) {
				TrackScales.Add(FVector(
					ReadKeyFloat(Scales, Key * 3),
					ReadKeyFloat(Scales, Key * 3 + 1),
					ReadKeyFloat(Scales, Key * 3 + 2)
				));
			}
		}

		/* The three channels go in as one track, and the controller takes them only if all three
		 * are the same length: a bone whose rotation was keyed and whose translation was left
		 * constant arrives as sixty keys against one, and the track is refused whole. So a channel
		 * shorter than the track it belongs to is held at its last value for the rest of it, which
		 * is the pose it was in anyway. */
		const int32 TrackKeys = FMath::Max3(TrackPositions.Num(), TrackRotations.Num(), TrackScales.Num());

		/* The key held onto is copied out first. Growing the array it came from moves the array,
		 * and the element would then be read back through a reference to the memory it has just
		 * been freed from. */
		auto HoldLastKey = [](auto& Keys, const int32 Count) {
			if (Keys.Num() >= Count) return;

			const auto Held = Keys.Last();

			Keys.Reserve(Count);

			while (Keys.Num() < Count) {
				Keys.Add(Held);
			}
		};

		HoldLastKey(TrackPositions, TrackKeys);
		HoldLastKey(TrackRotations, TrackKeys);
		HoldLastKey(TrackScales, TrackKeys);

		BoneNames.Add(FName(*BoneName));
		PositionKeys.Add(MoveTemp(TrackPositions));
		RotationKeys.Add(MoveTemp(TrackRotations));
		ScaleKeys.Add(MoveTemp(TrackScales));
	}

	if (BoneNames.Num() == 0) return 0;

	/* The rate the frames were laid out at, which is the sequence's own: the last frame lands on
	 * the end of it rather than a frame beyond it. */
	const float FrameRate = SequenceLength > 0.0f && NumFrames > 1
		? (NumFrames - 1) / SequenceLength
		: 30.0f;

/* 5.2 is where raw animation data moved behind the data model and its controller */
#if ENGINE_UE5 && ENGINE_MINOR_VERSION >= 2
	IAnimationDataController& Controller = AnimSequence->GetController();

	Controller.OpenBracket(NSLOCTEXT("Reflection", "BuildAnimationSequence", "Building animation sequence"), false);
	Controller.InitializeModel();

	Controller.SetFrameRate(FFrameRate(FMath::RoundToInt(FrameRate), 1), false);
	Controller.SetNumberOfFrames(FFrameNumber(FMath::Max(1, NumFrames - 1)), false);

	for (int32 Track = 0; Track < BoneNames.Num(); ++Track) {
		Controller.AddBoneCurve(BoneNames[Track], false);
		Controller.SetBoneTrackKeys(BoneNames[Track], PositionKeys[Track], RotationKeys[Track], ScaleKeys[Track], false);
	}

	Controller.NotifyPopulated();
	Controller.CloseBracket(false);
#elif ENGINE_UE4
	AnimSequence->SetRawNumberOfFrame(NumFrames);
	AnimSequence->SequenceLength = SequenceLength > 0.0f
		? SequenceLength
		: (NumFrames > 1 ? (NumFrames - 1) / 30.0f : 1.0f);

	for (int32 Track = 0; Track < BoneNames.Num(); ++Track) {
		FRawAnimSequenceTrack RawTrack;

		RawTrack.PosKeys = PositionKeys[Track];
		RawTrack.RotKeys = RotationKeys[Track];
		RawTrack.ScaleKeys = ScaleKeys[Track];

		AnimSequence->AddNewRawTrack(BoneNames[Track], &RawTrack);
	}

	/* Compresses what was just written, which is what the sequence plays from */
	AnimSequence->PostProcessSequence();
#else
	/* 5.0 and 5.1 have the data model and the same controller in front of it, under the names it
	 * was first written with: the model is emptied rather than initialized, a bone gets a track
	 * rather than a curve, and the span is set as the seconds it plays for rather than the frames
	 * it holds. Emptying it leaves 30fps over a minimum length behind, so the rate is set before
	 * the length and the length worked back out of it: a length the rate divides exactly is what
	 * lands the model on the frame count the branch above sets directly. */
	const int32 KeyRate = FMath::Max(1, FMath::RoundToInt(FrameRate));
	const int32 FrameSpan = FMath::Max(1, NumFrames - 1);

	IAnimationDataController& Controller = AnimSequence->GetController();

	Controller.OpenBracket(NSLOCTEXT("Reflection", "BuildAnimationSequence", "Building animation sequence"), false);
	Controller.ResetModel(false);

	Controller.SetFrameRate(FFrameRate(KeyRate, 1), false);
	Controller.SetPlayLength(static_cast<float>(FrameSpan) / static_cast<float>(KeyRate), false);

	for (int32 Track = 0; Track < BoneNames.Num(); ++Track) {
		Controller.AddBoneTrack(BoneNames[Track], false);
		Controller.SetBoneTrackKeys(BoneNames[Track], PositionKeys[Track], RotationKeys[Track], ScaleKeys[Track], false);
	}

	Controller.NotifyPopulated();
	Controller.CloseBracket(false);

#endif

	return BoneNames.Num();
}
