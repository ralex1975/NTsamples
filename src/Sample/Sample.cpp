#include <Windows.h>
#include <stdio.h>
#include <CommonLib.h>
#include <ConsolePrinter.h>

typedef struct _OperationContext {
    OVERLAPPED overlapped;
    PFILE_NOTIFY_INFORMATION changeInfo;
    HANDLE thread;
    HANDLE iocp;
    HANDLE directory;
    DWORD index;
} OperationContext, *POperationContext;

DWORD WINAPI WorkerThread(LPVOID lpParameter)
{
    POperationContext context = (POperationContext)lpParameter;
    DWORD returned;

    while (true)
    {
        LPOVERLAPPED overlapped;
        ULONG_PTR key;

        if (!GetQueuedCompletionStatus(context->iocp, &returned, &key, &overlapped, INFINITE))
        {
            printf("Error, GetQueuedCompletionStatus() returns %d\n", GetLastError());
            continue;
        }

        printf("%d ", context->index);

        PFILE_NOTIFY_INFORMATION currentInfo = context->changeInfo;

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
            default:
                printf("UNKNOWN ");
                break;
            }

            currentInfo->FileName[currentInfo->FileNameLength / sizeof(WCHAR)] = L'\0';
            printf("%S\n", currentInfo->FileName);

            if (!currentInfo->NextEntryOffset)
                break;

            currentInfo = (PFILE_NOTIFY_INFORMATION)((char*)currentInfo + currentInfo->NextEntryOffset);
        }

        if (!::ReadDirectoryChangesW(
            context->directory,
            context->changeInfo,
            0x1000,
            TRUE,
            FILE_NOTIFY_CHANGE_FILE_NAME,
            &returned,
            &context->overlapped,
            NULL))
        {
            printf("Error, failed to read directory changes, code: %d\n", GetLastError());
            return 1;
        }

        Sleep(100);
    }
}

void StartMonitor(wchar_t* SourceDir)
{
 
    OperationContext contexts[2];
    void* buffer;
    HANDLE monitoredDirHandle;


    buffer = ::VirtualAlloc(NULL, 0x1000 * _countof(contexts), MEM_COMMIT, PAGE_READWRITE);
    if (!buffer)
    {
        printf("Error, can't allocate change information block, code: %d\n", ::GetLastError());
        return;
    }

    if (!SetTokenPrivilege("SeCreateSymbolicLinkPrivilege", TRUE))
        return;

    monitoredDirHandle =
        CreateFileW(
            SourceDir,
            FILE_LIST_DIRECTORY,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            NULL,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
            NULL
        );
    if (monitoredDirHandle == INVALID_HANDLE_VALUE)
    {
        printf("Error, CreateFileW, code: %d\n", GetLastError());
        return;
    }

    HANDLE iocpPort = CreateIoCompletionPort(monitoredDirHandle, NULL, 0, NULL);
    if (!iocpPort)
    {
        printf("Error, CreateIoCompletionPort, code: %d\n", GetLastError());
        return;
    }

    for (int i = 0; i < _countof(contexts); i++)
    {
        DWORD threadId;

        /*contexts[i].changeInfo = (PFILE_NOTIFY_INFORMATION)VirtualAlloc((char*)buffer + 0x1000 * i, 0x1000, MEM_COMMIT, PAGE_READWRITE);
        if (!contexts[i].changeInfo)
        {
            printf("Error, VirtualAlloc, code: %d\n", GetLastError());
            continue;
        }*/
        contexts[i].index = i;
        contexts[i].changeInfo = (PFILE_NOTIFY_INFORMATION)buffer;
        contexts[i].iocp = iocpPort;
        contexts[i].directory = monitoredDirHandle;

        memset(&contexts[i].overlapped, 0, sizeof(contexts[i].overlapped));

        contexts[i].thread = CreateThread(NULL, 0, WorkerThread, contexts + i, 0, &threadId);
        if (!contexts[i].thread)
        {
            printf("Error, CreateThread, code: %d\n", GetLastError());
            continue;
        }
    }

    DWORD returned;

    if (!::ReadDirectoryChangesW(
        monitoredDirHandle,
        buffer,
        0x1000,
        TRUE,
        FILE_NOTIFY_CHANGE_FILE_NAME,
        &returned,
        &contexts[0].overlapped,
        NULL))
    {
        printf("Error, failed to read directory changes, code: %d\n", GetLastError());
        return;
    }

    printf("Monitoring started\n");
    getchar();

}

int wmain(int argc, wchar_t* argv[])
{
    /*uintptr_t i = AlignToBottom(1000, 1000);
    i = AlignToBottom(0, 57);

    i = AlignToTop(1001, 1000);
    i = AlignToTop(1000, 1000);


    void* console = CreateAsyncConsolePrinterContext();

    AssociateThreadWithConsolePrinterContext(console);

    for (int i = 0; i < 10000; i++)
    {
        PrintMsg((PrintColorsEnum)(i % PrintColorsEnum::MaxColor), L"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa %s %d\n", L"bbbb", i);
    }
    printf("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n");
    printf("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n");
    printf("!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n");
    getchar();*/
    StartMonitor(L"f:\\temp\\tests");
    return 0;
}
