/* Copyright Reflection Contributors 2024-2026 */

#include "Modules/Cloud/Tools/MeshGeometry.h"

#include "Engine/EngineUtilities.h"
#include "Utilities/JsonHelpers.h"

#include "GPUSkinPublicDefs.h"
#include "Modules/Cloud/Cloud.h"
#include "Rendering/SkeletalMeshModel.h"
#include "Rendering/SkeletalMeshLODModel.h"
#include "Rendering/SkeletalMeshLODImporterData.h"

/* FSkeletalMaterial moved out of SkeletalMesh.h in 5.1 */
#if UE5_1_BEYOND
#include "Engine/SkinnedAssetCommon.h"
#endif
#include "SkeletalMeshTypes.h"

/* Before 4.27 the LOD model is built here rather than by the mesh */
#if !UE4_27_AND_UE5
#include "MeshUtilities.h"
#endif

/* 4.25 put the reference skeleton and the colour flag behind accessors */
namespace {
#if UE4_25_BELOW
	const FReferenceSkeleton& MeshRefSkeleton(const USkeletalMesh* Mesh) { return Mesh->RefSkeleton; }
	void SetMeshHasVertexColors(USkeletalMesh* Mesh, const bool Value) { Mesh->bHasVertexColors = Value; }
	const TArray<FSkeletalMaterial>& MeshMaterials(const USkeletalMesh* Mesh) { return Mesh->Materials; }
#else
	const FReferenceSkeleton& MeshRefSkeleton(const USkeletalMesh* Mesh) { return Mesh->GetRefSkeleton(); }
	void SetMeshHasVertexColors(USkeletalMesh* Mesh, const bool Value) { Mesh->SetHasVertexColors(Value); }
	const TArray<FSkeletalMaterial>& MeshMaterials(const USkeletalMesh* Mesh) { return Mesh->GetMaterials(); }
#endif

	/* UE5 named the axis constants. The same vectors are spelled out here so both engines read the
	 * same way at the point they are used. */
	static const FVector3f GMeshUpAxis = FVector3f(0.0f, 0.0f, 1.0f);
	static const FVector3f GMeshForwardAxis = FVector3f(1.0f, 0.0f, 0.0f);
}

