#include "ConsolePrinter.h"
#include "BufferQueue.h"
#include <Windows.h>
#include <stdarg.h>
#include <wchar.h>

struct ConsoleContext
{
    WORD currentColor;
    WORD defaultColor;
    HANDLE output;
};

struct AsyncConsoleContext
{
    ConsoleContext console;
    HANDLE startStopEvent;
    HANDLE stopDispatcherEvent;
    HANDLE dispatcher;
    void*  bufferedQueue;
    bool   terminating;
};

struct MessageBlock
{
    PrintColorsEnum color;
    wchar_t message[256];
};

static _declspec(thread) AsyncConsoleContext* st_AssignedContext = NULL;

static bool InitConsoleContext(ConsoleContext* Context)
{
    CONSOLE_SCREEN_BUFFER_INFO info;

    Context->output = ::GetStdHandle(STD_OUTPUT_HANDLE);
    if (Context->output == INVALID_HANDLE_VALUE)
        return false;

    if (!::GetConsoleScreenBufferInfo(Context->output, &info))
        return false;

    Context->defaultColor = info.wAttributes;
    Context->currentColor = info.wAttributes;

    return true;
}

static WORD ConvertColorToAttribs(ConsoleContext* Context, PrintColorsEnum Color)
{
    WORD attribs;

    switch (Color)
    {
    case Black:
        attribs = 0;
        break;
    case DarkBlue:
        attribs = FOREGROUND_BLUE;
        break;
    case DarkGreen:
        attribs = FOREGROUND_GREEN;
        break;
    case DarkCyan:
        attribs = FOREGROUND_GREEN | FOREGROUND_BLUE;
        break;
    case DarkRed:
        attribs = FOREGROUND_RED;
        break;
    case DarkMagenta:
        attribs = FOREGROUND_RED | FOREGROUND_BLUE;
        break;
    case DarkYellow:
        attribs = FOREGROUND_RED | FOREGROUND_GREEN;
        break;
    case DarkGrey:
        attribs = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
        break;
    case Gray:
        attribs = FOREGROUND_INTENSITY;
        break;
    case Blue:
        attribs = FOREGROUND_INTENSITY | FOREGROUND_BLUE;
        break;
    case Green:
        attribs = FOREGROUND_INTENSITY | FOREGROUND_GREEN;
        break;
    case Cyan:
        attribs = FOREGROUND_INTENSITY | FOREGROUND_GREEN | FOREGROUND_BLUE;
        break;
    case Red:
        attribs = FOREGROUND_INTENSITY | FOREGROUND_RED;
        break;
    case Magenta:
        attribs = FOREGROUND_INTENSITY | FOREGROUND_RED | FOREGROUND_BLUE;
        break;
    case Yellow:
        attribs = FOREGROUND_INTENSITY | FOREGROUND_RED | FOREGROUND_GREEN;
        break;
    case White:
        attribs = FOREGROUND_INTENSITY | FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
        break;
    default:
        attribs = Context->defaultColor;
    }

    return attribs;
}

static void SetConsoleColor(ConsoleContext* Context, PrintColorsEnum Color)
{
    SetConsoleTextAttribute(Context->output, ConvertColorToAttribs(Context, Color));
}

static void PrintFromBufferToConsole(void* Data, size_t DataSize, void* Parameter)
{
    AsyncConsoleContext* context = (AsyncConsoleContext*)Parameter;
    MessageBlock* block = (MessageBlock*)Data;

    SetConsoleColor(&context->console, block->color);

    wprintf(block->message);

    SetConsoleColor(&context->console, block->color);
}

static DWORD WINAPI AsyncConsoleDispatcher(LPVOID Parameter)
{
    AsyncConsoleContext* context = (AsyncConsoleContext*)Parameter;

    ::SetEvent(context->startStopEvent);

    while (true)
    {
        DWORD error = ::WaitForMultipleObjects(1, &context->stopDispatcherEvent, FALSE, INFINITE);
        if (error != WAIT_OBJECT_0)
            break;

        if (context->terminating)
            break;

        PopAllDataFromBufferQueue(context->bufferedQueue, PrintFromBufferToConsole, context);
    }

    ::SetEvent(context->startStopEvent);

    return 0;
}

