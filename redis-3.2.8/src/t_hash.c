/*
 * Copyright (c) 2009-2012, Salvatore Sanfilippo <antirez at gmail dot com>
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

#include "server.h"
#include <math.h>

/*-----------------------------------------------------------------------------
 * Hash type API
 *----------------------------------------------------------------------------*/
/* Check the length of a number of objects to see if we need to convert a
 * ziplist to a real hash. Note that we only check string encoded objects
 * as their string length can be queried in constant time. */
// 检查一个数字对象的长度判断是否需要进行类型的转换，从ziplist转换到ht类型
// 只检查一个字符串类型长度，因为他的长度可以在常数时间内获取
void hashTypeTryConversion(robj *o, robj **argv, int start, int end) {
    int i;

    //只从OBJ_ENCODING_ZIPLIST类型转换为OBJ_ENCODING_HT
    if (o->encoding != OBJ_ENCODING_ZIPLIST) return;

    //遍历所有的数字对象
    for (i = start; i <= end; i++) {
        // 如果当前对象是字符串对象的编码且字符串长度大于了配置文件规定的ziplist最大的长度
        if (sdsEncodedObject(argv[i]) &&
            sdslen(argv[i]->ptr) > server.hash_max_ziplist_value)
        {
            //讲该对象编码转换为OBJ_ENCODING_HT
            hashTypeConvert(o, OBJ_ENCODING_HT);
            break;
        }
    }
}

/* Encode given objects in-place when the hash uses a dict. */
// 对键和值的对象尝试进行优化编码以节约内存
void hashTypeTryObjectEncoding(robj *subject, robj **o1, robj **o2) {
    // 如果当前subject对象的编码为OBJ_ENCODING_HT，则对o1对象和o2对象进行尝试优化编码
    if (subject->encoding == OBJ_ENCODING_HT) {
        if (o1) *o1 = tryObjectEncoding(*o1);
        if (o2) *o2 = tryObjectEncoding(*o2);
    }
}

/* Get the value from a ziplist encoded hash, identified by field.
 * Returns -1 when the field cannot be found. */
//从一个编码为ziplist的哈希对象中，取出对应field的值，并将值保存到vstr或vll中，没有找到field返回-1
int hashTypeGetFromZiplist(robj *o, robj *field,
                           unsigned char **vstr,
                           unsigned int *vlen,
                           long long *vll)
{
    unsigned char *zl, *fptr = NULL, *vptr = NULL;
    int ret;

    // 确保编码为OBJ_ENCODING_ZIPLIST
    serverAssert(o->encoding == OBJ_ENCODING_ZIPLIST);

    // 将field编码转换为字符串类型的两种编码EMBSTR或RAW的对象
    field = getDecodedObject(field);

    zl = o->ptr;
    // 返回头节点entry的地址，从头开始便利
    fptr = ziplistIndex(zl, ZIPLIST_HEAD);
    if (fptr != NULL) {
        // 在ziplist中查找和field值相等的entry节点，该节点对应key
        fptr = ziplistFind(fptr, field->ptr, sdslen(field->ptr), 1);
        if (fptr != NULL) {
            /* Grab pointer to the value (fptr points to the field) */
            // 找到下一个entry节点的地址，该节点对应value
            vptr = ziplistNext(zl, fptr);
            serverAssert(vptr != NULL);
        }
    }

    decrRefCount(field);    //释放field对象

    if (vptr != NULL) {
        // 将对应value节点的值保存在传入的参数中
        ret = ziplistGet(vptr, vstr, vlen, vll);
        serverAssert(ret);
        return 0;   //找到返回0
    }

    return -1;
}

/* Get the value from a hash table encoded hash, identified by field.
 * Returns -1 when the field cannot be found. */
// 从一个编码为OBJ_ENCODING_HT的哈希对象中，取出对应field的值，并将值保存到value对象中，没有找到field返回-1
int hashTypeGetFromHashTable(robj *o, robj *field, robj **value) {
    dictEntry *de;

    serverAssert(o->encoding == OBJ_ENCODING_HT);

    // 在字典中查找该field键
    de = dictFind(o->ptr, field);
    // field键不存在返回-1
    if (de == NULL) return -1;
    // 从该键中提取对应的值保存在*value中
    *value = dictGetVal(de);
    return 0;
}

/* Higher level function of hashTypeGet*() that always returns a Redis
 * object (either new or with refcount incremented), so that the caller
 * can retain a reference or call decrRefCount after the usage.
 *
 * The lower level function can prevent copy on write so it is
 * the preferred way of doing read operations. */
