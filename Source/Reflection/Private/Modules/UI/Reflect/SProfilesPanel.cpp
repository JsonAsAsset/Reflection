/* Copyright Reflection Contributors 2024-2026 */

#include "Modules/UI/Reflect/SProfilesPanel.h"

#include "Engine/Compatibility.h"
#include "Engine/EngineUtilities.h"

#include "Settings/Runtime.h"
#include "Settings/Static.h"
#include "Utilities/Process.h"

#include "Framework/Application/SlateApplication.h"
#include "Styling/CoreStyle.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "Reflection.Profiles"

#if ENGINE_UE5
static const FName ReadingIconName("Icons.Check");
#else
static const FName ReadingIconName("NotificationList.SuccessImage");
#endif

/* The one the page is about is marked rather than labelled, so every name starts at the same
 * place down the list */
constexpr float MarkWidth = 18.0f;

/* A profile that was asked to open and never arrives: Mirror could not read it, or is still
 * trying and nothing here can tell the two apart */
constexpr double GiveUpOpeningAfterSeconds = 300.0;

/* Mirror says nothing when a profile finishes opening, so it is asked this often instead */
constexpr double BetweenAsksSeconds = 1.0;

static const FString Separator = TEXT(" \x2022 ");

/* GAME_UE5_4 is how Mirror spells a version, which is not how anybody says one */
static FString ReadableVersion(const FString& Spelled) {
	FString Named = Spelled;

	Named.RemoveFromStart(TEXT("GAME_"));
	Named.ReplaceInline(TEXT("UE4_"), TEXT("UE 4."));
	Named.ReplaceInline(TEXT("UE5_"), TEXT("UE 5."));

	return Named;
}

/* The order Mirror lists them in, so the two read the same way round.
 *
 * Anything whose name is not a version comes first, by name, and the versions follow newest
 * first. Which is Mirror's own ordering: a name that parses as a number is treated as the
 * version it is, and one that does not is treated as a name. */
static bool SortsBefore(const Cloud::FBuild& Left, const Cloud::FBuild& Right) {
	const bool LeftIsVersion = Left.Name.IsNumeric();
	const bool RightIsVersion = Right.Name.IsNumeric();

	if (LeftIsVersion != RightIsVersion) {
		return RightIsVersion;
	}

	if (LeftIsVersion) {
		const double LeftVersion = FCString::Atod(*Left.Name);
		const double RightVersion = FCString::Atod(*Right.Name);

		if (!FMath::IsNearlyEqual(LeftVersion, RightVersion)) {
			return LeftVersion > RightVersion;
		}
	}

	return Left.Name.Compare(Right.Name, ESearchCase::IgnoreCase) < 0;
}

/* What a profile is worth saying about itself under its own name */
static FText DetailOf(const Cloud::FBuild& Build) {
	TArray<FString> Said;

	if (!Build.Version.IsEmpty()) {
		Said.Add(ReadableVersion(Build.Version));
	}

	/* Which one Mirror is showing is not the same question as which one the editor reads, so it
	 * is said here rather than marked */
	if (Build.Open) {
		Said.Add(TEXT("open in Mirror"));
	} else if (Build.Loaded) {
		Said.Add(TEXT("loaded"));
	}

	if (Build.Loaded && Build.Files > 0) {
		Said.Add(FString::Printf(TEXT("%d files"), Build.Files));
	}

	return FText::FromString(FString::Join(Said, *Separator));
}

