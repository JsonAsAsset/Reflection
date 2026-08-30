/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"
#include "DNASettings.generated.h"

/* Flattening the rig down to something that plays without it. */
UENUM()
enum class ERDnaBake : uint8 {
	/* Left as the rig, which needs RigLogic to read it */
	None UMETA(DisplayName = "None"),

	/* A pose asset beside the mesh: every control the DNA names is evaluated on its own and the
	 * joints it moves are kept as a pose. The corrective layers a DNA runs on top of the
	 * controls are not part of a pose and don't survive. */
	PoseAsset UMETA(DisplayName = "Pose Asset"),

	/* The faces as morph targets on the mesh rather than as poses of its joints. */
	MorphTargets UMETA(DisplayName = "Morph Targets")
};

/* Whose names the face is said in. */
UENUM()
enum class ERDnaCurves : uint8 {
	/* The names the rig was authored with, left alone */
	Controls UMETA(DisplayName = "Rig Controls"),

	/* The names an older head already drives, so a face built for the newer rig can be played by the older one */
	Legacy UMETA(DisplayName = "Legacy Head"),

	/* Both sets side by side. */
	Both UMETA(DisplayName = "Rig Controls and Legacy Head")
};

/* Whether the older head's names are wanted at all */
inline bool WantsLegacyCurves(const ERDnaCurves Curves) {
	return Curves != ERDnaCurves::Controls;
}

/* Whether the rig's own controls are kept beside them */
inline bool KeepsRigControls(const ERDnaCurves Curves) {
	return Curves != ERDnaCurves::Legacy;
}

/* Settings for the face rig a MetaHuman head carries. */
USTRUCT()
struct FRDnaSettings {
	GENERATED_BODY()
public:
	/* What the rig is flattened down to, where it is flattened at all.
	 *
	 * Helpful for Fortnite, bakes MetaHuman into the mesh, whereas beforehand it wouldn't of been possible in older Unreal Engine versions. */
	UPROPERTY(EditAnywhere, DisplayName = "Bake", Config, Category = MetaHuman)
	ERDnaBake Bake = ERDnaBake::None;

	/* Whose names the face comes out in.
	 *
	 * Helpful for Fortnite, uses Legacy curve names instead of 3L, of which the game doesn't use in older versions. */
	UPROPERTY(EditAnywhere, DisplayName = "Curves", Config, Category = MetaHuman)
	ERDnaCurves Curves = ERDnaCurves::Controls;
};