// 从一个哈希对象中返回field对应的值对象
robj *hashTypeGetObject(robj *o, robj *field) {
    robj *value = NULL;

    // 如果o编码是OBJ_ENCODING_ZIPLIST类型
    if (o->encoding == OBJ_ENCODING_ZIPLIST) {
        unsigned char *vstr = NULL;
        unsigned int vlen = UINT_MAX;
        long long vll = LLONG_MAX;

        // 从编码为ziplist的对象中提取对应的值保存在参数中
        if (hashTypeGetFromZiplist(o, field, &vstr, &vlen, &vll) == 0) {
            if (vstr) {
                // 如果是字符串类型，则创建一个字符串类型的对象
                value = createStringObject((char*)vstr, vlen);
            } else {
                // 如果是整数类型，则整数类型的对象转换为字符串类型
                value = createStringObjectFromLongLong(vll);
            }
        }

    // 如果o的编码是OBJ_ENCODING_HT，从字典中取值
    } else if (o->encoding == OBJ_ENCODING_HT) {
        robj *aux;

        // 从编码为HT的对象中提取对应的值保存在参数中
        if (hashTypeGetFromHashTable(o, field, &aux) == 0) {
            incrRefCount(aux);  //共享一个对象
            value = aux;
        }
    } else {
        serverPanic("Unknown hash encoding");
    }
    return value;   //返回value对象
}

/* Higher level function using hashTypeGet*() to return the length of the
 * object associated with the requested field, or 0 if the field does not
 * exist. */
// 返回field对象的值的长度
size_t hashTypeGetValueLength(robj *o, robj *field) {
    size_t len = 0;
    // 如果编码是OBJ_ENCODING_ZIPLIST类型
    if (o->encoding == OBJ_ENCODING_ZIPLIST) {
        unsigned char *vstr = NULL;
        unsigned int vlen = UINT_MAX;
        long long vll = LLONG_MAX;

        // 从ziplist中取出对象的值
        if (hashTypeGetFromZiplist(o, field, &vstr, &vlen, &vll) == 0)
            // 成功取出字符串值，保存字符串值的长度，否则计算整数值的位数保存
            len = vstr ? vlen : sdigits10(vll);
    // 如果是字典ht类型
    } else if (o->encoding == OBJ_ENCODING_HT) {
        robj *aux;

        // 从字典中取出field值的对象
        if (hashTypeGetFromHashTable(o, field, &aux) == 0)
            // 保存值对象长度
            len = stringObjectLen(aux);
    } else {
        serverPanic("Unknown hash encoding");
    }
    return len;
}

/* Test if the specified field exists in the given hash. Returns 1 if the field
 * exists, and 0 when it doesn't. */
// 判断field对象是否存在在o对象中
int hashTypeExists(robj *o, robj *field) {
    // 如果编码是OBJ_ENCODING_ZIPLIST类型
    if (o->encoding == OBJ_ENCODING_ZIPLIST) {
        unsigned char *vstr = NULL;
        unsigned int vlen = UINT_MAX;
        long long vll = LLONG_MAX;

        // 找到field对象返回1
        if (hashTypeGetFromZiplist(o, field, &vstr, &vlen, &vll) == 0) return 1;

    // 在字典中找
    } else if (o->encoding == OBJ_ENCODING_HT) {
        robj *aux;

        // 找到返回1
        if (hashTypeGetFromHashTable(o, field, &aux) == 0) return 1;
    } else {
        serverPanic("Unknown hash encoding");
    }
    return 0;
}

/* Add an element, discard the old if the key already exists.
 * Return 0 on insert and 1 on update.
 * This function will take care of incrementing the reference count of the
 * retained fields and value objects. */
// 将field-value添加到哈希对象中，返回1
// 如果field存在更新新的值，返回0
int hashTypeSet(robj *o, robj *field, robj *value) {
    int update = 0;

    // 如果是ziplist类型
    if (o->encoding == OBJ_ENCODING_ZIPLIST) {
        unsigned char *zl, *fptr, *vptr;

        // 如果field-value是整数，则解码为字符串类型
        field = getDecodedObject(field);
        value = getDecodedObject(value);

        zl = o->ptr;
        // 遍历整个ziplist，得到头entry节点的地址
        fptr = ziplistIndex(zl, ZIPLIST_HEAD);
        if (fptr != NULL) {
            // 在ziplist中查找并返回和field相等的entry节点
            fptr = ziplistFind(fptr, field->ptr, sdslen(field->ptr), 1);

            // 如果field存在
            if (fptr != NULL) {
                /* Grab pointer to the value (fptr points to the field) */
                // 返回当value的entry地址，也就是field的下一个entry
                vptr = ziplistNext(zl, fptr);
                serverAssert(vptr != NULL);
                update = 1; // 设置更新的标志

                /* Delete value */
                // 将找到的value删除
                zl = ziplistDelete(zl, &vptr);

                /* Insert new value */
                // 插入新的value节点
                zl = ziplistInsert(zl, vptr, value->ptr, sdslen(value->ptr));
            }
        }

        // 如果没有找到field
        if (!update) {
            /* Push new field/value pair onto the tail of the ziplist */
            // 讲field和value按序压入到ziplist中
            zl = ziplistPush(zl, field->ptr, sdslen(field->ptr), ZIPLIST_TAIL);
            zl = ziplistPush(zl, value->ptr, sdslen(value->ptr), ZIPLIST_TAIL);
        }
        // 更新哈希对象
        o->ptr = zl;
        // 释放临时的field-value
        decrRefCount(field);
        decrRefCount(value);

        /* Check if the ziplist needs to be converted to a hash table */
        // 在配置的条件下，如果能进行优化编码以便节约内存
        if (hashTypeLength(o) > server.hash_max_ziplist_entries)
            hashTypeConvert(o, OBJ_ENCODING_HT);

    // 如果是添加到字典
    } else if (o->encoding == OBJ_ENCODING_HT) {
        // 插入或替换字典的field-value对，插入返回1，替换返回0
        if (dictReplace(o->ptr, field, value)) { /* Insert */
            incrRefCount(field);    //如果是插入成功，则释放field
        } else { /* Update */
            update = 1;     //设置更新的标志
        }
        incrRefCount(value);    //释放value对象
    } else {
        serverPanic("Unknown hash encoding");
    }
    return update;  //更新返回1，替换返回0
}

