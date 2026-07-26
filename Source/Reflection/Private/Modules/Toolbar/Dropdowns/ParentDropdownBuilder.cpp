/* Copyright Reflection Contributors 2024-2026 */

#include "Modules/Toolbar/Dropdowns/ParentDropdownBuilder.h"

#include "Reflection.h"
#include "Modules/Metadata.h"
#include "Engine/Compatibility.h"
#include "Engine/EngineUtilities.h"

void IParentDropdownBuilder::Build(FMenuBuilder& MenuBuilder) const {
	MenuBuilder.BeginSection(
		"ReflectionSection", 
		FText::FromString(FRMetadata::Version)
	);
}
