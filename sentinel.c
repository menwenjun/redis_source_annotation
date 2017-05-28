/* Redis Sentinel implementation
 *  Redis 哨兵实现
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
#include "hiredis.h"
#include "async.h"

#include <ctype.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <fcntl.h>

extern char **environ;
// Redis哨兵默认的端口
#define REDIS_SENTINEL_PORT 26379

/* ======================== Sentinel global state =========================== */
// 哨兵全局状态
/* Address object, used to describe an ip:port pair. */
// 描述地址对象的结构
typedef struct sentinelAddr {
    char *ip;   //IP
    int port;   //port
} sentinelAddr;

/* A Sentinel Redis Instance object is monitoring. */
// 每个被监控的节点实例的flags值。
// 被监控的节点可以是：主节点、从节点或Sentinel节点

// 实例是主节点
#define SRI_MASTER  (1<<0)
// 实例是从节点
#define SRI_SLAVE   (1<<1)
// 实例是Sentinel
#define SRI_SENTINEL (1<<2)
// 实例处于主观下线状态
#define SRI_S_DOWN (1<<3)   /* Subjectively down (no quorum). */
// 实例处于客观下线状态
#define SRI_O_DOWN (1<<4)   /* Objectively down (confirmed by others). */
// Sentinel认为主节点下线
#define SRI_MASTER_DOWN (1<<5) /* A Sentinel with this flag set thinks that
                                   its master is down. */
// 正在进行主节点的故障转移
#define SRI_FAILOVER_IN_PROGRESS (1<<6) /* Failover is in progress for
                                           this master. */
// 实例是被选中的新主节点，但仍然是从节点
#define SRI_PROMOTED (1<<7)            /* Slave selected for promotion. */
// 向从节点发送SLAVEOF命令
#define SRI_RECONF_SENT (1<<8)     /* SLAVEOF <newmaster> sent. */
// 正在同步主节点的从节点
#define SRI_RECONF_INPROG (1<<9)   /* Slave synchronization in progress. */
// 从节点完成同步主节点
#define SRI_RECONF_DONE (1<<10)     /* Slave synchronized with new master. */
// 强制对主节点进行故障迁移操作
#define SRI_FORCE_FAILOVER (1<<11)  /* Force failover with master up. */
// 发送SCRIPT KILL命令给返回 -BUSY 的节点
#define SRI_SCRIPT_KILL_SENT (1<<12) /* SCRIPT KILL already sent on -BUSY */

/* Note: times are in milliseconds. */
#define SENTINEL_INFO_PERIOD 10000
#define SENTINEL_PING_PERIOD 1000
#define SENTINEL_ASK_PERIOD 1000
// publish命令发布的周期
#define SENTINEL_PUBLISH_PERIOD 2000
#define SENTINEL_DEFAULT_DOWN_AFTER 30000
#define SENTINEL_HELLO_CHANNEL "__sentinel__:hello"
#define SENTINEL_TILT_TRIGGER 2000
#define SENTINEL_TILT_PERIOD (SENTINEL_PING_PERIOD*30)
#define SENTINEL_DEFAULT_SLAVE_PRIORITY 100
#define SENTINEL_SLAVE_RECONF_TIMEOUT 10000
#define SENTINEL_DEFAULT_PARALLEL_SYNCS 1
#define SENTINEL_MIN_LINK_RECONNECT_PERIOD 15000
#define SENTINEL_DEFAULT_FAILOVER_TIMEOUT (60*3*1000)
#define SENTINEL_MAX_PENDING_COMMANDS 100
#define SENTINEL_ELECTION_TIMEOUT 10000
#define SENTINEL_MAX_DESYNC 1000

/* Failover machine different states. */
// 没有执行故障转移
#define SENTINEL_FAILOVER_STATE_NONE 0  /* No failover in progress. */
// 等待故障转移开始
#define SENTINEL_FAILOVER_STATE_WAIT_START 1  /* Wait for failover_start_time*/
// 选择晋升的从节点
#define SENTINEL_FAILOVER_STATE_SELECT_SLAVE 2 /* Select slave to promote */
// 发送slaveof no one ，使从节点变为主节点
#define SENTINEL_FAILOVER_STATE_SEND_SLAVEOF_NOONE 3 /* Slave -> Master */
// 等待从节点去晋升为主节点
#define SENTINEL_FAILOVER_STATE_WAIT_PROMOTION 4 /* Wait slave to change role */
// 给所有的从节点发送SLAVEOF newmaster命令，取复制新的主节点
#define SENTINEL_FAILOVER_STATE_RECONF_SLAVES 5 /* SLAVEOF newmaster */
// 监控晋升的从节点，更新一些配置
#define SENTINEL_FAILOVER_STATE_UPDATE_CONFIG 6 /* Monitor promoted slave. */

#define SENTINEL_MASTER_LINK_STATUS_UP 0
#define SENTINEL_MASTER_LINK_STATUS_DOWN 1

/* Generic flags that can be used with different functions.
 * They use higher bits to avoid colliding with the function specific
 * flags. */
#define SENTINEL_NO_FLAGS 0
#define SENTINEL_GENERATE_EVENT (1<<16)
#define SENTINEL_LEADER (1<<17)
#define SENTINEL_OBSERVER (1<<18)

/* Script execution flags and limits. */
#define SENTINEL_SCRIPT_NONE 0
#define SENTINEL_SCRIPT_RUNNING 1
#define SENTINEL_SCRIPT_MAX_QUEUE 256
#define SENTINEL_SCRIPT_MAX_RUNNING 16
#define SENTINEL_SCRIPT_MAX_RUNTIME 60000 /* 60 seconds max exec time. */
#define SENTINEL_SCRIPT_MAX_RETRY 10
#define SENTINEL_SCRIPT_RETRY_DELAY 30000 /* 30 seconds between retries. */

/* SENTINEL SIMULATE-FAILURE command flags. */
#define SENTINEL_SIMFAILURE_NONE 0
// 当选择完晋升的从节点之后发生模拟故障
#define SENTINEL_SIMFAILURE_CRASH_AFTER_ELECTION (1<<0)
// 当晋升从节点之后发生模拟故障
#define SENTINEL_SIMFAILURE_CRASH_AFTER_PROMOTION (1<<1)

/* The link to a sentinelRedisInstance. When we have the same set of Sentinels
 * monitoring many masters, we have different instances representing the
 * same Sentinels, one per master, and we need to share the hiredis connections
 * among them. Oherwise if 5 Sentinels are monitoring 100 masters we create
 * 500 outgoing connections instead of 5.
 *
 * So this structure represents a reference counted link in terms of the two
 * hiredis connections for commands and Pub/Sub, and the fields needed for
 * failure detection, since the ping/pong time are now local to the link: if
 * the link is available, the instance is avaialbe. This way we don't just
 * have 5 connections instead of 500, we also send 5 pings instead of 500.
 *
 * Links are shared only for Sentinels: master and slave instances have
 * a link with refcount = 1, always. */

// 连接到sentinelRedisInstance。当我们有一组Sentinel集合监控这许多主节点时，我们允许有不同实例表示相同的Sentinel，每一个主节点一个，我们需要在他们之中共享hiredis连接。否则如果有5个Sentinel在监控100个主节点，我们需要创建500个连接，而不是5个。

// 因此，这个结构代表了命令和Pub/Sub的两个hiredis连接以及故障检测所需的字段的引用计数链接，因为ping/pong次数会局部的：如果连接可用，则实例是可达的（available）。这样我们不止有5个连接而是500个，发送5个PING而是500个

// 连接只能被Sentinels共享，主节点和从节点实例的连接引用计数总为1

typedef struct instanceLink {
    // sentinelRedisInstance被共有的次数
    int refcount;          /* Number of sentinelRedisInstance owners. */
    // 是否需要重新连接cc或pc
    int disconnected;      /* Non-zero if we need to reconnect cc or pc. */
    // 已发送但是未回复命令的数量
    int pending_commands;  /* Number of commands sent waiting for a reply. */
    // 用于发送命令的异步连接
    redisAsyncContext *cc; /* Hiredis context for commands. */
    // 用于Pub / Sub的的异步连接
    // 仅在实例为主节点时使用
    redisAsyncContext *pc; /* Hiredis context for Pub / Sub. */
    // cc连接创建的时间
    mstime_t cc_conn_time; /* cc connection time. */
    // pc连接创建的时间
    mstime_t pc_conn_time; /* pc connection time. */
    // 最近一次接收到信息的时间
    mstime_t pc_last_activity; /* Last time we received any message. */
    // 实例最近一次返回正确PING命令回复的时间
    mstime_t last_avail_time; /* Last time the instance replied to ping with
                                 a reply we consider valid. */
    // 实例最近一次发送PING命令的时间（没有接收到PONG），当收到PONG时，被设置为0.
    // 如果act_ping_time为0，且一个PING被发送，则设置act_ping_time为当前时间
    mstime_t act_ping_time;   /* Time at which the last pending ping (no pong
                                 received after it) was sent. This field is
                                 set to 0 when a pong is received, and set again
                                 to the current time if the value is 0 and a new
                                 ping is sent. */
    // 最近一次发送PING的时间，用于避免在故障时发送太多的PING命令，使用act_ping_time字段来计算空转时间
    mstime_t last_ping_time;  /* Time at which we sent the last ping. This is
                                 only used to avoid sending too many pings
                                 during failure. Idle time is computed using
                                 the act_ping_time field. */
    // 最近一次实例回复PING命令的时间，无论回复是什么。被用来检查是否连接空转和重新建立连接
    mstime_t last_pong_time;  /* Last time the instance replied to ping,
                                 whatever the reply was. That's used to check
                                 if the link is idle and must be reconnected. */
    // 当连接中断后最近一次尝试重连的时间
    mstime_t last_reconn_time;  /* Last reconnection attempt performed when
                                   the link was down. */
} instanceLink;

typedef struct sentinelRedisInstance {
    // 标识值，记录了当前Redis实例的类型和状态
    int flags;      /* See SRI_... defines */
    // 实例的名字
    // 主节点的名字由用户在配置文件中设置
    // 从节点以及Sentinel节点的名字由Sentinel自动设置，格式为：ip:port
    char *name;     /* Master name from the point of view of this sentinel. */
    // 实例运行的独一无二ID
    char *runid;    /* Run ID of this instance, or unique ID if is a Sentinel.*/
    // 配置纪元，用于实现故障转移
    uint64_t config_epoch;  /* Configuration epoch. */
    // 实例地址：ip和port
    sentinelAddr *addr; /* Master host. */
    // 实例的连接，有可能是被Sentinel共享的
    instanceLink *link; /* Link to the instance, may be shared for Sentinels. */
    // 最近一次通过 Pub/Sub 发送信息的时间
    mstime_t last_pub_time;   /* Last time we sent hello via Pub/Sub. */
    // 只有被Sentinel实例使用
    // 最近一次接收到从Sentinel发送来hello的时间
    mstime_t last_hello_time; /* Only used if SRI_SENTINEL is set. Last time
                                 we received a hello from this Sentinel
                                 via Pub/Sub. */
    // 最近一次回复SENTINEL is-master-down的时间
    mstime_t last_master_down_reply_time; /* Time of last reply to
                                             SENTINEL is-master-down command. */
    // 实例被判断为主观下线的时间
    mstime_t s_down_since_time; /* Subjectively down since time. */
    // 实例被判断为客观下线的时间
    mstime_t o_down_since_time; /* Objectively down since time. */
    // 实例无响应多少毫秒之后被判断为主观下线
    // 由SENTINEL down-after-millisenconds配置设定
    mstime_t down_after_period; /* Consider it down after that period. */
    // 从实例获取INFO命令回复的时间
    mstime_t info_refresh;  /* Time at which we received INFO output from it. */

    /* Role and the first time we observed it.
     * This is useful in order to delay replacing what the instance reports
     * with our own configuration. We need to always wait some time in order
     * to give a chance to the leader to report the new configuration before
     * we do silly things. */
    // 实例的角色
    int role_reported;
    // 角色更新的时间
    mstime_t role_reported_time;
    // 最近一次从节点的主节点地址变更的时间
    mstime_t slave_conf_change_time; /* Last time slave master addr changed. */

    /* Master specific. */
    /*----------------------------------主节点特有的属性----------------------------------*/
    // 其他监控相同主节点的Sentinel
    dict *sentinels;    /* Other sentinels monitoring the same master. */
    // 如果当前实例是主节点，那么slaves保存着该主节点的所有从节点实例
    // 键是从节点命令，值是从节点服务器对应的sentinelRedisInstance
    dict *slaves;       /* Slaves for this master instance. */
    // 判定该主节点客观下线的投票数
    // 由SENTINEL monitor <master-name> <ip> <port> <quorum>配置
    unsigned int quorum;/* Number of sentinels that need to agree on failure. */
    // 在故障转移时，可以同时对新的主节点进行同步的从节点数量
    // 由sentinel parallel-syncs <master-name> <number>配置
    int parallel_syncs; /* How many slaves to reconfigure at same time. */
    // 连接主节点和从节点的认证密码
    char *auth_pass;    /* Password to use for AUTH against master & slaves. */

    /* Slave specific. */
    /*----------------------------------从节点特有的属性----------------------------------*/
    // 从节点复制操作断开时间
    mstime_t master_link_down_time; /* Slave replication link down time. */
    // 按照INFO命令输出的从节点优先级
    int slave_priority; /* Slave priority according to its INFO output. */
    // 故障转移时，从节点发送SLAVEOF <new>命令的时间
    mstime_t slave_reconf_sent_time; /* Time at which we sent SLAVE OF <new> */
    // 如果当前实例是从节点，那么保存该从节点连接的主节点实例
    struct sentinelRedisInstance *master; /* Master instance if it's slave. */
    // INFO命令的回复中记录的主节点的IP
    char *slave_master_host;    /* Master host as reported by INFO */
    // INFO命令的回复中记录的主节点的port
    int slave_master_port;      /* Master port as reported by INFO */
    // INFO命令的回复中记录的主从服务器连接的状态
    int slave_master_link_status; /* Master link status as reported by INFO */
    // 从节点复制偏移量
    unsigned long long slave_repl_offset; /* Slave replication offset. */

    /* Failover */
    /*----------------------------------故障转移的属性----------------------------------*/
    // 如果这是一个主节点实例，那么leader保存的是执行故障转移的Sentinel的runid
    // 如果这是一个Sentinel实例，那么leader保存的是当前这个Sentinel实例选举出来的领头的runid
    char *leader;       /* If this is a master instance, this is the runid of
                           the Sentinel that should perform the failover. If
                           this is a Sentinel, this is the runid of the Sentinel
                           that this Sentinel voted as leader. */
    // leader字段的纪元
    uint64_t leader_epoch; /* Epoch of the 'leader' field. */
    // 当前执行故障转移的纪元
    uint64_t failover_epoch; /* Epoch of the currently started failover. */
    // 故障转移操作的状态
    int failover_state; /* See SENTINEL_FAILOVER_STATE_* defines. */
    // 故障转移操作状态改变的时间
    mstime_t failover_state_change_time;
    // 最近一次故障转移尝试开始的时间
    mstime_t failover_start_time;   /* Last failover attempt start time. */
    // 更新故障转移状态的最大超时时间
    mstime_t failover_timeout;      /* Max time to refresh failover state. */
    // 记录故障转移延迟的时间
    mstime_t failover_delay_logged; /* For what failover_start_time value we
                                       logged the failover delay. */
    // 晋升为新主节点的从节点实例
    struct sentinelRedisInstance *promoted_slave; /* Promoted slave instance. */
    /* Scripts executed to notify admin or reconfigure clients: when they
     * are set to NULL no script is executed. */
    // 通知admin的可执行脚本的地址，如果设置为空，则没有执行的脚本
    char *notification_script;
    // 通知配置的client的可执行脚本的地址，如果设置为空，则没有执行的脚本
    char *client_reconfig_script;
    // 缓存INFO命令的输出
    sds info; /* cached INFO output */
} sentinelRedisInstance;

/* Main state. */
struct sentinelState {
    // Sentinel的id，41字节长的字符串
    char myid[CONFIG_RUN_ID_SIZE+1]; /* This sentinel ID. */
    // 当前纪元
    uint64_t current_epoch;         /* Current epoch. */
    // 监控的主节点字典
    // 键是主节点实例的名字
    // 值是主节点一个指向 sentinelRedisInstance 结构的指针
    dict *masters;      /* Dictionary of master sentinelRedisInstances.
                           Key is the instance name, value is the
                           sentinelRedisInstance structure pointer. */
    // 是否在TILT模式，该模式只收集数据，不做故障切换fail-over
    int tilt;           /* Are we in TILT mode? */
    // 当前正在执行的脚本的数量
    int running_scripts;    /* Number of scripts in execution right now. */
    // TILT模式开始的时间
    mstime_t tilt_start_time;       /* When TITL started. */
    // 最后一次执行时间处理程序的时间
    mstime_t previous_time;         /* Last time we ran the time handler. */
    // 要执行用户脚本的队列
    list *scripts_queue;            /* Queue of user scripts to execute. */

    // 多个 Sentinel 进程（progress）之间使用流言协议（gossip protocols)来接收关于主服务器是否下线的信息， 并使用投票协议（agreement protocols）来决定是否执行自动故障迁移， 以及选择哪个从服务器作为新的主服务器。
    // 被流言(gossip)到其他Sentinel的ip地址
    char *announce_ip;  /* IP addr that is gossiped to other sentinels if
                           not NULL. */
    // 被流言(gossip)到其他Sentinel的port
    int announce_port;  /* Port that is gossiped to other sentinels if
                           non zero. */
    // 故障模拟
    unsigned long simfailure_flags; /* Failures simulation. */
} sentinel;

/* A script execution job. */
typedef struct sentinelScriptJob {
    // 脚本的标志，记录脚本执行的限制
    int flags;              /* Script job flags: SENTINEL_SCRIPT_* */
    // 脚本执行的次数
    int retry_num;          /* Number of times we tried to execute it. */
    // 调用脚本的参数
    char **argv;            /* Arguments to call the script. */
    // 执行脚本的时间
    mstime_t start_time;    /* Script execution time if the script is running,
                               otherwise 0 if we are allowed to retry the
                               execution at any time. If the script is not
                               running and it's not 0, it means: do not run
                               before the specified time. */
    // 脚本执行子进程的pid
    pid_t pid;              /* Script execution pid. */
} sentinelScriptJob;

/* ======================= hiredis ae.c adapters =============================
 * Note: this implementation is taken from hiredis/adapters/ae.h, however
 * we have our modified copy for Sentinel in order to use our allocator
 * and to have full control over how the adapter works. */
// hiredis客户端的适配器结构
typedef struct redisAeEvents {
    redisAsyncContext *context; //客户端连接的上下文
    aeEventLoop *loop;          //服务器的事件循环
    int fd;                     //文件描述符
    int reading, writing;       //读写事件是否就绪标志
} redisAeEvents;

// 读事件处理程序
static void redisAeReadEvent(aeEventLoop *el, int fd, void *privdata, int mask) {
    ((void)el); ((void)fd); ((void)mask);

    redisAeEvents *e = (redisAeEvents*)privdata;
    redisAsyncHandleRead(e->context);
}

// 写事件处理程序
static void redisAeWriteEvent(aeEventLoop *el, int fd, void *privdata, int mask) {
    ((void)el); ((void)fd); ((void)mask);

    redisAeEvents *e = (redisAeEvents*)privdata;
    redisAsyncHandleWrite(e->context);
}

// 在事件循环中创建一个文件事件，并设置可读事件的处理程序为redisAeReadEvent
static void redisAeAddRead(void *privdata) {
    redisAeEvents *e = (redisAeEvents*)privdata;
    aeEventLoop *loop = e->loop;
    if (!e->reading) {
        e->reading = 1;
        aeCreateFileEvent(loop,e->fd,AE_READABLE,redisAeReadEvent,e);
    }
}

// 从事件循环中取消监听一个文件事件的可读事件
static void redisAeDelRead(void *privdata) {
    redisAeEvents *e = (redisAeEvents*)privdata;
    aeEventLoop *loop = e->loop;
    if (e->reading) {
        e->reading = 0;
        aeDeleteFileEvent(loop,e->fd,AE_READABLE);
    }
}

// 在事件循环中创建一个文件事件，并设置可写事件的处理程序为redisAeReadEvent
static void redisAeAddWrite(void *privdata) {
    redisAeEvents *e = (redisAeEvents*)privdata;
    aeEventLoop *loop = e->loop;
    if (!e->writing) {
        e->writing = 1;
        aeCreateFileEvent(loop,e->fd,AE_WRITABLE,redisAeWriteEvent,e);
    }
}

// 从事件循环中取消监听一个文件事件的可写事件
static void redisAeDelWrite(void *privdata) {
    redisAeEvents *e = (redisAeEvents*)privdata;
    aeEventLoop *loop = e->loop;
    if (e->writing) {
        e->writing = 0;
        aeDeleteFileEvent(loop,e->fd,AE_WRITABLE);
    }
}

// 从事件循环中取消监听一个文件事件的所有事件
static void redisAeCleanup(void *privdata) {
    redisAeEvents *e = (redisAeEvents*)privdata;
    redisAeDelRead(privdata);
    redisAeDelWrite(privdata);
    zfree(e);
}

// 为上下文ac和事件循环loop创建hiredis客户端的适配器
static int redisAeAttach(aeEventLoop *loop, redisAsyncContext *ac) {
    redisContext *c = &(ac->c);
    redisAeEvents *e;

    /* Nothing should be attached when something is already attached */
    if (ac->ev.data != NULL)
        return C_ERR;

    /* Create container for context and r/w events */
    // 创建一个适配器，并初始化
    e = (redisAeEvents*)zmalloc(sizeof(*e));
    e->context = ac;
    e->loop = loop;
    e->fd = c->fd;
    e->reading = e->writing = 0;

    /* Register functions to start/stop listening for events */
    // 设置异步调用的函数
    ac->ev.addRead = redisAeAddRead;
    ac->ev.delRead = redisAeDelRead;
    ac->ev.addWrite = redisAeAddWrite;
    ac->ev.delWrite = redisAeDelWrite;
    ac->ev.cleanup = redisAeCleanup;
    ac->ev.data = e;

    return C_OK;
}

/* ============================= Prototypes ================================= */

void sentinelLinkEstablishedCallback(const redisAsyncContext *c, int status);
void sentinelDisconnectCallback(const redisAsyncContext *c, int status);
void sentinelReceiveHelloMessages(redisAsyncContext *c, void *reply, void *privdata);
sentinelRedisInstance *sentinelGetMasterByName(char *name);
char *sentinelGetSubjectiveLeader(sentinelRedisInstance *master);
char *sentinelGetObjectiveLeader(sentinelRedisInstance *master);
int yesnotoi(char *s);
void instanceLinkConnectionError(const redisAsyncContext *c);
const char *sentinelRedisInstanceTypeStr(sentinelRedisInstance *ri);
void sentinelAbortFailover(sentinelRedisInstance *ri);
void sentinelEvent(int level, char *type, sentinelRedisInstance *ri, const char *fmt, ...);
sentinelRedisInstance *sentinelSelectSlave(sentinelRedisInstance *master);
void sentinelScheduleScriptExecution(char *path, ...);
void sentinelStartFailover(sentinelRedisInstance *master);
void sentinelDiscardReplyCallback(redisAsyncContext *c, void *reply, void *privdata);
int sentinelSendSlaveOf(sentinelRedisInstance *ri, char *host, int port);
char *sentinelVoteLeader(sentinelRedisInstance *master, uint64_t req_epoch, char *req_runid, uint64_t *leader_epoch);
void sentinelFlushConfig(void);
void sentinelGenerateInitialMonitorEvents(void);
int sentinelSendPing(sentinelRedisInstance *ri);
int sentinelForceHelloUpdateForMaster(sentinelRedisInstance *master);
sentinelRedisInstance *getSentinelRedisInstanceByAddrAndRunID(dict *instances, char *ip, int port, char *runid);
void sentinelSimFailureCrash(void);

/* ========================= Dictionary types =============================== */

unsigned int dictSdsHash(const void *key);
int dictSdsKeyCompare(void *privdata, const void *key1, const void *key2);
void releaseSentinelRedisInstance(sentinelRedisInstance *ri);

void dictInstancesValDestructor (void *privdata, void *obj) {
    UNUSED(privdata);
    releaseSentinelRedisInstance(obj);
}

/* Instance name (sds) -> instance (sentinelRedisInstance pointer)
 *
 * also used for: sentinelRedisInstance->sentinels dictionary that maps
 * sentinels ip:port to last seen time in Pub/Sub hello message. */

// 实例的命令(sds) 映射 实例(sentinelRedisInstance结构指针)
// sentinelRedisInstance的sentinels字典，字典的键是Sentinel的ip:port，字典的值是Sentinel最后一次向频道发送信息的时间
dictType instancesDictType = {
    dictSdsHash,               /* hash function */
    NULL,                      /* key dup */
    NULL,                      /* val dup */
    dictSdsKeyCompare,         /* key compare */
    NULL,                      /* key destructor */
    dictInstancesValDestructor /* val destructor */
};

/* Instance runid (sds) -> votes (long casted to void*)
 *
 * This is useful into sentinelGetObjectiveLeader() function in order to
 * count the votes and understand who is the leader. */

// 实例的runid(sds) 映射 votes(一个整型的投票数，强制转换为void*类型)
// 用于sentinelGetObjectiveLeader() 函数去计算投票数和了解哪个Sentinel是领导者
dictType leaderVotesDictType = {
    dictSdsHash,               /* hash function */
    NULL,                      /* key dup */
    NULL,                      /* val dup */
    dictSdsKeyCompare,         /* key compare */
    NULL,                      /* key destructor */
    NULL                       /* val destructor */
};

/* =========================== Initialization =============================== */

void sentinelCommand(client *c);
void sentinelInfoCommand(client *c);
void sentinelSetCommand(client *c);
void sentinelPublishCommand(client *c);
void sentinelRoleCommand(client *c);

// Sentinel的命令表
struct redisCommand sentinelcmds[] = {
    {"ping",pingCommand,1,"",0,NULL,0,0,0,0,0},
    {"sentinel",sentinelCommand,-2,"",0,NULL,0,0,0,0,0},
    {"subscribe",subscribeCommand,-2,"",0,NULL,0,0,0,0,0},
    {"unsubscribe",unsubscribeCommand,-1,"",0,NULL,0,0,0,0,0},
    {"psubscribe",psubscribeCommand,-2,"",0,NULL,0,0,0,0,0},
    {"punsubscribe",punsubscribeCommand,-1,"",0,NULL,0,0,0,0,0},
    {"publish",sentinelPublishCommand,3,"",0,NULL,0,0,0,0,0},
    {"info",sentinelInfoCommand,-1,"",0,NULL,0,0,0,0,0},
    {"role",sentinelRoleCommand,1,"l",0,NULL,0,0,0,0,0},
    {"client",clientCommand,-2,"rs",0,NULL,0,0,0,0,0},
    {"shutdown",shutdownCommand,-1,"",0,NULL,0,0,0,0,0}
};

/* This function overwrites a few normal Redis config default with Sentinel
 * specific defaults. */
// 设置Sentinel的默认端口，覆盖服务器的默认属性
void initSentinelConfig(void) {
    server.port = REDIS_SENTINEL_PORT;
}

/* Perform the Sentinel mode initialization. */
// 执行Sentinel模式的初始化操作
void initSentinel(void) {
    unsigned int j;

    /* Remove usual Redis commands from the command table, then just add
     * the SENTINEL command. */
    // 将服务器的命令表清空
    dictEmpty(server.commands,NULL);
    // 只添加Sentinel模式的相关命令
    for (j = 0; j < sizeof(sentinelcmds)/sizeof(sentinelcmds[0]); j++) {
        int retval;
        struct redisCommand *cmd = sentinelcmds+j;

        retval = dictAdd(server.commands, sdsnew(cmd->name), cmd);
        serverAssert(retval == DICT_OK);
    }

    /* Initialize various data structures. */
    // 初始化各种Sentinel状态的数据结构

    // 当前纪元，用于实现故障转移操作
    sentinel.current_epoch = 0;
    // 监控的主节点信息的字典
    sentinel.masters = dictCreate(&instancesDictType,NULL);
    // TILT模式
    sentinel.tilt = 0;
    sentinel.tilt_start_time = 0;
    // 最后执行时间处理程序的时间
    sentinel.previous_time = mstime();
    // 正在执行的脚本数量
    sentinel.running_scripts = 0;
    // 用户脚本的队列
    sentinel.scripts_queue = listCreate();
    // Sentinel通过流言协议接收关于主服务器的ip和port
    sentinel.announce_ip = NULL;
    sentinel.announce_port = 0;
    // 故障模拟
    sentinel.simfailure_flags = SENTINEL_SIMFAILURE_NONE;
    // Sentinel的ID置为0
    memset(sentinel.myid,0,sizeof(sentinel.myid));
}

/* This function gets called when the server is in Sentinel mode, started,
 * loaded the configuration, and is ready for normal operations. */
// 这个函数在服务器以Sentinel模式启动时被调用，载入配置，并且准备好一些操作
void sentinelIsRunning(void) {
    int j;

    // 配置文件路径为空，退出程序
    if (server.configfile == NULL) {
        serverLog(LL_WARNING,
            "Sentinel started without a config file. Exiting...");
        exit(1);
    // 检查配置文件的权限是否可写，否则退出程序
    } else if (access(server.configfile,W_OK) == -1) {
        serverLog(LL_WARNING,
            "Sentinel config file %s is not writable: %s. Exiting...",
            server.configfile,strerror(errno));
        exit(1);
    }

    /* If this Sentinel has yet no ID set in the configuration file, we
     * pick a random one and persist the config on disk. From now on this
     * will be this Sentinel ID across restarts. */
    // 如果Sentinel在配置文件中没有添加ID，则随机生成一个保存到文件中
    for (j = 0; j < CONFIG_RUN_ID_SIZE; j++)
        if (sentinel.myid[j] != 0) break;

    // 如果没有ID
    if (j == CONFIG_RUN_ID_SIZE) {
        /* Pick ID and presist the config. */
        // 通过SHA1算法随机生成一个ID
        getRandomHexChars(sentinel.myid,CONFIG_RUN_ID_SIZE);
        // 保存到配置文件中
        sentinelFlushConfig();
    }

    /* Log its ID to make debugging of issues simpler. */
    serverLog(LL_WARNING,"Sentinel ID is %s", sentinel.myid);

    /* We want to generate a +monitor event for every configured master
     * at startup. */
    // Sentinel启动时，生成一个"+monitor"事件
    sentinelGenerateInitialMonitorEvents();
}

