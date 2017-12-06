#define _CRT_SECURE_NO_WARNINGS

#include <Windows.h>
#include <stdlib.h>
#include <stdio.h>
#include <tchar.h>
#include <ntdef.h>
#include <AVLTree.h>
#include <CommonLib.h>

/*TODO list:
- Add IOCP support if needed
+ Think about modified files : 0xC0000034 STATUS_OBJECT_NAME_NOT_FOUND
+ Exclude backup path from monitored
+ Solve backup duplication
+ Move stuff to common lib
+ Temporary generation
- More informative errors
*/

struct
{
    HANDLE           SourceDirHandle;
    wchar_t*         SourceDir;
    wchar_t*         DestTempDir;
    wchar_t*         DestBackupDir;
    wchar_t*         ExcludedPath;
    size_t           ExcludedPathLen;
    CRITICAL_SECTION FilesContextCS;
    AVL_TREE         FilesContext;
} g_MonitorContext;

struct FileContext
{
    wchar_t* Key;
    wchar_t* BackupFileName;
    wchar_t* TempFileName;
    HANDLE   TempFile;
};

// =============================================

void* AVLTreeAllocate(size_t NodeBufSize)
{
    return malloc(NodeBufSize);
}

void AVLTreeFree(void* NodeBuf, void* Node)
{
    FileContext* fileContext = (FileContext*)Node;

    DeleteFileW(fileContext->TempFileName);

    FreeWideString(fileContext->BackupFileName);
    FreeWideString(fileContext->TempFileName);
    FreeWideString(fileContext->Key);
    CloseHandle(fileContext->TempFile);

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
    bool result = false;

    if (IsPathExcluded(SourceFile))
    {
        printf("File skipped: %S\n", SourceFile);
        return true;
    }

    if (GetTempFileNameW(g_MonitorContext.DestTempDir, L"db_", 0, tempFile) == 0)
    {
        printf("Error, can't generate temporary file name, code %d\n", GetLastError());
        return false;
    }

    memset(&fileContext, 0, sizeof(fileContext));
    
    fullSourcePath = BuildWideString(g_MonitorContext.SourceDir, SourceFile, NULL);
    if (!fullSourcePath)
    {
        printf("Error, can't prepare source file name\n");
        goto ReleaseBlock;
    }

    if (!CreateHardLinkToExistingFile(tempFile, fullSourcePath))
    {
        printf("Error, can't create hard link, code: %d\n", GetLastError());
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
        printf("Error, can't open hard link, code: %d\n", GetLastError());
        goto ReleaseBlock;
    }

    fileContext.TempFileName = BuildWideString(tempFile, NULL);
    if (!fileContext.TempFileName)
    {
        printf("Error, can't allocate temp file name\n");
        goto ReleaseBlock;
    }

    fileContext.Key = BuildWideString(SourceFile, NULL);
    if (!fileContext.Key)
    {
        printf("Error, can't allocate key string\n");
        goto ReleaseBlock;
    }

    _wcslwr(fileContext.Key);

    fileContext.BackupFileName = BuildWideString(SourceFile, NULL);
    if (!fileContext.BackupFileName)
    {
        printf("Error, can't allocate key string\n");
        goto ReleaseBlock;
    }

    if (!InsertAVLElement(&g_MonitorContext.FilesContext, &fileContext, sizeof(fileContext)))
    {
        printf("Error, can't save file cache\n");
        goto ReleaseBlock;
    }

    result = true;
    
ReleaseBlock:

    if (!result)
    {
        if (fileContext.TempFile != INVALID_HANDLE_VALUE)
            CloseHandle(fileContext.TempFile);

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

    if (CreateHardLinkW(RestoredFilePath, FileContext->TempFileName, NULL))
        return true;

    if (GetLastError() != ERROR_ALREADY_EXISTS)
        return false;

    // If a file with the same name exists we should try to find out a different name for a restoration

    for (i = 1; i < 10000; i++)
    {
        wchar_t postfix[100];
        wchar_t* pathWithPostfix;
        BOOL result;

        _swprintf(postfix, L".%d", i);

        pathWithPostfix = BuildWideString(RestoredFilePath, postfix, NULL);
        if (!pathWithPostfix)
            return false;

        result = CreateHardLinkW(pathWithPostfix, FileContext->TempFileName, NULL);
        FreeWideString(pathWithPostfix);

        if (result || GetLastError() != ERROR_ALREADY_EXISTS)
            return result;
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
        printf("File skipped: %S\n", SourceFile);
        return true;
    }

    memset(&lookFileContext, 0, sizeof(lookFileContext));

    lookFileContext.Key = BuildWideString(SourceFile, NULL);
    if (!lookFileContext.Key)
    {
        printf("Error, can't allocate file key\n");
        goto ReleaseBlock;
    }

    _wcslwr(lookFileContext.Key);

    fileContext = (FileContext*)FindAVLElement(&g_MonitorContext.FilesContext, &lookFileContext);
    if (!fileContext)
        goto ReleaseBlock;

    restoredFilePath = BuildWideString(g_MonitorContext.DestBackupDir, SourceFile, NULL);
    if (!restoredFilePath)
    {
        printf("Error, can't allocate restored path\n");
        goto ReleaseBlock;
    }

    found = true;

    if (!RestoreBackupFromTemp(fileContext, restoredFilePath))
        goto ReleaseBlock;

    printf("File backuped: %S\n", SourceFile);
    result = true;

ReleaseBlock:

    if (found)
        RemoveAVLElement(&g_MonitorContext.FilesContext, &lookFileContext);

    if (lookFileContext.Key)
        FreeWideString(lookFileContext.Key);

    if (restoredFilePath)
        FreeWideString(restoredFilePath);

    return result;
}

// =============================================

void ReleaseBackupMonitorContext()
{
    if (g_MonitorContext.SourceDirHandle && g_MonitorContext.SourceDirHandle != INVALID_HANDLE_VALUE)
        ::CloseHandle(g_MonitorContext.SourceDirHandle);

    if (g_MonitorContext.SourceDir)
        FreeWideString(g_MonitorContext.SourceDir);

    if (g_MonitorContext.DestTempDir)
        FreeWideString(g_MonitorContext.DestTempDir);

    if (g_MonitorContext.DestBackupDir)
        FreeWideString(g_MonitorContext.DestBackupDir);

    if (g_MonitorContext.ExcludedPath)
        FreeWideString(g_MonitorContext.ExcludedPath);

    DestroyAVLTree(&g_MonitorContext.FilesContext);
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
            FILE_FLAG_BACKUP_SEMANTICS,
            NULL
        );
    if (g_MonitorContext.SourceDirHandle == INVALID_HANDLE_VALUE)
    {
        printf("Error, can't open directory '%S', code %d\n", SourceDir, GetLastError());
        return false;
    }

    g_MonitorContext.SourceDir = BuildWideString(SourceDir, L"\\", NULL);
    if (!g_MonitorContext.SourceDir)
    {
        printf("Error, can't prepare source directory path\n");
        return false;
    }

    return true;
}

