/* Copyright Reflection Contributors 2024-2026 */

#include "Modules/Toolbar/Dropdowns/CloudToolsDropdownBuilder.h"

#include "Modules/Cloud/Cloud.h"
#include "Modules/Cloud/Tools/AnimationData.h"
#include "Modules/Cloud/Tools/ConvexCollision.h"
#include "Modules/Cloud/Tools/CurveLinearColorData.h"
#include "Modules/Cloud/Tools/FontData.h"
#include "Modules/Cloud/Tools/SkeletalMeshData.h"
#include "Modules/Cloud/Tools/WidgetAnimations.h"

namespace {
	/* A run outlives the click that started it, so a tool has to outlive its own requests. One
	 * instance per tool for the editor's lifetime is also what stops a second click from
	 * starting a second run on top of the first. */
	template <typename ToolType>
	TSelectedAssetsBase& GetTool() {
		static ToolType Tool;

		return Tool;
	}

	/* Menu entries are only useful with something on the other end, and a tool that is still
	 * working would refuse a second run anyway */
	template <typename ToolType>
	bool CanRunTool() {
		return Cloud::Status::IsOpened() && !GetTool<ToolType>().IsRunning();
	}

	template <typename ToolType>
	FUIAction MakeToolAction() {
		return FUIAction(
			FExecuteAction::CreateLambda([] {
				GetTool<ToolType>().Execute();
			}),
			FCanExecuteAction::CreateStatic(&CanRunTool<ToolType>)
		);
	}
}

void ICloudToolsDropdownBuilder::Build(FMenuBuilder& MenuBuilder) const {
	MenuBuilder.BeginSection("ReflectionCloudSection", FText::FromString("Cloud"));

	MenuBuilder.AddMenuEntry(
		FText::FromString("Static Meshes"),
		FText::FromString("Imports collision and other properties"),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "ClassIcon.StaticMeshActor"),
		MakeToolAction<TToolConvexCollision>(),
		NAME_None
	);

	MenuBuilder.AddMenuEntry(
		FText::FromString("Animations"),
		FText::FromString("Imports curve data, notifies and other properties"),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "GraphEditor.Animation_24x"),
		MakeToolAction<TToolAnimationData>(),
		NAME_None
	);

	MenuBuilder.AddMenuEntry(
		FText::FromString("Skeletal Meshes"),
		FText::FromString("Imports sockets and other properties"),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "ClassIcon.SkeletalMeshComponent"),
		MakeToolAction<TSkeletalMeshData>(),
		NAME_None
	);

	MenuBuilder.AddMenuEntry(
		FText::FromString("Fonts"),
		FText::FromString("Imports font properties (not vectorized data)"),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "ClassIcon.FontFace"),
		MakeToolAction<TToolFontData>(),
		NAME_None
	);

	MenuBuilder.AddMenuEntry(
		FText::FromString("Widget Animations"),
		FText::FromString("Imports widget animations"),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "ClassIcon.WidgetBlueprint"),
		MakeToolAction<TWidgetAnimations>(),
		NAME_None
	);

	MenuBuilder.AddMenuEntry(
		FText::FromString("Linear Colors"),
		FText::FromString("Imports colors if any changes were made"),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "ClassIcon.CurveBase"),
		MakeToolAction<TCurveLinearColorData>(),
		NAME_None
	);

	MenuBuilder.EndSection();
}