/* ============================== sentinelAddr ============================== */

/* Create a sentinelAddr object and return it on success.
 * On error NULL is returned and errno is set to:
 *  ENOENT: Can't resolve the hostname.
 *  EINVAL: Invalid port number.
 */
// 返回一个的sentinelAddr对象。如果失败则返回NULL，且设置errno
// ENOENT：不能解析到主机地址
// EINVAL：port值非法
sentinelAddr *createSentinelAddr(char *hostname, int port) {
    char ip[NET_IP_STR_LEN];
    sentinelAddr *sa;
    // 检查port
    if (port < 0 || port > 65535) {
        errno = EINVAL;
        return NULL;
    }
    // 解析hostname，保存地址到ip中
    if (anetResolve(NULL,hostname,ip,sizeof(ip)) == ANET_ERR) {
        errno = ENOENT;
        return NULL;
    }
    // 分配一个sentinelAddr对象，并设置ip和port
    sa = zmalloc(sizeof(*sa));
    sa->ip = sdsnew(ip);
    sa->port = port;
    return sa;
}

/* Return a duplicate of the source address. */
// 返回一个源地址的复制品
sentinelAddr *dupSentinelAddr(sentinelAddr *src) {
    sentinelAddr *sa;
    // 重新创建一个sentinelAddr对象
    sa = zmalloc(sizeof(*sa));
    sa->ip = sdsnew(src->ip);
    sa->port = src->port;
    return sa;
}

/* Free a Sentinel address. Can't fail. */
// 释放Sentinel地址
void releaseSentinelAddr(sentinelAddr *sa) {
    sdsfree(sa->ip);
    zfree(sa);
}

/* Return non-zero if two addresses are equal. */
// 比较两个地址是否相同。相同返回1，不同返回0
int sentinelAddrIsEqual(sentinelAddr *a, sentinelAddr *b) {
    return a->port == b->port && !strcasecmp(a->ip,b->ip);
}

/* =========================== Events notification ========================== */

/* Send an event to log, pub/sub, user notification script.
 *
 * 'level' is the log level for logging. Only LL_WARNING events will trigger
 * the execution of the user notification script.
 *
 * 'type' is the message type, also used as a pub/sub channel name.
 *
 * 'ri', is the redis instance target of this event if applicable, and is
 * used to obtain the path of the notification script to execute.
 *
 * The remaining arguments are printf-alike.
 * If the format specifier starts with the two characters "%@" then ri is
 * not NULL, and the message is prefixed with an instance identifier in the
 * following format:
 *
 *  <instance type> <instance name> <ip> <port>
 *
 *  If the instance type is not master, than the additional string is
 *  added to specify the originating master:
 *
 *  @ <master name> <master ip> <master port>
 *
 *  Any other specifier after "%@" is processed by printf itself.
 */
/*=========================== 事件通知 ==========================*/
/*
    发送一个事件到日志，订阅的频道，用户的通知脚本中
    level   是日志的级别，只有LL_WARNING级别的日志才会触发用户通知脚本的执行
    type    是消息类型，也被用来作为频道的名字
    ri      是引发事件的Redis实例，被用来执行通知脚本的路径
    剩下的参数和printf的格式化输出格式类似
    如果格式是以 "%@" 两个字符开头的字符，并且 'ri' 不为空，那么这个消息将以一下字符串为前缀
        <instance type> <instance name> <ip> <port>
    如果实例类型不是主节点，那么一下信息会被添加到后面
        @ <master name> <master ip> <master port>
    "%@"之后的任何其他的说明符都和printf的一样
*/
void sentinelEvent(int level, char *type, sentinelRedisInstance *ri,
                   const char *fmt, ...) {
    va_list ap;
    char msg[LOG_MAX_LEN];
    robj *channel, *payload;

    /* Handle %@ */
    // 先处理"%@"说明符
    if (fmt[0] == '%' && fmt[1] == '@') {
        // 如果ri是一个主节点实例，那么master=NULL
        // 否则ri是一个从节点实例或Sentinel实例，那么master被设置为主节点结构的指针
        sentinelRedisInstance *master = (ri->flags & SRI_MASTER) ?
                                         NULL : ri->master;
        // 如果ri不是主节点实例，那么会打印主节点的属性
        if (master) {
            snprintf(msg, sizeof(msg), "%s %s %s %d @ %s %s %d",
                // ri的类型，slave 或者 Sentinel
                sentinelRedisInstanceTypeStr(ri),
                // ri的名字，ip，port
                ri->name, ri->addr->ip, ri->addr->port,
                // ri的主节点的命令，ip，port
                master->name, master->addr->ip, master->addr->port);
        // ri是主节点实例
        } else {
            snprintf(msg, sizeof(msg), "%s %s %s %d",
                // ri的类型：master
                sentinelRedisInstanceTypeStr(ri),
                // ri的名字，ip，port
                ri->name, ri->addr->ip, ri->addr->port);
        }
        fmt += 2;
    } else {
        msg[0] = '\0';
    }

    /* Use vsprintf for the rest of the formatting if any. */
    // 将其余的信息打印到msg中
    if (fmt[0] != '\0') {
        va_start(ap, fmt);
        vsnprintf(msg+strlen(msg), sizeof(msg)-strlen(msg), fmt, ap);
        va_end(ap);
    }

    /* Log the message if the log level allows it to be logged. */
    // 日志级别高于设置的日志级别限制，则打印日志
    if (level >= server.verbosity)
        serverLog(level,"%s %s",type,msg);

    /* Publish the message via Pub/Sub if it's not a debugging one. */
    // 如果不是DEBUG级别的日志，那么发送msg到type指定的频道中
    if (level != LL_DEBUG) {
        channel = createStringObject(type,strlen(type));
        payload = createStringObject(msg,strlen(msg));
        pubsubPublishMessage(channel,payload);
        decrRefCount(channel);
        decrRefCount(payload);
    }

    /* Call the notification script if applicable. */
    // 如果可以，则要执行通知脚本
    if (level == LL_WARNING && ri != NULL) {
        sentinelRedisInstance *master = (ri->flags & SRI_MASTER) ?
                                         ri : ri->master;
        // 执行主节点的通知脚本
        if (master && master->notification_script) {
            sentinelScheduleScriptExecution(master->notification_script,
                type,msg,NULL);
        }
    }
}

/* This function is called only at startup and is used to generate a
 * +monitor event for every configured master. The same events are also
 * generated when a master to monitor is added at runtime via the
 * SENTINEL MONITOR command. */
// 该函数只在服务器启动的时候被调用，被用来形成一个"+monitor"事件。
void sentinelGenerateInitialMonitorEvents(void) {
    dictIterator *di;
    dictEntry *de;

    di = dictGetIterator(sentinel.masters);
    // 遍历Sentinel监视所有的主节点
    while((de = dictNext(di)) != NULL) {
        sentinelRedisInstance *ri = dictGetVal(de);
        sentinelEvent(LL_WARNING,"+monitor",ri,"%@ quorum %d",ri->quorum);
    }
    dictReleaseIterator(di);
}

/* ============================ script execution ============================ */

/* Release a script job structure and all the associated data. */
// 释放一个脚本任务结构和所有关联的数据
void sentinelReleaseScriptJob(sentinelScriptJob *sj) {
    int j = 0;
    // 释放脚本的参数
    while(sj->argv[j]) sdsfree(sj->argv[j++]);
    zfree(sj->argv);
    zfree(sj);
}

#define SENTINEL_SCRIPT_MAX_ARGS 16
// 将给定参数和脚本放入用户脚本队列中
void sentinelScheduleScriptExecution(char *path, ...) {
    va_list ap;
    char *argv[SENTINEL_SCRIPT_MAX_ARGS+1];
    int argc = 1;
    sentinelScriptJob *sj;

    va_start(ap, path);
    // 将参数保存到argv中
    while(argc < SENTINEL_SCRIPT_MAX_ARGS) {
        argv[argc] = va_arg(ap,char*);
        if (!argv[argc]) break;
        argv[argc] = sdsnew(argv[argc]); /* Copy the string. */
        argc++;
    }
    va_end(ap);
    // 第一个参数是脚本的路径
    argv[0] = sdsnew(path);
    // 分配脚本任务结构的空间
    sj = zmalloc(sizeof(*sj));
    sj->flags = SENTINEL_SCRIPT_NONE;           //脚本限制
    sj->retry_num = 0;                          //执行次数
    sj->argv = zmalloc(sizeof(char*)*(argc+1)); //参数列表
    sj->start_time = 0;                         //开始时间
    sj->pid = 0;                                //执行脚本子进程的pid
    // 设置脚本的参数列表
    memcpy(sj->argv,argv,sizeof(char*)*(argc+1));
    // 添加到脚本队列中
    listAddNodeTail(sentinel.scripts_queue,sj);

    /* Remove the oldest non running script if we already hit the limit. */
    // 如果队列长度大于256个，那么删除最旧的脚本，只保留255个
    if (listLength(sentinel.scripts_queue) > SENTINEL_SCRIPT_MAX_QUEUE) {
        listNode *ln;
        listIter li;

        listRewind(sentinel.scripts_queue,&li);
        // 遍历脚本链表队列
        while ((ln = listNext(&li)) != NULL) {
            sj = ln->value;
            // 跳过正在执行的脚本
            if (sj->flags & SENTINEL_SCRIPT_RUNNING) continue;
            /* The first node is the oldest as we add on tail. */
            // 删除最旧的脚本
            listDelNode(sentinel.scripts_queue,ln);
            // 释放一个脚本任务结构和所有关联的数据
            sentinelReleaseScriptJob(sj);
            break;
        }
        serverAssert(listLength(sentinel.scripts_queue) <=
                    SENTINEL_SCRIPT_MAX_QUEUE);
    }
}

/* Lookup a script in the scripts queue via pid, and returns the list node
 * (so that we can easily remove it from the queue if needed). */
// 根据pid查找并返回正在运行的脚本节点
listNode *sentinelGetScriptListNodeByPid(pid_t pid) {
    listNode *ln;
    listIter li;

    listRewind(sentinel.scripts_queue,&li);
    // 遍历脚本链表队列
    while ((ln = listNext(&li)) != NULL) {
        sentinelScriptJob *sj = ln->value;
        // 如果正在执行该脚本，且等于pid，则返回节点地址
        if ((sj->flags & SENTINEL_SCRIPT_RUNNING) && sj->pid == pid)
            return ln;
    }
    return NULL;
}

/* Run pending scripts if we are not already at max number of running
 * scripts. */
// 执行在队列中等待的脚本，如果没有超过同一时刻最多运行脚本的数量
void sentinelRunPendingScripts(void) {
    listNode *ln;
    listIter li;
    mstime_t now = mstime();

    /* Find jobs that are not running and run them, from the top to the
     * tail of the queue, so we run older jobs first. */
    listRewind(sentinel.scripts_queue,&li);
    // 遍历脚本链表队列，如果没有超过同一时刻最多运行脚本的数量，找到没有正在运行的脚本
    while (sentinel.running_scripts < SENTINEL_SCRIPT_MAX_RUNNING &&
           (ln = listNext(&li)) != NULL)
    {
        sentinelScriptJob *sj = ln->value;
        pid_t pid;

        /* Skip if already running. */
        // 跳过正在运行的脚本
        if (sj->flags & SENTINEL_SCRIPT_RUNNING) continue;

        /* Skip if it's a retry, but not enough time has elapsed. */
        // 该脚本没有到达重新执行的时间，跳过
        if (sj->start_time && sj->start_time > now) continue;

        // 设置正在执行标志
        sj->flags |= SENTINEL_SCRIPT_RUNNING;
        // 开始执行时间
        sj->start_time = mstime();
        // 执行次数加1
        sj->retry_num++;
        // 创建子进程执行
        pid = fork();

        // fork()失败，报告错误
        if (pid == -1) {
            /* Parent (fork error).
             * We report fork errors as signal 99, in order to unify the
             * reporting with other kind of errors. */
            sentinelEvent(LL_WARNING,"-script-error",NULL,
                          "%s %d %d", sj->argv[0], 99, 0);
            sj->flags &= ~SENTINEL_SCRIPT_RUNNING;
            sj->pid = 0;
        // 子进程执行的代码
        } else if (pid == 0) {
            /* Child */
            // 执行该脚本
            execve(sj->argv[0],sj->argv,environ);
            /* If we are here an error occurred. */
            // 如果执行_exit(2)，表示发生了错误，不能重新执行
            _exit(2); /* Don't retry execution. */
        // 父进程，更新脚本的pid，和同时执行脚本的个数
        } else {
            sentinel.running_scripts++;
            sj->pid = pid;
            // 并且通知事件
            sentinelEvent(LL_DEBUG,"+script-child",NULL,"%ld",(long)pid);
        }
    }
}

/* How much to delay the execution of a script that we need to retry after
 * an error?
 *
 * We double the retry delay for every further retry we do. So for instance
 * if RETRY_DELAY is set to 30 seconds and the max number of retries is 10
 * starting from the second attempt to execute the script the delays are:
 * 30 sec, 60 sec, 2 min, 4 min, 8 min, 16 min, 32 min, 64 min, 128 min. */
// 计算重试脚本前的延迟时间
mstime_t sentinelScriptRetryDelay(int retry_num) {
    mstime_t delay = SENTINEL_SCRIPT_RETRY_DELAY;   //最大延迟时间30000ms，30s

    //delay =       30 sec,  60 sec, 2 min,  4 min,  8 min,  16 min, 32 min, 64 min, 128 min
    //retry_num  =  1        2       3       4       5       6       7       8       9
    while (retry_num-- > 1) delay *= 2;
    return delay;
}

/* Check for scripts that terminated, and remove them from the queue if the
 * script terminated successfully. If instead the script was terminated by
 * a signal, or returned exit code "1", it is scheduled to run again if
 * the max number of retries did not already elapsed. */
// 检查终止的脚本，如果脚本成功退出，则从队列中删除。如果脚本被一个信号终止，退出码为1，则他可以被安排下一次重新执行
// 当时要在最多执行脚本限制的数量下
void sentinelCollectTerminatedScripts(void) {
    int statloc;
    pid_t pid;

    // 接受子进程退出码
    // WNOHANG：如果没有子进程退出，则立刻返回
    while ((pid = wait3(&statloc,WNOHANG,NULL)) > 0) {
        int exitcode = WEXITSTATUS(statloc);
        int bysignal = 0;
        listNode *ln;
        sentinelScriptJob *sj;
        // 获取造成脚本终止的信号
        if (WIFSIGNALED(statloc)) bysignal = WTERMSIG(statloc);
        sentinelEvent(LL_DEBUG,"-script-child",NULL,"%ld %d %d",
            (long)pid, exitcode, bysignal);
        // 根据pid查找并返回正在运行的脚本节点
        ln = sentinelGetScriptListNodeByPid(pid);
        if (ln == NULL) {
            serverLog(LL_WARNING,"wait3() returned a pid (%ld) we can't find in our scripts execution queue!", (long)pid);
            continue;
        }
        sj = ln->value;

        /* If the script was terminated by a signal or returns an
         * exit code of "1" (that means: please retry), we reschedule it
         * if the max number of retries is not already reached. */
        // 如果退出码是1并且没到脚本最大的重试数量
        if ((bysignal || exitcode == 1) &&
            sj->retry_num != SENTINEL_SCRIPT_MAX_RETRY)
        {   // 取消正在执行的标志
            sj->flags &= ~SENTINEL_SCRIPT_RUNNING;
            sj->pid = 0;
            // 设置下次执行脚本的时间
            sj->start_time = mstime() +
                             sentinelScriptRetryDelay(sj->retry_num);
        // 脚本不能重新执行
        } else {
            /* Otherwise let's remove the script, but log the event if the
             * execution did not terminated in the best of the ways. */
            // 发送脚本错误的事件通知
            if (bysignal || exitcode != 0) {
                sentinelEvent(LL_WARNING,"-script-error",NULL,
                              "%s %d %d", sj->argv[0], bysignal, exitcode);
            }
            // 从脚本队列中删除脚本
            listDelNode(sentinel.scripts_queue,ln);
            // 释放一个脚本任务结构和所有关联的数据
            sentinelReleaseScriptJob(sj);
            // 目前正在执行脚本的数量减1
            sentinel.running_scripts--;
        }
    }
}

/* Kill scripts in timeout, they'll be collected by the
 * sentinelCollectTerminatedScripts() function. */
// 杀死执行超时的脚本，这些脚本会被sentinelCollectTerminatedScripts()收集
void sentinelKillTimedoutScripts(void) {
    listNode *ln;
    listIter li;
    mstime_t now = mstime();

    listRewind(sentinel.scripts_queue,&li);
    // 遍历脚本队列
    while ((ln = listNext(&li)) != NULL) {
        sentinelScriptJob *sj = ln->value;
        // 如果当前脚本正在执行且执行，且脚本执行的时间超过60s
        if (sj->flags & SENTINEL_SCRIPT_RUNNING &&
            (now - sj->start_time) > SENTINEL_SCRIPT_MAX_RUNTIME)
        {   // 发送脚本超时的事件
            sentinelEvent(LL_WARNING,"-script-timeout",NULL,"%s %ld",
                sj->argv[0], (long)sj->pid);
            // 杀死执行脚本的子进程
            kill(sj->pid,SIGKILL);
        }
    }
}

/* Implements SENTINEL PENDING-SCRIPTS command. */
// SENTINEL PENDING-SCRIPTS命令的工具
// 打印脚本队列的所有脚本的状态
void sentinelPendingScriptsCommand(client *c) {
    listNode *ln;
    listIter li;

    // 将脚本队列的长度回复给client
    addReplyMultiBulkLen(c,listLength(sentinel.scripts_queue));
    listRewind(sentinel.scripts_queue,&li);
    // 遍历脚本队列
    while ((ln = listNext(&li)) != NULL) {
        sentinelScriptJob *sj = ln->value;
        int j = 0;

        addReplyMultiBulkLen(c,10);

        addReplyBulkCString(c,"argv");
        while (sj->argv[j]) j++;
        addReplyMultiBulkLen(c,j);
        j = 0;
        while (sj->argv[j]) addReplyBulkCString(c,sj->argv[j++]);

        addReplyBulkCString(c,"flags");
        addReplyBulkCString(c,
            (sj->flags & SENTINEL_SCRIPT_RUNNING) ? "running" : "scheduled");

        addReplyBulkCString(c,"pid");
        addReplyBulkLongLong(c,sj->pid);

        if (sj->flags & SENTINEL_SCRIPT_RUNNING) {
            addReplyBulkCString(c,"run-time");
            addReplyBulkLongLong(c,mstime() - sj->start_time);
        } else {
            mstime_t delay = sj->start_time ? (sj->start_time-mstime()) : 0;
            if (delay < 0) delay = 0;
            addReplyBulkCString(c,"run-delay");
            addReplyBulkLongLong(c,delay);
        }

        addReplyBulkCString(c,"retry-num");
        addReplyBulkLongLong(c,sj->retry_num);
    }
}

/* This function calls, if any, the client reconfiguration script with the
 * following parameters:
 *
 * <master-name> <role> <state> <from-ip> <from-port> <to-ip> <to-port>
 *
 * It is called every time a failover is performed.
 *
 * <state> is currently always "failover".
 * <role> is either "leader" or "observer".
 *
 * from/to fields are respectively master -> promoted slave addresses for
 * "start" and "end". */
// 这个函数执行时，用以下格式的参数调用客户端重新配置脚本
// <master-name> <role> <state> <from-ip> <from-port> <to-ip> <to-port>
// 该函数在每次故障转移时都会被执行
// <state>总是"failover"，而<role>是"leader" or "observer"
// from/to字段分别表示主节点和被晋升的从节点的地址
void sentinelCallClientReconfScript(sentinelRedisInstance *master, int role, char *state, sentinelAddr *from, sentinelAddr *to) {
    char fromport[32], toport[32];
    // 如果主节点没有配置通知client的脚本路径则直接返回
    if (master->client_reconfig_script == NULL) return;
    // 字符串源端口
    ll2string(fromport,sizeof(fromport),from->port);
    // 字符串目的端口
    ll2string(toport,sizeof(toport),to->port);
    // 将给定参数和脚本放入用户脚本队列中，等待执行
    sentinelScheduleScriptExecution(master->client_reconfig_script,
        master->name,
        (role == SENTINEL_LEADER) ? "leader" : "observer",
        state, from->ip, fromport, to->ip, toport, NULL);
}

/* =============================== instanceLink ============================= */

/* Create a not yet connected link object. */
// 创建一个没有网络连接的连接对象
instanceLink *createInstanceLink(void) {
    instanceLink *link = zmalloc(sizeof(*link));

    link->refcount = 1;
    link->disconnected = 1;     //没有网络连接
    link->pending_commands = 0;
    link->cc = NULL;
    link->pc = NULL;
    link->cc_conn_time = 0;
    link->pc_conn_time = 0;
    link->last_reconn_time = 0;
    link->pc_last_activity = 0;
    /* We set the act_ping_time to "now" even if we actually don't have yet
     * a connection with the node, nor we sent a ping.
     * This is useful to detect a timeout in case we'll not be able to connect
     * with the node at all. */
    // 设置act_ping_time为当前时间，即使我们没有连接节点，也没有发送PING
    // 这被用来察觉超时以防我们无法连接节点
    link->act_ping_time = mstime();
    link->last_ping_time = 0;
    link->last_avail_time = mstime();
    link->last_pong_time = mstime();
    return link;
}

/* Disconnect an hiredis connection in the context of an instance link. */
// 断开一个与hiredis的连接
void instanceLinkCloseConnection(instanceLink *link, redisAsyncContext *c) {
    if (c == NULL) return;

    // 如果是用于发送命令的异步连接则设为空
    if (link->cc == c) {
        link->cc = NULL;
        link->pending_commands = 0;
    }
    // 如果是用于发布订阅命令的异步连接则设为空
    if (link->pc == c) link->pc = NULL;
    c->data = NULL;
    link->disconnected = 1;
    // 释放异步连接
    redisAsyncFree(c);
}

/* Decrement the refcount of a link object, if it drops to zero, actually
 * free it and return NULL. Otherwise don't do anything and return the pointer
 * to the object.
 *
 * If we are not going to free the link and ri is not NULL, we rebind all the
 * pending requests in link->cc (hiredis connection for commands) to a
 * callback that will just ignore them. This is useful to avoid processing
 * replies for an instance that no longer exists. */
// 将一个连接对象的引用计数键1，如果减为0，则释放连接对象返回NULL，否则不做任何事返回连接对象的指针
// 如果ri不是NULL，而且不会释放连接对象，我们重新绑定所有link中指定的连接
instanceLink *releaseInstanceLink(instanceLink *link, sentinelRedisInstance *ri)
{
    serverAssert(link->refcount > 0);
    // link引用计数减1
    link->refcount--;
    // 如果引用计数不为0
    if (link->refcount != 0) {
        if (ri && ri->link->cc) {
            /* This instance may have pending callbacks in the hiredis async
             * context, having as 'privdata' the instance that we are going to
             * free. Let's rewrite the callback list, directly exploiting
             * hiredis internal data structures, in order to bind them with
             * a callback that will ignore the reply at all. */
            // 该实例可能在hiredis异步环境中有未决的回调函数，该函数可以释放实例的'privdata'
            // 重写回调函数的列表，直接利用hiredis内部的数据结构，以便绑定一个忽略回复的回调函数
            redisCallback *cb;
            // 回复回调函数单链表
            redisCallbackList *callbacks = &link->cc->replies;

            cb = callbacks->head;
            while(cb) {
                // 如果当前实例ri和'privdata'相等
                if (cb->privdata == ri) {
                    // 绑定一个忽略回复的回调函数
                    cb->fn = sentinelDiscardReplyCallback;
                    cb->privdata = NULL; /* Not strictly needed. */
                }
                cb = cb->next;
            }
        }
        return link; /* Other active users. */
    }
    // 关闭连接
    instanceLinkCloseConnection(link,link->cc);
    instanceLinkCloseConnection(link,link->pc);
    zfree(link);
    return NULL;
}

/* This function will attempt to share the instance link we already have
 * for the same Sentinel in the context of a different master, with the
 * instance we are passing as argument.
 *
 * This way multiple Sentinel objects that refer all to the same physical
 * Sentinel instance but in the context of different masters will use
 * a single connection, will send a single PING per second for failure
 * detection and so forth.
 *
 * Return C_OK if a matching Sentinel was found in the context of a
 * different master and sharing was performed. Otherwise C_ERR
 * is returned. */
// 函数尝试共享ri实例的连接，我们已经有在不同主节点上下文中的同一Sentinel
// 多个Sentinel对象引用相同的物理Sentinel实例，但是在不同主节点的上下文中使用相同的单个连接，每秒发送PING检测故障等等
// 如果在一个不同主节点的上下文中找到一个匹配的Sentinel，返回C_OK并且执行共享操作。否则返回C_ERR
int sentinelTryConnectionSharing(sentinelRedisInstance *ri) {
    // 针对Sentinel实例操作
    serverAssert(ri->flags & SRI_SENTINEL);
    dictIterator *di;
    dictEntry *de;
    // 无法识别
    if (ri->runid == NULL) return C_ERR; /* No way to identify it. */
    // 已经共享
    if (ri->link->refcount > 1) return C_ERR; /* Already shared. */

    di = dictGetIterator(sentinel.masters);
    // 遍历Sentinel所监控的主节点
    while((de = dictNext(di)) != NULL) {
        // 获取主节点的实例
        sentinelRedisInstance *master = dictGetVal(de), *match;
        /* We want to share with the same physical Sentinel referenced
         * in other masters, so skip our master. */
        // 我们想去共享相同的物理的被其他主节点引用的Sentinel节点，跳过ri实例自己的主节点
        if (master == ri->master) continue;
        // 其他监控该主节点的Sentinel中找到和ri实例runid相同的实例
        match = getSentinelRedisInstanceByAddrAndRunID(master->sentinels,
                                                       NULL,0,ri->runid);
        // 没找到匹配的，跳过
        if (match == NULL) continue; /* No match. */
        // 应该从不执行，为了安全
        if (match == ri) continue; /* Should never happen but... safer. */

        /* We identified a matching Sentinel, great! Let's free our link
         * and use the one of the matching Sentinel. */
        releaseInstanceLink(ri->link,NULL);
        // 设置共享连接对象
        ri->link = match->link;
        // 共享连接对象引用计数加1
        match->link->refcount++;
        return C_OK;
    }
    dictReleaseIterator(di);
    return C_ERR;
}

/* When we detect a Sentinel to switch address (reporting a different IP/port
 * pair in Hello messages), let's update all the matching Sentinels in the
 * context of other masters as well and disconnect the links, so that everybody
 * will be updated.
 *
 * Return the number of updated Sentinel addresses. */
// 当我们察觉一个Sentinel节点转换地址（在hello信息中报告了不同ip/port）
// 更新所有在不同主节点环境中的所有相同的Sentinel，并且断开连接，以便所有都可以被更新
int sentinelUpdateSentinelAddressInAllMasters(sentinelRedisInstance *ri) {
    // 针对Sentinel实例操作
    serverAssert(ri->flags & SRI_SENTINEL);
    dictIterator *di;
    dictEntry *de;
    int reconfigured = 0;

    di = dictGetIterator(sentinel.masters);
    // 遍历Sentinel所监控的主节点
    while((de = dictNext(di)) != NULL) {
        // 获取主节点的实例
        sentinelRedisInstance *master = dictGetVal(de), *match;
        // 从所有监控当前主节点的Sentinel中，找到一个与ri的runid相同的Sentinel实例
        match = getSentinelRedisInstanceByAddrAndRunID(master->sentinels,
                                                       NULL,0,ri->runid);
        /* If there is no match, this master does not know about this
         * Sentinel, try with the next one. */
        // 没找到，则当前主节点不认识该ri实例的Sentinel类型节点，跳过当前主节点
        if (match == NULL) continue;

        /* Disconnect the old links if connected. */
        // 断开命令连接
        if (match->link->cc != NULL)
            instanceLinkCloseConnection(match->link,match->link->cc);
        // 断开发布订阅的连接
        if (match->link->pc != NULL)
            instanceLinkCloseConnection(match->link,match->link->pc);
        // 如果找到的实例是ri自己，则跳过
        if (match == ri) continue; /* Address already updated for it. */

        /* Update the address of the matching Sentinel by copying the address
         * of the Sentinel object that received the address update. */
        // 释放该实例的地址
        releaseSentinelAddr(match->addr);
        // 拷贝ri实例的地址，更新找到的同runid的Sentinel实例的地址
        match->addr = dupSentinelAddr(ri->addr);
        // 重新配置次数加1
        reconfigured++;
    }
    dictReleaseIterator(di);
    // 只要重新配置了，就通知事件
    if (reconfigured)
        sentinelEvent(LL_NOTICE,"+sentinel-address-update", ri,
                    "%@ %d additional matching instances", reconfigured);
    // 返回重新配置的次数
    return reconfigured;
}

/* This function is called when an hiredis connection reported an error.
 * We set it to NULL and mark the link as disconnected so that it will be
 * reconnected again.
 *
 * Note: we don't free the hiredis context as hiredis will do it for us
 * for async connections. */
