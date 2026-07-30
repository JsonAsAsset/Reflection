/* Copyright Reflection Contributors 2024-2026 */

#include "Serializers/ObjectSerializer.h"

#include "Animation/AnimNodeBase.h"
#include "Animation/WidgetAnimation.h"
#include "Engine/Compatibility.h"

#include "Serializers/PropertySerializer.h"
#include "UObject/Package.h"
#include "Utilities/JsonUtilities.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Components/PostProcessComponent.h"
#include "Particles/ParticleEmitter.h"
#include "Particles/ParticleLODLevel.h"
#include "Particles/ParticleSystem.h"
#include "Particles/TypeData/ParticleModuleTypeDataGpu.h"
#include "Settings/Runtime.h"

/* ReSharper disable once CppDeclaratorNeverUsed */
DECLARE_LOG_CATEGORY_CLASS(LogReflectionObjectSerializer, All, All);

#if UE5_2_BEYOND
UE_DISABLE_OPTIMIZATION
#else
PRAGMA_DISABLE_OPTIMIZATION
#endif

/* ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */

UObjectSerializer::UObjectSerializer(): Parent(nullptr), PropertySerializer(nullptr) { }

void UObjectSerializer::SetupExports(const TArray<TSharedPtr<FJsonValue>>& InObjects) {
	Exports = InObjects;
}

UObject* UObjectSerializer::SpawnExport(FUObjectExport* Export, const bool bOnlySerialize) {
	if (!bOnlySerialize) {
		if (Export->Object != nullptr) return nullptr;
	}

	const UClass* Class = Export->GetClass();
	if (!Class) return nullptr;

	const FString Outer = GetOuterFromObjectOuter(Export->JsonObject->TryGetField(TEXT("Outer")));
	UObject* ObjectOuter = nullptr;

	/* Find the outer */
	if (FUObjectExport* OuterExport = PropertySerializer->ExportsContainer->Find(Outer); OuterExport->JsonObject.IsValid()) {
		if (OuterExport->Object == nullptr) {
			SpawnExport(OuterExport);
		}
		
		ObjectOuter = OuterExport->Object;
	}

	/* Find the blueprint outer */
	if (!Outer.IsEmpty()) {
		FString PotentialBPName;
		Outer.Split("_C", &PotentialBPName, nullptr, ESearchCase::CaseSensitive, ESearchDir::FromEnd);
		
		if (FUObjectExport* OuterExport = PropertySerializer->ExportsContainer->Find(PotentialBPName); OuterExport->JsonObject.IsValid()) {
			if (OuterExport->Object == nullptr) {
				SpawnExport(OuterExport);
			}

			if (OuterExport->Object) {
				ObjectOuter = OuterExport->Object;
			}
		}
	}
	
	/* Find the outer using the tree segment */
	const TArray<FName> TreeSegments = Export->GetOuterTreeSegments(true);

	if (TreeSegments.Num() > 0) {
		if (FUObjectExport* OuterExport = PropertySerializer->ExportsContainer->FindByTreeSegment(TreeSegments); !IsEmpty(OuterExport->JsonObject->Values)) {
			if (OuterExport->Object == nullptr) {
				SpawnExport(OuterExport);
			}
		
			if (OuterExport->Object) {
				ObjectOuter = OuterExport->Object;
			}
		}
	}

	if (!ObjectOuter && !Export->Object) return nullptr;

	/* Default flags */
	EObjectFlags Flags = RF_Public | RF_Transactional;

	/* Parse the flags back into EObjectFlags, important for component archetypes */
	if (Export->Has("Flags")) {
		Flags = ParseObjectFlags(Export->GetString("Flags"));
	}

	if (!Export->Object) {
		FString ObjectName = Export->GetName().ToString();

		if (Class->IsChildOf(UWidgetAnimation::StaticClass())) {
			ObjectName.Split("_INST", &ObjectName, nullptr, ESearchCase::CaseSensitive);
		}
		
		Export->Object = NewObject<UObject>(ObjectOuter, Class, FName(ObjectName), Flags);
	}
	
	if (UParticleSystem* ParticleSystem = Cast<UParticleSystem>(Export->Object)) {
		ParticleSystem->PreEditChange(nullptr);
	}
	
	DeserializeObjectProperties(Export->GetProperties(), Export->Object);

	if (UParticleEmitter* ParticleEmitter = Cast<UParticleEmitter>(Export->Object)) {
		ParticleEmitter->EmitterEditorColor = FColor::MakeRandomColor();
		ParticleEmitter->EmitterEditorColor.A = 255;

		ParticleEmitter->UpdateModuleLists();
		ParticleEmitter->PostEditChange();
		
		/* Initialize epic detail mode to enabled if it's an older version of the engine */
		if (!GReflectionRuntime.IsUE5()) {
#if ENGINE_UE5 && ENGINE_MINOR_VERSION >= 3
			if (ParticleEmitter->DetailModeBitmask & 1 << EParticleDetailMode::PDM_High) {
				ParticleEmitter->DetailModeBitmask |= 1 << EParticleDetailMode::PDM_Epic;
			}
#endif
		}
	}

	if (UParticleLODLevel* ParticleLODLevel = Cast<UParticleLODLevel>(Export->Object)) {
		ParticleLODLevel->ConvertedModules = true;
	}
	
	if (UParticleSystem* ParticleSystem = Cast<UParticleSystem>(Export->Object)) {
		ParticleSystem->PostEditChange();
		ParticleSystem->SetupSoloing();
	}

	if (UParticleModule* ParticleModule = Cast<UParticleModule>(Export->Object)) {
		ParticleModule->PostLoad();
	}
	
	return Export->Object;
}

