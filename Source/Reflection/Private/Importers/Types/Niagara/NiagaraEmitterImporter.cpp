/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/Niagara/NiagaraEmitterImporter.h"
#include "Importers/Types/Niagara/NiagaraGraphs.h"
#include "Importers/Types/Niagara/NiagaraVersions.h"
#include "Importers/Types/Niagara/NiagaraCookedCache.h"

#include "Engine/EngineUtilities.h"

#include "UObject/UnrealType.h"

namespace {
	/* What a script says it is for.
	 *
	 * An emitter runs five of them and they sit under it named for their jobs, but the name is the
	 * editor's way of putting it and the usage is the script's own. Asked of the script, so a
	 * renamed one still lands where it belongs. */
	FString UsageOf(const UObject* Script) {
		if (Script == nullptr) return FString();

		FProperty* Named = FindFProperty<FProperty>(Script->GetClass(), TEXT("Usage"));

		if (Named == nullptr) return FString();

		const UEnum* Values = nullptr;

		int64 Held = 0;

		if (const FByteProperty* AsByte = CastField<FByteProperty>(Named)) {
			Values = AsByte->Enum;
			Held = AsByte->GetPropertyValue_InContainer(Script);
		} else if (const FEnumProperty* AsEnum = CastField<FEnumProperty>(Named)) {
			Values = AsEnum->GetEnum();
			Held = AsEnum->GetUnderlyingProperty()->GetSignedIntPropertyValue(AsEnum->ContainerPtrToValuePtr<void>(Script));
		}

		return Values != nullptr ? Values->GetNameStringByValue(Held) : FString();
	}

	/* Where a script of that usage belongs on the version, and what it is called there. An empty
	 * inner name is a script the version holds directly rather than inside properties of its own. */
	bool SlotFor(const FString& Usage, FString& OutField, FString& OutInner) {
		if (Usage.StartsWith(TEXT("ParticleSpawnScript"))) {
			OutField = TEXT("SpawnScriptProps");
			OutInner = TEXT("Script");
		} else if (Usage == TEXT("ParticleUpdateScript")) {
			OutField = TEXT("UpdateScriptProps");
			OutInner = TEXT("Script");
		} else if (Usage == TEXT("EmitterSpawnScript")) {
			OutField = TEXT("EmitterSpawnScriptProps");
			OutInner = TEXT("Script");
		} else if (Usage == TEXT("EmitterUpdateScript")) {
			OutField = TEXT("EmitterUpdateScriptProps");
			OutInner = TEXT("Script");
		} else if (Usage == TEXT("ParticleGPUComputeScript")) {
			OutField = TEXT("GPUComputeScript");
			OutInner = FString();
		} else {
			return false;
		}

		return true;
	}

	/* The source a script is written in, which is the one its own version names */
	UObject* SourceOfScript(UObject* Script) {
		for (const FNiagaraVersion& Version : FNiagaraVersions::All(Script)) {
			if (UObject* Written = FNiagaraVersions::GetObject(Version, TEXT("Source"))) return Written;
		}

		return nullptr;
	}

	/* The source a version is written in, asked of the scripts it says it runs.
	 *
	 * The version does not name the source itself in anything that comes across, since that is a
	 * field only an editor build keeps. What does come across is which scripts it runs, and a
	 * script names the source it is written in. All of a version's scripts are written in the one
	 * source, so asking any of them answers for the version. */
	UObject* SourceOfVersion(const FNiagaraVersion& Version) {
		for (const TCHAR* Field : { TEXT("SpawnScriptProps"), TEXT("UpdateScriptProps"),
			TEXT("EmitterSpawnScriptProps"), TEXT("EmitterUpdateScriptProps") }) {
			if (UObject* Script = FNiagaraVersions::GetObjectIn(Version, Field, TEXT("Script"))) {
				if (UObject* Written = SourceOfScript(Script)) return Written;
			}
		}

		if (UObject* Script = FNiagaraVersions::GetObject(Version, TEXT("GPUComputeScript"))) {
			return SourceOfScript(Script);
		}

		return nullptr;
	}
}

