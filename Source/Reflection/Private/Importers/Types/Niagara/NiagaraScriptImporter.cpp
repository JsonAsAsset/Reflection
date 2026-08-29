/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/Niagara/NiagaraScriptImporter.h"
#include "Importers/Types/Niagara/NiagaraGraphs.h"
#include "Importers/Types/Niagara/NiagaraVersions.h"
#include "Importers/Types/Niagara/NiagaraCookedCache.h"

#include "Engine/EngineUtilities.h"

#include "EdGraph/EdGraphNode.h"

void INiagaraScriptImporter::Repair(UObject* Asset) const {
	FUObjectExportContainer* Container = GetContainer();

	if (Container == nullptr) return;

	FNiagaraGraphStats Stats;

	FNiagaraGraphs::Rebuild(Container, GetPackage(), GetPropertySerializer(), Stats);

	if (Stats.Unpaired > 0) {
		FImportIssues::Report(
			EImportIssue::Data,
			FString::Printf(TEXT("%d pin pairing(s) name a pin that isn't there"), Stats.Unpaired),
			FString::Printf(
				TEXT("'%s' has nodes that pair their own pins off and name one that didn't come across, so the node drops the pairing and whatever fed the pin stops reaching what it stood for."),
				*GetAssetName())
		);
	}

	/* The source the script is written in.
	 *
	 * A script keeps a version of itself per revision, each naming the source its graph hangs off,
	 * and says which of them it exposes. That is what the version list is for, so it is read
	 * rather than guessed at, and the one it names is the one used.
	 *
	 * Where the list did not come across there is still the source itself sitting under the script.
	 * The last is taken, since versions are added in order and the newest is the one an asset is
	 * left at, and the guess is reported rather than made quietly. */
	const int32 Restored = FNiagaraCookedCache::Restore(Container);

	const FNiagaraVersion Version = FNiagaraVersions::Only(Asset);

	const bool bNamed = FNiagaraVersions::HasObject(Version, TEXT("Source"));

	int32 Sources = 0;

	if (!bNamed) {
		UObject* Source = nullptr;

		for (FUObjectExport* Export : Container->Exports) {
			if (Export->Object == nullptr || Export->GetType() != FName(TEXT("NiagaraScriptSource"))) continue;

			Source = Export->Object;

			Sources++;
		}

		if (Source != nullptr) {
			FNiagaraVersions::SetObject(Version, TEXT("Source"), Source);
		}

		if (Sources > 1) {
			FImportIssues::Report(
				EImportIssue::Data,
				FString::Printf(TEXT("%d graphs to choose between"), Sources),
				FString::Printf(
					TEXT("'%s' carries %d sources and its version list didn't come across, so nothing says which it uses and the last was taken. They are the same script at different points, and another may be the one you want."),
					*GetAssetName(), Sources)
			);
		}
	}

	UE_LOG(LogReflection, Display, TEXT("\"%s\" put %d node(s) in their graphs, made %d pin(s) and joined %d of them%s, %s%s"),
		*GetAssetName(), Stats.Placed, Stats.Pins, Stats.Links,
		Stats.Dangling > 0 ? *FString::Printf(TEXT(", %d reaching nothing"), Stats.Dangling) : TEXT(""),
		bNamed ? TEXT("and took the graph its exposed version names") : *FString::Printf(TEXT("and took the last of %d source(s)"), Sources),
		Restored > 0 ? *FString::Printf(TEXT(", and put %d field(s) back where the asset reads them"), Restored) : TEXT(""));
}

void INiagaraScriptImporter::Validate(UObject* Asset) const {
	FUObjectExportContainer* Container = GetContainer();

	if (Container == nullptr) return;

	/* A node the export gave pins for and that came out with none is a node nothing can reach, and
	 * it looks like an ordinary one until something follows a wire into it */
	int32 Nodes = 0;
	int32 Bare = 0;

	for (FUObjectExport* Export : Container->Exports) {
		const UEdGraphNode* Node = Cast<UEdGraphNode>(Export->Object);

		if (Node == nullptr) continue;

		Nodes++;

		const TArray<TSharedPtr<FJsonValue>>* Listed = nullptr;

		if (Export->IsJsonValid() && Export->JsonObject->TryGetArrayField(TEXT("Pins"), Listed)
			&& Listed->Num() > 0 && Node->Pins.Num() == 0) {
			Bare++;
		}
	}

	if (Bare > 0) {
		FImportIssues::Report(
			EImportIssue::Data,
			FString::Printf(TEXT("%d of %d nodes have no pins"), Bare, Nodes),
			FString::Printf(
				TEXT("'%s' has nodes the export gives pins for that came out with none, so nothing reaches them and nothing they fed is driven."),
				*GetAssetName())
		);
	}

	UE_LOG(LogReflection, Display, TEXT("\"%s\" carries %d node(s)%s"),
		*GetAssetName(), Nodes,
		Bare > 0 ? *FString::Printf(TEXT(", %d of them bare"), Bare) : TEXT(""));
}