// 当hiredis连接报告一个错误，调用该函数。设置连接为空，并且设置重新连接的标志，以便它能重新连接
// 注意：不释放hiredis上下文，因为下次还要执行异步连接
void instanceLinkConnectionError(const redisAsyncContext *c) {
    instanceLink *link = c->data;
    int pubsub;

    if (!link) return;
    // 记录传入的c是否是pub/sub命令连接
    pubsub = (link->pc == c);
    // 将连接置空
    if (pubsub)
        link->pc = NULL;
    else
        link->cc = NULL;
    // 设置重新连接标志
    link->disconnected = 1;
}

/* Hiredis connection established / disconnected callbacks. We need them
 * just to cleanup our link state. */
// hiredis处理已确立的连接的回调函数
void sentinelLinkEstablishedCallback(const redisAsyncContext *c, int status) {
    if (status != C_OK) instanceLinkConnectionError(c);
}
// hiredis处理已断开的连接的回调函数
void sentinelDisconnectCallback(const redisAsyncContext *c, int status) {
    UNUSED(status);
    instanceLinkConnectionError(c);
}

/* ========================== sentinelRedisInstance ========================= */

/* Create a redis instance, the following fields must be populated by the
 * caller if needed:
 * runid: set to NULL but will be populated once INFO output is received.
 * info_refresh: is set to 0 to mean that we never received INFO so far.
 *
 * If SRI_MASTER is set into initial flags the instance is added to
 * sentinel.masters table.
 *
 * if SRI_SLAVE or SRI_SENTINEL is set then 'master' must be not NULL and the
 * instance is added into master->slaves or master->sentinels table.
 *
 * If the instance is a slave or sentinel, the name parameter is ignored and
 * is created automatically as hostname:port.
 *
 * The function fails if hostname can't be resolved or port is out of range.
 * When this happens NULL is returned and errno is set accordingly to the
 * createSentinelAddr() function.
 *
 * The function may also fail and return NULL with errno set to EBUSY if
 * a master with the same name, a slave with the same address, or a sentinel
 * with the same ID already exists. */
// 创建一个Redis实例，调用者应该设置以下参数
/*
    runid: 设置为空，但是被接收到的INFO命令的输出所设置
    info_refresh：如果设置为0，以为这从来没有都没有接收到INFO
*/
/*
    如果flags设置了SRI_MASTER，该实例被添加进sentinel.masters表中
    如果flags设置了SRI_SLAVE or SRI_SENTINEL，'master'一定不为空并且该实例被添加到master->slaves或master->sentinels中
    如果该实例是从节点或者是Sentinel节点，name参数被忽略，并且被自动设置为hostname:port
*/
// 如果hostname不能被解析或者端口号非法那么函数执行失败，返回null并且设置errno
// 如果一个有相同的名字主节点，或者一个有相同的地址从节点或者一个有相同的ID的Sentinel节点已经存在，那么函数执行失败，返回NULL，设置errno为EBUSY
sentinelRedisInstance *createSentinelRedisInstance(char *name, int flags, char *hostname, int port, int quorum, sentinelRedisInstance *master) {
    sentinelRedisInstance *ri;
    sentinelAddr *addr;
    dict *table = NULL;
    char slavename[NET_PEER_ID_LEN], *sdsname;

    serverAssert(flags & (SRI_MASTER|SRI_SLAVE|SRI_SENTINEL));
    // 如果该实例不是主节点，那么一定就是从节点或这Sentinel节点，那么master实例一定不为空
    serverAssert((flags & SRI_MASTER) || master != NULL);

    /* Check address validity. */
    // 创建地址对象
    addr = createSentinelAddr(hostname,port);
    if (addr == NULL) return NULL;

    /* For slaves use ip:port as name. */
    // 如果该实例是从节点，则将从节点的名字设置为 ip:port 格式的字符串
    if (flags & SRI_SLAVE) {
        anetFormatAddr(slavename, sizeof(slavename), hostname, port);
        name = slavename;
    }

    /* Make sure the entry is not duplicated. This may happen when the same
     * name for a master is used multiple times inside the configuration or
     * if we try to add multiple times a slave or sentinel with same ip/port
     * to a master. */
    // 相同name的主节点被多次设置或者添加了相同ip/port的从节点或Sentinel节点，就可能会出现重复添加的实例的情况
    // 为了避免这种情况，需要先检查实例是否存在

    // 如果master实例是主节点，获取当前Sentinel监控的主节点表
    if (flags & SRI_MASTER) table = sentinel.masters;
    // 如果master实例是从节点，获取与master实例所建立的从节点表
    else if (flags & SRI_SLAVE) table = master->slaves;
    // 如果master实例是sentinel节点，获取其他监控相同master主节点的Sentinel节点的字典
    else if (flags & SRI_SENTINEL) table = master->sentinels;
    sdsname = sdsnew(name);
    // 从对应的表中找到name的节点，如果实例已经存在则返回
    if (dictFind(table,sdsname)) {
        releaseSentinelAddr(addr);
        sdsfree(sdsname);
        errno = EBUSY;
        return NULL;
    }

    /* Create the instance object. */
    // 表中不存在该实例，则创建一个实例对象
    ri = zmalloc(sizeof(*ri));
    /* Note that all the instances are started in the disconnected state,
     * the event loop will take care of connecting them. */
    // 注意所有的实例在开始时都是断开网络连接的状态，在事件循环中会为他们创建连接
    ri->flags = flags;
    ri->name = sdsname;
    ri->runid = NULL;
    ri->config_epoch = 0;
    ri->addr = addr;
    ri->link = createInstanceLink();
    ri->last_pub_time = mstime();
    ri->last_hello_time = mstime();
    ri->last_master_down_reply_time = mstime();
    ri->s_down_since_time = 0;
    ri->o_down_since_time = 0;
    ri->down_after_period = master ? master->down_after_period :
                            SENTINEL_DEFAULT_DOWN_AFTER;
    ri->master_link_down_time = 0;
    ri->auth_pass = NULL;
    ri->slave_priority = SENTINEL_DEFAULT_SLAVE_PRIORITY;
    ri->slave_reconf_sent_time = 0;
    ri->slave_master_host = NULL;
    ri->slave_master_port = 0;
    ri->slave_master_link_status = SENTINEL_MASTER_LINK_STATUS_DOWN;
    ri->slave_repl_offset = 0;
    ri->sentinels = dictCreate(&instancesDictType,NULL);
    ri->quorum = quorum;
    ri->parallel_syncs = SENTINEL_DEFAULT_PARALLEL_SYNCS;
    ri->master = master;
    ri->slaves = dictCreate(&instancesDictType,NULL);
    ri->info_refresh = 0;

    /* Failover state. */
    ri->leader = NULL;
    ri->leader_epoch = 0;
    ri->failover_epoch = 0;
    ri->failover_state = SENTINEL_FAILOVER_STATE_NONE;
    ri->failover_state_change_time = 0;
    ri->failover_start_time = 0;
    ri->failover_timeout = SENTINEL_DEFAULT_FAILOVER_TIMEOUT;
    ri->failover_delay_logged = 0;
    ri->promoted_slave = NULL;
    ri->notification_script = NULL;
    ri->client_reconfig_script = NULL;
    ri->info = NULL;

    /* Role */
    ri->role_reported = ri->flags & (SRI_MASTER|SRI_SLAVE);
    ri->role_reported_time = mstime();
    ri->slave_conf_change_time = mstime();

    /* Add into the right table. */
    // 将新创建的实例添加到对应的字典中，键是名字，值是实例的地址
    dictAdd(table, ri->name, ri);
    return ri;
}

/* Release this instance and all its slaves, sentinels, hiredis connections.
 * This function does not take care of unlinking the instance from the main
 * masters table (if it is a master) or from its master sentinels/slaves table
 * if it is a slave or sentinel. */
// 释放该ri实例的所有从节点，Sentinel节点和hiredis的连接
// 如果该ri实例是一个从节点或Sentinel节点，那么他会从所属的主节点表中删除该实例，或者从它的sentinels/slaves表中删除
void releaseSentinelRedisInstance(sentinelRedisInstance *ri) {
    /* Release all its slaves or sentinels if any. */
    // 如果ri是什么实例，都释放所有从节点和Sentinel节点
    dictRelease(ri->sentinels);
    dictRelease(ri->slaves);

    /* Disconnect the instance. */
    // 断开cc和pc连接
    releaseInstanceLink(ri->link,ri);

    /* Free other resources. */
    // 释放所有的资源
    sdsfree(ri->name);
    sdsfree(ri->runid);
    sdsfree(ri->notification_script);
    sdsfree(ri->client_reconfig_script);
    sdsfree(ri->slave_master_host);
    sdsfree(ri->leader);
    sdsfree(ri->auth_pass);
    sdsfree(ri->info);
    releaseSentinelAddr(ri->addr);

    /* Clear state into the master if needed. */
    // 如果是准备晋升主节点的从节点，那么删除故障转移的状态
    if ((ri->flags & SRI_SLAVE) && (ri->flags & SRI_PROMOTED) && ri->master)
        ri->master->promoted_slave = NULL;

    zfree(ri);
}

/* Lookup a slave in a master Redis instance, by ip and port. */
// 通过ip和port从主节点实例中找到并返回该从节点
sentinelRedisInstance *sentinelRedisInstanceLookupSlave(
                sentinelRedisInstance *ri, char *ip, int port)
{
    sds key;
    sentinelRedisInstance *slave;
    char buf[NET_PEER_ID_LEN];

    serverAssert(ri->flags & SRI_MASTER);
    // 转换ip:prot字符串
    anetFormatAddr(buf,sizeof(buf),ip,port);
    key = sdsnew(buf);
    // 根据ip:prot的key找到对应的value并返回
    slave = dictFetchValue(ri->slaves,key);
    sdsfree(key);
    return slave;
}

/* Return the name of the type of the instance as a string. */
// 以字符串的形式返回实例的类型
const char *sentinelRedisInstanceTypeStr(sentinelRedisInstance *ri) {
    if (ri->flags & SRI_MASTER) return "master";
    else if (ri->flags & SRI_SLAVE) return "slave";
    else if (ri->flags & SRI_SENTINEL) return "sentinel";
    else return "unknown";
}

/* This function remove the Sentinel with the specified ID from the
 * specified master.
 *
 * If "runid" is NULL the function returns ASAP.
 *
 * This function is useful because on Sentinels address switch, we want to
 * remove our old entry and add a new one for the same ID but with the new
 * address.
 *
 * The function returns 1 if the matching Sentinel was removed, otherwise
 * 0 if there was no Sentinel with this ID. */
// 函数从指定的主节点中根据runid删除指定的Sentinel节点
// 如果runid为NULL，函数直接返回
// 在Sentinel节点进行地址转换时使用，删除旧的节点，添加一个ID相同的新节点并且设置新的地址
// 如果找到匹配的Sentinel节点被删除返回1，否则返回0
int removeMatchingSentinelFromMaster(sentinelRedisInstance *master, char *runid) {
    dictIterator *di;
    dictEntry *de;
    int removed = 0;

    if (runid == NULL) return 0;
    di = dictGetSafeIterator(master->sentinels);
    // 遍历监控该主节点的Sentinel节点
    while((de = dictNext(di)) != NULL) {
        // 获取当前Sentinel实例的指针
        sentinelRedisInstance *ri = dictGetVal(de);
        // 如果runid匹配，则删除当前Sentinel
        if (ri->runid && strcmp(ri->runid,runid) == 0) {
            dictDelete(master->sentinels,ri->name);
            // 计数删除的Sentinel节点个数
            removed++;
        }
    }
    dictReleaseIterator(di);
    // 返回删除的个数
    return removed;
}

/* Search an instance with the same runid, ip and port into a dictionary
 * of instances. Return NULL if not found, otherwise return the instance
 * pointer.
 *
 * runid or ip can be NULL. In such a case the search is performed only
 * by the non-NULL field. */
// 在实例字典中查找一个实例，根据指定的runid，ip和port，如果找不到返回NULL，否则返回实例指针
sentinelRedisInstance *getSentinelRedisInstanceByAddrAndRunID(dict *instances, char *ip, int port, char *runid) {
    dictIterator *di;
    dictEntry *de;
    sentinelRedisInstance *instance = NULL;

    serverAssert(ip || runid);   /* User must pass at least one search param. */
    di = dictGetIterator(instances);
    // 遍历实例字典
    while((de = dictNext(di)) != NULL) {
        // 获取实例指针
        sentinelRedisInstance *ri = dictGetVal(de);
        // 如果根据runid来查找，但是当前实例没有runid，则没跳过当前实例
        if (runid && !ri->runid) continue;
        // 如果runid相等或者IP和port相等
        if ((runid == NULL || strcmp(ri->runid, runid) == 0) &&
            (ip == NULL || (strcmp(ri->addr->ip, ip) == 0 &&
                            ri->addr->port == port)))
        {
            // 保存找到实例的地址
            instance = ri;
            break;
        }
    }
    dictReleaseIterator(di);
    return instance;
}

/* Master lookup by name */
// 根据name查找主节点
sentinelRedisInstance *sentinelGetMasterByName(char *name) {
    sentinelRedisInstance *ri;
    sds sdsname = sdsnew(name);
    // 从Sentinel所监视的所有主节点中寻找名字为name的主节点，找到返回
    ri = dictFetchValue(sentinel.masters,sdsname);
    sdsfree(sdsname);
    return ri;
}

/* Add the specified flags to all the instances in the specified dictionary. */
// 在instances字典中将flags指定的状态添加到所有的实例中
void sentinelAddFlagsToDictOfRedisInstances(dict *instances, int flags) {
    dictIterator *di;
    dictEntry *de;

    di = dictGetIterator(instances);
    // 遍历所有的实例
    while((de = dictNext(di)) != NULL) {
        sentinelRedisInstance *ri = dictGetVal(de);
        // 设置指定的flags
        ri->flags |= flags;
    }
    dictReleaseIterator(di);
}

/* Remove the specified flags to all the instances in the specified
 * dictionary. */
// 在instances字典中将flags指定的状态从到所有的实例中删除
void sentinelDelFlagsToDictOfRedisInstances(dict *instances, int flags) {
    dictIterator *di;
    dictEntry *de;

    di = dictGetIterator(instances);
    // 遍历所有的实例
    while((de = dictNext(di)) != NULL) {
        sentinelRedisInstance *ri = dictGetVal(de);
        // 删除指定的flags
        ri->flags &= ~flags;
    }
    dictReleaseIterator(di);
}

/* Reset the state of a monitored master:
 * 1) Remove all slaves.
 * 2) Remove all sentinels.
 * 3) Remove most of the flags resulting from runtime operations.
 * 4) Reset timers to their default value. For example after a reset it will be
 *    possible to failover again the same master ASAP, without waiting the
 *    failover timeout delay.
 * 5) In the process of doing this undo the failover if in progress.
 * 6) Disconnect the connections with the master (will reconnect automatically).
 */

// 重置被监控的主节点状态
/*
    1. 移除所有的从节点
    2. 移除所有监控自己的Sentinel节点
    3. 移除大多数运行时操作的状态
    4. 重置计时器为默认值，例如，重置计时器后相同的主节点可能会立即进行故障转移操作，不会等故障转移延迟的超时时间
    5. 如果故障转移正在执行，那么取消它
    6. 断开所有Sentinel节点与主节点的连接，之后会重新连接
*/
#define SENTINEL_RESET_NO_SENTINELS (1<<0)
void sentinelResetMaster(sentinelRedisInstance *ri, int flags) {
    serverAssert(ri->flags & SRI_MASTER);
    // 1. 移除所有的从节点，并创建一个新的从节点字典
    dictRelease(ri->slaves);
    ri->slaves = dictCreate(&instancesDictType,NULL);
    // 2. 移除所有监控自己的Sentinel节点，并创建一个新的监控自己的Sentinel字典
    if (!(flags & SENTINEL_RESET_NO_SENTINELS)) {
        dictRelease(ri->sentinels);
        ri->sentinels = dictCreate(&instancesDictType,NULL);
    }
    // 断开pc和cc连接
    instanceLinkCloseConnection(ri->link,ri->link->cc);
    instanceLinkCloseConnection(ri->link,ri->link->pc);
    // 重置主节点的属性
    ri->flags &= SRI_MASTER;
    if (ri->leader) {
        sdsfree(ri->leader);
        ri->leader = NULL;
    }
    ri->failover_state = SENTINEL_FAILOVER_STATE_NONE;
    ri->failover_state_change_time = 0;
    ri->failover_start_time = 0; /* We can failover again ASAP. */
    ri->promoted_slave = NULL;
    sdsfree(ri->runid);
    sdsfree(ri->slave_master_host);
    ri->runid = NULL;
    ri->slave_master_host = NULL;
    ri->link->act_ping_time = mstime();
    ri->link->last_ping_time = 0;
    ri->link->last_avail_time = mstime();
    ri->link->last_pong_time = mstime();
    ri->role_reported_time = mstime();
    ri->role_reported = SRI_MASTER;
    // 如果执行了生成事件标志，则生成"+reset-master"的事件
    if (flags & SENTINEL_GENERATE_EVENT)
        sentinelEvent(LL_WARNING,"+reset-master",ri,"%@");
}

/* Call sentinelResetMaster() on every master with a name matching the specified
 * pattern. */
// 重置指定pattern的主节点，返回重置主节点的个数
int sentinelResetMastersByPattern(char *pattern, int flags) {
    dictIterator *di;
    dictEntry *de;
    int reset = 0;

    di = dictGetIterator(sentinel.masters);
    // 遍历Sentinel监控的所有主节点
    while((de = dictNext(di)) != NULL) {
        sentinelRedisInstance *ri = dictGetVal(de);

        if (ri->name) {
            // 如果名字和指定的pattern相同，则重置该主节点
            if (stringmatch(pattern,ri->name,0)) {
                sentinelResetMaster(ri,flags);
                // 计数重置的次数
                reset++;
            }
        }
    }
    dictReleaseIterator(di);
    return reset;
}

/* Reset the specified master with sentinelResetMaster(), and also change
 * the ip:port address, but take the name of the instance unmodified.
 *
 * This is used to handle the +switch-master event.
 *
 * The function returns C_ERR if the address can't be resolved for some
 * reason. Otherwise C_OK is returned.  */
// 使用sentinelResetMaster()重置函数重置指定的主节点，并且改变其 ip:port 地址，但是保持名字不变
// 这个用来处理+switch-master事件
// 如果地址不能被解析，返回C_ERR，否则成功返回C_OK
int sentinelResetMasterAndChangeAddress(sentinelRedisInstance *master, char *ip, int port) {
    sentinelAddr *oldaddr, *newaddr;
    sentinelAddr **slaves = NULL;
    int numslaves = 0, j;
    dictIterator *di;
    dictEntry *de;

    // 创建ip:port地址字符串
    newaddr = createSentinelAddr(ip,port);
    if (newaddr == NULL) return C_ERR;

    /* Make a list of slaves to add back after the reset.
     * Don't include the one having the address we are switching to. */
    // 创建一个从节点表，将重置后的主节点添加到该表中
    // 不包含有我们要转换地址的那一个从节点
    di = dictGetIterator(master->slaves);
    // 遍历所有的从节点
    while((de = dictNext(di)) != NULL) {
        sentinelRedisInstance *slave = dictGetVal(de);
        // 如果当前从节点的地址和指定的地址相同，说明该从节点是要晋升为主节点的，因此跳过该从节点
        if (sentinelAddrIsEqual(slave->addr,newaddr)) continue;
        // 否则将该从节点加入到一个数组中
        slaves = zrealloc(slaves,sizeof(sentinelAddr*)*(numslaves+1));
        slaves[numslaves++] = createSentinelAddr(slave->addr->ip,
                                                 slave->addr->port);
    }
    dictReleaseIterator(di);

    /* If we are switching to a different address, include the old address
     * as a slave as well, so that we'll be able to sense / reconfigure
     * the old master. */
    // 如果指定的地址和主节点地址不相同，说明，该主节点是要被替换的，那么将该主节点地址加入到从节点数组中
    if (!sentinelAddrIsEqual(newaddr,master->addr)) {
        slaves = zrealloc(slaves,sizeof(sentinelAddr*)*(numslaves+1));
        slaves[numslaves++] = createSentinelAddr(master->addr->ip,
                                                 master->addr->port);
    }

    /* Reset and switch address. */
    // 重置主节点，但不删除所有监控自己的Sentinel节点
    sentinelResetMaster(master,SENTINEL_RESET_NO_SENTINELS);
    // 备份旧地址
    oldaddr = master->addr;
    // 设置新地址
    master->addr = newaddr;
    // 下线时间清零
    master->o_down_since_time = 0;
    master->s_down_since_time = 0;

    /* Add slaves back. */
    // 为新的主节点加入从节点
    for (j = 0; j < numslaves; j++) {
        sentinelRedisInstance *slave;
        // 遍历所有的从节点表，创建从节点实例，并将该实例从属到当前新的主节点中
        slave = createSentinelRedisInstance(NULL,SRI_SLAVE,slaves[j]->ip,
                    slaves[j]->port, master->quorum, master);
        // 释放原有的表中的从节点
        releaseSentinelAddr(slaves[j]);
        // 事件通知
        if (slave) sentinelEvent(LL_NOTICE,"+slave",slave,"%@");
    }
    // 释放从节点表
    zfree(slaves);

    /* Release the old address at the end so we are safe even if the function
     * gets the master->addr->ip and master->addr->port as arguments. */
    // 将原主节点地址释放
    releaseSentinelAddr(oldaddr);
    // 刷新配置文件
    sentinelFlushConfig();
    return C_OK;
}

/* Return non-zero if there was no SDOWN or ODOWN error associated to this
 * instance in the latest 'ms' milliseconds. */
// 在最近的'ms'毫秒内，如果ri实例没有出现下线状态（SDOWN or ODOWN），那么函数返回一个非零值
int sentinelRedisInstanceNoDownFor(sentinelRedisInstance *ri, mstime_t ms) {
    mstime_t most_recent;
    //记录被主观下线是时间
    most_recent = ri->s_down_since_time;
    // 如果客观下线的时间比主观下线的时间要近，则更新最近的下线状态时间
    if (ri->o_down_since_time > most_recent)
        most_recent = ri->o_down_since_time;
    // 返回是否在ms毫秒内下线
    return most_recent == 0 || (mstime() - most_recent) > ms;
}

/* Return the current master address, that is, its address or the address
 * of the promoted slave if already operational. */
// 返回当前主节点的地址，如果被晋升的从节点已经进行故障迁移，则返回新的主节点地址
sentinelAddr *sentinelGetCurrentMasterAddress(sentinelRedisInstance *master) {
    /* If we are failing over the master, and the state is already
     * SENTINEL_FAILOVER_STATE_RECONF_SLAVES or greater, it means that we
     * already have the new configuration epoch in the master, and the
     * slave acknowledged the configuration switch. Advertise the new
     * address. */
    // 如果主节点正在进行故障迁移，已经设置了新的主节点
    if ((master->flags & SRI_FAILOVER_IN_PROGRESS) &&
        master->promoted_slave &&
        master->failover_state >= SENTINEL_FAILOVER_STATE_RECONF_SLAVES)
    {
        // 返回新主节点的地址
        return master->promoted_slave->addr;
    } else {
        // 否则返回当前主节点的地址
        return master->addr;
    }
}

/* This function sets the down_after_period field value in 'master' to all
 * the slaves and sentinel instances connected to this master. */
// 根据master的down_after_period字段的值设置所有连接该主节点的从节点和Sentinel实例的对应字段
void sentinelPropagateDownAfterPeriod(sentinelRedisInstance *master) {
    dictIterator *di;
    dictEntry *de;
    int j;
    dict *d[] = {master->slaves, master->sentinels, NULL};

    for (j = 0; d[j]; j++) {
        di = dictGetIterator(d[j]);
        // 将所有的从节点和Sentinel节点的down_after_period都设置成master实例的down_after_period值
        while((de = dictNext(di)) != NULL) {
            sentinelRedisInstance *ri = dictGetVal(de);
            ri->down_after_period = master->down_after_period;
        }
        dictReleaseIterator(di);
    }
}

// 返回ri实例的角色
char *sentinelGetInstanceTypeString(sentinelRedisInstance *ri) {
    if (ri->flags & SRI_MASTER) return "master";
    else if (ri->flags & SRI_SLAVE) return "slave";
    else if (ri->flags & SRI_SENTINEL) return "sentinel";
    else return "unknown";
}

/* ============================ Config handling ============================= */
// 配置处理
char *sentinelHandleConfiguration(char **argv, int argc) {
    sentinelRedisInstance *ri;

    // SENTINEL monitor选项
    if (!strcasecmp(argv[0],"monitor") && argc == 5) {
        /* monitor <name> <host> <port> <quorum> */
        int quorum = atoi(argv[4]); //获取投票数
        // 投票数必须大于等于1
        if (quorum <= 0) return "Quorum must be 1 or greater.";
        // 创建一个主节点实例，并加入到Sentinel所监控的master字典中
        if (createSentinelRedisInstance(argv[1],SRI_MASTER,argv[2],
                                        atoi(argv[3]),quorum,NULL) == NULL)
        {
            switch(errno) {
            case EBUSY: return "Duplicated master name.";
            case ENOENT: return "Can't resolve master instance hostname.";
            case EINVAL: return "Invalid port number";
            }
        }

    // sentinel down-after-milliseconds选项
    } else if (!strcasecmp(argv[0],"down-after-milliseconds") && argc == 3) {
        /* down-after-milliseconds <name> <milliseconds> */
        // 获取根据name查找主节点实例
        ri = sentinelGetMasterByName(argv[1]);
        if (!ri) return "No such master with specified name.";
        // 设置主节点实例的主观下线的判断时间
        ri->down_after_period = atoi(argv[2]);
        if (ri->down_after_period <= 0)
            return "negative or zero time parameter.";
        // 根据ri主节点的down_after_period字段的值设置所有连接该主节点的从节点和Sentinel实例的主观下线的判断时间
        sentinelPropagateDownAfterPeriod(ri);

    // sentinel failover-timeout选项
    } else if (!strcasecmp(argv[0],"failover-timeout") && argc == 3) {
        /* failover-timeout <name> <milliseconds> */
        // 获取根据name查找主节点实例
        ri = sentinelGetMasterByName(argv[1]);
        if (!ri) return "No such master with specified name.";
        // 设置主节点更新故障转移的超时时间
        ri->failover_timeout = atoi(argv[2]);
        if (ri->failover_timeout <= 0)
            return "negative or zero time parameter.";

    // sentinel parallel-syncs
   } else if (!strcasecmp(argv[0],"parallel-syncs") && argc == 3) {
        /* parallel-syncs <name> <milliseconds> */
        // 获取根据name查找主节点实例
        ri = sentinelGetMasterByName(argv[1]);
        if (!ri) return "No such master with specified name.";
        // 设置同时与主节点进行同步的从节点数量
        ri->parallel_syncs = atoi(argv[2]);

    // sentinel notification-script
   } else if (!strcasecmp(argv[0],"notification-script") && argc == 3) {
        /* notification-script <name> <path> */
        // 获取根据name查找主节点实例
        ri = sentinelGetMasterByName(argv[1]);
        if (!ri) return "No such master with specified name.";
        // 判断该脚本是否有可执行权限
        if (access(argv[2],X_OK) == -1)
            return "Notification script seems non existing or non executable.";
        // 设置通知脚本的执行路径
        ri->notification_script = sdsnew(argv[2]);

    // sentinel client-reconfig-script
   } else if (!strcasecmp(argv[0],"client-reconfig-script") && argc == 3) {
        /* client-reconfig-script <name> <path> */
        // 获取根据name查找主节点实例
        ri = sentinelGetMasterByName(argv[1]);
        if (!ri) return "No such master with specified name.";
        // 判断该脚本是否有可执行权限
        if (access(argv[2],X_OK) == -1)
            return "Client reconfiguration script seems non existing or "
                   "non executable.";
        // 设置通知用户的可执行脚本的路径
        ri->client_reconfig_script = sdsnew(argv[2]);

    // sentinel auth-pass
   } else if (!strcasecmp(argv[0],"auth-pass") && argc == 3) {
        /* auth-pass <name> <password> */
        // 获取根据name查找主节点实例
        ri = sentinelGetMasterByName(argv[1]);
        if (!ri) return "No such master with specified name.";
        // 设置主节点的认证密码
        ri->auth_pass = sdsnew(argv[2]);

    // sentinel current-epoch
    } else if (!strcasecmp(argv[0],"current-epoch") && argc == 2) {
        /* current-epoch <epoch> */
        // 获取配置的纪元
        unsigned long long current_epoch = strtoull(argv[1],NULL,10);
        // 更新Sentinel的纪元
        if (current_epoch > sentinel.current_epoch)
            sentinel.current_epoch = current_epoch;

    // myid <40字节的id>
    } else if (!strcasecmp(argv[0],"myid") && argc == 2) {
        if (strlen(argv[1]) != CONFIG_RUN_ID_SIZE)
            return "Malformed Sentinel id in myid option.";
        // 设置Sentinel的ID
        memcpy(sentinel.myid,argv[1],CONFIG_RUN_ID_SIZE);

    // sentinel config-epoch
    } else if (!strcasecmp(argv[0],"config-epoch") && argc == 3) {
        /* config-epoch <name> <epoch> */
        // 获取根据name查找主节点实例
        ri = sentinelGetMasterByName(argv[1]);
        if (!ri) return "No such master with specified name.";
        // 设置配置纪元
        ri->config_epoch = strtoull(argv[2],NULL,10);
        /* The following update of current_epoch is not really useful as
         * now the current epoch is persisted on the config file, but
         * we leave this check here for redundancy. */
        // 更新当前Sentinel的纪元
        if (ri->config_epoch > sentinel.current_epoch)
            sentinel.current_epoch = ri->config_epoch;

    // sentinel leader-epoch
    } else if (!strcasecmp(argv[0],"leader-epoch") && argc == 3) {
        /* leader-epoch <name> <epoch> */
        // 获取根据name查找主节点实例
        ri = sentinelGetMasterByName(argv[1]);
        if (!ri) return "No such master with specified name.";
        // 设置leader的纪元
        ri->leader_epoch = strtoull(argv[2],NULL,10);

    // sentinel known-slave
    } else if (!strcasecmp(argv[0],"known-slave") && argc == 4) {
        sentinelRedisInstance *slave;

        /* known-slave <name> <ip> <port> */
        // 获取根据name查找主节点实例
        ri = sentinelGetMasterByName(argv[1]);
        if (!ri) return "No such master with specified name.";
        // 创建 ip:port 的从节点实例，加入到ri主节点的从节点字典中
        if ((slave = createSentinelRedisInstance(NULL,SRI_SLAVE,argv[2],
                    atoi(argv[3]), ri->quorum, ri)) == NULL)
        {
            return "Wrong hostname or port for slave.";
        }

    // sentinel known-sentinel
    } else if (!strcasecmp(argv[0],"known-sentinel") &&
               (argc == 4 || argc == 5)) {
        sentinelRedisInstance *si;

        // 忽略没有runid格式的配置
        if (argc == 5) { /* Ignore the old form without runid. */
            /* known-sentinel <name> <ip> <port> [runid] */
            // 获取根据name查找主节点实例
            ri = sentinelGetMasterByName(argv[1]);
            if (!ri) return "No such master with specified name.";
            // 创建一个Sentinel实例，加入到ri主节点的Sentinel节点字典中
            if ((si = createSentinelRedisInstance(argv[4],SRI_SENTINEL,argv[2],
                        atoi(argv[3]), ri->quorum, ri)) == NULL)
            {
                return "Wrong hostname or port for sentinel.";
            }
            // 设置Sentinel节点的runid
            si->runid = sdsnew(argv[4]);
            // 尝试与其他Sentinel节点共享连接对象
            sentinelTryConnectionSharing(si);
        }

    // sentinel announce-ip
    } else if (!strcasecmp(argv[0],"announce-ip") && argc == 2) {
        /* announce-ip <ip-address> */
        if (strlen(argv[1]))
            // 设置流言到其他Sentinel节点的ip
            sentinel.announce_ip = sdsnew(argv[1]);

    // sentinel announce-port
    } else if (!strcasecmp(argv[0],"announce-port") && argc == 2) {
        /* announce-port <port> */
        // 设置流言到其他Sentinel节点的port
        sentinel.announce_port = atoi(argv[1]);
    } else {
        return "Unrecognized sentinel configuration statement.";
    }
    return NULL;
}

