/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "Importers/Constructor/ObjectImporter.h"

/* A Niagara script, and the graph it is written as.
 *
 * The script itself is almost nothing: what it does is a graph of nodes hanging off a source
 * beneath it, and the nodes are ordinary exports that the usual building makes. What the usual
 * building cannot make is the wiring. A node keeps its pins in an archive of its own written after
 * its properties, so a graph read as properties alone comes out as a pile of nodes with nothing
 * joined to anything, and the pins are put back here.
 *
 * The script is joined to its source here too. Nothing in the export says which source is the one
 * it uses, so that is chosen and said out loud rather than assumed quietly. */
class INiagaraScriptImporter final : public IObjectImporter {
public:
	virtual void Repair(UObject* Asset) const override;

	virtual void Validate(UObject* Asset) const override;
};

REGISTER_IMPORTER(INiagaraScriptImporter, TArray<FString>{
	TEXT("NiagaraScript")
}, TEXT("Niagara Assets"));
