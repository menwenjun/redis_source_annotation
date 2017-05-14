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
#include <sys/uio.h>
#include <math.h>

static void setProtocolError(client *c, int pos);

/* Return the size consumed from the allocator, for the specified SDS string,
 * including internal fragmentation. This function is used in order to compute
 * the client output buffer size. */
// 计算sds保存字符串数据的空间大小
size_t sdsZmallocSize(sds s) {
    // 返回buf[]柔性数组的地址
    void *sh = sdsAllocPtr(s);
    return zmalloc_size(sh);
}

/* Return the amount of memory used by the sds string at object->ptr
 * for a string object. */
// 返回字符串对象所使用的内存数
size_t getStringObjectSdsUsedMemory(robj *o) {
    serverAssertWithInfo(NULL,o,o->type == OBJ_STRING);
    switch(o->encoding) {
    // 返回buf的内存大小
    case OBJ_ENCODING_RAW: return sdsZmallocSize(o->ptr);
    case OBJ_ENCODING_EMBSTR: return zmalloc_size(o)-sizeof(robj);
    default: return 0; /* Just integer encoding for now. */
    }
}

// 复制client的回复内容，增加引用计数
void *dupClientReplyValue(void *o) {
    incrRefCount((robj*)o);
    return o;
}

// 订阅模式下，比较两个字符串对象，以二进制安全的方式进行比较
int listMatchObjects(void *a, void *b) {
    return equalStringObjects(a,b);
}

// 创建一个新的client
client *createClient(int fd) {
    client *c = zmalloc(sizeof(client));    //分配空间

    /* passing -1 as fd it is possible to create a non connected client.
     * This is useful since all the commands needs to be executed
     * in the context of a client. When commands are executed in other
     * contexts (for instance a Lua script) we need a non connected client. */
    // 如果fd为-1，表示创建的是一个无网络连接的伪客户端，用于执行lua脚本的时候。
    // 如果fd不等于-1，表示创建一个有网络连接的客户端
    if (fd != -1) {
        // 设置fd为非阻塞模式
        anetNonBlock(NULL,fd);
        // 禁止使用 Nagle 算法，client向内核递交的每个数据包都会立即发送给server出去，TCP_NODELAY
        anetEnableTcpNoDelay(NULL,fd);
        // 如果开启了tcpkeepalive，则设置 SO_KEEPALIVE
        if (server.tcpkeepalive)
            // 设置tcp连接的keep alive选项
            anetKeepAlive(NULL,fd,server.tcpkeepalive);
        // 创建一个文件事件状态el，且监听读事件，开始接受命令的输入
        if (aeCreateFileEvent(server.el,fd,AE_READABLE,
            readQueryFromClient, c) == AE_ERR)
        {
            close(fd);
            zfree(c);
            return NULL;
        }
    }

    // 默认选0号数据库
    selectDb(c,0);
    // 设置client的ID
    c->id = server.next_client_id++;
    // client的套接字
    c->fd = fd;
    // client的名字
    c->name = NULL;
    // 回复固定(静态)缓冲区的偏移量
    c->bufpos = 0;
    // 输入缓存区
    c->querybuf = sdsempty();
    // 输入缓存区的峰值
    c->querybuf_peak = 0;
    // 请求协议类型，内联或者多条命令，初始化为0
    c->reqtype = 0;
    // 参数个数
    c->argc = 0;
    // 参数列表
    c->argv = NULL;
    // 当前执行的命令和最近一次执行的命令
    c->cmd = c->lastcmd = NULL;
    // 查询缓冲区剩余未读取命令的数量
    c->multibulklen = 0;
    // 读入参数的长度
    c->bulklen = -1;
    // 已发的字节数
    c->sentlen = 0;
    // client的状态
    c->flags = 0;
    // 设置创建client的时间和最后一次互动的时间
    c->ctime = c->lastinteraction = server.unixtime;
    // 认证状态
    c->authenticated = 0;
    // replication复制的状态，初始为无
    c->replstate = REPL_STATE_NONE;
    // 设置从节点的写处理器为ack，是否在slave向master发送ack
    c->repl_put_online_on_ack = 0;
    // replication复制的偏移量
    c->reploff = 0;
    // 通过ack命令接收到的偏移量
    c->repl_ack_off = 0;
    // 通过ack命令接收到的偏移量所用的时间
    c->repl_ack_time = 0;
    // 从节点的端口号
    c->slave_listening_port = 0;
    // 从节点IP地址
    c->slave_ip[0] = '\0';
    // 从节点的功能
    c->slave_capa = SLAVE_CAPA_NONE;
    // 回复链表
    c->reply = listCreate();
    // 回复链表的字节数
    c->reply_bytes = 0;
    // 回复缓冲区的内存大小软限制
    c->obuf_soft_limit_reached_time = 0;
    // 回复链表的释放和复制方法
    listSetFreeMethod(c->reply,decrRefCountVoid);
    listSetDupMethod(c->reply,dupClientReplyValue);
    // 阻塞类型
    c->btype = BLOCKED_NONE;
    // 阻塞超过时间
    c->bpop.timeout = 0;
    // 造成阻塞的键字典
    c->bpop.keys = dictCreate(&setDictType,NULL);
    // 存储解除阻塞的键，用于保存PUSH入元素的键，也就是dstkey
    c->bpop.target = NULL;
    // 阻塞状态
    c->bpop.numreplicas = 0;
    // 要达到的复制偏移量
    c->bpop.reploffset = 0;
    // 全局的复制偏移量
    c->woff = 0;
    // 监控的键
    c->watched_keys = listCreate();
    // 订阅频道
    c->pubsub_channels = dictCreate(&setDictType,NULL);
    // 订阅模式
    c->pubsub_patterns = listCreate();
    // 被缓存的peerid，peerid就是 ip:port
    c->peerid = NULL;
    // 订阅发布模式的释放和比较方法
    listSetFreeMethod(c->pubsub_patterns,decrRefCountVoid);
    listSetMatchMethod(c->pubsub_patterns,listMatchObjects);
    // 将真正的client放在服务器的客户端链表中
    if (fd != -1) listAddNodeTail(server.clients,c);
    // 初始化client的事物状态
    initClientMultiState(c);
    return c;
}

/* This function is called every time we are going to transmit new data
 * to the client. The behavior is the following:
 *
 * If the client should receive new data (normal clients will) the function
 * returns C_OK, and make sure to install the write handler in our event
 * loop so that when the socket is writable new data gets written.
 *
 * If the client should not receive new data, because it is a fake client
 * (used to load AOF in memory), a master or because the setup of the write
 * handler failed, the function returns C_ERR.
 *
 * The function may return C_OK without actually installing the write
 * event handler in the following cases:
 *
 * 1) The event handler should already be installed since the output buffer
 *    already contained something.
 * 2) The client is a slave but not yet online, so we want to just accumulate
 *    writes in the buffer but not actually sending them yet.
 *
 * Typically gets called every time a reply is built, before adding more
 * data to the clients output buffers. If the function returns C_ERR no
 * data should be appended to the output buffers. */
/*
这个函数每次向客户端发送新数据时会调用，有如下行为：
如果client应该接收到了新数据，函数返回 C_OK，并且将写处理程序设置到数据循环中以便socket可写时将新数据写入
如果client不应该接收到新数据，可能是因为：载入AOF的伪client，主节点或者写处理程序设置失败，函数返回C_ERR
函数可能返回C_OK，在没有安装写处理程序的情况如下：
1.事件处理程序已经已经被安装，因为之前输出缓冲区已经有数据
2.client是一个从节点，但是还没有在线，所以只是想在缓冲区累计写操作，而不是想发送他们
通常每次在回复被创建时调用，在添加数据到client的缓冲区之前。如果函数返回C_ERR，没有数据被追加到输出缓冲区
*/
// 准备一个可写的client
int prepareClientToWrite(client *c) {
    /* If it's the Lua client we always return ok without installing any
     * handler since there is no socket at all. */
    // 如果是要执行lua脚本的伪client，则总是返回C_OK，总是可写的
    if (c->flags & CLIENT_LUA) return C_OK;

    /* CLIENT REPLY OFF / SKIP handling: don't send replies. */
    // 如果client没有开启这条命令的回复功能，则返回C_ERR
    // CLIENT_REPLY_OFF设置为不开启，服务器不会回复client命令
    // CLIENT_REPLY_SKIP设置为跳过该条回复，服务器会跳过这条命令的回复
    if (c->flags & (CLIENT_REPLY_OFF|CLIENT_REPLY_SKIP)) return C_ERR;

    /* Masters don't receive replies, unless CLIENT_MASTER_FORCE_REPLY flag
     * is set. */
    // 如果主节点服务器且没有设置强制回复，返回C_ERR
    if ((c->flags & CLIENT_MASTER) &&
        !(c->flags & CLIENT_MASTER_FORCE_REPLY)) return C_ERR;

    // 如果是载入AOF的伪client，则返回C_ERR
    if (c->fd <= 0) return C_ERR; /* Fake client for AOF loading. */

    /* Schedule the client to write the output buffers to the socket only
     * if not already done (there were no pending writes already and the client
     * was yet not flagged), and, for slaves, if the slave can actually
     * receive writes at this stage. */
    // 如果client的回复缓冲区为空，且client还有输出的数据，但是没有设置写处理程序，且
    // replication的状态为关闭状态，或已经将RDB传输完成且不向主节点发送ack
    if (!clientHasPendingReplies(c) &&
        !(c->flags & CLIENT_PENDING_WRITE) &&
        (c->replstate == REPL_STATE_NONE ||
         (c->replstate == SLAVE_STATE_ONLINE && !c->repl_put_online_on_ack)))
    {
        /* Here instead of installing the write handler, we just flag the
         * client and put it into a list of clients that have something
         * to write to the socket. This way before re-entering the event
         * loop, we can try to directly write to the client sockets avoiding
         * a system call. We'll only really install the write handler if
         * we'll not be able to write the whole reply at once. */
        // 将client设置为还有输出的数据，但是没有设置写处理程序
        c->flags |= CLIENT_PENDING_WRITE;
        // 将当前client加入到要写或者安装写处理程序的client链表
        listAddNodeHead(server.clients_pending_write,c);
    }

    /* Authorize the caller to queue in the output buffer of this client. */
    // 授权调用者在这个client的输出缓冲区排队
    return C_OK;
}

/* Create a duplicate of the last object in the reply list when
 * it is not exclusively owned by the reply list. */
// 当回复链表的最后一个对象被其他程序引用，则创建一份复制品
robj *dupLastObjectIfNeeded(list *reply) {
    robj *new, *cur;
    listNode *ln;
    serverAssert(listLength(reply) > 0);
    // 尾节点地址
    ln = listLast(reply);
    // 节点保存的对象
    cur = listNodeValue(ln);
    // 创建一份非共享的对象，替代原有的对象
    if (cur->refcount > 1) {
        new = dupStringObject(cur);
        decrRefCount(cur);
        listNodeValue(ln) = new;
    }
    return listNodeValue(ln);
}

/* -----------------------------------------------------------------------------
 * Low level functions to add more data to output buffers.
 * -------------------------------------------------------------------------- */
// 将字符串s添加到固定回复缓冲区c->buf中
int _addReplyToBuffer(client *c, const char *s, size_t len) {
    // 固定回复缓冲区可用的大小
    size_t available = sizeof(c->buf)-c->bufpos;

    // 如果client即将关闭，则直接成功返回
    if (c->flags & CLIENT_CLOSE_AFTER_REPLY) return C_OK;

    /* If there already are entries in the reply list, we cannot
     * add anything more to the static buffer. */
    // 如果回复链表中有内容，就不能继续添加到固定回复缓冲区中
    if (listLength(c->reply) > 0) return C_ERR;

    /* Check that the buffer has enough space available for this string. */
    // 检查空间大小是否满足
    if (len > available) return C_ERR;

    // 将s拷贝到client的buf中，并更新buf的偏移量
    memcpy(c->buf+c->bufpos,s,len);
    c->bufpos+=len;
    return C_OK;
}

