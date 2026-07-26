/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#ifndef __linux__
#include "Windows/WindowsPlatformApplicationMisc.h"
#endif // __linux__

#include "Interfaces/IMainFrameModule.h"
#include "DesktopPlatformModule.h"
#include "IDesktopPlatform.h"

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