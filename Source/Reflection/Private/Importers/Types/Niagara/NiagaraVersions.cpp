/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/Niagara/NiagaraVersions.h"

#include "UObject/UnrealType.h"

FNiagaraVersion FNiagaraVersions::Only(UObject* Asset) {
	FNiagaraVersion Version;

	if (Asset == nullptr) return Version;

	FArrayProperty* Listed = FindFProperty<FArrayProperty>(Asset->GetClass(), TEXT("VersionData"));

	const FStructProperty* Entry = Listed != nullptr ? CastField<FStructProperty>(Listed->Inner) : nullptr;

	if (Entry == nullptr) return Version;

	FScriptArrayHelper Held(Listed, Listed->ContainerPtrToValuePtr<void>(Asset));

	Version.Struct = Entry->Struct;

	/* Which version the asset answers to, which is the one it says it exposes */
	FGuid Exposed;

	if (const FStructProperty* Shown = FindFProperty<FStructProperty>(Asset->GetClass(), TEXT("ExposedVersion"))) {
		Exposed = *Shown->ContainerPtrToValuePtr<FGuid>(Asset);
	}

	const FStructProperty* Stamped = FindFProperty<FStructProperty>(Version.Struct, TEXT("Version"));

	const FStructProperty* Identifies = Stamped != nullptr
		? FindFProperty<FStructProperty>(Stamped->Struct, TEXT("VersionGuid"))
		: nullptr;

	/* The one the export named, where the export carries its versions at all. An asset that keeps
	 * several is several copies of itself at different points, and only the exposed one is the one
	 * it is now. */
	if (Exposed.IsValid() && Identifies != nullptr) {
		for (int32 Index = 0; Index < Held.Num(); Index++) {
			void* One = Held.GetRawPtr(Index);

			if (*Identifies->ContainerPtrToValuePtr<FGuid>(Stamped->ContainerPtrToValuePtr<void>(One)) == Exposed) {
				Version.Data = One;

				return Version;
			}
		}
	}

	/* Nothing named it, so there is one made to be named. Where the versions did not come across
	 * the asset is asked for one it hasn't got, and says it is corrupt. */
	if (Held.Num() == 0) Held.AddValue();

	Version.Data = Held.GetRawPtr(0);

	if (Exposed.IsValid() && Identifies != nullptr) {
		*Identifies->ContainerPtrToValuePtr<FGuid>(Stamped->ContainerPtrToValuePtr<void>(Version.Data)) = Exposed;
	}

	return Version;
}

TArray<FNiagaraVersion> FNiagaraVersions::All(UObject* Asset) {
	TArray<FNiagaraVersion> Versions;

	if (Asset == nullptr) return Versions;

	FArrayProperty* Listed = FindFProperty<FArrayProperty>(Asset->GetClass(), TEXT("VersionData"));

	const FStructProperty* Entry = Listed != nullptr ? CastField<FStructProperty>(Listed->Inner) : nullptr;

	if (Entry == nullptr) return Versions;

	FScriptArrayHelper Held(Listed, Listed->ContainerPtrToValuePtr<void>(Asset));

	for (int32 Index = 0; Index < Held.Num(); Index++) {
		FNiagaraVersion Version;

		Version.Struct = Entry->Struct;
		Version.Data = Held.GetRawPtr(Index);

		Versions.Add(Version);
	}

	return Versions;
}

UObject* FNiagaraVersions::GetObject(const FNiagaraVersion& Version, const TCHAR* Field) {
	if (!Version.IsValid()) return nullptr;

	const FObjectProperty* Named = FindFProperty<FObjectProperty>(Version.Struct, Field);

	return Named != nullptr ? Named->GetObjectPropertyValue(Named->ContainerPtrToValuePtr<void>(Version.Data)) : nullptr;
}

UObject* FNiagaraVersions::GetObjectIn(const FNiagaraVersion& Version, const TCHAR* Field, const TCHAR* Inner) {
	if (!Version.IsValid()) return nullptr;

	const FStructProperty* Holds = FindFProperty<FStructProperty>(Version.Struct, Field);

	const FObjectProperty* Named = Holds != nullptr ? FindFProperty<FObjectProperty>(Holds->Struct, Inner) : nullptr;

	return Named != nullptr
		? Named->GetObjectPropertyValue(Named->ContainerPtrToValuePtr<void>(Holds->ContainerPtrToValuePtr<void>(Version.Data)))
		: nullptr;
}

