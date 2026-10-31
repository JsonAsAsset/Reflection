/* Copyright Reflection Contributors 2024-2026 */

#include "Modules/UI/Reflect/SRedirectorsPanel.h"

#include "Engine/Compatibility.h"

#include "Settings/Redirector.h"
#include "Settings/Runtime.h"
#include "Settings/SettingsAccess.h"

#include "Input/DragAndDrop.h"
#include "Styling/CoreStyle.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "Reflection.Redirectors"

#if ENGINE_UE5
static const FName CrossButtonStyleName("SimpleButton");
static const FName CrossIconName("Icons.X");
#else
static const FName CrossButtonStyleName("HoverHintOnly");
static const FName CrossIconName("Symbols.X");
#endif

static FSlateColor GetCrossIconColor() {
#if ENGINE_UE5
	return FSlateColor::UseForeground();
#else
	return FAppStyle::Get().GetSlateColor("DefaultForeground");
#endif
}

/* A cross, which is the only way anything here is taken back out */
static TSharedRef<SWidget> MakeCross(const FText& Tooltip, const FOnClicked& Clicked) {
	return SNew(SButton)
		.ButtonStyle(FAppStyle::Get(), CrossButtonStyleName)
		.ContentPadding(FMargin(2.0f))
		.ToolTipText(Tooltip)
		.OnClicked(Clicked)
		[
			SNew(SImage)
			.Image(FAppStyle::Get().GetBrush(CrossIconName))
			.ColorAndOpacity(GetCrossIconColor())
		];
}

/* A redirector being moved somewhere else in the order.
 *
 * Only where it came from travels. Where it is going is whatever row it was let go over, which the
 * row that takes the drop is the one to say. */
class FRedirectorDragDropOp : public FDragDropOperation {
public:
	DRAG_DROP_OPERATOR_TYPE(FRedirectorDragDropOp, FDragDropOperation)

	int32 From = INDEX_NONE;

	FText Named;

	virtual TSharedPtr<SWidget> GetDefaultDecorator() const override {
		return SNew(SBorder)
			.BorderImage(FAppStyle::Get().GetBrush("ToolPanel.GroupBorder"))
			.Padding(FMargin(8.0f, 4.0f))
			[
				SNew(STextBlock)
				.Text(Named)
			];
	}

	static TSharedRef<FRedirectorDragDropOp> New(const int32 From, const FText& Named) {
		const TSharedRef<FRedirectorDragDropOp> Dragged = MakeShared<FRedirectorDragDropOp>();

		Dragged->From = From;
		Dragged->Named = Named;
		Dragged->Construct();

		return Dragged;
	}
};

void SRedirectorsPanel::Construct(const FArguments& InArgs) {
	OnChanged = InArgs._OnChanged;
	LandingPaths = InArgs._LandingPaths;

	ChildSlot
	[
		SNew(SVerticalBox)

		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(FMargin(8.0f, 6.0f, 8.0f, 0.0f))
		[
			SNew(STextBlock)
			.Text(LOCTEXT(
				"What",
				"An asset is fetched from the path it was queued under and written where these say. They run top to bottom, and each one works on what the one above it left, so dragging one up or down changes what it is given."
			))
			.AutoWrapText(true)
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
		]

		/* Said where the list would be, since an empty list is the one case it has nothing to show */
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(FMargin(8.0f, 8.0f))
		[
			SNew(STextBlock)
			.Text(LOCTEXT(
				"Empty",
				"Nothing is redirected. An asset is written where its own path says, which is what a build that was read straight through wants."
			))
			.AutoWrapText(true)
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			.Visibility(this, &SRedirectorsPanel::GetEmptyVisibility)
		]

		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		.Padding(FMargin(8.0f, 6.0f))
		[
			SAssignNew(List, SListView<FRowItem>)
			.ListItemsSource(&Items)

			/* Single, because a row cannot start a drag without one: the row only takes the mouse
			 * when it has something to select */
			.SelectionMode(ESelectionMode::Single)
			.OnGenerateRow(this, &SRedirectorsPanel::GenerateRow)
		]

		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(FMargin(8.0f, 0.0f, 8.0f, 8.0f))
		[
			SNew(SHorizontalBox)

			+ SHorizontalBox::Slot()
			.AutoWidth()
			[
				SNew(SButton)
				.Text(LOCTEXT("AddRedirector", "Add Redirector"))
				.ToolTipText(LOCTEXT("AddRedirectorTooltip", "Add one with no rules under it yet. It does nothing until it has one."))
				.OnClicked(this, &SRedirectorsPanel::OnAddRedirectorClicked)
			]

			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.HAlign(HAlign_Right)
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("Saved", "Saved as you type"))
				.Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			]
		]
	];

	Refresh();
}

