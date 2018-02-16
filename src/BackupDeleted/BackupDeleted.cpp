#define _CRT_SECURE_NO_WARNINGS

#include <Windows.h>
#include <stdlib.h>
#include <stdio.h>
#include <tchar.h>
#include <ntdef.h>
#include <AVLTree.h>
#include <CommonLib.h>
#include <ConsolePrinter.h>

/*TODO list:
+ Add IOCP support if needed
+ Think about modified files : 0xC0000034 STATUS_OBJECT_NAME_NOT_FOUND
+ Exclude backup path from monitored
+ Solve backup duplication
+ Move stuff to common lib
+ Temporary generation
+ More informative errors
- Investigate dir sharing violation
+ Investigate non-tracked deletion
- Increase temp file generation speed
- Investigate issue with multithreading
*/

struct OperationContext
{
    OVERLAPPED Overlapped;
    PFILE_NOTIFY_INFORMATION ChangeInfo;
    HANDLE     Thread;
    HANDLE     StartStopEvent;
    DWORD      Index;
};

struct
{
    HANDLE            SourceDirHandle;
    HANDLE            SourceDirIocp;
    wchar_t*          SourceDir;
    wchar_t*          DestTempDir;
    wchar_t*          DestBackupDir;
    wchar_t*          ExcludedPath;
    size_t            ExcludedPathLen;
    CRITICAL_SECTION  FilesContextCS;
    AVL_TREE          FilesContext;
    OperationContext* Operations;
    unsigned int      OperationsCount;
    void*             OperationsBuffer;
    size_t            OperationsBufferSize;
} g_MonitorContext;

struct FileContext
{
    wchar_t* Key;
    wchar_t* BackupFileName;
    wchar_t* TempFileName;
    HANDLE   TempFile;
};

ConsoleInstance g_consoleContext = NULL;

// =============================================

void* AVLTreeAllocate(size_t NodeBufSize)
{
    return malloc(NodeBufSize);
}

void AVLTreeFree(void* NodeBuf, void* Node)
{
    FileContext* fileContext = (FileContext*)Node;

    ::DeleteFileW(fileContext->TempFileName);

    FreeWideString(fileContext->BackupFileName);
    FreeWideString(fileContext->TempFileName);
    FreeWideString(fileContext->Key);
    ::CloseHandle(fileContext->TempFile);

    free(NodeBuf);
}

int AVLTreeCompare(void* Node1, void* Node2)
{
    FileContext* file1 = (FileContext*)Node1;
    FileContext* file2 = (FileContext*)Node2;
    return wcscmp(file1->Key, file2->Key);
}

// =============================================

bool IsPathExcluded(const wchar_t* Path)
{
    if (!g_MonitorContext.ExcludedPath)
        return false;

    return (_wcsnicmp(g_MonitorContext.ExcludedPath, Path, g_MonitorContext.ExcludedPathLen) == 0);
}

