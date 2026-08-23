/* Copyright Reflection Contributors 2024-2026 */

#include "Modules/Metadata.h"

#include "Interfaces/IPluginManager.h"
#include "Engine/EngineUtilities.h"
#include "Engine/Log.h"

FName GReflectionName = FName("Reflection");

TSharedPtr<IPlugin> FRMetadata::Plugin = nullptr;
FString FRMetadata::Version = "";

void FRMetadata::Initialize() {
    Plugin = GetPlugin(GReflectionName.ToString());

    /* FindPlugin answers with nothing when the plugin is not the name it looks itself up by, which
     * is what a renamed folder or a second copy in the project does. Nothing here is worth taking
     * the editor down over: the version is only ever shown. */
    if (!Plugin.IsValid()) {
        UE_LOG(LogReflection, Warning, TEXT("No plugin named \"%s\" is loaded, so its version is unknown"), *GReflectionName.ToString());

        Version = FString();

        return;
    }

    Version = Plugin->GetDescriptor().VersionName;
}
