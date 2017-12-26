#pragma once

void* CreateBufferQueue(size_t Size = 0x10000);
void DestroyBufferQueue(void* Context);

bool PushDataToBufferQueue(void* Context, void* Data, size_t DataSize);
bool PopDataFromBufferQueue(void* Context, void* OutputBuffer, size_t* OutputSize);

typedef void(*PopBufferQueueRoutine)(void* Data, size_t DataSize, void* Parameter);
bool PopAllDataFromBufferQueue(void* Context, PopBufferQueueRoutine Callback, void* Parameter);
