/* Copyright Reflection Contributors 2024-2026 */

#include "Serializers/PropertySerializer.h"

#include "GameplayTagContainer.h"
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
#include "Serializers/Structs/FallbackStructSerializer.h"
#include "Serializers/Structs/NiagaraVariableSerializer.h"
#include "Serializers/Structs/TimeSpanSerializer.h"

#include "Settings/Runtime.h"

DECLARE_LOG_CATEGORY_CLASS(LogReflectionPropertySerializer, Error, Log);

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

/* An object property takes whatever pointer it is handed without checking the type, so anything
 * that resolves to the wrong class lands in the property and is only discovered when something
 * calls through it, by which point the editor is already gone. */
static void SetObjectPropertyValueChecked(const FObjectPropertyBase* ObjectProperty, void* OutValue, UObject* Object) {
	/* A subobject the class made for itself is part of what the object is, and plenty of the engine
	 * reaches through one without checking: a material function asked for its expressions casts
	 * onto whatever is in EditorOnlyData and takes nothing for an answer. A reference that came
	 * across as nothing is not a reason to take that away, so it is left where it is.
	 *
	 * Anything living under another object rather than under a package is one of these, however it
	 * was made. A material function makes its editor data in PostInitProperties rather than as a
	 * default subobject, and it is no less part of the function for that. */
	if (Object == nullptr) {
		const UObject* Current = ObjectProperty->GetObjectPropertyValue(OutValue);

		if (Current != nullptr && Current->GetOuter() != nullptr && !Current->GetOuter()->IsA<UPackage>()) {
			return;
		}
	}

	if (Object != nullptr && ObjectProperty->PropertyClass != nullptr && !Object->IsA(ObjectProperty->PropertyClass)) {
		UE_LOG(LogReflectionPropertySerializer, Warning, TEXT("Skipped '%s': '%s' is a %s, not a %s."),
			*ObjectProperty->GetName(), *Object->GetPathName(), *Object->GetClass()->GetName(), *ObjectProperty->PropertyClass->GetName());

		return;
	}

	ObjectProperty->SetObjectPropertyValue(OutValue, Object);
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
	} else if (CastField<const FInterfaceProperty>(Property)) {
	}
	else if (FSoftObjectProperty* SoftObjectProperty = CastField<FSoftObjectProperty>(Property)) {
		TSharedPtr<FJsonObject> SoftJsonObjectProperty;
		FString PathString = "";
		
		switch (NewJsonValue->Type) {
			/* UEParse, extract it from the object */
			case EJson::Object:
				SoftJsonObjectProperty = NewJsonValue->AsObject();
				PathString = SoftJsonObjectProperty->GetStringField(TEXT("AssetPathName"));
			break;

			/* Older game builds */
			default:
				PathString = NewJsonValue->AsString();
			break;
		}

		if (PathString != "") {
			FSoftObjectPtr* ObjectPtr = static_cast<FSoftObjectPtr*>(OutValue);
			*ObjectPtr = FSoftObjectPath(PathString);

			if (!ObjectPtr->LoadSynchronous()) {
				/* Try importing it using Cloud */
				FString PackagePath;
				FString AssetName;
				PathString.Split(".", &PackagePath, &AssetName);
				TObjectPtr<UObject> T = nullptr;

				FString PropertyClassName = SoftObjectProperty->PropertyClass->GetName();
				
				IImporter::DownloadWrapper(T, PropertyClassName, AssetName, PackagePath);
			}
		}
	}
	else if (const FObjectPropertyBase* ObjectProperty = CastField<const FObjectPropertyBase>(Property)) {
		/* Need to serialize full UObject for object property */
		TObjectPtr<UObject> Object = nullptr;

		if (NewJsonValue->IsNull()) {
			SetObjectPropertyValueChecked(ObjectProperty, OutValue, nullptr);
		}

		if (NewJsonValue->Type == EJson::Object) {
			auto JsonValueAsObject = NewJsonValue->AsObject();

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

			if (UObject* CurrentObject = ObjectProperty->GetObjectPropertyValue(OutValue)) {
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
				bool bIsActorComponent = Object.Get()->IsA(UActorComponent::StaticClass());
				
				if (!bIsActorComponent) {
					SetObjectPropertyValueChecked(ObjectProperty, OutValue, Object);
				} else {
					if (FUObjectExport* TargetExport = ExportsContainer->GetExportByObjectPath(JsonValueAsObject)) {
						FUObjectJsonValueExport Properties = TargetExport->GetObject(TEXT("Properties"));

						if (TargetExport->Has(TEXT("LODData"))) {
							Properties.SetArray(TEXT("LODData"), TargetExport->GetArray(TEXT("LODData")));
						}

						if (GetPropertyName(ObjectProperty) != "AttachParent") {
							ObjectSerializer->DeserializeObjectProperties(Properties.JsonObject, Object);
						} else {
							SetObjectPropertyValueChecked(ObjectProperty, OutValue, Object);
						}
					}

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
			FGameplayTag NewTag = FGameplayTag::RequestGameplayTag(FName(*NewJsonValue->AsObject()->GetStringField(TEXT("TagName"))), false);
			*GameplayTagStr = NewTag;
			return;
		}

		/* FGameplayTagContainer (handled from UEParse data) */
		if (StructProperty->Struct == FGameplayTagContainer::StaticStruct()) {
			FGameplayTagContainer* GameplayTagContainerStr = static_cast<FGameplayTagContainer*>(OutValue);

			auto GameplayTags = JsonValue->AsArray();

			for (TSharedPtr GameplayTagValue : GameplayTags) {
				FString GameplayTagString = GameplayTagValue->AsString();
				FGameplayTag GameplayTag = FGameplayTag::RequestGameplayTag(FName(*GameplayTagString));
				
				GameplayTagContainerStr->AddTag(GameplayTag);
			}

			return;
		}

		if (StructProperty->Struct == FMovieSceneFrameRange::StaticStruct()) {
			FMovieSceneFrameRange* MovieSceneFrameRange = static_cast<FMovieSceneFrameRange*>(OutValue);
			TSharedPtr<FJsonObject> JsonObject = NewJsonValue->AsObject()->GetObjectField(TEXT("Value"));

			FMovieSceneFrameRange Range;

			if (JsonObject->HasField(TEXT("LowerBound"))) {
				TSharedPtr<FJsonObject> Bound = JsonObject->GetObjectField(TEXT("LowerBound"));
				TSharedPtr<FJsonObject> BoundValue = Bound->GetObjectField(TEXT("Value"));

				int32 Type = Bound->GetIntegerField(TEXT("Type"));
				int32 Value = BoundValue->GetIntegerField(TEXT("Value"));

				FFrameNumber BoundFrame;
				BoundFrame.Value = Value;

				if (Type == 0) {
					Range.Value.SetLowerBound(TRangeBound<FFrameNumber>::Exclusive(BoundFrame));
				} else if (Type == 1) {
					Range.Value.SetLowerBound(TRangeBound<FFrameNumber>::Inclusive(BoundFrame));
				}
			}

			if (JsonObject->HasField(TEXT("UpperBound"))) {
				TSharedPtr<FJsonObject> Bound = JsonObject->GetObjectField(TEXT("UpperBound"));
				TSharedPtr<FJsonObject> BoundValue = Bound->GetObjectField(TEXT("Value"));

				int32 Type = Bound->GetIntegerField(TEXT("Type"));
				int32 Value = BoundValue->GetIntegerField(TEXT("Value"));

				FFrameNumber BoundFrame;
				BoundFrame.Value = Value;

				if (Type == 0) {
					Range.Value.SetUpperBound(TRangeBound<FFrameNumber>::Exclusive(BoundFrame));
				} else if (Type == 1) {
					Range.Value.SetUpperBound(TRangeBound<FFrameNumber>::Inclusive(BoundFrame));
				}
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
				FSoftObjectPtr* ObjectPtr = static_cast<FSoftObjectPtr*>(OutValue);
				*ObjectPtr = FSoftObjectPath(PathString);

				if (!ObjectPtr->LoadSynchronous()) {
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

			check(ByteProperty->Enum);
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
			EnumProperty->GetUnderlyingProperty()->SetIntPropertyValue(OutValue, static_cast<int64>(NewJsonValue->AsNumber()));

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
		FieldPath.Generate(*NewJsonValue->AsString());
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