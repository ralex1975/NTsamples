#include <Windows.h>
#include <stdlib.h>
#include <stdio.h>
#include <tchar.h>
#include "ntdef.h"
#include "AVLTree.h"

/*TODO list:
- Add IOCP support if needed
+ Think about modified files : 0xC0000034 STATUS_OBJECT_NAME_NOT_FOUND
- Why we leak file handles?
*/

struct
{
    HANDLE           SourceDirHandle;
    wchar_t*         SourceDir;
    wchar_t*         DestTempDir;
    wchar_t*         DestBackupDir;
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

wchar_t* AllocateString(const wchar_t* String, const wchar_t* Postfix = NULL, bool MakeLowerCase = false)
{
    size_t len = wcslen(String);
    size_t size = len;

    if (Postfix)
        size += wcslen(Postfix);

    size = (size + 1) * sizeof(wchar_t);

    wchar_t* newString = (wchar_t*)malloc(size);
    if (!newString)
        return NULL;

    wcsncpy(newString, String, len);
    if (Postfix)
        wcscpy(newString + len, Postfix);
    else
        newString[len] = L'\0';

    if (MakeLowerCase)
        _wcslwr(newString);

    return newString;
}

void ReleaseString(const wchar_t* String)
{
    free((void*)String);
}

// =============================================

void* AVLTreeAllocate(size_t NodeBufSize)
{
    return malloc(NodeBufSize);
}

void AVLTreeFree(void* NodeBuf, void* Node)
{
    FileContext* fileContext = (FileContext*)Node;

    DeleteFileW(fileContext->TempFileName);

    ReleaseString(fileContext->BackupFileName);
    ReleaseString(fileContext->TempFileName);
    ReleaseString(fileContext->Key);
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

bool SetPrivileges(LPCSTR Privilege, BOOL Enable)
{
    HANDLE token = 0;
    TOKEN_PRIVILEGES tp = {};
    LUID luid = {};
    bool result = false;

    do
    {
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES, &token))
        {
            printf("Error, OpenProcessToken() failed, code %d\n", GetLastError());
            break;
        }

        if (!LookupPrivilegeValueA(NULL, Privilege, &luid))
        {
            printf("Error, LookupPrivilegeValue() failed, code %d\n", GetLastError());
            break;
        }

        tp.PrivilegeCount = 1;
        tp.Privileges[0].Luid = luid;
        tp.Privileges[0].Attributes = (Enable ? SE_PRIVILEGE_ENABLED : 0);

        if (!AdjustTokenPrivileges(token, FALSE, &tp, sizeof(TOKEN_PRIVILEGES), NULL, NULL))
        {
            printf("Error, AdjustTokenPrivileges() failed, code %d\n", GetLastError());
            break;
        }

        result = true;
    }
    while (false);

    if (token)
        CloseHandle(token);

    return result;
}

HANDLE CreateHardLinkInternal(const wchar_t* DestinationFile, const wchar_t* SourceFile)
{// Is it possible to replace NtCreateFile to CreateFile??
 // TODO: remove handle from routine
    UNICODE_STRING destPath = {}, srcPath = {};
    OBJECT_ATTRIBUTES attribs = {};
    IO_STATUS_BLOCK ioStatus;
    PFILE_LINK_INFORMATION info;
    NTSTATUS status;
    HANDLE file = INVALID_HANDLE_VALUE;
    HANDLE fileHL = INVALID_HANDLE_VALUE;
    char* buffer = 0;
    size_t buffSize, sourceSize;
    bool result = false;

    do
    {
        if (!RtlDosPathNameToNtPathName_U(SourceFile, &srcPath, NULL, NULL))
        {
            printf("Error, can't convert DOS path '%S' to NT path\n", SourceFile);
            break;
        }

        InitializeObjectAttributes(&attribs, &srcPath, OBJ_CASE_INSENSITIVE, 0, 0);

        status = NtCreateFile(
            &file,
            SYNCHRONIZE | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES,
            &attribs,
            &ioStatus,
            0,
            FILE_ATTRIBUTE_NORMAL,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            1,
            FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE,
            NULL,
            0
        );
        if (!NT_SUCCESS(status))
        {
            printf("Error, NtCreateFile(%S) failed, code %X\n", SourceFile, status);
            break;
        }

        if (!RtlDosPathNameToNtPathName_U(DestinationFile, &destPath, NULL, NULL))
        {
            printf("Error, can't convert DOS path '%S' to NT path\n", DestinationFile);
            break;
        }

        sourceSize = destPath.Length;
        buffSize = sizeof(FILE_LINK_INFORMATION) + sourceSize + 2;

        buffer = (char*)malloc(buffSize);
        info = (PFILE_LINK_INFORMATION)buffer;

        memcpy(info->FileName, destPath.Buffer, sourceSize);
        info->FileNameLength = sourceSize;
        info->ReplaceIfExists = FALSE;
        info->RootDirectory = NULL;

        status = NtSetInformationFile(file, &ioStatus, info, buffSize, FileLinkInformation);
        if (!NT_SUCCESS(status))
        {
            printf("Error, can't create hard link to file '%S', code %X\n", DestinationFile, status);
            break;
        }

        result = true;
    }
    while (false);

    if (result)
    {
        fileHL = CreateFileW(
            DestinationFile,
            SYNCHRONIZE | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES | DELETE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            NULL, OPEN_EXISTING, FILE_FLAG_DELETE_ON_CLOSE, NULL
        );

        if (fileHL != INVALID_HANDLE_VALUE)
            fileHL = CreateFileW(
                DestinationFile,
                SYNCHRONIZE | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                NULL, OPEN_EXISTING, 0, NULL
            );
    }

    if (file != INVALID_HANDLE_VALUE)
    {
        NtClose(file);
        file = INVALID_HANDLE_VALUE;
    }
    
    if (srcPath.Buffer)
        RtlFreeHeap(GetProcessHeap(), 0, srcPath.Buffer);

    if (destPath.Buffer)
        RtlFreeHeap(GetProcessHeap(), 0, destPath.Buffer);

    if (buffer)
        free(buffer);

    return fileHL;
}

