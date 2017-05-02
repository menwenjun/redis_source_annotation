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
#include "cluster.h"

#include <signal.h>
#include <ctype.h>

void slotToKeyAdd(robj *key);
void slotToKeyDel(robj *key);
void slotToKeyFlush(void);

/*-----------------------------------------------------------------------------
 * C-level DB API
 *----------------------------------------------------------------------------*/

/* Low level key lookup API, not actually called directly from commands
 * implementations that should instead rely on lookupKeyRead(),
 * lookupKeyWrite() and lookupKeyReadWithFlags(). */
// 该函数被lookupKeyRead()和lookupKeyWrite()和lookupKeyReadWithFlags()调用
// 从数据库db中取出key的值对象，如果存在返回该对象，否则返回NULL
// 返回key对象的值对象
robj *lookupKey(redisDb *db, robj *key, int flags) {
    // 在数据库中查找key对象，返回保存该key的节点地址
    dictEntry *de = dictFind(db->dict,key->ptr);
    if (de) {   //如果找到
        robj *val = dictGetVal(de); //取出键对应的值对象

        /* Update the access time for the ageing algorithm.
         * Don't do it if we have a saving child, as this will trigger
         * a copy on write madness. */
        // 更新键的使用时间
        if (server.rdb_child_pid == -1 &&
            server.aof_child_pid == -1 &&
            !(flags & LOOKUP_NOTOUCH))
        {
            val->lru = LRU_CLOCK();
        }
        return val; //返回值对象
    } else {
        return NULL;
    }
}

/* Lookup a key for read operations, or return NULL if the key is not found
 * in the specified DB.
 *
 * As a side effect of calling this function:
 * 1. A key gets expired if it reached it's TTL.
 * 2. The key last access time is updated.
 * 3. The global keys hits/misses stats are updated (reported in INFO).
 *
 * This API should not be used when we write to the key after obtaining
 * the object linked to the key, but only for read only operations.
 *
 * Flags change the behavior of this command:
 *
 *  LOOKUP_NONE (or zero): no special flags are passed.
 *  LOOKUP_NOTOUCH: don't alter the last access time of the key.
 *
 * Note: this function also returns NULL is the key is logically expired
 * but still existing, in case this is a slave, since this API is called only
 * for read operations. Even if the key expiry is master-driven, we can
 * correctly report a key is expired on slaves even if the master is lagging
 * expiring our key via DELs in the replication link. */
// 以读操作取出key的值对象，没找到返回NULL
// 调用该函数的副作用如下：
// 1.如果一个键的到达过期时间TTL，该键被设置为过期的
// 2.键的使用时间信息被更新
// 3.全局键 hits/misses 状态被更新
// 注意：如果键在逻辑上已经过期但是仍然存在，函数返回NULL
robj *lookupKeyReadWithFlags(redisDb *db, robj *key, int flags) {
    robj *val;

    // 如果键已经过期且被删除
    if (expireIfNeeded(db,key) == 1) {
        /* Key expired. If we are in the context of a master, expireIfNeeded()
         * returns 0 only when the key does not exist at all, so it's save
         * to return NULL ASAP. */
        // 键已过期，如果是主节点环境，表示key已经绝对被删除，如果是从节点，
        if (server.masterhost == NULL) return NULL;

        /* However if we are in the context of a slave, expireIfNeeded() will
         * not really try to expire the key, it only returns information
         * about the "logical" status of the key: key expiring is up to the
         * master in order to have a consistent view of master's data set.
         *
         * However, if the command caller is not the master, and as additional
         * safety measure, the command invoked is a read-only command, we can
         * safely return NULL here, and provide a more consistent behavior
         * to clients accessign expired values in a read-only fashion, that
         * will say the key as non exisitng.
         *
         * Notably this covers GETs when slaves are used to scale reads. */
        // 如果我们在从节点环境， expireIfNeeded()函数不会删除过期的键，它返回的仅仅是键是否被删除的逻辑值
        // 过期的键由主节点负责，为了保证主从节点数据的一致
        if (server.current_client &&
            server.current_client != server.master &&
            server.current_client->cmd &&
            server.current_client->cmd->flags & CMD_READONLY)
        {
            return NULL;
        }
    }
    // 键没有过期，则返回键的值对象
    val = lookupKey(db,key,flags);
    // 更新 是否命中 的信息
    if (val == NULL)
        server.stat_keyspace_misses++;
    else
        server.stat_keyspace_hits++;
    return val;
}

/* Like lookupKeyReadWithFlags(), but does not use any flag, which is the
 * common case. */
// 以读操作取出key的值对象，会更新是否命中的信息
robj *lookupKeyRead(redisDb *db, robj *key) {
    return lookupKeyReadWithFlags(db,key,LOOKUP_NONE);
}

/* Lookup a key for write operations, and as a side effect, if needed, expires
 * the key if its TTL is reached.
 *
 * Returns the linked value object if the key exists or NULL if the key
 * does not exist in the specified DB. */
// 以写操作取出key的值对象，不更新是否命中的信息
robj *lookupKeyWrite(redisDb *db, robj *key) {
    expireIfNeeded(db,key);
    return lookupKey(db,key,LOOKUP_NONE);
}

// 以读操作取出key的值对象，如果key不存在，则发送reply信息，并返回NULL
robj *lookupKeyReadOrReply(client *c, robj *key, robj *reply) {
    robj *o = lookupKeyRead(c->db, key);
    if (!o) addReply(c,reply);
    return o;
}

// 以写操作取出key的值对象，如果key不存在，则发送reply信息，并返回NULL
robj *lookupKeyWriteOrReply(client *c, robj *key, robj *reply) {
    robj *o = lookupKeyWrite(c->db, key);
    if (!o) addReply(c,reply);
    return o;
}

/* Add the key to the DB. It's up to the caller to increment the reference
 * counter of the value if needed.
 *
 * The program is aborted if the key already exists. */
// 讲key-val键值对添加到数据库中，该函数的调用者负责增加key-val的引用计数
void dbAdd(redisDb *db, robj *key, robj *val) {
    sds copy = sdsdup(key->ptr);    //复制key字符串
    int retval = dictAdd(db->dict, copy, val);  //将key-val添加到键值对字典

    serverAssertWithInfo(NULL,key,retval == DICT_OK);
    // 如果值对象是列表类型，有阻塞的命令，因此将key加入ready_keys字典中
    if (val->type == OBJ_LIST) signalListAsReady(db, key);
    // 如果开启了集群模式，则讲key添加到槽中
    if (server.cluster_enabled) slotToKeyAdd(key);
 }

/* Overwrite an existing key with a new value. Incrementing the reference
 * count of the new value is up to the caller.
 * This function does not modify the expire time of the existing key.
 *
 * The program is aborted if the key was not already present. */
 // 用一个新的val重写已经存在key，该函数的调用者负责增加key-val的引用计数
 // 该函数不修改该key的过期时间，如果key不存在，则程序终止
void dbOverwrite(redisDb *db, robj *key, robj *val) {
    dictEntry *de = dictFind(db->dict,key->ptr);    //找到保存key的节点地址

    serverAssertWithInfo(NULL,key,de != NULL);      //确保key被找到
    dictReplace(db->dict, key->ptr, val);           //重写val
}

/* High level Set operation. This function can be used in order to set
 * a key, whatever it was existing or not, to a new object.
 *
 * 1) The ref count of the value object is incremented.
 * 2) clients WATCHing for the destination key notified.
 * 3) The expire time of the key is reset (the key is made persistent). */
// 高级的设置key，无论key是否存在，都将val与其关联
// 1.value对象的引用计数被增加
// 2.监控key的客户端收到键被修改的通知
// 3.键的过期时间被设置为永久
void setKey(redisDb *db, robj *key, robj *val) {
    // 如果key不存在
    if (lookupKeyWrite(db,key) == NULL) {
        dbAdd(db,key,val);  //讲key-val添加到db中
    } else {    //key存在
        dbOverwrite(db,key,val);    //用val讲key的原值覆盖
    }
    incrRefCount(val);          //val引用计数加1
    removeExpire(db,key);       //移除key的过期时间
    signalModifiedKey(db,key);  //发送键被修改的信号
}

