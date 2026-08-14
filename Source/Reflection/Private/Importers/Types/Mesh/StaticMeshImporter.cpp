/* Copyright Reflection Contributors 2024-2026 */

#include "Importers/Types/Mesh/StaticMeshImporter.h"

#include "Engine/EngineUtilities.h"
#include "Utilities/JsonHelpers.h"
#include "Modules/Cloud/Cloud.h"

#include "Engine/StaticMesh.h"
#include "MeshDescription.h"
#include "StaticMeshAttributes.h"
#include "Engine/StaticMeshSocket.h"
#include "PhysicsEngine/BodySetup.h"

/* Spelled on the attribute set from UE5, and on the ref before that */
inline void SetStaticMeshUVChannelCount(FMeshDescription& MeshDescription, const int32 Count) {
#if ENGINE_UE5
	MeshDescription.VertexInstanceAttributes().SetAttributeChannelCount(MeshAttribute::VertexInstance::TextureCoordinate, Count);
#else
	MeshDescription.VertexInstanceAttributes().GetAttributesRef<FVector2D>(MeshAttribute::VertexInstance::TextureCoordinate).SetNumIndices(Count);
#endif
}

UObject* IStaticMeshImporter::CreateAsset(UObject* CreatedAsset) {
	return IImporter::CreateAsset(NewObject<UStaticMesh>(GetPackage(), UStaticMesh::StaticClass(), *GetAssetName(), RF_Public | RF_Standalone));
}

