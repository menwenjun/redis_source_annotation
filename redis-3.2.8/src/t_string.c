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
#include <math.h> /* isnan(), isinf() */

/*-----------------------------------------------------------------------------
 * String Commands
 *----------------------------------------------------------------------------*/
//检查字符串的长度是都超过512MB
static int checkStringLength(client *c, long long size) {
    if (size > 512*1024*1024) { //512MB
        addReplyError(c,"string exceeds maximum allowed size (512MB)");
        return C_ERR;   //超过则返回-1
    }
    return C_OK;
}

/* The setGenericCommand() function implements the SET operation with different
 * options and variants. This function is called in order to implement the
 * following commands: SET, SETEX, PSETEX, SETNX.
 *
 * 'flags' changes the behavior of the command (NX or XX, see belove).
 *
 * 'expire' represents an expire to set in form of a Redis object as passed
 * by the user. It is interpreted according to the specified 'unit'.
 *
 * 'ok_reply' and 'abort_reply' is what the function will reply to the client
 * if the operation is performed, or when it is not because of NX or
 * XX flags.
 *
 * If ok_reply is NULL "+OK" is used.
 * If abort_reply is NULL, "$-1" is used. */

#define OBJ_SET_NO_FLAGS 0
#define OBJ_SET_NX (1<<0)     /* Set if key not exists. */          //在key不存在的情况下才会设置
#define OBJ_SET_XX (1<<1)     /* Set if key exists. */              //在key存在的情况下才会设置
#define OBJ_SET_EX (1<<2)     /* Set if time in seconds is given */ //以秒(s)为单位设置键的key过期时间
#define OBJ_SET_PX (1<<3)     /* Set if time in ms in given */      //以毫秒(ms)为单位设置键的key过期时间

//setGenericCommand()函数是以下命令: SET, SETEX, PSETEX, SETNX.的最底层实现
//flags 可以是NX或XX，由上面的宏提供
//expire 定义key的过期时间，格式由unit指定
//ok_reply和abort_reply保存着回复client的内容，NX和XX也会改变回复
//如果ok_reply为空，则使用 "+OK"
//如果abort_reply为空，则使用 "$-1"
void setGenericCommand(client *c, int flags, robj *key, robj *val, robj *expire, int unit, robj *ok_reply, robj *abort_reply) {
    long long milliseconds = 0; /* initialized to avoid any harmness warning */ //初始化，避免错误

    //如果定义了key的过期时间
    if (expire) {
        //从expire对象中取出值，保存在milliseconds中，如果出错发送默认的信息给client
        if (getLongLongFromObjectOrReply(c, expire, &milliseconds, NULL) != C_OK)
            return;
        // 如果过期时间小于等于0，则发送错误信息给client
        if (milliseconds <= 0) {
            addReplyErrorFormat(c,"invalid expire time in %s",c->cmd->name);
            return;
        }
        //如果unit的单位是秒，则需要转换为毫秒保存
        if (unit == UNIT_SECONDS) milliseconds *= 1000;
    }

    //lookupKeyWrite函数是为执行写操作而取出key的值对象
    //如果设置了NX(不存在)，并且在数据库中 找到 该key，或者
    //设置了XX(存在)，并且在数据库中 没有找到 该key
    //回复abort_reply给client
    if ((flags & OBJ_SET_NX && lookupKeyWrite(c->db,key) != NULL) ||
        (flags & OBJ_SET_XX && lookupKeyWrite(c->db,key) == NULL))
    {
        addReply(c, abort_reply ? abort_reply : shared.nullbulk);
        return;
    }
    //在当前db设置键为key的值为val
    setKey(c->db,key,val);

    //设置数据库为脏(dirty)，服务器每次修改一个key后，都会对脏键(dirty)增1
    server.dirty++;

    //设置key的过期时间
    //mstime()返回毫秒为单位的格林威治时间
    if (expire) setExpire(c->db,key,mstime()+milliseconds);

    //发送"set"事件的通知，用于发布订阅模式，通知客户端接受发生的事件
    notifyKeyspaceEvent(NOTIFY_STRING,"set",key,c->db->id);

    //发送"expire"事件通知
    if (expire) notifyKeyspaceEvent(NOTIFY_GENERIC,
        "expire",key,c->db->id);

    //设置成功，则向客户端发送ok_reply
    addReply(c, ok_reply ? ok_reply : shared.ok);
}