void SRedirectorsPanel::Refresh() {
	const UReflectionSettings* Settings = GetSettings();

	const int32 Count = Settings != nullptr ? Settings->Redirectors.Num() : 0;

	Items.Reset(Count);

	for (int32 Index = 0; Index < Count; ++Index) {
		Items.Add(MakeShared<int32>(Index));
	}

	/* Rebuilt rather than refreshed: a row is the card for whatever sits at its index, and after a
	 * drag that is a different redirector */
	if (List.IsValid()) {
		List->RebuildList();
	}
}

EVisibility SRedirectorsPanel::GetEmptyVisibility() const {
	return Items.Num() == 0 ? EVisibility::Visible : EVisibility::Collapsed;
}

TSharedRef<ITableRow> SRedirectorsPanel::GenerateRow(const FRowItem Item, const TSharedRef<STableViewBase>& OwnerTable) {
	return SNew(STableRow<FRowItem>, OwnerTable)
		.Padding(FMargin(0.0f, 0.0f, 0.0f, 6.0f))
		.OnDragDetected(this, &SRedirectorsPanel::OnRowDragDetected, Item)
		.OnCanAcceptDrop(this, &SRedirectorsPanel::OnRowCanAcceptDrop)
		.OnAcceptDrop(this, &SRedirectorsPanel::OnRowAcceptDrop)
		[
			BuildCard(Item.IsValid() ? *Item : INDEX_NONE)
		];
}