/* Delete an element from a hash.
 * Return 1 on deleted and 0 on not found. */
// 从一个哈希对象中删除field，成功返回1，没找到field返回0
int hashTypeDelete(robj *o, robj *field) {
    int deleted = 0;

    // 从ziplist中删除
    if (o->encoding == OBJ_ENCODING_ZIPLIST) {
        unsigned char *zl, *fptr;

        // 得到字符串类型的field
        field = getDecodedObject(field);

        zl = o->ptr;
        // 遍历整个ziplist，得到头entry地址
        fptr = ziplistIndex(zl, ZIPLIST_HEAD);
        if (fptr != NULL) {
            // 查找到对应field的entry
            fptr = ziplistFind(fptr, field->ptr, sdslen(field->ptr), 1);
            if (fptr != NULL) {
                // 删除field和后一个对应value的entry
                zl = ziplistDelete(zl,&fptr);
                zl = ziplistDelete(zl,&fptr);
                // 更新哈希对象的值
                o->ptr = zl;
                deleted = 1;    //设置删除成功标志
            }
        }

        decrRefCount(field);    //释放field空间

    // 从字典中删除
    } else if (o->encoding == OBJ_ENCODING_HT) {
        // 删除成功，设置删除标志
        if (dictDelete((dict*)o->ptr, field) == C_OK) {
            deleted = 1;

            /* Always check if the dictionary needs a resize after a delete. */
            // 删除成功，则按需收缩字典大小
            if (htNeedsResize(o->ptr)) dictResize(o->ptr);
        }

    } else {
        serverPanic("Unknown hash encoding");
    }

    return deleted;
}

/* Return the number of elements in a hash. */
// 返回哈希对象中的键值对个数
unsigned long hashTypeLength(robj *o) {
    unsigned long length = ULONG_MAX;

    // 返回ziplist的entry节点个数的一半，则为一对field-value的个数
    if (o->encoding == OBJ_ENCODING_ZIPLIST) {
        length = ziplistLen(o->ptr) / 2;

    // 返回字典的大小
    } else if (o->encoding == OBJ_ENCODING_HT) {
        length = dictSize((dict*)o->ptr);
    } else {
        serverPanic("Unknown hash encoding");
    }

    return length;
}

// 返回一个初始化的哈希类型的迭代器
hashTypeIterator *hashTypeInitIterator(robj *subject) {
    // 分配空间初始化成员
    hashTypeIterator *hi = zmalloc(sizeof(hashTypeIterator));
    hi->subject = subject;
    hi->encoding = subject->encoding;

    // 根据不同的编码设置不同的成员
    if (hi->encoding == OBJ_ENCODING_ZIPLIST) {
        hi->fptr = NULL;
        hi->vptr = NULL;
    } else if (hi->encoding == OBJ_ENCODING_HT) {
        // 初始化一个字典迭代器返回给di成员
        hi->di = dictGetIterator(subject->ptr);
    } else {
        serverPanic("Unknown hash encoding");
    }

    return hi;
}

// 释放哈希类型迭代器空间
void hashTypeReleaseIterator(hashTypeIterator *hi) {
    // 如果是字典，则需要先释放字典迭代器的空间
    if (hi->encoding == OBJ_ENCODING_HT) {
        dictReleaseIterator(hi->di);
    }

    zfree(hi);
}

/* Move to the next entry in the hash. Return C_OK when the next entry
 * could be found and C_ERR when the iterator reaches the end. */
