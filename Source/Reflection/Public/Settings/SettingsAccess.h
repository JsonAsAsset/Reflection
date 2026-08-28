/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "ISettingsModule.h"
#include "Modules/Metadata.h"
#include "Settings/ReflectionSettings.h"
#include "Settings/ModdingSettings.h"
#include "Engine/Compatibility.h"

#if (ENGINE_MAJOR_VERSION != 4 || ENGINE_MINOR_VERSION < 27)
#include "Engine/DeveloperSettings.h"
#endif

inline UReflectionSettings* GetSettings() {
	return GetMutableDefault<UReflectionSettings>();
}

inline UReflectionModdingSettings* GetModdingSettings() {
	return GetMutableDefault<UReflectionModdingSettings>();
}

/* What the modding page asks for, or nothing where it is switched off.
 *
 * Read through here rather than off the page directly, so the switch is honoured in one place
 * instead of at every point that asks a question of it. */
inline const FRModdingSettings& GetModdingAssetSettings() {
	static const FRModdingSettings Off = [] {
		FRModdingSettings Settings;

		Settings.Material.Stubs = false;
		Settings.MetaHuman.Bake = ERDnaBake::None;
		Settings.MetaHuman.Curves = ERDnaCurves::Controls;

		return Settings;
	}();

	const UReflectionModdingSettings* Settings = GetModdingSettings();

	return Settings->Enabled ? Settings->Settings : Off;
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
