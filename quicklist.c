/* quicklist.c - A doubly linked list of ziplists
 * 一个ziplists的双向链表
 * Copyright (c) 2014, Matt Stancliff <matt@genges.com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must start the above copyright notice,
 *     this quicklist of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this quicklist of conditions and the following disclaimer in the
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

#include <string.h> /* for memcpy */
#include "quicklist.h"
#include "zmalloc.h"
#include "ziplist.h"
#include "util.h" /* for ll2string */
#include "lzf.h"

#if defined(REDIS_TEST) || defined(REDIS_TEST_VERBOSE)
#include <stdio.h> /* for printf (debug printing), snprintf (genstr) */
#endif

#ifndef REDIS_STATIC
#define REDIS_STATIC static
#endif

/* Optimization levels for size-based filling */
static const size_t optimization_level[] = {4096, 8192, 16384, 32768, 65536};

/* Maximum size in bytes of any multi-element ziplist.
 * Larger values will live in their own isolated ziplists. */
#define SIZE_SAFETY_LIMIT 8192

/* Minimum ziplist size in bytes for attempting compression. */
#define MIN_COMPRESS_BYTES 48

/* Minimum size reduction in bytes to store compressed quicklistNode data.
 * This also prevents us from storing compression if the compression
 * resulted in a larger size than the original data. */
#define MIN_COMPRESS_IMPROVE 8

/* If not verbose testing, remove all debug printing. */
#ifndef REDIS_TEST_VERBOSE
#define D(...)
#else
#define D(...)                                                                 \
    do {                                                                       \
        printf("%s:%s:%d:\t", __FILE__, __FUNCTION__, __LINE__);               \
        printf(__VA_ARGS__);                                                   \
        printf("\n");                                                          \
    } while (0);
#endif

/* Simple way to give quicklistEntry structs default values with one call. */
//初始化quicklistEntry结构
#define initEntry(e)                                                           \
    do {                                                                       \
        (e)->zi = (e)->value = NULL;                                           \
        (e)->longval = -123456789;                                             \
        (e)->quicklist = NULL;                                                 \
        (e)->node = NULL;                                                      \
        (e)->offset = 123456789;                                               \
        (e)->sz = 0;                                                           \
    } while (0)

#if __GNUC__ >= 3
#define likely(x) __builtin_expect(!!(x), 1)    //x为真的可能性大
#define unlikely(x) __builtin_expect(!!(x), 0)  //x为假的可能性大
#else
#define likely(x) (x)
#define unlikely(x) (x)
#endif
/*   __builtin_expect() 是 GCC (version >= 2.96）提供给程序员使用的，目的是将“分支转移”的信息提供给编译器，这样编译器可以对代码进行优化，以减少指令跳转带来的性能下降。
*   __GNUC__ 是gcc编译器编译代码时预定义的一个宏。需要针对gcc编写代码时， 可以使用该宏进行条件编译。
*   __GNUC__ 的值表示gcc的版本。需要针对gcc特定版本编写代码时，也可以使用该宏进行条件编译。
*   __GNUC__ 的类型是“int”，该宏被扩展后， 得到的是整数字面值。可以通过仅预处理，查看宏扩展后的文本。
*/

/* Create a new quicklist.
 * Free with quicklistRelease(). */
quicklist *quicklistCreate(void) {      //创建一个新的quicklist，并初始化成员
    struct quicklist *quicklist;

    quicklist = zmalloc(sizeof(*quicklist));    //分配空间

    //初始化各个成员
    quicklist->head = quicklist->tail = NULL;
    quicklist->len = 0;
    quicklist->count = 0;
    quicklist->compress = 0;                    //默认不压缩
    quicklist->fill = -2;                       //设置默认值，每个ziplist的字节数最大为8kb
    return quicklist;
}

#define COMPRESS_MAX (1 << 16)  //最大压缩程度为2^16，因为compress长度为16位
void quicklistSetCompressDepth(quicklist *quicklist, int compress) {    //设置压缩程度
    if (compress > COMPRESS_MAX) {
        compress = COMPRESS_MAX;
    } else if (compress < 0) {      //小于0也不压缩
        compress = 0;
    }
    quicklist->compress = compress;
}

#define FILL_MAX (1 << 15)      //每个ziplist最多有2^15个entry节点
void quicklistSetFill(quicklist *quicklist, int fill) {         //设置ziplist结构的大小
    if (fill > FILL_MAX) {
        fill = FILL_MAX;
    } else if (fill < -5) { //ziplist字节数最大可以为64kb
        fill = -5;
    }
    quicklist->fill = fill;
}

//设置压缩列表表头的fill和compress成员
void quicklistSetOptions(quicklist *quicklist, int fill, int depth) {
    quicklistSetFill(quicklist, fill);
    quicklistSetCompressDepth(quicklist, depth);
}

/* Create a new quicklist with some default parameters. */
//创建一个quicklist，并且设置默认的参数
quicklist *quicklistNew(int fill, int compress) {
    quicklist *quicklist = quicklistCreate();
    quicklistSetOptions(quicklist, fill, compress);
    return quicklist;
}

//创建一个quicklist节点quicklistNode，并初始化
REDIS_STATIC quicklistNode *quicklistCreateNode(void) {
    quicklistNode *node;
    node = zmalloc(sizeof(*node));
    node->zl = NULL;
    node->count = 0;
    node->sz = 0;
    node->next = node->prev = NULL;
    node->encoding = QUICKLIST_NODE_ENCODING_RAW;           //默认不压缩
    node->container = QUICKLIST_NODE_CONTAINER_ZIPLIST;     //默认使用ziplist结构存储数据
    node->recompress = 0;                                   //设置没压缩的标志
    return node;
}

/* Return cached quicklist count */
//返回ziplist中entry节点的个数
unsigned int quicklistCount(quicklist *ql) { return ql->count; }

/* Free entire quicklist. */
//释放整个quicklist
void quicklistRelease(quicklist *quicklist) {
    unsigned long len;
    quicklistNode *current, *next;

    current = quicklist->head;
    len = quicklist->len;       //记录quicklistNode节点个数
    while (len--) {
        next = current->next;   //备份后继节点地址

        zfree(current->zl);     //释放当前节点的ziplist结构
        quicklist->count -= current->count; //从quicklist的entry计数器减取当前节点中的entry个数

        zfree(current);         //释放当前quicklistNode节点

        quicklist->len--;       //更新quicklistNode节点计数器
        current = next;         //指向后继节点
    }
    zfree(quicklist);           //释放整个quicklist
}

/* Compress the ziplist in 'node' and update encoding details.
 * Returns 1 if ziplist compressed successfully.
 * Returns 0 if compression failed or if ziplist too small to compress. */
//压缩node节点,返回0表示压缩失败，1表示成功
REDIS_STATIC int __quicklistCompressNode(quicklistNode *node) {
#ifdef REDIS_TEST
    node->attempted_compress = 1;           //测试标志
#endif

    /* Don't bother compressing small values */
    if (node->sz < MIN_COMPRESS_BYTES)  //如果ziplist大小小于48字节，压缩失败，返回0
        return 0;

    quicklistLZF *lzf = zmalloc(sizeof(*lzf) + node->sz);   //分配空间

    /* Cancel if compression fails or doesn't compress small enough */
    //调用laf压缩函数进行压缩并返回压缩后的大小
    if (((lzf->sz = lzf_compress(node->zl, node->sz, lzf->compressed,
                                 node->sz)) == 0) ||
        lzf->sz + MIN_COMPRESS_IMPROVE >= node->sz) {
        /* lzf_compress aborts/rejects compression if value not compressable. */
        zfree(lzf); //太小不能压缩或压缩失败则释放空间，返回0
        return 0;
    }
    //压缩成功并分配压缩成功大小的空间
    lzf = zrealloc(lzf, sizeof(*lzf) + lzf->sz);
    zfree(node->zl);    //释放原来的空间
    node->zl = (unsigned char *)lzf;    //设置zl指向quicklistLZF结构
    node->encoding = QUICKLIST_NODE_ENCODING_LZF;   //设置encoding编码为lzf类型
    node->recompress = 0;       //不需要被再次压缩
    return 1;   //压缩成功返回1
}

/* Compress only uncompressed nodes. */
//只压缩之前没有压缩过的节点
#define quicklistCompressNode(_node)                                           \
    do {                                                                       \
        if ((_node) && (_node)->encoding == QUICKLIST_NODE_ENCODING_RAW) {     \
            __quicklistCompressNode((_node));                                  \
        }                                                                      \
    } while (0)

/* Uncompress the ziplist in 'node' and update encoding details.
 * Returns 1 on successful decode, 0 on failure to decode. */
//解压缩node节点的zl，成功返回1 ，失败返回0
REDIS_STATIC int __quicklistDecompressNode(quicklistNode *node) {
#ifdef REDIS_TEST
    node->attempted_compress = 0;
#endif

    void *decompressed = zmalloc(node->sz); //分配存储空间
    quicklistLZF *lzf = (quicklistLZF *)node->zl;

    //调用lzf_decompress进行解压缩
    if (lzf_decompress(lzf->compressed, lzf->sz, decompressed, node->sz) == 0) {
        /* Someone requested decompress, but we can't decompress.  Not good. */
        zfree(decompressed);    //解压缩失败要释放之前分配的空间
        return 0;               //解压缩失败
    }
    zfree(lzf);                 //释放之前的压缩过的空间
    node->zl = decompressed;    //指向解压缩出来的空间
    node->encoding = QUICKLIST_NODE_ENCODING_RAW;   //设置为没压缩的标志，成功返回1
    return 1;
}

/* Decompress only compressed nodes. */
//解压缩节点_node，_node必须是压缩过的节点
#define quicklistDecompressNode(_node)                                         \
    do {                                                                       \
        if ((_node) && (_node)->encoding == QUICKLIST_NODE_ENCODING_LZF) {     \
            __quicklistDecompressNode((_node));                                \
        }                                                                      \
    } while (0)

/* Force node to not be immediately re-compresable */
//标记被解压的_node节点已经被解压，等待被再次压缩
#define quicklistDecompressNodeForUse(_node)                                   \
    do {                                                                       \
        if ((_node) && (_node)->encoding == QUICKLIST_NODE_ENCODING_LZF) {     \
            __quicklistDecompressNode((_node));                                \
            (_node)->recompress = 1;                                           \
        }                                                                      \
    } while (0)

/* Extract the raw LZF data from this quicklistNode.
 * Pointer to LZF data is assigned to '*data'.
 * Return value is the length of compressed LZF data. */
//返回压缩过的ziplist结构的大小，并且将压缩过后的ziplist地址保存到*data中
size_t quicklistGetLzf(const quicklistNode *node, void **data) {
    quicklistLZF *lzf = (quicklistLZF *)node->zl;
    *data = lzf->compressed;
    return lzf->sz;
}

//返回1表示可以压缩，返回0表示不可以压缩
#define quicklistAllowsCompression(_ql) ((_ql)->compress != 0)

/* Force 'quicklist' to meet compression guidelines set by compress depth.
 * The only way to guarantee interior nodes get compressed is to iterate
 * to our "interior" compress depth then compress the next node we find.
 * If compress depth is larger than the entire list, we return immediately. */
//如果node不在压缩程度的范围内，就压缩
REDIS_STATIC void __quicklistCompress(const quicklist *quicklist,
                                      quicklistNode *node) {
    /* If length is less than our compress depth (from both sides),
     * we can't compress anything. */
    //如果quicklist不能压缩或者压缩程度太大直接返回
    //quicklist->compress * 2 就是压缩节点的个数，不能比总结的的个数len还要多
    if (!quicklistAllowsCompression(quicklist) ||
        quicklist->len < (unsigned int)(quicklist->compress * 2))
        return;

#if 0
    /* Optimized cases for small depth counts */
    if (quicklist->compress == 1) {
        quicklistNode *h = quicklist->head, *t = quicklist->tail;
        quicklistDecompressNode(h);
        quicklistDecompressNode(t);
        if (h != node && t != node)
            quicklistCompressNode(node);
        return;
    } else if (quicklist->compress == 2) {
        quicklistNode *h = quicklist->head, *hn = h->next, *hnn = hn->next;
        quicklistNode *t = quicklist->tail, *tp = t->prev, *tpp = tp->prev;
        quicklistDecompressNode(h);
        quicklistDecompressNode(hn);
        quicklistDecompressNode(t);
        quicklistDecompressNode(tp);
        if (h != node && hn != node && t != node && tp != node) {
            quicklistCompressNode(node);
        }
        if (hnn != t) {
            quicklistCompressNode(hnn);
        }
        if (tpp != h) {
            quicklistCompressNode(tpp);
        }
        return;
    }
#endif

    /* Iterate until we reach compress depth for both sides of the list.a
     * Note: because we do length checks at the *top* of this function,
     *       we can skip explicit null checks below. Everything exists. */

    //压缩：压缩中间的节点，所以从两边开始忘中间找节点
    //记录quicklistNode头部节点和尾部节点的地址
    quicklistNode *forward = quicklist->head;
    quicklistNode *reverse = quicklist->tail;
    int depth = 0;
    int in_depth = 0;

    //从两边往中间找，找compress次
    while (depth++ < quicklist->compress) {
        //如果压缩过，则将其解压缩
        quicklistDecompressNode(forward);
        quicklistDecompressNode(reverse);

        //如果找到node节点设置标记
        if (forward == node || reverse == node)
            in_depth = 1;

        //如果两个指针相遇，返回
        if (forward == reverse)
            return;

        //更新指针，指向下一个节点
        forward = forward->next;
        reverse = reverse->prev;
    }

    //如果node不在两边不需要压缩的范围内，则要压缩这个节点
    if (!in_depth)
        quicklistCompressNode(node);

    if (depth > 2) {    //压缩两个指针指向的节点
        /* At this point, forward and reverse are one node beyond depth */
        quicklistCompressNode(forward);
        quicklistCompressNode(reverse);
    }
}