void SProfilesPanel::Construct(const FArguments& InArgs) {
	OnChanged = InArgs._OnChanged;

	ChildSlot
	[
		SNew(SVerticalBox)

		/* What is being read, which is the one thing this page is here to answer. Framed the way
		 * the other pages frame what they are for, so it reads as part of the same window. */
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(FMargin(8.0f, 8.0f, 8.0f, 0.0f))
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::Get().GetBrush("ToolPanel.GroupBorder"))
			.Padding(FMargin(10.0f, 8.0f))
			[
				SNew(SHorizontalBox)

				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.VAlign(VAlign_Center)
				[
					SNew(SVerticalBox)

					+ SVerticalBox::Slot()
					.AutoHeight()
					[
						SNew(STextBlock)
						.Text(this, &SProfilesPanel::GetHeadlineText)
						.Font(FCoreStyle::GetDefaultFontStyle("Bold", 11))
					]

					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(FMargin(0.0f, 2.0f, 0.0f, 0.0f))
					[
						SNew(STextBlock)
						.Text(this, &SProfilesPanel::GetDetailText)
						.Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
						.ColorAndOpacity(FSlateColor::UseSubduedForeground())
						.AutoWrapText(true)
					]
				]

				/* Only there when the editor is reading somewhere Mirror is not looking */
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(FMargin(8.0f, 0.0f, 0.0f, 0.0f))
				[
					SNew(SButton)
					.Text(LOCTEXT("FollowMirror", "Follow Mirror"))
					.ToolTipText(LOCTEXT("FollowMirrorTooltip", "Go back to reading whichever profile Mirror has open, rather than the one pinned here."))
					.ContentPadding(FMargin(8.0f, 2.0f))
					.Visibility(this, &SProfilesPanel::GetPinnedVisibility)
					.IsEnabled(this, &SProfilesPanel::IsIdle)
					.OnClicked(this, &SProfilesPanel::OnFollowMirrorClicked)
				]

				/* Only there when there is nothing to refresh */
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(FMargin(8.0f, 0.0f, 0.0f, 0.0f))
				[
					SNew(SButton)
					.Text(LOCTEXT("Setup", "How to set up"))
					.ToolTipText(LOCTEXT("SetupTooltip", "Read how a Cloud instance is set up and started."))
					.ContentPadding(FMargin(8.0f, 2.0f))
					.Visibility(this, &SProfilesPanel::GetNotRunningVisibility)
					.OnClicked(this, &SProfilesPanel::OnSetupClicked)
				]

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(FMargin(6.0f, 0.0f, 0.0f, 0.0f))
				[
					SNew(SButton)
					.Text(LOCTEXT("Refresh", "Refresh"))
					.ToolTipText(LOCTEXT("RefreshTooltip", "Ask Mirror what it has again. A profile made while the editor was open shows up here."))
					.ContentPadding(FMargin(8.0f, 2.0f))
					.IsEnabled(this, &SProfilesPanel::IsIdle)
					.OnClicked(this, &SProfilesPanel::OnRefreshClicked)
				]
			]
		]

		/* The rest of them, in the same frame the other pages list things in */
		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		.Padding(FMargin(8.0f, 6.0f, 8.0f, 8.0f))
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::Get().GetBrush("ToolPanel.GroupBorder"))
			.Padding(FMargin(2.0f))
			[
				SNew(SOverlay)

				+ SOverlay::Slot()
				[
					SAssignNew(List, SListView<FRowItem>)
					.ListItemsSource(&Items)
					.SelectionMode(ESelectionMode::None)
					.OnGenerateRow(this, &SProfilesPanel::GenerateRow)
					.Visibility(this, &SProfilesPanel::GetRunningVisibility)
				]

				/* Inside the frame rather than above it, so a page with nothing to list still has
				 * the shape of one that has */
				+ SOverlay::Slot()
				.HAlign(HAlign_Center)
				.VAlign(VAlign_Center)
				[
					SNew(SBox)
					.Padding(FMargin(24.0f, 0.0f))
					[
						SNew(STextBlock)
						.Text(this, &SProfilesPanel::GetEmptyText)
						.Justification(ETextJustify::Center)
						.AutoWrapText(true)
						.ColorAndOpacity(FSlateColor::UseSubduedForeground())
						.Visibility(this, &SProfilesPanel::GetEmptyVisibility)
					]
				]
			]
		]
	];

	/* Not asked for here. The window this sits in is modal, so nothing is on screen until the
	 * page is opened, and that is when it is worth a request. */
}