// 检查key是否存在于db中，返回1 表示存在
int dbExists(redisDb *db, robj *key) {
    //从键值对字典中查找
    return dictFind(db->dict,key->ptr) != NULL;
}

/* Return a random key, in form of a Redis object.
 * If there are no keys, NULL is returned.
 *
 * The function makes sure to return keys not already expired. */
// 随机返回一个键的字符串类型的对象，且保证返回的键没有过期
robj *dbRandomKey(redisDb *db) {
    dictEntry *de;

    while(1) {
        sds key;
        robj *keyobj;

        // 从键值对字典中随机返回一个节点地址
        de = dictGetRandomKey(db->dict);
        if (de == NULL) return NULL;

        // 获取该节点保存的
        key = dictGetKey(de);
        // 为key创建一个字符串对象
        keyobj = createStringObject(key,sdslen(key));
        //如果这个key在过期字典中，检查key是否过期，如果过期且被删除，则释放该key对象，并且重新随机返回一个key
        if (dictFind(db->expires,key)) {
            if (expireIfNeeded(db,keyobj)) {
                decrRefCount(keyobj);
                continue; /* search for another key. This expired. */
            }
        }
        return keyobj;  //返回对象
    }
}

/* Delete a key, value, and associated expiration entry if any, from the DB */
// 删除一个键值对以及键的过期时间，返回1表示删除成功
int dbDelete(redisDb *db, robj *key) {
    /* Deleting an entry from the expires dict will not free the sds of
     * the key, because it is shared with the main dictionary. */
    // 过期字典中有键，那么将key对象从过期字典中删除
    if (dictSize(db->expires) > 0) dictDelete(db->expires,key->ptr);
    // 将key-value从键值对字典中删除
    if (dictDelete(db->dict,key->ptr) == DICT_OK) {
        // 如果开启了集群模式，那么从槽中删除给定的键
        if (server.cluster_enabled) slotToKeyDel(key);
        return 1;
    } else {
        return 0;
    }
}

/* Prepare the string object stored at 'key' to be modified destructively
 * to implement commands like SETBIT or APPEND.
 *
 * An object is usually ready to be modified unless one of the two conditions
 * are true:
 *
 * 1) The object 'o' is shared (refcount > 1), we don't want to affect
 *    other users.
 * 2) The object encoding is not "RAW".
 *
 * If the object is found in one of the above conditions (or both) by the
 * function, an unshared / not-encoded copy of the string object is stored
 * at 'key' in the specified 'db'. Otherwise the object 'o' itself is
 * returned.
 *
 * USAGE:
 *
 * The object 'o' is what the caller already obtained by looking up 'key'
 * in 'db', the usage pattern looks like this:
 *
 * o = lookupKeyWrite(db,key);
 * if (checkType(c,o,OBJ_STRING)) return;
 * o = dbUnshareStringValue(db,key,o);
 *
 * At this point the caller is ready to modify the object, for example
 * using an sdscat() call to append some data, or anything else.
 */
// 解除key的值对象的共享，用于修改key的值
robj *dbUnshareStringValue(redisDb *db, robj *key, robj *o) {
    serverAssert(o->type == OBJ_STRING);
    // 如果o对象是共享的(refcount > 1)，或者o对象的编码不是RAW的
    if (o->refcount != 1 || o->encoding != OBJ_ENCODING_RAW) {
        robj *decoded = getDecodedObject(o);    //获取o的字符串类型对象
        // 根据o的字符串类型对象新创建一个RAW对象
        o = createRawStringObject(decoded->ptr, sdslen(decoded->ptr));
        decrRefCount(decoded);  //原有的对象解除共享
        dbOverwrite(db,key,o);  //重写key的val对象此时val对象是唯一的
    }
    return o;
}

// 清空所有数据库，返回删除键的个数
long long emptyDb(void(callback)(void*)) {
    int j;
    long long removed = 0;

    // 遍历所有的数据库
    for (j = 0; j < server.dbnum; j++) {
        // 记录被删除键的数量
        removed += dictSize(server.db[j].dict);
        // 删除当前数据库的键值对字典
        dictEmpty(server.db[j].dict,callback);
        // 删除当前数据库的过期字典
        dictEmpty(server.db[j].expires,callback);
    }
    //如果开启了集群模式，那么移除槽记录
    if (server.cluster_enabled) slotToKeyFlush();
    return removed;
}

// 切换数据库
int selectDb(client *c, int id) {
    // id非法，返回错误
    if (id < 0 || id >= server.dbnum)
        return C_ERR;
    // 设置当前client的数据库
    c->db = &server.db[id];
    return C_OK;
}

/*-----------------------------------------------------------------------------
 * Hooks for key space changes.
 *
 * Every time a key in the database is modified the function
 * signalModifiedKey() is called.
 *
 * Every time a DB is flushed the function signalFlushDb() is called.
 *----------------------------------------------------------------------------*/
// 当key被修改，调用该函数
void signalModifiedKey(redisDb *db, robj *key) {
    touchWatchedKey(db,key);
}

// 当数据库被清空，调用该函数
void signalFlushedDb(int dbid) {
    touchWatchedKeysOnFlush(dbid);
}

/*-----------------------------------------------------------------------------
 * Type agnostic commands operating on the key space
 * 无类型命令的数据库操作
 *----------------------------------------------------------------------------*/

// 清空client的数据库
void flushdbCommand(client *c) {
    // 更新脏键
    server.dirty += dictSize(c->db->dict);
    // 当数据库被清空，调用该函数
    signalFlushedDb(c->db->id);
    // 清空键值对字典和过期字典
    dictEmpty(c->db->dict,NULL);
    dictEmpty(c->db->expires,NULL);
    // 如果开启了集群模式，那么移除槽记录
    if (server.cluster_enabled) slotToKeyFlush();
    addReply(c,shared.ok);  //回复client
}

// 清空服务器内的所有数据库
void flushallCommand(client *c) {
    // 当数据库被清空，调用该函数
    signalFlushedDb(-1);
    // 更新脏键
    server.dirty += emptyDb(NULL);
    addReply(c,shared.ok);  //回复client
    // 如果正在执行RDB，取消执行的进程
    if (server.rdb_child_pid != -1) {
        kill(server.rdb_child_pid,SIGUSR1);
        // 删除临时文件
        rdbRemoveTempFile(server.rdb_child_pid);
    }
    // 更新RDB文件
    if (server.saveparamslen > 0) {
        /* Normally rdbSave() will reset dirty, but we don't want this here
         * as otherwise FLUSHALL will not be replicated nor put into the AOF. */
        // 正常的rdbSave()将会重置脏键，为了将脏键值放入AOF，需要备份脏键值
        int saved_dirty = server.dirty;
        // RDB持久化：程序将当前内存中的数据库快照保存到磁盘文件中
        rdbSave(server.rdb_filename);
        // 还原脏键
        server.dirty = saved_dirty;
    }
    server.dirty++; //更新脏键
}

// DEL key [key ...]
// DEL 命令实现
void delCommand(client *c) {
    int deleted = 0, j;

    // 遍历所有的key
    for (j = 1; j < c->argc; j++) {
        // 检查是否过期，过期删除
        expireIfNeeded(c->db,c->argv[j]);
        // 将当前key从数据库中删除
        if (dbDelete(c->db,c->argv[j])) {
            // 键被修改，发送信号
            signalModifiedKey(c->db,c->argv[j]);
            // 发送"del"事件通知
            notifyKeyspaceEvent(NOTIFY_GENERIC,
                "del",c->argv[j],c->db->id);
            // 更新脏键和被删除的键的数量
            server.dirty++;
            deleted++;
        }
    }
    addReplyLongLong(c,deleted);    //发送被删除键的数量给client
}

/* EXISTS key1 key2 ... key_N.
 * Return value is the number of keys existing. */