bool IsDirExists(const wchar_t* DirPath)
{
    DWORD attribs;

    attribs = GetFileAttributesW(DirPath);
    if (attribs == INVALID_FILE_ATTRIBUTES)
        return false;

    if (attribs & FILE_ATTRIBUTE_DIRECTORY)
        return true;

    return false;
}

bool CreateDirRecursively(wchar_t* DirPath, size_t DirPathLen)
{
    bool result = true;
    size_t i;

    if (IsDirExists(DirPath))
        return true;

    if (CreateDirectoryW(DirPath, NULL))
        return true;

    if (GetLastError() != ERROR_PATH_NOT_FOUND)
    {
        printf("Error, can't create directory '%S', code %d\n", DirPath, GetLastError());
        return false;
    }

    // Attempt to create dir recursively

    for (i = DirPathLen - 1; i >= 2; i--)
    {
        wchar_t chr = DirPath[i];
        if (chr == L'\\' || chr == L'/')
        {
            DirPath[i] = L'\0';

            if (!CreateDirRecursively(DirPath, i))
                result = false;

            DirPath[i] = chr;
            break;
        }
    }

    if (!CreateDirectoryW(DirPath, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
    {
        printf("Error, can't create directory '%S', code %d\n", DirPath, GetLastError());
        result = false;
    }

    return result;
}

bool CreateDir(wchar_t* DirPath)
{
    size_t dirPathLen = wcslen(DirPath);
    return CreateDirRecursively(DirPath, dirPathLen);
}

// =============================================

bool CreateTemporaryBackup(const wchar_t* SourceFile)
{
    FileContext fileContext;
    wchar_t tempFile[MAX_PATH + 1];
    wchar_t* fullSourcePath = 0;
    size_t fullStrSize;
    bool result = false;

    if (GetTempFileNameW(g_MonitorContext.DestTempDir, L"backup_", 0, tempFile) == 0)
    {
        printf("Error, can't generate temporary file name, code %d\n", GetLastError());
        return false;
    }

    memset(&fileContext, 0, sizeof(fileContext));

    do
    {
        DeleteFileW(tempFile); // OMG!

        fullSourcePath = AllocateString(g_MonitorContext.SourceDir, SourceFile);
        if (!fullSourcePath)
        {
            printf("Error, can't prepare source file name\n");
            break;
        }

        fileContext.TempFile = CreateHardLinkInternal(tempFile, fullSourcePath);
        if (fileContext.TempFile == INVALID_HANDLE_VALUE)
            break;

        fileContext.TempFileName = AllocateString(tempFile);
        if (!fileContext.TempFileName)
        {
            printf("Error, can't allocate temp file name\n");
            break;
        }

        fileContext.Key = AllocateString(SourceFile, NULL, true);
        if (!fileContext.Key)
        {
            printf("Error, can't allocate key string\n");
            break;
        }

        fileContext.BackupFileName = AllocateString(SourceFile);
        if (!fileContext.BackupFileName)
        {
            printf("Error, can't allocate key string\n");
            break;
        }

        if (!InsertAVLElement(&g_MonitorContext.FilesContext, &fileContext, sizeof(fileContext)))
        {
            printf("Error, can't save file cache\n");
            break;
        }

        result = true;
    } 
    while (false);

    if (!result)
    {
        if (fileContext.TempFile != INVALID_HANDLE_VALUE)
            CloseHandle(fileContext.TempFile);

        if (fileContext.BackupFileName)
            ReleaseString(fileContext.BackupFileName);

        if (fileContext.TempFileName)
            ReleaseString(fileContext.TempFileName);

        if (fileContext.Key)
            ReleaseString(fileContext.Key);
    }

    return result;
}

bool RestoreBackupFromTemp(FileContext* FileContext, wchar_t* RestoredFilePath)
{
    size_t i = wcslen(RestoredFilePath);
    bool isDirReady = false;
    HANDLE restored;

    if (i == 0)
        return false;

    do
    {
        i--;

        if (RestoredFilePath[i] == L'\\' || RestoredFilePath[i] == L'/')
        {
            wchar_t chr = RestoredFilePath[i];
            RestoredFilePath[i] = L'\0';
            isDirReady = CreateDir(RestoredFilePath);
            RestoredFilePath[i] = chr;
            break;
        }
    }
    while (i > 0);

    if (!isDirReady)
        return false;

    /*if (!CopyFileW(FileContext->TempFileName, RestoredFilePath, TRUE))
    {
        //TODO: retry create file with different name
        printf("Error, can't restore file '%S', code %d\n", RestoredFilePath, GetLastError());
        return false;
    }*/

    restored = CreateHardLinkInternal(RestoredFilePath, FileContext->TempFileName);
    if (restored == INVALID_HANDLE_VALUE)
        return false;

    CloseHandle(restored);

    return true;
}

bool UpgradeBackupToConstant(const wchar_t* SourceFile)
{
    FileContext lookFileContext;
    FileContext* fileContext;
    wchar_t* restoredFilePath = 0;
    bool result = false;
    bool found = false;

    memset(&lookFileContext, 0, sizeof(lookFileContext));

    lookFileContext.Key = AllocateString(SourceFile, NULL, true);
    if (!lookFileContext.Key)
    {
        printf("Error, can't allocate file key\n");
        goto ReleaseBlock;
    }

    fileContext = (FileContext*)FindAVLElement(&g_MonitorContext.FilesContext, &lookFileContext);
    if (!fileContext)
    {
        printf("Error, can't find a context for the following file '%S'\n", SourceFile);
        goto ReleaseBlock;
    }

    restoredFilePath = AllocateString(g_MonitorContext.DestBackupDir, SourceFile);
    if (!restoredFilePath)
    {
        printf("Error, can't allocate restored path\n");
        goto ReleaseBlock;
    }

    found = true;

    if (!RestoreBackupFromTemp(fileContext, restoredFilePath))
        goto ReleaseBlock;

    result = true;

ReleaseBlock:

    if (found)
        RemoveAVLElement(&g_MonitorContext.FilesContext, &lookFileContext);

    if (lookFileContext.Key)
        ReleaseString(lookFileContext.Key);

    if (restoredFilePath)
        ReleaseString(restoredFilePath);

    return result;
}

// =============================================

void ReleaseBackupMonitorContext()
{
    if (g_MonitorContext.SourceDirHandle && g_MonitorContext.SourceDirHandle != INVALID_HANDLE_VALUE)
        CloseHandle(g_MonitorContext.SourceDirHandle);

    if (g_MonitorContext.SourceDir)
        ReleaseString(g_MonitorContext.SourceDir);

    if (g_MonitorContext.DestTempDir)
        ReleaseString(g_MonitorContext.DestTempDir);

    if (g_MonitorContext.DestBackupDir)
        ReleaseString(g_MonitorContext.DestBackupDir);

    DestroyAVLTree(&g_MonitorContext.FilesContext);
}

bool InitBackupMonitorContext(const wchar_t* SourceDir, wchar_t* BackupDir)
{
    bool result = false;

    memset(&g_MonitorContext, 0, sizeof(g_MonitorContext));

    InitializeAVLTree(&g_MonitorContext.FilesContext, AVLTreeAllocate, AVLTreeFree, AVLTreeCompare);

    if (!CreateDir(BackupDir))
        return false;

    do
    {
        g_MonitorContext.SourceDirHandle =
            CreateFileW(
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
            break;
        }

        g_MonitorContext.SourceDir = AllocateString(SourceDir, L"\\");
        if (!g_MonitorContext.SourceDir)
        {
            printf("Error, can't prepare source directory path\n");
            break;
        }

        g_MonitorContext.DestTempDir = AllocateString(BackupDir, L"\\temp");
        if (!g_MonitorContext.DestTempDir)
        {
            printf("Error, can't prepare temporary directory path\n");
            break;
        }

        if (!CreateDirectoryW(g_MonitorContext.DestTempDir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
        {
            printf("Error, can't create temporary directory '%S', code %d\n", g_MonitorContext.DestTempDir, GetLastError());
            break;
        }

        g_MonitorContext.DestBackupDir = AllocateString(BackupDir, L"\\backup\\");
        if (!g_MonitorContext.DestBackupDir)
        {
            printf("Error, can't prepare backup directory path\n");
            break;
        }

        if (!CreateDirectoryW(g_MonitorContext.DestBackupDir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
        {
            printf("Error, can't create backup directory '%S', code %d\n", g_MonitorContext.DestBackupDir, GetLastError());
            break;
        }

        result = true;
    }
    while (false);

    if (!result)
        ReleaseBackupMonitorContext();
    
    return result;
}

bool StartBackupMonitor(wchar_t* SourceDir, wchar_t* BackupDir)
{
    if (!SetPrivileges("SeCreateSymbolicLinkPrivilege", TRUE))
        return false;

    if (!InitBackupMonitorContext(SourceDir, BackupDir))
        return false;

    enum { ChangeInformationBlockSize = 0x1000 };
    PFILE_NOTIFY_INFORMATION changeInfo = (PFILE_NOTIFY_INFORMATION)VirtualAlloc(NULL, ChangeInformationBlockSize, MEM_COMMIT, PAGE_READWRITE);
    DWORD returned;

    if (!changeInfo)
    {
        printf("Error, can't allocate change information block, code: %d\n", GetLastError());
        ReleaseBackupMonitorContext();
        return false;
    }

    while (true)
    {
        PFILE_NOTIFY_INFORMATION currentInfo = changeInfo;

        if (!ReadDirectoryChangesW(
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
            case FILE_ACTION_MODIFIED:
                printf("FILE_ACTION_MODIFIED ");
                break;
            default:
                printf("UNKNOWN ");
                break;
            }

            currentInfo->FileName[currentInfo->FileNameLength / sizeof(WCHAR)] = L'\0';
            printf("%S\n", currentInfo->FileName);

            if (currentInfo->FileNameLength + sizeof(FILE_NOTIFY_INFORMATION)+sizeof(WCHAR) > ChangeInformationBlockSize)
                break;

            currentInfo->FileName[currentInfo->FileNameLength / sizeof(WCHAR)] = L'\0';

            if (currentInfo->Action == FILE_ACTION_ADDED || currentInfo->Action == FILE_ACTION_RENAMED_NEW_NAME)
            {
                CreateTemporaryBackup(currentInfo->FileName);
            }
            else if (currentInfo->Action == FILE_ACTION_REMOVED || currentInfo->Action == FILE_ACTION_RENAMED_OLD_NAME)
            {
                UpgradeBackupToConstant(currentInfo->FileName);
            }

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

    /*AVL_TREE tree;
    int value;
    void* ptr;

    InitializeAVLTree(&tree, AVL_ALLOCATE_CALLBACK2, AVL_FREE_CALLBACK2, AVL_COMPARE_CALLBACK2);

    value = 5;
    ptr = InsertAVLElement(&tree, &value, sizeof(value));

    value = 6;
    ptr = InsertAVLElement(&tree, &value, sizeof(value));
    value = 7;
    ptr = InsertAVLElement(&tree, &value, sizeof(value));
    ptr = InsertAVLElement(&tree, &value, sizeof(value));
    value = 8;
    ptr = InsertAVLElement(&tree, &value, sizeof(value));
    value = 1;
    ptr = InsertAVLElement(&tree, &value, sizeof(value));
    value = 2;
    ptr = InsertAVLElement(&tree, &value, sizeof(value));
    value = 3;
    ptr = InsertAVLElement(&tree, &value, sizeof(value));
    value = 4;
    ptr = InsertAVLElement(&tree, &value, sizeof(value));

    bool res;

    value = 3;
    res = RemoveAVLElement(&tree, &value);
    res = RemoveAVLElement(&tree, &value);

    value = 4;
    int* a = (int*)FindAVLElement(&tree, &value);

    value = 41;
    a = (int*)FindAVLElement(&tree, &value);

    DestroyAVLTree(&tree);*/

    return 0;
}