/* Implements CONFIG REWRITE for "sentinel" option.
 * This is used not just to rewrite the configuration given by the user
 * (the configured masters) but also in order to retain the state of
 * Sentinel across restarts: config epoch of masters, associated slaves
 * and sentinel instances, and so forth. */
// CONFIG REWRITE 命令的实现，设置一些sentinel的选项
// 该函数不仅重写配置，而且还可以保存当前Sentinel的状态，以便Sentinel重启时载入
void rewriteConfigSentinelOption(struct rewriteConfigState *state) {
    dictIterator *di, *di2;
    dictEntry *de;
    sds line;

    /* sentinel unique ID. */
    // 重写sentinel myid 的配置
    line = sdscatprintf(sdsempty(), "sentinel myid %s", sentinel.myid);
    rewriteConfigRewriteLine(state,"sentinel",line,1);

    /* For every master emit a "sentinel monitor" config entry. */
    // 设置每一个主节点的配置
    di = dictGetIterator(sentinel.masters);
    while((de = dictNext(di)) != NULL) {
        sentinelRedisInstance *master, *ri;
        sentinelAddr *master_addr;

        /* sentinel monitor */
        master = dictGetVal(de);
        master_addr = sentinelGetCurrentMasterAddress(master);
        line = sdscatprintf(sdsempty(),"sentinel monitor %s %s %d %d",
            master->name, master_addr->ip, master_addr->port,
            master->quorum);
        rewriteConfigRewriteLine(state,"sentinel",line,1);

        /* sentinel down-after-milliseconds */
        if (master->down_after_period != SENTINEL_DEFAULT_DOWN_AFTER) {
            line = sdscatprintf(sdsempty(),
                "sentinel down-after-milliseconds %s %ld",
                master->name, (long) master->down_after_period);
            rewriteConfigRewriteLine(state,"sentinel",line,1);
        }

        /* sentinel failover-timeout */
        if (master->failover_timeout != SENTINEL_DEFAULT_FAILOVER_TIMEOUT) {
            line = sdscatprintf(sdsempty(),
                "sentinel failover-timeout %s %ld",
                master->name, (long) master->failover_timeout);
            rewriteConfigRewriteLine(state,"sentinel",line,1);
        }

        /* sentinel parallel-syncs */
        if (master->parallel_syncs != SENTINEL_DEFAULT_PARALLEL_SYNCS) {
            line = sdscatprintf(sdsempty(),
                "sentinel parallel-syncs %s %d",
                master->name, master->parallel_syncs);
            rewriteConfigRewriteLine(state,"sentinel",line,1);
        }

        /* sentinel notification-script */
        if (master->notification_script) {
            line = sdscatprintf(sdsempty(),
                "sentinel notification-script %s %s",
                master->name, master->notification_script);
            rewriteConfigRewriteLine(state,"sentinel",line,1);
        }

        /* sentinel client-reconfig-script */
        if (master->client_reconfig_script) {
            line = sdscatprintf(sdsempty(),
                "sentinel client-reconfig-script %s %s",
                master->name, master->client_reconfig_script);
            rewriteConfigRewriteLine(state,"sentinel",line,1);
        }

        /* sentinel auth-pass */
        if (master->auth_pass) {
            line = sdscatprintf(sdsempty(),
                "sentinel auth-pass %s %s",
                master->name, master->auth_pass);
            rewriteConfigRewriteLine(state,"sentinel",line,1);
        }

        /* sentinel config-epoch */
        line = sdscatprintf(sdsempty(),
            "sentinel config-epoch %s %llu",
            master->name, (unsigned long long) master->config_epoch);
        rewriteConfigRewriteLine(state,"sentinel",line,1);

        /* sentinel leader-epoch */
        line = sdscatprintf(sdsempty(),
            "sentinel leader-epoch %s %llu",
            master->name, (unsigned long long) master->leader_epoch);
        rewriteConfigRewriteLine(state,"sentinel",line,1);

        /* sentinel known-slave */
        di2 = dictGetIterator(master->slaves);
        while((de = dictNext(di2)) != NULL) {
            sentinelAddr *slave_addr;

            ri = dictGetVal(de);
            slave_addr = ri->addr;

            /* If master_addr (obtained using sentinelGetCurrentMasterAddress()
             * so it may be the address of the promoted slave) is equal to this
             * slave's address, a failover is in progress and the slave was
             * already successfully promoted. So as the address of this slave
             * we use the old master address instead. */
            if (sentinelAddrIsEqual(slave_addr,master_addr))
                slave_addr = master->addr;
            line = sdscatprintf(sdsempty(),
                "sentinel known-slave %s %s %d",
                master->name, slave_addr->ip, slave_addr->port);
            rewriteConfigRewriteLine(state,"sentinel",line,1);
        }
        dictReleaseIterator(di2);

        /* sentinel known-sentinel */
        di2 = dictGetIterator(master->sentinels);
        while((de = dictNext(di2)) != NULL) {
            ri = dictGetVal(de);
            if (ri->runid == NULL) continue;
            line = sdscatprintf(sdsempty(),
                "sentinel known-sentinel %s %s %d %s",
                master->name, ri->addr->ip, ri->addr->port, ri->runid);
            rewriteConfigRewriteLine(state,"sentinel",line,1);
        }
        dictReleaseIterator(di2);
    }

    /* sentinel current-epoch is a global state valid for all the masters. */
    line = sdscatprintf(sdsempty(),
        "sentinel current-epoch %llu", (unsigned long long) sentinel.current_epoch);
    rewriteConfigRewriteLine(state,"sentinel",line,1);

    /* sentinel announce-ip. */
    if (sentinel.announce_ip) {
        line = sdsnew("sentinel announce-ip ");
        line = sdscatrepr(line, sentinel.announce_ip, sdslen(sentinel.announce_ip));
        rewriteConfigRewriteLine(state,"sentinel",line,1);
    }

    /* sentinel announce-port. */
    if (sentinel.announce_port) {
        line = sdscatprintf(sdsempty(),"sentinel announce-port %d",
                            sentinel.announce_port);
        rewriteConfigRewriteLine(state,"sentinel",line,1);
    }

    dictReleaseIterator(di);
}

/* This function uses the config rewriting Redis engine in order to persist
 * the state of the Sentinel in the current configuration file.
 *
 * Before returning the function calls fsync() against the generated
 * configuration file to make sure changes are committed to disk.
 *
 * On failure the function logs a warning on the Redis log. */
// 将Sentinel的状态保存到当前的配置文件中，在函数返回之前，调用fsync()同步，确保文件被同步到磁盘中
// 如果失败，则返回写Redis日志
void sentinelFlushConfig(void) {
    int fd = -1;
    int saved_hz = server.hz;
    int rewrite_status;

    server.hz = CONFIG_DEFAULT_HZ;
    rewrite_status = rewriteConfig(server.configfile);
    server.hz = saved_hz;

    if (rewrite_status == -1) goto werr;
    if ((fd = open(server.configfile,O_RDONLY)) == -1) goto werr;
    if (fsync(fd) == -1) goto werr;
    if (close(fd) == EOF) goto werr;
    return;

werr:
    if (fd != -1) close(fd);
    serverLog(LL_WARNING,"WARNING: Sentinel was not able to save the new configuration on disk!!!: %s", strerror(errno));
}

/* ====================== hiredis connection handling ======================= */

/* Send the AUTH command with the specified master password if needed.
 * Note that for slaves the password set for the master is used.
 *
 * We don't check at all if the command was successfully transmitted
 * to the instance as if it fails Sentinel will detect the instance down,
 * will disconnect and reconnect the link and so forth. */
// 发送AUTH命令给指定的主节点，如果需要还有进行认证
// 我们不检查命令是否成功传输给ri实例，如果ri下线，Sentinel会察觉到并且重新进行连接
void sentinelSendAuthIfNeeded(sentinelRedisInstance *ri, redisAsyncContext *c) {
    // 获取验证密码
    char *auth_pass = (ri->flags & SRI_MASTER) ? ri->auth_pass :
                                                 ri->master->auth_pass;

    if (auth_pass) {
        // 异步向 c 发送 AUTH auth_pass 命令，调用sentinelDiscardReplyCallback来处理命令回复
        // 并设置已发送但未回复的命令数加1，因为该函数的异步的
        if (redisAsyncCommand(c, sentinelDiscardReplyCallback, ri, "AUTH %s",
            auth_pass) == C_OK) ri->link->pending_commands++;
    }
}

/* Use CLIENT SETNAME to name the connection in the Redis instance as
 * sentinel-<first_8_chars_of_runid>-<connection_type>
 * The connection type is "cmd" or "pubsub" as specified by 'type'.
 *
 * This makes it possible to list all the sentinel instances connected
 * to a Redis servewr with CLIENT LIST, grepping for a specific name format. */
// 使用CLIENT SETNAME设置ri实例连接的名字，格式：sentinel-<first_8_chars_of_runid>-<connection_type>
// type被指定为 cmd或者pubsub
// 这样做可以通过 CLIENT LIST 命令列出所有连接到服务器的Sentinel实例，过滤一个指定的名字格式
void sentinelSetClientName(sentinelRedisInstance *ri, redisAsyncContext *c, char *type) {
    char name[64];

    // 按照格式写name
    snprintf(name,sizeof(name),"sentinel-%.8s-%s",sentinel.myid,type);
    // 异步向 c 发送 CLIENT SETNAME 命令，调用sentinelDiscardReplyCallback来处理命令回复
    // 并设置已发送但未回复的命令数加1，因为该函数的异步的
    if (redisAsyncCommand(c, sentinelDiscardReplyCallback, ri,
        "CLIENT SETNAME %s", name) == C_OK)
    {
        ri->link->pending_commands++;
    }
}

/* Create the async connections for the instance link if the link
 * is disconnected. Note that link->disconnected is true even if just
 * one of the two links (commands and pub/sub) is missing. */
// 如果Sentinel和ri连接被中断，则为ri实例创建一个异步连接
void sentinelReconnectInstance(sentinelRedisInstance *ri) {
    // 如果ri实例没有连接中断，则直接返回
    if (ri->link->disconnected == 0) return;
    // ri实例地址非法
    if (ri->addr->port == 0) return; /* port == 0 means invalid address. */
    instanceLink *link = ri->link;
    mstime_t now = mstime();

    // 如果还没有最近一次重连的时间距离现在太短，小于1s，则直接返回
    if (now - ri->link->last_reconn_time < SENTINEL_PING_PERIOD) return;
    // 设置最近重连的时间
    ri->link->last_reconn_time = now;

    /* Commands connection. */
    // cc：命令连接
    if (link->cc == NULL) {
        // 绑定ri实例的连接地址并建立连接
        link->cc = redisAsyncConnectBind(ri->addr->ip,ri->addr->port,NET_FIRST_BIND_ADDR);
        // 命令连接失败，则事件通知，且断开cc连接
        if (link->cc->err) {
            sentinelEvent(LL_DEBUG,"-cmd-link-reconnection",ri,"%@ #%s",
                link->cc->errstr);
            instanceLinkCloseConnection(link,link->cc);
        // 命令连接成功
        } else {
            // 重置cc连接的属性
            link->pending_commands = 0;
            link->cc_conn_time = mstime();
            link->cc->data = link;
            // 将服务器的事件循环关联到cc连接的上下文中
            redisAeAttach(server.el,link->cc);
            // 设置确立连接的回调函数
            redisAsyncSetConnectCallback(link->cc,
                    sentinelLinkEstablishedCallback);
            // 设置断开连接的回调处理
            redisAsyncSetDisconnectCallback(link->cc,
                    sentinelDisconnectCallback);
            // 发送AUTH 命令认证
            sentinelSendAuthIfNeeded(ri,link->cc);
            // 发送连接名字
            sentinelSetClientName(ri,link->cc,"cmd");

            /* Send a PING ASAP when reconnecting. */
            // 立即向ri实例发送PING命令
            sentinelSendPing(ri);
        }
    }
    /* Pub / Sub */
    // pc：发布订阅连接
    // 只对主节点和从节点如果没有设置pc连接则建立一个
    if ((ri->flags & (SRI_MASTER|SRI_SLAVE)) && link->pc == NULL) {
        // 绑定指定ri的连接地址并建立连接
        link->pc = redisAsyncConnectBind(ri->addr->ip,ri->addr->port,NET_FIRST_BIND_ADDR);
        // pc连接失败，则事件通知，且断开pc连接
        if (link->pc->err) {
            sentinelEvent(LL_DEBUG,"-pubsub-link-reconnection",ri,"%@ #%s",
                link->pc->errstr);
            instanceLinkCloseConnection(link,link->pc);
        // pc连接成功
        } else {
            int retval;

            link->pc_conn_time = mstime();
            link->pc->data = link;
            // 将服务器的事件循环关联到pc连接的上下文中
            redisAeAttach(server.el,link->pc);
            // 设置确立连接的回调函数
            redisAsyncSetConnectCallback(link->pc,
                    sentinelLinkEstablishedCallback);
            // 设置断开连接的回调处理
            redisAsyncSetDisconnectCallback(link->pc,
                    sentinelDisconnectCallback);
            //  发送AUTH 命令认证
            sentinelSendAuthIfNeeded(ri,link->pc);
            // 发送连接名字
            sentinelSetClientName(ri,link->pc,"pubsub");
            /* Now we subscribe to the Sentinels "Hello" channel. */
            // 发送订阅 __sentinel__:hello 频道的命令，设置回调函数处理回复
            // sentinelReceiveHelloMessages是处理Pub/Sub的频道返回信息的回调函数，可以发现订阅同一master的Sentinel节点
            retval = redisAsyncCommand(link->pc,
                sentinelReceiveHelloMessages, ri, "SUBSCRIBE %s",
                    SENTINEL_HELLO_CHANNEL);
            // 订阅频道出错，关闭
            if (retval != C_OK) {
                /* If we can't subscribe, the Pub/Sub connection is useless
                 * and we can simply disconnect it and try again. */
                // 关闭pc连接
                instanceLinkCloseConnection(link,link->pc);
                return;
            }
        }
    }
    /* Clear the disconnected status only if we have both the connections
     * (or just the commands connection if this is a sentinel instance). */
    // 如果已经建立了新的连接，则清除断开连接的状态
    if (link->cc && (ri->flags & SRI_SENTINEL || link->pc))
        link->disconnected = 0;
}

/* ======================== Redis instances pinging  ======================== */

/* Return true if master looks "sane", that is:
 * 1) It is actually a master in the current configuration.
 * 2) It reports itself as a master.
 * 3) It is not SDOWN or ODOWN.
 * 4) We obtained last INFO no more than two times the INFO period time ago. */
// 如果主节点是健壮的，则返回true，它满足：
/*
    1. 在当前配置中是主节点
    2. 它报告自己是主节点
    3. 没有处于下线的状态
    4. 获取INFO命令的回复不超过SENTINEL_INFO_PERIOD的2倍（20s）
*/
int sentinelMasterLooksSane(sentinelRedisInstance *master) {
    return
        master->flags & SRI_MASTER &&
        master->role_reported == SRI_MASTER &&
        (master->flags & (SRI_S_DOWN|SRI_O_DOWN)) == 0 &&
        (mstime() - master->info_refresh) < SENTINEL_INFO_PERIOD*2;
}

/* Process the INFO output from masters. */
// 处理从主节点返回INFO命令的输出
void sentinelRefreshInstanceInfo(sentinelRedisInstance *ri, const char *info) {
    sds *lines;
    int numlines, j;
    int role = 0;

    /* cache full INFO output for instance */
    // 释放原来的INFO命令输出缓存并创建新的
    sdsfree(ri->info);
    ri->info = sdsnew(info);

    /* The following fields must be reset to a given value in the case they
     * are not found at all in the INFO output. */
    // 重置从节点复制断开的时间，避免在INFO的输出中找不到该字段
    ri->master_link_down_time = 0;

    /* Process line by line. */
    // 逐行处理，保存行数到numlines
    lines = sdssplitlen(info,strlen(info),"\r\n",2,&numlines);
    // 遍历所有行
    for (j = 0; j < numlines; j++) {
        sentinelRedisInstance *slave;
        sds l = lines[j];

        /* run_id:<40 hex chars>*/
        // 分析run_id
        if (sdslen(l) >= 47 && !memcmp(l,"run_id:",7)) {
            // 新设置一个runid
            if (ri->runid == NULL) {
                ri->runid = sdsnewlen(l+7,40);
            } else {
                // runid发生变化
                if (strncmp(ri->runid,l+7,40) != 0) {
                    // 事件通知
                    sentinelEvent(LL_NOTICE,"+reboot",ri,"%@");
                    // 更新ri实例的runid
                    // 释放ri的runid，并设置为INFO输出的
                    sdsfree(ri->runid);
                    ri->runid = sdsnewlen(l+7,40);
                }
            }
        }

        /* old versions: slave0:<ip>,<port>,<state>
         * new versions: slave0:ip=127.0.0.1,port=9999,... */
        // 读取从节点的ip和port
        if ((ri->flags & SRI_MASTER) &&
            sdslen(l) >= 7 &&
            !memcmp(l,"slave",5) && isdigit(l[5]))
        {
            char *ip, *port, *end;
            // 寻找"ip="子串，旧格式返回NULL
            if (strstr(l,"ip=") == NULL) {
                /* Old format. */
                // 旧格式，分别定位到ip和port的地址
                ip = strchr(l,':'); if (!ip) continue;
                ip++; /* Now ip points to start of ip address. */
                port = strchr(ip,','); if (!port) continue;
                *port = '\0'; /* nul term for easy access. */
                port++; /* Now port points to start of port number. */
                end = strchr(port,','); if (!end) continue;
                *end = '\0'; /* nul term for easy access. */
            } else {
                /* New format. */
                // 新格式，分别定位到ip和port的地址
                ip = strstr(l,"ip="); if (!ip) continue;
                ip += 3; /* Now ip points to start of ip address. */
                port = strstr(l,"port="); if (!port) continue;
                port += 5; /* Now port points to start of port number. */
                /* Nul term both fields for easy access. */
                end = strchr(ip,','); if (end) *end = '\0';
                end = strchr(port,','); if (end) *end = '\0';
            }

            /* Check if we already have this slave into our table,
             * otherwise add it. */
            // 在ri主节点中查找是否存在当前ip和port的从节点
            if (sentinelRedisInstanceLookupSlave(ri,ip,atoi(port)) == NULL) {
                // 如果不存在，则创建一个从节点实例，并添加到主节点的从节点字典中
                if ((slave = createSentinelRedisInstance(NULL,SRI_SLAVE,ip,
                            atoi(port), ri->quorum, ri)) != NULL)
                {   //添加成功，事件通知，刷新配置
                    sentinelEvent(LL_NOTICE,"+slave",slave,"%@");
                    sentinelFlushConfig();
                }
            }
        }

        /* master_link_down_since_seconds:<seconds> */
        // 读取主从节点断线时长，设置为毫秒单位
        if (sdslen(l) >= 32 &&
            !memcmp(l,"master_link_down_since_seconds",30))
        {
            ri->master_link_down_time = strtoll(l+31,NULL,10)*1000;
        }

        /* role:<role> */
        // 读取实例的角色
        if (!memcmp(l,"role:master",11)) role = SRI_MASTER;
        else if (!memcmp(l,"role:slave",10)) role = SRI_SLAVE;

        // 如果是从节点
        if (role == SRI_SLAVE) {
            /* master_host:<host> */
            // 获取从节点的ip，并更新该从节点所属的主节点ip
            if (sdslen(l) >= 12 && !memcmp(l,"master_host:",12)) {
                if (ri->slave_master_host == NULL ||
                    strcasecmp(l+12,ri->slave_master_host))
                {
                    sdsfree(ri->slave_master_host);
                    ri->slave_master_host = sdsnew(l+12);
                    ri->slave_conf_change_time = mstime();
                }
            }

            /* master_port:<port> */
            // 获取从节点的port，并更新该从节点所属的主节点ip
            if (sdslen(l) >= 12 && !memcmp(l,"master_port:",12)) {
                int slave_master_port = atoi(l+12);

                if (ri->slave_master_port != slave_master_port) {
                    ri->slave_master_port = slave_master_port;
                    ri->slave_conf_change_time = mstime();
                }
            }

            /* master_link_status:<status> */
            // 获取主节点的连接状态，并更新到该ri实例中
            if (sdslen(l) >= 19 && !memcmp(l,"master_link_status:",19)) {
                ri->slave_master_link_status =
                    (strcasecmp(l+19,"up") == 0) ?
                    SENTINEL_MASTER_LINK_STATUS_UP :
                    SENTINEL_MASTER_LINK_STATUS_DOWN;
            }

            /* slave_priority:<priority> */
            // 更新从节点优先级
            if (sdslen(l) >= 15 && !memcmp(l,"slave_priority:",15))
                ri->slave_priority = atoi(l+15);

            /* slave_repl_offset:<offset> */
            // 更新从节点的复制偏移量
            if (sdslen(l) >= 18 && !memcmp(l,"slave_repl_offset:",18))
                ri->slave_repl_offset = strtoull(l+18,NULL,10);
        }
    }
    // 更新获取INFO的时间
    ri->info_refresh = mstime();
    sdsfreesplitres(lines,numlines);

    /* ---------------------------- Acting half -----------------------------
     * Some things will not happen if sentinel.tilt is true, but some will
     * still be processed. */
    // 如果Sentinel进入了TILT模式，那么会处理一部分动作
    /* Remember when the role changed. */
    // 更新角色和角色变换的时间
    if (role != ri->role_reported) {
        ri->role_reported_time = mstime();
        ri->role_reported = role;
        if (role == SRI_SLAVE) ri->slave_conf_change_time = mstime();
        /* Log the event with +role-change if the new role is coherent or
         * with -role-change if there is a mismatch with the current config. */
        sentinelEvent(LL_VERBOSE,
            ((ri->flags & (SRI_MASTER|SRI_SLAVE)) == role) ?
            "+role-change" : "-role-change",
            ri, "%@ new reported role is %s",
            role == SRI_MASTER ? "master" : "slave",
            ri->flags & SRI_MASTER ? "master" : "slave");
    }

    /* None of the following conditions are processed when in tilt mode, so
     * return asap. */
    // 如果进入了TILT模式，那么以下的动作都不会执行，所以立即返回
    if (sentinel.tilt) return;

    /* Handle master -> slave role switch. */
    // 该ri实例是主节点，但是INFO命令回复的却是从节点
    if ((ri->flags & SRI_MASTER) && role == SRI_SLAVE) {
        /* Nothing to do, but masters claiming to be slaves are
         * considered to be unreachable by Sentinel, so eventually
         * a failover will be triggered. */
        // 什么都不做，主节点变为从节点是被Sentinel节点判断为不可达，所以故障转移会被触发
    }

    /* Handle slave -> master role switch. */
    // 该ri实例是从节点，但是INFO命令回复的却是主节点
    if ((ri->flags & SRI_SLAVE) && role == SRI_MASTER) {
        /* If this is a promoted slave we can change state to the
         * failover state machine. */
        // 如果这是被晋升的从节点，并且主节点状态等待从节点去晋升为主节点，我们需要更新一些状态
        if ((ri->flags & SRI_PROMOTED) &&
            (ri->master->flags & SRI_FAILOVER_IN_PROGRESS) &&
            (ri->master->failover_state ==
                SENTINEL_FAILOVER_STATE_WAIT_PROMOTION))
        {
            /* Now that we are sure the slave was reconfigured as a master
             * set the master configuration epoch to the epoch we won the
             * election to perform this failover. This will force the other
             * Sentinels to update their config (assuming there is not
             * a newer one already available). */
            // 要确保这个从节点被配置为主节点，并且将纪元设置为赢得故障转移领头的纪元
            // 这将强制其他Sentinel节点取更新他们的配置
            // 更新旧的主节点的纪元
            ri->master->config_epoch = ri->master->failover_epoch;
            // 设置旧的主节点的故障转移状态为SENTINEL_FAILOVER_STATE_RECONF_SLAVES
            // 该状态会向旧的主节点所属的从节点向新的主节点发起复制操作
            ri->master->failover_state = SENTINEL_FAILOVER_STATE_RECONF_SLAVES;
            // 设置故障转移状态改变的时间
            ri->master->failover_state_change_time = mstime();
            // 将Sentinel状态保存到配置中
            sentinelFlushConfig();
            // 事件通知
            sentinelEvent(LL_WARNING,"+promoted-slave",ri,"%@");
            // 如果开启了故障模拟标识，而且是晋升从节点之后发生故障
            if (sentinel.simfailure_flags &
                SENTINEL_SIMFAILURE_CRASH_AFTER_PROMOTION)
                // 退出程序
                sentinelSimFailureCrash();
            // 事件通知
            sentinelEvent(LL_WARNING,"+failover-state-reconf-slaves",
                ri->master,"%@");
            // 重新配置脚本属性，放入脚本队列
            sentinelCallClientReconfScript(ri->master,SENTINEL_LEADER,
                "start",ri->master->addr,ri->addr);
            // 强制发送一个"Hello"信息给所有的Redis数据节点和Sentinel实例去关联指定的主节点实例
            sentinelForceHelloUpdateForMaster(ri->master);
        // 从节点晋升为了主节点，但是主节点在发生故障转移的超时时间限制内又重新上线
        // 因此，要将晋升为主节点的节点变回从节点
        } else {
            /* A slave turned into a master. We want to force our view and
             * reconfigure as slave. Wait some time after the change before
             * going forward, to receive new configs if any. */
            // 从节点晋升为了主节点，我们要强制其重新配置成从节点。等待一些时间，然后无论如何都要让其接收新的配置
            // 计算等待的时间，8s
            mstime_t wait_time = SENTINEL_PUBLISH_PERIOD*4;
            // 该ri实例已经成为主节点，且在最近的'wait_time'毫秒内，如果ri实例没有出现下线状态，
            // 并且ri的主节点实例看起来很健壮
            // 并且距离角色更新的时间已经超过缓冲的wait_time
            if (!(ri->flags & SRI_PROMOTED) &&
                 sentinelMasterLooksSane(ri->master) &&
                 sentinelRedisInstanceNoDownFor(ri,wait_time) &&
                 mstime() - ri->role_reported_time > wait_time)
            {
                // 发送slaveof命令，使其变为从节点
                int retval = sentinelSendSlaveOf(ri,
                        ri->master->addr->ip,
                        ri->master->addr->port);
                // 事件通知
                if (retval == C_OK)
                    sentinelEvent(LL_NOTICE,"+convert-to-slave",ri,"%@");
            }
        }
    }

    /* Handle slaves replicating to a different master address. */
    // 如果该实例的从节点，且INFO回复是从节点，但是主节点的地址发生变化，因此要让这一类从节点重新复制正确的主节点
    if ((ri->flags & SRI_SLAVE) &&
        role == SRI_SLAVE &&
        (ri->slave_master_port != ri->master->addr->port ||
         strcasecmp(ri->slave_master_host,ri->master->addr->ip)))
    {
        // 故障转移超时时间
        mstime_t wait_time = ri->master->failover_timeout;

        /* Make sure the master is sane before reconfiguring this instance
         * into a slave. */
        // 最近的'wait_time'毫秒内，ri实例没有出现下线状态，
        // ri的主节点看起来很健壮
        // 并且距离从节点所属的主节点地址变化的时间已经超过缓冲的wait_time
        if (sentinelMasterLooksSane(ri->master) &&
            sentinelRedisInstanceNoDownFor(ri,wait_time) &&
            mstime() - ri->slave_conf_change_time > wait_time)
        {
            // 发送slaveof命令，使其重新从属新的主节点
            int retval = sentinelSendSlaveOf(ri,
                    ri->master->addr->ip,
                    ri->master->addr->port);
            // 事件通知
            if (retval == C_OK)
                sentinelEvent(LL_NOTICE,"+fix-slave-config",ri,"%@");
        }
    }

    /* Detect if the slave that is in the process of being reconfigured
     * changed state. */
    // 如果ri是从节点，且Sentinel向ri发送了 slaveof 命令或者ri实例已经在同步新的主节点了
    if ((ri->flags & SRI_SLAVE) && role == SRI_SLAVE &&
        (ri->flags & (SRI_RECONF_SENT|SRI_RECONF_INPROG)))
    {
        /* SRI_RECONF_SENT -> SRI_RECONF_INPROG. */
        // 将SRI_RECONF_SENT状态改为SRI_RECONF_INPROG状态
        // 因为Sentinel向ri发送了 slaveof 命令，但是当前ri实例所属主节点的地址已经和新主节点地址相同
        // 因此，将状态给为SRI_RECONF_INPROG，表示正在同步复制操作
        if ((ri->flags & SRI_RECONF_SENT) &&
            ri->slave_master_host &&
            strcmp(ri->slave_master_host,
                    ri->master->promoted_slave->addr->ip) == 0 &&
            ri->slave_master_port == ri->master->promoted_slave->addr->port)
        {
            ri->flags &= ~SRI_RECONF_SENT;
            ri->flags |= SRI_RECONF_INPROG;
            // 事件通知
            sentinelEvent(LL_NOTICE,"+slave-reconf-inprog",ri,"%@");
        }

        /* SRI_RECONF_INPROG -> SRI_RECONF_DONE */
        // 将SRI_RECONF_INPROG状态给为SRI_RECONF_DONE，表示同步完成
        if ((ri->flags & SRI_RECONF_INPROG) &&
            ri->slave_master_link_status == SENTINEL_MASTER_LINK_STATUS_UP)
        {
            ri->flags &= ~SRI_RECONF_INPROG;
            ri->flags |= SRI_RECONF_DONE;
            // 事件通知
            sentinelEvent(LL_NOTICE,"+slave-reconf-done",ri,"%@");
        }
    }
}