//如果_node可以压缩，则要压缩
#define quicklistCompress(_ql, _node)                                          \
    do {                                                                       \
        /*如果_node节点需要被再次压缩，则压缩_node节点*/                        \
        if ((_node)->recompress)                                               \
            quicklistCompressNode((_node));                                    \
        else                                                                   \
            __quicklistCompress((_ql), (_node));                               \
        /*否则，进行范围压缩*/                                                  \
    } while (0)

/* If we previously used quicklistDecompressNodeForUse(), just recompress. */
//如果之前调用quicklistDecompressNodeForUse()，只需将_node再次压缩
#define quicklistRecompressOnly(_ql, _node)                                    \
    do {                                                                       \
        if ((_node)->recompress)                                               \
            quicklistCompressNode((_node));                                    \
    } while (0)

/* Insert 'new_node' after 'old_node' if 'after' is 1.
 * Insert 'new_node' before 'old_node' if 'after' is 0.
 * Note: 'new_node' is *always* uncompressed, so if we assign it to
 *       head or tail, we do not need to uncompress it. */
//插入节点，如果after为1，则new_node插在old_node后面，如果为0，则插在前面
//插入的new_node总是未压缩的，所以可以插在头部或尾部。
REDIS_STATIC void __quicklistInsertNode(quicklist *quicklist,
                                        quicklistNode *old_node,
                                        quicklistNode *new_node, int after) {
    if (after) {    //插入在后面
        new_node->prev = old_node;                  //new_node的前驱指针指向old_node
        if (old_node) {                             //如果old_node非空
            new_node->next = old_node->next;        //new_node插在old_new的后面
            if (old_node->next)
                old_node->next->prev = new_node;    //old_node的后驱节点的前驱指针变成new_node
            old_node->next = new_node;              //old_node的后驱节点更新为new_node
        }
        if (quicklist->tail == old_node)            //如果old_node节点是尾节点，需要更新尾节点指针的指向
            quicklist->tail = new_node;
    } else {        //插入在前面
        new_node->next = old_node;                  //将old_node插在new_node后面
        if (old_node) {
            new_node->prev = old_node->prev;        //更新new_node的前驱指针为old_node的前驱指针
            if (old_node->prev)
                old_node->prev->next = new_node;    //更新old_node的前驱节点的后继指针为new_node
            old_node->prev = new_node;              //将old_node的前驱指针指向new_node
        }
        if (quicklist->head == old_node)            //如果old_node节点是头节点，需要更新头节点指针的指向
            quicklist->head = new_node;
    }
    /* If this insert creates the only element so far, initialize head/tail. */
    if (quicklist->len == 0) {                      //如果quicklist为空列表
        quicklist->head = quicklist->tail = new_node;//更新头尾节点指针
    }

    if (old_node)
        quicklistCompress(quicklist, old_node);     //如果设置了compress，压缩old_node

    quicklist->len++;                               //更新quicklistNode节点计数器
}

/* Wrappers for node inserting around existing node. */
//将前插法封装封装起来
REDIS_STATIC void _quicklistInsertNodeBefore(quicklist *quicklist,
                                             quicklistNode *old_node,
                                             quicklistNode *new_node) {
    __quicklistInsertNode(quicklist, old_node, new_node, 0);
}

//将后插法封装封装起来
REDIS_STATIC void _quicklistInsertNodeAfter(quicklist *quicklist,
                                            quicklistNode *old_node,
                                            quicklistNode *new_node) {
    __quicklistInsertNode(quicklist, old_node, new_node, 1);
}

//sz是否满足小于fill所要求的最大值
REDIS_STATIC int
_quicklistNodeSizeMeetsOptimizationRequirement(const size_t sz,
                                               const int fill) {
    if (fill >= 0)
        return 0;

    //fill小于0
    size_t offset = (-fill) - 1;    //数组的偏移量

    //static const size_t optimization_level[] = {4096, 8192, 16384, 32768, 65536};
    //(sizeof(optimization_level) / sizeof(*optimization_level) 求出数组的元素个数
    //数组偏移量要小于数组元素
    if (offset < (sizeof(optimization_level) / sizeof(*optimization_level))) {
        if (sz <= optimization_level[offset]) {     //sz小于根据fill算出的等级，返回1
            return 1;
        } else {
            return 0;
        }
    } else {
        return 0;
    }
}

//sz是否超过ziplist所规定的安全界限8192字节，1表示安全，0表示不安全
#define sizeMeetsSafetyLimit(sz) ((sz) <= SIZE_SAFETY_LIMIT)

//node节点中ziplist能否插入entry节点中，根据fill和sz判断
REDIS_STATIC int _quicklistNodeAllowInsert(const quicklistNode *node,
                                           const int fill, const size_t sz) {
    if (unlikely(!node))    //!node为假的可能性大，如果Node为空直接返回
        return 0;

    int ziplist_overhead;
    /* size of previous offset */
    //上一个entry节点的信息
    if (sz < 254)                   //如果ziplist的大小小于254
        ziplist_overhead = 1;       //编码需要1个字节
    else
        ziplist_overhead = 5;       //否则需要5个字节

    /* size of forward offset */
    //当前entry节点的信息
    if (sz < 64)                    //长度小于2^6-1 用1个字节编码
        ziplist_overhead += 1;
    else if (likely(sz < 16384))    //长度小于2^14-1 用2个字节编码
        ziplist_overhead += 2;
    else
        ziplist_overhead += 5;      //长度小于2^32-1 用5个字节编码

    /* new_sz overestimates if 'sz' encodes to an integer type */
    unsigned int new_sz = node->sz + sz + ziplist_overhead;
    if (likely(_quicklistNodeSizeMeetsOptimizationRequirement(new_sz, fill)))   //new_sz符合fill配置，成功
        return 1;
    else if (!sizeMeetsSafetyLimit(new_sz)) //new_sz不满足安全界限，返回0
        return 0;
    else if ((int)node->count < fill)   //如果entry节点个数小于fill所设置的大小(正数情况)，返回1
        return 1;
    else
        return 0;
}

//根据fill判断两个quicklistNode中的ziplist能否合并，返回1可以，否则返回0
REDIS_STATIC int _quicklistNodeAllowMerge(const quicklistNode *a,
                                          const quicklistNode *b,
                                          const int fill) {
    if (!a || !b)   //a和b任意一个为空则返回0
        return 0;

    /* approximate merged ziplist size (- 11 to remove one ziplist
     * header/trailer) */
    //计算合并后的大小，减去以下成员
    //albtyes + zltail_offset + zllength + zlend = 11
    unsigned int merge_sz = a->sz + b->sz - 11;
    if (likely(_quicklistNodeSizeMeetsOptimizationRequirement(merge_sz, fill))) //merge_sz符合fill配置
        return 1;
    else if (!sizeMeetsSafetyLimit(merge_sz))   //merge_sz超过安全大小的界限
        return 0;
    else if ((int)(a->count + b->count) <= fill)    //合并后的entry个数小于fill所限制的个数
        return 1;
    else
        return 0;
}

//更新Node的ziplist大小sz，将zl的zlbytes成员赋值给sz
#define quicklistNodeUpdateSz(node)                                            \
    do {                                                                       \
        (node)->sz = ziplistBlobLen((node)->zl);                               \
    } while (0)     //ziplistBlobLen返回整个 ziplist 占用的内存字节数

/* Add new entry to head node of quicklist.
 *
 * Returns 0 if used existing head.
 * Returns 1 if new head created. */
//push一个entry节点到quicklist的头部
//返回0表示不改变头节点指针，返回1表示节点插入在头部，改变了头结点指针
int quicklistPushHead(quicklist *quicklist, void *value, size_t sz) {
    quicklistNode *orig_head = quicklist->head; //备份头结点地址

    //如果ziplist可以插入entry节点
    if (likely(
            _quicklistNodeAllowInsert(quicklist->head, quicklist->fill, sz))) {
        quicklist->head->zl =
            ziplistPush(quicklist->head->zl, value, sz, ZIPLIST_HEAD);  //将节点push到头部
        quicklistNodeUpdateSz(quicklist->head); //更新quicklistNode记录ziplist大小的sz
    } else {        //如果不能插入entry节点到ziplist
        quicklistNode *node = quicklistCreateNode();    //新创建一个quicklistNode节点

        //将entry节点push到新创建的quicklistNode节点中
        node->zl = ziplistPush(ziplistNew(), value, sz, ZIPLIST_HEAD);

        quicklistNodeUpdateSz(node);    //更新ziplist的大小sz
        _quicklistInsertNodeBefore(quicklist, quicklist->head, node);   //将新创建的节点插入到头节点前
    }
    quicklist->count++;                     //更新quicklistNode计数器
    quicklist->head->count++;               //更新entry计数器
    return (orig_head != quicklist->head);  //如果改变头节点指针则返回1，否则返回0
}

/* Add new entry to tail node of quicklist.
 *
 * Returns 0 if used existing tail.
 * Returns 1 if new tail created. */
//push一个entry节点到quicklist的尾节点中，如果不能push则新创建一个quicklistNode节点
//返回0表示不改变尾节点指针，返回1表示节点插入在尾部，改变了尾结点指针
int quicklistPushTail(quicklist *quicklist, void *value, size_t sz) {
    quicklistNode *orig_tail = quicklist->tail;

    //如果ziplist可以插入entry节点
    if (likely(
            _quicklistNodeAllowInsert(quicklist->tail, quicklist->fill, sz))) {
        quicklist->tail->zl =
            ziplistPush(quicklist->tail->zl, value, sz, ZIPLIST_TAIL);  //将节点push到尾部
        quicklistNodeUpdateSz(quicklist->tail); //更新quicklistNode记录ziplist大小的sz
    } else {
        quicklistNode *node = quicklistCreateNode();        //新创建一个quicklistNode节点

        //将entry节点push到新创建的quicklistNode节点中
        node->zl = ziplistPush(ziplistNew(), value, sz, ZIPLIST_TAIL);

        quicklistNodeUpdateSz(node);        //更新ziplist的大小sz
        _quicklistInsertNodeAfter(quicklist, quicklist->tail, node);//将新创建的节点插入到尾节点后
    }
    quicklist->count++;             //更新quicklistNode计数器
    quicklist->tail->count++;       //更新entry计数器
    return (orig_tail != quicklist->tail);  //如果改变尾节点指针则返回1，否则返回0
}

/* Create new node consisting of a pre-formed ziplist.
 * Used for loading RDBs where entire ziplists have been stored
 * to be retrieved later. */
//追加quicklist一个quicklist节点
void quicklistAppendZiplist(quicklist *quicklist, unsigned char *zl) {
    quicklistNode *node = quicklistCreateNode();    //创建一个新的quicklistNode节点

    node->zl = zl;                      //设置zl指向ziplist
    node->count = ziplistLen(node->zl); //设置entry节点计数器
    node->sz = ziplistBlobLen(zl);      //设置ziplist的大小

    _quicklistInsertNodeAfter(quicklist, quicklist->tail, node);    //将node节点追加到quicklist的尾部
    quicklist->count += node->count;    //更新quicklist的entry节点计数器
}

/* Append all values of ziplist 'zl' individually into 'quicklist'.
 *
 * This allows us to restore old RDB ziplists into new quicklists
 * with smaller ziplist sizes than the saved RDB ziplist.
 *
 * Returns 'quicklist' argument. Frees passed-in ziplist 'zl' */
//在quicklist末尾追加一个entry
quicklist *quicklistAppendValuesFromZiplist(quicklist *quicklist,
                                            unsigned char *zl) {
    unsigned char *value;
    unsigned int sz;
    long long longval;
    char longstr[32] = {0};

    unsigned char *p = ziplistIndex(zl, 0); //获得ziplist第一个节点的地址
    while (ziplistGet(p, &value, &sz, &longval)) {  //将p指向的entry节点信息，保存到value或sz或longval中
        if (!value) {   //如果字符串为空
            /* Write the longval as a string so we can re-add it */
            sz = ll2string(longstr, sizeof(longstr), longval);  //将longlong类型的整数转换为字符串
            value = (unsigned char *)longstr;                   //保存到value中
        }
        quicklistPushTail(quicklist, value, sz);                //追加到quicklist的尾部
        p = ziplistNext(zl, p); //更新p指向下一个entry节点
    }
    zfree(zl);              //释放空间
    return quicklist;
}

/* Create new (potentially multi-node) quicklist from a single existing ziplist.
 *
 * Returns new quicklist.  Frees passed-in ziplist 'zl'. */
//创建一个quicklist，并将zl追加到其中
quicklist *quicklistCreateFromZiplist(int fill, int compress,
                                      unsigned char *zl) {
    return quicklistAppendValuesFromZiplist(quicklistNew(fill, compress), zl);
}

//删除空quicklistNode节点
#define quicklistDeleteIfEmpty(ql, n)                                          \
    do {                                                                       \
        if ((n)->count == 0) {                                                 \
            __quicklistDelNode((ql), (n));                                     \
            (n) = NULL;                                                        \
        }                                                                      \
    } while (0)

//删除quicklistNode节点node
REDIS_STATIC void __quicklistDelNode(quicklist *quicklist,
                                     quicklistNode *node) {
    if (node->next) //如果后继节点指针不为空，则将后继节点的前驱指针跳过当前节点
        node->next->prev = node->prev;
    if (node->prev) //如果前驱节点指针不为空，则将前驱节点的后继指针跳过当前节点
        node->prev->next = node->next;

    if (node == quicklist->tail) {  //如果被删除的节点是尾节点，则要更新尾节点指针
        quicklist->tail = node->prev;
    }

    if (node == quicklist->head) {  //如果被删除的节点是头节点，则要更新头节点指针
        quicklist->head = node->next;
    }

    /* If we deleted a node within our compress depth, we
     * now have compressed nodes needing to be decompressed. */
    __quicklistCompress(quicklist, NULL);   //如果删除的节点在压缩的范围内，必须更新quicklist的压缩情况

    quicklist->count -= node->count;    //更新entry计数器

    zfree(node->zl);                    //释放空间
    zfree(node);
    quicklist->len--;                   //更新quicklistNode计数器
}

