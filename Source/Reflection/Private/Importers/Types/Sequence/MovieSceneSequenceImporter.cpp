/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/Sequence/MovieSceneSequenceImporter.h"

#include "Engine/Compatibility.h"
#include "Engine/Log.h"

#include "MovieScene.h"
#include "MovieSceneFolder.h"
#include "MovieSceneSection.h"
#include "MovieSceneSequence.h"
#include "MovieScenePossessable.h"
#include "MovieSceneSpawnable.h"
#include "MovieSceneTrack.h"
#include "UObject/UObjectHash.h"

namespace {
	/* Every track in the scene. Asked of the outer, since GetTracks/GetMasterTracks keep being renamed. */
	TArray<UMovieSceneTrack*> TracksOf(const UMovieScene* Scene) {
		TArray<UObject*> Inside;
		GetObjectsWithOuter(Scene, Inside, false);

		TArray<UMovieSceneTrack*> Tracks;

		for (UObject* One : Inside) {
			if (UMovieSceneTrack* Track = Cast<UMovieSceneTrack>(One)) Tracks.Add(Track);
		}

		return Tracks;
	}

	/* Whether a list of these is one of the lists a scene is walked through */
	bool PartOfAScene(const UClass* Class) {
		return Class != nullptr
			&& (Class->IsChildOf(UMovieSceneTrack::StaticClass())
				|| Class->IsChildOf(UMovieSceneSection::StaticClass())
				|| Class->IsChildOf(UMovieSceneFolder::StaticClass()));
	}

	/* Sorts one of the scene's lists by id. The scene binary searches them, and the cook writes them
	 * in its own order, so an unsorted list answers "not there" for something that is. Reached through
	 * the property since the list and its sort are both private. */
	void PutInOrderOfId(UMovieScene* Scene, const TCHAR* Listed, const TCHAR* Named) {
		const FArrayProperty* List = FindFProperty<FArrayProperty>(Scene->GetClass(), Listed);

		if (List == nullptr) return;

		const FStructProperty* Held = CastField<FStructProperty>(List->Inner);

		if (Held == nullptr || Held->Struct == nullptr) return;

		const FStructProperty* Id = FindFProperty<FStructProperty>(Held->Struct, Named);

		if (Id == nullptr || Id->Struct != TBaseStructure<FGuid>::Get()) return;

		FScriptArrayHelper Holds(List, List->ContainerPtrToValuePtr<void>(Scene));

		for (int32 At = 1; At < Holds.Num(); ++At) {
			for (int32 Back = At; Back > 0; --Back) {
				const FGuid* This = Id->ContainerPtrToValuePtr<FGuid>(Holds.GetRawPtr(Back));
				const FGuid* Before = Id->ContainerPtrToValuePtr<FGuid>(Holds.GetRawPtr(Back - 1));

				if (!(*This < *Before)) break;

				Holds.SwapValues(Back, Back - 1);
			}
		}
	}

	/* What a binding stands for: a spawnable is its template, a possessable is the subobject of that
	 * name inside whatever its parent stands for. */
	UObject* StandsFor(UMovieScene* Scene, const FGuid& Id) {
		if (FMovieSceneSpawnable* Spawnable = Scene->FindSpawnable(Id)) {
			return Spawnable->GetObjectTemplate();
		}

		const FMovieScenePossessable* Possessable = Scene->FindPossessable(Id);

		if (Possessable == nullptr || !Possessable->GetParent().IsValid()) return nullptr;

		UObject* Above = StandsFor(Scene, Possessable->GetParent());

		if (Above == nullptr) return nullptr;

		TArray<UObject*> Held;
		GetObjectsWithOuter(Above, Held, false);

		for (UObject* One : Held) {
			if (One != nullptr && One->GetName() == Possessable->GetName()) return One;
		}

		return nullptr;
	}

	/* Fills in the class each possessable animates, which the cook leaves out.
	 *
	 * The editor reads the property's EditCondition off that class to know which bOverride_ to drive.
	 * Without it a post process track animates its value and the override stays off, so nothing shows. */
	int32 SayWhatIsPossessed(UMovieScene* Scene) {
		int32 Told = 0;

		for (int32 At = 0; At < Scene->GetPossessableCount(); ++At) {
			FMovieScenePossessable& Possessable = Scene->GetPossessable(At);

			if (Possessable.GetPossessedObjectClass() != nullptr) continue;

			UObject* Stands = StandsFor(Scene, Possessable.GetGuid());

			if (Stands == nullptr) continue;

			Possessable.SetPossessedObjectClass(Stands->GetClass());

			Told++;
		}

		return Told;
	}