void INiagaraEmitterImporter::Repair(UObject* Asset) const {
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

	const int32 Restored = FNiagaraCookedCache::Restore(Container);

	/* Every version the emitter keeps, not only the one it says it is now.
	 *
	 * An emitter that has been revised keeps a copy of itself per revision, and its PostLoad goes
	 * through all of them. One left without the graph it is written in is one the emitter calls
	 * corrupt, whether or not anybody ever asks for that revision. */
	TArray<FNiagaraVersion> Versions = FNiagaraVersions::All(Asset);

	if (Versions.Num() == 0) {
		if (const FNiagaraVersion Made = FNiagaraVersions::Only(Asset); Made.IsValid()) Versions.Add(Made);
	}

	if (Versions.Num() == 0) {
		FImportIssues::Report(
			EImportIssue::Data,
			TEXT("The emitter has nowhere to put what it is made of"),
			FString::Printf(
				TEXT("'%s' has no version list to fill, so its scripts, graph and renderers are in the package with nothing pointing at them."),
				*GetAssetName())
		);

		return;
	}

	/* Everything the emitter is made of sits under it, so what it is made of is what is under it */
	TArray<UObject*> Scripts;
	TArray<UObject*> Sources;
	TArray<UObject*> ScratchPads;
	TArray<UObject*> Renderers;
	TArray<UObject*> EditorData;
	TArray<UObject*> EditorParameters;

	for (FUObjectExport* Export : Container->Exports) {
		UObject* Object = Export->Object;

		if (Object == nullptr || Object->GetOuter() != Asset) continue;

		const FString Type = Export->GetType().ToString();

		if (Type == TEXT("NiagaraScript")) Scripts.Add(Object);
		else if (Type == TEXT("NiagaraScriptSource")) Sources.Add(Object);
		else if (Type == TEXT("NiagaraScratchPadContainer")) ScratchPads.Add(Object);
		else if (Type == TEXT("NiagaraEmitterEditorData")) EditorData.Add(Object);
		else if (Type == TEXT("NiagaraEditorParametersAdapter")) EditorParameters.Add(Object);
		else if (Type.EndsWith(TEXT("RendererProperties"))) Renderers.Add(Object);
	}

	int32 Hung = 0;
	int32 Drawn = 0;
	int32 Pads = 0;
	int32 Made = 0;
	int32 Unplaced = 0;

	/* Taken in turn, so no two versions end up sharing one of something they each want */
	int32 NextPad = 0;
	int32 NextRenderer = 0;
	int32 NextEditorData = 0;
	int32 NextEditorParameters = 0;

	for (int32 At = 0; At < Versions.Num(); At++) {
		const FNiagaraVersion& Version = Versions[At];

		UObject* Written = SourceOfVersion(Version);

		/* Where the version named no scripts either there is only the order left, and an emitter
		 * with a source per version keeps them in the order it made them */
		if (Written == nullptr) {
			Written = Sources.Num() == 1 ? Sources[0] : (Sources.IsValidIndex(At) ? Sources[At] : nullptr);
		}

		if (Written != nullptr && FNiagaraVersions::GetObject(Version, TEXT("GraphSource")) == nullptr) {
			FNiagaraVersions::SetObject(Version, TEXT("GraphSource"), Written);
		}

		/* Scripts the version did not already name, put where their usage says they belong. Only
		 * the ones written in this version's source, so a revision does not end up running
		 * another's scripts alongside its own. */
		for (UObject* Script : Scripts) {
			if (Written != nullptr && SourceOfScript(Script) != Written) continue;

			FString Field, Inner;

			if (!SlotFor(UsageOf(Script), Field, Inner)) {
				if (At == 0) Unplaced++;

				continue;
			}

			const bool bStanding = Inner.IsEmpty()
				? FNiagaraVersions::GetObject(Version, *Field) != nullptr
				: FNiagaraVersions::GetObjectIn(Version, *Field, *Inner) != nullptr;

			if (bStanding) continue;

			const bool bSet = Inner.IsEmpty()
				? FNiagaraVersions::SetObject(Version, *Field, Script)
				: FNiagaraVersions::SetObjectIn(Version, *Field, *Inner, Script);

			if (bSet) Hung++;
		}

		if (FNiagaraVersions::GetObject(Version, TEXT("EditorData")) == nullptr && EditorData.IsValidIndex(NextEditorData)) {
			if (FNiagaraVersions::SetObject(Version, TEXT("EditorData"), EditorData[NextEditorData])) NextEditorData++;
		}

		if (FNiagaraVersions::GetObject(Version, TEXT("EditorParameters")) == nullptr && EditorParameters.IsValidIndex(NextEditorParameters)) {
			if (FNiagaraVersions::SetObject(Version, TEXT("EditorParameters"), EditorParameters[NextEditorParameters])) NextEditorParameters++;
		}

		/* Renderers the version named for itself are left as they are. Only where it named none
		 * are the ones under the emitter handed out, and then a version at a time. */
		if (FNiagaraVersions::NumObjects(Version, TEXT("RendererProperties")) == 0 && Renderers.IsValidIndex(NextRenderer)) {
			TArray<UObject*> Mine;

			if (Versions.Num() == 1) {
				Mine = Renderers;
				NextRenderer = Renderers.Num();
			} else {
				Mine.Add(Renderers[NextRenderer++]);
			}

			Drawn += FNiagaraVersions::SetObjects(Version, TEXT("RendererProperties"), Mine);
		}

		/* The scratch pads hold the scripts written inside the emitter rather than beside it.
		 *
		 * The containers are in the package, sitting under the emitter, but it is the version that
		 * names them and that naming is not in the export. So they are taken in the order the
		 * package put them in, which is the order the emitter made them in.
		 *
		 * Where the package has none left, one is made. An emitter's PostLoad walks both without
		 * asking whether they are there, and an empty container is a scratch pad with nothing in
		 * it while a missing one is nothing at all. */
		for (const TCHAR* Held : { TEXT("ScratchPads"), TEXT("ParentScratchPads") }) {
			if (FNiagaraVersions::GetObject(Version, Held) != nullptr) continue;

			if (ScratchPads.IsValidIndex(NextPad) && FNiagaraVersions::SetObject(Version, Held, ScratchPads[NextPad])) {
				NextPad++;
				Pads++;

				continue;
			}

			if (FNiagaraVersions::MakeObject(Version, Held, Asset) != nullptr) Made++;
		}
	}

	if (Unplaced > 0) {
		FImportIssues::Report(
			EImportIssue::Data,
			FString::Printf(TEXT("%d script(s) have nowhere to go"), Unplaced),
			FString::Printf(
				TEXT("'%s' holds scripts whose usage names no part of an emitter, so nothing runs them."),
				*GetAssetName())
		);
	}

	UE_LOG(LogReflection, Display,
		TEXT("\"%s\" put %d node(s) in their graphs, made %d pin(s) and joined %d of them%s, and hung %d script(s), %d renderer(s) and %d scratch pad(s)%s off %d version(s)%s"),
		*GetAssetName(), Stats.Placed, Stats.Pins, Stats.Links,
		Stats.Dangling > 0 ? *FString::Printf(TEXT(", %d reaching nothing"), Stats.Dangling) : TEXT(""),
		Hung, Drawn, Pads,
		Made > 0 ? *FString::Printf(TEXT(" with %d made for want of one, of %d in the package"), Made, ScratchPads.Num()) : TEXT(""),
		Versions.Num(),
		Restored > 0 ? *FString::Printf(TEXT(", and put %d field(s) back where the asset reads them"), Restored) : TEXT(""));
}

