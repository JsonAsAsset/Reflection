/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "Importers/Constructor/Importer.h"

class USkeleton;
class UAnimSequence;

/* Builds an animation sequence out of the keys the game shipped.
 *
 * A cook keeps a sequence as a compressed stream and drops the keys it was built from, so nothing
 * in the export says where a bone is at a given frame. The Cloud reads the stream back out, a key
 * a frame, and this writes those keys in as the sequence's own animation data: what the engine
 * compresses again on save is what the game was playing.
 *
 * The sequence keeps what it was cooked as. An additive one stays additive against the same pose,
 * and nothing is retargeted on the way through: a sequence is meant for the skeleton it names. */
class IAnimSequenceImporter : public IImporter {
public:
	virtual UObject* CreateAsset(UObject* CreatedAsset) override;
	virtual bool Import() override;

private:
	/* The skeleton the sequence is played on, imported through the Cloud when the project doesn't
	 * have it. Null when the export names none, or when the one it names could not be reached. */
	USkeleton* ResolveSkeleton();

	/* Writes the Cloud's tracks in as the sequence's animation data, one track a bone. Returns how
	 * many were written, which is not the number sent: a track naming a bone this skeleton hasn't
	 * got is nothing the sequence can play. */
	int32 BuildTracks(UAnimSequence* AnimSequence, USkeleton* Skeleton, const TSharedPtr<FJsonObject>& Payload) const;
};

REGISTER_IMPORTER(IAnimSequenceImporter, (TArray<FString>{
	TEXT("AnimSequence")
}), TEXT("Animation Assets"));
