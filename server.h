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

#ifndef __REDIS_H
#define __REDIS_H

#include "fmacros.h"
#include "config.h"
#include "solarisfixes.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <limits.h>
#include <unistd.h>
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <syslog.h>
#include <netinet/in.h>
#include <lua.h>
#include <signal.h>

typedef long long mstime_t; /* 毫秒时间类型millisecond time type. */

#include "ae.h"      /* Event driven programming library */
#include "sds.h"     /* Dynamic safe strings */
#include "dict.h"    /* Hash tables */
#include "adlist.h"  /* Linked lists */
#include "zmalloc.h" /* total memory usage aware version of malloc/free */
#include "anet.h"    /* Networking the easy way */
#include "ziplist.h" /* Compact list data structure */
#include "intset.h"  /* Compact integer set structure */
#include "version.h" /* Version macro */
#include "util.h"    /* Misc functions useful in many places */
#include "latency.h" /* Latency monitor API */
#include "sparkline.h" /* ASCII graphs API */
#include "quicklist.h"

/* Following includes allow test functions to be called from Redis main() */
#include "zipmap.h"
#include "sha1.h"
#include "endianconv.h"
#include "crc64.h"

/* Error codes */
#define C_OK                    0
#define C_ERR                   -1

/* Static server configuration */
// 默认的频率：每10秒执行一次
#define CONFIG_DEFAULT_HZ        10      /* Time interrupt calls/sec. */
#define CONFIG_MIN_HZ            1
#define CONFIG_MAX_HZ            500
#define CONFIG_DEFAULT_SERVER_PORT        6379    /* TCP port */
#define CONFIG_DEFAULT_TCP_BACKLOG       511     /* TCP listen backlog */
#define CONFIG_DEFAULT_CLIENT_TIMEOUT       0       /* default client timeout: infinite */
#define CONFIG_DEFAULT_DBNUM     16
#define CONFIG_MAX_LINE    1024
#define CRON_DBS_PER_CALL 16
#define NET_MAX_WRITES_PER_EVENT (1024*64)
#define PROTO_SHARED_SELECT_CMDS 10
#define OBJ_SHARED_INTEGERS 10000
#define OBJ_SHARED_BULKHDR_LEN 32       //共享的回复长度
#define LOG_MAX_LEN    1024 /* Default maximum length of syslog messages */
#define AOF_REWRITE_PERC  100
#define AOF_REWRITE_MIN_SIZE (64*1024*1024)
#define AOF_REWRITE_ITEMS_PER_CMD 64
#define CONFIG_DEFAULT_SLOWLOG_LOG_SLOWER_THAN 10000
#define CONFIG_DEFAULT_SLOWLOG_MAX_LEN 128
#define CONFIG_DEFAULT_MAX_CLIENTS 10000
#define CONFIG_AUTHPASS_MAX_LEN 512
#define CONFIG_DEFAULT_SLAVE_PRIORITY 100
#define CONFIG_DEFAULT_REPL_TIMEOUT 60
#define CONFIG_DEFAULT_REPL_PING_SLAVE_PERIOD 10
#define CONFIG_RUN_ID_SIZE 40
#define RDB_EOF_MARK_SIZE 40                            //rdb文件EOF的长度
#define CONFIG_DEFAULT_REPL_BACKLOG_SIZE (1024*1024)    /* 1mb */
#define CONFIG_DEFAULT_REPL_BACKLOG_TIME_LIMIT (60*60)  /* 1 hour */
#define CONFIG_REPL_BACKLOG_MIN_SIZE (1024*16)          /* 16k */
#define CONFIG_BGSAVE_RETRY_DELAY 5 /* Wait a few secs before trying again. */
#define CONFIG_DEFAULT_PID_FILE "/var/run/redis.pid"
#define CONFIG_DEFAULT_SYSLOG_IDENT "redis"
#define CONFIG_DEFAULT_CLUSTER_CONFIG_FILE "nodes.conf"
#define CONFIG_DEFAULT_DAEMONIZE 0
#define CONFIG_DEFAULT_UNIX_SOCKET_PERM 0
#define CONFIG_DEFAULT_TCP_KEEPALIVE 300
#define CONFIG_DEFAULT_PROTECTED_MODE 1
#define CONFIG_DEFAULT_LOGFILE ""
#define CONFIG_DEFAULT_SYSLOG_ENABLED 0
#define CONFIG_DEFAULT_STOP_WRITES_ON_BGSAVE_ERROR 1
#define CONFIG_DEFAULT_RDB_COMPRESSION 1
#define CONFIG_DEFAULT_RDB_CHECKSUM 1
#define CONFIG_DEFAULT_RDB_FILENAME "dump.rdb"
#define CONFIG_DEFAULT_REPL_DISKLESS_SYNC 0
#define CONFIG_DEFAULT_REPL_DISKLESS_SYNC_DELAY 5
#define CONFIG_DEFAULT_SLAVE_SERVE_STALE_DATA 1
#define CONFIG_DEFAULT_SLAVE_READ_ONLY 1
#define CONFIG_DEFAULT_SLAVE_ANNOUNCE_IP NULL
#define CONFIG_DEFAULT_SLAVE_ANNOUNCE_PORT 0
#define CONFIG_DEFAULT_REPL_DISABLE_TCP_NODELAY 0
#define CONFIG_DEFAULT_MAXMEMORY 0
#define CONFIG_DEFAULT_MAXMEMORY_SAMPLES 5
#define CONFIG_DEFAULT_AOF_FILENAME "appendonly.aof"
#define CONFIG_DEFAULT_AOF_NO_FSYNC_ON_REWRITE 0
#define CONFIG_DEFAULT_AOF_LOAD_TRUNCATED 1
#define CONFIG_DEFAULT_ACTIVE_REHASHING 1
#define CONFIG_DEFAULT_AOF_REWRITE_INCREMENTAL_FSYNC 1
#define CONFIG_DEFAULT_MIN_SLAVES_TO_WRITE 0
#define CONFIG_DEFAULT_MIN_SLAVES_MAX_LAG 10
// 46 here is to support ipv4-mapped-on-ipv6
// 0000:0000:0000:0000:0000:FFFF:111.222.212.222
#define NET_IP_STR_LEN 46 /* INET6_ADDRSTRLEN is 46, but we need to be sure */
#define NET_PEER_ID_LEN (NET_IP_STR_LEN+32) /* Must be enough for ip:port */
#define CONFIG_BINDADDR_MAX 16
#define CONFIG_MIN_RESERVED_FDS 32
#define CONFIG_DEFAULT_LATENCY_MONITOR_THRESHOLD 0

#define ACTIVE_EXPIRE_CYCLE_LOOKUPS_PER_LOOP 20 /* Loopkups per loop. */
#define ACTIVE_EXPIRE_CYCLE_FAST_DURATION 1000 /* Microseconds */
#define ACTIVE_EXPIRE_CYCLE_SLOW_TIME_PERC 25 /* CPU max % for keys collection */
#define ACTIVE_EXPIRE_CYCLE_SLOW 0
#define ACTIVE_EXPIRE_CYCLE_FAST 1

/* Instantaneous metrics tracking. */
#define STATS_METRIC_SAMPLES 16     /* Number of samples per metric. */
#define STATS_METRIC_COMMAND 0      /* Number of commands executed. */
#define STATS_METRIC_NET_INPUT 1    /* Bytes read to network .*/
#define STATS_METRIC_NET_OUTPUT 2   /* Bytes written to network. */
#define STATS_METRIC_COUNT 3

/* Protocol and I/O related defines */
#define PROTO_MAX_QUERYBUF_LEN  (1024*1024*1024) /* 1GB max query buffer. */
// IO大小
#define PROTO_IOBUF_LEN         (1024*16)  /*IO大小 Generic I/O buffer size */
// 16k输出缓冲区的大小
#define PROTO_REPLY_CHUNK_BYTES (16*1024) /* 16k output buffer */
#define PROTO_INLINE_MAX_SIZE   (1024*64) /* Max size of inline reads */
#define PROTO_MBULK_BIG_ARG     (1024*32)
// long类型转换为字符串所能使用的最大字节数
#define LONG_STR_SIZE      21          /* Bytes needed for long -> str + '\0' */
#define AOF_AUTOSYNC_BYTES (1024*1024*32) /* 自动执行同步的限制 fdatasync every 32MB */

/* When configuring the server eventloop, we setup it so that the total number
 * of file descriptors we can handle are server.maxclients + RESERVED_FDS +
 * a few more to stay safe. Since RESERVED_FDS defaults to 32, we add 96
 * in order to make sure of not over provisioning more than 128 fds. */
#define CONFIG_FDSET_INCR (CONFIG_MIN_RESERVED_FDS+96)

/* Hash table parameters */
#define HASHTABLE_MIN_FILL        10      /* Minimal hash table fill 10% */

/* Command flags. Please check the command table defined in the redis.c file
 * for more information about the meaning of every flag. */
#define CMD_WRITE 1                   /* "w" flag */
#define CMD_READONLY 2                /* "r" flag */
#define CMD_DENYOOM 4                 /* "m" flag */
#define CMD_NOT_USED_1 8              /* no longer used flag */
#define CMD_ADMIN 16                  /* "a" flag */
#define CMD_PUBSUB 32                 /* "p" flag */
#define CMD_NOSCRIPT  64              /* "s" flag */
#define CMD_RANDOM 128                /* "R" flag */
#define CMD_SORT_FOR_SCRIPT 256       /* "S" flag */
#define CMD_LOADING 512               /* "l" flag */
#define CMD_STALE 1024                /* "t" flag */
#define CMD_SKIP_MONITOR 2048         /* "M" flag */
#define CMD_ASKING 4096               /* "k" flag */
#define CMD_FAST 8192                 /* "F" flag */

/* Object types */
#define OBJ_STRING 0    //字符串对象
#define OBJ_LIST 1      //列表对象
#define OBJ_SET 2       //集合对象
#define OBJ_ZSET 3      //有序集合对象
#define OBJ_HASH 4      //哈希对象

/* Objects encoding. Some kind of objects like Strings and Hashes can be
 * internally represented in multiple ways. The 'encoding' field of the object
 * is set to one of this fields for this object. */
#define OBJ_ENCODING_RAW 0     /* Raw representation */     //原始表示方式，字符串对象是简单动态字符串
#define OBJ_ENCODING_INT 1     /* Encoded as integer */         //long类型的整数
#define OBJ_ENCODING_HT 2      /* Encoded as hash table */      //字典
#define OBJ_ENCODING_ZIPMAP 3  /* Encoded as zipmap */          //已废弃，不使用
#define OBJ_ENCODING_LINKEDLIST 4 /* Encoded as regular linked list */  //双端链表
#define OBJ_ENCODING_ZIPLIST 5 /* Encoded as ziplist */         //压缩列表
#define OBJ_ENCODING_INTSET 6  /* Encoded as intset */          //整数集合
#define OBJ_ENCODING_SKIPLIST 7  /* Encoded as skiplist */      //跳跃表和字典
#define OBJ_ENCODING_EMBSTR 8  /* Embedded sds string encoding */   //embstr编码的简单动态字符串
#define OBJ_ENCODING_QUICKLIST 9 /* Encoded as linked list of ziplists */   //快速列表

/* Defines related to the dump file format. To store 32 bits lengths for short
 * keys requires a lot of space, so we check the most significant 2 bits of
 * the first byte to interpreter the length:
 *
 * 00|000000 => if the two MSB are 00 the len is the 6 bits of this byte
 * 01|000000 00000000 =>  01, the len is 14 byes, 6 bits + 8 bits of next byte
 * 10|000000 [32 bit integer] => if it's 10, a full 32 bit len will follow
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

/* AOF states */
#define AOF_OFF 0             /* AOF is off */
#define AOF_ON 1              /* AOF is on */
#define AOF_WAIT_REWRITE 2    /* AOF waits rewrite to start appending */

