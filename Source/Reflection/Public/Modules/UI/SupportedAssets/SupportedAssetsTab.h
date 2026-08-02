/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"

class SDockTab;
class FSpawnTabArgs;
class SSupportedAssetsPanel;

/* The window listing every asset type Reflection can build */
class REFLECTION_API FSupportedAssetsTab {
public:
	static void Register();
	static void Unregister();

	/* Opens the tab, reading the registries again on the way in */
	static void Open();

	static const FName TabId;

private:
	static TSharedRef<SDockTab> Spawn(const FSpawnTabArgs& Args);

	static TWeakPtr<SSupportedAssetsPanel> ActivePanel;
};
