/* Copyright Reflection Contributors 2024-2026 */

#include "Serializers/VolumeBrush.h"

#include "BSPOps.h"
#include "Builders/CubeBuilder.h"
#include "Components/BrushComponent.h"
#include "Engine/Brush.h"
#include "Engine/Polys.h"
#include "Model.h"
#include "PhysicsEngine/BodySetup.h"
#include "Containers/ExportContainer.h"
#include "Serializers/PropertySerializer.h"

DECLARE_LOG_CATEGORY_CLASS(LogReflectionVolumeBrush, All, All);

namespace {
	/* Two triangles belong to the same face when they lie on the same plane. Tighter than anything
	 * a hull is built to, and loose enough for what rounding does to one. */
	constexpr double FaceNormalTolerance = 0.9999;
	constexpr double FaceOffsetTolerance = 0.1;

	/* Past this many corners, reading the faces off the corners alone stops being cheap */
	constexpr int32 MaximumHullCorners = 64;

	/* How near a corner has to sit to a cut before it counts as being on it */
	constexpr double ClipTolerance = 0.01;

	/* One face of a piece, as the plane it sits on and the corners found on it */
	struct FBrushFace {
		FVector Normal = FVector::ZeroVector;
		double Offset = 0.0;

		TArray<FVector> Corners;
	};

	/* One convex piece of the shape. A brush that is not itself convex was cut into several of
	 * these on the way to collision, and comes back as several. */
	struct FBrushHull {
		TArray<FVector> Corners;
		TArray<FBrushFace> Faces;
	};

	void AddCorner(FBrushFace& Face, const FVector& Corner) {
		for (const FVector& Existing : Face.Corners) {
			if (Existing.Equals(Corner, 0.01)) return;
		}

		Face.Corners.Add(Corner);
	}

	FBrushFace& FindFace(TArray<FBrushFace>& Faces, const FVector& Normal, const double Offset) {
		for (FBrushFace& Face : Faces) {
			if (FVector::DotProduct(Face.Normal, Normal) > FaceNormalTolerance && FMath::Abs(Face.Offset - Offset) < FaceOffsetTolerance) {
				return Face;
			}
		}

		FBrushFace& Face = Faces.AddDefaulted_GetRef();

		Face.Normal = Normal;
		Face.Offset = Offset;

		return Face;
	}

	FVector CentreOf(const TArray<FVector>& Corners) {
		FVector Centre = FVector::ZeroVector;

		for (const FVector& Corner : Corners) {
			Centre += Corner;
		}

		return Corners.Num() > 0 ? Centre / Corners.Num() : Centre;
	}

	/* A brush poly wants its corners in order around the face, and a hull arrives as loose
	 * triangles, so they are put back in order by their angle about the centre of the face */
	void SortCornersAroundNormal(const FVector& Normal, TArray<FVector>& Corners) {
		if (Corners.Num() < 3) return;

		const FVector Centre = CentreOf(Corners);

		FVector Right, Up;
		Normal.FindBestAxisVectors(Up, Right);

		Corners.Sort([&Centre, &Right, &Up](const FVector& A, const FVector& B) {
			const FVector OffsetA = A - Centre;
			const FVector OffsetB = B - Centre;

			return FMath::Atan2(FVector::DotProduct(OffsetA, Up), FVector::DotProduct(OffsetA, Right))
				 < FMath::Atan2(FVector::DotProduct(OffsetB, Up), FVector::DotProduct(OffsetB, Right));
		});
	}

	bool AddPoly(TArray<FPoly>& Polys, const FVector& Normal, TArray<FVector> Corners) {
		SortCornersAroundNormal(Normal, Corners);

		if (Corners.Num() < 3) return false;

		/* Wound the way the face points, which is what tells a brush its inside from its outside */
		const FVector Ordered = FVector::CrossProduct(Corners[1] - Corners[0], Corners[2] - Corners[0]);

		if (FVector::DotProduct(Ordered, Normal) < 0.0) {
			Algo::Reverse(Corners);
		}

		FPoly Poly;

		Poly.Init();

		Poly.Base = FVector3f(Corners[0]);
		Poly.PolyFlags = 0;

		/* No material on a volume, which is what keeps one from holding a texture alive */
		Poly.Material = nullptr;

		Poly.Vertices.Reserve(Corners.Num());

		for (const FVector& Corner : Corners) {
			Poly.Vertices.Add(FVector3f(Corner));
		}

		/* Works out the normal and the texture vectors, and says so when the corners describe
		 * nothing a face can be built from */
		if (Poly.Finalize(nullptr, 1) != 0) return false;

		Polys.Add(MoveTemp(Poly));

		return true;
	}

