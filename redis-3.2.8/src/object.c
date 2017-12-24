/* Redis Object implementation.
 *
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
#include <ctype.h>

#ifdef __CYGWIN__
#define strtold(a,b) ((long double)strtod((a),(b))) //将a和b转换为long double类型
#endif

robj *createObject(int type, void *ptr) {   //创建一个默认的对象
    robj *o = zmalloc(sizeof(*o));          //分配空间
    o->type = type;                         //设置对象类型
    o->encoding = OBJ_ENCODING_RAW;         //设置默认的编码方式
    o->ptr = ptr;                           //设置
    o->refcount = 1;                        //引用计数为1

    /* Set the LRU to the current lruclock (minutes resolution). */
    o->lru = LRU_CLOCK();                   //计算设置当前LRU时间
    return o;
}

/* Create a string object with encoding OBJ_ENCODING_RAW, that is a s
 * string object where o->ptr points to a proper sds string. */
//创建一个字符串对象，编码默认为OBJ_ENCODING_RAW，指向的数据为一个sds
robj *createRawStringObject(const char *ptr, size_t len) {
    return createObject(OBJ_STRING,sdsnewlen(ptr,len));
}

/* Create a string object with encoding OBJ_ENCODING_EMBSTR, that is
 * an object where the sds string is actually an unmodifiable string
 * allocated in the same chunk as the object itself. */
//创建一个embstr编码的字符串对象
robj *createEmbeddedStringObject(const char *ptr, size_t len) {
    robj *o = zmalloc(sizeof(robj)+sizeof(struct sdshdr8)+len+1);   //分配空间
    struct sdshdr8 *sh = (void*)(o+1);  //o+1刚好就是struct sdshdr8的地址

    o->type = OBJ_STRING;               //类型为字符串对象
    o->encoding = OBJ_ENCODING_EMBSTR;  //设置编码类型OBJ_ENCODING_EMBSTR
    o->ptr = sh+1;                      //指向分配的sds对象，分配的len+1的空间首地址
    o->refcount = 1;                    //设置引用计数
    o->lru = LRU_CLOCK();               //计算设置当前LRU时间

    sh->len = len;                      //设置字符串长度
    sh->alloc = len;                    //设置最大容量
    sh->flags = SDS_TYPE_8;             //设置sds的类型
    if (ptr) {                          //如果传了字符串参数
        memcpy(sh->buf,ptr,len);        //将传进来的ptr保存到对象中
        sh->buf[len] = '\0';            //结束符标志
    } else {
        memset(sh->buf,0,len+1);        //否则将对象的空间初始化为0
    }
    return o;
}

/* Create a string object with EMBSTR encoding if it is smaller than
 * REIDS_ENCODING_EMBSTR_SIZE_LIMIT, otherwise the RAW encoding is
 * used.
 *
 * The current limit of 39 is chosen so that the biggest string object
 * we allocate as EMBSTR will still fit into the 64 byte arena of jemalloc. */

//sdshdr8的大小为3个字节，加上1个结束符共4个字节
//redisObject的大小为16个字节
//redis使用jemalloc内存分配器，且jemalloc会分配8，16，32，64等字节的内存
//一个embstr固定的大小为16+3+1 = 20个字节，因此一个最大的embstr字符串为64-20 = 44字节
#define OBJ_ENCODING_EMBSTR_SIZE_LIMIT 44

// 创建字符串对象，根据长度使用不同的编码类型
// createRawStringObject和createEmbeddedStringObject的区别是：
// createRawStringObject是当字符串长度大于44字节时，robj结构和sdshdr结构在内存上是分开的
// createEmbeddedStringObject是当字符串长度小于等于44字节时，robj结构和sdshdr结构在内存上是连续的
robj *createStringObject(const char *ptr, size_t len) {
    if (len <= OBJ_ENCODING_EMBSTR_SIZE_LIMIT)
        return createEmbeddedStringObject(ptr,len);
    else
        return createRawStringObject(ptr,len);
}

