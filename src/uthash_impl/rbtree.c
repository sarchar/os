#include "kernel/common.h"

#include "rbtree.h"

#define has_parent(node) ((node) && (node)->parent)
#define has_grandparent(node) (has_parent() && (node)->parent->parent)
#define sibling(node) (((node)->parent == null) ? null : (((node)->parent->left == (node)) ? (node)->parent->right : (node)->parent->left))

static __always_inline bool is_red(struct rb_node* node)
{
    // null nodes are black
    return (node != null) && (node->color);
}

static void _left_rotate(struct rb_node** rootptr, struct rb_node* rot)
{
    /* rotate the `rot` node counter-clockwise around its right child
     * so a tree that looks like this:
     *
     *      P
     *      |
     *     rot
     *    /   \
     *   L     R
     *  / \   / \
     * a   b c   d
     *
     * will look like:
     *
     *       P
     *       |
     *       R
     *      / \
     *    rot  d
     *    / \
     *   L   c
     *  / \
     * a   b
     */
    
    struct rb_node* tmp = rot->right;
    
    if(rot->parent != null) {
        if(rot->parent->left == rot) rot->parent->left  = tmp;
        else                         rot->parent->right = tmp; // must be the ase
    } else {
        // null parent means its the root of the tree
        *rootptr = tmp;
    }

    tmp->parent = rot->parent; // might be null
    rot->parent = tmp;
    rot->right  = tmp->left;
    if(rot->right != null) rot->right->parent = rot;
    tmp->left   = rot;
}

static void _right_rotate(struct rb_node** rootptr, struct rb_node* rot)
{
    /* rotate the `rot` node clockwise around its left child
     * so a tree that looks like this:
     *
     *      P
     *      |
     *     rot
     *    /   \
     *   L     R
     *  / \   / \
     * a   b c   d
     *
     * will look like:
     *
     *       P
     *       |
     *       L
     *      / \
     *     a  rot
     *        / \
     *       b   R
     *            \
     *             d
     */

    struct rb_node* tmp = rot->left;
    
    if(rot->parent != null) {
        if(rot->parent->left == rot) rot->parent->left  = tmp;
        else                         rot->parent->right = tmp; // must be the ase
    } else {
        // null parent means its the root of the tree
        *rootptr = tmp;
        tmp->parent = null;
    }

    tmp->parent = rot->parent; // might be null
    rot->parent = tmp;
    rot->left   = tmp->right;
    if(rot->left != null) rot->left->parent = rot;
    tmp->right  = rot;
}

static void _insert_fixup(struct rb_node** rootptr, struct rb_node* node)
{
    struct rb_node* cur = node;

    while(is_red(cur->parent)) {
        if(cur->parent == cur->parent->parent->left) { 
            /* if node's parent == node's grandparents left child, i.e., either
             *        g           g
             *       /           /
             *      p     or    p
             *     /             \
             *    cur             cur
             */
            struct rb_node* aunt = cur->parent->parent->right; // take a look at the aunt
            if(is_red(aunt)) { // for red aunt, simple recolor
                // color grandparent red and her children black
                cur->parent->parent->color = true; 
                aunt->color = false; 
                cur->parent->color = false; 
                cur = cur->parent->parent;
            } else { // black aunt, so we rotate
                if(cur == cur->parent->right) { 
                    /* right branch of left parent branch, so:
                     *        g                      g
                     *       /                      / \
                     *      p     or likely        p   ...
                     *       \                    / \
                     *        cur              ...   cur
                     * we need to perform a Left-Right rotate, since we fall through to a right rotate, we just do left here
                     */
                    cur = cur->parent;
                    _left_rotate(rootptr, cur);

                    /* now our tree looks like (note cur changes too):
                     *      g         g
                     *     /         /
                     *   cur   =>   p
                     *   /         /
                     *  p         cur
                     */
                } 

                cur->parent->color = false; // color the parent black
                cur->parent->parent->color = true; // color the grandparent red

                /* now we have a left branch of left parent branch, so:
                 *        g                      g
                 *       /                      / \
                 *      p     or likely        p   ...
                 *     /                      / \
                 *    cur                   cur  ...
                 * easy case means a right rotate of g about p.
                 */
                _right_rotate(rootptr, cur->parent->parent);

                /* now our tree looks like:
                 *      p
                 *     / \
                 *   cur  g
                 */
            }
        } else { // cur->parent == cur->parent->parent->right
            /* if node's parent == node's grandparents right child, i.e., either
             *    g         g
             *     \         \
             *      p  or     p
             *     /           \
             *    cur          cur
             */
            struct rb_node* aunt = cur->parent->parent->left; // take a look at the aunt
            if(is_red(aunt)) { // for red aunt, simple recolor
                // color grandparent red and her children black
                cur->parent->parent->color = true; 
                aunt->color = false; 
                cur->parent->color = false; 
                cur = cur->parent->parent;
            } else { // black aunt, so we rotate
                if(cur == cur->parent->left) { 
                    /* left branch of right parent branch, so:
                     *        g                      g
                     *       / \                    / \
                     *          p     or likely   ...  p 
                     *         /                      / \
                     *        cur                   cur ...
                     * we need to perform a Right-Left rotate, since we fall through to a left rotate, we just do right here
                     */
                    cur = cur->parent;
                    _right_rotate(rootptr, cur);

                    /* now our tree looks like (note cur changes too):
                     *  g        g
                     *   \        \
                     *  cur   =>   p   
                     *     \        \
                     *      p       cur
                     */
                } 

                cur->parent->color = false; // color the parent black
                cur->parent->parent->color = true; // color the grandparent red

                /* now we have a left branch of left parent branch, so:
                 *   g                      g
                 *    \                    / \
                 *     p     or likely   ...  p
                 *      \                    / \
                 *      cur               ...  cur
                 * easy case means a left rotate of g about p.
                 */
                _left_rotate(rootptr, cur->parent->parent);

                /* now our tree looks like:
                 *      p
                 *     / \
                 *    g  cur
                 */
            }
        }
    }

    // make root black
    (*rootptr)->color = false;
}