void SProfilesPanel::Refresh() {
	if (!Cloud::Status::IsOpened()) {
		Items.Reset();

		Asked = true;

		if (List.IsValid()) {
			List->RebuildList();
		}

		return;
	}

	BeginWaiting(FText::GetEmpty(), FString());

	TWeakPtr<SProfilesPanel> Weak = SharedThis(this);

	Cloud::Builds::ListAsync([Weak](const TArray<Cloud::FBuild>& Builds) {
		const TSharedPtr<SProfilesPanel> Panel = Weak.Pin();

		/* The window was shut while Mirror was answering */
		if (!Panel.IsValid()) return;

		Panel->Items.Reset(Builds.Num());

		for (const Cloud::FBuild& Build : Builds) {
			Panel->Items.Add(MakeShared<Cloud::FBuild>(Build));
		}

		Panel->Items.Sort([](const FRowItem& Left, const FRowItem& Right) {
			if (!Left.IsValid() || !Right.IsValid()) return Left.IsValid();

			return SortsBefore(*Left, *Right);
		});

		Panel->Asked = true;

		Panel->FinishWaiting();
	});
}

bool SProfilesPanel::IsReadingFrom(const Cloud::FBuild& Build) const {
	/* Nothing pinned, so whatever Mirror has open is what answers */
	return Cloud::Build.IsEmpty() ? Build.Open : Cloud::Build == Build.Id;
}

/* State ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
FText SProfilesPanel::GetHeadlineText() const {
	if (!Cloud::Status::IsOpened()) {
		return LOCTEXT("HeadlineClosed", "Mirror is not running");
	}

	if (GReflectionRuntime.Profile.Name.IsEmpty()) {
		return Asked ? LOCTEXT("HeadlineNone", "No profile open") : LOCTEXT("HeadlineAsking", "Asking Mirror");
	}

	return FText::FromString(GReflectionRuntime.Profile.Name);
}

FText SProfilesPanel::GetDetailText() const {
	if (!Cloud::Status::IsOpened()) {
		return LOCTEXT("DetailClosed", "Start it, then press Refresh");
	}

	if (GReflectionRuntime.Profile.Name.IsEmpty()) {
		return LOCTEXT("DetailNone", "Pick one below and everything here is read out of it");
	}

	TArray<FString> Said;

	/* The project is the half that matters to a path, so it is said under the name rather than
	 * left to be worked out from it */
	if (!GReflectionRuntime.Profile.ProjectName.IsEmpty()) {
		Said.Add(GReflectionRuntime.Profile.ProjectName);
	}

	if (GReflectionRuntime.MajorVersion > 0) {
		Said.Add(FString::Printf(TEXT("UE %d.%d"), GReflectionRuntime.MajorVersion, GReflectionRuntime.MinorVersion));
	}

	if (!Cloud::Build.IsEmpty()) {
		Said.Add(TEXT("pinned here, not open in Mirror"));
	}

	return FText::FromString(FString::Join(Said, *Separator));
}

FText SProfilesPanel::GetEmptyText() const {
	if (!Cloud::Status::IsOpened()) {
		return LOCTEXT("EmptyClosed", "There is nothing to list while Mirror is shut");
	}

	return LOCTEXT("EmptyNone", "Mirror has no profiles to read from");
}

EVisibility SProfilesPanel::GetRunningVisibility() const {
	return Cloud::Status::IsOpened() ? EVisibility::Visible : EVisibility::Collapsed;
}

EVisibility SProfilesPanel::GetNotRunningVisibility() const {
	return Cloud::Status::IsOpened() ? EVisibility::Collapsed : EVisibility::Visible;
}

EVisibility SProfilesPanel::GetEmptyVisibility() const {
	return Items.Num() == 0 ? EVisibility::Visible : EVisibility::Collapsed;
}

EVisibility SProfilesPanel::GetPinnedVisibility() const {
	return Cloud::Build.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible;
}

