

#ifndef __LIST__H__
#define __LIST__H__


#include "config.h"


struct list_node_s {
    struct list_node_s *prev;
    struct list_node_s *next;
};


struct list_s {
    struct list_node_s *head;
    struct list_node_s *tail;
};


typedef struct list_s         list_t;
typedef struct list_node_s    list_node_t;
typedef struct list_node_s*   list_iter_t;


#define list_init(list)                                                       \
    do {                                                                      \
        (list).head = NULL;                                                   \
        (list).tail = NULL;                                                   \
    } while (false)

#define list_element(ptr, type, member)                                       \
    ((type)(((unsigned char*)(ptr)) - (unsigned char*)(&(((type)0)->member))))

#define list_is_empty(list)                                                   \
    (((list).head == NULL) && ((list).tail == NULL))

#define list_is_singular(list)                                                \
    ((!list_is_empty(list)) && ((list).head == (list).tail))

#define list_replace(o, n)                                                    \
    __list_replace__(&(o), &(n))

#define list_push_back(list, node)                                            \
    __list_push_back__(&(list), &(node))

#define list_push_front(list, node)                                           \
    __list_push_front__(&(list), &(node))

#define list_pop_back(list)                                                   \
    do {                                                                      \
        __list_erase__(&(list), (list).tail);                                 \
        ((list).head) ? ((list).tail = (list).head->prev) : ((list).tail = NULL);\
    } while (false)

#define list_pop_front(list)                                                  \
    do {                                                                      \
        __list_erase__(&(list), (list).head);                                 \
        ((list).tail) ? ((list).head = (list).tail->next) : ((list).tail = NULL);\
    } while (false)

#define list_begin(list)                                                      \
    ((list).head)

#define list_rbegin(list)                                                     \
    ((list).tail)

#define list_end(list)                                                        \
    (NULL)

#define list_rend(list)                                                       \
    (NULL)

#define list_next(list, iter)                                                 \
    (((iter)->next == (list).head) ? ((iter) = NULL) : ((iter) = (iter)->next))

#define list_rnext(list, iter)                                                \
    (((iter)->prev == (list).tail) ? ((iter) = NULL) : ((iter) = (iter)->prev))

#define list_erase(list, node)                                                \
    __list_erase__(&(list), &(node))

#define list_for_each(list, iter)                                             \
    for ((iter) = (list).head;                                                \
         ((iter) != NULL);                                                    \
         (iter)->next == (list).head ? ((iter) = NULL) : ((iter) = (iter)->next))

#define list_safe_for_each(list, iter, next_iter)                                                                 \
    for (((iter) = (list).head), ((next_iter) = (((iter) && (!list_is_singular((list)))) ? (iter)->next : NULL)); \
         (iter) && (list).head;                                                                                   \
         ((iter) = (next_iter), (next_iter) = ((iter) ? (iter)->next : NULL)), ((next_iter) = (((next_iter) == list.head) ? NULL : (next_iter))))

#define list_reverse_for_each(list, iter)                                     \
    for ((iter) = (list).tail;                                                \
         ((iter) != NULL);                                                    \
         (iter)->prev == (list).tail ? ((iter) = NULL) : ((iter) = (iter)->prev))

#define list_safe_reverse_for_each(list, iter, next_iter)                                                         \
    for (((iter) = (list).tail), ((next_iter) = (((iter) && (!list_is_singular((list)))) ? (iter)->prev : NULL)); \
         (iter) && (list).tail;                                                                                   \
         ((iter) = (next_iter), (next_iter) = ((iter) ? (iter)->prev : NULL)), ((next_iter) = (((next_iter) == list.tail) ? NULL : (next_iter))))


void __list_push_back__(list_t *list, list_node_t *node);
void __list_push_front__(list_t *list, list_node_t *node);
void __list_replace__(list_node_t *o, list_node_t *n);
void __list_erase__(list_t *list, list_node_t *node);


#endif