void  _RBTREE__insert(void** _rootptr, void* _node, _rb_node_comparison_func* cmp)
{
    struct rb_node** rootptr = (struct rb_node**)_rootptr;
    struct rb_node* node = (struct rb_node*)_node;

    // locate the position for node in typical binary tree fashion, keeping track of the parent
    struct rb_node* parent = null;
    struct rb_node* cur = *rootptr;
    while(cur != null) {
        parent = cur;
        if(cmp(node, cur) < 0) {
            cur = cur->left;
        } else {
            cur = cur->right;
        }
    }

    // save the parent
    node->parent = parent;

    if(parent == null) { // handle root node situation
        *rootptr = node;
    } else if(cmp(node, parent) < 0) { // if new value is less than parent, take left child
        parent->left = node;
    } else { // new value is gte than parent, take right child
        parent->right = node;
    }

    // initialize the new node to red with null children
    node->right = null;
    node->left = null;
    node->color = true;

    _insert_fixup(rootptr, node);
}

static void _remove_fixup(struct rb_node** rootptr, struct rb_node* node)
{
    while(node != *rootptr) {
        struct rb_node* parent = node->parent;
        struct rb_node* sib = sibling(node);
        if(sib == null) {
            node = parent;
            continue;
        }

        // non-null sibling implies non-null parent
        if(is_red(sib)) {
            // recolor parent and sibling
            parent->color = true;
            sib->color = false;
            if(parent->left == sib) _right_rotate(rootptr, parent);
            else                    _left_rotate(rootptr, parent);

            // fix up 'node' again
            continue;
        }

        // sibling black
        if(is_red(sib->left) || is_red(sib->right)) { // and has at least one red child
            if(is_red(sib->left)) { // left child of sib
                if(parent->left == sib) { // left child of parent: left-left case
                    sib->left->color = sib->color;
                    sib->color = parent->color;
                    _right_rotate(rootptr, parent);
                } else { // right child of parent: right-left case
                    sib->left->color = parent->color;
                    _right_rotate(rootptr, sib);
                    _left_rotate(rootptr, parent); // can't use sib->parent here because it changes after the right_rotate, we need to rotate the old sib->parent
                }
            } else { // right child of sib is red
                if(parent->left == sib) { // left child of parent: left-right case
                    sib->right->color = parent->color;
                    _left_rotate(rootptr, sib);
                    _right_rotate(rootptr, parent);
                } else { // right child of parent: right-right case
                    sib->right->color = sib->color;
                    sib->color = parent->color;
                    _left_rotate(rootptr, parent);
                }
            }
            parent->color = false; // parent turns black in this case
            break; // done, no more fixups
        }

        // sib has two black children nodes
        sib->color = true;
        if(is_red(parent)) {
            // done
            parent->color = false;
            break;
        }
    
        // continue fixing up the parent
        node = parent;
    }
}

