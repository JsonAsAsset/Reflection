/* Copyright Reflection Contributors 2024-2026 */

#include "Settings/ModdingSettings.h"
#include "Modules/Metadata.h"
#include "Settings/ReflectionSettings.h"

UReflectionModdingSettings::UReflectionModdingSettings() {
	CategoryName = GReflectionSettingsCategoryName;
	SectionName = FName(*(GReflectionName.ToString() + TEXT("Modding")));
}

FText UReflectionModdingSettings::GetSectionText() const {
	return FText::FromString("Modding");
}
