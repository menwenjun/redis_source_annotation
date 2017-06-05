#ifndef __CLUSTER_H
#define __CLUSTER_H

/*-----------------------------------------------------------------------------
 * Redis cluster data structures, defines, exported API.
 *----------------------------------------------------------------------------*/

#define CLUSTER_SLOTS 16384
#define CLUSTER_OK 0          /* Everything looks ok */
#define CLUSTER_FAIL 1        /* The cluster can't work */
#define CLUSTER_NAMELEN 40    /* sha1 hex length */
#define CLUSTER_PORT_INCR 10000 /* Cluster port = baseport + PORT_INCR */

/* The following defines are amount of time, sometimes expressed as
 * multiplicators of the node timeout value (when ending with MULT). */
#define CLUSTER_DEFAULT_NODE_TIMEOUT 15000
#define CLUSTER_DEFAULT_SLAVE_VALIDITY 10 /* Slave max data age factor. */
#define CLUSTER_DEFAULT_REQUIRE_FULL_COVERAGE 1
// 故障报告的有效期
#define CLUSTER_FAIL_REPORT_VALIDITY_MULT 2 /* Fail report validity. */
#define CLUSTER_FAIL_UNDO_TIME_MULT 2 /* Undo fail if master is back. */
#define CLUSTER_FAIL_UNDO_TIME_ADD 10 /* Some additional time. */
#define CLUSTER_FAILOVER_DELAY 5 /* Seconds */
#define CLUSTER_DEFAULT_MIGRATION_BARRIER 1
#define CLUSTER_MF_TIMEOUT 5000 /* Milliseconds to do a manual failover. */
#define CLUSTER_MF_PAUSE_MULT 2 /* Master pause manual failover mult. */
#define CLUSTER_SLAVE_MIGRATION_DELAY 5000 /* Delay for slave migration. */

/* Redirection errors returned by getNodeByQuery(). */
#define CLUSTER_REDIR_NONE 0          /* Node can serve the request. */
#define CLUSTER_REDIR_CROSS_SLOT 1    /* -CROSSSLOT request. */
#define CLUSTER_REDIR_UNSTABLE 2      /* -TRYAGAIN redirection required */
#define CLUSTER_REDIR_ASK 3           /* -ASK redirection required. */
#define CLUSTER_REDIR_MOVED 4         /* -MOVED redirection required. */
#define CLUSTER_REDIR_DOWN_STATE 5    /* -CLUSTERDOWN, global state. */
#define CLUSTER_REDIR_DOWN_UNBOUND 6  /* -CLUSTERDOWN, unbound slot. */

struct clusterNode;

/* clusterLink encapsulates everything needed to talk with a remote node. */
typedef struct clusterLink {
    // 连接创建的时间
    mstime_t ctime;             /* Link creation time */
    // TCP连接的文件描述符
    int fd;                     /* TCP socket file descriptor */
    // 输出（发送）缓冲区
    sds sndbuf;                 /* Packet send buffer */
    // 输入（接收）缓冲区
    sds rcvbuf;                 /* Packet reception buffer */
    // 关联该连接的节点
    struct clusterNode *node;   /* Node related to this link if any, or NULL */
} clusterLink;

/* Cluster node flags and macros. */
// 主节点
#define CLUSTER_NODE_MASTER 1     /* The node is a master */
// 从节点
#define CLUSTER_NODE_SLAVE 2      /* The node is a slave */
// 需要确认是否处于故障状态
#define CLUSTER_NODE_PFAIL 4      /* Failure? Need acknowledge */
// 已经确认处于故障状态
#define CLUSTER_NODE_FAIL 8       /* The node is believed to be malfunctioning */
// 节点是自己
#define CLUSTER_NODE_MYSELF 16    /* This node is myself */
// 需要进行握手，发送PING
#define CLUSTER_NODE_HANDSHAKE 32 /* We have still to exchange the first ping */
// 节点没有地址
#define CLUSTER_NODE_NOADDR   64  /* We don't know the address of this node */
// 需要发送MEET给节点
#define CLUSTER_NODE_MEET 128     /* Send a MEET message to this node */
// 主节点可以为复制操作将槽中的数据导出
#define CLUSTER_NODE_MIGRATE_TO 256 /* Master elegible for replica migration. */
#define CLUSTER_NODE_NULL_NAME "\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000\000"