// 回调函数，用来处理INFO命令的回复
void sentinelInfoReplyCallback(redisAsyncContext *c, void *reply, void *privdata) {
    sentinelRedisInstance *ri = privdata;
    instanceLink *link = c->data;
    redisReply *r;

    if (!reply || !link) return;
    // 设置已发送但未回复的命令数减1
    link->pending_commands--;
    r = reply;

    // 处理从主节点返回INFO命令的输出
    if (r->type == REDIS_REPLY_STRING)
        sentinelRefreshInstanceInfo(ri,r->str);
}

/* Just discard the reply. We use this when we are not monitoring the return
 * value of the command but its effects directly. */
// 丢弃回复。当我们不是监控命令的返回值，而是关心他的影响：将已发送但未回复的命令数减1
void sentinelDiscardReplyCallback(redisAsyncContext *c, void *reply, void *privdata) {
    instanceLink *link = c->data;
    UNUSED(reply);
    UNUSED(privdata);
    // 将已发送但未回复的命令数减1
    if (link) link->pending_commands--;
}

// 处理PING命令回复的回调函数
void sentinelPingReplyCallback(redisAsyncContext *c, void *reply, void *privdata) {
    sentinelRedisInstance *ri = privdata;
    instanceLink *link = c->data;
    redisReply *r;

    if (!reply || !link) return;
    // 将已发送但未回复的命令数减1
    link->pending_commands--;
    r = reply;
    // 如果ri实例处于REDIS_REPLY_STATUS或REDIS_REPLY_ERROR状态
    if (r->type == REDIS_REPLY_STATUS ||
        r->type == REDIS_REPLY_ERROR) {
        /* Update the "instance available" field only if this is an
         * acceptable reply. */
        // 如果回复是"PONG"、"LOADING"、或"MASTERDOWN"，这些属于可接受的回复，则更新最近一个接收到PING回复的时间
        if (strncmp(r->str,"PONG",4) == 0 ||
            strncmp(r->str,"LOADING",7) == 0 ||
            strncmp(r->str,"MASTERDOWN",10) == 0)
        {
            link->last_avail_time = mstime();
            // 接收到了PONG，设置了最近发送PING的时间为0
            link->act_ping_time = 0; /* Flag the pong as received. */
        } else {
            /* Send a SCRIPT KILL command if the instance appears to be
             * down because of a busy script. */
            // 如果收到"BUSY"回复，则发送一个"SCRIPT KILL"命令来恢复因为执行脚本而看起来是主观下线状态的服务器
            if (strncmp(r->str,"BUSY",4) == 0 &&
                (ri->flags & SRI_S_DOWN) &&
                !(ri->flags & SRI_SCRIPT_KILL_SENT))
            {
                // 异步发送一个"SCRIPT KILL"命令
                if (redisAsyncCommand(ri->link->cc,
                        sentinelDiscardReplyCallback, ri,
                        "SCRIPT KILL") == C_OK)
                    // 发送成功，将已发送但未回复的命令数加1
                    ri->link->pending_commands++;
                // 设置ri为SRI_SCRIPT_KILL_SENT(发送SCRIPT KILL命令给返回 -BUSY 的节点)的状态
                ri->flags |= SRI_SCRIPT_KILL_SENT;
            }
        }
    }
    // 更新最近一次回复PING命令的时间
    link->last_pong_time = mstime();
}

/* This is called when we get the reply about the PUBLISH command we send
 * to the master to advertise this sentinel. */
// 处理收到一个PUBLISH command命令的回复的回调函数，发送主节点通知这个Sentinel
void sentinelPublishReplyCallback(redisAsyncContext *c, void *reply, void *privdata) {
    sentinelRedisInstance *ri = privdata;
    instanceLink *link = c->data;
    redisReply *r;

    if (!reply || !link) return;
    // 将已发送但未回复的命令数减1
    link->pending_commands--;
    r = reply;

    /* Only update pub_time if we actually published our message. Otherwise
     * we'll retry again in 100 milliseconds. */
    // ri实例状态不是回复错误，那么说明命令发送成功
    if (r->type != REDIS_REPLY_ERROR)
        // 更新最近一次通过频道发送信息的时间
        ri->last_pub_time = mstime();
}

/* Process an hello message received via Pub/Sub in master or slave instance,
 * or sent directly to this sentinel via the (fake) PUBLISH command of Sentinel.
 *
 * If the master name specified in the message is not known, the message is
 * discarded. */
// 处理从主节点或从节点通过频道发送过来的hello信息
// 或者hello信息是另一个Sentinel通过PUBLISH命令发送而来的
// 如果指定主节点的名字是未知的，那么信息会被丢弃
void sentinelProcessHelloMessage(char *hello, int hello_len) {
    /* Format is composed of 8 tokens:
     * 0=ip,1=port,2=runid,3=current_epoch,4=master_name,
     * 5=master_ip,6=master_port,7=master_config_epoch. */
    // 一共八部分：0=ip,1=port,2=runid,3=current_epoch,4=master_name,5=master_ip,6=master_port,7=master_config_epoch
    int numtokens, port, removed, master_port;
    uint64_t current_epoch, master_config_epoch;
    // 用","将hello信息分隔开，保存在token中
    char **token = sdssplitlen(hello, hello_len, ",", 1, &numtokens);
    sentinelRedisInstance *si, *master;

    if (numtokens == 8) {
        /* Obtain a reference to the master this hello message is about */
        // 根据master_name查找主节点
        master = sentinelGetMasterByName(token[4]);
        // 未知的主节点，跳出该信息
        if (!master) goto cleanup; /* Unknown master, skip the message. */

        /* First, try to see if we already have this sentinel. */
        // 首先，尝试去发现我们是否已经有这个Sentinel节点
        port = atoi(token[1]);
        master_port = atoi(token[6]);
        // 根据指定的runid，ip和port，在主节点的Sentinel节点字典中查找Sentinel实例
        si = getSentinelRedisInstanceByAddrAndRunID(
                        master->sentinels,token[0],port,token[2]);
        current_epoch = strtoull(token[3],NULL,10);
        master_config_epoch = strtoull(token[7],NULL,10);

        // 如果没有在主节点的Sentinel节点字典找到该地址的Sentinel节点
        if (!si) {
            /* If not, remove all the sentinels that have the same runid
             * because there was an address change, and add the same Sentinel
             * with the new address back. */
            // 删除所有有相同runid的Sentinel节点，因为要改变地址，并且添加有新地址的同一Sentinel实例
            // 函数从master主节点中根据runid删除指定的Sentinel节点
            removed = removeMatchingSentinelFromMaster(master,token[2]);
            // 删除成功，事件通知
            if (removed) {
                sentinelEvent(LL_NOTICE,"+sentinel-address-switch",master,
                    "%@ ip %s port %d for %s", token[0],port,token[2]);
            } else {
                /* Check if there is another Sentinel with the same address this
                 * new one is reporting. What we do if this happens is to set its
                 * port to 0, to signal the address is invalid. We'll update it
                 * later if we get an HELLO message. */
                // 检查是否有另一个Sentinel具有和信息中所报告的地址一致。 如果发生这种情况，我们做的是将其端口设置为0，以表示地址无效。 如果我们收到一条HELLO消息，我们稍后会更新

                // 根据指定的ip和port，在主节点的Sentinel节点字典中查找Sentinel实例
                sentinelRedisInstance *other =
                    getSentinelRedisInstanceByAddrAndRunID(
                        master->sentinels, token[0],port,NULL);
                // 如果找到了相同地址的Sentinel实例，那么重置地址的端口，表示地址无效
                if (other) {
                    sentinelEvent(LL_NOTICE,"+sentinel-invalid-addr",other,"%@");
                    other->addr->port = 0; /* It means: invalid address. */
                    // 找到的Sentinel节点发生地址转换（在hello信息中报告了不同ip/port）
                    // 更新所有在不同主节点环境中的所有相同的Sentinel，并且断开连接，以便所有都可以被更新
                    sentinelUpdateSentinelAddressInAllMasters(other);
                }
            }

            /* Add the new sentinel. */
            // 添加一个新的Sentinel节点
            // 创建Sentinel实例，添加到master主节点的sentinel字典中
            si = createSentinelRedisInstance(token[2],SRI_SENTINEL,
                            token[0],port,master->quorum,master);

            // 添加成功
            if (si) {
                // 如果没有删除之前的Sentinel，发送事件通知
                if (!removed) sentinelEvent(LL_NOTICE,"+sentinel",si,"%@");
                /* The runid is NULL after a new instance creation and
                 * for Sentinels we don't have a later chance to fill it,
                 * so do it now. */
                // 设置新Sentinel节点的runid
                si->runid = sdsnew(token[2]);
                // 尝试与其他Sentinel节点共享连接对象
                sentinelTryConnectionSharing(si);
                // 如果之前删除了Sentinel节点，更新其他的Sentinel节点信息
                if (removed) sentinelUpdateSentinelAddressInAllMasters(si);
                // 刷新配置
                sentinelFlushConfig();
            }
        }

        /* Update local current_epoch if received current_epoch is greater.*/
        // 如果接收到的纪元比当前纪元高，那么更新当前纪元
        if (current_epoch > sentinel.current_epoch) {
            sentinel.current_epoch = current_epoch;
            // 刷新配置
            sentinelFlushConfig();
            // 发送事件通知
            sentinelEvent(LL_WARNING,"+new-epoch",master,"%llu",
                (unsigned long long) sentinel.current_epoch);
        }

        /* Update master info if received configuration is newer. */
        // 如果接受到的配置是新的，那么更新主节点的信息
        if (si && master->config_epoch < master_config_epoch) {
            master->config_epoch = master_config_epoch;
            // 地址不相同
            if (master_port != master->addr->port ||
                strcmp(master->addr->ip, token[5]))
            {
                sentinelAddr *old_addr;

                sentinelEvent(LL_WARNING,"+config-update-from",si,"%@");
                sentinelEvent(LL_WARNING,"+switch-master",
                    master,"%s %s %d %s %d",
                    master->name,
                    master->addr->ip, master->addr->port,
                    token[5], master_port);
                // 创建一个主节点地址的副本
                old_addr = dupSentinelAddr(master->addr);
                // 重置指定的master主节点，并且改变其 ip:port 地址，但是保持名字不变
                sentinelResetMasterAndChangeAddress(master, token[5], master_port);
                // 更新脚本配置
                sentinelCallClientReconfScript(master,
                    SENTINEL_OBSERVER,"start",
                    old_addr,master->addr);
                releaseSentinelAddr(old_addr);
            }
        }

        /* Update the state of the Sentinel. */
        // 更新最近一次收到Sentinel发来hello信息的时间
        if (si) si->last_hello_time = mstime();
    }

// 清理工作
cleanup:
    sdsfreesplitres(token,numtokens);
}


/* This is our Pub/Sub callback for the Hello channel. It's useful in order
 * to discover other sentinels attached at the same master. */
// 该函数是处理Pub/Sub的频道返回信息的回调函数，可以发现订阅同一master节点的Sentinel节点
void sentinelReceiveHelloMessages(redisAsyncContext *c, void *reply, void *privdata) {
    sentinelRedisInstance *ri = privdata;
    redisReply *r;
    UNUSED(c);

    if (!reply || !ri) return;
    r = reply;

    /* Update the last activity in the pubsub channel. Note that since we
     * receive our messages as well this timestamp can be used to detect
     * if the link is probably disconnected even if it seems otherwise. */
    // 更新最近收到频道信息的时间。
    ri->link->pc_last_activity = mstime();

    /* Sanity check in the reply we expect, so that the code that follows
     * can avoid to check for details. */
    // 只处理频道发来的信息，不处理订阅和退订的信息
    if (r->type != REDIS_REPLY_ARRAY ||
        r->elements != 3 ||
        r->element[0]->type != REDIS_REPLY_STRING ||
        r->element[1]->type != REDIS_REPLY_STRING ||
        r->element[2]->type != REDIS_REPLY_STRING ||
        strcmp(r->element[0]->str,"message") != 0) return;

    /* We are not interested in meeting ourselves */
    // 处理不是自己发送来的信息
    if (strstr(r->element[2]->str,sentinel.myid) != NULL) return;
    // 处理从主节点或从节点通过频道发送过来的hello信息
    sentinelProcessHelloMessage(r->element[2]->str, r->element[2]->len);
}

/* Send an "Hello" message via Pub/Sub to the specified 'ri' Redis
 * instance in order to broadcast the current configuraiton for this
 * master, and to advertise the existence of this Sentinel at the same time.
 *
 * The message has the following format:
 *
 * sentinel_ip,sentinel_port,sentinel_runid,current_epoch,
 * master_name,master_ip,master_port,master_config_epoch.
 *
 * Returns C_OK if the PUBLISH was queued correctly, otherwise
 * C_ERR is returned. */
// 通过频道发送一个hello信息给指定的ri实例，以便广播当前关于主节点的配置，并且同时向其他Sentinel节点通告Sentinel的存在
// 信息的格式如下：
// sentinel_ip,sentinel_port,sentinel_runid,current_epoch,master_name,master_ip,master_port,master_config_epoch
// 如果成功将PUBLISH放入队列中，返回C_OK，否则返回C_ERR
int sentinelSendHello(sentinelRedisInstance *ri) {
    char ip[NET_IP_STR_LEN];
    char payload[NET_IP_STR_LEN+1024];
    int retval;
    char *announce_ip;
    int announce_port;
    // 使用主节点的实例
    sentinelRedisInstance *master = (ri->flags & SRI_MASTER) ? ri : ri->master;
    // 获取当前主节点的地址
    sentinelAddr *master_addr = sentinelGetCurrentMasterAddress(master);
    // 如果连接处于关闭状态，返回C_ERR
    if (ri->link->disconnected) return C_ERR;

    /* Use the specified announce address if specified, otherwise try to
     * obtain our own IP address. */
    // 如果Sentinel指定了宣告地址（announce address），则使用该地址
    if (sentinel.announce_ip) {
        announce_ip = sentinel.announce_ip;
    // 否则使用主节点自己的地址
    } else {
        // 获取主节点的地址
        if (anetSockName(ri->link->cc->c.fd,ip,sizeof(ip),NULL) == -1)
            return C_ERR;
        announce_ip = ip;
    }
    // 获取端口
    announce_port = sentinel.announce_port ?
                    sentinel.announce_port : server.port;

    /* Format and send the Hello message. */
    // 按照格式将hello信息写到payload中
    snprintf(payload,sizeof(payload),
        "%s,%d,%s,%llu," /* Info about this sentinel. */
        "%s,%s,%d,%llu", /* Info about current master. */
        announce_ip, announce_port, sentinel.myid,
        (unsigned long long) sentinel.current_epoch,
        /* --- */
        master->name,master_addr->ip,master_addr->port,
        (unsigned long long) master->config_epoch);
    // 异步执行PUBLISH命令，发布hello信息
    retval = redisAsyncCommand(ri->link->cc,
        sentinelPublishReplyCallback, ri, "PUBLISH %s %s",
            SENTINEL_HELLO_CHANNEL,payload);
    if (retval != C_OK) return C_ERR;
    // 已发送未回复的命令个数加1
    ri->link->pending_commands++;
    return C_OK;
}

/* Reset last_pub_time in all the instances in the specified dictionary
 * in order to force the delivery of an Hello update ASAP. */
// 重置所有指定字典的实例的last_pub_time值，为了取强制立即传输一个hello信息
void sentinelForceHelloUpdateDictOfRedisInstances(dict *instances) {
    dictIterator *di;
    dictEntry *de;

    di = dictGetSafeIterator(instances);
    // 遍历指定字典的所有实例
    while((de = dictNext(di)) != NULL) {
        sentinelRedisInstance *ri = dictGetVal(de);
        // 将实例的最近通过频道发布信息的时间减少指定的周期，就可以立即传输一个hello信息
        if (ri->last_pub_time >= (SENTINEL_PUBLISH_PERIOD+1))
            ri->last_pub_time -= (SENTINEL_PUBLISH_PERIOD+1);
    }
    dictReleaseIterator(di);
}

/* This function forces the delivery of an "Hello" message (see
 * sentinelSendHello() top comment for further information) to all the Redis
 * and Sentinel instances related to the specified 'master'.
 *
 * It is technically not needed since we send an update to every instance
 * with a period of SENTINEL_PUBLISH_PERIOD milliseconds, however when a
 * Sentinel upgrades a configuration it is a good idea to deliever an update
 * to the other Sentinels ASAP. */
// 该函数强制发送一个"Hello"信息给所有的Redis数据节点和Sentinel实例去关联指定的主节点实例
// 它一般不会被需要因为我们在一个SENTINEL_PUBLISH_PERIOD毫秒内会发送一个更新信息给所有的实例，但是当Sentinel升级配置了，我们就必须立即更新所有的Sentinel节点
int sentinelForceHelloUpdateForMaster(sentinelRedisInstance *master) {
    if (!(master->flags & SRI_MASTER)) return C_ERR;
    // 将主节点、从节点、监控该主节点的Sentinel节点的last_pub_time值都减少指定的周期，就可以立即传输一个hello信息
    if (master->last_pub_time >= (SENTINEL_PUBLISH_PERIOD+1))
        master->last_pub_time -= (SENTINEL_PUBLISH_PERIOD+1);
    sentinelForceHelloUpdateDictOfRedisInstances(master->sentinels);
    sentinelForceHelloUpdateDictOfRedisInstances(master->slaves);
    return C_OK;
}

/* Send a PING to the specified instance and refresh the act_ping_time
 * if it is zero (that is, if we received a pong for the previous ping).
 *
 * On error zero is returned, and we can't consider the PING command
 * queued in the connection. */
// 发送一个PING命令给指定的实例，并且更新act_ping_time，出错返回0
int sentinelSendPing(sentinelRedisInstance *ri) {
    // 异步发送一个PING命令给实例ri
    int retval = redisAsyncCommand(ri->link->cc,
        sentinelPingReplyCallback, ri, "PING");
    // 发送成功
    if (retval == C_OK) {
        // 已发送未回复的命令个数加1
        ri->link->pending_commands++;
        // 更新最近一次发送PING命令的时间
        ri->link->last_ping_time = mstime();
        /* We update the active ping time only if we received the pong for
         * the previous ping, otherwise we are technically waiting since the
         * first ping that did not received a reply. */
        // 更新最近一次发送PING命令，但没有收到PONG命令的时间
        if (ri->link->act_ping_time == 0)
            ri->link->act_ping_time = ri->link->last_ping_time;
        return 1;
    } else {
        return 0;
    }
}

/* Send periodic PING, INFO, and PUBLISH to the Hello channel to
 * the specified master or slave instance. */
// 定期发送PING PONG 和 PUBLISH 命令到指定的主节点和从节点实例的hello频道中
void sentinelSendPeriodicCommands(sentinelRedisInstance *ri) {
    mstime_t now = mstime();
    mstime_t info_period, ping_period;
    int retval;

    /* Return ASAP if we have already a PING or INFO already pending, or
     * in the case the instance is not properly connected. */
    // 如果ri实例连接处于关闭状态，直接返回
    if (ri->link->disconnected) return;

    /* For INFO, PING, PUBLISH that are not critical commands to send we
     * also have a limit of SENTINEL_MAX_PENDING_COMMANDS. We don't
     * want to use a lot of memory just because a link is not working
     * properly (note that anyway there is a redundant protection about this,
     * that is, the link will be disconnected and reconnected if a long
     * timeout condition is detected. */
    // 对于不是发送关键命令的INFO，PING，PUBLISH，我们也有SENTINEL_MAX_PENDING_COMMANDS的限制。 我们不想使用大量的内存，只是因为连接对象无法正常工作（请注意，无论如何，还有一个冗余的保护措施，即如果检测到长时间的超时条件，连接将被断开连接并重新连接
    // 每个实例的已发送未回复的命令个数不能超过100个，否则直接返回
    if (ri->link->pending_commands >=
        SENTINEL_MAX_PENDING_COMMANDS * ri->link->refcount) return;

    /* If this is a slave of a master in O_DOWN condition we start sending
     * it INFO every second, instead of the usual SENTINEL_INFO_PERIOD
     * period. In this state we want to closely monitor slaves in case they
     * are turned into masters by another Sentinel, or by the sysadmin.
     *
     * Similarly we monitor the INFO output more often if the slave reports
     * to be disconnected from the master, so that we can have a fresh
     * disconnection time figure. */
    // 如果主节点处于O_DOWN状态下，那么Sentinel默认每秒发送INFO命令给它的从节点，而不是通常的SENTINEL_INFO_PERIOD(10s)周期。在这种状态下，我们想更密切的监控从节点，万一他们被其他的Sentinel晋升为主节点
    // 如果从节点报告和主节点断开连接，我们同样也监控INFO命令的输出更加频繁，以便我们能有一个更新鲜的断开连接的时间

    // 如果ri是从节点，且他的主节点处于故障状态的状态或者从节点和主节点断开复制了
    if ((ri->flags & SRI_SLAVE) &&
        ((ri->master->flags & (SRI_O_DOWN|SRI_FAILOVER_IN_PROGRESS)) ||
         (ri->master_link_down_time != 0)))
    {
        // 设置INFO命令的周期时间为1s
        info_period = 1000;
    } else {
        // 否则就是默认的10s
        info_period = SENTINEL_INFO_PERIOD;
    }

    /* We ping instances every time the last received pong is older than
     * the configured 'down-after-milliseconds' time, but every second
     * anyway if 'down-after-milliseconds' is greater than 1 second. */
    // 每次最后一次接收到的PONG比配置的 'down-after-milliseconds' 时间更长，但是如果 'down-after-milliseconds'大于1秒，则每秒钟进行一次ping

    // 获取ri设置的主观下线的时间
    ping_period = ri->down_after_period;
    // 如果大于1秒，则设置为1秒
    if (ping_period > SENTINEL_PING_PERIOD) ping_period = SENTINEL_PING_PERIOD;

    // 如果实例不是Sentinel节点且Sentinel节点从该数据节点(主节点或从节点)没有收到过INFO回复或者收到INFO回复超时
    if ((ri->flags & SRI_SENTINEL) == 0 &&
        (ri->info_refresh == 0 ||
        (now - ri->info_refresh) > info_period))
    {
        /* Send INFO to masters and slaves, not sentinels. */
        // 发送INFO命令给主节点和从节点
        retval = redisAsyncCommand(ri->link->cc,
            sentinelInfoReplyCallback, ri, "INFO");
        // 已发送未回复的命令个数加1
        if (retval == C_OK) ri->link->pending_commands++;

    // 如果发送和回复PING命令超时
    } else if ((now - ri->link->last_pong_time) > ping_period &&
               (now - ri->link->last_ping_time) > ping_period/2) {
        /* Send PING to all the three kinds of instances. */
        // 发送一个PING命令给ri实例，并且更新act_ping_time
        sentinelSendPing(ri);

    // 发送频道的定时命令超时
    } else if ((now - ri->last_pub_time) > SENTINEL_PUBLISH_PERIOD) {
        /* PUBLISH hello messages to all the three kinds of instances. */
        // 发布hello信息给ri实例
        sentinelSendHello(ri);
    }
}

/* =========================== SENTINEL command ============================= */

// 返回字符串形式的故障状态
const char *sentinelFailoverStateStr(int state) {
    switch(state) {
    case SENTINEL_FAILOVER_STATE_NONE: return "none";
    case SENTINEL_FAILOVER_STATE_WAIT_START: return "wait_start";
    case SENTINEL_FAILOVER_STATE_SELECT_SLAVE: return "select_slave";
    case SENTINEL_FAILOVER_STATE_SEND_SLAVEOF_NOONE: return "send_slaveof_noone";
    case SENTINEL_FAILOVER_STATE_WAIT_PROMOTION: return "wait_promotion";
    case SENTINEL_FAILOVER_STATE_RECONF_SLAVES: return "reconf_slaves";
    case SENTINEL_FAILOVER_STATE_UPDATE_CONFIG: return "update_config";
    default: return "unknown";
    }
}

