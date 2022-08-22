#ifndef __RBTREE_H__
#define __RBTREE_H__

// 25 (or 32) bytes overhead per node, depending on if your usage is packed or not
// TODO get rid of 'color' by embedding it into the pointers themselves, and forcing alignment to 8 bytes?
// TODO to do that would require looking up parent->left, for example, to determine this node's color
// TODO support MAKE_RB_TREE not having to be first in the structure. can do this with offsetof() and
// TODO passing that value into each of the _RBTREE_ functions
struct rb_node {
    struct rb_node* parent;
    struct rb_node* left;
    struct rb_node* right;
    bool color;
} __packed;

#define MAKE_RB_TREE \
    struct rb_node _rbn;

#define RB_TREE_INSERT(root,newnode,cmp) \
    _RBTREE__insert((void**)&(root), newnode, (_rb_node_comparison_func*)&(cmp))

// returns boolean if `ret` is not null
#define RB_TREE_FIND(root,ret,key,cmp) \
    (((ret) = _RBTREE__find((void*)(root), &(key), (_rb_node_comparison_func*)&(cmp))) != null)

#define RB_TREE_REMOVE(root,node) \
    _RBTREE__remove((void**)&(root), node)

#define RB_TREE_FOREACH(root, node) \
    for(node = _RBTREE__first(root, true); \
            (node) != null; node = _RBTREE__next(node, true))

#define RB_TREE_FOREACH_REVERSE(root, node) \
    for(node = _RBTREE__first(root, false); \
            (node) != null; node = _RBTREE__next(node, false))

#define RB_TREE_FOREACH_SAFE(root, node, next) \
    for(node = _RBTREE__first(root, true), next = _RBTREE__next(node, true); \
            (node) != null; node = next, next = _RBTREE__next(next, true))

#define RB_TREE_FOREACH_REVERSE_SAFE(root, node, next) \
    for(node = _RBTREE__first(root, false), next = _RBTREE__next(node, false); \
            (node) != null; node = next, next = _RBTREE__next(next, false))


// usage:
//
// struct my_node {
//      MAKE_RB_TREE;
//      
//      // fields relevant to my_node, especially a key for sorting
//      u64 key;
//      u64 other;
// };
//
// // a comparison function
// s64 cmp_my_node(struct my_node* a, struct my_node* b) {
//    return (b->key - a->key);  
// }
//
// // declare root
// struct my_node* root;
//
// // insert
// struct my_node* new_node = create_my_node();
// RB_TREE_INSERT(root, newnode, cmp_my_node);
//
// // lookup
// struct my_node* result;
// struct my_node key = { .key = value; };
// RB_TREE_FIND(root, result, key, cmp_my_node);
//
// // remove
// RB_TREE_REMOVE(root, node); // node must exist in the tree
//
// // iterate (ascending)
// struct my_node* node;
// RB_TREE_FOREACH(root, node) {
//    print_node(node);
// }
//
// // iterate (descending)
// RB_TREE_FOREACH_REVERSE(root, node) {
//    print_node(node);
// }
//
// // safe iterate (allows removal during loop traversal but requires an extra temp pointer)
// strut my_node* next;
// RB_TREE_FOREACH_SAFE(root, node, next) {
//    or
// RB_TREE_FOREACH_REVERSE_SAFE(root, node, next) {
//    print_node(node);
// }

// do not call these functions directly
typedef s64 (_rb_node_comparison_func)(void const*, void const*);

void  _RBTREE__insert(void**, void*, _rb_node_comparison_func*);
void* _RBTREE__find(void*, void*, _rb_node_comparison_func*);
void  _RBTREE__remove(void**, void*);
void* _RBTREE__first(void*, bool);
void* _RBTREE__next(void*, bool);

#endif