TSharedRef<SWidget> SRedirectorsPanel::BuildCard(const int32 Index) {
	UReflectionSettings* Settings = GetSettings();

	if (Settings == nullptr || !Settings->Redirectors.IsValidIndex(Index)) {
		return SNullWidget::NullWidget;
	}

	const FRRedirector& Redirect = Settings->Redirectors[Index];

	const TSharedRef<SVerticalBox> Rules = SNew(SVerticalBox);

	for (int32 At = 0; At < Redirect.Points.Num(); ++At) {
		Rules->AddSlot()
		.AutoHeight()
		.Padding(FMargin(0.0f, 1.0f))
		[
			BuildRule(Index, At)
		];
	}

	/* A redirector with no rules reads as broken rather than new, so it says which it is */
	if (Redirect.Points.Num() == 0) {
		Rules->AddSlot()
		.AutoHeight()
		.Padding(FMargin(2.0f, 2.0f))
		[
			SNew(STextBlock)
			.Text(LOCTEXT("NoRules", "No rules yet, so this one moves nothing."))
			.Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
		];
	}

	return SNew(SBorder)
		.BorderImage(FAppStyle::Get().GetBrush("ToolPanel.GroupBorder"))
		.Padding(FMargin(6.0f, 4.0f))
		[
			SNew(SVerticalBox)

			/* Header: where it sits, what it is called, whether it is on, and what it is doing */
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(SHorizontalBox)

				/* Says the row can be taken hold of. It draws only: the drag comes off the row
				 * underneath it, which is what anywhere on the card that is not a control does. */
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(FMargin(0.0f, 0.0f, 4.0f, 0.0f))
				[
					SNew(SImage)
					.Image(FCoreStyle::Get().GetBrush("VerticalBoxDragIndicatorShort"))
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				]

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				[
					SNew(SCheckBox)
					.ToolTipText(LOCTEXT("OnTooltip", "Off, this one is skipped and its rules are left where they are."))
					.IsChecked(Redirect.IsOn() ? ECheckBoxState::Checked : ECheckBoxState::Unchecked)
					.OnCheckStateChanged_Lambda([this, Index](const ECheckBoxState NewState) {
						UReflectionSettings* Changed = GetSettings();

						if (Changed == nullptr || !Changed->Redirectors.IsValidIndex(Index)) return;

						Changed->Redirectors[Index].SetOn(NewState == ECheckBoxState::Checked);

						Commit();
						Refresh();
					})
				]

				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.VAlign(VAlign_Center)
				.Padding(FMargin(4.0f, 0.0f, 0.0f, 0.0f))
				[
					SNew(SEditableTextBox)
					.HintText(LOCTEXT("NameHint", "Name"))
					.ToolTipText(LOCTEXT("NameTooltip", "What a moved path says it was moved by. Nothing depends on it."))
					.Text(FText::FromName(Redirect.GetName()))
					.OnTextCommitted_Lambda([this, Index](const FText& NewText, ETextCommit::Type) {
						UReflectionSettings* Changed = GetSettings();

						if (Changed == nullptr || !Changed->Redirectors.IsValidIndex(Index)) return;

						Changed->Redirectors[Index].SetName(FName(*NewText.ToString().TrimStartAndEnd()));

						Commit();
					})
				]

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(FMargin(8.0f, 0.0f, 0.0f, 0.0f))
				[
					SNew(STextBlock)
					.Text_Lambda([this, Index] { return EffectText(Index); })
					.Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				]

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(FMargin(4.0f, 0.0f, 0.0f, 0.0f))
				[
					MakeCross(
						LOCTEXT("RemoveRedirectorTooltip", "Take this redirector out"),
						FOnClicked::CreateSP(this, &SRedirectorsPanel::OnRemoveRedirectorClicked, Index)
					)
				]
			]

			/* The rules themselves, indented under the thing they belong to */
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(22.0f, 4.0f, 0.0f, 0.0f))
			[
				Rules
			]

			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(22.0f, 4.0f, 0.0f, 0.0f))
			[
				SNew(SHorizontalBox)

				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					SNew(SButton)
					.Text(LOCTEXT("AddRule", "Add Rule"))
					.ToolTipText(LOCTEXT("AddRuleTooltip", "Another From and To under this one. They are applied in order."))
					.ContentPadding(FMargin(6.0f, 1.0f))
					.OnClicked(this, &SRedirectorsPanel::OnAddRuleClicked, Index)
				]

				/* Which profiles it is for, since a redirector that is on and still doing nothing
				 * is almost always one kept to a profile that is not loaded */
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(FMargin(12.0f, 0.0f, 4.0f, 0.0f))
				[
					SNew(STextBlock)
					.Text(LOCTEXT("Profiles", "Profiles"))
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				]

				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.VAlign(VAlign_Center)
				[
					SNew(SEditableTextBox)
					.HintText(LOCTEXT("ProfilesHint", "every profile"))
					.ToolTipText(LOCTEXT("ProfilesTooltip", "Kept to these profiles, by name, separated by commas. Empty is every profile."))
					.Text(FText::FromString(FString::Join(Redirect.GetProfiles(), TEXT(", "))))
					.OnTextCommitted_Lambda([this, Index](const FText& NewText, ETextCommit::Type) {
						UReflectionSettings* Changed = GetSettings();

						if (Changed == nullptr || !Changed->Redirectors.IsValidIndex(Index)) return;

						TArray<FString> Named;
						NewText.ToString().ParseIntoArray(Named, TEXT(","));

						for (FString& One : Named) {
							One.TrimStartAndEndInline();
						}

						Named.Remove(FString());

						Changed->Redirectors[Index].SetProfiles(Named);

						Commit();
						Refresh();
					})
				]
			]
		];
}

