/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#ifndef __linux__
#include "Windows/WindowsPlatformApplicationMisc.h"
#endif // __linux__

#include "Interfaces/IMainFrameModule.h"
#include "DesktopPlatformModule.h"
#include "IDesktopPlatform.h"

#include "Framework/Application/SlateApplication.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SWindow.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Text/STextBlock.h"

#include "Engine/Compatibility.h"

/* 4.25 and below build this module without the engine's shared PCH (see Reflection.Build.cs),
 * which is where the message dialog used to come in from */
#if UE4_25_BELOW
#include "Misc/MessageDialog.h"
#endif

/* Prompts ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
inline void SpawnPrompt(const FString& Title, const FString& Text) {
	FText DialogTitle = FText::FromString(Title);
	const FText DialogMessage = FText::FromString(Text);

	FMessageDialog::Open(EAppMsgType::Ok, DialogMessage);
}

inline auto SpawnYesNoPrompt = [](const FString& Title, const FString& Text, const TFunction<void(bool)>& OnResponse) {
	const FText DialogTitle = FText::FromString(Title);
	const FText DialogMessage = FText::FromString(Text);

#if UE5_3_BEYOND
	const EAppReturnType::Type Response = FMessageDialog::Open(EAppMsgType::YesNo, DialogMessage, DialogTitle);
#else
	const EAppReturnType::Type Response = FMessageDialog::Open(EAppMsgType::YesNo, DialogMessage, &DialogTitle);
#endif

	OnResponse(Response == EAppReturnType::Yes);
};

/*
 * Asks the user for some text, seeded with whatever is already in InOutText.
 *
 * Multi line, because everything that needs typed input here takes a list. Returns false when the
 * user cancels or leaves it empty, in which case InOutText is left alone.
 */
inline bool SpawnTextEntryPrompt(const FString& Title, const FString& Hint, FString& InOutText) {
	bool Accepted = false;

	TSharedPtr<SMultiLineEditableTextBox> TextBox;

	const TSharedRef<SWindow> Window = SNew(SWindow)
		.Title(FText::FromString(Title))
		.ClientSize(FVector2D(640, 280))
		.SupportsMinimize(false)
		.SupportsMaximize(false);

	Window->SetContent(
		SNew(SBorder)
		.Padding(12.0f)
		[
			SNew(SVerticalBox)

			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 0.0f, 0.0f, 8.0f)
			[
				SNew(STextBlock)
				.Text(FText::FromString(Hint))
				.AutoWrapText(true)
			]

			+ SVerticalBox::Slot()
			.FillHeight(1.0f)
			[
				SAssignNew(TextBox, SMultiLineEditableTextBox)
				.Text(FText::FromString(InOutText))
			]

			+ SVerticalBox::Slot()
			.AutoHeight()
			.HAlign(HAlign_Right)
			.Padding(0.0f, 10.0f, 0.0f, 0.0f)
			[
				SNew(SHorizontalBox)

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.Padding(0.0f, 0.0f, 6.0f, 0.0f)
				[
					SNew(SButton)
					.Text(FText::FromString("Reflect"))
					.OnClicked_Lambda([&Accepted, &InOutText, &TextBox, Window]() {
						Accepted = true;
						InOutText = TextBox->GetText().ToString();

						Window->RequestDestroyWindow();

						return FReply::Handled();
					})
				]

				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					SNew(SButton)
					.Text(FText::FromString("Cancel"))
					.OnClicked_Lambda([Window]() {
						Window->RequestDestroyWindow();

						return FReply::Handled();
					})
				]
			]
		]
	);

	const IMainFrameModule& MainFrameModule = IMainFrameModule::Get();
	FSlateApplication::Get().AddModalWindow(Window, MainFrameModule.GetParentWindow());

	return Accepted && !InOutText.IsEmpty();
}

/* Clipboard and file pickers ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~ */
inline FString GetClipboard() {
	FString ClipboardContent;
#ifndef __linux__
	/* @LINUX.CLIPBOARD */
	FPlatformApplicationMisc::ClipboardPaste(ClipboardContent);
#endif

	return ClipboardContent;
}

inline TArray<FString> OpenFileDialog(const FString& Title, const FString& Type) {
	TArray<FString> ReturnValue;

	/* Window Handler for Windows */
	const void* ParentWindowHandle = nullptr;
	const IMainFrameModule& MainFrameModule = IMainFrameModule::Get();
	const TSharedPtr<SWindow> MainWindow = MainFrameModule.GetParentWindow();

	if (MainWindow.IsValid() && MainWindow->GetNativeWindow().IsValid()) {
		ParentWindowHandle = MainWindow->GetNativeWindow()->GetOSWindowHandle();
	}

	FString ClipboardContent = GetClipboard();
	FString DefaultPath = FString("");

	if (!ClipboardContent.IsEmpty()) {
		if (FPaths::FileExists(ClipboardContent)) {
			DefaultPath = FPaths::GetPath(ClipboardContent);
		}
		else if (FPaths::DirectoryExists(ClipboardContent)) {
			DefaultPath = ClipboardContent;
		}
	}

	if (IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get()) {
		constexpr uint32 SelectionFlag = 1;
		
		DesktopPlatform->OpenFileDialog(ParentWindowHandle, Title, DefaultPath, FString(""), Type, SelectionFlag, ReturnValue);
	}

	return ReturnValue;
}

inline FString OpenFolderDialog(const FString& Title, const FString& DefaultPath) {
	FString OutFolder;
	const void* ParentWindowHandle = nullptr;

	const IMainFrameModule& MainFrameModule = IMainFrameModule::Get();
	const TSharedPtr<SWindow> MainWindow = MainFrameModule.GetParentWindow();
	if (MainWindow.IsValid() && MainWindow->GetNativeWindow().IsValid()) {
		ParentWindowHandle = MainWindow->GetNativeWindow()->GetOSWindowHandle();
	}

	if (IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get()) {
		DesktopPlatform->OpenDirectoryDialog(ParentWindowHandle, Title, DefaultPath, OutFolder);
	}

	return OutFolder;
}

inline TArray<FString> OpenFolderDialog(const FString& Title) {
	TArray<FString> ReturnValue;

	/* Window Handler for Windows */
	const void* ParentWindowHandle = nullptr;

	const IMainFrameModule& MainFrameModule = IMainFrameModule::Get();
	const TSharedPtr<SWindow> MainWindow = MainFrameModule.GetParentWindow();

	if (MainWindow.IsValid() && MainWindow->GetNativeWindow().IsValid()) {
		ParentWindowHandle = MainWindow->GetNativeWindow()->GetOSWindowHandle();
	}

	if (IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get()) {
		FString SelectedFolder;

		/* Open Folder Dialog */
		if (DesktopPlatform->OpenDirectoryDialog(ParentWindowHandle, Title, FString(""), SelectedFolder)) {
			ReturnValue.Add(SelectedFolder);
		}
	}

	return ReturnValue;
}