// 添加大小到回复链表中
void _addReplyObjectToList(client *c, robj *o) {
    robj *tail;

    // 如果client即将关闭，则直接成功返回
    if (c->flags & CLIENT_CLOSE_AFTER_REPLY) return;

    // 如果链表为空，则将对象追加到链表末尾
    if (listLength(c->reply) == 0) {
        incrRefCount(o);
        listAddNodeTail(c->reply,o);
        // 返回字符串对象所使用的内存数，更新回复链表所占的字节数大小
        c->reply_bytes += getStringObjectSdsUsedMemory(o);
    // 链表不为空
    } else {
        // 获取尾节点的值
        tail = listNodeValue(listLast(c->reply));

        /* Append to this object when possible. */
        // 如果是sds，且最后一个节点的sds大小加上添加对象的sds大小小于16k
        if (tail->ptr != NULL &&
            tail->encoding == OBJ_ENCODING_RAW &&
            sdslen(tail->ptr)+sdslen(o->ptr) <= PROTO_REPLY_CHUNK_BYTES)
        {
            // 从链表字节数中，减去尾节点sds保存字符串数据的空间大小
            c->reply_bytes -= sdsZmallocSize(tail->ptr);
            // 然后创建一个份尾节点的非共享的复制品替代链表原有的
            tail = dupLastObjectIfNeeded(c->reply);
            // 将两者拼接到一起
            tail->ptr = sdscatlen(tail->ptr,o->ptr,sdslen(o->ptr));
            // 重新计算链表字节数
            c->reply_bytes += sdsZmallocSize(tail->ptr);

        // 空间大小超过16k
        } else {
            incrRefCount(o);
            // 添加到链表尾部
            listAddNodeTail(c->reply,o);
            // 重新计算链表字节数
            c->reply_bytes += getStringObjectSdsUsedMemory(o);
        }
    }
    // 检查回复缓冲区的大小是否超过系统限制，如果超过则关闭client
    asyncCloseClientOnOutputBufferLimitReached(c);
}

/* This method takes responsibility over the sds. When it is no longer
 * needed it will be free'd, otherwise it ends up in a robj. */
// 添加一个sds到回复链表中，会负责释放sds
void _addReplySdsToList(client *c, sds s) {
    robj *tail;

    // 如果client即将关闭，则释放s，返回
    if (c->flags & CLIENT_CLOSE_AFTER_REPLY) {
        sdsfree(s);
        return;
    }

    // 如果链表为空，则将对象追加到链表末尾
    if (listLength(c->reply) == 0) {
        // 将sds构建成字符串的追加到链表末尾
        listAddNodeTail(c->reply,createObject(OBJ_STRING,s));
        // 重新计算链表字节数
        c->reply_bytes += sdsZmallocSize(s);

    // 链表不为空
    } else {
        // 获取尾节点的值
        tail = listNodeValue(listLast(c->reply));

        /* Append to this object when possible. */
        // 最后一个节点的sds大小加上添加的sds大小小于16k
        if (tail->ptr != NULL && tail->encoding == OBJ_ENCODING_RAW &&
            sdslen(tail->ptr)+sdslen(s) <= PROTO_REPLY_CHUNK_BYTES)
        {
            // 将sds和尾节点的sds拼接起来，更新链表字节数
            c->reply_bytes -= sdsZmallocSize(tail->ptr);
            tail = dupLastObjectIfNeeded(c->reply);
            tail->ptr = sdscatlen(tail->ptr,s,sdslen(s));
            c->reply_bytes += sdsZmallocSize(tail->ptr);
            sdsfree(s);

        // 空间大小超过16k，新创建一个节点，追加到链表尾部
        } else {
            listAddNodeTail(c->reply,createObject(OBJ_STRING,s));
            c->reply_bytes += sdsZmallocSize(s);
        }
    }
    // 检查回复缓冲区的大小是否超过系统限制，如果超过则关闭client
    asyncCloseClientOnOutputBufferLimitReached(c);
}

// 添加一个c字符串到回复链表中
void _addReplyStringToList(client *c, const char *s, size_t len) {
    robj *tail;

    // 如果client即将关闭，则释放s，返回
    if (c->flags & CLIENT_CLOSE_AFTER_REPLY) return;

    // 如果链表为空，则将对象追加到链表末尾
    if (listLength(c->reply) == 0) {
        // 将c字符串构建成字符串的追加到链表末尾
        robj *o = createStringObject(s,len);
        // 追加到链表末尾
        listAddNodeTail(c->reply,o);
        // 更新链表字节数
        c->reply_bytes += getStringObjectSdsUsedMemory(o);
    // 链表不为空
    } else {
        // 获取尾节点的值
        tail = listNodeValue(listLast(c->reply));

        /* Append to this object when possible. */
        // 最后一个节点的sds大小加上添加字符串长度大小小于16k
        if (tail->ptr != NULL && tail->encoding == OBJ_ENCODING_RAW &&
            sdslen(tail->ptr)+len <= PROTO_REPLY_CHUNK_BYTES)
        {
            // 将字符串和尾节点的sds拼接起来，更新链表字节数
            c->reply_bytes -= sdsZmallocSize(tail->ptr);
            tail = dupLastObjectIfNeeded(c->reply);
            tail->ptr = sdscatlen(tail->ptr,s,len);
            c->reply_bytes += sdsZmallocSize(tail->ptr);
        // 空间大小超过16k，新创建一个节点，追加到链表尾部
        } else {
            robj *o = createStringObject(s,len);

            listAddNodeTail(c->reply,o);
            c->reply_bytes += getStringObjectSdsUsedMemory(o);
        }
    }
    // 检查回复缓冲区的大小是否超过系统限制，如果超过则关闭client
    asyncCloseClientOnOutputBufferLimitReached(c);
}

/* -----------------------------------------------------------------------------
 * Higher level functions to queue data on the client output buffer.
 * The following functions are the ones that commands implementations will call.
 * -------------------------------------------------------------------------- */
// 添加obj到client的回复缓冲区中
void addReply(client *c, robj *obj) {
    // 准备client为可写的
    if (prepareClientToWrite(c) != C_OK) return;

    /* This is an important place where we can avoid copy-on-write
     * when there is a saving child running, avoiding touching the
     * refcount field of the object if it's not needed.
     *
     * If the encoding is RAW and there is room in the static buffer
     * we'll be able to send the object to the client without
     * messing with its page. */
    // 如果子进程正在执行save操作，尽量不要避免修改对象的引用计数
    // 如果是原生的字符串编码，则添加到固定的回复缓冲区中，
    if (sdsEncodedObject(obj)) {
        // 如果固定的回复缓冲区空间不足够，则添加到回复链表中，可能引起内存分配
        if (_addReplyToBuffer(c,obj->ptr,sdslen(obj->ptr)) != C_OK)
            _addReplyObjectToList(c,obj);
    // 如果是int编码的对象
    } else if (obj->encoding == OBJ_ENCODING_INT) {
        /* youhua: if there is room in the static buffer for 32 bytes
         * (more than the max chars a 64 bit integer can take as string) we
         * avoid decoding the object and go for the lower level approach. */
        // 最优化：如果固定的缓冲区大小等于多于32字节，则将整数转换成字符串，保存在固定的缓冲区buf中
        if (listLength(c->reply) == 0 && (sizeof(c->buf) - c->bufpos) >= 32) {
            char buf[32];
            int len;
            // 转换为字符串
            len = ll2string(buf,sizeof(buf),(long)obj->ptr);
            // 将字符串添加到client的buf中
            if (_addReplyToBuffer(c,buf,len) == C_OK)
                return;
            /* else... continue with the normal code path, but should never
             * happen actually since we verified there is room. */
        }
        // 当前对象是整数，但是长度大于32位，则解码成字符串对象
        obj = getDecodedObject(obj);
        // 添加字符串对象的值到固定的回复buf中
        if (_addReplyToBuffer(c,obj->ptr,sdslen(obj->ptr)) != C_OK)
            // 如果添加失败，则保存到回复链表中
            _addReplyObjectToList(c,obj);
        decrRefCount(obj);
    } else {
        serverPanic("Wrong obj->encoding in addReply()");
    }
}

// 将sds复制到client的回复缓冲区中
void addReplySds(client *c, sds s) {
    // 准备client为可写的
    if (prepareClientToWrite(c) != C_OK) {
        /* The caller expects the sds to be free'd. */
        sdsfree(s);
        return;
    }
    // 将sds复制到client的回复缓冲区中，成功要释放
    if (_addReplyToBuffer(c,s,sdslen(s)) == C_OK) {
        sdsfree(s);
    } else {
        /* This method free's the sds when it is no longer needed. */
        // 否则追加到回复链表中
        _addReplySdsToList(c,s);
    }
}

// 将c字符串复制到client的回复缓冲区中
void addReplyString(client *c, const char *s, size_t len) {
    if (prepareClientToWrite(c) != C_OK) return;    //准备client为可写的
    if (_addReplyToBuffer(c,s,len) != C_OK)         //将字符串复制到client的回复缓冲区中
        _addReplyStringToList(c,s,len);
}

// 按格式添加一个错误回复
void addReplyErrorLength(client *c, const char *s, size_t len) {
    addReplyString(c,"-ERR ",5);
    addReplyString(c,s,len);
    addReplyString(c,"\r\n",2);
}

// 按格式添加一个错误回复
void addReplyError(client *c, const char *err) {
    addReplyErrorLength(c,err,strlen(err));
}

// 按格式添加多个错误回复
void addReplyErrorFormat(client *c, const char *fmt, ...) {
    size_t l, j;
    va_list ap;
    va_start(ap,fmt);
    sds s = sdscatvprintf(sdsempty(),fmt,ap);
    va_end(ap);
    /* Make sure there are no newlines in the string, otherwise invalid protocol
     * is emitted. */
    l = sdslen(s);
    for (j = 0; j < l; j++) {
        if (s[j] == '\r' || s[j] == '\n') s[j] = ' ';
    }
    addReplyErrorLength(c,s,sdslen(s));
    sdsfree(s);
}

// 按格式添加一个状态回复
void addReplyStatusLength(client *c, const char *s, size_t len) {
    addReplyString(c,"+",1);
    addReplyString(c,s,len);
    addReplyString(c,"\r\n",2);
}

// 按格式添加一个状态回复
void addReplyStatus(client *c, const char *status) {
    addReplyStatusLength(c,status,strlen(status));
}

// 按格式添加多个状态回复
void addReplyStatusFormat(client *c, const char *fmt, ...) {
    va_list ap;
    va_start(ap,fmt);
    sds s = sdscatvprintf(sdsempty(),fmt,ap);
    va_end(ap);
    addReplyStatusLength(c,s,sdslen(s));
    sdsfree(s);
}

/* Adds an empty object to the reply list that will contain the multi bulk
 * length, which is not known when this function is called. */
// 添加一个空对象到回复链表中，之后去在往链表中添加回复
void *addDeferredMultiBulkLength(client *c) {
    /* Note that we install the write event here even if the object is not
     * ready to be sent, since we are sure that before returning to the
     * event loop setDeferredMultiBulkLength() will be called. */
    // 准备client为可写的
    if (prepareClientToWrite(c) != C_OK) return NULL;
    // 创建一个空字符串对象追加到回复链表的末尾
    listAddNodeTail(c->reply,createObject(OBJ_STRING,NULL));
    // 返回这个空对象的地址
    return listLast(c->reply);
}

/* Populate the length object and try gluing it to the next chunk. */
// 设置回复的一个length对象，并尝试粘合下一个节点
void setDeferredMultiBulkLength(client *c, void *node, long length) {
    listNode *ln = (listNode*)node;
    robj *len, *next;

    /* Abort when *node is NULL (see addDeferredMultiBulkLength). */
    if (node == NULL) return;
    // 节点的值对象
    len = listNodeValue(ln);
    // length写入值对象
    len->ptr = sdscatprintf(sdsempty(),"*%ld\r\n",length);
    // 设置对象的编码
    len->encoding = OBJ_ENCODING_RAW; /* in case it was an EMBSTR. */
    // 更新回复链表的字节数
    c->reply_bytes += sdsZmallocSize(len->ptr);
    // 后继节点不为空
    if (ln->next != NULL) {
        next = listNodeValue(ln->next);

        /* Only glue when the next node is non-NULL (an sds in this case) */
        // 尝试将当前节点和后继节点的值粘合起来
        if (next->ptr != NULL) {
            c->reply_bytes -= sdsZmallocSize(len->ptr);
            c->reply_bytes -= getStringObjectSdsUsedMemory(next);
            len->ptr = sdscatlen(len->ptr,next->ptr,sdslen(next->ptr));
            c->reply_bytes += sdsZmallocSize(len->ptr);
            listDelNode(c->reply,ln->next);
        }
    }
    // 检查回复缓冲区的大小是否超过系统限制，如果超过则关闭client
    asyncCloseClientOnOutputBufferLimitReached(c);
}