/* Rows ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
TSharedRef<ITableRow> SProfilesPanel::GenerateRow(const FRowItem Item, const TSharedRef<STableViewBase>& OwnerTable) {
	if (!Item.IsValid()) {
		return SNew(STableRow<FRowItem>, OwnerTable);
	}

	const Cloud::FBuild& Build = *Item;
	const bool Reading = IsReadingFrom(Build);

	const TSharedRef<SHorizontalBox> Actions = SNew(SHorizontalBox);

	/* The row Mirror is still answering for says so where its buttons were, rather than leaving
	 * them there to be pressed again */
	if (!BusyId.IsEmpty() && BusyId == Build.Id) {
		Actions->AddSlot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		[
			SNew(STextBlock)
			.Text(this, &SProfilesPanel::GetBusyText)
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
		];
	} else {
		if (!Reading) {
			Actions->AddSlot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(SButton)
				.Text(LOCTEXT("ReadFrom", "Read From"))
				.ToolTipText(LOCTEXT("ReadFromTooltip", "Reflect out of this one from now on. Mirror stays on whatever it has open, and this one's files are mounted first if it has let them go."))
				.ContentPadding(FMargin(8.0f, 1.0f))
				.IsEnabled(this, &SProfilesPanel::IsIdle)
				.OnClicked(this, &SProfilesPanel::OnReadFromClicked, Item)
			];
		}

		/* Moving Mirror is the heavier of the two, so it is never the one on the left */
		if (!Build.Open) {
			Actions->AddSlot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(FMargin(4.0f, 0.0f, 0.0f, 0.0f))
			[
				SNew(SButton)
				.Text(LOCTEXT("OpenInMirror", "Open in Mirror"))
				.ToolTipText(LOCTEXT("OpenInMirrorTooltip", "Ask Mirror to open this one, so its own window moves to it as well. Whatever it had open is closed."))
				.ContentPadding(FMargin(8.0f, 1.0f))
				.IsEnabled(this, &SProfilesPanel::IsIdle)
				.OnClicked(this, &SProfilesPanel::OnOpenInMirrorClicked, Item)
			];
		}

		/* Letting go of what is being read, or of what Mirror has open, is not on offer */
		if (Build.Loaded && !Build.Open && !Reading) {
			Actions->AddSlot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(FMargin(4.0f, 0.0f, 0.0f, 0.0f))
			[
				SNew(SButton)
				.Text(LOCTEXT("Unload", "Unload"))
				.ToolTipText(LOCTEXT("UnloadTooltip", "Let its files go. Anything naming this profile mounts it again."))
				.ContentPadding(FMargin(8.0f, 1.0f))
				.IsEnabled(this, &SProfilesPanel::IsIdle)
				.OnClicked(this, &SProfilesPanel::OnUnloadClicked, Item)
			];
		}
	}

	return SNew(STableRow<FRowItem>, OwnerTable)
		.ToolTipText(Reading
			? LOCTEXT("ReadingRowTooltip", "The profile everything here is read out of")
			: FText::GetEmpty())
		[
			SNew(SHorizontalBox)

			/* The one being read is marked rather than labelled, so every name down the list
			 * starts at the same place */
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(FMargin(6.0f, 0.0f, 0.0f, 0.0f))
			[
				SNew(SBox)
				.WidthOverride(MarkWidth)
				.HAlign(HAlign_Center)
				[
					SNew(SImage)
					.Image(FAppStyle::Get().GetBrush(ReadingIconName))
					.Visibility(Reading ? EVisibility::Visible : EVisibility::Hidden)
				]
			]

			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			.Padding(FMargin(4.0f, 6.0f))
			[
				SNew(SVerticalBox)

				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.Text(FText::FromString(Build.Name.IsEmpty() ? Build.Id : Build.Name))
				]

				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.Text(DetailOf(Build))
					.Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				]
			]

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(FMargin(4.0f, 0.0f, 6.0f, 0.0f))
			[
				Actions
			]
		];
}