//讲哈希类型迭代器指向哈希对象中的下一个节点
int hashTypeNext(hashTypeIterator *hi) {
    // 迭代ziplist
    if (hi->encoding == OBJ_ENCODING_ZIPLIST) {
        unsigned char *zl;
        unsigned char *fptr, *vptr;

        // 备份迭代器的成员信息
        zl = hi->subject->ptr;
        fptr = hi->fptr;
        vptr = hi->vptr;

        // field的指针为空，则指向第一个entry，只在第一次执行时，初始化指针
        if (fptr == NULL) {
            /* Initialize cursor */
            serverAssert(vptr == NULL);
            fptr = ziplistIndex(zl, 0);
        } else {
            /* Advance cursor */
            // 获取value节点的下一个entry地址，即为下一个field的地址
            serverAssert(vptr != NULL);
            fptr = ziplistNext(zl, vptr);
        }
        // 迭代完毕或返回C_ERR
        if (fptr == NULL) return C_ERR;

        /* Grab pointer to the value (fptr points to the field) */
        // 保存下一个value的地址
        vptr = ziplistNext(zl, fptr);
        serverAssert(vptr != NULL);

        /* fptr, vptr now point to the first or next pair */
        // 更新迭代器的成员信息
        hi->fptr = fptr;
        hi->vptr = vptr;

    // 如果是迭代字典
    } else if (hi->encoding == OBJ_ENCODING_HT) {
        // 得到下一个字典节点的地址
        if ((hi->de = dictNext(hi->di)) == NULL) return C_ERR;
    } else {
        serverPanic("Unknown hash encoding");
    }
    return C_OK;
}

/* Get the field or value at iterator cursor, for an iterator on a hash value
 * encoded as a ziplist. Prototype is similar to `hashTypeGetFromZiplist`. */
// 从ziplist类型的哈希类型迭代器中获取对应的field或value，保存在参数中
void hashTypeCurrentFromZiplist(hashTypeIterator *hi, int what,
                                unsigned char **vstr,
                                unsigned int *vlen,
                                long long *vll)
{
    int ret;

    serverAssert(hi->encoding == OBJ_ENCODING_ZIPLIST);

    // 如果获取field
    if (what & OBJ_HASH_KEY) {
        // 保存键到参数中
        ret = ziplistGet(hi->fptr, vstr, vlen, vll);
        serverAssert(ret);
    } else {
        // 保存值到参数中
        ret = ziplistGet(hi->vptr, vstr, vlen, vll);
        serverAssert(ret);
    }
}

/* Get the field or value at iterator cursor, for an iterator on a hash value
 * encoded as a ziplist. Prototype is similar to `hashTypeGetFromHashTable`. */
//从ht字典类型的哈希类型迭代器中获取对应的field或value，保存在参数中
void hashTypeCurrentFromHashTable(hashTypeIterator *hi, int what, robj **dst) {
    serverAssert(hi->encoding == OBJ_ENCODING_HT);

    // 如果获取field
    if (what & OBJ_HASH_KEY) {
        // 保存键到参数中
        *dst = dictGetKey(hi->de);
    } else {
        // 保存值到参数中
        *dst = dictGetVal(hi->de);
    }
}

/* A non copy-on-write friendly but higher level version of hashTypeCurrent*()
 * that returns an object with incremented refcount (or a new object). It is up
 * to the caller to decrRefCount() the object if no reference is retained. */
// 从哈希类型的迭代器中获取键或值
robj *hashTypeCurrentObject(hashTypeIterator *hi, int what) {
    robj *dst;

    // 如果从ziplist中获取
    if (hi->encoding == OBJ_ENCODING_ZIPLIST) {
        unsigned char *vstr = NULL;
        unsigned int vlen = UINT_MAX;
        long long vll = LLONG_MAX;

        // 获取键或值
        hashTypeCurrentFromZiplist(hi, what, &vstr, &vlen, &vll);

        // 为获取的键或值创建字符串对象，如果是整数需要转换为字符串
        if (vstr) {
            dst = createStringObject((char*)vstr, vlen);
        } else {
            dst = createStringObjectFromLongLong(vll);
        }
    // 从字典中获取
    } else if (hi->encoding == OBJ_ENCODING_HT) {
        // 获取键或值
        hashTypeCurrentFromHashTable(hi, what, &dst);
        // 增加键或值对象的引用计数
        incrRefCount(dst);
    } else {
        serverPanic("Unknown hash encoding");
    }
    return dst;
}

// 以写操作在数据库中查找对应key的哈希对象，如果不存在则创建
robj *hashTypeLookupWriteOrCreate(client *c, robj *key) {
    robj *o = lookupKeyWrite(c->db,key);    //以写操作在数据库中查找对应key的哈希对象

    // 如果key不存在，则创建一个哈希对象，并加入到数据库中
    if (o == NULL) {
        o = createHashObject();
        dbAdd(c->db,key,o);
    } else {

        // 如果key存在于数据库中，检查其类型是否是哈希类型对象
        if (o->type != OBJ_HASH) {
            addReply(c,shared.wrongtypeerr);
            return NULL;
        }
    }
    return o;
}

