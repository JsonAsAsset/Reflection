/* Copyright Reflection Contributors 2024-2026 */

#include "Modules/Toolbar/Dropdowns/DonateDropdownBuilder.h"

#include "Reflection.h"
#include "Modules/UI/StyleModule.h"
#include "Engine/EngineUtilities.h"
#include "Settings/Static.h"
#include "Utilities/Process.h"

void IDonateDropdownBuilder::Build(FMenuBuilder& MenuBuilder) const {
	MenuBuilder.BeginSection("ReflectionSupportSection", FText::FromString("Support"));
	
	MenuBuilder.AddMenuEntry(
		FText::FromString("Become A Supporter"),
		FText::FromString("Help support Reflection's development"),
		FSlateIcon(FReflectionStyle::Get().GetStyleSetName(), FName("Toolbar.Heart")),
		FUIAction(
			FExecuteAction::CreateLambda([this] {
				LaunchURL(Donation::KO_FI);
			})
		)
	);
	
	MenuBuilder.EndSection();
}
