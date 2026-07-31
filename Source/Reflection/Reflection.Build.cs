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

#if !UE_4_26_OR_LATER
		/* The engine's shared PCH is built at the engine default standard on these versions, and
		 * MSVC refuses to consume a PCH compiled under a different /std. The sources are already
		 * include-what-you-use, so dropping the PCH entirely costs build time and nothing else. */
		PCHUsage = PCHUsageMode.NoPCHs;
#endif
#endif

		PublicDependencyModuleNames.AddRange(new[] {
			"Core",
			"Json",
			"JsonUtilities",
			"UMG",
			"RenderCore",
			"HTTP",
			"Niagara",
			"UnrealEd",
			"MainFrame",
			"GameplayTags",
			"ApplicationCore",
			"AnimGraph",
			"UMGEditor",
			"MovieScene",

#if UE_4_26_OR_LATER
			/* Unreal Engine 4.26 and later.
			 * DeveloperSettings lived inside Engine before it was split out, and the cloth
			 * runtime was a single ClothingSystemRuntime module until it was broken apart. */
			"DeveloperSettings",
			"ClothingSystemRuntimeCommon",
#else
			/* Unreal Engine 4.25 and below */
			"ClothingSystemRuntime",
#endif

#if UE_5_0_OR_LATER
			"ContentBrowserData"
#endif
		});

		PrivateDependencyModuleNames.AddRange(new[] {
			"Projects",
			"InputCore",
			"CoreUObject",
			"Engine",
			"Slate",
			"SlateCore",
			"MaterialEditor",
			"Landscape",
			"AssetTools",
			"EditorStyle",
			"Settings",
			"RHI",
			"Detex",
			"NVTT",
			"RenderCore",
			"AnimGraphRuntime",
			"AnimGraph",

#if UE_4_24_OR_LATER
			/* Unreal Engine 4.24 and later.
			 * ToolMenus is what replaced the level editor's FExtender based toolbar */
			"ToolMenus",
#endif

#if UE_4_26_OR_LATER
			/* Unreal Engine 4.26 and later.
			 * PhysicsCore was carved out of Engine, PluginUtils and AudioModulation
			 * did not ship before this point */
			"PhysicsCore",
			"PluginUtils",
			"AudioModulation",
#endif

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