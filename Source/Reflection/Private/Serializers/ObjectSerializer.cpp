/* Copyright Reflection Contributors 2024-2026 */

#include "Serializers/ObjectSerializer.h"

#include "Animation/AnimNodeBase.h"
#include "Animation/WidgetAnimation.h"
#include "Engine/Compatibility.h"

#include "Serializers/PropertySerializer.h"
#include "Serializers/VolumeBrush.h"
#include "Serializers/FoliageActor.h"
#include "Importers/Constructor/ImportIssues.h"
#include "Engine/Log.h"
#include "InstancedFoliageActor.h"
#include "UObject/Package.h"
#include "Utilities/JsonHelpers.h"
#include "Utilities/Containers.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Components/PostProcessComponent.h"
#include "Components/BrushComponent.h"
#include "Engine/Brush.h"
#include "Engine/Properties.h"
#include "Particles/ParticleEmitter.h"
#include "Particles/ParticleLODLevel.h"
#include "Particles/ParticleSystem.h"
#include "Particles/TypeData/ParticleModuleTypeDataGpu.h"
#include "Components/StaticMeshComponent.h"
/* Spelled out on its own from UE5, and inside the component before that */
#if __has_include("StaticMeshComponentLODInfo.h")
#include "StaticMeshComponentLODInfo.h"
#endif
#include "Engine/StaticMesh.h"
#include "MeshDescription.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshResources.h"
#include "Misc/Parse.h"
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

/* Whether an export can be made at all, and what is already there under that name.
 *
 * Two ways a class read out of an export cannot be built. One is abstract, which the engine
 * refuses and then ensures over; the other is a name the outer already has something at, which
 * the engine takes as two objects claiming one path and calls fatal. An actor spawned from its
 * class arrives with its default subobjects already built, so every export naming one of those is
 * the second case: RootComponent0 on a foliage actor is made by the actor's own constructor, and
 * the export naming it is naming the thing that is there rather than asking for another. */
namespace {
	UObject* AlreadyThere(UObject* Outer, const FName Named) {
		if (Outer == nullptr || Named.IsNone()) return nullptr;

		return StaticFindObjectFast(UObject::StaticClass(), Outer, Named);
	}

	bool CanBeMade(const UClass* Class, const FString& Named, const FString& Type) {
		if (Class == nullptr) return false;

		if (!Class->HasAnyClassFlags(CLASS_Abstract)) return true;

		UE_LOG(LogReflection, Error, TEXT("\"%s\" is written as a %s and that read back as %s, which cannot be made"), *Named, *Type, *Class->GetName());

		FImportIssues::Report(
			EImportIssue::Data,
			FString::Printf(TEXT("\"%s\" could not be made"), *Named),
			FString::Printf(TEXT("The export says it is a %s. The class of that name in this engine is %s, which is abstract and so is not something that can be made. Nothing was put in the object's place and the rest of the import carried on."), *Type, *Class->GetName())
		);

		return false;
	}
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
	if (FUObjectExport* OuterExport = PropertySerializer->ExportsContainer->Find(Outer); !Outer.IsEmpty() && OuterExport->JsonObject.IsValid()) {
		if (OuterExport->Object == nullptr) {
			SpawnExport(OuterExport);
		}
		
		ObjectOuter = OuterExport->Object;
	}