int32 TMeshGeometry::RebuildLodModels(USkeletalMesh* SkeletalMesh, const TSharedPtr<FJsonObject>& Response) {
	if (SkeletalMesh == nullptr || !Response.IsValid()) return 0;

	FSkeletalMeshModel* ImportedModel = SkeletalMesh->GetImportedModel();
	if (ImportedModel == nullptr) return 0;

	const TArray<TSharedPtr<FJsonValue>>* Lods;
	if (!Response->TryGetArrayField(TEXT("lods"), Lods)) return 0;

	const FReferenceSkeleton& RefSkeleton = MeshRefSkeleton(SkeletalMesh);

	int32 RebuiltLods = 0;

	{
	FScopedSkeletalMeshPostEditChange ScopedPostEditChange(SkeletalMesh);

	SkeletalMesh->Modify();

	for (const TSharedPtr<FJsonValue>& LodValue : *Lods) {
		const TSharedPtr<FJsonObject> Lod = LodValue.IsValid() ? LodValue->AsObject() : nullptr;
		if (!Lod.IsValid()) continue;

		const int32 LodIndex = Lod->GetIntegerField(TEXT("Index"));

		const TSharedPtr<FJsonObject>* Vertices;
		if (!Lod->TryGetObjectField(TEXT("Vertices"), Vertices)) continue;

		const TArray<TSharedPtr<FJsonValue>>* Indices;
		const TArray<TSharedPtr<FJsonValue>>* SectionsJson;

		if (!Lod->TryGetArrayField(TEXT("Indices"), Indices)) continue;
		if (!Lod->TryGetArrayField(TEXT("Sections"), SectionsJson)) continue;

		const int32 VertexCount = (*Vertices)->GetIntegerField(TEXT("Count"));
		const int32 BonesPerVertex = (*Vertices)->GetIntegerField(TEXT("BonesPerVertex"));
		/* Two counts, because they are not always the same number. The payload writes every set the
		 * mesh was cooked with, one after another per vertex, and that stride is what the UVs have
		 * to be read at; a wedge only holds four of them, which is what ends up on the mesh. Read
		 * at the wrong stride, every UV after the first vertex comes off the wrong set. */
		const int32 PayloadTexCoords = FMath::Max((*Vertices)->GetIntegerField(TEXT("NumTexCoords")), 1);
		const int32 NumTexCoords = FMath::Min(PayloadTexCoords, static_cast<int32>(MAX_TEXCOORDS));

		if (VertexCount == 0 || BonesPerVertex == 0) continue;

		const TArray<TSharedPtr<FJsonValue>>* Positions = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Normals = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Tangents = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Binormals = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* UVs = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Colors = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Bones = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Weights = nullptr;

		(*Vertices)->TryGetArrayField(TEXT("Positions"), Positions);
		(*Vertices)->TryGetArrayField(TEXT("Normals"), Normals);
		(*Vertices)->TryGetArrayField(TEXT("Binormals"), Binormals);
		(*Vertices)->TryGetArrayField(TEXT("Tangents"), Tangents);
		(*Vertices)->TryGetArrayField(TEXT("UVs"), UVs);
		(*Vertices)->TryGetArrayField(TEXT("Colors"), Colors);
		(*Vertices)->TryGetArrayField(TEXT("Bones"), Bones);
		(*Vertices)->TryGetArrayField(TEXT("Weights"), Weights);

		/* LODInfo grows with the models: the build asserts on a model without one, and a source
		 * model clears its mesh data against the model, so the model comes first. */
		while (ImportedModel->LODModels.Num() <= LodIndex) {
			ImportedModel->LODModels.Add(new FSkeletalMeshLODModel());
		}

		/* Newer engines keep a source model per LOD alongside the info, and saving imported data
		 * goes through it. Deserialized properties can fill in the infos alone, so the shorter of
		 * the two is what decides; AddLODInfo is the only thing that grows either. */
		while (SkeletalMesh->GetLODNum() <= LodIndex
#if UE5_4_BEYOND
			|| SkeletalMesh->GetNumSourceModels() <= LodIndex
#endif
			) {
			SkeletalMesh->AddLODInfo();
		}


		/* The build regenerates the LOD model out of this, so this is the layer worth writing.
		 * One point per cooked vertex and one wedge pointing at it, which keeps the splits the
		 * cooked mesh already had rather than letting the builder reintroduce its own. */
		FSkeletalMeshImportData ImportData;

		ImportData.NumTexCoords = NumTexCoords;
		ImportData.bHasNormals = true;
		ImportData.bHasTangents = true;
		ImportData.bHasVertexColors = false;

		ImportData.Points.Empty(VertexCount);
		ImportData.PointToRawMap.Empty(VertexCount);

		TArray<FVector3f> VertexNormals;
		TArray<FVector3f> VertexBinormals;
		TArray<FVector3f> VertexTangents;

		VertexNormals.Empty(VertexCount);
		VertexBinormals.Empty(VertexCount);
		VertexTangents.Empty(VertexCount);

		/* Which section owns each vertex, so a wedge can name the material it belongs to */
		TArray<int32> VertexSection;
		VertexSection.Init(0, VertexCount);

		for (int32 SectionIndex = 0; SectionIndex < SectionsJson->Num(); ++SectionIndex) {
			const TSharedPtr<FJsonObject> SectionJson = (*SectionsJson)[SectionIndex]->AsObject();
			if (!SectionJson.IsValid()) continue;

			const int32 BaseVertex = SectionJson->GetIntegerField(TEXT("BaseVertexIndex"));
			const int32 SectionVertices = SectionJson->GetIntegerField(TEXT("NumVertices"));

			for (int32 Local = 0; Local < SectionVertices; ++Local) {
				if (VertexSection.IsValidIndex(BaseVertex + Local)) {
					VertexSection[BaseVertex + Local] = SectionIndex;
				}
			}
		}

		TArray<FVector2f> VertexUVs;
		TArray<FColor> VertexColors;

		VertexUVs.Empty(VertexCount * NumTexCoords);
		VertexColors.Empty(VertexCount);

		for (int32 Vertex = 0; Vertex < VertexCount; ++Vertex) {
			ImportData.Points.Add(FVector3f(ReadFloat(Positions, Vertex * 3), ReadFloat(Positions, Vertex * 3 + 1), ReadFloat(Positions, Vertex * 3 + 2)));
			ImportData.PointToRawMap.Add(Vertex);

			VertexNormals.Add(FVector3f(ReadFloat(Normals, Vertex * 3), ReadFloat(Normals, Vertex * 3 + 1), ReadFloat(Normals, Vertex * 3 + 2)));

			if (Binormals != nullptr) {
				VertexBinormals.Add(FVector3f(ReadFloat(Binormals, Vertex * 3), ReadFloat(Binormals, Vertex * 3 + 1), ReadFloat(Binormals, Vertex * 3 + 2)));
			}
			VertexTangents.Add(FVector3f(ReadFloat(Tangents, Vertex * 3), ReadFloat(Tangents, Vertex * 3 + 1), ReadFloat(Tangents, Vertex * 3 + 2)));

			for (int32 UV = 0; UV < NumTexCoords; ++UV) {
				const int32 Offset = (Vertex * PayloadTexCoords + UV) * 2;

				VertexUVs.Add(FVector2f(ReadFloat(UVs, Offset), ReadFloat(UVs, Offset + 1)));
			}

			/* By component: FColor is BGRA in memory, and the wire order is RGBA */
			const uint32 Packed = ReadUInt(Colors, Vertex);

			const FColor Color((Packed >> 24) & 0xFF, (Packed >> 16) & 0xFF, (Packed >> 8) & 0xFF, Packed & 0xFF);

			VertexColors.Add(Color);

			if (Color != FColor::White) {
				ImportData.bHasVertexColors = true;
			}
		}

		/* One material per section, since a wedge names its section rather than a slot */
		ImportData.Materials.Empty(SectionsJson->Num());

		for (int32 SectionIndex = 0; SectionIndex < SectionsJson->Num(); ++SectionIndex) {
			const TSharedPtr<FJsonObject> SectionJson = (*SectionsJson)[SectionIndex]->AsObject();

			SkeletalMeshImportData::FMaterial& Material = ImportData.Materials.AddDefaulted_GetRef();

			const int32 MaterialIndex = SectionJson.IsValid() ? SectionJson->GetIntegerField(TEXT("MaterialIndex")) : 0;
			const TArray<FSkeletalMaterial>& Slots = MeshMaterials(SkeletalMesh);

			Material.MaterialImportName = Slots.IsValidIndex(MaterialIndex)
				? Slots[MaterialIndex].MaterialSlotName.ToString()
				: FString::Printf(TEXT("Material_%d"), MaterialIndex);

			/* What the build reads to decide which slot the section draws with, written out so a
			 * section that comes back wearing somebody else's material can be traced to whichever
			 * of the two it was: the slot the cook named, or the name the build matched it by. */
			UE_LOG(LogReflection, Display, TEXT("LOD %d section %d: cooked material %d of %d, matched by slot name \"%s\""),
				LodIndex, SectionIndex, MaterialIndex, Slots.Num(), *Material.MaterialImportName);
		}

		ImportData.MaxMaterialIndex = FMath::Max(ImportData.Materials.Num() - 1, 0);

		/* A wedge per face corner, not per vertex: the builder walks wedges through faces and
		 * expects three of them for every triangle. Faces come off the section owning their index
		 * range, so a triangle keeps the material it was cooked with. */
		ImportData.Faces.Empty(Indices->Num() / 3);
		ImportData.Wedges.Empty(Indices->Num());

		for (int32 SectionIndex = 0; SectionIndex < SectionsJson->Num(); ++SectionIndex) {
			const TSharedPtr<FJsonObject> SectionJson = (*SectionsJson)[SectionIndex]->AsObject();
			if (!SectionJson.IsValid()) continue;

			const int32 BaseIndex = SectionJson->GetIntegerField(TEXT("BaseIndex"));
			const int32 NumTriangles = SectionJson->GetIntegerField(TEXT("NumTriangles"));

			for (int32 Triangle = 0; Triangle < NumTriangles; ++Triangle) {
				SkeletalMeshImportData::FTriangle& Face = ImportData.Faces.AddDefaulted_GetRef();

				Face.MatIndex = static_cast<uint16>(SectionIndex);
				Face.AuxMatIndex = 0;
				Face.SmoothingGroups = 1;

				for (int32 Corner = 0; Corner < 3; ++Corner) {
					const int32 Vertex = ReadInt(Indices, BaseIndex + Triangle * 3 + Corner);

					SkeletalMeshImportData::FVertex& Wedge = ImportData.Wedges.AddDefaulted_GetRef();

					Wedge.VertexIndex = Vertex;
					Wedge.MatIndex = static_cast<uint8>(SectionIndex);
					Wedge.Color = VertexColors.IsValidIndex(Vertex) ? VertexColors[Vertex] : FColor::White;

					for (int32 UV = 0; UV < NumTexCoords; ++UV) {
						const int32 Offset = Vertex * NumTexCoords + UV;

						Wedge.UVs[UV] = VertexUVs.IsValidIndex(Offset) ? VertexUVs[Offset] : FVector2f::ZeroVector;
					}

					Face.WedgeIndex[Corner] = ImportData.Wedges.Num() - 1;

					Face.TangentZ[Corner] = VertexNormals.IsValidIndex(Vertex) ? VertexNormals[Vertex] : GMeshUpAxis;
					Face.TangentX[Corner] = VertexTangents.IsValidIndex(Vertex) ? VertexTangents[Vertex] : GMeshForwardAxis;
					/* The game's own binormal, not one worked back out of the other two: the cross
					 * product is always right handed, and a UV shell mirrored against its
					 * neighbour is lit by the sign that says it isn't. */
					Face.TangentY[Corner] = VertexBinormals.IsValidIndex(Vertex)
						? VertexBinormals[Vertex]
						: FVector3f::CrossProduct(Face.TangentZ[Corner], Face.TangentX[Corner]);
				}
			}
		}

		/* Influences are per point, and a bone id indexes the owning section's bone map */
		ImportData.Influences.Empty(VertexCount * BonesPerVertex);

		TArray<TArray<int32>> SectionBoneMaps;
		SectionBoneMaps.SetNum(FMath::Max(SectionsJson->Num(), 1));

		for (int32 SectionIndex = 0; SectionIndex < SectionsJson->Num(); ++SectionIndex) {
			const TSharedPtr<FJsonObject> SectionJson = (*SectionsJson)[SectionIndex]->AsObject();
			if (!SectionJson.IsValid()) continue;

			const TArray<TSharedPtr<FJsonValue>>* BoneMap = nullptr;
			if (!SectionJson->TryGetArrayField(TEXT("BoneMap"), BoneMap)) continue;

			for (const TSharedPtr<FJsonValue>& BoneName : *BoneMap) {
				SectionBoneMaps[SectionIndex].Add(RefSkeleton.FindBoneIndex(FName(*BoneName->AsString())));
			}
		}

		for (int32 Vertex = 0; Vertex < VertexCount; ++Vertex) {
			const TArray<int32>& BoneMap = SectionBoneMaps[VertexSection.IsValidIndex(Vertex) ? VertexSection[Vertex] : 0];

			for (int32 Influence = 0; Influence < BonesPerVertex; ++Influence) {
				const int32 Offset = Vertex * BonesPerVertex + Influence;

				const float Weight = ReadFloat(Weights, Offset);
				if (Weight <= 0.0f) continue;

				const int32 BoneMapIndex = ReadInt(Bones, Offset);
				if (!BoneMap.IsValidIndex(BoneMapIndex) || BoneMap[BoneMapIndex] == INDEX_NONE) continue;

				SkeletalMeshImportData::FRawBoneInfluence& RawInfluence = ImportData.Influences.AddDefaulted_GetRef();

				RawInfluence.VertexIndex = Vertex;
				RawInfluence.BoneIndex = BoneMap[BoneMapIndex];
				RawInfluence.Weight = Weight;
			}
		}


#if !UE4_27_AND_UE5
		/* Before 4.27 nothing builds a LOD out of the mesh's imported data, so the model is built
		 * here the way an importer of that era does it: the same points, wedges, faces and
		 * influences, handed straight to the mesh builder. The imported data is kept alongside so
		 * anything that later asks the LOD what it was made from still has an answer. */
		ImportedModel->LODModels[LodIndex].RawSkeletalMeshBulkData.SaveRawMesh(ImportData);

		TArray<FVector> LodPoints;
		TArray<SkeletalMeshImportData::FMeshWedge> LodWedges;
		TArray<SkeletalMeshImportData::FMeshFace> LodFaces;
		TArray<SkeletalMeshImportData::FVertInfluence> LodInfluences;
		TArray<int32> LodPointToRawMap;

		ImportData.CopyLODImportData(LodPoints, LodWedges, LodFaces, LodInfluences, LodPointToRawMap);

		IMeshUtilities& MeshUtilities = FModuleManager::Get().LoadModuleChecked<IMeshUtilities>("MeshUtilities");

		IMeshUtilities::MeshBuildOptions BuildOptions;

		/* The normals, tangents and triangles are the game's own, the same reasoning as the build
		 * settings the newer path writes */
		BuildOptions.bComputeNormals = false;
		BuildOptions.bComputeTangents = false;
		BuildOptions.bRemoveDegenerateTriangles = false;

		if (!MeshUtilities.BuildSkeletalMesh(ImportedModel->LODModels[LodIndex], RefSkeleton, LodInfluences, LodWedges, LodFaces, LodPoints, LodPointToRawMap, BuildOptions)) {
			continue;
		}

		/* The builder leaves this to whoever called it, and the render data is sized from it. At
		 * zero the vertex buffer holds no texture coordinates, and the shader that goes looking for
		 * them is handed a stream that isn't there. */
		ImportedModel->LODModels[LodIndex].NumTexCoords = FMath::Max(1, NumTexCoords);
#else
		SkeletalMesh->SaveLODImportedData(LodIndex, ImportData);

		/* What says the imported data is new enough for the builder to read.
		 *
		 * A build asks the LOD whether its source data was written by an importer that knows the
		 * current format, and an asset old enough to predate the question answers no and is left
		 * alone. Nothing here fills that in, so the versions are stamped by hand: without them the
		 * build walks past the LOD without a word and hands back an empty mesh. */
		SkeletalMesh->SetLODImportedDataVersions(
			LodIndex,
			ESkeletalMeshGeoImportVersions::LatestVersion,
			ESkeletalMeshSkinningImportVersions::LatestVersion
		);

		/* BuildLODModel skips a LOD it thinks was generated from a lower one, and an importer that
		 * set this leaves the new source data unread. Cleared here, the same way a real build
		 * clears it before running. */
		if (FSkeletalMeshLODInfo* LodInfo = SkeletalMesh->GetLODInfo(LodIndex)) {
			LodInfo->bHasBeenSimplified = false;
			LodInfo->ReductionSettings.NumOfTrianglesPercentage = 1.0f;
			LodInfo->ReductionSettings.NumOfVertPercentage = 1.0f;

			/* The normals and tangents above are the game's own, per corner. Left on, the build
			 * throws them away and works its own out of the triangles, which is not the shading
			 * the mesh was authored with. */
			LodInfo->BuildSettings.bRecomputeNormals = false;
			LodInfo->BuildSettings.bRecomputeTangents = false;

			/* The triangles are the game's too. Dropping the degenerate ones would renumber what
			 * is left, and the morph deltas below name their vertices by that numbering. */
			LodInfo->BuildSettings.bRemoveDegenerates = false;
		}

#endif

		if (ImportData.bHasVertexColors) {
			SetMeshHasVertexColors(SkeletalMesh, true);
		}

		RebuiltLods++;
	}

	/* AddLODInfo appends, so bringing the source models up can leave an info the geometry has no
	 * LOD for. Those are dropped rather than left as an empty LOD on the mesh. */
	while (SkeletalMesh->GetLODNum() > Lods->Num()) {
		SkeletalMesh->RemoveLODInfo(SkeletalMesh->GetLODNum() - 1);
	}

	/* Otherwise the build hands back the geometry it cached against the old source data */
	if (RebuiltLods > 0) {
		SkeletalMesh->InvalidateDeriveDataCacheGUID();
	}

	SkeletalMesh->MarkPackageDirty();
	}

	return RebuiltLods;
}

void TMeshGeometry::Process(UObject* Object, const TArray<TSharedPtr<FJsonValue>>& Exports) {
	USkeletalMesh* SkeletalMesh = Cast<USkeletalMesh>(Object);
	if (SkeletalMesh == nullptr) return;

	const TSharedPtr<FJsonObject> Response = Cloud::Export::GetLodModelBlocking(SkeletalMesh->GetPathName());
	if (!Response.IsValid()) return;

	const int32 RebuiltLods = RebuildLodModels(SkeletalMesh, Response);

	BrowseToWhenFinished(SkeletalMesh);

	AppendNotification(
		FText::FromString("Reflected Mesh Geometry: " + SkeletalMesh->GetName()),
		FText::FromString(FString::Printf(TEXT("%d LOD(s) rebuilt"), RebuiltLods)),
		3.5f,
		FAppStyle::Get().GetBrush("PhysicsAssetEditor.EnableCollision.Small"),
		RebuiltLods > 0 ? SNotificationItem::CS_Success : SNotificationItem::CS_Fail,
		false,
		310.0f
	);
}
