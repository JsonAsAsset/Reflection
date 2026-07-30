/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"
#include "Modules/Toolbar/Tools/ToolBase.h"

/*
 * Base for the Cloud tools, which all do the same thing: take what the user selected in the
 * Content Browser, ask the Cloud for each asset's exports, and apply them.
 *
 * Execute returns as soon as it has read the selection. The run then walks the selection one
 * asset at a time off the back of each Cloud response, so the editor stays usable throughout and
 * a tool that can't reach the Cloud costs a request timeout instead of a frozen editor.
 *
 * Because a run outlives the call that started it, a tool instance has to outlive its requests
 * and can only have one run in flight; the Cloud dropdown keeps a single instance of each tool.
 */
class REFLECTION_API TSelectedAssetsBase : public TToolBase {
public:
	/* Starts a run over the current selection. Does nothing if a run is already going. */
	virtual void Execute();

	/* Called once per selected asset with the exports the Cloud returned for it. Never called
	 * with an empty export list, and never with an asset that failed to resolve. */
	virtual void Process(UObject* Object, const TArray<TSharedPtr<FJsonValue>>& Exports) { }

	/* Asset class this tool applies to. NAME_None takes everything in the selection. */
	virtual FName GetSupportedClass() const { return NAME_None; }

	bool IsRunning() const { return bRunning; }

private:
	/* Fetches the export list for the asset at QueueIndex, or finishes the run */
	void RequestNext();

	void HandleExports(const TArray<TSharedPtr<FJsonValue>>& Exports);
	void Finish();

	/* Object paths rather than asset pointers: a run spans many ticks, and holding a UObject*
	 * across them would only be safe by accident */
	TArray<FString> Queue;

	int32 QueueIndex = 0;
	bool bRunning = false;
};