void UObjectSerializer::SetPropertySerializer(UPropertySerializer* NewPropertySerializer) {
	check(NewPropertySerializer);

	PropertySerializer = NewPropertySerializer;
	NewPropertySerializer->ObjectSerializer = this;
}

void UObjectSerializer::SetExportForDeserialization(const TSharedPtr<FJsonObject>& JsonObject, UObject* Object) {
	ExportsToNotDeserialize.Add(JsonObject->GetStringField(TEXT("Name")));
	ConstructedObjects.Add(JsonObject->GetStringField(TEXT("Name")), Object);
}

void UObjectSerializer::DeserializeExports(FUObjectExportContainer* Container, const bool CreateObjects) {
	if (Container == nullptr) return;

	/* Callers that build their own container never go through USerializerContainer::Initialize,
	 * so this is the only place the property serializer learns about it. */
	if (PropertySerializer != nullptr) {
		PropertySerializer->ExportsContainer = Container;
	}

	if (CreateObjects) {
		TMap<TSharedPtr<FJsonObject>, UObject*> ExportsMap;
		
		for (FUObjectExport* Export : Container->Exports) {
			FString Type = Export->GetType().ToString();
		
			/* Check if it's not supposed to be deserialized */
			if (ExportsToNotDeserialize.Contains(Export->GetName().ToString())) continue;

			if (WhitelistedTypes.Num() > 0) {
				bool bMatchFound = false;

				for (const FString& Whitelisted : WhitelistedTypes) {
					if (Type.Contains(Whitelisted)) {
						bMatchFound = true;
						break;
					}
				}

				if (!bMatchFound) {
					continue;
				}
			}

			if (WhitelistedTypesStartingWith.Num() > 0) {
				bool bMatchFound = false;

				for (const FString& Whitelisted : WhitelistedTypesStartingWith) {
					if (Type.StartsWith(Whitelisted)) {
						bMatchFound = true;
						break;
					}
				}

				if (!bMatchFound) {
					continue;
				}
			}

			if (WhitelistedTreeSegments.Num() > 0) {
				auto TreeSegments = Export->GetOuterTreeSegments(true);
			
				if (TreeSegments.Num() > 0 && WhitelistedTreeSegments != TreeSegments) {
					continue;
				}
			}
			
			if (BlacklistedTypes.Num() > 0) {
				if (BlacklistedTypes.Contains(Type)) {
					continue;
				}
			}
		
			if (Type == "NavCollision" || Type == "MorphTarget") continue;
			
			DeserializeExport(Export, ExportsMap);
		}

		for (const auto& Pair : ExportsMap) {
			TSharedPtr<FJsonObject> Properties = Pair.Key;
			UObject* Object = Pair.Value;

			DeserializeObjectProperties(Properties, Object);
		}
	}
}