/* Add a double as a bulk reply */
// 按照格式添加一个double类型的回复
void addReplyDouble(client *c, double d) {
    char dbuf[128], sbuf[128];
    int dlen, slen;
    // 是否是正负无穷值
    if (isinf(d)) {
        /* Libc in odd systems (Hi Solaris!) will format infinite in a
         * different way, so better to handle it in an explicit way. */
        addReplyBulkCString(c, d > 0 ? "inf" : "-inf");
    // 双精度浮点数
    } else {
        // 获取double的位数
        dlen = snprintf(dbuf,sizeof(dbuf),"%.17g",d);
        // 按照获取长度
        slen = snprintf(sbuf,sizeof(sbuf),"$%d\r\n%s\r\n",dlen,dbuf);
        // 将以字符串添加到回复中
        addReplyString(c,sbuf,slen);
    }
}

/* Add a long double as a bulk reply, but uses a human readable formatting
 * of the double instead of exposing the crude behavior of doubles to the
 * dear user. */
// 根据long double构建建一个人类友好的字符串对象，添加到回复中
void addReplyHumanLongDouble(client *c, long double d) {
    robj *o = createStringObjectFromLongDouble(d,1);
    addReplyBulk(c,o);
    decrRefCount(o);
}

/* Add a long long as integer reply or bulk len / multi bulk count.
 * Basically this is used to output <prefix><long long><crlf>. */
// 添加一个longlong作为整型回复或回复的长度，格式：<prefix><long long><crlf>
void addReplyLongLongWithPrefix(client *c, long long ll, char prefix) {
    char buf[128];
    int len;

    /* Things like $3\r\n or *2\r\n are emitted very often by the protocol
     * so we have a few shared objects to use if the integer is small
     * like it is most of the times. */
    // *2\r\n，前缀是'*'，ll小于共享的长度，则添加多条回复
    if (prefix == '*' && ll < OBJ_SHARED_BULKHDR_LEN && ll >= 0) {
        addReply(c,shared.mbulkhdr[ll]);
        return;
    // $3\r\n，前缀是'$'，ll小于共享的长度，则添加一条回复
    } else if (prefix == '$' && ll < OBJ_SHARED_BULKHDR_LEN && ll >= 0) {
        addReply(c,shared.bulkhdr[ll]);
        return;
    }

    // 超过共享的长度，则自己构建一条回复添加到client中
    buf[0] = prefix;
    len = ll2string(buf+1,sizeof(buf)-1,ll);
    buf[len+1] = '\r';
    buf[len+2] = '\n';
    addReplyString(c,buf,len+3);
}

// 添加一个整数回复
void addReplyLongLong(client *c, long long ll) {
    if (ll == 0)
        addReply(c,shared.czero);
    else if (ll == 1)
        addReply(c,shared.cone);
    else    //  :<ll>\r\n
        addReplyLongLongWithPrefix(c,ll,':');
}

// 添加多条回复的长度
void addReplyMultiBulkLen(client *c, long length) {
    if (length < OBJ_SHARED_BULKHDR_LEN)
        addReply(c,shared.mbulkhdr[length]);
    else    // *<length>\r\n
        addReplyLongLongWithPrefix(c,length,'*');
}

/* Create the length prefix of a bulk reply, example: $2234 */
// 创建一个回复的前缀长度，例如：$2234
void addReplyBulkLen(client *c, robj *obj) {
    size_t len;

    // 解码成字符串对象，获取长度
    if (sdsEncodedObject(obj)) {
        len = sdslen(obj->ptr);
    // 整数值
    } else {
        long n = (long)obj->ptr;

        /* Compute how many bytes will take this integer as a radix 10 string */
        // 计算出整数有多少位
        len = 1;
        // 负数还要加上负号
        if (n < 0) {
            len++;
            n = -n;
        }
        while((n = n/10) != 0) {
            len++;
        }
    }

    // 添加一个有前缀的整数回复
    if (len < OBJ_SHARED_BULKHDR_LEN)
        addReply(c,shared.bulkhdr[len]);
    else
        addReplyLongLongWithPrefix(c,len,'$');
}

/* Add a Redis Object as a bulk reply */
// 添加一个对象回复
void addReplyBulk(client *c, robj *obj) {
    addReplyBulkLen(c,obj);
    addReply(c,obj);
    addReply(c,shared.crlf);
}

/* Add a C buffer as bulk reply */
// 添加一个c缓冲区回复
void addReplyBulkCBuffer(client *c, const void *p, size_t len) {
    addReplyLongLongWithPrefix(c,len,'$');
    addReplyString(c,p,len);
    addReply(c,shared.crlf);
}

/* Add sds to reply (takes ownership of sds and frees it) */
// 添加一个sds回复
void addReplyBulkSds(client *c, sds s)  {
    addReplySds(c,sdscatfmt(sdsempty(),"$%u\r\n",
        (unsigned long)sdslen(s)));
    addReplySds(c,s);
    addReply(c,shared.crlf);
}

/* Add a C nul term string as bulk reply */
// 添加一个c字符串回复
void addReplyBulkCString(client *c, const char *s) {
    if (s == NULL) {
        addReply(c,shared.nullbulk);
    } else {
        addReplyBulkCBuffer(c,s,strlen(s));
    }
}

/* Add a long long as a bulk reply */
// 添加一个 long long类型的回复
void addReplyBulkLongLong(client *c, long long ll) {
    char buf[64];
    int len;

    len = ll2string(buf,64,ll);
    addReplyBulkCBuffer(c,buf,len);
}

/* Copy 'src' client output buffers into 'dst' client output buffers.
 * The function takes care of freeing the old output buffers of the
 * destination client. */
// 将src的所有输出拷贝给dst的所有输出中
void copyClientOutputBuffer(client *dst, client *src) {
    // 释放dst的回复链表
    listRelease(dst->reply);
    // 拷贝一份回复链表
    dst->reply = listDup(src->reply);
    // 拷贝固定回复缓冲区的内容
    memcpy(dst->buf,src->buf,src->bufpos);
    // 拷贝固定回复缓冲区的偏移量和回复链表的字节数
    dst->bufpos = src->bufpos;
    dst->reply_bytes = src->reply_bytes;
}

/* Return true if the specified client has pending reply buffers to write to
 * the socket. */
// 如果指定的client的回复缓冲区中还有数据，则返回真，表示可以写socket
int clientHasPendingReplies(client *c) {
    return c->bufpos || listLength(c->reply);
}

#define MAX_ACCEPTS_PER_CALL 1000
// TCP连接处理程序，创建一个client的连接状态
static void acceptCommonHandler(int fd, int flags, char *ip) {
    client *c;
    // 创建一个新的client
    if ((c = createClient(fd)) == NULL) {
        serverLog(LL_WARNING,
            "Error registering fd event for the new client: %s (fd=%d)",
            strerror(errno),fd);
        close(fd); /* May be already closed, just ignore errors */
        return;
    }
    /* If maxclient directive is set and this is one client more... close the
     * connection. Note that we create the client instead to check before
     * for this condition, since now the socket is already set in non-blocking
     * mode and we can send an error for free using the Kernel I/O */
    // 如果新的client超过server规定的maxclients的限制，那么想新client的fd写入错误信息，关闭该client
    // 先创建client，在进行数量检查，是因为更好的写入错误信息
    if (listLength(server.clients) > server.maxclients) {
        char *err = "-ERR max number of clients reached\r\n";

        /* That's a best effort error message, don't check write errors */
        if (write(c->fd,err,strlen(err)) == -1) {
            /* Nothing to do, Just to avoid the warning... */
        }
        // 更新拒接连接的个数
        server.stat_rejected_conn++;
        freeClient(c);
        return;
    }

    /* If the server is running in protected mode (the default) and there
     * is no password set, nor a specific interface is bound, we don't accept
     * requests from non loopback interfaces. Instead we try to explain the
     * user what to do to fix it if needed. */
    // 如果服务器正在以保护模式运行（默认），且没有设置密码，也没有绑定指定的接口，我们就不接受非回环接口的请求。相反，如果需要，我们会尝试解释用户如何解决问题
    if (server.protected_mode &&
        server.bindaddr_count == 0 &&
        server.requirepass == NULL &&
        !(flags & CLIENT_UNIX_SOCKET) &&
        ip != NULL)
    {
        if (strcmp(ip,"127.0.0.1") && strcmp(ip,"::1")) {
            char *err =
                "-DENIED Redis is running in protected mode because protected "
                "mode is enabled, no bind address was specified, no "
                "authentication password is requested to clients. In this mode "
                "connections are only accepted from the loopback interface. "
                "If you want to connect from external computers to Redis you "
                "may adopt one of the following solutions: "
                "1) Just disable protected mode sending the command "
                "'CONFIG SET protected-mode no' from the loopback interface "
                "by connecting to Redis from the same host the server is "
                "running, however MAKE SURE Redis is not publicly accessible "
                "from internet if you do so. Use CONFIG REWRITE to make this "
                "change permanent. "
                "2) Alternatively you can just disable the protected mode by "
                "editing the Redis configuration file, and setting the protected "
                "mode option to 'no', and then restarting the server. "
                "3) If you started the server manually just for testing, restart "
                "it with the '--protected-mode no' option. "
                "4) Setup a bind address or an authentication password. "
                "NOTE: You only need to do one of the above things in order for "
                "the server to start accepting connections from the outside.\r\n";
            if (write(c->fd,err,strlen(err)) == -1) {
                /* Nothing to do, Just to avoid the warning... */
            }
            // 更新拒接连接的个数
            server.stat_rejected_conn++;
            freeClient(c);
            return;
        }
    }

    // 更新连接的数量
    server.stat_numconnections++;
    // 更新client状态的标志
    c->flags |= flags;
}

// 创建一个TCP的连接处理程序
void acceptTcpHandler(aeEventLoop *el, int fd, void *privdata, int mask) {
    int cport, cfd, max = MAX_ACCEPTS_PER_CALL; //最大一个处理1000次连接
    char cip[NET_IP_STR_LEN];
    UNUSED(el);
    UNUSED(mask);
    UNUSED(privdata);

    while(max--) {
        // accept接受client的连接
        cfd = anetTcpAccept(server.neterr, fd, cip, sizeof(cip), &cport);
        if (cfd == ANET_ERR) {
            if (errno != EWOULDBLOCK)
                serverLog(LL_WARNING,
                    "Accepting client connection: %s", server.neterr);
            return;
        }
        // 打印连接的日志
        serverLog(LL_VERBOSE,"Accepted %s:%d", cip, cport);
        // 创建一个连接状态的client
        acceptCommonHandler(cfd,0,cip);
    }
}

// 创建一个本地连接处理程序
void acceptUnixHandler(aeEventLoop *el, int fd, void *privdata, int mask) {
    int cfd, max = MAX_ACCEPTS_PER_CALL;
    UNUSED(el);
    UNUSED(mask);
    UNUSED(privdata);

    while(max--) {
        // accept接受client的连接
        cfd = anetUnixAccept(server.neterr, fd);
        if (cfd == ANET_ERR) {
            if (errno != EWOULDBLOCK)
                serverLog(LL_WARNING,
                    "Accepting client connection: %s", server.neterr);
            return;
        }
        serverLog(LL_VERBOSE,"Accepted connection to %s", server.unixsocket);
        // 创建一个本地连接状态的client
        acceptCommonHandler(cfd,CLIENT_UNIX_SOCKET,NULL);
    }
}

