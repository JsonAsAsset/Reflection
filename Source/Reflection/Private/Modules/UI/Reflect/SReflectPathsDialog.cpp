/* Copyright Reflection Contributors 2024-2026 */

#include "Modules/UI/Reflect/SReflectPathsDialog.h"

#include "Engine/Compatibility.h"

#include "Containers/Export.h"
#include "Modules/UI/Reflect/SProfilesPanel.h"
#include "Modules/UI/Reflect/SRedirectorsPanel.h"
#include "Modules/UI/Reflect/SReflectFolderDialog.h"
#include "Modules/Cloud/Cloud.h"
#include "Settings/Redirector.h"
#include "Utilities/AssetPaths.h"
#include "Utilities/Dialog.h"

#include "Interfaces/IMainFrameModule.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SWindow.h"
#include "Widgets/Input/SButton.h"
#include "Settings/SettingsAccess.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SWidgetSwitcher.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/STableRow.h"

#define LOCTEXT_NAMESPACE "Reflection.ReflectPaths"

/* The queue, and the redirectors that decide where it lands. Two pages rather than one window
 * because a redirect is written for a path somebody is about to reflect, and the only way to see
 * whether it was written right is to have both in front of you. */
static constexpr int32 PathsPage = 0;
static constexpr int32 FolderPage = 1;
static constexpr int32 RedirectorsPage = 2;

/* Away from the other three, since it is not a way of saying what to reflect: it says what any of
 * them means */
static constexpr int32 ProfilePage = 3;

#if ENGINE_UE5
static const FName RemoveButtonStyleName("SimpleButton");
static const FName RemoveIconName("Icons.X");
#else
static const FName RemoveButtonStyleName("HoverHintOnly");
static const FName RemoveIconName("Symbols.X");
#endif

static FSlateColor GetRemoveIconColor() {
#if ENGINE_UE5
	return FSlateColor::UseForeground();
#else
	return FAppStyle::Get().GetSlateColor("DefaultForeground");
#endif
}

/* Whether one line could be an asset path.
 *
 * Deliberately shallow: only Cloud can say whether a path leads anywhere, and this runs against
 * whatever happened to be copied last. It is here to tell a path from a sentence, not to validate. */
static bool IsPath(const FString& Line) {
	return IsAssetPathLike(Line);
}

/* Whether a line names a folder rather than an asset in one.
 *
 * Only the trailing slash counts. A path is otherwise no help: /Game/Foo/Bar is a package as
 * readily as it is the folder one sits in, and guessing wrong queues something that cannot come
 * down, or skips something that could. */
static bool IsFolderPath(const FString& Line) {
	FString Path = StripObjectOuter(Line.TrimStartAndEnd());
	Path.ReplaceInline(TEXT("\\"), TEXT("/"));

	return Path.EndsWith(TEXT("/"));
}

/* The package a queued path lands at, with no redirect applied.
 *
 * A redirect runs against the editor path rather than the one Cloud is asked for, so the path is
 * turned into one first, and cut the way a fetch cuts it: an export name or an extension is not
 * part of the package a redirect moves. */
static FString LandingPathOf(const FString& Queued) {
	FString Path = StripObjectOuter(Queued.TrimStartAndEnd());
	Path.ReplaceInline(TEXT("\\"), TEXT("/"));

	int32 Dot;

	if (Path.FindChar(TEXT('.'), Dot)) {
		LeftInline(Path, Dot);
	}

	return ToEditorPackagePathDirect(Path);
}

/* Where a path lands once the redirectors have had it, which is worked out again whenever they
 * change rather than held from when the path went in */
static void ApplyRedirect(FReflectPathEntry& Entry) {
	Entry.Redirected.Reset();
	Entry.RedirectedBy.Reset();

	const FRRedirectPreview Preview = FRRedirects::Preview(Entry.Landing);

	/* A point whose To reads the same as its From matched without moving anything, and a path
	 * that did not move has nothing to say about where it went */
	if (Preview.Moved() && Preview.Path != Entry.Landing) {
		Entry.Redirected = Preview.Path;
		Entry.RedirectedBy = Preview.By;
	}
}

/* Which redirectors moved a path, for the line above it */
static FText RedirectedByText(const TArray<FName>& By) {
	if (By.Num() == 0) return FText::GetEmpty();

	TArray<FString> Names;

	for (const FName Name : By) {
		/* A redirector is not made to carry a name, and one that has not got one still moved it */
		Names.Add(Name.IsNone() ? TEXT("an unnamed redirector") : Name.ToString());
	}

	return FText::Format(LOCTEXT("RedirectedByFmt", "Redirected by {0}"), FText::FromString(FString::Join(Names, TEXT(", "))));
}

