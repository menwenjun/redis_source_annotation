/* Hash Tables Implementation.
 *
 * This file implements in-memory hash tables with insert/del/replace/find/
 * get-random-element operations. Hash tables will auto-resize if needed
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

#include <stdint.h>

#ifndef __DICT_H
#define __DICT_H

#define DICT_OK 0       //成功代表0
#define DICT_ERR 1      //出差代表1

/* Unused arguments generate annoying warnings... */
#define DICT_NOTUSED(V) ((void) V)

typedef struct dictEntry {
    void *key;          //key
    union {
        void *val;
        uint64_t u64;
        int64_t s64;
        double d;
    } v;                //value
    struct dictEntry *next; //指向下一个hash节点，用来解决hash键冲突（collision）
} dictEntry;

typedef struct dictType {
    unsigned int (*hashFunction)(const void *key);  //计算hash值的函数
    void *(*keyDup)(void *privdata, const void *key);   //复制key的函数
    void *(*valDup)(void *privdata, const void *obj);   //复制value的函数
    int (*keyCompare)(void *privdata, const void *key1, const void *key2);//比较key的函数
    void (*keyDestructor)(void *privdata, void *key);   //销毁key的析构函数
    void (*valDestructor)(void *privdata, void *obj);   //销毁val的析构函数
} dictType;

/* This is our hash table structure. Every dictionary has two of this as we
 * implement incremental rehashing, for the old to the new table. */
typedef struct dictht { //哈希表
    dictEntry **table;  //存放一个数组的地址，数组存放着哈希表节点dictEntry的地址。
    unsigned long size; //哈希表table的大小
    unsigned long sizemask; //用于将哈希值映射到table的位置索引。它的值总是等于(size-1)。
    unsigned long used; //记录哈希表已有的节点（键值对）数量。
} dictht;

typedef struct dict {
    dictType *type; //指向dictType结构，dictType结构中包含自定义的函数，这些函数使得key和value能够存储任何类型的数据。
    void *privdata; //私有数据，保存着dictType结构中函数的参数。
    dictht ht[2];       //两张哈希表。
    long rehashidx; //rehash的标记，rehashidx==-1，表示没在进行rehash
    int iterators; //正在迭代的迭代器数量
} dict;

/* If safe is set to 1 this is a safe iterator, that means, you can call
 * dictAdd, dictFind, and other functions against the dictionary even while
 * iterating. Otherwise it is a non safe iterator, and only dictNext()
 * should be called while iterating. */
typedef struct dictIterator {
    dict *d;                    //被迭代的字典
    long index;                 //迭代器当前所指向的哈希表索引位置
    int table, safe;            //table表示正迭代的哈希表号码，ht[0]或ht[1]。safe表示这个迭代器是否安全。
    dictEntry *entry, *nextEntry;   //entry指向当前迭代的哈希表节点，nextEntry则指向当前节点的下一个节点。
    /* unsafe iterator fingerprint for misuse detection. */
    long long fingerprint;      //避免不安全迭代器的指纹标记
} dictIterator;

typedef void (dictScanFunction)(void *privdata, const dictEntry *de);   //字典扫描方法

/* This is the initial size of every hash table */
#define DICT_HT_INITIAL_SIZE     4      //初始化的哈希表大小为4

/* ------------------------------- Macros 宏指令------------------------------------*/
#define dictFreeVal(d, entry) \         //释放字典d的entry节点的value
    if ((d)->type->valDestructor) \     //如果在dictType定义了释放value的析构函数，则调用该函数释放value
        (d)->type->valDestructor((d)->privdata, (entry)->v.val)

#define dictSetVal(d, entry, _val_) do { \  //设置字典d的entry节点的value为_val_，(void *类型)
    if ((d)->type->valDup) \                //如果在dictType定义了复制value的函数valDup，则调用这个函数
        entry->v.val = (d)->type->valDup((d)->privdata, _val_); \
    else \
        entry->v.val = (_val_); \
} while(0)

#define dictSetSignedIntegerVal(entry, _val_) \     //将entry节点的value设置为有符号整数
    do { entry->v.s64 = _val_; } while(0)

#define dictSetUnsignedIntegerVal(entry, _val_) \   //将entry节点的value设置为无符号整数
    do { entry->v.u64 = _val_; } while(0)

#define dictSetDoubleVal(entry, _val_) \            //将entry节点的value设置为double类型
    do { entry->v.d = _val_; } while(0)

#define dictFreeKey(d, entry) \                     //释放字典d的entry节点的key
    if ((d)->type->keyDestructor) \     //如果在dictType定义了释放key的析构函数，则调用该函数释放
        (d)->type->keyDestructor((d)->privdata, (entry)->key)

