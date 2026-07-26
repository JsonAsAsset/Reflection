/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "Importers/Constructor/Graph/SoundGraph.h"

class ISoundCueImporter : public ISoundGraph {
public:
	virtual UObject* CreateAsset(UObject* CreatedAsset) override;
	virtual bool Import() override;
};

REGISTER_IMPORTER(ISoundCueImporter, {
	"SoundCue"
}, "Sound Assets");