// 释放client的参数列表
static void freeClientArgv(client *c) {
    int j;
    for (j = 0; j < c->argc; j++)
        decrRefCount(c->argv[j]);
    c->argc = 0;
    c->cmd = NULL;
}

/* Close all the slaves connections. This is useful in chained replication
 * when we resync with our own master and want to force all our slaves to
 * resync with us as well. */
// 关闭所有从节点服务器的连接，强制从节点服务器进行重新同步操作
void disconnectSlaves(void) {
    // 遍历服务器的从节点链表，释放
    while (listLength(server.slaves)) {
        listNode *ln = listFirst(server.slaves);
        freeClient((client*)ln->value);
    }
}

/* Remove the specified client from global lists where the client could
 * be referenced, not including the Pub/Sub channels.
 * This is used by freeClient() and replicationCacheMaster(). */
// 从client所有保存各种client状态的链表中删除指定的client
void unlinkClient(client *c) {
    listNode *ln;

    /* If this is marked as current client unset it. */
    // 如果指定的client被被标记为用于崩溃报告的client，则删除
    if (server.current_client == c) server.current_client = NULL;

    /* Certain operations must be done only if the client has an active socket.
     * If the client was already unlinked or if it's a "fake client" the
     * fd is already set to -1. */
    // 指定的client不是伪client，或不是已经删除的client
    if (c->fd != -1) {
        /* Remove from the list of active clients. */
        // 从client链表中找到地址
        ln = listSearchKey(server.clients,c);
        serverAssert(ln != NULL);
        // 删除当前client的节点
        listDelNode(server.clients,ln);

        /* Unregister async I/O handlers and close the socket. */
        // 从文件事件中删除对该client的fd的监听
        aeDeleteFileEvent(server.el,c->fd,AE_READABLE);
        aeDeleteFileEvent(server.el,c->fd,AE_WRITABLE);
        // 释放文件描述符
        close(c->fd);
        c->fd = -1;
    }

    /* Remove from the list of pending writes if needed. */
    // 如果client还有输出的数据，但是没有设置写处理程序
    if (c->flags & CLIENT_PENDING_WRITE) {
        // 要写或者安装写处理程序的client链表找到当前client
        ln = listSearchKey(server.clients_pending_write,c);
        serverAssert(ln != NULL);
        // 删除当前client的节点
        listDelNode(server.clients_pending_write,ln);
        // 取消标志
        c->flags &= ~CLIENT_PENDING_WRITE;
    }

    /* When client was just unblocked because of a blocking operation,
     * remove it from the list of unblocked clients. */
    // 如果指定的client是非阻塞的
    if (c->flags & CLIENT_UNBLOCKED) {
        // 则从非阻塞的client链表中找到并删除
        ln = listSearchKey(server.unblocked_clients,c);
        serverAssert(ln != NULL);
        listDelNode(server.unblocked_clients,ln);
        // 取消标志
        c->flags &= ~CLIENT_UNBLOCKED;
    }
}

// 释放client
void freeClient(client *c) {
    listNode *ln;

    /* If it is our master that's beging disconnected we should make sure
     * to cache the state to try a partial resynchronization later.
     *
     * Note that before doing this we make sure that the client is not in
     * some unexpected state, by checking its flags. */
    // 如果client是主服务器的，则要缓存client
    if (server.master && c->flags & CLIENT_MASTER) {
        serverLog(LL_WARNING,"Connection with master lost.");
        // 如果client不是 即将要关闭的 或 正要关闭的 阻塞或非阻塞的client
        if (!(c->flags & (CLIENT_CLOSE_AFTER_REPLY|
                          CLIENT_CLOSE_ASAP|
                          CLIENT_BLOCKED|
                          CLIENT_UNBLOCKED)))
        {
            // 如果是主节点的client，要缓存client，可以迅速重新启用恢复，不用整体从头建立连接
            replicationCacheMaster(c);
            return;
        }
    }

    /* Log link disconnection with slave */
    // 如果是从服务器的client，且不是执行监控的client
    if ((c->flags & CLIENT_SLAVE) && !(c->flags & CLIENT_MONITOR)) {
        // 更新日志
        serverLog(LL_WARNING,"Connection with slave %s lost.",
            replicationGetSlaveName(c));
    }

    /* Free the query buffer */
    // 清空查询缓存
    sdsfree(c->querybuf);
    c->querybuf = NULL;

    /* Deallocate structures used to block on blocking ops. */
    // 如果是阻塞的client，则解除阻塞
    if (c->flags & CLIENT_BLOCKED) unblockClient(c);
    // 释放造成client阻塞的键
    dictRelease(c->bpop.keys);

    /* UNWATCH all the keys */
    // 清空监视的键
    unwatchAllKeys(c);
    listRelease(c->watched_keys);

    /* Unsubscribe from all the pubsub channels */
    // 退订所有的频道和模式
    pubsubUnsubscribeAllChannels(c,0);
    pubsubUnsubscribeAllPatterns(c,0);
    dictRelease(c->pubsub_channels);
    listRelease(c->pubsub_patterns);

    /* Free data structures. */
    // 释放回复链表
    listRelease(c->reply);
    // 释放client的参数列表
    freeClientArgv(c);

    /* Unlink the client: this will close the socket, remove the I/O
     * handlers, and remove references of the client from different
     * places where active clients may be referenced. */
    // 从不同状态的client链表中删除client，会关闭socket，从事件循环中移除对该client的监听
    unlinkClient(c);

    /* Master/slave cleanup Case 1:
     * we lost the connection with a slave. */
    // 如果是从节点的client
    if (c->flags & CLIENT_SLAVE) {
        // 如果当前服务器的复制状态为：正在发送RDB文件给从节点
        if (c->replstate == SLAVE_STATE_SEND_BULK) {
            // 关闭用于保存主服务器发送RDB文件的文件描述符
            if (c->repldbfd != -1) close(c->repldbfd);
            // 释放RDB文件的字符串形式的大小
            if (c->replpreamble) sdsfree(c->replpreamble);
        }
        // 获取保存当前client的链表地址，监控器链表或从节点链表
        list *l = (c->flags & CLIENT_MONITOR) ? server.monitors : server.slaves;
        // 取出保存client的节点
        ln = listSearchKey(l,c);
        serverAssert(ln != NULL);
        // 删除该client
        listDelNode(l,ln);
        /* We need to remember the time when we started to have zero
         * attached slaves, as after some time we'll free the replication
         * backlog. */
        // 服务器从节点链表为空，要保存当前时间
        if (c->flags & CLIENT_SLAVE && listLength(server.slaves) == 0)
            server.repl_no_slaves_since = server.unixtime;
        // 重新计算状态良好的从节点服务器的数量
        refreshGoodSlavesCount();
    }

    /* Master/slave cleanup Case 2:
     * we lost the connection with the master. */
    // 如果是主节点的client，处理从服务器和主服务器的断开
    if (c->flags & CLIENT_MASTER) replicationHandleMasterDisconnection();

    /* If this client was scheduled for async freeing we need to remove it
     * from the queue. */
    // 如果client即将关闭，则从clients_to_close中找到并删除
    if (c->flags & CLIENT_CLOSE_ASAP) {
        ln = listSearchKey(server.clients_to_close,c);
        serverAssert(ln != NULL);
        listDelNode(server.clients_to_close,ln);
    }

    /* Release other dynamically allocated client structure fields,
     * and finally release the client structure itself. */
    // 如果client有名字，则释放
    if (c->name) decrRefCount(c->name);
    // 释放参数列表
    zfree(c->argv);
    // 清除事物状态
    freeClientMultiState(c);
    sdsfree(c->peerid);
    zfree(c);
}

/* Schedule a client to free it at a safe time in the serverCron() function.
 * This function is useful when we need to terminate a client but we are in
 * a context where calling freeClient() is not possible, because the client
 * should be valid for the continuation of the flow of the program. */
// 异步释放client
void freeClientAsync(client *c) {
    // 如果是已经即将关闭或者是lua脚本的伪client，则直接返回
    if (c->flags & CLIENT_CLOSE_ASAP || c->flags & CLIENT_LUA) return;
    c->flags |= CLIENT_CLOSE_ASAP;
    // 将client加入到即将关闭的client链表中
    listAddNodeTail(server.clients_to_close,c);
}

// 取消设置异步释放的client
void freeClientsInAsyncFreeQueue(void) {
    // 遍历所有即将关闭的client
    while (listLength(server.clients_to_close)) {
        listNode *ln = listFirst(server.clients_to_close);
        client *c = listNodeValue(ln);

        // 取消立即关闭的标志
        c->flags &= ~CLIENT_CLOSE_ASAP;
        freeClient(c);
        // 从即将关闭的client链表中删除
        listDelNode(server.clients_to_close,ln);
    }
}

/* Write data in output buffers to client. Return C_OK if the client
 * is still valid after the call, C_ERR if it was freed. */
// 将输出缓冲区的数据写给client，如果client被释放则返回C_ERR，没被释放则返回C_OK
int writeToClient(int fd, client *c, int handler_installed) {
    ssize_t nwritten = 0, totwritten = 0;
    size_t objlen;
    size_t objmem;
    robj *o;

    // 如果指定的client的回复缓冲区中还有数据，则返回真，表示可以写socket
    while(clientHasPendingReplies(c)) {
        // 固定缓冲区发送未完成
        if (c->bufpos > 0) {
            // 将缓冲区的数据写到fd中
            nwritten = write(fd,c->buf+c->sentlen,c->bufpos-c->sentlen);
            // 写失败跳出循环
            if (nwritten <= 0) break;
            // 更新发送的数据计数器
            c->sentlen += nwritten;
            totwritten += nwritten;

            /* If the buffer was sent, set bufpos to zero to continue with
             * the remainder of the reply. */
            // 如果发送的数据等于buf的偏移量，表示发送完成
            if ((int)c->sentlen == c->bufpos) {
                // 则将其重置
                c->bufpos = 0;
                c->sentlen = 0;
            }
        // 固定缓冲区发送完成，发送回复链表的内容
        } else {
            // 回复链表的第一条回复对象，和对象值的长度和所占的内存
            o = listNodeValue(listFirst(c->reply));
            objlen = sdslen(o->ptr);
            objmem = getStringObjectSdsUsedMemory(o);

            // 跳过空对象，并删除这个对象
            if (objlen == 0) {
                listDelNode(c->reply,listFirst(c->reply));
                c->reply_bytes -= objmem;
                continue;
            }

            // 将当前节点的值写到fd中
            nwritten = write(fd, ((char*)o->ptr)+c->sentlen,objlen-c->sentlen);
            // 写失败跳出循环
            if (nwritten <= 0) break;
            // 更新发送的数据计数器
            c->sentlen += nwritten;
            totwritten += nwritten;

            /* If we fully sent the object on head go to the next one */
            // 发送完成，则删除该节点，重置发送的数据长度，更新回复链表的总字节数
            if (c->sentlen == objlen) {
                listDelNode(c->reply,listFirst(c->reply));
                c->sentlen = 0;
                c->reply_bytes -= objmem;
            }
        }
        /* Note that we avoid to send more than NET_MAX_WRITES_PER_EVENT
         * bytes, in a single threaded server it's a good idea to serve
         * other clients as well, even if a very large request comes from
         * super fast link that is always able to accept data (in real world
         * scenario think about 'KEYS *' against the loopback interface).
         *
         * However if we are over the maxmemory limit we ignore that and
         * just deliver as much data as it is possible to deliver. */
        // 更新写到网络的字节数
        server.stat_net_output_bytes += totwritten;
        // 如果这次写的总量大于NET_MAX_WRITES_PER_EVENT的限制，则会中断本次的写操作，将处理时间让给其他的client，以免一个非常的回复独占服务器，剩余的数据下次继续在写
        // 但是，如果当服务器的内存数已经超过maxmemory，即使超过最大写NET_MAX_WRITES_PER_EVENT的限制，也会继续执行写入操作，是为了尽快写入给客户端
        if (totwritten > NET_MAX_WRITES_PER_EVENT &&
            (server.maxmemory == 0 ||
             zmalloc_used_memory() < server.maxmemory)) break;
    }
    // 处理写入失败
    if (nwritten == -1) {
        if (errno == EAGAIN) {
            nwritten = 0;
        } else {
            serverLog(LL_VERBOSE,
                "Error writing to client: %s", strerror(errno));
            freeClient(c);
            return C_ERR;
        }
    }
    // 写入成功
    if (totwritten > 0) {
        /* For clients representing masters we don't count sending data
         * as an interaction, since we always send REPLCONF ACK commands
         * that take some time to just fill the socket output buffer.
         * We just rely on data / pings received for timeout detection. */
        // 如果不是主节点服务器，则更新最近和服务器交互的时间
        if (!(c->flags & CLIENT_MASTER)) c->lastinteraction = server.unixtime;
    }
    // 如果指定的client的回复缓冲区中已经没有数据，发送完成
    if (!clientHasPendingReplies(c)) {
        c->sentlen = 0;
        // 删除当前client的可读事件的监听
        if (handler_installed) aeDeleteFileEvent(server.el,c->fd,AE_WRITABLE);

        /* Close connection after entire reply has been sent. */
        // 如果指定了写入按成之后立即关闭的标志，则释放client
        if (c->flags & CLIENT_CLOSE_AFTER_REPLY) {
            freeClient(c);
            return C_ERR;
        }
    }
    return C_OK;
}

