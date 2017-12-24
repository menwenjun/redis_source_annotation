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

/* ================================ MULTI/EXEC ============================== */

/* Client state initialization for MULTI/EXEC */
// 初始化client的事务状态
void initClientMultiState(client *c) {
    // 事务命令数组为空
    c->mstate.commands = NULL;
    // 事务命令数组长度为0
    c->mstate.count = 0;
}

/* Release all the resources associated with MULTI/EXEC state */
// 释放客户端的事务状态的所有资源
void freeClientMultiState(client *c) {
    int j;
    // 遍历事务命令数组
    for (j = 0; j < c->mstate.count; j++) {
        int i;
        multiCmd *mc = c->mstate.commands+j;
        // 释放命令的参数列表保存的对象
        for (i = 0; i < mc->argc; i++)
            decrRefCount(mc->argv[i]);
        // 释放命令的参数列表
        zfree(mc->argv);
    }
    // 释放事务命令的状态
    zfree(c->mstate.commands);
}

/* Add a new command into the MULTI commands queue */
// 添加一个新命令到事务命令数组中
void queueMultiCommand(client *c) {
    multiCmd *mc;
    int j;
    // 增加队列的空间
    c->mstate.commands = zrealloc(c->mstate.commands,
            sizeof(multiCmd)*(c->mstate.count+1));
    // 获取新命令的存放地址
    mc = c->mstate.commands+c->mstate.count;
    // 设置新命令的参数列表、参数数量和命令函数
    mc->cmd = c->cmd;
    mc->argc = c->argc;
    mc->argv = zmalloc(sizeof(robj*)*c->argc);
    memcpy(mc->argv,c->argv,sizeof(robj*)*c->argc);
    // 增加引用计数
    for (j = 0; j < c->argc; j++)
        incrRefCount(mc->argv[j]);
    // 事务命令个数加1
    c->mstate.count++;
}

// 取消事务状态
void discardTransaction(client *c) {
    // 释放客户端的事务状态的所有资源
    freeClientMultiState(c);
    // 初始化client的事务状态
    initClientMultiState(c);
    // 取消客户端的事务有关的状态标识
    c->flags &= ~(CLIENT_MULTI|CLIENT_DIRTY_CAS|CLIENT_DIRTY_EXEC);
    // 取消对客户端的所有的键的监视
    unwatchAllKeys(c);
}

/* Flag the transacation as DIRTY_EXEC so that EXEC will fail.
 * Should be called every time there is an error while queueing a command. */
// 设置事务的状态为DIRTY_EXEC，以便执行执行EXEC失败
// 每次在入队命令出错时调用
void flagTransaction(client *c) {
    if (c->flags & CLIENT_MULTI)
        // 如果客户端处于事务状态，设置DIRTY_EXEC标识
        c->flags |= CLIENT_DIRTY_EXEC;
}

// MULTI命令的实现，标记一个事务的开始
void multiCommand(client *c) {
    // 客户端已经处于事务状态，回复错误后返回
    if (c->flags & CLIENT_MULTI) {
        addReplyError(c,"MULTI calls can not be nested");
        return;
    }
    // 打开客户的的事务状态标识
    c->flags |= CLIENT_MULTI;
    // 回复OK
    addReply(c,shared.ok);
}

// DISCARD取消事务的命令实现
void discardCommand(client *c) {
    // 客户端当前不处于事务状态，回复错误后返回
    if (!(c->flags & CLIENT_MULTI)) {
        addReplyError(c,"DISCARD without MULTI");
        return;
    }
    // 取消事务
    discardTransaction(c);
    // 回复OK
    addReply(c,shared.ok);
}

/* Send a MULTI command to all the slaves and AOF file. Check the execCommand
 * implementation for more information. */
