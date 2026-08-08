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
	void Execute();
};
