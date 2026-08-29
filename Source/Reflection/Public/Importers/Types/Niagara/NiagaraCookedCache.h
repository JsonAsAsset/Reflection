/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"

struct FUObjectExportContainer;

/* What the cook moved aside, put back where the asset reads it from.
 *
 * Niagara keeps a handful of things twice over. A script holds the values its module inputs were
 * left at, a curve data interface holds its curve, and cooking moves each of them out of the
 * place the editor reads and into a second one beside it, named for the first with
 * "CookedEditorCache" on the end. The engine puts them back as it reads a package it knows was
 * cooked, and only then.
 *
 * What comes out of an import is a package the engine has just been handed and does not think is
 * cooked, so nothing puts them back: a module reads its inputs at whatever its own graph says
 * rather than what the emitter set them to, and a curve comes up flat.
 *
 * Which fields those are is not written down here. The pairing is the name, so that is what is
 * looked for, and anything else Niagara comes to keep this way is already covered. */
class REFLECTION_API FNiagaraCookedCache {
public:
	/* Answers with the number of fields moved back */
	static int32 Restore(FUObjectExportContainer* Container);
};