/* SET key value [NX] [XX] [EX <seconds>] [PX <milliseconds>] */
//SET命令
void setCommand(client *c) {
    int j;
    robj *expire = NULL;
    int unit = UNIT_SECONDS;        //单位为秒
    int flags = OBJ_SET_NO_FLAGS;   //初始化为0，表示默认为没有后面的[NX] [XX] [EX] [PX]参数

    //从第四个参数开始解析，
    for (j = 3; j < c->argc; j++) {
        char *a = c->argv[j]->ptr;  //保存第四个参数的首地址
        //EX和PX 参数后要 一个时间参数，next保存时间参数的地址
        robj *next = (j == c->argc-1) ? NULL : c->argv[j+1];

        //如果是 "nx" 或 "NX" 并且 flags没有设置 "XX" 的标志位
        if ((a[0] == 'n' || a[0] == 'N') &&
            (a[1] == 'x' || a[1] == 'X') && a[2] == '\0' &&
            !(flags & OBJ_SET_XX))
        {
            flags |= OBJ_SET_NX;    //设置 "NX" 标志位

        //如果是 "xx" 或 "XX" 并且 flags没有设置 "NX" 的标志位
        } else if ((a[0] == 'x' || a[0] == 'X') &&
                   (a[1] == 'x' || a[1] == 'X') && a[2] == '\0' &&
                   !(flags & OBJ_SET_NX))
        {
            flags |= OBJ_SET_XX;    //设置 "XX" 标志位

        //如果是 "ex" 或 "EX" 并且 flags没有设置 "PX" 的标志位，并且后面跟了时间
        } else if ((a[0] == 'e' || a[0] == 'E') &&
                   (a[1] == 'x' || a[1] == 'X') && a[2] == '\0' &&
                   !(flags & OBJ_SET_PX) && next)
        {
            flags |= OBJ_SET_EX;    //设置 "EX" 标志位
            unit = UNIT_SECONDS;    //EX 单位为秒
            expire = next;          //保存时间值
            j++;                    //跳过时间参数的下标

        //如果是 "px" 或 "PX" 并且 flags没有设置 "EX" 的标志位，并且后面跟了时间
        } else if ((a[0] == 'p' || a[0] == 'P') &&
                   (a[1] == 'x' || a[1] == 'X') && a[2] == '\0' &&
                   !(flags & OBJ_SET_EX) && next)
        {
            flags |= OBJ_SET_PX;    //设置 "PX" 标志位
            unit = UNIT_MILLISECONDS;//PX 单位为毫秒
            expire = next;           //保存时间值
            j++;                    //跳过时间参数的下标
        } else {
            //不是以上格式则回复client语法错误
            addReply(c,shared.syntaxerr);
            return;
        }
    }

    //对value进行最优的编码
    c->argv[2] = tryObjectEncoding(c->argv[2]);

    //调用底层的setGenericCommand函数实现SET命令
    setGenericCommand(c,flags,c->argv[1],c->argv[2],expire,unit,NULL,NULL);
}

// SETNX 命令实现
void setnxCommand(client *c) {
    c->argv[2] = tryObjectEncoding(c->argv[2]);
    setGenericCommand(c,OBJ_SET_NX,c->argv[1],c->argv[2],NULL,0,shared.cone,shared.czero);
}

// SETEX 命令实现
void setexCommand(client *c) {
    c->argv[3] = tryObjectEncoding(c->argv[3]);
    setGenericCommand(c,OBJ_SET_NO_FLAGS,c->argv[1],c->argv[3],c->argv[2],UNIT_SECONDS,NULL,NULL);
}

// PSETEX 命令实现
void psetexCommand(client *c) {
    c->argv[3] = tryObjectEncoding(c->argv[3]);
    setGenericCommand(c,OBJ_SET_NO_FLAGS,c->argv[1],c->argv[3],c->argv[2],UNIT_MILLISECONDS,NULL,NULL);
}