/* Write event handler. Just send data to the client. */
// 写事件处理程序，只是发送回复给client
void sendReplyToClient(aeEventLoop *el, int fd, void *privdata, int mask) {
    UNUSED(el);
    UNUSED(mask);
    // 发送完数据会删除fd的可读事件
    writeToClient(fd,privdata,1);
}

/* This function is called just before entering the event loop, in the hope
 * we can just write the replies to the client output buffer without any
 * need to use a syscall in order to install the writable event handler,
 * get it called, and so forth. */
// 这个函数是在进入事件循环之前调用的，希望我们只需要将回复写入客户端输出缓冲区，而不需要使用系统调用来安装可写事件处理程序，调用它等等。
int handleClientsWithPendingWrites(void) {
    listIter li;
    listNode *ln;
    // 要写或者安装写处理程序的client链表的长度
    int processed = listLength(server.clients_pending_write);
    // 设置遍历方向
    listRewind(server.clients_pending_write,&li);
    // 遍历链表
    while((ln = listNext(&li))) {
        // 取出当前client
        client *c = listNodeValue(ln);
        // 删除client的 要写或者安装写处理程序 的标志
        c->flags &= ~CLIENT_PENDING_WRITE;
        // 从要写或者安装写处理程序的client链表中删除
        listDelNode(server.clients_pending_write,ln);

        /* Try to write buffers to the client socket. */
        // 将client的回复数据发送给client，但是不会删除fd的可读事件
        if (writeToClient(c->fd,c,0) == C_ERR) continue;

        /* If there is nothing left, do nothing. Otherwise install
         * the write handler. */
        // 如果指定的client的回复缓冲区中还有数据，那么安装写处理程序，否则异步释放client
        if (clientHasPendingReplies(c) &&
            aeCreateFileEvent(server.el, c->fd, AE_WRITABLE,
                sendReplyToClient, c) == AE_ERR)
        {
            freeClientAsync(c);
        }
    }
    // 返回处理的client的个数
    return processed;
}

/* resetClient prepare the client to process the next command */
// 重置client准备去处理下一个命令
void resetClient(client *c) {
    redisCommandProc *prevcmd = c->cmd ? c->cmd->proc : NULL;

    // 释放参数列表，重置client
    freeClientArgv(c);
    c->reqtype = 0;
    c->multibulklen = 0;
    c->bulklen = -1;

    /* We clear the ASKING flag as well if we are not inside a MULTI, and
     * if what we just executed is not the ASKING command itself. */
    // 清除CLIENT_ASKING的标志
    if (!(c->flags & CLIENT_MULTI) && prevcmd != askingCommand)
        c->flags &= ~CLIENT_ASKING;

    /* Remove the CLIENT_REPLY_SKIP flag if any so that the reply
     * to the next command will be sent, but set the flag if the command
     * we just processed was "CLIENT REPLY SKIP". */
    // 清理CLIENT_REPLY_SKIP标识，不跳过当前命令的回复
    c->flags &= ~CLIENT_REPLY_SKIP;
    // 为下一条命令设置CLIENT_REPLY_SKIP标志
    if (c->flags & CLIENT_REPLY_SKIP_NEXT) {
        c->flags |= CLIENT_REPLY_SKIP;
        c->flags &= ~CLIENT_REPLY_SKIP_NEXT;
    }
}

// 处理Telnet发来的内联命令，并创建成对象，保存在client的参数列表中
int processInlineBuffer(client *c) {
    char *newline;
    int argc, j;
    sds *argv, aux;
    size_t querylen;

    /* Search for end of line */
    // 定位到一行的换行符
    newline = strchr(c->querybuf,'\n');

    /* Nothing to do without a \r\n */
    // 没有找到\r\n，表示不符合协议，返回错误
    if (newline == NULL) {
        if (sdslen(c->querybuf) > PROTO_INLINE_MAX_SIZE) {
            addReplyError(c,"Protocol error: too big inline request");
            setProtocolError(c,0);
        }
        return C_ERR;
    }

    /* Handle the \r\n case. */
    // newline不等于空，且查询缓存不光是一个换行\r\n
    if (newline && newline != c->querybuf && *(newline-1) == '\r')
        newline--;

    /* Split the input buffer up to the \r\n */
    // 分割缓存直到换行
    querylen = newline-(c->querybuf);       //querybuf的大小
    aux = sdsnewlen(c->querybuf,querylen);  //根据querylen，将querybuf的大小重新分配返回aux
    argv = sdssplitargs(aux,&argc);         //将aux分割
    sdsfree(aux);
    // 分割失败，返回错误
    if (argv == NULL) {
        addReplyError(c,"Protocol error: unbalanced quotes in request");
        setProtocolError(c,0);
        return C_ERR;
    }

    /* Newline from slaves can be used to refresh the last ACK time.
     * This is useful for a slave to ping back while loading a big
     * RDB file. */
    // 来自从节点的换行符可用于刷新上一个ACK时间。 这对于从节点在加载大型RDB文件时进行ping返回是有用的
    if (querylen == 0 && c->flags & CLIENT_SLAVE)
        c->repl_ack_time = server.unixtime;

    /* Leave data after the first line of the query in the buffer */
    // 从querybuf中将已经分割的到argv的数据删除
    sdsrange(c->querybuf,querylen+2,-1);

    /* Setup argv array on client structure */
    // 分割成功，则为client的参数列表分配空间
    if (argc) {
        if (c->argv) zfree(c->argv);    //释放原有的
        c->argv = zmalloc(sizeof(robj*)*argc);
    }

    /* Create redis objects for all arguments. */
    // 为每一个参数创建对象，保存到client的参数列表中
    for (c->argc = 0, j = 0; j < argc; j++) {
        if (sdslen(argv[j])) {
            c->argv[c->argc] = createObject(OBJ_STRING,argv[j]);
            c->argc++;
        } else {
            sdsfree(argv[j]);
        }
    }
    zfree(argv);
    return C_OK;
}

/* Helper function. Trims query buffer to make the function that processes
 * multi bulk requests idempotent. */
// 修剪查询缓冲区，用来处理多批量请求
static void setProtocolError(client *c, int pos) {
    // 处理日志
    if (server.verbosity <= LL_VERBOSE) {
        // 将client的所有信息转换为sds，打印到日志里
        sds client = catClientInfoString(sdsempty(),c);
        serverLog(LL_VERBOSE,
            "Protocol error from client: %s", client);
        sdsfree(client);
    }
    // 设置发送回复后立即关闭client
    c->flags |= CLIENT_CLOSE_AFTER_REPLY;
    // 修剪查询缓冲区
    sdsrange(c->querybuf,pos,-1);
}

// 将client的querybuf中的协议内容转换为client的参数列表中的对象
int processMultibulkBuffer(client *c) {
    char *newline = NULL;
    int pos = 0, ok;
    long long ll;

    // 参数列表中命令数量为0，因此先分配空间
    if (c->multibulklen == 0) {
        /* The client should have been reset */
        serverAssertWithInfo(c,NULL,c->argc == 0);

        /* Multi bulk length cannot be read without a \r\n */
        // 查询第一个换行符
        newline = strchr(c->querybuf,'\r');
        // 没有找到\r\n，表示不符合协议，返回错误
        if (newline == NULL) {
            if (sdslen(c->querybuf) > PROTO_INLINE_MAX_SIZE) {
                addReplyError(c,"Protocol error: too big mbulk count string");
                setProtocolError(c,0);
            }
            return C_ERR;
        }

        /* Buffer should also contain \n */
        // 检查格式
        if (newline-(c->querybuf) > ((signed)sdslen(c->querybuf)-2))
            return C_ERR;

        /* We know for sure there is a whole line since newline != NULL,
         * so go ahead and find out the multi bulk length. */
        // 保证第一个字符为'*'
        serverAssertWithInfo(c,NULL,c->querybuf[0] == '*');
        // 将'*'之后的数字转换为整数。*3\r\n
        ok = string2ll(c->querybuf+1,newline-(c->querybuf+1),&ll);
        if (!ok || ll > 1024*1024) {
            addReplyError(c,"Protocol error: invalid multibulk length");
            setProtocolError(c,pos);
            return C_ERR;
        }

        // 指向"*3\r\n"的"\r\n"之后的位置
        pos = (newline-c->querybuf)+2;
        // 空白命令，则将之前的删除，保留未阅读的部分
        if (ll <= 0) {
            sdsrange(c->querybuf,pos,-1);
            return C_OK;
        }

        // 参数数量
        c->multibulklen = ll;

        /* Setup argv array on client structure */
        // 分配client参数列表的空间
        if (c->argv) zfree(c->argv);
        c->argv = zmalloc(sizeof(robj*)*c->multibulklen);
    }

    serverAssertWithInfo(c,NULL,c->multibulklen > 0);
    // 读入multibulklen个参数，并创建对象保存在参数列表中
    while(c->multibulklen) {
        /* Read bulk length if unknown */
        // 读入参数的长度
        if (c->bulklen == -1) {
            // 找到换行符，确保"\r\n"存在
            newline = strchr(c->querybuf+pos,'\r');
            if (newline == NULL) {
                if (sdslen(c->querybuf) > PROTO_INLINE_MAX_SIZE) {
                    addReplyError(c,
                        "Protocol error: too big bulk count string");
                    setProtocolError(c,0);
                    return C_ERR;
                }
                break;
            }

            /* Buffer should also contain \n */
            // 检查格式
            if (newline-(c->querybuf) > ((signed)sdslen(c->querybuf)-2))
                break;

            // $3\r\nSET\r\n...，确保是'$'字符，保证格式
            if (c->querybuf[pos] != '$') {
                addReplyErrorFormat(c,
                    "Protocol error: expected '$', got '%c'",
                    c->querybuf[pos]);
                setProtocolError(c,pos);
                return C_ERR;
            }

            // 将参数长度保存到ll。
            ok = string2ll(c->querybuf+pos+1,newline-(c->querybuf+pos+1),&ll);
            if (!ok || ll < 0 || ll > 512*1024*1024) {
                addReplyError(c,"Protocol error: invalid bulk length");
                setProtocolError(c,pos);
                return C_ERR;
            }

            // 定位第一个参数的位置，也就是SET的S
            pos += newline-(c->querybuf+pos)+2;
            // 参数长度太长，进行优化
            if (ll >= PROTO_MBULK_BIG_ARG) {
                size_t qblen;

                /* If we are going to read a large object from network
                 * try to make it likely that it will start at c->querybuf
                 * boundary so that we can optimize object creation
                 * avoiding a large copy of data. */
                // 如果我们要从网络中读取一个大的对象，尝试使它可能从c-> querybuf边界开始，以便我们可以优化对象创建，避免大量的数据副本
                // 保存未读取的部分
                sdsrange(c->querybuf,pos,-1);
                // 重置偏移量
                pos = 0;
                // 获取querybuf中已使用的长度
                qblen = sdslen(c->querybuf);
                /* Hint the sds library about the amount of bytes this string is
                 * going to contain. */
                // 扩展querybuf的大小
                if (qblen < (size_t)ll+2)
                    c->querybuf = sdsMakeRoomFor(c->querybuf,ll+2-qblen);
            }
            // 保存参数的长度
            c->bulklen = ll;
        }

        /* Read bulk argument */
        // 因为只读了multibulklen字节的数据，读到的数据不够，则直接跳出循环，执行processInputBuffer()函数循环读取
        if (sdslen(c->querybuf)-pos < (unsigned)(c->bulklen+2)) {
            /* Not enough data (+2 == trailing \r\n) */
            break;
        // 为参数创建了对象
        } else {
            /* Optimization: if the buffer contains JUST our bulk element
             * instead of creating a new object by *copying* the sds we
             * just use the current sds string. */
            // 如果读入的长度大于32k
            if (pos == 0 &&
                c->bulklen >= PROTO_MBULK_BIG_ARG &&
                (signed) sdslen(c->querybuf) == c->bulklen+2)
            {
                c->argv[c->argc++] = createObject(OBJ_STRING,c->querybuf);
                // 跳过换行
                sdsIncrLen(c->querybuf,-2); /* remove CRLF */
                /* Assume that if we saw a fat argument we'll see another one
                 * likely... */
                // 设置一个新长度
                c->querybuf = sdsnewlen(NULL,c->bulklen+2);
                sdsclear(c->querybuf);
                pos = 0;
            // 创建对象保存在client的参数列表中
            } else {
                c->argv[c->argc++] =
                    createStringObject(c->querybuf+pos,c->bulklen);
                pos += c->bulklen+2;
            }
            // 清空命令内容的长度
            c->bulklen = -1;
            // 未读取命令参数的数量，读取一个，该值减1
            c->multibulklen--;
        }
    }

    /* Trim to pos */
    // 删除已经读取的，保留未读取的
    if (pos) sdsrange(c->querybuf,pos,-1);

    /* We're done when c->multibulk == 0 */
    // 命令的参数全部被读取完
    if (c->multibulklen == 0) return C_OK;

    /* Still not read to process the command */
    return C_ERR;
}

