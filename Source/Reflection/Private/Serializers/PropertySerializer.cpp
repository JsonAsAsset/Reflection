/* Copyright Reflection Contributors 2024-2026 */

#include "Serializers/PropertySerializer.h"

#include "GameplayTagContainer.h"
#include "GameplayTagsEditorModule.h"
#include "GameplayTagsManager.h"
#include "Importers/Constructor/Importer.h"
#include "Serializers/ObjectSerializer.h"
#include "UObject/TextProperty.h"
#include "Misc/Base64.h"

/* Struct Serializers */
#include "Distributions.h"

#if ENGINE_UE4
#include "MaterialExpressionIO.h"
#endif

/* Reached through for its outputs, which a forward declaration cannot answer for */
#include "Materials/MaterialExpression.h"

#include "MovieSceneSection.h"
#include "Engine/FontFace.h"
#include "Importers/Types/Cascade/ParticleSystemDecooking.h"
#include "Serializers/Structs/DateTimeSerializer.h"
#include "Serializers/Structs/BitArrayWrapperSerializer.h"
#include "Serializers/Structs/FallbackStructSerializer.h"
#include "Serializers/Structs/ConnectivityCubeSerializer.h"
#include "Serializers/Structs/NiagaraVariableSerializer.h"
#include "Serializers/Structs/TimeSpanSerializer.h"

#include "Settings/Runtime.h"
#include "Utilities/AssetPaths.h"

/* Said at Log rather than Error, or nothing below an error is ever seen */
DECLARE_LOG_CATEGORY_CLASS(LogReflectionPropertySerializer, Log, All);

namespace {
	/* Whether a reference has something to point at, without making it point at it. */
	bool AlreadyHere(const FSoftObjectPath& Reference) {
		if (Reference.ResolveObject() != nullptr) {
			return true;
		}

		FString Held;

		return PackageFileOf(Reference.GetLongPackageName(), Held);
	}

	/* The components being filled in right now, innermost last. */
	TArray<UObject*> GBeingFilled;

	/* Marks one as being filled for as long as the call filling it is running. */
	struct FFillScope {
		explicit FFillScope(UObject* InObject) : Object(InObject) { GBeingFilled.Add(Object); }

		~FFillScope() { GBeingFilled.Remove(Object); }

		UObject* Object;
	};
}

/* A tag the game had and this project has not. */
static FGameplayTag RequestOrMakeGameplayTag(const FString& Named) {
	if (Named.IsEmpty() || Named == TEXT("None")) return FGameplayTag();

	FGameplayTag Asked = FGameplayTag::RequestGameplayTag(FName(*Named), false);

	if (Asked.IsValid()) return Asked;

#if WITH_EDITOR
	if (IGameplayTagsEditorModule* Tags = FModuleManager::Get().LoadModulePtr<IGameplayTagsEditorModule>(TEXT("GameplayTagsEditor"))) {
		if (Tags->AddNewGameplayTagToINI(Named, TEXT(""))) {
			Asked = FGameplayTag::RequestGameplayTag(FName(*Named), false);

			UE_LOG(LogReflectionPropertySerializer, Display, TEXT("\"%s\" is a tag this project hadn't got, so it was added to the list"), *Named);
		}
	}
#endif

	if (!Asked.IsValid()) {
		UE_LOG(LogReflectionPropertySerializer, Warning, TEXT("\"%s\" is a tag this project hasn't got, and it could not be added"), *Named);
	}

	return Asked;
}

/* Material Attributes ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */

namespace {
	/* 4.25 ~~> 425, 5.3 ~~> 503. Only ever compared against another one of these. */
	constexpr int32 ToVersionKey(const int32 Major, const int32 Minor) {
		return Major * 100 + Minor;
	}

	/* The outputs of a BreakMaterialAttributes node, in the order the node lists them, against the
	 * engines each one is there for. Read out of the engine sources rather than worked out from a
	 * material: 4.16 through 5.7 all lay the common attributes out in this same relative order and
	 * only ever add one or drop one, which is the whole reason an index recorded against one
	 * engine can be named on another.
	 *
	 * Anisotropy and Tangent arrive in 4.25, ShadingModel in 4.23, Displacement in 5.3.
	 * WorldDisplacement and TessellationMultiplier leave with tessellation in 5.0. */
	struct FBreakAttributeSlot {
		const TCHAR* Name;

		/* Version keys, 0 meaning no bound on that end */
		int32 First;
		int32 Last;

		bool IsPresentIn(const int32 Version) const {
			return (First == 0 || Version >= First) && (Last == 0 || Version <= Last);
		}
	};

	const FBreakAttributeSlot BreakAttributeSlots[] = {
		{ TEXT("BaseColor"),              0,   0   },
		{ TEXT("Metallic"),               0,   0   },
		{ TEXT("Specular"),               0,   0   },
		{ TEXT("Roughness"),              0,   0   },
		{ TEXT("Anisotropy"),             425, 0   },
		{ TEXT("EmissiveColor"),          0,   0   },
		{ TEXT("Opacity"),                0,   0   },
		{ TEXT("OpacityMask"),            0,   0   },
		{ TEXT("Normal"),                 0,   0   },
		{ TEXT("Tangent"),                425, 0   },
		{ TEXT("WorldPositionOffset"),    0,   0   },
		{ TEXT("WorldDisplacement"),      0,   427 },
		{ TEXT("TessellationMultiplier"), 0,   427 },
		{ TEXT("SubsurfaceColor"),        0,   0   },
		{ TEXT("ClearCoat"),              0,   0   },
		{ TEXT("ClearCoatRoughness"),     0,   0   },
		{ TEXT("AmbientOcclusion"),       0,   0   },
		{ TEXT("Refraction"),             0,   0   },
		{ TEXT("CustomizedUV0"),          0,   0   },
		{ TEXT("CustomizedUV1"),          0,   0   },
		{ TEXT("CustomizedUV2"),          0,   0   },
		{ TEXT("CustomizedUV3"),          0,   0   },
		{ TEXT("CustomizedUV4"),          0,   0   },
		{ TEXT("CustomizedUV5"),          0,   0   },
		{ TEXT("CustomizedUV6"),          0,   0   },
		{ TEXT("CustomizedUV7"),          0,   0   },
		{ TEXT("PixelDepthOffset"),       0,   0   },
		{ TEXT("ShadingModel"),           423, 0   },
		{ TEXT("Displacement"),           503, 0   },
	};

	/* What the engine the json came from called the output sitting at this index */
	FString GetSourceBreakOutputName(const int32 OutputIndex) {
		if (OutputIndex < 0 || GReflectionRuntime.MajorVersion <= 0 || GReflectionRuntime.MinorVersion < 0) {
			return FString();
		}

		const int32 SourceVersion = ToVersionKey(GReflectionRuntime.MajorVersion, GReflectionRuntime.MinorVersion);

		int32 Index = 0;

		for (const FBreakAttributeSlot& Slot : BreakAttributeSlots) {
			if (!Slot.IsPresentIn(SourceVersion)) {
				continue;
			}

			if (Index == OutputIndex) {
				return Slot.Name;
			}

			Index++;
		}

		return FString();
	}

