/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"
#include "Modules/Tools/SelectedAssetsBase.h"

/* Notifies off the game's own copy of a sequence, a class at a time.
 *
 * Nothing else about the animation is touched: not its keys, not its curves, not a property it
 * came in with. Only the notifies of the classes picked come over, and they stand in for whatever
 * the sequence already had of those classes rather than piling up beside it, so running this
 * twice leaves the animation where the first run left it.
 *
 * Which is the whole point of it being its own tool. An animation somebody has already worked on
 * in the project still wants the game's footsteps and its sound library events on it, and
 * reflecting the sequence to get them would take the work with it. */
class REFLECTION_API TToolAnimationNotifies : public TSelectedAssetsBase {
public:
	virtual void Process(UObject* Object, const TArray<TSharedPtr<FJsonValue>>& Exports) override;

	virtual FName GetSupportedClass() const override { return FName("AnimSequence"); }

	virtual FText GetDisplayName() const override { return FText::FromString("Animation Notifies"); }
	virtual FText GetTooltip() const override { return FText::FromString("Puts the notifies of the classes you pick onto the animation, leaving the rest of it as it is"); }
	virtual FSlateIcon GetIcon() const override { return FSlateIcon(FAppStyle::GetAppStyleSetName(), "ClassIcon.AnimNotify"); }
};

REGISTER_TOOL(TToolAnimationNotifies)