// 处理client输入的命令内容
void processInputBuffer(client *c) {
    server.current_client = c;
    /* Keep processing while there is something in the input buffer */
    // 一直读输入缓冲区的内容
    while(sdslen(c->querybuf)) {
        /* Return if clients are paused. */
        // 如果处于暂停状态，直接返回
        if (!(c->flags & CLIENT_SLAVE) && clientsArePaused()) break;

        /* Immediately abort if the client is in the middle of something. */
        // 如果client处于被阻塞状态，直接返回
        if (c->flags & CLIENT_BLOCKED) break;

        /* CLIENT_CLOSE_AFTER_REPLY closes the connection once the reply is
         * written to the client. Make sure to not let the reply grow after
         * this flag has been set (i.e. don't process more commands).
         *
         * The same applies for clients we want to terminate ASAP. */
        // 如果client处于关闭状态，则直接返回
        if (c->flags & (CLIENT_CLOSE_AFTER_REPLY|CLIENT_CLOSE_ASAP)) break;

        /* Determine request type when unknown. */
        // 如果是未知的请求类型，则判定请求类型
        if (!c->reqtype) {
            // 如果是"*"开头，则是多条请求，是client发来的
            if (c->querybuf[0] == '*') {
                c->reqtype = PROTO_REQ_MULTIBULK;
            // 否则就是内联请求，是Telnet发来的
            } else {
                c->reqtype = PROTO_REQ_INLINE;
            }
        }

        // 如果是内联请求
        if (c->reqtype == PROTO_REQ_INLINE) {
            // 处理Telnet发来的内联命令，并创建成对象，保存在client的参数列表中
            if (processInlineBuffer(c) != C_OK) break;
        // 如果是多条请求
        } else if (c->reqtype == PROTO_REQ_MULTIBULK) {
            // 将client的querybuf中的协议内容转换为client的参数列表中的对象
            if (processMultibulkBuffer(c) != C_OK) break;
        } else {
            serverPanic("Unknown request type");
        }

        /* Multibulk processing could see a <= 0 length. */
        // 如果参数为0，则重置client
        if (c->argc == 0) {
            resetClient(c);
        } else {
            /* Only reset the client when the command was executed. */
            // 执行命令成功后重置client
            if (processCommand(c) == C_OK)
                resetClient(c);
            /* freeMemoryIfNeeded may flush slave output buffers. This may result
             * into a slave, that may be the active client, to be freed. */
            if (server.current_client == NULL) break;
        }
    }
    // 执行成功，则将用于崩溃报告的client设置为NULL
    server.current_client = NULL;
}

// 读取client的输入缓冲区的内容
void readQueryFromClient(aeEventLoop *el, int fd, void *privdata, int mask) {
    client *c = (client*) privdata;
    int nread, readlen;
    size_t qblen;
    UNUSED(el);
    UNUSED(mask);

    // 读入的长度，默认16MB
    readlen = PROTO_IOBUF_LEN;
    /* If this is a multi bulk request, and we are processing a bulk reply
     * that is large enough, try to maximize the probability that the query
     * buffer contains exactly the SDS string representing the object, even
     * at the risk of requiring more read(2) calls. This way the function
     * processMultiBulkBuffer() can avoid copying buffers to create the
     * Redis Object representing the argument. */
    // 如果是多条请求，根据请求的大小，设置读入的长度readlen
    if (c->reqtype == PROTO_REQ_MULTIBULK && c->multibulklen && c->bulklen != -1
        && c->bulklen >= PROTO_MBULK_BIG_ARG)
    {
        int remaining = (unsigned)(c->bulklen+2)-sdslen(c->querybuf);

        if (remaining < readlen) readlen = remaining;
    }

    // 输入缓冲区的长度
    qblen = sdslen(c->querybuf);
    // 更新缓冲区的峰值
    if (c->querybuf_peak < qblen) c->querybuf_peak = qblen;
    // 扩展缓冲区的大小
    c->querybuf = sdsMakeRoomFor(c->querybuf, readlen);
    // 将client发来的命令，读入到输入缓冲区中
    nread = read(fd, c->querybuf+qblen, readlen);
    // 读操作出错
    if (nread == -1) {
        if (errno == EAGAIN) {
            return;
        } else {
            serverLog(LL_VERBOSE, "Reading from client: %s",strerror(errno));
            freeClient(c);
            return;
        }
    // 读操作完成
    } else if (nread == 0) {
        serverLog(LL_VERBOSE, "Client closed connection");
        freeClient(c);
        return;
    }
    // 更新输入缓冲区的已用大小和未用大小。
    sdsIncrLen(c->querybuf,nread);
    // 设置最后一次服务器和client交互的时间
    c->lastinteraction = server.unixtime;
    // 如果是主节点，则更新复制操作的偏移量
    if (c->flags & CLIENT_MASTER) c->reploff += nread;
    // 更新从网络输入的字节数
    server.stat_net_input_bytes += nread;
    // 如果输入缓冲区长度超过服务器设置的最大缓冲区长度
    if (sdslen(c->querybuf) > server.client_max_querybuf_len) {
        // 将client信息转换为sds
        sds ci = catClientInfoString(sdsempty(),c), bytes = sdsempty();

        // 输入缓冲区保存在bytes中
        bytes = sdscatrepr(bytes,c->querybuf,64);
        // 打印到日志
        serverLog(LL_WARNING,"Closing client that reached max query buffer length: %s (qbuf initial bytes: %s)", ci, bytes);
        // 释放空间
        sdsfree(ci);
        sdsfree(bytes);
        freeClient(c);
        return;
    }
    // 处理client输入的命令内容
    processInputBuffer(c);
}

// 获取当前服务器中，所有client中最大的输入缓冲区和回复链表
void getClientsMaxBuffers(unsigned long *longest_output_list,
                          unsigned long *biggest_input_buffer) {
    client *c;
    listNode *ln;
    listIter li;
    unsigned long lol = 0, bib = 0;

    // 设置链表迭代方向和起始节点
    listRewind(server.clients,&li);
    // 遍历所有的client节点
    while ((ln = listNext(&li)) != NULL) {
        c = listNodeValue(ln);
        // 保存当前client空间最大的回复链表大小
        if (listLength(c->reply) > lol) lol = listLength(c->reply);
        // 保存当前client最大的输入缓冲区大小
        if (sdslen(c->querybuf) > bib) bib = sdslen(c->querybuf);
    }
    // 保存到参数中
    *longest_output_list = lol;
    *biggest_input_buffer = bib;
}

/* A Redis "Peer ID" is a colon separated ip:port pair.
 * For IPv4 it's in the form x.y.z.k:port, example: "127.0.0.1:1234".
 * For IPv6 addresses we use [] around the IP part, like in "[::1]:1234".
 * For Unix sockets we use path:0, like in "/tmp/redis:0".
 *
 * A Peer ID always fits inside a buffer of NET_PEER_ID_LEN bytes, including
 * the null term.
 *
 * On failure the function still populates 'peerid' with the "?:0" string
 * in case you want to relax error checking or need to display something
 * anyway (see anetPeerToString implementation for more info). */
// 生成client的Peer ID
void genClientPeerId(client *client, char *peerid,
                            size_t peerid_len) {
    // 本机socket的client
    if (client->flags & CLIENT_UNIX_SOCKET) {
        /* Unix socket client. */
        snprintf(peerid,peerid_len,"%s:0",server.unixsocket);
    } else {
        // TCP连接的client
        /* TCP client. */
        anetFormatPeer(client->fd,peerid,peerid_len);
    }
}

/* This function returns the client peer id, by creating and caching it
 * if client->peerid is NULL, otherwise returning the cached value.
 * The Peer ID never changes during the life of the client, however it
 * is expensive to compute. */
// 获取client的Peer ID
char *getClientPeerId(client *c) {
    char peerid[NET_PEER_ID_LEN];

    // 如果client没有，则生成一个
    if (c->peerid == NULL) {
        genClientPeerId(c,peerid,sizeof(peerid));
        c->peerid = sdsnew(peerid);
    }
    return c->peerid;
}

/* Concatenate a string representing the state of a client in an human
 * readable format, into the sds string 's'. */
// 获取client的每种信息，并以sds形式返回
sds catClientInfoString(sds s, client *client) {
    char flags[16], events[3], *p;
    int emask;

    p = flags;
    if (client->flags & CLIENT_SLAVE) {
        if (client->flags & CLIENT_MONITOR)
            *p++ = 'O';
        else
            *p++ = 'S';
    }
    if (client->flags & CLIENT_MASTER) *p++ = 'M';
    if (client->flags & CLIENT_MULTI) *p++ = 'x';
    if (client->flags & CLIENT_BLOCKED) *p++ = 'b';
    if (client->flags & CLIENT_DIRTY_CAS) *p++ = 'd';
    if (client->flags & CLIENT_CLOSE_AFTER_REPLY) *p++ = 'c';
    if (client->flags & CLIENT_UNBLOCKED) *p++ = 'u';
    if (client->flags & CLIENT_CLOSE_ASAP) *p++ = 'A';
    if (client->flags & CLIENT_UNIX_SOCKET) *p++ = 'U';
    if (client->flags & CLIENT_READONLY) *p++ = 'r';
    if (p == flags) *p++ = 'N';
    *p++ = '\0';

    emask = client->fd == -1 ? 0 : aeGetFileEvents(server.el,client->fd);
    p = events;
    if (emask & AE_READABLE) *p++ = 'r';
    if (emask & AE_WRITABLE) *p++ = 'w';
    *p = '\0';
    return sdscatfmt(s,
        "id=%U addr=%s fd=%i name=%s age=%I idle=%I flags=%s db=%i sub=%i psub=%i multi=%i qbuf=%U qbuf-free=%U obl=%U oll=%U omem=%U events=%s cmd=%s",
        (unsigned long long) client->id,
        getClientPeerId(client),
        client->fd,
        client->name ? (char*)client->name->ptr : "",
        (long long)(server.unixtime - client->ctime),
        (long long)(server.unixtime - client->lastinteraction),
        flags,
        client->db->id,
        (int) dictSize(client->pubsub_channels),
        (int) listLength(client->pubsub_patterns),
        (client->flags & CLIENT_MULTI) ? client->mstate.count : -1,
        (unsigned long long) sdslen(client->querybuf),
        (unsigned long long) sdsavail(client->querybuf),
        (unsigned long long) client->bufpos,
        (unsigned long long) listLength(client->reply),
        (unsigned long long) getClientOutputBufferMemoryUsage(client),
        events,
        client->lastcmd ? client->lastcmd->name : "NULL");
}

