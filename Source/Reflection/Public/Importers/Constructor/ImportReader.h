/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "Importer.h"
#include "Dom/JsonValue.h"
#include "Containers/Export.h"

class REFLECTION_API IImportReader {
public:
	static bool ReadExportsAndImport(const TArray<TSharedPtr<FJsonValue>>& Exports, const FString& File, IImporter*& OutImporter, bool HideNotifications = false);
	static IImporter* ReadExportAndImport(FUObjectExportContainer* Container, FUObjectExport* Export, FString File, bool HideNotifications = false);
	static IImporter* ImportReference(const FString& File);
};
