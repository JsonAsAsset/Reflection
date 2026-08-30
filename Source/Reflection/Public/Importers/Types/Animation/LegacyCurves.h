/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"

/* One of the newer rig's controls, and how much of an older head's curve went into it */
struct FLegacyCurveDrive {
	FName Control;
	float Weight = 0.0f;
};

/* Reading an animation of the newer rig back into the names an older head drives.
 *
 * The curve mapping is written the other way round each of the newer rig's controls as an
 * expression over the older head's curves so every control the animation drives is one statement
 * about the curves beneath it, and an emote drives more controls than there are curves. The set of
 * curves that best accounts for all of them at once is what comes back. */
struct REFLECTION_API FReflectionLegacyCurves {
	/* The mapping as a table, keyed by the older head's curves, as the Cloud weighs it.
	 *
	 * Weighed by running the compiled expressions rather than by reading the text they were written
	 * as, because half of them clamp and coefficients read off a clamp are not coefficients. Which
	 * mapping it is, is the Cloud's to know: it has the project open, and the answer is a fact about
	 * the game rather than anything worth asking for. */
	static bool Read(TMap<FName, TArray<FLegacyCurveDrive>>& OutByCurve);

	/* Rewrites the curves in place, and says whether it did. Left alone where there is no mapping to
	 * read, or nothing in the animation the mapping knows a control by. Curves that are not controls
	 * at all are carried through untouched.
	 *
	 * Keeping the controls leaves both sets on the animation, for a head that answers to one of
	 * them and a set of idles written against the other. */
	static bool Rewrite(TArray<TSharedPtr<FJsonValue>>& FloatCurves, const FString& Named, bool bKeepControls = false);
};