//GET 命令的底层实现
int getGenericCommand(client *c) {
    robj *o;

    //lookupKeyReadOrReply函数是为执行读操作而返回key的值对象，找到返回该对象，找不到会发送信息给client
    //如果key不存在直接，返回0表示GET命令执行成功
    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.nullbulk)) == NULL)
        return C_OK;

    //如果key的值的编码类型不是字符串对象
    if (o->type != OBJ_STRING) {
        addReply(c,shared.wrongtypeerr);    //返回类型错误的信息给client，返回-1表示GET命令执行失败
        return C_ERR;
    } else {
        addReplyBulk(c,o);  //返回之前找到的对象作为回复给client，返回0表示GET命令执行成功
        return C_OK;
    }
}

//调用getGenericCommand实现GET命令
void getCommand(client *c) {
    getGenericCommand(c);
}

//
//GETSET 命令的实现
void getsetCommand(client *c) {
    //先用GET命令得到val对象返回发给client
    if (getGenericCommand(c) == C_ERR) return;

    //在对新的val进行优化编码
    c->argv[2] = tryObjectEncoding(c->argv[2]);

    //设置key的值为新val
    setKey(c->db,c->argv[1],c->argv[2]);

    //发送一个"set"事件通知
    notifyKeyspaceEvent(NOTIFY_STRING,"set",c->argv[1],c->db->id);

    //将脏键加1
    server.dirty++;
}

//SETRANGE key offset value
//SETRANGE 命令的实现
void setrangeCommand(client *c) {
    robj *o;
    long offset;
    sds value = c->argv[3]->ptr;    //获得要设置的value值

    //从offset对象中取出long类型的值，保存在offset中，不成功直接发送错误信息给client
    if (getLongFromObjectOrReply(c,c->argv[2],&offset,NULL) != C_OK)
        return;

    //如果偏移量小于0，则回复错误信息给client
    if (offset < 0) {
        addReplyError(c,"offset is out of range");
        return;
    }

    //以写操作的取出key的值对象保存在o中
    o = lookupKeyWrite(c->db,c->argv[1]);

    //key没有找到
    if (o == NULL) {
        /* Return 0 when setting nothing on a non-existing string */
        //且当value为空，返回0，什么也不做
        if (sdslen(value) == 0) {
            addReply(c,shared.czero);
            return;
        }

        /* Return when the resulting string exceeds allowed size */
        //检查字符串长度是否超过512MB的设置，超过会给client发送一个错误信息
        if (checkStringLength(c,offset+sdslen(value)) != C_OK)
            return;

        //如果key在当前数据库中不存在，且命令中制定了value，且value值的长度没有超过redis的限制
        //创建一个新的key字符串对象
        o = createObject(OBJ_STRING,sdsnewlen(NULL, offset+sdslen(value)));

        //设置新键key的value
        dbAdd(c->db,c->argv[1],o);
    } else {    //key已经存在于数据库
        size_t olen;

        /* Key exists, check type */
        //检查value对象是否是字符串类型的对象，是返回0，不是返回1且发送错误信息
        if (checkType(c,o,OBJ_STRING))
            return;

        /* Return existing string length when setting nothing */
        //返回value的长度
        olen = stringObjectLen(o);

        //value长度为0，发送0给client
        if (sdslen(value) == 0) {
            addReplyLongLong(c,olen);
            return;
        }

        /* Return when the resulting string exceeds allowed size */
        //检查字符串长度是否超过512MB的设置，超过会给client发送一个错误信息
        if (checkStringLength(c,offset+sdslen(value)) != C_OK)
            return;

        /* Create a copy when the object is shared or encoded. */
        //因为要根据value修改key的值，因此如果key原来的值是共享的，需要解除共享，新创建一个值对象与key组对
        o = dbUnshareStringValue(c->db,c->argv[1],o);
    }

    //如果value对象的长度大于0
    if (sdslen(value) > 0) {
        //将值对象扩展offset+sdslen(value)的长度，扩充的内存使用"\0"填充
        o->ptr = sdsgrowzero(o->ptr,offset+sdslen(value));

        //将value填入到偏移量起始的地址上
        memcpy((char*)o->ptr+offset,value,sdslen(value));

        //当数据库的键被改动，则会调用该函数发送信号
        signalModifiedKey(c->db,c->argv[1]);

        //发送"setrange"时间通知
        notifyKeyspaceEvent(NOTIFY_STRING,
            "setrange",c->argv[1],c->db->id);

        //将脏键加1
        server.dirty++;
    }
    addReplyLongLong(c,sdslen(o->ptr)); //发送新的vlaue值给client
}