int32 FNiagaraVersions::NumObjects(const FNiagaraVersion& Version, const TCHAR* Field) {
	if (!Version.IsValid()) return 0;

	FArrayProperty* Listed = FindFProperty<FArrayProperty>(Version.Struct, Field);

	if (Listed == nullptr) return 0;

	return FScriptArrayHelper(Listed, Listed->ContainerPtrToValuePtr<void>(Version.Data)).Num();
}

bool FNiagaraVersions::HasObject(const FNiagaraVersion& Version, const TCHAR* Field) {
	if (!Version.IsValid()) return false;

	const FObjectProperty* Named = FindFProperty<FObjectProperty>(Version.Struct, Field);

	return Named != nullptr && Named->GetObjectPropertyValue(Named->ContainerPtrToValuePtr<void>(Version.Data)) != nullptr;
}

bool FNiagaraVersions::SetObject(const FNiagaraVersion& Version, const TCHAR* Field, UObject* Value) {
	if (!Version.IsValid() || Value == nullptr) return false;

	FObjectProperty* Named = FindFProperty<FObjectProperty>(Version.Struct, Field);

	if (Named == nullptr) return false;

	/* An object of the wrong class in a property is only found when something calls through it */
	if (Named->PropertyClass != nullptr && !Value->IsA(Named->PropertyClass)) return false;

	Named->SetObjectPropertyValue(Named->ContainerPtrToValuePtr<void>(Version.Data), Value);

	return true;
}

bool FNiagaraVersions::SetObjectIn(const FNiagaraVersion& Version, const TCHAR* Field, const TCHAR* Inner, UObject* Value) {
	if (!Version.IsValid() || Value == nullptr) return false;

	const FStructProperty* Holds = FindFProperty<FStructProperty>(Version.Struct, Field);

	if (Holds == nullptr) return false;

	FObjectProperty* Named = FindFProperty<FObjectProperty>(Holds->Struct, Inner);

	if (Named == nullptr) return false;

	if (Named->PropertyClass != nullptr && !Value->IsA(Named->PropertyClass)) return false;

	Named->SetObjectPropertyValue(Named->ContainerPtrToValuePtr<void>(Holds->ContainerPtrToValuePtr<void>(Version.Data)), Value);

	return true;
}

UObject* FNiagaraVersions::MakeObject(const FNiagaraVersion& Version, const TCHAR* Field, UObject* Outer) {
	if (!Version.IsValid() || Outer == nullptr) return nullptr;

	FObjectProperty* Named = FindFProperty<FObjectProperty>(Version.Struct, Field);

	if (Named == nullptr || Named->PropertyClass == nullptr) return nullptr;

	void* At = Named->ContainerPtrToValuePtr<void>(Version.Data);

	if (UObject* Standing = Named->GetObjectPropertyValue(At)) return Standing;

	if (Named->PropertyClass->HasAnyClassFlags(CLASS_Abstract)) return nullptr;

	UObject* Made = NewObject<UObject>(Outer, Named->PropertyClass, NAME_None, RF_Transactional);

	Named->SetObjectPropertyValue(At, Made);

	return Made;
}

int32 FNiagaraVersions::SetObjects(const FNiagaraVersion& Version, const TCHAR* Field, const TArray<UObject*>& Values) {
	if (!Version.IsValid() || Values.Num() == 0) return 0;

	FArrayProperty* Listed = FindFProperty<FArrayProperty>(Version.Struct, Field);

	FObjectProperty* Named = Listed != nullptr ? CastField<FObjectProperty>(Listed->Inner) : nullptr;

	if (Named == nullptr) return 0;

	FScriptArrayHelper Held(Listed, Listed->ContainerPtrToValuePtr<void>(Version.Data));

	Held.EmptyValues();

	int32 Added = 0;

	for (UObject* Value : Values) {
		if (Value == nullptr) continue;

		if (Named->PropertyClass != nullptr && !Value->IsA(Named->PropertyClass)) continue;

		Named->SetObjectPropertyValue(Held.GetRawPtr(Held.AddValue()), Value);

		Added++;
	}

	return Added;
}
