#include "AVLTree.h"
#include <memory.h>

// =================================================

static AVL_NODE* AllocateNode(AVL_TREE* Tree, size_t Size)
{
    size_t totalSize = Size + sizeof(AVL_NODE);
    AVL_NODE* node = (AVL_NODE*)Tree->Allocate(totalSize);

    if (!node)
        return 0;

    node->Left = 0;
    node->Right = 0;
    node->Height = 1;
    node->Value = node + 1;

    return node;
}

static void ReleaseNode(AVL_TREE* Tree, AVL_NODE* Node)
{
    Tree->Free(Node, Node->Value);
}

static unsigned int GetNodeHeight(AVL_NODE* Node)
{
    return (Node ? Node->Height : 0);
}

static int GetNodeBalanceFactor(AVL_NODE* Node)
{
    return GetNodeHeight(Node->Right) - GetNodeHeight(Node->Left);
}

static void RenewAVLNodeHeight(AVL_NODE* Node)
{
    unsigned int leftHeight = GetNodeHeight(Node->Left);
    unsigned int rightHeight = GetNodeHeight(Node->Right);

    Node->Height = (leftHeight > rightHeight ? leftHeight : rightHeight) + 1;
}

static AVL_NODE* RotateRight(AVL_NODE* Node)
{
    AVL_NODE* leftNode = Node->Left;

    Node->Left = leftNode->Right;
    leftNode->Right = Node;
    
    RenewAVLNodeHeight(Node);
    RenewAVLNodeHeight(leftNode);
    
    return leftNode;
}

static AVL_NODE* RotateLeft(AVL_NODE* Node)
{
    AVL_NODE* rightNode = Node->Right;

    Node->Right = rightNode->Left;
    rightNode->Left = Node;

    RenewAVLNodeHeight(Node);
    RenewAVLNodeHeight(rightNode);

    return rightNode;
}

static AVL_NODE* BalanceNode(AVL_NODE* Node)
{
    int balanceFactor;

    RenewAVLNodeHeight(Node);

    balanceFactor = GetNodeBalanceFactor(Node);
    if (balanceFactor == 2)
    {
        int balanceFactorRight = GetNodeBalanceFactor(Node->Right);

        if (balanceFactorRight < 0)
            Node->Right = RotateRight(Node->Right);

        return RotateLeft(Node);
    }
    else if (balanceFactor == -2)
    {
        int balanceFactorLeft = GetNodeBalanceFactor(Node->Left);

        if (balanceFactorLeft > 0)
            Node->Left = RotateLeft(Node->Left);

        return RotateRight(Node);
    }

    return Node;
}

static AVL_NODE* InsertNode(AVL_TREE* Tree, AVL_NODE* Node, void* Buffer, size_t Size)
{
    AVL_NODE* node = 0;

    if (!Node)
    {
        node = AllocateNode(Tree, Size);

        if (!node)
            return 0;

        memcpy(node->Value, Buffer, Size);
        Tree->Latest = node;
    }
    else
    {
        int result = Tree->Compare(Buffer, Node->Value);

        if (result < 0)
        { // Buffer < Value
            AVL_NODE* left = InsertNode(Tree, Node->Left, Buffer, Size);
            
            if (!left)
                return 0;

            Node->Left = left;
        }
        else if (result > 0)
        { // Buffer > Value
            AVL_NODE* right = InsertNode(Tree, Node->Right, Buffer, Size);
        
            if (!right)
                return 0;

            Node->Right = right;
        }
        else
        { // Buffer == Value
            return 0;
        }

        node = BalanceNode(Node);
    }

    return node;
}

static AVL_NODE* FindMinNode(AVL_NODE* Node)
{
    return (Node->Left ? FindMinNode(Node->Left) : Node);
}

static AVL_NODE* RemoveMinNode(AVL_NODE* Node)
{
    if (Node->Left == 0)
        return Node->Right;

    Node->Left = RemoveMinNode(Node->Left);

    return BalanceNode(Node);
}

static AVL_NODE* RemoveNode(AVL_TREE* Tree, AVL_NODE* Node, void* Buffer)
{
    if (!Node)
        return 0;

    int result = Tree->Compare(Buffer, Node->Value);
    if (result < 0)
    { // Buffer > Value
        Node->Left = RemoveNode(Tree, Node->Left, Buffer);
    }
    else if (result > 0)
    { // Buffer > Value
        Node->Right = RemoveNode(Tree, Node->Right, Buffer);
    }
    else
    { // Buffer == Value
        AVL_NODE* left = Node->Left;
        AVL_NODE* right = Node->Right;

        ReleaseNode(Tree, Node);
        Tree->Latest = Node;

        if (!right)
            return left;

        AVL_NODE* min = FindMinNode(right);
        min->Right = RemoveMinNode(right);
        min->Left = left;

        return BalanceNode(min);
    }

    return BalanceNode(Node);
}

static void RemoveTree(AVL_TREE* Tree, AVL_NODE* Node)
{
    if (!Node)
        return;

    RemoveTree(Tree, Node->Left);
    RemoveTree(Tree, Node->Right);

    ReleaseNode(Tree, Node);
}

static AVL_NODE* FindNode(AVL_TREE* Tree, AVL_NODE* Node, void* Buffer)
{
    if (!Node)
        return 0;

    int result = Tree->Compare(Buffer, Node->Value);
    if (result < 0)
    { // Buffer > Value
        return FindNode(Tree, Node->Left, Buffer);
    }
    else if (result > 0)
    { // Buffer > Value
        return FindNode(Tree, Node->Right, Buffer);
    }
    else
    { // Buffer == Value
        return Node;
    }
}

// =================================================

void InitializeAVLTree(AVL_TREE* Tree, AVL_ALLOCATE_CALLBACK Allocate, AVL_FREE_CALLBACK Free, AVL_COMPARE_CALLBACK Compare)
{
    Tree->Root = 0;
    Tree->Latest = 0;
    Tree->Allocate = Allocate;
    Tree->Free = Free;
    Tree->Compare = Compare;
}

void DestroyAVLTree(AVL_TREE* Tree)
{
    RemoveTree(Tree, Tree->Root);
    memset(Tree, 0, sizeof(AVL_TREE));
}

void* InsertAVLElement(AVL_TREE* Tree, void* Buffer, size_t Size)
{
    AVL_NODE* newRoot = InsertNode(Tree, Tree->Root, Buffer, Size);

    if (!newRoot)
        return 0;

    Tree->Root = newRoot;
    
    return Tree->Latest->Value;
}

bool RemoveAVLElement(AVL_TREE* Tree, void* Buffer)
{
    Tree->Latest = 0;
    Tree->Root = RemoveNode(Tree, Tree->Root, Buffer);
    return (Tree->Latest != 0);
}

void* FindAVLElement(AVL_TREE* Tree, void* Buffer)
{
    AVL_NODE* node = FindNode(Tree, Tree->Root, Buffer);
    
    if (!node)
        return 0;

    return node->Value;
}