/* Client flags */
// client是从节点服务器
#define CLIENT_SLAVE (1<<0)   /* This client is a slave server */
// client是主节点服务器
#define CLIENT_MASTER (1<<1)  /* This client is a master server */
// client是一个从节点监控器
#define CLIENT_MONITOR (1<<2) /* This client is a slave monitor, see MONITOR */
// client处于事务环境中
#define CLIENT_MULTI (1<<3)   /* This client is in a MULTI context */
#define CLIENT_BLOCKED (1<<4) /* The client is waiting in a blocking operation */
// 监视的键被修改，EXEC执行失败
#define CLIENT_DIRTY_CAS (1<<5) /* Watched keys modified. EXEC will fail. */
// 发送回复后要关闭client，当执行client kill命令等
#define CLIENT_CLOSE_AFTER_REPLY (1<<6) /* Close after writing entire reply. */
// 不被阻塞的client，保存在unblocked_clients中
#define CLIENT_UNBLOCKED (1<<7) /* This client was unblocked and is stored in
                                  server.unblocked_clients */
// 表示该客户端是一个专门处理lua脚本的伪客户端
#define CLIENT_LUA (1<<8) /* This is a non connected client used by Lua */
// 发送了ASKING命令
#define CLIENT_ASKING (1<<9)     /* Client issued the ASKING command */
// 正要关闭的client
#define CLIENT_CLOSE_ASAP (1<<10)/* Close this client ASAP */
#define CLIENT_UNIX_SOCKET (1<<11) /* Client connected via Unix domain socket */
// 命令入队时错误
#define CLIENT_DIRTY_EXEC (1<<12)  /* EXEC will fail for errors while queueing */
// 强制进行回复
#define CLIENT_MASTER_FORCE_REPLY (1<<13)  /* Queue replies even if is master */
// 强制将节点传播到AOF中
#define CLIENT_FORCE_AOF (1<<14)   /* Force AOF propagation of current cmd. */
// 强制将命令传播到从节点
#define CLIENT_FORCE_REPL (1<<15)  /* Force replication of current cmd. */
// Redis 2.8版本以前只有SYNC命令，该这个宏来标记client的版本以便选择不同的同步命令
#define CLIENT_PRE_PSYNC (1<<16)   /* Instance don't understand PSYNC. */
#define CLIENT_READONLY (1<<17)    /* Cluster client is in read-only state. */
#define CLIENT_PUBSUB (1<<18)      /* Client is in Pub/Sub mode. */
#define CLIENT_PREVENT_AOF_PROP (1<<19)  /* Don't propagate to AOF. */
#define CLIENT_PREVENT_REPL_PROP (1<<20)  /* Don't propagate to slaves. */
#define CLIENT_PREVENT_PROP (CLIENT_PREVENT_AOF_PROP|CLIENT_PREVENT_REPL_PROP)
// client还有输出的数据，但是没有设置写处理程序
#define CLIENT_PENDING_WRITE (1<<21) /* Client has output to send but a write
                                        handler is yet not installed. */

// 控制服务器是否回复客户端，默认为开启ON，
// 设置为不开启，服务器不会回复client命令
#define CLIENT_REPLY_OFF (1<<22)   /* Don't send replies to client. */
// 为下一条命令设置CLIENT_REPLY_SKIP标志
#define CLIENT_REPLY_SKIP_NEXT (1<<23)  /* Set CLIENT_REPLY_SKIP for next cmd */
// 设置为跳过该条回复，服务器会跳过这条命令的回复
#define CLIENT_REPLY_SKIP (1<<24)  /* Don't send just this reply. */
#define CLIENT_LUA_DEBUG (1<<25)  /* Run EVAL in debug mode. */
#define CLIENT_LUA_DEBUG_SYNC (1<<26)  /* EVAL debugging without fork() */

/* Client block type (btype field in client structure)
 * if CLIENT_BLOCKED flag is set. */
#define BLOCKED_NONE 0    /* Not blocked, no CLIENT_BLOCKED flag set. */
#define BLOCKED_LIST 1    /* BLPOP & co. */
#define BLOCKED_WAIT 2    /* WAIT for synchronous replication. */

/* Client request types */
#define PROTO_REQ_INLINE 1
#define PROTO_REQ_MULTIBULK 2

/* Client classes for client limits, currently used only for
 * the max-client-output-buffer limit implementation. */
#define CLIENT_TYPE_NORMAL 0 /* Normal req-reply clients + MONITORs */
#define CLIENT_TYPE_SLAVE 1  /* Slaves. */
#define CLIENT_TYPE_PUBSUB 2 /* Clients subscribed to PubSub channels. */
#define CLIENT_TYPE_MASTER 3 /* Master. */
#define CLIENT_TYPE_OBUF_COUNT 3 /* Number of clients to expose to output
                                    buffer configuration. Just the first
                                    three: normal, slave, pubsub. */

/* Slave replication state. Used in server.repl_state for slaves to remember
 * what to do next. */
// replication关闭状态
#define REPL_STATE_NONE 0 /* No active replication */
// 必须重新连接主节点
#define REPL_STATE_CONNECT 1 /* Must connect to master */
// 处于和主节点正在连接的状态
#define REPL_STATE_CONNECTING 2 /* Connecting to master */
/* --- Handshake states, must be ordered --- */
// 握手状态，有序
// 等待主节点回复PING命令一个PONG
#define REPL_STATE_RECEIVE_PONG 3 /* Wait for PING reply */
// 发送认证命令AUTH给主节点
#define REPL_STATE_SEND_AUTH 4 /* Send AUTH to master */
// 设置状态为等待接受认证回复
#define REPL_STATE_RECEIVE_AUTH 5 /* Wait for AUTH reply */
// 发送从节点的端口号
#define REPL_STATE_SEND_PORT 6 /* Send REPLCONF listening-port */
// 接受一个从节点监听端口号
#define REPL_STATE_RECEIVE_PORT 7 /* Wait for REPLCONF reply */
#define REPL_STATE_SEND_IP 8 /* Send REPLCONF ip-address */
#define REPL_STATE_RECEIVE_IP 9 /* Wait for REPLCONF reply */
// 发送一个
#define REPL_STATE_SEND_CAPA 10 /* Send REPLCONF capa */
#define REPL_STATE_RECEIVE_CAPA 11 /* Wait for REPLCONF reply */
#define REPL_STATE_SEND_PSYNC 12 /* Send PSYNC */
// 等待一个PSYNC回复
#define REPL_STATE_RECEIVE_PSYNC 13 /* Wait for PSYNC reply */
/* --- End of handshake states --- */
// 正从主节点接受RDB文件
#define REPL_STATE_TRANSFER 14 /* Receiving .rdb from master */
// 和主节点保持连接
#define REPL_STATE_CONNECTED 15 /* Connected to master */

/* State of slaves from the POV of the master. Used in client->replstate.
 * In SEND_BULK and ONLINE state the slave receives new updates
 * in its output queue. In the WAIT_BGSAVE states instead the server is waiting
 * to start the next background saving in order to send updates to it. */
// 从服务器节点等待BGSAVE节点的开始，因此要生成一个新的RDB文件
#define SLAVE_STATE_WAIT_BGSAVE_START 6 /* We need to produce a new RDB file. */
// 已经创建子进程执行写RDB操作，等待完成
#define SLAVE_STATE_WAIT_BGSAVE_END 7 /* Waiting RDB file creation to finish. */
// 正在发送RDB文件给从节点
#define SLAVE_STATE_SEND_BULK 8 /* Sending RDB file to slave. */
// RDB文件传输完成
#define SLAVE_STATE_ONLINE 9 /* RDB file transmitted, sending just updates. */

/* Slave capabilities. */
#define SLAVE_CAPA_NONE 0
// 能够解析出RDB文件的EOF流格式
#define SLAVE_CAPA_EOF (1<<0)   /* Can parse the RDB EOF streaming format. */

/* Synchronous read timeout - slave side */
#define CONFIG_REPL_SYNCIO_TIMEOUT 5

/* List related stuff */
#define LIST_HEAD 0     //列表头
#define LIST_TAIL 1     //列表尾

/* Sort operations */
#define SORT_OP_GET 0

/* Log levels */
#define LL_DEBUG 0
#define LL_VERBOSE 1
#define LL_NOTICE 2     //日志通知
#define LL_WARNING 3    //日志的警告
#define LL_RAW (1<<10) /* Modifier to log without timestamp */
#define CONFIG_DEFAULT_VERBOSITY LL_NOTICE

/* Supervision options */
#define SUPERVISED_NONE 0
#define SUPERVISED_AUTODETECT 1
#define SUPERVISED_SYSTEMD 2
#define SUPERVISED_UPSTART 3

/* Anti-warning macro... */
#define UNUSED(V) ((void) V)

#define ZSKIPLIST_MAXLEVEL 32 /* Should be enough for 2^32 elements */
#define ZSKIPLIST_P 0.25      /* Skiplist P = 1/4 */

/* Append only defines */
#define AOF_FSYNC_NO 0           //不执行同步，由系统执行
#define AOF_FSYNC_ALWAYS 1       //每次写入都执行同步
#define AOF_FSYNC_EVERYSEC 2     //每秒同步一次
#define CONFIG_DEFAULT_AOF_FSYNC AOF_FSYNC_EVERYSEC

/* Zip structure related defaults */
#define OBJ_HASH_MAX_ZIPLIST_ENTRIES 512
#define OBJ_HASH_MAX_ZIPLIST_VALUE 64
#define OBJ_SET_MAX_INTSET_ENTRIES 512
#define OBJ_ZSET_MAX_ZIPLIST_ENTRIES 128
#define OBJ_ZSET_MAX_ZIPLIST_VALUE 64

/* List defaults */
#define OBJ_LIST_MAX_ZIPLIST_SIZE -2
#define OBJ_LIST_COMPRESS_DEPTH 0

/* HyperLogLog defines */
#define CONFIG_DEFAULT_HLL_SPARSE_MAX_BYTES 3000

/* Sets operations codes */
#define SET_OP_UNION 0
#define SET_OP_DIFF 1
#define SET_OP_INTER 2

/* Redis maxmemory strategies */
// Redis 过期键回收策略
#define MAXMEMORY_VOLATILE_LRU 0
#define MAXMEMORY_VOLATILE_TTL 1
#define MAXMEMORY_VOLATILE_RANDOM 2
#define MAXMEMORY_ALLKEYS_LRU 3
#define MAXMEMORY_ALLKEYS_RANDOM 4
#define MAXMEMORY_NO_EVICTION 5
#define CONFIG_DEFAULT_MAXMEMORY_POLICY MAXMEMORY_NO_EVICTION

/* Scripting */
#define LUA_SCRIPT_TIME_LIMIT 5000 /* milliseconds */

/* Units */
#define UNIT_SECONDS 0      //单位是秒
#define UNIT_MILLISECONDS 1 //单位是毫秒

/* SHUTDOWN flags */
#define SHUTDOWN_NOFLAGS 0      /* 不指定标志 No flags. */
#define SHUTDOWN_SAVE 1         /* 指定停机保存标志即使配置了SHUTDOWN_NOSAVE标志 Force SAVE on SHUTDOWN even if no save
                                   points are configured. */
#define SHUTDOWN_NOSAVE 2       /* 指定停机不保存标志 Don't SAVE on SHUTDOWN. */

/* Command call flags, see call() function */
#define CMD_CALL_NONE 0
#define CMD_CALL_SLOWLOG (1<<0)
#define CMD_CALL_STATS (1<<1)
#define CMD_CALL_PROPAGATE_AOF (1<<2)
#define CMD_CALL_PROPAGATE_REPL (1<<3)
#define CMD_CALL_PROPAGATE (CMD_CALL_PROPAGATE_AOF|CMD_CALL_PROPAGATE_REPL)
#define CMD_CALL_FULL (CMD_CALL_SLOWLOG | CMD_CALL_STATS | CMD_CALL_PROPAGATE)

/* Command propagation flags, see propagate() function */
#define PROPAGATE_NONE 0
#define PROPAGATE_AOF 1
#define PROPAGATE_REPL 2

/* RDB active child save type. */
#define RDB_CHILD_TYPE_NONE 0
#define RDB_CHILD_TYPE_DISK 1     /* RDB 被写入磁盘 RDB is written to disk. */
#define RDB_CHILD_TYPE_SOCKET 2   /* RDB 被写到从节点的套接字中 RDB is written to slave socket. */

/* Keyspace changes notification classes. Every class is associated with a
 * character for configuration purposes. */