bool IStaticMeshImporter::Import() {
	UStaticMesh* StaticMesh = Create<UStaticMesh>();
	if (StaticMesh == nullptr) return false;

	/* Geometry is not in the export, so it comes off the reflected path */
	FString FetchPath = GetPackage()->GetPathName(); {
		FRRedirects::Reverse(FetchPath);
	}

	const TSharedPtr<FJsonObject> Geometry = Cloud::Export::GetStaticMeshBlocking(FetchPath);

	if (!Geometry.IsValid()) {
		FImportIssues::Report(EImportIssue::Failed, TEXT("No geometry from the Cloud"), TEXT("The Cloud has to be running, and the mesh needs cooked render data to read."));

		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* Slots;

	/* Before the LODs: a polygon group names its slot */
	if (Geometry->TryGetArrayField(TEXT("slots"), Slots)) {
		BuildMaterialSlots(StaticMesh, *Slots);
	}

	const TArray<TSharedPtr<FJsonValue>>* Lods;
	if (!Geometry->TryGetArrayField(TEXT("lods"), Lods)) return false;

	int32 BuiltLods = 0;

	for (const TSharedPtr<FJsonValue>& LodValue : *Lods) {
		const FUObjectJsonValueExport Lod = LodValue;
		if (!Lod.JsonObject.IsValid()) continue;

		if (BuildLod(StaticMesh, Lod, BuiltLods)) {
			BuiltLods++;

			continue;
		}

		FImportIssues::Report(EImportIssue::Data, FString::Printf(TEXT("LOD%d has no usable cooked geometry"), Lod.GetInteger(TEXT("Index"), 0)), TEXT("Its buffers were stripped in the cook, so the remaining LODs move up to fill the gap."));
	}

	if (BuiltLods == 0) return false;

	/* The build asserts on a mesh whose first LOD has no description */
	if (!StaticMesh->IsMeshDescriptionValid(0)) {
		FImportIssues::Report(EImportIssue::Data, TEXT("No geometry for LOD 0"), TEXT("The mesh build requires it. A Nanite only mesh keeps its geometry in the Nanite stream, which is the usual reason."));

		return false;
	}

	/* Stops the build recomputing over the cooked screen sizes */
	StaticMesh->bAutoComputeLODScreenSize = false;

	BuildCollisionAndSockets(StaticMesh);

	GetObjectSerializer()->DeserializeObjectProperties(RemovePropertiesShared(GetAssetData(), {
		"RenderData",
		"LODGroup",
		"BodySetup",
		"NavCollision",
		"ThumbnailInfo",

		/* Built from the exports below, so the references here would only be null */
		"Sockets",

	}), StaticMesh);

	/* Dropped to the first LOD worth showing. A cook can strip the colours from LOD 0, and landing
	 * there renders the mesh white. */
#if ENGINE_UE5
	if (GetSettings()->AssetSettings.StaticMesh.IgnoreMinQualityLevelLODDefault) {
		FPerQualityLevelInt MinQualityLevelLOD = StaticMesh->GetQualityLevelMinLOD();

		MinQualityLevelLOD.Default = FMath::Max(FirstColoredLod, 0);

		StaticMesh->SetQualityLevelMinLOD(MinQualityLevelLOD);
	}
#endif

	return OnAssetCreation(StaticMesh);
}

void IStaticMeshImporter::BuildMaterialSlots(UStaticMesh* StaticMesh, const TArray<TSharedPtr<FJsonValue>>& Slots) {
	TArray<FStaticMaterial> Materials;

	for (const TSharedPtr<FJsonValue>& SlotValue : Slots) {
		const TSharedPtr<FJsonObject> Slot = SlotValue.IsValid() ? SlotValue->AsObject() : nullptr;
		if (!Slot.IsValid()) continue;

		FStaticMaterial& Material = Materials.AddDefaulted_GetRef();

		Material.MaterialSlotName = FName(*Slot->GetStringField(TEXT("SlotName")));
		Material.ImportedMaterialSlotName = FName(*Slot->GetStringField(TEXT("ImportedSlotName")));

		/* The materials arrive through the export's StaticMaterials. Slots and order only. */
		Material.MaterialInterface = nullptr;
	}

	if (Materials.Num() > 0) {
		GetStaticMaterials(StaticMesh) = Materials;
	}
}

void IStaticMeshImporter::BuildCollisionAndSockets(UStaticMesh* StaticMesh) {
	DeserializeExports(StaticMesh);

	FUObjectExportContainer* Container = GetContainer();
	if (Container == nullptr) return;

	StaticMesh->Sockets.Empty();

	for (const FUObjectExport* Export : Container->Exports) {
		if (UStaticMeshSocket* Socket = Cast<UStaticMeshSocket>(Export->Object)) {
			StaticMesh->AddSocket(Socket);
		}
	}

	/* The object serializer skips BodySetup, so it is done by hand */
	FUObjectExport* BodyExport = Container->FindByType(FString(TEXT("BodySetup")));
	if (!BodyExport->IsJsonValid()) return;

	if (GetBodySetup(StaticMesh) == nullptr) {
		StaticMesh->CreateBodySetup();
	}

	UBodySetup* BodySetup = GetBodySetup(StaticMesh);
	if (BodySetup == nullptr) return;

	BodySetup->AggGeom.EmptyElements();
	BodySetup->CollisionTraceFlag = CTF_UseDefault;

	GetObjectSerializer()->DeserializeObjectProperties(BodyExport->GetProperties(), BodySetup);

	BodySetup->PostEditChange();
	BodySetup->InvalidatePhysicsData();
	BodySetup->CreatePhysicsMeshes();
}

bool IStaticMeshImporter::BuildLod(UStaticMesh* StaticMesh, const FUObjectJsonValueExport& Lod, const int32 LodIndex) {
	const FUObjectJsonValueExport Vertices = Lod.GetObject(TEXT("Vertices"));

	const TArray<uint32> Indices = Lod.GetUnsigneds(TEXT("Indices"));
	if (Indices.Num() == 0 || !Lod.Has(TEXT("Sections"))) return false;

	const TArray<FUObjectJsonValueExport> Sections = Lod.GetArray(TEXT("Sections"));

	const int32 VertexCount = Vertices.GetInteger(TEXT("Count"), 0);
	const int32 NumTexCoords = FMath::Max(Vertices.GetInteger(TEXT("NumTexCoords"), 1), 1);

	if (VertexCount == 0) return false;

	const TArray<float> Positions = Vertices.GetFloats(TEXT("Positions"));
	const TArray<float> Normals = Vertices.GetFloats(TEXT("Normals"));
	const TArray<float> Tangents = Vertices.GetFloats(TEXT("Tangents"));
	const TArray<float> Signs = Vertices.GetFloats(TEXT("Signs"));
	const TArray<float> UVs = Vertices.GetFloats(TEXT("UVs"));
	const TArray<uint32> Colors = Vertices.GetUnsigneds(TEXT("Colors"));

	if (Colors.Num() > 0 && FirstColoredLod == INDEX_NONE) {
		FirstColoredLod = LodIndex;
	}

	while (StaticMesh->GetNumSourceModels() <= LodIndex) {
		StaticMesh->AddSourceModel();
	}

	FMeshDescription* MeshDescription = StaticMesh->CreateMeshDescription(LodIndex);
	if (MeshDescription == nullptr) return false;

	MeshDescription->Empty();

	FStaticMeshAttributes Attributes(*MeshDescription);
	Attributes.Register();

	/* On the attribute set, not a temporary ref, and after Register defaults it to one */
	SetStaticMeshUVChannelCount(*MeshDescription, NumTexCoords);

	TVertexAttributesRef<FVector3f> VertexPositions = Attributes.GetVertexPositions();
	TVertexInstanceAttributesRef<FVector3f> InstanceNormals = Attributes.GetVertexInstanceNormals();
	TVertexInstanceAttributesRef<FVector3f> InstanceTangents = Attributes.GetVertexInstanceTangents();
	TVertexInstanceAttributesRef<float> InstanceSigns = Attributes.GetVertexInstanceBinormalSigns();
	TVertexInstanceAttributesRef<FVector4f> InstanceColors = Attributes.GetVertexInstanceColors();
	TVertexInstanceAttributesRef<FVector2f> InstanceUVs = Attributes.GetVertexInstanceUVs();
	TPolygonGroupAttributesRef<FName> GroupSlotNames = Attributes.GetPolygonGroupMaterialSlotNames();

	/* The game already split what needed splitting, so nothing here merges it back */
	MeshDescription->ReserveNewVertices(VertexCount);

	TArray<FVertexID> VertexIDs;
	VertexIDs.Reserve(VertexCount);

	for (int32 Vertex = 0; Vertex < VertexCount; ++Vertex) {
		const FVertexID VertexID = MeshDescription->CreateVertex();

		VertexPositions[VertexID] = FVector3f(ReadFloat(Positions, Vertex * 3), ReadFloat(Positions, Vertex * 3 + 1), ReadFloat(Positions, Vertex * 3 + 2));

		VertexIDs.Add(VertexID);
	}

	const TArray<FStaticMaterial>& MaterialSlots = GetStaticMaterials(StaticMesh);

	int32 BuiltTriangles = 0;

	for (const FUObjectJsonValueExport& Section : Sections) {
		const int32 MaterialIndex = Section.GetInteger(TEXT("MaterialIndex"), 0);
		const int32 FirstIndex = Section.GetInteger(TEXT("FirstIndex"), 0);
		const int32 NumTriangles = Section.GetInteger(TEXT("NumTriangles"), 0);

			/* A polygon group per section, named for its slot */
		const FPolygonGroupID GroupID = MeshDescription->CreatePolygonGroup();

		GroupSlotNames[GroupID] = MaterialSlots.IsValidIndex(MaterialIndex)
			? MaterialSlots[MaterialIndex].MaterialSlotName
			: FName(*FString::Printf(TEXT("Material_%d"), MaterialIndex));

		for (int32 Triangle = 0; Triangle < NumTriangles; ++Triangle) {
			/* Checked first: a half built triangle orphans vertex instances */
			int32 Corners3[3] = { INDEX_NONE, INDEX_NONE, INDEX_NONE };
			bool bUsable = true;

			for (int32 Corner = 0; Corner < 3; ++Corner) {
				Corners3[Corner] = ReadInt(Indices, FirstIndex + Triangle * 3 + Corner);

				if (!VertexIDs.IsValidIndex(Corners3[Corner])) {
					bUsable = false;
				}
			}

			/* A triangle naming one vertex twice has no area and cannot be built */
			if (Corners3[0] == Corners3[1] || Corners3[1] == Corners3[2] || Corners3[0] == Corners3[2]) {
				bUsable = false;
			}

			if (!bUsable) continue;

			TArray<FVertexInstanceID> Corners;
			Corners.Reserve(3);

			for (int32 Corner = 0; Corner < 3; ++Corner) {
				const int32 Vertex = Corners3[Corner];

				const FVertexInstanceID InstanceID = MeshDescription->CreateVertexInstance(VertexIDs[Vertex]);

				InstanceNormals[InstanceID] = FVector3f(ReadFloat(Normals, Vertex * 3), ReadFloat(Normals, Vertex * 3 + 1), ReadFloat(Normals, Vertex * 3 + 2));
				InstanceTangents[InstanceID] = FVector3f(ReadFloat(Tangents, Vertex * 3), ReadFloat(Tangents, Vertex * 3 + 1), ReadFloat(Tangents, Vertex * 3 + 2));
				InstanceSigns[InstanceID] = Signs.Num() > 0 ? ReadFloat(Signs, Vertex) : 1.0f;

				for (int32 UV = 0; UV < NumTexCoords; ++UV) {
					const int32 Offset = (Vertex * NumTexCoords + UV) * 2;

					InstanceUVs.Set(InstanceID, UV, FVector2f(ReadFloat(UVs, Offset), ReadFloat(UVs, Offset + 1)));
				}

				/* By component: FColor is BGRA in memory, and the wire order is RGBA */
				const uint32 Packed = ReadUInt(Colors, Vertex);

				const FColor Color((Packed >> 24) & 0xFF, (Packed >> 16) & 0xFF, (Packed >> 8) & 0xFF, Packed & 0xFF);

				InstanceColors[InstanceID] = FVector4f(FLinearColor(Color));

				Corners.Add(InstanceID);
			}

			if (Corners.Num() != 3) continue;

			MeshDescription->CreatePolygon(GroupID, Corners);

			BuiltTriangles++;
		}
	}

	if (BuiltTriangles == 0) return false;

	FStaticMeshSourceModel& SourceModel = StaticMesh->GetSourceModel(LodIndex);

	SourceModel.BuildSettings.bRecomputeNormals = false;
	SourceModel.BuildSettings.bRecomputeTangents = false;
	SourceModel.BuildSettings.bRemoveDegenerates = false;
	SourceModel.BuildSettings.bUseMikkTSpace = false;

	/* On by default, and it writes over DstLightmapIndex, which is the game's second UV set */
	SourceModel.BuildSettings.bGenerateLightmapUVs = false;

	/* Section flags and screen sizes, keyed by the LOD's position in the export */
	const int32 SourceLod = Lod.GetInteger(TEXT("Index"), 0);

	const FUObjectJsonValueExport RenderData = FUObjectJsonValueExport(GetAssetExport()).GetObject(TEXT("RenderData"));
	const TArray<FUObjectJsonValueExport> ExportLods = RenderData.Has(TEXT("LODs")) ? RenderData.GetArray(TEXT("LODs")) : TArray<FUObjectJsonValueExport>();

	if (ExportLods.IsValidIndex(SourceLod) && ExportLods[SourceLod].Has(TEXT("Sections"))) {
		const TArray<FUObjectJsonValueExport> ExportSections = ExportLods[SourceLod].GetArray(TEXT("Sections"));

		for (int32 SectionIndex = 0; SectionIndex < ExportSections.Num(); ++SectionIndex) {
			FMeshSectionInfo Info = StaticMesh->GetSectionInfoMap().Get(LodIndex, SectionIndex);

			GetPropertySerializer()->DeserializeStruct(FMeshSectionInfo::StaticStruct(), ExportSections[SectionIndex].JsonObject.ToSharedRef(), &Info);

			StaticMesh->GetSectionInfoMap().Set(LodIndex, SectionIndex, Info);
		}
	}

	const float ScreenSize = ReadFloat(RenderData.GetFloats(TEXT("ScreenSize")), SourceLod);

	if (ScreenSize > 0.0f) {
		SourceModel.ScreenSize.Default = ScreenSize;
	}

	StaticMesh->CommitMeshDescription(LodIndex);

	return true;
}
