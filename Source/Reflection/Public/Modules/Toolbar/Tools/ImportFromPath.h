/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"
#include "Modules/Tools/ToolBase.h"

/* Reflects assets straight out of Cloud by path.
 *
 * Everything else in Reflection starts from something already in the project: a selected asset, a
 * folder of json, a reference another import ran into. This one starts from the path alone, which
 * is the only way to reach an asset nothing in the project points at yet. */
class TToolImportFromPath : public TToolBase {
public:
	void Execute();

	/* Reflects one asset. The path can be in either Cloud or editor form, with or without an extension, an export name or a Type'...' wrapper around it.
	 * False when Cloud has nothing at that path, or the import itself failed.
	 *
	 * AllowedTypes empty means every type. A type outside it is left alone and reported through
	 * bOutFiltered, which is not the same as failing: the type is only known once the export is
	 * down, so the request happens either way. */
	static REFLECTION_API bool Import(const FString& InPath, const TSet<FString>& AllowedTypes = TSet<FString>(), bool* bOutFiltered = nullptr);
};
