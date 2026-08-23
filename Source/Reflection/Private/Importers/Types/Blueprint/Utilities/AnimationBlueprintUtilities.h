/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "AnimGraphNode_Base.h"
#include "AnimGraphNode_BlendListByEnum.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Log.h"
#include "Serializers/ObjectSerializer.h"
#include "Utilities/JsonHelpers.h"

inline FStructProperty* GetNodeStructProperty(const UAnimGraphNode_Base* Node) {
	if (!Node) return nullptr;

	for (TFieldIterator<FStructProperty> It(Node->GetClass()); It; ++It) {
		if (FStructProperty* StructProp = *It; StructProp->Struct && StructProp->Struct->IsChildOf(FAnimNode_Base::StaticStruct())) {
			return StructProp;
		}
	}

	return nullptr;
}

inline UEdGraphPin* GetFirstOutputPin(UAnimGraphNode_Base* Node) {
	if (!Node) return nullptr;

	for (UEdGraphPin* Pin : Node->Pins) {
		if (Pin && Pin->Direction == EGPD_Output) {
			return Pin;
		}
	}

	return nullptr;
}

inline UEdGraphPin* FindOutputPin(UEdGraphNode* Node) {
	for (UEdGraphPin* Pin : Node->Pins) {
		if (Pin->Direction == EGPD_Output) {
			return Pin;
		}
	}
	return nullptr;
}

inline UEdGraphPin* FindInputPin(UEdGraphNode* Node) {
	for (UEdGraphPin* Pin : Node->Pins) {
		if (Pin->Direction == EGPD_Input) {
			return Pin;
		}
	}
	return nullptr;
}

inline void ReplaceLinkID(const TSharedPtr<FJsonValue>& Data, const TArray<FString>& NodesKeys, const FString& PinName = TEXT("")) {
	if (!Data.IsValid()) return;
	
	if (Data->Type == EJson::Object) {
		const TSharedPtr<FJsonObject> Obj = Data->AsObject();
		for (auto& Pair : Obj->Values) {
			if (Pair.Key == TEXT("LinkID")) {
				const double d = Pair.Value->AsNumber();
				const int32 Index = static_cast<int32>(d);

				if (Index == -1) continue;
				
				if (Index < NodesKeys.Num()) {
					Pair.Value = MakeShared<FJsonValueString>(NodesKeys[Index]);
				}
			} else {
				ReplaceLinkID(Pair.Value, NodesKeys, JsonKeyToString(Pair.Key));
			}
		}
	} else if (Data->Type == EJson::Array) {
		TArray<TSharedPtr<FJsonValue>> Array = Data->AsArray();
		
		for (int32 i = 0; i < Array.Num(); i++) {
			ReplaceLinkID(Array[i], NodesKeys, PinName);
		}
	}
}

inline void FilterAnimGraphNodeProperties(const TSharedPtr<FJsonObject>& JsonObject) {
	if (!JsonObject.IsValid()) return;
	
	TArray<FString> KeysToRemove;
	for (auto& Pair : JsonObject->Values) {
		if (!JsonKeyToString(Pair.Key).Contains(TEXT("AnimGraphNode"))) {
			KeysToRemove.Add(JsonKeyToString(Pair.Key));
		}
	}

	for (const FString& Key : KeysToRemove) {
		JsonObject->Values.Remove(StringToJsonKey(Key));
	}
}

inline TArray<TPair<FString, FString>> FindLinkIDs(const TSharedPtr<FJsonValue>& Data, const FString& ParentProperty = TEXT("")) {
    TArray<TPair<FString, FString>> Results;
    if (!Data.IsValid()) return Results;
	
    if (Data->Type == EJson::Object) {
        const TSharedPtr<FJsonObject> Object = Data->AsObject();
    	
        for (auto& Pair : Object->Values) {
            if (Pair.Key == TEXT("LinkID")) {
                FString LinkStr = Pair.Value->AsString();
                FString Prop = ParentProperty.IsEmpty() ? JsonKeyToString(Pair.Key) : ParentProperty;
            	
                Results.Add(TPair<FString, FString>(Prop, LinkStr));
            	continue;
            }
        	
            TArray<TPair<FString, FString>> SubResults = FindLinkIDs(Pair.Value, JsonKeyToString(Pair.Key));
            Results.Append(SubResults);
        }
    } else if (Data->Type == EJson::Array) {
        TArray<TSharedPtr<FJsonValue>> Arr = Data->AsArray();
        
        for (const TSharedPtr<FJsonValue>& Item : Arr) {
            TArray<TPair<FString, FString>> SubResults = FindLinkIDs(Item, ParentProperty);
        	
            Results.Append(SubResults);
        }
    }
	
    return Results;
}