static bool IsPathList(const FString& Text) {
	TArray<FString> Lines;
	Text.ParseIntoArrayLines(Lines);

	int32 Paths = 0;

	for (const FString& Line : Lines) {
		if (Line.TrimStartAndEnd().IsEmpty()) continue;

		if (!IsPath(Line)) {
			return false;
		}

		Paths++;
	}

	return Paths > 0;
}

EReflectPathsChoice SReflectPathsDialog::Open(TArray<FString>& OutPaths, TSet<FString>& OutAllowedTypes) {
	const TSharedRef<SWindow> Window = SNew(SWindow)
		.Title(LOCTEXT("Title", "Reflection"))
		.ClientSize(FVector2D(720.0f, 480.0f))
		.SupportsMinimize(false)
		.SupportsMaximize(false);

	TSharedPtr<SReflectPathsDialog> Dialog;

	Window->SetContent(
		SAssignNew(Dialog, SReflectPathsDialog)
		.ParentWindow(Window)
	);

	const IMainFrameModule& MainFrameModule = IMainFrameModule::Get();
	FSlateApplication::Get().AddModalWindow(Window, MainFrameModule.GetParentWindow());

	/* Asked for before the queue is looked at: the folder page runs what it listed itself, and
	 * what was queued on the other page is not part of it */
	if (Dialog->FolderPanel.IsValid() && Dialog->FolderPanel->WasAccepted()) {
		OutPaths = Dialog->FolderPanel->GetPaths();
		OutAllowedTypes = Dialog->FolderPanel->GetAllowedTypes();

		return OutPaths.Num() > 0 ? EReflectPathsChoice::Folder : EReflectPathsChoice::Cancelled;
	}

	if (!Dialog->Accepted) {
		return EReflectPathsChoice::Cancelled;
	}

	OutPaths.Reset();

	/* The queued path rather than the redirected one: where it lands is this project's business,
	 * and Cloud only knows the asset by where it came from */
	for (const TSharedPtr<FReflectPathEntry>& Row : Dialog->Rows) {
		if (Row.IsValid()) {
			OutPaths.Add(Row->Path);
		}
	}

	return OutPaths.Num() > 0 ? EReflectPathsChoice::Reflect : EReflectPathsChoice::Cancelled;
}

