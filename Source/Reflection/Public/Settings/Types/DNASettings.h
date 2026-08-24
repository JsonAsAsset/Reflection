/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"
#include "DNASettings.generated.h"

/* Baking an older head's poses in place of the rig's own controls.
 *
 * A DNA names every control its own rig was authored with, which is far more than an older head
 * ever had, and a pose per control is only useful to something that knows those names. The game
 * ships the correspondence between the two as a curve mapping, so each of its curves is baked as
 * one pose, driven by the rig controls that curve is made of in the amounts the mapping gives them.
 *
 * What comes out is keyed by the names an older head's animation already drives, so a face built
 * for the newer rig can be played by the older one. */
USTRUCT()
struct FRDnaBackportSettings {
	GENERATED_BODY()
public:
	/* Bake the older head's poses rather than one per control the DNA names. */
	UPROPERTY(EditAnywhere, DisplayName = "Backport Poses", Config, Category = DNASettings)
	bool BackportPoses = true;

	/* The curve mapping to read the correspondence out of. Any CurveExpressionsDataAsset whose curves are written in terms of the rig's controls works. */
	UPROPERTY(EditAnywhere, DisplayName = "Curve Mapping", Config, Category = DNASettings, meta = (EditCondition = "BackportPoses", HideEditConditionToggle))
	FString CurveMapping = "/Game/Characters/Player/Common/Fortnite_Base_Head/Facials/CurveMappings/FN_LegacyTo3L_Main_Mapping";
};

/* Settings for the face rig a MetaHuman head carries. */
USTRUCT()
struct FRDnaSettings {
	GENERATED_BODY()
public:
	/* A MetaHuman head animates through RigLogic, which reads the mesh's DNA and poses the face's
	 * joints. Turn this on to bake that rig down into a pose asset beside the mesh: every control
	 * the DNA names is evaluated on its own and the joints it moves are kept as a pose.
	 *
	 * The poses are additive, against the rig standing still, so they add up the way the controls
	 * do. What a control does on its own is exact; the corrective layers a DNA runs on top of the
	 * controls are not part of a pose and don't survive. */
	UPROPERTY(EditAnywhere, DisplayName = "Bake to Pose Asset", Config, Category = DNASettings)
	bool BakeToPoseAsset = false;

	/* Bake the faces as morph targets on the mesh rather than as poses of its joints.
	 *
	 * A pose asset stores a rotation a pose and multiplies them together as they stack, where the
	 * rig adds the angles up and turns the total into one rotation. Those two agree while the angles
	 * are small and part company as they grow, and this face turns lips, eyelids and tongue by forty
	 * degrees on a single curve, so that is where it shows.
	 *
	 * A morph target is an offset a vertex, and offsets add. The skinning is done once for each face
	 * and only the difference is kept, which puts the whole thing back where the arithmetic is
	 * linear. Costs a morph target a pose and the memory that goes with it. */
	UPROPERTY(EditAnywhere, DisplayName = "Bake to Morph Targets", Config, Category = DNASettings)
	bool MorphTargets = false;

	/* Which poses to bake, when there is a mapping to bake them from. Ignored once the poses are
	 * exact, since those are named by the rig's own controls and the mapping drives them instead. */
	UPROPERTY(EditAnywhere, DisplayName = "Backport", Config, Category = DNASettings, meta = (EditCondition = "BakeToPoseAsset || MorphTargets", HideEditConditionToggle))
	FRDnaBackportSettings Backport;
};
