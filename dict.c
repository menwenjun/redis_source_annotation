/* Hash Tables Implementation.
 *
 * This file implements in memory hash tables with insert/del/replace/find/
 * get-random-element operations. Hash tables will auto resize if needed
 * tables of power of two in size are used, collisions are handled by
 * chaining. See the source code for more information... :)
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

#include "fmacros.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <limits.h>
#include <sys/time.h>
#include <ctype.h>

#include "dict.h"
#include "zmalloc.h"
#include "redisassert.h"

/* Using dictEnableResize() / dictDisableResize() we make possible to
 * enable/disable resizing of the hash table as needed. This is very important
 * for Redis, as we use copy-on-write and don't want to move too much memory
 * around when there is a child performing saving operations.
 *
 * Note that even when dict_can_resize is set to 0, not all resizes are
 * prevented: a hash table is still allowed to grow if the ratio between
 * the number of elements and the buckets > dict_force_resize_ratio. */
static int dict_can_resize = 1;     //表示字典是否启用rehash，dictEnableResize()和dictDisableResize()可以修改该变量
static unsigned int dict_force_resize_ratio = 5;    //强制进行rehash的比例 used/size 如果大于dict_force_resize_ratio就会强制触发rehash进行扩大哈希表的操作

/* -------------------------- private prototypes 私有方法---------------------------- */

static int _dictExpandIfNeeded(dict *ht);                   //将字典ht扩展
static unsigned long _dictNextPower(unsigned long size);    //计算一个大于等于 size 的 2 的 N 次方，用作哈希表的值
static int _dictKeyIndex(dict *ht, const void *key);        //返回可以将 key 插入到哈希表的索引位置
static int _dictInit(dict *ht, dictType *type, void *privDataPtr);  //初始化哈希表

/* -------------------------- hash functions -哈希函数计算哈希值------------------------------- */
//Thomas Wang认为好的hash函数具有两个好的特点
//1. hash函数是可逆的。
//2. 具有雪崩效应，意思是，输入值1bit位的变化会造成输出值1/2的bit位发生变化
/* Thomas Wang's 32 bit Mix Function */
unsigned int dictIntHashFunction(unsigned int key)      //用于计算int整型哈希值的哈希函数
{
    key += ~(key << 15);
    key ^=  (key >> 10);
    key +=  (key << 3);
    key ^=  (key >> 6);
    key += ~(key << 11);
    key ^=  (key >> 16);
    return key;
}

static uint32_t dict_hash_function_seed = 5381;     //哈希函数的种子

void dictSetHashFunctionSeed(uint32_t seed) {       //设置哈希函数的种子
    dict_hash_function_seed = seed;
}

uint32_t dictGetHashFunctionSeed(void) {            //返回哈希函数的种子
    return dict_hash_function_seed;
}

/* MurmurHash2, by Austin Appleby
 * Note - This code makes a few assumptions about how your machine behaves -
 * 1. We can read a 4-byte value from any address without crashing
 * 2. sizeof(int) == 4
 *
 * And it has a few limitations -
 *
 * 1. It will not work incrementally.
 * 2. It will not produce the same results on little-endian and big-endian
 *    machines.
 */
/*
MurmurHash是一种很出名的非加密型哈希函数，适用于一般的哈希检索操作。目前有三个版本（MurmurHash1、MurmurHash2、MurmurHash3）。最新的是MurmurHash3，可以产生出32-bit或128-bit哈希值。redis中应用的是MurmurHash2，能产生32-bit或64-bit哈希值。
*/

unsigned int dictGenHashFunction(const void *key, int len) {    //用于计算字符串的哈希值的哈希函数
    /* 'm' and 'r' are mixing constants generated offline.
     They're not really 'magic', they just happen to work well.  */
    //m和r这两个值用于计算哈希值，只是因为效果好。
    uint32_t seed = dict_hash_function_seed;
    const uint32_t m = 0x5bd1e995;
    const int r = 24;

    /* Initialize the hash to a 'random' value */
    uint32_t h = seed ^ len;    //初始化

    /* Mix 4 bytes at a time into the hash */
    const unsigned char *data = (const unsigned char *)key;

    //将字符串key每四个一组看成uint32_t类型，进行运算的到h
    while(len >= 4) {
        uint32_t k = *(uint32_t*)data;

        k *= m;
        k ^= k >> r;
        k *= m;

        h *= m;
        h ^= k;

        data += 4;
        len -= 4;
    }

    /* Handle the last few bytes of the input array  */
    switch(len) {
    case 3: h ^= data[2] << 16;
    case 2: h ^= data[1] << 8;
    case 1: h ^= data[0]; h *= m;
    };

    /* Do a few final mixes of the hash to ensure the last few
     * bytes are well-incorporated. */
    h ^= h >> 13;
    h *= m;
    h ^= h >> 15;

    return (unsigned int)h;
}

/* And a case insensitive hash function (based on djb hash) */
//djb哈希算法，算法的思想是利用字符串中的ascii码值与一个随机seed，通过len次变换，得到最后的hash值。
unsigned int dictGenCaseHashFunction(const unsigned char *buf, int len) {   //用于计算字符串的哈希值的哈希函数
    unsigned int hash = (unsigned int)dict_hash_function_seed;

    while (len--)
        hash = ((hash << 5) + hash) + (tolower(*buf++)); /* hash * 33 + c */
    return hash;
}

/* ----------------------------- API implementation ------------------------- */

/* Reset a hash table already initialized with ht_init().
 * NOTE: This function should only be called by ht_destroy(). */
static void _dictReset(dictht *ht)  //重置哈希表ht的成员，被ht_destroy()函数调用
{
    ht->table = NULL;
    ht->size = 0;
    ht->sizemask = 0;
    ht->used = 0;
}

/* Create a new hash table */
dict *dictCreate(dictType *type,    //创建一个新的hash表
        void *privDataPtr)
{
    dict *d = zmalloc(sizeof(*d));  //分配内存

    _dictInit(d,type,privDataPtr);  //初始化
    return d;
}