void SReflectPathsDialog::Construct(const FArguments& InArgs) {
	ParentWindow = InArgs._ParentWindow;

	ChildSlot
	[
		SNew(SVerticalBox)

		/* Pages ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(FMargin(8.0f, 8.0f, 8.0f, 0.0f))
		[
			SNew(SHorizontalBox)

			+ SHorizontalBox::Slot()
			.AutoWidth()
			[
				BuildTab(PathsPage, LOCTEXT("PathsTab", "Paths"), LOCTEXT("PathsTabTooltip", "What to reflect"))
			]

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(FMargin(4.0f, 0.0f, 0.0f, 0.0f))
			[
				BuildTab(FolderPage, LOCTEXT("FolderTab", "Folder"), LOCTEXT("FolderTabTooltip", "Everything under a folder of the game files, listed before it runs"))
			]

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(FMargin(4.0f, 0.0f, 0.0f, 0.0f))
			[
				BuildTab(RedirectorsPage, LOCTEXT("RedirectorsTab", "Redirectors"), LOCTEXT("RedirectorsTabTooltip", "Where it lands once it is here"))
			]

			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.HAlign(HAlign_Right)
			[
				BuildTab(ProfilePage, LOCTEXT("ProfileTab", "Profile"), LOCTEXT("ProfileTabTooltip", "Which of Mirror's profiles everything here is read out of"))
			]
		]

		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		[
			SAssignNew(Pages, SWidgetSwitcher)

			+ SWidgetSwitcher::Slot()
			[
				SNew(SVerticalBox)

				/* Entry ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(SBorder)
					.BorderImage(FAppStyle::Get().GetBrush("ToolPanel.GroupBorder"))
					.Padding(FMargin(8.0f, 6.0f))
					[
						SNew(SHorizontalBox)

						+ SHorizontalBox::Slot()
						.FillWidth(1.0f)
						.VAlign(VAlign_Center)
						[
							SAssignNew(PathBox, SEditableTextBox)
							.HintText(LOCTEXT("PathHint", "/Game/Asset"))
							.ToolTipText(LOCTEXT("PathTooltip", "An asset path, in either form. A copied reference or several paths at once are fine."))
							.OnTextCommitted(this, &SReflectPathsDialog::OnPathCommitted)
						]

						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						.Padding(FMargin(6.0f, 0.0f, 0.0f, 0.0f))
						[
							SNew(SButton)
							.Text(LOCTEXT("Add", "Add"))
							.ToolTipText(LOCTEXT("AddTooltip", "Queue this path. Several pasted at once are queued one per line."))
							.IsEnabled(this, &SReflectPathsDialog::CanAdd)
							.OnClicked(this, &SReflectPathsDialog::OnAddClicked)
						]
					]
				]

				/* Queue ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(FMargin(8.0f, 6.0f, 8.0f, 0.0f))
				[
					SNew(STextBlock)
					.Text(this, &SReflectPathsDialog::GetStatusText)
				]

				+ SVerticalBox::Slot()
				.FillHeight(1.0f)
				.Padding(FMargin(8.0f, 6.0f))
				[
					SNew(SBorder)
					.BorderImage(FAppStyle::Get().GetBrush("ToolPanel.GroupBorder"))
					.Padding(FMargin(2.0f))
					[
						SAssignNew(ListView, SListView<TSharedPtr<FReflectPathEntry>>)
						.ListItemsSource(&Rows)
						.SelectionMode(ESelectionMode::None)
						.OnGenerateRow(this, &SReflectPathsDialog::GenerateRow)
					]
				]

				/* Footer ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(FMargin(8.0f, 0.0f, 8.0f, 8.0f))
				[
					SNew(SHorizontalBox)

					/* Beside the run, since it is decided for the asset in front of somebody */
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					[
						SNew(SCheckBox)
						.ToolTipText(LOCTEXT("ReferencesTooltip", "Fetch what an asset names as well as the asset. Off, only the paths queued here are built, and everything they refer to is left to whatever the project already has."))
						.IsChecked_Lambda([] {
							return GetSettings()->AssetSettings.ImportReferences ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
						})
						.OnCheckStateChanged_Lambda([](const ECheckBoxState NewState) {
							GetSettings()->AssetSettings.ImportReferences = NewState == ECheckBoxState::Checked;
							GetSettings()->SaveConfig();
						})
						[
							SNew(STextBlock)
							.Text(LOCTEXT("References", "References"))
							.Margin(FMargin(4.0f, 0.0f, 0.0f, 0.0f))
						]
					]

					+ SHorizontalBox::Slot()
					.FillWidth(1.0f)
					.HAlign(HAlign_Right)
					.VAlign(VAlign_Center)
					.Padding(FMargin(6.0f, 0.0f, 0.0f, 0.0f))
					[
						SNew(SButton)
						.Text(this, &SReflectPathsDialog::GetReflectText)
						.IsEnabled(this, &SReflectPathsDialog::CanReflect)
						.OnClicked(this, &SReflectPathsDialog::OnReflectClicked)
					]
				]
			]

			+ SWidgetSwitcher::Slot()
			[
				SAssignNew(FolderSlot, SBox)
			]

			+ SWidgetSwitcher::Slot()
			[
				SAssignNew(RedirectorsPanel, SRedirectorsPanel)
				.OnChanged(this, &SReflectPathsDialog::OnRedirectorsChanged)
				.LandingPaths(this, &SReflectPathsDialog::GetLandingPaths)
			]

			+ SWidgetSwitcher::Slot()
			[
				SAssignNew(ProfilesPanel, SProfilesPanel)
				.OnChanged(this, &SReflectPathsDialog::OnProfileSwitched)
			]
		]
	];

	/* The path is usually already on the clipboard, straight out of the asset it was copied from.
	 * Whatever else was copied since is not, so nothing goes in unless all of it is paths. */
	const FString Clipboard = GetClipboard();

	/* A folder is as likely to be what was copied, and it is not something the queue can hold: it
	 * names a listing to ask Cloud for, not an asset to fetch. It goes in the box, where the folder
	 * page picks it up and Add is still there for anyone who meant it as a path after all. */
	if (IsAssetPathLike(Clipboard) && IsFolderPath(Clipboard)) {
		PathBox->SetText(FText::FromString(Clipboard.TrimStartAndEnd()));
	} else if (IsPathList(Clipboard)) {
		AddPaths(Clipboard);
	}

	FSlateApplication::Get().SetKeyboardFocus(PathBox);
}