void UObjectSerializer::DeserializeExport(FUObjectExport* Export, TMap<TSharedPtr<FJsonObject>, UObject*>& ExportsMap) {
	if (Export->Object != nullptr) return;

	const TSharedPtr<FJsonObject> ExportObject = Export->JsonObject;

	/* No name means no export */
	if (!ExportObject->HasField(TEXT("Name"))) return;

	const FString Name = ExportObject->GetStringField(TEXT("Name"));
	const FString Type = ExportObject->GetStringField(TEXT("Type")).Replace(TEXT("CommonWidgetSwitcher"), TEXT("CommonActivatableWidgetSwitcher"));
		
	/* Check if it's not supposed to be deserialized */
	if (ExportsToNotDeserialize.Contains(Name)) return;
	if (Type == "BodySetup" || Type == "NavCollision") return;

	FString ClassName = ExportObject->GetStringField(TEXT("Class"));

	if (ExportObject->HasField(TEXT("Template"))) {
		auto TemplateObject = Export->GetObject("Template");
		ClassName = ReadPathFromObject(TemplateObject).Replace(TEXT("Default__"), TEXT(""));
	}
	
	if (ClassName.Contains("'")) {
		ClassName.Split("'", nullptr, &ClassName, ESearchCase::IgnoreCase, ESearchDir::FromStart);
		ClassName.Split("'", &ClassName, nullptr, ESearchCase::IgnoreCase, ESearchDir::FromStart);
	}
	
	const UClass* Class = FindClassByType(ClassName);
	
	if (!Class) {
		Class = FindClassByType(Type);
	}

	if (!Class) return;

	const FString Outer = GetOuterFromObjectOuter(ExportObject->TryGetField(TEXT("Outer")));
	UObject* ObjectOuter = nullptr;

	if (FUObjectExport* FoundExport = PropertySerializer->ExportsContainer->Find(Outer); FoundExport->JsonObject.IsValid()) {
		if (FoundExport->Object == nullptr) {
			DeserializeExport(FoundExport, ExportsMap);
		}
		
		UObject* FoundObject = FoundExport->Object;
		ObjectOuter = FoundObject;
	}

	const TArray<FName> TreeSegments = Export->GetOuterTreeSegments(true);

	if (TreeSegments.Num() > 0) {
		if (FUObjectExport* FoundExport = PropertySerializer->ExportsContainer->FindByTreeSegment(TreeSegments); !IsEmpty(FoundExport->JsonObject->Values)) {
			if (FoundExport->Object == nullptr) {
				DeserializeExport(FoundExport, ExportsMap);
			}
		
			UObject* FoundObject = FoundExport->Object;
			ObjectOuter = FoundObject;
		}
	}

	if (UObject** ConstructedObject = ConstructedObjects.Find(Outer)) {
		ObjectOuter = *ConstructedObject;
	}

	if (Export->Object) return;
	
	if (ObjectOuter == nullptr) {
		ObjectOuter = Parent;
	}

	UObject* NewUObject = NewObject<UObject>(ObjectOuter, Class, FName(*Name), RF_Public | RF_Transactional);

	if (ExportObject->HasField(TEXT("Properties"))) {
		const TSharedPtr<FJsonObject> Properties = ExportObject->GetObjectField(TEXT("Properties"));

		ExportsMap.Add(Properties, NewUObject);
	} else {
		ExportsMap.Add(ExportObject, NewUObject);
	}

	/* Add it to the referenced objects */
	Export->Object = NewUObject;
}

void UObjectSerializer::DeserializeObjectProperties(const TSharedPtr<FJsonObject>& Properties, UObject* Object) const {
	if (Object == nullptr) return;

	if (Cast<UParticleSystem>(Object)) {
		Object->PreEditChange(nullptr);
	}

	const UClass* ObjectClass = Object->GetClass();

	for (FProperty* Property = ObjectClass->PropertyLink; Property; Property = Property->PropertyLinkNext) {
		FString PropertyName = Property->GetName();

		if (!PropertySerializer->ShouldDeserializeProperty(Property)) continue;

		void* PropertyValue = Property->ContainerPtrToValuePtr<void>(Object);
		const bool HasHandledProperty = PassthroughPropertyHandler(Property, PropertyName, PropertyValue, Properties, PropertySerializer);
		
		/* Handler Specifically for Animation Blueprint Graph Nodes */
		if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property)) {
			if (StructProperty->Struct->IsChildOf(FAnimNode_Base::StaticStruct())) {
				void* StructPtr = StructProperty->ContainerPtrToValuePtr<void>(Object);

				if (static_cast<FAnimNode_Base*>(StructPtr)) {
					PropertySerializer->DeserializeStruct(StructProperty->Struct, Properties.ToSharedRef(), PropertyValue, Object);
				}
			}
		}