TSharedRef<SWidget> SRedirectorsPanel::BuildRule(const int32 Index, const int32 At) {
	UReflectionSettings* Settings = GetSettings();

	if (Settings == nullptr || !Settings->Redirectors.IsValidIndex(Index)) {
		return SNullWidget::NullWidget;
	}

	const TArray<FRRedirectorPoint>& Points = Settings->Redirectors[Index].Points;

	if (!Points.IsValidIndex(At)) {
		return SNullWidget::NullWidget;
	}

	const FRRedirectorPoint& Point = Points[At];

	return SNew(SHorizontalBox)

		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.VAlign(VAlign_Center)
		[
			SNew(SEditableTextBox)
			.HintText(LOCTEXT("FromHint", "/Game/From/"))
			.ToolTipText(LOCTEXT("FromTooltip", "Matched anywhere in the path, the first time it appears. A folder needs its slashes or it matches halfway through a name."))
			.Text(FText::FromString(Point.From))
			.OnTextCommitted_Lambda([this, Index, At](const FText& NewText, ETextCommit::Type) {
				UReflectionSettings* Changed = GetSettings();

				if (Changed == nullptr || !Changed->Redirectors.IsValidIndex(Index)) return;
				if (!Changed->Redirectors[Index].Points.IsValidIndex(At)) return;

				Changed->Redirectors[Index].Points[At].From = NewText.ToString().TrimStartAndEnd();

				Commit();
				Refresh();
			})
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(FMargin(6.0f, 0.0f))
		[
			SNew(STextBlock)
			.Text(LOCTEXT("Becomes", "becomes"))
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
		]

		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.VAlign(VAlign_Center)
		[
			SNew(SEditableTextBox)
			.HintText(LOCTEXT("ToHint", "/Game/To/"))
			.ToolTipText(LOCTEXT("ToTooltip", "What takes its place. Empty cuts the matched part out of the path."))
			.Text(FText::FromString(Point.To))
			.OnTextCommitted_Lambda([this, Index, At](const FText& NewText, ETextCommit::Type) {
				UReflectionSettings* Changed = GetSettings();

				if (Changed == nullptr || !Changed->Redirectors.IsValidIndex(Index)) return;
				if (!Changed->Redirectors[Index].Points.IsValidIndex(At)) return;

				Changed->Redirectors[Index].Points[At].To = NewText.ToString().TrimStartAndEnd();

				Commit();
				Refresh();
			})
		]

		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		.Padding(FMargin(4.0f, 0.0f, 0.0f, 0.0f))
		[
			MakeCross(
				LOCTEXT("RemoveRuleTooltip", "Take this rule out"),
				FOnClicked::CreateSP(this, &SRedirectorsPanel::OnRemoveRuleClicked, Index, At)
			)
		];
}

FText SRedirectorsPanel::EffectText(const int32 Index) const {
	const UReflectionSettings* Settings = GetSettings();

	if (Settings == nullptr || !Settings->Redirectors.IsValidIndex(Index)) return FText::GetEmpty();

	const FRRedirector& Redirect = Settings->Redirectors[Index];

	/* Said by the checkbox already */
	if (!Redirect.IsOn()) return FText::GetEmpty();

	/* On and skipped anyway, which is worth saying where it is happening rather than leaving it
	 * to be worked out from a count of nothing */
	if (!Redirect.IsEnabled()) {
		return GReflectionRuntime.Profile.Name.IsEmpty()
			? LOCTEXT("NoProfile", "waiting on a profile")
			: FText::Format(LOCTEXT("NotThisProfileFmt", "not for {0}"), FText::FromString(GReflectionRuntime.Profile.Name));
	}

	if (!LandingPaths.IsBound()) return FText::GetEmpty();

	const TArray<FString> Landing = LandingPaths.Execute();

	if (Landing.Num() == 0) return FText::GetEmpty();

	int32 Moved = 0;

	for (const FString& Path : Landing) {
		/* Asked of the whole run rather than of this one alone, so a rule that only matches what
		 * an earlier redirector left is counted where it actually applies */
		if (FRRedirects::Preview(Path).Indices.Contains(Index)) {
			Moved++;
		}
	}

	if (Moved == 0) {
		return FText::Format(LOCTEXT("MovesNoneFmt", "none of the {0} queued"), FText::AsNumber(Landing.Num()));
	}

	return FText::Format(LOCTEXT("MovesFmt", "moves {0} of {1} queued"), FText::AsNumber(Moved), FText::AsNumber(Landing.Num()));
}

/* Dragging ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
FReply SRedirectorsPanel::OnRowDragDetected(const FGeometry& Geometry, const FPointerEvent& MouseEvent, const FRowItem Item) {
	const UReflectionSettings* Settings = GetSettings();

	if (!Item.IsValid() || Settings == nullptr || !Settings->Redirectors.IsValidIndex(*Item)) {
		return FReply::Unhandled();
	}

	const FName Named = Settings->Redirectors[*Item].GetName();

	return FReply::Handled().BeginDragDrop(FRedirectorDragDropOp::New(
		*Item,
		Named.IsNone() ? LOCTEXT("DraggingUnnamed", "Redirector") : FText::FromName(Named)
	));
}

TOptional<EItemDropZone> SRedirectorsPanel::OnRowCanAcceptDrop(const FDragDropEvent& DragDropEvent, const EItemDropZone Zone, const FRowItem Item) {
	if (!DragDropEvent.GetOperationAs<FRedirectorDragDropOp>().IsValid()) {
		return TOptional<EItemDropZone>();
	}

	/* Onto one of these means nothing. They are a sequence, and the only thing a drop can say is
	 * where in it this one goes, so the middle of a row counts as below it. */
	return Zone == EItemDropZone::OntoItem ? EItemDropZone::BelowItem : Zone;
}

