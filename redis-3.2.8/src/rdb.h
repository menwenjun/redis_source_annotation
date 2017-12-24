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

#ifndef __RDB_H
#define __RDB_H

#include <stdio.h>
#include "rio.h"

/* TBD: include only necessary headers. */
#include "server.h"

/* The current RDB version. When the format changes in a way that is no longer
 * backward compatible this number gets incremented. */
#define RDB_VERSION 7   //RDB的版本

/* Defines related to the dump file format. To store 32 bits lengths for short
 * keys requires a lot of space, so we check the most significant 2 bits of
 * the first byte to interpreter the length:
 *
 * 00|000000 => if the two MSB are 00 the len is the 6 bits of this byte
 * 01|000000 00000000 =>  01, the len is 14 byes, 6 bits + 8 bits of next byte
 * 10|000000 [32 bit integer] => if it's 01, a full 32 bit len will follow
 * 11|000000 this means: specially encoded object will follow. The six bits
 *           number specify the kind of object that follows.
 *           See the RDB_ENC_* defines.
 *
 * Lengths up to 63 are stored using a single byte, most DB keys, and may
 * values, will fit inside. */
#define RDB_6BITLEN 0           //6位长
#define RDB_14BITLEN 1          //14位长
#define RDB_32BITLEN 2          //32位长
#define RDB_ENCVAL 3            //编码值
#define RDB_LENERR UINT_MAX     //错误值

/* When a length of a string object stored on disk has the first two bits
 * set, the remaining two bits specify a special encoding for the object
 * accordingly to the following defines: */
#define RDB_ENC_INT8 0        /* 8位有符号整数 8 bit signed integer */
#define RDB_ENC_INT16 1       /* 16位有符号整数 16 bit signed integer */
#define RDB_ENC_INT32 2       /* 32位有符号整数 32 bit signed integer */
#define RDB_ENC_LZF 3         /* LZF压缩过的字符串 string compressed with FASTLZ */

/* Dup object types to RDB object types. Only reason is readability (are we
 * dealing with RDB types or with in-memory object types?). */
#define RDB_TYPE_STRING 0           //字符串类型
#define RDB_TYPE_LIST   1           //列表类型
#define RDB_TYPE_SET    2           //集合类型
#define RDB_TYPE_ZSET   3           //有序集合类型
#define RDB_TYPE_HASH   4           //哈希类型
/* NOTE: WHEN ADDING NEW RDB TYPE, UPDATE rdbIsObjectType() BELOW */

/* Object types for encoded objects. */
#define RDB_TYPE_HASH_ZIPMAP    9
#define RDB_TYPE_LIST_ZIPLIST  10   //列表对象的ziplist编码类型
#define RDB_TYPE_SET_INTSET    11   //集合对象的intset编码类型
#define RDB_TYPE_ZSET_ZIPLIST  12   //有序集合的ziplist编码类型
#define RDB_TYPE_HASH_ZIPLIST  13   //哈希对象的ziplist编码类型
#define RDB_TYPE_LIST_QUICKLIST 14  //列表对象的quicklist编码类型
/* NOTE: WHEN ADDING NEW RDB TYPE, UPDATE rdbIsObjectType() BELOW */

/* Test if a type is an object type. */
// 测试t是否是一个对象的编码类型
#define rdbIsObjectType(t) ((t >= 0 && t <= 4) || (t >= 9 && t <= 14))

/* Special RDB opcodes (saved/loaded with rdbSaveType/rdbLoadType). */
#define RDB_OPCODE_AUX        250       //辅助标识
#define RDB_OPCODE_RESIZEDB   251       //提示调整哈希表大小的操作码
#define RDB_OPCODE_EXPIRETIME_MS 252    //过期时间毫秒
#define RDB_OPCODE_EXPIRETIME 253       //过期时间秒
#define RDB_OPCODE_SELECTDB   254       //选择数据库的操作
#define RDB_OPCODE_EOF        255       //EOF码

// 将长度为1的type字符写到rdb中
int rdbSaveType(rio *rdb, unsigned char type);
// 从rdb中载入1字节的数据保存在type中，并返回其type
int rdbLoadType(rio *rdb);
//
int rdbSaveTime(rio *rdb, time_t t);
// 从rio读出一个时间，单位为秒，长度为4字节
time_t rdbLoadTime(rio *rdb);
// 将一个被编码的长度写入到rio中，返回保存编码后的len需要的字节数
int rdbSaveLen(rio *rdb, uint32_t len);
// 返回一个从rio读出的len值，如果该len值不是整数，而是被编码后的值，那么将isencoded设置为1
uint32_t rdbLoadLen(rio *rdb, int *isencoded);
// 将对象o的类型写到rio中
int rdbSaveObjectType(rio *rdb, robj *o);
// 从rio中读出一个类型并返回
int rdbLoadObjectType(rio *rdb);
// 将指定的RDB文件读到数据库中
int rdbLoad(char *filename);
// 后台进行RDB持久化BGSAVE操作
int rdbSaveBackground(char *filename);
// fork一个子进程将rdb写到状态为等待BGSAVE开始的从节点的socket中
int rdbSaveToSlavesSockets(void);
// 删除临时文件，当BGSAVE执行被中断时使用
void rdbRemoveTempFile(pid_t childpid);
// 将数据库保存在磁盘上，返回C_OK成功，否则返回C_ERR
int rdbSave(char *filename);
// 将一个对象写到rio中，出错返回-1，成功返回写的字节数
ssize_t rdbSaveObject(rio *rdb, robj *o);
// 返回一个对象的长度，通过写入的方式
size_t rdbSavedObjectLen(robj *o);
// 从rio中读出一个rdbtype类型的对象，成功返回新对象地址，否则返回NULL
robj *rdbLoadObject(int type, rio *rdb);
// 当BGSAVE 完成RDB文件，要么发送给从节点，要么保存到磁盘，调用正确的处理
void backgroundSaveDoneHandler(int exitcode, int bysignal);
// 将一个键对象，值对象，过期时间，和类型写入到rio中，出错返回-1，成功返回1，键过期返回0
int rdbSaveKeyValuePair(rio *rdb, robj *key, robj *val, long long expiretime, long long now);
// 从rio中读出一个字符串编码的对象
robj *rdbLoadStringObject(rio *rdb);

#endif