	/* Where the same attribute sits on the node this engine built, asked of the node itself. The
	 * outputs are named in the constructor of every version, so this side needs nothing written
	 * down: whatever this engine put in the list is what the index has to end up matching. */
	int32 FindBreakOutputIndex(const UMaterialExpression* Break, const FString& Name) {
		for (int32 Index = 0; Index < Break->Outputs.Num(); Index++) {
			if (OutputNameToString(Break->Outputs[Index].OutputName) == Name) {
				return Index;
			}
		}

		return INDEX_NONE;
	}

	/* An input reading a BreakMaterialAttributes names its attribute by position, and the list it
	 * is a position into is not the same list on every engine. Ported straight across, an input
	 * lands on whichever attribute happens to sit at that index here, which is the graph coming
	 * out wired one or two pins off all the way down.
	 *
	 * The index is turned back into the attribute it meant on the engine that wrote it, then asked
	 * for again by name here. */
	void RemapBreakMaterialAttributesOutput(FExpressionInput* Input) {
		if (Input == nullptr || Input->Expression == nullptr) {
			return;
		}

		if (Input->Expression->GetClass()->GetName() != TEXT("MaterialExpressionBreakMaterialAttributes")) {
			return;
		}

		const FString AttributeName = GetSourceBreakOutputName(Input->OutputIndex);

		/* No metadata to say which engine this came from, so there is nothing to map between */
		if (AttributeName.IsEmpty()) {
			return;
		}

		const int32 TargetIndex = FindBreakOutputIndex(Input->Expression, AttributeName);

		/* An attribute this engine does not have. Leaving the index where it is would read some
		 * unrelated attribute instead, and there is no honest answer to give, so the wire comes
		 * off and the input falls back to whatever the node it sits on defaults to.  */
		if (TargetIndex == INDEX_NONE) {
			UE_LOG(LogReflectionPropertySerializer, Warning, TEXT("Disconnected a material attribute input: this engine has no \"%s\""), *AttributeName);

			Input->Expression = nullptr;
			Input->OutputIndex = 0;

			return;
		}

		Input->OutputIndex = TargetIndex;
	}
}

/* The property where it points at an object, held outright or through an interface. Both are read
 * the same way; only writing the answer back differs. */
static const FProperty* PointsAtObject(const FProperty* Property) {
	if (Property->IsA<FObjectPropertyBase>() || Property->IsA<FInterfaceProperty>()) return Property;

	return nullptr;
}

/* What it holds now. An interface keeps the object beside a pointer into the part answering for it. */
static UObject* PointedObject(const FProperty* Property, const void* Value) {
	if (const FObjectPropertyBase* AsObject = CastField<const FObjectPropertyBase>(Property)) {
		return AsObject->GetObjectPropertyValue(Value);
	}

	if (CastField<const FInterfaceProperty>(Property) != nullptr) {
		return static_cast<const FScriptInterface*>(Value)->GetObject();
	}

	return nullptr;
}

/* And what it will take, which is a class either way */
static UClass* PointedClass(const FProperty* Property) {
	if (const FObjectPropertyBase* AsObject = CastField<const FObjectPropertyBase>(Property)) {
		return AsObject->PropertyClass;
	}

	if (const FInterfaceProperty* AsInterface = CastField<const FInterfaceProperty>(Property)) {
		return AsInterface->InterfaceClass;
	}

	return nullptr;
}

/* An object property takes whatever pointer it is handed without checking the type, so anything
 * that resolves to the wrong class lands in the property and is only discovered when something
 * calls through it, by which point the editor is already gone. */
static void SetObjectPropertyValueChecked(const FProperty* Property, void* OutValue, UObject* Object) {
	/* A subobject the class made for itself is part of what the object is, and plenty of the engine
	 * reaches through one without checking: a material function asked for its expressions casts
	 * onto whatever is in EditorOnlyData and takes nothing for an answer. A reference that came
	 * across as nothing is not a reason to take that away, so it is left where it is.
	 *
	 * Anything living under another object rather than under a package is one of these, however it
	 * was made. A material function makes its editor data in PostInitProperties rather than as a
	 * default subobject, and it is no less part of the function for that. */
	if (Object == nullptr) {
		const UObject* Current = PointedObject(Property, OutValue);

		if (Current != nullptr && Current->GetOuter() != nullptr && !Current->GetOuter()->IsA<UPackage>()) {
			return;
		}
	}

	const FInterfaceProperty* AsInterface = CastField<const FInterfaceProperty>(Property);

	if (const UClass* Wanted = PointedClass(Property); Object != nullptr && Wanted != nullptr) {
		/* An interface is implemented, not derived from */
		const bool bTakes = AsInterface != nullptr
			? Object->GetClass()->ImplementsInterface(Wanted)
			: Object->IsA(Wanted);

		if (!bTakes) {
			UE_LOG(LogReflectionPropertySerializer, Warning, TEXT("Skipped '%s': '%s' is a %s, not a %s."),
				*Property->GetName(), *Object->GetPathName(), *Object->GetClass()->GetName(), *Wanted->GetName());

			return;
		}
	}

	if (AsInterface != nullptr) {
		FScriptInterface* Holds = static_cast<FScriptInterface*>(OutValue);

		Holds->SetObject(Object);
		Holds->SetInterface(Object != nullptr && AsInterface->InterfaceClass != nullptr
			? Object->GetClass()->GetInterfaceAddress(AsInterface->InterfaceClass)
			: nullptr);

		return;
	}

	if (const FObjectPropertyBase* AsObject = CastField<const FObjectPropertyBase>(Property)) {
		AsObject->SetObjectPropertyValue(OutValue, Object);
	}
}

void UPropertySerializer::RemapConvertOutput(const UStruct* Struct, void* StructValue) const {
	/* Matched by shape rather than by type: an expression connects through FExpressionInput, the
	 * material's own inputs through FColorMaterialInput and friends, and which of those are
	 * separate reflected structs (and whether they share a reflected base) changes by version.
	 * Everything that carries a connection has both of these. */
	const FObjectProperty* ExpressionProperty = FindFProperty<FObjectProperty>(Struct, TEXT("Expression"));
	const FIntProperty* OutputIndexProperty = FindFProperty<FIntProperty>(Struct, TEXT("OutputIndex"));

	if (ExpressionProperty == nullptr || OutputIndexProperty == nullptr) {
		return;
	}

	void* ExpressionValue = ExpressionProperty->ContainerPtrToValuePtr<void>(StructValue);

	const TArray<UObject*>* Roots = ConvertOutputRoots.Find(ExpressionProperty->GetObjectPropertyValue(ExpressionValue));
	if (Roots == nullptr) {
		return;
	}

	int32* OutputIndexValue = OutputIndexProperty->ContainerPtrToValuePtr<int32>(StructValue);
	if (!Roots->IsValidIndex(*OutputIndexValue)) {
		return;
	}

	/* Each root has the one output, so the index the convert node was asked for is spent here */
	ExpressionProperty->SetObjectPropertyValue(ExpressionValue, (*Roots)[*OutputIndexValue]);
	*OutputIndexValue = 0;
}

#if UE5_2_BEYOND
UE_DISABLE_OPTIMIZATION
#else
PRAGMA_DISABLE_OPTIMIZATION
#endif

