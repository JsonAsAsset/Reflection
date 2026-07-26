/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"
#include "Interfaces/IPluginManager.h"

extern FName GReflectionName;

/* Caches basic metadata about Reflection */
struct FRMetadata {
	static TSharedPtr<IPlugin> Plugin;
	static FString Version;

	static void Initialize();
};