//GETRANGE key start end
//GETRANGE 命令的实现
void getrangeCommand(client *c) {
    robj *o;
    long long start, end;
    char *str, llbuf[32];
    size_t strlen;

    //将起始下标以long long 类型保存到start中
    if (getLongLongFromObjectOrReply(c,c->argv[2],&start,NULL) != C_OK)
        return;
    //将起始下标以long long 类型保存到end中
    if (getLongLongFromObjectOrReply(c,c->argv[3],&end,NULL) != C_OK)
        return;
    //以读操作的取出key的值对象返回到o中
    //如果value对象不是字符串对象或key对象不存在，直接返回
    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.emptybulk)) == NULL ||
        checkType(c,o,OBJ_STRING)) return;

    //如果value对象的编码为整型类型
    if (o->encoding == OBJ_ENCODING_INT) {
        str = llbuf;
        strlen = ll2string(llbuf,sizeof(llbuf),(long)o->ptr);   //将整型转换为字符串型，并保存字符串长度
    } else {
        str = o->ptr;
        strlen = sdslen(str);   //保存字符串长度
    }

    /* Convert negative indexes */
    //参数范围出错，发送错误信息
    if (start < 0 && end < 0 && start > end) {
        addReply(c,shared.emptybulk);
        return;
    }
    //将负数下标转换为正数形式
    if (start < 0) start = strlen+start;
    if (end < 0) end = strlen+end;

    //如果转换后的下标仍为负数，则设置为0
    if (start < 0) start = 0;
    if (end < 0) end = 0;
    if ((unsigned long long)end >= strlen) end = strlen-1;  //end不能超过字符串长度

    /* Precondition: end >= 0 && end < strlen, so the only condition where
     * nothing can be returned is: start > end. */
    if (start > end || strlen == 0) {
        addReply(c,shared.emptybulk);   //参数范围出错，发送错误信息
    } else {
        addReplyBulkCBuffer(c,(char*)str+start,end-start+1);    //发送给定范围内的内容给client
    }
}

// MGET key [key ...]
// MGET 命令的实现
void mgetCommand(client *c) {
    int j;

    //发送key的个数给client
    addReplyMultiBulkLen(c,c->argc-1);

    //从下标为1的key，遍历argc-1次
    for (j = 1; j < c->argc; j++) {
        robj *o = lookupKeyRead(c->db,c->argv[j]);  //以读操作取出key的value对象
        if (o == NULL) {
            addReply(c,shared.nullbulk);    //没找到当前的key的value，发送空信息
        } else {
            if (o->type != OBJ_STRING) {    //找到的value对象不是字符串类型的对象
                addReply(c,shared.nullbulk);//发送空信息
            } else {
                addReplyBulk(c,o);          //如果是字符串类型的对象，则发送value对象给client
            }
        }
    }
}

// MSET key value [key value ...]
// MSET 的底层实现
void msetGenericCommand(client *c, int nx) {
    int j, busykeys = 0;

    //如果参数个数不是奇数个，则发送错误信息给client
    if ((c->argc % 2) == 0) {
        addReplyError(c,"wrong number of arguments for MSET");
        return;
    }
    /* Handle the NX flag. The MSETNX semantic is to return zero and don't
     * set nothing at all if at least one already key exists. */
    //如果制定NX标志
    if (nx) {
        for (j = 1; j < c->argc; j += 2) {  //遍历所有的key对象
            //以写操作取出key的value对象，查找所有的key对象是否存在，并计数存在的key对象
            if (lookupKeyWrite(c->db,c->argv[j]) != NULL) {
                busykeys++;
            }
        }
        //只要有一个存在的key就放弃所有key的操作，发送0给client，之久返回
        if (busykeys) {
            addReply(c, shared.czero);
            return;
        }
    }

    //没有制定NX标志，遍历所有的key对象
    for (j = 1; j < c->argc; j += 2) {
        c->argv[j+1] = tryObjectEncoding(c->argv[j+1]);         //对value对象优化编码
        setKey(c->db,c->argv[j],c->argv[j+1]);                  //设置key的值为value
        notifyKeyspaceEvent(NOTIFY_STRING,"set",c->argv[j],c->db->id);  //发送"set"事件通知
    }
    //更新脏键
    server.dirty += (c->argc-1)/2;

    //MSETNX返回1，MSET返回ok
    addReply(c, nx ? shared.cone : shared.ok);
}

