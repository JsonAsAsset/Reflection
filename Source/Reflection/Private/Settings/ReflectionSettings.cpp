/* Copyright Reflection Contributors 2024-2026 */

#include "Settings/ReflectionSettings.h"
#include "Modules/Metadata.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/SBoxPanel.h"

FName GReflectionSettingsCategoryName = FName("General");
FName GReflectionInternalName = FName("AmbientAudio");

UReflectionSettings::UReflectionSettings() {
	CategoryName = GReflectionSettingsCategoryName;
	SectionName = GReflectionName;
}

FText UReflectionSettings::GetSectionText() const {
	return FText::FromString("Reflection");
}
