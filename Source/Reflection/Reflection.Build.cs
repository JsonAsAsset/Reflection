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
			/* UDeveloperSettings lived inside Engine, at the same header path, until 4.26 gave
			 * it a module of its own */
			"DeveloperSettings",
#endif

#if UE_4_25_OR_LATER
			/* The cloth runtime was a single ClothingSystemRuntime module until it was split */
			"ClothingSystemRuntimeCommon",
#else
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

			/* FNodeFactory/SGraphNode, used to measure anim graph nodes when auto-laying them out */
			"GraphEditor",

			/* UEdGraphSchema_K2, whose PC_* pin categories name the type of a blueprint variable */
			"BlueprintGraph",

#if UE_4_23_OR_LATER
			/* PhysicsCore was carved out of Engine in 4.23 */
			"PhysicsCore",
#endif

#if UE_4_24_OR_LATER
			/* ToolMenus is what replaced the level editor's FExtender based toolbar */
			"ToolMenus",
#endif

#if UE_4_25_OR_LATER
			"AudioModulation",
#endif

#if UE_4_26_OR_LATER
			"PluginUtils",
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