//创建字符串对象，根据整数值
robj *createStringObjectFromLongLong(long long value) {
    robj *o;

    //redis中[0, 10000)内的整数是共享的
    if (value >= 0 && value < OBJ_SHARED_INTEGERS) {    //如果value属于redis共享整数的范围
        incrRefCount(shared.integers[value]);           //引用计数加1
        o = shared.integers[value];                     //返回一个编码类型为OBJ_ENCODING_INT的字符串对象

    //如果不在共享整数的范围
    } else {
        if (value >= LONG_MIN && value <= LONG_MAX) {   //value在long类型所表示的范围内
            o = createObject(OBJ_STRING, NULL);         //创建对象
            o->encoding = OBJ_ENCODING_INT;             //编码类型为OBJ_ENCODING_INT
            o->ptr = (void*)((long)value);              //指向这个value值
        } else {
            //value不在long类型所表示的范围内，将long long类型的整数转换为字符串
            //编码类型为OBJ_ENCODING_RAW
            o = createObject(OBJ_STRING,sdsfromlonglong(value));
        }
    }
    return o;
}

/* Create a string object from a long double. If humanfriendly is non-zero
 * it does not use exponential format and trims trailing zeroes at the end,
 * however this results in loss of precision. Otherwise exp format is used
 * and the output of snprintf() is not modified.
 *
 * The 'humanfriendly' option is used for INCRBYFLOAT and HINCRBYFLOAT. */
//将long double类型的value创建一个字符串对象
robj *createStringObjectFromLongDouble(long double value, int humanfriendly) {
    char buf[256];
    int len;

    if (isinf(value)) { //value是否为正无穷，返回1表示正无穷，返回-1表示负无穷
        /* Libc in odd systems (Hi Solaris!) will format infinite in a
         * different way, so better to handle it in an explicit way. */
        if (value > 0) {
            memcpy(buf,"inf",3);    //正无穷
            len = 3;
        } else {
            memcpy(buf,"-inf",4);   //负无穷
            len = 4;
        }
    } else if (humanfriendly) { //如果humanfriendly选项为真，则表示可以使用INCRBYFLOAT和HINCRBYFLOAT命令
        /* We use 17 digits precision since with 128 bit floats that precision
         * after rounding is able to represent most small decimal numbers in a
         * way that is "non surprising" for the user (that is, most small
         * decimal numbers will be represented in a way that when converted
         * back into a string are exactly the same as what the user typed.) */
        //以17位精度浮点数将value写入buf中
        len = snprintf(buf,sizeof(buf),"%.17Lf", value);

        /* Now remove trailing zeroes after the '.' */
        //去除小数点尾部没有用的零
        if (strchr(buf,'.') != NULL) {  //如果在buf中能找到'.'
            char *p = buf+len-1;        //定位到buf的最后一个字符的地址
            while(*p == '0') {          //如果是'0'，则将p向前指向一个字符，且将buf的len减1
                p--;
                len--;
            }
            if (*p == '.') len--;       //例如：3.0000 变成 3
        }
    } else {
        len = snprintf(buf,sizeof(buf),"%.17Lg", value);
    }
    return createStringObject(buf,len);
}

/* Duplicate a string object, with the guarantee that the returned object
 * has the same encoding as the original one.
 *
 * This function also guarantees that duplicating a small integere object
 * (or a string object that contains a representation of a small integer)
 * will always result in a fresh object that is unshared (refcount == 1).
 *
 * The resulting object always has refcount set to 1. */
//返回 复制的o对象的副本的地址，且创建的对象非共享
robj *dupStringObject(robj *o) {
    robj *d;

    serverAssert(o->type == OBJ_STRING);    //一定是OBJ_STRING类型

    switch(o->encoding) {                   //根据不同的编码类型
    case OBJ_ENCODING_RAW:
        return createRawStringObject(o->ptr,sdslen(o->ptr));        //创建的对象非共享
    case OBJ_ENCODING_EMBSTR:
        return createEmbeddedStringObject(o->ptr,sdslen(o->ptr));   //创建的对象非共享
    case OBJ_ENCODING_INT:                  //整数编码类型
        d = createObject(OBJ_STRING, NULL); //即使是共享整数范围内的整数，创建的对象也是非共享的
        d->encoding = OBJ_ENCODING_INT;
        d->ptr = o->ptr;
        return d;
    default:
        serverPanic("Wrong encoding.");
        break;
    }
}