// EXISTS key [key ...]
// EXISTS 命令实现
void existsCommand(client *c) {
    long long count = 0;
    int j;

    // 遍历所有key
    for (j = 1; j < c->argc; j++) {
        // 检查是否过期，过期删除
        expireIfNeeded(c->db,c->argv[j]);
        // 如果当前key存在于数据库中，则计数加1
        if (dbExists(c->db,c->argv[j])) count++;
    }
    addReplyLongLong(c,count);//发送key存在的数量给client
}

// SELECT index
// SELECT命令实现
void selectCommand(client *c) {
    long id;

    // 将index转换为整数保存在id中
    if (getLongFromObjectOrReply(c, c->argv[1], &id,
        "invalid DB index") != C_OK)
        return;

    // 如果开启了集群模式但是id不是0好数据库，发送错误信息
    // 因为集群模式下，Redis只能使用ID为0的数据库，不支持多数据库空间。
    if (server.cluster_enabled && id != 0) {
        addReplyError(c,"SELECT is not allowed in cluster mode");
        return;
    }
    // 切换数据库
    if (selectDb(c,id) == C_ERR) {
        addReplyError(c,"invalid DB index");
    } else {
        addReply(c,shared.ok);
    }
}

// RANDOMKEY 命令实现 ，不删除返回的key
void randomkeyCommand(client *c) {
    robj *key;

    // 随机返回一个key，如果数据库为空则发送空回复
    if ((key = dbRandomKey(c->db)) == NULL) {
        addReply(c,shared.nullbulk);
        return;
    }

    addReplyBulk(c,key);    //将key回复给client
    decrRefCount(key);      //释放临时key对象
}

// KEYS pattern
// KEYS 命令实现
void keysCommand(client *c) {
    dictIterator *di;
    dictEntry *de;
    sds pattern = c->argv[1]->ptr;          //保存pattern参数
    int plen = sdslen(pattern), allkeys;    //
    unsigned long numkeys = 0;
    void *replylen = addDeferredMultiBulkLength(c); //因为不知道有多少命令回复，那么创建一个空链表，之后将回复填入

    // 安全字典迭代器
    di = dictGetSafeIterator(c->db->dict);
    // 如果pattern是以"*"开头，那么就返回所有键
    allkeys = (pattern[0] == '*' && pattern[1] == '\0');
    // 迭代字典中的节点
    while((de = dictNext(di)) != NULL) {
        sds key = dictGetKey(de);   //保存当前节点中的key
        robj *keyobj;

        // 如果有和pattern匹配的key
        if (allkeys || stringmatchlen(pattern,plen,key,sdslen(key),0)) {
            // 创建字符串对象
            keyobj = createStringObject(key,sdslen(key));
            // 检查是否可以对象过期，没有过期就将该键对象回复给client
            if (expireIfNeeded(c->db,keyobj) == 0) {
                addReplyBulk(c,keyobj);
                numkeys++;
            }
            decrRefCount(keyobj);   //释放临时对象
        }
    }
    dictReleaseIterator(di);    //释放字典迭代器
    setDeferredMultiBulkLength(c,replylen,numkeys); //设置回复client的长度
}

/* This callback is used by scanGenericCommand in order to collect elements
 * returned by the dictionary iterator into a list. */
// scanCallback函数被scanGenericCommand函数使用，为了保存被字典迭代器返回到列表中的元素
void scanCallback(void *privdata, const dictEntry *de) {
    void **pd = (void**) privdata;
    list *keys = pd[0];         //被迭代的元素列表
    robj *o = pd[1];            //当前值对象
    robj *key, *val = NULL;

    // 根据不同的编码类型，将字典节点de保存的键对象和值对象取出来，保存到key中，值对象保存到val中
    if (o == NULL) {
        sds sdskey = dictGetKey(de);
        key = createStringObject(sdskey, sdslen(sdskey));
    } else if (o->type == OBJ_SET) {
        key = dictGetKey(de);
        incrRefCount(key);
    } else if (o->type == OBJ_HASH) {
        key = dictGetKey(de);
        incrRefCount(key);
        val = dictGetVal(de);
        incrRefCount(val);
    } else if (o->type == OBJ_ZSET) {
        key = dictGetKey(de);
        incrRefCount(key);
        val = createStringObjectFromLongDouble(*(double*)dictGetVal(de),0);
    } else {
        serverPanic("Type not handled in SCAN callback.");
    }

    // 将key保存到被迭代元素的列表中，如果有值val，同样加入到列表中
    listAddNodeTail(keys, key);
    if (val) listAddNodeTail(keys, val);
}

/* Try to parse a SCAN cursor stored at object 'o':
 * if the cursor is valid, store it as unsigned integer into *cursor and
 * returns C_OK. Otherwise return C_ERR and send an error to the
 * client. */
// 获取scan命令的游标，尝试取解析一个保存在o中的游标，如果游标合法，保存到cursor中否则返回C_ERR
int parseScanCursorOrReply(client *c, robj *o, unsigned long *cursor) {
    char *eptr;

    /* Use strtoul() because we need an *unsigned* long, so
     * getLongLongFromObject() does not cover the whole cursor space. */
    errno = 0;
    //将o对象的字符串类型值转换为unsigned  long int类型10进制数
    *cursor = strtoul(o->ptr, &eptr, 10);
    // 转换错误检查
    if (isspace(((char*)o->ptr)[0]) || eptr[0] != '\0' || errno == ERANGE)
    {
        addReplyError(c, "invalid cursor");
        return C_ERR;
    }
    return C_OK;
}

/* This command implements SCAN, HSCAN and SSCAN commands.
 * If object 'o' is passed, then it must be a Hash or Set object, otherwise
 * if 'o' is NULL the command will operate on the dictionary associated with
 * the current database.
 *
 * When 'o' is not NULL the function assumes that the first argument in
 * the client arguments vector is a key so it skips it before iterating
 * in order to parse options.
 *
 * In the case of a Hash object the function returns both the field and value
 * of every element on the Hash. */
