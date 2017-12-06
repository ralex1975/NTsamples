#include "ConsolePrinter.h"
#include <Windows.h>

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
};

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

static DWORD WINAPI AsyncConsoleDispatcher(LPVOID lpParameter)
{
    AsyncConsoleContext* context = (AsyncConsoleContext*)lpParameter;

    ::SetEvent(context->startStopEvent);

    while (true)
    {
        DWORD error = ::WaitForMultipleObjects(1, &context->stopDispatcherEvent, FALSE, INFINITE);
        if (error == WAIT_OBJECT_0)
            break;

        //TODO: console buffer processing
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
    }

    return context;
}

void DestroyAsyncConsolePrinterContext(void* Context)
{
    AsyncConsoleContext* context = (AsyncConsoleContext*)Context;
    DWORD error;

    ::SetEvent(context->stopDispatcherEvent);

    error = ::WaitForSingleObject(context->startStopEvent, INFINITE);
    if (error != WAIT_OBJECT_0)
        ::TerminateThread(context->dispatcher, 0xC0FEC0FE);

    ::CloseHandle(context->dispatcher);
    ::CloseHandle(context->startStopEvent);
    ::CloseHandle(context->stopDispatcherEvent);

    free(context);
}

void AssociateThreadWithConsolePrinterContext(void* Context)
{
    AsyncConsoleContext* context = (AsyncConsoleContext*)Context;
}

static _declspec(thread) AsyncConsoleContext* st_AssignedContext = NULL;

void PrintMsg(PrintColorsEnum Color, const wchar_t* Format ...)
{

}

void PrintMsgEx(void* Context, PrintColorsEnum Color, const wchar_t* Format ...)
{
    AsyncConsoleContext* context = (AsyncConsoleContext*)Context;

}