/* Initialize the hash table */
int _dictInit(dict *d, dictType *type,  //初始化字典d的hash表
        void *privDataPtr)
{
    _dictReset(&d->ht[0]);      //初始化ht[2]的两张hash表
    _dictReset(&d->ht[1]);
    //初始化字典d结构各成员
    d->type = type;             //特定类型函数的结构体指针
    d->privdata = privDataPtr;  //私有数据指针
    d->rehashidx = -1;          //rehash的状态 -1表示没有进行rehash
    d->iterators = 0;           //正在迭代的迭代器数量
    return DICT_OK;             //初始化成功返回DICT_OK

/* Resize the table to the minimal size that contains all the elements,
 * but with the invariant of a USED/BUCKETS ratio near to <= 1 */
// 让used/size之间的比率接近1:1
int dictResize(dict *d)     //缩小字典d
{
    int minimal;

    //如果dict_can_resize被设置成0，表示不能进行rehash，或正在进行rehash，返回出错标志DICT_ERR
    if (!dict_can_resize || dictIsRehashing(d)) return DICT_ERR;

    minimal = d->ht[0].used;            //获得已经有的节点数量作为最小限度minimal
    if (minimal < DICT_HT_INITIAL_SIZE) //但是minimal不能小于最低值DICT_HT_INITIAL_SIZE（4）
        minimal = DICT_HT_INITIAL_SIZE;
    return dictExpand(d, minimal);      //用minimal调整字典d的大小
}

/* Expand or create the hash table */
int dictExpand(dict *d, unsigned long size)     //根据size调整或创建字典d的哈希表
{
    dictht n; /* the new hash table */
    unsigned long realsize = _dictNextPower(size);  //获得一个最接近2的倍数的realsize

    /* the size is invalid if it is smaller than the number of
     * elements already inside the hash table */
    if (dictIsRehashing(d) || d->ht[0].used > size) //正在rehash或size不够大返回出错标志
        return DICT_ERR;

    /* Rehashing to the same table size is not useful. */
    if (realsize == d->ht[0].size) return DICT_ERR; //如果新的realsize和原本的size一样则返回出错标志

    /* Allocate the new hash table and initialize all pointers to NULL */
    //初始化新的哈希表的成员
    n.size = realsize;
    n.sizemask = realsize-1;
    n.table = zcalloc(realsize*sizeof(dictEntry*));
    n.used = 0;

    /* Is this the first initialization? If so it's not really a rehashing
     * we just set the first hash table so that it can accept keys. */
    if (d->ht[0].table == NULL) {   //如果ht[0]哈希表为空，则将新的哈希表n设置为ht[0]
        d->ht[0] = n;
        return DICT_OK;
    }

    /* Prepare a second hash table for incremental rehashing */
    d->ht[1] = n;           //如果ht[0]非空，则需要rehash
    d->rehashidx = 0;       //设置rehash标志位为0，开始渐进式rehash（incremental rehashing）
    return DICT_OK;
}

/* Performs N steps of incremental rehashing. Returns 1 if there are still
 * keys to move from the old to the new hash table, otherwise 0 is returned.
 * 执行n步渐进式rehash
 * 如果还有keys要从ht[0]移到ht[1] 返回1，否则返回0，表示所有key已经移动完毕
 *
 * 每一步rehash都是以一个哈希表的索引作为单位，而这一个索引可能会有多个节点
 * Note that a rehashing step consists in moving a bucket (that may have more
 * than one key as we use chaining) from the old to the new hash table, however
 * since part of the hash table may be composed of empty spaces, it is not
 * guaranteed that this function will rehash even a single bucket, since it
 * will visit at max N*10 empty buckets in total, otherwise the amount of
 * work it does would be unbound and the function may block for a long time. */

int dictRehash(dict *d, int n) {       //n步进行渐进式rehash
    int empty_visits = n*10; /* Max number of empty buckets to visit. */
    if (!dictIsRehashing(d)) return 0;  //只有rehashidx不等于-1时，才表示正在进行rehash，否则返回0

    while(n-- && d->ht[0].used != 0) {  //分n步，而且ht[0]上还有没有移动的节点
        dictEntry *de, *nextde;

        /* Note that rehashidx can't overflow as we are sure there are more
         * elements because ht[0].used != 0 */
        //确保rehashidx没有越界，因为rehashidx是从-1开始，0表示已经移动1个节点，它总是小于hash表的size的
        assert(d->ht[0].size > (unsigned long)d->rehashidx);

        //第一个循环用来更新 rehashidx 的值，因为有些桶为空，所以 rehashidx并非每次都比原来前进一个位置，而是有可能前进几个位置，但最多不超过 10。
        //将rehashidx移动到ht[0]有节点的下标，也就是table[d->rehashidx]非空
        while(d->ht[0].table[d->rehashidx] == NULL) {
            d->rehashidx++;
            if (--empty_visits == 0) return 1;
        }
        de = d->ht[0].table[d->rehashidx];     //ht[0]下标为rehashidx有节点，得到该节点的地址
        /* Move all the keys in this bucket from the old to the new hash HT */
        //第二个循环用来将ht[0]表中每次找到的非空桶中的链表（或者就是单个节点）拷贝到ht[1]中
        while(de) {
            unsigned int h;

            nextde = de->next;  //备份下一个节点的地址
            /* Get the index in the new hash table */
            h = dictHashKey(d, de->key) & d->ht[1].sizemask;    //获得计算哈希值并得到哈希表中的下标h

            //将该节点插入到下标为h的位置
            de->next = d->ht[1].table[h];
            d->ht[1].table[h] = de;

            //更新两个表节点数目计数器
            d->ht[0].used--;
            d->ht[1].used++;

            //将de指向以一个处理的节点
            de = nextde;
        }
        d->ht[0].table[d->rehashidx] = NULL;    //迁移过后将该下标的指针置为空
        d->rehashidx++;                         //更新rehashidx
    }

    /* Check if we already rehashed the whole table... */
    if (d->ht[0].used == 0) {           //ht[0]上已经没有节点了，说明已经迁移完成
        zfree(d->ht[0].table);          //释放hash表内存
        d->ht[0] = d->ht[1];            //将迁移过的1号哈希表设置为0号哈希表
        _dictReset(&d->ht[1]);          //重置ht[1]哈希表
        d->rehashidx = -1;              //rehash标志关闭
        return 0;                       //表示前已完成
    }

    /* More to rehash... */
    return 1;           //表示还有节点等待迁移
}

long long timeInMilliseconds(void) {        //返回以毫秒为单位的Unix时间戳
    struct timeval tv;

    gettimeofday(&tv,NULL);
    return (((long long)tv.tv_sec)*1000)+(tv.tv_usec/1000);
}

/* Rehash for an amount of time between ms milliseconds and ms+1 milliseconds */
int dictRehashMilliseconds(dict *d, int ms) {  //在ms毫秒内，以100步为单位，对字典d进行rehash
    long long start = timeInMilliseconds();
    int rehashes = 0;   //记录rehash到哪一步

    while(dictRehash(d,100)) {
        rehashes += 100;
        if (timeInMilliseconds()-start > ms) break;
    }
    return rehashes;
}

/* This function performs just a step of rehashing, and only if there are
 * no safe iterators bound to our hash table. When we have iterators in the
 * middle of a rehashing we can't mess with the two hash tables otherwise
 * some element can be missed or duplicated.
 * 在字典不存在安全迭代器的情况下，对字典进行单步 rehash 。
 * 字典有安全迭代器的情况下不能进行 rehash ，因为两种不同的迭代和修改操作可能会弄乱字典。
 * This function is called by common lookup or update operations in the
 * dictionary so that the hash table automatically migrates from H1 to H2
 * while it is actively used. */
static void _dictRehashStep(dict *d) {
    if (d->iterators == 0) dictRehash(d,1); //没有迭代器，进行1步rehash
}

/* Add an element to the target hash table */
int dictAdd(dict *d, void *key, void *val)  //往字典d中添加一个key-value
{
    dictEntry *entry = dictAddRaw(d,key);  //在字典中创建一个键为key的哈希节点并返回该节点地址

    if (!entry) return DICT_ERR;
    dictSetVal(d, entry, val);          //设置该节点的val
    return DICT_OK;                     //成功添加该节点
}

/* Low level add. This function adds the entry but instead of setting
 * a value returns the dictEntry structure to the user, that will make
 * sure to fill the value field as he wishes.
 *
 * This function is also directly exposed to the user API to be called
 * mainly in order to store non-pointers inside the hash value, example:
 *
 * entry = dictAddRaw(dict,mykey);
 * if (entry != NULL) dictSetSignedIntegerVal(entry,1000);
 *
 * Return values:
 *
 * If key already exists NULL is returned.
 * If key was added, the hash entry is returned to be manipulated by the caller.
 */
dictEntry *dictAddRaw(dict *d, void *key)   //将key插入到字典d的新创建的节点上
{
    int index;
    dictEntry *entry;
    dictht *ht;

    if (dictIsRehashing(d)) _dictRehashStep(d); //如果正在进行rehash，则进行1步rehash

    /* Get the index of the new element, or -1 if
     * the element already exists. */
    if ((index = _dictKeyIndex(d, key)) == -1)//根据key计算哈希表的下标，如果已经存在则返回-1
        return NULL;

    /* Allocate the memory and store the new entry */
    ht = dictIsRehashing(d) ? &d->ht[1] : &d->ht[0];    //如果正在进行rehash，则直接将key添加到ht[1]表上，否则添加到ht[0]上。
    entry = zmalloc(sizeof(*entry));    //分配节点内存

    //将节点插在链表的表头
    entry->next = ht->table[index];
    ht->table[index] = entry;
    ht->used++;     //更新哈希表节点计数器

    /* Set the hash entry fields. */
    dictSetKey(d, entry, key);  //将entry节点的键值设置为key
    return entry;           //返回节点地址
}

/* Add an element, discarding the old if the key already exists.
 * Return 1 if the key was added from scratch, 0 if there was already an
 * element with such key and dictReplace() just performed a value update
 * operation. */
int dictReplace(dict *d, void *key, void *val)  //替换键为key的value
{
    dictEntry *entry, auxentry;

    /* Try to add the element. If the key
     * does not exists dictAdd will suceed. */
    if (dictAdd(d, key, val) == DICT_OK)        //如果key不存在则添加key，返回1
        return 1;
    /* It already exists, get the entry */
    entry = dictFind(d, key);               //找到键为key的节点地址
    /* Set the new value and free the old one. Note that it is important
     * to do that in this order, as the value may just be exactly the same
     * as the previous one. In this context, think to reference counting,
     * you want to increment (set), and then decrement (free), and not the
     * reverse. */
    auxentry = *entry;              //备份节点中的value的地址
    dictSetVal(d, entry, val);      //设置新的value
    dictFreeVal(d, &auxentry);      //释放value的空间
    return 0;
}

/* dictReplaceRaw() is simply a version of dictAddRaw() that always
 * returns the hash entry of the specified key, even if the key already
 * exists and can't be added (in that case the entry of the already
 * existing key is returned.)
 *
 * See dictAddRaw() for more information. */
dictEntry *dictReplaceRaw(dict *d, void *key) { //仅仅添加一个节点，与HSETNX命令类似
    dictEntry *entry = dictFind(d,key);         //返回键为key的节点

    return entry ? entry : dictAddRaw(d,key); //找到直接返回，没找到则添加该键并返回该节点地址
}

/* Search and remove an element */
static int dictGenericDelete(dict *d, const void *key, int nofree)//查找并删除一个键为key的节点，nofree表示是否调用key和value的释放函数
{
    unsigned int h, idx;
    dictEntry *he, *prevHe;
    int table;

    if (d->ht[0].size == 0) return DICT_ERR; /* d->ht[0].table is NULL */
    if (dictIsRehashing(d)) _dictRehashStep(d); //如果正在rehash，则进行1步rehash
    h = dictHashKey(d, key);                    //计算哈希值

    for (table = 0; table <= 1; table++) {      //遍历两个哈希表
        idx = h & d->ht[table].sizemask;   //计算下标值
        he = d->ht[table].table[idx];      //获得链表头节点的地址
        prevHe = NULL;
        while(he) {       //遍历链表
            if (dictCompareKeys(d, key, he->key)) {     //查找目标key的节点
                /* Unlink the element from the list */
                if (prevHe)
                    prevHe->next = he->next;
                else
                    d->ht[table].table[idx] = he->next;     //跳过he节点，删除
                if (!nofree) {              //如果nofree为0，则调用释放函数
                    dictFreeKey(d, he);
                    dictFreeVal(d, he);
                }
                zfree(he);      //释放节点空间
                d->ht[table].used--;    //更新节点数量计数器
                return DICT_OK;         //删除节点成功
            }
            prevHe = he;        //没找到则指向链表的下一个节点
            he = he->next;
        }
        //如果执行到这里，说明在 0 号哈希表中找不到key,如果没有正在进行rehash，则跳出循环体，否则在1好哈希表中寻找key
        if (!dictIsRehashing(d)) break;
    }
    return DICT_ERR; /* not found */
}

int dictDelete(dict *ht, const void *key) {     //从ht字典中删除键为key的节点，并删除键值对
    return dictGenericDelete(ht,key,0);
}

int dictDeleteNoFree(dict *ht, const void *key) {  //从ht字典中删除键为key的节点，但不删除键值对
    return dictGenericDelete(ht,key,1);
}

/* Destroy an entire dictionary */
int _dictClear(dict *d, dictht *ht, void(callback)(void *)) {   //删除哈希表上的所有节点，并重置哈希表的各项属性
    unsigned long i;

    /* Free all the elements */
    for (i = 0; i < ht->size && ht->used > 0; i++) {    //遍历哈希表
        dictEntry *he, *nextHe;

        if (callback && (i & 65535) == 0) callback(d->privdata);    //调用callback函数对私有数据进行操作

        if ((he = ht->table[i]) == NULL) continue;  //如果当前下标为空则跳过本层循环
        while(he) {                 //遍历整个链表
            nextHe = he->next;      //跳过当前键，删除
            dictFreeKey(d, he);     //释放key和value空间
            dictFreeVal(d, he);
            zfree(he);              //释放节点空间
            ht->used--;             //更新节点计数器
            he = nextHe;            //指向链表的下一个节点
        }
    }
    /* Free the table and the allocated cache structure */
    zfree(ht->table);   //释放哈希表
    /* Re-initialize the table */
    _dictReset(ht);         //重置哈希表的各个成员
    return DICT_OK; /* never fails */
}

/* Clear & Release the hash table */
void dictRelease(dict *d)           //删除释放字典d的哈希表
{
    _dictClear(d,&d->ht[0],NULL);       //删除并清空两张表
    _dictClear(d,&d->ht[1],NULL);
    zfree(d);                       //释放字典的空间
}

dictEntry *dictFind(dict *d, const void *key)   //返回字典d中键值为key的节点地址
{
    dictEntry *he;
    unsigned int h, idx, table;

    if (d->ht[0].size == 0) return NULL; /* We don't have a table at all */
    if (dictIsRehashing(d)) _dictRehashStep(d); //如果正在进行rehash，则进行一步rehash
    h = dictHashKey(d, key);        //计算哈希值
    for (table = 0; table <= 1; table++) {      //遍历哈希表
        idx = h & d->ht[table].sizemask;        //计算哈希表下标
        he = d->ht[table].table[idx];           //根据下标获得链表头结点地址
        while(he) {         //遍历链表
            if (dictCompareKeys(d, key, he->key))   //比较key，相等则返回
                return he;
            he = he->next;
        }
        if (!dictIsRehashing(d)) return NULL;   //如果正在进行rehash则需要在ht[1]中查找
    }
    return NULL;
}

void *dictFetchValue(dict *d, const void *key) {    //返回字典d中键为key的value
    dictEntry *he;

    he = dictFind(d,key);
    return he ? dictGetVal(he) : NULL;
}

/* A fingerprint is a 64 bit number that represents the state of the dictionary
 * at a given time, it's just a few dict properties xored together.
 * When an unsafe iterator is initialized, we get the dict fingerprint, and check
 * the fingerprint again when the iterator is released.
 * If the two fingerprints are different it means that the user of the iterator
 * performed forbidden operations against the dictionary while iterating. */
//一个fingerprint为一个64位数值,用以表示某个时刻dict的状态,它由dict的一些属性通过位操作计算得
//到，当一个不安全的迭代器被初始化，我们就会得到该字典的fingerprint，并且在迭代器被释放时再一次检查fingerprint
//如果两个fingerprints不同，这意味着这两个迭代器的user在进行字典迭代时执行了非法操作
long long dictFingerprint(dict *d) {
    long long integers[6], hash = 0;    //6个元素的整型数组
    int j;

    //分别是两张表的表地址，表的大小，表节点个数
    integers[0] = (long) d->ht[0].table;
    integers[1] = d->ht[0].size;
    integers[2] = d->ht[0].used;
    integers[3] = (long) d->ht[1].table;
    integers[4] = d->ht[1].size;
    integers[5] = d->ht[1].used;

    /* We hash N integers by summing every successive integer with the integer
     * hashing of the previous sum. Basically:
     *
     * Result = hash(hash(hash(int1)+int2)+int3) ...
     *
     * This way the same set of integers in a different order will (likely) hash
     * to a different number. */
    for (j = 0; j < 6; j++) {
        hash += integers[j];
        /* For the hashing step we use Tomas Wang's 64 bit integer hash. */
        hash = (~hash) + (hash << 21); // hash = (hash << 21) - hash - 1;
        hash = hash ^ (hash >> 24);
        hash = (hash + (hash << 3)) + (hash << 8); // hash * 265
        hash = hash ^ (hash >> 14);
        hash = (hash + (hash << 2)) + (hash << 4); // hash * 21
        hash = hash ^ (hash >> 28);
        hash = hash + (hash << 31);
    }
    return hash;    //返回hash值
}

dictIterator *dictGetIterator(dict *d)  //创建并返回一个不安全的迭代器
{
    dictIterator *iter = zmalloc(sizeof(*iter));

    iter->d = d;
    iter->table = 0;
    iter->index = -1;
    iter->safe = 0;         //不安全标志
    iter->entry = NULL;
    iter->nextEntry = NULL;
    return iter;
}

dictIterator *dictGetSafeIterator(dict *d) {    //创建并返回一个安全迭代器
    dictIterator *i = dictGetIterator(d);

    i->safe = 1;        //设置为安全标志
    return i;
}

dictEntry *dictNext(dictIterator *iter)     //返回迭代器指向的当前节点
{
    while (1) {     //这个循环的目的是：跳过哈希表下标为NULL的桶
        if (iter->entry == NULL) {      //当前指向的节点为空
            dictht *ht = &iter->d->ht[iter->table]; //得到迭代器所迭代的哈希表的地址
            //第一次迭代执行，迭代器迭代ht[0]哈希表且哈希表的下标为-1
            if (iter->index == -1 && iter->table == 0) {
                if (iter->safe)         //如果迭代器安全，那么更新安全迭代器的计数器
                    iter->d->iterators++;
                else        //不安全则计算指纹fingerprint
                    iter->fingerprint = dictFingerprint(iter->d);
            }
            iter->index++;  //更新哈希表的下标,默认值为-1，自加后刚好为0，即第一个桶

            //如果哈希表的下标大于等于哈希表的大小，则该表已经迭代完毕
            if (iter->index >= (long) ht->size) {
                if (dictIsRehashing(iter->d) && iter->table == 0) { //是否迭代ht[1]号表
                    iter->table++;
                    iter->index = 0;
                    ht = &iter->d->ht[1];   //更新要迭代的哈希表地址
                } else {
                    break;
                }
            }
            iter->entry = ht->table[iter->index];   //得到哈希表下一个下标的链表头结点地址
        } else {
            iter->entry = iter->nextEntry;  //将指针指向下一个节点
        }
        if (iter->entry) {  //如果当前节点非空，备份当前节点的下一个节点
            /* We need to save the 'next' here, the iterator user
             * may delete the entry we are returning. */
            iter->nextEntry = iter->entry->next;
            return iter->entry; //返回当前节点的地址
        }
    }
    return NULL;
}

void dictReleaseIterator(dictIterator *iter)    //释放字典的迭代器
{
    if (!(iter->index == -1 && iter->table == 0)) {
        if (iter->safe)     //释放安全迭代器，安全迭代器计数器-1
            iter->d->iterators--;
        else
            assert(iter->fingerprint == dictFingerprint(iter->d));//确保指纹相等
    }
    zfree(iter);
}

/* Return a random entry from the hash table. Useful to
 * implement randomized algorithms */
dictEntry *dictGetRandomKey(dict *d)    //随机返回字典中的任意一个节点
{
    dictEntry *he, *orighe;
    unsigned int h;
    int listlen, listele;

    if (dictSize(d) == 0) return NULL;  //字典大小为0，返回空
    if (dictIsRehashing(d)) _dictRehashStep(d); //如果正在rehash，则进行1步rehash
    if (dictIsRehashing(d)) {       //如果正在rehash，则将1号哈希表作为随机查找的目标
        do {
            /* We are sure there are no elements in indexes from 0
             * to rehashidx-1 */
            h = d->rehashidx + (random() % (d->ht[0].size +
                                            d->ht[1].size -
                                            d->rehashidx));
            he = (h >= d->ht[0].size) ? d->ht[1].table[h - d->ht[0].size] :
                                      d->ht[0].table[h];
        } while(he == NULL);    //如果指向空，则在随机取一次
    } else {        //否则从第0号表中查找节点
        do {
            h = random() & d->ht[0].sizemask;
            he = d->ht[0].table[h];
        } while(he == NULL);    //如果指向空，则在随机取一次
    }

    /* Now we found a non empty bucket, but it is a linked
     * list and we need to get a random element from the list.
     * The only sane way to do so is counting the elements and
     * select a random index. */
    //此时he指向一个节点或是一条链表
    listlen = 0;
    orighe = he;
    while(he) {     //如果是链表，计算出链表长度
        he = he->next;
        listlen++;
    }
    listele = random() % listlen;
    he = orighe;
    while(listele--) he = he->next; //从链表中随机取出一个节点，并返回
    return he;
}

/* This function samples the dictionary to return a few keys from random
 * locations.
 *
 * It does not guarantee to return all the keys specified in 'count', nor
 * it does guarantee to return non-duplicated elements, however it will make
 * some effort to do both things.
 *
 * Returned pointers to hash table entries are stored into 'des' that
 * points to an array of dictEntry pointers. The array must have room for
 * at least 'count' elements, that is the argument we pass to the function
 * to tell how many random elements we need.
 *
 * The function returns the number of items stored into 'des', that may
 * be less than 'count' if the hash table has less than 'count' elements
 * inside, or if not enough elements were found in a reasonable amount of
 * steps.
 *
 * Note that this function is not suitable when you need a good distribution
 * of the returned items, but only when you need to "sample" a given number
 * of continuous elements to run some kind of algorithm or to produce
 * statistics. However the function is much faster than dictGetRandomKey()
 * at producing N elements. */
unsigned int dictGetSomeKeys(dict *d, dictEntry **des, unsigned int count) {//返回count个key，并且将地址存在des数组中
    unsigned int j; /* internal hash table id, 0 or 1. */   //哈希表0或1
    unsigned int tables; /* 1 or 2 tables? */   //
    unsigned int stored = 0, maxsizemask;
    unsigned int maxsteps;

    if (dictSize(d) < count) count = dictSize(d);   //count小于等于字典的总节点数
    maxsteps = count*10;

    /* Try to do a rehashing work proportional to 'count'. */
    for (j = 0; j < count; j++) {
        if (dictIsRehashing(d))
            _dictRehashStep(d);     //单步rehash count次
        else
            break;
    }

    tables = dictIsRehashing(d) ? 2 : 1; //如果正在rehash，则对两个tables进行操作，否则对一个
    maxsizemask = d->ht[0].sizemask;
    if (tables > 1 && maxsizemask < d->ht[1].sizemask)
        maxsizemask = d->ht[1].sizemask;    //如果是对两个表，则要更新maxsizemask

    /* Pick a random point inside the larger table. */
    unsigned int i = random() & maxsizemask;
    unsigned int emptylen = 0; /* Continuous empty entries so far. */
    while(stored < count && maxsteps--) {   //没找够，则继续找
        for (j = 0; j < tables; j++) {      //对两个表遍历
            /* Invariant of the dict.c rehashing: up to the indexes already
             * visited in ht[0] during the rehashing, there are no populated
             * buckets, so we can skip ht[0] for indexes between 0 and idx-1. */
            if (tables == 2 && j == 0 && i < (unsigned int) d->rehashidx) {
                /* Moreover, if we are currently out of range in the second
                 * table, there will be no elements in both tables up to
                 * the current rehashing index, so we jump if possible.
                 * (this happens when going from big to small table). */
                if (i >= d->ht[1].size) i = d->rehashidx;
                continue;
            }
            if (i >= d->ht[j].size) continue; /* Out of range for this table. */
            dictEntry *he = d->ht[j].table[i];

            /* Count contiguous empty buckets, and jump to other
             * locations if they reach 'count' (with a minimum of 5). */
            if (he == NULL) {
                emptylen++;
                if (emptylen >= 5 && emptylen > count) {    //找了多次还没找到
                    i = random() & maxsizemask;     //重新设置maxsizemask
                    emptylen = 0;
                }
            } else {
                emptylen = 0;
                while (he) {
                    /* Collect all the elements of the buckets found non
                     * empty while iterating. */
                    *des = he;      //存储找到了节点
                    des++;
                    he = he->next;  //指向下一个节点
                    stored++;
                    if (stored == count) return stored; //找够了返回数量
                }
            }
        }
        i = (i+1) & maxsizemask;
    }
    return stored;  //找够了返回数量
}

/* Function to reverse bits. Algorithm from:
 * http://graphics.stanford.edu/~seander/bithacks.html#ReverseParallel */
static unsigned long rev(unsigned long v) { //翻转
    unsigned long s = 8 * sizeof(v); // 位数bit size; must be power of 2
    unsigned long mask = ~0;    //s个位全为1
    while ((s >>= 1) > 0) {
        mask ^= (mask << s);
        v = ((v >> s) & mask) | ((v << s) & ~mask);
    }
    return v;
}


/* dictScan() is used to iterate over the elements of a dictionary.
 *
 * dictScan() 函数用于迭代给定字典中的元素。
 *
 * Iterating works in the following way:
 *
 * 迭代按以下方式执行：
 *
 * 1) Initially you call the function using a cursor (v) value of 0.
 *    一开始，你使用 0 作为游标来调用函数。
 * 2) The function performs one step of the iteration, and returns the
 *    new cursor value that you must use in the next call.
 *    函数执行一步迭代操作，
 *    并返回一个下次迭代时使用的新游标。
 * 3) When the returned cursor is 0, the iteration is complete.
 *    当函数返回的游标为 0 时，迭代完成。
 *
 * The function guarantees that all the elements that are present in the
 * dictionary from the start to the end of the iteration are returned.
 * However it is possible that some element is returned multiple time.
 *
 * 函数保证，在迭代从开始到结束期间，一直存在于字典的元素肯定会被迭代到，
 * 但一个元素可能会被返回多次。
 *
 * For every element returned, the callback 'fn' passed as argument is
 * called, with 'privdata' as first argument and the dictionar entry
 * 'de' as second argument.
 *
 * 每当一个元素被返回时，回调函数 fn 就会被执行，
 * fn 函数的第一个参数是 privdata ，而第二个参数则是字典节点 de 。
 *
 * HOW IT WORKS.
 * 工作原理
 *
 * The algorithm used in the iteration was designed by Pieter Noordhuis.
 * The main idea is to increment a cursor starting from the higher order
 * bits, that is, instead of incrementing the cursor normally, the bits
 * of the cursor are reversed, then the cursor is incremented, and finally
 * the bits are reversed again.
 *
 * 迭代所使用的算法是由 Pieter Noordhuis 设计的，
 * 算法的主要思路是在二进制高位上对游标进行加法计算
 * 也即是说，不是按正常的办法来对游标进行加法计算，
 * 而是首先将游标的二进制位翻转（reverse）过来，
 * 然后对翻转后的值进行加法计算，
 * 最后再次对加法计算之后的结果进行翻转。
 *
 * This strategy is needed because the hash table may be resized from one
 * call to the other call of the same iteration.
 *
 * 这一策略是必要的，因为在一次完整的迭代过程中，
 * 哈希表的大小有可能在两次迭代之间发生改变。
 *
 * dict.c hash tables are always power of two in size, and they
 * use chaining, so the position of an element in a given table is given
 * always by computing the bitwise AND between Hash(key) and SIZE-1
 * (where SIZE-1 is always the mask that is equivalent to taking the rest
 *  of the division between the Hash of the key and SIZE).
 *
 * 哈希表的大小总是 2 的某个次方，并且哈希表使用链表来解决冲突，
 * 因此一个给定元素在一个给定表的位置总可以通过 Hash(key) & SIZE-1
 * 公式来计算得出，
 * 其中 SIZE-1 是哈希表的最大索引值，
 * 这个最大索引值就是哈希表的 mask （掩码）。
 *
 * For example if the current hash table size is 16, the mask is
 * (in binary) 1111. The position of a key in the hash table will be always
 * the last four bits of the hash output, and so forth.
 *
 * 举个例子，如果当前哈希表的大小为 16 ，
 * 那么它的掩码就是二进制值 1111 ，
 * 这个哈希表的所有位置都可以使用哈希值的最后四个二进制位来记录。
 *
 * WHAT HAPPENS IF THE TABLE CHANGES IN SIZE?
 * 如果哈希表的大小改变了怎么办？
 *
 * If the hash table grows, elements can go anyway in one multiple of
 * the old bucket: for example let's say that we already iterated with
 * a 4 bit cursor 1100, since the mask is 1111 (hash table size = 16).
 *
 * 当对哈希表进行扩展时，元素可能会从一个槽移动到另一个槽，
 * 举个例子，假设我们刚好迭代至 4 位游标 1100 ，
 * 而哈希表的 mask 为 1111 （哈希表的大小为 16 ）。
 *
 * If the hash table will be resized to 64 elements, and the new mask will
 * be 111111, the new buckets that you obtain substituting in ??1100
 * either 0 or 1, can be targeted only by keys that we already visited
 * when scanning the bucket 1100 in the smaller hash table.
 *
 * 如果这时哈希表将大小改为 64 ，那么哈希表的 mask 将变为 111111 ，
 *
 * By iterating the higher bits first, because of the inverted counter, the
 * cursor does not need to restart if the table size gets bigger, and will
 * just continue iterating with cursors that don't have '1100' at the end,
 * nor any other combination of final 4 bits already explored.
 *
 * Similarly when the table size shrinks over time, for example going from
 * 16 to 8, If a combination of the lower three bits (the mask for size 8
 * is 111) was already completely explored, it will not be visited again
 * as we are sure that, we tried for example, both 0111 and 1111 (all the
 * variations of the higher bit) so we don't need to test it again.
 *
 * WAIT... YOU HAVE *TWO* TABLES DURING REHASHING!
 * 等等。。。在 rehash 的时候可是会出现两个哈希表的阿！
 *
 * Yes, this is true, but we always iterate the smaller one of the tables,
 * testing also all the expansions of the current cursor into the larger
 * table. So for example if the current cursor is 101 and we also have a
 * larger table of size 16, we also test (0)101 and (1)101 inside the larger
 * table. This reduces the problem back to having only one table, where
 * the larger one, if exists, is just an expansion of the smaller one.
 *
 * LIMITATIONS
 * 限制
 *
 * This iterator is completely stateless, and this is a huge advantage,
 * including no additional memory used.
 * 这个迭代器是完全无状态的，这是一个巨大的优势，
 * 因为迭代可以在不使用任何额外内存的情况下进行。
 *
 * The disadvantages resulting from this design are:
 * 这个设计的缺陷在于：
 *
 * 1) It is possible that we return duplicated elements. However this is usually
 *    easy to deal with in the application level.
 *    函数可能会返回重复的元素，不过这个问题可以很容易在应用层解决。
 * 2) The iterator must return multiple elements per call, as it needs to always
 *    return all the keys chained in a given bucket, and all the expansions, so
 *    we are sure we don't miss keys moving.
 *    为了不错过任何元素，
 *    迭代器需要返回给定桶上的所有键，
 *    以及因为扩展哈希表而产生出来的新表，
 *    所以迭代器必须在一次迭代中返回多个元素。
 * 3) The reverse cursor is somewhat hard to understand at first, but this
 *    comment is supposed to help.
 *    对游标进行翻转（reverse）的原因初看上去比较难以理解，
 *    不过阅读这份注释应该会有所帮助。
 */ //可以参考http://chenzhenianqing.cn/articles/1101.html
unsigned long dictScan(dict *d,
                       unsigned long v,
                       dictScanFunction *fn,
                       void *privdata)
{
    dictht *t0, *t1;
    const dictEntry *de;
    unsigned long m0, m1;

    if (dictSize(d) == 0) return 0; //跳过空字典

    //没有进行rehash，则只有一个哈希表
    if (!dictIsRehashing(d)) {
        t0 = &(d->ht[0]);   //指向哈希表
        m0 = t0->sizemask;  //记录掩码

        /* Emit entries at cursor */
        de = t0->table[v & m0]; //指向一个桶
        while (de) {         //遍历桶中的所有节点
            fn(privdata, de);
            de = de->next;
        }

    } else {        //有两个哈希表的情况
        t0 = &d->ht[0]; //指向两个哈希表
        t1 = &d->ht[1];

        /* Make sure t0 is the smaller and t1 is the bigger table */
        if (t0->size > t1->size) {  //确保t0小于t1
            t0 = &d->ht[1];
            t1 = &d->ht[0];
        }

        m0 = t0->sizemask;      //记录掩码
        m1 = t1->sizemask;

        /* Emit entries at cursor */
        de = t0->table[v & m0]; //指向哈希桶
        while (de) {            //遍历链表
            fn(privdata, de);
            de = de->next;
        }

        /* Iterate over indices in larger table that are the expansion
         * of the index pointed to by the cursor in the smaller table */
        do {
            /* Emit entries at cursor */
            de = t1->table[v & m1]; // 指向桶，并迭代桶中的所有节点
            while (de) {
                fn(privdata, de);
                de = de->next;
            }

            /* Increment bits not covered by the smaller mask */
            v = (((v | m0) + 1) & ~m0) | (v & m0);

            /* Continue while bits covered by mask difference is non-zero */
        } while (v & (m0 ^ m1));
    }

    /* Set unmasked bits so incrementing the reversed cursor
     * operates on the masked bits of the smaller table */
    v |= ~m0;

    /* Increment the reverse cursor */
    v = rev(v);
    v++;
    v = rev(v);

    return v;
}

/* ------------------------- private functions ------------------------------ */

/* Expand the hash table if needed */
static int _dictExpandIfNeeded(dict *d) //扩展d字典，并初始化
{
    /* Incremental rehashing already in progress. Return. */
    if (dictIsRehashing(d)) return DICT_OK;     //正在进行rehash，直接返回

    /* If the hash table is empty expand it to the initial size. */
    if (d->ht[0].size == 0) return dictExpand(d, DICT_HT_INITIAL_SIZE); //如果字典（的 0 号哈希表）为空，那么创建并返回初始化大小的 0 号哈希表

    /* If we reached the 1:1 ratio, and we are allowed to resize the hash
     * table (global setting) or we should avoid it but the ratio between
     * elements/buckets is over the "safe" threshold, we resize doubling
     * the number of buckets. */
    if (d->ht[0].used >= d->ht[0].size &&   //字典已使用节点数和字典大小之间的比率接近 1：1
        (dict_can_resize ||         //能够扩展的标志为真
         d->ht[0].used/d->ht[0].size > dict_force_resize_ratio))    //已使用节点数和字典大小之间的比率超过 dict_force_resize_ratio
    {
        return dictExpand(d, d->ht[0].used*2);  //扩展为节点个数的2倍
    }
    return DICT_OK;
}

/* Our hash table capability is a power of two */
static unsigned long _dictNextPower(unsigned long size) //计算第一个大于等于 size 的 2 的 N 次方，用作哈希表的值
{
    unsigned long i = DICT_HT_INITIAL_SIZE;

    if (size >= LONG_MAX) return LONG_MAX;//size最大为LONG_MAX
    while(1) {
        if (i >= size)
            return i;
        i *= 2;
    }
}

/* Returns the index of a free slot that can be populated with
 * a hash entry for the given 'key'.
 * If the key already exists, -1 is returned.
 *
 * Note that if we are in the process of rehashing the hash table, the
 * index is always returned in the context of the second (new) hash table. */
static int _dictKeyIndex(dict *d, const void *key)  //返回键为key的在哈希表中的索引值(下标)
{
    unsigned int h, idx, table;
    dictEntry *he;

    /* Expand the hash table if needed */
    if (_dictExpandIfNeeded(d) == DICT_ERR) //插入前如果需要则扩展
        return -1;
    /* Compute the key hash value */
    h = dictHashKey(d, key);        //计算哈希值
    for (table = 0; table <= 1; table++) {
        idx = h & d->ht[table].sizemask;        //计算哈希表的下标
        /* Search if this slot does not already contain the given key */
        he = d->ht[table].table[idx];       //桶中链表的地址
        while(he) {     //遍历链表
            if (dictCompareKeys(d, key, he->key))   //如果键为key的已经存在则返回-1
                return -1;
            he = he->next;
        }
        if (!dictIsRehashing(d)) break; //是否需要遍历ht[1]
    }
    return idx;     //返回计算哈希表中的下标值
}

void dictEmpty(dict *d, void(callback)(void*)) {//清空字典上的所有哈希表节点，并重置字典属性
    _dictClear(d,&d->ht[0],callback);   //删除两个哈希表上的所有节点
    _dictClear(d,&d->ht[1],callback);
    d->rehashidx = -1;  //重置字典d的成员
    d->iterators = 0;
}

void dictEnableResize(void) {   //开启自动 rehash
    dict_can_resize = 1;
}

void dictDisableResize(void) {  //关闭自动 rehash
    dict_can_resize = 0;
}

#if 0

/* The following is code that we don't use for Redis currently, but that is part
of the library. */

/* ----------------------- Debugging ------------------------*/

#define DICT_STATS_VECTLEN 50
static void _dictPrintStatsHt(dictht *ht) {
    unsigned long i, slots = 0, chainlen, maxchainlen = 0;
    unsigned long totchainlen = 0;
    unsigned long clvector[DICT_STATS_VECTLEN];

    if (ht->used == 0) {
        printf("No stats available for empty dictionaries\n");
        return;
    }

    for (i = 0; i < DICT_STATS_VECTLEN; i++) clvector[i] = 0;
    for (i = 0; i < ht->size; i++) {
        dictEntry *he;

        if (ht->table[i] == NULL) {
            clvector[0]++;
            continue;
        }
        slots++;
        /* For each hash entry on this slot... */
        chainlen = 0;
        he = ht->table[i];
        while(he) {
            chainlen++;
            he = he->next;
        }
        clvector[(chainlen < DICT_STATS_VECTLEN) ? chainlen : (DICT_STATS_VECTLEN-1)]++;
        if (chainlen > maxchainlen) maxchainlen = chainlen;
        totchainlen += chainlen;
    }
    printf("Hash table stats:\n");
    printf(" table size: %ld\n", ht->size);
    printf(" number of elements: %ld\n", ht->used);
    printf(" different slots: %ld\n", slots);
    printf(" max chain length: %ld\n", maxchainlen);
    printf(" avg chain length (counted): %.02f\n", (float)totchainlen/slots);
    printf(" avg chain length (computed): %.02f\n", (float)ht->used/slots);
    printf(" Chain length distribution:\n");
    for (i = 0; i < DICT_STATS_VECTLEN-1; i++) {
        if (clvector[i] == 0) continue;
        printf("   %s%ld: %ld (%.02f%%)\n",(i == DICT_STATS_VECTLEN-1)?">= ":"", i, clvector[i], ((float)clvector[i]/ht->size)*100);
    }
}

void dictPrintStats(dict *d) {
    _dictPrintStatsHt(&d->ht[0]);
    if (dictIsRehashing(d)) {
        printf("-- Rehashing into ht[1]:\n");
        _dictPrintStatsHt(&d->ht[1]);
    }
}

/* ----------------------- StringCopy Hash Table Type ------------------------*/

static unsigned int _dictStringCopyHTHashFunction(const void *key)
{
    return dictGenHashFunction(key, strlen(key));
}

static void *_dictStringDup(void *privdata, const void *key)
{
    int len = strlen(key);
    char *copy = zmalloc(len+1);
    DICT_NOTUSED(privdata);

    memcpy(copy, key, len);
    copy[len] = '\0';
    return copy;
}

static int _dictStringCopyHTKeyCompare(void *privdata, const void *key1,
        const void *key2)
{
    DICT_NOTUSED(privdata);

    return strcmp(key1, key2) == 0;
}

static void _dictStringDestructor(void *privdata, void *key)
{
    DICT_NOTUSED(privdata);

    zfree(key);
}

dictType dictTypeHeapStringCopyKey = {
    _dictStringCopyHTHashFunction, /* hash function */
    _dictStringDup,                /* key dup */
    NULL,                          /* val dup */
    _dictStringCopyHTKeyCompare,   /* key compare */
    _dictStringDestructor,         /* key destructor */
    NULL                           /* val destructor */
};

/* This is like StringCopy but does not auto-duplicate the key.
 * It's used for intepreter's shared strings. */
dictType dictTypeHeapStrings = {
    _dictStringCopyHTHashFunction, /* hash function */
    NULL,                          /* key dup */
    NULL,                          /* val dup */
    _dictStringCopyHTKeyCompare,   /* key compare */
    _dictStringDestructor,         /* key destructor */
    NULL                           /* val destructor */
};

/* This is like StringCopy but also automatically handle dynamic
 * allocated C strings as values. */
dictType dictTypeHeapStringCopyKeyValue = {
    _dictStringCopyHTHashFunction, /* hash function */
    _dictStringDup,                /* key dup */
    _dictStringDup,                /* val dup */
    _dictStringCopyHTKeyCompare,   /* key compare */
    _dictStringDestructor,         /* key destructor */
    _dictStringDestructor,         /* val destructor */
};
#endif