//创建一个quicklist编码的列表对象
robj *createQuicklistObject(void) {
    quicklist *l = quicklistCreate();       //创建一个quicklist
    robj *o = createObject(OBJ_LIST,l);     //创建一个对象，对象的数据类型为OBJ_LIST
    o->encoding = OBJ_ENCODING_QUICKLIST;   //对象的编码类型OBJ_ENCODING_QUICKLIST
    return o;
}

//创建一个ziplist编码的列表对象
robj *createZiplistObject(void) {
    unsigned char *zl = ziplistNew();       //创建一个ziplist
    robj *o = createObject(OBJ_LIST,zl);    //创建一个对象，对象的数据类型为OBJ_LIST
    o->encoding = OBJ_ENCODING_ZIPLIST;     //对象的编码类型OBJ_ENCODING_ZIPLIST
    return o;
}

//创建一个ht编码的集合对象
robj *createSetObject(void) {
    dict *d = dictCreate(&setDictType,NULL);//创建一个字典
    robj *o = createObject(OBJ_SET,d);      //创建一个对象，对象的数据类型为OBJ_SET
    o->encoding = OBJ_ENCODING_HT;          //对象的编码类型OBJ_ENCODING_HT
    return o;
}

//创建一个intset编码的集合对象
robj *createIntsetObject(void) {
    intset *is = intsetNew();               //创建一个整数集合
    robj *o = createObject(OBJ_SET,is);     //创建一个对象，对象的数据类型为OBJ_SET
    o->encoding = OBJ_ENCODING_INTSET;      //对象的编码类型OBJ_ENCODING_INTSET
    return o;
}

//创建一个ziplist编码的哈希对象
robj *createHashObject(void) {
    unsigned char *zl = ziplistNew();       //创建一个ziplist
    robj *o = createObject(OBJ_HASH, zl);   //创建一个对象，对象的数据类型为OBJ_HASH
    o->encoding = OBJ_ENCODING_ZIPLIST;     //对象的编码类型OBJ_ENCODING_ZIPLIST
    return o;
}

//创建一个skiplist编码的有序集合对象
robj *createZsetObject(void) {
    zset *zs = zmalloc(sizeof(*zs));
    robj *o;

    zs->dict = dictCreate(&zsetDictType,NULL);  //创建一个字典
    zs->zsl = zslCreate();                      //创建一个跳跃表
    o = createObject(OBJ_ZSET,zs);              //创建一个对象，对象的数据类型为OBJ_ZSET
    o->encoding = OBJ_ENCODING_SKIPLIST;        //对象的编码类型OBJ_ENCODING_SKIPLIST
    return o;
}
//创建一个ziplist编码的有序集合对象
robj *createZsetZiplistObject(void) {
    unsigned char *zl = ziplistNew();           //创建一个ziplist
    robj *o = createObject(OBJ_ZSET,zl);        //创建一个对象，对象的数据类型为OBJ_ZSET
    o->encoding = OBJ_ENCODING_ZIPLIST;         //对象的编码类型OBJ_ENCODING_ZIPLIST
    return o;
}

//释放字符串对象ptr指向的对象
void freeStringObject(robj *o) {
    if (o->encoding == OBJ_ENCODING_RAW) {
        sdsfree(o->ptr);
    }
}

//释放列表对象ptr指向的对象
void freeListObject(robj *o) {
    if (o->encoding == OBJ_ENCODING_QUICKLIST) {
        quicklistRelease(o->ptr);
    } else {
        serverPanic("Unknown list encoding type");
    }
}

//释放集合对象ptr指向的对象
void freeSetObject(robj *o) {
    switch (o->encoding) {
    case OBJ_ENCODING_HT:
        dictRelease((dict*) o->ptr);
        break;
    case OBJ_ENCODING_INTSET:
        zfree(o->ptr);
        break;
    default:
        serverPanic("Unknown set encoding type");
    }
}

//释放有序集合对象ptr指向的对象
void freeZsetObject(robj *o) {
    zset *zs;
    switch (o->encoding) {
    case OBJ_ENCODING_SKIPLIST:
        zs = o->ptr;
        dictRelease(zs->dict);
        zslFree(zs->zsl);
        zfree(zs);
        break;
    case OBJ_ENCODING_ZIPLIST:
        zfree(o->ptr);
        break;
    default:
        serverPanic("Unknown sorted set encoding");
    }
}

