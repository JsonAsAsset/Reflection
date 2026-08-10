/* Copyright Reflection Contributors 2024-2026 */

#pragma once

/*
 * This file is used to allow the same code used on UE5 to be used on UE4,
 * it contains structures and classes to replicate missing classes/structs.
*/

/* Every macro below reads ENGINE_MAJOR_VERSION and friends. Nothing here guarantees they are
 * already in scope, so pull in the header that defines them rather than relying on whichever
 * shared PCH the current engine happens to build with. */
#include "Misc/EngineVersionComparison.h"

/* Compiles an experimental version of Reflection */
#ifndef REFLECTION_EXPERIMENTAL
#define REFLECTION_EXPERIMENTAL 0
#endif

#include "Engine/TextureCube.h"
#include "Engine/VolumeTexture.h"

/*
 * Which way the reflect button goes in.
 *
 * On, it asks for asset paths and Cloud fetches the exports behind them. Off, it opens a file
 * dialog and imports the json picked off disk.
 *
 * Only the way in changes. Cloud is compiled in either way, its tools and menu entries work the
 * same, and references still resolve through it while it is running.
 */
#ifndef REFLECTION_CLOUD_SERVER
#define REFLECTION_CLOUD_SERVER 1
#endif

#if ENGINE_MAJOR_VERSION == 5
	#define ENGINE_UE5 1
#else
	#define ENGINE_UE5 0
#endif

#if ENGINE_UE5 && ENGINE_MINOR_VERSION >= 8
	#define UE5_8_BEYOND 1
#else
	#define UE5_8_BEYOND 0
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

/* 5.4 is where UToolMenu grew SetStyleSet and UAnimBlueprint started naming its own pin binding
 * class. Both are only reachable from that version on. */
#if ENGINE_UE5 && ENGINE_MINOR_VERSION >= 4
	#define UE5_4_BEYOND 1
#else
	#define UE5_4_BEYOND 0
#endif

/* 4.25 is where properties stopped being UObjects (UProperty -> FProperty) */
#if ENGINE_UE4 && ENGINE_MINOR_VERSION <= 24
	#define UE4_24_BELOW 1
#else
	#define UE4_24_BELOW 0
#endif

/* 4.24 is where UToolMenus replaced the level editor's FExtender toolbar */
#if ENGINE_UE4 && ENGINE_MINOR_VERSION <= 23
	#define UE4_23_BELOW 1
#else
	#define UE4_23_BELOW 0
#endif

/* The oldest engine Reflection builds against */
#if ENGINE_UE4 && ENGINE_MINOR_VERSION <= 22
	#define UE4_22_BELOW 1
#else
	#define UE4_22_BELOW 0
#endif

#if ENGINE_UE5 && ENGINE_MINOR_VERSION < 2
	#define UE5_1_BELOW 1
#else
	#define UE5_1_BELOW 0
#endif

/* UTexture2DArray is a 4.24 class. On 4.23 the type does not exist under any spelling, so the
 * array paths are compiled out rather than shimmed, and the importer reports the type as one it
 * cannot make. Gated here rather than with the other texture includes at the top of the file
 * because the version macros are only defined above this point. */
#if !UE4_23_BELOW
#include "Engine/Texture2DArray.h"
#endif

/*
 * Properties were UObjects until 4.25 moved them onto FField, renaming UProperty to FProperty
 * and the whole U*Property family with it. Reflection is written against the 4.25+ spelling,
 * so on 4.24 and below the old types are aliased back into it instead of every call site
 * carrying a branch. UProperty is a UField, so Cast/FindField stand in for the FField helpers.
 */
#if UE4_24_BELOW
#include "UObject/UnrealType.h"
#include "UObject/EnumProperty.h"
#include "UObject/TextProperty.h"

