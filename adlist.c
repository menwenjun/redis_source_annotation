/* adlist.c - A generic doubly linked list implementation
 *
 * Copyright (c) 2006-2010, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */


#include <stdlib.h>
#include "adlist.h"
#include "zmalloc.h"

/* Create a new list. The created list can be freed with
 * AlFreeList(), but private value of every node need to be freed
 * by the user before to call AlFreeList().
 *
 * On error, NULL is returned. Otherwise the pointer to the new list. */
list *listCreate(void)  //创建一个表头
{
    struct list *list;

    //为表头分配内存
    if ((list = zmalloc(sizeof(*list))) == NULL)
        return NULL;
    //初始化表头
    list->head = list->tail = NULL;
    list->len = 0;
    list->dup = NULL;
    list->free = NULL;
    list->match = NULL;

    return list;    //返回表头
}

/* Free the whole list.
 *
 * This function can't fail. */
void listRelease(list *list)    //释放list表头和链表
{
    unsigned long len;
    listNode *current, *next;

    current = list->head;   //备份头节点地址
    len = list->len;        //备份链表元素个数，使用备份操作防止更改原有信息
    while(len--) {          //遍历链表
        next = current->next;
        if (list->free) list->free(current->value); //如果设置了list结构的释放函数，则调用该函数释放节点值
        zfree(current);
        current = next;
    }
    zfree(list);    //最后释放表头
}

/* Add a new node to the list, to head, containing the specified 'value'
 * pointer as value.
 *
 * On error, NULL is returned and no operation is performed (i.e. the
 * list remains unaltered).
 * On success the 'list' pointer you pass to the function is returned. */
list *listAddNodeHead(list *list, void *value)  //将value添加到list链表的头部
{
    listNode *node;

    if ((node = zmalloc(sizeof(*node))) == NULL)    //为新节点分配空间
        return NULL;
    node->value = value;    //设置node的value值

    if (list->len == 0) {   //将node头插到空链表
        list->head = list->tail = node;
        node->prev = node->next = NULL;
    } else {                //将node头插到非空链表
        node->prev = NULL;
        node->next = list->head;
        list->head->prev = node;
        list->head = node;
    }

    list->len++;    //链表元素计数器加1

    return list;
}

/* Add a new node to the list, to tail, containing the specified 'value'
 * pointer as value.
 *
 * On error, NULL is returned and no operation is performed (i.e. the
 * list remains unaltered).
 * On success the 'list' pointer you pass to the function is returned. */
list *listAddNodeTail(list *list, void *value)  //将value添加到list链表的尾部
{
    listNode *node;

    if ((node = zmalloc(sizeof(*node))) == NULL)    //为新节点分配空间
        return NULL;
    node->value = value;    //设置node的value值
    if (list->len == 0) {   //将node尾插到空链表
        list->head = list->tail = node;
        node->prev = node->next = NULL;
    } else {                //将node头插到非空链表
        node->prev = list->tail;
        node->next = NULL;
        list->tail->next = node;
        list->tail = node;
    }
    list->len++;    //更新链表节点计数器

    return list;
}

list *listInsertNode(list *list, listNode *old_node, void *value, int after)    //在list中，根据after在old_node节点前后插入值为value的节点。
{
    listNode *node;

    if ((node = zmalloc(sizeof(*node))) == NULL) //为新节点分配空间
        return NULL;
    node->value = value;    //设置node的value值

    if (after) {    //after 非零，则将节点插入到old_node的后面
        node->prev = old_node;
        node->next = old_node->next;
        if (list->tail == old_node) {   //目标节点如果是链表的尾节点，更新list的tail指针
            list->tail = node;
        }
    } else {        //after 为零，则将节点插入到old_node的前面
        node->next = old_node;
        node->prev = old_node->prev;
        if (list->head == old_node) {   //如果节点如果是链表的头节点，更新list的head指针
            list->head = node;
        }
    }
    if (node->prev != NULL) {   //如果有，则更新node的前驱节点的指针
        node->prev->next = node;
    }
    if (node->next != NULL) {   //如果有，则更新node的后继节点的指针
        node->next->prev = node;
    }
    list->len++;    //更新链表节点计数器
    return list;
}

/* Remove the specified node from the specified list.
 * It's up to the caller to free the private value of the node.
 *
 * This function can't fail. */
void listDelNode(list *list, listNode *node)    //从list删除node节点
{
    if (node->prev) //更新node的前驱节点的指针
        node->prev->next = node->next;
    else
        list->head = node->next;
    if (node->next) //更新node的后继节点的指针
        node->next->prev = node->prev;
    else
        list->tail = node->prev;

    if (list->free) list->free(node->value);    //如果设置了list结构的释放函数，则调用该函数释放节点值
    zfree(node);    //释放节点
    list->len--;    //更新链表节点计数器
}

/* Returns a list iterator 'iter'. After the initialization every
 * call to listNext() will return the next element of the list.
 *
 * This function can't fail. */
listIter *listGetIterator(list *list, int direction)    //为list创建一个迭代器iterator
{
    listIter *iter;

    if ((iter = zmalloc(sizeof(*iter))) == NULL) return NULL;   //为迭代器申请空间
    if (direction == AL_START_HEAD)     //设置迭代指针的起始位置
        iter->next = list->head;
    else
        iter->next = list->tail;
    iter->direction = direction;        //设置迭代方向
    return iter;
}