bool CreateTemporaryBackup(const wchar_t* SourceFile)
{
    FileContext fileContext;
    wchar_t tempFile[MAX_PATH + 1];
    wchar_t* fullSourcePath = 0;
    void* insert;
    bool result = false;

    if (IsPathExcluded(SourceFile))
    {
        PrintMsg(PrintColors::Default, L"File skipped: %s\n", SourceFile);
        return true;
    }

    if (::GetTempFileNameW(g_MonitorContext.DestTempDir, L"db_", 0, tempFile) == 0)
    {
        PrintMsg(PrintColors::Red, L"Error, can't generate temporary file name, code %d\n", ::GetLastError());
        return false;
    }

    memset(&fileContext, 0, sizeof(fileContext));
    
    fullSourcePath = BuildWideString(g_MonitorContext.SourceDir, SourceFile, NULL);
    if (!fullSourcePath)
    {
        PrintMsg(PrintColors::Red, L"Error, can't prepare source file name\n");
        goto ReleaseBlock;
    }

    if (!CreateHardLinkToExistingFile(tempFile, fullSourcePath))
    {
        PrintMsg(PrintColors::Red, L"Error, can't create hard link, code: %d\n", ::GetLastError());
        goto ReleaseBlock;
    }

    fileContext.TempFile = ::CreateFileW(
        tempFile,
        SYNCHRONIZE, 
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, 
        OPEN_EXISTING, 
        0, 
        NULL
    );
    if (fileContext.TempFile == INVALID_HANDLE_VALUE)
    {
        PrintMsg(PrintColors::Red, L"Error, can't open hard link, code: %d\n", ::GetLastError());
        goto ReleaseBlock;
    }

    fileContext.TempFileName = BuildWideString(tempFile, NULL);
    if (!fileContext.TempFileName)
    {
        PrintMsg(PrintColors::Red, L"Error, can't allocate temp file name\n");
        goto ReleaseBlock;
    }

    fileContext.Key = BuildWideString(SourceFile, NULL);
    if (!fileContext.Key)
    {
        PrintMsg(PrintColors::Red, L"Error, can't allocate key string\n");
        goto ReleaseBlock;
    }

    _wcslwr(fileContext.Key);

    fileContext.BackupFileName = BuildWideString(SourceFile, NULL);
    if (!fileContext.BackupFileName)
    {
        PrintMsg(PrintColors::Red, L"Error, can't allocate backup file name string\n");
        goto ReleaseBlock;
    }

    ::EnterCriticalSection(&g_MonitorContext.FilesContextCS);
    insert = InsertAVLElement(&g_MonitorContext.FilesContext, &fileContext, sizeof(fileContext));
    ::LeaveCriticalSection(&g_MonitorContext.FilesContextCS);
    if (!insert)
    {
        PrintMsg(PrintColors::Red, L"Error, can't save file cache\n");
        goto ReleaseBlock;
    }

    result = true;
    
ReleaseBlock:

    if (!result)
    {
        if (fileContext.TempFile != INVALID_HANDLE_VALUE)
            ::CloseHandle(fileContext.TempFile);

        if (fileContext.BackupFileName)
            FreeWideString(fileContext.BackupFileName);

        if (fileContext.TempFileName)
            FreeWideString(fileContext.TempFileName);

        if (fileContext.Key)
            FreeWideString(fileContext.Key);
    }

    return result;
}

bool RestoreBackupFromTemp(FileContext* FileContext, wchar_t* RestoredFilePath)
{
    size_t i = wcslen(RestoredFilePath);
    bool isDirReady = false;

    if (i == 0)
        return false;

    do
    {
        i--;

        if (RestoredFilePath[i] == L'\\' || RestoredFilePath[i] == L'/')
        {
            wchar_t chr = RestoredFilePath[i];
            RestoredFilePath[i] = L'\0';
            isDirReady = CreateDirectoryByFullPath(RestoredFilePath);
            RestoredFilePath[i] = chr;
            break;
        }
    }
    while (i > 0);

    if (!isDirReady)
        return false;

    if (::CreateHardLinkW(RestoredFilePath, FileContext->TempFileName, NULL))
        return true;

    if (::GetLastError() != ERROR_ALREADY_EXISTS)
        return false;

    // If a file with the same name exists we should try to find out a different name for a restoration

    for (i = 1; i < 10000; i++)
    {
        wchar_t postfix[100];
        wchar_t* pathWithPostfix;
        BOOL result;

        swprintf_s(postfix, L".%d", i);

        pathWithPostfix = BuildWideString(RestoredFilePath, postfix, NULL);
        if (!pathWithPostfix)
            return false;

        result = ::CreateHardLinkW(pathWithPostfix, FileContext->TempFileName, NULL);
        FreeWideString(pathWithPostfix);

        if (result || ::GetLastError() != ERROR_ALREADY_EXISTS)
            return (result == TRUE);
    }

    return false;
}

