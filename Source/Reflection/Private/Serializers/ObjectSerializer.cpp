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
#include "MovieSceneSection.h"
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
/* Split out into StaticMeshDescription in 4.24; before that the attributes are named by hand */
#if !UE4_23_BELOW
#include "StaticMeshAttributes.h"
#endif
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
	UObject* AlreadyThere(UObject* Outer, const FName Named, const UClass* Wanted) {
		if (Outer == nullptr || Named.IsNone()) return nullptr;

		UObject* Standing = StaticFindObjectFast(UObject::StaticClass(), Outer, Named);

		if (Standing == nullptr) return nullptr;

		/* There under that name, and not the thing the export is talking about. */
		if (Wanted != nullptr && !Standing->IsA(Wanted)) {
			MoveToTransientPackageAndRename(Standing);

			return nullptr;
		}

		return Standing;
	}

	bool CanBeMade(const UClass* Class, const FString& Named, const FString& Type) {
		if (Class == nullptr) return false;

		/* A class is found, never made. */
		if (Class->IsChildOf(UClass::StaticClass())) {
			UE_LOG(LogReflection, Error, TEXT("\"%s\" is written as a %s and this build has no class of that name, so nothing was made for it"), *Named, *Type);

			FImportIssues::Report(
				EImportIssue::MissingClass,
				FString::Printf(TEXT("\"%s\" could not be made"), *Named),
				FString::Printf(TEXT("The export says it is a %s and this build has no class of that name. What is left to go on names a class rather than an object, and a class is not something that can be made: it would come out with no parent and no default object and take a later compile down with it. Nothing was put in its place and the rest of the import carried on."), *Type)
			);

			return false;
		}

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

namespace {
	/* The exports part way through being made, innermost last. */
	TArray<FUObjectExport*> GBeingMade;

	/* Marks one as being made for as long as the call making it is running. */
	struct FSpawnScope {
		explicit FSpawnScope(FUObjectExport* InExport) : Export(InExport) { GBeingMade.Add(Export); }

		~FSpawnScope() { GBeingMade.Remove(Export); }

		FUObjectExport* Export;
	};

	const UClass* GetClassToMake(FUObjectExport* Export) {
#if ENGINE_UE5
		/* UE5's CommonUI has no UCommonWidgetSwitcher of its own, so the export resolves to no class at all and
		   the widget is never made. CommonUILegacy carries the original under its own name, and made as that one
		   it still behaves the way the asset was authored against. */
		if (Export->GetType() == TEXT("CommonWidgetSwitcher")) {
			if (const UClass* LegacyClass = FindClassByType(TEXT("CommonWidgetSwitcherLegacy"))) {
				return LegacyClass;
			}
		}
#endif

		return Export->GetClass();
	}

	FString GetObjectNameToMake(FUObjectExport* Export, const UClass* Class) {
		FString ObjectName = Export->GetName().ToString();

		if (Class->IsChildOf(UWidgetAnimation::StaticClass())) {
			ObjectName.Split("_INST", &ObjectName, nullptr, ESearchCase::CaseSensitive);
		}

		return ObjectName;
	}

	/* Made in the outer its whole path names, and only there.
	 *
	 * Nothing is read into it here, and nothing is said when it cannot be made: the call further up
	 * that is making it does both, and finding an object already there it only fills it in. */
	void MakeAheadOfOuter(FUObjectExportContainer* Container, FUObjectExport* Export) {
		const UClass* Class = GetClassToMake(Export);

		if (Container == nullptr || Class == nullptr) return;
		if (Class->IsChildOf(UClass::StaticClass()) || Class->HasAnyClassFlags(CLASS_Abstract)) return;

		const TArray<FName> TreeSegments = Export->GetOuterTreeSegments(true);

		if (TreeSegments.Num() == 0) return;

		const FUObjectExport* OuterExport = Container->FindByTreeSegment(TreeSegments);
		UObject* ObjectOuter = OuterExport->IsJsonValid() ? OuterExport->Object : nullptr;

		if (ObjectOuter == nullptr) return;
		if (Class->ClassWithin != nullptr && !ObjectOuter->IsA(Class->ClassWithin)) return;

		const FName Named = StringToName(GetObjectNameToMake(Export, Class));

		if (UObject* Standing = AlreadyThere(ObjectOuter, Named, Class)) {
			Export->Object = Standing;

			return;
		}

		EObjectFlags Flags = RF_Public | RF_Transactional;

		if (Export->Has("Flags")) {
			Flags = ParseObjectFlags(Export->GetString("Flags"));
		}

		Export->Object = NewObject<UObject>(ObjectOuter, ToNewObjectClass(Class), Named, Flags);
	}
}

UObject* UObjectSerializer::SpawnExport(FUObjectExport* Export, const bool bOnlySerialize) {
	if (Export == nullptr) return nullptr;

	/* Already being made further up the stack, so what there is of it is all there is to give.
	 *
	 * Unless there is nothing of it yet because it is still looking for its outer, and the outer it
	 * went to make is the one asking. A movie scene makes its sections before its tracks, a section
	 * makes the track it lives in, and the track's own list of sections names the one that made it:
	 * handed nothing, the track keeps nothing and the section is never part of it. The outer is
	 * there by now, so the object is made in it here and left for the call that started it to fill. */
	if (GBeingMade.Contains(Export)) {
		if (Export->Object == nullptr) {
			MakeAheadOfOuter(PropertySerializer->ExportsContainer, Export);
		}

		return Export->Object;
	}

	const FSpawnScope BeingMade(Export);

	if (!bOnlySerialize) {
		if (Export->Object != nullptr) return nullptr;
	}

	const UClass* Class = GetClassToMake(Export);

	if (!Class) return nullptr;

	const FString Outer = GetOuterFromObjectOuter(Export->JsonObject->TryGetField(TEXT("Outer")));
	UObject* ObjectOuter = nullptr;

	/* Find the outer using the tree segment
	 *
	 * The whole way down first, and the names below only when it misses. A name is not one thing:
	 * a widget's animations each keep a MovieScene2DTransformTrack_0, and asked by name the first
	 * of them answers for all. Looking was never harmless either, since what it finds it makes.
	 * One animation's section made another animation's track that way, which made that one's
	 * movie scene and then the animation above it from underneath, and the animation asking
	 * after its own movie scene, still part way made, was given nothing and kept nothing. */
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

	/* Find the outer */
	const bool bWalkedToOuter = ObjectOuter != nullptr;

	/* Never itself, which is what a name alone hands back when an object is called what its outer is */
	if (!bWalkedToOuter) {
		if (FUObjectExport* OuterExport = PropertySerializer->ExportsContainer->FindOther(FName(*Outer), Export); !Outer.IsEmpty() && OuterExport->JsonObject.IsValid()) {
			if (OuterExport->Object == nullptr) {
				SpawnExport(OuterExport);
			}

			ObjectOuter = OuterExport->Object;
		}
	}

	/* Find the blueprint outer */
	if (!bWalkedToOuter && !Outer.IsEmpty()) {
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

	if (!ObjectOuter && !Export->Object) return nullptr;

	/* Default flags */
	EObjectFlags Flags = RF_Public | RF_Transactional;

	/* Parse the flags back into EObjectFlags, important for component archetypes */
	if (Export->Has("Flags")) {
		Flags = ParseObjectFlags(Export->GetString("Flags"));
	}

	if (!Export->Object) {
		const FString ObjectName = GetObjectNameToMake(Export, Class);
		const FName Named = StringToName(ObjectName);

		/* Only made where its kind is allowed to live.
		 *
		 * An outer named as a generated class is taken back to the blueprint above, which is right for
		 * a widget tree and wrong for a function: a function lives in the class, and the engine calls
		 * one made anywhere else fatal rather than handing it back. That class is the compiler's to
		 * fill in, so a function the export names is left for it. */
		if (ObjectOuter != nullptr && Class->ClassWithin != nullptr && !ObjectOuter->IsA(Class->ClassWithin)) {
			/* A function of a blueprint is expected here and is no loss: the graph it was compiled from is rebuilt, and the
			 * compiler makes the function again from that, an event's stub included. Only anything else left out is worth
			 * saying. */
			if (Class->IsChildOf(UFunction::StaticClass()) && ObjectOuter->IsA<UBlueprint>()) {
				UE_LOG(LogReflection, Verbose, TEXT("\"%s\" is a function of %s, left for the compiler to make from the graph"), *ObjectName, *ObjectOuter->GetFullName());

				return nullptr;
			}

			UE_LOG(LogReflection, Error, TEXT("\"%s\" is a %s, which has to live in a %s, and its outer is %s, so nothing was made for it"), *ObjectName, *Class->GetName(), *Class->ClassWithin->GetName(), *ObjectOuter->GetFullName());

			FImportIssues::Report(
				EImportIssue::Data,
				FString::Printf(TEXT("\"%s\" could not be made"), *ObjectName),
				FString::Printf(TEXT("A %s can only be made inside a %s, and the outer this export resolved to is %s. Making it there would stop the editor, so nothing was put in its place and the rest of the import carried on."), *Class->GetName(), *Class->ClassWithin->GetName(), *ObjectOuter->GetFullName())
			);

			return nullptr;
		}

		if (UObject* Standing = AlreadyThere(ObjectOuter, Named, Class)) {
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

	UObject* NewUObject = AlreadyThere(ObjectOuter, FName(*Name), Class);

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

	/* Watched rather than trusted: reading one property can fetch, compile, and take this object apart underneath the loop */
	const TWeakObjectPtr<UObject> Alive(Object);

	for (FProperty* Property = ObjectClass->PropertyLink; Property; Property = Property->PropertyLinkNext) {
		if (!Property->IsValidLowLevel()) continue;
		
		FString PropertyName = Property->GetName();

		if (!PropertySerializer->ShouldDeserializeProperty(Property)) continue;

		/* Gone while something further in was being read, so there is nothing left to read into */
		if (!Alive.IsValid()) {
			UE_LOG(LogReflection, Warning, TEXT("\"%s\" was taken apart while its properties were being read, and the rest were left"), *ObjectClass->GetName());

			return;
		}

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
	
	{
		/* An effect written before the tags moved off it. */
		static const UClass* Effects = nullptr;

		if (Effects == nullptr) {
			Effects = FindObject<UClass>(nullptr, TEXT("/Script/GameplayAbilities.GameplayEffect"));
		}

		if (Effects != nullptr && Object != nullptr && Object->IsA(Effects) && Object->HasAnyFlags(RF_ClassDefaultObject)) {
			/* The version is left out of the reflected data, and nought predates every upgrade */
			const FStructProperty* Stamped = FindFProperty<FStructProperty>(Effects, TEXT("DataVersion"));

			if (Stamped != nullptr && Stamped->Struct != nullptr && Stamped->Struct->GetFName() == TEXT("GameplayEffectVersion")) {
				/* The upgrade empties a component whenever the old field it is filled from differs from the parent's, and moves nothing across an entry it nulled on an earlier pass.
				 * A field the reflected data does not carry is left as an earlier import saved it, so it takes the parent's value and the upgrade leaves the component alone */
				const UObject* Inherited = Object->GetArchetype();

				if (Inherited != nullptr && Inherited != Object && Inherited->IsA(Effects)) {
					static const TCHAR* Superseded[] = {
						TEXT("GrantedAbilities"),
						TEXT("InheritableGameplayEffectTags"),
						TEXT("InheritableOwnedTagsContainer"),
						TEXT("InheritableBlockedAbilityTagsContainer"),
						TEXT("OngoingTagRequirements"),
						TEXT("ApplicationTagRequirements"),
						TEXT("RemovalTagRequirements"),
						TEXT("RemoveGameplayEffectsWithTags"),
						TEXT("GrantedApplicationImmunityTags"),
						TEXT("GrantedApplicationImmunityQuery"),
						TEXT("RemoveGameplayEffectQuery")
					};

					for (const TCHAR* Field : Superseded) {
						const FProperty* Old = FindFProperty<FProperty>(Effects, Field);

						if (Old == nullptr || Properties->HasField(Field)) continue;

						Old->CopyCompleteValue_InContainer(Object, Inherited);
					}
				}

				*Stamped->ContainerPtrToValuePtr<uint8>(Object) = 0;

				/* A component reaches the effect it belongs to through its outer and nothing else */
				if (const FArrayProperty* Parts = FindFProperty<FArrayProperty>(Effects, TEXT("GEComponents"))) {
					if (const FObjectPropertyBase* Each = CastField<FObjectPropertyBase>(Parts->Inner)) {
						FScriptArrayHelper Held(Parts, Parts->ContainerPtrToValuePtr<void>(Object));

						for (int32 At = 0; At < Held.Num(); ++At) {
							UObject* Part = Each->GetObjectPropertyValue(Held.GetRawPtr(At));

							if (Part == nullptr || Part->GetTypedOuter(const_cast<UClass*>(Effects)) == Object) continue;

							UE_LOG(LogReflection, Display, TEXT("\"%s\" was kept outside the effect whose list it is in, and was moved under it"), *Part->GetName());

							Part->Rename(nullptr, Object, REN_DontCreateRedirectors | REN_NonTransactional);
						}
					}
				}

				/* Told the default object is settled, which is when the upgrade runs */
#if ENGINE_UE5
				/* Only where there is an effect this one was made from. */
				const UObject* Made = Object->GetArchetype();

				if (Made != nullptr && Made != Object && Made->IsA(Effects)) {
					FPostCDOCompiledContext Brought;

					Object->PostCDOCompiled(Brought);

					/* Loading stamps the current version once the upgrade has run, and nothing loads an imported effect before it is saved.
					 * Left at nought, every load upgrades it again; stamped, saving clears the old fields the components were filled from */
					Stamped->CopyCompleteValue_InContainer(Object, Effects->GetDefaultObject());
				} else {
					FImportIssues::Report(
						EImportIssue::Data,
						FString::Printf(TEXT("\"%s\" was not brought forward"), *Object->GetName()),
						TEXT("An effect keeps its tags in the old places and the engine moves them into components of their own, reading the effect this one was made from as it goes. That one is not settled yet, which happens where a reference opened this import partway through another. The tags are left where they were, so the effect grants none of them, and importing it on its own will bring them forward.")
					);
				}
#endif
			}
		}
	}

	{ /* PaperSprite handles */
		static const UClass* Sprites = FindObject<UClass>(nullptr, TEXT("/Script/Paper2D.PaperSprite"));

		if (Sprites != nullptr && Object != nullptr && Object->IsA(Sprites)) {
			UObject* Cut = Object;

			const FObjectPropertyBase* Sheet = FindFProperty<FObjectPropertyBase>(Sprites, TEXT("BakedSourceTexture"));
			const FSoftObjectProperty* From = FindFProperty<FSoftObjectProperty>(Sprites, TEXT("SourceTexture"));
			const FStructProperty* BakedAt = FindFProperty<FStructProperty>(Sprites, TEXT("BakedSourceUV"));
			const FStructProperty* At = FindFProperty<FStructProperty>(Sprites, TEXT("SourceUV"));
			const FStructProperty* BakedBig = FindFProperty<FStructProperty>(Sprites, TEXT("BakedSourceDimension"));
			const FStructProperty* Big = FindFProperty<FStructProperty>(Sprites, TEXT("SourceDimension"));

			const UScriptStruct* Flat = TBaseStructure<FVector2D>::Get();

			const bool bKnown = Sheet != nullptr && From != nullptr
				&& At != nullptr && BakedAt != nullptr && At->Struct == Flat && BakedAt->Struct == Flat
				&& Big != nullptr && BakedBig != nullptr && Big->Struct == Flat && BakedBig->Struct == Flat;

			if (bKnown) {
				UObject* Texture = Sheet->GetObjectPropertyValue_InContainer(Cut);

				/* Only where the question is missing: a build that kept it knows better */
				const bool bAsked = !From->GetPropertyValue_InContainer(Cut).IsNull();

				/* And only what the question is allowed to hold */
				const bool bFits = Texture != nullptr && From->PropertyClass != nullptr && Texture->IsA(From->PropertyClass);

				if (!bAsked && bFits) {
					From->SetPropertyValue_InContainer(Cut, FSoftObjectPtr(Texture));

					At->Struct->CopyScriptStruct(At->ContainerPtrToValuePtr<void>(Cut), BakedAt->ContainerPtrToValuePtr<void>(Cut));
					Big->Struct->CopyScriptStruct(Big->ContainerPtrToValuePtr<void>(Cut), BakedBig->ContainerPtrToValuePtr<void>(Cut));
				}
			}
		}
	}

	if (Cast<UStaticMeshComponent>(Object)
		|| Cast<UParticleSystem>(Object)
		|| Cast<UParticleLODLevel>(Object)
		|| Cast<UParticleModule>(Object)
		|| Cast<UParticleEmitter>(Object)
		/* A section keeps its keys in channels, and which channels it has is worked out from what
		 * it was just told rather than declared. A vector section made from nothing has none: it is
		 * constructed using no channels at all and builds however many it turns out to use when it
		 * is told, which is here. Left untold the keys are all in the object and the editor is
		 * looking at a section that says it has nowhere to keep any, so the track draws empty over
		 * a full set of them. */
		|| Cast<UMovieSceneSection>(Object)) {
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