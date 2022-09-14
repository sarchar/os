#ifndef __DEQUE_H__
#define __DEQUE_H__

// deques are implemented using uthash's doubly linked list
#define uthash_assert(cond) assert(cond, "failure in utlist")
#include "uthash/src/utlist.h"

#define _DEQUE_NEXT _deque_next
#define _DEQUE_PREV _deque_prev

#define DEQUE_NEXT(node) ((node)->_DEQUE_NEXT)
#define DEQUE_PREV(node) ((node)->_DEQUE_PREV)
#define DEQUE_TAIL(head) ((head) == null) ? null : DEQUE_PREV(head)

#define MAKE_DEQUE(T) \
    T* _DEQUE_NEXT; T* _DEQUE_PREV

#define DEQUE_PUSH_BACK(head,newnode) \
    DL_APPEND2(head,newnode,_DEQUE_PREV,_DEQUE_NEXT)

#define DEQUE_PUSH_FRONT(head,newnode) \
    DL_PREPEND2(head,newnode,_DEQUE_PREV,_DEQUE_NEXT)

#define DEQUE_DELETE(head,node) \
    DL_DELETE2(head,node,_DEQUE_PREV,_DEQUE_NEXT)

#define DEQUE_POP_BACK(head,res) \
    (res) = DEQUE_TAIL(head);    \
    DEQUE_DELETE(head,res)

#define DEQUE_POP_FRONT(head,res) \
    (res) = (head);               \
    if((head) != null) DEQUE_DELETE(head,res)

#endif
