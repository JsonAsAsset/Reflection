/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "Importers/Constructor/ObjectImporter.h"

/* A Niagara emitter, and everything hanging off the version of itself it exposes.
 *
 * The emitter's own properties are almost nothing. What it is made of sits under it as its own
 * exports: the scripts it runs, the source its graph hangs off, the renderers it draws with, the
 * editor's notes about it. What ties them together is the version list, and that is not in the
 * export, so the list is made here and filled from what is under the emitter.
 *
 * Which script is which is asked of the scripts rather than assumed from their names: each one
 * says what it is for, and that is what puts it in the right place. */
class INiagaraEmitterImporter final : public IObjectImporter {
public:
	virtual void Repair(UObject* Asset) const override;

	virtual void Validate(UObject* Asset) const override;
};

REGISTER_IMPORTER(INiagaraEmitterImporter, TArray<FString>{
	TEXT("NiagaraEmitter")
}, TEXT("Niagara Assets"));