	/* Find the blueprint outer */
	if (!Outer.IsEmpty()) {
		FString PotentialBPName;
		Outer.Split("_C", &PotentialBPName, nullptr, ESearchCase::CaseSensitive, ESearchDir::FromEnd);
		
		if (FUObjectExport* OuterExport = PropertySerializer->ExportsContainer->Find(PotentialBPName); OuterExport->JsonObject.IsValid() && !IsEmpty(OuterExport->JsonObject->Values)) {
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
		if (FUObjectExport* OuterExport = PropertySerializer->ExportsContainer->FindByTreeSegment(TreeSegments); OuterExport->IsJsonValid() && !IsEmpty(OuterExport->JsonObject->Values)) {
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

		const FName Named = StringToName(ObjectName);

		if (UObject* Standing = AlreadyThere(ObjectOuter, Named)) {
			Export->Object = Standing;
		} else if (CanBeMade(Class, ObjectName, Export->GetType().ToString())) {
			Export->Object = NewObject<UObject>(ObjectOuter, ToNewObjectClass(Class), Named, Flags);
		} else {
			return nullptr;
		}
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
		if (!GReflectionRuntime.IsUE5OrNewer()) {
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
		
			if (Type == "NavCollision") continue;
			
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
		if (FUObjectExport* FoundExport = PropertySerializer->ExportsContainer->FindByTreeSegment(TreeSegments); FoundExport->IsJsonValid() && !IsEmpty(FoundExport->JsonObject->Values)) {
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

	UObject* NewUObject = AlreadyThere(ObjectOuter, FName(*Name));

	if (NewUObject == nullptr) {
		if (!CanBeMade(Class, Name, Type)) return;

		NewUObject = NewObject<UObject>(ObjectOuter, ToNewObjectClass(Class), FName(*Name), RF_Public | RF_Transactional);
	}

	if (ExportObject->HasField(TEXT("Properties"))) {
		const TSharedPtr<FJsonObject> Properties = ExportObject->GetObjectField(TEXT("Properties"));

		ExportsMap.Add(Properties, NewUObject);
	} else {
		ExportsMap.Add(ExportObject, NewUObject);
	}

	/* Add it to the referenced objects */
	Export->Object = NewUObject;
}

/* Painted vertices: the position and normal each colour was painted at.
 *
 * The colours the game cooked are indexed by the vertex order the game cooked, and the editor
 * builds an order of its own out of the mesh description. Its build merges any two wedges sharing
 * a position, a colour and a normal whose UVs sit within a thousandth of each other, so a mesh
 * carrying a pair of those comes out a vertex short and every colour past the first merge lands on
 * the wrong vertex. Most meshes have no such pair and survive; the ones that do come out scrambled.
 *
 * Painted vertices are how the engine repairs a mesh that was rebuilt under its colours: it matches
 * each one onto the built mesh by position and normal rather than trusting the index. Read off the
 * mesh description, whose vertices are still in the order the game cooked them.
 *
 * Handed back rather than written into the text ImportCustomProperties takes: that reads painted
 * vertices of its own and then empties them again on its way to the colours, by way of
 * RemoveInstanceVertexColorsFromLOD, so nothing spelled out there lives to be used. */
static TArray<FPaintedVertex> BuildPaintedVertices(UStaticMesh* StaticMesh, const int32 LodIndex, const TArray<TSharedPtr<FJsonValue>>& Colors) {
	TArray<FPaintedVertex> Painted;

	if (StaticMesh == nullptr) return Painted;

	FMeshDescription* MeshDescription = StaticMesh->IsMeshDescriptionValid(LodIndex) ? StaticMesh->GetMeshDescription(LodIndex) : nullptr;

	if (MeshDescription == nullptr) {
		UE_LOG(LogReflectionObjectSerializer, Warning, TEXT("\"%s\" LOD%d holds no mesh description, so there is nothing to say where its %d cooked colour(s) were painted"), *StaticMesh->GetName(), LodIndex, Colors.Num());

		return Painted;
	}

	/* Only an index for index match says anything about where a colour was painted */
	if (MeshDescription->Vertices().Num() != Colors.Num()) {
		UE_LOG(LogReflectionObjectSerializer, Warning, TEXT("\"%s\" LOD%d describes %d vertices and the game cooked %d colour(s) for it, which are not the same mesh"), *StaticMesh->GetName(), LodIndex, MeshDescription->Vertices().Num(), Colors.Num());

		return Painted;
	}

	FStaticMeshAttributes Attributes(*MeshDescription);

	const TVertexAttributesRef<FVector3f> Positions = Attributes.GetVertexPositions();
	const TVertexInstanceAttributesRef<FVector3f> Normals = Attributes.GetVertexInstanceNormals();

	/* A normal lives on the instances rather than the vertex, and every instance the mesh importer
	 * made carries the one normal the game gave that vertex, so any of them answers for all */
	TArray<FVector3f> VertexNormals;
	VertexNormals.SetNumZeroed(Colors.Num());

	for (const FVertexInstanceID InstanceID : MeshDescription->VertexInstances().GetElementIDs()) {
		const int32 Vertex = MeshDescription->GetVertexInstanceVertex(InstanceID).GetValue();

		if (VertexNormals.IsValidIndex(Vertex)) {
			VertexNormals[Vertex] = Normals[InstanceID];
		}
	}

	Painted.Reserve(Colors.Num());

	for (int32 Vertex = 0; Vertex < Colors.Num(); ++Vertex) {
		const FString Color = Colors[Vertex]->AsString();

		if (Color.Len() < 8) {
			UE_LOG(LogReflectionObjectSerializer, Warning, TEXT("\"%s\" LOD%d spells a colour in %d character(s) where eight were expected, so its paint is left where the game indexed it"), *StaticMesh->GetName(), LodIndex, Color.Len());

			return TArray<FPaintedVertex>();
		}

		/* ARGB, which is the order the colour buffer reads the same string in */
		auto Channel = [&Color](const int32 Offset) {
			return static_cast<uint8>(FParse::HexDigit(Color[Offset]) * 16 + FParse::HexDigit(Color[Offset + 1]));
		};

		const FVector3f Position = Positions[FVertexID(Vertex)];
		const FVector3f Normal = VertexNormals[Vertex];

		FPaintedVertex& Entry = Painted.AddDefaulted_GetRef();

		Entry.Position = FVector(Position.X, Position.Y, Position.Z);
		Entry.Normal = FVector4(Normal.X, Normal.Y, Normal.Z, 1.0f);
		Entry.Color = FColor(Channel(2), Channel(4), Channel(6), Channel(0));
	}

	return Painted;
}

/* The cooked colours put back where they were painted, one for each vertex the editor built.
 *
 * Worked out before the component is handed anything. Replacing a colour buffer the component
 * already holds frees one the render thread may be part way through uploading, and it answers that
 * with a pure virtual call on the array it was reading.
 *
 * Empty when there is nothing to work from, which leaves the colours as the game indexed them. */
static TArray<FColor> RepaintOverrideColors(UStaticMesh* StaticMesh, const int32 LodIndex, const TArray<FPaintedVertex>& Painted) {
	TArray<FColor> Repainted;

	if (StaticMesh == nullptr || Painted.Num() == 0) return Repainted;

	FStaticMeshRenderData* RenderData = GetStaticMeshRenderData(StaticMesh);

	if (RenderData == nullptr || !RenderData->LODResources.IsValidIndex(LodIndex)) return Repainted;

	FStaticMeshLODResources& Resources = RenderData->LODResources[LodIndex];
	FStaticMeshVertexBuffers& Buffers = Resources.VertexBuffers;

	const int32 BuiltVertices = Resources.GetNumVertices();

	/* A mesh still being built answers with nothing, and there is nothing to match onto */
	if (BuiltVertices == 0) {
		UE_LOG(LogReflectionObjectSerializer, Warning, TEXT("\"%s\" LOD%d has not been built yet, so its %d cooked colour(s) stay where the game indexed them"), *StaticMesh->GetName(), LodIndex, Painted.Num());

		return Repainted;
	}

	/* The old buffers go unread when the painted vertices carry the colours themselves */
	RemapPaintedVertexColors(
		Painted,
		nullptr,
		Buffers.PositionVertexBuffer,
		Buffers.StaticMeshVertexBuffer,
		Buffers.PositionVertexBuffer,
		&Buffers.StaticMeshVertexBuffer,
		Repainted
	);

	if (Repainted.Num() != BuiltVertices) {
		UE_LOG(LogReflectionObjectSerializer, Warning, TEXT("\"%s\" LOD%d found colours for %d of the %d vertices the editor built, so the ones the game cooked stay as they are"), *StaticMesh->GetName(), LodIndex, Repainted.Num(), BuiltVertices);

		Repainted.Empty();

		return Repainted;
	}

	if (BuiltVertices != Painted.Num()) {
		UE_LOG(LogReflectionObjectSerializer, Display, TEXT("\"%s\" LOD%d was built with %d vertices where the game cooked %d, and its colours were placed by where they were painted"), *StaticMesh->GetName(), LodIndex, BuiltVertices, Painted.Num());
	}

	return Repainted;
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
			if (Property->NamePrivate == "FogInscatteringLuminance" && !Properties->HasField(TEXT("FogInscatteringLuminance"))) {
				PropertyName = "FogInscatteringColor";
			}

			if (Property->NamePrivate == "DirectionalInscatteringLuminance" && !Properties->HasField(TEXT("DirectionalInscatteringLuminance"))) {
				PropertyName = "DirectionalInscatteringColor";
			}
		}
#endif

		if (Properties->HasField(PropertyName) && !HasHandledProperty && PropertyName != "LODParentPrimitive" && PropertyName != "bIsCookedForEditor") {
			const TSharedPtr<FJsonValue>& ValueObject = Properties->Values.FindChecked(StringToJsonKey(PropertyName));

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

	/* Volumes keep their shape in a brush the cook strips, and it reads back out of the collision
	 * the editor built from it */
	if (UBrushComponent* BrushComponent = Cast<UBrushComponent>(Object)) {
		FVolumeBrush::Rebuild(BrushComponent, Properties, PropertySerializer);
	}

	if (ABrush* BrushActor = Cast<ABrush>(Object)) {
		FVolumeBrush::Rebuild(BrushActor);
	}

	/* What a foliage actor grows is not among its properties either */
	if (AInstancedFoliageActor* FoliageActor = Cast<AInstancedFoliageActor>(Object)) {
		FFoliageActor::Rebuild(FoliageActor, PropertySerializer);
	}

	/* This is a use case for importing maps and parsing static mesh components
	 * using the object and property serializer, this was initially wanted to be
	 * done completely without any manual work. (using the de-serializers)
	 * 
	 * However I don't think it's possible to do so. as I haven't seen any native
	 * property that can do this using the data provided in UEParse. */
	if (Properties->HasField(TEXT("LODData")) && Cast<UStaticMeshComponent>(Object)) {
		UStaticMeshComponent* StaticMeshComponent = Cast<UStaticMeshComponent>(Object);
		if (!StaticMeshComponent) return;
		
		TArray<TSharedPtr<FJsonValue>> ObjectLODData = Properties->GetArrayField(TEXT("LODData"));
		int CurrentLOD = -1;

		int32 ColouredLods = 0;
		bool bPaintedEveryLod = true;
		
		for (const auto& CurrentLODValue : ObjectLODData) {
			CurrentLOD++;

			const TSharedPtr<FJsonObject> CurrentLODObject = CurrentLODValue->AsObject();

			/* Must contain vertex colors, or else it's an empty LOD */
			if (!CurrentLODObject->HasField(TEXT("OverrideVertexColors"))) continue;

			const TSharedPtr<FJsonObject> OverrideVertexColorsObject = CurrentLODObject->GetObjectField(TEXT("OverrideVertexColors"));

			if (!OverrideVertexColorsObject->HasField(TEXT("Data"))) continue;

			const int32 NumVertices = OverrideVertexColorsObject->GetIntegerField(TEXT("NumVertices"));
			const TArray<TSharedPtr<FJsonValue>> DataArray = OverrideVertexColorsObject->GetArrayField(TEXT("Data"));

			/* Where each colour was painted, which is the one thing that can place them again when the
			 * editor builds the mesh differently from the way the game cooked it */
			TArray<FPaintedVertex> Painted = BuildPaintedVertices(StaticMeshComponent->GetStaticMesh(), CurrentLOD, DataArray);

			ColouredLods++;
			bPaintedEveryLod &= Painted.Num() > 0;

			const TArray<FColor> Repainted = RepaintOverrideColors(StaticMeshComponent->GetStaticMesh(), CurrentLOD, Painted);

			/* Template of the target data, laid out the way the engine exports its own */
			FString Output = FString::Printf(TEXT("CustomLODData LOD=%d ColorVertexData(%d)=("), CurrentLOD, Repainted.Num() > 0 ? Repainted.Num() : NumVertices);

			/* Append the colors in the expected format */
			if (Repainted.Num() > 0) {
				for (int32 i = 0; i < Repainted.Num(); ++i) {
					const FColor& Color = Repainted[i];

					/* ARGB, the order the buffer reads them back in */
					Output.Append(FString::Printf(TEXT("%02X%02X%02X%02X"), Color.A, Color.R, Color.G, Color.B));

					if (i < Repainted.Num() - 1) {
						Output.Append(TEXT(","));
					}
				}
			} else {
				for (int32 i = 0; i < DataArray.Num(); ++i) {
					FString ColorValue = DataArray.operator[](i)->AsString();
					Output.Append(ColorValue);

					/* Add a comma unless it's the last element */
					if (i < DataArray.Num() - 1) {
						Output.Append(TEXT(","));
					}
				}
			}

			Output.Append(TEXT(")"));
		
			StaticMeshComponent->ImportCustomProperties(*Output, GWarn);

			/* Set after the import, which empties whatever it was holding on its way to the colours */
			if (StaticMeshComponent->LODData.IsValidIndex(CurrentLOD)) {
				StaticMeshComponent->LODData[CurrentLOD].PaintedVertices = MoveTemp(Painted);
			}
		}

		/* The game painted every LOD on its own, and the engine only keeps a LOD's own colours when
		 * it is told to. Left alone it derives the lower LODs from LOD0's paint instead. */
		if (ColouredLods > 1 && bPaintedEveryLod) {
			StaticMeshComponent->bCustomOverrideVertexColorPerLOD = true;
		}

	}
}

#if UE5_2_BEYOND
UE_ENABLE_OPTIMIZATION
#else
PRAGMA_ENABLE_OPTIMIZATION
#endif