#pragma once

enum PrintColorsEnum
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

void* CreateAsyncConsolePrinterContext();
void  DestroyAsyncConsolePrinterContext(void* context);

void AssociateThreadWithConsolePrinterContext(void* context);

void PrintMsg(PrintColorsEnum color, const wchar_t* format ...);
void PrintMsgEx(void* context, PrintColorsEnum color, const wchar_t* format ...);
