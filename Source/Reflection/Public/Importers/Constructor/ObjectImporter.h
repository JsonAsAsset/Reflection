/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "Importers/Constructor/Importer.h"

/* An asset that is nothing but its properties.
 *
 * Plenty of assets are built the same way: make the object of whatever class the export names, make
 * the subobjects it owns, read the properties over the top, and hand it to the content browser. A
 * blackboard, a font, a data asset and most things without geometry or a payload behind them are
 * all that and nothing else.
 *
 * Importers with something of their own to do still override Import. Importers that only need to
 * say when the result came across half built override Validate instead, and leave the building to
 * this. */
class REFLECTION_API IObjectImporter : public IImporter {
public:
	virtual UObject* CreateAsset(UObject* CreatedAsset = nullptr) override;
	virtual bool Import() override;

	/* Puts right anything the export left in a state the engine will not take, and nothing by
	 * default.
	 *
	 * Different from Validate, which only says what it found: this one runs while the asset is
	 * still being built and is allowed to change it. A behavior tree whose node classes this build
	 * hasn't got keeps the empty child slots where they were, and the editor walks those without
	 * checking, so they are taken out here rather than left to crash on opening. */
	virtual void Repair(UObject* Asset) const {}
};
