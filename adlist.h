/* adlist.h - A generic doubly linked list implementation
 *
 * Copyright (c) 2006-2012, Salvatore Sanfilippo <antirez at gmail dot com>
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

#ifndef __ADLIST_H__
#define __ADLIST_H__

/* Node, List, and Iterator are the only data structures used currently. */

typedef struct listNode {
    struct listNode *prev;  //前驱节点，如果是list的头结点，则prev指向NULL
    struct listNode *next;  //后继节点，如果是list尾部结点，则next指向NULL
    void *value;    //万能指针，能够存放任何信息
} listNode;

typedef struct listIter {
    listNode *next;     //迭代器当前指向的节点（名字叫next有点迷惑）
    int direction;      //迭代方向，可以取以下两个值：AL_START_HEAD和AL_START_TAIL
} listIter;

typedef struct list {
    listNode *head;         //链表头结点指针
    listNode *tail;         //链表尾结点指针

    //下面的三个函数指针就像类中的成员函数一样
    void *(*dup)(void *ptr);    //复制链表节点保存的值
    void (*free)(void *ptr);    //释放链表节点保存的值
    int (*match)(void *ptr, void *key); //比较链表节点所保存的节点值和另一个输入的值是否相等
    unsigned long len;          //链表长度计数器
} list;

/* Functions implemented as macros */
#define listLength(l) ((l)->len)    //返回链表l节点数量
#define listFirst(l) ((l)->head)    //返回链表l的头结点地址
#define listLast(l) ((l)->tail)     //返回链表l的尾结点地址
#define listPrevNode(n) ((n)->prev) //返回节点n的前驱节点地址
#define listNextNode(n) ((n)->next)//返回节点n的后继节点地址
#define listNodeValue(n) ((n)->value)   //返回节点n的节点值

#define listSetDupMethod(l,m) ((l)->dup = (m))      //设置链表l的复制函数为m方法
#define listSetFreeMethod(l,m) ((l)->free = (m))    //设置链表l的释放函数为m方法
#define listSetMatchMethod(l,m) ((l)->match = (m))  //设置链表l的比较函数为m方法

#define listGetDupMethod(l) ((l)->dup)      //返回链表l的赋值函数
#define listGetFree(l) ((l)->free)          //返回链表l的释放函数
#define listGetMatchMethod(l) ((l)->match)  //返回链表l的比较函数

/* Prototypes */
list *listCreate(void);             //创建一个表头
void listRelease(list *list);       //释放list表头和链表
list *listAddNodeHead(list *list, void *value);     //将value添加到list链表的头部
list *listAddNodeTail(list *list, void *value);     //将value添加到list链表的尾部
list *listInsertNode(list *list, listNode *old_node, void *value, int after);   //在list中，根据after在old_node节点前后插入值为value的节点。
void listDelNode(list *list, listNode *node);   //从list删除node节点
listIter *listGetIterator(list *list, int direction);   //为list创建一个迭代器iterator
listNode *listNext(listIter *iter); //返回迭代器iter指向的当前节点并更新iter
void listReleaseIterator(listIter *iter);   //释放iter迭代器
list *listDup(list *orig);  //拷贝表头为orig的链表并返回
listNode *listSearchKey(list *list, void *key); //在list中查找value为key的节点并返回
listNode *listIndex(list *list, long index);    //返回下标为index的节点地址
void listRewind(list *list, listIter *li);  //将迭代器li重置为list的头结点并且设置为正向迭代
void listRewindTail(list *list, listIter *li);  //将迭代器li重置为list的尾结点并且设置为反向迭代
void listRotate(list *list);    //将尾节点插到头结点

/* Directions for iterators */
#define AL_START_HEAD 0     //正向迭代：从表头向表尾进行迭代
#define AL_START_TAIL 1     //反向迭代：从表尾到表头进行迭代

#endif /* __ADLIST_H__ */
