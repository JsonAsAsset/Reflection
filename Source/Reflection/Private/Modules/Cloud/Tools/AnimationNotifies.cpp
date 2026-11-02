/* Copyright Reflection Contributors 2024-2026 */

#include "Modules/Cloud/Tools/AnimationNotifies.h"

#include "Engine/AssetCompatibility.h"
#include "Engine/Compatibility.h"
#include "Engine/Log.h"

#include "Containers/Export.h"
#include "Importers/Constructor/ImportIssues.h"
#include "Modules/UI/Animation/SAnimNotifyPickerDialog.h"

#include "Animation/AnimSequenceBase.h"
#include "Animation/AnimNotifies/AnimNotify.h"
#include "Animation/AnimNotifies/AnimNotifyState.h"

namespace {
	/* What a notify entry names the thing that does the work.
	 *
	 * One of the two is filled and the other is null: a notify happens at an instant and a notify
	 * state lasts, and the entry carries whichever it is. Either way the reference is written as
	 * Class'Outer:Name', so the class is the half in front of the quote. */
	FString ClassOf(const TSharedPtr<FJsonObject>& Notify) {
		for (const TCHAR* Field : { TEXT("Notify"), TEXT("NotifyStateClass") }) {
			const TSharedPtr<FJsonObject>* Held = nullptr;

			if (!Notify->TryGetObjectField(Field, Held) || !(*Held).IsValid()) continue;

			FString Named;

			if (!(*Held)->TryGetStringField(TEXT("ObjectName"), Named) || Named.IsEmpty()) continue;

			FString Class;

			return Named.Split(TEXT("'"), &Class, nullptr) ? Class : Named;
		}

		return FString();
	}

	/* And the same question of one already on the sequence, answered the same way. A blueprint
	 * notify's class carries the _C the export writes, and a native one is its own name, so the
	 * two spellings line up without being translated. */
	FString ClassOf(const FAnimNotifyEvent& Notify) {
		if (Notify.Notify != nullptr) return Notify.Notify->GetClass()->GetName();
		if (Notify.NotifyStateClass != nullptr) return Notify.NotifyStateClass->GetClass()->GetName();

		return FString();
	}

	/* And the object itself, which is the export that has to come over with it */
	FString ObjectOf(const TSharedPtr<FJsonObject>& Notify) {
		for (const TCHAR* Field : { TEXT("Notify"), TEXT("NotifyStateClass") }) {
			const TSharedPtr<FJsonObject>* Held = nullptr;

			if (!Notify->TryGetObjectField(Field, Held) || !(*Held).IsValid()) continue;

			FString Named;

			if (!(*Held)->TryGetStringField(TEXT("ObjectName"), Named) || Named.IsEmpty()) continue;

			return GetObjectNameFromOuter(Named);
		}

		return FString();
	}

	/* Whatever an export says it lives in, by name */
	FString OuterOf(const TSharedPtr<FJsonObject>& Export) {
		const TSharedPtr<FJsonObject>* Outer = nullptr;

		if (!Export->TryGetObjectField(TEXT("Outer"), Outer) || !(*Outer).IsValid()) return FString();

		FString Named;

		if (!(*Outer)->TryGetStringField(TEXT("ObjectName"), Named)) return FString();

		return GetObjectNameFromOuter(Named);
	}
}

