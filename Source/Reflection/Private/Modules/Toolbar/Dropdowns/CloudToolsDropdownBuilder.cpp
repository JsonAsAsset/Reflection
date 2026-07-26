/* Copyright Reflection Contributors 2024-2026 */

#include "Modules/Toolbar/Dropdowns/CloudToolsDropdownBuilder.h"

#include "Modules/Cloud/Cloud.h"
#include "Modules/Cloud/Tools/AnimationData.h"
#include "Modules/Cloud/Tools/ConvexCollision.h"
#include "Modules/Cloud/Tools/CurveLinearColorData.h"
#include "Modules/Cloud/Tools/FontData.h"
#include "Modules/Cloud/Tools/SkeletalMeshData.h"
#include "Modules/Cloud/Tools/WidgetAnimations.h"

void ICloudToolsDropdownBuilder::Build(FMenuBuilder& MenuBuilder) const {
	static TSkeletalMeshData* SkeletalMeshTool;

	if (SkeletalMeshTool == nullptr) {
		SkeletalMeshTool = new TSkeletalMeshData();
	}
	
	MenuBuilder.BeginSection("ReflectionCloudSection", FText::FromString("Cloud"));
	
	MenuBuilder.AddMenuEntry(
		FText::FromString("Static Meshes"),
		FText::FromString("Imports collision and other properties"),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "ClassIcon.StaticMeshActor"),

		FUIAction(
			FExecuteAction::CreateLambda([] {
				TToolConvexCollision* Tool = new TToolConvexCollision();
				Tool->Execute();
			}),
			FCanExecuteAction::CreateLambda([this] {
				return Cloud::Status::IsOpened();
			})
		),
		NAME_None
	);

	MenuBuilder.AddMenuEntry(
		FText::FromString("Animations"),
		FText::FromString("Imports curve data, notifies and other properties"),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "GraphEditor.Animation_24x"),

		FUIAction(
			FExecuteAction::CreateLambda([] {
				TToolAnimationData* Tool = new TToolAnimationData();
				Tool->Execute();
			}),
			FCanExecuteAction::CreateLambda([this] {
				return Cloud::Status::IsOpened();
			})
		),
		NAME_None
	);

	MenuBuilder.AddMenuEntry(
		FText::FromString("Skeletal Meshes"),
		FText::FromString("Imports sockets and other properties"),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "ClassIcon.SkeletalMeshComponent"),

		FUIAction(
			FExecuteAction::CreateLambda([] {
				if (SkeletalMeshTool != nullptr) {
					SkeletalMeshTool->Execute();
				}
			}),
			FCanExecuteAction::CreateLambda([this] {
				return Cloud::Status::IsOpened();
			})
		),
		NAME_None
	);

	MenuBuilder.AddMenuEntry(
		FText::FromString("Fonts"),
		FText::FromString("Imports font properties (not vectorized data)"),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "ClassIcon.FontFace"),

		FUIAction(
			FExecuteAction::CreateLambda([] {
				TToolFontData* Tool = new TToolFontData();
				Tool->Execute();
			}),
			FCanExecuteAction::CreateLambda([this] {
				return Cloud::Status::IsOpened();
			})
		),
		NAME_None
	);
	
	MenuBuilder.AddMenuEntry(
		FText::FromString("Widget Animations"),
		FText::FromString("Imports widget animations"),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "ClassIcon.WidgetBlueprint"),

		FUIAction(
			FExecuteAction::CreateLambda([] {
				TWidgetAnimations* Tool = new TWidgetAnimations();
				Tool->Execute();
			}),
			FCanExecuteAction::CreateLambda([this] {
				return Cloud::Status::IsOpened();
			})
		),
		NAME_None
	);
	
	MenuBuilder.AddMenuEntry(
		FText::FromString("Linear Colors"),
		FText::FromString("Imports colors if any changes were made"),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "ClassIcon.CurveBase"),

		FUIAction(
			FExecuteAction::CreateLambda([] {
				TCurveLinearColorData* Tool = new TCurveLinearColorData();
				Tool->Execute();
			}),
			FCanExecuteAction::CreateLambda([this] {
				return Cloud::Status::IsOpened();
			})
		),
		NAME_None
	);

	MenuBuilder.EndSection();
}