// 键空间通知的类型，每个类型都关联着一个有目的的字符
#define NOTIFY_KEYSPACE (1<<0)    /* K */   //键空间
#define NOTIFY_KEYEVENT (1<<1)    /* E */   //键事件
#define NOTIFY_GENERIC (1<<2)     /* g */   //通用无类型通知
#define NOTIFY_STRING (1<<3)      /* $ */   //字符串类型键通知
#define NOTIFY_LIST (1<<4)        /* l */   //列表键通知
#define NOTIFY_SET (1<<5)         /* s */   //集合键通知
#define NOTIFY_HASH (1<<6)        /* h */   //哈希键通知
#define NOTIFY_ZSET (1<<7)        /* z */   //有序集合键通知
#define NOTIFY_EXPIRED (1<<8)     /* x */   //过期有关的键通知
#define NOTIFY_EVICTED (1<<9)     /* e */   //驱逐有关的键通知
#define NOTIFY_ALL (NOTIFY_GENERIC | NOTIFY_STRING | NOTIFY_LIST | NOTIFY_SET | NOTIFY_HASH | NOTIFY_ZSET | NOTIFY_EXPIRED | NOTIFY_EVICTED)      /* A */   //所有键通知

/* Get the first bind addr or NULL */
#define NET_FIRST_BIND_ADDR (server.bindaddr_count ? server.bindaddr[0] : NULL)

/* Using the following macro you can run code inside serverCron() with the
 * specified period, specified in milliseconds.
 * The actual resolution depends on server.hz. */
// 用这个宏来控制if条件中的代码执行的频率
#define run_with_period(_ms_) if ((_ms_ <= 1000/server.hz) || !(server.cronloops%((_ms_)/(1000/server.hz))))

/* We can print the stacktrace, so our assert is defined this way: */
#define serverAssertWithInfo(_c,_o,_e) ((_e)?(void)0 : (_serverAssertWithInfo(_c,_o,#_e,__FILE__,__LINE__),_exit(1)))
#define serverAssert(_e) ((_e)?(void)0 : (_serverAssert(#_e,__FILE__,__LINE__),_exit(1)))
#define serverPanic(_e) _serverPanic(#_e,__FILE__,__LINE__),_exit(1)    //#_e 将_e字符串化

/*-----------------------------------------------------------------------------
 * Data types
 *----------------------------------------------------------------------------*/

/* A redis object, that is a type able to hold a string / list / set */

/* The actual Redis Object */
#define LRU_BITS 24
#define LRU_CLOCK_MAX ((1<<LRU_BITS)-1) /* Max value of obj->lru */
#define LRU_CLOCK_RESOLUTION 1000 /* LRU clock resolution in ms */
typedef struct redisObject {
    //对象的数据类型，占4bits，共5种类型
    unsigned type:4;
    //对象的编码，占4bits，共10种类型
    unsigned encoding:4;

    //least recently used
    //实用LRU算法计算相对server.lruclock的LRU时间
    unsigned lru:LRU_BITS; /* lru time (relative to server.lruclock) */

    //引用计数
    int refcount;

    //指向底层数据实现的指针
    void *ptr;
} robj;

/* Macro used to obtain the current LRU clock.
 * If the current resolution is lower than the frequency we refresh the
 * LRU clock (as it should be in production servers) we return the
 * precomputed value, otherwise we need to resort to a system call. */
//计算当前LRU时间
#define LRU_CLOCK() ((1000/server.hz <= LRU_CLOCK_RESOLUTION) ? server.lruclock : getLRUClock())

/* Macro used to initialize a Redis object allocated on the stack.
 * Note that this macro is taken near the structure definition to make sure
 * we'll update it when the structure is changed, to avoid bugs like
 * bug #85 introduced exactly in this way. */
// 初始化一个在栈中分配的Redis对象
#define initStaticStringObject(_var,_ptr) do { \
    _var.refcount = 1; \
    _var.type = OBJ_STRING; \
    _var.encoding = OBJ_ENCODING_RAW; \
    _var.ptr = _ptr; \
} while(0)

/* To improve the quality of the LRU approximation we take a set of keys
 * that are good candidate for eviction across freeMemoryIfNeeded() calls.
 *
 * Entries inside the eviciton pool are taken ordered by idle time, putting
 * greater idle times to the right (ascending order).
 *
 * Empty entries have the key pointer set to NULL. */
#define MAXMEMORY_EVICTION_POOL_SIZE 16
struct evictionPoolEntry {
    // 空转时间
    unsigned long long idle;    /* Object idle time. */
    // 键名称，一个字符串
    sds key;                    /* Key name. */
};

/* Redis database representation. There are multiple databases identified
 * by integers from 0 (the default database) up to the max configured
 * database. The database number is the 'id' field in the structure. */
typedef struct redisDb {
    // 键值对字典，保存数据库中所有的键值对
    dict *dict;                 /* The keyspace for this DB */
    // 过期字典，保存着设置过期的键和键的过期时间
    dict *expires;              /* Timeout of keys with a timeout set */
    // 保存着 所有造成客户端阻塞的键和被阻塞的客户端
    dict *blocking_keys;        /*Keys with clients waiting for data (BLPOP) */
    // 保存着 处于阻塞状态的键，value为NULL
    dict *ready_keys;           /* Blocked keys that received a PUSH */
    // 事物模块，用于保存被WATCH命令所监控的键
    dict *watched_keys;         /* WATCHED keys for MULTI/EXEC CAS */
    // 当内存不足时，Redis会根据LRU算法回收一部分键所占的空间，而该eviction_pool是一个长为16数组，保存可能被回收的键
    // eviction_pool中所有键按照idle空转时间，从小到大排序，每次回收空转时间最长的键
    struct evictionPoolEntry *eviction_pool;    /* Eviction pool of keys */
    // 数据库ID
    int id;                     /* Database ID */
    // 键的平均过期时间
    long long avg_ttl;          /* Average TTL, just for stats */
} redisDb;

/* Client MULTI/EXEC state */
// 事务命令状态
typedef struct multiCmd {
    // 命令的参数列表
    robj **argv;
    // 命令的参数个数
    int argc;
    // 命令函数指针
    struct redisCommand *cmd;
} multiCmd;

// 事务状态
typedef struct multiState {
    // 事务命令队列数组
    multiCmd *commands;     /* Array of MULTI commands */
    // 事务命令的个数
    int count;              /* Total number of MULTI commands */
    // 同步复制的标识
    int minreplicas;        /* MINREPLICAS for synchronous replication */
    // 同步复制的超时时间
    time_t minreplicas_timeout; /* MINREPLICAS timeout as unixtime. */
} multiState;

/* This structure holds the blocking operation state for a client.
 * The fields used depend on client->btype. */
typedef struct blockingState {
    /* Generic fields. */
    //阻塞的时间
    mstime_t timeout;       /* Blocking operation timeout. If UNIX current time
                             * is > timeout then the operation timed out. */

    /* BLOCKED_LIST */
    //造成阻塞的键
    dict *keys;             /* The keys we are waiting to terminate a blocking
                             * operation such as BLPOP. Otherwise NULL. */
    //用于BRPOPLPUSH命令
    //用于保存PUSH入元素的键，也就是dstkey
    robj *target;           /* The key that should receive the element,
                             * for BRPOPLPUSH. */

    /* BLOCKED_WAIT */
    // 阻塞状态
    int numreplicas;        /* Number of replicas we are waiting for ACK. */
    // 要达到的复制偏移量
    long long reploffset;   /* Replication offset to reach. */
} blockingState;

/* The following structure represents a node in the server.ready_keys list,
 * where we accumulate all the keys that had clients blocked with a blocking
 * operation such as B[LR]POP, but received new data in the context of the
 * last executed command.
 *
 * After the execution of every command or script, we run this list to check
 * if as a result we should serve data to clients blocked, unblocking them.
 * Note that server.ready_keys will not have duplicates as there dictionary
 * also called ready_keys in every structure representing a Redis database,
 * where we make sure to remember if a given key was already added in the
 * server.ready_keys list. */
typedef struct readyList {
    redisDb *db;
    robj *key;
} readyList;

/* With multiplexing we need to take per-client state.
 * Clients are taken in a linked list. */
typedef struct client {
    // client独一无二的ID
    uint64_t id;            /* Client incremental unique ID. */
    // client的套接字
    int fd;                 /* Client socket. */
    // 指向当前的数据库
    redisDb *db;            /* Pointer to currently SELECTed DB. */
    // 保存指向数据库的ID
    int dictid;             /* ID of the currently SELECTed DB. */
    // client的名字
    robj *name;             /* As set by CLIENT SETNAME. */
    // 输入缓冲区
    sds querybuf;           /* Buffer we use to accumulate client queries. */
    // 输入缓存的峰值
    size_t querybuf_peak;   /* Recent (100ms or more) peak of querybuf size. */
    // client输入命令时，参数的数量
    int argc;               /* Num of arguments of current command. */
    // client输入命令的参数列表
    robj **argv;            /* Arguments of current command. */
    // 保存客户端执行命令的历史记录
    struct redisCommand *cmd, *lastcmd;  /* Last command executed. */
    // 请求协议类型，内联或者多条命令
    int reqtype;            /* Request protocol type: PROTO_REQ_* */
    // 参数列表中未读取命令参数的数量，读取一个，该值减1
    int multibulklen;       /* Number of multi bulk arguments left to read. */
    // 命令内容的长度
    long bulklen;           /* Length of bulk argument in multi bulk request. */
    // 回复缓存列表，用于发送大于固定回复缓冲区的回复
    list *reply;            /* List of reply objects to send to the client. */
    // 回复缓存列表对象的总字节数
    unsigned long long reply_bytes; /* Tot bytes of objects in reply list. */
    // 已发送的字节数或对象的字节数
    size_t sentlen;         /* Amount of bytes already sent in the current
                               buffer or object being sent. */
    // client创建所需时间
    time_t ctime;           /* Client creation time. */
    // 最后一次和服务器交互的时间
    time_t lastinteraction; /* Time of the last interaction, used for timeout */
    // 客户端的输出缓冲区超过软性限制的时间，记录输出缓冲区第一次到达软性限制的时间
    time_t obuf_soft_limit_reached_time;
    // client状态的标志
    int flags;              /* Client flags: CLIENT_* macros. */
    // 认证标志，0表示未认证，1表示已认证
    int authenticated;      /* When requirepass is non-NULL. */
    // 从节点的复制状态
    int replstate;          /* Replication state if this is a slave. */
    // 在ack上设置从节点的写处理器，是否在slave向master发送ack，
    int repl_put_online_on_ack; /* Install slave write handler on ACK. */
    // 保存主服务器传来的RDB文件的文件描述符
    int repldbfd;           /* Replication DB file descriptor. */
    // 读取主服务器传来的RDB文件的偏移量
    off_t repldboff;        /* Replication DB file offset. */
    // 主服务器传来的RDB文件的大小
    off_t repldbsize;       /* Replication DB file size. */
    // 主服务器传来的RDB文件的大小，符合协议的字符串形式
    sds replpreamble;       /* Replication DB preamble. */
    // replication复制的偏移量
    long long reploff;      /* Replication offset if this is our master. */
    // 通过ack命令接收到的偏移量
    long long repl_ack_off; /* Replication ack offset, if this is a slave. */
    // 通过ack命令接收到的偏移量所用的时间
    long long repl_ack_time;/* Replication ack time, if this is a slave. */
    // FULLRESYNC回复给从节点的offset
    long long psync_initial_offset; /* FULLRESYNC reply offset other slaves
                                       copying this slave output buffer
                                       should use. */
    char replrunid[CONFIG_RUN_ID_SIZE+1]; /* Master run id if is a master. */
    // 从节点的端口号
    int slave_listening_port; /* As configured with: REPLCONF listening-port */
    // 从节点IP地址
    char slave_ip[NET_IP_STR_LEN]; /* Optionally given by REPLCONF ip-address */
    // 从节点的功能
    int slave_capa;         /* Slave capabilities: SLAVE_CAPA_* bitwise OR. */

    // 事物状态
    multiState mstate;      /* MULTI/EXEC state */
    // 阻塞类型
    int btype;              /* Type of blocking op if CLIENT_BLOCKED. */
    // 阻塞的状态
    blockingState bpop;     /* blocking state */
    // 最近一个写全局的复制偏移量
    long long woff;         /* Last write global replication offset. */

    // 监控列表
    list *watched_keys;     /* Keys WATCHED for MULTI/EXEC CAS */
    // 订阅频道
    dict *pubsub_channels;  /* channels a client is interested in (SUBSCRIBE) */
    // 订阅的模式
    list *pubsub_patterns;  /* patterns a client is interested in (SUBSCRIBE) */
    // 被缓存的ID
    sds peerid;             /* Cached peer ID. */

    /* Response buffer */
    // 回复固定缓冲区的偏移量
    int bufpos;
    // 回复固定缓冲区
    char buf[PROTO_REPLY_CHUNK_BYTES];
} client;

