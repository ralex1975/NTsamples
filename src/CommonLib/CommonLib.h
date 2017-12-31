#pragma once

// =============================================
//  Security

bool SetTokenPrivilege(const char* Privilege, bool Enable);

// =============================================
//  File System

bool IsDirExists(const wchar_t* DirPath);
bool IsFileExists(const wchar_t* FilePath);

bool CreateDirectoryByFullPath(wchar_t* DirPath);

bool CreateHardLinkToExistingFile(const wchar_t* DestinationFile, const wchar_t* SourceFile);

// =============================================
//  String

wchar_t* BuildWideString(const wchar_t* String ...); // The last variadic parameter should be always NULL
void FreeWideString(const wchar_t* String);

// =============================================
//  Numeric

size_t AlignToTop(size_t What, size_t Align);
size_t AlignToBottom(size_t What, size_t Align);

// =============================================
//  Sync

#ifdef _WIN64
typedef volatile long long SpinAtom;
#define AtomExchange InterlockedExchange64
#else
typedef volatile long SpinAtom;
#define AtomExchange InterlockedExchange
#endif

void AcquireSpinLock(SpinAtom* Spinlock);
void ReleaseSpinLock(SpinAtom* Spinlock);

unsigned int GetAmountOfCPUCores();