//MSET命令的实现
void msetCommand(client *c) {
    msetGenericCommand(c,0);
}

//MSETNX命令的实现
void msetnxCommand(client *c) {
    msetGenericCommand(c,1);
}

// DECR key
// INCR key
//INCR和DECR命令的底层实现
void incrDecrCommand(client *c, long long incr) {
    long long value, oldvalue;
    robj *o, *new;

    o = lookupKeyWrite(c->db,c->argv[1]);   //以写操作获取key的value对象

    //找到了value对象但是value对象不是字符串类型，直接返回
    if (o != NULL && checkType(c,o,OBJ_STRING)) return;

    //将字符串类型的value转换为longlong类型保存在value中
    if (getLongLongFromObjectOrReply(c,o,&value,NULL) != C_OK) return;

    oldvalue = value;   //备份旧的value

    //如果incr超出longlong类型所能表示的范围，发送错误信息
    if ((incr < 0 && oldvalue < 0 && incr < (LLONG_MIN-oldvalue)) ||
        (incr > 0 && oldvalue > 0 && incr > (LLONG_MAX-oldvalue))) {
        addReplyError(c,"increment or decrement would overflow");
        return;
    }
    value += incr;  //计算新的value值

    //value对象目前非共享，编码为整型类型，且新value值不在共享范围，且value处于long类型所表示的范围内
    if (o && o->refcount == 1 && o->encoding == OBJ_ENCODING_INT &&
        (value < 0 || value >= OBJ_SHARED_INTEGERS) &&
        value >= LONG_MIN && value <= LONG_MAX)
    {
        new = o;
        o->ptr = (void*)((long)value);  //设置vlaue对象的值
    } else {
        //当不满足以上任意条件，则新创建一个字符串对象
        new = createStringObjectFromLongLong(value);

        //如果之前的value对象存在
        if (o) {
            dbOverwrite(c->db,c->argv[1],new);  //用new对象去重写key的值
        } else {
            dbAdd(c->db,c->argv[1],new);        //如果之前的value不存在，将key和new组成新的key-value对
        }
    }
    signalModifiedKey(c->db,c->argv[1]);    //当数据库的键被改动，则会调用该函数发送信号
    //发送"incrby"事件通知
    notifyKeyspaceEvent(NOTIFY_STRING,"incrby",c->argv[1],c->db->id);
    //设置脏键
    server.dirty++;

    //回复信息给client
    addReply(c,shared.colon);
    addReply(c,new);
    addReply(c,shared.crlf);
}


//INCR命令实现
void incrCommand(client *c) {
    incrDecrCommand(c,1);
}

//DECR命令实现
void decrCommand(client *c) {
    incrDecrCommand(c,-1);
}

//INCRBY key decrement
//INCRBY命令实现
void incrbyCommand(client *c) {
    long long incr;

    //将decrement对象的值转换为long long 类型保存在incr中
    if (getLongLongFromObjectOrReply(c, c->argv[2], &incr, NULL) != C_OK) return;
    incrDecrCommand(c,incr);
}

//DECRBY key decrement
//DECRBY命令实现
void decrbyCommand(client *c) {
    long long incr;

    //将decrement对象的值转换为long long 类型保存在incr中
    if (getLongLongFromObjectOrReply(c, c->argv[2], &incr, NULL) != C_OK) return;
    incrDecrCommand(c,-incr);   //取incr的相反数
}