// 发送一个MULTI命令给所有的从节点和AOF文件
void execCommandPropagateMulti(client *c) {
    // 创建一个MULTI命令
    robj *multistring = createStringObject("MULTI",5);
    // 传播MULTI命令到AOF文件中和从节点服务器中
    propagate(server.multiCommand,c->db->id,&multistring,1,
              PROPAGATE_AOF|PROPAGATE_REPL);
    // 释放临时对象
    decrRefCount(multistring);
}

// EXEC 命令实现
void execCommand(client *c) {
    int j;
    robj **orig_argv;
    int orig_argc;
    struct redisCommand *orig_cmd;
    // 传播的标识
    int must_propagate = 0; /* Need to propagate MULTI/EXEC to AOF / slaves? */
    // 如果客户端当前不处于事务状态，回复错误后返回
    if (!(c->flags & CLIENT_MULTI)) {
        addReplyError(c,"EXEC without MULTI");
        return;
    }

    /* Check if we need to abort the EXEC because:
     * 1) Some WATCHed key was touched.
     * 2) There was a previous error while queueing commands.
     * A failed EXEC in the first case returns a multi bulk nil object
     * (technically it is not an error but a special behavior), while
     * in the second an EXECABORT error is returned. */
    // 检查是否需要中断EXEC的执行因为：
    /*
        1. 被监控的key被修改
        2. 入队命令时发生了错误
    */
    // 第一种情况返回空回复对象，第二种情况返回一个EXECABORT错误
    // 如果客户的处于 1.命令入队时错误或者2.被监控的key被修改
    if (c->flags & (CLIENT_DIRTY_CAS|CLIENT_DIRTY_EXEC)) {
        // 回复错误信息
        addReply(c, c->flags & CLIENT_DIRTY_EXEC ? shared.execaborterr :
                                                  shared.nullmultibulk);
        // 取消事务
        discardTransaction(c);
        // 跳转到处理监控器代码
        goto handle_monitor;
    }

    /* Exec all the queued commands */
    // 执行队列数组中的命令
    // 因为所有的命令都是安全的，因此取消对客户端的所有的键的监视
    unwatchAllKeys(c); /* Unwatch ASAP otherwise we'll waste CPU cycles */
    // 备份EXEC命令
    orig_argv = c->argv;
    orig_argc = c->argc;
    orig_cmd = c->cmd;
    // 回复一个事务命令的个数
    addReplyMultiBulkLen(c,c->mstate.count);
    // 遍历执行所有事务命令
    for (j = 0; j < c->mstate.count; j++) {
        // 设置一个当前事务命令给客户端
        c->argc = c->mstate.commands[j].argc;
        c->argv = c->mstate.commands[j].argv;
        c->cmd = c->mstate.commands[j].cmd;

        /* Propagate a MULTI request once we encounter the first write op.
         * This way we'll deliver the MULTI/..../EXEC block as a whole and
         * both the AOF and the replication link will have the same consistency
         * and atomicity guarantees. */
        // 当执行到第一个写命令时，传播事务状态
        if (!must_propagate && !(c->cmd->flags & CMD_READONLY)) {
            // 发送一个MULTI命令给所有的从节点和AOF文件
            execCommandPropagateMulti(c);
            // 设置已经传播过的标识
            must_propagate = 1;
        }
        // 执行该命令
        call(c,CMD_CALL_FULL);

        /* Commands may alter argc/argv, restore mstate. */
        // 命令可能会被修改，重新存储在事务命令队列中
        c->mstate.commands[j].argc = c->argc;
        c->mstate.commands[j].argv = c->argv;
        c->mstate.commands[j].cmd = c->cmd;
    }
    // 还原命令和参数
    c->argv = orig_argv;
    c->argc = orig_argc;
    c->cmd = orig_cmd;
    // 取消事务状态
    discardTransaction(c);
    /* Make sure the EXEC command will be propagated as well if MULTI
     * was already propagated. */
    // 如果传播了EXEC命令，表示执行了写命令，更新数据库脏键数
    if (must_propagate) server.dirty++;

handle_monitor:
    /* Send EXEC to clients waiting data from MONITOR. We do it here
     * since the natural order of commands execution is actually:
     * MUTLI, EXEC, ... commands inside transaction ...
     * Instead EXEC is flagged as CMD_SKIP_MONITOR in the command
     * table, and we do it here with correct ordering. */
    // 如果服务器设置了监控器，并且服务器不处于载入文件的状态
    if (listLength(server.monitors) && !server.loading)
        // 将参数列表中的参数发送给监控器
        replicationFeedMonitors(c,server.monitors,c->db->id,c->argv,c->argc);
}