bool InitBackupDirContext(wchar_t* BackupDir)
{
    g_MonitorContext.DestTempDir = BuildWideString(BackupDir, L"\\temp", NULL);
    if (!g_MonitorContext.DestTempDir)
    {
        printf("Error, can't prepare temporary directory path\n");
        return false;
    }

    g_MonitorContext.DestBackupDir = BuildWideString(BackupDir, L"\\backup\\", NULL);
    if (!g_MonitorContext.DestBackupDir)
    {
        printf("Error, can't prepare backup directory path\n");
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
        printf("Error, can't create temporary directory '%S', code %d\n", g_MonitorContext.DestTempDir, ::GetLastError());
        return false;
    }

    if (!::CreateDirectoryW(g_MonitorContext.DestBackupDir, NULL) && ::GetLastError() != ERROR_ALREADY_EXISTS)
    {
        printf("Error, can't create backup directory '%S', code %d\n", g_MonitorContext.DestBackupDir, ::GetLastError());
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
        CreateFileW(
            SourceDir,
            FILE_LIST_DIRECTORY,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            NULL,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS,
            NULL
        );
    if (monitoredDirHandle == INVALID_HANDLE_VALUE)
        goto ReleaseBlock;

    if (!GetFileInformationByHandleEx(monitoredDirHandle, FileNameInfo, fileInfoBuf[0], sizeof(fileInfoBuf[0]) - sizeof(wchar_t)))
        goto ReleaseBlock;

    backupDirHandle =
        CreateFileW(
            BackupDir,
            FILE_LIST_DIRECTORY,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            NULL,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS,
            NULL
        );
    if (backupDirHandle == INVALID_HANDLE_VALUE)
        goto ReleaseBlock;

    if (!GetFileInformationByHandleEx(backupDirHandle, FileNameInfo, fileInfoBuf[1], sizeof(fileInfoBuf[1]) - sizeof(wchar_t)))
        goto ReleaseBlock;

    monitoredFileInfo->FileName[monitoredFileInfo->FileNameLength / sizeof(wchar_t)] = L'\0';
    backupFileInfo->FileName[backupFileInfo->FileNameLength / sizeof(wchar_t)] = L'\0';

    SetExcludedPath(monitoredFileInfo->FileName, backupFileInfo->FileName);

ReleaseBlock:

    if (backupDirHandle != INVALID_HANDLE_VALUE)
        CloseHandle(backupDirHandle);

    if (monitoredDirHandle != INVALID_HANDLE_VALUE)
        CloseHandle(monitoredDirHandle);

    return result;
}

bool InitBackupMonitorContext(const wchar_t* SourceDir, wchar_t* BackupDir)
{
    bool result = false;

    memset(&g_MonitorContext, 0, sizeof(g_MonitorContext));

    InitializeAVLTree(&g_MonitorContext.FilesContext, AVLTreeAllocate, AVLTreeFree, AVLTreeCompare);

    if (!InitMonitoredDirContext(SourceDir))
        goto ReleaseBlock;

    if (!InitBackupDirContext(BackupDir))
        goto ReleaseBlock;

    if (!CreateBackupDir(BackupDir))
        goto ReleaseBlock;

    if (!InitExcludedPath(SourceDir, BackupDir))
        goto ReleaseBlock;

    result = true;

ReleaseBlock:

    if (!result)
        ReleaseBackupMonitorContext();
    
    return result;
}

bool StartBackupMonitor(wchar_t* SourceDir, wchar_t* BackupDir)
{
    enum { ChangeInformationBlockSize = 0x1000 };
    PFILE_NOTIFY_INFORMATION changeInfo;
    DWORD returned;

    if (!SetTokenPrivilege("SeCreateSymbolicLinkPrivilege", TRUE))
        return false;

    if (!InitBackupMonitorContext(SourceDir, BackupDir))
        return false;
    
    changeInfo = (PFILE_NOTIFY_INFORMATION)::VirtualAlloc(NULL, ChangeInformationBlockSize, MEM_COMMIT, PAGE_READWRITE);
    if (!changeInfo)
    {
        printf("Error, can't allocate change information block, code: %d\n", ::GetLastError());
        ReleaseBackupMonitorContext();
        return false;
    }

    while (true)
    {
        PFILE_NOTIFY_INFORMATION currentInfo = changeInfo;

        if (!::ReadDirectoryChangesW(
                g_MonitorContext.SourceDirHandle, 
                changeInfo, 
                ChangeInformationBlockSize,
                TRUE, 
                FILE_NOTIFY_CHANGE_FILE_NAME,
                &returned, 
                NULL, 
                NULL))
        {
            printf("Error, failed to read directory changes, code: %d\n", GetLastError());
            ReleaseBackupMonitorContext();
            return false;
        }

        while (true)
        {
            if (currentInfo->FileNameLength + sizeof(FILE_NOTIFY_INFORMATION) + sizeof(WCHAR) > ChangeInformationBlockSize)
                break;

            switch (currentInfo->Action)
            {
            case FILE_ACTION_ADDED:
                printf("FILE_ACTION_ADDED ");
                break;
            case FILE_ACTION_RENAMED_NEW_NAME:
                printf("FILE_ACTION_RENAMED_NEW_NAME ");
                break;
            case FILE_ACTION_REMOVED:
                printf("FILE_ACTION_REMOVED ");
                break;
            case FILE_ACTION_RENAMED_OLD_NAME:
                printf("FILE_ACTION_RENAMED_OLD_NAME ");
                break;
            default:
                printf("UNKNOWN ");
                break;
            }

            currentInfo->FileName[currentInfo->FileNameLength / sizeof(WCHAR)] = L'\0';
            printf("%S\n", currentInfo->FileName);

            if (currentInfo->Action == FILE_ACTION_ADDED || currentInfo->Action == FILE_ACTION_RENAMED_NEW_NAME)
                CreateTemporaryBackup(currentInfo->FileName);
            else if (currentInfo->Action == FILE_ACTION_REMOVED || currentInfo->Action == FILE_ACTION_RENAMED_OLD_NAME)
                UpgradeBackupToConstant(currentInfo->FileName);
            
            if (!currentInfo->NextEntryOffset)
                break;

            currentInfo = (PFILE_NOTIFY_INFORMATION)((char*)currentInfo + currentInfo->NextEntryOffset);
        }
    }
    
    return true;
}

void StopBackupMonitor()
{
    ReleaseBackupMonitorContext();
}

// =============================================

int wmain(int argc, wchar_t* argv[])
{
    if (argc != 3)
    {
        printf("Error, invalid arguments, usage: tmpbackup <src dir> <backup dir>\n");
        return 1;
    }

    if (!StartBackupMonitor(argv[1], argv[2]))
        return 2;

    printf("Press ENTER to exit\n");
    getchar();

    StopBackupMonitor();

    return 0;
}