//释放哈希对象ptr指向的对象
void freeHashObject(robj *o) {
    switch (o->encoding) {
    case OBJ_ENCODING_HT:
        dictRelease((dict*) o->ptr);
        break;
    case OBJ_ENCODING_ZIPLIST:
        zfree(o->ptr);
        break;
    default:
        serverPanic("Unknown hash encoding type");
        break;
    }
}

//引用计数加1
void incrRefCount(robj *o) {
    o->refcount++;
}

//引用计数减1
void decrRefCount(robj *o) {
    if (o->refcount <= 0) serverPanic("decrRefCount against refcount <= 0");

    //当引用对象等于1时，在操作引用计数减1，直接释放对象的ptr和对象空间
    if (o->refcount == 1) {
        switch(o->type) {
        case OBJ_STRING: freeStringObject(o); break;
        case OBJ_LIST: freeListObject(o); break;
        case OBJ_SET: freeSetObject(o); break;
        case OBJ_ZSET: freeZsetObject(o); break;
        case OBJ_HASH: freeHashObject(o); break;
        default: serverPanic("Unknown object type"); break;
        }
        zfree(o);
    } else {
        o->refcount--;  //否则减1
    }
}

/* This variant of decrRefCount() gets its argument as void, and is useful
 * as free method in data structures that expect a 'void free_object(void*)'
 * prototype for the free method. */
//释放特定的数据类型的封装
void decrRefCountVoid(void *o) {
    decrRefCount(o);
}

/* This function set the ref count to zero without freeing the object.
 * It is useful in order to pass a new object to functions incrementing
 * the ref count of the received object. Example:
 *
 *    functionThatWillIncrementRefCount(resetRefCount(CreateObject(...)));
 *
 * Otherwise you need to resort to the less elegant pattern:
 *
 *    *obj = createObject(...);
 *    functionThatWillIncrementRefCount(obj);
 *    decrRefCount(obj);
 */
//重置obj对象的引用计数为0，用于上面注释中的情况
robj *resetRefCount(robj *obj) {
    obj->refcount = 0;
    return obj;
}

//检查对象o的对象类型是否是type，是返回0，不是返回1，并向client发送一个错误
int checkType(client *c, robj *o, int type) {
    if (o->type != type) {
        addReply(c,shared.wrongtypeerr);
        return 1;
    }
    return 0;
}

//判断对象的ptr指向的值能否转换为long long类型，如果可以保存在llval中
int isObjectRepresentableAsLongLong(robj *o, long long *llval) {
    serverAssertWithInfo(NULL,o,o->type == OBJ_STRING);
    if (o->encoding == OBJ_ENCODING_INT) {      //如果本身就是整数
        if (llval) *llval = (long) o->ptr;
        return C_OK;                            //成功返回0
    } else {
        //字符串转换为longlong类型，成功返回0，失败返回-1
        return string2ll(o->ptr,sdslen(o->ptr),llval) ? C_OK : C_ERR;
    }
}