/* Delete one entry from list given the node for the entry and a pointer
 * to the entry in the node.
 *
 * Note: quicklistDelIndex() *requires* uncompressed nodes because you
 *       already had to get *p from an uncompressed node somewhere.
 *
 * Returns 1 if the entire node was deleted, 0 if node still exists.
 * Also updates in/out param 'p' with the next offset in the ziplist. */
//删除ziplist中的entry，如果entry是最后一个，则删除当前quicklistNode节点，返回1，没有删除当前节点则返回0
REDIS_STATIC int quicklistDelIndex(quicklist *quicklist, quicklistNode *node,
                                   unsigned char **p) {
    int gone = 0;

    node->zl = ziplistDelete(node->zl, p);  //删除p指向的entry
    node->count--;                          //更新计数器
    if (node->count == 0) {                 //如果entry为0
        gone = 1;
        __quicklistDelNode(quicklist, node);//删除当前的quicklistNode节点
    } else {
        quicklistNodeUpdateSz(node);        //否则更新node中ziplist大小sz
    }
    quicklist->count--;                     //更新quicklist表头中的quicklistNode节点计数器
    /* If we deleted the node, the original node is no longer valid */
    return gone ? 1 : 0;
}

/* Delete one element represented by 'entry'
 *
 * 'entry' stores enough metadata to delete the proper position in
 * the correct ziplist in the correct quicklist node. */
//删除一个entry通过quicklistEntry结构的形式
void quicklistDelEntry(quicklistIter *iter, quicklistEntry *entry) {

    //获得entry所属节点的前驱节点和后继节点的地址
    quicklistNode *prev = entry->node->prev;
    quicklistNode *next = entry->node->next;

    //删除entry所属的节点
    int deleted_node = quicklistDelIndex((quicklist *)entry->quicklist,
                                         entry->node, &entry->zi);

    /* after delete, the zi is now invalid for any future usage. */
    iter->zi = NULL;    //更新迭代器的zi

    /* If current node is deleted, we must update iterator node and offset. */
    //如果当前的quicklistNode节点被删除，需要更新迭代器的正在指向的节点的地址
    if (deleted_node) {
        if (iter->direction == AL_START_HEAD) { //如果是正向迭代则指向next，offset为0
            iter->current = next;
            iter->offset = 0;
        } else if (iter->direction == AL_START_TAIL) {  //如果是反向迭代则指向prev，offset为-1
            iter->current = prev;
            iter->offset = -1;
        }
    }
    /* else if (!deleted_node), no changes needed.
     * we already reset iter->zi above, and the existing iter->offset
     * doesn't move again because:
     *   - [1, 2, 3] => delete offset 1 => [1, 3]: next element still offset 1
     *   - [1, 2, 3] => delete offset 0 => [2, 3]: next element still offset 0
     *  if we deleted the last element at offet N and now
     *  length of this ziplist is N-1, the next call into
     *  quicklistNext() will jump to the next node. */
}

/* Replace quicklist entry at offset 'index' by 'data' with length 'sz'.
 *
 * Returns 1 if replace happened.
 * Returns 0 if replace failed and no changes happened. */
//下标为index的entry被data替换
int quicklistReplaceAtIndex(quicklist *quicklist, long index, void *data,
                            int sz) {
    quicklistEntry entry;
    if (likely(quicklistIndex(quicklist, index, &entry))) { //找到下标为index的entry
        /* quicklistIndex provides an uncompressed node */
        entry.node->zl = ziplistDelete(entry.node->zl, &entry.zi);  //删除被替换的entry
        entry.node->zl = ziplistInsert(entry.node->zl, entry.zi, data, sz); //在删除的地方插入一个entry
        quicklistNodeUpdateSz(entry.node);          //更新quicklistNode节点的信息
        quicklistCompress(quicklist, entry.node);   //按需压缩
        return 1;
    } else {
        return 0;
    }
}

/* Given two nodes, try to merge their ziplists.
 *
 * This helps us not have a quicklist with 3 element ziplists if
 * our fill factor can handle much higher levels.
 *
 * Note: 'a' must be to the LEFT of 'b'.
 *
 * After calling this function, both 'a' and 'b' should be considered
 * unusable.  The return value from this function must be used
 * instead of re-using any of the quicklistNode input arguments.
 *
 * Returns the input node picked to merge against or NULL if
 * merging was not possible. */
//合并a和b的ziplist，返回空说明合并失败或没有发生改变
REDIS_STATIC quicklistNode *_quicklistZiplistMerge(quicklist *quicklist,
                                                   quicklistNode *a,
                                                   quicklistNode *b) {
    D("Requested merge (a,b) (%u, %u)", a->count, b->count);

    //将a和b先解压
    quicklistDecompressNode(a);
    quicklistDecompressNode(b);
    //合并a和b的ziplist
    if ((ziplistMerge(&a->zl, &b->zl))) {
        /* We merged ziplists! Now remove the unused quicklistNode. */
        quicklistNode *keep = NULL, *nokeep = NULL;
        if (!a->zl) {   //a的zl为空，说明a发生改变
            nokeep = a;
            keep = b;
        } else if (!b->zl) {    //b的zl为空，说明a发生改变
            nokeep = b;
            keep = a;
        }
        keep->count = ziplistLen(keep->zl); //合并之后的entry元素个数
        quicklistNodeUpdateSz(keep);        //更新keep的ziplist大小sz

        nokeep->count = 0;                  //a被合并到b中，因此a中的节点数为0
        __quicklistDelNode(quicklist, nokeep);  //删除被合并的节点
        quicklistCompress(quicklist, keep);     //按需压缩
        return keep;
    } else {
        /* else, the merge returned NULL and nothing changed. */
        return NULL;
    }
}

/* Attempt to merge ziplists within two nodes on either side of 'center'.
 *
 * We attempt to merge:
 *   - (center->prev->prev, center->prev)
 *   - (center->next, center->next->next)
 *   - (center->prev, center)
 *   - (center, center->next)
 */
//合并以center为中心的左右两个节点，最多将5个节点合并为1个
REDIS_STATIC void _quicklistMergeNodes(quicklist *quicklist,
                                       quicklistNode *center) {
    int fill = quicklist->fill; //备份配置的ziplist的大小
    quicklistNode *prev, *prev_prev, *next, *next_next, *target;
    prev = prev_prev = next = next_next = target = NULL;

    //备份center节点的prev节点和prev的prev节点
    if (center->prev) {
        prev = center->prev;
        if (center->prev->prev)
            prev_prev = center->prev->prev;
    }

    //备份center节点的next节点和next的next节点
    if (center->next) {
        next = center->next;
        if (center->next->next)
            next_next = center->next->next;
    }

    /* Try to merge prev_prev and prev */
    //如果允许合并，合并prev_prev and prev节点
    if (_quicklistNodeAllowMerge(prev, prev_prev, fill)) {
        _quicklistZiplistMerge(quicklist, prev_prev, prev);
        prev_prev = prev = NULL; /* they could have moved, invalidate them. */
    }

    /* Try to merge next and next_next */
    //如果允许合并，合并next and next_next节点
    if (_quicklistNodeAllowMerge(next, next_next, fill)) {
        _quicklistZiplistMerge(quicklist, next, next_next);
        next = next_next = NULL; /* they could have moved, invalidate them. */
    }

    /* Try to merge center node and previous node */
    //如果允许合并，合并center和prev节点
    if (_quicklistNodeAllowMerge(center, center->prev, fill)) {
        target = _quicklistZiplistMerge(quicklist, center->prev, center);
        center = NULL; /* center could have been deleted, invalidate it. */
    } else {
        /* else, we didn't merge here, but target needs to be valid below. */
        target = center;    //防止野指针
    }

    /* Use result of center merge (or original) to merge with next node. */
    //如果允许合并，合并target和next节点
    if (_quicklistNodeAllowMerge(target, target->next, fill)) {
        _quicklistZiplistMerge(quicklist, target, target->next);
    }
}

/* Split 'node' into two parts, parameterized by 'offset' and 'after'.
 *
 * The 'after' argument controls which quicklistNode gets returned.
 * If 'after'==1, returned node has elements after 'offset'.
 *                input node keeps elements up to 'offset', including 'offset'.
 * If 'after'==0, returned node has elements up to 'offset', including 'offset'.
 *                input node keeps elements after 'offset'.
 *
 * If 'after'==1, returned node will have elements _after_ 'offset'.
 *                The returned node will have elements [OFFSET+1, END].
 *                The input node keeps elements [0, OFFSET].
 *
 * If 'after'==0, returned node will keep elements up to and including 'offset'.
 *                The returned node will have elements [0, OFFSET].
 *                The input node keeps elements [OFFSET+1, END].
 *
 * The input node keeps all elements not taken by the returned node.
 *
 * Returns newly created node or NULL if split not possible. */
//通过offset和after将node分割成两个部分，返回分离出的节点地址
REDIS_STATIC quicklistNode *_quicklistSplitNode(quicklistNode *node, int offset,
                                                int after) {
    size_t zl_sz = node->sz;

    quicklistNode *new_node = quicklistCreateNode();    //创建一个新quicklistNode节点
    new_node->zl = zmalloc(zl_sz);  //为ziplist分配空间

    /* Copy original ziplist so we can split it */
    memcpy(new_node->zl, node->zl, zl_sz);  //将ziplist拷贝1份到new_node中

    /* -1 here means "continue deleting until the list ends" */
    //-1 表示ziplist列表的最后一个entry节点的下标
    //如果after为1，则表示：将node分割成new+orig的前后顺序
    //如果after为0，则表示：将node分割成orig+new的前后顺序
    //计算删除的范围
    int orig_start = after ? offset + 1 : 0;
    int orig_extent = after ? -1 : offset;
    int new_start = after ? 0 : offset;
    int new_extent = after ? offset + 1 : -1;

    D("After %d (%d); ranges: [%d, %d], [%d, %d]", after, offset, orig_start,
      orig_extent, new_start, new_extent);

    node->zl = ziplistDeleteRange(node->zl, orig_start, orig_extent);   //删除原来节点的orig部分
    node->count = ziplistLen(node->zl);                                 //更新entry节点计数器
    quicklistNodeUpdateSz(node);                                        //更新ziplist的大小sz

    new_node->zl = ziplistDeleteRange(new_node->zl, new_start, new_extent); //删除新节点的new部分
    new_node->count = ziplistLen(new_node->zl);                             //更新entry节点计数器
    quicklistNodeUpdateSz(new_node);                                        //更新ziplist的大小sz

    D("After split lengths: orig (%d), new (%d)", node->count, new_node->count);
    return new_node;
}

/* Insert a new entry before or after existing entry 'entry'.
 *
 * If after==1, the new value is inserted after 'entry', otherwise
 * the new value is inserted before 'entry'. */
