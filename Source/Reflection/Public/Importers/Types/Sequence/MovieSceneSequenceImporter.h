/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "Importers/Constructor/ObjectImporter.h"

/* A sequence is a movie scene and everything hung off it, and all of it is ordinary properties in
 * the right outers, so the serializers build the whole asset. Left over is only what the cook
 * derived or dropped: compiled data, holes where a class is missing, the possessed classes, and the
 * timeline window.
 *
 * Registered against MovieSceneSequence too, so any game subclass comes through here. */
class IMovieSceneSequenceImporter final : public IObjectImporter {
public:
	virtual void Prepare(UObject* Asset) const override;
	virtual void Repair(UObject* Asset) const override;
	virtual void Validate(UObject* Asset) const override;
};

REGISTER_IMPORTER(IMovieSceneSequenceImporter, (TArray<FString>{
	TEXT("LevelSequence"),
	TEXT("TemplateSequence"),
	TEXT("CameraAnimationSequence"),
	TEXT("MovieSceneSequence")
}), "Sequencer Assets");