// algorithm followed from here https://www.geeksforgeeks.org/red-black-tree-set-3-delete-2/
void  _RBTREE__remove(void** _rootptr, void* _node)
{
    struct rb_node** rootptr = (struct rb_node**)_rootptr;
    struct rb_node* node = (struct rb_node*)_node;

    while(true) {
        // find the replacement node
        struct rb_node* repl;
        if(node->left != null && node->right != null) { // have both subtrees
            // find the inorder successor after `node`, i.e., the minimum of the right subtree
            repl = node->right;
            while(repl->left != null) repl = repl->left;
        } else if(node->left == null && node->right == null) { // leaf
            repl = null;
        } else if(node->left != null) {
            repl = node->left;
        } else {
            repl = node->right;
        }

        bool both_black = !is_red(node) && !is_red(repl);
        struct rb_node* parent = node->parent;

        if(repl == null) { // leaf node case
            if(parent == null) { // tree is now empty
                (*rootptr) = null;
            } else {
                if(both_black) _remove_fixup(rootptr, node); // since repl was null, this really just checks if `node` was black
                else {
                    // since repl was null (black), that means `node` is red. if there's a sibling, change it to red
                    struct rb_node* sib = sibling(node);
                    if(sib != null) sib->color = true;
                }

                // remove node from the tree (node has no children)
                if(parent->left == node) {
                    parent->left = null;
                } else {
                    parent->right = null;
                }
            }

            // done
            return;
        }

        if(node->left == null || node->right == null) { // node has one child since repl isn't null
            if(parent == null) { // if we're removing the root
                (*rootptr) = repl; // will be the non-null child of `node`
                repl->parent = null;
            } else {
                // remove `node` from the tree, and move `repl` up
                if(parent->left == node) parent->left = repl;
                else                     parent->right = repl;
                repl->parent = parent;

                // fixup the tree in the double-black situation
                if(both_black) _remove_fixup(rootptr, repl);
                else {
                    // otherwise, we can keep the black color at this location
                    repl->color = false;
                }
            }

            return;
        }

        // `node` has two valid children, and repl is the inorder successor to this node
        // so repl has no left child, and might have a right child. we have to swap these two nodes
        // keeping all the other surrounding nodes in the same location
        //
        struct rb_node* tmp = repl->right; // save repl's right child
        repl->right = node->right;         // make new right be node's right
        if(repl->right != null) repl->right->parent = repl; // fix up node->right's parent to be repl
        repl->left = node->left;           // make new left be node's left (repl->left was null before and doesn't need to be saved)
        if(repl->left != null) repl->left->parent = repl; // fix up node->left's parent to be repl

        // update repl's parent
        struct rb_node* replparent = repl->parent; // save the old one
        if(parent == null) (*rootptr) = repl; // if we're swapping the root, update the root
        else if(parent->left == node) parent->left = repl;
        else                          parent->right = repl;
        repl->parent = parent;       // update the parent of repl, might be null

        // update node's new parent
        if(replparent->left == repl) replparent->left = node;
        else                         replparent->right = node;
        node->parent = replparent;

        // fix up node's children to be those of repl
        node->right = tmp;
        if(node->right != null) node->right->parent = node;
        node->left = null; // has no left now

        // swap the colors
        bool oldcolor = node->color;
        node->color = repl->color;
        repl->color = oldcolor;

        // try to delete `node` again, which now has at most 1 child
    }
}

void* _RBTREE__find(void* _root, void* _key, _rb_node_comparison_func* cmp)
{
    struct rb_node* cur = (struct rb_node*)_root;
    struct rb_node* key = (struct rb_node*)_key;

    while(cur != null) {
        s64 c = cmp(key, cur);
        if(c < 0) {
            cur = cur->left;
        } else if(c > 0) {
            cur = cur->right;
        } else {
            return cur;
        }
    }

    return null;
}

void* _RBTREE__first(void* _root, bool asc)
{
    struct rb_node* cur = (struct rb_node*)_root;
    if(cur == null) return null;

    if(asc) {
        while(cur->left != null) cur = cur->left;   
    } else {
        while(cur->right != null) cur = cur->right;   
    }

    return cur;
}

void* _RBTREE__next(void* _cur, bool asc)
{
    struct rb_node* cur = (struct rb_node*)_cur;

    // cur has been processed, so find the next node
    if(asc) {
        if(cur->right) return _RBTREE__first(cur->right, true);

        // go up the tree looking for another non-null right node
        while(cur->parent != null) {
            cur = cur->parent;
            if(cur->right != null) {
                return _RBTREE__first(cur->right, true);
            }
        }

        // if we get there then no more nodes
    } else {
        if(cur->left) return _RBTREE__first(cur->right, false);

        // go up the tree looking for another non-null left node
        while(cur->parent != null) {
            cur = cur->parent;
            if(cur->left != null) {
                return _RBTREE__first(cur->right, false);
            }
        }

        // if we get there then terminate the loop
    }

    return null;
}