// 获取服务器所有client的信息
sds getAllClientsInfoString(void) {
    listNode *ln;
    listIter li;
    client *client;
    sds o = sdsnewlen(NULL,200*listLength(server.clients));
    sdsclear(o);
    // 设置链表迭代方向和起始节点
    listRewind(server.clients,&li);
    // 遍历服务器的所有client
    while ((ln = listNext(&li)) != NULL) {
        client = listNodeValue(ln);
        // 获取client的每种信息，并以sds形式保存
        o = catClientInfoString(o,client);
        // 每个client的信息之间用'\n'连接
        o = sdscatlen(o,"\n",1);
    }
    return o;
}

// client 命令的实现
void clientCommand(client *c) {
    listNode *ln;
    listIter li;
    client *client;

    //  CLIENT LIST 的实现
    if (!strcasecmp(c->argv[1]->ptr,"list") && c->argc == 2) {
        /* CLIENT LIST */
        // 获取所有的client信息
        sds o = getAllClientsInfoString();
        // 添加到到输入缓冲区中
        addReplyBulkCBuffer(c,o,sdslen(o));
        sdsfree(o);
    // CLIENT REPLY ON|OFF|SKIP 命令实现
    } else if (!strcasecmp(c->argv[1]->ptr,"reply") && c->argc == 3) {
        /* CLIENT REPLY ON|OFF|SKIP */
        // 如果是 ON
        if (!strcasecmp(c->argv[2]->ptr,"on")) {
            // 取消 off 和 skip 的标志
            c->flags &= ~(CLIENT_REPLY_SKIP|CLIENT_REPLY_OFF);
            // 回复 +OK
            addReply(c,shared.ok);
        // 如果是 OFF
        } else if (!strcasecmp(c->argv[2]->ptr,"off")) {
            // 打开 OFF标志
            c->flags |= CLIENT_REPLY_OFF;
        // 如果是 SKIP
        } else if (!strcasecmp(c->argv[2]->ptr,"skip")) {
            // 没有设置 OFF 则设置 SKIP 标志
            if (!(c->flags & CLIENT_REPLY_OFF))
                c->flags |= CLIENT_REPLY_SKIP_NEXT;
        } else {
            addReply(c,shared.syntaxerr);
            return;
        }
    //  CLIENT KILL [ip:port] [ID client-id] [TYPE normal | master | slave | pubsub] [ADDR ip:port] [SKIPME yes / no]
    } else if (!strcasecmp(c->argv[1]->ptr,"kill")) {
        /* CLIENT KILL <ip:port>
         * CLIENT KILL <option> [value] ... <option> [value] */
        char *addr = NULL;
        int type = -1;
        uint64_t id = 0;
        int skipme = 1;
        int killed = 0, close_this_client = 0;

        // CLIENT KILL addr:port只能通过地址杀死client，旧版本兼容
        if (c->argc == 3) {
            /* Old style syntax: CLIENT KILL <addr> */
            addr = c->argv[2]->ptr;
            skipme = 0; /* With the old form, you can kill yourself. */
        // 新版本可以根据[ID client-id] [master|normal|slave|pubsub] [ADDR ip:port] [SKIPME yes/no]杀死client
        } else if (c->argc > 3) {
            int i = 2; /* Next option index. */

            /* New style syntax: parse options. */
            // 解析语法
            while(i < c->argc) {
                int moreargs = c->argc > i+1;

                // CLIENT KILL [ID client-id]
                if (!strcasecmp(c->argv[i]->ptr,"id") && moreargs) {
                    long long tmp;
                    // 获取client的ID
                    if (getLongLongFromObjectOrReply(c,c->argv[i+1],&tmp,NULL)
                        != C_OK) return;
                    id = tmp;
                // CLIENT KILL TYPE type, 这里的 type 可以是 [master|normal|slave|pubsub]
                } else if (!strcasecmp(c->argv[i]->ptr,"type") && moreargs) {
                    // 获取client的类型，[master|normal|slave|pubsub]四种之一
                    type = getClientTypeByName(c->argv[i+1]->ptr);
                    if (type == -1) {
                        addReplyErrorFormat(c,"Unknown client type '%s'",
                            (char*) c->argv[i+1]->ptr);
                        return;
                    }
                // CLIENT KILL [ADDR ip:port]
                } else if (!strcasecmp(c->argv[i]->ptr,"addr") && moreargs) {
                    // 获取ip:port
                    addr = c->argv[i+1]->ptr;
                // CLIENT KILL [SKIPME yes/no]
                } else if (!strcasecmp(c->argv[i]->ptr,"skipme") && moreargs) {
                    // 如果是yes，设置设置skipme，调用该命令的客户端将不会被杀死
                    if (!strcasecmp(c->argv[i+1]->ptr,"yes")) {
                        skipme = 1;
                    // 设置为no会影响到还会杀死调用该命令的客户端。
                    } else if (!strcasecmp(c->argv[i+1]->ptr,"no")) {
                        skipme = 0;
                    } else {
                        addReply(c,shared.syntaxerr);
                        return;
                    }
                } else {
                    addReply(c,shared.syntaxerr);
                    return;
                }
                i += 2;
            }
        } else {
            addReply(c,shared.syntaxerr);
            return;
        }

        /* Iterate clients killing all the matching clients. */
        listRewind(server.clients,&li);
        // 迭代所有的client节点
        while ((ln = listNext(&li)) != NULL) {
            client = listNodeValue(ln);
            // 比较当前client和这四类信息，如果有一个不符合就跳过本层循环，否则就比较下一个信息
            if (addr && strcmp(getClientPeerId(client),addr) != 0) continue;
            if (type != -1 && getClientType(client) != type) continue;
            if (id != 0 && client->id != id) continue;
            if (c == client && skipme) continue;

            /* Kill it. */
            // 杀死当前的client
            if (c == client) {
                close_this_client = 1;
            } else {
                freeClient(client);
            }
            // 计算杀死client的个数
            killed++;
        }

        /* Reply according to old/new format. */
        // 回复client信息
        if (c->argc == 3) {
            // 没找到符合信息的
            if (killed == 0)
                addReplyError(c,"No such client");
            else
                addReply(c,shared.ok);
        } else {
            // 发送杀死的个数
            addReplyLongLong(c,killed);
        }

        /* If this client has to be closed, flag it as CLOSE_AFTER_REPLY
         * only after we queued the reply to its output buffers. */
        if (close_this_client) c->flags |= CLIENT_CLOSE_AFTER_REPLY;
    //  CLIENT SETNAME connection-name
    } else if (!strcasecmp(c->argv[1]->ptr,"setname") && c->argc == 3) {
        int j, len = sdslen(c->argv[2]->ptr);
        char *p = c->argv[2]->ptr;

        /* Setting the client name to an empty string actually removes
         * the current name. */
        // 设置名字为空
        if (len == 0) {
            // 先释放掉原来的名字
            if (c->name) decrRefCount(c->name);
            c->name = NULL;
            addReply(c,shared.ok);
            return;
        }

        /* Otherwise check if the charset is ok. We need to do this otherwise
         * CLIENT LIST format will break. You should always be able to
         * split by space to get the different fields. */
        // 检查名字格式是否正确
        for (j = 0; j < len; j++) {
            if (p[j] < '!' || p[j] > '~') { /* ASCII is assumed. */
                addReplyError(c,
                    "Client names cannot contain spaces, "
                    "newlines or special characters.");
                return;
            }
        }
        // 释放原来的名字
        if (c->name) decrRefCount(c->name);
        // 设置新名字
        c->name = c->argv[2];
        incrRefCount(c->name);
        addReply(c,shared.ok);
    //  CLIENT GETNAME
    } else if (!strcasecmp(c->argv[1]->ptr,"getname") && c->argc == 2) {
        // 回复名字
        if (c->name)
            addReplyBulk(c,c->name);
        else
            addReply(c,shared.nullbulk);
    //  CLIENT PAUSE timeout
    } else if (!strcasecmp(c->argv[1]->ptr,"pause") && c->argc == 3) {
        long long duration;

        // 以毫秒为单位将等待时间保存在duration中
        if (getTimeoutFromObjectOrReply(c,c->argv[2],&duration,UNIT_MILLISECONDS)
                                        != C_OK) return;
        // 暂停client
        pauseClients(duration);
        addReply(c,shared.ok);
    } else {
        addReplyError(c, "Syntax error, try CLIENT (LIST | KILL | GETNAME | SETNAME | PAUSE | REPLY)");
    }
}

/* This callback is bound to POST and "Host:" command names. Those are not
 * really commands, but are used in security attacks in order to talk to
 * Redis instances via HTTP, with a technique called "cross protocol scripting"
 * which exploits the fact that services like Redis will discard invalid
 * HTTP headers and will process what follows.
 *
 * As a protection against this attack, Redis will terminate the connection
 * when a POST or "Host:" header is seen, and will log the event from
 * time to time (to avoid creating a DOS as a result of too many logs). */
// 安全保护命令，用于避免DOS攻击的命令
void securityWarningCommand(client *c) {
    static time_t logged_time;
    time_t now = time(NULL);

    // 每间隔60秒，写一次日志
    if (labs(now-logged_time) > 60) {
        serverLog(LL_WARNING,"Possible SECURITY ATTACK detected. It looks like somebody is sending POST or Host: commands to Redis. This is likely due to an attacker attempting to use Cross Protocol Scripting to compromise your Redis instance. Connection aborted.");
        logged_time = now;
    }
    // 异步释放client
    freeClientAsync(c);
}

/* Rewrite the command vector of the client. All the new objects ref count
 * is incremented. The old command vector is freed, and the old objects
 * ref count is decremented. */
// 重写命令的参数列表
void rewriteClientCommandVector(client *c, int argc, ...) {
    va_list ap;
    int j;
    robj **argv; /* The new argument vector */

    // 分配空间
    argv = zmalloc(sizeof(robj*)*argc);
    va_start(ap,argc);
    // 遍历所有命令对象，写入新的参数列表
    for (j = 0; j < argc; j++) {
        robj *a;

        a = va_arg(ap, robj*);
        argv[j] = a;
        incrRefCount(a);
    }
    /* We free the objects in the original vector at the end, so we are
     * sure that if the same objects are reused in the new vector the
     * refcount gets incremented before it gets decremented. */
    // 释放原来的参数列表
    for (j = 0; j < c->argc; j++) decrRefCount(c->argv[j]);
    zfree(c->argv);
    /* Replace argv and argc with our new versions. */
    // 设置client的参数列表
    c->argv = argv;
    c->argc = argc;
    // 指向当前的命令
    c->cmd = lookupCommandOrOriginal(c->argv[0]->ptr);
    serverAssertWithInfo(c,NULL,c->cmd != NULL);
    va_end(ap);
}

/* Completely replace the client command vector with the provided one. */
// 用指定的值替代client的参数列表
void replaceClientCommandVector(client *c, int argc, robj **argv) {
    // 释放原来的
    freeClientArgv(c);
    zfree(c->argv);
    // 设置传入的值
    c->argv = argv;
    c->argc = argc;
    c->cmd = lookupCommandOrOriginal(c->argv[0]->ptr);
    serverAssertWithInfo(c,NULL,c->cmd != NULL);
}

/* Rewrite a single item in the command vector.
 * The new val ref count is incremented, and the old decremented.
 *
 * It is possible to specify an argument over the current size of the
 * argument vector: in this case the array of objects gets reallocated
 * and c->argc set to the max value. However it's up to the caller to
 *
 * 1. Make sure there are no "holes" and all the arguments are set.
 * 2. If the original argument vector was longer than the one we
 *    want to end with, it's up to the caller to set c->argc and
 *    free the no longer used objects on c->argv. */