/* Waiting ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
void SProfilesPanel::BeginWaiting(const FText& What, const FString& Id) {
	BusyWhat = What;
	BusyId = Id;

	/* Slate is the only thing being ticked in here, so the requests are driven off a Slate timer.
	 * Started when there is something to drive rather than left running, since a page with nothing
	 * outstanding has no business ticking the HTTP manager for the rest of the editor. */
	if (!Waiting.IsValid()) {
		Waiting = RegisterActiveTimer(0.0f, FWidgetActiveTimerDelegate::CreateSP(this, &SProfilesPanel::Pump));
	}

	Working = true;

	if (List.IsValid()) {
		List->RebuildList();
	}
}

void SProfilesPanel::FinishWaiting() {
	BusyId.Reset();
	BusyWhat = FText::GetEmpty();

	/* Opening one is not over when the request that asked for it comes back */
	Working = !Awaiting.IsEmpty();

	if (List.IsValid()) {
		List->RebuildList();
	}
}

EActiveTimerReturnType SProfilesPanel::Pump(const double CurrentTime, const float DeltaTime) {
	Cloud::PumpRequests(DeltaTime);

	if (!Awaiting.IsEmpty() && !AskingMetadata && CurrentTime >= AskAgainAt) {
		AskingMetadata = true;
		AskAgainAt = CurrentTime + BetweenAsksSeconds;

		TWeakPtr<SProfilesPanel> Weak = SharedThis(this);

		Cloud::Update([Weak](const bool bAnswered) {
			const TSharedPtr<SProfilesPanel> Panel = Weak.Pin();

			if (!Panel.IsValid()) return;

			Panel->AskingMetadata = false;

			if (!bAnswered || Panel->Awaiting.IsEmpty()) return;

			/* Both, since Mirror answers with the name before the files are readable */
			if (GReflectionRuntime.Profile.Name != Panel->Awaiting) return;
			if (GReflectionRuntime.Profile.ProjectName.IsEmpty()) return;

			AppendNotification(
				FText::FromString("Opened Profile"),
				FText::FromString(Panel->Awaiting),
				2.0f,
				SNotificationItem::CS_Success,
				true,
				310.0f
			);

			Panel->Awaiting.Reset();

			Panel->OnChanged.ExecuteIfBound();
			Panel->FinishWaiting();
			Panel->Refresh();
		});
	}

	/* A profile that failed to open never arrives, so the wait ends by itself rather than lasting
	 * as long as the window does */
	if (!Awaiting.IsEmpty() && CurrentTime >= GiveUpOpeningAt) {
		AppendNotification(
			FText::FromString("Profile Did Not Open"),
			FText::Format(LOCTEXT("OpeningGaveUpFmt", "Mirror is still not reading {0}"), FText::FromString(Awaiting)),
			5.0f,
			SNotificationItem::CS_Fail,
			true,
			310.0f
		);

		Awaiting.Reset();

		FinishWaiting();
		Refresh();
	}

	if (Working || AskingMetadata) {
		return EActiveTimerReturnType::Continue;
	}

	Waiting.Reset();

	return EActiveTimerReturnType::Stop;
}

/* Actions ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
FReply SProfilesPanel::OnRefreshClicked() {
	Refresh();

	return FReply::Handled();
}

FReply SProfilesPanel::OnSetupClicked() {
	LaunchURL(GitHub::README::Cloud);

	return FReply::Handled();
}

FReply SProfilesPanel::OnFollowMirrorClicked() {
	ReadFrom(FString(), FString());

	return FReply::Handled();
}

void SProfilesPanel::ReadFrom(const FString& Id, const FString& Named) {
	Cloud::Build = Id;

	BeginWaiting(LOCTEXT("Reading", "reading"), Id);

	TWeakPtr<SProfilesPanel> Weak = SharedThis(this);

	/* Asked again through the profile that was just pinned, since the project a path belongs to
	 * and the engine it was cooked by are that profile's to say and not the last one's */
	Cloud::Update([Weak, Named](const bool bAnswered) {
		const TSharedPtr<SProfilesPanel> Panel = Weak.Pin();

		if (!Panel.IsValid()) return;

		if (bAnswered && !Named.IsEmpty()) {
			AppendNotification(
				FText::FromString("Reading Profile"),
				FText::FromString(Named),
				2.0f,
				SNotificationItem::CS_Success,
				true,
				310.0f
			);
		}

		Panel->OnChanged.ExecuteIfBound();
		Panel->FinishWaiting();
		Panel->Refresh();
	});
}