void* CreateAsyncConsolePrinterContext()
{
    bool result = false;
    AsyncConsoleContext* context = NULL;

    context = (AsyncConsoleContext*)malloc(sizeof(AsyncConsoleContext));
    if (!context)
        goto ReleaseBlock;

    if (!InitConsoleContext(&context->console))
        goto ReleaseBlock;

    context->startStopEvent = ::CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!context->startStopEvent)
        goto ReleaseBlock;

    context->stopDispatcherEvent = ::CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!context->stopDispatcherEvent)
        goto ReleaseBlock;

    context->dispatcher = ::CreateThread(NULL, 0, AsyncConsoleDispatcher, context, 0, NULL);
    if (!context->dispatcher)
        goto ReleaseBlock;

    if (::WaitForSingleObject(context->startStopEvent, INFINITE) != WAIT_OBJECT_0)
        goto ReleaseBlock;

    context->bufferedQueue = CreateBufferQueue();
    if (!context->bufferedQueue)
        goto ReleaseBlock;

    context->terminating = false;

    result = true;

ReleaseBlock:

    if (!result && context)
    {
        if (context->startStopEvent)
            ::CloseHandle(context->startStopEvent);

        if (context->stopDispatcherEvent)
            ::CloseHandle(context->stopDispatcherEvent);

        if (context->dispatcher)
        {
            ::TerminateThread(context->dispatcher, 0x0BADBAD0);
            ::CloseHandle(context->dispatcher);
        }

        if (context->bufferedQueue)
            DestroyBufferQueue(context->bufferedQueue);
    }

    return context;
}

void DestroyAsyncConsolePrinterContext(void* Context)
{
    AsyncConsoleContext* context = (AsyncConsoleContext*)Context;
    DWORD error;

    context->terminating = true;

    ::SetEvent(context->stopDispatcherEvent);

    error = ::WaitForSingleObject(context->startStopEvent, INFINITE);
    if (error != WAIT_OBJECT_0)
        ::TerminateThread(context->dispatcher, 0xC0FEC0FE);

    ::CloseHandle(context->dispatcher);
    ::CloseHandle(context->startStopEvent);
    ::CloseHandle(context->stopDispatcherEvent);

    DestroyBufferQueue(context->bufferedQueue);

    free(context);
}


void AssociateThreadWithConsolePrinterContext(void* Context)
{
    st_AssignedContext = (AsyncConsoleContext*)Context;
}


void PrintMsg(PrintColorsEnum Color, const wchar_t* Format ...)
{
    MessageBlock block;
    int len;

    if (!st_AssignedContext)
        return;

    va_list args;
    va_start(args, Format);
    len = vswprintf_s(block.message, Format, args);
    va_end(args);

    if (len < 0)
        return;

    block.color = Color;

    if (PushDataToBufferQueue(st_AssignedContext->bufferedQueue, &block, FIELD_OFFSET(MessageBlock, message) + (len + 1) * sizeof(wchar_t)))
        ::SetEvent(st_AssignedContext->stopDispatcherEvent);
}

void PrintMsgEx(void* Context, PrintColorsEnum Color, const wchar_t* Format ...)
{
    AsyncConsoleContext* context = (AsyncConsoleContext*)Context;
    MessageBlock block;
    int len;

    va_list args;
    va_start(args, Format);
    len = vswprintf_s(block.message, Format, args);
    va_end(args);

    if (len < 0)
        return;

    block.color = Color;

    if (PushDataToBufferQueue(context->bufferedQueue, &block, FIELD_OFFSET(MessageBlock, message) + (len + 1) * sizeof(wchar_t)))
        ::SetEvent(context->stopDispatcherEvent);
}