void INiagaraEmitterImporter::Validate(UObject* Asset) const {
	const TArray<FNiagaraVersion> Versions = FNiagaraVersions::All(Asset);

	if (Versions.Num() == 0) return;

	/* A version with no spawn or update script, or with nothing to say where it is written, is one
	 * that runs nothing. It opens looking like an emitter until it is asked to do anything, and a
	 * version nobody selects still goes through PostLoad with the rest of them. */
	TArray<FString> Missing;

	int32 Whole = 0;

	for (int32 At = 0; At < Versions.Num(); At++) {
		const FNiagaraVersion& Version = Versions[At];

		int32 Empty = 0;

		for (const TCHAR* Field : { TEXT("SpawnScriptProps"), TEXT("UpdateScriptProps"),
			TEXT("EmitterSpawnScriptProps"), TEXT("EmitterUpdateScriptProps") }) {
			if (FindFProperty<FStructProperty>(Version.Struct, Field) == nullptr) continue;

			if (FNiagaraVersions::GetObjectIn(Version, Field, TEXT("Script")) == nullptr) Empty++;
		}

		const bool bWritten = FNiagaraVersions::GetObject(Version, TEXT("GraphSource")) != nullptr;

		if (Empty == 0 && bWritten) {
			Whole++;

			continue;
		}

		Missing.Add(FString::Printf(TEXT("version %d is missing %s"), At + 1,
			Empty > 0 && !bWritten
				? *FString::Printf(TEXT("%d of its scripts and its graph"), Empty)
				: (bWritten ? *FString::Printf(TEXT("%d of its scripts"), Empty) : TEXT("its graph"))));
	}

	if (Missing.Num() > 0) {
		FImportIssues::Report(
			EImportIssue::Data,
			FString::Printf(TEXT("%d of the emitter's %d version(s) are incomplete"), Missing.Num(), Versions.Num()),
			FString::Printf(TEXT("'%s' has %s, and the emitter goes through every version it keeps rather than only the one it is now."),
				*GetAssetName(), *FString::Join(Missing, TEXT(", ")))
		);
	}

	UE_LOG(LogReflection, Display, TEXT("\"%s\" has %d of %d version(s) whole"),
		*GetAssetName(), Whole, Versions.Num());
}
