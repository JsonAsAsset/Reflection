/* Copyright Reflection Contributors 2024-2026 */

#include "Commandlets/AssetSetPropertyCommandlet.h"

#include "Engine/Compatibility.h"
#include "Misc/PackageName.h"
#include "UObject/SavePackage.h"

DEFINE_LOG_CATEGORY_STATIC(LogAssetSetProperty, Display, All);

int32 UAssetSetPropertyCommandlet::Main(const FString& Params) {
	FString Path;

	if (!FParse::Value(*Params, TEXT("path="), Path) || Path.IsEmpty()) {
		UE_LOG(LogAssetSetProperty, Error, TEXT("usage: -run=AssetSetProperty -path=/Game/Some/Asset -set=Property=Value [-set=...]"));

		return 1;
	}

	/* Every -set= on the line, since FParse::Value only ever finds the first. Split at the first
	 * equals sign, because a value is free to hold more of them. */
	TArray<TPair<FString, FString>> Sets;

	/* Read token by token rather than through ParseCommandLine, which keeps a switch with a value
	 * in a map by its name and so holds only the last -set= given. */
	{
		const TCHAR* Stream = *Params;
		FString Token;

		while (FParse::Token(Stream, Token, false)) {
			Token.TrimStartInline();

			if (!Token.StartsWith(TEXT("-set="), ESearchCase::IgnoreCase)) continue;

			const FString Rest = Token.Mid(5).TrimQuotes();

			FString Name;
			FString Value;

			if (!Rest.Split(TEXT("="), &Name, &Value) || Name.IsEmpty()) {
				UE_LOG(LogAssetSetProperty, Error, TEXT("\"%s\" says no property to set: write it as -set=Property=Value"), *Rest);

				return 1;
			}

			Sets.Emplace(Name, Value.TrimQuotes());
		}
	}

	if (Sets.Num() == 0) {
		UE_LOG(LogAssetSetProperty, Error, TEXT("nothing to set: give it at least one -set=Property=Value"));

		return 1;
	}

	/* A package path names the asset of the same name inside it, which is how every asset here is laid out */
	FString ObjectPath = Path;

	if (!ObjectPath.Contains(TEXT("."))) {
		ObjectPath = Path + TEXT(".") + FPackageName::GetShortName(Path);
	}

	UObject* Asset = LoadObject<UObject>(nullptr, *ObjectPath);

	if (Asset == nullptr) {
		UE_LOG(LogAssetSetProperty, Error, TEXT("there is no asset at \"%s\""), *ObjectPath);

		return 1;
	}

	Asset->Modify();

	for (const TPair<FString, FString>& Set : Sets) {
		FProperty* Property = FindFProperty<FProperty>(Asset->GetClass(), *Set.Key);

		if (Property == nullptr) {
			UE_LOG(LogAssetSetProperty, Error, TEXT("%s has no property named %s, nothing saved"), *Asset->GetClass()->GetName(), *Set.Key);

			return 1;
		}

		Asset->PreEditChange(Property);

		/* Read the way a pasted value is, which resolves an object path by loading what it names */
		const TCHAR* Read = Property->ImportText_InContainer(*Set.Value, Asset, Asset, PPF_None);

		FPropertyChangedEvent Changed(Property, EPropertyChangeType::ValueSet);
		Asset->PostEditChangeProperty(Changed);

		if (Read == nullptr) {
			UE_LOG(LogAssetSetProperty, Error, TEXT("\"%s\" could not be read as %s (%s), nothing saved"), *Set.Value, *Set.Key, *Property->GetCPPType());

			return 1;
		}

		/* Said back as it now stands, so a value that read as something other than what was meant shows */
		FString Now;
		Property->ExportText_InContainer(0, Now, Asset, Asset, Asset, PPF_None);

		/* An object that could not be found reads as nothing without failing, which is a value lost rather than set */
		if (const FObjectPropertyBase* Object = CastField<FObjectPropertyBase>(Property); Object != nullptr && !Set.Value.IsEmpty() && Set.Value != TEXT("None")
			&& Object->GetObjectPropertyValue_InContainer(Asset) == nullptr) {
			UE_LOG(LogAssetSetProperty, Error, TEXT("%s was asked to be \"%s\" and nothing by that name could be found, nothing saved"), *Set.Key, *Set.Value);

			return 1;
		}

		UE_LOG(LogAssetSetProperty, Display, TEXT("%s = %s"), *Set.Key, *Now);
	}

	UPackage* Package = Asset->GetOutermost();
	Package->MarkPackageDirty();

	const FString FileName = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());

	/* 5.0 gathered what a save is told into one struct. Before that it is told the same
	 * things one argument at a time. */
#if ENGINE_UE5
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;

	if (!UPackage::SavePackage(Package, Asset, *FileName, SaveArgs)) {
#else
	if (!UPackage::SavePackage(Package, Asset, RF_Public | RF_Standalone, *FileName)) {
#endif
		UE_LOG(LogAssetSetProperty, Error, TEXT("could not save %s"), *FileName);

		return 1;
	}

	UE_LOG(LogAssetSetProperty, Display, TEXT("%d propert%s set on %s, saved %s"), Sets.Num(), Sets.Num() == 1 ? TEXT("y") : TEXT("ies"), *Asset->GetName(), *FileName);

	return 0;
}
