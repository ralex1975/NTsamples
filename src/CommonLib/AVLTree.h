#pragma once

typedef void*(*AVL_ALLOCATE_CALLBACK)(size_t NodeBufSize);
typedef void(*AVL_FREE_CALLBACK)(void* NodeBuf, void* Node);
typedef int(*AVL_COMPARE_CALLBACK)(void* Node1, void* Node2);

struct AVL_NODE
{
    AVL_NODE*    Left;
    AVL_NODE*    Right;
    unsigned int Height;
    void*        Value;
};

struct AVL_TREE
{
    AVL_NODE*             Root;
    AVL_NODE*             Latest;
    AVL_ALLOCATE_CALLBACK Allocate;
    AVL_FREE_CALLBACK     Free;
    AVL_COMPARE_CALLBACK  Compare;
};

void InitializeAVLTree(AVL_TREE* Tree, AVL_ALLOCATE_CALLBACK Allocate, AVL_FREE_CALLBACK Free, AVL_COMPARE_CALLBACK Compare);
void DestroyAVLTree(AVL_TREE* Tree);

void* InsertAVLElement(AVL_TREE* Tree, void* Buffer, size_t Size);
bool RemoveAVLElement(AVL_TREE* Tree, void* Buffer);

void* FindAVLElement(AVL_TREE* Tree, void* Buffer);