#define dictSetKey(d, entry, _key_) do { \          //设置字典d的entry节点的key设置为_key_
    if ((d)->type->keyDup) \                //如果在dictType定义了复制key的函数keyDup，则调用这个函数
        entry->key = (d)->type->keyDup((d)->privdata, _key_); \
    else \
        entry->key = (_key_); \
} while(0)

#define dictCompareKeys(d, key1, key2) \            //比较字典d的key1和key2
    (((d)->type->keyCompare) ? \            //如果在dictType定义了keyCompare函数则调用
        (d)->type->keyCompare((d)->privdata, key1, key2) : \
        (key1) == (key2))

#define dictHashKey(d, key) (d)->type->hashFunction(key)        //计算字典d中key的哈希键
#define dictGetKey(he) ((he)->key)                              //返回he节点的key
#define dictGetVal(he) ((he)->v.val)                            //返回he节点的value
#define dictGetSignedIntegerVal(he) ((he)->v.s64)               //返回he节点的有符号整数的value
#define dictGetUnsignedIntegerVal(he) ((he)->v.u64)             //返回he节点的无符号整数的value
#define dictGetDoubleVal(he) ((he)->v.d)                        //返回he节点的双精度浮点型double的value
#define dictSlots(d) ((d)->ht[0].size+(d)->ht[1].size)          //返回字典d的哈希表大小
#define dictSize(d) ((d)->ht[0].used+(d)->ht[1].used)           //返回字典d中节点数之和
#define dictIsRehashing(d) ((d)->rehashidx != -1)               //返回字典d是否正在rehash

/* API */
dict *dictCreate(dictType *type, void *privDataPtr);    //创建一个新的hash表
int dictExpand(dict *d, unsigned long size);    //根据size调整或创建字典d的哈希表
int dictAdd(dict *d, void *key, void *val); //往字典d中添加一个key-value
dictEntry *dictAddRaw(dict *d, void *key);   //将key插入到字典d的新创建的节点上
int dictReplace(dict *d, void *key, void *val);//替换键为key的value
dictEntry *dictReplaceRaw(dict *d, void *key);//仅仅添加一个节点，与HSETNX命令类似
int dictDelete(dict *d, const void *key);//从ht字典中删除键为key的节点，并删除键值对
int dictDeleteNoFree(dict *d, const void *key);//从ht字典中删除键为key的节点，但不删除键值对
void dictRelease(dict *d);//删除释放字典d的哈希表
dictEntry * dictFind(dict *d, const void *key);//返回字典d中键值为key的节点地址
void *dictFetchValue(dict *d, const void *key);//返回字典d中键为key的value
int dictResize(dict *d);//缩小字典d
dictIterator *dictGetIterator(dict *d);//创建并返回一个不安全的迭代器
dictIterator *dictGetSafeIterator(dict *d); //创建并返回一个安全迭代器
dictEntry *dictNext(dictIterator *iter);//返回迭代器指向的当前节点
void dictReleaseIterator(dictIterator *iter);//释放字典的迭代器
dictEntry *dictGetRandomKey(dict *d);//随机返回字典中的任意一个节点
unsigned int dictGetSomeKeys(dict *d, dictEntry **des, unsigned int count);//返回count个key，并且将地址存在des数组中
void dictPrintStats(dict *d);
unsigned int dictGenHashFunction(const void *key, int len);//用于计算字符串的哈希值的哈希函数
unsigned int dictGenCaseHashFunction(const unsigned char *buf, int len);//用于计算字符串的哈希值的哈希函数
void dictEmpty(dict *d, void(callback)(void*));//清空字典上的所有哈希表节点，并重置字典属性
void dictEnableResize(void); //开启自动 rehash
void dictDisableResize(void);//关闭自动 rehash
int dictRehash(dict *d, int n);//执行n步进行渐进式rehash
int dictRehashMilliseconds(dict *d, int ms);//在ms毫秒内，以100步为单位，对字典d进行rehash
void dictSetHashFunctionSeed(unsigned int initval);//设置哈希函数的种子
unsigned int dictGetHashFunctionSeed(void);//返回哈希函数的种子
unsigned long dictScan(dict *d, unsigned long v, dictScanFunction *fn, void *privdata);//用于迭代给定字典中的元素。

/* Hash table types */、
//哈希表类型
//这三个变量定义在dict.c文件中
extern dictType dictTypeHeapStringCopyKey;
extern dictType dictTypeHeapStrings;
extern dictType dictTypeHeapStringCopyKeyValue;

#endif /* __DICT_H */