/* Try to encode a string object in order to save space */
//尝试优化字符串对象的编码方式以节约空间
robj *tryObjectEncoding(robj *o) {
    long value;
    sds s = o->ptr;
    size_t len;

    /* Make sure this is a string object, the only type we encode
     * in this function. Other types use encoded memory efficient
     * representations but are handled by the commands implementing
     * the type. */
    serverAssertWithInfo(NULL,o,o->type == OBJ_STRING);

    /* We try some specialized encoding only for objects that are
     * RAW or EMBSTR encoded, in other words objects that are still
     * in represented by an actually array of chars. */
    //如果字符串对象的编码类型为RAW或EMBSTR时，才对其重新编码
    if (!sdsEncodedObject(o)) return o;

    /* It's not safe to encode shared objects: shared objects can be shared
     * everywhere in the "object space" of Redis and may end in places where
     * they are not handled. We handle them only as values in the keyspace. */
    //如果refcount大于1，则说明对象的ptr指向的值是共享的，不对共享对象进行编码
     if (o->refcount > 1) return o;

    /* Check if we can represent this string as a long integer.
     * Note that we are sure that a string larger than 20 chars is not
     * representable as a 32 nor 64 bit integer. */
    len = sdslen(s);            //获得字符串s的长度

    //如果len小于等于20，表示符合long long可以表示的范围，且可以转换为long类型的字符串进行编码
    if (len <= 20 && string2l(s,len,&value)) {
        /* This object is encodable as a long. Try to use a shared object.
         * Note that we avoid using shared integers when maxmemory is used
         * because every object needs to have a private LRU field for the LRU
         * algorithm to work well. */
        if ((server.maxmemory == 0 ||
             (server.maxmemory_policy != MAXMEMORY_VOLATILE_LRU &&
              server.maxmemory_policy != MAXMEMORY_ALLKEYS_LRU)) &&
            value >= 0 &&
            value < OBJ_SHARED_INTEGERS)    //如果value处于共享整数的范围内
        {
            decrRefCount(o);                //原对象的引用计数减1，释放对象
            incrRefCount(shared.integers[value]); //增加共享对象的引用计数
            return shared.integers[value];      //返回一个编码为整数的字符串对象
        } else {        //如果不处于共享整数的范围
            if (o->encoding == OBJ_ENCODING_RAW) sdsfree(o->ptr);   //释放编码为OBJ_ENCODING_RAW的对象
            o->encoding = OBJ_ENCODING_INT;     //转换为OBJ_ENCODING_INT编码
            o->ptr = (void*) value;             //指针ptr指向value对象
            return o;
        }
    }

    /* If the string is small and is still RAW encoded,
     * try the EMBSTR encoding which is more efficient.
     * In this representation the object and the SDS string are allocated
     * in the same chunk of memory to save space and cache misses. */
    //如果len小于44，44是最大的编码为EMBSTR类型的字符串对象长度
    if (len <= OBJ_ENCODING_EMBSTR_SIZE_LIMIT) {
        robj *emb;

        if (o->encoding == OBJ_ENCODING_EMBSTR) return o;   //将RAW对象转换为OBJ_ENCODING_EMBSTR编码类型
        emb = createEmbeddedStringObject(s,sdslen(s)); //创建一个编码类型为OBJ_ENCODING_EMBSTR的字符串对象
        decrRefCount(o);    //释放之前的对象
        return emb;
    }

    /* We can't encode the object...
     *
     * Do the last try, and at least optimize the SDS string inside
     * the string object to require little space, in case there
     * is more than 10% of free space at the end of the SDS string.
     *
     * We do that only for relatively large strings as this branch
     * is only entered if the length of the string is greater than
     * OBJ_ENCODING_EMBSTR_SIZE_LIMIT. */
    //无法进行编码，但是如果s的未使用的空间大于使用空间的10分之1
    if (o->encoding == OBJ_ENCODING_RAW &&
        sdsavail(s) > len/10)
    {
        o->ptr = sdsRemoveFreeSpace(o->ptr);    //释放所有的未使用空间
    }

    /* Return the original object. */
    return o;
}

/* Get a decoded version of an encoded object (returned as a new object).
 * If the object is already raw-encoded just increment the ref count. */
//将对象是整型的解码为字符串并返回，如果是字符串编码则直接返回输入对象，只需增加引用计数
robj *getDecodedObject(robj *o) {
    robj *dec;

    if (sdsEncodedObject(o)) {  //如果是OBJ_ENCODING_RAW或OBJ_ENCODING_EMBSTR类型的对象
        incrRefCount(o);        //增加引用计数，返回一个共享的对象
        return o;
    }
    if (o->type == OBJ_STRING && o->encoding == OBJ_ENCODING_INT) { //如果是整数对象
        char buf[32];

        ll2string(buf,32,(long)o->ptr); //将整数转换为字符串
        dec = createStringObject(buf,strlen(buf));  //创建一个字符串对象
        return dec;
    } else {
        serverPanic("Unknown encoding type");
    }
}

/* Compare two string objects via strcmp() or strcoll() depending on flags.
 * Note that the objects may be integer-encoded. In such a case we
 * use ll2string() to get a string representation of the numbers on the stack
 * and compare the strings, it's much faster than calling getDecodedObject().
 *
 * Important note: when REDIS_COMPARE_BINARY is used a binary-safe comparison
 * is used. */