// INCRBYFLOAT key increment
// INCRBYFLOAT命令的实现
void incrbyfloatCommand(client *c) {
    long double incr, value;
    robj *o, *new, *aux;

    o = lookupKeyWrite(c->db,c->argv[1]);   //以写操作获取key的value对象

    //找到了value对象但是value对象不是字符串类型，直接返回
    if (o != NULL && checkType(c,o,OBJ_STRING)) return;

    //将key的值转换为long double类型保存在value中
    //将increment对象的值保存在incr中
    //两者任一不成功，则直接返回并且发送错误信息
    if (getLongDoubleFromObjectOrReply(c,o,&value,NULL) != C_OK ||
        getLongDoubleFromObjectOrReply(c,c->argv[2],&incr,NULL) != C_OK)
        return;

    //计算出新的value值
    value += incr;
    //如果value不是数字或者value是正无穷或负无穷，发送错误信息
    if (isnan(value) || isinf(value)) {
        addReplyError(c,"increment would produce NaN or Infinity");
        return;
    }

    //创建一个字符串类型的value对象
    new = createStringObjectFromLongDouble(value,1);

    //如果之前的value对象存在
    if (o)
        dbOverwrite(c->db,c->argv[1],new);  //用new对象去重写key的值
    else
        dbAdd(c->db,c->argv[1],new);    //如果之前的value不存在，将key和new组成新的key-value对

    signalModifiedKey(c->db,c->argv[1]);    //当数据库的键被改动，则会调用该函数发送信号

    //发送"incrbyfloat"事件通知
    notifyKeyspaceEvent(NOTIFY_STRING,"incrbyfloat",c->argv[1],c->db->id);
    //设置脏键
    server.dirty++;
    //将新的value对象返回给client
    addReplyBulk(c,new);

    /* Always replicate INCRBYFLOAT as a SET command with the final value
     * in order to make sure that differences in float precision or formatting
     * will not create differences in replicas or after an AOF restart. */
    //防止因为不同的浮点精度和格式化造成AOF重启时的不一致
    //创建一个值为"SET"的字符串对象
    aux = createStringObject("SET",3);

    //将client中的"INCRBYFLOAT"替换为"SET"命令
    rewriteClientCommandArgument(c,0,aux);

    //释放aux对象
    decrRefCount(aux);

    //将client中的"increment"参数替换为new对象中的值
    rewriteClientCommandArgument(c,2,new);
}

// APPEND key value
// APPEND命令的实现
void appendCommand(client *c) {
    size_t totlen;
    robj *o, *append;

    o = lookupKeyWrite(c->db,c->argv[1]);   //以写操作获取key的value对象

    //如果没有获取到vlaue，则要创建一个
    if (o == NULL) {
        /* Create the key */
        c->argv[2] = tryObjectEncoding(c->argv[2]); //对参数value进行优化编码
        dbAdd(c->db,c->argv[1],c->argv[2]); //将key和value组成新的key-value对
        incrRefCount(c->argv[2]);           //增加value的引用计数
        totlen = stringObjectLen(c->argv[2]);   //返回vlaue的长度
    } else {    //获取到value
        /* Key exists, check type */
        if (checkType(c,o,OBJ_STRING))  //如果value不是字符串类型的对象直接返回
            return;

        /* "append" is an argument, so always an sds */
        //获得追加的值对象
        append = c->argv[2];
        //计算追加后的长度
        totlen = stringObjectLen(o)+sdslen(append->ptr);
        //如果追加后的长度超出范围，则返回
        if (checkStringLength(c,totlen) != C_OK)
            return;

        /* Append the value */
        //因为要根据value修改key的值，因此如果key原来的值是共享的，需要解除共享，新创建一个值对象与key组对
        o = dbUnshareStringValue(c->db,c->argv[1],o);
        //将vlaue对象的值后面追加上append的值
        o->ptr = sdscatlen(o->ptr,append->ptr,sdslen(append->ptr));
        //计算出追加后值的长度
        totlen = sdslen(o->ptr);
    }
    signalModifiedKey(c->db,c->argv[1]);//当数据库的键被改动，则会调用该函数发送信号
    //发送"append"事件通知
    notifyKeyspaceEvent(NOTIFY_STRING,"append",c->argv[1],c->db->id);
    //设置脏键
    server.dirty++;
    //发送追加后value的长度给client
    addReplyLongLong(c,totlen);
}

// STRLEN key
// STRLEN命令的实现
void strlenCommand(client *c) {
    robj *o;
    //以读操作取出key对象的value对象，并且检查value是否是字符串对象
    //如果没找到value或者不是字符串对象则直接返回
    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.czero)) == NULL ||
        checkType(c,o,OBJ_STRING)) return;

    //发送字符串的长度给client
    addReplyLongLong(c,stringObjectLen(o));
}