/* Release the iterator memory */
void listReleaseIterator(listIter *iter) {  //释放iter迭代器
    zfree(iter);
}

/* Create an iterator in the list private iterator structure */
void listRewind(list *list, listIter *li) { //将迭代器li重置为list的头结点并且设置为正向迭代
    li->next = list->head;              //设置迭代指针的起始位置
    li->direction = AL_START_HEAD;      //设置迭代方向从头到尾
}

void listRewindTail(list *list, listIter *li) { //将迭代器li重置为list的尾结点并且设置为反向迭代
    li->next = list->tail;              //设置迭代指针的起始位置
    li->direction = AL_START_TAIL;      //设置迭代方向从尾到头
}

/* Return the next element of an iterator.
 * It's valid to remove the currently returned element using
 * listDelNode(), but not to remove other elements.
 *
 * The function returns a pointer to the next element of the list,
 * or NULL if there are no more elements, so the classical usage patter
 * is:
 *
 * iter = listGetIterator(list,<direction>);
 * while ((node = listNext(iter)) != NULL) {
 *     doSomethingWith(listNodeValue(node));
 * }
 *
 * */
listNode *listNext(listIter *iter)  //返回迭代器iter指向的当前节点并更新iter
{
    listNode *current = iter->next; //备份当前迭代器指向的节点

    if (current != NULL) {
        if (iter->direction == AL_START_HEAD)   //根据迭代方向更新迭代指针
            iter->next = current->next;
        else
            iter->next = current->prev;
    }
    return current;     //返回备份的当前节点地址
}

/* Duplicate the whole list. On out of memory NULL is returned.
 * On success a copy of the original list is returned.
 *
 * The 'Dup' method set with listSetDupMethod() function is used
 * to copy the node value. Otherwise the same pointer value of
 * the original node is used as value of the copied node.
 *
 * The original list both on success or error is never modified. */
list *listDup(list *orig)   //拷贝表头为orig的链表并返回
{
    list *copy;
    listIter *iter;
    listNode *node;

    if ((copy = listCreate()) == NULL)  //创建一个表头
        return NULL;

    //设置新建表头的处理函数
    copy->dup = orig->dup;
    copy->free = orig->free;
    copy->match = orig->match;
    //迭代整个orig的链表
    iter = listGetIterator(orig, AL_START_HEAD);    //为orig定义一个迭代器并设置迭代方向
    while((node = listNext(iter)) != NULL) {    //迭代器根据迭代方向不停迭代
        void *value;

        //复制节点值到新节点
        if (copy->dup) {
            value = copy->dup(node->value); //如果定义了list结构中的dup指针，则使用该方法拷贝节点值。
            if (value == NULL) {
                listRelease(copy);
                listReleaseIterator(iter);
                return NULL;
            }
        } else
            value = node->value;    //获得当前node的value值

        if (listAddNodeTail(copy, value) == NULL) { //将node节点尾插到copy表头的链表中
            listRelease(copy);
            listReleaseIterator(iter);
            return NULL;
        }
    }
    listReleaseIterator(iter);  //自行释放迭代器
    return copy;    //返回拷贝副本
}

/* Search the list for a node matching a given key.
 * The match is performed using the 'match' method
 * set with listSetMatchMethod(). If no 'match' method
 * is set, the 'value' pointer of every node is directly
 * compared with the 'key' pointer.
 *
 * On success the first matching node pointer is returned
 * (search starts from head). If no matching node exists
 * NULL is returned. */
listNode *listSearchKey(list *list, void *key)  //在list中查找value为key的节点并返回
{
    listIter *iter;
    listNode *node;

    iter = listGetIterator(list, AL_START_HEAD);    //创建迭代器
    while((node = listNext(iter)) != NULL) {        //迭代整个链表
        if (list->match) {                          //如果设置list结构中的match方法，则用该方法比较
            if (list->match(node->value, key)) {
                listReleaseIterator(iter);          //如果找到，释放迭代器返回node地址
                return node;
            }
        } else {
            if (key == node->value) {
                listReleaseIterator(iter);
                return node;
            }
        }
    }
    listReleaseIterator(iter);      //释放迭代器
    return NULL;
}

/* Return the element at the specified zero-based index
 * where 0 is the head, 1 is the element next to head
 * and so on. Negative integers are used in order to count
 * from the tail, -1 is the last element, -2 the penultimate
 * and so on. If the index is out of range NULL is returned. */
listNode *listIndex(list *list, long index) {   //返回下标为index的节点地址
    listNode *n;

    if (index < 0) {
        index = (-index)-1;         //如果下标为负数，从链表尾部开始
        n = list->tail;
        while(index-- && n) n = n->prev;
    } else {
        n = list->head;             //如果下标为正数，从链表头部开始
        while(index-- && n) n = n->next;
    }
    return n;
}

/* Rotate the list removing the tail node and inserting it to the head. */
void listRotate(list *list) {       //将尾节点插到头结点
    listNode *tail = list->tail;

    if (listLength(list) <= 1) return;  //只有一个节点或空链表直接返回

    /* Detach current tail */
    list->tail = tail->prev;        //取出尾节点，更新list的tail指针
    list->tail->next = NULL;
    /* Move it as head */
    list->head->prev = tail;        //将节点插到表头，更新list的head指针
    tail->prev = NULL;
    tail->next = list->head;
    list->head = tail;
}