#define nodeIsMaster(n) ((n)->flags & CLUSTER_NODE_MASTER)
#define nodeIsSlave(n) ((n)->flags & CLUSTER_NODE_SLAVE)
#define nodeInHandshake(n) ((n)->flags & CLUSTER_NODE_HANDSHAKE)
#define nodeHasAddr(n) (!((n)->flags & CLUSTER_NODE_NOADDR))
#define nodeWithoutAddr(n) ((n)->flags & CLUSTER_NODE_NOADDR)
#define nodeTimedOut(n) ((n)->flags & CLUSTER_NODE_PFAIL)
#define nodeFailed(n) ((n)->flags & CLUSTER_NODE_FAIL)

/* Reasons why a slave is not able to failover. */
#define CLUSTER_CANT_FAILOVER_NONE 0
#define CLUSTER_CANT_FAILOVER_DATA_AGE 1
#define CLUSTER_CANT_FAILOVER_WAITING_DELAY 2
#define CLUSTER_CANT_FAILOVER_EXPIRED 3
#define CLUSTER_CANT_FAILOVER_WAITING_VOTES 4
#define CLUSTER_CANT_FAILOVER_RELOG_PERIOD (60*5) /* seconds. */

/* This structure represent elements of node->fail_reports. */
typedef struct clusterNodeFailReport {
    // 报告的故障的节点
    struct clusterNode *node;  /* Node reporting the failure condition. */
    // 报告的时间
    mstime_t time;             /* Time of the last report from this node. */
} clusterNodeFailReport;

typedef struct clusterNode {
    // 节点创建的时间
    mstime_t ctime; /* Node object creation time. */
    // 名字
    char name[CLUSTER_NAMELEN]; /* Node name, hex string, sha1-size */
    // 状态标识
    int flags;      /* CLUSTER_NODE_... */
    uint64_t configEpoch; /* Last configEpoch observed for this node */
    // 节点的槽位图
    unsigned char slots[CLUSTER_SLOTS/8]; /* slots handled by this node */
    // 当前节点复制槽的数量
    int numslots;   /* Number of slots handled by this node */
    // 从节点的数量
    int numslaves;  /* Number of slave nodes, if this is a master */
    // 从节点指针数组
    struct clusterNode **slaves; /* pointers to slave nodes */
    // 指向主节点，即使是从节点也可以为NULL
    struct clusterNode *slaveof; /* pointer to the master node. Note that it
                                    may be NULL even if the node is a slave
                                    if we don't have the master node in our
                                    tables. */
    // 最近一次发送PING的时间
    mstime_t ping_sent;      /* Unix time we sent latest ping */
    // 接收到PONG的时间
    mstime_t pong_received;  /* Unix time we received the pong */
    // 被设置为FAIL的下线时间
    mstime_t fail_time;      /* Unix time when FAIL flag was set */
    // 最近一次为从节点投票的时间
    mstime_t voted_time;     /* Last time we voted for a slave of this master */
    // 更新复制偏移量的时间
    mstime_t repl_offset_time;  /* Unix time we received offset for this node */
    // 孤立的主节点迁移的时间
    mstime_t orphaned_time;     /* Starting time of orphaned master condition */
    // 该节点已知的复制偏移量
    long long repl_offset;      /* Last known repl offset for this node. */
    // ip地址
    char ip[NET_IP_STR_LEN];  /* Latest known IP address of this node */
    // 节点端口号
    int port;                   /* Latest known port of this node */
    // 与该节点关联的连接对象
    clusterLink *link;          /* TCP/IP link with this node */
    // 保存下线报告的链表
    list *fail_reports;         /* List of nodes signaling this as failing */
} clusterNode;