template <typename KeyType>
inline void HarvestAndTagConnectedStateMachineNodes(const FString& StartKey, const FString& StateName, const FString& MachineName, TMap<KeyType, TSharedPtr<FJsonValue>>& Nodes) {
	if (!Nodes.Contains(StringToJsonKey(StartKey))) return;

	const TSharedPtr<FJsonValue> NodeValue = Nodes.FindChecked(StringToJsonKey(StartKey));

	if (NodeValue->Type == EJson::Object) {
		TSharedPtr<FJsonObject> NodeObject = NodeValue->AsObject();
		NodeObject->SetStringField(TEXT("State"), StateName);
		NodeObject->SetStringField(TEXT("Machine"), MachineName);

		Nodes[StringToJsonKey(StartKey)] = MakeShared<FJsonValueObject>(NodeObject);
	}
	
	TArray<TPair<FString, FString>> Links = FindLinkIDs(NodeValue, StartKey);
	
	for (const TPair<FString, FString>& LinkPair : Links) {
		FString NextKey = LinkPair.Value;

		/* AnimGraphNode_SaveCachedPose shouldn't be in a State Machine */
		if (NextKey.Contains("AnimGraphNode_SaveCachedPose")) {
			return;
		}
		
		HarvestAndTagConnectedStateMachineNodes(NextKey, StateName, MachineName, Nodes);
	}
}

/* Property bindings on animation graph nodes, and the struct describing them, arrived in 4.26 */
#if !UE4_25_BELOW
/* Stores a pin binding on a node.
 *
 * 5.4 moved the map off the node into an instanced binding object, leaving PropertyBindings behind
 * as PropertyBindings_DEPRECATED. The replacement lives in UAnimGraphNodeBinding_Base, which sits
 * in the module's Private folder with no public way to add an entry, so the map is reached through
 * reflection instead. The struct stored in it is the same FAnimGraphNodePropertyBinding either way.
 *
 * 5.3 and below still keep PropertyBindings on the node itself, public and undeprecated, so there
 * the map is simply written to. */
inline bool AddPropertyBinding(UAnimGraphNode_Base* Node, const FName PinName, const FAnimGraphNodePropertyBinding& PropertyBinding) {
#if UE5_4_BEYOND
	/* Taken by reflection so the private binding type never has to be named */
	const FObjectProperty* BindingProperty = CastField<FObjectProperty>(Node->GetClass()->FindPropertyByName(TEXT("Binding")));
	if (!BindingProperty) return false;

	UObject* BindingObject = BindingProperty->GetObjectPropertyValue_InContainer(Node);

	/* A node built straight from NewObject never ran PostPlacedNewNode, which is where the editor
	 * hands it one of these. UAnimGraphNode_Base::EnsureBindingsArePresent does the job but is
	 * protected, so the same thing happens here: the blueprint's binding class if it names one,
	 * the engine's own otherwise. */
	if (!BindingObject) {
		UClass* BindingClass = nullptr;

		if (const UAnimBlueprint* OuterBlueprint = Node->GetTypedOuter<UAnimBlueprint>()) {
			BindingClass = OuterBlueprint->GetDefaultBindingClass();
		}

		if (!BindingClass) {
			BindingClass = FindClassByType(TEXT("AnimGraphNodeBinding_Base"));
		}

		if (!BindingClass) return false;

		BindingObject = NewObject<UObject>(Node, BindingClass, NAME_None, RF_Transactional);
		if (!BindingObject) return false;

		Node->Modify();
		BindingProperty->SetObjectPropertyValue_InContainer(Node, BindingObject);
	}

	const FMapProperty* MapProperty = CastField<FMapProperty>(BindingObject->GetClass()->FindPropertyByName(TEXT("PropertyBindings")));
	if (!MapProperty) return false;

	const FStructProperty* ValueProperty = CastField<FStructProperty>(MapProperty->ValueProp);
	if (!ValueProperty || ValueProperty->Struct != FAnimGraphNodePropertyBinding::StaticStruct()) return false;

	BindingObject->Modify();

	FScriptMapHelper MapHelper(MapProperty, MapProperty->ContainerPtrToValuePtr<void>(BindingObject));

	const int32 Index = MapHelper.AddDefaultValue_Invalid_NeedsRehash();

	*static_cast<FName*>(static_cast<void*>(MapHelper.GetKeyPtr(Index))) = PinName;
	ValueProperty->Struct->CopyScriptStruct(MapHelper.GetValuePtr(Index), &PropertyBinding);

	MapHelper.Rehash();

	return true;
#else
	Node->PropertyBindings.Add(PinName, PropertyBinding);

	return true;
#endif
}
#endif