FReply SRedirectorsPanel::OnRowAcceptDrop(const FDragDropEvent& DragDropEvent, const EItemDropZone Zone, const FRowItem Item) {
	const TSharedPtr<FRedirectorDragDropOp> Dragged = DragDropEvent.GetOperationAs<FRedirectorDragDropOp>();

	UReflectionSettings* Settings = GetSettings();

	if (!Dragged.IsValid() || !Item.IsValid() || Settings == nullptr) return FReply::Unhandled();

	const int32 From = Dragged->From;

	if (!Settings->Redirectors.IsValidIndex(From) || !Settings->Redirectors.IsValidIndex(*Item)) {
		return FReply::Unhandled();
	}

	/* Above the row it was let go over means in its place, below it means after it */
	int32 To = Zone == EItemDropZone::AboveItem ? *Item : *Item + 1;

	/* Lifting it out closes the gap, so everything after where it was shifts up one */
	if (From < To) {
		To--;
	}

	if (From == To) return FReply::Handled();

	const FRRedirector Moved = Settings->Redirectors[From];

	Settings->Redirectors.RemoveAt(From);
	Settings->Redirectors.Insert(Moved, FMath::Clamp(To, 0, Settings->Redirectors.Num()));

	Commit();
	Refresh();

	return FReply::Handled();
}

/* Changes ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
FReply SRedirectorsPanel::OnAddRedirectorClicked() {
	UReflectionSettings* Settings = GetSettings();

	if (Settings == nullptr) return FReply::Handled();

	FRRedirector Added;

	/* One empty rule, since a redirector without one is a thing that cannot be typed into */
	Added.Points.Add(FRRedirectorPoint());

	Settings->Redirectors.Add(Added);

	Commit();
	Refresh();

	return FReply::Handled();
}

FReply SRedirectorsPanel::OnRemoveRedirectorClicked(const int32 Index) {
	UReflectionSettings* Settings = GetSettings();

	if (Settings == nullptr || !Settings->Redirectors.IsValidIndex(Index)) return FReply::Handled();

	Settings->Redirectors.RemoveAt(Index);

	Commit();
	Refresh();

	return FReply::Handled();
}

FReply SRedirectorsPanel::OnAddRuleClicked(const int32 Index) {
	UReflectionSettings* Settings = GetSettings();

	if (Settings == nullptr || !Settings->Redirectors.IsValidIndex(Index)) return FReply::Handled();

	Settings->Redirectors[Index].Points.Add(FRRedirectorPoint());

	Commit();
	Refresh();

	return FReply::Handled();
}

FReply SRedirectorsPanel::OnRemoveRuleClicked(const int32 Index, const int32 At) {
	UReflectionSettings* Settings = GetSettings();

	if (Settings == nullptr || !Settings->Redirectors.IsValidIndex(Index)) return FReply::Handled();
	if (!Settings->Redirectors[Index].Points.IsValidIndex(At)) return FReply::Handled();

	Settings->Redirectors[Index].Points.RemoveAt(At);

	Commit();
	Refresh();

	return FReply::Handled();
}

void SRedirectorsPanel::Commit() {
	if (UReflectionSettings* Settings = GetSettings()) {
		Settings->SaveConfig();
	}

	/* A path already queued was worked out against the redirectors as they were, so whoever is
	 * holding that queue is told to work it out again */
	OnChanged.ExecuteIfBound();
}

#undef LOCTEXT_NAMESPACE