// 修改当个参数
void rewriteClientCommandArgument(client *c, int i, robj *newval) {
    robj *oldval;

    // 追加了一个参数
    if (i >= c->argc) {
        c->argv = zrealloc(c->argv,sizeof(robj*)*(i+1));
        c->argc = i+1;
        c->argv[i] = NULL;
    }
    // 备份旧值，要释放
    oldval = c->argv[i];
    // 设置新值
    c->argv[i] = newval;
    incrRefCount(newval);
    if (oldval) decrRefCount(oldval);

    /* If this is the command name make sure to fix c->cmd. */
    // 如果设置是命令，则要更新client的cmd的指向
    if (i == 0) {
        c->cmd = lookupCommandOrOriginal(c->argv[0]->ptr);
        serverAssertWithInfo(c,NULL,c->cmd != NULL);
    }
}

/* This function returns the number of bytes that Redis is virtually
 * using to store the reply still not read by the client.
 * It is "virtual" since the reply output list may contain objects that
 * are shared and are not really using additional memory.
 *
 * The function returns the total sum of the length of all the objects
 * stored in the output list, plus the memory used to allocate every
 * list node. The static reply buffer is not taken into account since it
 * is allocated anyway.
 *
 * Note: this function is very fast so can be called as many time as
 * the caller wishes. The main usage of this function currently is
 * enforcing the client output length limits. */
// 返回输出缓冲区的大小，不包含静态的固定回复缓冲区，因为他总被分配
unsigned long getClientOutputBufferMemoryUsage(client *c) {
    // 回复链表节点和值对象的空间
    unsigned long list_item_size = sizeof(listNode)+sizeof(robj);

    // 所有的回复数据的字节数，加上保存这些数据的数据结构所使用的空间
    return c->reply_bytes + (list_item_size*listLength(c->reply));
}

/* Get the class of a client, used in order to enforce limits to different
 * classes of clients.
 *
 * The function will return one of the following:
 * CLIENT_TYPE_NORMAL -> Normal client
 * CLIENT_TYPE_SLAVE  -> Slave or client executing MONITOR command
 * CLIENT_TYPE_PUBSUB -> Client subscribed to Pub/Sub channels
 * CLIENT_TYPE_MASTER -> The client representing our replication master.
 */
// 获取client的类型，用于不同类型的client应用不同的限制
// CLIENT_TYPE_NORMAL 普通client
// CLIENT_TYPE_SLAVE 从节点或执行 MONITOR 命令的client
// CLIENT_TYPE_PUBSUB 进行订阅操作的client
// CLIENT_TYPE_MASTER 执行复制主节点服务器操作的client
int getClientType(client *c) {
    if (c->flags & CLIENT_MASTER) return CLIENT_TYPE_MASTER;
    if ((c->flags & CLIENT_SLAVE) && !(c->flags & CLIENT_MONITOR))
        return CLIENT_TYPE_SLAVE;
    if (c->flags & CLIENT_PUBSUB) return CLIENT_TYPE_PUBSUB;
    return CLIENT_TYPE_NORMAL;
}

// 根据类型name获取client类型
int getClientTypeByName(char *name) {
    if (!strcasecmp(name,"normal")) return CLIENT_TYPE_NORMAL;
    else if (!strcasecmp(name,"slave")) return CLIENT_TYPE_SLAVE;
    else if (!strcasecmp(name,"pubsub")) return CLIENT_TYPE_PUBSUB;
    else if (!strcasecmp(name,"master")) return CLIENT_TYPE_MASTER;
    else return -1;
}

// 根据type获取client的类型名字
char *getClientTypeName(int class) {
    switch(class) {
    case CLIENT_TYPE_NORMAL: return "normal";
    case CLIENT_TYPE_SLAVE:  return "slave";
    case CLIENT_TYPE_PUBSUB: return "pubsub";
    case CLIENT_TYPE_MASTER: return "master";
    default:                       return NULL;
    }
}

/* The function checks if the client reached output buffer soft or hard
 * limit, and also update the state needed to check the soft limit as
 * a side effect.
 *
 * Return value: non-zero if the client reached the soft or the hard limit.
 *               Otherwise zero is returned. */
// 检查client的输出缓冲区是否到达限制，到达限制则进行标记
int checkClientOutputBufferLimits(client *c) {
    int soft = 0, hard = 0, class;
    // 获取输出缓冲区的大小，不包含静态的固定回复缓冲区
    unsigned long used_mem = getClientOutputBufferMemoryUsage(c);

    // 获取client的类型
    class = getClientType(c);
    /* For the purpose of output buffer limiting, masters are handled
     * like normal clients. */
    // 将主节点服务器当做普通的类型的client看待
    if (class == CLIENT_TYPE_MASTER) class = CLIENT_TYPE_NORMAL;

    // 检查硬限制
    if (server.client_obuf_limits[class].hard_limit_bytes &&
        used_mem >= server.client_obuf_limits[class].hard_limit_bytes)
        hard = 1;
    // 检查软限制
    if (server.client_obuf_limits[class].soft_limit_bytes &&
        used_mem >= server.client_obuf_limits[class].soft_limit_bytes)
        soft = 1;

    /* We need to check if the soft limit is reached continuously for the
     * specified amount of seconds. */
    // 如果达到软限制
    if (soft) {
        // 第一次达到软限制，设置到达软限制的时间
        if (c->obuf_soft_limit_reached_time == 0) {
            c->obuf_soft_limit_reached_time = server.unixtime;
            soft = 0; /* First time we see the soft limit reached */
        // 之后到达软限制
        } else {
            // 计算软限制之间的时间
            time_t elapsed = server.unixtime - c->obuf_soft_limit_reached_time;

            // 小于设置的软限制时间，则取消标志，可以在规定的时间内暂时超过软限制
            if (elapsed <=
                server.client_obuf_limits[class].soft_limit_seconds) {
                soft = 0; /* The client still did not reached the max number of
                             seconds for the soft limit to be considered
                             reached. */
            }
        }
    // 没有到达软限制，则清空达到软限制的时间
    } else {
        c->obuf_soft_limit_reached_time = 0;
    }
    // 返回是否达到的限制
    return soft || hard;
}

/* Asynchronously close a client if soft or hard limit is reached on the
 * output buffer size. The caller can check if the client will be closed
 * checking if the client CLIENT_CLOSE_ASAP flag is set.
 *
 * Note: we need to close the client asynchronously because this function is
 * called from contexts where the client can't be freed safely, i.e. from the
 * lower level functions pushing data inside the client output buffers. */
// 如果client达到缓冲区的限制，则异步关闭client，防止底层函数正在向client的输出缓冲区写数据
void asyncCloseClientOnOutputBufferLimitReached(client *c) {
    serverAssert(c->reply_bytes < SIZE_MAX-(1024*64));
    // 已经设置 CLIENT_CLOSE_ASAP 标志则直接返回
    if (c->reply_bytes == 0 || c->flags & CLIENT_CLOSE_ASAP) return;
    // 检查client是否达到缓冲区的限制
    if (checkClientOutputBufferLimits(c)) {
        // 将client的信息保存到sds中
        sds client = catClientInfoString(sdsempty(),c);
        // 异步关闭client
        freeClientAsync(c);
        serverLog(LL_WARNING,"Client %s scheduled to be closed ASAP for overcoming of output buffer limits.", client);
        sdsfree(client);
    }
}

/* Helper function used by freeMemoryIfNeeded() in order to flush slaves
 * output buffers without returning control to the event loop.
 * This is also called by SHUTDOWN for a best-effort attempt to send
 * slaves the latest writes. */
// 不进入事件循环的情况下，刷新所有的输出缓冲区数据
void flushSlavesOutputBuffers(void) {
    listIter li;
    listNode *ln;

    listRewind(server.slaves,&li);
    // 遍历从节点链表
    while((ln = listNext(&li))) {
        client *slave = listNodeValue(ln);
        int events;

        /* Note that the following will not flush output buffers of slaves
         * in STATE_ONLINE but having put_online_on_ack set to true: in this
         * case the writable event is never installed, since the purpose
         * of put_online_on_ack is to postpone the moment it is installed.
         * This is what we want since slaves in this state should not receive
         * writes before the first ACK. */
        // 获取当前从节点的事件类型
        events = aeGetFileEvents(server.el,slave->fd);
        // 如果是写事件，当前从节点client的回复缓冲区中还有数据
        if (events & AE_WRITABLE &&
            slave->replstate == SLAVE_STATE_ONLINE &&
            clientHasPendingReplies(slave))
        {
            // 则将当前从节点client的缓冲区数据写到fd中
            writeToClient(slave->fd,slave,0);
        }
    }
}

/* Pause clients up to the specified unixtime (in ms). While clients
 * are paused no command is processed from clients, so the data set can't
 * change during that time.
 *
 * However while this function pauses normal and Pub/Sub clients, slaves are
 * still served, so this function can be used on server upgrades where it is
 * required that slaves process the latest bytes from the replication stream
 * before being turned to masters.
 *
 * This function is also internally used by Redis Cluster for the manual
 * failover procedure implemented by CLUSTER FAILOVER.
 *
 * The function always succeed, even if there is already a pause in progress.
 * In such a case, the pause is extended if the duration is more than the
 * time left for the previous duration. However if the duration is smaller
 * than the time left for the previous pause, no change is made to the
 * left duration. */
// 暂停client，使服务器在制定时间内停止接收client的命令
void pauseClients(mstime_t end) {
    // 如果client没处于暂停，则更新暂停到时的时间
    if (!server.clients_paused || end > server.clients_pause_end_time)
        server.clients_pause_end_time = end;
    // 设置暂停状态
    server.clients_paused = 1;
}

/* Return non-zero if clients are currently paused. As a side effect the
 * function checks if the pause time was reached and clear it. */
// 返回client是否处于暂停状态
int clientsArePaused(void) {
    if (server.clients_paused &&
        server.clients_pause_end_time < server.mstime)
    {
        listNode *ln;
        listIter li;
        client *c;

        server.clients_paused = 0;

        /* Put all the clients in the unblocked clients queue in order to
         * force the re-processing of the input buffer if any. */
        listRewind(server.clients,&li);
        // 遍历所有的client链表节点
        while ((ln = listNext(&li)) != NULL) {
            c = listNodeValue(ln);

            /* Don't touch slaves and blocked clients. The latter pending
             * requests be processed when unblocked. */
            // 如果当前client整被阻塞或是从节点client，则跳过当前节点
            if (c->flags & (CLIENT_SLAVE|CLIENT_BLOCKED)) continue;
            // 解除client的阻塞
            c->flags |= CLIENT_UNBLOCKED;
            // 加入非阻塞client的链表中
            listAddNodeTail(server.unblocked_clients,c);
        }
    }
    return server.clients_paused;
}

/* This function is called by Redis in order to process a few events from
 * time to time while blocked into some not interruptible operation.
 * This allows to reply to clients with the -LOADING error while loading the
 * data set at startup or after a full resynchronization with the master
 * and so forth.
 *
 * It calls the event loop in order to process a few events. Specifically we
 * try to call the event loop 4 times as long as we receive acknowledge that
 * some event was processed, in order to go forward with the accept, read,
 * write, close sequence needed to serve a client.
 *
 * The function returns the total number of events processed. */
// 让服务器在非阻塞状态下，处理部分文件事件
int processEventsWhileBlocked(void) {
    int iterations = 4; /* See the function top-comment. */
    int count = 0;
    // 具体来说，我们尝试将事件循环调用4次，只要我们收到确认某些事件被处理，以便继续接受，读取，写入，关闭序列来为客户端提供服务。
    while (iterations--) {
        int events = 0;
        // 处理发生的文件事件，且非阻塞
        events += aeProcessEvents(server.el, AE_FILE_EVENTS|AE_DONT_WAIT);
        // 处理放在clients_pending_write链表中的待写的client
        events += handleClientsWithPendingWrites();
        if (!events) break;
        count += events;
    }
    // 返回处理的事件数
    return count;
}