	/* Every face of a piece, worked out from its corners alone.
	 *
	 * A cooked hull carries its points and no triangles: the physics engine builds those back when
	 * it loads one, so the indices are left out of the asset. A plane through any three of the
	 * points is a face of the piece exactly when the whole of it lies on one side of that plane. */
	void GatherFacesFromPoints(const TArray<FVector>& Points, TArray<FBrushFace>& Faces) {
		const int32 Count = Points.Num();

		for (int32 First = 0; First < Count; ++First) {
			for (int32 Second = First + 1; Second < Count; ++Second) {
				for (int32 Third = Second + 1; Third < Count; ++Third) {
					const FVector Normal = FVector::CrossProduct(Points[Second] - Points[First], Points[Third] - Points[First]).GetSafeNormal();

					/* Three points in a line name no plane */
					if (Normal.IsNearlyZero()) continue;

					const double Offset = FVector::DotProduct(Normal, Points[First]);

					bool bAnyInFront = false;
					bool bAnyBehind = false;

					for (int32 Point = 0; Point < Count; ++Point) {
						const double Distance = FVector::DotProduct(Normal, Points[Point]) - Offset;

						if (Distance > FaceOffsetTolerance) bAnyInFront = true;
						else if (Distance < -FaceOffsetTolerance) bAnyBehind = true;

						if (bAnyInFront && bAnyBehind) break;
					}

					/* Points on both sides, so the plane cuts through the piece rather than closing it */
					if (bAnyInFront && bAnyBehind) continue;

					/* Turned to face outwards, which is the side nothing is on */
					const FVector Outward = bAnyInFront ? -Normal : Normal;
					const double OutwardOffset = bAnyInFront ? -Offset : Offset;

					FBrushFace& Face = FindFace(Faces, Outward, OutwardOffset);

					for (const FVector& Point : Points) {
						if (FMath::Abs(FVector::DotProduct(Outward, Point) - OutwardOffset) < FaceOffsetTolerance) {
							AddCorner(Face, Point);
						}
					}
				}
			}
		}
	}

	/* A box is already a face at a time, so it is spelled out rather than looked for */
	void GatherHullFromBox(const FKBoxElem& Box, FBrushHull& Hull) {
		const FTransform Transform(Box.Rotation, Box.Center);
		const FVector Extent(Box.X * 0.5, Box.Y * 0.5, Box.Z * 0.5);

		static const FVector Normals[6] = {
			FVector(1, 0, 0), FVector(-1, 0, 0),
			FVector(0, 1, 0), FVector(0, -1, 0),
			FVector(0, 0, 1), FVector(0, 0, -1)
		};

		TArray<FVector> Local;

		for (int32 Corner = 0; Corner < 8; ++Corner) {
			Local.Add(FVector(
				(Corner & 1) ? Extent.X : -Extent.X,
				(Corner & 2) ? Extent.Y : -Extent.Y,
				(Corner & 4) ? Extent.Z : -Extent.Z
			));

			Hull.Corners.Add(Transform.TransformPosition(Local.Last()));
		}

		for (const FVector& Normal : Normals) {
			const FVector Outward = Transform.TransformVectorNoScale(Normal);

			FBrushFace& Face = Hull.Faces.AddDefaulted_GetRef();

			Face.Normal = Outward;

			/* The four corners of the box that sit on this face */
			for (int32 Corner = 0; Corner < Local.Num(); ++Corner) {
				if (FVector::DotProduct(Local[Corner], Normal) > 0.0) {
					Face.Corners.Add(Hull.Corners[Corner]);
				}
			}

			Face.Offset = Face.Corners.Num() > 0 ? FVector::DotProduct(Outward, Face.Corners[0]) : 0.0;
		}
	}

