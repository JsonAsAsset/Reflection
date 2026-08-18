/* Copyright Reflection Contributors 2024-2026 */

#include "Modules/Cloud/Tools/MeshGeometry.h"

#include "Engine/EngineUtilities.h"
#include "Utilities/JsonHelpers.h"

#if UE4_27_AND_UE5
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

#endif

int32 TMeshGeometry::RebuildLodModels(USkeletalMesh* SkeletalMesh, const TSharedPtr<FJsonObject>& Response) {
#if UE4_27_AND_UE5
	if (SkeletalMesh == nullptr || !Response.IsValid()) return 0;

	FSkeletalMeshModel* ImportedModel = SkeletalMesh->GetImportedModel();
	if (ImportedModel == nullptr) return 0;

	const TArray<TSharedPtr<FJsonValue>>* Lods;
	if (!Response->TryGetArrayField(TEXT("lods"), Lods)) return 0;

	const FReferenceSkeleton& RefSkeleton = SkeletalMesh->GetRefSkeleton();

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
		const int32 NumTexCoords = FMath::Clamp((*Vertices)->GetIntegerField(TEXT("NumTexCoords")), 1, static_cast<int32>(MAX_TEXCOORDS));

		if (VertexCount == 0 || BonesPerVertex == 0) continue;

		const TArray<TSharedPtr<FJsonValue>>* Positions = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Normals = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Tangents = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* UVs = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Colors = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Bones = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Weights = nullptr;

		(*Vertices)->TryGetArrayField(TEXT("Positions"), Positions);
		(*Vertices)->TryGetArrayField(TEXT("Normals"), Normals);
		(*Vertices)->TryGetArrayField(TEXT("Tangents"), Tangents);
		(*Vertices)->TryGetArrayField(TEXT("UVs"), UVs);
		(*Vertices)->TryGetArrayField(TEXT("Colors"), Colors);
		(*Vertices)->TryGetArrayField(TEXT("Bones"), Bones);
		(*Vertices)->TryGetArrayField(TEXT("Weights"), Weights);

		/* LODInfo grows with the models: the build asserts on a model without one */
		while (ImportedModel->LODModels.Num() <= LodIndex) {
			ImportedModel->LODModels.Add(new FSkeletalMeshLODModel());
		}

		while (SkeletalMesh->GetLODNum() <= LodIndex) {
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
		TArray<FVector3f> VertexTangents;

		VertexNormals.Empty(VertexCount);
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
			VertexTangents.Add(FVector3f(ReadFloat(Tangents, Vertex * 3), ReadFloat(Tangents, Vertex * 3 + 1), ReadFloat(Tangents, Vertex * 3 + 2)));

			for (int32 UV = 0; UV < NumTexCoords; ++UV) {
				const int32 Offset = (Vertex * NumTexCoords + UV) * 2;

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
			const TArray<FSkeletalMaterial>& Slots = SkeletalMesh->GetMaterials();

			Material.MaterialImportName = Slots.IsValidIndex(MaterialIndex)
				? Slots[MaterialIndex].MaterialSlotName.ToString()
				: FString::Printf(TEXT("Material_%d"), MaterialIndex);
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

					Face.TangentZ[Corner] = VertexNormals.IsValidIndex(Vertex) ? VertexNormals[Vertex] : FVector3f::ZAxisVector;
					Face.TangentX[Corner] = VertexTangents.IsValidIndex(Vertex) ? VertexTangents[Vertex] : FVector3f::XAxisVector;
					Face.TangentY[Corner] = FVector3f::CrossProduct(Face.TangentZ[Corner], Face.TangentX[Corner]);
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


		SkeletalMesh->SaveLODImportedData(LodIndex, ImportData);

		/* BuildLODModel skips a LOD it thinks was generated from a lower one, and an importer that
		 * set this leaves the new source data unread. Cleared here, the same way a real build
		 * clears it before running. */
		if (FSkeletalMeshLODInfo* LodInfo = SkeletalMesh->GetLODInfo(LodIndex)) {
			LodInfo->bHasBeenSimplified = false;
			LodInfo->ReductionSettings.NumOfTrianglesPercentage = 1.0f;
			LodInfo->ReductionSettings.NumOfVertPercentage = 1.0f;
		}

		if (ImportData.bHasVertexColors) {
			SkeletalMesh->SetHasVertexColors(true);
		}

		RebuiltLods++;
	}

	/* Otherwise the build hands back the geometry it cached against the old source data */
	if (RebuiltLods > 0) {
		SkeletalMesh->InvalidateDeriveDataCacheGUID();
	}

	SkeletalMesh->MarkPackageDirty();
	}

	return RebuiltLods;
#else
	return 0;
#endif
}

void TMeshGeometry::Process(UObject* Object, const TArray<TSharedPtr<FJsonValue>>& Exports) {
#if UE4_27_AND_UE5
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
#endif
}