// SCAN cursor [MATCH pattern] [COUNT count]
// SCAN、HSCAN、SSCAN、ZSCAN一类命令底层实现
// o对象必须是哈希对象或集合对象，否则命令将操作当前数据库
// 如果o不是NULL，那么说明他是一个哈希或集合对象，函数将跳过这些键对象，对参数进行分析
// 如果是哈希对象，返回返回的是键值对
void scanGenericCommand(client *c, robj *o, unsigned long cursor) {
    int i, j;
    list *keys = listCreate();  //创建一个列表
    listNode *node, *nextnode;
    long count = 10;
    sds pat = NULL;
    int patlen = 0, use_pattern = 0;
    dict *ht;

    /* Object must be NULL (to iterate keys names), or the type of the object
     * must be Set, Sorted Set, or Hash. */
    // 输入类型的检查，要么迭代键名，要么当前集合对象，要么迭代哈希对象，要么迭代有序集合对象
    serverAssert(o == NULL || o->type == OBJ_SET || o->type == OBJ_HASH ||
                o->type == OBJ_ZSET);

    /* Set i to the first option argument. The previous one is the cursor. */
    // 计算第一个参数的下标，如果是键名，要条跳过该键
    i = (o == NULL) ? 2 : 3; /* Skip the key argument if needed. */

    /* Step 1: Parse options. */
    // 1. 解析选项
    while (i < c->argc) {
        j = c->argc - i;
        // 设定COUNT参数，COUNT 选项的作用就是让用户告知迭代命令， 在每次迭代中应该返回多少元素。
        if (!strcasecmp(c->argv[i]->ptr, "count") && j >= 2) {
            //保存个数到count
            if (getLongFromObjectOrReply(c, c->argv[i+1], &count, NULL)
                != C_OK)
            {
                goto cleanup;
            }

            // 如果个数小于1，语法错误
            if (count < 1) {
                addReply(c,shared.syntaxerr);
                goto cleanup;
            }

            i += 2; //参数跳过两个已经解析过的
        // 设定MATCH参数，让命令只返回和给定模式相匹配的元素。
        } else if (!strcasecmp(c->argv[i]->ptr, "match") && j >= 2) {
            pat = c->argv[i+1]->ptr;    //pattern字符串
            patlen = sdslen(pat);       //pattern字符串长度

            /* The pattern always matches if it is exactly "*", so it is
             * equivalent to disabling it. */
            // 如果pattern是"*"，就不用匹配，全部返回，设置为0
            use_pattern = !(pat[0] == '*' && patlen == 1);

            i += 2;
        } else {
            addReply(c,shared.syntaxerr);
            goto cleanup;
        }
    }

    /* Step 2: Iterate the collection.
     *
     * Note that if the object is encoded with a ziplist, intset, or any other
     * representation that is not a hash table, we are sure that it is also
     * composed of a small number of elements. So to avoid taking state we
     * just return everything inside the object in a single call, setting the
     * cursor to zero to signal the end of the iteration. */

    /* Handle the case of a hash table. */
    // 2.如果对象是ziplist、intset或其他而不是哈希表，那么这些类型只是包含少量的元素
    // 我们一次将其所有的元素全部返回给调用者，并设置游标cursor为0，标示迭代完成
    ht = NULL;
    // 迭代目标是数据库
    if (o == NULL) {
        ht = c->db->dict;
    // 迭代目标是HT编码的集合对象
    } else if (o->type == OBJ_SET && o->encoding == OBJ_ENCODING_HT) {
        ht = o->ptr;
    // 迭代目标是HT编码的哈希对象
    } else if (o->type == OBJ_HASH && o->encoding == OBJ_ENCODING_HT) {
        ht = o->ptr;
        count *= 2; /* We return key / value for this type. */
    // 迭代目标是skiplist编码的有序集合对象
    } else if (o->type == OBJ_ZSET && o->encoding == OBJ_ENCODING_SKIPLIST) {
        zset *zs = o->ptr;
        ht = zs->dict;
        count *= 2; /* We return key / value for this type. */
    }

    if (ht) {
        void *privdata[2];
        /* We set the max number of iterations to ten times the specified
         * COUNT, so if the hash table is in a pathological state (very
         * sparsely populated) we avoid to block too much time at the cost
         * of returning no or very few elements. */
        // 设置最大的迭代长度为10*count次
        long maxiterations = count*10;

        /* We pass two pointers to the callback: the list to which it will
         * add new elements, and the object containing the dictionary so that
         * it is possible to fetch more data in a type-dependent way. */
        // 回调函数scanCallback的参数privdata是一个数组，保存的是被迭代对象的键和值
        // 回调函数scanCallback的另一个参数，是一个字典对象
        // 回调函数scanCallback的作用，从字典对象中将键值对提取出来，不用管字典对象是什么数据类型
        privdata[0] = keys;
        privdata[1] = o;
        // 循环扫描ht，从游标cursor开始，调用指定的scanCallback函数，提出ht中的数据到刚开始创建的列表keys中
        do {
            cursor = dictScan(ht, cursor, scanCallback, privdata);
        } while (cursor &&
              maxiterations-- &&
              listLength(keys) < (unsigned long)count);//没迭代完，或没迭代够count，就继续循环

    // 如果是集合对象但编码不是HT是整数集合
    } else if (o->type == OBJ_SET) {
        int pos = 0;
        int64_t ll;
        // 将整数值取出来，构建成字符串对象加入到keys列表中，游标设置为0，表示迭代完成
        while(intsetGet(o->ptr,pos++,&ll))
            listAddNodeTail(keys,createStringObjectFromLongLong(ll));
        cursor = 0;
    // 如果是哈希对象，或有序集合对象，但是编码都不是HT，是ziplist
    } else if (o->type == OBJ_HASH || o->type == OBJ_ZSET) {
        unsigned char *p = ziplistIndex(o->ptr,0);
        unsigned char *vstr;
        unsigned int vlen;
        long long vll;

        while(p) {
            // 将值取出来，根据不同类型的值，构建成相同的字符串对象，加入到keys列表中
            ziplistGet(p,&vstr,&vlen,&vll);
            listAddNodeTail(keys,
                (vstr != NULL) ? createStringObject((char*)vstr,vlen) :
                                 createStringObjectFromLongLong(vll));
            p = ziplistNext(o->ptr,p);
        }
        cursor = 0;
    } else {
        serverPanic("Not handled encoding in SCAN.");
    }

    /* Step 3: Filter elements. */
    // 3. 如果设置MATCH参数，要进行过滤
    node = listFirst(keys); //链表首节点地址
    while (node) {
        robj *kobj = listNodeValue(node);   //key对象
        nextnode = listNextNode(node);      //下一个节点地址
        int filter = 0; //默认为不过滤

        /* Filter element if it does not match the pattern. */
        //pattern不是"*"因此要过滤
        if (!filter && use_pattern) {
            // 如果kobj是字符串对象
            if (sdsEncodedObject(kobj)) {
                // kobj的值不匹配pattern，设置过滤标志
                if (!stringmatchlen(pat, patlen, kobj->ptr, sdslen(kobj->ptr), 0))
                    filter = 1;
            // 如果kobj是整数对象
            } else {
                char buf[LONG_STR_SIZE];
                int len;

                serverAssert(kobj->encoding == OBJ_ENCODING_INT);
                // 将整数转换为字符串类型，保存到buf中
                len = ll2string(buf,sizeof(buf),(long)kobj->ptr);
                //buf的值不匹配pattern，设置过滤标志
                if (!stringmatchlen(pat, patlen, buf, len, 0)) filter = 1;
            }
        }

        /* Filter element if it is an expired key. */
        // 迭代目标是数据库，如果kobj是过期键，则过滤
        if (!filter && o == NULL && expireIfNeeded(c->db, kobj)) filter = 1;

        /* Remove the element and its associted value if needed. */
        // 如果该键满足了上述的过滤条件，那么将其从keys列表删除并释放
        if (filter) {
            decrRefCount(kobj);
            listDelNode(keys, node);
        }

        /* If this is a hash or a sorted set, we have a flat list of
         * key-value elements, so if this element was filtered, remove the
         * value, or skip it if it was not filtered: we only match keys. */
        // 如果当前迭代目标是有序集合或哈希对象，因此keys列表中保存的是键值对，如果key键对象被过滤，值对象也应当被过滤
        if (o && (o->type == OBJ_ZSET || o->type == OBJ_HASH)) {
            node = nextnode;
            nextnode = listNextNode(node);  //值对象的节点地址
            // 如果该键满足了上述的过滤条件，那么将其从keys列表删除并释放
            if (filter) {
                kobj = listNodeValue(node); //取出值对象
                decrRefCount(kobj);
                listDelNode(keys, node);    //删除
            }
        }
        node = nextnode;
    }

    /* Step 4: Reply to the client. */
    // 4. 回复信息给client
    addReplyMultiBulkLen(c, 2);     //2部分，一个是游标，一个是列表
    addReplyBulkLongLong(c,cursor); //回复游标

    addReplyMultiBulkLen(c, listLength(keys));  //回复列表长度

    //循环回复列表中的元素，并释放
    while ((node = listFirst(keys)) != NULL) {
        robj *kobj = listNodeValue(node);
        addReplyBulk(c, kobj);
        decrRefCount(kobj);
        listDelNode(keys, node);
    }

// 清理代码
cleanup:
    listSetFreeMethod(keys,decrRefCountVoid);   //设置特定的释放列表的方式decrRefCountVoid
    listRelease(keys);                          //释放
}

/* The SCAN command completely relies on scanGenericCommand. */
// SCAN cursor [MATCH pattern] [COUNT count]
// SCAN 命令实现
void scanCommand(client *c) {
    unsigned long cursor;
    // 获取scan命令的游标，尝试取解析一个保存cursor参数中的游标，如果游标合法，保存到cursor中否则返回C_ERR
    if (parseScanCursorOrReply(c,c->argv[1],&cursor) == C_ERR) return;
    scanGenericCommand(c,NULL,cursor);
}

// DBSIZE 命令实现，返回当前数据库的 key 的数量。
void dbsizeCommand(client *c) {
    addReplyLongLong(c,dictSize(c->db->dict));  //回复数据库中键值对字典的大小值
}

