/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Constructor/ImportIssues.h"

#include "Engine/Log.h"
#include "Engine/Package.h"
#include "Modules/UI/Import/SImportIssuesPanel.h"
#include "Settings/Redirector.h"

#define LOCTEXT_NAMESPACE "Reflection.ImportIssues"

namespace {
	TArray<TSharedPtr<FImportIssueAsset>> GAssets;

	/* Innermost last: a reference imports from inside the asset that named it */
	TArray<TSharedPtr<FImportIssueAsset>> GStack;

	/* A reference that hadn't resolved yet, held back until the run is over */
	struct FPendingReference {
		FString AssetName;
		FString AssetPath;
		FString AssetType;

		FString Type;
		FString Name;
		FString Path;
	};

	TArray<FPendingReference> GPending;

	/* Anything reported outside an asset's import lands here */
	const FString UnattributedName = TEXT("Import");

	/* Whether the reference is in the project now, at the path it named or the redirected one.
	 *
	 * And beside the file it named, since an asset out of a package holding several is written to a
	 * file of its own where they are being split apart. Asked only at the path the reference used,
	 * every one of those reads as missing however well it resolved. */
	bool ResolvesAt(const FString& Path) {
		if (LoadObjectByPath<UObject>(Path) != nullptr) {
			return true;
		}

		FString Redirected = Path;
		FRRedirects::Redirect(Redirected);

		return Redirected != Path && LoadObjectByPath<UObject>(Redirected) != nullptr;
	}

	bool ReferenceResolves(const FPendingReference& Pending) {
		if (ResolvesAt(Pending.Path + TEXT(".") + Pending.Name)) {
			return true;
		}

		/* A part of an asset is never a file of its own, so there is no second place to look */
		if (Pending.Name.Contains(TEXT(":"))) {
			return false;
		}

		FString Folder;

		if (!Pending.Path.Split(TEXT("/"), &Folder, nullptr, ESearchCase::IgnoreCase, ESearchDir::FromEnd)) {
			return false;
		}

		return ResolvesAt(Folder + TEXT("/") + Pending.Name + TEXT(".") + Pending.Name);
	}

	TSharedPtr<FImportIssueAsset> FindOrAddAsset(const FString& Name, const FString& Path, const FString& Type) {
		for (const TSharedPtr<FImportIssueAsset>& Asset : GAssets) {
			if (Asset->Name == Name && Asset->Path == Path) {
				return Asset;
			}
		}

		const TSharedPtr<FImportIssueAsset> Asset = MakeShared<FImportIssueAsset>();

		Asset->Name = Name;
		Asset->Path = Path;
		Asset->Type = Type;

		GAssets.Add(Asset);

		return Asset;
	}
}

FText GetImportIssueText(const EImportIssue Kind) {
	switch (Kind) {
		case EImportIssue::MissingAsset: return LOCTEXT("MissingAsset", "Missing Asset");
		case EImportIssue::MissingClass: return LOCTEXT("MissingClass", "Missing Class");
		case EImportIssue::Setting: return LOCTEXT("Setting", "Setting");
		case EImportIssue::Data: return LOCTEXT("Data", "Data");
		default: return LOCTEXT("Failed", "Failed");
	}
}

FLinearColor GetImportIssueColor(const EImportIssue Kind) {
	switch (Kind) {
		case EImportIssue::MissingAsset: return FLinearColor(1.0f, 0.55f, 0.15f);
		case EImportIssue::MissingClass: return FLinearColor(1.0f, 0.75f, 0.2f);
		case EImportIssue::Setting: return FLinearColor(0.45f, 0.7f, 1.0f);
		case EImportIssue::Data: return FLinearColor(0.8f, 0.8f, 0.8f);
		default: return FLinearColor(1.0f, 0.35f, 0.35f);
	}
}

void FImportIssues::Begin() {
	Clear();
}

void FImportIssues::Finish() {
	/* The stack outliving the run means a Push went unpopped, and the next run would inherit it */
	GStack.Reset();

	/* Only the ones still missing now were ever worth reporting */
	for (const FPendingReference& Pending : GPending) {
		if (ReferenceResolves(Pending)) {
			continue;
		}

		ReportFor(
			Pending.AssetName,
			Pending.AssetPath,
			Pending.AssetType,
			EImportIssue::MissingAsset,
			TEXT("Couldn't resolve ") + Pending.Name,
			Pending.Type + TEXT(" at ") + Pending.Path
		);
	}

	GPending.Reset();

	if (GAssets.Num() == 0) {
		return;
	}

	UE_LOG(LogReflection, Warning, TEXT("Reflection finished with %d issue(s) across %d asset(s)."), NumIssues(), GAssets.Num());

	SImportIssuesPanel::Open();
}

