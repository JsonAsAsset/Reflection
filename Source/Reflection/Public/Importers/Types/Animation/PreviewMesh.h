/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"

class USkeleton;

/* The mesh an animation asset opens on.
 *
 * Persona needs one to show anything, and the cook keeps none: which mesh an asset was previewed on
 * is editor-only and goes at cook time. What the asset was made for is almost always sitting beside
 * it, so the folder it landed in is where one is looked for. */
struct REFLECTION_API FPreviewMesh {
	/* Gives Asset a preview mesh from its own folder and says whether it found one. Anything the
	 * asset already names is left alone, and a mesh built on another skeleton is passed over. */
	static bool FromFolder(UObject* Asset, const USkeleton* Skeleton);
};