FReply SProfilesPanel::OnReadFromClicked(const FRowItem Item) {
	if (!Item.IsValid() || !IsIdle()) return FReply::Handled();

	const FString Id = Item->Id;
	const FString Named = Item->Name;

	/* Already mounted, so there is nothing to wait for before reading out of it */
	if (Item->Loaded) {
		ReadFrom(Id, Named);

		return FReply::Handled();
	}

	BeginWaiting(LOCTEXT("Loading", "loading"), Id);

	TWeakPtr<SProfilesPanel> Weak = SharedThis(this);

	Cloud::Builds::StartAsync(Id, [Weak, Id, Named](const bool bLoaded) {
		const TSharedPtr<SProfilesPanel> Panel = Weak.Pin();

		if (!Panel.IsValid()) return;

		if (!bLoaded) {
			AppendNotification(
				FText::FromString("Could Not Load Profile"),
				FText::Format(LOCTEXT("LoadRefusedFmt", "Mirror would not mount {0}"), FText::FromString(Named)),
				5.0f,
				SNotificationItem::CS_Fail,
				true,
				310.0f
			);

			Panel->FinishWaiting();
			Panel->Refresh();

			return;
		}

		Panel->ReadFrom(Id, Named);
	});

	return FReply::Handled();
}

FReply SProfilesPanel::OnOpenInMirrorClicked(const FRowItem Item) {
	if (!Item.IsValid() || !IsIdle()) return FReply::Handled();

	const FString Id = Item->Id;
	const FString Named = Item->Name;

	/* A pin would go on overriding whatever Mirror opens, which is the opposite of what asking it
	 * to open one means */
	Cloud::Build.Reset();

	BeginWaiting(LOCTEXT("Opening", "opening"), Id);

	TWeakPtr<SProfilesPanel> Weak = SharedThis(this);

	Cloud::Builds::UseAsync(Id, [Weak, Named](const bool bAsked) {
		const TSharedPtr<SProfilesPanel> Panel = Weak.Pin();

		if (!Panel.IsValid()) return;

		if (!bAsked || Named.IsEmpty()) {
			AppendNotification(
				FText::FromString("Could Not Open Profile"),
				FText::Format(LOCTEXT("OpenRefusedFmt", "Mirror would not open {0}"), FText::FromString(Named)),
				5.0f,
				SNotificationItem::CS_Fail,
				true,
				310.0f
			);

			Panel->FinishWaiting();
			Panel->Refresh();

			return;
		}

		/* Mirror answered before its archives were mounted, and says nothing when they are, so
		 * from here it is asked what it has open until it says this one */
		Panel->Awaiting = Named;
		Panel->AskAgainAt = 0.0;
		Panel->GiveUpOpeningAt = FSlateApplication::Get().GetCurrentTime() + GiveUpOpeningAfterSeconds;
	});

	return FReply::Handled();
}

FReply SProfilesPanel::OnUnloadClicked(const FRowItem Item) {
	if (!Item.IsValid() || !IsIdle()) return FReply::Handled();

	BeginWaiting(LOCTEXT("Unloading", "unloading"), Item->Id);

	TWeakPtr<SProfilesPanel> Weak = SharedThis(this);

	Cloud::Builds::StopAsync(Item->Id, [Weak](const bool bStopped) {
		const TSharedPtr<SProfilesPanel> Panel = Weak.Pin();

		if (!Panel.IsValid()) return;

		Panel->FinishWaiting();
		Panel->Refresh();
	});

	return FReply::Handled();
}

#undef LOCTEXT_NAMESPACE
