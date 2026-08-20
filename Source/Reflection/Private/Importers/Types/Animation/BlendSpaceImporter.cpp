/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/Animation/BlendSpaceImporter.h"

#include "Animation/AnimSequence.h"
#include "Engine/Log.h"

#if ENGINE_UE4
#include "Animation/BlendSpaceBase.h"
#else
#include "Animation/BlendSpace.h"
#endif

UObject* IBlendSpaceImporter::CreateAsset(UObject* CreatedAsset) {
#if ENGINE_UE5
	auto BlendSpace = NewObject<UBlendSpace>(GetPackage(), GetAssetClass(), *GetAssetName(), RF_Public | RF_Standalone);
#else
	auto BlendSpace = NewObject<UBlendSpaceBase>(GetPackage(), GetAssetClass(), *GetAssetName(), RF_Public | RF_Standalone);
#endif
	
	return IImporter::CreateAsset(BlendSpace);
}

bool IBlendSpaceImporter::Import() {
#if ENGINE_UE4
	auto BlendSpace = Create<UBlendSpaceBase>();
#else
	auto BlendSpace = Create<UBlendSpace>();
#endif
	
	BlendSpace->Modify();

	PreloadSampleAnimations();

	GetObjectSerializer()->DeserializeObjectProperties(GetAssetData(), BlendSpace);

	ResolveEmptySamples(BlendSpace);

	/* Ensure internal state is refreshed after adding all samples */
	BlendSpace->ValidateSampleData();
	BlendSpace->MarkPackageDirty();
	BlendSpace->PostEditChange();
	BlendSpace->PostLoad();

	return OnAssetCreation(BlendSpace);
}

void IBlendSpaceImporter::PreloadSampleAnimations() {
	const TArray<TSharedPtr<FJsonValue>>* SampleArray;

	if (!GetAssetData()->TryGetArrayField(TEXT("SampleData"), SampleArray)) {
		return;
	}

	TSet<FString> Loaded;

	for (const TSharedPtr<FJsonValue>& Value : *SampleArray) {
		const TSharedPtr<FJsonObject> Sample = Value->AsObject();
		const TSharedPtr<FJsonObject>* Reference = nullptr;

		if (!Sample.IsValid() || !Sample->TryGetObjectField(TEXT("Animation"), Reference)) {
			continue;
		}

		/* A blend space names the same animation once per direction it is blended into */
		bool bAlreadyLoaded;
		Loaded.Add((*Reference)->GetStringField(TEXT("ObjectPath")), &bAlreadyLoaded);

		if (bAlreadyLoaded) {
			continue;
		}

		TObjectPtr<UAnimSequence> Animation = nullptr;
		LoadExport(Reference, Animation);
	}
}

void IBlendSpaceImporter::ResolveEmptySamples(UReflectionBlendSpace* BlendSpace) {
	const TArray<TSharedPtr<FJsonValue>>* SampleArray;

	if (!GetAssetData()->TryGetArrayField(TEXT("SampleData"), SampleArray)) {
		return;
	}

	int32 Empty = 0;
	int32 Resolved = 0;

	TArray<FString> Missing;

	for (int32 SampleIndex = 0; SampleIndex < SampleArray->Num(); SampleIndex++) {
		/* Read afresh each time round: resolving one sample can import an asset, and an import is
		 * free to touch anything already in the project */
		const TArray<FBlendSample>& Samples = BlendSpace->GetBlendSamples();

		if (!Samples.IsValidIndex(SampleIndex) || Samples[SampleIndex].Animation != nullptr) {
			continue;
		}

		Empty++;

		const FVector SampleValue = Samples[SampleIndex].SampleValue;

		const TSharedPtr<FJsonObject> Sample = (*SampleArray)[SampleIndex]->AsObject();
		const TSharedPtr<FJsonObject>* Reference = nullptr;

		/* Nothing was named, so nothing is missing: the sample is empty in the game too */
		if (!Sample.IsValid() || !Sample->TryGetObjectField(TEXT("Animation"), Reference)) {
			continue;
		}

		TObjectPtr<UAnimSequence> Animation = nullptr;
		LoadExport(Reference, Animation);

		if (Animation == nullptr) {
			Missing.Add((*Reference)->GetStringField(TEXT("ObjectName")));

			continue;
		}

		if (BlendSpace->UpdateSampleAnimation(Animation, SampleValue)) {
			Resolved++;
		}
	}

	const int32 Deserialized = BlendSpace->GetBlendSamples().Num();

	if (Deserialized != SampleArray->Num()) {
		UE_LOG(LogReflection, Warning, TEXT("\"%s\" was exported with %d samples and came through with %d"), *GetAssetName(), SampleArray->Num(), Deserialized);
	}

	if (Empty > 0) {
		UE_LOG(LogReflection, Warning, TEXT("\"%s\": %d of %d samples came through without an animation, %d of them resolved on a second look"), *GetAssetName(), Empty, SampleArray->Num(), Resolved);
	}

	if (Missing.Num() > 0) {
		UE_LOG(LogReflection, Warning, TEXT("\"%s\" is blended out of animations this project hasn't got: %s"), *GetAssetName(), *FString::Join(Missing, TEXT(", ")));
	}
}