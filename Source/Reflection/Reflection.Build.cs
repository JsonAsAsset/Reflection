/* Copyright Reflection Contributors 2024-2026 */

using System;
using System.IO;
using UnrealBuildTool;

/* NOTE: Please make sure to put UE5 only modules in the #if statement below, we want UE4 and UE5 compatibility */
public class Reflection : ModuleRules {
	public Reflection(ReadOnlyTargetRules Target) : base(Target)  {
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		var bIsLinux = Target.Platform == UnrealTargetPlatform.Linux;

		/* Nothing reads this any more: the way in is the Direct Asset Data setting */
		var bCloudServer = true;

		PublicDefinitions.Add("REFLECTION_CLOUD_SERVER=" + (bCloudServer ? "1" : "0"));

		/* Control Rig ships as an engine plugin, and an engine it was stripped out of has nothing to
		 * link the rig importer against. The rig API this is written against is the UE5 one, so 4.26
		 * and 4.27, where the plugin exists but a rig is still a hierarchy container, count as not
		 * having it either. */
		var bControlRig = false;

		/* 5.2 is where RigVM became a plugin of its own and a rig blueprint became a RigVM one */
#if UE_5_2_OR_LATER
		bControlRig = Directory.Exists(Path.Combine(EngineDirectory, "Plugins", "Animation", "ControlRig"))
			&& Directory.Exists(Path.Combine(EngineDirectory, "Plugins", "Runtime", "RigVM"));
#endif

		PublicDefinitions.Add("REFLECTION_CONTROL_RIG=" + (bControlRig ? "1" : "0"));

		/* Drawing a graph is a smaller thing to ask than reading one back.
		 *
		 * The rig importer above rebuilds a cooked rig out of its bytecode, and that reads a rig the
		 * way 5.2 and later keep one. Laying out a graph that was never cooked asks for none of
		 * that: nodes, links, pin defaults, and the units that read and write a curve, all of which
		 * RigVM has had since it arrived.
		 *
		 * So it is asked for separately. RigVM was an engine module before it was a plugin, and the
		 * check knows both shapes. */
		var bRigVM = bControlRig;

		if (!bRigVM) {
			bRigVM = Directory.Exists(Path.Combine(EngineDirectory, "Plugins", "Experimental", "ControlRig"))
				&& Directory.Exists(Path.Combine(EngineDirectory, "Source", "Developer", "RigVMDeveloper"));
		}

		PublicDefinitions.Add("REFLECTION_RIGVM=" + (bRigVM ? "1" : "0"));

		/* RigLogic is what reads a MetaHuman's DNA, and it ships as an engine plugin the same way
		 * Control Rig does. An engine without it has nothing to hand the bit stream to.
		 *
		 * 5.2 is the floor, and 5.0 and 5.1 count as not having it. RigLogic there hands a DNA back
		 * a layer at a time rather than as one reader, knows nothing of the machine learned
		 * behavior the face is run from, and reads its stream through a class of another name; the
		 * pose the rig is baked into is written through the animation data controller, which those
		 * versions haven't got either. None of that is one API away. */
		var bRigLogic = false;

#if UE_5_2_OR_LATER
		bRigLogic = Directory.Exists(Path.Combine(EngineDirectory, "Plugins", "Animation", "RigLogic"));
#endif

		PublicDefinitions.Add("REFLECTION_RIG_LOGIC=" + (bRigLogic ? "1" : "0"));

		/* Newer RigLogic keeps the DNA in an asset of its own and hangs a UDNAAssetUserData off the
		 * mesh to name it. The anim node reads that in preference to the old UDNAAsset, so which one
		 * a DNA has to be written into depends on whether the engine has it. */
		var bDnaUserData = bRigLogic && File.Exists(Path.Combine(EngineDirectory, "Plugins", "Animation", "RigLogic",
			"Source", "RigLogicModule", "Public", "DNAAssetUserData.h"));

		PublicDefinitions.Add("REFLECTION_DNA_USER_DATA=" + (bDnaUserData ? "1" : "0"));

		/* Newer RigLogic converts a DNA into UE's axes as RigLogic reads it, through a wrapper the
		 * engine puts between the two, and its anim node writes the pose without flipping anything
		 * itself. Older engines hand the DNA straight to RigLogic and let the node do the axes. So
		 * anything evaluating the rig outside the node has to know which of the two it is looking
		 * at, or it reads every rotation and every sideways offset the wrong way round. */
		var bUeSpaceReader = bRigLogic && File.Exists(Path.Combine(EngineDirectory, "Plugins", "Animation", "RigLogic",
			"Source", "RigLogicModule", "Public", "RigLogicDNAReader.h"));

		PublicDefinitions.Add("REFLECTION_RIG_LOGIC_UE_SPACE_READER=" + (bUeSpaceReader ? "1" : "0"));

		/* CurveExpression compiles the arithmetic that drives one curve from others. It started out
		 * an experimental plugin and has been promoted since, so both homes are looked at.
		 *
		 * The asset the expressions are kept in came later than the plugin around it: the earliest
		 * versions ship the evaluator and the anim node with nothing to store a compiled expression
		 * in. So it is that asset's header that decides this, not the directory it sits under. */
		var bCurveExpression = false;

#if UE_5_0_OR_LATER
		foreach (var CurveExpressionRoot in new[] {
			Path.Combine(EngineDirectory, "Plugins", "Animation", "CurveExpression", "Source"),
			Path.Combine(EngineDirectory, "Plugins", "Experimental", "Animation", "CurveExpression", "Source")
		}) {
			if (Directory.Exists(CurveExpressionRoot)
				&& Directory.GetFiles(CurveExpressionRoot, "CurveExpressionsDataAsset.h", SearchOption.AllDirectories).Length > 0) {
				bCurveExpression = true;

				break;
			}
		}
#endif

		PublicDefinitions.Add("REFLECTION_CURVE_EXPRESSION=" + (bCurveExpression ? "1" : "0"));

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
			"AIModule",
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

#if UE_4_24_OR_LATER
			/* The cloth runtime was a single ClothingSystemRuntime module until 4.24 split it */
			"ClothingSystemRuntimeCommon",
#else
			"ClothingSystemRuntime",
#endif

#if UE_5_0_OR_LATER
			"ContentBrowserData",

			/* FBSPOps, which built brushes from inside UnrealEd until UE5 moved it out */
			"BSPUtils"
#endif
		});

		PrivateDependencyModuleNames.AddRange(new[] {
			"ScriptDisassembler",
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

			/* FContentBrowserModule, for the selection behind the right click menu */
			"ContentBrowser",
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

			/* FMeshDescription and FStaticMeshAttributes, which the static mesh importer describes
			 * its geometry into */
			"MeshDescription",
			"StaticMeshDescription",

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
		
		/* Where the rig importer is not built, these are still what a graph is drawn with */
		if (bRigVM && !bControlRig) {
			PrivateDependencyModuleNames.AddRange(new[] {
				"ControlRig",
				"ControlRigDeveloper",
				"RigVMDeveloper"
			});
		}

		if (bControlRig) {
			PrivateDependencyModuleNames.AddRange(new[] {
				/* URigHierarchy and the controller every element is added through */
				"ControlRig",

				/* UControlRigBlueprint, and URigVMBlueprint under it */
				"ControlRigDeveloper",
				"RigVMDeveloper",

				/* The VM itself: its function registry is what names the node behind an instruction */
				"RigVM",

				/* UControlRigBlueprintFactory, which is what gives a new rig its graph */
				"ControlRigEditor"
			});
		}

		if (bRigLogic) {
			PrivateDependencyModuleNames.AddRange(new[] {
				/* UDNAAsset, and the reader that turns a DNA stream into one */
				"RigLogicModule",

				/* The DNA reader and writer themselves, for putting one into the axes the anim
				 * node reads */
				"RigLogicLib",

				/* UAnimGraphNode_RigLogic, which is how the node is put in a graph to test it */
				"RigLogicDeveloper"
			});
		}

		/* A build may carry work of its own beside the module. What that work needs comes with it,
		 * so nothing here is asked of a build that hasn't got it. */
		if (Directory.Exists(Path.Combine(ModuleDirectory, "Extensions"))) {
			PrivateDependencyModuleNames.AddRange(new[] {
				/* SObjectPropertyEntryBox and the rest of the editor's asset pickers */
				"PropertyEditor"
			});
		}

		if (bCurveExpression) {
			PrivateDependencyModuleNames.AddRange(new[] {
				/* UCurveExpressionsDataAsset, and the list its expressions are written into */
				"CurveExpression"
			});
		}

		if (!bIsLinux) {
			PrivateDependencyModuleNames.AddRange(new[] {
				"Detex",
				"NVTT"
			});
		}
	}
}