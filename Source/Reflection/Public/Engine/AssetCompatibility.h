/* Copyright Reflection Contributors 2024-2026 */

#pragma once

/*
 * Version shims that need real asset types in scope, which is why they are not in
 * Compatibility.h: that one is included almost everywhere and is kept cheap.
 */

#include "Engine/Compatibility.h"

#include "Animation/AnimSequence.h"
/* AssetData.h only moved under an AssetRegistry/ folder later on */
#if UE4_25_BELOW
#include "AssetData.h"
#else
#include "AssetRegistry/AssetData.h"
#endif
#include "Engine/SkeletalMesh.h"
#include "ScopedTransaction.h"

/* SetAnimSequenceLength drives the sequence through its controller from 5.2 on. The old file got
 * this header by accident, off something else in its include list. */
#if ENGINE_UE5 && ENGINE_MINOR_VERSION >= 2
#include "Animation/AnimData/IAnimationDataController.h"
#endif

inline UAssetImportData* GetAssetImportData(USkeletalMesh* InMesh) {
#if UE4_27_AND_UE5
	return InMesh->GetAssetImportData();
#else
	return InMesh->AssetImportData;
#endif
}

inline void SetAssetImportData(USkeletalMesh* InMesh, UAssetImportData* AssetImportData) {
#if UE4_27_AND_UE5
	InMesh->SetAssetImportData(AssetImportData);
#else
	InMesh->AssetImportData = AssetImportData;
#endif
}

/* FAssetData kept the flat FName fields through 5.0; AssetClassPath/GetObjectPathString are 5.1+ */
inline FName GetAssetDataClass(const FAssetData& AssetData) {
#if UE5_1_BEYOND
	return AssetData.AssetClassPath.GetAssetName();
#else
	return AssetData.AssetClass;
#endif
}

inline FString GetAssetObjectPath(const FAssetData& AssetData) {
#if UE5_1_BEYOND
	return AssetData.GetObjectPathString();
#else
	return AssetData.ObjectPath.ToString();
#endif
}

/* Lays out the rows an animation's notifies sit on in the editor.
 *
 * A cook keeps the notifies and drops the rows, so every notify comes back naming a row that isn't
 * there. The engine works that out for itself the first time it refreshes the animation, and says
 * so first, through an ensure that stops a debugger on every import of an animation carrying
 * notifies. Laid out here instead, one row per row the notifies ask for, so there is nothing left
 * for it to work out.
 *
 * Kept the same shape as the engine's own recovery, down to the row names and the point past which
 * it stops believing an index, so the two agree about where a notify ended up. */
inline void BuildAnimNotifyTracks(UAnimSequenceBase* Sequence) {
#if WITH_EDITORONLY_DATA
	if (!Sequence) {
		return;
	}

	int32 RowCount = Sequence->Notifies.Num() > 0 ? 1 : 0;

	for (FAnimNotifyEvent& Notify : Sequence->Notifies) {
		if (Notify.TrackIndex < 0 || Notify.TrackIndex > 20) {
			Notify.TrackIndex = 0;
		}

		RowCount = FMath::Max(RowCount, Notify.TrackIndex + 1);
	}

	while (Sequence->AnimNotifyTracks.Num() < RowCount) {
		Sequence->AnimNotifyTracks.Add(FAnimNotifyTrack(
			*FString::FromInt(Sequence->AnimNotifyTracks.Num() + 1),
			FLinearColor::White
		));
	}
#endif
}

inline void SetAnimSequenceLength(UAnimSequenceBase* Sequence, const float NewLength) {
	if (!Sequence || NewLength <= 0.f) {
		return;
	}

	const float OldLength = Sequence->GetPlayLength();
	if (FMath::IsNearlyEqual(OldLength, NewLength)) {
		return;
	}

	const FScopedTransaction Transaction(FText::FromString(FString::Printf(TEXT("Change Sequence Length %.3f to %.3f"), OldLength, NewLength)));

	Sequence->Modify();
#if ENGINE_UE5 && ENGINE_MINOR_VERSION >= 2
	Sequence->GetController().SetNumberOfFrames(Sequence->GetController().ConvertSecondsToFrameNumber(NewLength), true);
#else
	if (UAnimSequence* AnimSequence = Cast<UAnimSequence>(Sequence)) {
		Sequence->SequenceLength = NewLength;
#if ENGINE_UE4
		AnimSequence->PostProcessSequence();
#endif
	}
#endif

	Sequence->PostEditChange();
	Sequence->MarkPackageDirty();
}
