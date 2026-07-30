/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "Framework/Notifications/NotificationManager.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Serializers/PropertySerializer.h"
#include "Serializers/ObjectSerializer.h"
#include "Settings/ReflectionSettings.h"

#ifndef __linux__
#include "Windows/WindowsHWrapper.h"
#endif

#include "Utilities/ContentBrowser.h"
#include "Editor.h"
#include "TimerManager.h"
#include "PluginUtils.h"
#include "ISettingsModule.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Interfaces/IPluginManager.h"
#include "Modules/Metadata.h"
#include "VectorField/VectorFieldStatic.h"

#if (ENGINE_MAJOR_VERSION != 4 || ENGINE_MINOR_VERSION < 27)
#include "Engine/DeveloperSettings.h"
#endif

#if ENGINE_UE5
#include "UObject/SavePackage.h"
#endif

inline UReflectionSettings* GetSettings() {
	return GetMutableDefault<UReflectionSettings>();
}

template <typename TEnum> 
TEnum StringToEnum(const FString& StringValue) {
	return StaticEnum<TEnum>() ? static_cast<TEnum>(StaticEnum<TEnum>()->GetValueByNameString(StringValue)) : TEnum();
}

inline void SavePluginSettings(UDeveloperSettings* EditorSettings) {
	EditorSettings->SaveConfig();
	
#if ENGINE_UE5
	EditorSettings->TryUpdateDefaultConfigFile();
	EditorSettings->ReloadConfig(nullptr, nullptr, UE::LCPF_PropagateToInstances);
#else
	EditorSettings->UpdateDefaultConfigFile();
	EditorSettings->ReloadConfig(nullptr, nullptr, UE4::LCPF_PropagateToInstances);
#endif
        	
	EditorSettings->LoadConfig();
}

inline void OpenPluginSettings() {
	FModuleManager::LoadModuleChecked<ISettingsModule>("Settings").ShowViewer("Editor", GReflectionSettingsCategoryName, GReflectionName);
}

/* ReSharper disable once CppParameterNeverUsed */
inline void SetNotificationSubText(FNotificationInfo& Notification, const FText& SubText) {
#if ENGINE_UE5
	Notification.SubText = SubText;
#endif
}

/* Hands a notification to the manager on a call stack that owns itself.
 *
 * A notification is a Slate window, and a blocking Cloud wait keeps the editor painting by
 * ticking Slate from inside whatever call stack it is parked on. One added from there gets built
 * by that nested tick and torn down by the outer one, and the double free lands seconds later
 * when the notification expires, nowhere near the code that caused it. */
inline TSharedPtr<SNotificationItem> AddNotificationWhenSafe(const FNotificationInfo& Info, const SNotificationItem::ECompletionState CompletionState) {
	if (FBlockingRequestScope::IsActive() && GEditor != nullptr) {
		GEditor->GetTimerManager()->SetTimerForNextTick(FTimerDelegate::CreateLambda([Info, CompletionState] {
			if (const TSharedPtr<SNotificationItem> DeferredItem = FSlateNotificationManager::Get().AddNotification(Info)) {
				DeferredItem->SetCompletionState(CompletionState);
			}
		}));

		return nullptr;
	}

	const TSharedPtr<SNotificationItem> NotificationPtr = FSlateNotificationManager::Get().AddNotification(Info);

	if (NotificationPtr.IsValid()) {
		NotificationPtr->SetCompletionState(CompletionState);
	}

	return NotificationPtr;
}

/* Show the user a Notification */
inline auto AppendNotification(const FText& Text, const FText& SubText, const float ExpireDuration,
                               const SNotificationItem::ECompletionState CompletionState, const bool UseSuccessFailIcons,
                               const float WidthOverride) -> void
{
	FNotificationInfo Info = FNotificationInfo(Text);
	Info.ExpireDuration = ExpireDuration;
	Info.bUseLargeFont = true;
	Info.bUseSuccessFailIcons = UseSuccessFailIcons;
	Info.WidthOverride = FOptionalSize(WidthOverride);

	SetNotificationSubText(Info, SubText);

	AddNotificationWhenSafe(Info, CompletionState);
}

/* Show the user a Notification with Subtext */
inline auto AppendNotification(const FText& Text, const FText& SubText, float ExpireDuration,
                               const FSlateBrush* SlateBrush, SNotificationItem::ECompletionState CompletionState,
                               const bool UseSuccessFailIcons, const float WidthOverride) -> void
{
	FNotificationInfo Info = FNotificationInfo(Text);
	Info.ExpireDuration = ExpireDuration;
	Info.bUseLargeFont = true;
	Info.bUseSuccessFailIcons = UseSuccessFailIcons;
	Info.WidthOverride = FOptionalSize(WidthOverride);
	Info.Image = SlateBrush;

	SetNotificationSubText(Info, SubText);

	AddNotificationWhenSafe(Info, CompletionState);
}