#define REDIS_COMPARE_BINARY (1<<0)     //以二进制方式进行比较
#define REDIS_COMPARE_COLL (1<<1)       //以本地指定的字符次序进行比较

//根据flags比较两个字符串对象a和b，返回0表示相等，非零表示不相等
int compareStringObjectsWithFlags(robj *a, robj *b, int flags) {
    serverAssertWithInfo(NULL,a,a->type == OBJ_STRING && b->type == OBJ_STRING);
    char bufa[128], bufb[128], *astr, *bstr;
    size_t alen, blen, minlen;

    if (a == b) return 0;   //如果是同一对象直接返回

    //如果是指向字符串值的两种OBJ_ENCODING_EMBSTR或OBJ_ENCODING_RAW的两类对象
    if (sdsEncodedObject(a)) {
        astr = a->ptr;
        alen = sdslen(astr);    //获取字符串的长度
    } else {    //如果是整数类型的OBJ_ENCODING_INT编码
        alen = ll2string(bufa,sizeof(bufa),(long) a->ptr);  //转换为字符串
        astr = bufa;
    }
    //如果是指向字符串值的两种OBJ_ENCODING_EMBSTR或OBJ_ENCODING_RAW的两类对象
    if (sdsEncodedObject(b)) {
        bstr = b->ptr;
        blen = sdslen(bstr);    //获取字符串的长度
    } else {    //如果是整数类型的OBJ_ENCODING_INT编码
        blen = ll2string(bufb,sizeof(bufb),(long) b->ptr);  //转换为字符串
        bstr = bufb;
    }

    //以本地指定的字符次序进行比较
    if (flags & REDIS_COMPARE_COLL) {
        //strcoll()会依环境变量LC_COLLATE所指定的文字排列次序来比较两字符串
        return strcoll(astr,bstr);  //比较a和b的字符串对象，相等返回0
    } else {    //以二进制方式进行比较
        int cmp;

        minlen = (alen < blen) ? alen : blen;
        cmp = memcmp(astr,bstr,minlen);         //相等返回0，否则返回第一个字符串和第二个字符串的长度差
        if (cmp == 0) return alen-blen;
        return cmp;
    }
}

/* Wrapper for compareStringObjectsWithFlags() using binary comparison. */
//以二进制安全的方式进行对象的比较
int compareStringObjects(robj *a, robj *b) {
    return compareStringObjectsWithFlags(a,b,REDIS_COMPARE_BINARY);
}

/* Wrapper for compareStringObjectsWithFlags() using collation. */
//以制定的字符次序进行两个对象的比较
int collateStringObjects(robj *a, robj *b) {
    return compareStringObjectsWithFlags(a,b,REDIS_COMPARE_COLL);
}

/* Equal string objects return 1 if the two objects are the same from the
 * point of view of a string comparison, otherwise 0 is returned. Note that
 * this function is faster then checking for (compareStringObject(a,b) == 0)
 * because it can perform some more optimization. */
//比较两个字符串对象，以二进制安全的方式进行比较
int equalStringObjects(robj *a, robj *b) {
    if (a->encoding == OBJ_ENCODING_INT &&      //如果两个对象的编码都是整型INT
        b->encoding == OBJ_ENCODING_INT){
        /* If both strings are integer encoded just check if the stored
         * long is the same. */
        return a->ptr == b->ptr;            //直接比较数值大小
    } else {
        return compareStringObjects(a,b) == 0;  //否则进行二进制安全比较字符串
    }
}

//返回字符串对象的字符串长度
size_t stringObjectLen(robj *o) {
    serverAssertWithInfo(NULL,o,o->type == OBJ_STRING);

    //如果是字符串编码的两种类型
    if (sdsEncodedObject(o)) {
        return sdslen(o->ptr);
    } else {    //如果是整数编码类型
        return sdigits10((long)o->ptr); //计算出整数值的位数返回
    }
}

