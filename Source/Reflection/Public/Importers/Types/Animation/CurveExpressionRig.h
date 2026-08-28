/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "Engine/Compatibility.h"

#if REFLECTION_RIGVM

#include "CoreMinimal.h"

class UControlRigBlueprint;

/* What came of turning a curve mapping into a rig */
struct FCurveExpressionRigStats {
	/* Curves the mapping writes, and the ones a graph was built for */
	int32 Targets = 0;
	int32 Written = 0;

	/* Curves it reads, one Get node each however many targets want it */
	int32 Read = 0;

	int32 Nodes = 0;

	/* Targets left out because the Cloud would not call them a weighted sum */
	TArray<FString> Refused;
};

/* A curve mapping, built as the rig that performs it.
 *
 * The asset says each of one head's curves is so much of this control plus so much of that one,
 * held between nothing and one. That is a statement about arithmetic, and a rig is a place
 * arithmetic runs: read the controls the animation drives, weigh them, add them up, clamp, and
 * write the curve the older head wants under its own name.
 *
 * Built rather than baked, so it converts while the animation plays instead of being folded into
 * the animation beforehand. Every curve stays where it was authored and the rig does the arithmetic
 * each frame, which is what the mapping was written to describe in the first place. */
class REFLECTION_API FCurveExpressionRig {
public:
	/* Lays the whole mapping into the rig's forwards solve. False when there is nothing to lay.
	 *
	 * Expressions arrive as the Cloud says them: a target, the constants that reach it, what each
	 * one is worth, and which shape the Cloud found the expression to be when it ran it. */
	static bool Build(UControlRigBlueprint* Blueprint, const TArray<TSharedPtr<FJsonValue>>& Expressions,
		const FString& Named, FCurveExpressionRigStats& OutStats);
};

#endif