/* Actions ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
void SReflectPathsDialog::OnPathCommitted(const FText& NewText, const ETextCommit::Type CommitType) {
	/* Enter queues what was typed, so paths can be pasted one after another without the mouse */
	if (CommitType == ETextCommit::OnEnter) {
		AddFromBox();
	}
}

FReply SReflectPathsDialog::OnAddClicked() {
	AddFromBox();

	return FReply::Handled();
}

FReply SReflectPathsDialog::OnReflectClicked() {
	Accepted = true;

	if (ParentWindow.IsValid()) {
		ParentWindow->RequestDestroyWindow();
	}

	return FReply::Handled();
}

void SReflectPathsDialog::AddFromBox() {
	AddPaths(PathBox->GetText().ToString());

	PathBox->SetText(FText::GetEmpty());

	FSlateApplication::Get().SetKeyboardFocus(PathBox);
}

void SReflectPathsDialog::AddPaths(const FString& Text) {
	TArray<FString> Lines;
	Text.ParseIntoArrayLines(Lines);

	for (const FString& Line : Lines) {
		const FString Path = Line.TrimStartAndEnd();
		if (Path.IsEmpty()) continue;

		/* Reflecting the same path twice is just a wasted request */
		const bool Queued = Rows.ContainsByPredicate([&Path](const TSharedPtr<FReflectPathEntry>& Row) {
			return Row.IsValid() && Row->Path.Equals(Path, ESearchCase::IgnoreCase);
		});

		if (Queued) continue;

		const TSharedRef<FReflectPathEntry> Entry = MakeShared<FReflectPathEntry>();
		Entry->Path = Path;
		Entry->Landing = LandingPathOf(Path);

		ApplyRedirect(Entry.Get());

		Rows.Add(Entry);
	}

	if (ListView.IsValid()) {
		ListView->RequestListRefresh();
	}
}

FReply SReflectPathsDialog::RemovePath(TSharedPtr<FReflectPathEntry> Entry) {
	Rows.Remove(Entry);

	if (ListView.IsValid()) {
		ListView->RequestListRefresh();
	}

	return FReply::Handled();
}

/* State ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
bool SReflectPathsDialog::CanAdd() const {
	return PathBox.IsValid() && !PathBox->GetText().ToString().TrimStartAndEnd().IsEmpty();
}

bool SReflectPathsDialog::CanReflect() const {
	return Rows.Num() > 0;
}

void SReflectPathsDialog::ShowPage(const int32 Which) {
	Page = Which;

	if (Pages.IsValid()) {
		Pages->SetActiveWidgetIndex(Which);
	}

	/* Built on the way in rather than with the window, since opening it is what lists a folder */
	if (Which == FolderPage && !FolderPanel.IsValid() && FolderSlot.IsValid()) {
		/* Whatever is in the path box goes with it, the way the button used to hand it over, so a
		 * folder typed on the other page is not typed again here */
		const FString Typed = PathBox.IsValid() ? PathBox->GetText().ToString().TrimStartAndEnd() : FString();

		FolderSlot->SetContent(
			SAssignNew(FolderPanel, SReflectFolderDialog)
			.ParentWindow(ParentWindow)
			.InitialFolder(Typed)
			.UseClipboard(true)
		);

		/* A listing is a Cloud request, and a page that cannot make one says so rather than
		 * reporting the folder as holding nothing */
		if (!Cloud::Status::IsOpened()) {
			Cloud::Status::NotifyNotRunning();
		}
	}

	/* The queue may have been added to since the page was last up, and what each redirector does
	 * is counted against it */
	if (Which == RedirectorsPage && RedirectorsPanel.IsValid()) {
		RedirectorsPanel->Refresh();
	}

	/* Mirror may have been pointed somewhere else by its own window in the meantime */
	if (Which == ProfilePage && ProfilesPanel.IsValid()) {
		ProfilesPanel->Refresh();
	}
}

TSharedRef<SWidget> SReflectPathsDialog::BuildTab(const int32 Which, const FText& Label, const FText& Tooltip) {
	return SNew(SCheckBox)
		.Style(FAppStyle::Get(), "ToggleButtonCheckbox")
		.Padding(FMargin(10.0f, 3.0f))
		.ToolTipText(Tooltip)
		.IsChecked_Lambda([this, Which] {
			return Page == Which ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
		})
		.OnCheckStateChanged_Lambda([this, Which](ECheckBoxState) {
			/* Pressing the page that is already up is not a way off it */
			ShowPage(Which);
		})
		[
			SNew(STextBlock)
			.Text(Label)
		];
}

