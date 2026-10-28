/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/Blueprint/WidgetTreeSource.h"

#include "Engine/Compatibility.h"
#include "Engine/Log.h"
#include "Importers/Constructor/ImportIssues.h"
#include "Modules/Cloud/Cloud.h"
#include "Settings/Runtime.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace {
	/* What an export calls itself, which is where its outer leaves off */
	FString FullPathOf(const TSharedPtr<FJsonObject>& Export) {
		FString Named;

		if (!Export->TryGetStringField(TEXT("Name"), Named)) return FString();

		const TSharedPtr<FJsonObject>* Outer = nullptr;

		if (!Export->TryGetObjectField(TEXT("Outer"), Outer)) return Named;

		const FString Above = FWidgetTreeSource::PathOf(*Outer);

		return Above.IsEmpty() ? Named : Above + TEXT(".") + Named;
	}

	/* Whether Path is Under, or sits somewhere beneath it */
	bool IsUnder(const FString& Path, const FString& Under) {
		return Path == Under || Path.StartsWith(Under + TEXT("."), ESearchCase::CaseSensitive);
	}
}

FString FWidgetTreeSource::PathOf(const TSharedPtr<FJsonObject>& Reference) {
	if (!Reference.IsValid()) return FString();

	FString Named;

	/* A reference is written as Class'Path' by the exporter, and the path is the half that says
	 * where the thing is rather than what it is */
	if (Reference->TryGetStringField(TEXT("ObjectName"), Named)) {
		int32 Quote = INDEX_NONE;

		if (Named.FindChar(TEXT('\''), Quote)) {
			Named = Named.RightChop(Quote + 1);
			Named.RemoveFromEnd(TEXT("'"));
		}

		return Named;
	}

	return FString();
}

FUObjectExport* FWidgetTreeSource::Borrow(FUObjectExportContainer* Container, const FName ClassName) {
	if (Container == nullptr || ClassName.IsNone()) return nullptr;

	/* Named by Mirror rather than by the project, since which widgets were cooked this way is
	 * a fact about the build being read */
	if (GReflectionRuntime.WidgetTreeSources.Num() == 0) {
		UE_LOG(LogReflection, Display, TEXT("no widget tree sources, so nothing to read a tree for %s out of"), *ClassName.ToString());

		return nullptr;
	}

	/* The containing asset names its instance by the generated class, and whether the name
	 * reaching here carries the _C depends on which export the import read it off. */
	TArray<FString> Wanted;
	Wanted.Add(ClassName.ToString());

	if (ClassName.ToString().EndsWith(TEXT("_C"))) {
		Wanted.AddUnique(ClassName.ToString().LeftChop(2));
	} else {
		Wanted.AddUnique(ClassName.ToString() + TEXT("_C"));
	}

	UE_LOG(LogReflection, Display, TEXT("looking for a widget tree for %s across %d source(s)"),
		*FString::Join(Wanted, TEXT(" or ")), GReflectionRuntime.WidgetTreeSources.Num());

	for (const FString& Source : GReflectionRuntime.WidgetTreeSources) {
		if (Source.TrimStartAndEnd().IsEmpty()) continue;

		const TSharedPtr<FJsonObject> Response = Cloud::Export::GetRawBlocking(Source.TrimStartAndEnd());

		if (!Response.IsValid() || !Response->HasField(TEXT("exports"))) {
			UE_LOG(LogReflection, Warning, TEXT("nothing came back for \"%s\", named as a widget tree source"), *Source);

			continue;
		}

		const TArray<TSharedPtr<FJsonValue>> Exports = Response->GetArrayField(TEXT("exports"));

		/* The instance of this widget placed inside that asset, which is the only thing there
		 * saying it is one of these */
		FString InstancePath;

		for (const TSharedPtr<FJsonValue>& Value : Exports) {
			if (!Value.IsValid() || Value->Type != EJson::Object) continue;

			const TSharedPtr<FJsonObject> Export = Value->AsObject();

			if (FString Type; Export->TryGetStringField(TEXT("Type"), Type) && Wanted.Contains(Type)) {
				InstancePath = FullPathOf(Export);

				break;
			}
		}

		if (InstancePath.IsEmpty()) {
			UE_LOG(LogReflection, Display, TEXT("\"%s\" holds no instance of it, %d export(s) looked at"), *Source, Exports.Num());

			continue;
		}

		UE_LOG(LogReflection, Display, TEXT("found it in \"%s\" at %s"), *Source, *InstancePath);

		/* And the tree kept under it, which is the one this widget would have carried */
		FString TreePath;

		for (const TSharedPtr<FJsonValue>& Value : Exports) {
			if (!Value.IsValid() || Value->Type != EJson::Object) continue;

			const TSharedPtr<FJsonObject> Export = Value->AsObject();

			FString Type;

			if (!Export->TryGetStringField(TEXT("Type"), Type) || Type != TEXT("WidgetTree")) continue;

			const TSharedPtr<FJsonObject>* Outer = nullptr;

			if (!Export->TryGetObjectField(TEXT("Outer"), Outer)) continue;
			if (PathOf(*Outer) != InstancePath) continue;

			TreePath = FullPathOf(Export);

			break;
		}

		if (TreePath.IsEmpty()) {
			UE_LOG(LogReflection, Warning,
				TEXT("\"%s\" holds %s at %s but no tree under it"), *Source, *ClassName.ToString(), *InstancePath);

			continue;
		}

		/* The tree and everything it holds, by where each one sits rather than by walking the
		 * references, since a slot names its widget and a widget names its slot */
		TArray<TSharedPtr<FJsonValue>> Taking;

		for (const TSharedPtr<FJsonValue>& Value : Exports) {
			if (!Value.IsValid() || Value->Type != EJson::Object) continue;

			if (IsUnder(FullPathOf(Value->AsObject()), TreePath)) Taking.Add(Value);
		}

		if (Taking.Num() == 0) continue;

		Container->Append(Taking);

		UE_LOG(LogReflection, Display,
			TEXT("read the widget tree for %s out of \"%s\", %d export(s)"), *ClassName.ToString(), *Source, Taking.Num());

		FImportIssues::Report(
			EImportIssue::Data,
			FString::Printf(TEXT("\"%s\" was cooked without its own widget tree"), *ClassName.ToString()),
			FString::Printf(
				TEXT("The tree was read out of \"%s\", where this widget is placed inside another one. What it holds is whatever that asset was cooked with, which is the widget as it appears there rather than as it was authored."),
				*Source)
		);

		/* Appended last, so the tree is the one that was just put in */
		for (int32 Index = Container->Exports.Num() - 1; Index >= 0; --Index) {
			FUObjectExport* Export = Container->Exports[Index];

			if (!Export->IsJsonValid()) continue;
			if (FullPathOf(Export->JsonObject) == TreePath) return Export;
		}
	}

	return nullptr;
}