// SAVE 900 1
// SAVE 300 10
// SAVE 60 10000
// 服务器在900秒之内，对数据库执行了至少1次修改
struct saveparam {
    time_t seconds;     //秒数
    int changes;        //修改的次数
};

struct sharedObjectsStruct {
    robj *crlf, *ok, *err, *emptybulk, *czero, *cone, *cnegone, *pong, *space,
    *colon, *nullbulk, *nullmultibulk, *queued,
    *emptymultibulk, *wrongtypeerr, *nokeyerr, *syntaxerr, *sameobjecterr,
    *outofrangeerr, *noscripterr, *loadingerr, *slowscripterr, *bgsaveerr,
    *masterdownerr, *roslaveerr, *execaborterr, *noautherr, *noreplicaserr,
    *busykeyerr, *oomerr, *plus, *messagebulk, *pmessagebulk, *subscribebulk,
    *unsubscribebulk, *psubscribebulk, *punsubscribebulk, *del, *rpop, *lpop,
    *lpush, *emptyscan, *minstring, *maxstring,
    *select[PROTO_SHARED_SELECT_CMDS],
    *integers[OBJ_SHARED_INTEGERS],
    *mbulkhdr[OBJ_SHARED_BULKHDR_LEN], /* "*<value>\r\n" */
    *bulkhdr[OBJ_SHARED_BULKHDR_LEN];  /* "$<value>\r\n" */
};

/* ZSETs use a specialized version of Skiplists */
typedef struct zskiplistNode {
    robj *obj;                          //保存成员对象的地址
    double score;                       //分值
    struct zskiplistNode *backward;     //后退指针
    struct zskiplistLevel {
        struct zskiplistNode *forward;  //前进指针
        unsigned int span;              //跨度
    } level[];                          //层级，柔型数组
} zskiplistNode;

typedef struct zskiplist {
    struct zskiplistNode *header, *tail;//header指向跳跃表的表头节点，tail指向跳跃表的表尾节点
    unsigned long length;       //跳跃表的长度或跳跃表节点数量计数器，除去第一个节点
    int level;                  //跳跃表中节点的最大层数，除了第一个节点
} zskiplist;

typedef struct zset {
    dict *dict;         //字典
    zskiplist *zsl;     //跳跃表
} zset; //有序集合类型

typedef struct clientBufferLimitsConfig {
    unsigned long long hard_limit_bytes;
    unsigned long long soft_limit_bytes;
    time_t soft_limit_seconds;
} clientBufferLimitsConfig;

extern clientBufferLimitsConfig clientBufferLimitsDefaults[CLIENT_TYPE_OBUF_COUNT];

/* The redisOp structure defines a Redis Operation, that is an instance of
 * a command with an argument vector, database ID, propagation target
 * (PROPAGATE_*), and command pointer.
 *
 * Currently only used to additionally propagate more commands to AOF/Replication
 * after the propagation of the executed command. */
typedef struct redisOp {
    robj **argv;                //命令的参数列表
    int argc, dbid, target;     //参数个数，数据库的ID，传播的目标
    struct redisCommand *cmd;   //命令指针
} redisOp;          //Redis操作

/* Defines an array of Redis operations. There is an API to add to this
 * structure in a easy way.
 *
 * redisOpArrayInit();
 * redisOpArrayAppend();
 * redisOpArrayFree();
 */
typedef struct redisOpArray {
    redisOp *ops;   //数组指针
    int numops;     //数组个数
} redisOpArray;     //Redis操作数组

/*-----------------------------------------------------------------------------
 * Global server state
 *----------------------------------------------------------------------------*/

struct clusterState;

/* AIX defines hz to __hz, we don't use this define and in order to allow
 * Redis build on AIX we need to undef it. */
#ifdef _AIX
#undef hz
#endif

struct redisServer {
    /* General ××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××*/
    pid_t pid;                  /* Main process pid. */
    // 配置文件的绝对路径
    char *configfile;           /* Absolute config file path, or NULL */
    // 可执行文件的绝对路径
    char *executable;           /* Absolute executable file path. */
    // 执行executable文件的参数
    char **exec_argv;           /* Executable argv vector (copy). */
    // serverCron()调用的频率
    int hz;                     /* serverCron() calls frequency in hertz */

    // 数据库数组，长度为16
    redisDb *db;
    // 命令表
    dict *commands;             /* Command table */
    // rename之前的命令表
    dict *orig_commands;        /* Command table before command renaming. */
    // 事件循环
    aeEventLoop *el;
    // 服务器的LRU时钟
    unsigned lruclock:LRU_BITS; /* Clock for LRU eviction */
    // 立即关闭服务器
    int shutdown_asap;          /* SHUTDOWN needed ASAP */
    // 主动rehashing的标志
    int activerehashing;        /* Incremental rehash in serverCron() */
    // 是否设置了密码
    char *requirepass;          /* Pass for AUTH command, or NULL */
    // 保存子进程pid文件
    char *pidfile;              /* PID file path */
    int arch_bits;              /* 32 or 64 depending on sizeof(long) */
    // serverCron()函数运行的次数
    int cronloops;              /* Number of times the cron function run */
    // 服务器每次重启都会分配一个ID
    char runid[CONFIG_RUN_ID_SIZE+1];  /* ID always different at every exec. */
    // 哨兵模式
    int sentinel_mode;          /* True if this instance is a Sentinel. */
    /* Networking ××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××*/
    // TCP监听的端口
    int port;                   /* TCP listening port */
    // listen()函数的backlog参数，提示系统该进程要入队的未完成连接请求的数量。默认值为128
    int tcp_backlog;            /* TCP listen() backlog */
    char *bindaddr[CONFIG_BINDADDR_MAX]; /* Addresses we should bind to */
    // 绑定地址的数量
    int bindaddr_count;         /* Number of addresses in server.bindaddr[] */
    // Unix socket的路径
    char *unixsocket;           /* UNIX socket path */
    mode_t unixsocketperm;      /* UNIX socket permission */
    int ipfd[CONFIG_BINDADDR_MAX]; /* TCP socket file descriptors */
    int ipfd_count;             /* Used slots in ipfd[] */
    // 本地Unix连接的fd
    int sofd;                   /* Unix socket file descriptor */
    // 集群的fd
    int cfd[CONFIG_BINDADDR_MAX];/* Cluster bus listening socket */
    // 集群的fd个数
    int cfd_count;              /* Used slots in cfd[] */
    list *clients;              /* List of active clients */
    // 所有待关闭的client链表
    list *clients_to_close;     /* Clients to close asynchronously */
    // 要写或者安装写处理程序的client链表
    list *clients_pending_write; /* There is to write or install handler. */

    // 从节点列表和监视器列表
    list *slaves, *monitors;    /* List of slaves and MONITORs */
    // 当前的client，被用于崩溃报告
    client *current_client; /* Current client, only used on crash report */
    // 如果当前client正处于暂停状态，则设置为真
    int clients_paused;         /* True if clients are currently paused */
    // 取消暂停状态的时间
    mstime_t clients_pause_end_time; /* Time when we undo clients_paused */
    char neterr[ANET_ERR_LEN];   /* Error buffer for anet.c */
    // 迁移缓存套接字的字典，键是host:ip，值是TCP socket的的结构
    dict *migrate_cached_sockets;/* MIGRATE cached sockets */
    uint64_t next_client_id;    /* Next client unique ID. Incremental. */
    // 受保护模式，不接受外部的连接
    int protected_mode;         /* Don't accept external connections. */
    /* RDB / AOF loading information ××××××××××××××××××××××××××××××××××××××××××××××××××××××××××*/
    // 正在载入状态
    int loading;                /* We are loading data from disk if true */

    // 设置载入的总字节
    off_t loading_total_bytes;

    // 已载入的字节数
    off_t loading_loaded_bytes;

    // 载入的开始时间
    time_t loading_start_time;

    // 在load时，用来设置读或写的最大字节数max_processing_chunk
    off_t loading_process_events_interval_bytes;
    /* Fast pointers to often looked up command ×××××××××××××××××××××××××××××××××××××××××××××××*/
    struct redisCommand *delCommand, *multiCommand, *lpushCommand, *lpopCommand,
                        *rpopCommand, *sremCommand, *execCommand;
    /* Fields used only for stats ×××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××*/
    time_t stat_starttime;          /* Server start time */
    // 命令执行的次数
    long long stat_numcommands;     /* Number of processed commands */
    // 连接接受的数量
    long long stat_numconnections;  /* Number of connections received */
    // 过期键的数量
    long long stat_expiredkeys;     /* Number of expired keys */
    // 回收键的个数
    long long stat_evictedkeys;     /* Number of evicted keys (maxmemory) */
    long long stat_keyspace_hits;   /* Number of successful lookups of keys */
    long long stat_keyspace_misses; /* Number of failed lookups of keys */

    // 服务器内存使用的
    size_t stat_peak_memory;        /* Max used memory record */

    // 计算fork()消耗的时间
    long long stat_fork_time;       /* Time needed to perform latest fork() */

    // 计算fork的速率，GB/每秒
    double stat_fork_rate;          /* Fork rate in GB/sec. */
    // 因为最大连接数限制而被拒绝的client个数
    long long stat_rejected_conn;   /* Clients rejected because of maxclients */
    // 执行全量重同步的次数
    long long stat_sync_full;       /* Number of full resyncs with slaves. */
    // 接受PSYNC请求的个数
    long long stat_sync_partial_ok; /* Number of accepted PSYNC requests. */
    long long stat_sync_partial_err;/* Number of unaccepted PSYNC requests. */
    list *slowlog;                  /* SLOWLOG list of commands */
    long long slowlog_entry_id;     /* SLOWLOG current entry ID */
    long long slowlog_log_slower_than; /* SLOWLOG time limit (to get logged) */
    unsigned long slowlog_max_len;     /* SLOWLOG max number of items logged */
    // 常驻内存的大小
    size_t resident_set_size;       /* RSS sampled in serverCron(). */
    // 从网络读的字节数
    long long stat_net_input_bytes; /* Bytes read from network. */
    // 已经写到网络的字节数
    long long stat_net_output_bytes; /* Bytes written to network. */
    /* The following two are used to track instantaneous metrics, like number of operations per second，
     * network traffic. ××××××××××××××××××××××××××××××××××××××*/
    struct {
        // 上一个进行抽样的时间戳
        long long last_sample_time; /* Timestamp of last sample in ms */
        // 上一个抽样时的样品数量
        long long last_sample_count;/* Count in last sample */
        // 样品表
        long long samples[STATS_METRIC_SAMPLES];
        // 样品表的下标
        int idx;
    } inst_metric[STATS_METRIC_COUNT];
    /* Configuration ××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××*/
    // 可见性日志，日志级别
    int verbosity;                  /* Loglevel in redis.conf */
    // client超过的最大时间，单位秒
    int maxidletime;                /* Client timeout in seconds */
    int tcpkeepalive;               /* Set SO_KEEPALIVE if non-zero. */
    // 关闭测试，默认初始化为1
    int active_expire_enabled;      /* Can be disabled for testing purposes. */
    size_t client_max_querybuf_len; /* Limit for client query buffer length */
    int dbnum;                      /* Total number of configured DBs */
    // 1表示被监视，否则0
    int supervised;                 /* 1 if supervised, 0 otherwise. */
    // 监视模式
    int supervised_mode;            /* See SUPERVISED_* */
    // 如果是以守护进程运行，则为真
    int daemonize;                  /* True if running as a daemon */
    // 不同类型的client的输出缓冲区限制
    clientBufferLimitsConfig client_obuf_limits[CLIENT_TYPE_OBUF_COUNT];
    /* AOF persistence ××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××*/
    // AOF的状态，开启|关闭|等待重写
    int aof_state;                  /* AOF_(ON|OFF|WAIT_REWRITE) */

    // 同步策略
    int aof_fsync;                  /* Kind of fsync() policy */