/* ===================== WATCH (CAS alike for MULTI/EXEC) ===================
 *
 * The implementation uses a per-DB hash table mapping keys to list of clients
 * WATCHing those keys, so that given a key that is going to be modified
 * we can mark all the associated clients as dirty.
 *
 * Also every client contains a list of WATCHed keys so that's possible to
 * un-watch such keys when the client is freed or when UNWATCH is called. */

/* In the client->watched_keys list we need to use watchedKey structures
 * as in order to identify a key in Redis we need both the key name and the
 * DB */
// 为每个数据库都设置key和监视这个key的client链表的映射，这样以便当key被修改后，可以标记关联的client为dirty状态
// 每一个client包含一个被监视的key的链表，以便对所有被监视的key进行UNWATCH操作
// 在 client->watched_keys 链表中我们需要使用watchedKey结构，因为要保存被监视的key和key所在的数据库
typedef struct watchedKey {
    // 被监视的key
    robj *key;
    // 被监视的key所在的数据库
    redisDb *db;
} watchedKey;

/* Watch for the specified key */
// 让client监视所有的指定的key
void watchForKey(client *c, robj *key) {
    list *clients = NULL;
    listIter li;
    listNode *ln;
    watchedKey *wk;

    /* Check if we are already watching for this key */
    listRewind(c->watched_keys,&li);
    // 遍历客户端监视的键的链表，检查是否已经监视了指定的键
    while((ln = listNext(&li))) {
        wk = listNodeValue(ln);
        // 如果键已经被监视，则直接返回
        if (wk->db == c->db && equalStringObjects(key,wk->key))
            return; /* Key already watched */
    }
    /* This key is not already watched in this DB. Let's add it */
    // 如果数据库中该键没有被client监视则添加它
    clients = dictFetchValue(c->db->watched_keys,key);
    // 没有被client监视
    if (!clients) {
        // 创建一个空链表
        clients = listCreate();
        // 值是被client监控的key，键是client，添加到数据库的watched_keys字典中
        dictAdd(c->db->watched_keys,key,clients);
        incrRefCount(key);
    }
    // 将当前client添加到监视该key的client链表的尾部
    listAddNodeTail(clients,c);
    /* Add the new key to the list of keys watched by this client */
    // 将新的被监视的key和与该key关联的数据库加入到客户端的watched_keys中
    wk = zmalloc(sizeof(*wk));
    wk->key = key;
    wk->db = c->db;
    incrRefCount(key);
    listAddNodeTail(c->watched_keys,wk);
}

/* Unwatch all the keys watched by this client. To clean the EXEC dirty
 * flag is up to the caller. */
// 取消客户端对所有的键的监视，清理 EXEC dirty 标识状态由调用者决定
void unwatchAllKeys(client *c) {
    listIter li;
    listNode *ln;
    // 如果客户端没有监视key则直接返回
    if (listLength(c->watched_keys) == 0) return;
    listRewind(c->watched_keys,&li);
    // 遍历客户端监视的key
    while((ln = listNext(&li))) {
        list *clients;
        watchedKey *wk;

        /* Lookup the watched key -> clients list and remove the client
         * from the list */
        wk = listNodeValue(ln);
        // 从数据库中的watched_keys字典中查找出监视key的client
        clients = dictFetchValue(wk->db->watched_keys, wk->key);
        serverAssertWithInfo(c,NULL,clients != NULL);
        // 从client的链表中删除当前client节点
        listDelNode(clients,listSearchKey(clients,c));
        /* Kill the entry at all if this was the only client */
        // 如果client链表为空，标识给key没有被监视
        if (listLength(clients) == 0)
            // 从数据库的watched_keys中删除该key
            dictDelete(wk->db->watched_keys, wk->key);
        /* Remove this watched key from the client->watched list */
        // 从客户端的watched_keys中删除该节点
        listDelNode(c->watched_keys,ln);
        decrRefCount(wk->key);
        zfree(wk);
    }
}