inline TSharedPtr<SNotificationItem> AppendNotificationWithHandler(const FText& Text, const FText& SubText, const float ExpireDuration,
	const FSlateBrush* SlateBrush, const SNotificationItem::ECompletionState CompletionState, const bool UseSuccessFailIcons,
	const float WidthOverride, const TFunction<void(FNotificationInfo&)>& PreAddHandler = nullptr)
{
	FNotificationInfo Info(Text);
	Info.ExpireDuration = ExpireDuration;
	Info.bUseLargeFont = true;
	Info.bUseSuccessFailIcons = UseSuccessFailIcons;

	if (WidthOverride != 0.0f) {
		Info.WidthOverride = FOptionalSize(WidthOverride);
	}
	
	Info.Image = SlateBrush;

	SetNotificationSubText(Info, SubText);

	/* Call handler before adding notification */
	if (PreAddHandler) {
		PreAddHandler(Info);
	}

	/* Callers here want the item back to drive it later, so this one is added on the spot and
	 * hands back nothing when a blocking wait made that unsafe */
	return AddNotificationWhenSafe(Info, CompletionState);
}

/* Creates a plugin in the name (may result in bugs if inputted wrong) */
static void CreatePlugin(FString PluginName) {
	/* Plugin creation is different between UE5 and UE4 */
#if ENGINE_UE5
	FPluginUtils::FNewPluginParamsWithDescriptor CreationParams;
	CreationParams.Descriptor.bCanContainContent = true;

	CreationParams.Descriptor.FriendlyName = PluginName;
	CreationParams.Descriptor.Version = 1;
	CreationParams.Descriptor.VersionName = TEXT("1.0");
	CreationParams.Descriptor.Category = TEXT("Other");

	FText FailReason;
	FPluginUtils::FLoadPluginParams LoadParams;
	LoadParams.bEnablePluginInProject = true;
	LoadParams.bUpdateProjectPluginSearchPath = true;
	LoadParams.bSelectInContentBrowser = false;

	FPluginUtils::CreateAndLoadNewPlugin(PluginName, FPaths::ProjectPluginsDir(), CreationParams, LoadParams);
#else
	FPluginUtils::FNewPluginParams CreationParams;
	CreationParams.bCanContainContent = true;

	FText FailReason;
	FPluginUtils::FMountPluginParams LoadParams;
	LoadParams.bEnablePluginInProject = true;
	LoadParams.bUpdateProjectPluginSearchPath = true;
	LoadParams.bSelectInContentBrowser = false;

	FPluginUtils::CreateAndMountNewPlugin(PluginName, FPaths::ProjectPluginsDir(), CreationParams, LoadParams, FailReason);
#endif

#define LOCTEXT_NAMESPACE "UMG"
#if WITH_EDITOR
	/* Setup notification's arguments */
	FFormatNamedArguments Args;
	Args.Add(TEXT("PluginName"), FText::FromString(PluginName));

	/* Create notification */
	FNotificationInfo Info(FText::Format(LOCTEXT("PluginCreated", "Plugin Created: {PluginName}"), Args));
	Info.ExpireDuration = 10.0f;
	Info.bUseLargeFont = true;
	Info.bUseSuccessFailIcons = false;
	Info.WidthOverride = FOptionalSize(350);
	SetNotificationSubText(Info, FText::FromString(FString("Created successfully")));

	const TSharedPtr<SNotificationItem> NotificationPtr = FSlateNotificationManager::Get().AddNotification(Info);
	NotificationPtr->SetCompletionState(SNotificationItem::CS_Success);
#endif
#undef LOCTEXT_NAMESPACE
}

inline UObjectSerializer* CreateObjectSerializer() {
	UPropertySerializer* PropertySerializer = NewObject<UPropertySerializer>();
	UObjectSerializer* ObjectSerializer = NewObject<UObjectSerializer>();
	ObjectSerializer->SetPropertySerializer(PropertySerializer);

	return ObjectSerializer;
}

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

inline void RemoveNotification(TWeakPtr<SNotificationItem> Notification) {
	const TSharedPtr<SNotificationItem> Item = Notification.Pin();

	if (Item.IsValid()) {
		Item->SetFadeOutDuration(0.001);
		Item->Fadeout();
		Notification.Reset();
	}
}