using FField = UField;
using FProperty = UProperty;
using FNumericProperty = UNumericProperty;
using FByteProperty = UByteProperty;
using FInt8Property = UInt8Property;
using FInt16Property = UInt16Property;
using FIntProperty = UIntProperty;
using FInt64Property = UInt64Property;
using FUInt16Property = UUInt16Property;
using FUInt32Property = UUInt32Property;
using FUInt64Property = UUInt64Property;
using FFloatProperty = UFloatProperty;
using FDoubleProperty = UDoubleProperty;
using FBoolProperty = UBoolProperty;
using FObjectPropertyBase = UObjectPropertyBase;
using FObjectProperty = UObjectProperty;
using FWeakObjectProperty = UWeakObjectProperty;
using FLazyObjectProperty = ULazyObjectProperty;
using FSoftObjectProperty = USoftObjectProperty;
using FClassProperty = UClassProperty;
using FSoftClassProperty = USoftClassProperty;
using FInterfaceProperty = UInterfaceProperty;
using FNameProperty = UNameProperty;
using FStrProperty = UStrProperty;
using FTextProperty = UTextProperty;
using FArrayProperty = UArrayProperty;
using FMapProperty = UMapProperty;
using FSetProperty = USetProperty;
using FStructProperty = UStructProperty;
using FEnumProperty = UEnumProperty;
using FDelegateProperty = UDelegateProperty;
using FMulticastDelegateProperty = UMulticastDelegateProperty;

/* FField's class object is a plain UClass here, not the separate FFieldClass 4.25 introduced */
using FFieldClass = UClass;

template <typename To, typename From>
FORCEINLINE To* CastField(From* Src) {
	return Cast<To>(Src);
}

template <typename To, typename From>
FORCEINLINE To* CastField(const From* Src) {
	return Cast<To>(const_cast<From*>(Src));
}

template <typename To, typename From>
FORCEINLINE To* CastFieldChecked(From* Src) {
	return CastChecked<To>(Src);
}

template <typename To, typename From>
FORCEINLINE To* CastFieldChecked(const From* Src) {
	return CastChecked<To>(const_cast<From*>(Src));
}

template <typename T>
FORCEINLINE T* FindFProperty(const UStruct* Owner, const TCHAR* FieldName) {
	return FindField<T>(Owner, FieldName);
}

template <typename T>
FORCEINLINE T* FindFProperty(const UStruct* Owner, const FName FieldName) {
	return FindField<T>(Owner, FieldName);
}
#endif

/*
 * TSharedPtr only learned to compare against nullptr later on. Reflection leans on that spelling
 * in a lot of validity checks, so the operators are supplied here rather than rewritten into
 * IsValid() at every call site.
 */
#if UE4_22_BELOW
#include "Templates/SharedPointer.h"

template <typename ObjectType, ESPMode Mode>
FORCEINLINE bool operator==(const TSharedPtr<ObjectType, Mode>& Ptr, TYPE_OF_NULLPTR) {
	return !Ptr.IsValid();
}

template <typename ObjectType, ESPMode Mode>
FORCEINLINE bool operator==(TYPE_OF_NULLPTR, const TSharedPtr<ObjectType, Mode>& Ptr) {
	return !Ptr.IsValid();
}

template <typename ObjectType, ESPMode Mode>
FORCEINLINE bool operator!=(const TSharedPtr<ObjectType, Mode>& Ptr, TYPE_OF_NULLPTR) {
	return Ptr.IsValid();
}

template <typename ObjectType, ESPMode Mode>
FORCEINLINE bool operator!=(TYPE_OF_NULLPTR, const TSharedPtr<ObjectType, Mode>& Ptr) {
	return Ptr.IsValid();
}
#endif

/*
 * FName and FGuid only grew constructors taking an FString later on. Reflection turns strings out
 * of JSON into both of these all over the place, so the version split lives here once rather than
 * at every call site.
 */
#include "Misc/Guid.h"
#include "UObject/NameTypes.h"

inline FName StringToName(const FString& String) {
#if UE4_24_BELOW
	return FName(*String);
#else
	return FName(String);
#endif
}

/* FExpressionOutput spelled its name as an FString until 4.19 and as an FName after. Overloads
 * rather than a version branch: the compiler already knows which one this engine has. */
inline FString OutputNameToString(const FString& Name) {
	return Name;
}

inline FString OutputNameToString(const FName Name) {
	return Name.ToString();
}

/* The same split the other way round, for names Reflection writes rather than reads */
inline void SetExpressionName(FString& Name, const FString& Value) {
	Name = Value;
}

inline void SetExpressionName(FName& Name, const FString& Value) {
	Name = StringToName(Value);
}

