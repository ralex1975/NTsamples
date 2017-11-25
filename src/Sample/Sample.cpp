#include <Windows.h>
#include <stdio.h>

int wmain(int argc, wchar_t* argv[])
{

    size_t i = 100000;

    DWORD tick = GetTickCount();

    while (i != 0)
    {
        char buffer[100];

        sprintf(buffer, "%d %s %S\n", i, "aaaa", L"bbbb");
        printf(buffer);

        i--;
    }

    printf("Sample app %d\n", GetTickCount() - tick);

    return 0;
}