#if ENGINE_UE5
		/* ExponentialHeightFog: Migration to new API */
		if (Object->IsA<UExponentialHeightFogComponent>()) {
			if (Property->NamePrivate == "FogInscatteringLuminance" && !Properties->Values.Contains("FogInscatteringLuminance")) {
				PropertyName = "FogInscatteringColor";
			}

			if (Property->NamePrivate == "DirectionalInscatteringLuminance" && !Properties->Values.Contains("DirectionalInscatteringLuminance")) {
				PropertyName = "DirectionalInscatteringColor";
			}
		}
#endif
		
		if (Properties->HasField(PropertyName) && !HasHandledProperty && PropertyName != "LODParentPrimitive" && PropertyName != "bIsCookedForEditor") {
			const TSharedPtr<FJsonValue>& ValueObject = Properties->Values.FindChecked(PropertyName);

			if (Property->ArrayDim == 1 || ValueObject->Type == EJson::Array) {
				PropertySerializer->DeserializePropertyValue(Property, ValueObject.ToSharedRef(), PropertyValue, Object);
			}
		}
	}
	
	if (Cast<UStaticMeshComponent>(Object)
		|| Cast<UParticleSystem>(Object)
		|| Cast<UParticleLODLevel>(Object)
		|| Cast<UParticleModule>(Object)
		|| Cast<UParticleEmitter>(Object)) {
		Object->PostEditImport();
	}

#if 0 /* @REVISIT: Sometimes entire modules are cooked into GPU data */
	if (UParticleModuleTypeDataGpu* ParticleModuleTypeDataGPU = Cast<UParticleModuleTypeDataGpu>(Object)) {
		ParticleModuleTypeDataGPU->GetOutermost()->bIsCookedForEditor = true;
	}
#endif

	/* Volumes are not supported, yet. ;] */
	if (UPostProcessComponent* PostProcessComponent = Cast<UPostProcessComponent>(Object)) {
		PostProcessComponent->bUnbound = true;
	}

	/* This is a use case for importing maps and parsing static mesh components
	 * using the object and property serializer, this was initially wanted to be
	 * done completely without any manual work. (using the de-serializers)
	 * 
	 * However I don't think it's possible to do so. as I haven't seen any native
	 * property that can do this using the data provided in UEParse.
	 */
	if (Properties->HasField(TEXT("LODData")) && Cast<UStaticMeshComponent>(Object)) {
		UStaticMeshComponent* StaticMeshComponent = Cast<UStaticMeshComponent>(Object);
		if (!StaticMeshComponent) return;
		
		TArray<TSharedPtr<FJsonValue>> ObjectLODData = Properties->GetArrayField(TEXT("LODData"));
		int CurrentLOD = -1;
		
		for (const auto& CurrentLODValue : ObjectLODData) {
			CurrentLOD++;

			const TSharedPtr<FJsonObject> CurrentLODObject = CurrentLODValue->AsObject();

			/* Must contain vertex colors, or else it's an empty LOD */
			if (!CurrentLODObject->HasField(TEXT("OverrideVertexColors"))) continue;

			const TSharedPtr<FJsonObject> OverrideVertexColorsObject = CurrentLODObject->GetObjectField(TEXT("OverrideVertexColors"));

			if (!OverrideVertexColorsObject->HasField(TEXT("Data"))) continue;

			const int32 NumVertices = OverrideVertexColorsObject->GetIntegerField(TEXT("NumVertices"));
			const TArray<TSharedPtr<FJsonValue>> DataArray = OverrideVertexColorsObject->GetArrayField(TEXT("Data"));

			/* Template of the target data */
			FString Output = FString::Printf(TEXT("CustomLODData LOD=%d, ColorVertexData(%d)=("), CurrentLOD, NumVertices);

			/* Append the colors in the expected format */
			for (int32 i = 0; i < DataArray.Num(); ++i) {
				FString ColorValue = DataArray.operator[](i)->AsString();
				Output.Append(ColorValue);

				/* Add a comma unless it's the last element */
				if (i < DataArray.Num() - 1) {
					Output.Append(TEXT(","));
				}
			}

			Output.Append(TEXT(")"));
		
			StaticMeshComponent->ImportCustomProperties(*Output, GWarn);
		}
	}
}

#if UE5_2_BEYOND
UE_ENABLE_OPTIMIZATION
#else
PRAGMA_ENABLE_OPTIMIZATION
#endif