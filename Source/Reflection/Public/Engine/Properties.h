/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "Dom/JsonObject.h"
#include "Engine/Compatibility.h"

#if ENGINE_UE4 && (!UE4_27_BELOW)
inline UStructProperty* LoadStructProperty(const TSharedPtr<FJsonObject>& JsonObject) {
#else
inline FStructProperty* LoadStructProperty(const TSharedPtr<FJsonObject>& JsonObject) {
#endif
    if (!JsonObject.IsValid()) {
        return nullptr;
    }

    FString ObjectName;
    if (!JsonObject->TryGetStringField(TEXT("ObjectName"), ObjectName)) {
        return nullptr;
    }

    const int32 FirstQuoteIndex = ObjectName.Find(TEXT("'"));
    const int32 LastQuoteIndex = ObjectName.Find(TEXT("'"), ESearchCase::IgnoreCase, ESearchDir::FromEnd);

    if (FirstQuoteIndex == INDEX_NONE || LastQuoteIndex == INDEX_NONE || LastQuoteIndex <= FirstQuoteIndex) {
        return nullptr;
    }

    const FString InnerString = ObjectName.Mid(FirstQuoteIndex + 1, LastQuoteIndex - FirstQuoteIndex - 1);

    FString StructName, PropertyName;
    if (!InnerString.Split(TEXT(":"), &StructName, &PropertyName)) {
        return nullptr;
    }

    FString ObjectPath = JsonObject->GetStringField(TEXT("ObjectPath"));

#if UE5_1_BEYOND
    const UStruct* StructDef = FindFirstObject<UStruct>(*StructName);
#else
	const UStruct* StructDef = FindObject<UStruct>(ANY_PACKAGE, *StructName);
#endif

    if (!StructDef) {
        return nullptr;
    }

#if ENGINE_UE4 && (!UE4_27_BELOW)
    UStructProperty* StructProp = FindFProperty<UStructProperty>(StructDef, *PropertyName);
#else
    FStructProperty* StructProp = FindFProperty<FStructProperty>(StructDef, *PropertyName);
#endif
    if (!StructProp) {
        return nullptr;
    }

	return StructProp;
}

inline EObjectFlags ParseObjectFlags(const FString& FlagsString) {
	static const TMap<FString, EObjectFlags> FlagMap = {
		{ TEXT("RF_Public"), RF_Public },
		{ TEXT("RF_Standalone"), RF_Standalone },
		{ TEXT("RF_MarkAsNative"), RF_MarkAsNative },
		{ TEXT("RF_Transactional"), RF_Transactional },
		{ TEXT("RF_ClassDefaultObject"), RF_ClassDefaultObject },
		{ TEXT("RF_ArchetypeObject"), RF_ArchetypeObject },
		{ TEXT("RF_Transient"), RF_Transient },

		{ TEXT("RF_MarkAsRootSet"), RF_MarkAsRootSet },
		{ TEXT("RF_TagGarbageTemp"), RF_TagGarbageTemp },

		{ TEXT("RF_NeedInitialization"), RF_NeedInitialization },
		{ TEXT("RF_NeedLoad"), RF_NeedLoad },
		{ TEXT("RF_KeepForCooker"), RF_KeepForCooker },
		{ TEXT("RF_NeedPostLoad"), RF_NeedPostLoad },
		{ TEXT("RF_NeedPostLoadSubobjects"), RF_NeedPostLoadSubobjects },
		{ TEXT("RF_NewerVersionExists"), RF_NewerVersionExists },
		{ TEXT("RF_BeginDestroyed"), RF_BeginDestroyed },
		{ TEXT("RF_FinishDestroyed"), RF_FinishDestroyed },

		{ TEXT("RF_BeingRegenerated"), RF_BeingRegenerated },
		{ TEXT("RF_DefaultSubObject"), RF_DefaultSubObject },
		{ TEXT("RF_TextExportTransient"), RF_TextExportTransient },
		{ TEXT("RF_InheritableComponentTemplate"), RF_InheritableComponentTemplate },
		{ TEXT("RF_DuplicateTransient"), RF_DuplicateTransient },
		{ TEXT("RF_StrongRefOnFrame"), RF_StrongRefOnFrame },
		{ TEXT("RF_NonPIEDuplicateTransient"), RF_NonPIEDuplicateTransient },
		{ TEXT("RF_WillBeLoaded"), RF_WillBeLoaded },
		{ TEXT("RF_HasExternalPackage"), RF_HasExternalPackage },
#if ENGINE_UE5
#if ENGINE_MINOR_VERSION > 3
		{ TEXT("RF_HasPlaceholderType"), RF_HasPlaceholderType },
		{ TEXT("RF_MirroredGarbage"), RF_MirroredGarbage },
#endif
		{ TEXT("RF_AllocatedInSharedPage"), RF_AllocatedInSharedPage }
#endif
	};

	EObjectFlags Result = RF_NoFlags;

	TArray<FString> Parts;
	FlagsString.ParseIntoArray(Parts, TEXT("|"), true);

	for (FString& Part : Parts) {
		Part.TrimStartAndEndInline();

		if (const EObjectFlags* Found = FlagMap.Find(Part)) {
			Result |= *Found;
		}
	}

	return Result;
}