typedef struct clusterState {
    clusterNode *myself;  /* This node */
    // 当前纪元
    uint64_t currentEpoch;
    // 集群的状态
    int state;            /* CLUSTER_OK, CLUSTER_FAIL, ... */
    // 集群中至少负责一个槽的主节点个数
    int size;             /* Num of master nodes with at least one slot */
    // 保存集群节点的字典，键是节点名字，值是clusterNode结构的指针
    dict *nodes;          /* Hash table of name -> clusterNode structures */
    // 防止重复添加节点的黑名单
    dict *nodes_black_list; /* Nodes we don't re-add for a few seconds. */
    // 导出槽数据到目标节点，该数组记录这些节点
    clusterNode *migrating_slots_to[CLUSTER_SLOTS];
    // 导入槽数据到目标节点，该数组记录这些节点
    clusterNode *importing_slots_from[CLUSTER_SLOTS];
    // 槽和负责槽节点的映射
    clusterNode *slots[CLUSTER_SLOTS];
    // 槽映射到键的有序集合
    zskiplist *slots_to_keys;
    /* The following fields are used to take the slave state on elections. */
    // 之前或下一次选举的时间
    mstime_t failover_auth_time; /* Time of previous or next election. */
    // 节点获得支持的票数
    int failover_auth_count;    /* Number of votes received so far. */
    // 如果为真，表示本节点已经向其他节点发送了投票请求
    int failover_auth_sent;     /* True if we already asked for votes. */
    // 该从节点在当前请求中的排名
    int failover_auth_rank;     /* This slave rank for current auth request. */
    // 当前选举的纪元
    uint64_t failover_auth_epoch; /* Epoch of the current election. */
    // 从节点不能执行故障转移的原因
    int cant_failover_reason;   /* Why a slave is currently not able to
                                   failover. See the CANT_FAILOVER_* macros. */
    /* Manual failover state in common. */
    // 如果为0，表示没有正在进行手动的故障转移。否则表示手动故障转移的时间限制
    mstime_t mf_end;            /* Manual failover time limit (ms unixtime).
                                   It is zero if there is no MF in progress. */
    /* Manual failover state of master. */
    // 执行手动孤战转移的从节点
    clusterNode *mf_slave;      /* Slave performing the manual failover. */
    /* Manual failover state of slave. */
    // 从节点记录手动故障转移时的主节点偏移量
    long long mf_master_offset; /* Master offset the slave needs to start MF
                                   or zero if stil not received. */
    // 非零值表示手动故障转移能开始
    int mf_can_start;           /* If non-zero signal that the manual failover
                                   can start requesting masters vote. */
    /* The followign fields are used by masters to take state on elections. */
    // 集群最近一次投票的纪元
    uint64_t lastVoteEpoch;     /* Epoch of the last vote granted. */
    // 调用clusterBeforeSleep()所做的一些事
    int todo_before_sleep; /* Things to do in clusterBeforeSleep(). */
    // 发送的字节数
    long long stats_bus_messages_sent;  /* Num of msg sent via cluster bus. */
    // 通过Cluster接收到的消息数量
    long long stats_bus_messages_received; /* Num of msg rcvd via cluster bus.*/
} clusterState;

/* clusterState todo_before_sleep flags. */
// 节点在结束一个事件循环后要做的工作
// 处理故障转移
#define CLUSTER_TODO_HANDLE_FAILOVER (1<<0)
// 更新状态
#define CLUSTER_TODO_UPDATE_STATE (1<<1)
// 保存配置
#define CLUSTER_TODO_SAVE_CONFIG (1<<2)
// 同步配置
#define CLUSTER_TODO_FSYNC_CONFIG (1<<3)

/* Redis cluster messages header */

/* Note that the PING, PONG and MEET messages are actually the same exact
 * kind of packet. PONG is the reply to ping, in the exact format as a PING,
 * while MEET is a special PING that forces the receiver to add the sender
 * as a node (if it is not already in the list). */
#define CLUSTERMSG_TYPE_PING 0          /* Ping */
#define CLUSTERMSG_TYPE_PONG 1          /* Pong (reply to Ping) */
#define CLUSTERMSG_TYPE_MEET 2          /* Meet "let's join" message */
#define CLUSTERMSG_TYPE_FAIL 3          /* Mark node xxx as failing */
#define CLUSTERMSG_TYPE_PUBLISH 4       /* Pub/Sub Publish propagation */
// 请求获得故障迁移授权的消息
#define CLUSTERMSG_TYPE_FAILOVER_AUTH_REQUEST 5 /* May I failover? */
// 故障迁移投票的信息
#define CLUSTERMSG_TYPE_FAILOVER_AUTH_ACK 6     /* Yes, you have my vote */
#define CLUSTERMSG_TYPE_UPDATE 7        /* Another node slots configuration */
// 暂停手动故障转移
#define CLUSTERMSG_TYPE_MFSTART 8       /* Pause clients for manual failover */

/* Initially we don't know our "name", but we'll find it once we connect
 * to the first node, using the getsockname() function. Then we'll use this
 * address for all the next messages. */
typedef struct {
    // 节点名字
    char nodename[CLUSTER_NAMELEN];
    // 最近一次发送PING的时间戳
    uint32_t ping_sent;
    // 最近一次接收PONG的时间戳
    uint32_t pong_received;
    // 节点的IP地址
    char ip[NET_IP_STR_LEN];  /* IP address last time it was seen */
    // 节点的端口号
    uint16_t port;              /* port last time it was seen */
    // 节点的标识
    uint16_t flags;             /* node->flags copy */
    // 未使用
    uint16_t notused1;          /* Some room for future improvements. */
    uint32_t notused2;
} clusterMsgDataGossip;

