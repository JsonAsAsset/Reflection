/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "ISettingsModule.h"
#include "Modules/Metadata.h"
#include "Settings/ReflectionSettings.h"

#if (ENGINE_MAJOR_VERSION != 4 || ENGINE_MINOR_VERSION < 27)
#include "Engine/DeveloperSettings.h"
#endif

inline UReflectionSettings* GetSettings() {
	return GetMutableDefault<UReflectionSettings>();
}

inline void SavePluginSettings(UDeveloperSettings* EditorSettings) {
	EditorSettings->SaveConfig();

#if ENGINE_UE5
	EditorSettings->TryUpdateDefaultConfigFile();
	EditorSettings->ReloadConfig(nullptr, nullptr, UE::LCPF_PropagateToInstances);
#else
	EditorSettings->UpdateDefaultConfigFile();
	EditorSettings->ReloadConfig(nullptr, nullptr, UE4::LCPF_PropagateToInstances);
#endif

	EditorSettings->LoadConfig();
}

inline void OpenPluginSettings() {
	FModuleManager::LoadModuleChecked<ISettingsModule>("Settings").ShowViewer("Editor", GReflectionSettingsCategoryName, GReflectionName);
}