// LASTSAVE 返回最近一次 Redis 成功将数据保存到磁盘上的时间，以 UNIX 时间戳格式表示。
void lastsaveCommand(client *c) {
    addReplyLongLong(c,server.lastsave);
}

// TYPE key 返回 key 所储存的值的类型。
// TYPE 命令实现
void typeCommand(client *c) {
    robj *o;
    char *type;

    // 以读操作取出key参数的值对象，并且不修改键的使用时间
    o = lookupKeyReadWithFlags(c->db,c->argv[1],LOOKUP_NOTOUCH);
    if (o == NULL) {
        type = "none";
    } else {
        switch(o->type) {
        case OBJ_STRING: type = "string"; break;
        case OBJ_LIST: type = "list"; break;
        case OBJ_SET: type = "set"; break;
        case OBJ_ZSET: type = "zset"; break;
        case OBJ_HASH: type = "hash"; break;
        default: type = "unknown"; break;
        }
    }
    addReplyStatus(c,type);     //返回类型字符串
}

// SHUTDOWN [SAVE|NOSAVE]
// 执行 SHUTDOWN SAVE 会强制让数据库执行保存操作，即使没有设定(configure)保存点
// 执行 SHUTDOWN NOSAVE 会阻止数据库执行保存操作，即使已经设定有一个或多个保存点(你可以将这一用法看作是强制停止服务器的一个假想的 ABORT 命令)

// SHUTDOWN 命令实现
void shutdownCommand(client *c) {
    int flags = 0;

    if (c->argc > 2) {
        addReply(c,shared.syntaxerr);   //语法错误
        return;
    } else if (c->argc == 2) {
        //指定NOSAVE，停机不保存
        if (!strcasecmp(c->argv[1]->ptr,"nosave")) {
            flags |= SHUTDOWN_NOSAVE;
        // 制定SAVE，停机保存
        } else if (!strcasecmp(c->argv[1]->ptr,"save")) {
            flags |= SHUTDOWN_SAVE;
        } else {
            addReply(c,shared.syntaxerr);
            return;
        }
    }
    /* When SHUTDOWN is called while the server is loading a dataset in
     * memory we need to make sure no attempt is performed to save
     * the dataset on shutdown (otherwise it could overwrite the current DB
     * with half-read data).
     *
     * Also when in Sentinel mode clear the SAVE flag and force NOSAVE. */
    // 如果服务器正在载入数据集或者是正在处于集群模式
    if (server.loading || server.sentinel_mode)
        // 清除SHUTDOWN_SAVE标志，强制设置为SHUTDOWN_NOSAVE
        flags = (flags & ~SHUTDOWN_SAVE) | SHUTDOWN_NOSAVE;
    // 准备停机，处理停机前的操作，例如杀死子进程，刷新缓冲区，关闭socket等，调用exit(0)退出
    if (prepareForShutdown(flags) == C_OK) exit(0);
    addReplyError(c,"Errors trying to SHUTDOWN. Check logs.");
}
// RENAME key newkey
// RENAMENX key newkey
// RENAME、RENAMENX命令底层实现
void renameGenericCommand(client *c, int nx) {
    robj *o;
    long long expire;
    int samekey = 0;

    /* When source and dest key is the same, no operation is performed,
     * if the key exists, however we still return an error on unexisting key. */
    // key和newkey相同的话，设置samekey标志
    if (sdscmp(c->argv[1]->ptr,c->argv[2]->ptr) == 0) samekey = 1;

    // 以写操作读取key的值对象
    if ((o = lookupKeyWriteOrReply(c,c->argv[1],shared.nokeyerr)) == NULL)
        return;

    // 如果key和newkey相同，nx为1发送0，否则为ok
    if (samekey) {
        addReply(c,nx ? shared.czero : shared.ok);
        return;
    }

    // 增加值对象的引用计数，保护起来，用于关联newkey，以防删除了key顺带将值对象也删除
    incrRefCount(o);
    // 备份key的过期时间，将来作为newkey的过期时间
    expire = getExpire(c->db,c->argv[1]);
    // 判断newkey的值对象是否存在
    if (lookupKeyWrite(c->db,c->argv[2]) != NULL) {
        // 设置nx标志，则不符合已存在的条件，发送0
        if (nx) {
            decrRefCount(o);
            addReply(c,shared.czero);
            return;
        }
        /* Overwrite: delete the old key before creating the new one
         * with the same name. */
        dbDelete(c->db,c->argv[2]); //将旧的newkey对象删除
    }
    // 将newkey和key的值对象关联
    dbAdd(c->db,c->argv[2],o);
    // 如果newkey设置过过期时间，则为newkey设置过期时间
    if (expire != -1) setExpire(c->db,c->argv[2],expire);
    // 删除key
    dbDelete(c->db,c->argv[1]);
    // 发送这两个键被修改的信号
    signalModifiedKey(c->db,c->argv[1]);
    signalModifiedKey(c->db,c->argv[2]);
    // 发送不同命令的事件通知
    notifyKeyspaceEvent(NOTIFY_GENERIC,"rename_from",
        c->argv[1],c->db->id);
    notifyKeyspaceEvent(NOTIFY_GENERIC,"rename_to",
        c->argv[2],c->db->id);
    server.dirty++;     //更新脏键
    addReply(c,nx ? shared.cone : shared.ok);
}

// RENAME key newkey
// RENAME 命令实现
void renameCommand(client *c) {
    renameGenericCommand(c,0);
}

// RENAMENX key newkey
// RENAMENX 命令实现
void renamenxCommand(client *c) {
    renameGenericCommand(c,1);
}

// MOVE key db 将当前数据库的 key 移动到给定的数据库 db 当中。
// MOVE 命令实现
void moveCommand(client *c) {
    robj *o;
    redisDb *src, *dst;
    int srcid;
    long long dbid, expire;

    // 服务器处于集群模式，不支持多数据库
    if (server.cluster_enabled) {
        addReplyError(c,"MOVE is not allowed in cluster mode");
        return;
    }

    /* Obtain source and target DB pointers */
    // 获得源数据库和源数据库的id
    src = c->db;
    srcid = c->db->id;

    // 将参数db的值保存到dbid，并且切换到该数据库中
    if (getLongLongFromObject(c->argv[2],&dbid) == C_ERR ||
        dbid < INT_MIN || dbid > INT_MAX ||
        selectDb(c,dbid) == C_ERR)
    {
        addReply(c,shared.outofrangeerr);
        return;
    }
    // 目标数据库
    dst = c->db;
    // 切换回源数据库
    selectDb(c,srcid); /* Back to the source DB */

    /* If the user is moving using as target the same
     * DB as the source DB it is probably an error. */
    // 如果前后切换的数据库相同，则返回有关错误
    if (src == dst) {
        addReply(c,shared.sameobjecterr);
        return;
    }

    /* Check if the element exists and get a reference */
    // 以写操作取出源数据库的对象
    o = lookupKeyWrite(c->db,c->argv[1]);
    if (!o) {
        addReply(c,shared.czero);   //不存在发送0
        return;
    }
    // 备份key的过期时间
    expire = getExpire(c->db,c->argv[1]);

    /* Return zero if the key already exists in the target DB */
    // 判断当前key是否存在于目标数据库，存在直接返回，发送0
    if (lookupKeyWrite(dst,c->argv[1]) != NULL) {
        addReply(c,shared.czero);
        return;
    }
    // 将key-value对象添加到目标数据库中
    dbAdd(dst,c->argv[1],o);
    // 设置移动后key的过期时间
    if (expire != -1) setExpire(dst,c->argv[1],expire);
    incrRefCount(o);    //增加引用计数

    /* OK! key moved, free the entry in the source DB */
    // 从源数据库中将key和关联的值对象删除
    dbDelete(src,c->argv[1]);
    server.dirty++; //更新脏键
    addReply(c,shared.cone);    //回复1
}

/*-----------------------------------------------------------------------------
 * Expires API
 *----------------------------------------------------------------------------*/