//如果after为1，在已存在的entry后插入一个entry，否则在前面插入
REDIS_STATIC void _quicklistInsert(quicklist *quicklist, quicklistEntry *entry,
                                   void *value, const size_t sz, int after) {
    int full = 0, at_tail = 0, at_head = 0, full_next = 0, full_prev = 0;
    int fill = quicklist->fill;
    quicklistNode *node = entry->node;
    quicklistNode *new_node = NULL;

    if (!node) {    //如果entry为没有所属的quicklistNode节点，需要新创建
        /* we have no reference node, so let's create only node in the list */
        D("No node given!");
        new_node = quicklistCreateNode();   //创建一个节点
        //将entry值push到new_node新节点的ziplist中
        new_node->zl = ziplistPush(ziplistNew(), value, sz, ZIPLIST_HEAD);
        //将新的quicklistNode节点插入到quicklist中
        __quicklistInsertNode(quicklist, NULL, new_node, after);
        //更新entry计数器
        new_node->count++;
        quicklist->count++;
        return;
    }

    /* Populate accounting flags for easier boolean checks later */
    //如果node不能插入entry
    if (!_quicklistNodeAllowInsert(node, fill, sz)) {
        D("Current node is full with count %d with requested fill %lu",
          node->count, fill);
        full = 1;   //设置full的标志
    }

    //如果是后插入且当前entry为尾部的entry
    if (after && (entry->offset == node->count)) {
        D("At Tail of current ziplist");
        at_tail = 1;    //设置在尾部at_tail标示
        //如果node的后继节点不能插入
        if (!_quicklistNodeAllowInsert(node->next, fill, sz)) {
            D("Next node is full too.");
            full_next = 1;  //设置标示
        }
    }

    //如果是前插入且当前entry为头部的entry
    if (!after && (entry->offset == 0)) {
        D("At Head");
        at_head = 1;    //设置at_head表示
        if (!_quicklistNodeAllowInsert(node->prev, fill, sz)) { //如果node的前驱节点不能插入
            D("Prev node is full too.");
            full_prev = 1;      //设置标示
        }
    }

    /* Now determine where and how to insert the new element */
    //如果node不满，且是后插入
    if (!full && after) {
        D("Not full, inserting after current position.");
        quicklistDecompressNodeForUse(node);    //将node临时解压
        unsigned char *next = ziplistNext(node->zl, entry->zi); //返回下一个entry的地址
        if (next == NULL) { //如果next为空，则直接在尾部push一个entry
            node->zl = ziplistPush(node->zl, value, sz, ZIPLIST_TAIL);
        } else {            //否则，后插入一个entry
            node->zl = ziplistInsert(node->zl, next, value, sz);
        }
        node->count++;  //更新entry计数器
        quicklistNodeUpdateSz(node);    //更新ziplist的大小sz
        quicklistRecompressOnly(quicklist, node);   //将临时解压的重压缩

    //如果node不满且是前插
    } else if (!full && !after) {
        D("Not full, inserting before current position.");
        quicklistDecompressNodeForUse(node);    //将node临时解压
        node->zl = ziplistInsert(node->zl, entry->zi, value, sz);   //前插入
        node->count++;  //更新entry计数器
        quicklistNodeUpdateSz(node);     //更新ziplist的大小sz
        quicklistRecompressOnly(quicklist, node);   //将临时解压的重压缩

    //当前node满了，且当前已存在的entry是尾节点，node的后继节点指针不为空，且node的后驱节点能插入
    //本来要插入当前node中，但是当前的node满了，所以插在next节点的头部
    } else if (full && at_tail && node->next && !full_next && after) {
        /* If we are: at tail, next has free space, and inserting after:
         *   - insert entry at head of next node. */
        D("Full and tail, but next isn't full; inserting next node head");
        new_node = node->next;  //new_node指向node的后继节点
        quicklistDecompressNodeForUse(new_node);    //将node临时解压
        new_node->zl = ziplistPush(new_node->zl, value, sz, ZIPLIST_HEAD);  //在new_node头部push一个entry
        new_node->count++;  //更新entry计数器
        quicklistNodeUpdateSz(new_node);    //更新ziplist的大小sz
        quicklistRecompressOnly(quicklist, new_node);   //将临时解压的重压缩

    //当前node满了，且当前已存在的entry是头节点，node的前驱节点指针不为空，且前驱节点可以插入
    //因此插在前驱节点的尾部
    } else if (full && at_head && node->prev && !full_prev && !after) {
        /* If we are: at head, previous has free space, and inserting before:
         *   - insert entry at tail of previous node. */
        D("Full and head, but prev isn't full, inserting prev node tail");
        new_node = node->prev;  //new_node指向node的后继节点
        quicklistDecompressNodeForUse(new_node);    //将node临时解压
        new_node->zl = ziplistPush(new_node->zl, value, sz, ZIPLIST_TAIL);//在new_node尾部push一个entry
        new_node->count++;  //更新entry计数器
        quicklistNodeUpdateSz(new_node);    //更新ziplist的大小sz
        quicklistRecompressOnly(quicklist, new_node);   //将临时解压的重压缩

    //当前node满了
    //要么已存在的entry是尾节点，且后继节点指针不为空，且后继节点不可以插入，且要后插
    //要么已存在的entry为头节点，且前驱节点指针不为空，且前驱节点不可以插入，且要前插
    } else if (full && ((at_tail && node->next && full_next && after) ||
                        (at_head && node->prev && full_prev && !after))) {
        /* If we are: full, and our prev/next is full, then:
         *   - create new node and attach to quicklist */
        D("\tprovisioning new node...");
        new_node = quicklistCreateNode();   //创建一个节点
        new_node->zl = ziplistPush(ziplistNew(), value, sz, ZIPLIST_HEAD);  //将entrypush到new_node的头部
        new_node->count++;  //更新entry计数器
        quicklistNodeUpdateSz(new_node);        //更新ziplist的大小sz
        __quicklistInsertNode(quicklist, node, new_node, after);    //将new_node插入在当前node的后面

    //当前node满了，且要将entry插入在中间的任意地方，需要将node分割
    } else if (full) {
        /* else, node is full we need to split it. */
        /* covers both after and !after cases */
        D("\tsplitting node...");
        quicklistDecompressNodeForUse(node);    //将node临时解压
        new_node = _quicklistSplitNode(node, entry->offset, after);//分割node成两块
        new_node->zl = ziplistPush(new_node->zl, value, sz,
                                   after ? ZIPLIST_HEAD : ZIPLIST_TAIL);//将entry push到new_node中
        new_node->count++;  //更新entry计数器
        quicklistNodeUpdateSz(new_node);        //更新ziplist的大小sz
        __quicklistInsertNode(quicklist, node, new_node, after);    //将new_node插入进去
        _quicklistMergeNodes(quicklist, node);  //左右能合并的合并
    }

    quicklist->count++;     //更新总的entry计数器
}

//将entry前插入封装起来
void quicklistInsertBefore(quicklist *quicklist, quicklistEntry *entry,
                           void *value, const size_t sz) {
    _quicklistInsert(quicklist, entry, value, sz, 0);
}

//将entry后插入封装起来
void quicklistInsertAfter(quicklist *quicklist, quicklistEntry *entry,
                          void *value, const size_t sz) {
    _quicklistInsert(quicklist, entry, value, sz, 1);
}

/* Delete a range of elements from the quicklist.
 *
 * elements may span across multiple quicklistNodes, so we
 * have to be careful about tracking where we start and end.
 *
 * Returns 1 if entries were deleted, 0 if nothing was deleted. */
//删除一个范围内的entry节点，返回1表示全部被删除，返回0表示什么都没有删除
int quicklistDelRange(quicklist *quicklist, const long start,
                      const long count) {
    if (count <= 0)
        return 0;

    //记录删除的个数
    unsigned long extent = count; /* range is inclusive of start position */

    //如果start是正数，正向删除，且删除的个数大于总节点数
    if (start >= 0 && extent > (quicklist->count - start)) {
        /* if requesting delete more elements than exist, limit to list size. */
        extent = quicklist->count - start;  //将删除的个数限制到count范围内

    //start是负数时，反向删除，如果删除的数目大于-start，则删除到末尾
    } else if (start < 0 && extent > (unsigned long)(-start)) {
        /* else, if at negative offset, limit max size to rest of list. */
        extent = -start; /* c.f. LREM -29 29; just delete until end. */
    }

    quicklistEntry entry;
    if (!quicklistIndex(quicklist, start, &entry))  //将quicklistNode节点信息读取到entry中
        return 0;

    D("Quicklist delete request for start %ld, count %ld, extent: %ld", start,
      count, extent);
    quicklistNode *node = entry.node;   //获取当前quicklistNode节点地址

    /* iterate over next nodes until everything is deleted. */
    //遍历删除的entry个数
    while (extent) {
        quicklistNode *next = node->next;   //备份下一节点的信息

        unsigned long del;
        int delete_entire_node = 0;
        //如果当前偏移量为0，且要删除的个数大于entry的个数，则删除全部entry节点
        if (entry.offset == 0 && extent >= node->count) {
            /* If we are deleting more than the count of this node, we
             * can just delete the entire node without ziplist math. */
            delete_entire_node = 1;     //全部删除的标识
            del = node->count;          //更新删除的节点数

        //如果是从中间删除到尾节点
        } else if (entry.offset >= 0 && extent >= node->count) {
            /* If deleting more nodes after this one, calculate delete based
             * on size of current node. */
            del = node->count - entry.offset;   //更新删除的节点数

        //如果偏移量为负数，则从尾节点向前删除offset个节点
        } else if (entry.offset < 0) {
            /* If offset is negative, we are in the first run of this loop
             * and we are deleting the entire range
             * from this start offset to end of list.  Since the Negative
             * offset is the number of elements until the tail of the list,
             * just use it directly as the deletion count. */
            del = -entry.offset;

            /* If the positive offset is greater than the remaining extent,
             * we only delete the remaining extent, not the entire offset.
             */
            if (del > extent)   //最多删除count个entry
                del = extent;
        //在中间删除一部分
        } else {
            /* else, we are deleting less than the extent of this node, so
             * use extent directly. */
            del = extent;   //一般情况，
        }

        D("[%ld]: asking to del: %ld because offset: %d; (ENTIRE NODE: %d), "
          "node count: %u",
          extent, del, entry.offset, delete_entire_node, node->count);

        if (delete_entire_node) {   //删除整个ziplist标识为真，则删除整个quicklistNode
            __quicklistDelNode(quicklist, node);
        } else {
            quicklistDecompressNodeForUse(node);    //临时解压送node节点
            node->zl = ziplistDeleteRange(node->zl, entry.offset, del); //删除计算出的范围内的节点
            quicklistNodeUpdateSz(node);            //更新ziplist的打下sz
            node->count -= del;                     //更新quicklistNode的entry节点计数器
            quicklist->count -= del;                //更新quicklist表头的总的entry计数器
            quicklistDeleteIfEmpty(quicklist, node);//如果ziplist为空，则删除quicklistNode节点
            if (node)                               //如果当前节点不为空，则要重压缩
                quicklistRecompressOnly(quicklist, node);
        }

        extent -= del;  //更新删除的计数器

        node = next;    //指向下一个节点

        entry.offset = 0;   //重置当前quicklistEntry的offset，以便下次读取
    }
    return 1;
}

/* Passthrough to ziplistCompare() */
//将ziplist的比较两个ziplist的函数封装成quicklistCompare
int quicklistCompare(unsigned char *p1, unsigned char *p2, int p2_len) {
    return ziplistCompare(p1, p2, p2_len);
}

/* Returns a quicklist iterator 'iter'. After the initialization every
 * call to quicklistNext() will return the next element of the quicklist. */
//返回当前节点的迭代器的地址
quicklistIter *quicklistGetIterator(const quicklist *quicklist, int direction) {
    quicklistIter *iter;

    iter = zmalloc(sizeof(*iter));  //分配空间

    if (direction == AL_START_HEAD) {   //如果是前向迭代
        iter->current = quicklist->head;    //设置current指向quicklist的头节点
        iter->offset = 0;
    } else if (direction == AL_START_TAIL) {    //如果是反向迭代
        iter->current = quicklist->tail;    //设置current指向quicklist的尾节点
        iter->offset = -1;                  //-1代表反向迭代时尾节点的下标
    }

    //设置成员的初始值
    iter->direction = direction;
    iter->quicklist = quicklist;

    iter->zi = NULL;

    return iter;
}

/* Initialize an iterator at a specific offset 'idx' and make the iterator
 * return nodes in 'direction' direction. */
//将迭代器和一个quicklistNode结合
quicklistIter *quicklistGetIteratorAtIdx(const quicklist *quicklist,
                                         const int direction,
                                         const long long idx) {
    quicklistEntry entry;

    //将下标为idx的quicklistNode节点信息读取到quicklistEntry结构中
    if (quicklistIndex(quicklist, idx, &entry)) {
        quicklistIter *base = quicklistGetIterator(quicklist, direction);   //生成一个迭代器
        base->zi = NULL;
        base->current = entry.node;     //结合entry描述的quicklistNode节点
        base->offset = entry.offset;    //设置偏移量
        return base;
    } else {
        return NULL;
    }
}

/* Release iterator.
 * If we still have a valid current node, then re-encode current node. */
//释放迭代器
void quicklistReleaseIterator(quicklistIter *iter) {
    if (iter->current)  //如果该迭代器已经和一个quicklistNode结合
        quicklistCompress(iter->quicklist, iter->current);  //按需压缩entry节点

    zfree(iter);
}

/* Get next element in iterator.
 *
 * Note: You must NOT insert into the list while iterating over it.
 * You *may* delete from the list while iterating using the
 * quicklistDelEntry() function.
 * If you insert into the quicklist while iterating, you should
 * re-create the iterator after your addition.
 *
 * iter = quicklistGetIterator(quicklist,<direction>);
 * quicklistEntry entry;
 * while (quicklistNext(iter, &entry)) {
 *     if (entry.value)
 *          [[ use entry.value with entry.sz ]]
 *     else
 *          [[ use entry.longval ]]
 * }
 *
 * Populates 'entry' with values for this iteration.
 * Returns 0 when iteration is complete or if iteration not possible.
 * If return value is 0, the contents of 'entry' are not valid.
 */
//将迭代器当前指向的节点的信息读到quicklistEntry结构中，并且指向下一个节点
int quicklistNext(quicklistIter *iter, quicklistEntry *entry) {
    initEntry(entry);   //初始化

    if (!iter) {
        D("Returning because no iter!");
        return 0;
    }

    entry->quicklist = iter->quicklist; //记录迭代器所属的quicklist
    entry->node = iter->current;        //记录迭代器所属的quicklistNode

    if (!iter->current) {
        D("Returning because current node is NULL")
        return 0;
    }

    //函数指针，返回值和两个参数都是unsigned char *类型
    unsigned char *(*nextFn)(unsigned char *, unsigned char *) = NULL;
    int offset_update = 0;

    //如果迭代器的zi指针指向的ziplist为空，则要根据offset设置
    if (!iter->zi) {
        /* If !zi, use current index. */
        quicklistDecompressNodeForUse(iter->current);   //临时解压缩
        iter->zi = ziplistIndex(iter->current->zl, iter->offset);   //根据ziplist中偏移量，这只zi指向
    } else {
        /* else, use existing iterator offset and get prev/next as necessary. */
        if (iter->direction == AL_START_HEAD) { //如果是正向迭代，则设置指向next的函数指针
            nextFn = ziplistNext;
            offset_update = 1;                          //向后走1个，记录该值
        } else if (iter->direction == AL_START_TAIL) {//如果是反向迭代，则设置指向prev的函数指针
            nextFn = ziplistPrev;
            offset_update = -1;                         //向前走1个，记录该值
        }
        iter->zi = nextFn(iter->current->zl, iter->zi); //更新zi指向的ziplist中的节点
        iter->offset += offset_update;                  //更新偏移量
    }

    //更新quicklistEntry结构中的信息
    entry->zi = iter->zi;
    entry->offset = iter->offset;

    //如果迭代到的下一个entry节点不为空
    if (iter->zi) {
        /* Populate value from existing ziplist position */
        //将当前entry节点的信息读到quicklistEntry中
        ziplistGet(entry->zi, &entry->value, &entry->sz, &entry->longval);
        return 1;
    } else {    //如果已经迭代完了ziplist
        /* We ran out of ziplist entries.
         * Pick next node, update offset, then re-run retrieval. */
        quicklistCompress(iter->quicklist, iter->current);  //按需压缩
        if (iter->direction == AL_START_HEAD) { //如果正向迭代，将迭代器指向下一个节点
            /* Forward traversal */
            D("Jumping to start of next node");
            iter->current = iter->current->next;
            iter->offset = 0;
        } else if (iter->direction == AL_START_TAIL) {//如果反向迭代，将迭代器指向上一个节点
            /* Reverse traversal */
            D("Jumping to end of previous node");
            iter->current = iter->current->prev;
            iter->offset = -1;
        }
        iter->zi = NULL;
        return quicklistNext(iter, entry);
    }
}