// 将一个ziplist类型的哈希对象，转换为enc类型的对象
void hashTypeConvertZiplist(robj *o, int enc) {
    serverAssert(o->encoding == OBJ_ENCODING_ZIPLIST);  //确保为ziplist类型的对象

    // 如果enc为OBJ_ENCODING_ZIPLIST则什么都不做
    if (enc == OBJ_ENCODING_ZIPLIST) {
        /* Nothing to do... */

    // 如果要转换为OBJ_ENCODING_HT类型
    } else if (enc == OBJ_ENCODING_HT) {
        hashTypeIterator *hi;
        dict *dict;
        int ret;

        // 创建并初始化一个哈希类型的迭代器
        hi = hashTypeInitIterator(o);
        // 创建一个新的字典并初始化
        dict = dictCreate(&hashDictType, NULL);

        // 遍历ziplist
        while (hashTypeNext(hi) != C_ERR) {
            robj *field, *value;

            // 取出哈希对象的键field，并且优化编码
            field = hashTypeCurrentObject(hi, OBJ_HASH_KEY);
            field = tryObjectEncoding(field);

            // 取出哈希对象的值value，并且优化编码
            value = hashTypeCurrentObject(hi, OBJ_HASH_VALUE);
            value = tryObjectEncoding(value);

            //讲field-value对添加到新创建的字典中
            ret = dictAdd(dict, field, value);

            if (ret != DICT_OK) {
                // 添加失败返回错误日志信息，并且中断程序
                serverLogHexDump(LL_WARNING,"ziplist with dup elements dump",
                    o->ptr,ziplistBlobLen(o->ptr));
                serverAssert(ret == DICT_OK);
            }
        }

        // 释放迭代器空间
        hashTypeReleaseIterator(hi);
        zfree(o->ptr);

        // 更新编码类型和值对象
        o->encoding = OBJ_ENCODING_HT;
        o->ptr = dict;

    } else {
        serverPanic("Unknown hash encoding");
    }
}

// 转换一个哈希对象的编码类型，enc指定新的编码类型
void hashTypeConvert(robj *o, int enc) {

    // 当前的ziplist转换为字典或不变
    if (o->encoding == OBJ_ENCODING_ZIPLIST) {
        hashTypeConvertZiplist(o, enc);

    // 当前是字典类型，不支持转换
    } else if (o->encoding == OBJ_ENCODING_HT) {
        serverPanic("Not implemented");
    } else {
        serverPanic("Unknown hash encoding");
    }
}

/*-----------------------------------------------------------------------------
 * Hash type commands
 *----------------------------------------------------------------------------*/
// HSET key field value
// HSET命令实现
void hsetCommand(client *c) {
    int update;
    robj *o;

    // 以写方式取出哈希对象，失败则直接返回
    if ((o = hashTypeLookupWriteOrCreate(c,c->argv[1])) == NULL) return;

    // 是否需要进行哈希对象的编码类型转换，是存储在ziplist或字典中
    hashTypeTryConversion(o,c->argv,2,3);

    // 将键和值对象的编码进行优化，以节省空间，是以embstr或raw或整型存储
    hashTypeTryObjectEncoding(o,&c->argv[2], &c->argv[3]);

    // 设置field-value对，update为1则是更新，为0则是替换
    update = hashTypeSet(o,c->argv[2],c->argv[3]);

    // 发送更新或替换的信息给client
    addReply(c, update ? shared.czero : shared.cone);

    // 修改数据库的键则发送信号
    signalModifiedKey(c->db,c->argv[1]);

    // 发送"hset"事件通知
    notifyKeyspaceEvent(NOTIFY_HASH,"hset",c->argv[1],c->db->id);

    // 更新脏键
    server.dirty++;
}

// HSETNX key field value
// HSETNX命令实现
void hsetnxCommand(client *c) {
    robj *o;
    // 以写方式取出哈希对象，失败则直接返回
    if ((o = hashTypeLookupWriteOrCreate(c,c->argv[1])) == NULL) return;

    // 是否需要进行哈希对象的编码类型转换，是存储在ziplist或字典中
    hashTypeTryConversion(o,c->argv,2,3);

    // 当前键已经存在则发送0给client
    if (hashTypeExists(o, c->argv[2])) {
        addReply(c, shared.czero);

    // 当前的key不存在
    } else {
        // 将键和值对象的编码进行优化，以节省空间，是以embstr或raw或整型存储
        hashTypeTryObjectEncoding(o,&c->argv[2], &c->argv[3]);
        // 设置field-value对
        hashTypeSet(o,c->argv[2],c->argv[3]);
        // 发送1给client
        addReply(c, shared.cone);

        // 修改数据库的键则发送信号，发送"hset"事件通知，更新脏键
        signalModifiedKey(c->db,c->argv[1]);
        notifyKeyspaceEvent(NOTIFY_HASH,"hset",c->argv[1],c->db->id);
        server.dirty++;
    }
}

// HMSET key field value [field value ...]
// HMSET的实现
void hmsetCommand(client *c) {
    int i;
    robj *o;

    // 参数必须为奇数，键值对必须成对出现
    if ((c->argc % 2) == 1) {
        addReplyError(c,"wrong number of arguments for HMSET");
        return;
    }

    // 以写方式取出哈希对象，失败则直接返回
    if ((o = hashTypeLookupWriteOrCreate(c,c->argv[1])) == NULL) return;
    // 是否需要进行哈希对象的编码类型转换，是存储在ziplist或字典中
    hashTypeTryConversion(o,c->argv,2,c->argc-1);
    // 遍历所有键值对
    for (i = 2; i < c->argc; i += 2) {
        // 将键和值对象的编码进行优化，以节省空间，是以embstr或raw或整型存储
        hashTypeTryObjectEncoding(o,&c->argv[i], &c->argv[i+1]);
        // 设置field-value对
        hashTypeSet(o,c->argv[i],c->argv[i+1]);
    }
    // 发送设置ok给client
    addReply(c, shared.ok);

    // 修改数据库的键则发送信号，发送"hset"事件通知，更新脏键
    signalModifiedKey(c->db,c->argv[1]);
    notifyKeyspaceEvent(NOTIFY_HASH,"hset",c->argv[1],c->db->id);
    server.dirty++;
}