// 移除key的过期时间，成功返回1
int removeExpire(redisDb *db, robj *key) {
    /* An expire may only be removed if there is a corresponding entry in the
     * main dict. Otherwise, the key will never be freed. */
    // key存在于键值对字典中
    serverAssertWithInfo(NULL,key,dictFind(db->dict,key->ptr) != NULL);
    // 从过期字典中删除该键
    return dictDelete(db->expires,key->ptr) == DICT_OK;
}

// 设置过期时间
void setExpire(redisDb *db, robj *key, long long when) {
    dictEntry *kde, *de;

    /* Reuse the sds from the main dict in the expire dict */
    kde = dictFind(db->dict,key->ptr);
    serverAssertWithInfo(NULL,key,kde != NULL);
    de = dictReplaceRaw(db->expires,dictGetKey(kde));
    dictSetSignedIntegerVal(de,when);
}

/* Return the expire time of the specified key, or -1 if no expire
 * is associated with this key (i.e. the key is non volatile) */
// 返回一个键的过期时间，如果该键没有设定过期时间，则返回-1
long long getExpire(redisDb *db, robj *key) {
    dictEntry *de;

    /* No expire? return ASAP */
    // 如果过期字典为空，或者过期字典中找不到指定的key，立即返回
    if (dictSize(db->expires) == 0 ||
       (de = dictFind(db->expires,key->ptr)) == NULL) return -1;

    /* The entry was found in the expire dict, this means it should also
     * be present in the main dict (safety check). */
    // 保存当前key的节点de不为空，说明该key设置了过期时间
    // 还要保证该key在键值对字典中存在
    serverAssertWithInfo(NULL,key,dictFind(db->dict,key->ptr) != NULL);
    // 将de节点的有符号整数据返回，也就是过期时间
    return dictGetSignedIntegerVal(de);
}

/* Propagate expires into slaves and the AOF file.
 * When a key expires in the master, a DEL operation for this key is sent
 * to all the slaves and the AOF file if enabled.
 *
 * This way the key expiry is centralized in one place, and since both
 * AOF and the master->slave link guarantee operation ordering, everything
 * will be consistent even if we allow write operations against expiring
 * keys. */
// 将过期时间传播到从节点和AOF文件
// 当一个键在主节点中过期时，主节点会发送del命令给从节点和AOF文件
void propagateExpire(redisDb *db, robj *key) {
    robj *argv[2];

    // 构造一个参数列表
    argv[0] = shared.del;
    argv[1] = key;
    incrRefCount(argv[0]);
    incrRefCount(argv[1]);

    // 如果AOF状态为开启或可写的状态，
    if (server.aof_state != AOF_OFF)
        feedAppendOnlyFile(server.delCommand,db->id,argv,2);    //将del命令追加到AOF文件中
    replicationFeedSlaves(server.slaves,db->id,argv,2);         //将argv列表发送给服务器的从节点

    // 释放参数列表
    decrRefCount(argv[0]);
    decrRefCount(argv[1]);
}

// 检查键是否过期，如果过期，从数据库中删除
// 返回0表示没有过期或没有过期时间，返回1 表示键被删除
int expireIfNeeded(redisDb *db, robj *key) {
    //得到过期时间，单位毫秒
    mstime_t when = getExpire(db,key);
    mstime_t now;

    // 没有过期时间，直接返回
    if (when < 0) return 0; /* No expire for this key */

    /* Don't expire anything while loading. It will be done later. */
    // 服务器正在载入，那么不进行过期检查
    if (server.loading) return 0;

    /* If we are in the context of a Lua script, we claim that time is
     * blocked to when the Lua script started. This way a key can expire
     * only the first time it is accessed and not in the middle of the
     * script execution, making propagation to slaves / AOF consistent.
     * See issue #1525 on Github for more information. */
    // 返回一个Unix时间，单位毫秒
    now = server.lua_caller ? server.lua_time_start : mstime();

    /* If we are running in the context of a slave, return ASAP:
     * the slave key expiration is controlled by the master that will
     * send us synthesized DEL operations for expired keys.
     *
     * Still we try to return the right information to the caller,
     * that is, 0 if we think the key should be still valid, 1 if
     * we think the key is expired at this time. */
    // 如果服务器正在进行主从节点的复制，从节点的过期键应该被 主节点发送同步删除的操作 删除，而自己不主动删除
    // 从节点只返回正确的逻辑信息，0表示key仍然没有过期，1表示key过期。
    if (server.masterhost != NULL) return now > when;

    /* Return when this key has not expired */
    // 当键还没有过期时，直接返回0
    if (now <= when) return 0;

    /* Delete the key */
    // 键已经过期，删除键
    server.stat_expiredkeys++;              //过期键的数量加1
    propagateExpire(db,key);                //将过期键key传播给AOF文件和从节点
    notifyKeyspaceEvent(NOTIFY_EXPIRED,     //发送"expired"事件通知
        "expired",key,db->id);
    return dbDelete(db,key);                //从数据库中删除key
}

/*-----------------------------------------------------------------------------
 * Expires Commands
 *----------------------------------------------------------------------------*/

/* This is the generic command implementation for EXPIRE, PEXPIRE, EXPIREAT
 * and PEXPIREAT. Because the commad second argument may be relative or absolute
 * the "basetime" argument is used to signal what the base time is (either 0
 * for *AT variants of the command, or the current time for relative expires).
 *
 * unit is either UNIT_SECONDS or UNIT_MILLISECONDS, and is only used for
 * the argv[2] parameter. The basetime is always specified in milliseconds. */

// EXPIRE key seconds
// EXPIREAT key timestamp
// PEXPIRE key milliseconds
// PEXPIREAT key milliseconds-timestamp
// EXPIRE, PEXPIRE, EXPIREAT,PEXPIREAT命令的底层实现
// basetime参数可能是绝对值，可能是相对值。执行AT命令时basetime为0，否则保存的是当前的绝对时间
// unit 是UNIT_SECONDS 或者 UNIT_MILLISECONDS，但是basetime总是以毫秒为单位的。
void expireGenericCommand(client *c, long long basetime, int unit) {
    robj *key = c->argv[1], *param = c->argv[2];
    long long when; /* unix time in milliseconds when the key will expire. */

    // 取出时间参数保存到when中
    if (getLongLongFromObjectOrReply(c, param, &when, NULL) != C_OK)
        return;

    // 如果过期时间是以秒为单位，则转换为毫秒值
    if (unit == UNIT_SECONDS) when *= 1000;
    // 绝对时间
    when += basetime;

    /* No key, return zero. */
    // 判断key是否在数据库中，不在返回0
    if (lookupKeyWrite(c->db,key) == NULL) {
        addReply(c,shared.czero);
        return;
    }

    /* EXPIRE with negative TTL, or EXPIREAT with a timestamp into the past
     * should never be executed as a DEL when load the AOF or in the context
     * of a slave instance.
     *
     * Instead we take the other branch of the IF statement setting an expire
     * (possibly in the past) and wait for an explicit DEL from the master. */
    // 如果当前正在载入AOF数据或者在从节点环境中，即使EXPIRE的TTL为负数，或者EXPIREAT的时间戳已经过期
    // 服务器都不会执行DEL命令，且将过期TTL设置为键的过期时间，等待主节点发来的DEL命令

    // 如果when已经过时，服务器为主节点且没有载入AOF数据
    if (when <= mstime() && !server.loading && !server.masterhost) {
        robj *aux;

        // 将key从数据库中删除
        serverAssertWithInfo(c,key,dbDelete(c->db,key));
        server.dirty++; //更新脏键

        /* Replicate/AOF this as an explicit DEL. */
        // 创建一个"DEL"命令
        aux = createStringObject("DEL",3);
        rewriteClientCommandVector(c,2,aux,key);    //修改客户端的参数列表为DEL命令
        decrRefCount(aux);
        // 发送键被修改的信号
        signalModifiedKey(c->db,key);
        // 发送"del"的事件通知
        notifyKeyspaceEvent(NOTIFY_GENERIC,"del",key,c->db->id);
        addReply(c, shared.cone);
        return;

    // 如果当前服务器是从节点，或者服务器正在载入AOF数据
    // 不管when有没有过时，都设置为过期时间
    } else {
        // 设置过期时间
        setExpire(c->db,key,when);
        addReply(c,shared.cone);
        signalModifiedKey(c->db,key);   //发送键被修改的信号
        notifyKeyspaceEvent(NOTIFY_GENERIC,"expire",key,c->db->id); //发送"expire"的事件通知
        server.dirty++; //更新脏键
        return;
    }
}

