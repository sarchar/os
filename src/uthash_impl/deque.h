#ifndef __DEQUE_H__
#define __DEQUE_H__

// deques are implemented with a circular doubly linked list

#define MAKE_DEQUE \
    void* _deque_next; void* _deque_prev

#define DEQUE_PUSH_BACK(head,newnode) do {                                \
        if((head) == null) {                                              \
            (newnode)->_deque_next = (newnode)->_deque_prev = (newnode);  \
            (head) = (newnode);                                           \
        } else {                                                          \
            *(&(head)->_deque_prev - 1) = (newnode); /* same as head->prev->next = newnode; next pointer is 1 before prev pointer */ \
            (newnode)->_deque_prev = (head)->_deque_prev;                 \
            (newnode)->_deque_next = (head);                              \
            (head)->_deque_prev = (newnode);                              \
        }                                                                 \
    } while(0); 

#define DEQUE_PUSH_FRONT(head,newnode) do {                               \
        if((head) == null) {                                              \
            (newnode)->_deque_next = (newnode)->_deque_prev = (newnode);  \
            (head) = (newnode);                                           \
        } else {                                                          \
            *(&(head)->_deque_prev - 1) = (newnode); /* same as head->prev->next = newnode; next pointer is 1 before prev pointer */ \
            (newnode)->_deque_prev = (head)->_deque_prev;                 \
            (newnode)->_deque_next = (head)->_deque_next;                 \
            *(&(head)->_deque_next + 1) = (newnode); /* same as head->next->prev = newnode; prev pointer is 1 after next pointer */  \
            (head) = (newnode);                                           \
        }                                                                 \
    } while(0); 

#define DEQUE_REMOVE(head,node) do {                                      \
        if((node) == (node)->_deque_prev) {                               \
            assert((node) == (node)->_deque_next, "required");            \
            (head) = null;                                                \
        } else {                                                          \
            *(&(node)->_deque_next + 1) = (node)->_deque_prev; /* node->next->prev = node->prev */     \
            *(&(node)->_deque_prev - 1) = (node)->_deque_next; /* node->prev->next = node->next */     \
            if((head) == (node)) (head) = (node)->_deque_next;            \
        }                                                                 \
    } while(0);

#define DEQUE_POP_BACK(head,res)      \
    if((head) == null) {              \
        (res) = null;                 \
    } else {                          \
        (res) = (head)->_deque_prev;  \
       DEQUE_REMOVE(head, res);       \
    }

#define DEQUE_POP_FRONT(head,res) \
    (res) = (head);               \
    if((res) != null) {           \
        DEQUE_REMOVE(head, res);  \
    }

#define DEQUE_NEXT(node) ((node)->_deque_next)
#define DEQUE_PREV(node) ((node)->_deque_prev)
#define DEQUE_HEAD(head) (head)
#define DEQUE_TAIL(head) DEQUE_PREV(head)

#endif