// HINCRBY key field increment
// HINCRBY命令实现
void hincrbyCommand(client *c) {
    long long value, incr, oldvalue;
    robj *o, *current, *new;

    // 取出increment参数，long long 类型，不成功则直接返回
    if (getLongLongFromObjectOrReply(c,c->argv[3],&incr,NULL) != C_OK) return;
    // 以写方式取出哈希对象，失败则直接返回
    if ((o = hashTypeLookupWriteOrCreate(c,c->argv[1])) == NULL) return;
    // 返回field在哈希对象o中的值对象
    if ((current = hashTypeGetObject(o,c->argv[2])) != NULL) {
        // 取出当前值对象的整数值，保存在value中，如果不是整数的值，则发送"hash value is not an integer"信息给client
        if (getLongLongFromObjectOrReply(c,current,&value,
            "hash value is not an integer") != C_OK) {
            decrRefCount(current);  //取值成功，释放临时的value对象空间，直接返回
            return;
        }
        decrRefCount(current);  //取值失败也要释放空间
    } else {
        value = 0;      //如果没有值，则设置为默认的0
    }

    oldvalue = value;   //备份原先的值

    // 如果增量不合法，造成溢出，则发送溢出的错误信息给client，直接返回
    if ((incr < 0 && oldvalue < 0 && incr < (LLONG_MIN-oldvalue)) ||
        (incr > 0 && oldvalue > 0 && incr > (LLONG_MAX-oldvalue))) {
        addReplyError(c,"increment or decrement would overflow");
        return;
    }

    // 增量合法，更新增量
    value += incr;
    // 将value转换为字符串类型的对象
    new = createStringObjectFromLongLong(value);
    // 将键和值对象的编码进行优化，以节省空间，是以embstr或raw或整型存储
    hashTypeTryObjectEncoding(o,&c->argv[2],NULL);
    // 设置原来的key为新的值对象
    hashTypeSet(o,c->argv[2],new);
    // 释放临时的对象空间，减引用计数
    decrRefCount(new);
    // 发送新的值给client
    addReplyLongLong(c,value);
    // 修改数据库的键则发送信号，发送"hincrby"事件通知，更新脏键
    signalModifiedKey(c->db,c->argv[1]);
    notifyKeyspaceEvent(NOTIFY_HASH,"hincrby",c->argv[1],c->db->id);
    server.dirty++;
}
// HINCRBYFLOAT key field increment
// HINCRBYFLOAT命令的实现
void hincrbyfloatCommand(client *c) {
    double long value, incr;
    robj *o, *current, *new, *aux;

    // 得到一个long double类型的增量increment
    if (getLongDoubleFromObjectOrReply(c,c->argv[3],&incr,NULL) != C_OK) return;
    // 以写方式取出哈希对象，失败则直接返回
    if ((o = hashTypeLookupWriteOrCreate(c,c->argv[1])) == NULL) return;
    // 返回field在哈希对象o中的值对象
    if ((current = hashTypeGetObject(o,c->argv[2])) != NULL) {

        //从值对象中得到一个long double类型的value，如果不是浮点数的值，则发送"hash value is not a valid float"信息给client
        if (getLongDoubleFromObjectOrReply(c,current,&value,
            "hash value is not a valid float") != C_OK) {
            decrRefCount(current);  //取值成功，释放临时的value对象空间，直接返回
            return;
        }
        decrRefCount(current);  //取值失败也要释放空间
    } else {
        value = 0;  //如果没有值，则设置为默认的0
    }

    value += incr;  //备份原先的值
    // 将value转换为字符串类型的对象
    new = createStringObjectFromLongDouble(value,1);
    //将键和值对象的编码进行优化，以节省空间，是以embstr或raw或整型存储
    hashTypeTryObjectEncoding(o,&c->argv[2],NULL);
    // 设置原来的key为新的值对象
    hashTypeSet(o,c->argv[2],new);
    // 讲新的值对象发送给client
    addReplyBulk(c,new);
     // 修改数据库的键则发送信号，发送"hincrbyfloat"事件通知，更新脏键
    signalModifiedKey(c->db,c->argv[1]);
    notifyKeyspaceEvent(NOTIFY_HASH,"hincrbyfloat",c->argv[1],c->db->id);
    server.dirty++;

    /* Always replicate HINCRBYFLOAT as an HSET command with the final value
     * in order to make sure that differences in float pricision or formatting
     * will not create differences in replicas or after an AOF restart. */
    // 用HSET命令代替HINCRBYFLOAT，以防不同的浮点精度造成的误差
    // 创建HSET字符串对象
    aux = createStringObject("HSET",4);
    // 修改HINCRBYFLOAT命令为HSET对象
    rewriteClientCommandArgument(c,0,aux);
    // 释放空间
    decrRefCount(aux);
    // 修改increment为新的值对象new
    rewriteClientCommandArgument(c,3,new);
    // 释放空间
    decrRefCount(new);
}

