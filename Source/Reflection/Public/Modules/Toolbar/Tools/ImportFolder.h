/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"
#include "Modules/Tools/ToolBase.h"

/* Reflects every asset under a folder of the game files.
 *
 * The folder is one the Cloud knows about, not one in this project: nothing has to point at any of
 * it yet. Cloud hands back the paths, and each of them goes through the same import the by-path
 * tool runs. */
class TToolImportFolder : public TToolBase {
public:
	/* InitialFolder is what the dialog opens on. bUseClipboard lets an empty one fall back to
	 * whatever was copied, which is what the reflect dialog hands over with and what right-clicking
	 * a folder does not: that names its own folder. bCanGoBack offers the way back to the window
	 * this was opened from, so only the reflect dialog passes it.
	 *
	 * True when that way back was the one taken, which is the reflect dialog's cue to open again.
	 * Nothing was reflected in that case. */
	bool Execute(const FString& InitialFolder = FString(), bool bUseClipboard = false, bool bCanGoBack = false);
};