/* Duplicate the quicklist.
 * On success a copy of the original quicklist is returned.
 *
 * The original quicklist both on success or error is never modified.
 *
 * Returns newly allocated quicklist. */
//复制一个quicklist，并返回
quicklist *quicklistDup(quicklist *orig) {
    quicklist *copy;

    copy = quicklistNew(orig->fill, orig->compress);    //新创建一个quicklist

    //遍历orig的quicklistNode节点
    for (quicklistNode *current = orig->head; current;
         current = current->next) {
        quicklistNode *node = quicklistCreateNode();

        if (node->encoding == QUICKLIST_NODE_ENCODING_LZF) {    //如果已经压缩，则创建quicklistLZF结构
            //复制orig中的quicklistLZF到新创建的quicklistLZF中
            quicklistLZF *lzf = (quicklistLZF *)node->zl;
            size_t lzf_sz = sizeof(*lzf) + lzf->sz;
            node->zl = zmalloc(lzf_sz);
            memcpy(node->zl, current->zl, lzf_sz);
        } else if (node->encoding == QUICKLIST_NODE_ENCODING_RAW) { //如果没有经过压缩，则直接复制
            node->zl = zmalloc(current->sz);
            memcpy(node->zl, current->zl, current->sz);
        }

        //更新当前quicklistNode节点的成员信息到新节点node中
        node->count = current->count;
        copy->count += node->count;
        node->sz = current->sz;
        node->encoding = current->encoding;

        //将新创建的node尾插在新quicklist中
        _quicklistInsertNodeAfter(copy, copy->tail, node);
    }

    /* copy->count must equal orig->count here */
    return copy;
}

/* Populate 'entry' with the element at the specified zero-based index
 * where 0 is the head, 1 is the element next to head
 * and so on. Negative integers are used in order to count
 * from the tail, -1 is the last element, -2 the penultimate
 * and so on. If the index is out of range 0 is returned.
 *
 * Returns 1 if element found
 * Returns 0 if element not found */
//查找下标为idx的entry，返回1 表示找到，0表示没找到
int quicklistIndex(const quicklist *quicklist, const long long idx,
                   quicklistEntry *entry) {
    quicklistNode *n;
    unsigned long long accum = 0;
    unsigned long long index;
    //idx小于0反向迭代，大于等于0正向迭代
    int forward = idx < 0 ? 0 : 1; /* < 0 -> reverse, 0+ -> forward */

    initEntry(entry);
    entry->quicklist = quicklist;   //将quicklistEntry和当前quicklist结合

    if (!forward) { //idx小于0，反向迭代
        index = (-idx) - 1; //反向迭代的下标
        n = quicklist->tail;    //指向尾节点
    } else {
        index = idx;        //正向迭代的下标
        n = quicklist->head;    //指向头节点
    }

    if (index >= quicklist->count)  //下标超过entry数目范围
        return 0;

    while (likely(n)) {     //遍历节点
        if ((accum + n->count) > index) {   //找到跳出循环
            break;
        } else {
            D("Skipping over (%p) %u at accum %lld", (void *)n, n->count,
              accum);
            accum += n->count;  //遍历过quicklistNode节点的entry和
            n = forward ? n->next : n->prev;    //更新n的指向
        }
    }

    if (!n)     //如果遍历完所有的节点，则返回
        return 0;

    D("Found node: %p at accum %llu, idx %llu, sub+ %llu, sub- %llu", (void *)n,
      accum, index, index - accum, (-index) - 1 + accum);

    entry->node = n;    //更新ziplist所属的节点信息
    if (forward) {      //如果是前向遍历，计算出当前ziplist中的偏移量
        /* forward = normal head-to-tail offset. */
        entry->offset = index - accum;
    } else {
        /* reverse = need negative offset for tail-to-head, so undo
         * the result of the original if (index < 0) above. */
        entry->offset = (-index) - 1 + accum;
    }

    quicklistDecompressNodeForUse(entry->node);             //临时解压缩
    entry->zi = ziplistIndex(entry->node->zl, entry->offset);   //设置ziplist所偏移到的offset的entry
    ziplistGet(entry->zi, &entry->value, &entry->sz, &entry->longval);  //将当前entry的信息读取出来
    /* The caller will use our result, so we don't re-compress here.
     * The caller can recompress or delete the node as needed. */
    return 1;
}

/* Rotate quicklist by moving the tail element to the head. */
//将尾quicklistNode节点的尾entry节点旋转到头quicklistNode节点的头部
void quicklistRotate(quicklist *quicklist) {
    if (quicklist->count <= 1)  //只有一个entry，返回
        return;

    /* First, get the tail entry */
    //首先找到尾entry节点
    unsigned char *p = ziplistIndex(quicklist->tail->zl, -1);
    unsigned char *value;
    long long longval;
    unsigned int sz;
    char longstr[32] = {0};
    ziplistGet(p, &value, &sz, &longval);   //将尾entry的信息读取出来

    /* If value found is NULL, then ziplistGet populated longval instead */
    if (!value) {   //如果value为空，说明entry存放的是整数值
        /* Write the longval as a string so we can re-add it */
        sz = ll2string(longstr, sizeof(longstr), longval);  //将整数转为字符串，存到value中
        value = (unsigned char *)longstr;
    }

    /* Add tail entry to head (must happen before tail is deleted). */
    quicklistPushHead(quicklist, value, sz);    //将尾entry节点的信息push到quicklistNode的头部

    /* If quicklist has only one node, the head ziplist is also the
     * tail ziplist and PushHead() could have reallocated our single ziplist,
     * which would make our pre-existing 'p' unusable. */
    if (quicklist->len == 1) {  //如果只有一个quicklistNode节点
        p = ziplistIndex(quicklist->tail->zl, -1);  //返回尾entry节点的指针
    }

    /* Remove tail entry. */
    //p指向尾entry节点，删除这个节点
    quicklistDelIndex(quicklist, quicklist->tail, &p);
}

/* pop from quicklist and return result in 'data' ptr.  Value of 'data'
 * is the return value of 'saver' function pointer if the data is NOT a number.
 *
 * If the quicklist element is a long long, then the return value is returned in
 * 'sval'.
 *
 * Return value of 0 means no elements available.
 * Return value of 1 means check 'data' and 'sval' for values.
 * If 'data' is set, use 'data' and 'sz'.  Otherwise, use 'sval'. */
//从quicklist的头节点或尾节点pop弹出出一个entry，并将value保存在传入传出参数
//返回0表示没有可pop出的entry
//返回1表示pop出了entry，存在data或sval中
int quicklistPopCustom(quicklist *quicklist, int where, unsigned char **data,
                       unsigned int *sz, long long *sval,
                       void *(*saver)(unsigned char *data, unsigned int sz)) {
    unsigned char *p;
    unsigned char *vstr;
    unsigned int vlen;
    long long vlong;
    int pos = (where == QUICKLIST_HEAD) ? 0 : -1;   //位置下标

    if (quicklist->count == 0)  //entry数量为0，弹出失败
        return 0;

    //初始化
    if (data)
        *data = NULL;
    if (sz)
        *sz = 0;
    if (sval)
        *sval = -123456789;

    quicklistNode *node;
    //记录quicklist的头quicklistNode节点或尾quicklistNode节点
    if (where == QUICKLIST_HEAD && quicklist->head) {
        node = quicklist->head;
    } else if (where == QUICKLIST_TAIL && quicklist->tail) {
        node = quicklist->tail;
    } else {
        return 0;           //只能从头或尾弹出
    }

    p = ziplistIndex(node->zl, pos);    //获得当前pos的entry地址
    if (ziplistGet(p, &vstr, &vlen, &vlong)) {  //将entry信息读入到参数中
        if (vstr) {     //entry中是字符串值
            if (data)
                *data = saver(vstr, vlen);  //调用特定的函数将字符串值保存到*data
            if (sz)
                *sz = vlen;                 //保存字符串长度
        } else {        //整数值
            if (data)
                *data = NULL;
            if (sval)
                *sval = vlong;  //将整数值保存在*sval中
        }
        quicklistDelIndex(quicklist, node, &p); //将该entry从ziplist中删除
        return 1;
    }
    return 0;
}

/* Return a malloc'd copy of data passed in */
//将data内容拷贝一份并返回地址
REDIS_STATIC void *_quicklistSaver(unsigned char *data, unsigned int sz) {
    unsigned char *vstr;
    if (data) {
        vstr = zmalloc(sz);     //分配空间
        memcpy(vstr, data, sz); //拷贝
        return vstr;
    }
    return NULL;
}

/* Default pop function
 *
 * Returns malloc'd value from quicklist */
//pop一个entry值，调用quicklistPopCustom，封装起来
int quicklistPop(quicklist *quicklist, int where, unsigned char **data,
                 unsigned int *sz, long long *slong) {
    unsigned char *vstr;
    unsigned int vlen;
    long long vlong;
    if (quicklist->count == 0)
        return 0;
    int ret = quicklistPopCustom(quicklist, where, &vstr, &vlen, &vlong,
                                 _quicklistSaver);  //pop出一个where位置的entry

    //将pop出的值保存起来
    if (data)
        *data = vstr;
    if (slong)
        *slong = vlong;
    if (sz)
        *sz = vlen;
    return ret;
}

/* Wrapper to allow argument-based switching between HEAD/TAIL pop */
//将push函数封装起来，通过where 表示push头部或push尾部
void quicklistPush(quicklist *quicklist, void *value, const size_t sz,
                   int where) {
    if (where == QUICKLIST_HEAD) {                  //头插
        quicklistPushHead(quicklist, value, sz);
    } else if (where == QUICKLIST_TAIL) {           //尾插
        quicklistPushTail(quicklist, value, sz);
    }
}

/* The rest of this file is test cases and test helpers. */
//测试代码
#ifdef REDIS_TEST
#include <stdint.h>
#include <sys/time.h>