void TToolAnimationNotifies::Process(UObject* Object, const TArray<TSharedPtr<FJsonValue>>& Exports) {
	UAnimSequenceBase* Sequence = Cast<UAnimSequenceBase>(Object);

	if (Sequence == nullptr) return;

	FUObjectExportContainer* Container = new FUObjectExportContainer(Exports);

	/* The sequence's own export, which is the one the notify list hangs off. Named by the package
	 * it is the asset of, since everything else in there lives inside it. */
	FUObjectExport* SequenceExport = nullptr;

	for (FUObjectExport* Export : Container->Exports) {
		if (Export->IsJsonValid() && Export->JsonObject->HasField(TEXT("Package"))) {
			SequenceExport = Export;

			break;
		}
	}

	if (SequenceExport == nullptr) {
		FImportIssues::Report(
			EImportIssue::MissingAsset,
			TEXT("The Cloud has no sequence at that path"),
			FString::Printf(TEXT("\"%s\" came back without an asset export, so there was nothing to read notifies off."), *Sequence->GetName())
		);

		return;
	}

	const TSharedPtr<FJsonObject> Properties = SequenceExport->GetProperties();

	const TArray<TSharedPtr<FJsonValue>>* Listed = nullptr;

	if (!Properties->TryGetArrayField(TEXT("Notifies"), Listed) || Listed->Num() == 0) {
		FImportIssues::Report(
			EImportIssue::Data,
			TEXT("The game's copy carries no notifies"),
			FString::Printf(TEXT("\"%s\" has none on it, so there was nothing to pick from."), *Sequence->GetName())
		);

		return;
	}

	/* Gathered by class: a notify is only interesting for what it does, and a sequence carries
	 * the same handful of them over and over at different times */
	TArray<FAnimNotifyClassEntry> Classes;

	for (const TSharedPtr<FJsonValue>& Value : *Listed) {
		if (!Value.IsValid() || Value->Type != EJson::Object) continue;

		const FString Class = ClassOf(Value->AsObject());

		if (Class.IsEmpty()) continue;

		FAnimNotifyClassEntry* Already = Classes.FindByPredicate([&Class](const FAnimNotifyClassEntry& Entry) {
			return Entry.Class == Class;
		});

		if (Already == nullptr) {
			FAnimNotifyClassEntry Entry;

			Entry.Class = Class;
			Entry.Named = Value->AsObject()->HasField(TEXT("NotifyName"))
				? Value->AsObject()->GetStringField(TEXT("NotifyName"))
				: Class;

			Already = &Classes[Classes.Add(Entry)];
		}

		Already->Count++;
	}

	if (Classes.Num() == 0) {
		FImportIssues::Report(
			EImportIssue::Data,
			TEXT("None of the notifies name a class"),
			FString::Printf(TEXT("\"%s\" carries %d of them and not one says what it is, which is a notify whose class this build hasn't got."), *Sequence->GetName(), Listed->Num())
		);

		return;
	}

	if (!SAnimNotifyPickerDialog::Open(Sequence->GetName(), Classes)) return;

	TSet<FString> Wanted;

	for (const FAnimNotifyClassEntry& Entry : Classes) {
		if (Entry.Chosen) Wanted.Add(Entry.Class);
	}

	if (Wanted.Num() == 0) return;

	TArray<TSharedPtr<FJsonValue>> Taking;

	/* The objects those entries name, which are the only exports that come over */
	TSet<FString> Keep;

	for (const TSharedPtr<FJsonValue>& Value : *Listed) {
		if (!Value.IsValid() || Value->Type != EJson::Object) continue;
		if (!Wanted.Contains(ClassOf(Value->AsObject()))) continue;

		Taking.Add(Value);

		if (const FString Named = ObjectOf(Value->AsObject()); !Named.IsEmpty()) {
			Keep.Add(Named);
		}
	}

	if (Taking.Num() == 0) return;

	/* And whatever lives inside one of them, however deep: a sound library notify keeps its
	 * context under itself, and the notify is nothing without it */
	for (bool Grew = true; Grew; ) {
		Grew = false;

		for (const FUObjectExport* Export : Container->Exports) {
			if (!Export->IsJsonValid()) continue;

			const FString Named = Export->GetName().ToString();

			if (Keep.Contains(Named)) continue;

			const FString Outer = OuterOf(Export->JsonObject);

			if (!Outer.IsEmpty() && Keep.Contains(Outer)) {
				Keep.Add(Named);

				Grew = true;
			}
		}
	}

	/* Everything else is left where it is. This is the half that keeps the rest of the game's
	 * sequence out: its curves, its compression settings and its skeleton are all exports in the
	 * same answer, and none of them were asked for. */
	UObjectSerializer* Objects = GetObjectSerializer();

	Objects->ExportsToNotDeserialize.Empty();

	for (const FUObjectExport* Export : Container->Exports) {
		if (!Export->IsJsonValid()) continue;

		const FString Named = Export->GetName().ToString();

		if (!Keep.Contains(Named)) Objects->ExportsToNotDeserialize.AddUnique(Named);
	}

	/* Before anything is changed, so undo has the sequence as it was rather than as it ended up */
	Sequence->Modify();

	/* Said to be this asset, so a notify naming the sequence it sits on lands on the one in the
	 * project rather than fetching the game's */
	Objects->SetExportForDeserialization(SequenceExport->JsonObject, Sequence);
	Objects->Parent = Sequence;
	Objects->GetPropertySerializer()->ExportsContainer = Container;

	Objects->DeserializeExports(Container);

	/* Read over the top of the list rather than into it, so what the sequence already carried is
	 * put back afterwards and the new ones follow it */
	TArray<FAnimNotifyEvent> Carried = Sequence->Notifies;

	/* A class that was picked is the game's to say, so whatever the sequence already had of that
	 * class goes. Which is what makes running this twice leave the sequence as it was after the
	 * first time rather than stacking a second copy of everything.
	 *
	 * Only the classes picked: anything else on the sequence is somebody's own work and is not
	 * this tool's to touch. */
	const int32 Had = Carried.Num();

	Carried.RemoveAll([&Wanted](const FAnimNotifyEvent& Notify) {
		const FString Class = ClassOf(Notify);

		return !Class.IsEmpty() && Wanted.Contains(Class);
	});

	const int32 Replaced = Had - Carried.Num();

	const TSharedPtr<FJsonObject> Said = MakeShared<FJsonObject>();
	Said->SetArrayField(TEXT("Notifies"), Taking);

	Objects->DeserializeObjectProperties(Said, Sequence);

	const TArray<FAnimNotifyEvent> Added = Sequence->Notifies;

	Sequence->Notifies = Carried;
	Sequence->Notifies.Append(Added);

	/* A notify was written for the game's sequence, and this one may not be that long */
	double Plays = 0.0;

	if (Properties->TryGetNumberField(TEXT("SequenceLength"), Plays) && Plays > 0.0) {
		const float Here = Sequence->GetPlayLength();

		if (!FMath::IsNearlyEqual(static_cast<float>(Plays), Here, 0.01f)) {
			FImportIssues::Report(
				EImportIssue::Data,
				TEXT("The notifies were written for a longer or shorter animation"),
				FString::Printf(
					TEXT("The game's \"%s\" plays for %.3fs and this one for %.3fs, so the times the notifies carry are not where they would sit on it."),
					*Sequence->GetName(), Plays, Here
				)
			);
		}
	}

	BuildAnimNotifyTracks(Sequence);

	/* Each track holds pointers into the notify array rather than copies, and appending to that
	 * array moves every one of them. Laying the tracks out again only says how many there are, so
	 * without this the rows still point at where the notifies used to be: the editor draws nothing
	 * for the new ones and reads freed memory for the old ones the moment the panel repaints. */
	Sequence->RefreshCacheData();

	Sequence->PostEditChange();
	Sequence->MarkPackageDirty();

	UE_LOG(LogReflection, Display, TEXT("%d notify(s) of %d class(es) were put on \"%s\", taking the place of %d it already had, and %d of other classes were left alone"),
		Added.Num(), Wanted.Num(), *Sequence->GetName(), Replaced, Carried.Num());

	BrowseToWhenFinished(Sequence);
}