bool UpgradeBackupToConstant(const wchar_t* SourceFile)
{
    FileContext lookFileContext;
    FileContext* fileContext;
    wchar_t* restoredFilePath = 0;
    bool result = false;
    bool found = false;

    if (IsPathExcluded(SourceFile))
    {
        PrintMsg(PrintColors::Default, L"File skipped: %s\n", SourceFile);
        return true;
    }

    memset(&lookFileContext, 0, sizeof(lookFileContext));

    lookFileContext.Key = BuildWideString(SourceFile, NULL);
    if (!lookFileContext.Key)
    {
        PrintMsg(PrintColors::Red, L"Error, can't allocate key string\n");
        goto ReleaseBlock;
    }

    _wcslwr(lookFileContext.Key);

    ::EnterCriticalSection(&g_MonitorContext.FilesContextCS);
    fileContext = (FileContext*)FindAVLElement(&g_MonitorContext.FilesContext, &lookFileContext);
    ::LeaveCriticalSection(&g_MonitorContext.FilesContextCS);
    if (!fileContext)
        goto ReleaseBlock;

    restoredFilePath = BuildWideString(g_MonitorContext.DestBackupDir, SourceFile, NULL);
    if (!restoredFilePath)
    {
        PrintMsg(PrintColors::Red, L"Error, can't allocate restored path\n");
        goto ReleaseBlock;
    }

    found = true;

    if (!RestoreBackupFromTemp(fileContext, restoredFilePath))
        goto ReleaseBlock;

    PrintMsg(PrintColors::Green, L"File backuped: %s\n", SourceFile);
    result = true;

ReleaseBlock:

    if (found)
    {
        ::EnterCriticalSection(&g_MonitorContext.FilesContextCS);
        RemoveAVLElement(&g_MonitorContext.FilesContext, &lookFileContext);
        ::LeaveCriticalSection(&g_MonitorContext.FilesContextCS);
    }

    if (lookFileContext.Key)
        FreeWideString(lookFileContext.Key);

    if (restoredFilePath)
        FreeWideString(restoredFilePath);

    return result;
}

// =============================================

void ReleaseBackupMonitorContext()
{
    unsigned int i;

    if (g_MonitorContext.SourceDirHandle && g_MonitorContext.SourceDirHandle != INVALID_HANDLE_VALUE)
        ::CloseHandle(g_MonitorContext.SourceDirHandle);

    if (g_MonitorContext.SourceDirIocp)
        ::CloseHandle(g_MonitorContext.SourceDirIocp);

    if (g_MonitorContext.SourceDir)
        FreeWideString(g_MonitorContext.SourceDir);

    if (g_MonitorContext.DestTempDir)
        FreeWideString(g_MonitorContext.DestTempDir);

    if (g_MonitorContext.DestBackupDir)
        FreeWideString(g_MonitorContext.DestBackupDir);

    if (g_MonitorContext.ExcludedPath)
        FreeWideString(g_MonitorContext.ExcludedPath);

    if (g_MonitorContext.Operations)
    {
        for (i = 0; i < g_MonitorContext.OperationsCount; i++)
            if (g_MonitorContext.Operations[i].Thread)
                ::CloseHandle(g_MonitorContext.Operations[i].Thread);

        free(g_MonitorContext.Operations);
    }

    if (g_MonitorContext.OperationsBuffer)
        ::VirtualFree(g_MonitorContext.OperationsBuffer, 0, MEM_RELEASE);

    DestroyAVLTree(&g_MonitorContext.FilesContext);
    ::DeleteCriticalSection(&g_MonitorContext.FilesContextCS);
}

bool InitMonitoredDirContext(const wchar_t* SourceDir)
{
    g_MonitorContext.SourceDirHandle =
        ::CreateFileW(
            SourceDir,
            FILE_LIST_DIRECTORY,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            NULL,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
            NULL
        );
    if (g_MonitorContext.SourceDirHandle == INVALID_HANDLE_VALUE)
    {
        PrintMsg(PrintColors::Red, L"Error, can't open directory '%s', code %d\n", SourceDir, ::GetLastError());
        return false;
    }

    g_MonitorContext.SourceDir = BuildWideString(SourceDir, L"\\", NULL);
    if (!g_MonitorContext.SourceDir)
    {
        PrintMsg(PrintColors::Red, L"Error, can't prepare source directory path\n");
        return false;
    }

    g_MonitorContext.SourceDirIocp = CreateIoCompletionPort(g_MonitorContext.SourceDirHandle, NULL, 0, NULL);
    if (!g_MonitorContext.SourceDirIocp)
    {
        PrintMsg(PrintColors::Red, L"Error, can't init io completion port, code %d\n", ::GetLastError());
        return false;
    }

    return true;
}

