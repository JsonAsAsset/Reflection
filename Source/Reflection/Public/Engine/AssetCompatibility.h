/* Copyright Reflection Contributors 2024-2026 */

#pragma once

/*
 * Version shims that need real asset types in scope, which is why they are not in
 * Compatibility.h: that one is included almost everywhere and is kept cheap.
 */

#include "Engine/Compatibility.h"

#include "Animation/AnimSequence.h"
/* AssetData.h only moved under an AssetRegistry/ folder later on */
#if UE4_22_BELOW
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

inline FName GetAssetDataClass(const FAssetData& AssetData) {
#if ENGINE_UE4
	return AssetData.AssetClass;
#else
	return AssetData.AssetClassPath.GetAssetName();
#endif
}

inline FString GetAssetObjectPath(const FAssetData& AssetData) {
#if ENGINE_UE4
	return AssetData.ObjectPath.ToString();
#else
	return AssetData.GetObjectPathString();
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
