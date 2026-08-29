/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/Niagara/NiagaraParameterCollectionImporter.h"

#include "Engine/EngineUtilities.h"

#include "NiagaraParameterCollection.h"

void INiagaraParameterCollectionImporter::Repair(UObject* Asset) const {
	UNiagaraParameterCollection* Collection = Cast<UNiagaraParameterCollection>(Asset);

	if (Collection == nullptr) return;

	UNiagaraParameterCollectionInstance* Instance = Collection->GetDefaultInstance();

	if (Instance == nullptr) return;

	FNiagaraParameterStore& Store = Instance->GetParameterStore();

	/* Anything the instance came in holding, kept before the store is taken apart.
	 *
	 * Read at the offsets the export gave, which were written for the sizes the parameters really
	 * are, so each value sits where it says it does once the types are right. */
	const TArray<uint8> Arrived = Store.GetParameterDataArray();

	TMap<FName, TArray<uint8>> Overrides;

	for (const FNiagaraVariableWithOffset& Variable : Store.ReadParameterVariables()) {
		const int32 Size = Variable.GetSizeInBytes();

		/* One whose bytes are not all there is left to the parameter's own default rather than read
		 * past the end of what came across */
		if (Variable.Offset < 0 || Size <= 0 || Variable.Offset + Size > Arrived.Num()) continue;

		Overrides.Add(Variable.GetName(), TArray<uint8>(Arrived.GetData() + Variable.Offset, Size));
	}

	/* Laid out again from the collection's own list.
	 *
	 * The instance is made as a subobject the moment the collection is, and squares itself against
	 * a parameter list that is still empty at that point, which leaves its store cleared. What
	 * survives that is the parameters, so the store is built from them, one at a time, with the
	 * engine handing out the offsets. That is what the engine does for a default instance anyway,
	 * and it is what keeps the two from ever disagreeing again.
	 *
	 * A value the instance did come in holding is taken ahead of the parameter's own default, since
	 * that is the instance saying something the collection does not. */
	Store.Empty();

	int32 Kept = 0;

	for (const FNiagaraVariable& Parameter : Collection->GetParameters()) {
		if (const TArray<uint8>* Bytes = Overrides.Find(Parameter.GetName())) {
			Store.SetParameterData(Bytes->GetData(), Parameter, true);

			Kept++;
		} else if (Parameter.IsDataAllocated()) {
			Store.SetParameterData(Parameter.GetData(), Parameter, true);
		} else {
			Store.AddParameter(Parameter, true, false);
		}
	}

	/* Ordered the way the store is searched, which is a binary search and answers wrongly on a list
	 * that is not in that order */
	Store.SortParameters();

	UE_LOG(LogReflection, Display, TEXT("\"%s\" laid out %d parameter(s)%s"),
		*GetAssetName(), Collection->GetParameters().Num(),
		Kept > 0 ? *FString::Printf(TEXT(", %d held by the instance"), Kept) : TEXT(""));
}

void INiagaraParameterCollectionImporter::Validate(UObject* Asset) const {
	UNiagaraParameterCollection* Collection = Cast<UNiagaraParameterCollection>(Asset);

	if (Collection == nullptr) return;

	UNiagaraParameterCollectionInstance* Instance = Collection->GetDefaultInstance();

	if (Instance == nullptr) {
		FImportIssues::Report(
			EImportIssue::Data,
			TEXT("The collection has no default instance"),
			FString::Printf(
				TEXT("'%s' keeps its values in an instance beside it, and without one its parameters are names with nothing behind them."),
				*GetAssetName())
		);

		return;
	}

	const FNiagaraParameterStore& Store = Instance->GetParameterStore();

	const int32 Held = Store.GetParameterDataArray().Num();

	/* Every parameter has to fit inside the blob it points into. One that doesn't is the store
	 * disagreeing with itself, and the engine finds it the moment anything reads that parameter. */
	int32 Overruns = 0;

	for (const FNiagaraVariableWithOffset& Variable : Store.ReadParameterVariables()) {
		if (Variable.Offset < 0 || Variable.Offset + Variable.GetSizeInBytes() > Held) {
			Overruns++;
		}
	}

	if (Overruns > 0) {
		FImportIssues::Report(
			EImportIssue::Data,
			FString::Printf(TEXT("%d parameter(s) reach past the end of the store"), Overruns),
			FString::Printf(
				TEXT("'%s' holds parameters whose offset and size put them outside the values it carries, so reading one reads whatever follows."),
				*GetAssetName())
		);
	}

	UE_LOG(LogReflection, Display, TEXT("\"%s\" carries %d parameter(s) over %d byte(s)%s"),
		*GetAssetName(), Store.ReadParameterVariables().Num(), Held,
		Overruns > 0 ? TEXT(", not all of which fit") : TEXT(""));
}