/* "Touch" a key, so that if this key is being WATCHed by some client the
 * next EXEC will fail. */
// Touch 一个 key，如果该key正在被监视，那么客户端会执行EXEC失败
void touchWatchedKey(redisDb *db, robj *key) {
    list *clients;
    listIter li;
    listNode *ln;
    // 如果数据库中没有被监视的key，直接返回
    if (dictSize(db->watched_keys) == 0) return;
    // 找出监视该key的client链表
    clients = dictFetchValue(db->watched_keys, key);
    // 没找到返回
    if (!clients) return;

    /* Mark all the clients watching this key as CLIENT_DIRTY_CAS */
    /* Check if we are already watching for this key */
    listRewind(clients,&li);
    // 遍历所有监视该key的client
    while((ln = listNext(&li))) {
        client *c = listNodeValue(ln);
        // 设置CLIENT_DIRTY_CAS标识
        c->flags |= CLIENT_DIRTY_CAS;
    }
}

/* On FLUSHDB or FLUSHALL all the watched keys that are present before the
 * flush but will be deleted as effect of the flushing operation should
 * be touched. "dbid" is the DB that's getting the flush. -1 if it is
 * a FLUSHALL operation (all the DBs flushed). */
// 当数据库执行了FLUSHDB或FLUSHALL命令，所有的key都被清空，那么数据库中的所有的键都被touched
// dbid指定被FLUSH的数据库id，如果dbid为-1，表示执行了FLUSHALL，所有的数据库都被FLUSH
void touchWatchedKeysOnFlush(int dbid) {
    listIter li1, li2;
    listNode *ln;

    /* For every client, check all the waited keys */
    listRewind(server.clients,&li1);
    // 遍历所有的client
    while((ln = listNext(&li1))) {
        client *c = listNodeValue(ln);
        listRewind(c->watched_keys,&li2);
        // 遍历当前client所监视的key
        while((ln = listNext(&li2))) {
            // 取出被监视的key和关联的数据库
            watchedKey *wk = listNodeValue(ln);

            /* For every watched key matching the specified DB, if the
             * key exists, mark the client as dirty, as the key will be
             * removed. */
            // 如果数据库id合法
            if (dbid == -1 || wk->db->id == dbid) {
                // 数据库中存在该key，则设置client的DIRTY_CAS标识
                if (dictFind(wk->db->dict, wk->key->ptr) != NULL)
                    c->flags |= CLIENT_DIRTY_CAS;
            }
        }
    }
}

// WATCH命令实现
void watchCommand(client *c) {
    int j;
    // 如果已经处于事务状态，则回复错误后返回，必须在执行MULTI命令执行前执行WATCH
    if (c->flags & CLIENT_MULTI) {
        addReplyError(c,"WATCH inside MULTI is not allowed");
        return;
    }
    // 遍历所有的参数
    for (j = 1; j < c->argc; j++)
        // 监控当前key
        watchForKey(c,c->argv[j]);
    // 回复OK
    addReply(c,shared.ok);
}

// 取消client所监视的key
void unwatchCommand(client *c) {
    // 取消客户端对所有的键的监视
    unwatchAllKeys(c);
    // 清理 EXEC dirty 标识状态
    c->flags &= (~CLIENT_DIRTY_CAS);
    // 回复ok
    addReply(c,shared.ok);
}