// 将哈希对象中的field添加到回复中
static void addHashFieldToReply(client *c, robj *o, robj *field) {
    int ret;

    // 对象不存在，发送空信息返回
    if (o == NULL) {
        addReply(c, shared.nullbulk);
        return;
    }

    // 编码为ziplist类型
    if (o->encoding == OBJ_ENCODING_ZIPLIST) {
        unsigned char *vstr = NULL;
        unsigned int vlen = UINT_MAX;
        long long vll = LLONG_MAX;

        // 获得field字段的值保存到函数参数中
        ret = hashTypeGetFromZiplist(o, field, &vstr, &vlen, &vll);
        if (ret < 0) {  //取值失败，发送空信息给client
            addReply(c, shared.nullbulk);
        } else {
            if (vstr) { //取值为字符串类型，发送字符串和长度给client
                addReplyBulkCBuffer(c, vstr, vlen);
            } else {    //取值为整数类型，发送整数值给client
                addReplyBulkLongLong(c, vll);
            }
        }

    // 编码为字典
    } else if (o->encoding == OBJ_ENCODING_HT) {
        robj *value;

        // 从字典中获得field字段的值，保存在value对象中
        ret = hashTypeGetFromHashTable(o, field, &value);
        if (ret < 0) {  //取值失败，发送空信息给client
            addReply(c, shared.nullbulk);
        } else {        //取值成功，发送value对象给client
            addReplyBulk(c, value);
        }

    } else {
        serverPanic("Unknown hash encoding");
    }
}

// HGET key field
// HGET命令实现
void hgetCommand(client *c) {
    robj *o;

    // 以读操作取出哈希对象，若失败，或取出的对象不是哈希类型的对象，则直接返回
    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.nullbulk)) == NULL ||
        checkType(c,o,OBJ_HASH)) return;

    addHashFieldToReply(c, o, c->argv[2]);  //发送相应的值给client
}

//HMGET key field [field ...]
// HMGET命令的实现
void hmgetCommand(client *c) {
    robj *o;
    int i;

    /* Don't abort when the key cannot be found. Non-existing keys are empty
     * hashes, where HMGET should respond with a series of null bulks. */
    // 以读操作取出哈希对象，若失败，或取出的对象不是哈希类型的对象，则发送类型错误信息后直接返回
    o = lookupKeyRead(c->db, c->argv[1]);
    if (o != NULL && o->type != OBJ_HASH) {
        addReply(c, shared.wrongtypeerr);
        return;
    }

    // 发送取出的field字段个数
    addReplyMultiBulkLen(c, c->argc-2);
    // 遍历所有的字段
    for (i = 2; i < c->argc; i++) {
        // 将所有字段的值发送给client
        addHashFieldToReply(c, o, c->argv[i]);
    }
}

// HDEL key field [field ...]
// HDEL命令实现
void hdelCommand(client *c) {
    robj *o;
    int j, deleted = 0, keyremoved = 0;

    // 以写操作取出哈希对象，若失败，或取出的对象不是哈希类型的对象，则发送0后直接返回
    if ((o = lookupKeyWriteOrReply(c,c->argv[1],shared.czero)) == NULL ||
        checkType(c,o,OBJ_HASH)) return;

    // 遍历所有的字段field
    for (j = 2; j < c->argc; j++) {
        // 从哈希对象中删除当前字段
        if (hashTypeDelete(o,c->argv[j])) {
            deleted++;  //更新删除的个数

            // 如果哈希对象为空，则删除该对象
            if (hashTypeLength(o) == 0) {
                dbDelete(c->db,c->argv[1]);
                keyremoved = 1; //设置删除标志
                break;
            }
        }
    }

    // 只要删除了字段
    if (deleted) {
        // 发送信号表示键被改变
        signalModifiedKey(c->db,c->argv[1]);
        // 发送"hdel"事件通知
        notifyKeyspaceEvent(NOTIFY_HASH,"hdel",c->argv[1],c->db->id);

        // 如果哈希对象被删除
        if (keyremoved)
            // 发送"hdel"事件通知
            notifyKeyspaceEvent(NOTIFY_GENERIC,"del",c->argv[1],
                                c->db->id);
        server.dirty += deleted;    // 更新脏键
    }
    addReplyLongLong(c,deleted);    //发送删除的个数给client
}

// HLEN key
// HLEN命令实现
void hlenCommand(client *c) {
    robj *o;

    // 以写操作取出哈希对象，若失败，或取出的对象不是哈希类型的对象，则发送0后直接返回
    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.czero)) == NULL ||
        checkType(c,o,OBJ_HASH)) return;

    // 发送哈希对象的字段数给client
    addReplyLongLong(c,hashTypeLength(o));
}