void FImportIssues::Push(const FString& Name, const FString& Path, const FString& Type) {
	const TSharedPtr<FImportIssueAsset> Asset = MakeShared<FImportIssueAsset>();

	Asset->Name = Name;
	Asset->Path = Path;
	Asset->Type = Type;

	/* Not added to the report yet: an asset that imports cleanly never gets a row */
	GStack.Push(Asset);
}

void FImportIssues::Pop() {
	if (GStack.Num() > 0) {
		GStack.Pop();
	}
}

void FImportIssues::Report(const EImportIssue Kind, const FString& Summary, const FString& Detail) {
	const TSharedPtr<FImportIssueAsset> Current = GStack.Num() > 0 ? GStack.Last() : nullptr;

	if (Current.IsValid()) {
		ReportFor(Current->Name, Current->Path, Current->Type, Kind, Summary, Detail);

		return;
	}

	ReportFor(UnattributedName, FString(), FString(), Kind, Summary, Detail);
}

void FImportIssues::ReportFor(const FString& Name, const FString& Path, const FString& Type, const EImportIssue Kind, const FString& Summary, const FString& Detail) {
	const TSharedPtr<FImportIssueAsset> Asset = FindOrAddAsset(Name, Path, Type);

	/* One node class missing is one line, however many nodes wanted it */
	for (const TSharedPtr<FImportIssue>& Existing : Asset->Issues) {
		if (Existing->Kind == Kind && Existing->Summary == Summary && Existing->Detail == Detail) {
			Existing->Count++;

			return;
		}
	}

	const TSharedPtr<FImportIssue> Issue = MakeShared<FImportIssue>();

	Issue->Kind = Kind;
	Issue->Summary = Summary;
	Issue->Detail = Detail;

	Asset->Issues.Add(Issue);
}

/* An asset's own exports come through here unresolved too, so nothing is reported until the end */
void FImportIssues::ReportUnresolvedReference(const FString& Type, const FString& Name, const FString& Path) {
	if (Name.IsEmpty()) {
		return;
	}

	/* "Asset:Export" names something inside a package rather than a package */
	if (Name.Contains(TEXT(":"))) {
		return;
	}

	/* Engine content ships with the engine */
	if (Path.StartsWith(TEXT("/Engine/")) || Path.StartsWith(TEXT("Engine/"))) {
		return;
	}

	const TSharedPtr<FImportIssueAsset> Current = GStack.Num() > 0 ? GStack.Last() : nullptr;

	if (Current.IsValid() && !Path.IsEmpty()) {
		/* Same package, so it is one of this asset's own exports */
		if (Path == Current->Path) {
			return;
		}

		/* Same again, for a package the redirects moved out from under the reference */
		if (!Current->Name.IsEmpty() && Path.EndsWith(TEXT("/") + Current->Name)) {
			return;
		}
	}

	FPendingReference Pending;

	Pending.AssetName = Current.IsValid() ? Current->Name : UnattributedName;
	Pending.AssetPath = Current.IsValid() ? Current->Path : FString();
	Pending.AssetType = Current.IsValid() ? Current->Type : FString();

	Pending.Type = Type;
	Pending.Name = Name;
	Pending.Path = Path;

	/* Held until the run ends, where whether it resolved is a question with an answer */
	GPending.Add(Pending);
}

const TArray<TSharedPtr<FImportIssueAsset>>& FImportIssues::GetAssets() {
	return GAssets;
}

int32 FImportIssues::NumIssues() {
	int32 Count = 0;

	for (const TSharedPtr<FImportIssueAsset>& Asset : GAssets) {
		for (const TSharedPtr<FImportIssue>& Issue : Asset->Issues) {
			Count += Issue->Count;
		}
	}

	return Count;
}

void FImportIssues::Clear() {
	GAssets.Reset();
	GStack.Reset();
	GPending.Reset();
}

#undef LOCTEXT_NAMESPACE