inline void HandlePropertyBinding(FUObjectExport* NodeExport, const TArray<TSharedPtr<FJsonValue>>& JsonObjects, UAnimGraphNode_Base* Node, IImporter* Importer, UAnimBlueprint* AnimBlueprint) {
	const TSharedPtr<FJsonObject> NodeProperties = NodeExport->JsonObject;
	
	/* Let the user know that this node has nodes plugged into it */
	if (NodeProperties->HasField(TEXT("EvaluateGraphExposedInputs"))) {
		const TSharedPtr<FJsonObject> EvaluateGraphExposedInputs = NodeProperties->GetObjectField(TEXT("EvaluateGraphExposedInputs"));

		bool bBoundFunction = EvaluateGraphExposedInputs->GetStringField(TEXT("BoundFunction")) != "None";
		
		if (EvaluateGraphExposedInputs->HasField(TEXT("CopyRecords")) || bBoundFunction) {
			const TArray<TSharedPtr<FJsonValue>> CopyRecords = EvaluateGraphExposedInputs->GetArrayField(TEXT("CopyRecords"));

			if (CopyRecords.Num() > 0) {
				for (const auto& CopyRecordAsValue : CopyRecords) {
					const TSharedPtr<FJsonObject>& CopyRecordAsObject = CopyRecordAsValue->AsObject();

					if (!CopyRecordAsObject->HasField(TEXT("DestProperty"))) continue;
					if (CopyRecordAsObject->HasField(TEXT("BoundFunction")) && CopyRecordAsObject->GetStringField(TEXT("BoundFunction")) != TEXT("None")) {
						bBoundFunction = true;

						continue;
					}

					FString SourcePropertyName = CopyRecordAsObject->GetStringField(TEXT("SourcePropertyName"));
					
					/* Take the property's name from the object name:
					 *
					 * FloatProperty'AnimNode_SkeletalControlBase:Alpha' ->
					 * :Alpha' ->
					 * Alpha */
					const TSharedPtr<FJsonObject> DestProperty = CopyRecordAsObject->GetObjectField(TEXT("DestProperty"));
					FString PinName = DestProperty->GetStringField(TEXT("ObjectName")); {
						PinName.Split(TEXT(":"), nullptr, &PinName);
						PinName = PinName.Replace(TEXT("'"), TEXT(""));
					}

					FString PinCategory = DestProperty->GetStringField(TEXT("ObjectName")); {
						PinCategory.Split(TEXT("'"), &PinCategory, nullptr);
						PinCategory.Split(TEXT("Property"), &PinCategory, nullptr);
						PinCategory = PinCategory.ToLower();
					}

					/* Cannot be compiled */
					if (PinName.Equals(TEXT("BlendTime")) || PinName.Equals(TEXT("BlendWeights"))) continue;

					UClass* AnimClass = AnimBlueprint->GeneratedClass;

					/* Property bindings on animation graph nodes, and the struct describing
					 * them, arrived in 4.26. Before that a node carries no binding to fill in. */
#if !UE4_25_BELOW
					FName PinNameAsName(PinName);

					/* Setup Property Binding */
					FAnimGraphNodePropertyBinding PropertyBinding;
					PropertyBinding.PropertyName = PinNameAsName;
					PropertyBinding.PathAsText = FText::FromString(SourcePropertyName);
					PropertyBinding.PinType.PinCategory = FName(PinCategory);
					PropertyBinding.bIsBound = true;
					PropertyBinding.PropertyPath.Append({ SourcePropertyName });

					/* The category above is the property's type lowercased, which was the pin
					 * category's spelling in 4.x. 5.0 folded float and double into "real" with the
					 * width in the sub category, so "float" now names no pin at all and the binding
					 * describes a pin that doesn't exist. The pin itself settles it when it's there. */
					if (const UEdGraphPin* DestinationPin = Node->FindPin(PinNameAsName, EGPD_Input)) {
						PropertyBinding.PinType = DestinationPin->PinType;
					}

					TSharedPtr<FJsonObject> SourcePropertyObject = GetExportMatchingWith(SourcePropertyName, "Name", JsonObjects);
					if (PinCategory == "struct" && SourcePropertyObject.IsValid() && SourcePropertyObject->HasField(TEXT("Struct"))) {
						TSharedPtr<FJsonObject> StructObject = SourcePropertyObject->GetObjectField(TEXT("Struct"));

						TObjectPtr<UObject> LoadedObject;
						Importer->LoadExport<UObject>(&StructObject, LoadedObject);

						PropertyBinding.PinType.PinSubCategoryObject = LoadedObject.Get();
					} else {
						FProperty* Prop = AnimClass->FindPropertyByName(*SourcePropertyName);
						
						if (FStructProperty* StructProp = CastField<FStructProperty>(Prop))
						{
							UScriptStruct* ScriptStruct = StructProp->Struct;
							PropertyBinding.PinType.PinSubCategoryObject = ScriptStruct;
						}
					}

					if (CopyRecordAsObject->HasField(TEXT("SourceSubPropertyName")) && CopyRecordAsObject->GetStringField(TEXT("SourceSubPropertyName")) != "None") {
						FString SourceSubPropertyName = CopyRecordAsObject->GetStringField(TEXT("SourceSubPropertyName"));
						PropertyBinding.PathAsText = FText::FromString(SourcePropertyName + "." + SourceSubPropertyName);

						PropertyBinding.PropertyPath.Append({ SourceSubPropertyName });

						if (CopyRecordAsObject->GetObjectField(TEXT("CachedSourceStructSubProperty"))) {
							TSharedPtr<FJsonObject> StructObject = CopyRecordAsObject->GetObjectField(TEXT("CachedSourceStructSubProperty"));
							
							TObjectPtr<UObject> LoadedObject;
							Importer->LoadExport<UObject>(&StructObject, LoadedObject);

							auto StructProperty = LoadStructProperty(StructObject);

							if (StructProperty) {
								PropertyBinding.PinType.PinSubCategoryObject = StructProperty->Struct;
							}
						}
					}

					if (!AddPropertyBinding(Node, PinNameAsName, PropertyBinding)) {
						UE_LOG(LogReflection, Warning, TEXT("Binding dropped: %s.%s <- %s"), *Node->GetClass()->GetName(), *PinName, *SourcePropertyName);
					}
#endif

					if (PinName == "ActiveEnumValue" && Node != nullptr) {
						if (UAnimGraphNode_BlendListByEnum* BlendListByEnum = Cast<UAnimGraphNode_BlendListByEnum>(Node)) {
							FProperty* Prop = AnimClass->FindPropertyByName(*SourcePropertyName);

							UEnum* EnumRef = nullptr;
							
							if (FEnumProperty* EnumProp = CastField<FEnumProperty>(Prop)) {
								EnumRef = EnumProp->GetEnum();
							} else if (FByteProperty* ByteProp = CastField<FByteProperty>(Prop)) {
								EnumRef = ByteProp->Enum;
							}

							if (EnumRef) {
								FProperty* BoundEnumProp = BlendListByEnum->GetClass()->FindPropertyByName(TEXT("BoundEnum"));
								
								if (BoundEnumProp) {
									if (FObjectProperty* ObjectProp = CastField<FObjectProperty>(BoundEnumProp)) {
										ObjectProp->SetObjectPropertyValue_InContainer(BlendListByEnum, EnumRef);
									}
								}
							}
						}
					}
				}
			}

			Node->NodeComment = NodeExport->GetName().ToString();
			Node->bCommentBubbleVisible = bBoundFunction;
		}
	}
}