bool InitBackupDirContext(wchar_t* BackupDir)
{
    g_MonitorContext.DestTempDir = BuildWideString(BackupDir, L"\\temp", NULL);
    if (!g_MonitorContext.DestTempDir)
    {
        PrintMsg(PrintColors::Red, L"Error, can't prepare temporary directory path\n");
        return false;
    }

    g_MonitorContext.DestBackupDir = BuildWideString(BackupDir, L"\\backup\\", NULL);
    if (!g_MonitorContext.DestBackupDir)
    {
        PrintMsg(PrintColors::Red, L"Error, can't prepare backup directory path\n");
        return false;
    }

    return true;
}

bool CreateBackupDir(wchar_t* BackupDir)
{
    if (!CreateDirectoryByFullPath(BackupDir))
        return false;

    if (!::CreateDirectoryW(g_MonitorContext.DestTempDir, NULL) && ::GetLastError() != ERROR_ALREADY_EXISTS)
    {
        PrintMsg(
            PrintColors::Red, 
            L"Error, can't create temporary directory '%s', code %d\n", 
            g_MonitorContext.DestTempDir, 
            ::GetLastError()
        );
        return false;
    }

    if (!::CreateDirectoryW(g_MonitorContext.DestBackupDir, NULL) && ::GetLastError() != ERROR_ALREADY_EXISTS)
    {
        PrintMsg(
            PrintColors::Red, 
            L"Error, can't create backup directory '%s', code %d\n", 
            g_MonitorContext.DestBackupDir, 
            ::GetLastError()
        );
        return false;
    }

    return true;
}

void SetExcludedPath(const wchar_t* SourceDir, wchar_t* BackupDir)
{
    size_t i;
    bool isExcludable = false;

    for (i = 0; SourceDir[i]; i++)
    {
        if (!BackupDir[i])
            return;

        if (SourceDir[i] != BackupDir[i])
            return;
    }

    if (i > 0 && SourceDir[i - 1] == L'\\' && BackupDir[i - 1] == L'\\')
        isExcludable = true;
    else if (BackupDir[i] == L'\0' || BackupDir[i] == L'\\')
        isExcludable = true;
    
    if (!isExcludable)
        return;

    if (BackupDir[i] == '\\')
        i++;

    g_MonitorContext.ExcludedPath = BuildWideString(BackupDir + i, L"\\", NULL);
    g_MonitorContext.ExcludedPathLen = wcslen(g_MonitorContext.ExcludedPath);
}

