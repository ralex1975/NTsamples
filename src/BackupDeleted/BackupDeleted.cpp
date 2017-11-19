#include <Windows.h>
#include <stdlib.h>
#include <stdio.h>
#include <tchar.h>
#include "ntdef.h"
#include "AVLTree.h"

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
{//TODO: is there any profit when NT variant is used?
    UNICODE_STRING destPath = {}, srcPath = {};
    OBJECT_ATTRIBUTES attribs = {};
    IO_STATUS_BLOCK ioStatus;
    PFILE_LINK_INFORMATION info;
    NTSTATUS status;
    HANDLE file = INVALID_HANDLE_VALUE;
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

        //TODO: it's a good idea to make a research how sharing violations works
        status = NtCreateFile(
            &file,
            GENERIC_READ | /*DELETE |*/ SYNCHRONIZE | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES,
            &attribs,
            &ioStatus,
            0,
            FILE_ATTRIBUTE_NORMAL,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, // ???
            1,
            FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE | FILE_TRANSACTED_MODE,
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

    if (result)
        file = CreateFileW(
            DestinationFile,
            GENERIC_READ | DELETE, 
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            NULL,
            OPEN_ALWAYS,
            FILE_FLAG_DELETE_ON_CLOSE, 
            NULL
        );

    return file;
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
        {
            printf("oops can't create backup, retrying\n");
            Sleep(50); //TODO: just a trick, will be replaced to delayed queue
            fileContext.TempFile = CreateHardLinkInternal(tempFile, fullSourcePath);
            if (fileContext.TempFile == INVALID_HANDLE_VALUE)
                break;
        }

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

    if (!CopyFileW(FileContext->TempFileName, RestoredFilePath, TRUE))
    {
        //TODO: retry create file with different name
        printf("Error, can't restore file '%S', code %d\n", RestoredFilePath, GetLastError());
        return false;
    }

    return true;
}

bool UpgradeBackupToConstant(const wchar_t* SourceFile)
{
    FileContext lookFileContext;
    FileContext* fileContext;
    wchar_t* restoredFilePath = 0;
    bool result = false;

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

    if (!RestoreBackupFromTemp(fileContext, restoredFilePath))
        goto ReleaseBlock;

    if (!RemoveAVLElement(&g_MonitorContext.FilesContext, &lookFileContext))
    {
        printf("Error, can't find a context for the following file '%S'\n", SourceFile);
        goto ReleaseBlock;
    }

    result = true;

ReleaseBlock:

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

        switch (changeInfo->Action)
        {
        case FILE_ACTION_ADDED:
        case FILE_ACTION_RENAMED_NEW_NAME:
            printf("create file ");
            break;
        case FILE_ACTION_REMOVED:
        case FILE_ACTION_RENAMED_OLD_NAME: //TODO: check what is a new name using file context
            printf("delete file ");
            break;
        }

        changeInfo->FileName[changeInfo->FileNameLength / sizeof(WCHAR)] = L'\0';
        printf("%S\n", changeInfo->FileName);

        if (changeInfo->FileNameLength + sizeof(FILE_NOTIFY_INFORMATION) + sizeof(WCHAR) > ChangeInformationBlockSize)
            continue;

        changeInfo->FileName[changeInfo->FileNameLength / sizeof(WCHAR)] = L'\0';

        if (changeInfo->Action == FILE_ACTION_ADDED || changeInfo->Action == FILE_ACTION_RENAMED_NEW_NAME)
        {
            CreateTemporaryBackup(changeInfo->FileName);
        }
        else if (changeInfo->Action == FILE_ACTION_REMOVED || changeInfo->Action == FILE_ACTION_RENAMED_OLD_NAME)
        {
            UpgradeBackupToConstant(changeInfo->FileName);
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