	bool GatherHullFromConvex(const FKConvexElem& Convex, FBrushHull& Hull) {
		const TArray<FVector>& Points = Convex.VertexData;
		const TArray<int32>& Indices = Convex.IndexData;

		if (Points.Num() < 4) return false;

		const FTransform Transform = Convex.GetTransform();

		Hull.Corners.Reserve(Points.Num());

		for (const FVector& Point : Points) {
			Hull.Corners.Add(Transform.TransformPosition(Point));
		}

		/* Cooked hulls carry no triangles, so the faces come from the corners, which costs a pass
		 * over every three of them. Volumes are boxes and near enough, so the count that would make
		 * that dear is one nothing here reaches. */
		if (Indices.Num() < 12 || Indices.Num() % 3 != 0) {
			if (Hull.Corners.Num() > MaximumHullCorners) return false;

			GatherFacesFromPoints(Hull.Corners, Hull.Faces);

			return Hull.Faces.Num() > 0;
		}

		for (int32 Index = 0; Index + 2 < Indices.Num(); Index += 3) {
			if (!Hull.Corners.IsValidIndex(Indices[Index]) || !Hull.Corners.IsValidIndex(Indices[Index + 1]) || !Hull.Corners.IsValidIndex(Indices[Index + 2])) {
				continue;
			}

			const FVector A = Hull.Corners[Indices[Index]];
			const FVector B = Hull.Corners[Indices[Index + 1]];
			const FVector C = Hull.Corners[Indices[Index + 2]];

			const FVector Normal = FVector::CrossProduct(B - A, C - A).GetSafeNormal();

			/* A triangle with no area names no plane */
			if (Normal.IsNearlyZero()) continue;

			FBrushFace& Face = FindFace(Hull.Faces, Normal, FVector::DotProduct(Normal, A));

			AddCorner(Face, A);
			AddCorner(Face, B);
			AddCorner(Face, C);
		}

		return Hull.Faces.Num() > 0;
	}

	/* Sutherland and Hodgman, on a polygon lying flat on a plane of its own: the halfspace crosses
	 * it along a line, and the polygon is cut there and one side of it kept */
	TArray<FVector> ClipToHalfspace(const TArray<FVector>& Polygon, const FVector& Normal, const double Offset, const bool bKeepBeyond) {
		TArray<FVector> Result;

		if (Polygon.Num() < 3) return Result;

		/* Positive on the side being kept */
		const double Side = bKeepBeyond ? -1.0 : 1.0;

		for (int32 Index = 0; Index < Polygon.Num(); ++Index) {
			const FVector& Current = Polygon[Index];
			const FVector& Next = Polygon[(Index + 1) % Polygon.Num()];

			const double Here = Side * (Offset - FVector::DotProduct(Normal, Current));
			const double There = Side * (Offset - FVector::DotProduct(Normal, Next));

			if (Here >= -ClipTolerance) Result.Add(Current);

			/* Crossing the line, so the crossing itself becomes a corner */
			if ((Here > ClipTolerance && There < -ClipTolerance) || (Here < -ClipTolerance && There > ClipTolerance)) {
				Result.Add(FMath::Lerp(Current, Next, Here / (Here - There)));
			}
		}

		return Result;
	}

	/* Whether any of Face lies within Hull at all */
	bool OverlapsFace(const TArray<FVector>& Face, const FVector& FaceNormal, const FBrushHull& Hull) {
		TArray<FVector> Within = Face;

		for (const FBrushFace& Cut : Hull.Faces) {
			/* A face lying along this one closes nothing across it */
			if (FMath::Abs(FVector::DotProduct(Cut.Normal, FaceNormal)) > FaceNormalTolerance) continue;

			Within = ClipToHalfspace(Within, Cut.Normal, Cut.Offset, false);

			if (Within.Num() < 3) return false;
		}

		return Within.Num() >= 3;
	}