/* Redis instance to Redis protocol representation. */
// 以Redis协议形式回复Redis实例的情况给client
void addReplySentinelRedisInstance(client *c, sentinelRedisInstance *ri) {
    char *flags = sdsempty();
    void *mbl;
    int fields = 0;

    mbl = addDeferredMultiBulkLength(c);

    addReplyBulkCString(c,"name");
    addReplyBulkCString(c,ri->name);
    fields++;

    addReplyBulkCString(c,"ip");
    addReplyBulkCString(c,ri->addr->ip);
    fields++;

    addReplyBulkCString(c,"port");
    addReplyBulkLongLong(c,ri->addr->port);
    fields++;

    addReplyBulkCString(c,"runid");
    addReplyBulkCString(c,ri->runid ? ri->runid : "");
    fields++;

    addReplyBulkCString(c,"flags");
    if (ri->flags & SRI_S_DOWN) flags = sdscat(flags,"s_down,");
    if (ri->flags & SRI_O_DOWN) flags = sdscat(flags,"o_down,");
    if (ri->flags & SRI_MASTER) flags = sdscat(flags,"master,");
    if (ri->flags & SRI_SLAVE) flags = sdscat(flags,"slave,");
    if (ri->flags & SRI_SENTINEL) flags = sdscat(flags,"sentinel,");
    if (ri->link->disconnected) flags = sdscat(flags,"disconnected,");
    if (ri->flags & SRI_MASTER_DOWN) flags = sdscat(flags,"master_down,");
    if (ri->flags & SRI_FAILOVER_IN_PROGRESS)
        flags = sdscat(flags,"failover_in_progress,");
    if (ri->flags & SRI_PROMOTED) flags = sdscat(flags,"promoted,");
    if (ri->flags & SRI_RECONF_SENT) flags = sdscat(flags,"reconf_sent,");
    if (ri->flags & SRI_RECONF_INPROG) flags = sdscat(flags,"reconf_inprog,");
    if (ri->flags & SRI_RECONF_DONE) flags = sdscat(flags,"reconf_done,");

    if (sdslen(flags) != 0) sdsrange(flags,0,-2); /* remove last "," */
    addReplyBulkCString(c,flags);
    sdsfree(flags);
    fields++;

    addReplyBulkCString(c,"link-pending-commands");
    addReplyBulkLongLong(c,ri->link->pending_commands);
    fields++;

    addReplyBulkCString(c,"link-refcount");
    addReplyBulkLongLong(c,ri->link->refcount);
    fields++;

    if (ri->flags & SRI_FAILOVER_IN_PROGRESS) {
        addReplyBulkCString(c,"failover-state");
        addReplyBulkCString(c,(char*)sentinelFailoverStateStr(ri->failover_state));
        fields++;
    }

    addReplyBulkCString(c,"last-ping-sent");
    addReplyBulkLongLong(c,
        ri->link->act_ping_time ? (mstime() - ri->link->act_ping_time) : 0);
    fields++;

    addReplyBulkCString(c,"last-ok-ping-reply");
    addReplyBulkLongLong(c,mstime() - ri->link->last_avail_time);
    fields++;

    addReplyBulkCString(c,"last-ping-reply");
    addReplyBulkLongLong(c,mstime() - ri->link->last_pong_time);
    fields++;

    if (ri->flags & SRI_S_DOWN) {
        addReplyBulkCString(c,"s-down-time");
        addReplyBulkLongLong(c,mstime()-ri->s_down_since_time);
        fields++;
    }

    if (ri->flags & SRI_O_DOWN) {
        addReplyBulkCString(c,"o-down-time");
        addReplyBulkLongLong(c,mstime()-ri->o_down_since_time);
        fields++;
    }

    addReplyBulkCString(c,"down-after-milliseconds");
    addReplyBulkLongLong(c,ri->down_after_period);
    fields++;

    /* Masters and Slaves */
    if (ri->flags & (SRI_MASTER|SRI_SLAVE)) {
        addReplyBulkCString(c,"info-refresh");
        addReplyBulkLongLong(c,mstime() - ri->info_refresh);
        fields++;

        addReplyBulkCString(c,"role-reported");
        addReplyBulkCString(c, (ri->role_reported == SRI_MASTER) ? "master" :
                                                                   "slave");
        fields++;

        addReplyBulkCString(c,"role-reported-time");
        addReplyBulkLongLong(c,mstime() - ri->role_reported_time);
        fields++;
    }

    /* Only masters */
    if (ri->flags & SRI_MASTER) {
        addReplyBulkCString(c,"config-epoch");
        addReplyBulkLongLong(c,ri->config_epoch);
        fields++;

        addReplyBulkCString(c,"num-slaves");
        addReplyBulkLongLong(c,dictSize(ri->slaves));
        fields++;

        addReplyBulkCString(c,"num-other-sentinels");
        addReplyBulkLongLong(c,dictSize(ri->sentinels));
        fields++;

        addReplyBulkCString(c,"quorum");
        addReplyBulkLongLong(c,ri->quorum);
        fields++;

        addReplyBulkCString(c,"failover-timeout");
        addReplyBulkLongLong(c,ri->failover_timeout);
        fields++;

        addReplyBulkCString(c,"parallel-syncs");
        addReplyBulkLongLong(c,ri->parallel_syncs);
        fields++;

        if (ri->notification_script) {
            addReplyBulkCString(c,"notification-script");
            addReplyBulkCString(c,ri->notification_script);
            fields++;
        }

        if (ri->client_reconfig_script) {
            addReplyBulkCString(c,"client-reconfig-script");
            addReplyBulkCString(c,ri->client_reconfig_script);
            fields++;
        }
    }

    /* Only slaves */
    if (ri->flags & SRI_SLAVE) {
        addReplyBulkCString(c,"master-link-down-time");
        addReplyBulkLongLong(c,ri->master_link_down_time);
        fields++;

        addReplyBulkCString(c,"master-link-status");
        addReplyBulkCString(c,
            (ri->slave_master_link_status == SENTINEL_MASTER_LINK_STATUS_UP) ?
            "ok" : "err");
        fields++;

        addReplyBulkCString(c,"master-host");
        addReplyBulkCString(c,
            ri->slave_master_host ? ri->slave_master_host : "?");
        fields++;

        addReplyBulkCString(c,"master-port");
        addReplyBulkLongLong(c,ri->slave_master_port);
        fields++;

        addReplyBulkCString(c,"slave-priority");
        addReplyBulkLongLong(c,ri->slave_priority);
        fields++;

        addReplyBulkCString(c,"slave-repl-offset");
        addReplyBulkLongLong(c,ri->slave_repl_offset);
        fields++;
    }

    /* Only sentinels */
    if (ri->flags & SRI_SENTINEL) {
        addReplyBulkCString(c,"last-hello-message");
        addReplyBulkLongLong(c,mstime() - ri->last_hello_time);
        fields++;

        addReplyBulkCString(c,"voted-leader");
        addReplyBulkCString(c,ri->leader ? ri->leader : "?");
        fields++;

        addReplyBulkCString(c,"voted-leader-epoch");
        addReplyBulkLongLong(c,ri->leader_epoch);
        fields++;
    }

    setDeferredMultiBulkLength(c,mbl,fields*2);
}

/* Output a number of instances contained inside a dictionary as
 * Redis protocol. */
//以Redis协议的形式，输出一个实例字典所包含的所有实例
void addReplyDictOfRedisInstances(client *c, dict *instances) {
    dictIterator *di;
    dictEntry *de;

    di = dictGetIterator(instances);
    addReplyMultiBulkLen(c,dictSize(instances));
    // 遍历所有的实例
    while((de = dictNext(di)) != NULL) {
        sentinelRedisInstance *ri = dictGetVal(de);
        // 将当前实例添加到client的输出缓冲区中
        addReplySentinelRedisInstance(c,ri);
    }
    dictReleaseIterator(di);
}

/* Lookup the named master into sentinel.masters.
 * If the master is not found reply to the client with an error and returns
 * NULL. */
// 在sentinel.masters字典中查找名字为name的主节点
// 如果没有找到主节点，回复client错误信息返回NULL
sentinelRedisInstance *sentinelGetMasterByNameOrReplyError(client *c,
                        robj *name)
{
    sentinelRedisInstance *ri;

    // 查找name命名的主节点
    ri = dictFetchValue(sentinel.masters,name->ptr);
    if (!ri) {
        // 没找到回复错误信息
        addReplyError(c,"No such master with that name");
        return NULL;
    }
    return ri;
}

#define SENTINEL_ISQR_OK 0
#define SENTINEL_ISQR_NOQUORUM (1<<0)
#define SENTINEL_ISQR_NOAUTH (1<<1)
// 判断该主节点的投票能力
int sentinelIsQuorumReachable(sentinelRedisInstance *master, int *usableptr) {
    dictIterator *di;
    dictEntry *de;
    int usable = 1; /* Number of usable Sentinels. Init to 1 to count myself. */
    int result = SENTINEL_ISQR_OK;
    int voters = dictSize(master->sentinels)+1; /* Known Sentinels + myself. */

    di = dictGetIterator(master->sentinels);
    // 遍历所有监控master节点的Sentinel节点
    while((de = dictNext(di)) != NULL) {
        sentinelRedisInstance *ri = dictGetVal(de);
        // 如果Sentinel节点处于下线状态，则跳过该Sentinel节点
        if (ri->flags & (SRI_S_DOWN|SRI_O_DOWN)) continue;
        // 计算正常Sentinel节点个数
        usable++;
    }
    dictReleaseIterator(di);
    // 可用的投票数小于客观下线的票数
    if (usable < (int)master->quorum) result |= SENTINEL_ISQR_NOQUORUM;
    // 可用的投票数比所有的Sentinel节点个数的一般加1还小
    if (usable < voters/2+1) result |= SENTINEL_ISQR_NOAUTH;
    // 保存可用的票数
    if (usableptr) *usableptr = usable;
    // 返回结果
    return result;
}

// SENTINEL 命令的实现
void sentinelCommand(client *c) {

    // SENTINEL MASTERS：列出所有被监视的主服务器，以及这些主服务器的当前状态。
    if (!strcasecmp(c->argv[1]->ptr,"masters")) {
        /* SENTINEL MASTERS */
        if (c->argc != 2) goto numargserr;
        // 以Redis协议的形式，输出所有主节点实例
        addReplyDictOfRedisInstances(c,sentinel.masters);

    } else if (!strcasecmp(c->argv[1]->ptr,"master")) {
        /* SENTINEL MASTER <name> */
        sentinelRedisInstance *ri;

        if (c->argc != 3) goto numargserr;
        // 根据name找到对应的主节点实例
        if ((ri = sentinelGetMasterByNameOrReplyError(c,c->argv[2]))
            == NULL) return;
        // 以Redis协议形式回复ri实例的情况给client
        addReplySentinelRedisInstance(c,ri);

    } else if (!strcasecmp(c->argv[1]->ptr,"slaves")) {
        /* SENTINEL SLAVES <master-name> */
        sentinelRedisInstance *ri;

        if (c->argc != 3) goto numargserr;
        // 根据name找到对应的主节点实例
        if ((ri = sentinelGetMasterByNameOrReplyError(c,c->argv[2])) == NULL)
            return;
        // 以Redis协议的形式，输出所有从节点实例
        addReplyDictOfRedisInstances(c,ri->slaves);

    } else if (!strcasecmp(c->argv[1]->ptr,"sentinels")) {
        /* SENTINEL SENTINELS <master-name> */
        sentinelRedisInstance *ri;

        if (c->argc != 3) goto numargserr;
        // 根据name找到对应的主节点实例
        if ((ri = sentinelGetMasterByNameOrReplyError(c,c->argv[2])) == NULL)
            return;
        // 以Redis协议的形式，输出所有Sentinel节点实例
        addReplyDictOfRedisInstances(c,ri->sentinels);

    } else if (!strcasecmp(c->argv[1]->ptr,"is-master-down-by-addr")) {
        /* SENTINEL IS-MASTER-DOWN-BY-ADDR <ip> <port> <current-epoch> <runid>
         *
         * Arguments:
         *
         * ip and port are the ip and port of the master we want to be
         * checked by Sentinel. Note that the command will not check by
         * name but just by master, in theory different Sentinels may monitor
         * differnet masters with the same name.
         *
         * current-epoch is needed in order to understand if we are allowed
         * to vote for a failover leader or not. Each Sentinel can vote just
         * one time per epoch.
         *
         * runid is "*" if we are not seeking for a vote from the Sentinel
         * in order to elect the failover leader. Otherwise it is set to the
         * runid we want the Sentinel to vote if it did not already voted.
         */
        // SENTINEL IS-MASTER-DOWN-BY-ADDR <ip> <port> <current-epoch> <runid>
        // <ip> <port> 是我们想要检查的主节点地址
        // <current-epoch> 是选举领头时所需要的。每一个Sentinel节点只能投1票
        // <runid> 是"*"，表示不进行投票而是向选择一个故障转移的领头。否则被设置为runid，表示用于选举领头Sentinel
        sentinelRedisInstance *ri;
        long long req_epoch;
        uint64_t leader_epoch = 0;
        char *leader = NULL;
        long port;
        int isdown = 0;

        if (c->argc != 6) goto numargserr;
        // 获取port和epoch
        if (getLongFromObjectOrReply(c,c->argv[3],&port,NULL) != C_OK ||
            getLongLongFromObjectOrReply(c,c->argv[4],&req_epoch,NULL)
                                                              != C_OK)
            return;
        // 根据port和ip查找主节点
        ri = getSentinelRedisInstanceByAddrAndRunID(sentinel.masters,
            c->argv[2]->ptr,port,NULL);

        /* It exists? Is actually a master? Is subjectively down? It's down.
         * Note: if we are in tilt mode we always reply with "0". */
        // port和ip的主节点存在，没有开启TILT模式，且是主观下线的状态
        if (!sentinel.tilt && ri && (ri->flags & SRI_S_DOWN) &&
                                    (ri->flags & SRI_MASTER))
            isdown = 1; //设置下线的标志

        /* Vote for the master (or fetch the previous vote) if the request
         * includes a runid, otherwise the sender is not seeking for a vote. */
        // 如果请求包含一个runid，投票给主节点（或取得上一票），否则Sentinel不投票用于选举领头Sentinel
        // 如果port和ip主节点存在，且<runid>被指定为"*"
        if (ri && ri->flags & SRI_MASTER && strcasecmp(c->argv[5]->ptr,"*")) {
            // 为ri实例投一票
            leader = sentinelVoteLeader(ri,(uint64_t)req_epoch,
                                            c->argv[5]->ptr,
                                            &leader_epoch);
        }

        /* Reply with a three-elements multi-bulk reply:
         * down state, leader, vote epoch. */
        // 返回3条信息
        addReplyMultiBulkLen(c,3);
        // 1. 下线信息
        addReply(c, isdown ? shared.cone : shared.czero);
        // 2. 领头的runid
        addReplyBulkCString(c, leader ? leader : "*");
        // 3. 领头的纪元
        addReplyLongLong(c, (long long)leader_epoch);
        if (leader) sdsfree(leader);

    } else if (!strcasecmp(c->argv[1]->ptr,"reset")) {
        /* SENTINEL RESET <pattern> */
        if (c->argc != 3) goto numargserr;
        // 重置指定pattern的主节点，返回重置主节点的个数
        addReplyLongLong(c,sentinelResetMastersByPattern(c->argv[2]->ptr,SENTINEL_GENERATE_EVENT));

    } else if (!strcasecmp(c->argv[1]->ptr,"get-master-addr-by-name")) {
        /* SENTINEL GET-MASTER-ADDR-BY-NAME <master-name> */
        sentinelRedisInstance *ri;

        if (c->argc != 3) goto numargserr;
        // 根据name找到对应的主节点实例
        ri = sentinelGetMasterByName(c->argv[2]->ptr);
        if (ri == NULL) {
            addReply(c,shared.nullmultibulk);
        } else {
            // 获取当前主节点的地址
            sentinelAddr *addr = sentinelGetCurrentMasterAddress(ri);
            // 回复IP和端口
            addReplyMultiBulkLen(c,2);
            addReplyBulkCString(c,addr->ip);
            addReplyBulkLongLong(c,addr->port);
        }

    } else if (!strcasecmp(c->argv[1]->ptr,"failover")) {
        /* SENTINEL FAILOVER <master-name> */
        sentinelRedisInstance *ri;

        if (c->argc != 3) goto numargserr;
        // 根据name找到对应的主节点实例
        if ((ri = sentinelGetMasterByNameOrReplyError(c,c->argv[2])) == NULL)
            return;
        // 如果主节点处于故障转移状态，回复-INPROG错误
        if (ri->flags & SRI_FAILOVER_IN_PROGRESS) {
            addReplySds(c,sdsnew("-INPROG Failover already in progress\r\n"));
            return;
        }
        // 选择晋升的从节点
        if (sentinelSelectSlave(ri) == NULL) {
            addReplySds(c,sdsnew("-NOGOODSLAVE No suitable slave to promote\r\n"));
            return;
        }
        serverLog(LL_WARNING,"Executing user requested FAILOVER of '%s'",
            ri->name);
        // 设置主节点状态为开始故障转移
        sentinelStartFailover(ri);
        ri->flags |= SRI_FORCE_FAILOVER;
        addReply(c,shared.ok);

    } else if (!strcasecmp(c->argv[1]->ptr,"pending-scripts")) {
        /* SENTINEL PENDING-SCRIPTS */

        if (c->argc != 2) goto numargserr;
        // 回复脚本队列的所有脚本的状态
        sentinelPendingScriptsCommand(c);

    } else if (!strcasecmp(c->argv[1]->ptr,"monitor")) {
        /* SENTINEL MONITOR <name> <ip> <port> <quorum> */
        sentinelRedisInstance *ri;
        long quorum, port;
        char ip[NET_IP_STR_LEN];

        if (c->argc != 6) goto numargserr;
        // 获取投票数
        if (getLongFromObjectOrReply(c,c->argv[5],&quorum,"Invalid quorum")
            != C_OK) return;
        // 获取端口
        if (getLongFromObjectOrReply(c,c->argv[4],&port,"Invalid port")
            != C_OK) return;

        if (quorum <= 0) {
            addReplyError(c, "Quorum must be 1 or greater.");
            return;
        }

        /* Make sure the IP field is actually a valid IP before passing it
         * to createSentinelRedisInstance(), otherwise we may trigger a
         * DNS lookup at runtime. */
        // 解析IP地址
        if (anetResolveIP(NULL,c->argv[3]->ptr,ip,sizeof(ip)) == ANET_ERR) {
            addReplyError(c,"Invalid IP address specified");
            return;
        }

        /* Parameters are valid. Try to create the master instance. */
        // 以上参数合法，创建一个主节点实例，添加到Sentinel监控的主节点中
        ri = createSentinelRedisInstance(c->argv[2]->ptr,SRI_MASTER,
                c->argv[3]->ptr,port,quorum,NULL);
        if (ri == NULL) {
            switch(errno) {
            case EBUSY:
                addReplyError(c,"Duplicated master name");
                break;
            case EINVAL:
                addReplyError(c,"Invalid port number");
                break;
            default:
                addReplyError(c,"Unspecified error adding the instance");
                break;
            }
        } else {
            // 成功创建则刷新配置，"+monitor"事件通知，回复ok
            sentinelFlushConfig();
            sentinelEvent(LL_WARNING,"+monitor",ri,"%@ quorum %d",ri->quorum);
            addReply(c,shared.ok);
        }

    } else if (!strcasecmp(c->argv[1]->ptr,"flushconfig")) {
        if (c->argc != 2) goto numargserr;
        // 则刷新配置，回复ok
        sentinelFlushConfig();
        addReply(c,shared.ok);
        return;

    } else if (!strcasecmp(c->argv[1]->ptr,"remove")) {
        /* SENTINEL REMOVE <name> */
        sentinelRedisInstance *ri;

        if (c->argc != 3) goto numargserr;
        // 根据name找到对应的主节点实例
        if ((ri = sentinelGetMasterByNameOrReplyError(c,c->argv[2]))
            == NULL) return;
        sentinelEvent(LL_WARNING,"-monitor",ri,"%@");
        // 从主节点字典中删除该主节点
        dictDelete(sentinel.masters,c->argv[2]->ptr);
        // 刷新配置，回复ok
        sentinelFlushConfig();
        addReply(c,shared.ok);

    } else if (!strcasecmp(c->argv[1]->ptr,"ckquorum")) {
        /* SENTINEL CKQUORUM <name> */
        sentinelRedisInstance *ri;
        int usable;

        if (c->argc != 3) goto numargserr;
        // 根据name找到对应的主节点实例
        if ((ri = sentinelGetMasterByNameOrReplyError(c,c->argv[2]))
            == NULL) return;
        // 获取ri主节点的投票能力
        int result = sentinelIsQuorumReachable(ri,&usable);
        // 打印不同投票能力的信息
        if (result == SENTINEL_ISQR_OK) {
            addReplySds(c, sdscatfmt(sdsempty(),
                "+OK %i usable Sentinels. Quorum and failover authorization "
                "can be reached\r\n",usable));
        } else {
            sds e = sdscatfmt(sdsempty(),
                "-NOQUORUM %i usable Sentinels. ",usable);
            if (result & SENTINEL_ISQR_NOQUORUM)
                e = sdscat(e,"Not enough available Sentinels to reach the"
                             " specified quorum for this master");
            if (result & SENTINEL_ISQR_NOAUTH) {
                if (result & SENTINEL_ISQR_NOQUORUM) e = sdscat(e,". ");
                e = sdscat(e, "Not enough available Sentinels to reach the"
                              " majority and authorize a failover");
            }
            e = sdscat(e,"\r\n");
            addReplySds(c,e);
        }

    // SENTINEL SET <mastername> [<option> <value> ...]
    } else if (!strcasecmp(c->argv[1]->ptr,"set")) {
        if (c->argc < 3 || c->argc % 2 == 0) goto numargserr;
        sentinelSetCommand(c);

    } else if (!strcasecmp(c->argv[1]->ptr,"info-cache")) {
        /* SENTINEL INFO-CACHE <name> */
        if (c->argc < 2) goto numargserr;
        mstime_t now = mstime();

        /* Create an ad-hoc dictionary type so that we can iterate
         * a dictionary composed of just the master groups the user
         * requested. */
        // 创建一个专门的字典类型以便通过用户设置的方法正确迭代
        dictType copy_keeper = instancesDictType;
        copy_keeper.valDestructor = NULL;
        dict *masters_local = sentinel.masters;
        if (c->argc > 2) {
            masters_local = dictCreate(&copy_keeper, NULL);
            for (int i = 2; i < c->argc; i++) {
                sentinelRedisInstance *ri;
                // 获取name指定的主节点实例
                ri = sentinelGetMasterByName(c->argv[i]->ptr);
                if (!ri) continue; /* ignore non-existing names */
                // 将该实例缓存在新字典中
                dictAdd(masters_local, ri->name, ri);
            }
        }

        /* Reply format:
         *   1.) master name
         *   2.) 1.) info from master
         *       2.) info from replica
         *       ...
         *   3.) other master name
         *   ...
         */
        // 添加格式回复，先添加长度
        addReplyMultiBulkLen(c,dictSize(masters_local) * 2);

        dictIterator  *di;
        dictEntry *de;
        di = dictGetIterator(masters_local);
        // 迭代缓存字典
        while ((de = dictNext(di)) != NULL) {
            sentinelRedisInstance *ri = dictGetVal(de);
            // 回复当前主节点实例的信息
            addReplyBulkCBuffer(c,ri->name,strlen(ri->name));
            addReplyMultiBulkLen(c,dictSize(ri->slaves) + 1); /* +1 for self */
            addReplyMultiBulkLen(c,2);
            addReplyLongLong(c, now - ri->info_refresh);
            if (ri->info)
                addReplyBulkCBuffer(c,ri->info,sdslen(ri->info));
            else
                addReply(c,shared.nullbulk);

            dictIterator *sdi;
            dictEntry *sde;
            sdi = dictGetIterator(ri->slaves);
            // 回复该主节点的从节点实例的信息
            while ((sde = dictNext(sdi)) != NULL) {
                sentinelRedisInstance *sri = dictGetVal(sde);
                addReplyMultiBulkLen(c,2);
                addReplyLongLong(c, now - sri->info_refresh);
                if (sri->info)
                    addReplyBulkCBuffer(c,sri->info,sdslen(sri->info));
                else
                    addReply(c,shared.nullbulk);
            }
            dictReleaseIterator(sdi);
        }
        dictReleaseIterator(di);
        if (masters_local != sentinel.masters) dictRelease(masters_local);

    } else if (!strcasecmp(c->argv[1]->ptr,"simulate-failure")) {
        /* SENTINEL SIMULATE-FAILURE <flag> <flag> ... <flag> */
        int j;
        // 模拟故障，打印不同类型模拟的日志
        sentinel.simfailure_flags = SENTINEL_SIMFAILURE_NONE;
        for (j = 2; j < c->argc; j++) {
            if (!strcasecmp(c->argv[j]->ptr,"crash-after-election")) {
                sentinel.simfailure_flags |=
                    SENTINEL_SIMFAILURE_CRASH_AFTER_ELECTION;
                serverLog(LL_WARNING,"Failure simulation: this Sentinel "
                    "will crash after being successfully elected as failover "
                    "leader");
            } else if (!strcasecmp(c->argv[j]->ptr,"crash-after-promotion")) {
                sentinel.simfailure_flags |=
                    SENTINEL_SIMFAILURE_CRASH_AFTER_PROMOTION;
                serverLog(LL_WARNING,"Failure simulation: this Sentinel "
                    "will crash after promoting the selected slave to master");
            } else if (!strcasecmp(c->argv[j]->ptr,"help")) {
                addReplyMultiBulkLen(c,2);
                addReplyBulkCString(c,"crash-after-election");
                addReplyBulkCString(c,"crash-after-promotion");
            } else {
                addReplyError(c,"Unknown failure simulation specified");
                return;
            }
        }
        addReply(c,shared.ok);
    } else {
        addReplyErrorFormat(c,"Unknown sentinel subcommand '%s'",
                               (char*)c->argv[1]->ptr);
    }
    return;

numargserr:
    addReplyErrorFormat(c,"Wrong number of arguments for 'sentinel %s'",
                          (char*)c->argv[1]->ptr);
}

#define info_section_from_redis(section_name) do { \
    if (defsections || allsections || !strcasecmp(section,section_name)) { \
        sds redissection; \
        if (sections++) info = sdscat(info,"\r\n"); \
        redissection = genRedisInfoString(section_name); \
        info = sdscatlen(info,redissection,sdslen(redissection)); \
        sdsfree(redissection); \
    } \
} while(0)

/* SENTINEL INFO [section] */
// SENTINEL模式下的INFO [section]命令实现
void sentinelInfoCommand(client *c) {
    if (c->argc > 2) {
        addReply(c,shared.syntaxerr);
        return;
    }

    int defsections = 0, allsections = 0;
    char *section = c->argc == 2 ? c->argv[1]->ptr : NULL;
    // 获取指定[section]的标识，如果是all或者default，则输出所有INFO信息
    if (section) {
        allsections = !strcasecmp(section,"all");
        defsections = !strcasecmp(section,"default");
    } else {
        defsections = 1;
    }

    int sections = 0;
    sds info = sdsempty();

    info_section_from_redis("server");
    info_section_from_redis("clients");
    info_section_from_redis("cpu");
    info_section_from_redis("stats");

    if (defsections || allsections || !strcasecmp(section,"sentinel")) {
        dictIterator *di;
        dictEntry *de;
        int master_id = 0;

        if (sections++) info = sdscat(info,"\r\n");
        info = sdscatprintf(info,
            "# Sentinel\r\n"
            "sentinel_masters:%lu\r\n"
            "sentinel_tilt:%d\r\n"
            "sentinel_running_scripts:%d\r\n"
            "sentinel_scripts_queue_length:%ld\r\n"
            "sentinel_simulate_failure_flags:%lu\r\n",
            dictSize(sentinel.masters),
            sentinel.tilt,
            sentinel.running_scripts,
            listLength(sentinel.scripts_queue),
            sentinel.simfailure_flags);

        di = dictGetIterator(sentinel.masters);
        while((de = dictNext(di)) != NULL) {
            sentinelRedisInstance *ri = dictGetVal(de);
            char *status = "ok";

            if (ri->flags & SRI_O_DOWN) status = "odown";
            else if (ri->flags & SRI_S_DOWN) status = "sdown";
            info = sdscatprintf(info,
                "master%d:name=%s,status=%s,address=%s:%d,"
                "slaves=%lu,sentinels=%lu\r\n",
                master_id++, ri->name, status,
                ri->addr->ip, ri->addr->port,
                dictSize(ri->slaves),
                dictSize(ri->sentinels)+1);
        }
        dictReleaseIterator(di);
    }

    addReplyBulkSds(c, info);
}

/* Implements Sentinel verison of the ROLE command. The output is
 * "sentinel" and the list of currently monitored master names. */
// SENTINEL模式下ROLE命令的实现。输出的是Sentinel的角色名称和Sentinel节点监控的所有主节点名字
void sentinelRoleCommand(client *c) {
    dictIterator *di;
    dictEntry *de;
    // 输出Sentinel的角色名称
    addReplyMultiBulkLen(c,2);
    addReplyBulkCBuffer(c,"sentinel",8);
    addReplyMultiBulkLen(c,dictSize(sentinel.masters));
    // 输出Sentinel节点监控的所有主节点名字
    di = dictGetIterator(sentinel.masters);
    while((de = dictNext(di)) != NULL) {
        sentinelRedisInstance *ri = dictGetVal(de);

        addReplyBulkCString(c,ri->name);
    }
    dictReleaseIterator(di);
}

/* SENTINEL SET <mastername> [<option> <value> ...] */
void sentinelSetCommand(client *c) {
    sentinelRedisInstance *ri;
    int j, changes = 0;
    char *option, *value;
    // 根据mastername查找对应的主节点实例
    if ((ri = sentinelGetMasterByNameOrReplyError(c,c->argv[2]))
        == NULL) return;

    /* Process option - value pairs. */
    for (j = 3; j < c->argc; j += 2) {
        option = c->argv[j]->ptr;
        value = c->argv[j+1]->ptr;
        robj *o = c->argv[j+1];
        long long ll;

        if (!strcasecmp(option,"down-after-milliseconds")) {
            /* down-after-millisecodns <milliseconds> */
            // 获取milliseconds值并设置down_after_period
            if (getLongLongFromObject(o,&ll) == C_ERR || ll <= 0)
                goto badfmt;
            ri->down_after_period = ll;
            // 根据ri主节点的down_after_period字段的值设置所有连接该主节点的从节点和Sentinel实例的主观下线的判断时间
            sentinelPropagateDownAfterPeriod(ri);
            changes++;
        } else if (!strcasecmp(option,"failover-timeout")) {
            /* failover-timeout <milliseconds> */
            // 获取milliseconds值并设置failover_timeout
            if (getLongLongFromObject(o,&ll) == C_ERR || ll <= 0)
                goto badfmt;
            ri->failover_timeout = ll;
            changes++;
       } else if (!strcasecmp(option,"parallel-syncs")) {
            /* parallel-syncs <milliseconds> */
            // 获取milliseconds值并设置parallel_syncs
            if (getLongLongFromObject(o,&ll) == C_ERR || ll <= 0)
                goto badfmt;
            ri->parallel_syncs = ll;
            changes++;
       } else if (!strcasecmp(option,"notification-script")) {
            /* notification-script <path> */
            // 获取路径长度且判断该脚本是否具有可执行权限
            if (strlen(value) && access(value,X_OK) == -1) {
                addReplyError(c,
                    "Notification script seems non existing or non executable");
                if (changes) sentinelFlushConfig();
                return;
            }
            sdsfree(ri->notification_script);
            // 设置脚本路径
            ri->notification_script = strlen(value) ? sdsnew(value) : NULL;
            changes++;
       } else if (!strcasecmp(option,"client-reconfig-script")) {
            /* client-reconfig-script <path> */
            // 获取路径长度且判断该脚本是否具有可执行权限
            if (strlen(value) && access(value,X_OK) == -1) {
                addReplyError(c,
                    "Client reconfiguration script seems non existing or "
                    "non executable");
                if (changes) sentinelFlushConfig();
                return;
            }
            sdsfree(ri->client_reconfig_script);
            // 设置脚本路径
            ri->client_reconfig_script = strlen(value) ? sdsnew(value) : NULL;
            changes++;
       } else if (!strcasecmp(option,"auth-pass")) {
            /* auth-pass <password> */
            sdsfree(ri->auth_pass);
            // 设置认证密码
            ri->auth_pass = strlen(value) ? sdsnew(value) : NULL;
            changes++;
       } else if (!strcasecmp(option,"quorum")) {
            /* quorum <count> */
            // 获取投票个数，并设置quorum
            if (getLongLongFromObject(o,&ll) == C_ERR || ll <= 0)
                goto badfmt;
            ri->quorum = ll;
            changes++;
        } else {
            addReplyErrorFormat(c,"Unknown option '%s' for SENTINEL SET",
                option);
            if (changes) sentinelFlushConfig();
            return;
        }
        sentinelEvent(LL_WARNING,"+set",ri,"%@ %s %s",option,value);
    }
    // 如果发生改变则刷新配置
    if (changes) sentinelFlushConfig();
    addReply(c,shared.ok);
    return;

// 格式错误
badfmt: /* Bad format errors */
    if (changes) sentinelFlushConfig();
    addReplyErrorFormat(c,"Invalid argument '%s' for SENTINEL SET '%s'",
            value, option);
}