void SReflectPathsDialog::OnRedirectorsChanged() {
	for (const TSharedPtr<FReflectPathEntry>& Row : Rows) {
		if (Row.IsValid()) {
			ApplyRedirect(*Row);
		}
	}

	if (ListView.IsValid()) {
		ListView->RebuildList();
	}
}

void SReflectPathsDialog::OnProfileSwitched() {
	for (const TSharedPtr<FReflectPathEntry>& Row : Rows) {
		if (!Row.IsValid()) continue;

		/* Worked out again rather than redirected again: which project the path belongs to is the
		 * profile's to say, and that is what the editor path is built out of */
		Row->Landing = LandingPathOf(Row->Path);

		ApplyRedirect(*Row);
	}

	if (ListView.IsValid()) {
		ListView->RebuildList();
	}

	/* Which redirectors are for this profile is one of the things that just changed */
	if (RedirectorsPanel.IsValid()) {
		RedirectorsPanel->Refresh();
	}
}

TArray<FString> SReflectPathsDialog::GetLandingPaths() {
	TArray<FString> Landing;

	for (const TSharedPtr<FReflectPathEntry>& Row : Rows) {
		if (Row.IsValid()) {
			Landing.Add(Row->Landing);
		}
	}

	return Landing;
}

FText SReflectPathsDialog::GetStatusText() const {
	if (Rows.Num() == 0) {
		return LOCTEXT("StatusEmpty", "Nothing queued. Paths can be added one at a time, or pasted several at once.");
	}

	return FText::Format(LOCTEXT("StatusQueuedFmt", "{0} queued"), FText::AsNumber(Rows.Num()));
}

FText SReflectPathsDialog::GetReflectText() const {
	if (Rows.Num() == 0) {
		return LOCTEXT("Reflect", "Reflect");
	}

	return FText::Format(LOCTEXT("ReflectCountFmt", "Reflect {0}"), FText::AsNumber(Rows.Num()));
}

TSharedRef<ITableRow> SReflectPathsDialog::GenerateRow(TSharedPtr<FReflectPathEntry> Entry, const TSharedRef<STableViewBase>& OwnerTable) {
	const bool Moved = Entry.IsValid() && Entry->Moved();

	const TSharedRef<SVerticalBox> Lines = SNew(SVerticalBox);

	/* Above the path, since it is what the path underneath it needs explaining by */
	if (Moved) {
		Lines->AddSlot()
		.AutoHeight()
		[
			SNew(STextBlock)
			.Text(RedirectedByText(Entry->RedirectedBy))
			.Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
		];
	}

	Lines->AddSlot()
	.AutoHeight()
	[
		SNew(STextBlock)
		/* Where it lands, which is the only one of the two that is not already on the row it was
		 * typed into. What Cloud gets asked for is on the tooltip rather than beside it: the
		 * queue is read to check what is about to be built, not where it comes from. */
		.Text(FText::FromString(Entry.IsValid() ? (Moved ? Entry->Redirected : Entry->Path) : FString()))
		.ToolTipText(Moved
			? FText::Format(LOCTEXT("RedirectedFromFmt", "Fetched as {0}"), FText::FromString(Entry->Path))
			: FText::GetEmpty())
	];

	return SNew(STableRow<TSharedPtr<FReflectPathEntry>>, OwnerTable)
		[
			SNew(SHorizontalBox)

			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			.Padding(FMargin(8.0f, 2.0f))
			[
				Lines
			]

			/* The only way back out of the queue, so it sits on the entry it removes */
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(FMargin(4.0f, 2.0f, 4.0f, 2.0f))
			[
				SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), RemoveButtonStyleName)
				.ContentPadding(FMargin(2.0f))
				.ToolTipText(LOCTEXT("RemoveTooltip", "Take this path back out of the queue"))
				.OnClicked(this, &SReflectPathsDialog::RemovePath, Entry)
				[
					SNew(SImage)
					.Image(FAppStyle::Get().GetBrush(RemoveIconName))
					.ColorAndOpacity(GetRemoveIconColor())
				]
			]
		];
}

#undef LOCTEXT_NAMESPACE
