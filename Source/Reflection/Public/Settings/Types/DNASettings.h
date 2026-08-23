/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"
#include "DNASettings.generated.h"

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

	/* A DNA names every control its own rig was authored with, which is far more than an older
	 * head ever had, and a pose per control is only useful to something that knows those names.
	 * Turn this on to bake the older head's poses instead: the game ships the correspondence
	 * between the two as a curve mapping, and each of its curves is baked as one pose, driven by
	 * the rig controls that curve is made of in the amounts the mapping gives them.
	 *
	 * What comes out is keyed by the names an older head's animation already drives, so a face
	 * built for the newer rig can be played by the older one. */
	UPROPERTY(EditAnywhere, DisplayName = "Backport Poses", Config, Category = DNASettings, meta = (EditCondition = "BakeToPoseAsset"))
	bool BackportPoses = false;

	/* The curve mapping to read the correspondence out of. Any CurveExpressionsDataAsset whose curves are written in terms of the rig's controls works. */
	UPROPERTY(EditAnywhere, DisplayName = "Curve Mapping", Config, Category = DNASettings, meta = (EditCondition = "BakeToPoseAsset && BackportPoses"))
	FString CurveMapping = TEXT("/Game/Characters/Player/Common/Fortnite_Base_Head/Facials/CurveMappings/FN_3LToLegacy_Main_Mapping");

	/* A mapping says what an older head's curve is made of, not how far it should be pushed, and
	 * taken at face value some of them overshoot: a blink driven the whole way the mapping allows
	 * closes past the eye rather than onto it. The game gets away with it because nothing drives
	 * those curves to one on their own.
	 *
	 * Turn this on to bake the poses at a strength that reads properly on its own instead, scaling
	 * the controls behind a curve by the amount below before the rig is asked what it looks like.
	 * A pose with no entry is baked as the mapping has it. */
	UPROPERTY(EditAnywhere, DisplayName = "Adjust Pose Strengths", Config, Category = DNASettings, meta = (EditCondition = "BakeToPoseAsset && BackportPoses"))
	bool AdjustPoseStrengths = false;

	/* How far to drive each named pose, as a fraction of what the mapping asks for. Keyed by the
	 * curve's name, which is matched however it is typed. */
	UPROPERTY(EditAnywhere, DisplayName = "Pose Strengths", Config, Category = DNASettings, meta = (EditCondition = "BakeToPoseAsset && BackportPoses && AdjustPoseStrengths"))
	TMap<FString, float> PoseStrengths;

	FRDnaSettings() {
		/* Eyes shut and eyes wide both land where they should at roughly two thirds, and a frown
		 * at half. Everything else reads well enough taken as the mapping has it. */
		PoseStrengths.Add(TEXT("L_blink_pose"), 0.652f);
		PoseStrengths.Add(TEXT("R_blink_pose"), 0.652f);

		PoseStrengths.Add(TEXT("L_wide_pose"), 0.652f);
		PoseStrengths.Add(TEXT("R_wide_pose"), 0.652f);

		PoseStrengths.Add(TEXT("L_frown_pose"), 0.5f);
		PoseStrengths.Add(TEXT("R_frown_pose"), 0.5f);
	}
};