	/* The parts of Face that Hull does not cover, each of them still convex.
	 *
	 * Whatever lies outside a convex piece lies beyond at least one of its faces, so the difference
	 * falls out as one part per face: what reaches past that face, of what stayed within all the
	 * faces before it. */
	void SubtractHull(const TArray<FVector>& Face, const FVector& FaceNormal, const FBrushHull& Hull, TArray<TArray<FVector>>& Parts) {
		TArray<FVector> Remaining = Face;

		for (const FBrushFace& Cut : Hull.Faces) {
			if (FMath::Abs(FVector::DotProduct(Cut.Normal, FaceNormal)) > FaceNormalTolerance) continue;

			TArray<FVector> Beyond = ClipToHalfspace(Remaining, Cut.Normal, Cut.Offset, true);

			if (Beyond.Num() >= 3) Parts.Add(MoveTemp(Beyond));

			Remaining = ClipToHalfspace(Remaining, Cut.Normal, Cut.Offset, false);

			/* Nothing of it left within, so nothing of it is covered */
			if (Remaining.Num() < 3) return;
		}
	}

	/* The faces that bound the shape, with the parts that ended up inside it taken off.
	 *
	 * A brush that is not convex reaches collision as several convex pieces, and those pieces meet
	 * along faces that were never part of the volume: they are where it was cut, not where it ends.
	 * Handing them to a brush along with the rest describes a solid with walls through the middle of
	 * it, so what one piece covers of another is taken back off. Part at a time, since a piece can
	 * cover a corner of a face and leave the rest of it standing. */
	int32 BuildBoundaryPolys(TArray<FBrushHull>& Hulls, TArray<FPoly>& Polys) {
		int32 Covered = 0;

		for (int32 Index = 0; Index < Hulls.Num(); ++Index) {
			for (FBrushFace& Face : Hulls[Index].Faces) {
				if (Face.Corners.Num() < 3) continue;

				SortCornersAroundNormal(Face.Normal, Face.Corners);

				TArray<TArray<FVector>> Parts;
				Parts.Add(Face.Corners);

				for (int32 Other = 0; Other < Hulls.Num() && Parts.Num() > 0; ++Other) {
					if (Other == Index) continue;

					/* Only a piece reaching past this face can stand in front of it */
					bool bBeyond = false;

					for (const FVector& Corner : Hulls[Other].Corners) {
						if (FVector::DotProduct(Face.Normal, Corner) - Face.Offset > FaceOffsetTolerance) {
							bBeyond = true;

							break;
						}
					}

					if (!bBeyond) continue;

					TArray<TArray<FVector>> Kept;

					for (const TArray<FVector>& Part : Parts) {
						/* Left whole where the piece is elsewhere on the plane, which keeps a face
						 * from being cut up by something that never touched it */
						if (OverlapsFace(Part, Face.Normal, Hulls[Other])) {
							SubtractHull(Part, Face.Normal, Hulls[Other], Kept);
						} else {
							Kept.Add(Part);
						}
					}

					Parts = MoveTemp(Kept);
				}

				if (Parts.Num() == 0) {
					Covered++;

					continue;
				}

				for (TArray<FVector>& Part : Parts) {
					AddPoly(Polys, Face.Normal, MoveTemp(Part));
				}
			}
		}

		return Covered;
	}