// EXPIRE key seconds
// EXPIRE 命令实现
void expireCommand(client *c) {
    expireGenericCommand(c,mstime(),UNIT_SECONDS);
}

// EXPIREAT key timestamp
// EXPIREAT 命令实现
void expireatCommand(client *c) {
    expireGenericCommand(c,0,UNIT_SECONDS);
}

// PEXPIRE key milliseconds
// PEXPIRE 命令实现
void pexpireCommand(client *c) {
    expireGenericCommand(c,mstime(),UNIT_MILLISECONDS);
}

// PEXPIREAT key milliseconds-timestamp
// PEXPIREAT 命令实现
void pexpireatCommand(client *c) {
    expireGenericCommand(c,0,UNIT_MILLISECONDS);
}

// TTL key
// PTTL key
// TTL、PTTL命令底层实现，output_ms为1，返回毫秒，为0返回秒
void ttlGenericCommand(client *c, int output_ms) {
    long long expire, ttl = -1;

    /* If the key does not exist at all, return -2 */
    // 判断key是否存在于数据库，并且不修改键的使用时间
    if (lookupKeyReadWithFlags(c->db,c->argv[1],LOOKUP_NOTOUCH) == NULL) {
        addReplyLongLong(c,-2);
        return;
    }
    /* The key exists. Return -1 if it has no expire, or the actual
     * TTL value otherwise. */
    // 如果key存在，则备份当前key的过期时间
    expire = getExpire(c->db,c->argv[1]);

    // 如果设置了过期时间
    if (expire != -1) {
        ttl = expire-mstime();  //计算生存时间
        if (ttl < 0) ttl = 0;
    }
    // 如果键是永久的
    if (ttl == -1) {
        addReplyLongLong(c,-1); //发送-1
    } else {
        addReplyLongLong(c,output_ms ? ttl : ((ttl+500)/1000)); //发送生存时间
    }
}

// TTL key
// TTL 命令实现
void ttlCommand(client *c) {
    ttlGenericCommand(c, 0);
}

// PTTL key
// PTTL 命令实现
void pttlCommand(client *c) {
    ttlGenericCommand(c, 1);
}

// PERSIST key 移除给定key的生存时间
// PERSIST 命令实现
void persistCommand(client *c) {
    dictEntry *de;

    // 在数据库中的键值对字典中查找当前key对象
    de = dictFind(c->db->dict,c->argv[1]->ptr);
    // 没找到回复0
    if (de == NULL) {
        addReply(c,shared.czero);
    } else {
        // 如果找到，则移除它的过期时间
        if (removeExpire(c->db,c->argv[1])) {
            addReply(c,shared.cone);
            server.dirty++;
        // 找到但是本来没有过期
        } else {
            addReply(c,shared.czero);
        }
    }
}

/* TOUCH key1 [key2 key3 ... keyN] */
// TOUCH key arg ...options...
// TOUCH 命令实现
void touchCommand(client *c) {
    int touched = 0;
    // 遍历所有的key参数
    for (int j = 1; j < c->argc; j++)
        // 如果存在数据库中，更新 是否击中 标志
        if (lookupKeyRead(c->db,c->argv[j]) != NULL) touched++; //计数存在的键的个数
    addReplyLongLong(c,touched);    //发送个数给client
}

/* -----------------------------------------------------------------------------
 * API to get key arguments from commands
 * ---------------------------------------------------------------------------*/

/* The base case is to use the keys position as given in the command table
 * (firstkey, lastkey, step). */
//获取命令中的所有 key
int *getKeysUsingCommandTable(struct redisCommand *cmd,robj **argv, int argc, int *numkeys) {
    int j, i = 0, last, *keys;
    UNUSED(argv);

    // 如果第一个参数就是key，返回空，numkeys设置为0
    if (cmd->firstkey == 0) {
        *numkeys = 0;
        return NULL;
    }
    // 最后一个参数下标
    last = cmd->lastkey;
    // 如果是负数形式，则转换为正数
    if (last < 0) last = argc+last;
    // 分配整型数组的空间
    keys = zmalloc(sizeof(int)*((last - cmd->firstkey)+1));
    //遍历所有的参数
    for (j = cmd->firstkey; j <= last; j += cmd->keystep) {
        serverAssert(j < argc);
        keys[i++] = j;  //记录参数的下标
    }
    *numkeys = i;   //保存参数的个数
    return keys;    //返回数组地址
}

/* Return all the arguments that are keys in the command passed via argc / argv.
 *
 * The command returns the positions of all the key arguments inside the array,
 * so the actual return value is an heap allocated array of integers. The
 * length of the array is returned by reference into *numkeys.
 *
 * 'cmd' must be point to the corresponding entry into the redisCommand
 * table, according to the command name in argv[0].
 *
 * This function uses the command table if a command-specific helper function
 * is not required, otherwise it calls the command-specific function. */
// 从argv和argc指定的参数列表中返回所有的键
int *getKeysFromCommand(struct redisCommand *cmd, robj **argv, int argc, int *numkeys) {
    if (cmd->getkeys_proc) {    //如果指定了特定的函数，则调用该函数
        return cmd->getkeys_proc(cmd,argv,argc,numkeys);
    } else {
        return getKeysUsingCommandTable(cmd,argv,argc,numkeys);
    }
}

/* Free the result of getKeysFromCommand. */
// 释放整型数组空间
void getKeysFreeResult(int *result) {
    zfree(result);
}

/* Helper function to extract keys from following commands:
 * ZUNIONSTORE <destkey> <num-keys> <key> <key> ... <key> <options>
 * ZINTERSTORE <destkey> <num-keys> <key> <key> ... <key> <options> */
// 从ZUNIONSTORE、ZINTERSTORE命令中提取key的下标
int *zunionInterGetKeys(struct redisCommand *cmd, robj **argv, int argc, int *numkeys) {
    int i, num, *keys;
    UNUSED(cmd);

    // 计算key的个数
    num = atoi(argv[2]->ptr);
    /* Sanity check. Don't return any key if the command is going to
     * reply with syntax error. */
    // 语法检查
    if (num > (argc-3)) {
        *numkeys = 0;
        return NULL;
    }

    /* Keys in z{union,inter}store come from two places:
     * argv[1] = storage key,
     * argv[3...n] = keys to intersect */
    // 分配空间
    keys = zmalloc(sizeof(int)*(num+1));

    /* Add all key positions for argv[3...n] to keys[] */
    // key的参数的下标，保存在*keys中
    for (i = 0; i < num; i++) keys[i] = 3+i;

    /* Finally add the argv[1] key position (the storage key target). */
    keys[num] = 1;  //设置destkey的下标
    //保存参数个数
    *numkeys = num+1;  /* Total keys = {union,inter} keys + storage key */
    return keys;
}

/* Helper function to extract keys from the following commands:
 * EVAL <script> <num-keys> <key> <key> ... <key> [more stuff]
 * EVALSHA <script> <num-keys> <key> <key> ... <key> [more stuff] */
// 从EVAL和EVALSHA命令中获取key的下标
int *evalGetKeys(struct redisCommand *cmd, robj **argv, int argc, int *numkeys) {
    int i, num, *keys;
    UNUSED(cmd);
    // 计算key的个数
    num = atoi(argv[2]->ptr);
    /* Sanity check. Don't return any key if the command is going to
     * reply with syntax error. */
    // 语法检查
    if (num > (argc-3)) {
        *numkeys = 0;
        return NULL;
    }

    // 分配空间
    keys = zmalloc(sizeof(int)*num);
    *numkeys = num; //设置参数个数

    /* Add all key positions for argv[3...n] to keys[] */
    // key的参数的下标，保存在*keys中
    for (i = 0; i < num; i++) keys[i] = 3+i;

    return keys;
}