UPropertySerializer::UPropertySerializer() {
	FallbackStructSerializer = MakeShared<FFallbackStructSerializer>(this);

	UScriptStruct* DateTimeStruct = FindObject<UScriptStruct>(nullptr, TEXT("/Script/CoreUObject.DateTime"));
	UScriptStruct* TimespanStruct = FindObject<UScriptStruct>(nullptr, TEXT("/Script/CoreUObject.TimeSpan"));
	check(DateTimeStruct);
	check(TimespanStruct);

	StructSerializers.Add(DateTimeStruct, MakeShared<FDateTimeSerializer>());
	StructSerializers.Add(TimespanStruct, MakeShared<FTimeSpanSerializer>());

	/* Put down against the base every kind of Niagara variable is built on, so the one written for
	 * it stands for all of them. Asked for rather than checked, since a build without Niagara has
	 * no variables to read and nothing to register. */
	if (UScriptStruct* NiagaraVariableStruct = FindObject<UScriptStruct>(nullptr, TEXT("/Script/Niagara.NiagaraVariableBase"))) {
		StructSerializers.Add(NiagaraVariableStruct, MakeShared<FNiagaraVariableSerializer>(this));
	}

	/* Put down against the connectivity cube on building edit mode patterns, whose faces are bit arrays with
	 * no reflection data to write through. Asked for rather than checked, since a build without the game
	 * module has no cubes to read. */
	if (UScriptStruct* ConnectivityCubeStruct = FindObject<UScriptStruct>(nullptr, TEXT("/Script/FortniteGame.ConnectivityCube"))) {
		StructSerializers.Add(ConnectivityCubeStruct, MakeShared<FConnectivityCubeSerializer>());
	}

	/* Put down against the bit array wrapper character dynamics decision trees keep their masks in, which is a
	 * bit array with no reflection data to write through, the same as a cube face. Asked for rather than checked,
	 * since a build without the module has no decision trees to read. */
	if (UScriptStruct* BitArrayWrapperStruct = FindObject<UScriptStruct>(nullptr, TEXT("/Script/CharacterDynamicsControlCore.BitArrayWrapper"))) {
		StructSerializers.Add(BitArrayWrapperStruct, MakeShared<FBitArrayWrapperSerializer>());
	}
}