/* Our fake PUBLISH command: it is actually useful only to receive hello messages
 * from the other sentinel instances, and publishing to a channel other than
 * SENTINEL_HELLO_CHANNEL is forbidden.
 *
 * Because we have a Sentinel PUBLISH, the code to send hello messages is the same
 * for all the three kind of instances: masters, slaves, sentinels. */
// 伪PUBLISH命令，只有用在从其他Sentinel节点实例接受hello信息时，去执行发布信息操作，而且只能发布到SENTINEL_HELLO_CHANNEL频道中，因为我们有Sentinel模式的PUBLISH命令，发送hello信息的代码对于主节点、从节点或Sentinel节点都是一样的
void sentinelPublishCommand(client *c) {
    // 限制发送的频道，只能发布SENTINEL_HELLO_CHANNEL频道中
    if (strcmp(c->argv[1]->ptr,SENTINEL_HELLO_CHANNEL)) {
        addReplyError(c, "Only HELLO messages are accepted by Sentinel instances.");
        return;
    }
    // 处理从主节点或从节点通过频道发送过来的hello信息
    sentinelProcessHelloMessage(c->argv[2]->ptr,sdslen(c->argv[2]->ptr));
    addReplyLongLong(c,1);
}

/* ===================== SENTINEL availability checks ======================= */

/* Is this instance down from our point of view? */
// 检查ri实例是否下线
void sentinelCheckSubjectivelyDown(sentinelRedisInstance *ri) {
    mstime_t elapsed = 0;
    // 获取ri实例回复命令已经过去的时长
    if (ri->link->act_ping_time)
        // 获取最近一次发送PING命令过去了多少时间
        elapsed = mstime() - ri->link->act_ping_time;
    // 如果实例的连接已经断开
    else if (ri->link->disconnected)
        // 获取最近一次回复PING命令过去了多少时间
        elapsed = mstime() - ri->link->last_avail_time;

    /* Check if we are in need for a reconnection of one of the
     * links, because we are detecting low activity.
     *
     * 1) Check if the command link seems connected, was connected not less
     *    than SENTINEL_MIN_LINK_RECONNECT_PERIOD, but still we have a
     *    pending ping for more than half the timeout. */
    // 如果连接处于低活跃度，那么进行重新连接
    // cc命令连接超过了1.5s，并且之前发送过PING命令但是连接活跃度很低
    if (ri->link->cc &&
        (mstime() - ri->link->cc_conn_time) >
        SENTINEL_MIN_LINK_RECONNECT_PERIOD &&
        ri->link->act_ping_time != 0 && /* Ther is a pending ping... */
        /* The pending ping is delayed, and we did not received
         * error replies as well. */
        (mstime() - ri->link->act_ping_time) > (ri->down_after_period/2) &&
        (mstime() - ri->link->last_pong_time) > (ri->down_after_period/2))
    {   // 断开ri实例的cc命令连接
        instanceLinkCloseConnection(ri->link,ri->link->cc);
    }

    /* 2) Check if the pubsub link seems connected, was connected not less
     *    than SENTINEL_MIN_LINK_RECONNECT_PERIOD, but still we have no
     *    activity in the Pub/Sub channel for more than
     *    SENTINEL_PUBLISH_PERIOD * 3.
     */
    // 检查pc发布订阅的连接是否也处于低活跃状态
    if (ri->link->pc &&
        (mstime() - ri->link->pc_conn_time) >
         SENTINEL_MIN_LINK_RECONNECT_PERIOD &&
        (mstime() - ri->link->pc_last_activity) > (SENTINEL_PUBLISH_PERIOD*3))
    {   // 断开ri实例的pc发布订阅连接
        instanceLinkCloseConnection(ri->link,ri->link->pc);
    }

    /* Update the SDOWN flag. We believe the instance is SDOWN if:
     *
     * 1) It is not replying.
     * 2) We believe it is a master, it reports to be a slave for enough time
     *    to meet the down_after_period, plus enough time to get two times
     *    INFO report from the instance. */
    // 更新主观下线标志，条件如下：
    /*
        1. 没有回复命令
        2. Sentinel节点认为ri是主节点，但是它报告它是从节点
    */
    // ri实例回复命令已经过去的时长已经超过主观下线的时限，并且ri实例是主节点，但是报告是从节点
    if (elapsed > ri->down_after_period ||
        (ri->flags & SRI_MASTER &&
         ri->role_reported == SRI_SLAVE &&
         mstime() - ri->role_reported_time >
          (ri->down_after_period+SENTINEL_INFO_PERIOD*2)))
    {
        /* Is subjectively down */
        // 设置主观下线的标识
        if ((ri->flags & SRI_S_DOWN) == 0) {
            // 发送"+sdown"的事件通知
            sentinelEvent(LL_WARNING,"+sdown",ri,"%@");
            // 设置实例被判断主观下线的时间
            ri->s_down_since_time = mstime();
            ri->flags |= SRI_S_DOWN;
        }
    } else {
        /* Is subjectively up */
        // 如果设置了主观下线的标识，则取消标识
        if (ri->flags & SRI_S_DOWN) {
            sentinelEvent(LL_WARNING,"-sdown",ri,"%@");
            ri->flags &= ~(SRI_S_DOWN|SRI_SCRIPT_KILL_SENT);
        }
    }
}

/* Is this instance down according to the configured quorum?
 *
 * Note that ODOWN is a weak quorum, it only means that enough Sentinels
 * reported in a given time range that the instance was not reachable.
 * However messages can be delayed so there are no strong guarantees about
 * N instances agreeing at the same time about the down state. */
// 根据配置的投票数，判断实例是否下线
// 客观下线意味这有足够多的Sentinel节点报告该实例在一个时间范围内不可达。但是信息可能被延迟，不能保证N个实例在同一时间都同意该实例进入下线状态。所以说 ODOWN 是虚弱的
void sentinelCheckObjectivelyDown(sentinelRedisInstance *master) {
    dictIterator *di;
    dictEntry *de;
    unsigned int quorum = 0, odown = 0;
    // 如果该master实例已经被当前Sentinel节点判断为客观下线
    if (master->flags & SRI_S_DOWN) {
        /* Is down for enough sentinels? */
        // 当前Sentinel节点认为下线投1票
        quorum = 1; /* the current sentinel. */
        /* Count all the other sentinels. */
        di = dictGetIterator(master->sentinels);
        // 遍历监控该master实例的所有的Sentinel节点
        while((de = dictNext(di)) != NULL) {
            sentinelRedisInstance *ri = dictGetVal(de);
            // 如果Sentinel也认为master实例客观下线，那么增加投票数
            if (ri->flags & SRI_MASTER_DOWN) quorum++;
        }
        dictReleaseIterator(di);
        // 如果超过master设置的客观下线票数，则设置客观下线标识
        if (quorum >= master->quorum) odown = 1;
    }

    /* Set the flag accordingly to the outcome. */
    // 如果被判断为客观下线
    if (odown) {
        // master没有客观下线标识则要设置
        if ((master->flags & SRI_O_DOWN) == 0) {
            // 发送"+odown"事件通知
            sentinelEvent(LL_WARNING,"+odown",master,"%@ #quorum %d/%d",
                quorum, master->quorum);
            // 设置master客观下线标识
            master->flags |= SRI_O_DOWN;
            // 设置master被判断客观下线的时间
            master->o_down_since_time = mstime();
        }
    // master实例没有客观下线
    } else {
        // 取消master客观下线标识
        if (master->flags & SRI_O_DOWN) {
            // 发送"-odown"事件通知
            sentinelEvent(LL_WARNING,"-odown",master,"%@");
            master->flags &= ~SRI_O_DOWN;
        }
    }
}

/* Receive the SENTINEL is-master-down-by-addr reply, see the
 * sentinelAskMasterStateToOtherSentinels() function for more information. */
// 接受Sentinel关于is-master-down-by-addr的回复
void sentinelReceiveIsMasterDownReply(redisAsyncContext *c, void *reply, void *privdata) {
    sentinelRedisInstance *ri = privdata;
    instanceLink *link = c->data;
    redisReply *r;

    if (!reply || !link) return;
    // 已发送未回复的命令个数减1
    link->pending_commands--;
    r = reply;

    /* Ignore every error or unexpected reply.
     * Note that if the command returns an error for any reason we'll
     * end clearing the SRI_MASTER_DOWN flag for timeout anyway. */
    // 忽略错误回复。
    // 如果回复的满足：integer string integer类型
    if (r->type == REDIS_REPLY_ARRAY && r->elements == 3 &&
        r->element[0]->type == REDIS_REPLY_INTEGER &&
        r->element[1]->type == REDIS_REPLY_STRING &&
        r->element[2]->type == REDIS_REPLY_INTEGER)
    {
        // 设置最近一次回复下线状态的时间
        ri->last_master_down_reply_time = mstime();
        // 如果回复的第一个元素是1，表示Sentinel同意客观下线
        if (r->element[0]->integer == 1) {
            // 设置主节点客观下线标识
            ri->flags |= SRI_MASTER_DOWN;
        } else {
            // 否则取消主节点客观下线标识
            ri->flags &= ~SRI_MASTER_DOWN;
        }
        // 如果回复的第一个元素不是"*"，表示这是带投票的回复
        if (strcmp(r->element[1]->str,"*")) {
            /* If the runid in the reply is not "*" the Sentinel actually
             * replied with a vote. */
            // 释放之前的选举的领头
            sdsfree(ri->leader);
            // 打印日志
            if ((long long)ri->leader_epoch != r->element[2]->integer)
                serverLog(LL_WARNING,
                    "%s voted for %s %llu", ri->name,
                    r->element[1]->str,
                    (unsigned long long) r->element[2]->integer);
            // 设置实例的领头
            ri->leader = sdsnew(r->element[1]->str);
            ri->leader_epoch = r->element[2]->integer;
        }
    }
}

/* If we think the master is down, we start sending
 * SENTINEL IS-MASTER-DOWN-BY-ADDR requests to other sentinels
 * in order to get the replies that allow to reach the quorum
 * needed to mark the master in ODOWN state and trigger a failover. */
// 如果Sentinel认为主节点下线，发送一个SENTINEL IS-MASTER-DOWN-BY-ADDR给所有的Sentinel获取回复，尝试获得足够的票数，标记主节点为客观下线状态，触发故障转移
#define SENTINEL_ASK_FORCED (1<<0)
void sentinelAskMasterStateToOtherSentinels(sentinelRedisInstance *master, int flags) {
    dictIterator *di;
    dictEntry *de;

    di = dictGetIterator(master->sentinels);
    // 遍历监控master的所有的Sentinel节点
    while((de = dictNext(di)) != NULL) {
        sentinelRedisInstance *ri = dictGetVal(de);
        // 当前Sentinel实例最近一个回复SENTINEL IS-MASTER-DOWN-BY-ADDR命令所过去的时间
        mstime_t elapsed = mstime() - ri->last_master_down_reply_time;
        char port[32];
        int retval;

        /* If the master state from other sentinel is too old, we clear it. */
        // 如果master状态太旧没有更新，则清除它保存的主节点状态
        if (elapsed > SENTINEL_ASK_PERIOD*5) {
            ri->flags &= ~SRI_MASTER_DOWN;
            sdsfree(ri->leader);
            ri->leader = NULL;
        }

        /* Only ask if master is down to other sentinels if:
         *
         * 1) We believe it is down, or there is a failover in progress.
         * 2) Sentinel is connected.
         * 3) We did not received the info within SENTINEL_ASK_PERIOD ms. */
        // 满足以下条件向其他Sentinel节点询问主节点是否下线
        /*
            1. 当前Sentinel节点认为它已经下线，并且处于故障转移状态
            2. 其他Sentinel与当前Sentinel保持连接状态
            3. 在SENTINEL_ASK_PERIOD毫秒内没有收到INFO回复
        */
        // 主节点没有处于客观下线状态，则跳过当前Sentinel节点
        if ((master->flags & SRI_S_DOWN) == 0) continue;
        // 如果当前Sentinel节点断开连接，也跳过
        if (ri->link->disconnected) continue;
        // 最近回复SENTINEL IS-MASTER-DOWN-BY-ADDR命令在SENTINEL_ASK_PERIODms时间内已经回复过了，则跳过
        if (!(flags & SENTINEL_ASK_FORCED) &&
            mstime() - ri->last_master_down_reply_time < SENTINEL_ASK_PERIOD)
            continue;

        /* Ask */
        // 发送SENTINEL IS-MASTER-DOWN-BY-ADDR命令
        ll2string(port,sizeof(port),master->addr->port);
        // 异步发送命令
        retval = redisAsyncCommand(ri->link->cc,
                    sentinelReceiveIsMasterDownReply, ri,
                    "SENTINEL is-master-down-by-addr %s %s %llu %s",
                    master->addr->ip, port,
                    sentinel.current_epoch,
                    // 如果主节点处于故障转移的状态，那么发送该Sentinel的ID，让收到命令的Sentinel节点选举自己为领头
                    // 否则发送"*"表示发送投票
                    (master->failover_state > SENTINEL_FAILOVER_STATE_NONE) ?
                    sentinel.myid : "*");
        // 已发送未回复的命令个数加1
        if (retval == C_OK) ri->link->pending_commands++;
    }
    dictReleaseIterator(di);
}

/* =============================== FAILOVER ================================= */

/* Crash because of user request via SENTINEL simulate-failure command. */
// 退出程序，退出码99
void sentinelSimFailureCrash(void) {
    serverLog(LL_WARNING,
        "Sentinel CRASH because of SENTINEL simulate-failure");
    exit(99);
}

/* Vote for the sentinel with 'req_runid' or return the old vote if already
 * voted for the specifed 'req_epoch' or one greater.
 *
 * If a vote is not available returns NULL, otherwise return the Sentinel
 * runid and populate the leader_epoch with the epoch of the vote. */
// 为运行ID为'req_runid'的Sentinel节点投一票，
// 如果Sentinel在req_epoch纪元已经投过票了，那么返回之前投的票
// 如果Sentinel已经为大于req_epoch的纪元投过票，那么返回更大纪元的投票
// 如果投票不可用，则返回NULL，否则返回Sentinel的运行ID并且将被投票的纪元保存到leader_epoch中
char *sentinelVoteLeader(sentinelRedisInstance *master, uint64_t req_epoch, char *req_runid, uint64_t *leader_epoch) {
    // 如果被投票的Sentinel节点纪元大于当前的纪元，则更新当前的纪元
    if (req_epoch > sentinel.current_epoch) {
        sentinel.current_epoch = req_epoch;
        // 刷新配置
        sentinelFlushConfig();
        // 发送"+new-epoch"事件通知
        sentinelEvent(LL_WARNING,"+new-epoch",master,"%llu",
            (unsigned long long) sentinel.current_epoch);
    }
    // master的领头纪元小于被投票的Sentinel节点纪元
    if (master->leader_epoch < req_epoch && sentinel.current_epoch <= req_epoch)
    {
        // 释放执行故障转移的Sentinel节点
        sdsfree(master->leader);
        // 重新设置执行故障转移的Sentinel节点
        master->leader = sdsnew(req_runid);
        master->leader_epoch = sentinel.current_epoch;
        sentinelFlushConfig();  //刷新配置
        // 发送"+vote-for-leader"事件通知
        sentinelEvent(LL_WARNING,"+vote-for-leader",master,"%s %llu",
            master->leader, (unsigned long long) master->leader_epoch);
        /* If we did not voted for ourselves, set the master failover start
         * time to now, in order to force a delay before we can start a
         * failover for the same master. */
        // 如果我们不投票给自己，设置主节点的故障转移开始的时间，以便在开始同一个主节点的故障转移时强制延迟一段时间
        if (strcasecmp(master->leader,sentinel.myid))
            // 随机设置一个开始的时间
            master->failover_start_time = mstime()+rand()%SENTINEL_MAX_DESYNC;
    }
    // 保存主节点的领头纪元
    *leader_epoch = master->leader_epoch;
    return master->leader ? sdsnew(master->leader) : NULL;
}

struct sentinelLeader {
    char *runid;
    unsigned long votes;
};

/* Helper function for sentinelGetLeader, increment the counter
 * relative to the specified runid. */
// 为给定运行ID的Sentinel节点增加1票
int sentinelLeaderIncr(dict *counters, char *runid) {
    dictEntry *de = dictFind(counters,runid);   //查找到runid的实例
    uint64_t oldval;

    if (de) {
        oldval = dictGetUnsignedIntegerVal(de);
        // 将票数增加1
        dictSetUnsignedIntegerVal(de,oldval+1);
        return oldval+1;
    } else {
        // 不存在则将runid加入到字典中
        de = dictAddRaw(counters,runid);
        serverAssert(de != NULL);
        // 票数为1
        dictSetUnsignedIntegerVal(de,1);
        return 1;
    }
}

/* Scan all the Sentinels attached to this master to check if there
 * is a leader for the specified epoch.
 *
 * To be a leader for a given epoch, we should have the majority of
 * the Sentinels we know (ever seen since the last SENTINEL RESET) that
 * reported the same instance as leader for the same epoch. */
// 扫描所有监控主节点的Sentinel节点，检查是否存在指定纪元的领头
// 要成为给定纪元的领头，我们应该获得大多数Sentinel节点的支持
// 选举master在指定epoch上的领头
char *sentinelGetLeader(sentinelRedisInstance *master, uint64_t epoch) {
    dict *counters;
    dictIterator *di;
    dictEntry *de;
    unsigned int voters = 0, voters_quorum;
    char *myvote;
    char *winner = NULL;
    uint64_t leader_epoch;
    uint64_t max_votes = 0;

    // 处理处于客观下线和处于故障转移状态的主节点
    serverAssert(master->flags & (SRI_O_DOWN|SRI_FAILOVER_IN_PROGRESS));
    // 创建一个投票统计字典
    counters = dictCreate(&leaderVotesDictType,NULL);
    // 设置字典的大小
    voters = dictSize(master->sentinels)+1; /* All the other sentinels and me. */

    /* Count other sentinels votes */
    di = dictGetIterator(master->sentinels);
    // 遍历所有监控master的Sentinel节点
    while((de = dictNext(di)) != NULL) {
        sentinelRedisInstance *ri = dictGetVal(de);
        // 如果ri已经有了领头，那么为其投上1票
        if (ri->leader != NULL && ri->leader_epoch == sentinel.current_epoch)
            sentinelLeaderIncr(counters,ri->leader);
    }
    dictReleaseIterator(di);

    /* Check what's the winner. For the winner to win, it needs two conditions:
     * 1) Absolute majority between voters (50% + 1).
     * 2) And anyway at least master->quorum votes. */
    // 选出胜选的leader。满足以下条件：
    /*
        1. (50% + 1)的投票数
        2. 投票数只是要有master->quorum多
    */
    di = dictGetIterator(counters);
    // 遍历投票统计字典
    while((de = dictNext(di)) != NULL) {
        // 取出当前Sentinel节点的票数
        uint64_t votes = dictGetUnsignedIntegerVal(de);
        // 选出胜选的winner
        if (votes > max_votes) {
            max_votes = votes;
            winner = dictGetKey(de);
        }
    }
    dictReleaseIterator(di);

    /* Count this Sentinel vote:
     * if this Sentinel did not voted yet, either vote for the most
     * common voted sentinel, or for itself if no vote exists at all. */
    // 计算当前的Sentinel的投票：如果当前Sentinel还没有投票，要么投票投票数最多了，要么投给自己
    // 如果有胜选的winner
    if (winner)
        // 为胜选的winner投一票
        myvote = sentinelVoteLeader(master,epoch,winner,&leader_epoch);
    else
        // 否则投给自己
        myvote = sentinelVoteLeader(master,epoch,sentinel.myid,&leader_epoch);

    if (myvote && leader_epoch == epoch) {
        // 为给定运行ID的myvote节点增加1票
        uint64_t votes = sentinelLeaderIncr(counters,myvote);
        // 更新最大票数
        if (votes > max_votes) {
            max_votes = votes;
            winner = myvote;
        }
    }
    // 计算投票的最少票数
    voters_quorum = voters/2+1;
    // 如果胜选的winner的票数少于最少的限制，或者小于配置最小的限制
    // 那么本次选举无效
    if (winner && (max_votes < voters_quorum || max_votes < master->quorum))
        winner = NULL;
    // 返回胜选的winner
    winner = winner ? sdsnew(winner) : NULL;
    sdsfree(myvote);
    dictRelease(counters);
    return winner;
}

/* Send SLAVEOF to the specified instance, always followed by a
 * CONFIG REWRITE command in order to store the new configuration on disk
 * when possible (that is, if the Redis instance is recent enough to support
 * config rewriting, and if the server was started with a configuration file).
 *
 * If Host is NULL the function sends "SLAVEOF NO ONE".
 *
 * The command returns C_OK if the SLAVEOF command was accepted for
 * (later) delivery otherwise C_ERR. The command replies are just
 * discarded. */
// 发送SLAVEOF给指定的实例，当可能时（如果实例支持配置重写，并且服务器是读取配置文件开启的），执行 CONFIG REWRITE 命令将当前配置保存到磁盘中
// 如果host是null，那么函数发送SLAVEOF NO ONE命令
// 如果SLAVEOF成功被加入队列中(异步发送命令)，返回C_OK，否则返回C_ERR。命令回复被丢弃
int sentinelSendSlaveOf(sentinelRedisInstance *ri, char *host, int port) {
    char portstr[32];
    int retval;

    ll2string(portstr,sizeof(portstr),port);

    /* If host is NULL we send SLAVEOF NO ONE that will turn the instance
     * into a master. */
    // 如果host为null，我们发送 SLAVEOF NO ONE 将该实例转变为主节点
    if (host == NULL) {
        host = "NO";
        memcpy(portstr,"ONE",4);
    }

    /* In order to send SLAVEOF in a safe way, we send a transaction performing
     * the following tasks:
     * 1) Reconfigure the instance according to the specified host/port params.
     * 2) Rewrite the configuraiton.
     * 3) Disconnect all clients (but this one sending the commnad) in order
     *    to trigger the ask-master-on-reconnection protocol for connected
     *    clients.
     *
     * Note that we don't check the replies returned by commands, since we
     * will observe instead the effects in the next INFO output. */
    // 为了安全的发送SLAVEOF命令，我们发送执行以下事务：
    /*
        1. 根据指定的 host/port 参数重新配置实例
        2. 重写配置
        3. 断开所有的client的连接（除了当前这个用发送命令）以便触发ask-master-on-reconnection 所指定的连接client个数
    */
    // 异步发送 "MULTI"命令，且丢弃命令回复
    retval = redisAsyncCommand(ri->link->cc,
        sentinelDiscardReplyCallback, ri, "MULTI");
    if (retval == C_ERR) return retval;
    // 已发送未回复的命令个数加1
    ri->link->pending_commands++;

    // 异步发送 "SLAVEOF"命令，且丢弃命令回复
    retval = redisAsyncCommand(ri->link->cc,
        sentinelDiscardReplyCallback, ri, "SLAVEOF %s %s", host, portstr);
    if (retval == C_ERR) return retval;
    // 已发送未回复的命令个数加1
    ri->link->pending_commands++;

    // 异步发送 "CONFIG REWRITE"命令，且丢弃命令回复
    retval = redisAsyncCommand(ri->link->cc,
        sentinelDiscardReplyCallback, ri, "CONFIG REWRITE");
    if (retval == C_ERR) return retval;
    // 已发送未回复的命令个数加1
    ri->link->pending_commands++;

    /* CLIENT KILL TYPE <type> is only supported starting from Redis 2.8.12,
     * however sending it to an instance not understanding this command is not
     * an issue because CLIENT is variadic command, so Redis will not
     * recognized as a syntax error, and the transaction will not fail (but
     * only the unsupported command will fail). */
    // CLIENT KILL TYPE <type> 命令从Redis 2.8.12开始支持，有可能不被识别作为语法错误
    // 异步发送 "CLIENT KILL TYPE normal"命令，且丢弃命令回复
    retval = redisAsyncCommand(ri->link->cc,
        sentinelDiscardReplyCallback, ri, "CLIENT KILL TYPE normal");
    if (retval == C_ERR) return retval;
    // 已发送未回复的命令个数加1
    ri->link->pending_commands++;

    // 异步发送 "EXEC"命令，且丢弃命令回复
    retval = redisAsyncCommand(ri->link->cc,
        sentinelDiscardReplyCallback, ri, "EXEC");
    if (retval == C_ERR) return retval;
    // 已发送未回复的命令个数加1
    ri->link->pending_commands++;

    return C_OK;
}

/* Setup the master state to start a failover. */
// 设置主节点状态为开始故障转移
void sentinelStartFailover(sentinelRedisInstance *master) {
    serverAssert(master->flags & SRI_MASTER);

    // 更新master节点的故障转移状态
    master->failover_state = SENTINEL_FAILOVER_STATE_WAIT_START;
    // 更新master节点的状态处于故障转移状态
    master->flags |= SRI_FAILOVER_IN_PROGRESS;
    // 更新纪元
    master->failover_epoch = ++sentinel.current_epoch;
    // 发送事件通知
    sentinelEvent(LL_WARNING,"+new-epoch",master,"%llu",
        (unsigned long long) sentinel.current_epoch);
    sentinelEvent(LL_WARNING,"+try-failover",master,"%@");
    // 更新故障转移的时间属性
    master->failover_start_time = mstime()+rand()%SENTINEL_MAX_DESYNC;
    master->failover_state_change_time = mstime();
}

/* This function checks if there are the conditions to start the failover,
 * that is:
 *
 * 1) Master must be in ODOWN condition.
 * 2) No failover already in progress.
 * 3) No failover already attempted recently.
 *
 * We still don't know if we'll win the election so it is possible that we
 * start the failover but that we'll not be able to act.
 *
 * Return non-zero if a failover was started. */
// 该函数用于检查是否可以开始进行故障转移：
/*
    1. 主节点必须处于客观下线状态
    2. 没有正在对主节点进行故障转移
    3. 一段时间内没有尝试进行故障转移，防止频繁执行故障转移
*/
// 我们仍然不知道是否能够赢得选举，所以有可能我们开始一个故障转移的状态的Sentinel节点，但是我们不是执行故障转移的Sentinel节点
// 如果开始故障转移，返回非零值
int sentinelStartFailoverIfNeeded(sentinelRedisInstance *master) {
    /* We can't failover if the master is not in O_DOWN state. */
    // 不处于客观下线状态
    if (!(master->flags & SRI_O_DOWN)) return 0;

    /* Failover already in progress? */
    // 已经存在Sentinel节点正在对主节点进行故障转移
    if (master->flags & SRI_FAILOVER_IN_PROGRESS) return 0;

    /* Last failover attempt started too little time ago? */
    // 如果故障转移时间相隔没有超过限制的两倍，则不进行故障转移，防止频繁执行故障转移
    if (mstime() - master->failover_start_time <
        master->failover_timeout*2)
    {
        if (master->failover_delay_logged != master->failover_start_time) {
            // 计算故障转移延迟时间
            time_t clock = (master->failover_start_time +
                            master->failover_timeout*2) / 1000;
            char ctimebuf[26];

            ctime_r(&clock,ctimebuf);
            ctimebuf[24] = '\0'; /* Remove newline. */
            // 设置故障转移开始的时间，打印到日志中
            master->failover_delay_logged = master->failover_start_time;
            serverLog(LL_WARNING,
                "Next failover delay: I will not start a failover before %s",
                ctimebuf);
        }
        return 0;
    }
    // 以上条件满足，设置主节点状态为开始故障转移
    sentinelStartFailover(master);
    return 1;
}

/* Select a suitable slave to promote. The current algorithm only uses
 * the following parameters:
 *
 * 1) None of the following conditions: S_DOWN, O_DOWN, DISCONNECTED.
 * 2) Last time the slave replied to ping no more than 5 times the PING period.
 * 3) info_refresh not older than 3 times the INFO refresh period.
 * 4) master_link_down_time no more than:
 *     (now - master->s_down_since_time) + (master->down_after_period * 10).
 *    Basically since the master is down from our POV, the slave reports
 *    to be disconnected no more than 10 times the configured down-after-period.
 *    This is pretty much black magic but the idea is, the master was not
 *    available so the slave may be lagging, but not over a certain time.
 *    Anyway we'll select the best slave according to replication offset.
 * 5) Slave priority can't be zero, otherwise the slave is discarded.
 *
 * Among all the slaves matching the above conditions we select the slave
 * with, in order of sorting key:
 *
 * - lower slave_priority.
 * - bigger processed replication offset.
 * - lexicographically smaller runid.
 *
 * Basically if runid is the same, the slave that processed more commands
 * from the master is selected.
 *
 * The function returns the pointer to the selected slave, otherwise
 * NULL if no suitable slave was found.
 */

/* Helper for sentinelSelectSlave(). This is used by qsort() in order to
 * sort suitable slaves in a "better first" order, to take the first of
 * the list. */