/* Helper function to extract keys from the SORT command.
 *
 * SORT <sort-key> ... STORE <store-key> ...
 *
 * The first argument of SORT is always a key, however a list of options
 * follow in SQL-alike style. Here we parse just the minimum in order to
 * correctly identify keys in the "STORE" option. */
// SORT key [BY pattern] [LIMIT offset count] [GET pattern [GET pattern ...]] [ASC | DESC] [ALPHA] [STORE destination]
// 从SORT命令中获取key的下标
int *sortGetKeys(struct redisCommand *cmd, robj **argv, int argc, int *numkeys) {
    int i, j, num, *keys, found_store = 0;
    UNUSED(cmd);    //不使用该指针，将其设置为void类型

    num = 0;
    // 最多两个位置
    keys = zmalloc(sizeof(int)*2); /* Alloc 2 places for the worst case. */

    // <sort-key>的下标为1
    keys[num++] = 1; /* <sort-key> is always present. */

    /* Search for STORE option. By default we consider options to don't
     * have arguments, so if we find an unknown option name we scan the
     * next. However there are options with 1 or 2 arguments, so we
     * provide a list here in order to skip the right number of args. */
    // 默认的的SORT命令是没有参数的，如果不是下面列表所有的参数，则
    struct {
        char *name;
        int skip;
    } skiplist[] = {
        {"limit", 2},
        {"get", 1},
        {"by", 1},
        {NULL, 0} /* End of elements. */
    };

    // 从第三个参数开始遍历
    for (i = 2; i < argc; i++) {
        // 遍历skiplist[]
        for (j = 0; skiplist[j].name != NULL; j++) {
            // 如果当前选项等于skiplist[]的name
            if (!strcasecmp(argv[i]->ptr,skiplist[j].name)) {
                i += skiplist[j].skip;  //记录跳过的下标
                break;
            // 如果是store选项
            } else if (!strcasecmp(argv[i]->ptr,"store") && i+1 < argc) {
                /* Note: we don't increment "num" here and continue the loop
                 * to be sure to process the *last* "STORE" option if multiple
                 * ones are provided. This is same behavior as SORT. */
                found_store = 1;    //设置发现store选项的标志
                // 将第二个key的下标保存在keys的第二个位置上
                keys[num] = i+1; /* <store-key> */
                break;
            }
        }
    }
    *numkeys = num + found_store;   //保存key的个数
    return keys;
}

// MIGRATE host port key|"" destination-db timeout [COPY] [REPLACE] [KEYS key [key ...]]
// MIGRATE 192.168.1.34 6379 "" 0 5000 KEYS key1 key2 key3
// 将 key 原子性地从当前实例传送到目标实例的指定数据库上，一旦传送成功， key保证会出现在目标实例上，而当前实例上的 key 会被删除。
// MIGRATE命令中获取key的下标
int *migrateGetKeys(struct redisCommand *cmd, robj **argv, int argc, int *numkeys) {
    int i, num, first, *keys;
    UNUSED(cmd);    //不使用该指针，将其设置为void类型

    /* Assume the obvious form. */
    first = 3;  //第一个key的下标
    num = 1;    //key的计数器

    /* But check for the extended one with the KEYS option. */
    // MIGRATE是否有扩展的选项，如果有argc > 6
    if (argc > 6) {
        //遍历扩展的选项
        for (i = 6; i < argc; i++) {
            // 如果出现KEYS选项，且第三个参数长度为0，那么说明MIGRATE是批量迁移模式
            // 使用KEYS选项，并将普通键参数设置为空字符串。实际的键名将在KEYS参数本身之后提供
            if (!strcasecmp(argv[i]->ptr,"keys") &&
                sdslen(argv[3]->ptr) == 0)
            {
                first = i+1;        //第一个键的下标为KEYS选项之后的键的下标
                num = argc-first;   //key的个数
                break;
            }
        }
    }

    //分配空间
    keys = zmalloc(sizeof(int)*num);
    //设置下标
    for (i = 0; i < num; i++) keys[i] = first+i;
    *numkeys = num; //保存key的
    return keys;
}

/* Slot to Key API. This is used by Redis Cluster in order to obtain in
 * a fast way a key that belongs to a specified hash slot. This is useful
 * while rehashing the cluster. */
// 将key添加到槽里
void slotToKeyAdd(robj *key) {
    // 计算所属的槽
    unsigned int hashslot = keyHashSlot(key->ptr,sdslen(key->ptr));

    // 将槽slot作为分值，key作为成员，添加到跳跃表中
    zslInsert(server.cluster->slots_to_keys,hashslot,key);
    incrRefCount(key);
}

// 从槽中删除指定的key
void slotToKeyDel(robj *key) {
    // 计算所属的槽
    unsigned int hashslot = keyHashSlot(key->ptr,sdslen(key->ptr));

    // 从跳跃表中删除
    zslDelete(server.cluster->slots_to_keys,hashslot,key);
}

// 清空节点所有槽保存的所有键
void slotToKeyFlush(void) {
    zslFree(server.cluster->slots_to_keys); //释放原来的跳跃表
    server.cluster->slots_to_keys = zslCreate();    //创建一个新的
}

// 记录count个属于hashslot槽的键到keys数组中
unsigned int getKeysInSlot(unsigned int hashslot, robj **keys, unsigned int count) {
    zskiplistNode *n;
    zrangespec range;
    int j = 0;

    range.min = range.max = hashslot;
    range.minex = range.maxex = 0;

    // 定位到第一个符合range的跳跃表节点地址
    n = zslFirstInRange(server.cluster->slots_to_keys, &range);
    // 遍历跳跃表
    while(n && n->score == hashslot && count--) {
        keys[j++] = n->obj; //保存 符合range范围的节点所保存 的键对象
        n = n->level[0].forward;    //指向下一个节点
    }
    return j;
}

/* Remove all the keys in the specified hash slot.
 * The number of removed items is returned. */
// 删除所有槽位上的键值对
unsigned int delKeysInSlot(unsigned int hashslot) {
    zskiplistNode *n;
    zrangespec range;
    int j = 0;

    range.min = range.max = hashslot;
    range.minex = range.maxex = 0;

    // 定位到第一个符合range的跳跃表节点地址
    n = zslFirstInRange(server.cluster->slots_to_keys, &range);
    // 遍历跳跃表
    while(n && n->score == hashslot) {
        robj *key = n->obj; //当前节点的键对象
        n = n->level[0].forward; /* 指向下一个节点 Go to the next item before freeing it. */
        incrRefCount(key); /*保护值对象 Protect the object while freeing it. */
        dbDelete(&server.db[0],key);    //从数据库删除当前键
        decrRefCount(key);              //释放key
        j++;
    }
    return j;
}

// 返回指定slot中包含的键数量
unsigned int countKeysInSlot(unsigned int hashslot) {
    zskiplist *zsl = server.cluster->slots_to_keys;
    zskiplistNode *zn;
    zrangespec range;
    int rank, count = 0;

    range.min = range.max = hashslot;
    range.minex = range.maxex = 0;

    /* Find first element in range */
    // 定位到第一个符合range的跳跃表节点地址
    zn = zslFirstInRange(zsl, &range);

    /* Use rank of first element, if any, to determine preliminary count */
    // 若有符合range的节点
    if (zn != NULL) {
        // 计算第一个节点的排位值
        rank = zslGetRank(zsl, zn->score, zn->obj);
        count = (zsl->length - (rank - 1)); //计算符合范围第一个节点到跳跃表最后一个节点的节点数

        /* Find last element in range */
        // 定位到最后一个符合range的跳跃表节点地址
        zn = zslLastInRange(zsl, &range);

        /* Use rank of last element, if any, to determine the actual count */
        if (zn != NULL) {
            // 计算最后一个节点的排位值
            rank = zslGetRank(zsl, zn->score, zn->obj);
            // 计算符合范围最后一个节点到跳跃表最后一个节点的节点数，然后用刚才的count减去该值就是区间内的节点数
            count -= (zsl->length - rank);
        }
    }
    return count;
}
