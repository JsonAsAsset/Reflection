/* Copyright Reflection Contributors 2024-2026 */

using System;
using UnrealBuildTool;

/* NOTE: Please make sure to put UE5 only modules in the #if statement below, we want UE4 and UE5 compatibility */
public class Reflection : ModuleRules {
	public Reflection(ReadOnlyTargetRules Target) : base(Target)  {
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		
		var bIsLinux = Target.Platform == UnrealTargetPlatform.Linux;

#if UE_5_0_OR_LATER
	    /* Unreal Engine 5 and later */
	    CppStandard = CppStandardVersion.Cpp20;
#else
		/* Unreal Engine 4 */
		CppStandard = CppStandardVersion.Cpp17;
#endif

		PublicDependencyModuleNames.AddRange(new[] {
			"Core",
			"Json",
			"JsonUtilities",
			"UMG",
			"RenderCore",
			"HTTP",
			"DeveloperSettings",
			"Niagara",
			"UnrealEd", 
			"MainFrame",
			"GameplayTags",
			"ApplicationCore",
			"AnimGraph",
			"UMGEditor",
			"ClothingSystemRuntimeCommon",
			"MovieScene",
			
#if UE_5_0_OR_LATER
			"ContentBrowserData"
#endif
		});

		PrivateDependencyModuleNames.AddRange(new[] {
			"Projects",
			"InputCore",
			"ToolMenus",
			"CoreUObject",
			"Engine",
			"Slate",
			"SlateCore",
			"MaterialEditor",
			"Landscape",
			"AssetTools",
			"EditorStyle",
			"Settings",
			"PhysicsCore",
			"PluginUtils",
			"AudioModulation",
			"RHI",
			"Detex",
			"NVTT",
			"RenderCore",
			"AnimGraphRuntime",
			"AnimGraph",

#if UE_5_0_OR_LATER
			/* Only Unreal Engine 5 */

			"AnimationDataController",
			"ToolWidgets"
#endif
		});
		
		if (!bIsLinux) {
			PrivateDependencyModuleNames.AddRange(new[] {
				"Detex",
				"NVTT"
			});
		}
	}
}