void UPropertySerializer::DeserializePropertyValue(FProperty* Property, const TSharedRef<FJsonValue>& JsonValue, void* OutValue, UObject* OptionalOuter) {
	const FMapProperty* MapProperty = CastField<const FMapProperty>(Property);
	const FSetProperty* SetProperty = CastField<const FSetProperty>(Property);
	const FArrayProperty* ArrayProperty = CastField<const FArrayProperty>(Property);

	TSharedRef<FJsonValue> NewJsonValue = JsonValue;
	
	if (BlacklistedPropertyNames.Contains(Property->GetName())) {
		return;
	}
	
	if (MapProperty) {
		if (NewJsonValue->IsNull()) {
			return;
		}
		
		FProperty* KeyProperty = MapProperty->KeyProp;
		FProperty* ValueProperty = MapProperty->ValueProp;
		FScriptMapHelper MapHelper(MapProperty, OutValue);

		/* A map whose values are simple enough to write inline comes through as a json object keyed
		 * by the map key ({"Mobile": 1}), and one whose values are not comes through as an array of
		 * Key/Value pairs. Both are the same map, and reading only the array form leaves the other
		 * kind empty. */
		if (NewJsonValue->Type == EJson::Object) {
			for (const auto& Pair : NewJsonValue->AsObject()->Values) {
				const int32 Index = MapHelper.AddDefaultValue_Invalid_NeedsRehash();
				uint8* PairPtr = MapHelper.GetPairPtr(Index);

				/* The key is the field name, which json only ever has as a string */
				const TSharedRef<FJsonValue> EntryKey = MakeShared<FJsonValueString>(JsonKeyToString(Pair.Key));

				DeserializePropertyValue(KeyProperty, EntryKey, PairPtr);
				DeserializePropertyValue(ValueProperty, Pair.Value.ToSharedRef(), PairPtr + MapHelper.MapLayout.ValueOffset, OptionalOuter);
			}

			MapHelper.Rehash();

			return;
		}

		const TArray<TSharedPtr<FJsonValue>>& PairArray = NewJsonValue->AsArray();

		for (int32 i = 0; i < PairArray.Num(); i++) {
			const TSharedPtr<FJsonObject>& Pair = PairArray[i]->AsObject();
			const TSharedPtr<FJsonValue>& EntryKey = Pair->Values.FindChecked(TEXT("Key"));
			const TSharedPtr<FJsonValue>& EntryValue = Pair->Values.FindChecked(TEXT("Value"));
			const int32 Index = MapHelper.AddDefaultValue_Invalid_NeedsRehash();
			uint8* PairPtr = MapHelper.GetPairPtr(Index);

			/* Copy over imported key and value from temporary storage */
			DeserializePropertyValue(KeyProperty, EntryKey.ToSharedRef(), PairPtr);
			DeserializePropertyValue(ValueProperty, EntryValue.ToSharedRef(), PairPtr + MapHelper.MapLayout.ValueOffset);
		}
		MapHelper.Rehash();

	} else if (SetProperty) {
		FProperty* ElementProperty = SetProperty->ElementProp;
		FScriptSetHelper SetHelper(SetProperty, OutValue);
		const TArray<TSharedPtr<FJsonValue>>& SetArray = NewJsonValue->AsArray();
		SetHelper.EmptyElements();
		uint8* TempElementStorage = static_cast<uint8*>(FMemory::Malloc(GetElementSize(ElementProperty)));
		ElementProperty->InitializeValue(TempElementStorage);

		for (int32 i = 0; i < SetArray.Num(); i++) {
			const TSharedPtr<FJsonValue>& Element = SetArray[i];
			DeserializePropertyValue(ElementProperty, Element.ToSharedRef(), TempElementStorage);

			const int32 NewElementIndex = SetHelper.AddDefaultValue_Invalid_NeedsRehash();
			uint8* NewElementPtr = SetHelper.GetElementPtr(NewElementIndex);

			/* Copy over imported key from temporary storage */
			ElementProperty->CopyCompleteValue_InContainer(NewElementPtr, TempElementStorage);
		}
		SetHelper.Rehash();

		ElementProperty->DestroyValue(TempElementStorage);
		FMemory::Free(TempElementStorage);
	} else if (ArrayProperty) {
		FProperty* ElementProperty = ArrayProperty->Inner;
		FScriptArrayHelper ArrayHelper(ArrayProperty, OutValue);

		/* Raw bytes come across written out as base64, which is the only way an array ever arrives
		 * as a string. Read as a list of numbers it comes out empty and the property is left
		 * holding nothing, which is a value silently gone rather than a value read wrongly. */
		if (TArray<uint8> Bytes; NewJsonValue->Type == EJson::String && ElementProperty->IsA<FByteProperty>()
			&& FBase64::Decode(NewJsonValue->AsString(), Bytes)) {
			ArrayHelper.EmptyAndAddUninitializedValues(Bytes.Num());

			if (Bytes.Num() > 0) {
				FMemory::Memcpy(ArrayHelper.GetRawPtr(0), Bytes.GetData(), Bytes.Num());
			}
		} else {
			const TArray<TSharedPtr<FJsonValue>>& SetArray = NewJsonValue->AsArray();

			/* Read each element beside the array rather than into it.
			 *
			 * An element naming an object has that object built while it is being read, and
			 * building one comes back through here for whatever it is made of. Where that leads
			 * round to the same array again, which it does for an asset that keeps a copy of
			 * itself, the array is emptied and filled from the start while this is partway
			 * through writing into it, and the place being written to is no longer there.
			 *
			 * So nothing of the array's is held across a read. The elements are made here, filled
			 * here, and the array is only touched once they are all finished, which also makes
			 * this the one that has the last word on what it holds. */
			uint8* TempElementStorage = SetArray.Num() > 0
				? static_cast<uint8*>(FMemory::Malloc(static_cast<SIZE_T>(GetElementSize(ElementProperty)) * SetArray.Num(),
					ElementProperty->GetMinAlignment()))
				: nullptr;

			for (int32 i = 0; i < SetArray.Num(); i++) {
				uint8* ElementPtr = TempElementStorage + static_cast<SIZE_T>(GetElementSize(ElementProperty)) * i;

				ElementProperty->InitializeValue(ElementPtr);
				DeserializePropertyValue(ElementProperty, SetArray[i].ToSharedRef(), ElementPtr, OptionalOuter);
			}

			ArrayHelper.EmptyValues();

			for (int32 i = 0; i < SetArray.Num(); i++) {
				uint8* ElementPtr = TempElementStorage + static_cast<SIZE_T>(GetElementSize(ElementProperty)) * i;

				ElementProperty->CopyCompleteValue(ArrayHelper.GetRawPtr(ArrayHelper.AddValue()), ElementPtr);
				ElementProperty->DestroyValue(ElementPtr);
			}

			if (TempElementStorage != nullptr) FMemory::Free(TempElementStorage);
		}
	}
	else if (Property->IsA<FMulticastDelegateProperty>()) {
	} else if (Property->IsA<FDelegateProperty>()) {
	}
	else if (FSoftObjectProperty* SoftObjectProperty = CastField<FSoftObjectProperty>(Property)) {
		TSharedPtr<FJsonObject> SoftJsonObjectProperty;
		FString PathString = "";

		/* What the reference calls itself, which the property cannot be asked for */
		FString Kind;
		
		switch (NewJsonValue->Type) {
			/* UEParse, extract it from the object */
			case EJson::Object:
				SoftJsonObjectProperty = NewJsonValue->AsObject();

				if (!SoftJsonObjectProperty->TryGetStringField(TEXT("AssetPathName"), PathString)) {
					/* Named the way every other reference is named, rather than written as a path. */
					FString Named;
					FString Where;

					SoftJsonObjectProperty->TryGetStringField(TEXT("ObjectName"), Named);
					SoftJsonObjectProperty->TryGetStringField(TEXT("ObjectPath"), Where);

					if (Named.Split(TEXT("'"), &Kind, &Named)) {
						Named.Split(TEXT("'"), &Named, nullptr, ESearchCase::CaseSensitive, ESearchDir::FromEnd);
					}

					/* Inside is inside, however it is spelled */
					Named = Named.Replace(TEXT(":"), TEXT("."));

					/* The package, which is the path as far as the export it names */
					Where.Split(TEXT("."), &Where, nullptr);

					if (!Named.IsEmpty() && !Where.IsEmpty()) {
						PathString = ToEditorPackagePath(Where) + TEXT(".") + Named;
					}
				}
			break;

			/* Older game builds */
			default:
				PathString = NewJsonValue->AsString();
			break;
		}

		if (PathString != "") {
			const FSoftObjectPath Reference(PathString);

			FSoftObjectPtr* ObjectPtr = static_cast<FSoftObjectPtr*>(OutValue);
			*ObjectPtr = Reference;

			if (!AlreadyHere(Reference)) {
				/* Try importing it using Cloud */
				FString PackagePath;
				FString AssetName;
				PathString.Split(".", &PackagePath, &AssetName);
				TObjectPtr<UObject> T = nullptr;

				/* Asked for as the thing it is, rather than as whatever the property will hold. */
				FString PropertyClassName = Kind.IsEmpty() ? SoftObjectProperty->PropertyClass->GetName() : Kind;
				
				IImporter::DownloadWrapper(T, PropertyClassName, AssetName, PackagePath);
			}
		}
	}
	else if (const FProperty* ObjectProperty = PointsAtObject(Property)) {
		/* Need to serialize full UObject for object property */
		TObjectPtr<UObject> Object = nullptr;

		if (NewJsonValue->IsNull()) {
			SetObjectPropertyValueChecked(ObjectProperty, OutValue, nullptr);
		}

		/* Named the short way, as the one string rather than as a reference spelled out. */
		if (NewJsonValue->Type == EJson::String) {
			FString Named = NewJsonValue->AsString();

			/* What the reference calls itself, which the property cannot be asked for */
			FString Kind;

			if (Named.Split(TEXT("'"), &Kind, &Named)) {
				Named.Split(TEXT("'"), &Named, nullptr, ESearchCase::CaseSensitive, ESearchDir::FromEnd);
			}

			if (Named.IsEmpty() || Named == TEXT("None")) return;

			const FString Where = ToEditorPackagePath(Named);

			TObjectPtr<UObject> Points = LoadObjectByPath<UObject>(Where);

			/* Something this package holds, which the loader cannot read yet and the Cloud answers with the same file */
			if (Points == nullptr && ExportsContainer != nullptr && Named.Contains(TEXT(":"))) {
				FString Leaf;

				if (!Named.Split(TEXT(":"), nullptr, &Leaf, ESearchCase::CaseSensitive, ESearchDir::FromEnd)) {
					Leaf = Named;
				}

				/* Only the last segment names the export, whatever it is kept under */
				if (FString Deeper; Leaf.Split(TEXT("."), nullptr, &Deeper, ESearchCase::CaseSensitive, ESearchDir::FromEnd)) {
					Leaf = Deeper;
				}

				if (FUObjectExport* Held = ExportsContainer->Find(Leaf); Held != nullptr && Held->IsJsonValid()) {
					if (Held->Object == nullptr && ObjectSerializer != nullptr) {
						ObjectSerializer->SpawnExport(Held);
					}

					Points = Held->Object;
				}
			}

			/* Only the game has it, and nothing under Script is ever fetched */
			if (Points == nullptr && !Named.StartsWith(TEXT("/Script/"))) {
				FString PackagePath;
				FString AssetName;

				if (Named.Split(TEXT("."), &PackagePath, &AssetName)) {
					IImporter::DownloadWrapper(Points, Kind.IsEmpty() ? GetNameSafe(PointedClass(ObjectProperty)) : Kind, AssetName, PackagePath);
				}

				if (Points == nullptr) Points = LoadObjectByPath<UObject>(Where);
			}

			if (Points == nullptr) {
				UE_LOG(LogReflectionPropertySerializer, Warning, TEXT("\"%s\" names %s, and nothing of that came back"),
					*GetPropertyName(Property).ToString(), *Named);

				return;
			}

			SetObjectPropertyValueChecked(ObjectProperty, OutValue, Points);

			return;
		}

		if (NewJsonValue->Type == EJson::Object) {
			auto JsonValueAsObject = NewJsonValue->AsObject();

			/* What is being filled in, which importing a blueprint recompiles and throws away underneath this */
			UObject* const Into = OptionalOuter != nullptr
				? OptionalOuter
				: (ObjectSerializer != nullptr ? ObjectSerializer->Parent : nullptr);

			const TWeakObjectPtr<UObject> Filling(Into);

			/* Something of this package's own, asked for by the whole way down to it.
			 *
			 * A reference into the package being read names an export of it, and that export
			 * already has its object. Left to the loader it is asked for as Package.Leaf, which
			 * is not where a subobject lives: it answers only where nothing else in the package
			 * shares the name, and plenty does. An emitter holding a copy of what it was made
			 * from has a script source under each and a graph under both, and an assignment
			 * node's own script has a third of each again. So everything nested that deeply came
			 * back empty, and the properties they were for were left holding nothing. */
			FString Reference;

			if (ExportsContainer != nullptr && JsonValueAsObject->TryGetStringField(TEXT("ObjectName"), Reference)) {
				FUObjectExport* Held = ExportsContainer->FindByFullObjectName(Reference);

				/* Built now where it has not been yet.
				 *
				 * The exports are gone through in the order the package lists them, and a
				 * reference can name one that has not been reached. Left for later it falls to
				 * being looked for by name, and a name is not one thing: the first export called
				 * NiagaraGraph_0 answers for all of them, so a source ends up naming a graph
				 * belonging to another. The export is here and says what it is, so it is made. */
				if (Held->IsJsonValid() && Held->Object == nullptr && ObjectSerializer != nullptr) {
					ObjectSerializer->SpawnExport(Held);
				}

				if (Held->IsJsonValid() && Held->Object != nullptr) {
					Object = Held->Object;
				}
			}

			if (Importer == nullptr) {
				Importer = new IImporter();
			}

			Importer->SetParent(ObjectSerializer->Parent);

			if (!Object) Importer->LoadExport(&JsonValueAsObject, Object);

			/* Taken apart while that ran, so the rest of this is written nowhere */
			if (Into != nullptr && !Filling.IsValid()) {
				UE_LOG(LogReflectionPropertySerializer, Warning, TEXT("\"%s\" was being read into something an import took apart, and was left"),
					*GetPropertyName(Property).ToString());

				return;
			}

			/* Whatever it holds may point at something taken apart since, which answers a null check and then crashes */
			UObject* CurrentObject = PointedObject(ObjectProperty, OutValue);

			if (CurrentObject != nullptr && (!CurrentObject->IsValidLowLevelFast() || !IsValid(CurrentObject))) {
				CurrentObject = nullptr;
			}

			if (CurrentObject != nullptr) {
				if (!Object && CurrentObject->IsA(UActorComponent::StaticClass())) {
					Object = CurrentObject;
				}
			}

			if (!Object && ObjectSerializer->bUseExperimentalSpawning) {
				if (FUObjectExport* TargetExport = ExportsContainer->GetExportByObjectPath(JsonValueAsObject)) {
					FUObjectJsonValueExport Properties = TargetExport->GetObject(TEXT("Properties"));

					if (TargetExport->Has(TEXT("LODData"))) {
						Properties.SetArray(TEXT("LODData"), TargetExport->GetArray(TEXT("LODData")));
					}
					
					ObjectSerializer->SpawnExport(TargetExport);

					if (TargetExport->Object) {
						Object = TargetExport->Object;
					}
				}
			}


			if (Object != nullptr) {
				const bool bIsActorComponent = Object.Get()->IsA(UActorComponent::StaticClass());

				/* Pointed at, whatever it is. */
				if (!bIsActorComponent || GetPropertyName(ObjectProperty) == "AttachParent" || GetPropertyName(ObjectProperty) == "ComponentTemplate") {
					SetObjectPropertyValueChecked(ObjectProperty, OutValue, Object);
				}

				/* And filled in, but not while the first is still running: two components naming each other never stop */
				if (bIsActorComponent && GetPropertyName(ObjectProperty) != "AttachParent" && !GBeingFilled.Contains(Object)) {
					const FFillScope BeingFilled(Object);

					if (FUObjectExport* TargetExport = ExportsContainer->GetExportByObjectPath(JsonValueAsObject)) {
						FUObjectJsonValueExport Properties = TargetExport->GetObject(TEXT("Properties"));

						if (TargetExport->Has(TEXT("LODData"))) {
							Properties.SetArray(TEXT("LODData"), TargetExport->GetArray(TEXT("LODData")));
						}

						ObjectSerializer->DeserializeObjectProperties(Properties.JsonObject, Object);
					}
				}

				if (bIsActorComponent) {
					if (UStaticMeshComponent* StaticMeshComponent = Cast<UStaticMeshComponent>(Object.Get())) {
						StaticMeshComponent->PostEditImport();
					}
				}
			}

			/* Everything below looks for the object by name, and a name is not one thing.
			 *
			 * A package holds one NiagaraGraph_0 under every script source it has, and one
			 * NiagaraScriptSource_0 under every emitter and every assignment node's own script. Asked
			 * for by the last part of the name, or by that and its outer's last part, the first of them
			 * answers for all of them.
			 *
			 * That is a last resort for a reference nothing else could place, and it was running on
			 * every reference: one placed correctly by the whole way down to it was then looked up
			 * again by name and quietly replaced with a namesake. */
			if (Object != nullptr) return;

			FString ObjectName = JsonValueAsObject->GetStringField(TEXT("ObjectName"));
			FString ObjectPath = JsonValueAsObject->GetStringField(TEXT("ObjectPath"));
			FString ObjectOuter;
			int ObjectIndex = -1;

			if (ObjectName.Contains(".")) {
				ObjectName.Split(".", &ObjectOuter, &ObjectName);
				ObjectName.Split("'", &ObjectName, nullptr);
			}

			if (ObjectName.Contains(":")) {
				ObjectName.Split(":", nullptr, &ObjectName);
				ObjectName.Split("'", &ObjectName, nullptr);
			}

			if (ObjectPath.Contains(".")) {
				FString ObjectIndexString;
				ObjectPath.Split(".", nullptr, &ObjectIndexString);

				ObjectIndex = FCString::Atoi(*ObjectIndexString);
			}

			if (ExportsContainer) {
				if (FUObjectExport* Export = ExportsContainer->Find(ObjectName); Export->IsJsonValid() && Export->Object != nullptr) {
					if (UObject* FoundObject = Export->Object) {
						SetObjectPropertyValueChecked(ObjectProperty, OutValue, FoundObject);
					}
				}
			}

			if (ObjectName.Contains(".")) {
				TArray<FString> Parts;
				ObjectName.ParseIntoArray(Parts, TEXT("."), true);

				FString Penultimate = Parts.Num() > 1 ? Parts[Parts.Num() - 2] : TEXT("");
				FString LastSegment = Parts.Num() > 0 ? Parts.Last() : TEXT("");

				ObjectName = LastSegment;
				ObjectOuter = Penultimate;
			}

			if (!ObjectOuter.IsEmpty()) {
				if (ObjectOuter.Contains(":")) {
					ObjectOuter.Split(":", nullptr, &ObjectOuter);
				}
				
				if (FUObjectExport* Export = ExportsContainer->Find(ObjectName, ObjectOuter); Export->IsJsonValid() && Export->Object != nullptr) {
					if (UObject* FoundObject = Export->Object) {
						SetObjectPropertyValueChecked(ObjectProperty, OutValue, FoundObject);
					}
				}
			}

			if (FallbackToParentTrace) {
				if (UObject* Parent = ObjectSerializer->Parent) {
					FString Name = Parent->GetName();

					if (FUObjectExport* Export = ExportsContainer->Find(ObjectName, Name); Export->IsJsonValid() && Export->Object != nullptr) {
						if (UObject* FoundObject = Export->Object) {
							SetObjectPropertyValueChecked(ObjectProperty, OutValue, FoundObject);
						}
					}
				}
			}

			if (ObjectIndex != -1 && ExportsContainer) {
				if (FUObjectExport* Export = ExportsContainer->FindByPositionAndName(ObjectIndex, ObjectName); Export->IsJsonValid() && Export->Object != nullptr) {
					if (UObject* FoundObject = Export->Object) {
						SetObjectPropertyValueChecked(ObjectProperty, OutValue, FoundObject);
					}
				}
			}

			/* Named something and came back with nothing. */
			if (PointedObject(ObjectProperty, OutValue) == nullptr) {
				FString Named;

				JsonValueAsObject->TryGetStringField(TEXT("ObjectName"), Named);

				UE_LOG(LogReflectionPropertySerializer, Warning, TEXT("\"%s\" names %s, and nothing of that came back"),
					*GetPropertyName(Property).ToString(), Named.IsEmpty() ? TEXT("something it does not name") : *Named);
			}

			/* Too extreme it seems */
#if 0
			FUObjectExport NewExport(JsonValueAsObject);

			const TArray<FName> TreeSegments = NewExport.GetOuterTreeSegments();

			if (TreeSegments.Num() > 0) {
				if (FUObjectExport& FoundExport = ExportsContainer->FindByTreeSegment(TreeSegments); FoundExport.Object != nullptr) {
					SetObjectPropertyValueChecked(ObjectProperty, OutValue, FoundExport.Object);
				}
			}
#endif
		}
	}
	else if (const FStructProperty* StructProperty = CastField<const FStructProperty>(Property)) {
		/* A guid is written out as the one string its whole value fits in rather than as the four
		 * ints behind it, so the struct walk below would read it as an object and leave it zeroed.
		 * FGuid::Parse takes every format the engine prints, including the hyphenated quads that
		 * asset guids come through as. */
		if (StructProperty->Struct == TBaseStructure<FGuid>::Get() && NewJsonValue->Type == EJson::String) {
			FGuid::Parse(NewJsonValue->AsString(), *static_cast<FGuid*>(OutValue));

			return;
		}

		if (StructProperty->Struct == FGameplayTag::StaticStruct()) {
			FGameplayTag* GameplayTagStr = static_cast<FGameplayTag*>(OutValue);
			FGameplayTag NewTag = RequestOrMakeGameplayTag(NewJsonValue->AsObject()->GetStringField(TEXT("TagName")));
			*GameplayTagStr = NewTag;
			return;
		}

		/* FGameplayTagContainer (handled from UEParse data) */
		if (StructProperty->Struct == FGameplayTagContainer::StaticStruct()) {
			FGameplayTagContainer* GameplayTagContainerStr = static_cast<FGameplayTagContainer*>(OutValue);

			auto GameplayTags = JsonValue->AsArray();

			for (TSharedPtr GameplayTagValue : GameplayTags) {
				FString GameplayTagString = GameplayTagValue->AsString();
				const FGameplayTag GameplayTag = RequestOrMakeGameplayTag(GameplayTagString);

				if (GameplayTag.IsValid()) GameplayTagContainerStr->AddTag(GameplayTag);
			}

			return;
		}

		if (StructProperty->Struct == FMovieSceneFrameRange::StaticStruct()) {
			FMovieSceneFrameRange* MovieSceneFrameRange = static_cast<FMovieSceneFrameRange*>(OutValue);
			TSharedPtr<FJsonObject> JsonObject = NewJsonValue->AsObject()->GetObjectField(TEXT("Value"));

			FMovieSceneFrameRange Range;

			/* One end of it, of which there are three kinds and not two.
			 *
			 * Open is the third, and it is how a section that covers everything says so: a widget
			 * animation's sections are written open at both ends. Read as neither of the other two
			 * the bound was left as it was made, which is exclusive at nothing, and a range from
			 * nothing to nothing is empty. An empty section is one the editor draws nowhere, so a
			 * track came up blank over a full set of keys sitting in it.
			 *
			 * Said as a number where the export writes the byte and as a name where it writes the
			 * enum, since both spellings turn up in the same asset. */
			const auto ReadBound = [](const TSharedPtr<FJsonObject>& Bound) {
				int32 Kind = 0;

				if (const TSharedPtr<FJsonValue> Said = Bound->TryGetField(TEXT("Type")); Said.IsValid()) {
					if (Said->Type == EJson::String) {
						FString Spelled = Said->AsString();

						Spelled.Split(TEXT("::"), nullptr, &Spelled);

						Kind = Spelled == TEXT("Inclusive") ? 1 : Spelled == TEXT("Open") ? 2 : 0;
					} else {
						Kind = static_cast<int32>(Said->AsNumber());
					}
				}

				if (Kind == 2) return TRangeBound<FFrameNumber>::Open();

				FFrameNumber BoundFrame;

				if (const TSharedPtr<FJsonObject>* Held = nullptr; Bound->TryGetObjectField(TEXT("Value"), Held)) {
					(*Held)->TryGetNumberField(TEXT("Value"), BoundFrame.Value);
				}

				return Kind == 1 ? TRangeBound<FFrameNumber>::Inclusive(BoundFrame) : TRangeBound<FFrameNumber>::Exclusive(BoundFrame);
			};

			if (const TSharedPtr<FJsonObject>* Bound = nullptr; JsonObject->TryGetObjectField(TEXT("LowerBound"), Bound)) {
				Range.Value.SetLowerBound(ReadBound(*Bound));
			}

			if (const TSharedPtr<FJsonObject>* Bound = nullptr; JsonObject->TryGetObjectField(TEXT("UpperBound"), Bound)) {
				Range.Value.SetUpperBound(ReadBound(*Bound));
			}

			*MovieSceneFrameRange = Range;

			return;
		}

		if (StructProperty->Struct->GetFName() == "SoftObjectPath") {
			TSharedPtr<FJsonObject> SoftJsonObjectProperty;
			FString PathString = "";

			SoftJsonObjectProperty = NewJsonValue->AsObject();
			PathString = SoftJsonObjectProperty->GetStringField(TEXT("AssetPathName"));
			
			if (PathString != "") {
				const FSoftObjectPath Reference(PathString);

				FSoftObjectPtr* ObjectPtr = static_cast<FSoftObjectPtr*>(OutValue);
				*ObjectPtr = Reference;

				if (!AlreadyHere(Reference)) {
					/* Try importing it using Cloud */
					FString PackagePath;
					FString AssetName;
					PathString.Split(".", &PackagePath, &AssetName);
					TObjectPtr<UObject> T = nullptr;

					FString PropertyClassName = "DataAsset";
				
					IImporter::DownloadWrapper(T, PropertyClassName, AssetName, PackagePath);
				}
			}
		}
		
		/* JSON for FGuids are FStrings */
		FString OutString;
		
		if (JsonValue->TryGetString(OutString)) {
			FGuid GUID = StringToGuid(OutString); /* Create GUID from String */

			TSharedRef<FJsonObject> SharedObject = MakeShareable(new FJsonObject());
			SharedObject->SetNumberField(TEXT("A"), GUID.A); SharedObject->SetNumberField(TEXT("B"), GUID.B);
			SharedObject->SetNumberField(TEXT("C"), GUID.C); SharedObject->SetNumberField(TEXT("D"), GUID.D);

			const TSharedRef<FJsonValue> NewValue = MakeShareable(new FJsonValueObject(SharedObject));
			NewJsonValue = NewValue;
		}

		if (StructProperty->Struct == FFontData::StaticStruct()) {
			FFontData* FontData = static_cast<FFontData*>(OutValue);
			TSharedPtr<FJsonObject> JsonObject = NewJsonValue->AsObject();
			
			if (JsonObject->HasField(TEXT("LocalFontFaceAsset"))) {
				TSharedPtr<FJsonObject> LocalFontFaceExport = JsonObject->GetObjectField(TEXT("LocalFontFaceAsset"));

				if (Importer == nullptr) {
					Importer = new IImporter();
				}

				TObjectPtr<UFontFace> FontFacePtr;
				
				Importer->SetParent(ObjectSerializer->Parent);
				Importer->LoadExport(&LocalFontFaceExport, FontFacePtr);

				if (UFontFace* FontFace = FontFacePtr.Get()) {
					*FontData = FFontData(FontFace, 0);
				}
			}
		}

		/* To serialize struct, we need its type and value pointer, because struct value doesn't contain type information */
		DeserializeStruct(StructProperty->Struct, NewJsonValue->AsObject().ToSharedRef(), OutValue, OptionalOuter);

		/* An input that named one of a convert node's outputs has to follow that output onto the
		 * root the material graph built for it. Every connection in the graph passes through here,
		 * which is what catches the material's own BaseColor/Emissive inputs as well as the ones
		 * sitting on expressions. */
		if (ConvertOutputRoots.Num() > 0) {
			RemapConvertOutput(StructProperty->Struct, OutValue);
		}

		/* A material attribute is recorded by its position in a list this engine may not lay out
		 * the same way, so the index is put back onto the attribute it was written against */
		if (Property->GetCPPType(nullptr, CPPF_None) == TEXT("FExpressionInput")) {
			RemapBreakMaterialAttributesOutput(static_cast<FExpressionInput*>(OutValue));
		}
		
		if (IsStructPropertyADistribution(StructProperty)) {
			if (FRawDistribution* RawDistribution = static_cast<FRawDistribution*>(OutValue)) {
				const bool IsFloat = IsFloatDistribution(StructProperty);

				const TSharedPtr<FJsonObject> Written = NewJsonValue->Type == EJson::Object ? NewJsonValue->AsObject() : nullptr;

				const TSharedPtr<FJsonValue> Named = Written.IsValid() ? Written->TryGetField(TEXT("Distribution")) : nullptr;

				const bool bPackageNamedOne = Named.IsValid() && !Named->IsNull();

				/* Cleared first where the package named none */
				if (!bPackageNamedOne) {
					SetDistribution(RawDistribution, nullptr, IsFloat);
				}

				if (!GetDistribution(RawDistribution, IsFloat)) {
					if (UDistribution* NewDistribution = DecookDistribution(OptionalOuter, *RawDistribution, IsFloat)) {
						SetDistribution(RawDistribution, NewDistribution, IsFloat);
					}
				}
			}
		}
	}
	else if (const FByteProperty* ByteProperty = CastField<const FByteProperty>(Property)) {
		/* If we have a string provided, make sure Enum is not null */
		if (JsonValue->Type == EJson::String) {
			FString EnumAsString = JsonValue->AsString();

			if (!ByteProperty->Enum)
			{
				return;
			}
			
			int64 EnumerationValue = ByteProperty->Enum->GetValueByNameString(EnumAsString);

			/* Somethings wrong!!! */
			if (EnumerationValue == -1) {
				UE_LOG(LogReflection, Warning, TEXT("Invalid enum value for property '%s'!"), *Property->GetName());

				UE_LOG(LogReflection, Warning, TEXT("Enum name: %s"), *EnumAsString);
				UE_LOG(LogReflection, Warning, TEXT("Enum type: %s"), *ByteProperty->Enum->GetName());
				UE_LOG(LogReflection, Warning, TEXT("Available values:"));

				for (int32 Index = 0; Index < ByteProperty->Enum->NumEnums() - 1; ++Index)
				{
					FString Name = ByteProperty->Enum->GetNameStringByIndex(Index);
					int64 Value = ByteProperty->Enum->GetValueByIndex(Index);
					UE_LOG(LogReflection, Warning, TEXT("  [%d] %s = %lld"), Index, *Name, Value);
				}

				EnumerationValue = 0;
			}
			
			ByteProperty->SetIntPropertyValue(OutValue, EnumerationValue);
		}
		else {
			/* Should be a number, set property value accordingly */
			const int64 NumberValue = static_cast<int64>(NewJsonValue->AsNumber());

			/* A number the enum has no name for, which is what old data leaves lying about. */
			if (ByteProperty->Enum != nullptr && !ByteProperty->Enum->IsValidEnumValue(NumberValue)) {
				UE_LOG(LogReflectionPropertySerializer, Warning, TEXT("\"%s\" is a %s, which has no %lld, so it was left as it was built"), *GetPropertyName(ByteProperty).ToString(), *ByteProperty->Enum->GetName(), NumberValue);

				return;
			}

			ByteProperty->SetIntPropertyValue(OutValue, NumberValue);
		}
		/* Primitives below, they are serialized as plain json values */
	}
	else if (const FNumericProperty* NumberProperty = CastField<const FNumericProperty>(Property)) {
		const double NumberValue = NewJsonValue->AsNumber();
		if (NumberProperty->IsFloatingPoint()) {
			NumberProperty->SetFloatingPointPropertyValue(OutValue, NumberValue);
		}
		
		else {
			NumberProperty->SetIntPropertyValue(OutValue, static_cast<int64>(NumberValue));
		}
	}
	else if (const FBoolProperty* BoolProperty = CastField<const FBoolProperty>(Property)) {
		const bool BooleanValue = NewJsonValue->AsBool();
		BoolProperty->SetPropertyValue(OutValue, BooleanValue);
	}
	else if (Property->IsA<FStrProperty>()) {
		const FString StringValue = NewJsonValue->AsString();
		*static_cast<FString*>(OutValue) = StringValue;
	}
	else if (const FEnumProperty* EnumProperty = CastField<const FEnumProperty>(Property)) {
		/* Tagged properties name the value they hold, and anything an exporter reads by hand has
		 * only the number it was stored as to give. Read as a name, a number matches nothing and
		 * the property keeps whatever it was constructed with. */
		if (NewJsonValue->Type == EJson::Number) {
			const int64 NumberValue = static_cast<int64>(NewJsonValue->AsNumber());

			/* And the same for one written as its own type rather than as a byte */
			if (EnumProperty->GetEnum() != nullptr && !EnumProperty->GetEnum()->IsValidEnumValue(NumberValue)) {
				UE_LOG(LogReflectionPropertySerializer, Warning, TEXT("\"%s\" is a %s, which has no %lld, so it was left as it was built"), *GetPropertyName(EnumProperty).ToString(), *EnumProperty->GetEnum()->GetName(), NumberValue);

				return;
			}

			EnumProperty->GetUnderlyingProperty()->SetIntPropertyValue(OutValue, NumberValue);

			return;
		}

		FString EnumAsString = NewJsonValue->AsString();

		if (EnumAsString.Contains("::")) {
			EnumAsString.Split("::", nullptr, &EnumAsString);
		}
		
		/* Prefer readable enum names in result json to raw numbers */
		int64 EnumerationValue = EnumProperty->GetEnum()->GetValueByNameString(EnumAsString);

		if (EnumerationValue != INDEX_NONE) {
			EnumProperty->GetUnderlyingProperty()->SetIntPropertyValue(OutValue, EnumerationValue);
		}
	}
	else if (Property->IsA<FNameProperty>()) {
		/* Name is perfectly representable as string */
		const FString NameString = NewJsonValue->AsString();
		*static_cast<FName*>(OutValue) = *NameString;
	}
	else if (const FTextProperty* TextProperty = CastField<const FTextProperty>(Property)) {
		const FString SerializedValue = NewJsonValue->AsString();
		
		if (!SerializedValue.IsEmpty()) {
			FTextStringHelper::ReadFromBuffer(*SerializedValue, *static_cast<FText*>(OutValue));
		} else {
			/* TODO: Somehow add other needed things like Namespace, Key, and LocalizedString */
			TSharedPtr<FJsonObject> Object = NewJsonValue->AsObject().ToSharedRef();

			/* Retrieve properties */
			FString TextNamespace = Object->GetStringField(TEXT("Namespace"));
			FString UniqueKey = Object->GetStringField(TEXT("Key"));
			FString SourceString = Object->GetStringField(TEXT("SourceString"));
			FString CultureInvariantString = Object->GetStringField(TEXT("CultureInvariantString"));

			if (Object->HasField(TEXT("CultureInvariantString"))) {
				TextProperty->SetPropertyValue(OutValue, FText::FromString(*Object->GetStringField(TEXT("CultureInvariantString"))));
			} else {
	#if UE5_8_BEYOND
				/* ForUseOnlyByLocMacroAndGraphNodeTextLiterals_CreateText was removed in 5.8 */
				TextProperty->SetPropertyValue(OutValue, FText::FromString(SourceString));
	#else
				TextProperty->SetPropertyValue(OutValue, FInternationalization::ForUseOnlyByLocMacroAndGraphNodeTextLiterals_CreateText(*SourceString, *TextNamespace, *UniqueKey));
	#endif
			}
		}
	}
	/* FFieldPath, and the property type that holds one, came in with FField in 4.25. A property
	 * reference is still a plain UObject reference before that, handled by the object branch. */
#if !UE4_24_BELOW
	else if (CastField<const FFieldPathProperty>(Property)) {
		FFieldPath FieldPath;

		/* A field is pointed at rather than named, which takes what holds it and its own name */
		if (NewJsonValue->Type == EJson::String) {
			FieldPath.Generate(*NewJsonValue->AsString());
		} else if (NewJsonValue->Type == EJson::Object) {
			const TSharedPtr<FJsonObject> Said = NewJsonValue->AsObject();

			FString Name;
			FString Where;

			Said->TryGetStringField(TEXT("ObjectName"), Name);
			Said->TryGetStringField(TEXT("ObjectPath"), Where);

			if (Name.Contains(TEXT("'"))) {
				Name.Split(TEXT("'"), nullptr, &Name);
				Name.Split(TEXT("'"), &Name, nullptr, ESearchCase::CaseSensitive, ESearchDir::FromEnd);
			}

			FString Holder;
			FString Named;

			if (!Name.Split(TEXT(":"), &Holder, &Named)) {
				Named = Name;
			}

			/* The package it is in, which is what the path says where it says anything past it */
			FString Package = Where;

			if (int32 Dot; Where.FindChar(TEXT('.'), Dot)) {
				Package = Where.Left(Dot);
			}

			UStruct* Holds = nullptr;

			if (!Holder.IsEmpty() && !Package.IsEmpty()) {
				Holds = FindObject<UStruct>(nullptr, *(Package + TEXT(".") + Holder));

				/* And where this build has not got it, asked for the way anything else is */
				if (Holds == nullptr) {
					if (Importer == nullptr) {
						Importer = new IImporter();
					}

					Importer->SetParent(ObjectSerializer->Parent);

					TSharedPtr<FJsonObject> Points = MakeShared<FJsonObject>();

					Points->SetStringField(TEXT("ObjectName"), FString::Printf(TEXT("Class'%s'"), *Holder));
					Points->SetStringField(TEXT("ObjectPath"), Where);

					TObjectPtr<UObject> Found = nullptr;

					Importer->LoadExport(&Points, Found);

					Holds = Cast<UStruct>(Found.Get());
				}
			}

			if (FProperty* Points = Holds != nullptr && !Named.IsEmpty() ? FindFProperty<FProperty>(Holds, *Named) : nullptr) {
				FieldPath = FFieldPath(Points);
			} else if (!Named.IsEmpty()) {
				UE_LOG(LogReflectionPropertySerializer, Warning, TEXT("\"%s\" points at \"%s\" of \"%s\", and %s"),
					*GetPropertyName(Property).ToString(), *Named, Holder.IsEmpty() ? TEXT("nothing") : *Holder,
					Holds != nullptr ? TEXT("what holds it has nothing of that name") : TEXT("this build hasn't got what holds it"));
			}
		}

		*static_cast<FFieldPath*>(OutValue) = FieldPath;
	}
#endif
	else {
		UE_LOG(LogReflectionPropertySerializer, Fatal, TEXT("Found unsupported property type when deserializing value: %s"), *Property->GetClass()->GetName());
	}
}

