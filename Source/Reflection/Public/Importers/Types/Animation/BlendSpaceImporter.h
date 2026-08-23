/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "Importers/Constructor/Importer.h"

#if ENGINE_UE4
class UBlendSpaceBase;

using UReflectionBlendSpace = UBlendSpaceBase;
#else
class UBlendSpace;

using UReflectionBlendSpace = UBlendSpace;
#endif

class IBlendSpaceImporter : public IImporter {
public:
	virtual UObject* CreateAsset(UObject* CreatedAsset) override;
	virtual bool Import() override;

private:
	/* Brings every animation the samples name into memory, before a sample is written.
	 *
	 * Loading an animation sweeps every blend space already in memory and revalidates the ones
	 * holding a sample of it, this one included. That sweep lands in the middle of the sample array
	 * being built, where the sample being written is still empty, and the engine both complains
	 * about the empty sample and is free to drop it as a duplicate of another empty one out from
	 * under the pointer the array is being written through. Loading them all first leaves nothing
	 * for a sample to trigger. */
	void PreloadSampleAnimations();

	/* Looks up again, off the reference that named it, every sample the properties left without an
	 * animation.
	 *
	 * A sample with no animation is the one thing a blend space cannot be told apart from later:
	 * the engine says only that one of them is empty, never which, and the grid drops it the moment
	 * the asset is opened. Whatever is still missing after this is named outright instead. */
	void ResolveEmptySamples(UReflectionBlendSpace* BlendSpace);

	/* Puts the grid into the order this engine reads it in.  */
	void AlignGridToEngine(UReflectionBlendSpace* BlendSpace);
};

REGISTER_IMPORTER(IBlendSpaceImporter, (TArray<FString>{
	"BlendSpace",
	"BlendSpace1D",

	"AimOffsetBlendSpace",
	"AimOffsetBlendSpace1D"
}), TEXT("Animation Assets"));