    // AOf文件的名字
    char *aof_filename;             /* Name of the AOF file */
    // 如果正在执行AOF重写，设置为1
    int aof_no_fsync_on_rewrite;    /* Don't fsync if a rewrite is in prog. */
    // AOF文件所增长的比率，默认为100
    int aof_rewrite_perc;           /* Rewrite AOF if % growth is > M and... */
    // AOF文件最下的大小
    off_t aof_rewrite_min_size;     /* the AOF file is at least N bytes. */
    // AOF文件在重写或启动后的大小
    off_t aof_rewrite_base_size;    /* AOF size on latest startup or rewrite. */

    // AOF文件当前的大小
    off_t aof_current_size;         /* AOF current size. */

    // 将AOF重写提上日程，当RDB的BGSAVE结束后，立即执行AOF重写
    int aof_rewrite_scheduled;      /* Rewrite once BGSAVE terminates. */
    // 正在执行AOF操作的子进程id
    pid_t aof_child_pid;            /* PID if rewriting process */

    // AOF缓冲块的链表
    list *aof_rewrite_buf_blocks;   /* Hold changes during an AOF rewrite. */

    // AOF缓冲区，在进入事件loop之前写入
    sds aof_buf;      /* AOF buffer, written before entering the event loop */

    // AOF文件的文件描述符
    int aof_fd;       /* File descriptor of currently selected AOF file */

    // 执行AOF时，当前的数据库id
    int aof_selected_db; /* Currently selected DB in AOF */

    // 延迟执行flush操作的开始时间
    time_t aof_flush_postponed_start; /* UNIX time of postponed AOF flush */

    // AOF最近一次同步的时间
    time_t aof_last_fsync;            /* UNIX time of last fsync() */

    time_t aof_rewrite_time_last;   /* Time used by last AOF rewrite run. */

    // AOF开始的时间
    time_t aof_rewrite_time_start;  /* Current AOF rewrite start time. */
    int aof_lastbgrewrite_status;   /* C_OK or C_ERR */

    // 延迟fsync的次数
    unsigned long aof_delayed_fsync;  /* delayed AOF fsync() counter */
    // 重写时是否开启增量式同步，每次写入AOF_AUTOSYNC_BYTES个字节，就执行一次同步
    int aof_rewrite_incremental_fsync;/* fsync incrementally while rewriting? */
    // 上一次AOF操作的状态
    int aof_last_write_status;      /* C_OK or C_ERR */

    // 如果AOF最近一个写状态为错误的，则为真
    int aof_last_write_errno;       /* Valid if aof_last_write_status is ERR */

    // 在不是所预期的AOF结尾的地方继续加载
    // 如果发现末尾命令不完整则自动截掉,成功加载前面正确的数据。
    int aof_load_truncated;         /* Don't stop on unexpected AOF EOF. */
    /* AOF pipes used to communicate between parent and child during rewrite. ××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××*/
    int aof_pipe_write_data_to_child;   //父进程写给子进程的文件描述符
    int aof_pipe_read_data_from_parent; //子进程从父进程读的文件描述符
    int aof_pipe_write_ack_to_parent;   //子进程写ack给父进程的文件描述符
    int aof_pipe_read_ack_from_child;   //父进程从子进程读ack的文件描述符
    int aof_pipe_write_ack_to_child;    //父进程写ack给子进程的文件描述符
    int aof_pipe_read_ack_from_parent;  //子进程从父进程读ack的文件描述符

    // 如果为真，则停止发送累计的不同数据给子进程
    int aof_stop_sending_diff;     /* If true stop sending accumulated diffs
                                      to child process. */
    // 保存子进程AOF时差异累计数据的sds
    sds aof_child_diff;             /* AOF diff accumulator child side. */
    /* RDB persistence ××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××*/
    // 脏键，记录数据库被修改的次数
    long long dirty;                /* Changes to DB from the last save */

    // 在BGSAVE之前要备份脏键dirty的值，如果BGSAVE失败会还原
    long long dirty_before_bgsave;  /* Used to restore dirty on failed BGSAVE */

    // 执行BGSAVE的子进程的pid
    pid_t rdb_child_pid;            /* PID of RDB saving child */

    // 保存save参数的数组
    struct saveparam *saveparams;   /* Save points array for RDB */

    // 数组长度
    int saveparamslen;              /* Number of saving points */

    // RDB文件的名字，默认为dump.rdb
    char *rdb_filename;             /* Name of RDB file */

    // 是否采用LZF压缩算法压缩RDB文件，默认yes
    int rdb_compression;            /* Use compression in RDB? */

    // RDB文件是否使用校验和，默认yes
    int rdb_checksum;               /* Use RDB checksum? */

    // 上一次执行SAVE成功的时间
    time_t lastsave;                /* Unix time of last successful save */

    // 最近一个尝试执行BGSAVE的时间
    time_t lastbgsave_try;          /* Unix time of last attempted bgsave */

    // 最近执行BGSAVE的时间
    time_t rdb_save_time_last;      /* Time used by last RDB save run. */

    // BGSAVE开始的时间
    time_t rdb_save_time_start;     /* Current RDB save start time. */

    // 当rdb_bgsave_scheduled为真时，才能开始BGSAVE
    int rdb_bgsave_scheduled;       /* BGSAVE when possible if true. */

    // rdb执行的类型，是写入磁盘，还是写入从节点的socket
    int rdb_child_type;             /* Type of save by active child. */

    // BGSAVE执行完的状态
    int lastbgsave_status;          /* C_OK or C_ERR */
    // BGSAVE 出错，不允许写命令
    int stop_writes_on_bgsave_err;  /* Don't allow writes if can't BGSAVE */

    // 无磁盘同步，管道的写端
    int rdb_pipe_write_result_to_parent; /* RDB pipes used to return the state */
    // 无磁盘同步，管道的读端
    int rdb_pipe_read_result_from_child; /* of each slave in diskless SYNC. */
    /* Propagation of commands in AOF / replication ××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××*/
    // 将命令加到Redis操作数组中
    redisOpArray also_propagate;    /* Additional command to propagate. */
    /* Logging ××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××*/
    // 日志文件的路径
    char *logfile;                  /* Path of log file */
    // 是否开启系统日志
    int syslog_enabled;             /* Is syslog enabled? */
    // 系统日志标识
    char *syslog_ident;             /* Syslog ident */
    int syslog_facility;            /* Syslog facility */
    /* Replication (master) ×××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××*/
    // 复制缓冲区最近一次使用的数据库
    int slaveseldb;                 /* Last SELECTed DB in replication output */
    // 全局的复制偏移量
    long long master_repl_offset;   /* Global replication offset */
    // 每N秒，主节点PING从节点
    int repl_ping_slave_period;     /* Master pings the slave every N seconds */
    // 复制积压缓冲区backlog，用于局部同步
    char *repl_backlog;             /* Replication backlog for partial syncs */
    // 复制积压缓冲区backlog的大小
    long long repl_backlog_size;    /* Backlog circular buffer size */
    // 复制积压缓冲区backlog中实际的数据长度
    long long repl_backlog_histlen; /* Backlog actual data length */
    // 复制积压缓冲区backlog当前的偏移量，下次写操作的下标
    long long repl_backlog_idx;     /* Backlog circular buffer current offset */
    // 记录复制的偏移量，backlog的第一个字节的逻辑位置是master_repl_offset的下一个字节
    long long repl_backlog_off;     /* Replication offset of first byte in the
                                       backlog buffer. */
    // 没有从节点，backlog被释放的时间
    time_t repl_backlog_time_limit; /* Time without slaves after the backlog
                                       gets released. */
    // 没有从节点的时间
    time_t repl_no_slaves_since;    /* We have no slaves since that time.
                                       Only valid if server.slaves len is 0. */
    // 执行写操作的最少的从节点个数
    int repl_min_slaves_to_write;   /* Min number of slaves to write. */
    // 最小数量的从节点的最大延迟值
    int repl_min_slaves_max_lag;    /* Max lag of <count> slaves to write. */
    int repl_good_slaves_count;     /* Number of slaves with lag <= max_lag. */
    // 无盘同步，直接将RDB文件发送给socket
    int repl_diskless_sync;         /* Send RDB to slaves sockets directly. */
    // 延迟开始无盘同步的时间
    int repl_diskless_sync_delay;   /* Delay to start a diskless repl BGSAVE. */
    /* Replication (slave) ×××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××*/
    // 认证密码
    char *masterauth;               /* AUTH with this password with master */
    // 主节点的主机名(IP)
    char *masterhost;               /* Hostname of master */
    // 主节点的端口
    int masterport;                 /* Port of master */
    // 复制主从连接超时时间
    int repl_timeout;               /* Timeout after N seconds of master idle */
    // client是主服务器，对于这个从节点
    client *master;     /* Client that is master for this slave */
    // 主节点的同步缓存
    client *cached_master; /* Cached master to be reused for PSYNC. */
    // 同步IO调用的超时时间
    int repl_syncio_timeout; /* Timeout for synchronous I/O calls */
    int repl_state;          /* Replication status if the instance is a slave */
    // 同步期间从主节点读到的RDB的大小
    off_t repl_transfer_size; /* Size of RDB to read from master during sync. */
    // 同步期间从主节点读到的RDB的总量
    off_t repl_transfer_read; /* Amount of RDB read from master during sync. */
    // 最近一个执行fsync的偏移量
    off_t repl_transfer_last_fsync_off; /* Offset when we fsync-ed last time. */
    // 从节点和主节点的同步套接字
    int repl_transfer_s;     /* Slave -> Master SYNC socket */
    // 保存RDB文件的临时文件描述符
    int repl_transfer_fd;    /* Slave -> Master SYNC temp file descriptor */
    // 保存RDB文件的临时文件名
    char *repl_transfer_tmpfile; /* Slave-> master SYNC temp file name */
    // 最近一次读到RDB文件内容的时间
    time_t repl_transfer_lastio; /* Unix time of the latest read, for timeout */
    // 当连接下线后，是否维护旧的数据
    int repl_serve_stale_data; /* Serve stale data when link is down? */
    int repl_slave_ro;          /* Slave is read only? */
    // 连接断开的时长
    time_t repl_down_since; /* Unix time at which link with master went down */
    // 执行SYNC命令后是否关闭TCP_NODELAY
    int repl_disable_tcp_nodelay;   /* Disable TCP_NODELAY after SYNC? */
    int slave_priority;             /* Reported in INFO and used by Sentinel. */
    // 发送给主节点的从节点端口
    int slave_announce_port;        /* Give the master this listening port. */
    // 发送给主节点的IP
    char *slave_announce_ip;        /* Give the master this ip address. */
    // 主节点的运行ID
    char repl_master_runid[CONFIG_RUN_ID_SIZE+1];  /* Master run id for PSYNC.*/
    // 主节点PSYNC的偏移量
    long long repl_master_initial_offset;         /* Master PSYNC offset. */
    /* Replication script cache. ××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××*/
    // 保存SHA1键的字典
    dict *repl_scriptcache_dict;        /* SHA1 all slaves are aware of. */
    // 用作环形队列的链表
    list *repl_scriptcache_fifo;        /* First in, first out LRU eviction. */
    // 最大缓存脚本数
    unsigned int repl_scriptcache_size; /* Max number of elements. */
    /* Synchronous replication. ××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××*/
    // 等待 WAIT 命令的client链表
    list *clients_waiting_acks;         /* Clients waiting in WAIT command. */
    // 如果为真，则发送REPLCONF GETACK
    int get_ack_from_slaves;            /* If true we send REPLCONF GETACK. */
    /* Limits ××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××*/
    // 同时最多连接的client的数量
    unsigned int maxclients;            /* Max number of simultaneous clients */
    // 服务器最多使用的内存字节数
    unsigned long long maxmemory;   /* Max number of memory bytes to use */
    // 键占用内存的回收策略
    int maxmemory_policy;           /* Policy for key eviction */
    // 随机抽样的个数
    int maxmemory_samples;          /* Pricision of random sampling */
    /* Blocked clients ××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××*/
    unsigned int bpop_blocked_clients; /* Number of clients blocked by lists */
    // 非阻塞的client链表
    list *unblocked_clients; /* list of clients to unblock before next loop */
    // BLPOP 命令
    list *ready_keys;        /* List of readyList structures for BLPOP & co */
    /* Sort parameters - qsort_r() is only available under BSD so we have to take this state global,
     * in order to pass it to sortCompare() ××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××*/
    int sort_desc;
    int sort_alpha;
    int sort_bypattern;
    int sort_store;
    /* Zip structure config, see redis.conf for more information  ××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××*/
    size_t hash_max_ziplist_entries;
    size_t hash_max_ziplist_value;
    size_t set_max_intset_entries;
    size_t zset_max_ziplist_entries;
    size_t zset_max_ziplist_value;
    size_t hll_sparse_max_bytes;
    /* List parameters ××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××*/
    int list_max_ziplist_size;
    int list_compress_depth;
    /* time cache ××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××*/
    // 保存秒单位的Unix时间戳的缓存，每次重启时设置
    time_t unixtime;        /* Unix time sampled every cron cycle. */

