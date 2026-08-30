/* Copyright Reflection Contributors 2024-2026 */

#include "Modules/UI/StyleModule.h"
#include "Styling/SlateStyleRegistry.h"
#include "Interfaces/IPluginManager.h"
#include "Modules/Metadata.h"
#include "Engine/EngineUtilities.h"
#include "Engine/Compatibility.h"

/* 4.25 and below build this module without the engine's shared PCH (see Reflection.Build.cs),
 * which is where the slate application used to come in from */
#if UE4_25_BELOW
#include "Framework/Application/SlateApplication.h"
#endif

#if ENGINE_UE5
#include "Styling/ToolBarStyle.h"
#include "Styling/StyleColors.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#endif

#define IMAGE_BRUSH(RelativePath, ...) FSlateImageBrush(Style->RootToContentDir(RelativePath, TEXT(".png")), __VA_ARGS__)

#if ENGINE_UE5
/* Kept either side of the reflect button's 20px icon, in place of the 8px the callout button style
 * comes with */
static constexpr float ActionButtonSidePadding = 28.0f;

/* Sides replaced, top and bottom left where the style had them: the button is only meant to grow
 * across, and its height is what lines it up with everything else on the toolbar */
static FMargin WithSidePadding(const FMargin& Padding, const float Sides) {
	return FMargin(Sides, Padding.Top, Sides, Padding.Bottom);
}

static FButtonStyle WithWiderSides(const FButtonStyle& Button, const float Sides) {
	FButtonStyle Widened = Button;

	Widened.SetNormalPadding(WithSidePadding(Button.NormalPadding, Sides));
	Widened.SetPressedPadding(WithSidePadding(Button.PressedPadding, Sides));

	return Widened;
}
#endif

const FVector2D Icon40x40(40, 40);

TSharedRef<FSlateStyleSet> FReflectionStyle::Create() {
	TSharedRef<FSlateStyleSet> Style = MakeShareable(new FSlateStyleSet("ReflectionStyle"));
	Style->SetContentRoot(FRMetadata::Plugin->GetBaseDir() / TEXT("Resources"));

	Style->Set("Toolbar.Icon", new IMAGE_BRUSH(TEXT("./Toolbar/40px"), Icon40x40));
	Style->Set("Toolbar.Heart", new IMAGE_BRUSH(TEXT("./Toolbar/Heart_40px"), Icon40x40));
	Style->Set("Toolbar.Cloud", new IMAGE_BRUSH(TEXT("./Toolbar/Cloud_40px"), Icon40x40));

	return Style;
}

TSharedPtr<FSlateStyleSet> FReflectionStyle::StyleInstance = nullptr;

void FReflectionStyle::Initialize() {
	if (!StyleInstance.IsValid()) {
		StyleInstance = Create();
		FSlateStyleRegistry::RegisterSlateStyle(*StyleInstance);
	}
}

void FReflectionStyle::Shutdown() {
	FSlateStyleRegistry::UnRegisterSlateStyle(*StyleInstance);
	ensure(StyleInstance.IsUnique());
	StyleInstance.Reset();
}

FName FReflectionStyle::GetStyleSetName() {
	static FName StyleSetName(TEXT("ReflectionStyle"));
	return StyleSetName;
}

FName FReflectionStyle::GetEmbeddedToolbarStyleName() {
	static FName EmbeddedToolbarStyleName(TEXT("Reflection.EmbeddedToolbar"));
	return EmbeddedToolbarStyleName;
}

FName FReflectionStyle::GetActionButtonStyleName() {
	static FName ActionButtonStyleName(TEXT("Reflection.CalloutToolbar"));
	return ActionButtonStyleName;
}

#if ENGINE_UE5
void FReflectionStyle::EnsureEmbeddedToolbarStyleRegistered() {
	static bool bRegistered = false;

	if (bRegistered) {
		return;
	}

	bRegistered = true;

	FToolBarStyle EmbeddedToolbarStyle = FAppStyle::Get().GetWidgetStyle<FToolBarStyle>("AssetEditorToolbar");

	EmbeddedToolbarStyle.SetBackground(FSlateRoundedBoxBrush(FStyleColors::Recessed, 8.0f));
	EmbeddedToolbarStyle.SetBackgroundPadding(FMargin(6.f, 3.f, 6.f, 3.f));

	StyleInstance->Set(GetEmbeddedToolbarStyleName(), EmbeddedToolbarStyle);

	const FToolBarStyle CalloutToolbarStyle = FAppStyle::Get().GetWidgetStyle<FToolBarStyle>("CalloutToolbar");

	StyleInstance->Set(TEXT("CalloutToolbar"), CalloutToolbarStyle);

	/* The reflect button carries no label, so the callout style leaves it barely wider than its own
	 * icon. Kept under a name of Reflection's own rather than written into the engine's
	 * CalloutToolbar, which every other toolbar asking for that name draws with too. */
	FToolBarStyle ActionButtonStyle = CalloutToolbarStyle;

	/* Widened through the button's own padding rather than the toolbar's ButtonPadding, which only
	 * spaces the button off its neighbours and leaves the button itself the size it was.
	 *
	 * Both button styles get it. The dropdown that follows the button is a simple combo box, which
	 * MultiBox reads as that button's options rather than as an entry standing on its own: the two
	 * draw as one joined pill, and the button half is handed SettingsButtonStyle in place of
	 * ButtonStyle. Widening only the latter is worth nothing while the dropdown is there. */
	ActionButtonStyle.SetButtonStyle(WithWiderSides(ActionButtonStyle.ButtonStyle, ActionButtonSidePadding));
	ActionButtonStyle.SetSettingsButtonStyle(WithWiderSides(ActionButtonStyle.SettingsButtonStyle, ActionButtonSidePadding));

	StyleInstance->Set(GetActionButtonStyleName(), ActionButtonStyle);

#if !UE5_4_BEYOND
	/* Before 5.4 a UToolMenu keeps its style set to itself: the member is private, only UToolMenus
	 * is a friend, and there is no setter. Whatever the menu is handed, it looks its style name up
	 * in FCoreStyle::Get(), which in an editor build is the app style. So the style goes in there
	 * as well, under the same name, and the menu finds it without ever being told where to look.
	 * Its copy above stays put so Get() keeps answering for it either way.
	 *
	 * The cast holds because the app style is always an FSlateStyleSet in the editor: the editor's
	 * own FStarshipEditorStyle::FStyle derives from it, and this module only builds for the editor. */
	if (FSlateStyleSet* AppStyleSet = static_cast<FSlateStyleSet*>(const_cast<ISlateStyle*>(FSlateStyleRegistry::FindSlateStyle(FAppStyle::GetAppStyleSetName())))) {
		AppStyleSet->Set(GetEmbeddedToolbarStyleName(), EmbeddedToolbarStyle);
		AppStyleSet->Set(GetActionButtonStyleName(), ActionButtonStyle);
	}
#endif
}
#endif

const ISlateStyle& FReflectionStyle::Get() {
	return *StyleInstance;
}

#undef IMAGE_BRUSH

void FReflectionStyle::ReloadTextures() {
	if (FSlateApplication::IsInitialized()) {
		FSlateApplication::Get().GetRenderer()->ReloadTextureResources();
	}
}