#include "BufferQueue.h"
#include "CommonLib.h"
#include <Windows.h>
#include <intrin.h>

#ifdef _WIN64
typedef LONGLONG index_t;
#define IncrementIndex ::InterlockedAdd64
#else
typedef LONGLONG index_t;
#define IncrementIndex ::InterlockedAdd64
#endif

#define CACHE_BLOCK_SIZE 256

#pragma pack(push, 1)
struct BufferEntryHeader
{
    size_t size;
    size_t alignment;
    char data[1];
};
#pragma pack(pop)

struct BufferQueueContext
{
    HANDLE event;
    char*  buffer;
    size_t bufferSize;
    char*  cache;
    size_t cacheSize;
    volatile index_t topIndex;
    volatile index_t bottomIndex;
    CRITICAL_SECTION popSync;
};

void* CreateBufferQueue(size_t Size)
{
    BufferQueueContext* context = NULL;
    HANDLE event = NULL;
    char* buffer = NULL;
    char* cache  = NULL;

    if (!Size)
        return NULL;

    Size = AlignToTop(Size, 0x1000);

    memset(&context, 0, sizeof(context));

    event = ::CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!event)
        goto ReleaseBlock;

    buffer = (char*)::VirtualAlloc(NULL, Size, MEM_COMMIT, PAGE_READWRITE);
    if (!buffer)
        goto ReleaseBlock;

    cache = (char*)malloc(CACHE_BLOCK_SIZE);
    if (!cache)
        goto ReleaseBlock;

    context = (BufferQueueContext*)malloc(sizeof(BufferQueueContext));
    if (!context)
        goto ReleaseBlock;

    context->event = event;
    context->buffer = buffer;
    context->bufferSize = Size;
    context->cache = cache;
    context->cacheSize = CACHE_BLOCK_SIZE;
    context->topIndex = 0;
    context->bottomIndex = 0;

    ::InitializeCriticalSection(&context->popSync);

ReleaseBlock:

    if (!context)
    {
        if (event)
            ::CloseHandle(event);

        if (buffer)
            ::VirtualFree(buffer, 0, MEM_RELEASE);

        if (cache)
            free(cache);
    }

    return context;
}

void DestroyBufferQueue(void* Context)
{
    BufferQueueContext* context = (BufferQueueContext*)Context;

    ::CloseHandle(context->event);
    ::VirtualFree(context->buffer, 0, MEM_RELEASE);

    if (context->cache)
        free(context->cache);

    free(context);
}

bool PushDataToBufferQueue(void* Context, void* Data, size_t DataSize)
{
    BufferQueueContext* context = (BufferQueueContext*)Context;
    size_t blockSize = DataSize + sizeof(BufferEntryHeader) - 1;
    BufferEntryHeader* entry;
    index_t topIndex, bottomIndex;

    if (!DataSize)
        return false;

    ::EnterCriticalSection(&context->popSync);

    bottomIndex = context->bottomIndex % context->bufferSize;
    topIndex = context->topIndex % context->bufferSize;

    // If buffer is full
    if (context->topIndex + blockSize > context->bottomIndex + context->bufferSize)
    {
        ::LeaveCriticalSection(&context->popSync);
        return false;
    }

    context->topIndex += blockSize;

    if (context->topIndex % context->bufferSize < blockSize) // if buffer is divided
    {
        index_t part2Size = context->topIndex % context->bufferSize;
        index_t part1Size = DataSize - part2Size;

        entry = (BufferEntryHeader*)(context->buffer + topIndex);
        entry->size = DataSize;
        entry->alignment = 0;

        memcpy(entry->data, Data, part1Size);
        memcpy(context->buffer, (char*)Data + part1Size, part2Size);
    }
    else // if buffer isn't divided
    {
        index_t next = topIndex + blockSize + FIELD_OFFSET(BufferEntryHeader, data);
        size_t alignment = 0;

        if (next > context->bufferSize)
            alignment = FIELD_OFFSET(BufferEntryHeader, data) - (next - context->bufferSize);

        entry = (BufferEntryHeader*)(context->buffer + topIndex);
        entry->size = DataSize;
        entry->alignment = alignment;
        memcpy(entry->data, Data, DataSize);
    }

    ::LeaveCriticalSection(&context->popSync);

    return true;
}

bool PopDataFromBufferQueue(void* Context, void* OutputBuffer, size_t* OutputSize)
{
    BufferQueueContext* context = (BufferQueueContext*)Context;
    bool result = false;
    index_t bottomIndex, topIndex;
    BufferEntryHeader* entry;

    ::EnterCriticalSection(&context->popSync);

    bottomIndex = context->bottomIndex % context->bufferSize;
    topIndex = context->topIndex % context->bufferSize;

    if (topIndex != bottomIndex)
    {
        entry = (BufferEntryHeader*)(context->buffer + bottomIndex);
        if (entry->size <= *OutputSize)
        {
            index_t blockSize = entry->size + sizeof(BufferEntryHeader) - 1;
            index_t blockEnd = bottomIndex + blockSize;

            if (blockEnd > context->bufferSize)
            {
                index_t secondSize = blockEnd - context->bufferSize - (sizeof(BufferEntryHeader) - 1);
                size_t firstSize = entry->size - secondSize;

                memcpy(OutputBuffer, entry->data, firstSize);
                memcpy((char*)OutputBuffer + firstSize, (char*)context->buffer, secondSize);
            }
            else
            {
                memcpy(OutputBuffer, entry->data, entry->size);
            }

            *OutputSize = entry->size;
            context->bottomIndex += entry->size + entry->alignment + sizeof(BufferEntryHeader) - 1;
            result = true;
        }
    }

    ::LeaveCriticalSection(&context->popSync);

    return result;
}

bool PopAllDataFromBufferQueue(void* Context, PopBufferQueueRoutine Callback, void* Parameter)
{
    BufferQueueContext* context = (BufferQueueContext*)Context;

    while (true)
    {
        index_t bottomIndex, topIndex;
        BufferEntryHeader* entry;
        index_t blockSize, blockEnd;

        ::EnterCriticalSection(&context->popSync);

        bottomIndex = context->bottomIndex % context->bufferSize;
        topIndex = context->topIndex % context->bufferSize;

        ::LeaveCriticalSection(&context->popSync);
        
        if (bottomIndex == topIndex)
            break;

        entry = (BufferEntryHeader*)(context->buffer + bottomIndex);

        blockSize = entry->size + sizeof(BufferEntryHeader) - 1;
        blockEnd  = bottomIndex + blockSize;

        if (blockEnd > context->bufferSize) // if buffer is divided
        {
            index_t secondSize = blockEnd % context->bufferSize;
            size_t firstSize = entry->size - secondSize;

            if (context->cacheSize < entry->size)
            {
                char* cache = (char*)realloc(context->cache, entry->size);
                if (!cache)
                    return false;

                context->cache = cache;
                context->cacheSize = entry->size;
            }

            memcpy(context->cache, entry->data, firstSize);
            memcpy((char*)context->cache + firstSize, (char*)context->buffer, secondSize);

            Callback(context->cache, entry->size, Parameter);
        }
        else // if buffer isn't divided
        {
            Callback(entry->data, entry->size, Parameter);
        }

        ::EnterCriticalSection(&context->popSync);

        context->bottomIndex += entry->size + entry->alignment + sizeof(BufferEntryHeader) - 1;

        ::LeaveCriticalSection(&context->popSync);
    }

    return true;
}
