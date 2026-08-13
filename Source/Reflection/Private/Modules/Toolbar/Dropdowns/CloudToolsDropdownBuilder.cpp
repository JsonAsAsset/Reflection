/* Copyright Reflection Contributors 2024-2026 */

#include "Modules/Toolbar/Dropdowns/CloudToolsDropdownBuilder.h"

#include "Algo/Sort.h"

#include "Modules/Cloud/Cloud.h"
#include "Modules/Tools/ToolRegistry.h"

/* Every tool registers from its own header, so including them is what puts them in the menu */
#include "Modules/Cloud/Tools/AnimationData.h"
#include "Modules/Cloud/Tools/ClothingData.h"
#include "Modules/Cloud/Tools/ConvexCollision.h"
#include "Modules/Cloud/Tools/CurveLinearColorData.h"
#include "Modules/Cloud/Tools/FontData.h"
#include "Modules/Cloud/Tools/SkeletalMeshData.h"
#include "Modules/Cloud/Tools/WidgetAnimations.h"

void ICloudToolsDropdownBuilder::Build(FMenuBuilder& MenuBuilder) const {
	/* Everything in here works off whatever is selected in the content browser. Reflecting by path
	 * or by folder doesn't, so those live on the reflect button instead. */
	MenuBuilder.BeginSection("ReflectionCloudSection", FText::FromString("Selected Assets"));

	TArray<TSelectedAssetsBase*> Tools;

	for (const TPair<FName, FToolInstanceDelegate>& Registration : GetToolRegistry()) {
		TSelectedAssetsBase* Tool = Registration.Value();

		/* A tool with no name isn't meant to be reachable from the menu */
		if (Tool == nullptr || Tool->GetDisplayName().IsEmpty()) continue;

		Tools.Add(Tool);
	}

	/* Registration order is the linker's business, so the menu sorts itself */
	Algo::Sort(Tools, [](const TSelectedAssetsBase* A, const TSelectedAssetsBase* B) {
		return A->GetDisplayName().CompareTo(B->GetDisplayName()) < 0;
	});

	for (TSelectedAssetsBase* Tool : Tools) {
		MenuBuilder.AddMenuEntry(
			Tool->GetDisplayName(),
			Tool->GetTooltip(),
			Tool->GetIcon(),

			FUIAction(
				FExecuteAction::CreateLambda([Tool] {
					Tool->Execute();
				}),

				/* Menu entries are only useful with something on the other end, and a tool that is
				 * still working would refuse a second run anyway */
				FCanExecuteAction::CreateLambda([Tool] {
					return Cloud::Status::IsOpened() && !Tool->IsRunning();
				})
			),
			NAME_None
		);
	}

	MenuBuilder.EndSection();
}