// 选择合适的从节点晋升为主节点，当前算法使用如下参数：
/*
    1. 不选有以下状态的从节点： S_DOWN, O_DOWN, DISCONNECTED.
    2. 最近一次回复PING命令超过5s的从节点
    3. 最近一次获取INFO命令回复的时间不超过info_refresh的三倍时间长度
    4. 主从节点之间断开操作的时间不超过：
        从当前的Sentinel节点来看，主节点处于下线状态，从节点和主节点断开连接的时间不能超过down-after-period的10倍
        这看起来非常魔幻，但是实际上，当主节点不可达时，主从连接会断开，但是必然不超过一定时间。意思是，主从断开，一定是主节点造成的，而不是从节点。无论如何，我们将根据复制偏移量选择最佳的从节点
    5. 从节点的优先级不能为0，优先级为0的从节点被抛弃

*/
// 符合以上条件的从节点，按照以下顺序排序：
/*
    - 最低的优先级的优先
    - 复制偏移量较大的优先
    - 运行runid字典序小的优先
*/
// 如果runid相同，那么选择执行命令更多的从节点
// 函数返回被选中的从节点，否则返回NULL
// 比较函数，用于选出更适合的一个
int compareSlavesForPromotion(const void *a, const void *b) {
    sentinelRedisInstance **sa = (sentinelRedisInstance **)a,
                          **sb = (sentinelRedisInstance **)b;
    char *sa_runid, *sb_runid;
    // 选择优先级更小的
    if ((*sa)->slave_priority != (*sb)->slave_priority)
        return (*sa)->slave_priority - (*sb)->slave_priority;

    /* If priority is the same, select the slave with greater replication
     * offset (processed more data from the master). */
    // 比较复制偏移量，较大的优先
    if ((*sa)->slave_repl_offset > (*sb)->slave_repl_offset) {
        return -1; /* a < b */
    } else if ((*sa)->slave_repl_offset < (*sb)->slave_repl_offset) {
        return 1; /* a > b */
    }

    /* If the replication offset is the same select the slave with that has
     * the lexicographically smaller runid. Note that we try to handle runid
     * == NULL as there are old Redis versions that don't publish runid in
     * INFO. A NULL runid is considered bigger than any other runid. */
    // 复制偏移量相同，运行runid字典序小的优先
    // 我们尝试处理runid为NULL，是因为兼容旧版本。一个NULL的runid比任何runid都大
    sa_runid = (*sa)->runid;
    sb_runid = (*sb)->runid;
    if (sa_runid == NULL && sb_runid == NULL) return 0;
    else if (sa_runid == NULL) return 1;  /* a > b */
    else if (sb_runid == NULL) return -1; /* a < b */
    return strcasecmp(sa_runid, sb_runid);
}

// 从master节点中选出一个可以晋升的从节点，如果没有则返回NULL
sentinelRedisInstance *sentinelSelectSlave(sentinelRedisInstance *master) {
    // 从节点数组
    sentinelRedisInstance **instance =
        zmalloc(sizeof(instance[0])*dictSize(master->slaves));
    sentinelRedisInstance *selected = NULL;
    int instances = 0;
    dictIterator *di;
    dictEntry *de;
    mstime_t max_master_down_time = 0;
    // master主节点处于主观下线，计算出主节点被判断为处于主观下线的最大时长
    if (master->flags & SRI_S_DOWN)
        max_master_down_time += mstime() - master->s_down_since_time;
    max_master_down_time += master->down_after_period * 10;

    di = dictGetIterator(master->slaves);
    // 迭代下线的主节点的所有从节点
    while((de = dictNext(di)) != NULL) {
        sentinelRedisInstance *slave = dictGetVal(de);
        mstime_t info_validity_time;
        // 跳过下线的从节点
        if (slave->flags & (SRI_S_DOWN|SRI_O_DOWN)) continue;
        // 跳过已经断开主从连接的从节点
        if (slave->link->disconnected) continue;
        // 跳过回复PING命令过于久远的从节点
        if (mstime() - slave->link->last_avail_time > SENTINEL_PING_PERIOD*5) continue;
        // 跳过优先级为0的从节点
        if (slave->slave_priority == 0) continue;

        /* If the master is in SDOWN state we get INFO for slaves every second.
         * Otherwise we get it with the usual period so we need to account for
         * a larger delay. */
        // 如果主节点处于主观下线状态，Sentinel每秒发送INFO命令给从节点，否则以默认的频率发送。
        // 为了检查命令是否合法，因此计算一个延迟值
        if (master->flags & SRI_S_DOWN)
            info_validity_time = SENTINEL_PING_PERIOD*5;
        else
            info_validity_time = SENTINEL_INFO_PERIOD*3;
        // 如果从节点接受到INFO命令的回复已经过期，跳过该从节点
        if (mstime() - slave->info_refresh > info_validity_time) continue;
        // 跳过下线时间过长的从节点
        if (slave->master_link_down_time > max_master_down_time) continue;
        // 否则将选中的节点保存到数组中
        instance[instances++] = slave;
    }
    dictReleaseIterator(di);
    // 如果有选中的从节点
    if (instances) {
        // 将数组中的从节点排序
        qsort(instance,instances,sizeof(sentinelRedisInstance*),
            compareSlavesForPromotion);
        // 将排序最低的从节点返回
        selected = instance[0];
    }
    zfree(instance);
    return selected;
}

/* ---------------- Failover state machine implementation ------------------- */
// 准备执行故障转移
void sentinelFailoverWaitStart(sentinelRedisInstance *ri) {
    char *leader;
    int isleader;

    /* Check if we are the leader for the failover epoch. */
    // 获取给定纪元的领头Sentinel节点
    leader = sentinelGetLeader(ri, ri->failover_epoch);
    // 当前Sentinel是否就是领头节点
    isleader = leader && strcasecmp(leader,sentinel.myid) == 0;
    sdsfree(leader);

    /* If I'm not the leader, and it is not a forced failover via
     * SENTINEL FAILOVER, then I can't continue with the failover. */
    // 如果当前Sentinel节点不是领头节点，并且这次故障转移不是强制故障转移，那么就会返回
    if (!isleader && !(ri->flags & SRI_FORCE_FAILOVER)) {
        int election_timeout = SENTINEL_ELECTION_TIMEOUT;

        /* The election timeout is the MIN between SENTINEL_ELECTION_TIMEOUT
         * and the configured failover timeout. */
        // 当选的超时时间是SENTINEL_ELECTION_TIMEOUT和配置的故障转移时间failover_timeout两者中最小的
        if (election_timeout > ri->failover_timeout)
            election_timeout = ri->failover_timeout;
        /* Abort the failover if I'm not the leader after some time. */
        // 如果Sentinel当选领头节点时间超时
        if (mstime() - ri->failover_start_time > election_timeout) {
            // 发送取消故障转移的通知
            sentinelEvent(LL_WARNING,"-failover-abort-not-elected",ri,"%@");
            // 取消一个正在执行的故障转移
            sentinelAbortFailover(ri);
        }
        return; //返回
    }
    // 当前Sentinel节点是领头节点，则发送“赢得指定纪元的选举，可以进行故障迁移操作了”的事件通知
    sentinelEvent(LL_WARNING,"+elected-leader",ri,"%@");
    // 是否指定了模拟故障，当选之后的模拟故障
    if (sentinel.simfailure_flags & SENTINEL_SIMFAILURE_CRASH_AFTER_ELECTION)
        sentinelSimFailureCrash();  //退出当前Sentinel节点程序
    // 设置故障转移状态为：等待选择一个晋升的从节点
    ri->failover_state = SENTINEL_FAILOVER_STATE_SELECT_SLAVE;
    // 更新故障转移操作状态改变时间
    ri->failover_state_change_time = mstime();
    // 发送“Sentinel 正在寻找可以升级为主服务器的从服务器。”事件通知
    sentinelEvent(LL_WARNING,"+failover-state-select-slave",ri,"%@");
}

// 选择适合的从节点晋升为主节点
void sentinelFailoverSelectSlave(sentinelRedisInstance *ri) {
    // 从ri实例中选择一个合适的从节点
    sentinelRedisInstance *slave = sentinelSelectSlave(ri);

    /* We don't handle the timeout in this state as the function aborts
     * the failover or go forward in the next state. */
    // 没有找到合适的从节点，那么就终止故障转移操作
    if (slave == NULL) {
        // 发送事件通知
        sentinelEvent(LL_WARNING,"-failover-abort-no-good-slave",ri,"%@");
        sentinelAbortFailover(ri);
    } else {
        // 成功选择了晋升的从节点
        sentinelEvent(LL_WARNING,"+selected-slave",slave,"%@");
        // 设置晋升标识
        slave->flags |= SRI_PROMOTED;
        // 设置晋升从节点
        ri->promoted_slave = slave;
        // 设置故障转移状态为“发送slaveof no one”，是该从节点变为主节点
        ri->failover_state = SENTINEL_FAILOVER_STATE_SEND_SLAVEOF_NOONE;
        // 更新故障转移操作状态改变时间
        ri->failover_state_change_time = mstime();
        sentinelEvent(LL_NOTICE,"+failover-state-send-slaveof-noone",
            slave, "%@");
    }
}

// 发送slaveof no one命令给准备晋升的从节点
void sentinelFailoverSendSlaveOfNoOne(sentinelRedisInstance *ri) {
    int retval;

    /* We can't send the command to the promoted slave if it is now
     * disconnected. Retry again and again with this state until the timeout
     * is reached, then abort the failover. */
    // 如果要晋升的从节点处于断开连接的状态，那么不能发送命令。在当前状态，在规定的故障转移超时时间内可以重试。
    if (ri->promoted_slave->link->disconnected) {
        // 如果超出 配置的故障转移超时时间，那么中断本次故障转移后返回
        if (mstime() - ri->failover_state_change_time > ri->failover_timeout) {
            sentinelEvent(LL_WARNING,"-failover-abort-slave-timeout",ri,"%@");
            sentinelAbortFailover(ri);
        }
        return;
    }

    /* Send SLAVEOF NO ONE command to turn the slave into a master.
     * We actually register a generic callback for this command as we don't
     * really care about the reply. We check if it worked indirectly observing
     * if INFO returns a different role (master instead of slave). */
    // 发送 SLAVEOF NO ONE 命令将从节点晋升为主节点
    retval = sentinelSendSlaveOf(ri->promoted_slave,NULL,0);
    if (retval != C_OK) return;
    // 命令发送成功，发送事件通知
    sentinelEvent(LL_NOTICE, "+failover-state-wait-promotion",
        ri->promoted_slave,"%@");
    // 设置故障转移状态为等待从节点晋升为主节点
    ri->failover_state = SENTINEL_FAILOVER_STATE_WAIT_PROMOTION;
    // 更新故障转移操作状态改变时间
    ri->failover_state_change_time = mstime();
}

/* We actually wait for promotion indirectly checking with INFO when the
 * slave turns into a master. */
// 当奴隶变成主人时，我们实际上等待促销间接地检查INFO
// 当从节点晋升为主节点时，我们实际上是等待INFO命令回复来检查从节点是否晋升为主节点
void sentinelFailoverWaitPromotion(sentinelRedisInstance *ri) {
    /* Just handle the timeout. Switching to the next state is handled
     * by the function parsing the INFO command of the promoted slave. */
    // 所以，在这里只是处理故障转移超时的情况
    if (mstime() - ri->failover_state_change_time > ri->failover_timeout) {
        // 如果超出配置的故障转移超时时间，那么中断本次故障转移后返回
        sentinelEvent(LL_WARNING,"-failover-abort-slave-timeout",ri,"%@");
        sentinelAbortFailover(ri);
    }
}

// 判断故障转移是否结束
// 可以是超时被动结束，也可以因为从节点已经同步到新晋升的主节点结束
void sentinelFailoverDetectEnd(sentinelRedisInstance *master) {
    int not_reconfigured = 0, timeout = 0;
    dictIterator *di;
    dictEntry *de;
    // 自从上次更新故障转移状态的时间差
    mstime_t elapsed = mstime() - master->failover_state_change_time;

    /* We can't consider failover finished if the promoted slave is
     * not reachable. */
    // 如果被晋升的从节点不可达，直接返回
    if (master->promoted_slave == NULL ||
        master->promoted_slave->flags & SRI_S_DOWN) return;

    /* The failover terminates once all the reachable slaves are properly
     * configured. */
    // 遍历所有的从节点，找出还没有完成同步从节点
    di = dictGetIterator(master->slaves);
    while((de = dictNext(di)) != NULL) {
        sentinelRedisInstance *slave = dictGetVal(de);
        // 如果是被晋升为主节点的从节点或者是完成同步的从节点，则跳过
        if (slave->flags & (SRI_PROMOTED|SRI_RECONF_DONE)) continue;
        // 如果从节点处于客观下线，则跳过
        if (slave->flags & SRI_S_DOWN) continue;
        // 没有完成同步的节点数加1
        not_reconfigured++;
    }
    dictReleaseIterator(di);

    /* Force end of failover on timeout. */
    // 强制结束故障转移超时的节点
    if (elapsed > master->failover_timeout) {
        // 忽略未完成同步的从节点
        not_reconfigured = 0;
        // 设置超时标识
        timeout = 1;
        sentinelEvent(LL_WARNING,"+failover-end-for-timeout",master,"%@");
    }

    // 如果所有的从节点完成了同步，那么表示故障转移结束
    if (not_reconfigured == 0) {
        sentinelEvent(LL_WARNING,"+failover-end",master,"%@");
        // 监控晋升的主节点，更新配置
        master->failover_state = SENTINEL_FAILOVER_STATE_UPDATE_CONFIG;
        // 更新故障转移操作状态改变时间
        master->failover_state_change_time = mstime();
    }

    /* If I'm the leader it is a good idea to send a best effort SLAVEOF
     * command to all the slaves still not reconfigured to replicate with
     * the new master. */
    // 如果是因为超时导致故障转移结束
    if (timeout) {
        dictIterator *di;
        dictEntry *de;

        di = dictGetIterator(master->slaves);
        // 遍历所有的从节点
        while((de = dictNext(di)) != NULL) {
            sentinelRedisInstance *slave = dictGetVal(de);
            int retval;
            // 跳过完成同步和发送同步slaveof命令的从节点
            if (slave->flags & (SRI_RECONF_DONE|SRI_RECONF_SENT)) continue;
            // 跳过连接断开的从节点
            if (slave->link->disconnected) continue;
            // 给没有被发送同步命令的从节点发送同步新晋升主节点的slaveof IP port 命令
            retval = sentinelSendSlaveOf(slave,
                    master->promoted_slave->addr->ip,
                    master->promoted_slave->addr->port);
            // 如果发送成功，将这些从节点设置为已经发送slaveof命令的标识
            if (retval == C_OK) {
                sentinelEvent(LL_NOTICE,"+slave-reconf-sent-be",slave,"%@");
                slave->flags |= SRI_RECONF_SENT;
            }
        }
        dictReleaseIterator(di);
    }
}

/* Send SLAVE OF <new master address> to all the remaining slaves that
 * still don't appear to have the configuration updated. */
// 给所有没有同步新主节点的从节点发送 SLAVE OF <new master address> 命令
void sentinelFailoverReconfNextSlave(sentinelRedisInstance *master) {
    dictIterator *di;
    dictEntry *de;
    int in_progress = 0;

    di = dictGetIterator(master->slaves);
    // 遍历所有的从节点
    while((de = dictNext(di)) != NULL) {
        sentinelRedisInstance *slave = dictGetVal(de);
        // 计算处于已经发送同步命令或者已经正在同步的从节点
        if (slave->flags & (SRI_RECONF_SENT|SRI_RECONF_INPROG))
            in_progress++;
    }
    dictReleaseIterator(di);

    di = dictGetIterator(master->slaves);
    // 如果已经发送同步命令或者已经正在同步的从节点个数小于设置的同步个数限制，那么遍历所有的从节点
    while(in_progress < master->parallel_syncs &&
          (de = dictNext(di)) != NULL)
    {
        sentinelRedisInstance *slave = dictGetVal(de);
        int retval;

        /* Skip the promoted slave, and already configured slaves. */
        // 跳过被晋升的从节点和已经完成同步的从节点
        if (slave->flags & (SRI_PROMOTED|SRI_RECONF_DONE)) continue;

        /* If too much time elapsed without the slave moving forward to
         * the next state, consider it reconfigured even if it is not.
         * Sentinels will detect the slave as misconfigured and fix its
         * configuration later. */
        // 如果从节点设置了发送slaveof命令，但是故障转移更新到下一个状态超时
        if ((slave->flags & SRI_RECONF_SENT) &&
            (mstime() - slave->slave_reconf_sent_time) >
            SENTINEL_SLAVE_RECONF_TIMEOUT)
        {
            sentinelEvent(LL_NOTICE,"-slave-reconf-sent-timeout",slave,"%@");
            // 清除已发送slaveof命令的标识
            slave->flags &= ~SRI_RECONF_SENT;
            // 设置为完成同步的标识，随后重新发送SLAVEOF命令，进行同步
            slave->flags |= SRI_RECONF_DONE;
        }

        /* Nothing to do for instances that are disconnected or already
         * in RECONF_SENT state. */
        // 跳过已经发送了命令或者已经正在同步的从节点
        if (slave->flags & (SRI_RECONF_SENT|SRI_RECONF_INPROG)) continue;
        // 跳过连接断开的从节点
        if (slave->link->disconnected) continue;

        /* Send SLAVEOF <new master>. */
        // 发送 SLAVEOF <new master> 命令给从节点，包括刚才超时的从节点
        retval = sentinelSendSlaveOf(slave,
                master->promoted_slave->addr->ip,
                master->promoted_slave->addr->port);
        // 如果发送成功
        if (retval == C_OK) {
            // 设置已经发送了SLAVEOF命令标识
            slave->flags |= SRI_RECONF_SENT;
            // 设置发送slaveof命令的时间
            slave->slave_reconf_sent_time = mstime();
            sentinelEvent(LL_NOTICE,"+slave-reconf-sent",slave,"%@");
            in_progress++;
        }
    }
    dictReleaseIterator(di);

    /* Check if all the slaves are reconfigured and handle timeout. */
    // 判断故障转移是否结束
    sentinelFailoverDetectEnd(master);
}

/* This function is called when the slave is in
 * SENTINEL_FAILOVER_STATE_UPDATE_CONFIG state. In this state we need
 * to remove it from the master table and add the promoted slave instead. */
// 当从节点处于SENTINEL_FAILOVER_STATE_UPDATE_CONFIG状态，要将原来的主节点从主节点表中删除，并用晋升的主节点替代
void sentinelFailoverSwitchToPromotedSlave(sentinelRedisInstance *master) {
    // 获取要添加的从节点
    sentinelRedisInstance *ref = master->promoted_slave ?
                                 master->promoted_slave : master;
    // 配置变更，主服务器的 IP 和地址已经改变。 这是绝大多数外部用户都关心的信息。发送通知
    sentinelEvent(LL_WARNING,"+switch-master",master,"%s %s %d %s %d",
        master->name, master->addr->ip, master->addr->port,
        ref->addr->ip, ref->addr->port);
    // 用新晋升的主节点代替旧的主节点，包括所有从节点和旧的主节点从属当前新的主节点
    sentinelResetMasterAndChangeAddress(master,ref->addr->ip,ref->addr->port);
}

// 执行故障转移操作
void sentinelFailoverStateMachine(sentinelRedisInstance *ri) {
    // ri实例必须是主节点
    serverAssert(ri->flags & SRI_MASTER);
    // 如果主节点不处于进行故障转移操作的状态，则直接返回
    if (!(ri->flags & SRI_FAILOVER_IN_PROGRESS)) return;
    // 根据故障转移的状态，执行合适的操作
    switch(ri->failover_state) {
        // 等待故障转移的开始
        case SENTINEL_FAILOVER_STATE_WAIT_START:
            sentinelFailoverWaitStart(ri);
            break;
        // 选择一个要晋升的从节点
        case SENTINEL_FAILOVER_STATE_SELECT_SLAVE:
            sentinelFailoverSelectSlave(ri);
            break;
        // 发送slaveof no one命令，使从节点变为主节点
        case SENTINEL_FAILOVER_STATE_SEND_SLAVEOF_NOONE:
            sentinelFailoverSendSlaveOfNoOne(ri);
            break;
        // 等待被选择的从节点晋升为主节点，如果超时则重新选择晋升的从节点
        case SENTINEL_FAILOVER_STATE_WAIT_PROMOTION:
            sentinelFailoverWaitPromotion(ri);
            break;
        // 给所有的从节点发送slaveof命令，同步新的主节点
        case SENTINEL_FAILOVER_STATE_RECONF_SLAVES:
            sentinelFailoverReconfNextSlave(ri);
            break;
    }
}

/* Abort a failover in progress:
 *
 * This function can only be called before the promoted slave acknowledged
 * the slave -> master switch. Otherwise the failover can't be aborted and
 * will reach its end (possibly by timeout). */
// 中断正在进行的故障转移
// 该函数只能在被晋升的从节点晋升为主节点之前被调用。否则故障转移不能被中断并且会一直执行结束
void sentinelAbortFailover(sentinelRedisInstance *ri) {
    // ri实例必须正在进行的故障转移而且必须在等待从节点晋升为主节点之前的状态
    serverAssert(ri->flags & SRI_FAILOVER_IN_PROGRESS);
    serverAssert(ri->failover_state <= SENTINEL_FAILOVER_STATE_WAIT_PROMOTION);
    // 取消ri实例正在进行的故障转移的状态，和取消强制故障转移操作
    ri->flags &= ~(SRI_FAILOVER_IN_PROGRESS|SRI_FORCE_FAILOVER);
    // 设置故障转移操作状态为空
    ri->failover_state = SENTINEL_FAILOVER_STATE_NONE;
    // 更新故障转移操作状态改变的时间
    ri->failover_state_change_time = mstime();
    // 如果已经有选好的从节点，那么撤销它的晋升表示，并设置为空
    if (ri->promoted_slave) {
        ri->promoted_slave->flags &= ~SRI_PROMOTED;
        ri->promoted_slave = NULL;
    }
}

/* ======================== SENTINEL timer handler ==========================
 * This is the "main" our Sentinel, being sentinel completely non blocking
 * in design. The function is called every second.
 * -------------------------------------------------------------------------- */
// Sentinel定时任务处理，这是Sentinel的main函数，Sentinel是完全的非阻塞。每秒调用一次该函数

/* Perform scheduled operations for the specified Redis instance. */
// 对指定的ri实例执行周期性操作
void sentinelHandleRedisInstance(sentinelRedisInstance *ri) {
    /* ========== MONITORING HALF ============ */
    /* ========== 一半监控操作 ============ */

    /* Every kind of instance */
    /* 对所有的类型的实例进行操作 */

    // 为Sentinel和ri实例创建一个网络连接，包括cc和pc
    sentinelReconnectInstance(ri);
    // 定期发送PING、PONG、PUBLISH命令到ri实例中
    sentinelSendPeriodicCommands(ri);

    /* ============== ACTING HALF ============= */
    /* ============== 一半故障检测 ============= */

    /* We don't proceed with the acting half if we are in TILT mode.
     * TILT happens when we find something odd with the time, like a
     * sudden change in the clock. */
    // 如果Sentinel处于TILT模式，则不进行故障检测
    if (sentinel.tilt) {
        // 如果TILT模式的时间没到，则不执行后面的动作，直接返回
        if (mstime()-sentinel.tilt_start_time < SENTINEL_TILT_PERIOD) return;
        // 如果TILT模式时间已经到了，取消TILT模式的标识
        sentinel.tilt = 0;
        sentinelEvent(LL_WARNING,"-tilt",NULL,"#tilt mode exited");
    }

    /* Every kind of instance */
    // 对于各种实例进行是否下线的检测，是否处于主观下线状态
    sentinelCheckSubjectivelyDown(ri);

    /* Masters and slaves */
    // 目前对主节点和从节点的实例什么都不做
    if (ri->flags & (SRI_MASTER|SRI_SLAVE)) {
        /* Nothing so far. */
    }

    /* Only masters */
    // 只对主节点进行操作
    if (ri->flags & SRI_MASTER) {
        // 检查从节点是否客观下线
        sentinelCheckObjectivelyDown(ri);
        // 如果处于客观下线状态，则进行故障转移的状态设置
        if (sentinelStartFailoverIfNeeded(ri))
            // 强制向其他Sentinel节点发送SENTINEL IS-MASTER-DOWN-BY-ADDR给所有的Sentinel获取回复
            // 尝试获得足够的票数，标记主节点为客观下线状态，触发故障转移
            sentinelAskMasterStateToOtherSentinels(ri,SENTINEL_ASK_FORCED);
        // 执行故障转移操作
        sentinelFailoverStateMachine(ri);
        // 主节点ri没有处于客观下线的状态，那么也要尝试发送SENTINEL IS-MASTER-DOWN-BY-ADDR给所有的Sentinel获取回复
        // 因为ri主节点如果有回复延迟等等状况，可以通过该命令，更新一些主节点状态
        sentinelAskMasterStateToOtherSentinels(ri,SENTINEL_NO_FLAGS);
    }
}

/* Perform scheduled operations for all the instances in the dictionary.
 * Recursively call the function against dictionaries of slaves. */
// 对在instances字典中的所有实例执行周期性操作。并且递归的调用
void sentinelHandleDictOfRedisInstances(dict *instances) {
    dictIterator *di;
    dictEntry *de;
    sentinelRedisInstance *switch_to_promoted = NULL;

    /* There are a number of things we need to perform against every master. */
    di = dictGetIterator(instances);
    // 遍历字典中所有的实例
    while((de = dictNext(di)) != NULL) {
        sentinelRedisInstance *ri = dictGetVal(de);
        // 对指定的ri实例执行周期性操作
        sentinelHandleRedisInstance(ri);
        // 如果ri实例是主节点
        if (ri->flags & SRI_MASTER) {
            // 递归的对主节点从属的从节点执行周期性操作
            sentinelHandleDictOfRedisInstances(ri->slaves);
            // 递归的对监控主节点的Sentinel节点执行周期性操作
            sentinelHandleDictOfRedisInstances(ri->sentinels);
            // 如果ri实例处于完成故障转移操作的状态，所有从节点已经完成对新主节点的同步
            if (ri->failover_state == SENTINEL_FAILOVER_STATE_UPDATE_CONFIG) {
                // 设置主从转换的标识
                switch_to_promoted = ri;
            }
        }
    }
    // 如果主从节点发生了转换
    if (switch_to_promoted)
        // 将原来的主节点从主节点表中删除，并用晋升的主节点替代
        // 意味着已经用新晋升的主节点代替旧的主节点，包括所有从节点和旧的主节点从属当前新的主节点
        sentinelFailoverSwitchToPromotedSlave(switch_to_promoted);
    dictReleaseIterator(di);
}

/* This function checks if we need to enter the TITL mode.
 *
 * The TILT mode is entered if we detect that between two invocations of the
 * timer interrupt, a negative amount of time, or too much time has passed.
 * Note that we expect that more or less just 100 milliseconds will pass
 * if everything is fine. However we'll see a negative number or a
 * difference bigger than SENTINEL_TILT_TRIGGER milliseconds if one of the
 * following conditions happen:
 *
 * 1) The Sentiel process for some time is blocked, for every kind of
 * random reason: the load is huge, the computer was frozen for some time
 * in I/O or alike, the process was stopped by a signal. Everything.
 * 2) The system clock was altered significantly.
 *
 * Under both this conditions we'll see everything as timed out and failing
 * without good reasons. Instead we enter the TILT mode and wait
 * for SENTINEL_TILT_PERIOD to elapse before starting to act again.
 *
 * During TILT time we still collect information, we just do not act. */
// 该函数检查Sentinel是否需要进入TITL模式
// 如果发现Sentinel处于两次执行时间处理程序之间（两次执行Sentinel之间的时间差为负值，或者过大），则需要进入TITL模式。
// 一般来说，两次执行Sentinel之间的时间差会在100ms左右，但是出现以下情况，会出现异常：
/*
    1. Sentinel进程在某时被阻塞，有很多种原因，负载过大，IO任务密集，进程被信号停止等等
    2. 系统时钟发送明显变化
*/
// 以上情况都可以将Sentinel视为掉线，为了避免出现以上情况，让Sentinel进入TITL模式，等待一个SENTINEL_TILT_PERIOD过去
// 在SENTINEL_TILT_PERIOD周期内，Sentinel任然需要进行收集信息，但是只是不响应
void sentinelCheckTiltCondition(void) {
    mstime_t now = mstime();
    // 最后一次执行Sentinel时间处理程序的时间过去了过久
    mstime_t delta = now - sentinel.previous_time;
    // 差为负数，或者大于2秒
    if (delta < 0 || delta > SENTINEL_TILT_TRIGGER) {
        // 设置Sentinel进入TILT状态
        sentinel.tilt = 1;
        // 设置进入TILT状态的开始时间
        sentinel.tilt_start_time = mstime();
        sentinelEvent(LL_WARNING,"+tilt",NULL,"#tilt mode entered");
    }
    // 设置最近一次执行Sentinel时间处理程序的时间
    sentinel.previous_time = mstime();
}

// Sentinel模式的主函数，由serverCron()函数调用
void sentinelTimer(void) {
    // 先检查Sentinel是否需要进入TITL模式，更新最近一次执行Sentinel模式的周期函数的时间
    sentinelCheckTiltCondition();
    // 对Sentinel监控的所有主节点进行递归式的执行周期性操作
    sentinelHandleDictOfRedisInstances(sentinel.masters);
    // 运行在队列中等待的脚本
    sentinelRunPendingScripts();
    // 清理已成功执行的脚本，重试执行错误的脚本
    sentinelCollectTerminatedScripts();
    // 杀死执行超时的脚本，等到下个周期在sentinelCollectTerminatedScripts()函数中重试执行
    sentinelKillTimedoutScripts();

    /* We continuously change the frequency of the Redis "timer interrupt"
     * in order to desynchronize every Sentinel from every other.
     * This non-determinism avoids that Sentinels started at the same time
     * exactly continue to stay synchronized asking to be voted at the
     * same time again and again (resulting in nobody likely winning the
     * election because of split brain voting). */
    // 我们不断改变Redis定期任务的执行频率，以便使每个Sentinel节点都不同步，这种不确定性可以避免Sentinel在同一时间开始完全继续保持同步，当被要求进行投票时，一次又一次在同一时间进行投票，因为脑裂导致有可能没有胜选者
    server.hz = CONFIG_DEFAULT_HZ + rand() % CONFIG_DEFAULT_HZ;
}

