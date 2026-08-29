/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/Animation/BlendSpaceImporter.h"

#include "Animation/AnimSequence.h"
#include "Engine/Log.h"

#if ENGINE_UE4
#include "Animation/BlendSpaceBase.h"
#else
#include "Animation/BlendSpace.h"
#endif

/* Points the sample already sitting at a place at the animation the export named there.
 *
 * 5.8 dropped the one call that did both halves and left the two it was made of, so the sample is
 * looked for here and replaced by its index. The comparison is the engine's own: a sample stands
 * at a point, and a point is somewhere already taken or it is not. */
static bool SetSampleAnimation(UReflectionBlendSpace* BlendSpace, UAnimSequence* Animation, const FVector& SampleValue) {
#if UE5_8_BEYOND
	const TArray<FBlendSample>& Samples = BlendSpace->GetBlendSamples();

	for (int32 Index = 0; Index < Samples.Num(); Index++) {
		const FVector& Standing = Samples[Index].SampleValue;

		if (!FMath::IsNearlyEqual(Standing.X, SampleValue.X, static_cast<FVector::FReal>(UE_KINDA_SMALL_NUMBER))
			|| !FMath::IsNearlyEqual(Standing.Y, SampleValue.Y, static_cast<FVector::FReal>(UE_KINDA_SMALL_NUMBER))
			|| !FMath::IsNearlyEqual(Standing.Z, SampleValue.Z, static_cast<FVector::FReal>(UE_KINDA_SMALL_NUMBER))) {
			continue;
		}

		return BlendSpace->ReplaceSampleAnimation(Index, Animation);
	}

	return false;
#else
	return BlendSpace->UpdateSampleAnimation(Animation, SampleValue);
#endif
}

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
	AlignGridToEngine(BlendSpace);

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

void IBlendSpaceImporter::AlignGridToEngine(UReflectionBlendSpace* BlendSpace) {
/* 5.0 builds a triangulation of its own beside the grid and keeps the grid's setter to itself, so
 * there is no putting one back from out here */
#if ENGINE_UE4
	const TArray<FEditorElement>& Grid = BlendSpace->GetGridSamples();
	const TArray<FBlendSample>& Samples = BlendSpace->GetBlendSamples();

	const FBlendParameter& AxisX = BlendSpace->GetBlendParameter(0);
	const FBlendParameter& AxisY = BlendSpace->GetBlendParameter(1);

	const int32 DivisionsX = FMath::Max(1, AxisX.GridNum);
	const int32 DivisionsY = FMath::Max(1, AxisY.GridNum);

	const int32 PointsX = DivisionsX + 1;
	const int32 PointsY = DivisionsY + 1;

	/* A one dimensional blend space lays its grid out along the one axis, so there is nothing to
	 * transpose and nothing here that would describe it */
	if (Samples.Num() == 0 || Grid.Num() != PointsX * PointsY) {
		return;
	}

	const float StepX = AxisX.GetRange() / DivisionsX;
	const float StepY = AxisY.GetRange() / DivisionsY;

	double AsWritten = 0.0;
	double Transposed = 0.0;

	int32 Counted = 0;

	for (int32 Index = 0; Index < Grid.Num(); Index++) {
		const FEditorElement& Entry = Grid[Index];

		FVector Position = FVector::ZeroVector;
		float TotalWeight = 0.0f;

		for (int32 Vertex = 0; Vertex < FEditorElement::MAX_VERTICES; Vertex++) {
			if (!Samples.IsValidIndex(Entry.Indices[Vertex]) || Entry.Weights[Vertex] <= 0.0f) {
				continue;
			}

			Position += Samples[Entry.Indices[Vertex]].SampleValue * Entry.Weights[Vertex];
			TotalWeight += Entry.Weights[Vertex];
		}

		/* An entry blending nothing sits nowhere in particular, so it says nothing either way */
		if (TotalWeight <= KINDA_SMALL_NUMBER) {
			continue;
		}

		Position /= TotalWeight;
		Counted++;

		const auto ErrorAt = [&Position, &AxisX, &AxisY, StepX, StepY](const int32 GridX, const int32 GridY) {
			return FMath::Abs(Position.X - (AxisX.Min + GridX * StepX)) + FMath::Abs(Position.Y - (AxisY.Min + GridY * StepY));
		};

		/* The same entry read the two ways */
		AsWritten += ErrorAt(Index / PointsY, Index % PointsY);
		Transposed += ErrorAt(Index % PointsX, Index / PointsX);
	}

	/* Half is not a close call: a grid in this engine's order lands every entry on its own point and
	 * the other reading lands almost none of them, so the two are apart by the width of the grid
	 * rather than by rounding */
	if (Counted == 0 || AsWritten <= KINDA_SMALL_NUMBER || Transposed >= AsWritten * 0.5) {
		return;
	}

	TArray<FEditorElement> Reordered;
	Reordered.SetNum(Grid.Num());

	for (int32 GridY = 0; GridY < PointsY; GridY++) {
		for (int32 GridX = 0; GridX < PointsX; GridX++) {
			Reordered[GridX * PointsY + GridY] = Grid[GridY * PointsX + GridX];
		}
	}

	/* Handed to the engine the same way Persona hands over a grid it has just laid out: it
	 * normalises each entry's weights and drops any vertex naming a sample that isn't there. What
	 * Persona maps through is its own sorted point list, and an export's entries already name their
	 * samples the way the asset holds them, so the mapping here is one for one. */
	TArray<int32> SampleIndices;
	SampleIndices.Reserve(Samples.Num());

	for (int32 Sample = 0; Sample < Samples.Num(); Sample++) {
		SampleIndices.Add(Sample);
	}

	BlendSpace->FillupGridElements(SampleIndices, Reordered);

	UE_LOG(LogReflection, Display, TEXT("\"%s\" was cooked for the other engine's grid order, and its %d points were put back"),
		*GetAssetName(), Reordered.Num());
#endif
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

		if (SetSampleAnimation(BlendSpace, Animation, SampleValue)) {
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