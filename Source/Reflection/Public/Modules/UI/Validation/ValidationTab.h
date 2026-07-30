/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"
#include "Engine/Compatibility.h"

/* Validation is UE5 only */
#if ENGINE_UE5

class SDockTab;
class SValidationPanel;
class FSpawnTabArgs;

/* Owns the "Validation" nomad tab.
 *
 * The tab is spawned on demand from the Validation toolbar button, and lives in the global tab
 * manager so the editor can restore it with the rest of the layout. */
class REFLECTION_API FValidationTab {
public:
	static const FName TabId;

	/* Registers the tab spawner and adds this validator to the toolbar's validator list */
	static void Register();
	static void Unregister();

	/* Brings the tab up, creating it if it isn't open, scoped to the folder selected in the
	 * Content Browser. Leaves the panel's existing folder alone when nothing is selected. */
	static void Open();

	/* Brings the tab up scoped to a content folder.
	 * An empty RootPath leaves whatever folder the panel already had.
	 * Takes RootPath by value so it can be bound as a delegate payload. */
	static void OpenAt(FString RootPath, bool bRunImmediately);

	/* Brings the tab up scoped to the folder selected in the Content Browser */
	static void OpenAtSelectedFolder();

private:
	static TSharedRef<SDockTab> Spawn(const FSpawnTabArgs& Args);

	/* The panel of the open tab, invalid while the tab is closed */
	static TWeakPtr<SValidationPanel> ActivePanel;
};

#endif
