/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/Audio/SoundLibraryImporter.h"

#include "Engine/Compatibility.h"
#include "Engine/Log.h"

#include "UObject/UnrealType.h"

namespace {
	/* The two maps a library keeps, reached by name rather than by type. The class belongs to a
	 * plugin this build may not have, and a map can be read and copied without naming what it is. */
	const FName SoundsName("Sounds");
	const FName ListingName("FullLibrary");

	FMapProperty* MapOn(UObject* Asset, const FName Named) {
		return CastField<FMapProperty>(FindFProperty<FProperty>(Asset->GetClass(), Named));
	}

	int32 CountIn(const FMapProperty* Map, UObject* Asset) {
		const FScriptMapHelper Helper(Map, Map->ContainerPtrToValuePtr<void>(Asset));

		return Helper.Num();
	}
}

void ISoundLibraryImporter::Repair(UObject* Asset) const {
	if (Asset == nullptr) return;

	FMapProperty* Sounds = MapOn(Asset, SoundsName);
	FMapProperty* Listing = MapOn(Asset, ListingName);

	/* A build whose library is built some other way, which is not this one's to guess at */
	if (Sounds == nullptr || Listing == nullptr || !Listing->SameType(Sounds)) return;

	/* One that came across is the authored answer, parents and all, and nothing here could work
	 * out what a parent held. Only an empty one is filled. */
	if (CountIn(Listing, Asset) > 0) return;

	const int32 Authored = CountIn(Sounds, Asset);

	if (Authored == 0) return;

	Listing->CopyCompleteValue(
		Listing->ContainerPtrToValuePtr<void>(Asset),
		Sounds->ContainerPtrToValuePtr<void>(Asset)
	);

	UE_LOG(LogReflection, Display,
		TEXT("\"%s\" came across without its listing, so the %d sound(s) it names were made into one"),
		*GetAssetName(), Authored);
}