	/* Drops nulls from every list of tracks, sections or folders, however deep. A class this build
	 * hasn't got leaves a hole, and the editor walks those lists without checking. */
	int32 DropHoles(const UStruct* Struct, void* Data) {
		int32 Dropped = 0;

		for (TFieldIterator<FProperty> It(Struct); It; ++It) {
			FProperty* Property = *It;

			if (const FArrayProperty* AsArray = CastField<FArrayProperty>(Property)) {
				FScriptArrayHelper Holds(AsArray, Property->ContainerPtrToValuePtr<void>(Data));

				/* Held outright only: a weak pointer is checked by whoever holds it. */
				if (const FObjectProperty* Held = CastField<FObjectProperty>(AsArray->Inner)) {
					if (!PartOfAScene(Held->PropertyClass)) continue;

					for (int32 At = Holds.Num() - 1; At >= 0; --At) {
						if (Held->GetObjectPropertyValue(Holds.GetRawPtr(At)) != nullptr) continue;

						Holds.RemoveValues(At);

						Dropped++;
					}

					continue;
				}

				if (const FStructProperty* Inside = CastField<FStructProperty>(AsArray->Inner)) {
					for (int32 At = 0; At < Holds.Num(); ++At) {
						Dropped += DropHoles(Inside->Struct, Holds.GetRawPtr(At));
					}
				}

				continue;
			}

			if (const FStructProperty* Inside = CastField<FStructProperty>(Property)) {
				Dropped += DropHoles(Inside->Struct, Property->ContainerPtrToValuePtr<void>(Data));
			}
		}

		return Dropped;
	}
}

void IMovieSceneSequenceImporter::Prepare(UObject* Asset) const {
	/* The compiled data is the sequence worked out ahead of time, written by the cook so a packaged
	 * game has nothing to work out while it plays. The editor throws away whatever a sequence loads
	 * with and compiles it again, so bringing it across gains nothing: it would be a second copy of
	 * the asset's own structure, naming sections by the order the game cooked them and out of date
	 * the first time anything is changed.
	 *
	 * So it is named here as something not to build, which is the one point at which it can be: by
	 * the time anything is read the exports have already been made. Found by class rather than by
	 * the name the cook gave it, since it is the only thing in the package that is not part of what
	 * the sequence says. */
	const UClass* Compiled = FindClassByType(TEXT("MovieSceneCompiledData"));

	if (Compiled == nullptr) return;

	UObjectSerializer* Objects = GetObjectSerializer();

	TArray<FString> Left;

	for (FUObjectExport* Export : GetContainer()->Exports) {
		if (Export == nullptr || !Export->IsJsonValid()) continue;

		const UClass* Class = Export->GetClass();

		if (Class == nullptr || !Class->IsChildOf(Compiled)) continue;

		const FString Named = Export->GetName().ToString();

		Objects->ExportsToNotDeserialize.AddUnique(Named);
		Left.AddUnique(Named);
	}

	if (Left.Num() == 0) return;

	/* And the reference to it, or following it warns about something that was never missing */
	const TSharedPtr<FJsonObject> Says = GetAssetData();

	TArray<FString> Drop;

	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Says->Values) {
		if (!Pair.Value.IsValid() || Pair.Value->Type != EJson::Object) continue;

		FString Points;

		if (!Pair.Value->AsObject()->TryGetStringField(TEXT("ObjectName"), Points)) continue;

		if (Left.Contains(GetObjectNameFromOuter(Points))) Drop.Add(JsonKeyToString(Pair.Key));
	}

	for (const FString& One : Drop) {
		Says->RemoveField(One);
	}
}

