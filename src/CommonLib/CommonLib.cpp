#include "CommonLib.h"
#include <ntdef.h>
#include <stdarg.h>

// =============================================

bool SetTokenPrivilege(const char* Privilege, bool Enable)
{
    bool result = false;
    HANDLE token = NULL;
    TOKEN_PRIVILEGES tp;
    LUID luid;
    
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES, &token))
        goto ReleaseBlock;
    
    if (!::LookupPrivilegeValueA(NULL, Privilege, &luid))
        goto ReleaseBlock;

    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = (Enable ? SE_PRIVILEGE_ENABLED : 0);

    if (!::AdjustTokenPrivileges(token, FALSE, &tp, sizeof(TOKEN_PRIVILEGES), NULL, NULL))
        goto ReleaseBlock;

    result = true;

ReleaseBlock:

    if (token)
        ::CloseHandle(token);

    return result;
}

// =============================================

bool IsDirExists(const wchar_t* DirPath)
{
    DWORD attribs;

    attribs = ::GetFileAttributesW(DirPath);
    if (attribs == INVALID_FILE_ATTRIBUTES)
        return false;

    return ((attribs & FILE_ATTRIBUTE_DIRECTORY) != 0);
}

bool IsFileExists(const wchar_t* FilePath)
{
    DWORD attribs;

    attribs = ::GetFileAttributesW(FilePath);
    if (attribs == INVALID_FILE_ATTRIBUTES)
        return false;

    return ((attribs & FILE_ATTRIBUTE_DIRECTORY) == 0);
}

static bool CreateDirRecursively(wchar_t* DirPath, size_t DirPathLen)
{
    bool result = true;
    size_t i;

    if (IsDirExists(DirPath))
        return true;

    if (::CreateDirectoryW(DirPath, NULL))
        return true;

    if (::GetLastError() != ERROR_PATH_NOT_FOUND)
        return false;

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

    if (!::CreateDirectoryW(DirPath, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
        result = false;

    return result;
}

bool CreateDirectoryByFullPath(wchar_t* DirPath)
{
    size_t dirPathLen = wcslen(DirPath);
    return CreateDirRecursively(DirPath, dirPathLen);
}

bool CreateHardLinkToExistingFile(const wchar_t* SourceFile, const wchar_t* DestinationFile)
{
    UNICODE_STRING srcPath;
    IO_STATUS_BLOCK ioStatus;
    PFILE_LINK_INFORMATION info;
    NTSTATUS status;
    HANDLE file = INVALID_HANDLE_VALUE;
    char* buffer = NULL;
    size_t bufferSize;
    bool result = false;

    // Create a new empty file

    file = ::CreateFileW(
        DestinationFile,
        SYNCHRONIZE | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 
        NULL, OPEN_EXISTING, 0, NULL
    );

    // Link a new file with a file that we want to use as the source

    if (!::RtlDosPathNameToNtPathName_U(SourceFile, &srcPath, NULL, NULL))
        goto ReleaseBlock;

    bufferSize = sizeof(FILE_LINK_INFORMATION) + srcPath.Length + sizeof(wchar_t);
    buffer = (char*)::RtlAllocateHeap(::GetProcessHeap(), 0, bufferSize);
    if (!buffer)
        goto ReleaseBlock;

    info = (PFILE_LINK_INFORMATION)buffer;

    memcpy(info->FileName, srcPath.Buffer, srcPath.Length);
    info->FileNameLength = srcPath.Length;
    info->ReplaceIfExists = TRUE;
    info->RootDirectory = NULL;

    status = ::NtSetInformationFile(file, &ioStatus, info, bufferSize, FileLinkInformation);
    if (!NT_SUCCESS(status))
        goto ReleaseBlock;

    result = true;

ReleaseBlock:

    if (file != INVALID_HANDLE_VALUE)
        ::CloseHandle(file);

    if (buffer)
        ::RtlFreeHeap(::GetProcessHeap(), 0, buffer);

    return result;
}

// =============================================

wchar_t* BuildWideString(const wchar_t* String ...)
{
    const wchar_t* stringArg;
    wchar_t* buffer = 0;
    size_t lengthCache[10];
    size_t firstStringLength = wcslen(String);
    size_t fullStringLenght = firstStringLength;
    size_t i;

    {
        va_list args;
        va_start(args, String);

        stringArg = va_arg(args, const wchar_t*);
        for (i = 0; stringArg; i++)
        {
            if (i < _countof(lengthCache))
            {
                size_t length = wcslen(stringArg);
                lengthCache[i] = length;
                fullStringLenght += length;
            }
            else
            {
                fullStringLenght += wcslen(stringArg);
            }

            stringArg = va_arg(args, const wchar_t*);
        }

        va_end(args);
    }

    buffer = (wchar_t*)::RtlAllocateHeap(::GetProcessHeap(), 0, (fullStringLenght * sizeof(wchar_t)) + sizeof(wchar_t));
    if (!buffer)
        return 0;

    wcscpy_s(buffer, fullStringLenght + 1, String);

    {
        size_t offset = firstStringLength;
        va_list args;
        va_start(args, String);

        stringArg = va_arg(args, const wchar_t*);
        for (i = 0; stringArg; i++)
        {
            size_t length;

            if (i < _countof(lengthCache))
                length = lengthCache[i];
            else
                length = wcslen(stringArg);
            
            wcscpy_s(buffer + offset, fullStringLenght - offset + 1, stringArg);
            offset += length;

            stringArg = va_arg(args, const wchar_t*);
        }

        va_end(args);
    }

    return buffer;
}

void FreeWideString(const wchar_t* String)
{
    ::RtlFreeHeap(::GetProcessHeap(), 0, (void*)String);
}

// =============================================

uintptr_t AlignToTop(uintptr_t What, uintptr_t Align)
{
    uintptr_t diff = (What % Align);
    if (diff)
        What += Align - diff;
    return What;
}

uintptr_t AlignToBottom(uintptr_t What, uintptr_t Align)
{
    return What - (What % Align);
}