// 3.2 以上版本特有的命令 HSTRLEN key field
// HSTRLEN 命令的实现
void hstrlenCommand(client *c) {
    robj *o;

    // 以写操作取出哈希对象，若失败，或取出的对象不是哈希类型的对象，则发送0后直接返回
    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.czero)) == NULL ||
        checkType(c,o,OBJ_HASH)) return;

    // 发送field对象的值的长度给client
    addReplyLongLong(c,hashTypeGetValueLength(o,c->argv[2]));
}

// 保存哈希类型迭代器指向的键值对
static void addHashIteratorCursorToReply(client *c, hashTypeIterator *hi, int what) {

    // 如果是ziplist类型编码的哈希对象
    if (hi->encoding == OBJ_ENCODING_ZIPLIST) {
        unsigned char *vstr = NULL;
        unsigned int vlen = UINT_MAX;
        long long vll = LLONG_MAX;

        // 从ziplist类型的哈希类型迭代器中获取对应的field或value，保存在参数中
        hashTypeCurrentFromZiplist(hi, what, &vstr, &vlen, &vll);
        if (vstr) { //如果值为字符串，则发送字符串类型的值和其长度
            addReplyBulkCBuffer(c, vstr, vlen);
        } else {    //发送整数值给client
            addReplyBulkLongLong(c, vll);
        }

    // 如果是ht字典类型编码的哈希对象
    } else if (hi->encoding == OBJ_ENCODING_HT) {
        robj *value;

        // 从ht字典类型的哈希类型迭代器中获取对应的field或value，保存在value参数中
        hashTypeCurrentFromHashTable(hi, what, &value);
        addReplyBulk(c, value); //发送vlaue给client

    } else {
        serverPanic("Unknown hash encoding");
    }
}

// Hgetall一类命令的底层实现
void genericHgetallCommand(client *c, int flags) {
    robj *o;
    hashTypeIterator *hi;
    int multiplier = 0;
    int length, count = 0;

    // 以写操作取出哈希对象，若失败，或取出的对象不是哈希类型的对象，则发送0后直接返回
    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.emptymultibulk)) == NULL
        || checkType(c,o,OBJ_HASH)) return;

    // 计算一对键值对要返回的个数
    if (flags & OBJ_HASH_KEY) multiplier++;
    if (flags & OBJ_HASH_VALUE) multiplier++;

    // 计算整个哈希对象中的所有键值对要返回的个数
    length = hashTypeLength(o) * multiplier;
    addReplyMultiBulkLen(c, length);        //发get到的个数给client

    // 创建一个哈希类型的迭代器并初始化
    hi = hashTypeInitIterator(o);
    // 迭代所有的entry节点
    while (hashTypeNext(hi) != C_ERR) {
        // 如果取哈希键
        if (flags & OBJ_HASH_KEY) {
            // 保存当前迭代器指向的键
            addHashIteratorCursorToReply(c, hi, OBJ_HASH_KEY);
            count++;    //更新计数器
        }
        // 如果取哈希值
        if (flags & OBJ_HASH_VALUE) {
            // 保存当前迭代器指向的值
            addHashIteratorCursorToReply(c, hi, OBJ_HASH_VALUE);
            count++;    //更新计数器
        }
    }

    //释放迭代器
    hashTypeReleaseIterator(hi);
    serverAssert(count == length);
}

// HKEYS key
// HKEYS命令实现
void hkeysCommand(client *c) {
    genericHgetallCommand(c,OBJ_HASH_KEY);
}

// HVALS key
// HVALS命令实现
void hvalsCommand(client *c) {
    genericHgetallCommand(c,OBJ_HASH_VALUE);
}

// HGETALL key
// HGETALL命令实现
void hgetallCommand(client *c) {
    genericHgetallCommand(c,OBJ_HASH_KEY|OBJ_HASH_VALUE);
}

// HEXISTS key field
// HEXISTS命令实现
void hexistsCommand(client *c) {
    robj *o;
    // 以写操作取出哈希对象，若失败，或取出的对象不是哈希类型的对象，则发送0后直接返回
    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.czero)) == NULL ||
        checkType(c,o,OBJ_HASH)) return;

    // 存在发送1，不存在发送0给client
    addReply(c, hashTypeExists(o,c->argv[2]) ? shared.cone : shared.czero);
}

// HSCAN key cursor [MATCH pattern] [COUNT count]
// HSCAN 命令实现
void hscanCommand(client *c) {
    robj *o;
    unsigned long cursor;

    // 获取scan命令的游标cursor
    if (parseScanCursorOrReply(c,c->argv[2],&cursor) == C_ERR) return;
    // 以写操作取出哈希对象，若失败，或取出的对象不是哈希类型的对象，则发送0后直接返回
    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.emptyscan)) == NULL ||
        checkType(c,o,OBJ_HASH)) return;
    // 调用底层实现
    scanGenericCommand(c,o,cursor);
}