bool InitExcludedPath(const wchar_t* SourceDir, const wchar_t* BackupDir)
{
    HANDLE backupDirHandle = INVALID_HANDLE_VALUE;
    HANDLE monitoredDirHandle = INVALID_HANDLE_VALUE;
    wchar_t fileInfoBuf[2][MAX_PATH * 2];
    PFILE_NAME_INFO monitoredFileInfo = (PFILE_NAME_INFO)fileInfoBuf[0];
    PFILE_NAME_INFO backupFileInfo = (PFILE_NAME_INFO)fileInfoBuf[1];
    bool result = true;

    g_MonitorContext.ExcludedPath = NULL;

    monitoredDirHandle =
        ::CreateFileW(
            SourceDir,
            FILE_LIST_DIRECTORY,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            NULL,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS,
            NULL
        );
    if (monitoredDirHandle == INVALID_HANDLE_VALUE)
    {
        PrintMsg(
            PrintColors::Red, 
            L"Error, can't open directory '%s', code %d\n", 
            SourceDir,
            ::GetLastError()
        );
        goto ReleaseBlock;
    }

    if (!::GetFileInformationByHandleEx(monitoredDirHandle, FileNameInfo, fileInfoBuf[0], sizeof(fileInfoBuf[0]) - sizeof(wchar_t)))
    {
        PrintMsg(
            PrintColors::Red, 
            L"Error, can't receive information about directory '%s', code %d\n", 
            SourceDir,
            ::GetLastError()
        );
        goto ReleaseBlock;
    }

    backupDirHandle =
        ::CreateFileW(
            BackupDir,
            FILE_LIST_DIRECTORY,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            NULL,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS,
            NULL
        );
    if (backupDirHandle == INVALID_HANDLE_VALUE)
    {
        PrintMsg(
            PrintColors::Red, 
            L"Error, can't open directory '%s', code %d\n", 
            BackupDir,
            ::GetLastError()
        );
        goto ReleaseBlock;
    }

    if (!::GetFileInformationByHandleEx(backupDirHandle, FileNameInfo, fileInfoBuf[1], sizeof(fileInfoBuf[1]) - sizeof(wchar_t)))
    {
        PrintMsg(
            PrintColors::Red, 
            L"Error, can't receive information about directory '%s', code %d\n", 
            BackupDir,
            ::GetLastError()
        );
        goto ReleaseBlock;
    }

    monitoredFileInfo->FileName[monitoredFileInfo->FileNameLength / sizeof(wchar_t)] = L'\0';
    backupFileInfo->FileName[backupFileInfo->FileNameLength / sizeof(wchar_t)] = L'\0';

    SetExcludedPath(monitoredFileInfo->FileName, backupFileInfo->FileName);

ReleaseBlock:

    if (backupDirHandle != INVALID_HANDLE_VALUE)
        ::CloseHandle(backupDirHandle);

    if (monitoredDirHandle != INVALID_HANDLE_VALUE)
        ::CloseHandle(monitoredDirHandle);

    return result;
}

bool InitOperationsContext()
{
    enum { ChangeInformationBlockSize = 0x1000 };
    unsigned int i;

    g_MonitorContext.OperationsCount = GetAmountOfCPUCores() * 2;

    g_MonitorContext.Operations = (OperationContext*)malloc(sizeof(OperationContext) * g_MonitorContext.OperationsCount);
    if (!g_MonitorContext.Operations)
    {
        PrintMsg(PrintColors::Red, L"Error, can't allocate operations context\n");
        return false;
    }

    g_MonitorContext.OperationsBufferSize = ChangeInformationBlockSize;
    g_MonitorContext.OperationsBuffer = ::VirtualAlloc(
        NULL, 
        ChangeInformationBlockSize * g_MonitorContext.OperationsCount,
        MEM_COMMIT, 
        PAGE_READWRITE
    );
    if (!g_MonitorContext.OperationsBuffer)
    {
        PrintMsg(
            PrintColors::Red, 
            L"Error, can't allocate operations buffer, code %d\n", 
            ::GetLastError()
        );
        return false;
    }

    for (i = 0; i < g_MonitorContext.OperationsCount; i++)
    {
        g_MonitorContext.Operations[i].StartStopEvent = ::CreateEvent(NULL, FALSE, FALSE, NULL);
        if (!g_MonitorContext.Operations[i].StartStopEvent)
        {
            PrintMsg(
                PrintColors::Red, 
                L"Error, can't create sync event, code %d\n", 
                ::GetLastError()
            );
            return false;
        }

        g_MonitorContext.Operations[i].Index = i;
        g_MonitorContext.Operations[i].ChangeInfo = (PFILE_NOTIFY_INFORMATION)(
            (char*)g_MonitorContext.OperationsBuffer + (ChangeInformationBlockSize * i)
        );
        g_MonitorContext.Operations[i].Thread = NULL;
        memset(&g_MonitorContext.Operations[i].Overlapped, 0, sizeof(g_MonitorContext.Operations[i].Overlapped));
    }

    return true;
}

