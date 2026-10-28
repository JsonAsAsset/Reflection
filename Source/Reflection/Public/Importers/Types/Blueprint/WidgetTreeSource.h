/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Containers/ExportContainer.h"

/* A widget tree that was cooked into a different asset.
 *
 * A widget only ever placed inside another one can be cooked as part of that one. The widget keeps
 * its class, its variables and its animations, and its WidgetTree is null: the tree itself sits in
 * the containing asset, under the instance of this widget placed there.
 *
 * So the tree is fetched out of that asset and brought into this import, which is as far as this
 * goes. Nothing is placed and nothing is created: the exports are handed to the same reader that
 * would have read them had the widget carried them itself. */
struct REFLECTION_API FWidgetTreeSource {
	/* Looks through the assets the settings name for a tree belonging to ClassName, which is the
	 * generated class the widget is read as, so AthenaPlayerActionAlert_C.
	 *
	 * On a find the tree and everything under it are appended to Container, and the tree's own
	 * export is handed back. Null when none of the sources hold one, which is the ordinary answer
	 * for a widget whose tree simply was not cooked anywhere. */
	static FUObjectExport* Borrow(FUObjectExportContainer* Container, const FName ClassName);

	/* The path a reference names, out of Class'Outer:Name.Inner' and the like. Public because the
	 * same unwrapping is what decides whether one export sits under another. */
	static FString PathOf(const TSharedPtr<FJsonObject>& Reference);
};
