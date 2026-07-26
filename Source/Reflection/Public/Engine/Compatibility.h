/* Copyright Reflection Contributors 2024-2026 */

#pragma once

/*
 * This file is used to allow the same code used on UE5 to be used on UE4,
 * it contains structures and classes to replicate missing classes/structs.
*/

/* Compiles an experimental version of Reflection */
#ifndef REFLECTION_EXPERIMENTAL
#define REFLECTION_EXPERIMENTAL 0

#include "Engine/TextureCube.h"
#include "Engine/VolumeTexture.h"

#endif

#if ENGINE_MAJOR_VERSION == 5
	#define ENGINE_UE5 1
#else
	#define ENGINE_UE5 0
#endif

#if ENGINE_UE5 && ENGINE_MINOR_VERSION >= 6
	#define UE5_6_BEYOND 1
#else
	#define UE5_6_BEYOND 0
#endif

#if ENGINE_UE5 && ENGINE_MINOR_VERSION >= 5
	#define UE5_5_BEYOND 1
#else
	#define UE5_5_BEYOND 0
#endif

#if ENGINE_UE5 && ENGINE_MINOR_VERSION >= 3
	#define UE5_3_BEYOND 1
#else
	#define UE5_3_BEYOND 0
#endif

#if ENGINE_UE5 && ENGINE_MINOR_VERSION >= 4
	#define UE5_4_BEYOND 1
#else
	#define UE5_4_BEYOND 0
#endif

#if ENGINE_UE5 && ENGINE_MINOR_VERSION >= 1
	#define UE5_1_BEYOND 1
#else
	#define UE5_1_BEYOND 0
#endif

#if ENGINE_MAJOR_VERSION == 4
	#define ENGINE_UE4 1
#else
	#define ENGINE_UE4 0
#endif

#if ENGINE_UE4 && ENGINE_MINOR_VERSION == 26 && ENGINE_PATCH_VERSION == 0
	#define UE4_26_0 1
#else
	#define UE4_26_0 0
#endif

#if ENGINE_UE4 && ENGINE_MINOR_VERSION == 26
	#define UE4_26 1
#else
	#define UE4_26 0
#endif

#if ENGINE_UE4 && ENGINE_MINOR_VERSION <= 27
	#define UE4_27_BELOW 1
#else
	#define UE4_27_BELOW 0
#endif

#if ENGINE_UE4 && ENGINE_MINOR_VERSION < 26
	#define UE4_25_BELOW 1
#else
	#define UE4_25_BELOW 0
#endif

#if ENGINE_UE4 && ENGINE_MINOR_VERSION < 27
	#define UE4_27_ONLY_BELOW 1
#else
	#define UE4_27_ONLY_BELOW 0
#endif

#if ENGINE_UE4 && ENGINE_MINOR_VERSION >= 27
	#define UE4_27 1
#else
	#define UE4_27 0
#endif

#if (ENGINE_UE4 && ENGINE_MINOR_VERSION >= 27) || ENGINE_UE5
	#define UE4_27_AND_UE5 1
#else
	#define UE4_27_AND_UE5 0
#endif

#if ENGINE_UE4 && ENGINE_MINOR_VERSION <= 26
	#define UE4_26_BELOW 1
#else
	#define UE4_26_BELOW 0
#endif

#if ENGINE_UE5 && ENGINE_MINOR_VERSION >= 2
	#define UE5_2_BEYOND 1
#else
	#define UE5_2_BEYOND 0
#endif

#if ENGINE_UE5 && ENGINE_MINOR_VERSION < 2
	#define UE5_1_BELOW 1
#else
	#define UE5_1_BELOW 0
#endif

#if UE4_26_0
#include "AssetRegistry/Public/AssetRegistryModule.h"
#endif

#if (ENGINE_UE5 && ENGINE_MINOR_VERSION < 4) || ((ENGINE_UE4 && ENGINE_MINOR_VERSION >= 26) && !(ENGINE_MINOR_VERSION == 26 && ENGINE_PATCH_VERSION == 0))
#include "AssetRegistry/AssetRegistryModule.h"
#endif

#if ENGINE_UE5
#include "Animation/AnimData/IAnimationDataController.h"
#if ENGINE_MINOR_VERSION >= 4
#include "Animation/AnimData/IAnimationDataModel.h"
#endif
#include "AnimDataController.h"
#endif

#if ENGINE_UE5
#include "Styling/AppStyle.h"
using FAppStyle = FAppStyle;
#else

#include "EditorStyleSet.h"

class FAppStyle {
public:
	static const ISlateStyle& Get() {
		return FEditorStyle::Get();
	}

	static FName GetAppStyleSetName() {
		return FEditorStyle::GetStyleSetName();
	}

	static const FSlateBrush* GetBrush(const FName PropertyName) {
		return FEditorStyle::GetBrush(PropertyName);
	}
};