bool InitBackupMonitorContext(const wchar_t* SourceDir, wchar_t* BackupDir)
{
    bool result = false;

    memset(&g_MonitorContext, 0, sizeof(g_MonitorContext));

    ::InitializeCriticalSection(&g_MonitorContext.FilesContextCS);
    InitializeAVLTree(&g_MonitorContext.FilesContext, AVLTreeAllocate, AVLTreeFree, AVLTreeCompare);

    if (!InitMonitoredDirContext(SourceDir))
        goto ReleaseBlock;

    if (!InitBackupDirContext(BackupDir))
        goto ReleaseBlock;

    if (!CreateBackupDir(BackupDir))
        goto ReleaseBlock;

    if (!InitExcludedPath(SourceDir, BackupDir))
        goto ReleaseBlock;

    if (!InitOperationsContext())
        goto ReleaseBlock;

    result = true;

ReleaseBlock:

    if (!result)
        ReleaseBackupMonitorContext();
    
    return result;
}

DWORD WINAPI MonitoringRoutine(LPVOID Parameter)
{
    OperationContext* context = g_MonitorContext.Operations + (uintptr_t)Parameter;

    ::SetEvent(context->StartStopEvent);

    while (true)
    {
        OperationContext* current;
        PFILE_NOTIFY_INFORMATION info;
        LPOVERLAPPED overlapped;
        ULONG_PTR key;
        DWORD returned;

        if (!::GetQueuedCompletionStatus(g_MonitorContext.SourceDirIocp, &returned, &key, &overlapped, INFINITE))
        {
            PrintMsg(PrintColors::Red, L"Error, can't receive iocp status, code: %d\n", ::GetLastError());
            break;
        }

        if (!returned)
            break;

        current = (OperationContext*)overlapped;
        info = current->ChangeInfo;

        while (true)
        {
            wchar_t format[100];
            PrintColors color = PrintColors::Default;

            if (info->FileNameLength + sizeof(FILE_NOTIFY_INFORMATION) + sizeof(WCHAR) > g_MonitorContext.OperationsBufferSize)
                break;

            info->FileName[info->FileNameLength / sizeof(WCHAR)] = L'\0';

            if (info->Action == FILE_ACTION_ADDED || info->Action == FILE_ACTION_RENAMED_NEW_NAME)
                CreateTemporaryBackup(info->FileName);
            else if (info->Action == FILE_ACTION_REMOVED || info->Action == FILE_ACTION_RENAMED_OLD_NAME)
                UpgradeBackupToConstant(info->FileName);
            
            switch (info->Action)
            {
            case FILE_ACTION_ADDED:
                wcscpy_s(format, L"FILE_ACTION_ADDED (inx:%d) %s\n");
                color = PrintColors::DarkGreen;
                break;
            case FILE_ACTION_RENAMED_NEW_NAME:
                wcscpy_s(format, L"FILE_ACTION_RENAMED_NEW_NAME (inx:%d) %s\n");
                color = PrintColors::DarkYellow;
                break;
            case FILE_ACTION_REMOVED:
                wcscpy_s(format, L"FILE_ACTION_REMOVED (inx:%d) %s\n");
                color = PrintColors::DarkRed;
                break;
            case FILE_ACTION_RENAMED_OLD_NAME:
                wcscpy_s(format, L"FILE_ACTION_RENAMED_OLD_NAME (inx:%d) %s\n");
                color = PrintColors::DarkYellow;
                break;
            default:
                wcscpy_s(format, L"UNKNOWN (inx:%d) %s\n");
                color = PrintColors::Red;
                break;
            }

            PrintMsg(color, format, context->Index, info->FileName);

            if (!info->NextEntryOffset)
                break;

            info = (PFILE_NOTIFY_INFORMATION)((char*)info + info->NextEntryOffset);
        }

        if (!::ReadDirectoryChangesW(
            g_MonitorContext.SourceDirHandle,
            current->ChangeInfo,
            g_MonitorContext.OperationsBufferSize,
            TRUE,
            FILE_NOTIFY_CHANGE_FILE_NAME,
            &returned,
            overlapped,
            NULL))
        {
            PrintMsg(PrintColors::Red, L"Error, failed to read directory changes, code: %d\n", ::GetLastError());
            break;
        }
    }

    ::SetEvent(context->StartStopEvent);

    return 0;
}