void IMovieSceneSequenceImporter::Repair(UObject* Asset) const {
	const UMovieSceneSequence* Sequence = Cast<UMovieSceneSequence>(Asset);
	UMovieScene* Scene = Sequence != nullptr ? Sequence->GetMovieScene() : nullptr;

	if (Scene == nullptr) return;

	/* In order of id first, since everything below asks the scene for something by one */
	PutInOrderOfId(Scene, TEXT("Spawnables"), TEXT("Guid"));
	PutInOrderOfId(Scene, TEXT("Possessables"), TEXT("Guid"));
	PutInOrderOfId(Scene, TEXT("ObjectBindings"), TEXT("ObjectGuid"));

	/* The scene and everything in it: a hole can be in any of their lists */
	int32 Holes = DropHoles(Scene->GetClass(), Scene);

	TArray<UObject*> Inside;
	GetObjectsWithOuter(Scene, Inside, true);

	for (UObject* One : Inside) {
		if (One != nullptr && PartOfAScene(One->GetClass())) Holes += DropHoles(One->GetClass(), One);
	}

	/* A spawnable with no template has nothing to spawn and nothing to draw it as */
	TArray<FGuid> Gone;

	for (int32 At = 0; At < Scene->GetSpawnableCount(); ++At) {
		const FMovieSceneSpawnable& Spawnable = Scene->GetSpawnable(At);

		if (Spawnable.GetObjectTemplate() == nullptr) Gone.Add(Spawnable.GetGuid());
	}

	for (const FGuid& One : Gone) {
		Scene->RemoveSpawnable(One);
	}

	/* And whatever hung off one. Gone through until nothing more falls out, since taking one away
	 * can orphan the next. */
	for (bool Fell = true; Fell; ) {
		Fell = false;

		for (int32 At = 0; At < Scene->GetPossessableCount(); ++At) {
			const FGuid Parent = Scene->GetPossessable(At).GetParent();

			if (!Parent.IsValid()) continue;
			if (Scene->FindSpawnable(Parent) != nullptr || Scene->FindPossessable(Parent) != nullptr) continue;

			const FGuid Orphan = Scene->GetPossessable(At).GetGuid();

			Scene->RemovePossessable(Orphan);
			Gone.AddUnique(Orphan);

			Fell = true;

			break;
		}
	}

	if (Holes > 0 || Gone.Num() > 0) {
		UE_LOG(LogReflection, Display, TEXT("\"%s\" came across with %d hole(s) in its lists and %d binding(s) standing for something that never arrived, which were taken back out"),
			*GetAssetName(), Holes, Gone.Num());
	}

	/* Once the bindings are settled, since this is worked out by following them */
	if (const int32 Told = SayWhatIsPossessed(Scene); Told > 0) {
		UE_LOG(LogReflection, Display, TEXT("%d of \"%s\"'s bindings were told what kind of thing they animate, which the cook does not say"), Told, *GetAssetName());
	}

#if !UE4_22_BELOW && WITH_EDITORONLY_DATA
	/* The timeline window is editor data the cook strips, so the asset would open no frames wide.
	 * Set from the playback range, and only where the import left it at nothing. */
	FMovieSceneEditorData& Opens = Scene->GetEditorData();

	const TRange<FFrameNumber> Plays = Scene->GetPlaybackRange();

	if (Opens.ViewStart == Opens.ViewEnd && !Plays.GetLowerBound().IsOpen() && !Plays.GetUpperBound().IsOpen()) {
		const FFrameRate Ticks = Scene->GetTickResolution();

		const double From = Ticks.AsSeconds(FFrameTime(Plays.GetLowerBoundValue()));
		const double To = Ticks.AsSeconds(FFrameTime(Plays.GetUpperBoundValue()));

		/* A little either side, the way the editor leaves room around what it is showing */
		const double Room = FMath::Max((To - From) * 0.1, 0.1);

		Opens.ViewStart = From - Room;
		Opens.ViewEnd = To + Room;
		Opens.WorkStart = Opens.ViewStart;
		Opens.WorkEnd = Opens.ViewEnd;
	}
#endif
}

void IMovieSceneSequenceImporter::Validate(UObject* Asset) const {
	const UMovieSceneSequence* Sequence = Cast<UMovieSceneSequence>(Asset);
	const UMovieScene* Scene = Sequence != nullptr ? Sequence->GetMovieScene() : nullptr;

	if (Scene == nullptr) {
		FImportIssues::Report(
			EImportIssue::Data,
			TEXT("The sequence has no movie scene"),
			FString::Printf(TEXT("\"%s\" came across without the movie scene everything about a sequence is kept in, so there is nothing in it to play and nothing for the editor to show."), *GetAssetName())
		);

		return;
	}

	/* A binding stands for one of the things the sequence animates, and with no tracks against it
	 * nothing about that thing is animated: either every track on it was of a class this build
	 * hasn't got, or the export named tracks that resolved to nothing. It reads as an ordinary row
	 * in the editor and plays nothing. */
	const int32 Idle = FImportIssues::ReportIncomplete(
		Scene->GetBindings(),
		[](const FMovieSceneBinding& Binding) { return Binding.GetTracks().Num() == 0; },
		TEXT("bindings have no tracks left on them at all"),
		FString::Printf(TEXT("'%s' carries bindings with no tracks at all, which animate nothing. A track class this build hasn't got is the usual reason."), *GetAssetName())
	);

	const TArray<UMovieSceneTrack*> Tracks = TracksOf(Scene);

	int32 Sections = 0;

	for (const UMovieSceneTrack* Track : Tracks) {
		Sections += Track->GetAllSections().Num();
	}

	UE_LOG(LogReflection, Display, TEXT("\"%s\" animates %d thing(s) through %d track(s) and %d section(s)%s"),
		*GetAssetName(),
		Scene->GetBindings().Num(),
		Tracks.Num(),
		Sections,
		Idle > 0 ? *FString::Printf(TEXT(", %d of them animating nothing"), Idle) : TEXT(""));
}
