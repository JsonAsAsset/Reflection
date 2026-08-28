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
		Settings.CurveMapping = ERCurveMapping::DataAsset;

		return Settings;
	}();

	const UReflectionModdingSettings* Settings = GetModdingSettings();

	return Settings->Enabled ? Settings->Settings : Off;
}

/* Whether a curve mapping comes in as a rig.
 *
 * Asked for, or the only thing left. The asset the game keeps a mapping in belongs to a plugin that
 * ships with Unreal Engine 5, and an engine without it has no such asset to make. What the mapping
 * says is arithmetic either way, and a rig is somewhere arithmetic can still be put, so where there
 * is no asset to be had the rig stands in its place rather than the import coming to nothing.
 *
 * Where both are possible it is the setting's call. Where neither is, nothing here can be made. */
inline bool ImportsCurveMappingAsRig() {
#if REFLECTION_RIGVM
#if REFLECTION_CURVE_EXPRESSION
	return GetModdingAssetSettings().CurveMapping == ERCurveMapping::ControlRig;
#else
	return true;
#endif
#else
	return false;
#endif
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