template <typename TObjectType>
class TObjectPtr {
private:
	TWeakObjectPtr<TObjectType> WeakPtr;

public:
	TObjectPtr() {}
	// ReSharper disable once CppNonExplicitConvertingConstructor
	TObjectPtr(TObjectType* InObject) : WeakPtr(InObject) {}

	TObjectType* Get() const { return WeakPtr.Get(); }

	bool IsValid() const { return WeakPtr.IsValid(); }

	void Reset() { WeakPtr.Reset(); }

	void Set(TObjectType* InObject) { WeakPtr = InObject; }

	/* Additional constructor to allow raw pointer conversion */
	TObjectPtr(TObjectType* InObject, bool bRawPointer) : WeakPtr(InObject) {}

	/* Implicit conversion to raw pointer */
	// ReSharper disable once CppNonExplicitConversionOperator
	operator TObjectType*() const { return Get(); }

	/* Overload address-of operator */
	TObjectPtr<TObjectType>* operator&() { return this; }

	/* Assignment operator for TObjectType* */
	TObjectPtr& operator=(TObjectType* InObject) {
		WeakPtr = InObject;
		return *this;
	}

	// Comparison operator for nullptr
	bool operator==(std::nullptr_t) const { return Get() == nullptr; }
	bool operator!=(std::nullptr_t) const { return Get() != nullptr; }
};
#endif

template <typename T>
bool IsObjectPtrValid(TObjectPtr<T> ObjectPtr) {
#if ENGINE_UE5
	return ObjectPtr.Get() != nullptr;
#else
	return ObjectPtr.IsValid();
#endif
}

inline int32 GetElementSize(FProperty* Property) {
#if ENGINE_UE5 && UE5_6_BEYOND
	return Property->GetElementSize();
#else
	return Property->ElementSize;
#endif
}

inline const UObject* GetClassDefaultObject(UClass* Class) {
#if UE5_1_BEYOND
	return GetDefault<UObject>(Class);
#else
	return Class->ClassDefaultObject;
#endif
}

inline UClass* FindClassByType(const FString& Type) {
#if UE5_1_BEYOND
	UClass* Class = FindFirstObject<UClass>(*Type);
#else
	UClass* Class = FindObject<UClass>(ANY_PACKAGE, *Type);
#endif

	return Class;
}

inline FTexturePlatformData* GetPlatformData(UTexture* Texture) {
	if (UTexture2D* Texture2D = Cast<UTexture2D>(Texture)) {
#if ENGINE_UE5
		return Texture2D->GetPlatformData();
#else
		return Texture2D->PlatformData;
#endif
	}
	
	if (UTextureCube* TextureCube = Cast<UTextureCube>(Texture)) {
#if ENGINE_UE5
		return TextureCube->GetPlatformData();
#else
		return TextureCube->PlatformData;
#endif
	}

	if (UVolumeTexture* VolumeTexture = Cast<UVolumeTexture>(Texture)) {
#if ENGINE_UE5
		return VolumeTexture->GetPlatformData();
#else
		return VolumeTexture->PlatformData;
#endif
	}
	
	return nullptr;
}

inline void SetPlatformData(UTexture* Texture, FTexturePlatformData* PlatformData) {
	if (UTexture2D* Texture2D = Cast<UTexture2D>(Texture)) {
#if ENGINE_UE5
		Texture2D->SetPlatformData(PlatformData);
#else
		Texture2D->PlatformData = PlatformData;
#endif
	}
	
	if (UTextureCube* TextureCube = Cast<UTextureCube>(Texture)) {
#if ENGINE_UE5
		TextureCube->SetPlatformData(PlatformData);
#else
		TextureCube->PlatformData = PlatformData;
#endif
	}

	if (UVolumeTexture* VolumeTexture = Cast<UVolumeTexture>(Texture)) {
#if ENGINE_UE5
		VolumeTexture->SetPlatformData(PlatformData);
#else
		VolumeTexture->PlatformData = PlatformData;
#endif
	}
}

inline void UpdateAnimationCaching(UAnimSequenceBase* AnimationSequenceBase) {
	if (UAnimSequence* AnimationSequence = Cast<UAnimSequence>(AnimationSequenceBase)) {
#if UE5_2_BEYOND
		if (ITargetPlatform* RunningPlatform = GetTargetPlatformManagerRef().GetRunningTargetPlatform()) {
#if UE5_6_BEYOND
			AnimationSequence->CacheDerivedDataForPlatform(RunningPlatform);
#else
			AnimationSequence->CacheDerivedData(RunningPlatform);
#endif
		}
#else
		if (AnimationSequence) {
			AnimationSequence->RequestSyncAnimRecompression();
		}
#endif
	}
	
#if ENGINE_UE4
    AnimationSequenceBase->MarkRawDataAsModified();
#endif
}