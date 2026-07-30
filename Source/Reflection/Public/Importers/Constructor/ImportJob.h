/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"

/*
 * Runs a top-level import across frames instead of in one call.
 *
 * The editor's frame is the game thread, so an import that holds the game thread for thirty
 * seconds is thirty seconds of frozen editor. This walks the same per-export path
 * IImportReader::ReadExportAndImport does, a slice at a time, handing the frame back in between.
 *
 * What it does not do is make anything faster, and it cannot promise a steady frame rate: a single
 * export can run far past one frame's budget and there is no way to divide it from out here. What
 * it promises is that the editor keeps drawing, keeps responding, and reports where it is.
 *
 * Only for imports started by the user. A dependency import runs from inside property
 * deserialization and has to finish before that call returns, so it stays synchronous through
 * IImportReader::ReadExportsAndImport.
 *
 * One job runs at a time. Files handed over while a job is going are appended to it rather than
 * starting a second one, because two imports building assets in the same content at once is not
 * something the importers are written to survive.
 */
class REFLECTION_API FImportJob {
public:
	/* Queues Files, starting a job if none is running, and returns immediately. */
	static void Enqueue(const TArray<FString>& Files);

	static bool IsRunning();

	/* Drops every file and export not yet started. Whatever is mid-import still finishes, since
	 * stopping partway through one export would leave a half-built asset behind. */
	static void Cancel();
};