    // 保存毫秒单位的Unix时间戳的缓存
    long long mstime;       /* Like 'unixtime' but with milliseconds resolution. */
    /* Pubsub ××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××*/
    dict *pubsub_channels;  /* Map channels to list of subscribed clients */
    list *pubsub_patterns;  /* A list of pubsub_patterns */
    int notify_keyspace_events; /* Events to propagate via Pub/Sub. This is an
                                   xor of NOTIFY_... flags. */
    /* Cluster ××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××*/
    // 集群模式是否开启
    int cluster_enabled;      /* Is cluster enabled? */
    // 集群节点超时时间
    mstime_t cluster_node_timeout; /* Cluster node timeout. */

    char *cluster_configfile; /* Cluster auto-generated config file name. */
    // 集群的状态
    struct clusterState *cluster;  /* State of the cluster */
    // 集群迁移障碍的节点数
    int cluster_migration_barrier; /* Cluster replicas migration barrier. */
    // 从节点故障转移的最长复制数据时间
    int cluster_slave_validity_factor; /* Slave max data age for failover. */
    // 如果为真，那么：如果有一个未指定的槽，那么将集群设置为下线状态
    int cluster_require_full_coverage; /* If true, put the cluster down if
                                          there is at least an uncovered slot.*/
    /* Scripting ××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××*/
    lua_State *lua; /* The Lua interpreter. We use just one for all clients */
    client *lua_client;   /* The "fake client" to query Redis from Lua */
    // client正在执行 EVAL 命令
    client *lua_caller;   /* The client running EVAL right now, or NULL */
    dict *lua_scripts;         /* A dictionary of SHA1 -> Lua scripts */
    mstime_t lua_time_limit;  /* Script timeout in milliseconds */
    mstime_t lua_time_start;  /* Start time of script, milliseconds time */
    int lua_write_dirty;  /* True if a write command was called during the
                             execution of the current script. */
    int lua_random_dirty; /* True if a random command was called during the
                             execution of the current script. */
    int lua_replicate_commands; /* True if we are doing single commands repl. */
    int lua_multi_emitted;/* True if we already proagated MULTI. */
    int lua_repl;         /* Script replication flags for redis.set_repl(). */
    // 执行脚本超时
    int lua_timedout;     /* True if we reached the time limit for script
                             execution. */
    int lua_kill;         /* Kill the script if true. */
    int lua_always_replicate_commands; /* Default replication type. */
    /* Latency monitor ××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××*/
    // 延迟的阀值
    long long latency_monitor_threshold;
    // 延迟与造成延迟的事件关联的字典
    dict *latency_events;
    /* Assert & bug reporting ××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××*/
    char *assert_failed;
    char *assert_file;
    int assert_line;
    int bug_report_start; /* True if bug report header was already logged. */
    // 看门狗的周期，单位毫秒。0表示关闭
    int watchdog_period;  /* Software watchdog period in ms. 0 = off */
    /* System hardware info ×××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××××*/
    size_t system_memory_size;  /* Total memory in system as reported by OS */
};

typedef struct pubsubPattern {
    client *client;
    robj *pattern;
} pubsubPattern;

// 命令处理程序
typedef void redisCommandProc(client *c);
typedef int *redisGetKeysProc(struct redisCommand *cmd, robj **argv, int argc, int *numkeys);

// redis命令结构
struct redisCommand {
    // 命令名称
    char *name;
    // proc函数指针，指向返回值为void，参数为client *c的函数
    // 指向实现命令的函数
    redisCommandProc *proc;
    // 参数个数
    int arity;
    // 字符串形式的标示值
    char *sflags; /* Flags as string representation, one char per flag. */
    // 实际的标示值
    int flags;    /* The actual flags, obtained from the 'sflags' field. */
    /* Use a function to determine keys arguments in a command line.
     * Used for Redis Cluster redirect. */
    // getkeys_proc是函数指针，返回值是有个整型的数组
    // 从命令行判断该命令的参数
    redisGetKeysProc *getkeys_proc;
    /* What keys should be loaded in background when calling this command? */
    // 指定哪些参数是key
    int firstkey; /* 第一个参数是 key The first argument that's a key (0 = no keys) */
    int lastkey;  /* 最后一个参数是 key The last argument that's a key */
    int keystep;  /* 第一个参数和最后一个参数的步长  The step between first and last key */
    // microseconds记录执行命令的耗费总时长
    // calls记录命令被执行的总次数
    long long microseconds, calls;
};

struct redisFunctionSym {
    char *name;
    unsigned long pointer;
};

typedef struct _redisSortObject {
    robj *obj;
    union {
        double score;
        robj *cmpobj;
    } u;
} redisSortObject;

typedef struct _redisSortOperation {
    int type;
    robj *pattern;
} redisSortOperation;

/* Structure to hold list iteration abstraction. */
typedef struct {
    robj *subject;          //迭代器指向的对象
    unsigned char encoding; //迭代器指向对象的编码类型
    unsigned char direction;//迭代器的方向
    quicklistIter *iter;    //quicklist的迭代器
} listTypeIterator; //列表类型迭代器

/* Structure for an entry while iterating over a list. */
typedef struct {
    listTypeIterator *li;   //所属的列表类型迭代器
    quicklistEntry entry;   //quicklist中的entry结构
} listTypeEntry;    //列表类型的entry结构

/* Structure to hold set iteration abstraction. */
typedef struct {
    robj *subject;                  //所属的集合对象
    int encoding;                   //集合对象编码类型
    int ii; /* intset iterator */   //整数集合的迭代器，编码为INTSET使用
    dictIterator *di;               //字典的迭代器，编码为HT使用
} setTypeIterator;

/* Structure to hold hash iteration abstraction. Note that iteration over
 * hashes involves both fields and values. Because it is possible that
 * not both are required, store pointers in the iterator to avoid
 * unnecessary memory allocation for fields/values. */
typedef struct {
    robj *subject;              // 哈希类型迭代器所属的哈希对象
    int encoding;               // 哈希对象的编码类型

    // ziplist
    unsigned char *fptr, *vptr; // 指向当前的key和value节点的地址，ziplist类型编码时使用

    // 字典
    dictIterator *di;           // 迭代HT类型的哈希对象时的字典迭代器
    dictEntry *de;              // 指向当前的哈希表节点
} hashTypeIterator;

#define OBJ_HASH_KEY 1          // 哈希键
#define OBJ_HASH_VALUE 2        // 哈希值

/*-----------------------------------------------------------------------------
 * Extern declarations
 *----------------------------------------------------------------------------*/

extern struct redisServer server;
extern struct sharedObjectsStruct shared;
extern dictType setDictType;
extern dictType zsetDictType;
extern dictType clusterNodesDictType;
extern dictType clusterNodesBlackListDictType;
extern dictType dbDictType;
extern dictType shaScriptObjectDictType;
extern double R_Zero, R_PosInf, R_NegInf, R_Nan;
extern dictType hashDictType;
extern dictType replScriptCacheDictType;

/*-----------------------------------------------------------------------------
 * Functions prototypes
 *----------------------------------------------------------------------------*/

/* Utils */
long long ustime(void);
long long mstime(void);
void getRandomHexChars(char *p, unsigned int len);
uint64_t crc64(uint64_t crc, const unsigned char *s, uint64_t l);
void exitFromChild(int retcode);
size_t redisPopcount(void *s, long count);
void redisSetProcTitle(char *title);

/* networking.c -- Networking and Client related operations */
client *createClient(int fd);
void closeTimedoutClients(void);
void freeClient(client *c);
void freeClientAsync(client *c);
void resetClient(client *c);
void sendReplyToClient(aeEventLoop *el, int fd, void *privdata, int mask);
void *addDeferredMultiBulkLength(client *c);
void setDeferredMultiBulkLength(client *c, void *node, long length);
void processInputBuffer(client *c);
void acceptHandler(aeEventLoop *el, int fd, void *privdata, int mask);
void acceptTcpHandler(aeEventLoop *el, int fd, void *privdata, int mask);
void acceptUnixHandler(aeEventLoop *el, int fd, void *privdata, int mask);
void readQueryFromClient(aeEventLoop *el, int fd, void *privdata, int mask);
void addReplyBulk(client *c, robj *obj);
void addReplyBulkCString(client *c, const char *s);
void addReplyBulkCBuffer(client *c, const void *p, size_t len);
void addReplyBulkLongLong(client *c, long long ll);
void addReply(client *c, robj *obj);
void addReplySds(client *c, sds s);
void addReplyBulkSds(client *c, sds s);
void addReplyError(client *c, const char *err);
void addReplyStatus(client *c, const char *status);
void addReplyDouble(client *c, double d);
void addReplyHumanLongDouble(client *c, long double d);
void addReplyLongLong(client *c, long long ll);
void addReplyMultiBulkLen(client *c, long length);
void copyClientOutputBuffer(client *dst, client *src);
void *dupClientReplyValue(void *o);
void getClientsMaxBuffers(unsigned long *longest_output_list,
                          unsigned long *biggest_input_buffer);
char *getClientPeerId(client *client);
sds catClientInfoString(sds s, client *client);
sds getAllClientsInfoString(void);
void rewriteClientCommandVector(client *c, int argc, ...);
void rewriteClientCommandArgument(client *c, int i, robj *newval);
void replaceClientCommandVector(client *c, int argc, robj **argv);
unsigned long getClientOutputBufferMemoryUsage(client *c);
void freeClientsInAsyncFreeQueue(void);
void asyncCloseClientOnOutputBufferLimitReached(client *c);
int getClientType(client *c);
int getClientTypeByName(char *name);
char *getClientTypeName(int class);
void flushSlavesOutputBuffers(void);
void disconnectSlaves(void);
int listenToPort(int port, int *fds, int *count);
void pauseClients(mstime_t duration);
int clientsArePaused(void);
int processEventsWhileBlocked(void);
int handleClientsWithPendingWrites(void);
int clientHasPendingReplies(client *c);
void unlinkClient(client *c);
int writeToClient(int fd, client *c, int handler_installed);

