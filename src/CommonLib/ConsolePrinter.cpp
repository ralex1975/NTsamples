#include "ConsolePrinter.h"
#include "BufferQueue.h"
#include "CommonLib.h"
#include <Windows.h>
#include <stdarg.h>
#include <wchar.h>

enum PrinterType
{
    SynchronizedPrinter,
    AsynchronizedPrinter,
    MaxPrintrerType
};

struct ConsoleContext
{
    PrinterType type;
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
    PrintColors color;
    wchar_t message[512];
};

static SpinAtom s_DefaultContextLock = 0;
static volatile ConsoleContext* s_DefaultContext = NULL;

static _declspec(thread) ConsoleContext* st_AssignedContext = NULL;

static bool InitConsoleContext(ConsoleContext* Context, PrinterType Type, PrintColors DefaultColor)
{
    CONSOLE_SCREEN_BUFFER_INFO info;

    Context->type = Type;
    if (Context->type >= PrinterType::MaxPrintrerType)
        return false;

    Context->output = ::GetStdHandle(STD_OUTPUT_HANDLE);
    if (Context->output == INVALID_HANDLE_VALUE)
        return false;

    if (!::GetConsoleScreenBufferInfo(Context->output, &info))
        return false;

    Context->defaultColor = (DefaultColor == PrintColors::Default ? info.wAttributes : DefaultColor);
    Context->currentColor = info.wAttributes;

    return true;
}

static WORD ConvertColorToAttribs(ConsoleContext* Context, PrintColors Color)
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

static void SetConsoleColor(ConsoleContext* Context, PrintColors Color)
{
    SetConsoleTextAttribute(Context->output, ConvertColorToAttribs(Context, Color));
}

static void SetConsoleAttribs(ConsoleContext* Context, WORD Attribs)
{
    SetConsoleTextAttribute(Context->output, Attribs);
}

static void PrintFromBufferToConsole(void* Data, size_t DataSize, void* Parameter)
{
    AsyncConsoleContext* context = (AsyncConsoleContext*)Parameter;
    MessageBlock* block = (MessageBlock*)Data;

    SetConsoleColor(&context->console, block->color);

    wprintf(block->message);

    SetConsoleAttribs(&context->console, context->console.defaultColor);
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

        PopAllDataFromBufferQueue(context->bufferedQueue, PrintFromBufferToConsole, context);

        if (context->terminating)
            break;
    }

    ::SetEvent(context->startStopEvent);

    return 0;
}

void SetDefaultConsoleContext(ConsoleContext* Context)
{
    s_DefaultContext = Context;
}

ConsoleContext* GetDefaultConsoleContext()
{
    return (ConsoleContext*)s_DefaultContext;
}

ConsoleInstance CreateAsyncConsolePrinterContext(PrintColors DefaultColor, bool UseAsDefault)
{
    bool result = false;
    AsyncConsoleContext* context = NULL;

    context = (AsyncConsoleContext*)malloc(sizeof(AsyncConsoleContext));
    if (!context)
        goto ReleaseBlock;

    if (!InitConsoleContext(&context->console, PrinterType::AsynchronizedPrinter, DefaultColor))
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

    if (UseAsDefault)
        SetDefaultConsoleContext((ConsoleContext*)context);

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

void DestroyAsyncConsolePrinterContext(ConsoleInstance Context)
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

void AssociateThreadWithConsolePrinterContext(ConsoleInstance Context)
{
    st_AssignedContext = (ConsoleContext*)Context;
}

ConsoleContext* GetCurrentConsoleContext()
{
    ConsoleContext* context = st_AssignedContext;

    if (!context)
    {
        context = GetDefaultConsoleContext();
        if (!context)
            return NULL;

        st_AssignedContext = context;
    }

    return context;
}

void PrintMsg(PrintColors Color, const wchar_t* Format ...)
{
    ConsoleContext* context = GetCurrentConsoleContext();
    MessageBlock block;
    int len;

    if (!context)
        return;

    if (context->type >= PrinterType::MaxPrintrerType)
        return;

    va_list args;
    va_start(args, Format);
    len = vswprintf_s(block.message, Format, args);
    va_end(args);

    if (len < 0)
        return;

    block.color = Color;

    if (context->type == PrinterType::SynchronizedPrinter)
    {
        //TODO:
    }
    else
    {
        AsyncConsoleContext* async = (AsyncConsoleContext*)context;
        if (PushDataToBufferQueue(async->bufferedQueue, &block, FIELD_OFFSET(MessageBlock, message) + (len + 1) * sizeof(wchar_t)))
            ::SetEvent(async->stopDispatcherEvent);
    }
}

void PrintMsgEx(ConsoleInstance Context, PrintColors Color, const wchar_t* Format ...)
{
    ConsoleContext* context = (ConsoleContext*)Context;
    MessageBlock block;
    int len;

    if (context->type >= PrinterType::MaxPrintrerType)
        return;

    va_list args;
    va_start(args, Format);
    len = vswprintf_s(block.message, Format, args);
    va_end(args);

    if (len < 0)
        return;

    block.color = Color;

    if (context->type == PrinterType::SynchronizedPrinter)
    {
        //TODO:
    }
    else
    {
        AsyncConsoleContext* async = (AsyncConsoleContext*)context;
        if (PushDataToBufferQueue(async->bufferedQueue, &block, FIELD_OFFSET(MessageBlock, message) + (len + 1) * sizeof(wchar_t)))
            ::SetEvent(async->stopDispatcherEvent);
    }
}