#define assert(_e)                                                             \
    do {                                                                       \
        if (!(_e)) {                                                           \
            printf("\n\n=== ASSERTION FAILED ===\n");                          \
            printf("==> %s:%d '%s' is not true\n", __FILE__, __LINE__, #_e);   \
            err++;                                                             \
        }                                                                      \
    } while (0)

#define yell(str, ...) printf("ERROR! " str "\n\n", __VA_ARGS__)

#define OK printf("\tOK\n")

#define ERROR                                                                  \
    do {                                                                       \
        printf("\tERROR!\n");                                                  \
        err++;                                                                 \
    } while (0)

#define ERR(x, ...)                                                            \
    do {                                                                       \
        printf("%s:%s:%d:\t", __FILE__, __FUNCTION__, __LINE__);               \
        printf("ERROR! " x "\n", __VA_ARGS__);                                 \
        err++;                                                                 \
    } while (0)

#define TEST(name) printf("test — %s\n", name);
#define TEST_DESC(name, ...) printf("test — " name "\n", __VA_ARGS__);

#define QL_TEST_VERBOSE 0

#define UNUSED(x) (void)(x)
static void ql_info(quicklist *ql) {
#if QL_TEST_VERBOSE
    printf("Container length: %lu\n", ql->len);
    printf("Container size: %lu\n", ql->count);
    if (ql->head)
        printf("\t(zsize head: %d)\n", ziplistLen(ql->head->zl));
    if (ql->tail)
        printf("\t(zsize tail: %d)\n", ziplistLen(ql->tail->zl));
    printf("\n");
#else
    UNUSED(ql);
#endif
}

/* Return the UNIX time in microseconds */
static long long ustime(void) {
    struct timeval tv;
    long long ust;

    gettimeofday(&tv, NULL);
    ust = ((long long)tv.tv_sec) * 1000000;
    ust += tv.tv_usec;
    return ust;
}

/* Return the UNIX time in milliseconds */
static long long mstime(void) { return ustime() / 1000; }

/* Iterate over an entire quicklist.
 * Print the list if 'print' == 1.
 *
 * Returns physical count of elements found by iterating over the list. */
static int _itrprintr(quicklist *ql, int print, int forward) {
    quicklistIter *iter =
        quicklistGetIterator(ql, forward ? AL_START_HEAD : AL_START_TAIL);
    quicklistEntry entry;
    int i = 0;
    int p = 0;
    quicklistNode *prev = NULL;
    while (quicklistNext(iter, &entry)) {
        if (entry.node != prev) {
            /* Count the number of list nodes too */
            p++;
            prev = entry.node;
        }
        if (print) {
            printf("[%3d (%2d)]: [%.*s] (%lld)\n", i, p, entry.sz,
                   (char *)entry.value, entry.longval);
        }
        i++;
    }
    quicklistReleaseIterator(iter);
    return i;
}
static int itrprintr(quicklist *ql, int print) {
    return _itrprintr(ql, print, 1);
}

static int itrprintr_rev(quicklist *ql, int print) {
    return _itrprintr(ql, print, 0);
}

#define ql_verify(a, b, c, d, e)                                               \
    do {                                                                       \
        err += _ql_verify((a), (b), (c), (d), (e));                            \
    } while (0)

/* Verify list metadata matches physical list contents. */
static int _ql_verify(quicklist *ql, uint32_t len, uint32_t count,
                      uint32_t head_count, uint32_t tail_count) {
    int errors = 0;

    ql_info(ql);
    if (len != ql->len) {
        yell("quicklist length wrong: expected %d, got %u", len, ql->len);
        errors++;
    }

    if (count != ql->count) {
        yell("quicklist count wrong: expected %d, got %lu", count, ql->count);
        errors++;
    }

    int loopr = itrprintr(ql, 0);
    if (loopr != (int)ql->count) {
        yell("quicklist cached count not match actual count: expected %lu, got "
             "%d",
             ql->count, loopr);
        errors++;
    }

    int rloopr = itrprintr_rev(ql, 0);
    if (loopr != rloopr) {
        yell("quicklist has different forward count than reverse count!  "
             "Forward count is %d, reverse count is %d.",
             loopr, rloopr);
        errors++;
    }

    if (ql->len == 0 && !errors) {
        OK;
        return errors;
    }

    if (ql->head && head_count != ql->head->count &&
        head_count != ziplistLen(ql->head->zl)) {
        yell("quicklist head count wrong: expected %d, "
             "got cached %d vs. actual %d",
             head_count, ql->head->count, ziplistLen(ql->head->zl));
        errors++;
    }

    if (ql->tail && tail_count != ql->tail->count &&
        tail_count != ziplistLen(ql->tail->zl)) {
        yell("quicklist tail count wrong: expected %d, "
             "got cached %u vs. actual %d",
             tail_count, ql->tail->count, ziplistLen(ql->tail->zl));
        errors++;
    }

    if (quicklistAllowsCompression(ql)) {
        quicklistNode *node = ql->head;
        unsigned int low_raw = ql->compress;
        unsigned int high_raw = ql->len - ql->compress;

        for (unsigned int at = 0; at < ql->len; at++, node = node->next) {
            if (node && (at < low_raw || at >= high_raw)) {
                if (node->encoding != QUICKLIST_NODE_ENCODING_RAW) {
                    yell("Incorrect compression: node %d is "
                         "compressed at depth %d ((%u, %u); total "
                         "nodes: %u; size: %u; recompress: %d)",
                         at, ql->compress, low_raw, high_raw, ql->len, node->sz,
                         node->recompress);
                    errors++;
                }
            } else {
                if (node->encoding != QUICKLIST_NODE_ENCODING_LZF &&
                    !node->attempted_compress) {
                    yell("Incorrect non-compression: node %d is NOT "
                         "compressed at depth %d ((%u, %u); total "
                         "nodes: %u; size: %u; recompress: %d; attempted: %d)",
                         at, ql->compress, low_raw, high_raw, ql->len, node->sz,
                         node->recompress, node->attempted_compress);
                    errors++;
                }
            }
        }
    }

    if (!errors)
        OK;
    return errors;
}

/* Generate new string concatenating integer i against string 'prefix' */
static char *genstr(char *prefix, int i) {
    static char result[64] = {0};
    snprintf(result, sizeof(result), "%s%d", prefix, i);
    return result;
}

/* main test, but callable from other files */
int quicklistTest(int argc, char *argv[]) {
    UNUSED(argc);
    UNUSED(argv);

    unsigned int err = 0;
    int optimize_start =
        -(int)(sizeof(optimization_level) / sizeof(*optimization_level));

    printf("Starting optimization offset at: %d\n", optimize_start);

    int options[] = {0, 1, 2, 3, 4, 5, 6, 10};
    size_t option_count = sizeof(options) / sizeof(*options);
    long long runtime[option_count];

    for (int _i = 0; _i < (int)option_count; _i++) {
        printf("Testing Option %d\n", options[_i]);
        long long start = mstime();

        TEST("create list") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            ql_verify(ql, 0, 0, 0, 0);
            quicklistRelease(ql);
        }

        TEST("add to tail of empty list") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            quicklistPushTail(ql, "hello", 6);
            /* 1 for head and 1 for tail beacuse 1 node = head = tail */
            ql_verify(ql, 1, 1, 1, 1);
            quicklistRelease(ql);
        }

        TEST("add to head of empty list") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            quicklistPushHead(ql, "hello", 6);
            /* 1 for head and 1 for tail beacuse 1 node = head = tail */
            ql_verify(ql, 1, 1, 1, 1);
            quicklistRelease(ql);
        }

        for (int f = optimize_start; f < 32; f++) {
            TEST_DESC("add to tail 5x at fill %d at compress %d", f,
                      options[_i]) {
                quicklist *ql = quicklistNew(f, options[_i]);
                for (int i = 0; i < 5; i++)
                    quicklistPushTail(ql, genstr("hello", i), 32);
                if (ql->count != 5)
                    ERROR;
                if (f == 32)
                    ql_verify(ql, 1, 5, 5, 5);
                quicklistRelease(ql);
            }
        }

        for (int f = optimize_start; f < 32; f++) {
            TEST_DESC("add to head 5x at fill %d at compress %d", f,
                      options[_i]) {
                quicklist *ql = quicklistNew(f, options[_i]);
                for (int i = 0; i < 5; i++)
                    quicklistPushHead(ql, genstr("hello", i), 32);
                if (ql->count != 5)
                    ERROR;
                if (f == 32)
                    ql_verify(ql, 1, 5, 5, 5);
                quicklistRelease(ql);
            }
        }

        for (int f = optimize_start; f < 512; f++) {
            TEST_DESC("add to tail 500x at fill %d at compress %d", f,
                      options[_i]) {
                quicklist *ql = quicklistNew(f, options[_i]);
                for (int i = 0; i < 500; i++)
                    quicklistPushTail(ql, genstr("hello", i), 64);
                if (ql->count != 500)
                    ERROR;
                if (f == 32)
                    ql_verify(ql, 16, 500, 32, 20);
                quicklistRelease(ql);
            }
        }

        for (int f = optimize_start; f < 512; f++) {
            TEST_DESC("add to head 500x at fill %d at compress %d", f,
                      options[_i]) {
                quicklist *ql = quicklistNew(f, options[_i]);
                for (int i = 0; i < 500; i++)
                    quicklistPushHead(ql, genstr("hello", i), 32);
                if (ql->count != 500)
                    ERROR;
                if (f == 32)
                    ql_verify(ql, 16, 500, 20, 32);
                quicklistRelease(ql);
            }
        }

        TEST("rotate empty") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            quicklistRotate(ql);
            ql_verify(ql, 0, 0, 0, 0);
            quicklistRelease(ql);
        }

        for (int f = optimize_start; f < 32; f++) {
            TEST("rotate one val once") {
                quicklist *ql = quicklistNew(f, options[_i]);
                quicklistPushHead(ql, "hello", 6);
                quicklistRotate(ql);
                /* Ignore compression verify because ziplist is
                 * too small to compress. */
                ql_verify(ql, 1, 1, 1, 1);
                quicklistRelease(ql);
            }
        }

        for (int f = optimize_start; f < 3; f++) {
            TEST_DESC("rotate 500 val 5000 times at fill %d at compress %d", f,
                      options[_i]) {
                quicklist *ql = quicklistNew(f, options[_i]);
                quicklistPushHead(ql, "900", 3);
                quicklistPushHead(ql, "7000", 4);
                quicklistPushHead(ql, "-1200", 5);
                quicklistPushHead(ql, "42", 2);
                for (int i = 0; i < 500; i++)
                    quicklistPushHead(ql, genstr("hello", i), 64);
                ql_info(ql);
                for (int i = 0; i < 5000; i++) {
                    ql_info(ql);
                    quicklistRotate(ql);
                }
                if (f == 1)
                    ql_verify(ql, 504, 504, 1, 1);
                else if (f == 2)
                    ql_verify(ql, 252, 504, 2, 2);
                else if (f == 32)
                    ql_verify(ql, 16, 504, 32, 24);
                quicklistRelease(ql);
            }
        }

        TEST("pop empty") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            quicklistPop(ql, QUICKLIST_HEAD, NULL, NULL, NULL);
            ql_verify(ql, 0, 0, 0, 0);
            quicklistRelease(ql);
        }

        TEST("pop 1 string from 1") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            char *populate = genstr("hello", 331);
            quicklistPushHead(ql, populate, 32);
            unsigned char *data;
            unsigned int sz;
            long long lv;
            ql_info(ql);
            quicklistPop(ql, QUICKLIST_HEAD, &data, &sz, &lv);
            assert(data != NULL);
            assert(sz == 32);
            if (strcmp(populate, (char *)data))
                ERR("Pop'd value (%.*s) didn't equal original value (%s)", sz,
                    data, populate);
            zfree(data);
            ql_verify(ql, 0, 0, 0, 0);
            quicklistRelease(ql);
        }

        TEST("pop head 1 number from 1") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            quicklistPushHead(ql, "55513", 5);
            unsigned char *data;
            unsigned int sz;
            long long lv;
            ql_info(ql);
            quicklistPop(ql, QUICKLIST_HEAD, &data, &sz, &lv);
            assert(data == NULL);
            assert(lv == 55513);
            ql_verify(ql, 0, 0, 0, 0);
            quicklistRelease(ql);
        }

        TEST("pop head 500 from 500") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            for (int i = 0; i < 500; i++)
                quicklistPushHead(ql, genstr("hello", i), 32);
            ql_info(ql);
            for (int i = 0; i < 500; i++) {
                unsigned char *data;
                unsigned int sz;
                long long lv;
                int ret = quicklistPop(ql, QUICKLIST_HEAD, &data, &sz, &lv);
                assert(ret == 1);
                assert(data != NULL);
                assert(sz == 32);
                if (strcmp(genstr("hello", 499 - i), (char *)data))
                    ERR("Pop'd value (%.*s) didn't equal original value (%s)",
                        sz, data, genstr("hello", 499 - i));
                zfree(data);
            }
            ql_verify(ql, 0, 0, 0, 0);
            quicklistRelease(ql);
        }

        TEST("pop head 5000 from 500") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            for (int i = 0; i < 500; i++)
                quicklistPushHead(ql, genstr("hello", i), 32);
            for (int i = 0; i < 5000; i++) {
                unsigned char *data;
                unsigned int sz;
                long long lv;
                int ret = quicklistPop(ql, QUICKLIST_HEAD, &data, &sz, &lv);
                if (i < 500) {
                    assert(ret == 1);
                    assert(data != NULL);
                    assert(sz == 32);
                    if (strcmp(genstr("hello", 499 - i), (char *)data))
                        ERR("Pop'd value (%.*s) didn't equal original value "
                            "(%s)",
                            sz, data, genstr("hello", 499 - i));
                    zfree(data);
                } else {
                    assert(ret == 0);
                }
            }
            ql_verify(ql, 0, 0, 0, 0);
            quicklistRelease(ql);
        }

        TEST("iterate forward over 500 list") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            quicklistSetFill(ql, 32);
            for (int i = 0; i < 500; i++)
                quicklistPushHead(ql, genstr("hello", i), 32);
            quicklistIter *iter = quicklistGetIterator(ql, AL_START_HEAD);
            quicklistEntry entry;
            int i = 499, count = 0;
            while (quicklistNext(iter, &entry)) {
                char *h = genstr("hello", i);
                if (strcmp((char *)entry.value, h))
                    ERR("value [%s] didn't match [%s] at position %d",
                        entry.value, h, i);
                i--;
                count++;
            }
            if (count != 500)
                ERR("Didn't iterate over exactly 500 elements (%d)", i);
            ql_verify(ql, 16, 500, 20, 32);
            quicklistReleaseIterator(iter);
            quicklistRelease(ql);
        }

        TEST("iterate reverse over 500 list") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            quicklistSetFill(ql, 32);
            for (int i = 0; i < 500; i++)
                quicklistPushHead(ql, genstr("hello", i), 32);
            quicklistIter *iter = quicklistGetIterator(ql, AL_START_TAIL);
            quicklistEntry entry;
            int i = 0;
            while (quicklistNext(iter, &entry)) {
                char *h = genstr("hello", i);
                if (strcmp((char *)entry.value, h))
                    ERR("value [%s] didn't match [%s] at position %d",
                        entry.value, h, i);
                i++;
            }
            if (i != 500)
                ERR("Didn't iterate over exactly 500 elements (%d)", i);
            ql_verify(ql, 16, 500, 20, 32);
            quicklistReleaseIterator(iter);
            quicklistRelease(ql);
        }

        TEST("insert before with 0 elements") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            quicklistEntry entry;
            quicklistIndex(ql, 0, &entry);
            quicklistInsertBefore(ql, &entry, "abc", 4);
            ql_verify(ql, 1, 1, 1, 1);
            quicklistRelease(ql);
        }

        TEST("insert after with 0 elements") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            quicklistEntry entry;
            quicklistIndex(ql, 0, &entry);
            quicklistInsertAfter(ql, &entry, "abc", 4);
            ql_verify(ql, 1, 1, 1, 1);
            quicklistRelease(ql);
        }

        TEST("insert after 1 element") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            quicklistPushHead(ql, "hello", 6);
            quicklistEntry entry;
            quicklistIndex(ql, 0, &entry);
            quicklistInsertAfter(ql, &entry, "abc", 4);
            ql_verify(ql, 1, 2, 2, 2);
            quicklistRelease(ql);
        }

        TEST("insert before 1 element") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            quicklistPushHead(ql, "hello", 6);
            quicklistEntry entry;
            quicklistIndex(ql, 0, &entry);
            quicklistInsertAfter(ql, &entry, "abc", 4);
            ql_verify(ql, 1, 2, 2, 2);
            quicklistRelease(ql);
        }

        for (int f = optimize_start; f < 12; f++) {
            TEST_DESC("insert once in elements while iterating at fill %d at "
                      "compress %d\n",
                      f, options[_i]) {
                quicklist *ql = quicklistNew(f, options[_i]);
                quicklistPushTail(ql, "abc", 3);
                quicklistSetFill(ql, 1);
                quicklistPushTail(ql, "def", 3); /* force to unique node */
                quicklistSetFill(ql, f);
                quicklistPushTail(ql, "bob", 3); /* force to reset for +3 */
                quicklistPushTail(ql, "foo", 3);
                quicklistPushTail(ql, "zoo", 3);

                itrprintr(ql, 0);
                /* insert "bar" before "bob" while iterating over list. */
                quicklistIter *iter = quicklistGetIterator(ql, AL_START_HEAD);
                quicklistEntry entry;
                while (quicklistNext(iter, &entry)) {
                    if (!strncmp((char *)entry.value, "bob", 3)) {
                        /* Insert as fill = 1 so it spills into new node. */
                        quicklistInsertBefore(ql, &entry, "bar", 3);
                        break; /* didn't we fix insert-while-iterating? */
                    }
                }
                itrprintr(ql, 0);

                /* verify results */
                quicklistIndex(ql, 0, &entry);
                if (strncmp((char *)entry.value, "abc", 3))
                    ERR("Value 0 didn't match, instead got: %.*s", entry.sz,
                        entry.value);
                quicklistIndex(ql, 1, &entry);
                if (strncmp((char *)entry.value, "def", 3))
                    ERR("Value 1 didn't match, instead got: %.*s", entry.sz,
                        entry.value);
                quicklistIndex(ql, 2, &entry);
                if (strncmp((char *)entry.value, "bar", 3))
                    ERR("Value 2 didn't match, instead got: %.*s", entry.sz,
                        entry.value);
                quicklistIndex(ql, 3, &entry);
                if (strncmp((char *)entry.value, "bob", 3))
                    ERR("Value 3 didn't match, instead got: %.*s", entry.sz,
                        entry.value);
                quicklistIndex(ql, 4, &entry);
                if (strncmp((char *)entry.value, "foo", 3))
                    ERR("Value 4 didn't match, instead got: %.*s", entry.sz,
                        entry.value);
                quicklistIndex(ql, 5, &entry);
                if (strncmp((char *)entry.value, "zoo", 3))
                    ERR("Value 5 didn't match, instead got: %.*s", entry.sz,
                        entry.value);
                quicklistReleaseIterator(iter);
                quicklistRelease(ql);
            }
        }

        for (int f = optimize_start; f < 1024; f++) {
            TEST_DESC(
                "insert [before] 250 new in middle of 500 elements at fill"
                " %d at compress %d",
                f, options[_i]) {
                quicklist *ql = quicklistNew(f, options[_i]);
                for (int i = 0; i < 500; i++)
                    quicklistPushTail(ql, genstr("hello", i), 32);
                for (int i = 0; i < 250; i++) {
                    quicklistEntry entry;
                    quicklistIndex(ql, 250, &entry);
                    quicklistInsertBefore(ql, &entry, genstr("abc", i), 32);
                }
                if (f == 32)
                    ql_verify(ql, 25, 750, 32, 20);
                quicklistRelease(ql);
            }
        }

        for (int f = optimize_start; f < 1024; f++) {
            TEST_DESC("insert [after] 250 new in middle of 500 elements at "
                      "fill %d at compress %d",
                      f, options[_i]) {
                quicklist *ql = quicklistNew(f, options[_i]);
                for (int i = 0; i < 500; i++)
                    quicklistPushHead(ql, genstr("hello", i), 32);
                for (int i = 0; i < 250; i++) {
                    quicklistEntry entry;
                    quicklistIndex(ql, 250, &entry);
                    quicklistInsertAfter(ql, &entry, genstr("abc", i), 32);
                }

                if (ql->count != 750)
                    ERR("List size not 750, but rather %ld", ql->count);

                if (f == 32)
                    ql_verify(ql, 26, 750, 20, 32);
                quicklistRelease(ql);
            }
        }

        TEST("duplicate empty list") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            ql_verify(ql, 0, 0, 0, 0);
            quicklist *copy = quicklistDup(ql);
            ql_verify(copy, 0, 0, 0, 0);
            quicklistRelease(ql);
            quicklistRelease(copy);
        }

        TEST("duplicate list of 1 element") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            quicklistPushHead(ql, genstr("hello", 3), 32);
            ql_verify(ql, 1, 1, 1, 1);
            quicklist *copy = quicklistDup(ql);
            ql_verify(copy, 1, 1, 1, 1);
            quicklistRelease(ql);
            quicklistRelease(copy);
        }

        TEST("duplicate list of 500") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            quicklistSetFill(ql, 32);
            for (int i = 0; i < 500; i++)
                quicklistPushHead(ql, genstr("hello", i), 32);
            ql_verify(ql, 16, 500, 20, 32);

            quicklist *copy = quicklistDup(ql);
            ql_verify(copy, 16, 500, 20, 32);
            quicklistRelease(ql);
            quicklistRelease(copy);
        }

        for (int f = optimize_start; f < 512; f++) {
            TEST_DESC("index 1,200 from 500 list at fill %d at compress %d", f,
                      options[_i]) {
                quicklist *ql = quicklistNew(f, options[_i]);
                for (int i = 0; i < 500; i++)
                    quicklistPushTail(ql, genstr("hello", i + 1), 32);
                quicklistEntry entry;
                quicklistIndex(ql, 1, &entry);
                if (!strcmp((char *)entry.value, "hello2"))
                    OK;
                else
                    ERR("Value: %s", entry.value);
                quicklistIndex(ql, 200, &entry);
                if (!strcmp((char *)entry.value, "hello201"))
                    OK;
                else
                    ERR("Value: %s", entry.value);
                quicklistRelease(ql);
            }

            TEST_DESC("index -1,-2 from 500 list at fill %d at compress %d", f,
                      options[_i]) {
                quicklist *ql = quicklistNew(f, options[_i]);
                for (int i = 0; i < 500; i++)
                    quicklistPushTail(ql, genstr("hello", i + 1), 32);
                quicklistEntry entry;
                quicklistIndex(ql, -1, &entry);
                if (!strcmp((char *)entry.value, "hello500"))
                    OK;
                else
                    ERR("Value: %s", entry.value);
                quicklistIndex(ql, -2, &entry);
                if (!strcmp((char *)entry.value, "hello499"))
                    OK;
                else
                    ERR("Value: %s", entry.value);
                quicklistRelease(ql);
            }

            TEST_DESC("index -100 from 500 list at fill %d at compress %d", f,
                      options[_i]) {
                quicklist *ql = quicklistNew(f, options[_i]);
                for (int i = 0; i < 500; i++)
                    quicklistPushTail(ql, genstr("hello", i + 1), 32);
                quicklistEntry entry;
                quicklistIndex(ql, -100, &entry);
                if (!strcmp((char *)entry.value, "hello401"))
                    OK;
                else
                    ERR("Value: %s", entry.value);
                quicklistRelease(ql);
            }

            TEST_DESC("index too big +1 from 50 list at fill %d at compress %d",
                      f, options[_i]) {
                quicklist *ql = quicklistNew(f, options[_i]);
                for (int i = 0; i < 50; i++)
                    quicklistPushTail(ql, genstr("hello", i + 1), 32);
                quicklistEntry entry;
                if (quicklistIndex(ql, 50, &entry))
                    ERR("Index found at 50 with 50 list: %.*s", entry.sz,
                        entry.value);
                else
                    OK;
                quicklistRelease(ql);
            }
        }

        TEST("delete range empty list") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            quicklistDelRange(ql, 5, 20);
            ql_verify(ql, 0, 0, 0, 0);
            quicklistRelease(ql);
        }

        TEST("delete range of entire node in list of one node") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            for (int i = 0; i < 32; i++)
                quicklistPushHead(ql, genstr("hello", i), 32);
            ql_verify(ql, 1, 32, 32, 32);
            quicklistDelRange(ql, 0, 32);
            ql_verify(ql, 0, 0, 0, 0);
            quicklistRelease(ql);
        }

        TEST("delete range of entire node with overflow counts") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            for (int i = 0; i < 32; i++)
                quicklistPushHead(ql, genstr("hello", i), 32);
            ql_verify(ql, 1, 32, 32, 32);
            quicklistDelRange(ql, 0, 128);
            ql_verify(ql, 0, 0, 0, 0);
            quicklistRelease(ql);
        }

        TEST("delete middle 100 of 500 list") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            quicklistSetFill(ql, 32);
            for (int i = 0; i < 500; i++)
                quicklistPushTail(ql, genstr("hello", i + 1), 32);
            ql_verify(ql, 16, 500, 32, 20);
            quicklistDelRange(ql, 200, 100);
            ql_verify(ql, 14, 400, 32, 20);
            quicklistRelease(ql);
        }

        TEST("delete negative 1 from 500 list") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            quicklistSetFill(ql, 32);
            for (int i = 0; i < 500; i++)
                quicklistPushTail(ql, genstr("hello", i + 1), 32);
            ql_verify(ql, 16, 500, 32, 20);
            quicklistDelRange(ql, -1, 1);
            ql_verify(ql, 16, 499, 32, 19);
            quicklistRelease(ql);
        }

        TEST("delete negative 1 from 500 list with overflow counts") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            quicklistSetFill(ql, 32);
            for (int i = 0; i < 500; i++)
                quicklistPushTail(ql, genstr("hello", i + 1), 32);
            ql_verify(ql, 16, 500, 32, 20);
            quicklistDelRange(ql, -1, 128);
            ql_verify(ql, 16, 499, 32, 19);
            quicklistRelease(ql);
        }

        TEST("delete negative 100 from 500 list") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            quicklistSetFill(ql, 32);
            for (int i = 0; i < 500; i++)
                quicklistPushTail(ql, genstr("hello", i + 1), 32);
            quicklistDelRange(ql, -100, 100);
            ql_verify(ql, 13, 400, 32, 16);
            quicklistRelease(ql);
        }

        TEST("delete -10 count 5 from 50 list") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            quicklistSetFill(ql, 32);
            for (int i = 0; i < 50; i++)
                quicklistPushTail(ql, genstr("hello", i + 1), 32);
            ql_verify(ql, 2, 50, 32, 18);
            quicklistDelRange(ql, -10, 5);
            ql_verify(ql, 2, 45, 32, 13);
            quicklistRelease(ql);
        }

        TEST("numbers only list read") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            quicklistPushTail(ql, "1111", 4);
            quicklistPushTail(ql, "2222", 4);
            quicklistPushTail(ql, "3333", 4);
            quicklistPushTail(ql, "4444", 4);
            ql_verify(ql, 1, 4, 4, 4);
            quicklistEntry entry;
            quicklistIndex(ql, 0, &entry);
            if (entry.longval != 1111)
                ERR("Not 1111, %lld", entry.longval);
            quicklistIndex(ql, 1, &entry);
            if (entry.longval != 2222)
                ERR("Not 2222, %lld", entry.longval);
            quicklistIndex(ql, 2, &entry);
            if (entry.longval != 3333)
                ERR("Not 3333, %lld", entry.longval);
            quicklistIndex(ql, 3, &entry);
            if (entry.longval != 4444)
                ERR("Not 4444, %lld", entry.longval);
            if (quicklistIndex(ql, 4, &entry))
                ERR("Index past elements: %lld", entry.longval);
            quicklistIndex(ql, -1, &entry);
            if (entry.longval != 4444)
                ERR("Not 4444 (reverse), %lld", entry.longval);
            quicklistIndex(ql, -2, &entry);
            if (entry.longval != 3333)
                ERR("Not 3333 (reverse), %lld", entry.longval);
            quicklistIndex(ql, -3, &entry);
            if (entry.longval != 2222)
                ERR("Not 2222 (reverse), %lld", entry.longval);
            quicklistIndex(ql, -4, &entry);
            if (entry.longval != 1111)
                ERR("Not 1111 (reverse), %lld", entry.longval);
            if (quicklistIndex(ql, -5, &entry))
                ERR("Index past elements (reverse), %lld", entry.longval);
            quicklistRelease(ql);
        }

        TEST("numbers larger list read") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            quicklistSetFill(ql, 32);
            char num[32];
            long long nums[5000];
            for (int i = 0; i < 5000; i++) {
                nums[i] = -5157318210846258176 + i;
                int sz = ll2string(num, sizeof(num), nums[i]);
                quicklistPushTail(ql, num, sz);
            }
            quicklistPushTail(ql, "xxxxxxxxxxxxxxxxxxxx", 20);
            quicklistEntry entry;
            for (int i = 0; i < 5000; i++) {
                quicklistIndex(ql, i, &entry);
                if (entry.longval != nums[i])
                    ERR("[%d] Not longval %lld but rather %lld", i, nums[i],
                        entry.longval);
                entry.longval = 0xdeadbeef;
            }
            quicklistIndex(ql, 5000, &entry);
            if (strncmp((char *)entry.value, "xxxxxxxxxxxxxxxxxxxx", 20))
                ERR("String val not match: %s", entry.value);
            ql_verify(ql, 157, 5001, 32, 9);
            quicklistRelease(ql);
        }

        TEST("numbers larger list read B") {
            quicklist *ql = quicklistNew(-2, options[_i]);
            quicklistPushTail(ql, "99", 2);
            quicklistPushTail(ql, "98", 2);
            quicklistPushTail(ql, "xxxxxxxxxxxxxxxxxxxx", 20);
            quicklistPushTail(ql, "96", 2);
            quicklistPushTail(ql, "95", 2);
            quicklistReplaceAtIndex(ql, 1, "foo", 3);
            quicklistReplaceAtIndex(ql, -1, "bar", 3);
            quicklistRelease(ql);
            OK;
        }

        for (int f = optimize_start; f < 16; f++) {
            TEST_DESC("lrem test at fill %d at compress %d", f, options[_i]) {
                quicklist *ql = quicklistNew(f, options[_i]);
                char *words[] = {"abc", "foo", "bar",  "foobar", "foobared",
                                 "zap", "bar", "test", "foo"};
                char *result[] = {"abc", "foo",  "foobar", "foobared",
                                  "zap", "test", "foo"};
                char *resultB[] = {"abc",      "foo", "foobar",
                                   "foobared", "zap", "test"};
                for (int i = 0; i < 9; i++)
                    quicklistPushTail(ql, words[i], strlen(words[i]));

                /* lrem 0 bar */
                quicklistIter *iter = quicklistGetIterator(ql, AL_START_HEAD);
                quicklistEntry entry;
                int i = 0;
                while (quicklistNext(iter, &entry)) {
                    if (quicklistCompare(entry.zi, (unsigned char *)"bar", 3)) {
                        quicklistDelEntry(iter, &entry);
                    }
                    i++;
                }
                quicklistReleaseIterator(iter);

                /* check result of lrem 0 bar */
                iter = quicklistGetIterator(ql, AL_START_HEAD);
                i = 0;
                int ok = 1;
                while (quicklistNext(iter, &entry)) {
                    /* Result must be: abc, foo, foobar, foobared, zap, test,
                     * foo */
                    if (strncmp((char *)entry.value, result[i], entry.sz)) {
                        ERR("No match at position %d, got %.*s instead of %s",
                            i, entry.sz, entry.value, result[i]);
                        ok = 0;
                    }
                    i++;
                }
                quicklistReleaseIterator(iter);

                quicklistPushTail(ql, "foo", 3);

                /* lrem -2 foo */
                iter = quicklistGetIterator(ql, AL_START_TAIL);
                i = 0;
                int del = 2;
                while (quicklistNext(iter, &entry)) {
                    if (quicklistCompare(entry.zi, (unsigned char *)"foo", 3)) {
                        quicklistDelEntry(iter, &entry);
                        del--;
                    }
                    if (!del)
                        break;
                    i++;
                }
                quicklistReleaseIterator(iter);

                /* check result of lrem -2 foo */
                /* (we're ignoring the '2' part and still deleting all foo
                 * because
                 * we only have two foo) */
                iter = quicklistGetIterator(ql, AL_START_TAIL);
                i = 0;
                size_t resB = sizeof(resultB) / sizeof(*resultB);
                while (quicklistNext(iter, &entry)) {
                    /* Result must be: abc, foo, foobar, foobared, zap, test,
                     * foo */
                    if (strncmp((char *)entry.value, resultB[resB - 1 - i],
                                entry.sz)) {
                        ERR("No match at position %d, got %.*s instead of %s",
                            i, entry.sz, entry.value, resultB[resB - 1 - i]);
                        ok = 0;
                    }
                    i++;
                }

                quicklistReleaseIterator(iter);
                /* final result of all tests */
                if (ok)
                    OK;
                quicklistRelease(ql);
            }
        }

        for (int f = optimize_start; f < 16; f++) {
            TEST_DESC("iterate reverse + delete at fill %d at compress %d", f,
                      options[_i]) {
                quicklist *ql = quicklistNew(f, options[_i]);
                quicklistPushTail(ql, "abc", 3);
                quicklistPushTail(ql, "def", 3);
                quicklistPushTail(ql, "hij", 3);
                quicklistPushTail(ql, "jkl", 3);
                quicklistPushTail(ql, "oop", 3);

                quicklistEntry entry;
                quicklistIter *iter = quicklistGetIterator(ql, AL_START_TAIL);
                int i = 0;
                while (quicklistNext(iter, &entry)) {
                    if (quicklistCompare(entry.zi, (unsigned char *)"hij", 3)) {
                        quicklistDelEntry(iter, &entry);
                    }
                    i++;
                }
                quicklistReleaseIterator(iter);

                if (i != 5)
                    ERR("Didn't iterate 5 times, iterated %d times.", i);

                /* Check results after deletion of "hij" */
                iter = quicklistGetIterator(ql, AL_START_HEAD);
                i = 0;
                char *vals[] = {"abc", "def", "jkl", "oop"};
                while (quicklistNext(iter, &entry)) {
                    if (!quicklistCompare(entry.zi, (unsigned char *)vals[i],
                                          3)) {
                        ERR("Value at %d didn't match %s\n", i, vals[i]);
                    }
                    i++;
                }
                quicklistReleaseIterator(iter);
                quicklistRelease(ql);
            }
        }

        for (int f = optimize_start; f < 800; f++) {
            TEST_DESC("iterator at index test at fill %d at compress %d", f,
                      options[_i]) {
                quicklist *ql = quicklistNew(f, options[_i]);
                char num[32];
                long long nums[5000];
                for (int i = 0; i < 760; i++) {
                    nums[i] = -5157318210846258176 + i;
                    int sz = ll2string(num, sizeof(num), nums[i]);
                    quicklistPushTail(ql, num, sz);
                }

                quicklistEntry entry;
                quicklistIter *iter =
                    quicklistGetIteratorAtIdx(ql, AL_START_HEAD, 437);
                int i = 437;
                while (quicklistNext(iter, &entry)) {
                    if (entry.longval != nums[i])
                        ERR("Expected %lld, but got %lld", entry.longval,
                            nums[i]);
                    i++;
                }
                quicklistReleaseIterator(iter);
                quicklistRelease(ql);
            }
        }

        for (int f = optimize_start; f < 40; f++) {
            TEST_DESC("ltrim test A at fill %d at compress %d", f,
                      options[_i]) {
                quicklist *ql = quicklistNew(f, options[_i]);
                char num[32];
                long long nums[5000];
                for (int i = 0; i < 32; i++) {
                    nums[i] = -5157318210846258176 + i;
                    int sz = ll2string(num, sizeof(num), nums[i]);
                    quicklistPushTail(ql, num, sz);
                }
                if (f == 32)
                    ql_verify(ql, 1, 32, 32, 32);
                /* ltrim 25 53 (keep [25,32] inclusive = 7 remaining) */
                quicklistDelRange(ql, 0, 25);
                quicklistDelRange(ql, 0, 0);
                quicklistEntry entry;
                for (int i = 0; i < 7; i++) {
                    quicklistIndex(ql, i, &entry);
                    if (entry.longval != nums[25 + i])
                        ERR("Deleted invalid range!  Expected %lld but got "
                            "%lld",
                            entry.longval, nums[25 + i]);
                }
                if (f == 32)
                    ql_verify(ql, 1, 7, 7, 7);
                quicklistRelease(ql);
            }
        }

        for (int f = optimize_start; f < 40; f++) {
            TEST_DESC("ltrim test B at fill %d at compress %d", f,
                      options[_i]) {
                /* Force-disable compression because our 33 sequential
                 * integers don't compress and the check always fails. */
                quicklist *ql = quicklistNew(f, QUICKLIST_NOCOMPRESS);
                char num[32];
                long long nums[5000];
                for (int i = 0; i < 33; i++) {
                    nums[i] = i;
                    int sz = ll2string(num, sizeof(num), nums[i]);
                    quicklistPushTail(ql, num, sz);
                }
                if (f == 32)
                    ql_verify(ql, 2, 33, 32, 1);
                /* ltrim 5 16 (keep [5,16] inclusive = 12 remaining) */
                quicklistDelRange(ql, 0, 5);
                quicklistDelRange(ql, -16, 16);
                if (f == 32)
                    ql_verify(ql, 1, 12, 12, 12);
                quicklistEntry entry;
                quicklistIndex(ql, 0, &entry);
                if (entry.longval != 5)
                    ERR("A: longval not 5, but %lld", entry.longval);
                else
                    OK;
                quicklistIndex(ql, -1, &entry);
                if (entry.longval != 16)
                    ERR("B! got instead: %lld", entry.longval);
                else
                    OK;
                quicklistPushTail(ql, "bobobob", 7);
                quicklistIndex(ql, -1, &entry);
                if (strncmp((char *)entry.value, "bobobob", 7))
                    ERR("Tail doesn't match bobobob, it's %.*s instead",
                        entry.sz, entry.value);
                for (int i = 0; i < 12; i++) {
                    quicklistIndex(ql, i, &entry);
                    if (entry.longval != nums[5 + i])
                        ERR("Deleted invalid range!  Expected %lld but got "
                            "%lld",
                            entry.longval, nums[5 + i]);
                }
                quicklistRelease(ql);
            }
        }

        for (int f = optimize_start; f < 40; f++) {
            TEST_DESC("ltrim test C at fill %d at compress %d", f,
                      options[_i]) {
                quicklist *ql = quicklistNew(f, options[_i]);
                char num[32];
                long long nums[5000];
                for (int i = 0; i < 33; i++) {
                    nums[i] = -5157318210846258176 + i;
                    int sz = ll2string(num, sizeof(num), nums[i]);
                    quicklistPushTail(ql, num, sz);
                }
                if (f == 32)
                    ql_verify(ql, 2, 33, 32, 1);
                /* ltrim 3 3 (keep [3,3] inclusive = 1 remaining) */
                quicklistDelRange(ql, 0, 3);
                quicklistDelRange(ql, -29,
                                  4000); /* make sure not loop forever */
                if (f == 32)
                    ql_verify(ql, 1, 1, 1, 1);
                quicklistEntry entry;
                quicklistIndex(ql, 0, &entry);
                if (entry.longval != -5157318210846258173)
                    ERROR;
                else
                    OK;
                quicklistRelease(ql);
            }
        }

        for (int f = optimize_start; f < 40; f++) {
            TEST_DESC("ltrim test D at fill %d at compress %d", f,
                      options[_i]) {
                quicklist *ql = quicklistNew(f, options[_i]);
                char num[32];
                long long nums[5000];
                for (int i = 0; i < 33; i++) {
                    nums[i] = -5157318210846258176 + i;
                    int sz = ll2string(num, sizeof(num), nums[i]);
                    quicklistPushTail(ql, num, sz);
                }
                if (f == 32)
                    ql_verify(ql, 2, 33, 32, 1);
                quicklistDelRange(ql, -12, 3);
                if (ql->count != 30)
                    ERR("Didn't delete exactly three elements!  Count is: %lu",
                        ql->count);
                quicklistRelease(ql);
            }
        }

        for (int f = optimize_start; f < 72; f++) {
            TEST_DESC("create quicklist from ziplist at fill %d at compress %d",
                      f, options[_i]) {
                unsigned char *zl = ziplistNew();
                long long nums[64];
                char num[64];
                for (int i = 0; i < 33; i++) {
                    nums[i] = -5157318210846258176 + i;
                    int sz = ll2string(num, sizeof(num), nums[i]);
                    zl =
                        ziplistPush(zl, (unsigned char *)num, sz, ZIPLIST_TAIL);
                }
                for (int i = 0; i < 33; i++) {
                    zl = ziplistPush(zl, (unsigned char *)genstr("hello", i),
                                     32, ZIPLIST_TAIL);
                }
                quicklist *ql = quicklistCreateFromZiplist(f, options[_i], zl);
                if (f == 1)
                    ql_verify(ql, 66, 66, 1, 1);
                else if (f == 32)
                    ql_verify(ql, 3, 66, 32, 2);
                else if (f == 66)
                    ql_verify(ql, 1, 66, 66, 66);
                quicklistRelease(ql);
            }
        }

        long long stop = mstime();
        runtime[_i] = stop - start;
    }

    /* Run a longer test of compression depth outside of primary test loop. */
    int list_sizes[] = {250, 251, 500, 999, 1000};
    long long start = mstime();
    for (int list = 0; list < (int)(sizeof(list_sizes) / sizeof(*list_sizes));
         list++) {
        for (int f = optimize_start; f < 128; f++) {
            for (int depth = 1; depth < 40; depth++) {
                /* skip over many redundant test cases */
                TEST_DESC("verify specific compression of interior nodes with "
                          "%d list "
                          "at fill %d at compress %d",
                          list_sizes[list], f, depth) {
                    quicklist *ql = quicklistNew(f, depth);
                    for (int i = 0; i < list_sizes[list]; i++) {
                        quicklistPushTail(ql, genstr("hello TAIL", i + 1), 64);
                        quicklistPushHead(ql, genstr("hello HEAD", i + 1), 64);
                    }

                    quicklistNode *node = ql->head;
                    unsigned int low_raw = ql->compress;
                    unsigned int high_raw = ql->len - ql->compress;

                    for (unsigned int at = 0; at < ql->len;
                         at++, node = node->next) {
                        if (at < low_raw || at >= high_raw) {
                            if (node->encoding != QUICKLIST_NODE_ENCODING_RAW) {
                                ERR("Incorrect compression: node %d is "
                                    "compressed at depth %d ((%u, %u); total "
                                    "nodes: %u; size: %u)",
                                    at, depth, low_raw, high_raw, ql->len,
                                    node->sz);
                            }
                        } else {
                            if (node->encoding != QUICKLIST_NODE_ENCODING_LZF) {
                                ERR("Incorrect non-compression: node %d is NOT "
                                    "compressed at depth %d ((%u, %u); total "
                                    "nodes: %u; size: %u; attempted: %d)",
                                    at, depth, low_raw, high_raw, ql->len,
                                    node->sz, node->attempted_compress);
                            }
                        }
                    }
                    quicklistRelease(ql);
                }
            }
        }
    }
    long long stop = mstime();

    printf("\n");
    for (size_t i = 0; i < option_count; i++)
        printf("Test Loop %02d: %0.2f seconds.\n", options[i],
               (float)runtime[i] / 1000);
    printf("Compressions: %0.2f seconds.\n", (float)(stop - start) / 1000);
    printf("\n");

    if (!err)
        printf("ALL TESTS PASSED!\n");
    else
        ERR("Sorry, not all tests passed!  In fact, %d tests failed.", err);

    return err;
}
#endif