#ifdef __GNUC__
void addReplyErrorFormat(client *c, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
void addReplyStatusFormat(client *c, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
#else
void addReplyErrorFormat(client *c, const char *fmt, ...);
void addReplyStatusFormat(client *c, const char *fmt, ...);
#endif

/* List data type */
void listTypeTryConversion(robj *subject, robj *value);
// 列表类型的从where插入一个value，PUSH命令的底层实现
void listTypePush(robj *subject, robj *value, int where);
// 列表类型的从where弹出一个value，POP命令底层实现
robj *listTypePop(robj *subject, int where);
// 返回对象的长度，entry节点个数
unsigned long listTypeLength(robj *subject);
// 初始化列表类型的迭代器为一个指定的下标
listTypeIterator *listTypeInitIterator(robj *subject, long index, unsigned char direction);
// 释放迭代器空间
void listTypeReleaseIterator(listTypeIterator *li);
// 将列表类型的迭代器指向的entry保存在提供的listTypeEntry结构中，并且更新迭代器，1表示成功，0失败
int listTypeNext(listTypeIterator *li, listTypeEntry *entry);
// 返回一个节点的value对象，根据当前的迭代器
robj *listTypeGet(listTypeEntry *entry);
// 列表类型的插入操作，将value对象插到where
void listTypeInsert(listTypeEntry *entry, robj *value, int where);
// 比较列表类型的entry结构与对象的entry节点的值是否等，相等返回1
int listTypeEqual(listTypeEntry *entry, robj *o);
// 删除迭代器指向的entry
void listTypeDelete(listTypeIterator *iter, listTypeEntry *entry);
// 转换ZIPLIST编码类型为quicklist类型，enc指定OBJ_ENCODING_QUICKLIST
void listTypeConvert(robj *subject, int enc);
// 解阻塞一个正在阻塞中的client
void unblockClientWaitingData(client *c);
//处理client的阻塞状态
void handleClientsBlockedOnLists(void);
// POP命令的底层实现，where保存pop的位置
void popGenericCommand(client *c, int where);
//如果有client因为等待一个key被push而被阻塞，那么将这个key放入ready_keys
void signalListAsReady(redisDb *db, robj *key);

/* MULTI/EXEC/WATCH... */
void unwatchAllKeys(client *c);
void initClientMultiState(client *c);
void freeClientMultiState(client *c);
void queueMultiCommand(client *c);
void touchWatchedKey(redisDb *db, robj *key);
void touchWatchedKeysOnFlush(int dbid);
void discardTransaction(client *c);
void flagTransaction(client *c);
void execCommandPropagateMulti(client *c);

/* Redis object implementation */
void decrRefCount(robj *o);
void decrRefCountVoid(void *o);
void incrRefCount(robj *o);
robj *resetRefCount(robj *obj);
void freeStringObject(robj *o);
void freeListObject(robj *o);
void freeSetObject(robj *o);
void freeZsetObject(robj *o);
void freeHashObject(robj *o);
robj *createObject(int type, void *ptr);
robj *createStringObject(const char *ptr, size_t len);
robj *createRawStringObject(const char *ptr, size_t len);
robj *createEmbeddedStringObject(const char *ptr, size_t len);
robj *dupStringObject(robj *o);
int isObjectRepresentableAsLongLong(robj *o, long long *llongval);
robj *tryObjectEncoding(robj *o);
robj *getDecodedObject(robj *o);
size_t stringObjectLen(robj *o);
robj *createStringObjectFromLongLong(long long value);
robj *createStringObjectFromLongDouble(long double value, int humanfriendly);
robj *createQuicklistObject(void);
robj *createZiplistObject(void);
robj *createSetObject(void);
robj *createIntsetObject(void);
robj *createHashObject(void);
robj *createZsetObject(void);
robj *createZsetZiplistObject(void);
int getLongFromObjectOrReply(client *c, robj *o, long *target, const char *msg);
int checkType(client *c, robj *o, int type);
int getLongLongFromObjectOrReply(client *c, robj *o, long long *target, const char *msg);
int getDoubleFromObjectOrReply(client *c, robj *o, double *target, const char *msg);
int getLongLongFromObject(robj *o, long long *target);
int getLongDoubleFromObject(robj *o, long double *target);
int getLongDoubleFromObjectOrReply(client *c, robj *o, long double *target, const char *msg);
char *strEncoding(int encoding);
int compareStringObjects(robj *a, robj *b);
int collateStringObjects(robj *a, robj *b);
int equalStringObjects(robj *a, robj *b);
unsigned long long estimateObjectIdleTime(robj *o);

//检测objptr的编码类型是否是简单动态字符串类型为RAW或EMBSTR
#define sdsEncodedObject(objptr) (objptr->encoding == OBJ_ENCODING_RAW || objptr->encoding == OBJ_ENCODING_EMBSTR)

/* Synchronous I/O with timeout */
ssize_t syncWrite(int fd, char *ptr, ssize_t size, long long timeout);
ssize_t syncRead(int fd, char *ptr, ssize_t size, long long timeout);
ssize_t syncReadLine(int fd, char *ptr, ssize_t size, long long timeout);

/* Replication */
void replicationFeedSlaves(list *slaves, int dictid, robj **argv, int argc);
void replicationFeedMonitors(client *c, list *monitors, int dictid, robj **argv, int argc);
void updateSlavesWaitingBgsave(int bgsaveerr, int type);
void replicationCron(void);
void replicationHandleMasterDisconnection(void);
void replicationCacheMaster(client *c);
void resizeReplicationBacklog(long long newsize);
void replicationSetMaster(char *ip, int port);
void replicationUnsetMaster(void);
void refreshGoodSlavesCount(void);
void replicationScriptCacheInit(void);
void replicationScriptCacheFlush(void);
void replicationScriptCacheAdd(sds sha1);
int replicationScriptCacheExists(sds sha1);
void processClientsWaitingReplicas(void);
void unblockClientWaitingReplicas(client *c);
int replicationCountAcksByOffset(long long offset);
void replicationSendNewlineToMaster(void);
long long replicationGetSlaveOffset(void);
char *replicationGetSlaveName(client *c);
long long getPsyncInitialOffset(void);
int replicationSetupSlaveForFullResync(client *slave, long long offset);

/* Generic persistence functions */
void startLoading(FILE *fp);
void loadingProgress(off_t pos);
void stopLoading(void);

/* RDB persistence */
#include "rdb.h"

/* AOF persistence */
void flushAppendOnlyFile(int force);
void feedAppendOnlyFile(struct redisCommand *cmd, int dictid, robj **argv, int argc);
void aofRemoveTempFile(pid_t childpid);
int rewriteAppendOnlyFileBackground(void);
int loadAppendOnlyFile(char *filename);
void stopAppendOnly(void);
int startAppendOnly(void);
void backgroundRewriteDoneHandler(int exitcode, int bysignal);
void aofRewriteBufferReset(void);
unsigned long aofRewriteBufferSize(void);

/* Sorted sets data type */

/* Struct to hold a inclusive/exclusive range spec by score comparison. */
typedef struct {
    double min, max;    //最小值和最大值

    //minex表示最小值是否包含在这个范围，1表示不包含，0表示包含
    //同理maxex表示最大值
    int minex, maxex; /* are min or max exclusive? */
} zrangespec;

/* Struct to hold an inclusive/exclusive range spec by lexicographic comparison. */
typedef struct {
    robj *min, *max;  /* May be set to shared.(minstring|maxstring) *///最小值和最大值

    //minex表示最小值是否包含在这个范围，1表示不包含，0表示包含
    //同理maxex表示最大值
    int minex, maxex; /* are min or max exclusive? */
} zlexrangespec;

//创建返回一个跳跃表 表头zskiplist
zskiplist *zslCreate(void);
//释放跳跃表表头zsl，以及跳跃表节点
void zslFree(zskiplist *zsl);
// 创建一个节点，分数为score，对象为obj，插入到zsl表头管理的跳跃表中，并返回新节点的地址
zskiplistNode *zslInsert(zskiplist *zsl, double score, robj *obj);
// 将ele元素和分值score插入在ziplist中，从小到大排序
unsigned char *zzlInsert(unsigned char *zl, robj *ele, double score);
//删除score和obj的节点
int zslDelete(zskiplist *zsl, double score, robj *obj);
//返回第一个分数在range范围内的节点
zskiplistNode *zslFirstInRange(zskiplist *zsl, zrangespec *range);
//返回最后一个分数在range范围内的节点
zskiplistNode *zslLastInRange(zskiplist *zsl, zrangespec *range);
// 从sptr指向的entry中取出有序集合的分值
double zzlGetScore(unsigned char *sptr);
// 将当前的元素指针eptr和当前元素分值的指针sptr都指向下一个元素和元素的分值
void zzlNext(unsigned char *zl, unsigned char **eptr, unsigned char **sptr);
// 将当前的元素指针eptr和当前元素分值的指针sptr都指向上一个元素和元素的分值
void zzlPrev(unsigned char *zl, unsigned char **eptr, unsigned char **sptr);
// 返回有序集合的元素个数
unsigned int zsetLength(robj *zobj);
// 将有序集合对象的编码转换为encoding制定的编码类型
void zsetConvert(robj *zobj, int encoding);
// 按需转换编码成OBJ_ENCODING_ZIPLIST
void zsetConvertToZiplistIfNeeded(robj *zobj, size_t maxelelen);
// 将有序集合的member成员的分值保存到score中
int zsetScore(robj *zobj, robj *member, double *score);
//查找score和o对象在跳跃表中的排位
unsigned long zslGetRank(zskiplist *zsl, double score, robj *o);

/* Core functions */
int freeMemoryIfNeeded(void);
int processCommand(client *c);
void setupSignalHandlers(void);
struct redisCommand *lookupCommand(sds name);
struct redisCommand *lookupCommandByCString(char *s);
struct redisCommand *lookupCommandOrOriginal(sds name);
void call(client *c, int flags);
void propagate(struct redisCommand *cmd, int dbid, robj **argv, int argc, int flags);
void alsoPropagate(struct redisCommand *cmd, int dbid, robj **argv, int argc, int target);
void forceCommandPropagation(client *c, int flags);
void preventCommandPropagation(client *c);
void preventCommandAOF(client *c);
void preventCommandReplication(client *c);
int prepareForShutdown();
#ifdef __GNUC__
void serverLog(int level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
#else
void serverLog(int level, const char *fmt, ...);
#endif
void serverLogRaw(int level, const char *msg);
void serverLogFromHandler(int level, const char *msg);
void usage(void);
void updateDictResizePolicy(void);
int htNeedsResize(dict *dict);
void populateCommandTable(void);
void resetCommandTableStats(void);
void adjustOpenFilesLimit(void);
void closeListeningSockets(int unlink_unix_socket);
void updateCachedTime(void);
void resetServerStats(void);
unsigned int getLRUClock(void);
const char *evictPolicyToString(void);

#define RESTART_SERVER_NONE 0
#define RESTART_SERVER_GRACEFULLY (1<<0)     /* Do proper shutdown. */
#define RESTART_SERVER_CONFIG_REWRITE (1<<1) /* CONFIG REWRITE before restart.*/
int restartServer(int flags, mstime_t delay);

/* Set data type */
// 创建一个保存value的集合
robj *setTypeCreate(robj *value);
// 向subject集合中添加value，添加成功返回1，如果已经存在返回0
int setTypeAdd(robj *subject, robj *value);
// 从集合对象中删除一个值为value的元素，删除成功返回1，失败返回0
int setTypeRemove(robj *subject, robj *value);
// 集合中是否存在值为value的元素，存在返回1，否则返回0
int setTypeIsMember(robj *subject, robj *value);
// 创建并初始化一个集合类型的迭代器
setTypeIterator *setTypeInitIterator(robj *subject);
// 释放迭代器空间
void setTypeReleaseIterator(setTypeIterator *si);
// 将当前迭代器指向的元素保存在objele或llele中，迭代完毕返回-1
// 返回的对象的引用技术不增加，支持 读时共享写时复制
int setTypeNext(setTypeIterator *si, robj **objele, int64_t *llele);
// 返回迭代器当前指向的元素对象的地址，需要手动释放返回的对象
robj *setTypeNextObject(setTypeIterator *si);
// 从集合中随机取出一个对象，保存在参数中
int setTypeRandomElement(robj *setobj, robj **objele, int64_t *llele);
unsigned long setTypeRandomElements(robj *set, unsigned long count, robj *aux_set);
// 返回集合的元素数量
unsigned long setTypeSize(robj *subject);
// 将集合对象的INTSET编码类型转换为enc类型
void setTypeConvert(robj *subject, int enc);

/* Hash data type */
// 转换一个哈希对象的编码类型，enc指定新的编码类型
void hashTypeConvert(robj *o, int enc);
// 检查一个数字对象的长度判断是否需要进行类型的转换，从ziplist转换到ht类型
void hashTypeTryConversion(robj *subject, robj **argv, int start, int end);
// 对键和值的对象尝试进行优化编码以节约内存
void hashTypeTryObjectEncoding(robj *subject, robj **o1, robj **o2);
// 从一个哈希对象中返回field对应的值对象
robj *hashTypeGetObject(robj *o, robj *key);
// 判断field对象是否存在在o对象中
int hashTypeExists(robj *o, robj *key);
//  将field-value添加到哈希对象中，返回1，如果field存在更新新的值，返回0
int hashTypeSet(robj *o, robj *key, robj *value);
// 从一个哈希对象中删除field，成功返回1，没找到field返回0
int hashTypeDelete(robj *o, robj *key);
// 返回哈希对象中的键值对个数
unsigned long hashTypeLength(robj *o);
// 返回一个初始化的哈希类型的迭代器
hashTypeIterator *hashTypeInitIterator(robj *subject);
// 释放哈希类型迭代器空间
void hashTypeReleaseIterator(hashTypeIterator *hi);
// 讲哈希类型迭代器指向哈希对象中的下一个节点
int hashTypeNext(hashTypeIterator *hi);
// 从ziplist类型的哈希类型迭代器中获取对应的field或value，保存在参数中
void hashTypeCurrentFromZiplist(hashTypeIterator *hi, int what,
                                unsigned char **vstr,
                                unsigned int *vlen,
                                long long *vll);
// 从ziplist类型的哈希类型迭代器中获取对应的field或value，保存在参数中
void hashTypeCurrentFromHashTable(hashTypeIterator *hi, int what, robj **dst);
// 从哈希类型的迭代器中获取键或值
robj *hashTypeCurrentObject(hashTypeIterator *hi, int what);
// 以写操作在数据库中查找对应key的哈希对象，如果不存在则创建
robj *hashTypeLookupWriteOrCreate(client *c, robj *key);

/* Pub / Sub */
int pubsubUnsubscribeAllChannels(client *c, int notify);
int pubsubUnsubscribeAllPatterns(client *c, int notify);
void freePubsubPattern(void *p);
int listMatchPubsubPattern(void *a, void *b);
int pubsubPublishMessage(robj *channel, robj *message);

/* Keyspace events notification */
void notifyKeyspaceEvent(int type, char *event, robj *key, int dbid);
int keyspaceEventsStringToFlags(char *classes);
sds keyspaceEventsFlagsToString(int flags);

/* Configuration */
void loadServerConfig(char *filename, char *options);
void appendServerSaveParams(time_t seconds, int changes);
void resetServerSaveParams(void);
struct rewriteConfigState; /* Forward declaration to export API. */
void rewriteConfigRewriteLine(struct rewriteConfigState *state, const char *option, sds line, int force);
int rewriteConfig(char *path);

/* db.c -- Keyspace access API */
int removeExpire(redisDb *db, robj *key);
void propagateExpire(redisDb *db, robj *key);
int expireIfNeeded(redisDb *db, robj *key);
long long getExpire(redisDb *db, robj *key);
void setExpire(redisDb *db, robj *key, long long when);
robj *lookupKey(redisDb *db, robj *key, int flags);
robj *lookupKeyRead(redisDb *db, robj *key);
robj *lookupKeyWrite(redisDb *db, robj *key);
robj *lookupKeyReadOrReply(client *c, robj *key, robj *reply);
robj *lookupKeyWriteOrReply(client *c, robj *key, robj *reply);
robj *lookupKeyReadWithFlags(redisDb *db, robj *key, int flags);
#define LOOKUP_NONE 0               //zero，没有特殊意义
#define LOOKUP_NOTOUCH (1<<0)       //不修改键的使用时间
void dbAdd(redisDb *db, robj *key, robj *val);
void dbOverwrite(redisDb *db, robj *key, robj *val);
void setKey(redisDb *db, robj *key, robj *val);
int dbExists(redisDb *db, robj *key);
robj *dbRandomKey(redisDb *db);
int dbDelete(redisDb *db, robj *key);
robj *dbUnshareStringValue(redisDb *db, robj *key, robj *o);
long long emptyDb(void(callback)(void*));
int selectDb(client *c, int id);
void signalModifiedKey(redisDb *db, robj *key);
void signalFlushedDb(int dbid);
unsigned int getKeysInSlot(unsigned int hashslot, robj **keys, unsigned int count);
unsigned int countKeysInSlot(unsigned int hashslot);
unsigned int delKeysInSlot(unsigned int hashslot);
int verifyClusterConfigWithData(void);
void scanGenericCommand(client *c, robj *o, unsigned long cursor);
int parseScanCursorOrReply(client *c, robj *o, unsigned long *cursor);

/* API to get key arguments from commands */
int *getKeysFromCommand(struct redisCommand *cmd, robj **argv, int argc, int *numkeys);
void getKeysFreeResult(int *result);
int *zunionInterGetKeys(struct redisCommand *cmd,robj **argv, int argc, int *numkeys);
int *evalGetKeys(struct redisCommand *cmd, robj **argv, int argc, int *numkeys);
int *sortGetKeys(struct redisCommand *cmd, robj **argv, int argc, int *numkeys);
int *migrateGetKeys(struct redisCommand *cmd, robj **argv, int argc, int *numkeys);

/* Cluster */
void clusterInit(void);
unsigned short crc16(const char *buf, int len);
unsigned int keyHashSlot(char *key, int keylen);
void clusterCron(void);
void clusterPropagatePublish(robj *channel, robj *message);
void migrateCloseTimedoutSockets(void);
void clusterBeforeSleep(void);

/* Sentinel */
void initSentinelConfig(void);
void initSentinel(void);
void sentinelTimer(void);
char *sentinelHandleConfiguration(char **argv, int argc);
void sentinelIsRunning(void);

/* redis-check-rdb */
int redis_check_rdb(char *rdbfilename);
int redis_check_rdb_main(int argc, char **argv);

/* Scripting */
void scriptingInit(int setup);
int ldbRemoveChild(pid_t pid);
void ldbKillForkedSessions(void);
int ldbPendingChildren(void);

/* Blocked clients */
void processUnblockedClients(void);
void blockClient(client *c, int btype);
void unblockClient(client *c);
void replyToBlockedClientTimedOut(client *c);
int getTimeoutFromObjectOrReply(client *c, robj *object, mstime_t *timeout, int unit);
void disconnectAllBlockedClients(void);

/* Git SHA1 */
char *redisGitSHA1(void);
char *redisGitDirty(void);
uint64_t redisBuildId(void);

/* Commands prototypes */
void authCommand(client *c);
void pingCommand(client *c);
void echoCommand(client *c);
void commandCommand(client *c);
void setCommand(client *c);
void setnxCommand(client *c);
void setexCommand(client *c);
void psetexCommand(client *c);
void getCommand(client *c);
void delCommand(client *c);
void existsCommand(client *c);
void setbitCommand(client *c);
void getbitCommand(client *c);
void bitfieldCommand(client *c);
void setrangeCommand(client *c);
void getrangeCommand(client *c);
void incrCommand(client *c);
void decrCommand(client *c);
void incrbyCommand(client *c);
void decrbyCommand(client *c);
void incrbyfloatCommand(client *c);
void selectCommand(client *c);
void randomkeyCommand(client *c);
void keysCommand(client *c);
void scanCommand(client *c);
void dbsizeCommand(client *c);
void lastsaveCommand(client *c);
void saveCommand(client *c);
void bgsaveCommand(client *c);
void bgrewriteaofCommand(client *c);
void shutdownCommand(client *c);
void moveCommand(client *c);
void renameCommand(client *c);
void renamenxCommand(client *c);
void lpushCommand(client *c);
void rpushCommand(client *c);
void lpushxCommand(client *c);
void rpushxCommand(client *c);
void linsertCommand(client *c);
void lpopCommand(client *c);
void rpopCommand(client *c);
void llenCommand(client *c);
void lindexCommand(client *c);
void lrangeCommand(client *c);
void ltrimCommand(client *c);
void typeCommand(client *c);
void lsetCommand(client *c);
void saddCommand(client *c);
void sremCommand(client *c);
void smoveCommand(client *c);
void sismemberCommand(client *c);
void scardCommand(client *c);
void spopCommand(client *c);
void srandmemberCommand(client *c);
void sinterCommand(client *c);
void sinterstoreCommand(client *c);
void sunionCommand(client *c);
void sunionstoreCommand(client *c);
void sdiffCommand(client *c);
void sdiffstoreCommand(client *c);
void sscanCommand(client *c);
void syncCommand(client *c);
void flushdbCommand(client *c);
void flushallCommand(client *c);
void sortCommand(client *c);
void lremCommand(client *c);
void rpoplpushCommand(client *c);
void infoCommand(client *c);
void mgetCommand(client *c);
void monitorCommand(client *c);
void expireCommand(client *c);
void expireatCommand(client *c);
void pexpireCommand(client *c);
void pexpireatCommand(client *c);
void getsetCommand(client *c);
void ttlCommand(client *c);
void touchCommand(client *c);
void pttlCommand(client *c);
void persistCommand(client *c);
void slaveofCommand(client *c);
void roleCommand(client *c);
void debugCommand(client *c);
void msetCommand(client *c);
void msetnxCommand(client *c);
void zaddCommand(client *c);
void zincrbyCommand(client *c);
void zrangeCommand(client *c);
void zrangebyscoreCommand(client *c);
void zrevrangebyscoreCommand(client *c);
void zrangebylexCommand(client *c);
void zrevrangebylexCommand(client *c);
void zcountCommand(client *c);
void zlexcountCommand(client *c);
void zrevrangeCommand(client *c);
void zcardCommand(client *c);
void zremCommand(client *c);
void zscoreCommand(client *c);
void zremrangebyscoreCommand(client *c);
void zremrangebylexCommand(client *c);
void multiCommand(client *c);
void execCommand(client *c);
void discardCommand(client *c);
void blpopCommand(client *c);
void brpopCommand(client *c);
void brpoplpushCommand(client *c);
void appendCommand(client *c);
void strlenCommand(client *c);
void zrankCommand(client *c);
void zrevrankCommand(client *c);
void hsetCommand(client *c);
void hsetnxCommand(client *c);
void hgetCommand(client *c);
void hmsetCommand(client *c);
void hmgetCommand(client *c);
void hdelCommand(client *c);
void hlenCommand(client *c);
void hstrlenCommand(client *c);
void zremrangebyrankCommand(client *c);
void zunionstoreCommand(client *c);
void zinterstoreCommand(client *c);
void zscanCommand(client *c);
void hkeysCommand(client *c);
void hvalsCommand(client *c);
void hgetallCommand(client *c);
void hexistsCommand(client *c);
void hscanCommand(client *c);
void configCommand(client *c);
void hincrbyCommand(client *c);
void hincrbyfloatCommand(client *c);
void subscribeCommand(client *c);
void unsubscribeCommand(client *c);
void psubscribeCommand(client *c);
void punsubscribeCommand(client *c);
void publishCommand(client *c);
void pubsubCommand(client *c);
void watchCommand(client *c);
void unwatchCommand(client *c);
void clusterCommand(client *c);
void restoreCommand(client *c);
void migrateCommand(client *c);
void askingCommand(client *c);
void readonlyCommand(client *c);
void readwriteCommand(client *c);
void dumpCommand(client *c);
void objectCommand(client *c);
void clientCommand(client *c);
void evalCommand(client *c);
void evalShaCommand(client *c);
void scriptCommand(client *c);
void timeCommand(client *c);
void bitopCommand(client *c);
void bitcountCommand(client *c);
void bitposCommand(client *c);
void replconfCommand(client *c);
void waitCommand(client *c);
void geoencodeCommand(client *c);
void geodecodeCommand(client *c);
void georadiusByMemberCommand(client *c);
void georadiusCommand(client *c);
void geoaddCommand(client *c);
void geohashCommand(client *c);
void geoposCommand(client *c);
void geodistCommand(client *c);
void pfselftestCommand(client *c);
void pfaddCommand(client *c);
void pfcountCommand(client *c);
void pfmergeCommand(client *c);
void pfdebugCommand(client *c);
void latencyCommand(client *c);
void securityWarningCommand(client *c);

#if defined(__GNUC__)
void *calloc(size_t count, size_t size) __attribute__ ((deprecated));
void free(void *ptr) __attribute__ ((deprecated));
void *malloc(size_t size) __attribute__ ((deprecated));
void *realloc(void *ptr, size_t size) __attribute__ ((deprecated));
#endif

/* Debugging stuff */
void _serverAssertWithInfo(client *c, robj *o, char *estr, char *file, int line);
void _serverAssert(char *estr, char *file, int line);
void _serverPanic(char *msg, char *file, int line);
void bugReportStart(void);
void serverLogObjectDebugInfo(robj *o);
void sigsegvHandler(int sig, siginfo_t *info, void *secret);
sds genRedisInfoString(char *section);
void enableWatchdog(int period);
void disableWatchdog(void);
void watchdogScheduleSignal(int period);
void serverLogHexDump(int level, char *descr, void *value, size_t len);
int memtest_preserving_test(unsigned long *m, size_t bytes, int passes);

#define redisDebug(fmt, ...) \
    printf("DEBUG %s:%d > " fmt "\n", __FILE__, __LINE__, __VA_ARGS__)
#define redisDebugMark() \
    printf("-- MARK %s:%d --\n", __FILE__, __LINE__)

#endif
