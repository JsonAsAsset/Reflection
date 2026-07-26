/* Copyright Reflection Contributors 2024-2026 */

#include "Modules/Metadata.h"

#include "Interfaces/IPluginManager.h"
#include "Engine/EngineUtilities.h"

FName GReflectionName = FName("Reflection");

TSharedPtr<IPlugin> FRMetadata::Plugin = nullptr;
FString FRMetadata::Version = "";

void FRMetadata::Initialize() {
    Plugin = GetPlugin(GReflectionName.ToString());
    Version = Plugin->GetDescriptor().VersionName;
}