void UPropertySerializer::DisablePropertySerialization(const UStruct* Struct, const FName PropertyName) {
	FProperty* Property = Struct->FindPropertyByName(PropertyName);
	checkf(Property, TEXT("Cannot find Property %s in Struct %s"), *PropertyName.ToString(), *Struct->GetPathName());
	BlacklistedProperties.Add(Property);
}

void UPropertySerializer::AddStructSerializer(UScriptStruct* Struct, const TSharedPtr<FStructSerializer>& Serializer) {
	StructSerializers.Add(Struct, Serializer);
}

bool UPropertySerializer::ShouldDeserializeProperty(FProperty* Property) const {
	/* Skip deprecated properties */
	if (Property->HasAnyPropertyFlags(CPF_Deprecated)) {
		return false;
	}
	
	/* Skip blacklisted properties */
	if (this != nullptr && this && BlacklistedProperties.IsValidIndex(0) && BlacklistedProperties.Contains(Property)) {
		return false;
	}
	
	return true;
}

void UPropertySerializer::DeserializeStruct(UScriptStruct* Struct, const TSharedRef<FJsonObject>& Properties, void* OutValue, UObject* OptionalOuter) const {
	FStructSerializer* StructSerializer = GetStructSerializer(Struct);
	StructSerializer->Deserialize(Struct, OutValue, Properties, OptionalOuter);
}

FStructSerializer* UPropertySerializer::GetStructSerializer(const UScriptStruct* Struct) const {
	check(Struct);

	/* One put down for a struct stands for everything built on it, so a serializer written for a
	 * Niagara variable serves the four kinds of variable that derive from it without each having
	 * to be named. The struct itself is asked first, so anything with one of its own still wins. */
	for (const UStruct* Walk = Struct; Walk != nullptr; Walk = Walk->GetSuperStruct()) {
		const UScriptStruct* AsScriptStruct = Cast<UScriptStruct>(Walk);

		if (AsScriptStruct == nullptr) continue;

		if (TSharedPtr<FStructSerializer> const* StructSerializer = StructSerializers.Find(const_cast<UScriptStruct*>(AsScriptStruct))) {
			if (ensure(StructSerializer->IsValid())) {
				return StructSerializer->Get();
			}
		}
	}

	return FallbackStructSerializer.Get();
}

#if UE5_2_BEYOND
UE_ENABLE_OPTIMIZATION
#else
PRAGMA_ENABLE_OPTIMIZATION
#endif