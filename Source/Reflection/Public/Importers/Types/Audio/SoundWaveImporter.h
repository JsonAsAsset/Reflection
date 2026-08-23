/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "Importers/Constructor/Importer.h"

/* Brings a sound wave over from Cloud.
 *
 * A wave is the one asset whose contents never arrive as json: Cloud writes the audio out to a
 * file and says where it put it, and the engine's own factory is what turns that file into the
 * asset. Everything on the export that isn't a fact of the audio the group it plays in, whether
 * it streams, volume, pitch, looping, subtitles is a setting the factory knows nothing about,
 * and gets put on afterward the same way every other importer does it. */
class ISoundWaveImporter : public IImporter {
public:
	virtual bool Import() override;
};

REGISTER_IMPORTER(ISoundWaveImporter, {
	"SoundWave"
}, "Sound Assets");
