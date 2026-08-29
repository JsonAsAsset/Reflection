/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "Importers/Constructor/ObjectImporter.h"

/* A Niagara parameter collection, and the store its default instance keeps its values in.
 *
 * A variable holds its type as a handle into a registry, and that handle is written by an archive
 * of its own rather than as a property. Nothing here runs that archive: the properties are read out
 * of json, the handle is never among them, and every variable keeps what its constructor gave it,
 * which is Vec4. Sixteen bytes claimed by every parameter, whatever it actually is.
 *
 * What that looks like is a collection of floats drawn as four component vectors, each row reading
 * the three values after it as its own, and the last of them reading off the end of the store. */
class INiagaraParameterCollectionImporter final : public IObjectImporter {
public:
	virtual void Repair(UObject* Asset) const override;

	virtual void Validate(UObject* Asset) const override;
};

REGISTER_IMPORTER(INiagaraParameterCollectionImporter, TArray<FString>{
	TEXT("NiagaraParameterCollection")
}, TEXT("Niagara Assets"));