bool StartBackupMonitor(wchar_t* SourceDir, wchar_t* BackupDir)
{
    unsigned int i;

    if (!SetTokenPrivilege("SeCreateSymbolicLinkPrivilege", TRUE))
        return false;

    if (!InitBackupMonitorContext(SourceDir, BackupDir))
        return false;

    for (i = 0; i < g_MonitorContext.OperationsCount; i++)
    {
        DWORD threadId, error;
        OperationContext* context = g_MonitorContext.Operations + i;
        
        context->Thread = ::CreateThread(NULL, 0, MonitoringRoutine, (LPVOID)i, 0, &threadId);
        if (!context->Thread)
        {   
            PrintMsg(
                PrintColors::Yellow, 
                L"Warning, can't create working thread, code %d\n", 
                ::GetLastError()
            );
            continue;
        }

        error = ::WaitForSingleObject(context->StartStopEvent, 1000);
        if (error != WAIT_OBJECT_0)
        {
            PrintMsg(
                PrintColors::Yellow, 
                L"Warning, working thread (tid:%d,inx:%d) didn't respond, code %d\n",
                threadId, 
                context->Index,
                ::GetLastError()
            );
        }
    }

    for (i = 0; i < g_MonitorContext.OperationsCount; i++)
    {
        OperationContext* context = g_MonitorContext.Operations + i;
        DWORD returned;

        if (!::ReadDirectoryChangesW(
            g_MonitorContext.SourceDirHandle,
            context->ChangeInfo,
            g_MonitorContext.OperationsBufferSize,
            TRUE,
            FILE_NOTIFY_CHANGE_FILE_NAME,
            &returned,
            &context->Overlapped,
            NULL)
        )
            PrintMsg(
                PrintColors::Yellow,
                L"Warning, failed to read directory changes (inx:%d), code: %d\n", 
                context->Index, 
                ::GetLastError()
            );
    }

    return true;
}

void StopBackupMonitor()
{
    unsigned int i;

    for (i = 0; i < g_MonitorContext.OperationsCount; i++)
    {
        OperationContext* context = g_MonitorContext.Operations + i;

        if (!::PostQueuedCompletionStatus(g_MonitorContext.SourceDirIocp, 0, 0, &context->Overlapped))
            PrintMsg(
                PrintColors::Yellow,
                L"Warning, can't post completion status (inx:%d), code: %d\n", 
                context->Index,
                ::GetLastError()
            );
    }

    for (i = 0; i < g_MonitorContext.OperationsCount; i++)
    {
        OperationContext* context = g_MonitorContext.Operations + i;
        ::WaitForSingleObject(context->StartStopEvent, INFINITE);
    }

    ReleaseBackupMonitorContext();
}

// =============================================

int wmain(int argc, wchar_t* argv[])
{
    g_consoleContext = CreateAsyncConsolePrinterContext(PrintColors::Default, true);
    if (!g_consoleContext)
    {
        printf("Error, can't initialize console printer\n");
        return 1;
    }

    PrintMsg(PrintColors::Default, L"Backup deleted files by JKornev, 2017\n");

    if (argc != 3)
    {
        PrintMsg(PrintColors::Red, L"Error, invalid arguments, usage: BackupDeleted <SourceDir> <BackupDir>\n");
        DestroyAsyncConsolePrinterContext(g_consoleContext);
        return 1;
    }

    PrintMsg(PrintColors::Gray, L"Source directory: %s\n", argv[1]);
    PrintMsg(PrintColors::Gray, L"Backup directory: %s\n", argv[2]);

    if (!StartBackupMonitor(argv[1], argv[2]))
    {
        DestroyAsyncConsolePrinterContext(g_consoleContext);
        return 2;
    }

    PrintMsg(PrintColors::Default, L"Press ENTER to exit\n");
    getchar();

    StopBackupMonitor();

    DestroyAsyncConsolePrinterContext(g_consoleContext);

    return 0;
}