	/* The shape the volume was built with, off the body setup where one is standing and out of the
	 * export it names where none is.
	 *
	 * A map import spawns each actor and hands it the properties of its own export, which reaches
	 * the components the actor already made for itself. Anything hanging off one of those, a body
	 * setup among them, is a sub object nothing spawns, so the reference is left null and the shape
	 * has to be read out of the export the component named. */
	/* A box, told by its faces: six of them, in three facing pairs, square to the axes a cube is
	 * built along. Anything turned off them, or cut from more than one piece, is not one. */
	bool TryReadBox(const TArray<FBrushHull>& Hulls, FVector& OutSize, FVector& OutCentre) {
		if (Hulls.Num() != 1 || Hulls[0].Faces.Num() != 6) return false;

		double Least[3] = { 0.0, 0.0, 0.0 };
		double Most[3] = { 0.0, 0.0, 0.0 };

		bool bSeenLeast[3] = { false, false, false };
		bool bSeenMost[3] = { false, false, false };

		for (const FBrushFace& Face : Hulls[0].Faces) {
			if (Face.Corners.Num() != 4) return false;

			int32 Axis = INDEX_NONE;
			double Along = 0.0;

			for (int32 Index = 0; Index < 3; ++Index) {
				if (FMath::Abs(Face.Normal[Index]) > FaceNormalTolerance) {
					Axis = Index;
					Along = Face.Normal[Index];
				}
			}

			/* Turned off the axes, so a cube built along them is not this shape */
			if (Axis == INDEX_NONE) return false;

			/* The far face sits at its own offset, the near one at the other side of nothing */
			if (Along > 0.0) {
				Most[Axis] = Face.Offset;
				bSeenMost[Axis] = true;
			} else {
				Least[Axis] = -Face.Offset;
				bSeenLeast[Axis] = true;
			}
		}

		for (int32 Index = 0; Index < 3; ++Index) {
			if (!bSeenLeast[Index] || !bSeenMost[Index]) return false;

			OutSize[Index] = Most[Index] - Least[Index];
			OutCentre[Index] = (Most[Index] + Least[Index]) * 0.5;

			if (OutSize[Index] <= FaceOffsetTolerance) return false;
		}

		return true;
	}

	bool FindShape(const UBrushComponent* Component, const TSharedPtr<FJsonObject>& Properties, const UPropertySerializer* Serializer, FKAggregateGeom& OutShape) {
		if (Component->BrushBodySetup != nullptr) {
			OutShape = Component->BrushBodySetup->AggGeom;

			return true;
		}

		if (!Properties.IsValid() || Serializer == nullptr || Serializer->ExportsContainer == nullptr) return false;

		const TSharedPtr<FJsonObject>* Reference;
		if (!Properties->TryGetObjectField(TEXT("BrushBodySetup"), Reference)) return false;

		FUObjectExport* Export = Serializer->ExportsContainer->GetExportByObjectPath(*Reference);
		if (Export == nullptr || !Export->IsJsonValid()) return false;

		const TSharedPtr<FJsonObject> Setup = Export->GetProperties();
		if (!Setup.IsValid()) return false;

		const TSharedPtr<FJsonObject>* Geometry;
		if (!Setup->TryGetObjectField(TEXT("AggGeom"), Geometry)) return false;

		Serializer->DeserializeStruct(FKAggregateGeom::StaticStruct(), Geometry->ToSharedRef(), &OutShape);

		return true;
	}
}