/* FString grew the Left/Right/Mid Inline family in 4.25. A free function rather than a shim on the
 * type, so one spelling covers every version: before 4.25 the truncation is a plain reassignment. */
inline void LeftInline(FString& String, const int32 Count) {
#if UE4_24_BELOW
	String = String.Left(Count);
#else
	String.LeftInline(Count);
#endif
}

/*
 * NewObject only started taking a const UClass* later on, and the class Reflection has in hand is
 * almost always const. Returns whatever the current engine's overload wants.
 */
#if UE4_24_BELOW
inline UClass* ToNewObjectClass(const UClass* Class) {
	return const_cast<UClass*>(Class);
}
#else
inline const UClass* ToNewObjectClass(const UClass* Class) {
	return Class;
}
#endif

inline FGuid StringToGuid(const FString& GuidString) {
#if UE4_25_BELOW
	/* What the constructor added later does: parse, and leave the guid invalid when the
	 * string is not one */
	FGuid Guid;

	if (!FGuid::Parse(GuidString, Guid)) {
		Guid.Invalidate();
	}

	return Guid;
#else
	return FGuid(GuidString);
#endif
}

/* 5.8 moved FJsonObject's Values map onto UE::FSharedString keys, which convert to FString only explicitly */
#if UE5_8_BEYOND
#include "Containers/SharedString.h"
#endif

#if UE5_8_BEYOND
inline FString JsonKeyToString(const FString& Key) {
	return Key;
}

inline FString JsonKeyToString(const UE::FSharedString& Key) {
	return FString(*Key);
}
#else
/* Before 5.8 the key already is an FString, so hand the reference through untouched */
inline const FString& JsonKeyToString(const FString& Key) {
	return Key;
}
#endif

#if UE5_8_BEYOND
inline UE::FSharedString StringToJsonKey(const FString& Key) {
	return UE::FSharedString(*Key);
}
#else
inline const FString& StringToJsonKey(const FString& Key) {
	return Key;
}
#endif

#if UE4_26_0
#include "AssetRegistry/Public/AssetRegistryModule.h"
#endif

/* AssetRegistryModule.h only moved under an AssetRegistry/ folder later on */
#if UE4_25_BELOW
#include "AssetRegistryModule.h"
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

/*
 * NamePrivate is FField's own member from 4.25 on. Before that a property was still a UObject and
 * the member belongs to UObjectBase, which keeps it private.
 */
inline FName GetPropertyName(const FProperty* Property) {
#if UE4_24_BELOW
	return Property->GetFName();
#else
	return Property->NamePrivate;
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

	/* These two only grew accessors in 5.1; 5.0 and below still expose the member directly */
	if (UVolumeTexture* VolumeTexture = Cast<UVolumeTexture>(Texture)) {
#if UE5_1_BEYOND
		return VolumeTexture->GetPlatformData();
#else
		return VolumeTexture->PlatformData;
#endif
	}

	/* Derives from UTexture rather than UTexture2D, so the cast above never catches it */
#if !UE4_23_BELOW
	if (UTexture2DArray* Texture2DArray = Cast<UTexture2DArray>(Texture)) {
#if UE5_1_BEYOND
		return Texture2DArray->GetPlatformData();
#else
		return Texture2DArray->PlatformData;
#endif
	}
#endif

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

	/* As above: accessors are 5.1+, the raw member is what 5.0 and below have */
	if (UVolumeTexture* VolumeTexture = Cast<UVolumeTexture>(Texture)) {
#if UE5_1_BEYOND
		VolumeTexture->SetPlatformData(PlatformData);
#else
		VolumeTexture->PlatformData = PlatformData;
#endif
	}

#if !UE4_23_BELOW
	if (UTexture2DArray* Texture2DArray = Cast<UTexture2DArray>(Texture)) {
#if UE5_1_BEYOND
		Texture2DArray->SetPlatformData(PlatformData);
#else
		Texture2DArray->PlatformData = PlatformData;
#endif
	}
#endif
}

/* 4.25 and below build this module without the engine's shared PCH (see Reflection.Build.cs),
 * so the animation types this file uses have to be pulled in explicitly */
#if UE4_25_BELOW
#include "Animation/AnimSequence.h"
#endif

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