inline void SavePackage(UPackage* Package) {
	const FString PackageName = Package->GetName();
	const FString PackageFileName = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	
#if ENGINE_UE5
	FSavePackageArgs SaveArgs; {
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		SaveArgs.Error = GError;
		SaveArgs.SaveFlags = SAVE_NoError;
	}
		
	UPackage::SavePackage(Package, nullptr, *PackageFileName, SaveArgs);
#else
	UPackage::SavePackage(Package, nullptr, RF_Standalone, *PackageFileName);
#endif
}

/* Needed to compile macro *REGISTER_IMPORTER* */
FORCEINLINE uint32 GetTypeHash(const TArray<FString>& Array) {
	uint32 Hash = 0;
    
	for (const FString& Str : Array) {
		Hash = HashCombine(Hash, GetTypeHash(Str));
	}
    
	return Hash;
}

inline bool HandleAssetCreation(UObject* Asset, UPackage* Package) {
	{
		/* User Failsafe.... */
		const UPackage* AssetOutermostPackage = Asset->GetOutermost();
		const FString PackageName = AssetOutermostPackage->GetName();

		const FString Path = FPackageName::GetLongPackagePath(PackageName);
		if (!Path.StartsWith(TEXT("/")) || Path.Len() < 2) {
			SpawnPrompt("Failsafe", "Here's some reasons why:\n\n- You didn't export it from FModel\n- Reflected it from a random path, not in Exports/.../\n\nPlease reflect it again next time at the proper location.");
			
			return false;
		}
	}

	FAssetRegistryModule::AssetCreated(Asset);
	if (!Asset->MarkPackageDirty()) return false;
	
	Package->SetDirtyFlag(true);

	if (UVectorFieldStatic* VectorFieldStatic = Cast<UVectorFieldStatic>(Asset)) {
		VectorFieldStatic->InitResource();
	}
	
	Asset->PostEditChange();
	Asset->AddToRoot();
	
	Package->FullyLoad();

	BrowseToAsset(Asset);

	if (UVectorFieldStatic* VectorFieldStatic = Cast<UVectorFieldStatic>(Asset)) {
		VectorFieldStatic->Resource = nullptr;
	}
	
	Asset->PostLoad();
	
	return true;
}

inline UAssetImportData* GetAssetImportData(USkeletalMesh* InMesh) {
#if UE4_27_AND_UE5
	return InMesh->GetAssetImportData();
#else
	return InMesh->AssetImportData;
#endif
}

inline void SetAssetImportData(USkeletalMesh* InMesh, UAssetImportData* AssetImportData) {
#if UE4_27_AND_UE5
	InMesh->SetAssetImportData(AssetImportData);
#else
	InMesh->AssetImportData = AssetImportData;
#endif
}

inline TSharedPtr<IPlugin> GetPlugin(const FString& Name) {
	return IPluginManager::Get().FindPlugin(Name);
}

inline FName GetAssetDataClass(const FAssetData& AssetData) {
#if ENGINE_UE4
	return AssetData.AssetClass;
#else
	return AssetData.AssetClassPath.GetAssetName();
#endif
}

inline FString GetAssetObjectPath(const FAssetData& AssetData) {
#if ENGINE_UE4
	return AssetData.ObjectPath.ToString();
#else
	return AssetData.GetObjectPathString();
#endif
}

inline void SetAnimSequenceLength(UAnimSequenceBase* Sequence, const float NewLength) {
	if (!Sequence || NewLength <= 0.f) {
		return;
	}

	const float OldLength = Sequence->GetPlayLength();
	if (FMath::IsNearlyEqual(OldLength, NewLength)) {
		return;
	}

	const FScopedTransaction Transaction(FText::FromString(FString::Printf(TEXT("Change Sequence Length %.3f to %.3f"), OldLength, NewLength)));

	Sequence->Modify();
#if ENGINE_UE5 && ENGINE_MINOR_VERSION >= 2
	Sequence->GetController().SetNumberOfFrames(Sequence->GetController().ConvertSecondsToFrameNumber(NewLength), true);
#else
	if (UAnimSequence* AnimSequence = Cast<UAnimSequence>(Sequence)) {
		Sequence->SequenceLength = NewLength;
#if ENGINE_UE4
		AnimSequence->PostProcessSequence();
#endif
	}
#endif

	Sequence->PostEditChange();
	Sequence->MarkPackageDirty();
}

inline FString GetAssetPath(const UObject* Object) {
	if (!Object) {
		return FString();
	}

	if (const UPackage* Package = Object->GetOutermost()) {
		return Package->GetName();
	}

	return FString();
}

inline void MoveToTransientPackageAndRename(UObject* Object) {
	if (Object) {
		Object->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors);
		Object->SetFlags(RF_Transient);
	}
}

inline void MoveToTransientPackagesAndRename(TArray<UObject*> Objects) {
	for (UObject* Object : Objects) {
		MoveToTransientPackageAndRename(Object);
	}
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