//从对象中将字符串值转换为double并存储在target中
int getDoubleFromObject(robj *o, double *target) {
    double value;
    char *eptr;

    if (o == NULL) {    //对象不存在
        value = 0;
    } else {
        serverAssertWithInfo(NULL,o,o->type == OBJ_STRING);
        //如果是字符串编码的两种类型
        if (sdsEncodedObject(o)) {
            errno = 0;
            value = strtod(o->ptr, &eptr);  //将字符串转换为double类型
            if (isspace(((char*)o->ptr)[0]) ||
                eptr[0] != '\0' ||
                (errno == ERANGE &&
                    (value == HUGE_VAL || value == -HUGE_VAL || value == 0)) ||
                errno == EINVAL ||
                isnan(value))   //转换失败返回-1
                return C_ERR;
        } else if (o->encoding == OBJ_ENCODING_INT) {   //整数编码
            value = (long)o->ptr;                       //保存整数值
        } else {
            serverPanic("Unknown string encoding");
        }
    }
    *target = value;    //将值存到传入参数中，返回0成功
    return C_OK;
}

//从对象中将字符串值转换为double并存储在target中，若失败，失败发送信息给client
int getDoubleFromObjectOrReply(client *c, robj *o, double *target, const char *msg) {
    double value;
    if (getDoubleFromObject(o, &value) != C_OK) {   //如果出错
        if (msg != NULL) {                  //msg不为空
            addReplyError(c,(char*)msg);    //发送指定的msg给client
        } else {
            addReplyError(c,"value is not a valid float");  //发送普通字符串
        }
        return C_ERR;
    }
    *target = value;    //将转换成功的值存到传入参数中，返回0成功
    return C_OK;
}

//从对象中将字符串值转换为long double并存储在target中
int getLongDoubleFromObject(robj *o, long double *target) {
    long double value;
    char *eptr;

    if (o == NULL) {    //对象不存在
        value = 0;
    } else {
        serverAssertWithInfo(NULL,o,o->type == OBJ_STRING);

        //如果是字符串编码的两种类型
        if (sdsEncodedObject(o)) {
            errno = 0;
            value = strtold(o->ptr, &eptr); //将字符串转换为long double类型
            if (isspace(((char*)o->ptr)[0]) || eptr[0] != '\0' ||
                errno == ERANGE || isnan(value))    //转换失败返回-1
                return C_ERR;
        } else if (o->encoding == OBJ_ENCODING_INT) {   //整数编码
            value = (long)o->ptr;                       //保存整数值
        } else {
            serverPanic("Unknown string encoding");
        }
    }
    *target = value;        //将值存到传入参数中，返回0成功
    return C_OK;
}

//从对象中将字符串值转换为long double并存储在target中，若失败，失败发送信息给client
int getLongDoubleFromObjectOrReply(client *c, robj *o, long double *target, const char *msg) {
    long double value;
    if (getLongDoubleFromObject(o, &value) != C_OK) {   //如果出错
        if (msg != NULL) {                  //msg不为空
            addReplyError(c,(char*)msg);    //发送指定的msg给client
        } else {
            addReplyError(c,"value is not a valid float");  //发送普通字符串
        }
        return C_ERR;
    }
    *target = value;    //将转换成功的值存到传入参数中，返回0成功
    return C_OK;
}

//从对象中将字符串值转换为long long并存储在target中
int getLongLongFromObject(robj *o, long long *target) {
    long long value;

    if (o == NULL) {    //对象不存在
        value = 0;
    } else {
        serverAssertWithInfo(NULL,o,o->type == OBJ_STRING);

         //如果是字符串编码的两种类型
        if (sdsEncodedObject(o)) {
             //转换失败发送-1，成功保存值到value中
            if (string2ll(o->ptr,sdslen(o->ptr),&value) == 0) return C_ERR;
        } else if (o->encoding == OBJ_ENCODING_INT) {   //整型编码
            value = (long)o->ptr;                       //保存整数值
        } else {
            serverPanic("Unknown string encoding");
        }
    }
    if (target) *target = value;        //将值存到传入参数中，返回0成功
    return C_OK;
}
//从对象中将字符串值转换为long long并存储在target中，若失败，失败发送信息给client
int getLongLongFromObjectOrReply(client *c, robj *o, long long *target, const char *msg) {
    long long value;
    if (getLongLongFromObject(o, &value) != C_OK) { //如果出错
        if (msg != NULL) {          //msg不为空
            addReplyError(c,(char*)msg);    //发送指定的msg给client
        } else {
            addReplyError(c,"value is not an integer or out of range"); //发送普通字符串
        }
        return C_ERR;
    }
    *target = value;    //将转换成功的值存到传入参数中，返回0成功
    return C_OK;
}