typedef struct {
    // 故障节点的名字
    char nodename[CLUSTER_NAMELEN];
} clusterMsgDataFail;

typedef struct {
    // 频道名长度
    uint32_t channel_len;
    // 消息长度
    uint32_t message_len;
    /* We can't reclare bulk_data as bulk_data[] since this structure is
     * nested. The 8 bytes are removed from the count during the message
     * length computation. */
    // 消息内容
    unsigned char bulk_data[8];
} clusterMsgDataPublish;

typedef struct {
    // 节点的配置纪元
    uint64_t configEpoch; /* Config epoch of the specified instance. */
    // 负责槽的节点名字
    char nodename[CLUSTER_NAMELEN]; /* Name of the slots owner. */
    // 槽位图
    unsigned char slots[CLUSTER_SLOTS/8]; /* Slots bitmap. */
} clusterMsgDataUpdate;

union clusterMsgData {
    /* PING, MEET and PONG */
    struct {
        /* Array of N clusterMsgDataGossip structures */
        clusterMsgDataGossip gossip[1];
    } ping;

    /* FAIL */
    struct {
        clusterMsgDataFail about;
    } fail;

    /* PUBLISH */
    struct {
        clusterMsgDataPublish msg;
    } publish;

    /* UPDATE */
    struct {
        clusterMsgDataUpdate nodecfg;
    } update;
};

#define CLUSTER_PROTO_VER 0 /* Cluster bus protocol version. */

typedef struct {
    // "RCmb"的签名
    char sig[4];        /* Siganture "RCmb" (Redis Cluster message bus). */
    // 消息的总长
    uint32_t totlen;    /* Total length of this message */
    // 协议版本，当前为0
    uint16_t ver;       /* Protocol version, currently set to 0. */
    // 未使用的2字节
    uint16_t notused0;  /* 2 bytes not used. */
    // 消息类型
    uint16_t type;      /* Message type */
    // 只在发送PING、PONG和MEET消息时使用
    // 消息正文包含的节点信息数量
    uint16_t count;     /* Only used for some kind of messages. */
    // 发送消息的当前纪元
    uint64_t currentEpoch;  /* The epoch accordingly to the sending node. */
    // 如果发送消息是主节点，则记录发送消息的节点配置纪元
    // 如果发送消息是从节点，则记录当前从节点的主节点的配置纪元
    uint64_t configEpoch;   /* The config epoch if it's a master, or the last
                               epoch advertised by its master if it is a
                               slave. */
    // 复制偏移量
    uint64_t offset;    /* Master replication offset if node is a master or
                           processed replication offset if node is a slave. */
    // 发送消息的节点的name
    char sender[CLUSTER_NAMELEN]; /* Name of the sender node */
    // 发送消息节点的槽位图信息
    unsigned char myslots[CLUSTER_SLOTS/8];
    // 如果发送消息的是主节点，那么保存的是正在复制的主节点的名字
    // 如果发送消息的是从节点，保存的是空
    char slaveof[CLUSTER_NAMELEN];
    // 32字节未使用
    char notused1[32];  /* 32 bytes reserved for future usage. */
    // 发送消息节点的端口
    uint16_t port;      /* Sender TCP base port */
    // 发送消息节点的标识
    uint16_t flags;     /* Sender node flags */
    // 发送消息节点所处集群的状态
    unsigned char state; /* Cluster state from the POV of the sender */
    // 消息的标识
    unsigned char mflags[3]; /* Message flags: CLUSTERMSG_FLAG[012]_... */
    // 消息的数据
    union clusterMsgData data;
} clusterMsg;

#define CLUSTERMSG_MIN_LEN (sizeof(clusterMsg)-sizeof(union clusterMsgData))

/* Message flags better specify the packet content or are used to
 * provide some information about the node state. */
// 主节点暂停手动故障转移
#define CLUSTERMSG_FLAG0_PAUSED (1<<0) /* Master paused for manual failover. */
// 即使主节点在线，也要认证故障转移
#define CLUSTERMSG_FLAG0_FORCEACK (1<<1) /* Give ACK to AUTH_REQUEST even if
                                            master is up. */

/* ---------------------- API exported outside cluster.c -------------------- */
clusterNode *getNodeByQuery(client *c, struct redisCommand *cmd, robj **argv, int argc, int *hashslot, int *ask);
int clusterRedirectBlockedClientIfNeeded(client *c);
void clusterRedirectClient(client *c, clusterNode *n, int hashslot, int error_code);

#endif /* __CLUSTER_H */
