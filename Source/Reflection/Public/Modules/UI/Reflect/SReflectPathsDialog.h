/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"

class SBox;
class SEditableTextBox;
class SProfilesPanel;
class SRedirectorsPanel;
class SReflectFolderDialog;
class SWidgetSwitcher;
class SWindow;

/* What the dialog was shut with */
enum class EReflectPathsChoice : uint8 {
	/* Nothing to run: the window was closed, or the queue was empty */
	Cancelled,

	/* Run what is queued */
	Reflect,

	/* Run what the folder page listed, which is already in OutPaths. A different answer because
	 * a folder is counted as one run rather than as however many assets it came to. */
	Folder
};

/* One queued path, and where it lands.
 *
 * Cloud is asked for the path as it was queued, and a redirect only moves where the asset is
 * written in this project. So the two are kept apart: the one to fetch with, and the one to show. */
struct FReflectPathEntry {
	/* As queued, which is the spelling Cloud answers to */
	FString Path;

	/* The editor path it would take with nothing redirecting it, which is what the redirectors
	 * are run against. Kept so a change to them can be answered without parsing the path again. */
	FString Landing;

	/* Where a redirect puts it, empty when nothing moves it */
	FString Redirected;

	/* The redirectors that moved it */
	TArray<FName> RedirectedBy;

	bool Moved() const { return !Redirected.IsEmpty(); }
};

/* Builds up a list of asset paths to reflect, one Cloud knows about rather than one in this
 * project.
 *
 * Nothing here can be checked without asking Cloud for the asset itself, so the dialog is about
 * getting the list right: what is queued stays visible, and anything wrong can be taken back out
 * before the run starts. */
class REFLECTION_API SReflectPathsDialog : public SCompoundWidget {
public:
	SLATE_BEGIN_ARGS(SReflectPathsDialog) {}
		SLATE_ARGUMENT(TSharedPtr<SWindow>, ParentWindow)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/* Runs the dialog modally. Reflect fills OutPaths with everything queued, Folder with what the
	 * folder page listed and OutAllowedTypes with what it was told to build; Cancelled leaves
	 * both alone. */
	static EReflectPathsChoice Open(TArray<FString>& OutPaths, TSet<FString>& OutAllowedTypes);

private:
	FReply OnAddClicked();
	FReply OnReflectClicked();

	void OnPathCommitted(const FText& NewText, ETextCommit::Type CommitType);

	/* Queues whatever is in the box, which may be several paths pasted at once */
	void AddFromBox();

	/* One entry per path, ignoring blanks and anything already queued */
	void AddPaths(const FString& Text);

	/* Takes one entry back out, from the cross on its own row */
	FReply RemovePath(TSharedPtr<FReflectPathEntry> Entry);

	bool CanAdd() const;
	bool CanReflect() const;

	/* Switches which page is up. The redirectors are counted against the queue, so going to them
	 * is also when that count is worked out. */
	void ShowPage(int32 Which);

	TSharedRef<SWidget> BuildTab(int32 Which, const FText& Label, const FText& Tooltip);

	/* A redirector changed, so where everything queued lands is worked out again */
	void OnRedirectorsChanged();

	/* Mirror is reading a different profile, which changes more than the redirectors do: a Cloud
	 * path is turned into an editor one with the project's name, so the queue is worked out from
	 * the path up rather than just redirected again */
	void OnProfileSwitched();

	/* Where the queue lands before any redirect, which is what the redirectors page counts */
	TArray<FString> GetLandingPaths();

	FText GetStatusText() const;
	FText GetReflectText() const;

	TSharedRef<ITableRow> GenerateRow(TSharedPtr<FReflectPathEntry> Entry, const TSharedRef<STableViewBase>& OwnerTable);

	TSharedPtr<SWindow> ParentWindow;
	TSharedPtr<SEditableTextBox> PathBox;
	TSharedPtr<SWidgetSwitcher> Pages;
	TSharedPtr<SRedirectorsPanel> RedirectorsPanel;

	/* Built the first time that page is asked for rather than with the window. It lists a folder
	 * as it opens, and a window that was opened to type a path has no business asking Cloud for
	 * a listing nobody went looking for. */
	TSharedPtr<SBox> FolderSlot;
	TSharedPtr<SReflectFolderDialog> FolderPanel;

	TSharedPtr<SProfilesPanel> ProfilesPanel;
	TSharedPtr<SListView<TSharedPtr<FReflectPathEntry>>> ListView;

	TArray<TSharedPtr<FReflectPathEntry>> Rows;

	/* Which of the two buttons that close the window was pressed. Neither, and the window was shut
	 * some other way, which is neither answer. */
	bool Accepted = false;

	/* Which page is up, kept here as well as on the switcher so the tabs can read it */
	int32 Page = 0;
};