//从对象中将字符串值转换为long并存储在target中，若失败，失败发送信息给client
int getLongFromObjectOrReply(client *c, robj *o, long *target, const char *msg) {
    long long value;

    //如果将字符串值转换为long long出错，发送错误信息msg，返回-1，否则保存整数值到value中
    if (getLongLongFromObjectOrReply(c, o, &value, msg) != C_OK) return C_ERR;
    if (value < LONG_MIN || value > LONG_MAX) { //如果long long类型的value超出long所能表示的范围
        if (msg != NULL) {                      //msg不为空
            addReplyError(c,(char*)msg);            //发送指定的msg给client
        } else {
            addReplyError(c,"value is out of range");   //发送普通字符串
        }
        return C_ERR;
    }
    *target = value;    //否则将long大小的value返回到传入参数
    return C_OK;
}

//返回编码类型的字符串表示
char *strEncoding(int encoding) {
    switch(encoding) {
    case OBJ_ENCODING_RAW: return "raw";
    case OBJ_ENCODING_INT: return "int";
    case OBJ_ENCODING_HT: return "hashtable";
    case OBJ_ENCODING_QUICKLIST: return "quicklist";
    case OBJ_ENCODING_ZIPLIST: return "ziplist";
    case OBJ_ENCODING_INTSET: return "intset";
    case OBJ_ENCODING_SKIPLIST: return "skiplist";
    case OBJ_ENCODING_EMBSTR: return "embstr";
    default: return "unknown";
    }
}

/* Given an object returns the min number of milliseconds the object was never
 * requested, using an approximated LRU algorithm. */
//使用近似LRU算法，计算给定对象的空转时长
unsigned long long estimateObjectIdleTime(robj *o) {
    unsigned long long lruclock = LRU_CLOCK();
    if (lruclock >= o->lru) {
        return (lruclock - o->lru) * LRU_CLOCK_RESOLUTION;
    } else {
        return (lruclock + (LRU_CLOCK_MAX - o->lru)) *
                    LRU_CLOCK_RESOLUTION;
    }
}

/* This is a helper function for the OBJECT command. We need to lookup keys
 * without any modification of LRU or other parameters. */
//OBJECT 命令的辅助函数，用于在不修改 LRU 时间的情况下，尝试获取 key 对象
robj *objectCommandLookup(client *c, robj *key) {
    dictEntry *de;

    if ((de = dictFind(c->db->dict,key->ptr)) == NULL) return NULL;
    return (robj*) dictGetVal(de);
}

//在不修改 LRU 时间的情况下，获取 key 对应的对象。如果对象不存在，则发送回复信息
robj *objectCommandLookupOrReply(client *c, robj *key, robj *reply) {
    robj *o = objectCommandLookup(c,key);

    if (!o) addReply(c, reply);
    return o;
}

/* Object command allows to inspect the internals of an Redis Object.
 * Usage: OBJECT <refcount|encoding|idletime> <key> */
//检查对象的内部成员，且不修改 LRU 时间
void objectCommand(client *c) {
    robj *o;

    //返回对象的引用计数
    if (!strcasecmp(c->argv[1]->ptr,"refcount") && c->argc == 3) {
        if ((o = objectCommandLookupOrReply(c,c->argv[2],shared.nullbulk))
                == NULL) return;
        addReplyLongLong(c,o->refcount);

    //返回对象的编码类型
    } else if (!strcasecmp(c->argv[1]->ptr,"encoding") && c->argc == 3) {
        if ((o = objectCommandLookupOrReply(c,c->argv[2],shared.nullbulk))
                == NULL) return;
        addReplyBulkCString(c,strEncoding(o->encoding));

    //返回对象的空转时间
    } else if (!strcasecmp(c->argv[1]->ptr,"idletime") && c->argc == 3) {
        if ((o = objectCommandLookupOrReply(c,c->argv[2],shared.nullbulk))
                == NULL) return;
        addReplyLongLong(c,estimateObjectIdleTime(o)/1000);
    } else {
        addReplyError(c,"Syntax error. Try OBJECT (refcount|encoding|idletime)");
    }
}