void FVolumeBrush::Rebuild(UBrushComponent* Component, const TSharedPtr<FJsonObject>& Properties, const UPropertySerializer* Serializer) {
	if (Component == nullptr) return;

	ABrush* Brush = Cast<ABrush>(Component->GetOwner());
	if (Brush == nullptr) return;

	/* The actor has to know the component as its own before any of this: the BSP is prepared
	 * through the actor, which reaches for it and insists on finding one */
	if (Brush->GetBrushComponent() != Component) return;

	/* One that came through with a shape of its own is left with it */
	if (Brush->Brush != nullptr && Brush->Brush->Polys != nullptr && Brush->Brush->Polys->Element.Num() > 0) return;

	FKAggregateGeom Shape;

	if (!FindShape(Component, Properties, Serializer, Shape)) {
		UE_LOG(LogReflectionVolumeBrush, Warning, TEXT("\"%s\" came without collision, which is the one place a cooked volume keeps its shape, so it stays a point"), *Brush->GetName());

		return;
	}

	TArray<FBrushHull> Hulls;

	for (const FKBoxElem& Box : Shape.BoxElems) {
		GatherHullFromBox(Box, Hulls.AddDefaulted_GetRef());
	}

	for (const FKConvexElem& Convex : Shape.ConvexElems) {
		if (!GatherHullFromConvex(Convex, Hulls.AddDefaulted_GetRef())) {
			Hulls.Pop();
		}
	}

	const int32 Pieces = Hulls.Num();

	/* A box is the shape a volume is nearly always made of, and the editor keeps one as its
	 * dimensions rather than its corners, which is what lets it go on being edited as a box */
	FVector BoxSize = FVector::ZeroVector;
	FVector BoxCentre = FVector::ZeroVector;

	const bool bIsBox = TryReadBox(Hulls, BoxSize, BoxCentre);

	/* A cube is measured out from the pivot, so the shape is moved onto it and the component moved
	 * back by as much, which leaves the volume standing where the game had it */
	if (bIsBox && !BoxCentre.IsNearlyZero()) {
		for (FBrushHull& Hull : Hulls) {
			for (FVector& Corner : Hull.Corners) {
				Corner -= BoxCentre;
			}

			for (FBrushFace& Face : Hull.Faces) {
				for (FVector& Corner : Face.Corners) {
					Corner -= BoxCentre;
				}

				Face.Offset -= FVector::DotProduct(Face.Normal, BoxCentre);
			}
		}
	}

	TArray<FPoly> Polys;

	const int32 Covered = BuildBoundaryPolys(Hulls, Polys);

	if (Polys.Num() == 0) {
		UE_LOG(LogReflectionVolumeBrush, Warning, TEXT("\"%s\" has collision of a kind a brush cannot be cut from (%d box(es), %d hull(s), %d sphere(s), %d capsule(s)), so it stays a point"), *Brush->GetName(), Shape.BoxElems.Num(), Shape.ConvexElems.Num(), Shape.SphereElems.Num(), Shape.SphylElems.Num());

		return;
	}

	Brush->PreEditChange(nullptr);

	if (bIsBox && !BoxCentre.IsNearlyZero()) {
		const FTransform Relative = Component->GetRelativeTransform();

		Component->SetRelativeLocation(Relative.GetLocation() + Relative.GetRotation().RotateVector(BoxCentre * Relative.GetScale3D()));
	}

	/* The shape the editor gives a volume of its own: a model holding the polys, and a BSP
	 * prepared from them, which is what the bounds are then measured off */
	const EObjectFlags Flags = Brush->GetFlags() & (RF_Transient | RF_Transactional);

	Brush->PolyFlags = 0;

	/* The cooked map keeps the model the brush pointed at, empty of all the cook took off it, and
	 * both the actor and its component still name it, so it is filled rather than replaced */
	if (Brush->Brush == nullptr) {
		Brush->Brush = NewObject<UModel>(Brush, NAME_None, Flags);
	}

	Brush->Brush->Initialize(nullptr, true);
	Brush->Brush->Polys = NewObject<UPolys>(Brush->Brush, NAME_None, Flags);
	Brush->Brush->Polys->Element = MoveTemp(Polys);

	Component->Brush = Brush->Brush;

	/* What the editor asks a brush to describe itself with. The polys are already the ones a cube
	 * builder would lay down, so the two agree and rebuilding from it changes nothing. */
	if (bIsBox) {
		UCubeBuilder* Builder = NewObject<UCubeBuilder>(Brush, NAME_None, Flags);

		Builder->X = BoxSize.X;
		Builder->Y = BoxSize.Y;
		Builder->Z = BoxSize.Z;

		Brush->BrushBuilder = Builder;
	}

	FBSPOps::csgPrepMovingBrush(Brush);

	Brush->PostEditChange();

	Component->MarkRenderStateDirty();

	if (bIsBox) {
		UE_LOG(LogReflectionVolumeBrush, Display, TEXT("\"%s\" was given a cube of %.0f by %.0f by %.0f, with its pivot moved %.0f to the middle of it"), *Brush->GetName(), BoxSize.X, BoxSize.Y, BoxSize.Z, BoxCentre.Size());
	} else {
		UE_LOG(LogReflectionVolumeBrush, Display, TEXT("\"%s\" was given a brush of %d face(s) from %d piece(s), %d dropped from where they met"), *Brush->GetName(), Brush->Brush->Polys->Element.Num(), Pieces, Covered);
	}
}

void FVolumeBrush::Rebuild(ABrush* Brush) {
	if (Brush != nullptr) {
		Rebuild(Brush->GetBrushComponent());
	}
}
