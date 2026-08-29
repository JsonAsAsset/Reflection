/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/Niagara/NiagaraCookedCache.h"

#include "Engine/EngineUtilities.h"

#include "UObject/UnrealType.h"

int32 FNiagaraCookedCache::Restore(FUObjectExportContainer* Container) {
#if WITH_EDITORONLY_DATA
	if (Container == nullptr) return 0;

	static const FString Aside = TEXT("CookedEditorCache");

	int32 Moved = 0;

	for (FUObjectExport* Export : Container->Exports) {
		UObject* Object = Export->Object;

		if (Object == nullptr) continue;

		UClass* Class = Object->GetClass();

		const UObject* Untouched = Class->GetDefaultObject();

		if (Untouched == nullptr) continue;

		for (TFieldIterator<FProperty> Held(Class); Held; ++Held) {
			const FString Name = Held->GetName();

			if (!Name.EndsWith(Aside)) continue;

			FProperty* Reads = FindFProperty<FProperty>(Class, *Name.LeftChop(Aside.Len()));

			/* The two have to be the same thing said twice, or the name is a coincidence */
			if (Reads == nullptr || !Reads->SameType(*Held)) continue;

			void* Cached = Held->ContainerPtrToValuePtr<void>(Object);

			/* An asset that was not cooked this way keeps the cache as it was made, and there is
			 * nothing in it to put anywhere */
			if (Held->Identical(Cached, Held->ContainerPtrToValuePtr<void>(Untouched))) continue;

			Reads->CopyCompleteValue(Reads->ContainerPtrToValuePtr<void>(Object), Cached);

			/* Left empty after, the way the engine leaves it, so the same values are not sitting
			 * in two places for something later to disagree over */
			Held->CopyCompleteValue(Cached, Held->ContainerPtrToValuePtr<void>(Untouched));

			Moved++;
		}
	}

	return Moved;
#else
	return 0;
#endif
}
