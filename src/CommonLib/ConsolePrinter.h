#pragma once

enum PrintColors
{
    Default,
    Black,
    DarkBlue,
    DarkGreen,
    DarkCyan,
    DarkRed,
    DarkMagenta,
    DarkYellow,
    DarkGrey,
    Gray,
    Blue,
    Green,
    Cyan,
    Red,
    Magenta,
    Yellow,
    White,
    MaxColor
};

typedef void* ConsoleInstance;

ConsoleInstance CreateAsyncConsolePrinterContext(PrintColors DefaultColor = PrintColors::Default, bool UseAsDefault = false);
void  DestroyAsyncConsolePrinterContext(ConsoleInstance Context);

void AssociateThreadWithConsolePrinterContext(ConsoleInstance Context);

void PrintMsg(PrintColors Color, const wchar_t* Format ...);
void PrintMsgEx(ConsoleInstance Context, PrintColors Color, const wchar_t* Format ...);
