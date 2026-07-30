/* Copyright Reflection Contributors 2024-2026 */

#pragma once

#ifndef __linux__
#include "Windows/WindowsHWrapper.h"
#include "TlHelp32.h"
#endif

inline void CloseApplicationByProcessName(const FString& ProcessName) {
#ifndef __linux__
	DWORD ProcessID = 0;

	const HANDLE Snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);

	if (Snapshot != INVALID_HANDLE_VALUE) {
		PROCESSENTRY32 ProcessEntry;
		ProcessEntry.dwSize = sizeof(PROCESSENTRY32);

		if (Process32First(Snapshot, &ProcessEntry)) {
			do {
				if (FCString::Stricmp(ProcessEntry.szExeFile, ProcessName.GetCharArray().GetData()) == 0) {
					ProcessID = ProcessEntry.th32ProcessID;
					break;
				}
			} while (Process32Next(Snapshot, &ProcessEntry));
		}

		CloseHandle(Snapshot);
	}

	if (ProcessID != 0) {
		const HANDLE Process = OpenProcess(PROCESS_TERMINATE, false, ProcessID);

		if (Process != nullptr) {
			TerminateProcess(Process, 0);
			CloseHandle(Process);
		}
	}
#else
	/* @LINUX.PROCESSES */
#endif
}

inline bool IsProcessRunning(const FString& ProcessName) {
#ifndef __linux__
	bool IsRunning = false;

	/* Convert FString to WCHAR */
	const TCHAR* ProcessNameChar = *ProcessName;

	const HANDLE Snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (Snapshot != INVALID_HANDLE_VALUE) {
		PROCESSENTRY32 ProcessEntry;
		ProcessEntry.dwSize = sizeof(ProcessEntry);

		if (Process32First(Snapshot, &ProcessEntry)) {
			do {
				if (_wcsicmp(ProcessEntry.szExeFile, ProcessNameChar) == 0) {
					IsRunning = true;
					break;
				}
			} while (Process32Next(Snapshot, &ProcessEntry));
		}

		CloseHandle(Snapshot);
	}

	return IsRunning;
#else
	/* @LINUX.PROCESSES */
	return true;
#endif
}

inline void LaunchURL(const FString& URL) {
	FPlatformProcess::LaunchURL(*URL, nullptr, nullptr);
}
