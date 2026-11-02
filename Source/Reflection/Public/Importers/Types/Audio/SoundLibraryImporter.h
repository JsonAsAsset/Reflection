/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "Importers/Constructor/ObjectImporter.h"

/* A library of sounds, each under the gameplay tag whatever plays it asks for it by.
 *
 * Nothing but properties, so the building is the ordinary one. What it adds is the listing a
 * sound is actually looked up in: a library keeps the entries it was authored with and, beside
 * them, the complete listing those come to once a parent library's are folded in. Playback reads
 * the second. A cook carries both, and where it carries only the first the listing is made from
 * it rather than left empty, which is a library that answers nothing. */
class ISoundLibraryImporter final : public IObjectImporter {
public:
	virtual void Repair(UObject* Asset) const override;
};

REGISTER_IMPORTER(ISoundLibraryImporter, {
	"SoundLibrary"
}, "Sound Assets");
