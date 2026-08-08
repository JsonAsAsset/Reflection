/* Copyright Reflection Contributors 2024-2026 */

#include "Modules/Toolbar/Tools/ImportFolder.h"

#include "Modules/Cloud/Cloud.h"
#include "Modules/Cloud/Remote.h"
#include "Modules/Toolbar/Tools/ImportFromPath.h"
#include "Modules/UI/Reflect/SReflectFolderDialog.h"
#include "Engine/EngineUtilities.h"
#include "Utilities/Dialog.h"

void TToolImportFolder::Execute() {
	if (!Cloud::Status::IsOpened()) {
		SpawnPrompt("Reflect Folder", "Cloud isn't running, so there is nowhere to fetch from.");

		return;
	}

	/* Nothing here goes through the reflect button, so this is where the project name gets fetched */
	if (!Cloud::EnsureMetadataBlocking()) {
		SpawnPrompt("Reflect Folder", "Cloud didn't say which project it has loaded, so paths can't be resolved.");

		return;
	}

	/* The dialog does the listing itself: a folder is however big it is, and every asset in it is
	 * a request, so what comes down is worth seeing before it runs */
	TArray<FString> Paths;

	if (!SReflectFolderDialog::Open(Paths)) {
		return;
	}

	int32 Reflected = 0;
	int32 Attempted = 0;
	bool Cancelled = false; {
		/* Import opens a scope of its own per asset, which is what the dialog ends up showing.
		 * This one is here to own the Cancel button for the whole run rather than for one asset. */
		const FBlockingRequestScope BlockingScope(NSLOCTEXT("Reflection", "ReflectingFolder", "Reflecting folder"));

		for (const FString& Path : Paths) {
			Attempted++;

			if (TToolImportFromPath::Import(Path)) {
				Reflected++;
			}

			/* Cancelling only kills the request that was in flight, so the run has to notice it
			 * too or it would carry on failing its way through the rest of the folder */
			if (FBlockingRequestScope::Pump()) {
				Cancelled = true;

				break;
			}
		}
	}

	const bool Successful = !Cancelled && Reflected == Attempted;

	AppendNotification(
		FText::FromString(Cancelled ? "Reflect Folder Cancelled" : (Successful ? "Reflected Folder" : "Reflected With Failures")),
		FText::FromString(FString::Printf(TEXT("%d of %d"), Reflected, Paths.Num())),
		Successful ? 2.0f : 5.0f,
		Successful ? SNotificationItem::CS_Success : SNotificationItem::CS_Fail,
		true,
		310.0f
	);
}
