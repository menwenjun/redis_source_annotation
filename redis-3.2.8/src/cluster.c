/* Redis Cluster implementation.
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
#include "cluster.h"
#include "endianconv.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <math.h>

/* A global reference to myself is handy to make code more clear.
 * Myself always points to server.cluster->myself, that is, the clusterNode
 * that represents this node. */
// 一个全局的引用，指向cluster->myself
clusterNode *myself = NULL;

clusterNode *createClusterNode(char *nodename, int flags);
int clusterAddNode(clusterNode *node);
void clusterAcceptHandler(aeEventLoop *el, int fd, void *privdata, int mask);
void clusterReadHandler(aeEventLoop *el, int fd, void *privdata, int mask);
void clusterSendPing(clusterLink *link, int type);
void clusterSendFail(char *nodename);
void clusterSendFailoverAuthIfNeeded(clusterNode *node, clusterMsg *request);
void clusterUpdateState(void);
int clusterNodeGetSlotBit(clusterNode *n, int slot);
sds clusterGenNodesDescription(int filter);
clusterNode *clusterLookupNode(char *name);
int clusterNodeAddSlave(clusterNode *master, clusterNode *slave);
int clusterAddSlot(clusterNode *n, int slot);
int clusterDelSlot(int slot);
int clusterDelNodeSlots(clusterNode *node);
int clusterNodeSetSlotBit(clusterNode *n, int slot);
void clusterSetMaster(clusterNode *n);
void clusterHandleSlaveFailover(void);
void clusterHandleSlaveMigration(int max_slaves);
int bitmapTestBit(unsigned char *bitmap, int pos);
void clusterDoBeforeSleep(int flags);
void clusterSendUpdate(clusterLink *link, clusterNode *node);
void resetManualFailover(void);
void clusterCloseAllSlots(void);
void clusterSetNodeAsMaster(clusterNode *n);
void clusterDelNode(clusterNode *delnode);
sds representClusterNodeFlags(sds ci, uint16_t flags);
uint64_t clusterGetMaxEpoch(void);
int clusterBumpConfigEpochWithoutConsensus(void);

/* -----------------------------------------------------------------------------
 * Initialization
 * -------------------------------------------------------------------------- */

/* Load the cluster config from 'filename'.
 *
 * If the file does not exist or is zero-length (this may happen because
 * when we lock the nodes.conf file, we create a zero-length one for the
 * sake of locking if it does not already exist), C_ERR is returned.
 * If the configuration was loaded from the file, C_OK is returned. */
// 从filename载入集群的配置
// 如果文件不存在或者文件大小为0（文件被锁住）返回C_ERR，如果成功载入配置文件，则范返回C_OK
int clusterLoadConfig(char *filename) {
    FILE *fp = fopen(filename,"r");
    struct stat sb;
    char *line;
    int maxline, j;
    // 判断文件是否存在
    if (fp == NULL) {
        if (errno == ENOENT) {
            return C_ERR;
        } else {
            serverLog(LL_WARNING,
                "Loading the cluster node config from %s: %s",
                filename, strerror(errno));
            exit(1);
        }
    }

    /* Check if the file is zero-length: if so return C_ERR to signal
     * we have to write the config. */
    // 判断文件是否为空
    if (fstat(fileno(fp),&sb) != -1 && sb.st_size == 0) {
        fclose(fp);
        return C_ERR;
    }

    /* Parse the file. Note that single lines of the cluster config file can
     * be really long as they include all the hash slots of the node.
     * This means in the worst possible case, half of the Redis slots will be
     * present in a single line, possibly in importing or migrating state, so
     * together with the node ID of the sender/receiver.
     *
     * To simplify we allocate 1024+CLUSTER_SLOTS*128 bytes per line. */
    // 解析文件。集群配置文件可能会非常长，因为他会在每一行记录该节点的哈希槽，在最坏情况下，一半的哈希槽将会被记录在一行中，并且附带有导入导出状态，所以为每行分配 1024+CLUSTER_SLOTS*128 字节空间
    maxline = 1024+CLUSTER_SLOTS*128;
    line = zmalloc(maxline);
    // 每次从文件读一行
    while(fgets(line,maxline,fp) != NULL) {
        int argc;
        sds *argv;
        clusterNode *n, *master;
        char *p, *s;

        /* Skip blank lines, they can be created either by users manually
         * editing nodes.conf or by the config writing process if stopped
         * before the truncate() call. */
        // 跳过空行
        if (line[0] == '\n' || line[0] == '\0') continue;

        /* Split the line into arguments for processing. */
        // 将读入的一行，分隔开
        argv = sdssplitargs(line,&argc);
        if (argv == NULL) goto fmterr;

        /* Handle the special "vars" line. Don't pretend it is the last
         * line even if it actually is when generated by Redis. */
        // 处理 vars 变量，例如：vars currentEpoch 5 lastVoteEpoch 0
        if (strcasecmp(argv[0],"vars") == 0) {
            for (j = 1; j < argc; j += 2) {
                // currentEpoch选项
                if (strcasecmp(argv[j],"currentEpoch") == 0) {
                    server.cluster->currentEpoch =
                            strtoull(argv[j+1],NULL,10);
                // lastVoteEpoch选项
                } else if (strcasecmp(argv[j],"lastVoteEpoch") == 0) {
                    server.cluster->lastVoteEpoch =
                            strtoull(argv[j+1],NULL,10);
                } else {
                    serverLog(LL_WARNING,
                        "Skipping unknown cluster config variable '%s'",
                        argv[j]);
                }
            }
            sdsfreesplitres(argv,argc);
            continue;
        }

        /* Regular config lines have at least eight fields */
        // 主节点：66478bda726ae6ba4e8fb55034d8e5e5804223ff 127.0.0.1:6381 master - 0 1496130037660 2 connected 10923-16383
        // 从节点：6fb7dfdb6188a9fe53c48ea32d541724f36434e9 127.0.0.1:6383 slave 8f285670923d4f1c599ecc93367c95a30fb8bf34 0 1496130040668 4 connected
        // 29978c0169ecc0a9054de7f4142155c1ab70258b 127.0.0.1:6379 myself,master - 0 0 1 connected 0-5461

        // 参数最少8个
        if (argc < 8) goto fmterr;

        /* Create this node if it does not exist */
        // 根据runid查找对应节点
        n = clusterLookupNode(argv[0]);
        // 如果不存在，则根据runid创建节点
        if (!n) {
            n = createClusterNode(argv[0],0);
            // 加入到集群中
            clusterAddNode(n);
        }
        /* Address and port */
        // 解析 ip:port
        if ((p = strrchr(argv[1],':')) == NULL) goto fmterr;
        *p = '\0';
        memcpy(n->ip,argv[1],strlen(argv[1])+1);
        n->port = atoi(p+1);

        /* Parse flags */
        p = s = argv[2];
        while(p) {
            p = strchr(s,',');
            if (p) *p = '\0';
            // 如果是myself，则设置指向自己的指针
            if (!strcasecmp(s,"myself")) {
                serverAssert(server.cluster->myself == NULL);
                myself = server.cluster->myself = n;
                n->flags |= CLUSTER_NODE_MYSELF;
            // 如果是主节点master
            } else if (!strcasecmp(s,"master")) {
                n->flags |= CLUSTER_NODE_MASTER;
            // 如果是从节点slave
            } else if (!strcasecmp(s,"slave")) {
                n->flags |= CLUSTER_NODE_SLAVE;
            // 可能是一个下线的节点
            } else if (!strcasecmp(s,"fail?")) {
                n->flags |= CLUSTER_NODE_PFAIL;
            // 一个下线的节点
            } else if (!strcasecmp(s,"fail")) {
                n->flags |= CLUSTER_NODE_FAIL;
                n->fail_time = mstime();
            // 等待向节点发送PING
            } else if (!strcasecmp(s,"handshake")) {
                n->flags |= CLUSTER_NODE_HANDSHAKE;
            // 没有获取该节点的地址
            } else if (!strcasecmp(s,"noaddr")) {
                n->flags |= CLUSTER_NODE_NOADDR;
            // 无标识
            } else if (!strcasecmp(s,"noflags")) {
                /* nothing to do */
            } else {
                serverPanic("Unknown flag in redis cluster config file");
            }
            if (p) s = p+1;
        }

        /* Get master if any. Set the master and populate master's
         * slave list. */
        // 如果有主节点的话，那么设置主节点
        if (argv[3][0] != '-') {
            // 先查找，如果存在则直接设置该从节点从属的主节点
            master = clusterLookupNode(argv[3]);
            // 如果不存在则创建一个新的
            if (!master) {
                master = createClusterNode(argv[3],0);
                clusterAddNode(master);
            }
            n->slaveof = master;
            // 将n加入到主节点master的从节点表中
            clusterNodeAddSlave(master,n);
        }

        /* Set ping sent / pong received timestamps */
        // 设置发送PING 和 接收到PING回复的时间
        if (atoi(argv[4])) n->ping_sent = mstime();
        if (atoi(argv[5])) n->pong_received = mstime();

        /* Set configEpoch for this node. */
        // 设置配置纪元
        n->configEpoch = strtoull(argv[6],NULL,10);

        /* Populate hash slots served by this instance. */
        // 设置从节点的槽
        for (j = 8; j < argc; j++) {
            int start, stop;

            // 处理导出和导入 槽
            if (argv[j][0] == '[') {
                /* Here we handle migrating / importing slots */
                int slot;
                char direction;
                clusterNode *cn;

                p = strchr(argv[j],'-');
                serverAssert(p != NULL);
                *p = '\0';
                // 判断是导出还是导入
                direction = p[1]; /* Either '>' or '<' */
                // 槽
                slot = atoi(argv[j]+1);
                p += 3;
                // 查找目标节点
                cn = clusterLookupNode(p);
                // 目标节点不存在，则创建
                if (!cn) {
                    cn = createClusterNode(p,0);
                    clusterAddNode(cn);
                }
                // 根据方向，设置要导入和导出槽的目标
                if (direction == '>') {
                    server.cluster->migrating_slots_to[slot] = cn;
                } else {
                    server.cluster->importing_slots_from[slot] = cn;
                }
                continue;
            // 没有导出和导入，这是一个区间
            } else if ((p = strchr(argv[j],'-')) != NULL) {
                *p = '\0';
                // 设置开始的槽下标和停止的槽下标
                start = atoi(argv[j]);
                stop = atoi(p+1);
            // 没有导入或导出，这是一个单槽
            } else {
                start = stop = atoi(argv[j]);
            }
            // 将槽载入到节点中
            while(start <= stop) clusterAddSlot(n, start++);
        }

        sdsfreesplitres(argv,argc);
    }
    /* Config sanity check */
    if (server.cluster->myself == NULL) goto fmterr;

    zfree(line);
    fclose(fp);

    serverLog(LL_NOTICE,"Node configuration loaded, I'm %.40s", myself->name);

    /* Something that should never happen: currentEpoch smaller than
     * the max epoch found in the nodes configuration. However we handle this
     * as some form of protection against manual editing of critical files. */
    // 一些事从不应该发生：在集群配置中currentEpoch比最大的纪元小。但是要处理这种情况
    if (clusterGetMaxEpoch() > server.cluster->currentEpoch) {
        server.cluster->currentEpoch = clusterGetMaxEpoch();
    }
    return C_OK;

fmterr:
    serverLog(LL_WARNING,
        "Unrecoverable error: corrupted cluster config file.");
    zfree(line);
    if (fp) fclose(fp);
    exit(1);
}

/* Cluster node configuration is exactly the same as CLUSTER NODES output.
 *
 * This function writes the node config and returns 0, on error -1
 * is returned.
 *
 * Note: we need to write the file in an atomic way from the point of view
 * of the POSIX filesystem semantics, so that if the server is stopped
 * or crashes during the write, we'll end with either the old file or the
 * new one. Since we have the full payload to write available we can use
 * a single write to write the whole file. If the pre-existing file was
 * bigger we pad our payload with newlines that are anyway ignored and truncate
 * the file afterward. */
// 集群节点配置和 CLUSTER NODES 命令输出是一样的
// 这个函数写集群节点的配置，成功返回0，出错返回-1
// 这个写操作必须是原子性的写入
// do_fsync 参数指定是否做同步操作
int clusterSaveConfig(int do_fsync) {
    sds ci;
    size_t content_size;
    struct stat sb;
    int fd;

    server.cluster->todo_before_sleep &= ~CLUSTER_TODO_SAVE_CONFIG;

    /* Get the nodes description and concatenate our "vars" directive to
     * save currentEpoch and lastVoteEpoch. */
    // 获取节点的字符串描述信息，
    ci = clusterGenNodesDescription(CLUSTER_NODE_HANDSHAKE);
    // 追加上纪元信息
    ci = sdscatprintf(ci,"vars currentEpoch %llu lastVoteEpoch %llu\n",
        (unsigned long long) server.cluster->currentEpoch,
        (unsigned long long) server.cluster->lastVoteEpoch);
    content_size = sdslen(ci);
    // 读写打开配置文件
    if ((fd = open(server.cluster_configfile,O_WRONLY|O_CREAT,0644))
        == -1) goto err;

    /* Pad the new payload if the existing file length is greater. */
    // 获取配置文件的信息
    if (fstat(fd,&sb) != -1) {
        // 扩展字符串描述信息的大小
        if (sb.st_size > (off_t)content_size) {
            ci = sdsgrowzero(ci,sb.st_size);
            memset(ci+content_size,'\n',sb.st_size-content_size);
        }
    }
    // 将字符串描述信息写到配置文件中
    if (write(fd,ci,sdslen(ci)) != (ssize_t)sdslen(ci)) goto err;
    // 根据指定的，是否同步到磁盘中
    if (do_fsync) {
        server.cluster->todo_before_sleep &= ~CLUSTER_TODO_FSYNC_CONFIG;
        fsync(fd);
    }

    /* Truncate the file if needed to remove the final \n padding that
     * is just garbage. */
    // 将最后一个'\n'截去
    if (content_size != sdslen(ci) && ftruncate(fd,content_size) == -1) {
        /* ftruncate() failing is not a critical error. */
    }
    close(fd);
    sdsfree(ci);
    return 0;

err:
    if (fd != -1) close(fd);
    sdsfree(ci);
    return -1;
}

// 写配置文件，如果写出错直接退出程序
void clusterSaveConfigOrDie(int do_fsync) {
    if (clusterSaveConfig(do_fsync) == -1) {
        serverLog(LL_WARNING,"Fatal: can't update cluster config file.");
        exit(1);
    }
}

/* Lock the cluster config using flock(), and leaks the file descritor used to
 * acquire the lock so that the file will be locked forever.
 *
 * This works because we always update nodes.conf with a new version
 * in-place, reopening the file, and writing to it in place (later adjusting
 * the length with ftruncate()).
 *
 * On success C_OK is returned, otherwise an error is logged and
 * the function returns C_ERR to signal a lock was not acquired. */
// 使用flock()函数将集群配置文件上锁
int clusterLockConfig(char *filename) {
/* flock() does not exist on Solaris
 * and a fcntl-based solution won't help, as we constantly re-open that file,
 * which will release _all_ locks anyway
 */
#if !defined(__sun)
    /* To lock it, we need to open the file in a way it is created if
     * it does not exist, otherwise there is a race condition with other
     * processes. */
    // 如果文件不存在需要创建一个，因为可能会有一个竞态条件
    int fd = open(filename,O_WRONLY|O_CREAT,0644);
    if (fd == -1) {
        serverLog(LL_WARNING,
            "Can't open %s in order to acquire a lock: %s",
            filename, strerror(errno));
        return C_ERR;
    }
    // LOCK_EX：表示创建一个排他锁，在任意时间内，一个文件的排他锁只能被一个进程拥有
    // 通常情况下，如果加锁请求不能被立即满足，那么系统调用 flock() 会阻塞当前进程。
    // 可以指定LOCK_NB标志，那么系统就不会阻塞该进程
    if (flock(fd,LOCK_EX|LOCK_NB) == -1) {
        if (errno == EWOULDBLOCK) {
            serverLog(LL_WARNING,
                 "Sorry, the cluster configuration file %s is already used "
                 "by a different Redis Cluster node. Please make sure that "
                 "different nodes use different cluster configuration "
                 "files.", filename);
        } else {
            serverLog(LL_WARNING,
                "Impossible to lock %s: %s", filename, strerror(errno));
        }
        close(fd);
        return C_ERR;
    }
    /* Lock acquired: leak the 'fd' by not closing it, so that we'll retain the
     * lock to the file as long as the process exists. */
#endif /* __sun */

    return C_OK;
}

// 初始化集群状态
void clusterInit(void) {
    int saveconf = 0;
    // 初始化配置
    server.cluster = zmalloc(sizeof(clusterState));
    server.cluster->myself = NULL;
    server.cluster->currentEpoch = 0;
    server.cluster->state = CLUSTER_FAIL;
    server.cluster->size = 1;
    server.cluster->todo_before_sleep = 0;
    server.cluster->nodes = dictCreate(&clusterNodesDictType,NULL);
    server.cluster->nodes_black_list =
        dictCreate(&clusterNodesBlackListDictType,NULL);
    server.cluster->failover_auth_time = 0;
    server.cluster->failover_auth_count = 0;
    server.cluster->failover_auth_rank = 0;
    server.cluster->failover_auth_epoch = 0;
    server.cluster->cant_failover_reason = CLUSTER_CANT_FAILOVER_NONE;
    server.cluster->lastVoteEpoch = 0;
    server.cluster->stats_bus_messages_sent = 0;
    server.cluster->stats_bus_messages_received = 0;
    memset(server.cluster->slots,0, sizeof(server.cluster->slots));
    clusterCloseAllSlots();

    /* Lock the cluster config file to make sure every node uses
     * its own nodes.conf. */
    // 配置文件上锁
    if (clusterLockConfig(server.cluster_configfile) == C_ERR)
        exit(1);

    /* Load or create a new nodes configuration. */
    // 载入或创建一个新的节点配置文件
    if (clusterLoadConfig(server.cluster_configfile) == C_ERR) {
        /* No configuration found. We will just use the random name provided
         * by the createClusterNode() function. */
        // 没找到配置文件，随机创建一个集群节点
        myself = server.cluster->myself =
            createClusterNode(NULL,CLUSTER_NODE_MYSELF|CLUSTER_NODE_MASTER);
        serverLog(LL_NOTICE,"No cluster configuration found, I'm %.40s",
            myself->name);
        // 添加到当前集群节点的配置中
        clusterAddNode(myself);
        saveconf = 1;
    }
    // 写配置文件
    if (saveconf) clusterSaveConfigOrDie(1);

    /* We need a listening TCP port for our cluster messaging needs. */
    server.cfd_count = 0;

    /* Port sanity check II
     * The other handshake port check is triggered too late to stop
     * us from trying to use a too-high cluster port number. */
    // 检查端口号是否合法
    if (server.port > (65535-CLUSTER_PORT_INCR)) {
        serverLog(LL_WARNING, "Redis port number too high. "
                   "Cluster communication port is 10,000 port "
                   "numbers higher than your Redis port. "
                   "Your Redis port number must be "
                   "lower than 55535.");
        exit(1);
    }
    // 将该集群节点的端口和fd绑定
    if (listenToPort(server.port+CLUSTER_PORT_INCR,
        server.cfd,&server.cfd_count) == C_ERR)
    {
        exit(1);
    } else {
        int j;
        // 为所有集群的fd设置可读事件的处理函数clusterAcceptHandler
        for (j = 0; j < server.cfd_count; j++) {
            if (aeCreateFileEvent(server.el, server.cfd[j], AE_READABLE,
                clusterAcceptHandler, NULL) == AE_ERR)
                    serverPanic("Unrecoverable error creating Redis Cluster "
                                "file event.");
        }
    }

    /* The slots -> keys map is a sorted set. Init it. */
    // 创建槽映射到键的有序集合
    server.cluster->slots_to_keys = zslCreate();

    /* Set myself->port to my listening port, we'll just need to discover
     * the IP address via MEET messages. */
    // 设置集群端口
    myself->port = server.port;
    // 没有正在进行手动的故障转移
    server.cluster->mf_end = 0;
    // 重置与手动故障转移的状态
    resetManualFailover();
}

/* Reset a node performing a soft or hard reset:
 *
 * 1) All other nodes are forget.
 * 2) All the assigned / open slots are released.
 * 3) If the node is a slave, it turns into a master.
 * 5) Only for hard reset: a new Node ID is generated.
 * 6) Only for hard reset: currentEpoch and configEpoch are set to 0.
 * 7) The new configuration is saved and the cluster state updated.
 * 8) If the node was a slave, the whole data set is flushed away. */
// 重置当前集群节点，hard设置软硬重置
void clusterReset(int hard) {
    dictIterator *di;
    dictEntry *de;
    int j;

    /* Turn into master. */
    // 如果当前集群节点是从节点
    if (nodeIsSlave(myself)) {
        // 将指定的当前集群节点重新配置为主节点
        clusterSetNodeAsMaster(myself);
        // 取消当前的复制，并升级为主节点
        replicationUnsetMaster();
        // 清空所有数据库
        emptyDb(NULL);
    }

    /* Close slots, reset manual failover state. */
    // 清空所有槽的导入导出状态
    clusterCloseAllSlots();
    // 重置与手动故障转移的状态
    resetManualFailover();

    /* Unassign all the slots. */
    // 解除所有指定的槽
    for (j = 0; j < CLUSTER_SLOTS; j++) clusterDelSlot(j);

    /* Forget all the nodes, but myself. */
    // 将所有的其他集群节点忘记，除了自己
    di = dictGetSafeIterator(server.cluster->nodes);
    while((de = dictNext(di)) != NULL) {
        clusterNode *node = dictGetVal(de);

        if (node == myself) continue;
        // 从当前的集群节点保存其他节点的字典中删除指定的node
        clusterDelNode(node);
    }
    dictReleaseIterator(di);

    /* Hard reset only: set epochs to 0, change node ID. */
    // 如果指定硬重置：将纪元设置为0，改变当前节点的runid
    if (hard) {
        sds oldname;
        // 重置所有的纪元信息
        server.cluster->currentEpoch = 0;
        server.cluster->lastVoteEpoch = 0;
        myself->configEpoch = 0;
        serverLog(LL_WARNING, "configEpoch set to 0 via CLUSTER RESET HARD");

        /* To change the Node ID we need to remove the old name from the
         * nodes table, change the ID, and re-add back with new name. */
        // 改变runid
        oldname = sdsnewlen(myself->name, CLUSTER_NAMELEN);
        dictDelete(server.cluster->nodes,oldname);
        sdsfree(oldname);
        getRandomHexChars(myself->name, CLUSTER_NAMELEN);
        clusterAddNode(myself);
        serverLog(LL_NOTICE,"Node hard reset, now I'm %.40s", myself->name);
    }

    /* Make sure to persist the new config and update the state. */
    // 设置进入下一个事件循环之前做的事件，用一下标识来指定
    clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG|
                         CLUSTER_TODO_UPDATE_STATE|
                         CLUSTER_TODO_FSYNC_CONFIG);
}

/* -----------------------------------------------------------------------------
 * CLUSTER communication link
 * -------------------------------------------------------------------------- */
// 创建并返回一个连接对象，关联该node
clusterLink *createClusterLink(clusterNode *node) {
    clusterLink *link = zmalloc(sizeof(*link));
    link->ctime = mstime();
    link->sndbuf = sdsempty();
    link->rcvbuf = sdsempty();
    link->node = node;  //关联节点和该连接对象
    link->fd = -1;
    return link;
}

/* Free a cluster link, but does not free the associated node of course.
 * This function will just make sure that the original node associated
 * with this link will have the 'link' field set to NULL. */
// 释放连接对象
void freeClusterLink(clusterLink *link) {
    // 取消监听事件
    if (link->fd != -1) {
        aeDeleteFileEvent(server.el, link->fd, AE_WRITABLE);
        aeDeleteFileEvent(server.el, link->fd, AE_READABLE);
    }
    // 释放缓冲区
    sdsfree(link->sndbuf);
    sdsfree(link->rcvbuf);
    // 如果该连接对象关联有节点，将该节点的连接关闭
    if (link->node)
        link->node->link = NULL;
    // 释放fd
    close(link->fd);
    zfree(link);
}

#define MAX_CLUSTER_ACCEPTS_PER_CALL 1000
// 集群的fd所设置可读事件的处理函数
void clusterAcceptHandler(aeEventLoop *el, int fd, void *privdata, int mask) {
    int cport, cfd;
    int max = MAX_CLUSTER_ACCEPTS_PER_CALL;
    char cip[NET_IP_STR_LEN];
    clusterLink *link;
    UNUSED(el);
    UNUSED(mask);
    UNUSED(privdata);

    /* If the server is starting up, don't accept cluster connections:
     * UPDATE messages may interact with the database content. */
    // 如果当前节点正在载入数据，则直接返回。不接收集群的连接
    if (server.masterhost == NULL && server.loading) return;

    // 最大每次调用接收1000个连接
    while(max--) {
        // TCP连接的accept
        cfd = anetTcpAccept(server.neterr, fd, cip, sizeof(cip), &cport);
        if (cfd == ANET_ERR) {
            if (errno != EWOULDBLOCK)
                serverLog(LL_VERBOSE,
                    "Error accepting cluster node: %s", server.neterr);
            return;
        }
        // 设置fd为非阻塞模式
        anetNonBlock(NULL,cfd);
        // 禁用 nagle 算法
        anetEnableTcpNoDelay(NULL,cfd);

        /* Use non-blocking I/O for cluster messages. */
        serverLog(LL_VERBOSE,"Accepted cluster node %s:%d", cip, cport);
        /* Create a link object we use to handle the connection.
         * It gets passed to the readable handler when data is available.
         * Initiallly the link->node pointer is set to NULL as we don't know
         * which node is, but the right node is references once we know the
         * node identity. */
        // 当连接成功后，为其创建一个连接对象，但是不关联连接的节点
        link = createClusterLink(NULL);
        link->fd = cfd;
        // 监听该连接的可读事件，并设置处理函数为clusterReadHandler
        aeCreateFileEvent(server.el,cfd,AE_READABLE,clusterReadHandler,link);
    }
}

/* -----------------------------------------------------------------------------
 * Key space handling
 * -------------------------------------------------------------------------- */

/* We have 16384 hash slots. The hash slot of a given key is obtained
 * as the least significant 14 bits of the crc16 of the key.
 *
 * However if the key contains the {...} pattern, only the part between
 * { and } is hashed. This may be useful in the future to force certain
 * keys to be in the same node (assuming no resharding is in progress). */
// 我们有16384个哈希槽，获得给定key的哈希槽作为密钥的crc16的最低有效14比特
// 计算给定key应该被分配到哪个槽，如果key包含 {...} ，那么只对{}中的字符串计算哈希值
unsigned int keyHashSlot(char *key, int keylen) {
    int s, e; /* start-end indexes of { and } */
    // 找'{'字符
    for (s = 0; s < keylen; s++)
        if (key[s] == '{') break;

    /* No '{' ? Hash the whole key. This is the base case. */
    // 没有找到"{}"，直接计算整个key的哈希值
    if (s == keylen) return crc16(key,keylen) & 0x3FFF;

    /* '{' found? Check if we have the corresponding '}'. */
    // 找到'{'，检查是否有'}'
    for (e = s+1; e < keylen; e++)
        if (key[e] == '}') break;

    /* No '}' or nothing betweeen {} ? Hash the whole key. */
    // 没有找到配对的'}'，直接计算整个key的哈希值
    if (e == keylen || e == s+1) return crc16(key,keylen) & 0x3FFF;

    /* If we are here there is both a { and a } on its right. Hash
     * what is in the middle between { and }. */
    // 如果找到了"{}"，计算{}中间的哈希值
    return crc16(key+s+1,e-s-1) & 0x3FFF;
}

/* -----------------------------------------------------------------------------
 * CLUSTER node API
 * -------------------------------------------------------------------------- */

/* Create a new cluster node, with the specified flags.
 * If "nodename" is NULL this is considered a first handshake and a random
 * node name is assigned to this node (it will be fixed later when we'll
 * receive the first pong).
 *
 * The node is created and returned to the user, but it is not automatically
 * added to the nodes hash table. */
// 创建一个带有指定flags的集群节点
// 如果nodename为空，表示该节点还没有进行握手操作，随机指定一个节点名字
// 创建节点并返回给调用者，但不会自动被加入到节点的字典中
clusterNode *createClusterNode(char *nodename, int flags) {
    clusterNode *node = zmalloc(sizeof(*node));
    // 如果指定nodename，那么设置节点name
    if (nodename)
        memcpy(node->name, nodename, CLUSTER_NAMELEN);
    else
        // 随机设置节点的name
        getRandomHexChars(node->name, CLUSTER_NAMELEN);
    // 初始化节点的属性
    node->ctime = mstime();
    node->configEpoch = 0;
    node->flags = flags;
    memset(node->slots,0,sizeof(node->slots));
    node->numslots = 0;
    node->numslaves = 0;
    node->slaves = NULL;
    node->slaveof = NULL;
    node->ping_sent = node->pong_received = 0;
    node->fail_time = 0;
    node->link = NULL;
    memset(node->ip,0,sizeof(node->ip));
    node->port = 0;
    node->fail_reports = listCreate();
    node->voted_time = 0;
    node->orphaned_time = 0;
    node->repl_offset_time = 0;
    node->repl_offset = 0;
    listSetFreeMethod(node->fail_reports,zfree);
    return node;
}

/* This function is called every time we get a failure report from a node.
 * The side effect is to populate the fail_reports list (or to update
 * the timestamp of an existing report).
 *
 * 'failing' is the node that is in failure state according to the
 * 'sender' node.
 *
 * The function returns 0 if it just updates a timestamp of an existing
 * failure report from the same sender. 1 is returned if a new failure
 * report is created. */
// 每次从节点接收到故障报告时调用该函数
// 副作用是会将故障报告添加到fail_reports list中，如果故障报告之前存在，那么会跟新时间戳
// failing是发生故障的节点，而sender是做报告的节点
// 如果只是更新了已经存在故障报告的时间戳，那么返回0，如果创建了新的故障报告，那么返回1
int clusterNodeAddFailureReport(clusterNode *failing, clusterNode *sender) {
    // 获取故障报告的链表
    list *l = failing->fail_reports;
    listNode *ln;
    listIter li;
    clusterNodeFailReport *fr;

    /* If a failure report from the same sender already exists, just update
     * the timestamp. */
    listRewind(l,&li);
    // 遍历故障报告链表
    while ((ln = listNext(&li)) != NULL) {
        fr = ln->value;
        // 如果存在sender之前发送的故障报告
        if (fr->node == sender) {
            // 那么只更新时间戳
            fr->time = mstime();
            return 0;
        }
    }

    /* Otherwise create a new report. */
    // 否则创建新的故障报告
    fr = zmalloc(sizeof(*fr));
    // 设置发送该报告的节点
    fr->node = sender;
    // 设置时间
    fr->time = mstime();
    // 添加到故障报告的链表中
    listAddNodeTail(l,fr);
    return 1;
}

/* Remove failure reports that are too old, where too old means reasonably
 * older than the global node timeout. Note that anyway for a node to be
 * flagged as FAIL we need to have a local PFAIL state that is at least
 * older than the global node timeout, so we don't just trust the number
 * of failure reports from other nodes. */
// 删除超过全局node timeout的故障报告
// 无论如何要将node设置为FAIL，我们需要至少比全局 node timeout 更早的局部 PFAIL 状态
// 因此报告node已下线的节点数量并不是当前节点被标记为PFAIL的唯一条件。
void clusterNodeCleanupFailureReports(clusterNode *node) {
    list *l = node->fail_reports;
    listNode *ln;
    listIter li;
    clusterNodeFailReport *fr;
    // 计算节点报告的最大生存时间
    mstime_t maxtime = server.cluster_node_timeout *
                     CLUSTER_FAIL_REPORT_VALIDITY_MULT;
    mstime_t now = mstime();

    listRewind(l,&li);
    // 遍历报告当前node节点的故障报告链表
    while ((ln = listNext(&li)) != NULL) {
        fr = ln->value;
        // 如果有过期的报告则删除
        if (now - fr->time > maxtime) listDelNode(l,ln);
    }
}

/* Remove the failing report for 'node' if it was previously considered
 * failing by 'sender'. This function is called when a node informs us via
 * gossip that a node is OK from its point of view (no FAIL or PFAIL flags).
 *
 * Note that this function is called relatively often as it gets called even
 * when there are no nodes failing, and is O(N), however when the cluster is
 * fine the failure reports list is empty so the function runs in constant
 * time.
 *
 * The function returns 1 if the failure report was found and removed.
 * Otherwise 0 is returned. */
// 从node节点中删除sender对该节点的故障报告。该函数通过gossip通知当期哨兵节点自己处于OK状态时调用
// 该函数被相对频繁的调用即使没有节点处于故障状态，时间复杂度为O(N)，然而当集群处于良好状态时，故障报告链表为空，所有该函数时间复杂度为常数
// 如果故障报告被删除，返回1，否则返回0
int clusterNodeDelFailureReport(clusterNode *node, clusterNode *sender) {
    list *l = node->fail_reports;
    listNode *ln;
    listIter li;
    clusterNodeFailReport *fr;

    /* Search for a failure report from this sender. */
    // 从node节点的故障报告链表中寻找sender发送的报告
    listRewind(l,&li);
    while ((ln = listNext(&li)) != NULL) {
        fr = ln->value;
        if (fr->node == sender) break;
    }
    // 没找到sender发送的故障报告
    if (!ln) return 0; /* No failure report from this sender. */

    /* Remove the failure report. */
    // 删除该故障报告
    listDelNode(l,ln);
    // 删除过期的故障报告
    clusterNodeCleanupFailureReports(node);
    return 1;
}

/* Return the number of external nodes that believe 'node' is failing,
 * not including this node, that may have a PFAIL or FAIL state for this
 * node as well. */
// 返回认为node节点下线（标记为 PFAIL or FAIL 状态）的其他节点数量，不包含当前节点
int clusterNodeFailureReportsCount(clusterNode *node) {
    // 先将过期的故障报告清理
    clusterNodeCleanupFailureReports(node);
    // 返回报告node节点故障的节点个数
    return listLength(node->fail_reports);
}

// 删除主节点master的从节点slave
int clusterNodeRemoveSlave(clusterNode *master, clusterNode *slave) {
    int j;
    // 遍历所有从属master的从节点
    for (j = 0; j < master->numslaves; j++) {
        // 如果找到指定的slave
        if (master->slaves[j] == slave) {
            // 通过移动覆盖的方式删除slave
            if ((j+1) < master->numslaves) {
                int remaining_slaves = (master->numslaves - j) - 1;
                memmove(master->slaves+j,master->slaves+(j+1),
                        (sizeof(*master->slaves) * remaining_slaves));
            }
            // 计数减1
            master->numslaves--;
            // 没有从节点了，取消导出数据到从节点的标识
            if (master->numslaves == 0)
                master->flags &= ~CLUSTER_NODE_MIGRATE_TO;
            return C_OK;
        }
    }
    return C_ERR;
}

// 为主节点master添加从节点slave
int clusterNodeAddSlave(clusterNode *master, clusterNode *slave) {
    int j;

    /* If it's already a slave, don't add it again. */
    // 遍历所有从节点，以防重复添加
    for (j = 0; j < master->numslaves; j++)
        if (master->slaves[j] == slave) return C_ERR;
    // 分配空间，并添加到末尾
    master->slaves = zrealloc(master->slaves,
        sizeof(clusterNode*)*(master->numslaves+1));
    master->slaves[master->numslaves] = slave;
    // 更新从节点个数计数
    master->numslaves++;
    // 设置导出数据到从节点的标识
    master->flags |= CLUSTER_NODE_MIGRATE_TO;
    return C_OK;
}

// 返回从属n节点，并且处于良好状态的从节点个数
int clusterCountNonFailingSlaves(clusterNode *n) {
    int j, okslaves = 0;

    for (j = 0; j < n->numslaves; j++)
        if (!nodeFailed(n->slaves[j])) okslaves++;
    return okslaves;
}

/* Low level cleanup of the node structure. Only called by clusterDelNode(). */
// 清理释放节点结构，被 clusterDelNode() 调用
void freeClusterNode(clusterNode *n) {
    sds nodename;
    int j;

    /* If the node has associated slaves, we have to set
     * all the slaves->slaveof fields to NULL (unknown). */
    // 如果该节点有从节点，那么取消主从关系，将slaves->slaveof 设置为空
    for (j = 0; j < n->numslaves; j++)
        n->slaves[j]->slaveof = NULL;

    /* Remove this node from the list of slaves of its master. */
    // 如果该节点是从节点，将它从属的主节点中将该从节点删除
    if (nodeIsSlave(n) && n->slaveof) clusterNodeRemoveSlave(n->slaveof,n);

    /* Unlink from the set of nodes. */
    nodename = sdsnewlen(n->name, CLUSTER_NAMELEN);
    // 从集群中删除名字为nodename的节点
    serverAssert(dictDelete(server.cluster->nodes,nodename) == DICT_OK);
    sdsfree(nodename);

    /* Release link and associated data structures. */
    // 释放关联的连接对象结构
    if (n->link) freeClusterLink(n->link);
    // 释放故障报告链表
    listRelease(n->fail_reports);
    // 释放从节点字典
    zfree(n->slaves);
    zfree(n);
}

/* Add a node to the nodes hash table */
// 添加一个node节点到集群的
int clusterAddNode(clusterNode *node) {
    int retval;

    retval = dictAdd(server.cluster->nodes,
            sdsnewlen(node->name,CLUSTER_NAMELEN), node);
    return (retval == DICT_OK) ? C_OK : C_ERR;
}

/* Remove a node from the cluster. The functio performs the high level
 * cleanup, calling freeClusterNode() for the low level cleanup.
 * Here we do the following:
 *
 * 1) Mark all the slots handled by it as unassigned.
 * 2) Remove all the failure reports sent by this node and referenced by
 *    other nodes.
 * 3) Free the node with freeClusterNode() that will in turn remove it
 *    from the hash table and from the list of slaves of its master, if
 *    it is a slave node.
 */
// 从当前的集群保存其他节点的字典中删除指定的delnode
/*
    1. 将所有的槽设置为未指定状态
    2. 删除所有由该节点发送的下线报告
    3. 调用freeClusterNode()释放节点
*/
void clusterDelNode(clusterNode *delnode) {
    int j;
    dictIterator *di;
    dictEntry *de;

    /* 1) Mark slots as unassigned. */
    // 1. 标记所有的槽为未指定状态
    for (j = 0; j < CLUSTER_SLOTS; j++) {
        // 如果该槽被delnode执行导入操作，取消导入操作
        if (server.cluster->importing_slots_from[j] == delnode)
            server.cluster->importing_slots_from[j] = NULL;
        // 如果该槽被delnode执行导出操作，取消导出操作
        if (server.cluster->migrating_slots_to[j] == delnode)
            server.cluster->migrating_slots_to[j] = NULL;
        // 设置槽为未指定状态
        if (server.cluster->slots[j] == delnode)
            clusterDelSlot(j);
    }

    /* 2) Remove failure reports. */
    // 2. 删除故障报告
    di = dictGetSafeIterator(server.cluster->nodes);
    while((de = dictNext(di)) != NULL) {
        clusterNode *node = dictGetVal(de);
        // 跳过delnode
        if (node == delnode) continue;
        // 删除由delnode所发送的故障报告
        clusterNodeDelFailureReport(node,delnode);
    }
    dictReleaseIterator(di);

    /* 3) Free the node, unlinking it from the cluster. */
    // 释放delnode
    freeClusterNode(delnode);
}

/* Node lookup by name */
// 根据name从集群中查找并返回节点
clusterNode *clusterLookupNode(char *name) {
    sds s = sdsnewlen(name, CLUSTER_NAMELEN);
    dictEntry *de;

    de = dictFind(server.cluster->nodes,s);
    sdsfree(s);
    if (de == NULL) return NULL;
    return dictGetVal(de);
}

/* This is only used after the handshake. When we connect a given IP/PORT
 * as a result of CLUSTER MEET we don't have the node name yet, so we
 * pick a random one, and will fix it when we receive the PONG request using
 * this function. */
// 当第一次连接一个 IP/PORT 时，发送CLUSTER MEET 命令时不知道节点的name，所以随机设置了一个。
// 当目标节点返回PONG回复时，就知道了该节点的name，所有为该节点改名
void clusterRenameNode(clusterNode *node, char *newname) {
    int retval;
    sds s = sdsnewlen(node->name, CLUSTER_NAMELEN);

    serverLog(LL_DEBUG,"Renaming node %.40s into %.40s",
        node->name, newname);
    // 从集群中删除旧名字的节点
    retval = dictDelete(server.cluster->nodes, s);
    sdsfree(s);
    serverAssert(retval == DICT_OK);
    // 设置新名字，并且添加到集群中
    memcpy(node->name, newname, CLUSTER_NAMELEN);
    clusterAddNode(node);
}

/* -----------------------------------------------------------------------------
 * CLUSTER config epoch handling
 * -------------------------------------------------------------------------- */

/* Return the greatest configEpoch found in the cluster, or the current
 * epoch if greater than any node configEpoch. */
// 返回集群中最大的configEpoch
uint64_t clusterGetMaxEpoch(void) {
    uint64_t max = 0;
    dictIterator *di;
    dictEntry *de;

    di = dictGetSafeIterator(server.cluster->nodes);
    // 遍历集群中所有的节点
    while((de = dictNext(di)) != NULL) {
        clusterNode *node = dictGetVal(de);
        // 获取最大的configEpoch
        if (node->configEpoch > max) max = node->configEpoch;
    }
    dictReleaseIterator(di);
    // 如果最大的configEpoch还大于当前集群的纪元，那么返回当前集群的纪元
    if (max < server.cluster->currentEpoch) max = server.cluster->currentEpoch;
    return max;
}

/* If this node epoch is zero or is not already the greatest across the
 * cluster (from the POV of the local configuration), this function will:
 *
 * 1) Generate a new config epoch, incrementing the current epoch.
 * 2) Assign the new epoch to this node, WITHOUT any consensus.
 * 3) Persist the configuration on disk before sending packets with the
 *    new configuration.
 *
 * If the new config epoch is generated and assigend, C_OK is returned,
 * otherwise C_ERR is returned (since the node has already the greatest
 * configuration around) and no operation is performed.
 *
 * Important note: this function violates the principle that config epochs
 * should be generated with consensus and should be unique across the cluster.
 * However Redis Cluster uses this auto-generated new config epochs in two
 * cases:
 *
 * 1) When slots are closed after importing. Otherwise resharding would be
 *    too expensive.
 * 2) When CLUSTER FAILOVER is called with options that force a slave to
 *    failover its master even if there is not master majority able to
 *    create a new configuration epoch.
 *
 * Redis Cluster will not explode using this function, even in the case of
 * a collision between this node and another node, generating the same
 * configuration epoch unilaterally, because the config epoch conflict
 * resolution algorithm will eventually move colliding nodes to different
 * config epochs. However using this function may violate the "last failover
 * wins" rule, so should only be used with care. */
// 如果节点的纪元为0或者已经不是整个集群中最大的，该函数将会：
/*
    1. 生成一个新的配置纪元（config epoch），将当前的纪元加1
    2. 为该节点指定新的纪元，不需要一致性
    3. 在发送新的配置之前，将当前配置保存在磁盘上
*/
// 如果当前配置纪元被指定和生成，返回C_OK，否则返回C_ERR（因为节点已经有最大的配置），什么也不执行
// 如果函数违反了配置纪元应该被一致性的生成原则，那么应该在集群中独一无二存在。但是集群在以下情况自动生成配置纪元：
/*
    1. 当槽被导入后应该关闭，否则重新集群分配非常划不来
    2. 当CLUSTER FAILOVER被指定，强制从节点取故障转移他的主机的，即使没有主节点能够创建新的配置纪元
*/
// Redis Cluster不会使用此函数而退出，即使在此节点与另一个节点之间发生冲突的情况下，单向生成相同的配置纪元，因为配置纪元冲突解决算法最终将冲突节点移动到不同的配置时期。 但是使用此功能可能会违反“上次故障切换成功”规则，因此只能小心使用
int clusterBumpConfigEpochWithoutConsensus(void) {
    uint64_t maxEpoch = clusterGetMaxEpoch();
    // 如果节点的纪元为0或者已经不是整个集群中最大的
    if (myself->configEpoch == 0 ||
        myself->configEpoch != maxEpoch)
    {
        // 将当前纪元加1
        server.cluster->currentEpoch++;
        // 设置为当前节点的配置纪元
        myself->configEpoch = server.cluster->currentEpoch;
        // 设置进入下一个事件循环之前做的事件，用一下标识来指定
        clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG|
                             CLUSTER_TODO_FSYNC_CONFIG);
        serverLog(LL_WARNING,
            "New configEpoch set to %llu",
            (unsigned long long) myself->configEpoch);
        return C_OK;
    } else {
        return C_ERR;
    }
}

/* This function is called when this node is a master, and we receive from
 * another master a configuration epoch that is equal to our configuration
 * epoch.
 *
 * BACKGROUND
 *
 * It is not possible that different slaves get the same config
 * epoch during a failover election, because the slaves need to get voted
 * by a majority. However when we perform a manual resharding of the cluster
 * the node will assign a configuration epoch to itself without to ask
 * for agreement. Usually resharding happens when the cluster is working well
 * and is supervised by the sysadmin, however it is possible for a failover
 * to happen exactly while the node we are resharding a slot to assigns itself
 * a new configuration epoch, but before it is able to propagate it.
 *
 * So technically it is possible in this condition that two nodes end with
 * the same configuration epoch.
 *
 * Another possibility is that there are bugs in the implementation causing
 * this to happen.
 *
 * Moreover when a new cluster is created, all the nodes start with the same
 * configEpoch. This collision resolution code allows nodes to automatically
 * end with a different configEpoch at startup automatically.
 *
 * In all the cases, we want a mechanism that resolves this issue automatically
 * as a safeguard. The same configuration epoch for masters serving different
 * set of slots is not harmful, but it is if the nodes end serving the same
 * slots for some reason (manual errors or software bugs) without a proper
 * failover procedure.
 *
 * In general we want a system that eventually always ends with different
 * masters having different configuration epochs whatever happened, since
 * nothign is worse than a split-brain condition in a distributed system.
 *
 * BEHAVIOR
 *
 * When this function gets called, what happens is that if this node
 * has the lexicographically smaller Node ID compared to the other node
 * with the conflicting epoch (the 'sender' node), it will assign itself
 * the greatest configuration epoch currently detected among nodes plus 1.
 *
 * This means that even if there are multiple nodes colliding, the node
 * with the greatest Node ID never moves forward, so eventually all the nodes
 * end with a different configuration epoch.
 */
// 处理配置纪元冲突
void clusterHandleConfigEpochCollision(clusterNode *sender) {
    /* Prerequisites: nodes have the same configEpoch and are both masters. */
    // 如果纪元不冲突，或者sender不是主节点，或当前集群节点不是主节点，则直接返回
    if (sender->configEpoch != myself->configEpoch ||
        !nodeIsMaster(sender) || !nodeIsMaster(myself)) return;
    /* Don't act if the colliding node has a smaller Node ID. */
    // 如果冲突的节点有相同的节点ID，直接返回
    if (memcmp(sender->name,myself->name,CLUSTER_NAMELEN) <= 0) return;
    /* Get the next ID available at the best of this node knowledge. */
    // 否则获取下一个当前纪元
    server.cluster->currentEpoch++;
    // 设置为当前集群节点的配置纪元
    myself->configEpoch = server.cluster->currentEpoch;
    // 写配置文件
    clusterSaveConfigOrDie(1);
    serverLog(LL_VERBOSE,
        "WARNING: configEpoch collision with node %.40s."
        " configEpoch set to %llu",
        sender->name,
        (unsigned long long) myself->configEpoch);
}

/* -----------------------------------------------------------------------------
 * CLUSTER nodes blacklist
 *
 * The nodes blacklist is just a way to ensure that a given node with a given
 * Node ID is not readded before some time elapsed (this time is specified
 * in seconds in CLUSTER_BLACKLIST_TTL).
 *
 * This is useful when we want to remove a node from the cluster completely:
 * when CLUSTER FORGET is called, it also puts the node into the blacklist so
 * that even if we receive gossip messages from other nodes that still remember
 * about the node we want to remove, we don't re-add it before some time.
 *
 * Currently the CLUSTER_BLACKLIST_TTL is set to 1 minute, this means
 * that redis-trib has 60 seconds to send CLUSTER FORGET messages to nodes
 * in the cluster without dealing with the problem of other nodes re-adding
 * back the node to nodes we already sent the FORGET command to.
 *
 * The data structure used is a hash table with an sds string representing
 * the node ID as key, and the time when it is ok to re-add the node as
 * value.
 * -------------------------------------------------------------------------- */
/*
        集群节点黑名单
    节点黑名单是一个方法，确保一个有指定ID的指定节点不被在 CLUSTER_BLACKLIST_TTL 时间内被重新添加到集群中

    当我们向要从集群中完全删除一个节点是非常有用的：当 CLUSTER FORGET 被执行时，它将节点放到黑名单中，以便即使我们从其他节点的流言中接收到关于被移除节点的消息，我们也不会在指定时间内重复添加。

    当前 CLUSTER_BLACKLIST_TTL 被设置为1分钟，这意味着 redis-trib 有60s的时间发送 CLUSTER FORGET 命令给集群中的其他节点，而不需要处理其他节点将被删除的节点重新加入到集群中的问题。

    黑名单的数据结构是一个字典，键是字符串的节点ID，值是可以重新添加该节点的时间
*/

#define CLUSTER_BLACKLIST_TTL 60      /* 1 minute. */


/* Before of the addNode() or Exists() operations we always remove expired
 * entries from the black list. This is an O(N) operation but it is not a
 * problem since add / exists operations are called very infrequently and
 * the hash table is supposed to contain very little elements at max.
 * However without the cleanup during long uptimes and with some automated
 * node add/removal procedures, entries could accumulate. */
/*
    在执行 addNode() 或者  Exists() 操作之前我们总是需要先从黑名单中删除过期的节点。
    这个函数的复杂度为O(N)但是不是问题，因为 add / exists 操作很少执行并且哈希表的链表应该包含非常少的元素
    但是在很长时间内没有清理过去节点，一些节点的 add/removal 程序会造成节点累计
*/

// 清理黑名单中过期的节点
void clusterBlacklistCleanup(void) {
    dictIterator *di;
    dictEntry *de;

    di = dictGetSafeIterator(server.cluster->nodes_black_list);
    // 遍历黑名单
    while((de = dictNext(di)) != NULL) {
        int64_t expire = dictGetUnsignedIntegerVal(de);
        // 如果过期，则从黑名单字典中删除该节点
        if (expire < server.unixtime)
            dictDelete(server.cluster->nodes_black_list,dictGetKey(de));
    }
    dictReleaseIterator(di);
}

/* Cleanup the blacklist and add a new node ID to the black list. */
// 清理黑名单中过期的节点然后添加新的节点到黑名单中
void clusterBlacklistAddNode(clusterNode *node) {
    dictEntry *de;
    // 获取node的ID
    sds id = sdsnewlen(node->name,CLUSTER_NAMELEN);
    // 先清理黑名单中过期的节点
    clusterBlacklistCleanup();
    // 然后将node添加到黑名单中
    if (dictAdd(server.cluster->nodes_black_list,id,NULL) == DICT_OK) {
        /* If the key was added, duplicate the sds string representation of
         * the key for the next lookup. We'll free it at the end. */
        // 如果添加成功，创建一个id的复制品，以便能够在最后free
        id = sdsdup(id);
    }
    // 找到指定id的节点
    de = dictFind(server.cluster->nodes_black_list,id);
    // 为其设置过期时间
    dictSetUnsignedIntegerVal(de,time(NULL)+CLUSTER_BLACKLIST_TTL);
    sdsfree(id);
}

/* Return non-zero if the specified node ID exists in the blacklist.
 * You don't need to pass an sds string here, any pointer to 40 bytes
 * will work. */
// 如果指定ID的节点在黑名单中存在，返回非0，不需要传递一个sds，只需要一个指向40字节的字符串指针
int clusterBlacklistExists(char *nodeid) {
    // 获取sds类型的id
    sds id = sdsnewlen(nodeid,CLUSTER_NAMELEN);
    int retval;
    // 先清理黑名单中过期的节点
    clusterBlacklistCleanup();
    // 检查节点是否存在
    retval = dictFind(server.cluster->nodes_black_list,id) != NULL;
    sdsfree(id);
    return retval;
}

/* -----------------------------------------------------------------------------
 * CLUSTER messages exchange - PING/PONG and gossip
 * -------------------------------------------------------------------------- */

/* This function checks if a given node should be marked as FAIL.
 * It happens if the following conditions are met:
 *
 * 1) We received enough failure reports from other master nodes via gossip.
 *    Enough means that the majority of the masters signaled the node is
 *    down recently.
 * 2) We believe this node is in PFAIL state.
 *
 * If a failure is detected we also inform the whole cluster about this
 * event trying to force every other node to set the FAIL flag for the node.
 *
 * Note that the form of agreement used here is weak, as we collect the majority
 * of masters state during some time, and even if we force agreement by
 * propagating the FAIL message, because of partitions we may not reach every
 * node. However:
 *
 * 1) Either we reach the majority and eventually the FAIL state will propagate
 *    to all the cluster.
 * 2) Or there is no majority so no slave promotion will be authorized and the
 *    FAIL flag will be cleared after some time.
 */

/*
        集群节点的交互 - PING/PONG 和 流言（gossip）
    函数用来检查节点是否被标记为 FAIL ，一下两种条件满足，则节点被标记为FAIL
    1. 通过流言协议从其他的主节点接收到足够多的故障报告，大多数的主节点认为node处于下线
    2. 当前集群节点也认为node处于 PFAIL 状态

    如果察觉到了一个故障，会通知整个集群强制使所有的节点将该node设置 FAIL 标识

    这种判断节点下线的方法是弱（weak）的，因为我们在一段时间内收集大多数的主节点状态并且即使我们是强制通过传播 FAIL 消息来征求同意，由于分区的问题我们无法可达每一个节点。但是：
    1. 只要我们达到了大多数，最终 FAIL 状态会传播到所有的集群中
    2. 没有达到大多数所以不能晋升从节点，会在一段时间内将 FAIL 标识清除
*/
// 判断node节点是否处于下线
void markNodeAsFailingIfNeeded(clusterNode *node) {
    int failures;
    // 需要大多数的票数，超过一半的节点数量
    int needed_quorum = (server.cluster->size / 2) + 1;
    // 不处于pfail（需要确认是否故障）状态，则直接返回
    if (!nodeTimedOut(node)) return; /* We can reach it. */
    // 处于fail（已确认为故障）状态，则直接返回
    if (nodeFailed(node)) return; /* Already FAILing. */
    // 返回认为node节点下线（标记为 PFAIL or FAIL 状态）的其他节点数量
    failures = clusterNodeFailureReportsCount(node);
    /* Also count myself as a voter if I'm a master. */
    // 如果当前节点是主节点，也投一票
    if (nodeIsMaster(myself)) failures++;
    // 如果报告node故障的节点数量不够总数的一半，无法判定node是否下线，直接返回
    if (failures < needed_quorum) return; /* No weak agreement from masters. */

    serverLog(LL_NOTICE,
        "Marking node %.40s as failing (quorum reached).", node->name);

    /* Mark the node as failing. */
    // 取消PFAIL，设置为FAIL
    node->flags &= ~CLUSTER_NODE_PFAIL;
    node->flags |= CLUSTER_NODE_FAIL;
    // 并设置下线时间
    node->fail_time = mstime();

    /* Broadcast the failing node name to everybody, forcing all the other
     * reachable nodes to flag the node as FAIL. */
    // 广播下线节点的名字给所有的节点，强制所有的其他可达的节点为该节点设置FAIL标识
    if (nodeIsMaster(myself)) clusterSendFail(node->name);
    clusterDoBeforeSleep(CLUSTER_TODO_UPDATE_STATE|CLUSTER_TODO_SAVE_CONFIG);
}

/* This function is called only if a node is marked as FAIL, but we are able
 * to reach it again. It checks if there are the conditions to undo the FAIL
 * state. */
// 如果一个节点被标识为FAIL，调用该函数，但是我们对于该节点再次可达，需要检查是否取消该节点的FAIL标识
void clearNodeFailureIfNeeded(clusterNode *node) {
    mstime_t now = mstime();
    // 确保node已经下线
    serverAssert(nodeFailed(node));

    /* For slaves we always clear the FAIL flag if we can contact the
     * node again. */
    // 如果node节点是从节点，并且我们能够可达该节点，清除对应的FAIL标识
    if (nodeIsSlave(node) || node->numslots == 0) {
        serverLog(LL_NOTICE,
            "Clear FAIL state for node %.40s: %s is reachable again.",
                node->name,
                nodeIsSlave(node) ? "slave" : "master without slots");
        // 取消FAIL标识
        node->flags &= ~CLUSTER_NODE_FAIL;
        clusterDoBeforeSleep(CLUSTER_TODO_UPDATE_STATE|CLUSTER_TODO_SAVE_CONFIG);
    }

    /* If it is a master and...
     * 1) The FAIL state is old enough.
     * 2) It is yet serving slots from our point of view (not failed over).
     * Apparently no one is going to fix these slots, clear the FAIL flag. */
    // 如果node节点是主节点
    /*
        1. FAIL标识已经过去一段时间
        2. 该节点仍有负责的槽
    */
    // 显然该节点仍有未迁移的槽，所以要清除FAIL标识
    if (nodeIsMaster(node) && node->numslots > 0 &&
        (now - node->fail_time) >
        (server.cluster_node_timeout * CLUSTER_FAIL_UNDO_TIME_MULT))
    {
        serverLog(LL_NOTICE,
            "Clear FAIL state for node %.40s: is reachable again and nobody is serving its slots after some time.",
                node->name);
        // 清除FAIL标识
        node->flags &= ~CLUSTER_NODE_FAIL;
        clusterDoBeforeSleep(CLUSTER_TODO_UPDATE_STATE|CLUSTER_TODO_SAVE_CONFIG);
    }
}

/* Return true if we already have a node in HANDSHAKE state matching the
 * specified ip address and port number. This function is used in order to
 * avoid adding a new handshake node for the same address multiple times. */
// 如果当前ip和port的节点处于握手状态的话，返回1，该函数被用来避免重复和相同地址的节点进行握手
int clusterHandshakeInProgress(char *ip, int port) {
    dictIterator *di;
    dictEntry *de;

    di = dictGetSafeIterator(server.cluster->nodes);
    // 遍历集群中的节点
    while((de = dictNext(di)) != NULL) {
        clusterNode *node = dictGetVal(de);
        // 跳过不处于握手状态的节点
        if (!nodeInHandshake(node)) continue;
        // 如果当前正在握手的节点和给定的地址相同，跳出循环
        if (!strcasecmp(node->ip,ip) && node->port == port) break;
    }
    dictReleaseIterator(di);
    return de != NULL;
}

/* Start an handshake with the specified address if there is not one
 * already in progress. Returns non-zero if the handshake was actually
 * started. On error zero is returned and errno is set to one of the
 * following values:
 *
 * EAGAIN - There is already an handshake in progress for this address.
 * EINVAL - IP or port are not valid. */
// 如果没有正在进行握手，那么根据执行的地址开始进行握手
/*
    握手开始返回非零
    出错返回0，且设置errno

    EAGAIN：该地址应处于握手状态
    EINVAL：地址非法
*/
int clusterStartHandshake(char *ip, int port) {
    clusterNode *n;
    char norm_ip[NET_IP_STR_LEN];
    struct sockaddr_storage sa;

    /* IP sanity check */
    // 检查地址是否非法
    if (inet_pton(AF_INET,ip,
            &(((struct sockaddr_in *)&sa)->sin_addr)))
    {
        sa.ss_family = AF_INET;
    } else if (inet_pton(AF_INET6,ip,
            &(((struct sockaddr_in6 *)&sa)->sin6_addr)))
    {
        sa.ss_family = AF_INET6;
    } else {
        errno = EINVAL;
        return 0;
    }

    /* Port sanity check */
    // 检查端口号是否合法
    if (port <= 0 || port > (65535-CLUSTER_PORT_INCR)) {
        errno = EINVAL;
        return 0;
    }

    /* Set norm_ip as the normalized string representation of the node
     * IP address. */
    // 设置 norm_ip 作为节点地址的标准字符串表示形式
    memset(norm_ip,0,NET_IP_STR_LEN);
    if (sa.ss_family == AF_INET)
        inet_ntop(AF_INET,
            (void*)&(((struct sockaddr_in *)&sa)->sin_addr),
            norm_ip,NET_IP_STR_LEN);
    else
        inet_ntop(AF_INET6,
            (void*)&(((struct sockaddr_in6 *)&sa)->sin6_addr),
            norm_ip,NET_IP_STR_LEN);
    // 判断当前地址是否处于握手状态，如果是，则设置errno并返回，该函数被用来避免重复和相同地址的节点进行握手
    if (clusterHandshakeInProgress(norm_ip,port)) {
        errno = EAGAIN;
        return 0;
    }

    /* Add the node with a random address (NULL as first argument to
     * createClusterNode()). Everything will be fixed during the
     * handshake. */
    // 为node设置一个随机的名字，当握手完成时会为其设置真正的名字
    // 创建一个随机名字的节点
    n = createClusterNode(NULL,CLUSTER_NODE_HANDSHAKE|CLUSTER_NODE_MEET);
    // 设置地址
    memcpy(n->ip,norm_ip,sizeof(n->ip));
    n->port = port;
    // 添加到集群中
    clusterAddNode(n);
    return 1;
}

/* Process the gossip section of PING or PONG packets.
 * Note that this function assumes that the packet is already sanity-checked
 * by the caller, not in the content of the gossip section, but in the
 * length. */
// 处理流言中的 PING or PONG 数据包，函数调用者应该检查流言包的合法性
void clusterProcessGossipSection(clusterMsg *hdr, clusterLink *link) {
    // 获取该条消息包含的节点数信息
    uint16_t count = ntohs(hdr->count);
    // clusterMsgDataGossip数组的地址
    clusterMsgDataGossip *g = (clusterMsgDataGossip*) hdr->data.ping.gossip;
    // 发送消息的节点
    clusterNode *sender = link->node ? link->node : clusterLookupNode(hdr->sender);

    // 遍历所有节点的信息
    while(count--) {
        // 获取节点的标识信息
        uint16_t flags = ntohs(g->flags);
        clusterNode *node;
        sds ci;
        // 根据获取的标识信息，生成用逗号连接的sds字符串ci
        ci = representClusterNodeFlags(sdsempty(), flags);
        // 打印到日志中
        serverLog(LL_DEBUG,"GOSSIP %.40s %s:%d %s",
            g->nodename,
            g->ip,
            ntohs(g->port),
            ci);
        sdsfree(ci);

        /* Update our state accordingly to the gossip sections */
        // 根据指定name从集群中查找并返回节点
        node = clusterLookupNode(g->nodename);
        // 如果node存在
        if (node) {
            /* We already know this node.
               Handle failure reports, only when the sender is a master. */
            // 如果发送者是主节点，且不是node本身
            if (sender && nodeIsMaster(sender) && node != myself) {
                // 如果标识中指定了关于下线的状态
                if (flags & (CLUSTER_NODE_FAIL|CLUSTER_NODE_PFAIL)) {
                    // 将sender的添加到node的故障报告中
                    if (clusterNodeAddFailureReport(node,sender)) {
                        serverLog(LL_VERBOSE,
                            "Node %.40s reported node %.40s as not reachable.",
                            sender->name, node->name);
                    }
                    // 判断node节点是否处于真正的下线FAIL状态
                    markNodeAsFailingIfNeeded(node);
                // 如果标识表示节点处于正常状态
                } else {
                    // 将sender从node的故障报告中删除
                    if (clusterNodeDelFailureReport(node,sender)) {
                        serverLog(LL_VERBOSE,
                            "Node %.40s reported node %.40s is back online.",
                            sender->name, node->name);
                    }
                }
            }

            /* If we already know this node, but it is not reachable, and
             * we see a different address in the gossip section of a node that
             * can talk with this other node, update the address, disconnect
             * the old link if any, so that we'll attempt to connect with the
             * new address. */
            // 虽然node存在，但是node已经处于下线状态
            // 但是消息中的标识却反应该节点不处于下线状态，并且实际的地址和消息中的地址发生变化
            // 这些表明该节点换了新地址，尝试进行握手
            if (node->flags & (CLUSTER_NODE_FAIL|CLUSTER_NODE_PFAIL) &&
                !(flags & CLUSTER_NODE_NOADDR) &&
                !(flags & (CLUSTER_NODE_FAIL|CLUSTER_NODE_PFAIL)) &&
                (strcasecmp(node->ip,g->ip) || node->port != ntohs(g->port)))
            {
                // 释放原来的集群连接对象
                if (node->link) freeClusterLink(node->link);
                // 设置节点的地址为消息中的地址
                memcpy(node->ip,g->ip,NET_IP_STR_LEN);
                node->port = ntohs(g->port);
                // 清除无地址的标识
                node->flags &= ~CLUSTER_NODE_NOADDR;
            }
        // node不存在，没有在当前集群中找到
        } else {
            /* If it's not in NOADDR state and we don't have it, we
             * start a handshake process against this IP/PORT pairs.
             *
             * Note that we require that the sender of this gossip message
             * is a well known node in our cluster, otherwise we risk
             * joining another cluster. */
            // 如果node不处于NOADDR状态，并且集群中没有该节点，那么向node发送一个握手的消息
            // 注意，当前sender节点必须是本集群的众所周知的节点（不在集群的黑名单中），否则有加入另一个集群的风险
            if (sender &&
                !(flags & CLUSTER_NODE_NOADDR) &&
                !clusterBlacklistExists(g->nodename))
            {
                // 开始进行握手
                clusterStartHandshake(g->ip,ntohs(g->port));
            }
        }

        /* Next node */
        // 下一个节点
        g++;
    }
}

/* IP -> string conversion. 'buf' is supposed to at least be 46 bytes. */
// ip转换为字符串
void nodeIp2String(char *buf, clusterLink *link) {
    anetPeerToString(link->fd, buf, NET_IP_STR_LEN, NULL);
}

/* Update the node address to the IP address that can be extracted
 * from link->fd, and at the specified port.
 * Also disconnect the node link so that we'll connect again to the new
 * address.
 *
 * If the ip/port pair are already correct no operation is performed at
 * all.
 *
 * The function returns 0 if the node address is still the same,
 * otherwise 1 is returned. */
// 更新节点的IP地址，可以从 link->fd 中获取
// 而且断开节点的连接以便连接新的地址
// 如果新的ip和port和现在的相同，什么也不执行
// 如果节点地址不变，返回0，否则返回1
int nodeUpdateAddressIfNeeded(clusterNode *node, clusterLink *link, int port) {
    char ip[NET_IP_STR_LEN] = {0};

    /* We don't proceed if the link is the same as the sender link, as this
     * function is designed to see if the node link is consistent with the
     * symmetric link that is used to receive PINGs from the node.
     *
     * As a side effect this function never frees the passed 'link', so
     * it is safe to call during packet processing. */
    // 如果连接是相同的，直接返回
    if (link == node->link) return 0;
    // 将连接的IP地址转换为字符串
    nodeIp2String(ip,link);
    // 如果连接的ip和port和节点的地址相同，直接返回
    if (node->port == port && strcmp(ip,node->ip) == 0) return 0;

    /* IP / port is different, update it. */
    // 地址不同，更新它
    // 设置节点的新地址
    memcpy(node->ip,ip,sizeof(ip));
    node->port = port;
    // 释放旧的连接
    if (node->link) freeClusterLink(node->link);
    // 取消NOADDR标识
    node->flags &= ~CLUSTER_NODE_NOADDR;
    serverLog(LL_WARNING,"Address updated for node %.40s, now %s:%d",
        node->name, node->ip, node->port);

    /* Check if this is our master and we have to change the
     * replication target as well. */
    // 如果当前集群节点是从节点，且从属于node节点
    if (nodeIsSlave(myself) && myself->slaveof == node)
        // 需要更新当前节点执行复制的主节点地址
        replicationSetMaster(node->ip, node->port);
    return 1;
}

/* Reconfigure the specified node 'n' as a master. This function is called when
 * a node that we believed to be a slave is now acting as master in order to
 * update the state of the node. */
// 将指定的n节点重新配置为主节点
void clusterSetNodeAsMaster(clusterNode *n) {
    // 指定的节点已经是主节点，直接返回
    if (nodeIsMaster(n)) return;
    // 该从节点有从属的主节点
    if (n->slaveof) {
        // 删除从属主节点的指定从节点n
        clusterNodeRemoveSlave(n->slaveof,n);
        // 为指定n节点设置MIGRATE的标识
        if (n != myself) n->flags |= CLUSTER_NODE_MIGRATE_TO;
    }
    // 取消从节点标识，设置主节点标识
    n->flags &= ~CLUSTER_NODE_SLAVE;
    n->flags |= CLUSTER_NODE_MASTER;
    // 将从属的主节点置为空
    n->slaveof = NULL;

    /* Update config and state. */
    // 更新配置和状态
    clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG|
                         CLUSTER_TODO_UPDATE_STATE);
}

/* This function is called when we receive a master configuration via a
 * PING, PONG or UPDATE packet. What we receive is a node, a configEpoch of the
 * node, and the set of slots claimed under this configEpoch.
 *
 * What we do is to rebind the slots with newer configuration compared to our
 * local configuration, and if needed, we turn ourself into a replica of the
 * node (see the function comments for more info).
 *
 * The 'sender' is the node for which we received a configuration update.
 * Sometimes it is not actually the "Sender" of the information, like in the
 * case we receive the info via an UPDATE packet. */
// 当我们通过 PING, PONG or UPDATE 消息接收到主节点的配置时调用该函数。
// 接收到是一个节点，一个节点配置纪元和节点在该配置纪元下的槽信息。并将这些作为该函数的参数
// 我们所要做的是比较本地的配置，用新的配置绑定槽，如果需要，将本节点转换为一个复制的节点
// sender参数是发送消息的节点，也可以是是消息发送节点的主节点
void forgetclusterUpdateSlotsConfigWith(clusterNode *sender, uint64_t senderConfigEpoch, unsigned char *slots) {
    int j;
    clusterNode *curmaster, *newmaster = NULL;
    /* The dirty slots list is a list of slots for which we lose the ownership
     * while having still keys inside. This usually happens after a failover
     * or after a manual cluster reconfiguration operated by the admin.
     *
     * If the update message is not able to demote a master to slave (in this
     * case we'll resync with the master updating the whole key space), we
     * need to delete all the keys in the slots we lost ownership. */
    uint16_t dirty_slots[CLUSTER_SLOTS];
    int dirty_slots_count = 0;

    /* Here we set curmaster to this node or the node this node
     * replicates to if it's a slave. In the for loop we are
     * interested to check if slots are taken away from curmaster. */
    // 如果当前节点是主节点，那么获取当前节点
    // 如果当前节点是从节点，那么获取当前从节点所从属的主节点
    curmaster = nodeIsMaster(myself) ? myself : myself->slaveof;
    // 如果发送消息的节点就是本节点，则直接返回
    if (sender == myself) {
        serverLog(LL_WARNING,"Discarding UPDATE message about myself.");
        return;
    }
    // 遍历所有槽
    for (j = 0; j < CLUSTER_SLOTS; j++) {
        // 如果当前槽已经被分配
        if (bitmapTestBit(slots,j)) {
            /* The slot is already bound to the sender of this message. */
            // 如果当前槽是sender负责的，那么跳过当前槽
            if (server.cluster->slots[j] == sender) continue;

            /* The slot is in importing state, it should be modified only
             * manually via redis-trib (example: a resharding is in progress
             * and the migrating side slot was already closed and is advertising
             * a new config. We still want the slot to be closed manually). */
            // 如果当前槽处于导入状态，它应该只能通过redis-trib 被手动修改，所以跳过该槽
            if (server.cluster->importing_slots_from[j]) continue;

            /* We rebind the slot to the new node claiming it if:
             * 1) The slot was unassigned or the new node claims it with a
             *    greater configEpoch.
             * 2) We are not currently importing the slot. */
            // 将槽重新绑定到新的节点，如果满足以下条件
            /*
                1. 该槽没有被指定或者新的节点声称它有一个更大的配置纪元
                2. 当前没有导入该槽
            */
            if (server.cluster->slots[j] == NULL ||
                server.cluster->slots[j]->configEpoch < senderConfigEpoch)
            {
                /* Was this slot mine, and still contains keys? Mark it as
                 * a dirty slot. */
                // 如果当前槽被当前节点所负责，而且槽中有数据，表示该槽发生冲突
                if (server.cluster->slots[j] == myself &&
                    countKeysInSlot(j) &&
                    sender != myself)
                {
                    // 将发生冲突的槽记录到脏槽中
                    dirty_slots[dirty_slots_count] = j;
                    // 脏槽数加1
                    dirty_slots_count++;
                }
                // 如果当前槽属于当前节点的主节点，表示发生了故障转移
                if (server.cluster->slots[j] == curmaster)
                    newmaster = sender;
                // 删除当前被指定的槽
                clusterDelSlot(j);
                // 将槽分配给sender
                clusterAddSlot(sender,j);
                clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG|
                                     CLUSTER_TODO_UPDATE_STATE|
                                     CLUSTER_TODO_FSYNC_CONFIG);
            }
        }
    }

    /* If at least one slot was reassigned from a node to another node
     * with a greater configEpoch, it is possible that:
     * 1) We are a master left without slots. This means that we were
     *    failed over and we should turn into a replica of the new
     *    master.
     * 2) We are a slave and our master is left without slots. We need
     *    to replicate to the new slots owner. */
    // 如果至少一个槽被重新分配，从一个节点到另一个更大配置纪元的节点，那么可能发生了：
    /*
        1. 当前节点是一个不在处理任何槽的主节点，这是应该将当前节点设置为新主节点的从节点
        2. 当前节点是一个从节点，并且当前节点的主节点不在处理任何槽，这是应该将当前节点设置为新主节点的从节点
    */
    if (newmaster && curmaster->numslots == 0) {
        serverLog(LL_WARNING,
            "Configuration change detected. Reconfiguring myself "
            "as a replica of %.40s", sender->name);
        // 将sender设置为当前节点myself的主节点
        clusterSetMaster(sender);
        clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG|
                             CLUSTER_TODO_UPDATE_STATE|
                             CLUSTER_TODO_FSYNC_CONFIG);
    } else if (dirty_slots_count) {
        /* If we are here, we received an update message which removed
         * ownership for certain slots we still have keys about, but still
         * we are serving some slots, so this master node was not demoted to
         * a slave.
         *
         * In order to maintain a consistent state between keys and slots
         * we need to remove all the keys from the slots we lost. */
        // 如果执行到这里，我们接收到一个删除当前我们负责槽的所有者的更新消息，但是我们仍然负责该槽，所以主节点不能被降级为从节点
        // 为了保持键和槽的关系，需要从我们丢失的槽中将键删除
        for (j = 0; j < dirty_slots_count; j++)
            // 遍历所有的脏槽，删除槽中的键-
            delKeysInSlot(dirty_slots[j]);
    }
}

/* When this function is called, there is a packet to process starting
 * at node->rcvbuf. Releasing the buffer is up to the caller, so this
 * function should just handle the higher level stuff of processing the
 * packet, modifying the cluster state if needed.
 *
 * The function returns 1 if the link is still valid after the packet
 * was processed, otherwise 0 if the link was freed since the packet
 * processing lead to some inconsistency error (for instance a PONG
 * received from the wrong sender ID). */
// 当该函数被调用时，说明在 node->rcvbuf 的缓冲区中有一个消息包开始被处理。释放换从去由调用者决定，所以函数应该只处理包的内容，并且按需修改集群的状态
// 如果当消息包被处理后连接仍然有效，函数返回1，否则返回0，表示因为处理消息包导致一些不一致的错误，连接被释放。
// 例如：从错误ID的sender节点接收到一个的PONG
int clusterProcessPacket(clusterLink *link) {
    // 连接的输入（接收）缓冲区
    clusterMsg *hdr = (clusterMsg*) link->rcvbuf;
    // 消息的总长度
    uint32_t totlen = ntohl(hdr->totlen);
    // 消息的类型
    uint16_t type = ntohs(hdr->type);
    // 通过Cluster接收到的消息数量加1
    server.cluster->stats_bus_messages_received++;
    serverLog(LL_DEBUG,"--- Processing packet of type %d, %lu bytes",
        type, (unsigned long) totlen);

    /* Perform sanity checks */
    // 检查消息包的合法性
    // 至少包含一个签名、版本、总长、消息正文包含的节点信息数量
    if (totlen < 16) return 1; /* At least signature, version, totlen, count. */
    // 总长度大于接收缓冲区的大小
    if (totlen > sdslen(link->rcvbuf)) return 1;
    // 目前版本号为0，不处理其他版本
    if (ntohs(hdr->ver) != CLUSTER_PROTO_VER) {
        /* Can't handle messages of different versions. */
        return 1;
    }
    // 获取发送消息节点的标识
    uint16_t flags = ntohs(hdr->flags);
    uint64_t senderCurrentEpoch = 0, senderConfigEpoch = 0;
    clusterNode *sender;
    // 如果消息是PING、PONG或者MEET
    if (type == CLUSTERMSG_TYPE_PING || type == CLUSTERMSG_TYPE_PONG ||
        type == CLUSTERMSG_TYPE_MEET)
    {
        // 消息正文包含的节点信息数量
        uint16_t count = ntohs(hdr->count);
        uint32_t explen; /* expected length of this packet */
        // 计算消息包应该的长度
        explen = sizeof(clusterMsg)-sizeof(union clusterMsgData);
        explen += (sizeof(clusterMsgDataGossip)*count);
        // 总长度和计算的长度不相同返回1
        if (totlen != explen) return 1;
    // 如果消息是FAIL
    } else if (type == CLUSTERMSG_TYPE_FAIL) {
        // 计算消息包应该的长度
        uint32_t explen = sizeof(clusterMsg)-sizeof(union clusterMsgData);

        explen += sizeof(clusterMsgDataFail);
        // 总长度和计算的长度不相同返回1
        if (totlen != explen) return 1;
    // 如果消息是PUBLISH
    } else if (type == CLUSTERMSG_TYPE_PUBLISH) {
        uint32_t explen = sizeof(clusterMsg)-sizeof(union clusterMsgData);

        explen += sizeof(clusterMsgDataPublish) -
                8 +
                ntohl(hdr->data.publish.msg.channel_len) +
                ntohl(hdr->data.publish.msg.message_len);
        if (totlen != explen) return 1;
    // 如果消息是故障有关的
    } else if (type == CLUSTERMSG_TYPE_FAILOVER_AUTH_REQUEST ||
               type == CLUSTERMSG_TYPE_FAILOVER_AUTH_ACK ||
               type == CLUSTERMSG_TYPE_MFSTART)
    {
        uint32_t explen = sizeof(clusterMsg)-sizeof(union clusterMsgData);

        if (totlen != explen) return 1;
    //如果消息是UPDATE的
    } else if (type == CLUSTERMSG_TYPE_UPDATE) {
        uint32_t explen = sizeof(clusterMsg)-sizeof(union clusterMsgData);

        explen += sizeof(clusterMsgDataUpdate);
        if (totlen != explen) return 1;
    }

    /* Check if the sender is a known node. */
    // 检查sender节点是否是集群中众所周知的节点

    // 从集群中查找sender节点
    sender = clusterLookupNode(hdr->sender);
    // 如果sender存在，但是不是处于握手状态的节点
    if (sender && !nodeInHandshake(sender)) {
        /* Update our curretEpoch if we see a newer epoch in the cluster. */
        // 如果sender的纪元大于集群的纪元，更新集群的纪元
        senderCurrentEpoch = ntohu64(hdr->currentEpoch);
        senderConfigEpoch = ntohu64(hdr->configEpoch);
        // 更新集群的当前纪元
        if (senderCurrentEpoch > server.cluster->currentEpoch)
            server.cluster->currentEpoch = senderCurrentEpoch;
        /* Update the sender configEpoch if it is publishing a newer one. */
        // 更新sender的配置纪元
        if (senderConfigEpoch > sender->configEpoch) {
            sender->configEpoch = senderConfigEpoch;
            // 更新配置和状态
            clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG|
                                 CLUSTER_TODO_FSYNC_CONFIG);
        }
        /* Update the replication offset info for this node. */
        // 更新sender的复制偏移量和更新复制偏移量的时间
        sender->repl_offset = ntohu64(hdr->offset);
        sender->repl_offset_time = mstime();
        /* If we are a slave performing a manual failover and our master
         * sent its offset while already paused, populate the MF state. */
        // 如果当前节点是正在执行手动故障转移的从节点，该当前节点的主节点正是sender节点
        // 并且主节点发送复制偏移量时已经暂停手动故障转移
        if (server.cluster->mf_end &&
            nodeIsSlave(myself) &&
            myself->slaveof == sender &&
            hdr->mflags[0] & CLUSTERMSG_FLAG0_PAUSED &&
            server.cluster->mf_master_offset == 0)
        {
            // 设置当前从节点已经复制的偏移量
            server.cluster->mf_master_offset = sender->repl_offset;
            serverLog(LL_WARNING,
                "Received replication offset for paused "
                "master manual failover: %lld",
                server.cluster->mf_master_offset);
        }
    }

    /* Initial processing of PING and MEET requests replying with a PONG. */
    // 初始处理PING和MEET请求，用PONG作为回复
    if (type == CLUSTERMSG_TYPE_PING || type == CLUSTERMSG_TYPE_MEET) {
        serverLog(LL_DEBUG,"Ping packet received: %p", (void*)link->node);

        /* We use incoming MEET messages in order to set the address
         * for 'myself', since only other cluster nodes will send us
         * MEET messagses on handshakes, when the cluster joins, or
         * later if we changed address, and those nodes will use our
         * official address to connect to us. So by obtaining this address
         * from the socket is a simple way to discover / update our own
         * address in the cluster without it being hardcoded in the config.
         *
         * However if we don't have an address at all, we update the address
         * even with a normal PING packet. If it's wrong it will be fixed
         * by MEET later. */
        // 我们使用传入的MEET消息来设置当前myself节点的地址，因为只有其他集群中的节点在握手的时会发送MEET消息，当有节点加入集群时，或者如果我们改变地址，这些节点将使用我们公开的地址来连接我们，所以在集群中，通过套接字来获取地址是一个简单的方法去discover / update我们自己的地址，而不是在配置中的硬设置
        // 但是，如果我们根本没有地址，即使使用正常的PING数据包，我们也会更新该地址。 如果是错误的，那么会被MEET修改

        // 如果是MEET消息
        // 或者是其他消息但是当前集群节点的IP为空
        if (type == CLUSTERMSG_TYPE_MEET || myself->ip[0] == '\0') {
            char ip[NET_IP_STR_LEN];
            // 可以根据fd来获取ip，并设置myself节点的IP
            if (anetSockName(link->fd,ip,sizeof(ip),NULL) != -1 &&
                strcmp(ip,myself->ip))
            {
                memcpy(myself->ip,ip,NET_IP_STR_LEN);
                serverLog(LL_WARNING,"IP address for this node updated to %s",
                    myself->ip);
                clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG);
            }
        }

        /* Add this node if it is new for us and the msg type is MEET.
         * In this stage we don't try to add the node with the right
         * flags, slaveof pointer, and so forth, as this details will be
         * resolved when we'll receive PONGs from the node. */
        // 如果当前sender节点是一个新的节点，并且消息是MEET消息类型，那么将这个节点添加到集群中
        // 当前该节点的flags、slaveof等等都没有设置，当从其他节点接收到PONG时可以从中获取到信息
        if (!sender && type == CLUSTERMSG_TYPE_MEET) {
            clusterNode *node;
            // 创建一个处于握手状态的节点
            node = createClusterNode(NULL,CLUSTER_NODE_HANDSHAKE);
            // 设置ip和port
            nodeIp2String(node->ip,link);
            node->port = ntohs(hdr->port);
            // 添加到集群中
            clusterAddNode(node);
            clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG);
        }

        /* If this is a MEET packet from an unknown node, we still process
         * the gossip section here since we have to trust the sender because
         * of the message type. */
        // 如果是从一个未知的节点发送过来MEET包，处理流言信息
        if (!sender && type == CLUSTERMSG_TYPE_MEET)
            // 处理流言中的 PING or PONG 数据包
            clusterProcessGossipSection(hdr,link);

        /* Anyway reply with a PONG */
        // 回复一个PONG消息
        clusterSendPing(link,CLUSTERMSG_TYPE_PONG);
    }

    /* PING, PONG, MEET: process config information. */
    // 如果是PING、PONG或MEET消息，处理配置信息
    if (type == CLUSTERMSG_TYPE_PING || type == CLUSTERMSG_TYPE_PONG ||
        type == CLUSTERMSG_TYPE_MEET)
    {
        serverLog(LL_DEBUG,"%s packet received: %p",
            type == CLUSTERMSG_TYPE_PING ? "ping" : "pong",
            (void*)link->node);
        // 如果关联该连接的节点存在
        if (link->node) {
            // 如果关联该连接的节点处于握手状态
            if (nodeInHandshake(link->node)) {
                /* If we already have this node, try to change the
                 * IP/port of the node with the new one. */
                // sender节点存在，用该新的连接地址更新sender节点的地址
                if (sender) {
                    serverLog(LL_VERBOSE,
                        "Handshake: we already know node %.40s, "
                        "updating the address if needed.", sender->name);
                    if (nodeUpdateAddressIfNeeded(sender,link,ntohs(hdr->port)))
                    {
                        clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG|
                                             CLUSTER_TODO_UPDATE_STATE);
                    }
                    /* Free this node as we already have it. This will
                     * cause the link to be freed as well. */
                    // 释放关联该连接的节点
                    clusterDelNode(link->node);
                    return 0;
                }

                /* First thing to do is replacing the random name with the
                 * right node name if this was a handshake stage. */
                // 将关联该连接的节点的名字用sender的名字替代
                clusterRenameNode(link->node, hdr->sender);
                serverLog(LL_DEBUG,"Handshake with node %.40s completed.",
                    link->node->name);
                // 取消握手状态，设置节点的角色
                link->node->flags &= ~CLUSTER_NODE_HANDSHAKE;
                link->node->flags |= flags&(CLUSTER_NODE_MASTER|CLUSTER_NODE_SLAVE);
                clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG);
            // 如果sender的地址和关联该连接的节点的地址不相同
            } else if (memcmp(link->node->name,hdr->sender,
                        CLUSTER_NAMELEN) != 0)
            {
                /* If the reply has a non matching node ID we
                 * disconnect this node and set it as not having an associated
                 * address. */
                serverLog(LL_DEBUG,"PONG contains mismatching sender ID. About node %.40s added %d ms ago, having flags %d",
                    link->node->name,
                    (int)(mstime()-(link->node->ctime)),
                    link->node->flags);
                // 设置NOADDR标识，情况关联连接节点的地址
                link->node->flags |= CLUSTER_NODE_NOADDR;
                link->node->ip[0] = '\0';
                link->node->port = 0;
                // 释放连接对象
                freeClusterLink(link);
                clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG);
                return 0;
            }
        }

        /* Update the node address if it changed. */
        // 如果发送的消息为PING类型，sender节点不处于握手状态
        // 那么更新sender节点的IP地址
        if (sender && type == CLUSTERMSG_TYPE_PING &&
            !nodeInHandshake(sender) &&
            nodeUpdateAddressIfNeeded(sender,link,ntohs(hdr->port)))
        {
            clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG|
                                 CLUSTER_TODO_UPDATE_STATE);
        }

        /* Update our info about the node */
        // 关联该连接的节点存在，且消息类型为PONG
        if (link->node && type == CLUSTERMSG_TYPE_PONG) {
            // 更新接收到PONG的时间
            link->node->pong_received = mstime();
            // 清零最近一次发送PING的时间戳
            link->node->ping_sent = 0;

            /* The PFAIL condition can be reversed without external
             * help if it is momentary (that is, if it does not
             * turn into a FAIL state).
             *
             * The FAIL condition is also reversible under specific
             * conditions detected by clearNodeFailureIfNeeded(). */
            // 接收到PONG回复，可以删除PFAIL（疑似下线）标识
            // FAIL标识能否删除，需要clearNodeFailureIfNeeded()来决定
            // 如果关联该连接的节点疑似下线
            if (nodeTimedOut(link->node)) {
                // 取消PFAIL标识
                link->node->flags &= ~CLUSTER_NODE_PFAIL;
                clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG|
                                     CLUSTER_TODO_UPDATE_STATE);
            // 如果关联该连接的节点已经被判断为下线
            } else if (nodeFailed(link->node)) {
                // 如果一个节点被标识为FAIL，需要检查是否取消该节点的FAIL标识，因为该节点在一定时间内重新上线了
                clearNodeFailureIfNeeded(link->node);
            }
        }

        /* Check for role switch: slave -> master or master -> slave. */
        // 主从切换的检测
        if (sender) {
            // 如果消息头的slaveof为空名字，那么说明sender节点是主节点
            if (!memcmp(hdr->slaveof,CLUSTER_NODE_NULL_NAME,
                sizeof(hdr->slaveof)))
            {
                /* Node is a master. */
                // 将指定的sender节点重新配置为主节点
                clusterSetNodeAsMaster(sender);
            // sender是从节点
            } else {
                /* Node is a slave. */
                // 根据名字从集群中查找并返回sender从节点的主节点
                clusterNode *master = clusterLookupNode(hdr->slaveof);
                // sender标识自己为主节点，但是消息中显示它为从节点
                if (nodeIsMaster(sender)) {
                    /* Master turned into a slave! Reconfigure the node. */
                    // 删除主节点所负责的槽
                    clusterDelNodeSlots(sender);
                    // 消息主节点标识和导出标识
                    sender->flags &= ~(CLUSTER_NODE_MASTER|
                                       CLUSTER_NODE_MIGRATE_TO);
                    // 设置为从节点标识
                    sender->flags |= CLUSTER_NODE_SLAVE;

                    /* Update config and state. */
                    // 更新配置和状态
                    clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG|
                                         CLUSTER_TODO_UPDATE_STATE);
                }

                /* Master node changed for this slave? */
                // sender的主节点发生改变
                if (master && sender->slaveof != master) {
                    // 如果sender有新的主节点，将sender从旧的主节点保存其从节点字典中删除
                    if (sender->slaveof)
                        clusterNodeRemoveSlave(sender->slaveof,sender);
                    // 将sender添加到新的主节点的从节点字典中
                    clusterNodeAddSlave(master,sender);
                    // 设置sender的主节点
                    sender->slaveof = master;

                    /* Update config. */
                    // 更新配置
                    clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG);
                }
            }
        }

        /* Update our info about served slots.
         *
         * Note: this MUST happen after we update the master/slave state
         * so that CLUSTER_NODE_MASTER flag will be set. */

        /* Many checks are only needed if the set of served slots this
         * instance claims is different compared to the set of slots we have
         * for it. Check this ASAP to avoid other computational expansive
         * checks later. */
        // 更新当前节点所负责的槽信息
        // 这些操作必须在更新主从状态之后进行，因为需要CLUSTER_NODE_MASTER标识
        clusterNode *sender_master = NULL; /* Sender or its master if slave. */
        int dirty_slots = 0; /* Sender claimed slots don't match my view? */

        if (sender) {
            // 如果sender是从节点，那么获取其主节点信息
            // 如果sender是主节点，那么获取sender的信息
            sender_master = nodeIsMaster(sender) ? sender : sender->slaveof;
            if (sender_master) {
                // sender发送的槽信息和主节点的槽信息是否匹配
                dirty_slots = memcmp(sender_master->slots,
                        hdr->myslots,sizeof(hdr->myslots)) != 0;
            }
        }

        /* 1) If the sender of the message is a master, and we detected that
         *    the set of slots it claims changed, scan the slots to see if we
         *    need to update our configuration. */
        // 1. 如果sender是主节点，但是槽信息出现不匹配现象
        if (sender && nodeIsMaster(sender) && dirty_slots)
            // 检查当前节点对sender的槽信息，并且进行更新
            clusterUpdateSlotsConfigWith(sender,senderConfigEpoch,hdr->myslots);

        /* 2) We also check for the reverse condition, that is, the sender
         *    claims to serve slots we know are served by a master with a
         *    greater configEpoch. If this happens we inform the sender.
         *
         * This is useful because sometimes after a partition heals, a
         * reappearing master may be the last one to claim a given set of
         * hash slots, but with a configuration that other instances know to
         * be deprecated. Example:
         *
         * A and B are master and slave for slots 1,2,3.
         * A is partitioned away, B gets promoted.
         * B is partitioned away, and A returns available.
         *
         * Usually B would PING A publishing its set of served slots and its
         * configEpoch, but because of the partition B can't inform A of the
         * new configuration, so other nodes that have an updated table must
         * do it. In this way A will stop to act as a master (or can try to
         * failover if there are the conditions to win the election). */
        // 2. 检测和条件1相反的情况，sender处理的槽的配置纪元比当前节点已知的配置纪元要低，如果是这样，则通知sender
        // 这种情况可能出现在网络分裂中，一个重新上线的主节点可能会带有已经过时的槽信息
        /*
            A负责槽1,2,3，而B是A的从节点
            A被网络分割出去，B被晋升为主节点
            B被网络分割出去，A重新上线，但是A的槽信息是旧的
        */
        // 通常情况下，B应该向A发送一个PING消息，告知A，B已经被接替了槽1,2,3，并且还有配置纪元，但是因为网络分裂的缘故，节点B无法向A通知，所以通知节点A它所带有的槽信息一斤更新的工作交给其他知道B带有更高配置纪元的节点来做
        // 当A接到其他节点关于节点B的消息时，节点A就会停止自己的主节点工作，又或者重新进行故障转移
        if (sender && dirty_slots) {
            int j;
            // 遍历所有的槽
            for (j = 0; j < CLUSTER_SLOTS; j++) {
                // 检测当前的槽是否已经指定
                if (bitmapTestBit(hdr->myslots,j)) {
                    // 如果当前槽由sender负责或者没有人负责，则跳过该槽
                    if (server.cluster->slots[j] == sender ||
                        server.cluster->slots[j] == NULL) continue;
                    // 如果当前槽的配置纪元大于sender的配置纪元
                    if (server.cluster->slots[j]->configEpoch >
                        senderConfigEpoch)
                    {
                        serverLog(LL_VERBOSE,
                            "Node %.40s has old slots configuration, sending "
                            "an UPDATE message about %.40s",
                                sender->name, server.cluster->slots[j]->name);
                        // 向sender的发送关于当前槽的更新信息
                        clusterSendUpdate(sender->link,
                            server.cluster->slots[j]);

                        /* TODO: instead of exiting the loop send every other
                         * UPDATE packet for other nodes that are the new owner
                         * of sender's slots. */
                        break;
                    }
                }
            }
        }

        /* If our config epoch collides with the sender's try to fix
         * the problem. */
        // 如果当前节点是主节点sender也是主节点，但是sender的配置纪元和myself节点配置纪元相同，发生了冲突
        if (sender &&
            nodeIsMaster(myself) && nodeIsMaster(sender) &&
            senderConfigEpoch == myself->configEpoch)
        {   // 处理配置纪元冲突
            clusterHandleConfigEpochCollision(sender);
        }

        /* Get info from the gossip section */
        // 处理流言中的 PING or PONG 数据包的流言部分
        if (sender) clusterProcessGossipSection(hdr,link);
    // 如果是FAIL类型的消息
    } else if (type == CLUSTERMSG_TYPE_FAIL) {
        clusterNode *failing;

        if (sender) {
            // 获取下线节点的地址
            failing = clusterLookupNode(hdr->data.fail.about.nodename);
            // 如果下线节点不是myself节点也不是处于下线状态
            if (failing &&
                !(failing->flags & (CLUSTER_NODE_FAIL|CLUSTER_NODE_MYSELF)))
            {
                serverLog(LL_NOTICE,
                    "FAIL message received from %.40s about %.40s",
                    hdr->sender, hdr->data.fail.about.nodename);
                // 设置FAIL标识
                failing->flags |= CLUSTER_NODE_FAIL;
                // 设置下线时间
                failing->fail_time = mstime();
                // 取消PFAIL标识
                failing->flags &= ~CLUSTER_NODE_PFAIL;
                clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG|
                                     CLUSTER_TODO_UPDATE_STATE);
            }
        } else {
            serverLog(LL_NOTICE,
                "Ignoring FAIL message from unknown node %.40s about %.40s",
                hdr->sender, hdr->data.fail.about.nodename);
        }
    // 如果是PUBLISH类型的消息
    } else if (type == CLUSTERMSG_TYPE_PUBLISH) {
        robj *channel, *message;
        uint32_t channel_len, message_len;

        /* Don't bother creating useless objects if there are no
         * Pub/Sub subscribers. */
        // 如果有订阅者时才创建消息对象
        if (dictSize(server.pubsub_channels) ||
           listLength(server.pubsub_patterns))
        {
            // 频道名字长度
            channel_len = ntohl(hdr->data.publish.msg.channel_len);
            // 消息的长度
            message_len = ntohl(hdr->data.publish.msg.message_len);
            // 创建频道对象
            channel = createStringObject(
                        (char*)hdr->data.publish.msg.bulk_data,channel_len);
            // 创建消息对象
            message = createStringObject(
                        (char*)hdr->data.publish.msg.bulk_data+channel_len,
                        message_len);
            // 发布消息到指定的频道
            pubsubPublishMessage(channel,message);
            // 释放对象
            decrRefCount(channel);
            decrRefCount(message);
        }
    // 如果是一个请求获得故障迁移授权的消息
    } else if (type == CLUSTERMSG_TYPE_FAILOVER_AUTH_REQUEST) {
        if (!sender) return 1;  /* We don't know that node. */
        // 如果条件允许，向sender投票，支持它进行故障转移
        clusterSendFailoverAuthIfNeeded(sender,hdr);
    // 如果是一条故障迁移投票的信息
    } else if (type == CLUSTERMSG_TYPE_FAILOVER_AUTH_ACK) {
        if (!sender) return 1;  /* We don't know that node. */
        /* We consider this vote only if the sender is a master serving
         * a non zero number of slots, and its currentEpoch is greater or
         * equal to epoch where this node started the election. */
        // 如果sender是主节点，且sender有负责的槽对象，sender的配置纪元大于等于当前节点的配置纪元
        if (nodeIsMaster(sender) && sender->numslots > 0 &&
            senderCurrentEpoch >= server.cluster->failover_auth_epoch)
        {
            // 增加节点获得票数
            server.cluster->failover_auth_count++;
            /* Maybe we reached a quorum here, set a flag to make sure
             * we check ASAP. */
            clusterDoBeforeSleep(CLUSTER_TODO_HANDLE_FAILOVER);
        }
    // 如果是暂停手动故障转移信息
    } else if (type == CLUSTERMSG_TYPE_MFSTART) {
        /* This message is acceptable only if I'm a master and the sender
         * is one of my slaves. */
        // 如果myself是主节点并且sender是myself的从节点才处理该消息
        if (!sender || sender->slaveof != myself) return 1;
        /* Manual failover requested from slaves. Initialize the state
         * accordingly. */
        // 从从节点接收到手动故障转移，初始化状态
        // 重置与手动故障转移的状态
        resetManualFailover();
        // 设置手动故障转移的时间限制
        server.cluster->mf_end = mstime() + CLUSTER_MF_TIMEOUT;
        // 设置执行手动孤战转移的从节点
        server.cluster->mf_slave = sender;
        // 暂停client，使服务器在指定时间内停止接收client的命令
        pauseClients(mstime()+(CLUSTER_MF_TIMEOUT*2));
        serverLog(LL_WARNING,"Manual failover requested by slave %.40s.",
            sender->name);
    // 如果是一条更新消息
    } else if (type == CLUSTERMSG_TYPE_UPDATE) {
        clusterNode *n; /* The node the update is about. */
        // 获取消息中的节点的配置纪元
        uint64_t reportedConfigEpoch =
                    ntohu64(hdr->data.update.nodecfg.configEpoch);

        if (!sender) return 1;  /* We don't know the sender. */
        // 查找到需要更新的节点
        n = clusterLookupNode(hdr->data.update.nodecfg.nodename);
        if (!n) return 1;   /* We don't know the reported node. */
        // 如果需要更新的节点的配置纪元大于消息中的配置纪元，直接返回
        if (n->configEpoch >= reportedConfigEpoch) return 1; /* Nothing new. */

        /* If in our current config the node is a slave, set it as a master. */
        // 如果需要更新的节点是从节点，将该节点重新配置为主节点
        if (nodeIsSlave(n)) clusterSetNodeAsMaster(n);

        /* Update the node's configEpoch. */
        // 将报告中的配置纪元设置为该节点的配置纪元
        n->configEpoch = reportedConfigEpoch;
        clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG|
                             CLUSTER_TODO_FSYNC_CONFIG);

        /* Check the bitmap of served slots and update our
         * config accordingly. */
        // 对该节点的槽信息进行更新，通过消息中包含的信息
        clusterUpdateSlotsConfigWith(n,reportedConfigEpoch,
            hdr->data.update.nodecfg.slots);
    } else {
        serverLog(LL_WARNING,"Received unknown packet type: %d", type);
    }
    return 1;
}

/* This function is called when we detect the link with this node is lost.
   We set the node as no longer connected. The Cluster Cron will detect
   this connection and will try to get it connected again.

   Instead if the node is a temporary node used to accept a query, we
   completely free the node on error. */
// 当我们察觉到关联连接的节点丢失时，调用该函数。将该节点的状态设置为断开状态。集群的定期函数根据该状态重新连接节点。
// 如果连接是一个临时连接的话，那么他就会永远释放，不再进行重连
void handleLinkIOError(clusterLink *link) {
    freeClusterLink(link);
}

/* Send data. This is handled using a trivial send buffer that gets
 * consumed by write(). We don't try to optimize this for speed too much
 * as this is a very low traffic channel. */
// 可写事件的处理函数。用于向集群节点发送信息
void clusterWriteHandler(aeEventLoop *el, int fd, void *privdata, int mask) {
    clusterLink *link = (clusterLink*) privdata;
    ssize_t nwritten;
    UNUSED(el);
    UNUSED(mask);
    // 将连接对象的发送缓冲区的数据发送到fd中
    nwritten = write(fd, link->sndbuf, sdslen(link->sndbuf));
    if (nwritten <= 0) {
        serverLog(LL_DEBUG,"I/O error writing to node link: %s",
            strerror(errno));
        handleLinkIOError(link);
        return;
    }
    // 保留未发送的数据
    sdsrange(link->sndbuf,nwritten,-1);
    // 如果发送完成，则取消监听fd的可写事件
    if (sdslen(link->sndbuf) == 0)
        aeDeleteFileEvent(server.el, link->fd, AE_WRITABLE);
}

/* Read data. Try to read the first field of the header first to check the
 * full length of the packet. When a whole packet is in memory this function
 * will call the function to process the packet. And so forth. */
// 可读事件的处理函数。用于从集群节点中读数据
void clusterReadHandler(aeEventLoop *el, int fd, void *privdata, int mask) {
    char buf[sizeof(clusterMsg)];
    ssize_t nread;
    clusterMsg *hdr;
    clusterLink *link = (clusterLink*) privdata;
    unsigned int readlen, rcvbuflen;
    UNUSED(el);
    UNUSED(mask);

    // 循环从fd读取数据
    while(1) { /* Read as long as there is data to read. */
        // 获取连接对象的接收缓冲区的长度，表示一次最多能多大的数据量
        rcvbuflen = sdslen(link->rcvbuf);
        // 如果接收缓冲区的长度小于八字节，就无法读入消息的总长
        if (rcvbuflen < 8) {
            /* First, obtain the first 8 bytes to get the full message
             * length. */
            readlen = 8 - rcvbuflen;
        // 能够读入完整数据信息
        } else {
            /* Finally read the full message. */
            hdr = (clusterMsg*) link->rcvbuf;
            // 如果是8个字节
            if (rcvbuflen == 8) {
                /* Perform some sanity check on the message signature
                 * and length. */
                // 如果前四个字节不是"RCmb"签名，释放连接
                if (memcmp(hdr->sig,"RCmb",4) != 0 ||
                    ntohl(hdr->totlen) < CLUSTERMSG_MIN_LEN)
                {
                    serverLog(LL_WARNING,
                        "Bad message length or signature received "
                        "from Cluster bus.");
                    handleLinkIOError(link);
                    return;
                }
            }
            // 记录已经读入的内容长度
            readlen = ntohl(hdr->totlen) - rcvbuflen;
            if (readlen > sizeof(buf)) readlen = sizeof(buf);
        }
        // 从fd中读数据
        nread = read(fd,buf,readlen);
        // 没有数据可读
        if (nread == -1 && errno == EAGAIN) return; /* No more data ready. */
        // 读错误，释放连接
        if (nread <= 0) {
            /* I/O error... */
            serverLog(LL_DEBUG,"I/O error reading from node link: %s",
                (nread == 0) ? "connection closed" : strerror(errno));
            handleLinkIOError(link);
            return;
        } else {
            // 将读到的数据追加到连接对象的接收缓冲区中
            /* Read data and recast the pointer to the new buffer. */
            link->rcvbuf = sdscatlen(link->rcvbuf,buf,nread);
            hdr = (clusterMsg*) link->rcvbuf;
            rcvbuflen += nread;
        }

        /* Total length obtained? Process this packet. */
        // 检查接收的数据是否完整
        if (rcvbuflen >= 8 && rcvbuflen == ntohl(hdr->totlen)) {
            // 如果读到的数据有效，处理读到接收缓冲区的数据
            if (clusterProcessPacket(link)) {
                // 处理成功，则设置新的空的接收缓冲区
                sdsfree(link->rcvbuf);
                link->rcvbuf = sdsempty();
            } else {
                return; /* Link no longer valid. */
            }
        }
    }
}

/* Put stuff into the send buffer.
 *
 * It is guaranteed that this function will never have as a side effect
 * the link to be invalidated, so it is safe to call this function
 * from event handlers that will do stuff with the same link later. */
// 将数据添加到连接的发送缓冲区中
void clusterSendMessage(clusterLink *link, unsigned char *msg, size_t msglen) {
    // 如果发送缓冲区为空
    if (sdslen(link->sndbuf) == 0 && msglen != 0)
        // 监听连接的可写事件，并设置可写的事件处理程序clusterWriteHandler
        aeCreateFileEvent(server.el,link->fd,AE_WRITABLE,
                    clusterWriteHandler,link);
    // 将msg的数据追加到发送缓冲区
    link->sndbuf = sdscatlen(link->sndbuf, msg, msglen);
    // 设置发送的状态
    server.cluster->stats_bus_messages_sent++;
}

/* Send a message to all the nodes that are part of the cluster having
 * a connected link.
 *
 * It is guaranteed that this function will never have as a side effect
 * some node->link to be invalidated, so it is safe to call this function
 * from event handlers that will do stuff with node links later. */
// 向节点连接的所有其他节点发送信息
void clusterBroadcastMessage(void *buf, size_t len) {
    dictIterator *di;
    dictEntry *de;

    di = dictGetSafeIterator(server.cluster->nodes);
    // 迭代集群中的所有节点
    while((de = dictNext(di)) != NULL) {
        clusterNode *node = dictGetVal(de);
        // 跳过没有连接对象的节点
        if (!node->link) continue;
        // 跳过自身myself节点和处于握手状态的节点
        if (node->flags & (CLUSTER_NODE_MYSELF|CLUSTER_NODE_HANDSHAKE))
            continue;
        // 将buf中的信息发送给节点连接的发送缓冲区中
        clusterSendMessage(node->link,buf,len);
    }
    dictReleaseIterator(di);
}

/* Build the message header. hdr must point to a buffer at least
 * sizeof(clusterMsg) in bytes. */
// 构建消息的头部，hdr至少指向一个sizeof(clusterMsg)大小的缓冲区
void clusterBuildMessageHdr(clusterMsg *hdr, int type) {
    int totlen = 0;
    uint64_t offset;
    clusterNode *master;

    /* If this node is a master, we send its slots bitmap and configEpoch.
     * If this node is a slave we send the master's information instead (the
     * node is flagged as slave so the receiver knows that it is NOT really
     * in charge for this slots. */
    // 如果当前节点是主节点，发送它的槽位图信息和配置纪元
    // 如果当前节点是从节点，发送它主节点的槽位图信息和配置纪元
    master = (nodeIsSlave(myself) && myself->slaveof) ?
              myself->slaveof : myself;

    memset(hdr,0,sizeof(*hdr));
    // 设置头部的签名
    hdr->ver = htons(CLUSTER_PROTO_VER);
    hdr->sig[0] = 'R';
    hdr->sig[1] = 'C';
    hdr->sig[2] = 'm';
    hdr->sig[3] = 'b';
    // 设置信息类型
    hdr->type = htons(type);
    // 设置当前负责槽的节点名称
    memcpy(hdr->sender,myself->name,CLUSTER_NAMELEN);
    // 设置当前节点负责的槽位图信息
    memcpy(hdr->myslots,master->slots,sizeof(hdr->myslots));
    // 清零从属的主节点信息
    memset(hdr->slaveof,0,CLUSTER_NAMELEN);
    // 如果myself是从节点，设置消息头部从属主节点的信息
    if (myself->slaveof != NULL)
        memcpy(hdr->slaveof,myself->slaveof->name, CLUSTER_NAMELEN);
    // 设置port
    hdr->port = htons(server.port);
    // 设置myself节点类型
    hdr->flags = htons(myself->flags);
    // 设置当前集群的状态
    hdr->state = server.cluster->state;

    /* Set the currentEpoch and configEpochs. */
    // 设置集群当前纪元和主节点配置纪元
    hdr->currentEpoch = htonu64(server.cluster->currentEpoch);
    hdr->configEpoch = htonu64(master->configEpoch);

    /* Set the replication offset. */
    // 如果myself是从节点
    if (nodeIsSlave(myself))
        // 获取复制偏移量
        offset = replicationGetSlaveOffset();
    else
        // myself是主节点，获取复制的偏移量
        offset = server.master_repl_offset;
    // 设置复制的偏移量
    hdr->offset = htonu64(offset);

    /* Set the message flags. */
    // 如果myself是主节点，正在进行手动故障转移
    if (nodeIsMaster(myself) && server.cluster->mf_end)
        // 设置主节点暂停手动故障转移的标识
        hdr->mflags[0] |= CLUSTERMSG_FLAG0_PAUSED;

    /* Compute the message length for certain messages. For other messages
     * this is up to the caller. */
    // 如果消息是 FAIL 类型的，计算消息的总长度
    if (type == CLUSTERMSG_TYPE_FAIL) {
        totlen = sizeof(clusterMsg)-sizeof(union clusterMsgData);
        totlen += sizeof(clusterMsgDataFail);
    // 如果消息是 UPDATE 类型的，计算消息的总长度
    } else if (type == CLUSTERMSG_TYPE_UPDATE) {
        totlen = sizeof(clusterMsg)-sizeof(union clusterMsgData);
        totlen += sizeof(clusterMsgDataUpdate);
    }
    // 设置消息的总长度
    hdr->totlen = htonl(totlen);
    /* For PING, PONG, and MEET, fixing the totlen field is up to the caller. */
}

/* Send a PING or PONG packet to the specified node, making sure to add enough
 * gossip informations. */
// 发送PING或PONG消息包给指定的节点
void clusterSendPing(clusterLink *link, int type) {
    unsigned char *buf;
    clusterMsg *hdr;
    int gossipcount = 0; /* Number of gossip sections added so far. */
    int wanted; /* Number of gossip sections we want to append if possible. */
    int totlen; /* Total packet length. */
    /* freshnodes is the max number of nodes we can hope to append at all:
     * nodes available minus two (ourself and the node we are sending the
     * message to). However practically there may be less valid nodes since
     * nodes in handshake state, disconnected, are not considered. */
    // freshnodes 的值是除了当前myself节点和发送消息的两个节点之外，集群中的所有节点
    // freshnodes 表示的意思是gossip协议中可以包含的有关节点信息的最大个数
    int freshnodes = dictSize(server.cluster->nodes)-2;

    /* How many gossip sections we want to add? 1/10 of the number of nodes
     * and anyway at least 3. Why 1/10?
     *
     * If we have N masters, with N/10 entries, and we consider that in
     * node_timeout we exchange with each other node at least 4 packets
     * (we ping in the worst case in node_timeout/2 time, and we also
     * receive two pings from the host), we have a total of 8 packets
     * in the node_timeout*2 falure reports validity time. So we have
     * that, for a single PFAIL node, we can expect to receive the following
     * number of failure reports (in the specified window of time):
     *
     * PROB * GOSSIP_ENTRIES_PER_PACKET * TOTAL_PACKETS:
     *
     * PROB = probability of being featured in a single gossip entry,
     *        which is 1 / NUM_OF_NODES.
     * ENTRIES = 10.
     * TOTAL_PACKETS = 2 * 4 * NUM_OF_MASTERS.
     *
     * If we assume we have just masters (so num of nodes and num of masters
     * is the same), with 1/10 we always get over the majority, and specifically
     * 80% of the number of nodes, to account for many masters failing at the
     * same time.
     *
     * Since we have non-voting slaves that lower the probability of an entry
     * to feature our node, we set the number of entires per packet as
     * 10% of the total nodes we have. */
    // 计算我们要附加的gossip节数
    // gossip部分的节点数应该是所有节点数的1/10，但是最少应该包含3个节点信息。之所以在gossip部分需要包含所有节点数的1/10，是为了能够在下线检测时间，也就是2倍的node_timeout时间内，如果有节点下线的话，能够收到大部分集群节点发来的，关于该节点的下线报告； 1/10这个数是这样来的：
    // 如果共有N个集群节点，在超时时间node_timeout内，当前节点最少会收到其他任一节点发来的4个心跳包：因节点最长经过node_timeout/2时间，就会其他节点发送一次PING包。节点收到PING包后，会回复PONG包。因此，在node_timeout时间内，当前节点会收到节点A发来的两个PING包，并且会收到节点A发来的，对于我发过去的PING包的回复包，也就是2个PONG包。因此，在下线监测时间node_timeout*2内，会收到其他任一集群节点发来的8个心跳包。因此，当前节点总共可以收到8*N个心跳包，每个心跳包中，包含下线节点信息的概率是1/10，因此，收到下线报告的期望值就是8*N*(1/10)，也就是N*80%，因此，这意味着可以收到大部分节点发来的下线报告。
    // wanted 的值是集群节点的十分之一向下取整，并且最小等于3
    // wanted 表示的意思是gossip中要包含的其他节点信息个数
    wanted = floor(dictSize(server.cluster->nodes)/10);
    if (wanted < 3) wanted = 3;
    // 因此 wanted 最多等于 freshnodes。
    if (wanted > freshnodes) wanted = freshnodes;

    /* Compute the maxium totlen to allocate our buffer. We'll fix the totlen
     * later according to the number of gossip sections we really were able
     * to put inside the packet. */
    // 计算分配消息的最大空间
    totlen = sizeof(clusterMsg)-sizeof(union clusterMsgData);
    totlen += (sizeof(clusterMsgDataGossip)*wanted);
    /* Note: clusterBuildMessageHdr() expects the buffer to be always at least
     * sizeof(clusterMsg) or more. */
    // 消息的总长最少为一个消息结构的大小
    if (totlen < (int)sizeof(clusterMsg)) totlen = sizeof(clusterMsg);
    // 分配空间
    buf = zcalloc(totlen);
    hdr = (clusterMsg*) buf;

    /* Populate the header. */
    // 设置发送PING命令的时间
    if (link->node && type == CLUSTERMSG_TYPE_PING)
        link->node->ping_sent = mstime();
    // 构建消息的头部
    clusterBuildMessageHdr(hdr,type);

    /* Populate the gossip fields */
    int maxiterations = wanted*3;
    // 构建消息内容
    while(freshnodes > 0 && gossipcount < wanted && maxiterations--) {
        // 随机选择一个集群节点
        dictEntry *de = dictGetRandomKey(server.cluster->nodes);
        clusterNode *this = dictGetVal(de);
        clusterMsgDataGossip *gossip;
        int j;

        /* Don't include this node: the whole packet header is about us
         * already, so we just gossip about other nodes. */
        // 1. 跳过当前节点，不选myself节点
        if (this == myself) continue;

        /* Give a bias to FAIL/PFAIL nodes. */
        // 2. 偏爱选择处于下线状态或疑似下线状态的节点
        if (maxiterations > wanted*2 &&
            !(this->flags & (CLUSTER_NODE_PFAIL|CLUSTER_NODE_FAIL)))
            continue;

        /* In the gossip section don't include:
         * 1) Nodes in HANDSHAKE state.
         * 3) Nodes with the NOADDR flag set.
         * 4) Disconnected nodes if they don't have configured slots.
         */
        // 以下节点不能作为被选中的节点：
        /*
            1. 处于握手状态的节点
            2. 带有NOADDR标识的节点
            3. 因为不处理任何槽而断开连接的节点
        */
        if (this->flags & (CLUSTER_NODE_HANDSHAKE|CLUSTER_NODE_NOADDR) ||
            (this->link == NULL && this->numslots == 0))
        {
            freshnodes--; /* Tecnically not correct, but saves CPU. */
            continue;
        }

        /* Check if we already added this node */
        // 如果已经在gossip的消息中添加过了当前节点，则退出循环
        for (j = 0; j < gossipcount; j++) {
            if (memcmp(hdr->data.ping.gossip[j].nodename,this->name,
                    CLUSTER_NAMELEN) == 0) break;
        }
        // j 一定 == gossipcount
        if (j != gossipcount) continue;

        /* Add it */
        // 这个节点满足条件，则将其添加到gossip消息中
        freshnodes--;
        // 指向添加该节点的那个空间
        gossip = &(hdr->data.ping.gossip[gossipcount]);
        // 添加名字
        memcpy(gossip->nodename,this->name,CLUSTER_NAMELEN);
        // 记录发送PING的时间
        gossip->ping_sent = htonl(this->ping_sent);
        // 接收到PING回复的时间
        gossip->pong_received = htonl(this->pong_received);
        // 设置该节点的IP和port
        memcpy(gossip->ip,this->ip,sizeof(this->ip));
        gossip->port = htons(this->port);
        // 记录标识
        gossip->flags = htons(this->flags);
        gossip->notused1 = 0;
        gossip->notused2 = 0;
        // 已经添加到gossip消息的节点数加1
        gossipcount++;
    }

    /* Ready to send... fix the totlen fiend and queue the message in the
     * output buffer. */
    // 计算消息的总长度
    totlen = sizeof(clusterMsg)-sizeof(union clusterMsgData);
    totlen += (sizeof(clusterMsgDataGossip)*gossipcount);
    // 记录消息节点的数量到包头
    hdr->count = htons(gossipcount);
    // 记录消息节点的总长到包头
    hdr->totlen = htonl(totlen);
    // 发送消息
    clusterSendMessage(link,buf,totlen);
    zfree(buf);
}

/* Send a PONG packet to every connected node that's not in handshake state
 * and for which we have a valid link.
 *
 * In Redis Cluster pongs are not used just for failure detection, but also
 * to carry important configuration information. So broadcasting a pong is
 * useful when something changes in the configuration and we want to make
 * the cluster aware ASAP (for instance after a slave promotion).
 *
 * The 'target' argument specifies the receiving instances using the
 * defines below:
 *
 * CLUSTER_BROADCAST_ALL -> All known instances.
 * CLUSTER_BROADCAST_LOCAL_SLAVES -> All slaves in my master-slaves ring.
 */
// 发送一个PONG消息包给所有已连接不处于握手状态的的节点
// 在Redis集群中，PONG不仅用来故障检测，还可以携带一些重要的配置信息。所以当配置发生一些改变时，希望使所有集群节点的能感知到变化，因此广播一个PONG是非常有用的（例如，从节点晋升）
// target 参数使用以下宏来指定接收的实例：
// CLUSTER_BROADCAST_ALL -> 集群中所有实例
// CLUSTER_BROADCAST_LOCAL_SLAVES  -> 所有主节点的从节点
#define CLUSTER_BROADCAST_ALL 0
#define CLUSTER_BROADCAST_LOCAL_SLAVES 1
void clusterBroadcastPong(int target) {
    dictIterator *di;
    dictEntry *de;

    di = dictGetSafeIterator(server.cluster->nodes);
    // 遍历所有的节点
    while((de = dictNext(di)) != NULL) {
        clusterNode *node = dictGetVal(de);
        // 跳过没有连接对象的节点
        if (!node->link) continue;
        // 跳过myself节点和处于握手状态的节点
        if (node == myself || nodeInHandshake(node)) continue;
        // 如果指定给所有主节点的从节点的发送PONG的标识
        if (target == CLUSTER_BROADCAST_LOCAL_SLAVES) {
            // 判断当前节点是否是从节点
            int local_slave =
                nodeIsSlave(node) && node->slaveof &&
                (node->slaveof == myself || node->slaveof == myself->slaveof);
            // 如果不是从节点，跳过发送
            if (!local_slave) continue;
        }
        // 发送一个PONG回复
        clusterSendPing(node->link,CLUSTERMSG_TYPE_PONG);
    }
    dictReleaseIterator(di);
}

/* Send a PUBLISH message.
 *
 * If link is NULL, then the message is broadcasted to the whole cluster. */
// 发送一个PUBLISH消息，如果没有连接对象，消息广播给整个集群
void clusterSendPublish(clusterLink *link, robj *channel, robj *message) {
    unsigned char buf[sizeof(clusterMsg)], *payload;
    clusterMsg *hdr = (clusterMsg*) buf;
    uint32_t totlen;
    uint32_t channel_len, message_len;
    // 解码频道对象和消息对象
    channel = getDecodedObject(channel);
    message = getDecodedObject(message);
    // 获取频道名长度和消息长度
    channel_len = sdslen(channel->ptr);
    message_len = sdslen(message->ptr);
    // 建立一个PUBLISH消息包的包头
    clusterBuildMessageHdr(hdr,CLUSTERMSG_TYPE_PUBLISH);
    // 计算PUBLISH消息包的总大小
    totlen = sizeof(clusterMsg)-sizeof(union clusterMsgData);
    totlen += sizeof(clusterMsgDataPublish) - 8 + channel_len + message_len;
    // 设置消息包的属性
    hdr->data.publish.msg.channel_len = htonl(channel_len);
    hdr->data.publish.msg.message_len = htonl(message_len);
    hdr->totlen = htonl(totlen);

    /* Try to use the local buffer if possible */
    // 尝试使用本地缓存区，如果消息总大小小于buf的大小，可以直接使用
    if (totlen < sizeof(buf)) {
        payload = buf;
    // 需要扩充本地缓存区的大小
    } else {
        payload = zmalloc(totlen);
        memcpy(payload,hdr,sizeof(*hdr));
        hdr = (clusterMsg*) payload;
    }
    // 将所有的消息包中的数据复制到本地缓存区的指定位置
    memcpy(hdr->data.publish.msg.bulk_data,channel->ptr,sdslen(channel->ptr));
    memcpy(hdr->data.publish.msg.bulk_data+sdslen(channel->ptr),
        message->ptr,sdslen(message->ptr));

    // 如果连接大小存在
    if (link)
        // 发送给连接的大小
        clusterSendMessage(link,payload,totlen);
    else
        // 否则，广播给所有的集群节点
        clusterBroadcastMessage(payload,totlen);

    decrRefCount(channel);
    decrRefCount(message);
    if (payload != buf) zfree(payload);
}

/* Send a FAIL message to all the nodes we are able to contact.
 * The FAIL message is sent when we detect that a node is failing
 * (CLUSTER_NODE_PFAIL) and we also receive a gossip confirmation of this:
 * we switch the node state to CLUSTER_NODE_FAIL and ask all the other
 * nodes to do the same ASAP. */
// 发送一个FAIL消息给所有可达的节点。当察觉到一个节点处于PFAIL状态，发送一个FAIL消息。并且接收一个这么的gossip信息：我们要将该节点的状态设置为FAIL的，要求所有的节点也这么做。
void clusterSendFail(char *nodename) {
    unsigned char buf[sizeof(clusterMsg)];
    clusterMsg *hdr = (clusterMsg*) buf;
    // 构建FAIL的消息包包头
    clusterBuildMessageHdr(hdr,CLUSTERMSG_TYPE_FAIL);
    // 设置下线节点的名字
    memcpy(hdr->data.fail.about.nodename,nodename,CLUSTER_NAMELEN);
    // 发送给所有集群中的节点
    clusterBroadcastMessage(buf,ntohl(hdr->totlen));
}

/* Send an UPDATE message to the specified link carrying the specified 'node'
 * slots configuration. The node name, slots bitmap, and configEpoch info
 * are included. */
// 发送一个UPDATE消息包给指定的连接，并携带指定节点的槽位图信息，节点名字，和配置纪元信息
void clusterSendUpdate(clusterLink *link, clusterNode *node) {
    unsigned char buf[sizeof(clusterMsg)];
    clusterMsg *hdr = (clusterMsg*) buf;
    // 连接对象为空直接返回
    if (link == NULL) return;
    // 构建一个UPDATE的消息包包头
    clusterBuildMessageHdr(hdr,CLUSTERMSG_TYPE_UPDATE);
    // 设置UPDATE消息包的节点名
    memcpy(hdr->data.update.nodecfg.nodename,node->name,CLUSTER_NAMELEN);
    // 设置UPDATE消息包的配置纪元
    hdr->data.update.nodecfg.configEpoch = htonu64(node->configEpoch);
    // 设置UPDATE消息包的槽位图信息
    memcpy(hdr->data.update.nodecfg.slots,node->slots,sizeof(node->slots));
    // 发送UPDATE消息包给连接对象
    clusterSendMessage(link,buf,ntohl(hdr->totlen));
}

/* -----------------------------------------------------------------------------
 * CLUSTER Pub/Sub support
 *
 * For now we do very little, just propagating PUBLISH messages across the whole
 * cluster. In the future we'll try to get smarter and avoiding propagating those
 * messages to hosts without receives for a given channel.
 * -------------------------------------------------------------------------- */
// 向整个集群的指定频道中广播message消息
void clusterPropagatePublish(robj *channel, robj *message) {
    clusterSendPublish(NULL, channel, message);
}

/* -----------------------------------------------------------------------------
 * SLAVE node specific functions
 * -------------------------------------------------------------------------- */

/* This function sends a FAILOVE_AUTH_REQUEST message to every node in order to
 * see if there is the quorum for this slave instance to failover its failing
 * master.
 *
 * Note that we send the failover request to everybody, master and slave nodes,
 * but only the masters are supposed to reply to our query. */
// 发送一个FAILOVE_AUTH_REQUEST消息给所有的节点，判断这些节点是否同意该从节点为它的主节点执行故障转移操作
void clusterRequestFailoverAuth(void) {
    unsigned char buf[sizeof(clusterMsg)];
    clusterMsg *hdr = (clusterMsg*) buf;
    uint32_t totlen;
    // 建立REQUEST消息包包头
    clusterBuildMessageHdr(hdr,CLUSTERMSG_TYPE_FAILOVER_AUTH_REQUEST);
    /* If this is a manual failover, set the CLUSTERMSG_FLAG0_FORCEACK bit
     * in the header to communicate the nodes receiving the message that
     * they should authorized the failover even if the master is working. */
    // 如果是一个手动的故障转移，设置CLUSTERMSG_FLAG0_FORCEACK，表示即使主节点在线，也要认证故障转移
    if (server.cluster->mf_end) hdr->mflags[0] |= CLUSTERMSG_FLAG0_FORCEACK;
    // 计算REQUEST消息包的长度
    totlen = sizeof(clusterMsg)-sizeof(union clusterMsgData);
    hdr->totlen = htonl(totlen);
    // 广播这个消息包
    clusterBroadcastMessage(buf,totlen);
}

/* Send a FAILOVER_AUTH_ACK message to the specified node. */
// 发送一个FAILOVER_AUTH_ACK消息给指定的节点，表示支持它进行故障转移
void clusterSendFailoverAuth(clusterNode *node) {
    unsigned char buf[sizeof(clusterMsg)];
    clusterMsg *hdr = (clusterMsg*) buf;
    uint32_t totlen;

    if (!node->link) return;
    // 构建一个FAILOVER_AUTH_ACK消息包包头
    clusterBuildMessageHdr(hdr,CLUSTERMSG_TYPE_FAILOVER_AUTH_ACK);
    // 计算消息包长度
    totlen = sizeof(clusterMsg)-sizeof(union clusterMsgData);
    hdr->totlen = htonl(totlen);
    // 发送消息包
    clusterSendMessage(node->link,buf,totlen);
}

/* Send a MFSTART message to the specified node. */
// 发送一个MFSTART消息个指定节点
void clusterSendMFStart(clusterNode *node) {
    unsigned char buf[sizeof(clusterMsg)];
    clusterMsg *hdr = (clusterMsg*) buf;
    uint32_t totlen;

    if (!node->link) return;
    // 构建一个MFSTART消息包包头
    clusterBuildMessageHdr(hdr,CLUSTERMSG_TYPE_MFSTART);
    // 计算消息包长度
    totlen = sizeof(clusterMsg)-sizeof(union clusterMsgData);
    hdr->totlen = htonl(totlen);
    // 发送消息包
    clusterSendMessage(node->link,buf,totlen);
}

/* Vote for the node asking for our vote if there are the conditions. */
// 在满足条件的情况下，为请求执行故障转移的节点node进行投票
void clusterSendFailoverAuthIfNeeded(clusterNode *node, clusterMsg *request) {
    // 获取该请求从节点的主节点
    clusterNode *master = node->slaveof;
    // 获取请求的当前纪元和配置纪元
    uint64_t requestCurrentEpoch = ntohu64(request->currentEpoch);
    uint64_t requestConfigEpoch = ntohu64(request->configEpoch);
    // 获取该请求从节点的槽位图信息
    unsigned char *claimed_slots = request->myslots;
    // 是否指定强制认证故障转移的标识
    int force_ack = request->mflags[0] & CLUSTERMSG_FLAG0_FORCEACK;
    int j;

    /* IF we are not a master serving at least 1 slot, we don't have the
     * right to vote, as the cluster size in Redis Cluster is the number
     * of masters serving at least one slot, and quorum is the cluster
     * size + 1 */
    // 如果myself是从节点，或者myself没有负责的槽信息，那么myself节点没有投票权，直接返回
    if (nodeIsSlave(myself) || myself->numslots == 0) return;

    /* Request epoch must be >= our currentEpoch.
     * Note that it is impossible for it to actually be greater since
     * our currentEpoch was updated as a side effect of receiving this
     * request, if the request epoch was greater. */
    // 如果请求的当前纪元小于集群的当前纪元，直接返回。该节点有可能是长时间下线后重新上线，导致版本落后于就集群的版本
    // 因为该请求节点的版本小于集群的版本，每次有选举或投票都会更新每个节点的版本，使节点状态和集群的状态是一致的。
    if (requestCurrentEpoch < server.cluster->currentEpoch) {
        serverLog(LL_WARNING,
            "Failover auth denied to %.40s: reqEpoch (%llu) < curEpoch(%llu)",
            node->name,
            (unsigned long long) requestCurrentEpoch,
            (unsigned long long) server.cluster->currentEpoch);
        return;
    }

    /* I already voted for this epoch? Return ASAP. */
    // 如果最近一次投票的纪元和当前纪元相同，表示集群已经投过票了
    if (server.cluster->lastVoteEpoch == server.cluster->currentEpoch) {
        serverLog(LL_WARNING,
                "Failover auth denied to %.40s: already voted for epoch %llu",
                node->name,
                (unsigned long long) server.cluster->currentEpoch);
        return;
    }

    /* Node must be a slave and its master down.
     * The master can be non failing if the request is flagged
     * with CLUSTERMSG_FLAG0_FORCEACK (manual failover). */
    // 指定的node节点必须为从节点且它的主节点处于下线状态，否则打印日志后返回
    if (nodeIsMaster(node) || master == NULL ||
        (!nodeFailed(master) && !force_ack))
    {
        // 故障转移的请求必须由从节点发起
        if (nodeIsMaster(node)) {
            serverLog(LL_WARNING,
                    "Failover auth denied to %.40s: it is a master node",
                    node->name);
        // 从节点找不到他的主节点
        } else if (master == NULL) {
            serverLog(LL_WARNING,
                    "Failover auth denied to %.40s: I don't know its master",
                    node->name);
        // 从节点的主节点没有处于下线状态
        } else if (!nodeFailed(master)) {
            serverLog(LL_WARNING,
                    "Failover auth denied to %.40s: its master is up",
                    node->name);
        }
        return;
    }

    /* We did not voted for a slave about this master for two
     * times the node timeout. This is not strictly needed for correctness
     * of the algorithm but makes the base case more linear. */
    // 在cluster_node_timeout * 2时间内只能投1次票
    if (mstime() - node->slaveof->voted_time < server.cluster_node_timeout * 2)
    {
        serverLog(LL_WARNING,
                "Failover auth denied to %.40s: "
                "can't vote about this master before %lld milliseconds",
                node->name,
                (long long) ((server.cluster_node_timeout*2)-
                             (mstime() - node->slaveof->voted_time)));
        return;
    }

    /* The slave requesting the vote must have a configEpoch for the claimed
     * slots that is >= the one of the masters currently serving the same
     * slots in the current configuration. */
    // 请求投票的从节点必须有一个声明负责槽位的配置纪元，这些配置纪元必须比负责相同槽位的主节点的配置纪元要大
    for (j = 0; j < CLUSTER_SLOTS; j++) {
        // 跳过没有指定的槽位
        if (bitmapTestBit(claimed_slots, j) == 0) continue;
        // 如果请求从节点的配置纪元大于槽的配置纪元，则跳过
        if (server.cluster->slots[j] == NULL ||
            server.cluster->slots[j]->configEpoch <= requestConfigEpoch)
        {
            continue;
        }
        /* If we reached this point we found a slot that in our current slots
         * is served by a master with a greater configEpoch than the one claimed
         * by the slave requesting our vote. Refuse to vote for this slave. */
        // 如果请求从节点的配置纪元小于槽的配置纪元，那么表示该从节点的配置纪元已经过期，不能给该从节点投票，直接返回
        serverLog(LL_WARNING,
                "Failover auth denied to %.40s: "
                "slot %d epoch (%llu) > reqEpoch (%llu)",
                node->name, j,
                (unsigned long long) server.cluster->slots[j]->configEpoch,
                (unsigned long long) requestConfigEpoch);
        return;
    }

    /* We can vote for this slave. */
    // 发送一个FAILOVER_AUTH_ACK消息给指定的节点，表示支持该从节点进行故障转移
    clusterSendFailoverAuth(node);
    // 设置最近一次投票的纪元，防止给多个节点投多次票
    server.cluster->lastVoteEpoch = server.cluster->currentEpoch;
    // 设置最近一次投票的时间
    node->slaveof->voted_time = mstime();
    serverLog(LL_WARNING, "Failover auth granted to %.40s for epoch %llu",
        node->name, (unsigned long long) server.cluster->currentEpoch);
}

/* This function returns the "rank" of this instance, a slave, in the context
 * of its master-slaves ring. The rank of the slave is given by the number of
 * other slaves for the same master that have a better replication offset
 * compared to the local one (better means, greater, so they claim more data).
 *
 * A slave with rank 0 is the one with the greatest (most up to date)
 * replication offset, and so forth. Note that because how the rank is computed
 * multiple slaves may have the same rank, in case they have the same offset.
 *
 * The slave rank is used to add a delay to start an election in order to
 * get voted and replace a failing master. Slaves with better replication
 * offsets are more likely to win. */
// 该函数返回一个实例的排位，rank表示一个从节点在下线的主节点的所有从节点中的排名，排名主要是根据复制偏移量来定。
// 复制数据量越大，排名越靠前，因此，具有更多的复制量的的从节点优先发起故障转移流程，从而成为新的主节点
int clusterGetSlaveRank(void) {
    long long myoffset;
    int j, rank = 0;
    clusterNode *master;
    // myself节点必须为从节点
    serverAssert(nodeIsSlave(myself));
    // 获取myself从节点的主节点
    master = myself->slaveof;
    if (master == NULL) return 0; /* Never called by slaves without master. */
    // 返回从节点复制偏移量
    myoffset = replicationGetSlaveOffset();
    // 遍历所有的从节点
    for (j = 0; j < master->numslaves; j++)
        // 如果有节点的复制偏移量大于myself节点偏移量，则那么增加它的排名
        if (master->slaves[j] != myself &&
            master->slaves[j]->repl_offset > myoffset) rank++;
    return rank;
}

/* This function is called by clusterHandleSlaveFailover() in order to
 * let the slave log why it is not able to failover. Sometimes there are
 * not the conditions, but since the failover function is called again and
 * again, we can't log the same things continuously.
 *
 * This function works by logging only if a given set of conditions are
 * true:
 *
 * 1) The reason for which the failover can't be initiated changed.
 *    The reasons also include a NONE reason we reset the state to
 *    when the slave finds that its master is fine (no FAIL flag).
 * 2) Also, the log is emitted again if the master is still down and
 *    the reason for not failing over is still the same, but more than
 *    CLUSTER_CANT_FAILOVER_RELOG_PERIOD seconds elapsed.
 * 3) Finally, the function only logs if the slave is down for more than
 *    five seconds + NODE_TIMEOUT. This way nothing is logged when a
 *    failover starts in a reasonable time.
 *
 * The function is called with the reason why the slave can't failover
 * which is one of the integer macros CLUSTER_CANT_FAILOVER_*.
 *
 * The function is guaranteed to be called only if 'myself' is a slave. */
// 该函数被clusterHandleSlaveFailover()调用，根据指定的reason打印从节点不能执行故障转移的原因
void clusterLogCantFailover(int reason) {
    char *msg;
    static time_t lastlog_time = 0;
    mstime_t nolog_fail_time = server.cluster_node_timeout + 5000;

    /* Don't log if we have the same reason for some time. */
    if (reason == server.cluster->cant_failover_reason &&
        time(NULL)-lastlog_time < CLUSTER_CANT_FAILOVER_RELOG_PERIOD)
        return;

    server.cluster->cant_failover_reason = reason;

    /* We also don't emit any log if the master failed no long ago, the
     * goal of this function is to log slaves in a stalled condition for
     * a long time. */
    if (myself->slaveof &&
        nodeFailed(myself->slaveof) &&
        (mstime() - myself->slaveof->fail_time) < nolog_fail_time) return;

    switch(reason) {
    case CLUSTER_CANT_FAILOVER_DATA_AGE:
        msg = "Disconnected from master for longer than allowed. "
              "Please check the 'cluster-slave-validity-factor' configuration "
              "option.";
        break;
    case CLUSTER_CANT_FAILOVER_WAITING_DELAY:
        msg = "Waiting the delay before I can start a new failover.";
        break;
    case CLUSTER_CANT_FAILOVER_EXPIRED:
        msg = "Failover attempt expired.";
        break;
    case CLUSTER_CANT_FAILOVER_WAITING_VOTES:
        msg = "Waiting for votes, but majority still not reached.";
        break;
    default:
        msg = "Unknown reason code.";
        break;
    }
    lastlog_time = time(NULL);
    serverLog(LL_WARNING,"Currently unable to failover: %s", msg);
}

/* This function implements the final part of automatic and manual failovers,
 * where the slave grabs its master's hash slots, and propagates the new
 * configuration.
 *
 * Note that it's up to the caller to be sure that the node got a new
 * configuration epoch already. */
// 该函数实现自动和手动故障转移的最后一部分，从节点获取其主节点的哈希槽，并传播新配置
void clusterFailoverReplaceYourMaster(void) {
    int j;
    // 获取myself的主节点
    clusterNode *oldmaster = myself->slaveof;

    // 如果myself节点是主节点，直接返回
    if (nodeIsMaster(myself) || oldmaster == NULL) return;

    /* 1) Turn this node into a master. */
    // 将指定的myself节点重新配置为主节点
    clusterSetNodeAsMaster(myself);
    // 取消复制操作，设置myself为主节点
    replicationUnsetMaster();

    /* 2) Claim all the slots assigned to our master. */
    // 将所有之前主节点声明负责的槽位指定给现在的主节点myself节点。
    for (j = 0; j < CLUSTER_SLOTS; j++) {
        // 如果当前槽已经指定
        if (clusterNodeGetSlotBit(oldmaster,j)) {
            // 将该槽设置为未分配的
            clusterDelSlot(j);
            // 将该槽指定给myself节点
            clusterAddSlot(myself,j);
        }
    }

    /* 3) Update state and save config. */
    // 更新节点状态
    clusterUpdateState();
    // 写配置文件
    clusterSaveConfigOrDie(1);

    /* 4) Pong all the other nodes so that they can update the state
     *    accordingly and detect that we switched to master role. */
    // 发送一个PONG消息包给所有已连接不处于握手状态的的节点
    // 以便能够其他节点更新状态
    clusterBroadcastPong(CLUSTER_BROADCAST_ALL);

    /* 5) If there was a manual failover in progress, clear the state. */
    // 重置与手动故障转移的状态
    resetManualFailover();
}

/* This function is called if we are a slave node and our master serving
 * a non-zero amount of hash slots is in FAIL state.
 *
 * The gaol of this function is:
 * 1) To check if we are able to perform a failover, is our data updated?
 * 2) Try to get elected by masters.
 * 3) Perform the failover informing all the other nodes.
 */
// 如果当前节点是一个从节点，并且它负责有槽的主节点处于FAIL状态，调用该函数
// 这个函数的目标是：
/*
    1. 检查是否可以对主节点执行一次故障转移，节点的关于主节点的信息是否需要更新
    2. 尝试选举一个主节点
    3. 执行故障转移并通知所有的其他节点
*/
void clusterHandleSlaveFailover(void) {
    mstime_t data_age;
    // 计算上次选举所过去的时间
    mstime_t auth_age = mstime() - server.cluster->failover_auth_time;
    // 计算胜选需要的票数
    int needed_quorum = (server.cluster->size / 2) + 1;
    // 手动故障转移的标志
    int manual_failover = server.cluster->mf_end != 0 &&
                          server.cluster->mf_can_start;
    mstime_t auth_timeout, auth_retry_time;

    server.cluster->todo_before_sleep &= ~CLUSTER_TODO_HANDLE_FAILOVER;

    /* Compute the failover timeout (the max time we have to send votes
     * and wait for replies), and the failover retry time (the time to wait
     * before trying to get voted again).
     *
     * Timeout is MIN(NODE_TIMEOUT*2,2000) milliseconds.
     * Retry is two times the Timeout.
     */
    // 计算故障转移超时时间
    auth_timeout = server.cluster_node_timeout*2;
    if (auth_timeout < 2000) auth_timeout = 2000;
    // 重试的超时时间
    auth_retry_time = auth_timeout*2;

    /* Pre conditions to run the function, that must be met both in case
     * of an automatic or manual failover:
     * 1) We are a slave.
     * 2) Our master is flagged as FAIL, or this is a manual failover.
     * 3) It is serving slots. */
    // 运行函数的前提条件，在自动或手动故障转移的情况下都必须满足：
    /*
        1. 当前节点是从节点
        2. 该从节点的主节点被标记为FAIL状态，或者是一个手动故障转移状态
        3. 当前从节点有负责的槽位
    */
    // 如果不能满足以上条件，则直接返回
    if (nodeIsMaster(myself) ||
        myself->slaveof == NULL ||
        (!nodeFailed(myself->slaveof) && !manual_failover) ||
        myself->slaveof->numslots == 0)
    {
        /* There are no reasons to failover, so we set the reason why we
         * are returning without failing over to NONE. */
        // 设置故障转移失败的原因：CLUSTER_CANT_FAILOVER_NONE
        server.cluster->cant_failover_reason = CLUSTER_CANT_FAILOVER_NONE;
        return;
    }

    /* Set data_age to the number of seconds we are disconnected from
     * the master. */
    // 如果当前节点正在和主节点保持连接状态，计算从节点和主节点断开的时间
    if (server.repl_state == REPL_STATE_CONNECTED) {
        data_age = (mstime_t)(server.unixtime - server.master->lastinteraction)
                   * 1000;
    } else {
        data_age = (mstime_t)(server.unixtime - server.repl_down_since) * 1000;
    }

    /* Remove the node timeout from the data age as it is fine that we are
     * disconnected from our master at least for the time it was down to be
     * flagged as FAIL, that's the baseline. */
    // 从data_age删除一个cluster_node_timeout的时长，因为至少以从节点和主节点断开连接开始，因为超时的时间不算在内
    if (data_age > server.cluster_node_timeout)
        data_age -= server.cluster_node_timeout;

    /* Check if our data is recent enough according to the slave validity
     * factor configured by the user.
     *
     * Check bypassed for manual failovers. */
    // 检查这个从节点的数据是否比较新
    if (server.cluster_slave_validity_factor &&
        data_age >
        (((mstime_t)server.repl_ping_slave_period * 1000) +
         (server.cluster_node_timeout * server.cluster_slave_validity_factor)))
    {
        if (!manual_failover) {
            clusterLogCantFailover(CLUSTER_CANT_FAILOVER_DATA_AGE);
            return;
        }
    }

    /* If the previous failover attempt timedout and the retry time has
     * elapsed, we can setup a new one. */
    // 如果先前的尝试故障转移超时并且重试时间已过，我们可以设置一个新的。
    if (auth_age > auth_retry_time) {
        // 设置新的故障转移属性
        server.cluster->failover_auth_time = mstime() +
            500 + /* Fixed delay of 500 milliseconds, let FAIL msg propagate. */
            random() % 500; /* Random delay between 0 and 500 milliseconds. */
        server.cluster->failover_auth_count = 0;
        server.cluster->failover_auth_sent = 0;
        server.cluster->failover_auth_rank = clusterGetSlaveRank();
        /* We add another delay that is proportional to the slave rank.
         * Specifically 1 second * rank. This way slaves that have a probably
         * less updated replication offset, are penalized. */
        server.cluster->failover_auth_time +=
            server.cluster->failover_auth_rank * 1000;
        /* However if this is a manual failover, no delay is needed. */
        // 手动故障转移的情况
        if (server.cluster->mf_end) {
            server.cluster->failover_auth_time = mstime();
            server.cluster->failover_auth_rank = 0;
        }
        serverLog(LL_WARNING,
            "Start of election delayed for %lld milliseconds "
            "(rank #%d, offset %lld).",
            server.cluster->failover_auth_time - mstime(),
            server.cluster->failover_auth_rank,
            replicationGetSlaveOffset());
        /* Now that we have a scheduled election, broadcast our offset
         * to all the other slaves so that they'll updated their offsets
         * if our offset is better. */
        // 发送一个PONG消息包给所有的从节点，携带有当前的复制偏移量
        clusterBroadcastPong(CLUSTER_BROADCAST_LOCAL_SLAVES);
        return;
    }

    /* It is possible that we received more updated offsets from other
     * slaves for the same master since we computed our election delay.
     * Update the delay if our rank changed.
     *
     * Not performed if this is a manual failover. */
    // 如果没有开始故障转移，则调用clusterGetSlaveRank()获取当前从节点的最新排名。因为在故障转移之前可能会收到其他节点发送来的心跳包，因而可以根据心跳包的复制偏移量更新本节点的排名，获得新排名newrank，如果newrank比之前的排名靠后，则需要增加故障转移开始时间的延迟，然后将newrank记录到server.cluster->failover_auth_rank中
    if (server.cluster->failover_auth_sent == 0 &&
        server.cluster->mf_end == 0)
    {
        // 获取新排名
        int newrank = clusterGetSlaveRank();
        // 新排名比之前的靠后
        if (newrank > server.cluster->failover_auth_rank) {
            // 计算延迟故障转移时间
            long long added_delay =
                (newrank - server.cluster->failover_auth_rank) * 1000;
            // 更新下一次故障转移的时间和排名
            server.cluster->failover_auth_time += added_delay;
            server.cluster->failover_auth_rank = newrank;
            serverLog(LL_WARNING,
                "Slave rank updated to #%d, added %lld milliseconds of delay.",
                newrank, added_delay);
        }
    }

    /* Return ASAP if we can't still start the election. */
    // 如果还没有到故障转移选举的时间，直接返回
    if (mstime() < server.cluster->failover_auth_time) {
        clusterLogCantFailover(CLUSTER_CANT_FAILOVER_WAITING_DELAY);
        return;
    }

    /* Return ASAP if the election is too old to be valid. */
    // 如果距离故障转移的时间过了很久，那么不在执行故障转移，直接返回
    if (auth_age > auth_timeout) {
        // 故障转移过期
        clusterLogCantFailover(CLUSTER_CANT_FAILOVER_EXPIRED);
        return;
    }

    /* Ask for votes if needed. */
    // 如果没有向其他节点发送投票请求
    if (server.cluster->failover_auth_sent == 0) {
        // 增加当前纪元
        server.cluster->currentEpoch++;
        // 设置发其故障转移的纪元
        server.cluster->failover_auth_epoch = server.cluster->currentEpoch;
        serverLog(LL_WARNING,"Starting a failover election for epoch %llu.",
            (unsigned long long) server.cluster->currentEpoch);
        // 发送一个FAILOVE_AUTH_REQUEST消息给所有的节点，判断这些节点是否同意该从节点为它的主节点执行故障转移操作
        clusterRequestFailoverAuth();
        // 设置为真，表示本节点已经向其他节点发送了投票请求
        server.cluster->failover_auth_sent = 1;
        // 进入下一个事件循环执行的操作，保存配置文件，更新节点状态，同步配置
        clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG|
                             CLUSTER_TODO_UPDATE_STATE|
                             CLUSTER_TODO_FSYNC_CONFIG);
        return; /* Wait for replies. */
    }

    /* Check if we reached the quorum. */
    // 如果获得的票数到达quorum，那么对下线的主节点执行故障转移
    if (server.cluster->failover_auth_count >= needed_quorum) {
        /* We have the quorum, we can finally failover the master. */

        serverLog(LL_WARNING,
            "Failover election won: I'm the new master.");

        /* Update my configEpoch to the epoch of the election. */
        if (myself->configEpoch < server.cluster->failover_auth_epoch) {
            myself->configEpoch = server.cluster->failover_auth_epoch;
            serverLog(LL_WARNING,
                "configEpoch set to %llu after successful failover",
                (unsigned long long) myself->configEpoch);
        }

        /* Take responsability for the cluster slots. */
        // 执行自动或手动故障转移，从节点获取其主节点的哈希槽，并传播新配置
        clusterFailoverReplaceYourMaster();
    } else {
        clusterLogCantFailover(CLUSTER_CANT_FAILOVER_WAITING_VOTES);
    }
}

/* -----------------------------------------------------------------------------
 * CLUSTER slave migration
 *
 * Slave migration is the process that allows a slave of a master that is
 * already covered by at least another slave, to "migrate" to a master that
 * is orpaned, that is, left with no working slaves.
 * ------------------------------------------------------------------------- */

/* This function is responsible to decide if this replica should be migrated
 * to a different (orphaned) master. It is called by the clusterCron() function
 * only if:
 *
 * 1) We are a slave node.
 * 2) It was detected that there is at least one orphaned master in
 *    the cluster.
 * 3) We are a slave of one of the masters with the greatest number of
 *    slaves.
 *
 * This checks are performed by the caller since it requires to iterate
 * the nodes anyway, so we spend time into clusterHandleSlaveMigration()
 * if definitely needed.
 *
 * The fuction is called with a pre-computed max_slaves, that is the max
 * number of working (not in FAIL state) slaves for a single master.
 *
 * Additional conditions for migration are examined inside the function.
 */
// 此函数负责决定是否将一个从节点迁移到其他的孤立的主节点，使该从节点成为孤立主节点的从节点
// 只有在clusterCron()函数调用它：
/*
    1. myself节点是从节点
    2. 检测到集群中至少有一个孤立的主节点
    3. 我们是从节点个数最多的从节点之一
*/
// 这个检查是由调用者执行的，因为它需要迭代节点，所以如果绝对需要，我们花费时间进入clusterHandleSlaveMigration()。
// 使用传入的max_slaves调用该函数，即单个主节点的最大工作的从节点数（不在FAIL状态）
// 在函数内检查迁移的附加条件。
void clusterHandleSlaveMigration(int max_slaves) {
    int j, okslaves = 0;
    clusterNode *mymaster = myself->slaveof, *target = NULL, *candidate = NULL;
    dictIterator *di;
    dictEntry *de;

    /* Step 1: Don't migrate if the cluster state is not ok. */
    // 如果集群状态不OK，直接返回
    if (server.cluster->state != CLUSTER_OK) return;

    /* Step 2: Don't migrate if my master will not be left with at least
     *         'migration-barrier' slaves after my migration. */
    // 如果主节点在迁移后至少不会有“migration-barrier”个从节点，不迁移，直接返回
    if (mymaster == NULL) return;
    // 遍历所有的从节点
    for (j = 0; j < mymaster->numslaves; j++)
        // 如果从节点不处于FAIL状态且不处于超时状态（疑似下线）
        if (!nodeFailed(mymaster->slaves[j]) &&
            !nodeTimedOut(mymaster->slaves[j])) okslaves++; // 计数ok的从节点个数
    // 如果ok的从节点太少，直接返回
    if (okslaves <= server.cluster_migration_barrier) return;

    /* Step 3: Idenitfy a candidate for migration, and check if among the
     * masters with the greatest number of ok slaves, I'm the one with the
     * smallest node ID (the "candidate slave").
     *
     * Note: this means that eventually a replica migration will occurr
     * since slaves that are reachable again always have their FAIL flag
     * cleared, so eventually there must be a candidate. At the same time
     * this does not mean that there are no race conditions possible (two
     * slaves migrating at the same time), but this is unlikely to
     * happen, and harmless when happens. */
    // 确定迁移的候选人，并检查是否有最多数量的ok从节点的主节点之一，当前节点是具有最小节点ID(“候选从节点”)的主节点，
    candidate = myself;
    di = dictGetSafeIterator(server.cluster->nodes);
    // 遍历所有的集群节点
    while((de = dictNext(di)) != NULL) {
        clusterNode *node = dictGetVal(de);
        int okslaves = 0, is_orphaned = 1;

        /* We want to migrate only if this master is working, orphaned, and
         * used to have slaves or if failed over a master that had slaves
         * (MIGRATE_TO flag). This way we only migrate to instances that were
         * supposed to have replicas. */
        // 只在主节点正常工作或孤立状态并且曾经有从节点或者如果失败超过拥有从节点的主节点（MIGRATE_TO标志）时才迁移

        // 如果node节点是从节点或这处于FAIL状态，取消is_orphaned标识
        if (nodeIsSlave(node) || nodeFailed(node)) is_orphaned = 0;
        // 当前节点不处于迁移数据的状态，取消is_orphaned标识
        if (!(node->flags & CLUSTER_NODE_MIGRATE_TO)) is_orphaned = 0;

        /* Check number of working slaves. */
        // 检查节点所拥有的正常工作的从节点的个数
        if (nodeIsMaster(node)) okslaves = clusterCountNonFailingSlaves(node);
        // 正常工作的从节点的个数大于0，取消is_orphaned标识
        if (okslaves > 0) is_orphaned = 0;

        // 如果是孤立的节点，选择目标节点，更新迁移时间
        if (is_orphaned) {
            if (!target && node->numslots > 0) target = node;

            /* Track the starting time of the orphaned condition for this
             * master. */
            if (!node->orphaned_time) node->orphaned_time = mstime();
        } else {
            node->orphaned_time = 0;
        }

        /* Check if I'm the slave candidate for the migration: attached
         * to a master with the maximum number of slaves and with the smallest
         * node ID. */
        // 检查myself节点是从节点候选的情况
        if (okslaves == max_slaves) {
            for (j = 0; j < node->numslaves; j++) {
                if (memcmp(node->slaves[j]->name,
                           candidate->name,
                           CLUSTER_NAMELEN) < 0)
                {
                    candidate = node->slaves[j];
                }
            }
        }
    }
    dictReleaseIterator(di);

    /* Step 4: perform the migration if there is a target, and if I'm the
     * candidate, but only if the master is continuously orphaned for a
     * couple of seconds, so that during failovers, we give some time to
     * the natural slaves of this instance to advertise their switch from
     * the old master to the new one. */
    // 如果有目标节点，且myself是候选节点，执行迁移
    if (target && candidate == myself &&
        (mstime()-target->orphaned_time) > CLUSTER_SLAVE_MIGRATION_DELAY)
    {
        serverLog(LL_WARNING,"Migrating to orphaned master %.40s",
            target->name);
        // 将指定target节点设置为myself主节点
        clusterSetMaster(target);
    }
}

/* -----------------------------------------------------------------------------
 * CLUSTER manual failover
 *
 * This are the important steps performed by slaves during a manual failover:
 * 1) User send CLUSTER FAILOVER command. The failover state is initialized
 *    setting mf_end to the millisecond unix time at which we'll abort the
 *    attempt.
 * 2) Slave sends a MFSTART message to the master requesting to pause clients
 *    for two times the manual failover timeout CLUSTER_MF_TIMEOUT.
 *    When master is paused for manual failover, it also starts to flag
 *    packets with CLUSTERMSG_FLAG0_PAUSED.
 * 3) Slave waits for master to send its replication offset flagged as PAUSED.
 * 4) If slave received the offset from the master, and its offset matches,
 *    mf_can_start is set to 1, and clusterHandleSlaveFailover() will perform
 *    the failover as usually, with the difference that the vote request
 *    will be modified to force masters to vote for a slave that has a
 *    working master.
 *
 * From the point of view of the master things are simpler: when a
 * PAUSE_CLIENTS packet is received the master sets mf_end as well and
 * the sender in mf_slave. During the time limit for the manual failover
 * the master will just send PINGs more often to this slave, flagged with
 * the PAUSED flag, so that the slave will set mf_master_offset when receiving
 * a packet from the master with this flag set.
 *
 * The gaol of the manual failover is to perform a fast failover without
 * data loss due to the asynchronous master-slave replication.
 * -------------------------------------------------------------------------- */

/* Reset the manual failover state. This works for both masters and slavesa
 * as all the state about manual failover is cleared.
 *
 * The function can be used both to initialize the manual failover state at
 * startup or to abort a manual failover in progress. */
// 重置与手动故障转移的状态，主节点和从节点都可以使用
// 该函数可以被使用在初始化一个故障转移状态也可以终止一个故障转移状态的进程
void resetManualFailover(void) {
    // client处于暂停状态且正在进行一个手动的故障转移
    if (server.cluster->mf_end && clientsArePaused()) {
        // 设置取消暂停状态的时间
        server.clients_pause_end_time = 0;
        // 将解除阻塞的client加入到非阻塞client链表中
        clientsArePaused(); /* Just use the side effect of the function. */
    }
    // 重置故障转移的属性
    server.cluster->mf_end = 0; /* No manual failover in progress. */
    server.cluster->mf_can_start = 0;
    server.cluster->mf_slave = NULL;
    server.cluster->mf_master_offset = 0;
}

/* If a manual failover timed out, abort it. */
// 如果一个手动故障转移超时，则终止它
void manualFailoverCheckTimeout(void) {
    if (server.cluster->mf_end && server.cluster->mf_end < mstime()) {
        serverLog(LL_WARNING,"Manual failover timed out.");
        resetManualFailover();
    }
}

/* This function is called from the cluster cron function in order to go
 * forward with a manual failover state machine. */
// 在cluster cron函数调用此函数，以便使用手动故障转移的状态继续操作
void clusterHandleManualFailover(void) {
    /* Return ASAP if no manual failover is in progress. */
    // 如果没有正在执行手动故障转移，直接返回
    if (server.cluster->mf_end == 0) return;

    /* If mf_can_start is non-zero, the failover was already triggered so the
     * next steps are performed by clusterHandleSlaveFailover(). */
    // 如果mf_can_start为真，那么表示故障转移已经被触发，直接返回
    if (server.cluster->mf_can_start) return;
    // 从节点没有记录主节点的复制偏移量，直接返回
    if (server.cluster->mf_master_offset == 0) return; /* Wait for offset... */
    // 如果主节点的复制偏移量和从节点复制偏移量相同
    if (server.cluster->mf_master_offset == replicationGetSlaveOffset()) {
        /* Our replication offset matches the master replication offset
         * announced after clients were paused. We can start the failover. */
        // 将手动故障转移的开始的标志打开，可以开始手动故障转移
        server.cluster->mf_can_start = 1;
        serverLog(LL_WARNING,
            "All master replication stream processed, "
            "manual failover can start.");
    }
}

/* -----------------------------------------------------------------------------
 * CLUSTER cron job
 * -------------------------------------------------------------------------- */

/* This is executed 10 times every second */
// 集群的周期性执行函数。没秒执行10次，100ms执行一次
void clusterCron(void) {
    dictIterator *di;
    dictEntry *de;
    int update_state = 0;
    // 没有从节点从属的主节点个数
    int orphaned_masters; /* How many masters there are without ok slaves. */
    // 所有主节点附属的最多的从节点数量
    int max_slaves; /* Max number of ok slaves for a single master. */
    // 如果myself是从节点，该从节点对应的主节点下有多少个主节点
    int this_slaves; /* Number of ok slaves for our master (if we are slave). */
    mstime_t min_pong = 0, now = mstime();
    clusterNode *min_pong_node = NULL;
    // 静态变量，表示该函数执行的计数器
    static unsigned long long iteration = 0;
    mstime_t handshake_timeout;
    // 执行一次
    iteration++; /* Number of times this function was called so far. */

    /* The handshake timeout is the time after which a handshake node that was
     * not turned into a normal node is removed from the nodes. Usually it is
     * just the NODE_TIMEOUT value, but when NODE_TIMEOUT is too small we use
     * the value of 1 second. */
    // 获取握手状态超时的时间，最低为1s
    // 如果一个处于握手状态的节点如果没有在该超时时限内变成一个普通的节点，那么该节点从节点字典中被删除
    handshake_timeout = server.cluster_node_timeout;
    if (handshake_timeout < 1000) handshake_timeout = 1000;

    /* Check if we have disconnected nodes and re-establish the connection. */
    // 检查是否当前集群中有断开连接的节点和重新建立连接的节点
    di = dictGetSafeIterator(server.cluster->nodes);
    // 遍历所有集群中的节点，如果有未建立连接的节点，那么发送PING或PONG消息，建立连接
    while((de = dictNext(di)) != NULL) {
        clusterNode *node = dictGetVal(de);
        // 跳过myself节点和处于NOADDR状态的节点
        if (node->flags & (CLUSTER_NODE_MYSELF|CLUSTER_NODE_NOADDR)) continue;

        /* A Node in HANDSHAKE state has a limited lifespan equal to the
         * configured node timeout. */
        // 如果仍然node节点处于握手状态，但是从建立连接开始到现在已经超时
        if (nodeInHandshake(node) && now - node->ctime > handshake_timeout) {
            // 从集群中删除该节点，遍历下一个节点
            clusterDelNode(node);
            continue;
        }
        // 如果节点的连接对象为空
        if (node->link == NULL) {
            int fd;
            mstime_t old_ping_sent;
            clusterLink *link;
            // myself节点连接这个node节点
            fd = anetTcpNonBlockBindConnect(server.neterr, node->ip,
                node->port+CLUSTER_PORT_INCR, NET_FIRST_BIND_ADDR);
            // 连接出错，跳过该节点
            if (fd == -1) {
                /* We got a synchronous error from connect before
                 * clusterSendPing() had a chance to be called.
                 * If node->ping_sent is zero, failure detection can't work,
                 * so we claim we actually sent a ping now (that will
                 * be really sent as soon as the link is obtained). */
                // 如果ping_sent为0，察觉故障无法执行，因此要设置发送PING的时间，当建立连接后会真正的的发送PING命令
                if (node->ping_sent == 0) node->ping_sent = mstime();
                serverLog(LL_DEBUG, "Unable to connect to "
                    "Cluster Node [%s]:%d -> %s", node->ip,
                    node->port+CLUSTER_PORT_INCR,
                    server.neterr);
                continue;
            }
            // 为node节点创建一个连接对象
            link = createClusterLink(node);
            // 设置连接对象的属性
            link->fd = fd;
            // 为node设置连接对象
            node->link = link;
            // 监听该连接的可读事件，设置可读时间的读处理函数
            aeCreateFileEvent(server.el,link->fd,AE_READABLE,
                    clusterReadHandler,link);
            /* Queue a PING in the new connection ASAP: this is crucial
             * to avoid false positives in failure detection.
             *
             * If the node is flagged as MEET, we send a MEET message instead
             * of a PING one, to force the receiver to add us in its node
             * table. */
            // 备份旧的发送PING的时间
            old_ping_sent = node->ping_sent;
            // 如果node节点指定了MEET标识，那么发送MEET命令，否则发送PING命令
            clusterSendPing(link, node->flags & CLUSTER_NODE_MEET ?
                    CLUSTERMSG_TYPE_MEET : CLUSTERMSG_TYPE_PING);
            // 如果不是第一次发送PING命令，要将发送PING的时间还原，等待被clusterSendPing()更新
            if (old_ping_sent) {
                /* If there was an active ping before the link was
                 * disconnected, we want to restore the ping time, otherwise
                 * replaced by the clusterSendPing() call. */
                node->ping_sent = old_ping_sent;
            }
            /* We can clear the flag after the first packet is sent.
             * If we'll never receive a PONG, we'll never send new packets
             * to this node. Instead after the PONG is received and we
             * are no longer in meet/handshake status, we want to send
             * normal PING packets. */
            // 发送MEET消息后，清除MEET标识
            // 如果没有接收到PONG回复，那么不会在向该节点发送消息
            // 如果接收到了PONG回复，取消MEET/HANDSHAKE状态，发送一个正常的PING消息。
            node->flags &= ~CLUSTER_NODE_MEET;

            serverLog(LL_DEBUG,"Connecting with Node %.40s at %s:%d",
                    node->name, node->ip, node->port+CLUSTER_PORT_INCR);
        }
    }
    dictReleaseIterator(di);

    /* Ping some random node 1 time every 10 iterations, so that we usually ping
     * one random node every second. */
    // 每1s中发送一次PING消息
    if (!(iteration % 10)) {
        int j;

        /* Check a few random nodes and ping the one with the oldest
         * pong_received time. */
        // 随机抽查5个节点，向pong_received值最小的发送PING消息
        for (j = 0; j < 5; j++) {
            // 随机抽查一个节点
            de = dictGetRandomKey(server.cluster->nodes);
            clusterNode *this = dictGetVal(de);

            /* Don't ping nodes disconnected or with a ping currently active. */
            // 跳过无连接或已经发送过PING的节点
            if (this->link == NULL || this->ping_sent != 0) continue;
            // 跳过myself节点和处于握手状态的节点
            if (this->flags & (CLUSTER_NODE_MYSELF|CLUSTER_NODE_HANDSHAKE))
                continue;
            // 查找出这个5个随机抽查的节点，接收到PONG回复过去最久的节点
            if (min_pong_node == NULL || min_pong > this->pong_received) {
                min_pong_node = this;
                min_pong = this->pong_received;
            }
        }
        // 向接收到PONG回复过去最久的节点发送PING消息，判断是否可达
        if (min_pong_node) {
            serverLog(LL_DEBUG,"Pinging node %.40s", min_pong_node->name);
            clusterSendPing(min_pong_node->link, CLUSTERMSG_TYPE_PING);
        }
    }

    /* Iterate nodes to check if we need to flag something as failing.
     * This loop is also responsible to:
     * 1) Check if there are orphaned masters (masters without non failing
     *    slaves).
     * 2) Count the max number of non failing slaves for a single master.
     * 3) Count the number of slaves for our master, if we are a slave. */
    // 迭代所有的节点，检查是否需要标记某个节点下线的状态
    /*
        1. 检查是否有孤立的主节点（主节点的从节点全部下线）
        2. 计算单个主节点没下线从节点的最大个数
        3. 如果myself是从节点，计算该从节点的主节点有多少个从节点
    */
    orphaned_masters = 0;
    max_slaves = 0;
    this_slaves = 0;
    di = dictGetSafeIterator(server.cluster->nodes);
    // 迭代所有的节点
    while((de = dictNext(di)) != NULL) {
        clusterNode *node = dictGetVal(de);
        now = mstime(); /* Use an updated time at every iteration. */
        mstime_t delay;
        // 跳过myself节点，无地址NOADDR节点，和处于握手状态的节点
        if (node->flags &
            (CLUSTER_NODE_MYSELF|CLUSTER_NODE_NOADDR|CLUSTER_NODE_HANDSHAKE))
                continue;

        /* Orphaned master check, useful only if the current instance
         * is a slave that may migrate to another master. */
        // 如果myself是从节点并且node节点是主节点并且该主节点不处于下线状态
        if (nodeIsSlave(myself) && nodeIsMaster(node) && !nodeFailed(node)) {
            // 判断node主节点有多少个正常的从节点
            int okslaves = clusterCountNonFailingSlaves(node);

            /* A master is orphaned if it is serving a non-zero number of
             * slots, have no working slaves, but used to have at least one
             * slave, or failed over a master that used to have slaves. */
            // node主节点没有ok的从节点，并且node节点负责有槽位，并且node节点指定了槽迁移标识
            if (okslaves == 0 && node->numslots > 0 &&
                node->flags & CLUSTER_NODE_MIGRATE_TO)
            {
                // 孤立的节点数加1
                orphaned_masters++;
            }
            // 更新一个主节点最多ok从节点的数量
            if (okslaves > max_slaves) max_slaves = okslaves;
            // 如果myself是从节点，并且从属于当前node主节点
            if (nodeIsSlave(myself) && myself->slaveof == node)
                // 记录myself从节点的主节点的ok从节点数
                this_slaves = okslaves;
        }

        /* If we are waiting for the PONG more than half the cluster
         * timeout, reconnect the link: maybe there is a connection
         * issue even if the node is alive. */
        // 如果等待PONG回复的时间超过cluster_node_timeout的一半，重新建立连接。即使节点正常，但是连接出问题
        if (node->link && /* is connected */
            now - node->link->ctime >
            server.cluster_node_timeout && /* was not already reconnected */
            node->ping_sent && /* we already sent a ping */
            node->pong_received < node->ping_sent && /* still waiting pong */
            /* and we are waiting for the pong more than timeout/2 */
            now - node->ping_sent > server.cluster_node_timeout/2)
        {
            /* Disconnect the link, it will be reconnected automatically. */
            // 释放连接，下个周期会自动重连
            freeClusterLink(node->link);
        }

        /* If we have currently no active ping in this instance, and the
         * received PONG is older than half the cluster timeout, send
         * a new ping now, to ensure all the nodes are pinged without
         * a too big delay. */
        // 如果当前没有发送PING消息，并且在一定时间内也没有收到PONG回复
        if (node->link &&
            node->ping_sent == 0 &&
            (now - node->pong_received) > server.cluster_node_timeout/2)
        {
            // 给node节点发送一个PING消息
            clusterSendPing(node->link, CLUSTERMSG_TYPE_PING);
            continue;
        }

        /* If we are a master and one of the slaves requested a manual
         * failover, ping it continuously. */
        // 如果myself是主节点，一个从节点请求手动故障转移
        if (server.cluster->mf_end &&
            nodeIsMaster(myself) &&
            server.cluster->mf_slave == node &&
            node->link)
        {
            // 给该请求节点发送PING消息
            clusterSendPing(node->link, CLUSTERMSG_TYPE_PING);
            continue;
        }

        /* Check only if we have an active ping for this instance. */
        // 如果当前还没有发送PING消息，则跳过，只要发送了PING消息之后，才会执行以下操作
        if (node->ping_sent == 0) continue;

        /* Compute the delay of the PONG. Note that if we already received
         * the PONG, then node->ping_sent is zero, so can't reach this
         * code at all. */
        // 计算已经等待接收PONG回复的时长
        delay = now - node->ping_sent;
        // 如果等待的时间超过了限制
        if (delay > server.cluster_node_timeout) {
            /* Timeout reached. Set the node as possibly failing if it is
             * not already in this state. */
            // 设置该节点为疑似下线的标识
            if (!(node->flags & (CLUSTER_NODE_PFAIL|CLUSTER_NODE_FAIL))) {
                serverLog(LL_DEBUG,"*** NODE %.40s possibly failing",
                    node->name);
                node->flags |= CLUSTER_NODE_PFAIL;
                // 设置更新状态的标识
                update_state = 1;
            }
        }
    }
    dictReleaseIterator(di);

    /* If we are a slave node but the replication is still turned off,
     * enable it if we know the address of our master and it appears to
     * be up. */
    // 如果myself是从节点，但是主节点仍然为空。如果知道myself的主节点地址
    if (nodeIsSlave(myself) &&
        server.masterhost == NULL &&
        myself->slaveof &&
        nodeHasAddr(myself->slaveof))
    {
        // 为myself从节点设置复制操作的主节点IP和端口
        replicationSetMaster(myself->slaveof->ip, myself->slaveof->port);
    }

    /* Abourt a manual failover if the timeout is reached. */
    // 终止一个超时的手动故障转移操作
    manualFailoverCheckTimeout();

    // 如果myself是从节点
    if (nodeIsSlave(myself)) {
        // 设置手动故障转移的状态
        clusterHandleManualFailover();
        // 执行从节点的自动或手动故障转移，从节点获取其主节点的哈希槽，并传播新配置
        clusterHandleSlaveFailover();
        /* If there are orphaned slaves, and we are a slave among the masters
         * with the max number of non-failing slaves, consider migrating to
         * the orphaned masters. Note that it does not make sense to try
         * a migration if there is no master with at least *two* working
         * slaves. */
        // 如果存在孤立的主节点，并且集群中的某一主节点有超过2个正常的从节点，并且该主节点正好是myself节点的主节点
        if (orphaned_masters && max_slaves >= 2 && this_slaves == max_slaves)
            // 给孤立的主节点迁移一个从节点
            clusterHandleSlaveMigration(max_slaves);
    }
    // 更新集群状态
    if (update_state || server.cluster->state == CLUSTER_FAIL)
        clusterUpdateState();
}

/* This function is called before the event handler returns to sleep for
 * events. It is useful to perform operations that must be done ASAP in
 * reaction to events fired but that are not safe to perform inside event
 * handlers, or to perform potentially expansive tasks that we need to do
 * a single time before replying to clients. */
// 在下个事件循环之前调用，执行一些收尾工作
void clusterBeforeSleep(void) {
    /* Handle failover, this is needed when it is likely that there is already
     * the quorum from masters in order to react fast. */
    // 处理从节点故障转移，当可能已经有大多数主节点的投票数，以便快速执行
    if (server.cluster->todo_before_sleep & CLUSTER_TODO_HANDLE_FAILOVER)
        clusterHandleSlaveFailover();

    /* Update the cluster state. */
    // 更新集群状态
    if (server.cluster->todo_before_sleep & CLUSTER_TODO_UPDATE_STATE)
        clusterUpdateState();

    /* Save the config, possibly using fsync. */
    // 保存配置到文件中
    if (server.cluster->todo_before_sleep & CLUSTER_TODO_SAVE_CONFIG) {
        int fsync = server.cluster->todo_before_sleep &
                    CLUSTER_TODO_FSYNC_CONFIG;
        clusterSaveConfigOrDie(fsync);
    }

    /* Reset our flags (not strictly needed since every single function
     * called for flags set should be able to clear its flag). */
    // 清零下次执行的todo_before_sleep标识
    server.cluster->todo_before_sleep = 0;
}

// 设置sleep函数的执行内容的标识
void clusterDoBeforeSleep(int flags) {
    server.cluster->todo_before_sleep |= flags;
}

/* -----------------------------------------------------------------------------
 * Slots management
 * -------------------------------------------------------------------------- */

/* Test bit 'pos' in a generic bitmap. Return 1 if the bit is set,
 * otherwise 0. */
// 测试pos位置是否被设置为1，如果被设置则返回1，否则返回0
int bitmapTestBit(unsigned char *bitmap, int pos) {
    // pos位置位于哪一个字节
    off_t byte = pos/8;
    // byte字节的哪一位
    int bit = pos&7;
    return (bitmap[byte] & (1<<bit)) != 0;
}

/* Set the bit at position 'pos' in a bitmap. */
// 将指定pos位置的位设置为1
void bitmapSetBit(unsigned char *bitmap, int pos) {
    off_t byte = pos/8;
    int bit = pos&7;
    bitmap[byte] |= 1<<bit;
}

/* Clear the bit at position 'pos' in a bitmap. */
// 清除pos位置上的1
void bitmapClearBit(unsigned char *bitmap, int pos) {
    off_t byte = pos/8;
    int bit = pos&7;
    bitmap[byte] &= ~(1<<bit);
}

/* Return non-zero if there is at least one master with slaves in the cluster.
 * Otherwise zero is returned. Used by clusterNodeSetSlotBit() to set the
 * MIGRATE_TO flag the when a master gets the first slot. */
// 计算集群中主节点的从节点之和，如果为0，返回0，否则返回1
int clusterMastersHaveSlaves(void) {
    dictIterator *di = dictGetSafeIterator(server.cluster->nodes);
    dictEntry *de;
    int slaves = 0;
    // 遍历所有的节点
    while((de = dictNext(di)) != NULL) {
        clusterNode *node = dictGetVal(de);
        // 跳过从节点
        if (nodeIsSlave(node)) continue;
        // 计算主节点的从节点之和
        slaves += node->numslaves;
    }
    dictReleaseIterator(di);
    return slaves != 0;
}

/* Set the slot bit and return the old value. */
// 设置slot槽位并返回旧的值
int clusterNodeSetSlotBit(clusterNode *n, int slot) {
    // 查看slot槽位是否被设置
    int old = bitmapTestBit(n->slots,slot);
    // 将slot槽位设置为1
    bitmapSetBit(n->slots,slot);
    // 如果之前没有被设置
    if (!old) {
        // 那么要更新n节点负责槽的个数
        n->numslots++;
        /* When a master gets its first slot, even if it has no slaves,
         * it gets flagged with MIGRATE_TO, that is, the master is a valid
         * target for replicas migration, if and only if at least one of
         * the other masters has slaves right now.
         *
         * Normally masters are valid targerts of replica migration if:
         * 1. The used to have slaves (but no longer have).
         * 2. They are slaves failing over a master that used to have slaves.
         *
         * However new masters with slots assigned are considered valid
         * migration tagets if the rest of the cluster is not a slave-less.
         *
         * See https://github.com/antirez/redis/issues/3043 for more info. */
        // 如果主节点是第一次指定槽，即使它没有从节点，也要设置MIGRATE_TO标识
        // 当且仅当至少有一个其他的主节点有从节点时，主节点才是有效的迁移目标
        if (n->numslots == 1 && clusterMastersHaveSlaves())
            // 设置节点迁移的标识
            n->flags |= CLUSTER_NODE_MIGRATE_TO;
    }
    return old;
}

/* Clear the slot bit and return the old value. */
// 清理slot位的值，并返回旧的值
int clusterNodeClearSlotBit(clusterNode *n, int slot) {
    // 返回slot位的值
    int old = bitmapTestBit(n->slots,slot);
    // 将slot位的值清零
    bitmapClearBit(n->slots,slot);
    // 如果slot值之前被指定，将节点负责的槽位减1
    if (old) n->numslots--;
    return old;
}

/* Return the slot bit from the cluster node structure. */
// 获取指定的slot槽的值，判断该位的槽是否被n节点所负责
int clusterNodeGetSlotBit(clusterNode *n, int slot) {
    return bitmapTestBit(n->slots,slot);
}

/* Add the specified slot to the list of slots that node 'n' will
 * serve. Return C_OK if the operation ended with success.
 * If the slot is already assigned to another instance this is considered
 * an error and C_ERR is returned. */
// 将slot槽指定给n节点
int clusterAddSlot(clusterNode *n, int slot) {
    // 如果已经指定有节点，则返回C_ERR
    if (server.cluster->slots[slot]) return C_ERR;
    // 设置该槽被指定
    clusterNodeSetSlotBit(n,slot);
    // 设置负责该槽的节点n
    server.cluster->slots[slot] = n;
    return C_OK;
}

/* Delete the specified slot marking it as unassigned.
 * Returns C_OK if the slot was assigned, otherwise if the slot was
 * already unassigned C_ERR is returned. */
// 删除被标记为已指定的槽位，如果slot被指定返回C_OK，否则槽本就是未指定的返回C_ERR
int clusterDelSlot(int slot) {
    // 获取该负责该槽的节点
    clusterNode *n = server.cluster->slots[slot];

    if (!n) return C_ERR;
    // 确保该槽已经被指定
    serverAssert(clusterNodeClearSlotBit(n,slot) == 1);
    // 删除负责该槽的节点
    server.cluster->slots[slot] = NULL;
    return C_OK;
}

/* Delete all the slots associated with the specified node.
 * The number of deleted slots is returned. */
// 删除该node节点负责的所有槽
int clusterDelNodeSlots(clusterNode *node) {
    int deleted = 0, j;
    // 遍历所有的槽
    for (j = 0; j < CLUSTER_SLOTS; j++) {
        // 如果当前j位槽被node节点负责，那么节点取消负责该位
        if (clusterNodeGetSlotBit(node,j)) clusterDelSlot(j);
        deleted++;
    }
    // 返回删除的个数
    return deleted;
}

/* Clear the migrating / importing state for all the slots.
 * This is useful at initialization and when turning a master into slave. */
// 清空所有槽的导入导出状态
// 当主节点变为从节点时，用来初始化
void clusterCloseAllSlots(void) {
    memset(server.cluster->migrating_slots_to,0,
        sizeof(server.cluster->migrating_slots_to));
    memset(server.cluster->importing_slots_from,0,
        sizeof(server.cluster->importing_slots_from));
}

/* -----------------------------------------------------------------------------
 * Cluster state evaluation function
 * -------------------------------------------------------------------------- */

/* The following are defines that are only used in the evaluation function
 * and are based on heuristics. Actaully the main point about the rejoin and
 * writable delay is that they should be a few orders of magnitude larger
 * than the network latency. */
#define CLUSTER_MAX_REJOIN_DELAY 5000
#define CLUSTER_MIN_REJOIN_DELAY 500
#define CLUSTER_WRITABLE_DELAY 2000

// 更新集群的状态
void clusterUpdateState(void) {
    int j, new_state;
    int reachable_masters = 0;
    static mstime_t among_minority_time;
    static mstime_t first_call_time = 0;

    server.cluster->todo_before_sleep &= ~CLUSTER_TODO_UPDATE_STATE;

    /* If this is a master node, wait some time before turning the state
     * into OK, since it is not a good idea to rejoin the cluster as a writable
     * master, after a reboot, without giving the cluster a chance to
     * reconfigure this node. Note that the delay is calculated starting from
     * the first call to this function and not since the server start, in order
     * to don't count the DB loading time. */
    // 如果这是一个主节点，等待一段时间才能将状态转为OK，因为当节点重新启动后，将该节点作为可写的主节点重新加入集群中，而没有给集群去重新配置该节点的机会，这样不是合理的。
    // 第一次调用该函数就开始计算一个延迟时间，而不是从该节点启动开始，以便不计算加载数据的时间

    // 第一次调用该函数的时间
    if (first_call_time == 0) first_call_time = mstime();
    // 如果myself节点是主节点，并且集群处于下线状态，离第一次调用该函数不到2s，直接返回
    if (nodeIsMaster(myself) &&
        server.cluster->state == CLUSTER_FAIL &&
        mstime() - first_call_time < CLUSTER_WRITABLE_DELAY) return;

    /* Start assuming the state is OK. We'll turn it into FAIL if there
     * are the right conditions. */
    // 开始假设集群状态是ok的，如果满足一些情况则设置为FAIL
    new_state = CLUSTER_OK;

    /* Check if all the slots are covered. */
    // 如果设置集群的所有槽必须全部指定的标识
    if (server.cluster_require_full_coverage) {
        // 遍历所有的槽
        for (j = 0; j < CLUSTER_SLOTS; j++) {
            // 如果有槽处于为指定，或者指定的节点处于下线状态，那么设置集群的状态为下线状态
            if (server.cluster->slots[j] == NULL ||
                server.cluster->slots[j]->flags & (CLUSTER_NODE_FAIL))
            {
                new_state = CLUSTER_FAIL;
                break;
            }
        }
    }

    /* Compute the cluster size, that is the number of master nodes
     * serving at least a single slot.
     *
     * At the same time count the number of reachable masters having
     * at least one slot. */
    // 计算集群的大小，至少负责一个槽的主节点个数
    // 当同一时间，至少有一个槽的可达主节点个数
    {
        dictIterator *di;
        dictEntry *de;

        server.cluster->size = 0;
        // 迭代所有的节点
        di = dictGetSafeIterator(server.cluster->nodes);
        while((de = dictNext(di)) != NULL) {
            clusterNode *node = dictGetVal(de);
            // 如果node为主节点，且节点至少负责一个槽位
            if (nodeIsMaster(node) && node->numslots) {
                // 集群大小加1
                server.cluster->size++;
                // 该节点不处于下线状态
                if ((node->flags & (CLUSTER_NODE_FAIL|CLUSTER_NODE_PFAIL)) == 0)
                    // 可达的主节点个数加1
                    reachable_masters++;
            }
        }
        dictReleaseIterator(di);
    }

    /* If we are in a minority partition, change the cluster state
     * to FAIL. */
    // 如果不能获得多半的票数，那么将集群的状态设置为FAIL
    {
        // 需要获得票数
        int needed_quorum = (server.cluster->size / 2) + 1;
        // 可达的节点小于需要的个数
        if (reachable_masters < needed_quorum) {
            // 设置集群的状态为下线状态
            new_state = CLUSTER_FAIL;
            // 设置节点处于少数部分的时间
            among_minority_time = mstime();
        }
    }

    /* Log a state change */
    // 记录状态的变更
    if (new_state != server.cluster->state) {
        // 主节点重新加入集群的延迟时间
        mstime_t rejoin_delay = server.cluster_node_timeout;

        /* If the instance is a master and was partitioned away with the
         * minority, don't let it accept queries for some time after the
         * partition heals, to make sure there is enough time to receive
         * a configuration update. */
        //延迟时间[0.5s , 5s]之间
        if (rejoin_delay > CLUSTER_MAX_REJOIN_DELAY)
            rejoin_delay = CLUSTER_MAX_REJOIN_DELAY;
        if (rejoin_delay < CLUSTER_MIN_REJOIN_DELAY)
            rejoin_delay = CLUSTER_MIN_REJOIN_DELAY;

        // 如果集群状态还ok，且myself是主节点且距离上一次该节点处于少数部分的时间没有超过延迟时间
        // 那么直接返回，不需要记录状态变更
        if (new_state == CLUSTER_OK &&
            nodeIsMaster(myself) &&
            mstime() - among_minority_time < rejoin_delay)
        {
            return;
        }

        /* Change the state and log the event. */
        // 设置当前集群的状态
        serverLog(LL_WARNING,"Cluster state changed: %s",
            new_state == CLUSTER_OK ? "ok" : "fail");
        server.cluster->state = new_state;
    }
}

/* This function is called after the node startup in order to verify that data
 * loaded from disk is in agreement with the cluster configuration:
 *
 * 1) If we find keys about hash slots we have no responsibility for, the
 *    following happens:
 *    A) If no other node is in charge according to the current cluster
 *       configuration, we add these slots to our node.
 *    B) If according to our config other nodes are already in charge for
 *       this lots, we set the slots as IMPORTING from our point of view
 *       in order to justify we have those slots, and in order to make
 *       redis-trib aware of the issue, so that it can try to fix it.
 * 2) If we find data in a DB different than DB0 we return C_ERR to
 *    signal the caller it should quit the server with an error message
 *    or take other actions.
 *
 * The function always returns C_OK even if it will try to correct
 * the error described in "1". However if data is found in DB different
 * from DB0, C_ERR is returned.
 *
 * The function also uses the logging facility in order to warn the user
 * about desynchronizations between the data we have in memory and the
 * cluster configuration. */
// 校验该节点的配置是否正确，包含的数据是否正确
int verifyClusterConfigWithData(void) {
    int j;
    int update_config = 0;

    /* If this node is a slave, don't perform the check at all as we
     * completely depend on the replication stream. */
    // myself节点是从节点，不执行检查，直接返回C_OK
    if (nodeIsSlave(myself)) return C_OK;

    /* Make sure we only have keys in DB0. */
    // 确定我们只有0号数据库有数据
    for (j = 1; j < server.dbnum; j++) {
        // 如果其他数据有数据返回C_ERR
        if (dictSize(server.db[j].dict)) return C_ERR;
    }

    /* Check that all the slots we see populated memory have a corresponding
     * entry in the cluster table. Otherwise fix the table. */
    // 检查所有的槽是否都被指定，否则修改该表
    for (j = 0; j < CLUSTER_SLOTS; j++) {
        // 指定slot中没有包含键，跳过
        if (!countKeysInSlot(j)) continue; /* No keys in this slot. */
        /* Check if we are assigned to this slot or if we are importing it.
         * In both cases check the next slot as the configuration makes
         * sense. */
        // 跳过myself节点否则的槽和正在导入的槽
        if (server.cluster->slots[j] == myself ||
            server.cluster->importing_slots_from[j] != NULL) continue;

        /* If we are here data and cluster config don't agree, and we have
         * slot 'j' populated even if we are not importing it, nor we are
         * assigned to this slot. Fix this condition. */
        // 执行到这，则需要更新配置
        update_config++;
        /* Case A: slot is unassigned. Take responsibility for it. */
        // 如果该槽未指定负责的节点，那么由myself节点负责
        if (server.cluster->slots[j] == NULL) {
            serverLog(LL_WARNING, "I have keys for unassigned slot %d. "
                                    "Taking responsibility for it.",j);
            // 将该槽指定给myself节点
            clusterAddSlot(myself,j);
        // 该槽被其他节点负责
        } else {
            serverLog(LL_WARNING, "I have keys for slot %d, but the slot is "
                                    "assigned to another node. "
                                    "Setting it to importing state.",j);
            // 那么将该槽加入到集群的importing_slots_from表中，表示该槽被其他节点所负责
            server.cluster->importing_slots_from[j] = server.cluster->slots[j];
        }
    }
    // 需要更新配置
    if (update_config) clusterSaveConfigOrDie(1);
    return C_OK;
}

/* -----------------------------------------------------------------------------
 * SLAVE nodes handling
 * -------------------------------------------------------------------------- */

/* Set the specified node 'n' as master for this node.
 * If this node is currently a master, it is turned into a slave. */
// 将指定的节点n设置为myself的主节点。如果myself本来就是主节点，那么设置为从节点
void clusterSetMaster(clusterNode *n) {
    // 保证n节点不是myself节点
    serverAssert(n != myself);
    // 保证myself节点不负责槽位
    serverAssert(myself->numslots == 0);
    // 如果myself节点是主节点
    if (nodeIsMaster(myself)) {
        // 取消主节点和迁移从节点的标识
        myself->flags &= ~(CLUSTER_NODE_MASTER|CLUSTER_NODE_MIGRATE_TO);
        // 设置为从节点
        myself->flags |= CLUSTER_NODE_SLAVE;
        // 清空所有槽的导入导出状态
        clusterCloseAllSlots();
    // myself是从节点
    } else {
        // 如果myself从节点有从属的主节点，将myself从节点和它的主节点断开主从关系
        if (myself->slaveof)
            clusterNodeRemoveSlave(myself->slaveof,myself);
    }
    // 将节点n设置为myself的主节点
    myself->slaveof = n;
    // 将myself节点添加到主节点n的从节点字典中
    clusterNodeAddSlave(n,myself);
    // 使myself节点复制n节点
    replicationSetMaster(n->ip, n->port);
    // 重置与手动故障转移的状态
    resetManualFailover();
}

/* -----------------------------------------------------------------------------
 * Nodes to string representation functions.
 * -------------------------------------------------------------------------- */

struct redisNodeFlags {
    uint16_t flag;
    char *name;
};

// 节点状态表
static struct redisNodeFlags redisNodeFlagsTable[] = {
    {CLUSTER_NODE_MYSELF,       "myself,"},
    {CLUSTER_NODE_MASTER,       "master,"},
    {CLUSTER_NODE_SLAVE,        "slave,"},
    {CLUSTER_NODE_PFAIL,        "fail?,"},
    {CLUSTER_NODE_FAIL,         "fail,"},
    {CLUSTER_NODE_HANDSHAKE,    "handshake,"},
    {CLUSTER_NODE_NOADDR,       "noaddr,"}
};

/* Concatenate the comma separated list of node flags to the given SDS
 * string 'ci'. */
// 为指定的ci连接一个用逗号分隔开的标识
sds representClusterNodeFlags(sds ci, uint16_t flags) {
    // 没有指定flag则设置为"noflags,"
    if (flags == 0) {
        ci = sdscat(ci,"noflags,");
    } else {
        int i, size = sizeof(redisNodeFlagsTable)/sizeof(struct redisNodeFlags);
        // 遍历标识表
        for (i = 0; i < size; i++) {
            struct redisNodeFlags *nodeflag = redisNodeFlagsTable + i;
            // 根据指定的flags将指定的标识连接到ci中
            if (flags & nodeflag->flag) ci = sdscat(ci, nodeflag->name);
        }
    }
    // 删除尾部的'\n'
    sdsIncrLen(ci,-1); /* Remove trailing comma. */
    return ci;
}

/* Generate a csv-alike representation of the specified cluster node.
 * See clusterGenNodesDescription() top comment for more information.
 *
 * The function returns the string representation as an SDS string. */
// 生成一个node节点的sds类型的描述信息
sds clusterGenNodeDescription(clusterNode *node) {
    int j, start;
    sds ci;

    /* Node coordinates */
    // 节点的name和地址
    ci = sdscatprintf(sdsempty(),"%.40s %s:%d ",
        node->name,
        node->ip,
        node->port);

    /* Flags */
    // 节点的标识
    ci = representClusterNodeFlags(ci, node->flags);

    /* Slave of... or just "-" */
    // 如果是从节点，加上它主节点的名字，否则就是'-'
    if (node->slaveof)
        ci = sdscatprintf(ci," %.40s ",node->slaveof->name);
    else
        ci = sdscatlen(ci," - ",3);

    /* Latency from the POV of this node, config epoch, link status */
    // node节点的延迟信息，配置纪元，连接状态
    ci = sdscatprintf(ci,"%lld %lld %llu %s",
        (long long) node->ping_sent,
        (long long) node->pong_received,
        (unsigned long long) node->configEpoch,
        (node->link || node->flags & CLUSTER_NODE_MYSELF) ?
                    "connected" : "disconnected");

    /* Slots served by this instance */
    // 被该节点负责的槽信息
    start = -1;
    for (j = 0; j < CLUSTER_SLOTS; j++) {
        int bit;
        // 获得第一个该node节点负责的槽位
        if ((bit = clusterNodeGetSlotBit(node,j)) != 0) {
            // 设置为start槽位
            if (start == -1) start = j;
        }
        // 如果获取到了start的槽位，但是之后某些槽位不由node节点负责或者遍历完了所有的槽位，执行以下代码
        if (start != -1 && (!bit || j == CLUSTER_SLOTS-1)) {
            // 如果最后一个槽由node负责
            if (bit && j == CLUSTER_SLOTS-1) j++;
            // 如果node节点只负责最后一个槽位，按照单独格式打印
            if (start == j-1) {
                ci = sdscatprintf(ci," %d",start);
            // 否则按照范围的格式打印
            } else {
                ci = sdscatprintf(ci," %d-%d",start,j-1);
            }
            // 重置start标识
            start = -1;
        }
    }

    /* Just for MYSELF node we also dump info about slots that
     * we are migrating to other instances or importing from other
     * instances. */
    // 如果node节点是myself节点
    if (node->flags & CLUSTER_NODE_MYSELF) {
        // 遍历所有的槽
        for (j = 0; j < CLUSTER_SLOTS; j++) {
            // 打印导出槽和导入槽所属节点的信息
            if (server.cluster->migrating_slots_to[j]) {
                ci = sdscatprintf(ci," [%d->-%.40s]",j,
                    server.cluster->migrating_slots_to[j]->name);
            } else if (server.cluster->importing_slots_from[j]) {
                ci = sdscatprintf(ci," [%d-<-%.40s]",j,
                    server.cluster->importing_slots_from[j]->name);
            }
        }
    }
    return ci;
}

/* Generate a csv-alike representation of the nodes we are aware of,
 * including the "myself" node, and return an SDS string containing the
 * representation (it is up to the caller to free it).
 *
 * All the nodes matching at least one of the node flags specified in
 * "filter" are excluded from the output, so using zero as a filter will
 * include all the known nodes in the representation, including nodes in
 * the HANDSHAKE state.
 *
 * The representation obtained using this function is used for the output
 * of the CLUSTER NODES function, and as format for the cluster
 * configuration file (nodes.conf) for a given node. */
// 通常以逗号分隔（csv）的格式记录节点的信息，这些信息被函数保存在sds中然后返回
// filter可以指定被过滤节点的状态，带有filter状态的节点不被输出。filter为0表示记录所有节点的信息，包括握手状态的节点
// 这个函数用于生成CLUSTER NODES命令，以及生成配置文件
sds clusterGenNodesDescription(int filter) {
    sds ci = sdsempty(), ni;
    dictIterator *di;
    dictEntry *de;

    di = dictGetSafeIterator(server.cluster->nodes);
    // 迭代所有的节点
    while((de = dictNext(di)) != NULL) {
        clusterNode *node = dictGetVal(de);
        // 过滤指定状态的节点
        if (node->flags & filter) continue;
        // 获得当前节点的sds信息描述
        ni = clusterGenNodeDescription(node);
        // 保存到ci中，并且追加'\n'
        ci = sdscatsds(ci,ni);
        sdsfree(ni);
        ci = sdscatlen(ci,"\n",1);
    }
    dictReleaseIterator(di);
    return ci;
}

/* -----------------------------------------------------------------------------
 * CLUSTER command
 * -------------------------------------------------------------------------- */
// 获取槽位值
int getSlotOrReply(client *c, robj *o) {
    long long slot;
    // 从o对象中获取一个slot值
    if (getLongLongFromObject(o,&slot) != C_OK ||
        slot < 0 || slot >= CLUSTER_SLOTS)
    {
        addReplyError(c,"Invalid or out of range slot");
        return -1;
    }
    return (int) slot;
}

// 回复client集群槽的分配信息
void clusterReplyMultiBulkSlots(client *c) {
    /* Format: 1) 1) start slot
     *            2) end slot
     *            3) 1) master IP
     *               2) master port
     *               3) node ID
     *            4) 1) replica IP
     *               2) replica port
     *               3) node ID
     *           ... continued until done
     */

    int num_masters = 0;
    // 添加一个空对象到回复链表中，之后在往链表中添加回复
    void *slot_replylen = addDeferredMultiBulkLength(c);

    dictEntry *de;
    dictIterator *di = dictGetSafeIterator(server.cluster->nodes);
    // 迭代所有集群节点
    while((de = dictNext(di)) != NULL) {
        clusterNode *node = dictGetVal(de);
        int j = 0, start = -1;

        /* Skip slaves (that are iterated when producing the output of their
         * master) and  masters not serving any slot. */
        // 跳过从节点或者不负责槽的主节点
        if (!nodeIsMaster(node) || node->numslots == 0) continue;
        // 遍历所有的槽
        for (j = 0; j < CLUSTER_SLOTS; j++) {
            int bit, i;
            // 获得第一个该node节点负责的槽位
            if ((bit = clusterNodeGetSlotBit(node,j)) != 0) {
                // 设置为start槽位
                if (start == -1) start = j;
            }
            // 如果获取到了start的槽位，但是之后某些槽位不由node节点负责或者遍历完了所有的槽位，执行以下代码
            if (start != -1 && (!bit || j == CLUSTER_SLOTS-1)) {
                int nested_elements = 3; /* slots (2) + master addr (1). */
                // 添加一个空对象到嵌入回复链表中，之后在往链表中添加回复
                void *nested_replylen = addDeferredMultiBulkLength(c);
                // 如果最后一个槽由node负责
                if (bit && j == CLUSTER_SLOTS-1) j++;

                /* If slot exists in output map, add to it's list.
                 * else, create a new output map for this slot */
                // 如果node节点只负责最后一个槽位，添加回复start到start
                if (start == j-1) {
                    addReplyLongLong(c, start); /* only one slot; low==high */
                    addReplyLongLong(c, start);
                // 否则，添加回复start到j-1，是一个范围
                } else {
                    addReplyLongLong(c, start); /* low */
                    addReplyLongLong(c, j-1);   /* high */
                }
                // 重置start值
                start = -1;

                /* First node reply position is always the master */
                // 添加主节点回复新信息
                // 添加多条回复的长度：*3\r\n
                addReplyMultiBulkLen(c, 3);
                // 添加节点IP回复：$<ip>\r\n
                addReplyBulkCString(c, node->ip);
                // 添加节点port回复：$<port>\r\n
                addReplyLongLong(c, node->port);
                // 添加节点name回复：$<name>\r\n
                addReplyBulkCBuffer(c, node->name, CLUSTER_NAMELEN);

                /* Remaining nodes in reply are replicas for slot range */
                // 遍历该节点的所有从节点
                for (i = 0; i < node->numslaves; i++) {
                    /* This loop is copy/pasted from clusterGenNodeDescription()
                     * with modifications for per-slot node aggregation */
                    // 跳过处于下线的从节点
                    if (nodeFailed(node->slaves[i])) continue;
                    // 添加从节点回复新信息
                    addReplyMultiBulkLen(c, 3);                                     //多条回复的长度：*3\r\n
                    addReplyBulkCString(c, node->slaves[i]->ip);                    //IP回复：$<ip>\r\n
                    addReplyLongLong(c, node->slaves[i]->port);                     //port回复：$<port>\r\n
                    addReplyBulkCBuffer(c, node->slaves[i]->name, CLUSTER_NAMELEN); //name回复：$<name>\r\n
                    // 嵌入元素的个数
                    nested_elements++;
                }
                // 设置回复的一个nested_elements长度，并尝试粘合下一个节点
                setDeferredMultiBulkLength(c, nested_replylen, nested_elements);
                // 主节点的个数
                num_masters++;
            }
        }
    }
    dictReleaseIterator(di);
    // 设置回复的一个num_masters长度，并尝试粘合下一个节点
    setDeferredMultiBulkLength(c, slot_replylen, num_masters);
}

// CLUSTER命令实现
void clusterCommand(client *c) {
    // 没有开启集群模式，发送错误回复然后返回
    if (server.cluster_enabled == 0) {
        addReplyError(c,"This instance has cluster support disabled");
        return;
    }
    // CLUSTER MEET <ip> <port>命令
    // 与给定地址的节点建立连接
    if (!strcasecmp(c->argv[1]->ptr,"meet") && c->argc == 4) {
        long long port;
        // 获取端口
        if (getLongLongFromObject(c->argv[3], &port) != C_OK) {
            addReplyErrorFormat(c,"Invalid TCP port specified: %s",
                                (char*)c->argv[3]->ptr);
            return;
        }
        // 如果没有正在进行握手，那么根据执行的地址开始进行握手操作
        if (clusterStartHandshake(c->argv[2]->ptr,port) == 0 &&
            errno == EINVAL)
        {
            addReplyErrorFormat(c,"Invalid node address specified: %s:%s",
                            (char*)c->argv[2]->ptr, (char*)c->argv[3]->ptr);
        // 连接成功回复ok
        } else {
            addReply(c,shared.ok);
        }
    // 列出所有的节点信息
    } else if (!strcasecmp(c->argv[1]->ptr,"nodes") && c->argc == 2) {
        /* CLUSTER NODES */
        robj *o;
        // 获取集群中所有节点的sds形式信息
        sds ci = clusterGenNodesDescription(0);
        // 创建回复信息对象，并添加回复
        o = createObject(OBJ_STRING,ci);
        addReplyBulk(c,o);
        decrRefCount(o);
    // 回复当前节点myself的ID
    } else if (!strcasecmp(c->argv[1]->ptr,"myid") && c->argc == 2) {
        /* CLUSTER MYID */
        addReplyBulkCBuffer(c,myself->name, CLUSTER_NAMELEN);
    // 回复集群中槽的分配信息
    } else if (!strcasecmp(c->argv[1]->ptr,"slots") && c->argc == 2) {
        /* CLUSTER SLOTS */
        clusterReplyMultiBulkSlots(c);
    // 删除当前节点的所有负责的槽
    } else if (!strcasecmp(c->argv[1]->ptr,"flushslots") && c->argc == 2) {
        /* CLUSTER FLUSHSLOTS */
        // 数据库必须为空，才能删除
        if (dictSize(server.db[0].dict) != 0) {
            addReplyError(c,"DB must be empty to perform CLUSTER FLUSHSLOTS.");
            return;
        }
        clusterDelNodeSlots(myself);
        // 更新配置和状态，并回复ok
        clusterDoBeforeSleep(CLUSTER_TODO_UPDATE_STATE|CLUSTER_TODO_SAVE_CONFIG);
        addReply(c,shared.ok);
    // 将指定的槽添加到当前节点
    // 从当前节点删除指定的槽
    } else if ((!strcasecmp(c->argv[1]->ptr,"addslots") ||
               !strcasecmp(c->argv[1]->ptr,"delslots")) && c->argc >= 3)
    {
        /* CLUSTER ADDSLOTS <slot> [slot] ... */
        /* CLUSTER DELSLOTS <slot> [slot] ... */
        int j, slot;
        unsigned char *slots = zmalloc(CLUSTER_SLOTS);
        // 删除操作
        int del = !strcasecmp(c->argv[1]->ptr,"delslots");

        memset(slots,0,CLUSTER_SLOTS);
        /* Check that all the arguments are parseable and that all the
         * slots are not already busy. */
        // 遍历所有指定的槽
        for (j = 2; j < c->argc; j++) {
            // 获取槽位的位置
            if ((slot = getSlotOrReply(c,c->argv[j])) == -1) {
                zfree(slots);
                return;
            }
            // 如果是删除操作，但是槽没有指定负责的节点，回复错误信息
            if (del && server.cluster->slots[slot] == NULL) {
                addReplyErrorFormat(c,"Slot %d is already unassigned", slot);
                zfree(slots);
                return;
            // 如果是添加操作，但是槽已经指定负责的节点，回复错误信息
            } else if (!del && server.cluster->slots[slot]) {
                addReplyErrorFormat(c,"Slot %d is already busy", slot);
                zfree(slots);
                return;
            }
            // 如果某个槽已经指定过多次了（在参数中指定了多次），那么回复错误信息
            if (slots[slot]++ == 1) {
                addReplyErrorFormat(c,"Slot %d specified multiple times",
                    (int)slot);
                zfree(slots);
                return;
            }
        }
        // 上个循环保证了指定的槽的可以处理
        for (j = 0; j < CLUSTER_SLOTS; j++) {
            // 如果当前参数中指定槽
            if (slots[j]) {
                int retval;

                /* If this slot was set as importing we can clear this
                 * state as now we are the real owner of the slot. */
                // 如果这个槽被设置为导入状态，那么取消该状态
                if (server.cluster->importing_slots_from[j])
                    server.cluster->importing_slots_from[j] = NULL;
                // 执行删除或添加操作
                retval = del ? clusterDelSlot(j) :
                               clusterAddSlot(myself,j);
                serverAssertWithInfo(c,NULL,retval == C_OK);
            }
        }
        zfree(slots);
        // 更新集群状态和保存配置
        clusterDoBeforeSleep(CLUSTER_TODO_UPDATE_STATE|CLUSTER_TODO_SAVE_CONFIG);
        addReply(c,shared.ok);

    } else if (!strcasecmp(c->argv[1]->ptr,"setslot") && c->argc >= 4) {
        /* SETSLOT 10 MIGRATING <node ID> */    //设置10号槽处于MIGRATING状态，迁移到<node ID>指定的节点
        /* SETSLOT 10 IMPORTING <node ID> */    //设置10号槽处于IMPORTING状态，将<node ID>指定的节点的槽导入到myself中
        /* SETSLOT 10 STABLE */                 //取消10号槽的MIGRATING/IMPORTING状态
        /* SETSLOT 10 NODE <node ID> */         //将10号槽绑定到NODE节点上
        int slot;
        clusterNode *n;
        // 如果myself节点是从节点，回复错误信息
        if (nodeIsSlave(myself)) {
            addReplyError(c,"Please use SETSLOT only with masters.");
            return;
        }
        // 获取槽号
        if ((slot = getSlotOrReply(c,c->argv[2])) == -1) return;
        // 如果是migrating
        if (!strcasecmp(c->argv[3]->ptr,"migrating") && c->argc == 5) {
            // 如果该槽不是myself主节点负责，那么就不能进行迁移
            if (server.cluster->slots[slot] != myself) {
                addReplyErrorFormat(c,"I'm not the owner of hash slot %u",slot);
                return;
            }
            // 获取迁移的目标节点
            if ((n = clusterLookupNode(c->argv[4]->ptr)) == NULL) {
                addReplyErrorFormat(c,"I don't know about node %s",
                    (char*)c->argv[4]->ptr);
                return;
            }
            // 为该槽设置迁移的目标
            server.cluster->migrating_slots_to[slot] = n;
        // 如果是importing
        } else if (!strcasecmp(c->argv[3]->ptr,"importing") && c->argc == 5) {
            // 如果该槽已经是myself节点负责，那么不进行导入
            if (server.cluster->slots[slot] == myself) {
                addReplyErrorFormat(c,
                    "I'm already the owner of hash slot %u",slot);
                return;
            }
            // 获取导入的目标节点
            if ((n = clusterLookupNode(c->argv[4]->ptr)) == NULL) {
                addReplyErrorFormat(c,"I don't know about node %s",
                    (char*)c->argv[3]->ptr);
                return;
            }
            // 为该槽设置导入目标
            server.cluster->importing_slots_from[slot] = n;
        // 如果是stable
        } else if (!strcasecmp(c->argv[3]->ptr,"stable") && c->argc == 4) {
            /* CLUSTER SETSLOT <SLOT> STABLE */
            // 将该槽的迁移和导入目标全部设置为空
            server.cluster->importing_slots_from[slot] = NULL;
            server.cluster->migrating_slots_to[slot] = NULL;
        // 如果是node
        } else if (!strcasecmp(c->argv[3]->ptr,"node") && c->argc == 5) {
            /* CLUSTER SETSLOT <SLOT> NODE <NODE ID> */
            // 查找到目标节点
            clusterNode *n = clusterLookupNode(c->argv[4]->ptr);
            // 目标节点不存在，回复错误信息
            if (!n) {
                addReplyErrorFormat(c,"Unknown node %s",
                    (char*)c->argv[4]->ptr);
                return;
            }
            /* If this hash slot was served by 'myself' before to switch
             * make sure there are no longer local keys for this hash slot. */
            // 如果这个槽已经由myself节点负责，但是目标节点不是myself节点
            if (server.cluster->slots[slot] == myself && n != myself) {
                // 保证该槽中没有键，否则不能指定给其他节点
                if (countKeysInSlot(slot) != 0) {
                    addReplyErrorFormat(c,
                        "Can't assign hashslot %d to a different node "
                        "while I still hold keys for this hash slot.", slot);
                    return;
                }
            }
            /* If this slot is in migrating status but we have no keys
             * for it assigning the slot to another node will clear
             * the migratig status. */
            // 该槽处于被迁移的状态但是该槽中没有键
            if (countKeysInSlot(slot) == 0 &&
                server.cluster->migrating_slots_to[slot])
                // 取消迁移的状态
                server.cluster->migrating_slots_to[slot] = NULL;

            /* If this node was importing this slot, assigning the slot to
             * itself also clears the importing status. */
            // 如果该槽处于导入状态，且目标节点是myself节点
            if (n == myself &&
                server.cluster->importing_slots_from[slot])
            {
                /* This slot was manually migrated, set this node configEpoch
                 * to a new epoch so that the new version can be propagated
                 * by the cluster.
                 *
                 * Note that if this ever results in a collision with another
                 * node getting the same configEpoch, for example because a
                 * failover happens at the same time we close the slot, the
                 * configEpoch collision resolution will fix it assigning
                 * a different epoch to each node. */
                // 手动迁移该槽，将该节点的配置纪元设置为一个新的纪元，以便集群可以传播新的版本。
                // 注意，如果这导致与另一个获得相同配置纪元的节点冲突，例如因为取消槽的同时发生执行故障转移的操作，则配置纪元冲突的解决将修复它，指定不同节点有一个不同的纪元。
                if (clusterBumpConfigEpochWithoutConsensus() == C_OK) {
                    serverLog(LL_WARNING,
                        "configEpoch updated after importing slot %d", slot);
                }
                // 取消槽的导入状态
                server.cluster->importing_slots_from[slot] = NULL;
            }
            clusterDelSlot(slot);
            // 将slot槽指定给n节点
            clusterAddSlot(n,slot);
        } else {
            addReplyError(c,
                "Invalid CLUSTER SETSLOT action or number of arguments");
            return;
        }
        // 更新集群状态和保存配置
        clusterDoBeforeSleep(CLUSTER_TODO_SAVE_CONFIG|CLUSTER_TODO_UPDATE_STATE);
        addReply(c,shared.ok);
    // 处理节点纪元冲突
    } else if (!strcasecmp(c->argv[1]->ptr,"bumpepoch") && c->argc == 2) {
        /* CLUSTER BUMPEPOCH */
        int retval = clusterBumpConfigEpochWithoutConsensus();
        sds reply = sdscatprintf(sdsempty(),"+%s %llu\r\n",
                (retval == C_OK) ? "BUMPED" : "STILL",
                (unsigned long long) myself->configEpoch);
        addReplySds(c,reply);
    // 打印集群的当前信息
    } else if (!strcasecmp(c->argv[1]->ptr,"info") && c->argc == 2) {
        /* CLUSTER INFO */
        char *statestr[] = {"ok","fail","needhelp"};
        int slots_assigned = 0, slots_ok = 0, slots_pfail = 0, slots_fail = 0;
        uint64_t myepoch;
        int j;
        // 统计所有的槽信息
        for (j = 0; j < CLUSTER_SLOTS; j++) {
            clusterNode *n = server.cluster->slots[j];

            if (n == NULL) continue;
            // 计数已指定的槽个数
            slots_assigned++;
            if (nodeFailed(n)) {
                // 计数负责有槽，但是处于下线的节点
                slots_fail++;
            } else if (nodeTimedOut(n)) {
                // 计数负责有槽，但是处于疑似下线的节点
                slots_pfail++;
            } else {
                // 计数负责有槽，但是处于正常状态的节点
                slots_ok++;
            }
        }
        // 如果myself是从节点，则获取myself的主节点配置纪元，否则获取myself主节点的配置纪元
        myepoch = (nodeIsSlave(myself) && myself->slaveof) ?
                  myself->slaveof->configEpoch : myself->configEpoch;
        // sds形式的信息
        sds info = sdscatprintf(sdsempty(),
            "cluster_state:%s\r\n"
            "cluster_slots_assigned:%d\r\n"
            "cluster_slots_ok:%d\r\n"
            "cluster_slots_pfail:%d\r\n"
            "cluster_slots_fail:%d\r\n"
            "cluster_known_nodes:%lu\r\n"
            "cluster_size:%d\r\n"
            "cluster_current_epoch:%llu\r\n"
            "cluster_my_epoch:%llu\r\n"
            "cluster_stats_messages_sent:%lld\r\n"
            "cluster_stats_messages_received:%lld\r\n"
            , statestr[server.cluster->state],
            slots_assigned,
            slots_ok,
            slots_pfail,
            slots_fail,
            dictSize(server.cluster->nodes),
            server.cluster->size,
            (unsigned long long) server.cluster->currentEpoch,
            (unsigned long long) myepoch,
            server.cluster->stats_bus_messages_sent,
            server.cluster->stats_bus_messages_received
        );
        // 添加到回复链表中
        addReplySds(c,sdscatprintf(sdsempty(),"$%lu\r\n",
            (unsigned long)sdslen(info)));
        addReplySds(c,info);
        addReply(c,shared.crlf);
    // 将配置保存到磁盘中
    } else if (!strcasecmp(c->argv[1]->ptr,"saveconfig") && c->argc == 2) {
        // 这个函数写集群节点的配置到磁盘中，且指定了同步操作
        int retval = clusterSaveConfig(1);

        if (retval == 0)
            addReply(c,shared.ok);
        else
            addReplyErrorFormat(c,"error saving the cluster node config: %s",
                strerror(errno));
    // 返回key被hash到哪个槽上
    } else if (!strcasecmp(c->argv[1]->ptr,"keyslot") && c->argc == 3) {
        /* CLUSTER KEYSLOT <key> */
        sds key = c->argv[2]->ptr;
        // 计算key被hash到哪个槽上，并添加到回复中
        addReplyLongLong(c,keyHashSlot(key,sdslen(key)));
    // 计算指定槽上的键的数量
    } else if (!strcasecmp(c->argv[1]->ptr,"countkeysinslot") && c->argc == 3) {
        /* CLUSTER COUNTKEYSINSLOT <slot> */
        long long slot;
        // 获取槽号
        if (getLongLongFromObjectOrReply(c,c->argv[2],&slot,NULL) != C_OK)
            return;
        // 判断曹号的有效性
        if (slot < 0 || slot >= CLUSTER_SLOTS) {
            addReplyError(c,"Invalid slot");
            return;
        }
        // 计算槽中的键个数并添加回复
        addReplyLongLong(c,countKeysInSlot(slot));
    // 打印count个属于slot槽的键
    } else if (!strcasecmp(c->argv[1]->ptr,"getkeysinslot") && c->argc == 4) {
        /* CLUSTER GETKEYSINSLOT <slot> <count> */
        long long maxkeys, slot;
        unsigned int numkeys, j;
        robj **keys;
        // 获取槽号
        if (getLongLongFromObjectOrReply(c,c->argv[2],&slot,NULL) != C_OK)
            return;
        // 获取打印键的个数
        if (getLongLongFromObjectOrReply(c,c->argv[3],&maxkeys,NULL)
            != C_OK)
            return;
        // 判断槽号和个数是否非法
        if (slot < 0 || slot >= CLUSTER_SLOTS || maxkeys < 0) {
            addReplyError(c,"Invalid slot or number of keys");
            return;
        }
        // 分配保存键的空间
        keys = zmalloc(sizeof(robj*)*maxkeys);
        // 将count个键保存到数组中
        numkeys = getKeysInSlot(slot, keys, maxkeys);
        // 添加回复键的个数
        addReplyMultiBulkLen(c,numkeys);
        // 添加回复每一个键
        for (j = 0; j < numkeys; j++) addReplyBulk(c,keys[j]);
        zfree(keys);
    // 从集群中删除<NODE ID>指定的节点
    } else if (!strcasecmp(c->argv[1]->ptr,"forget") && c->argc == 3) {
        /* CLUSTER FORGET <NODE ID> */
        // 根据<NODE ID>查找节点ilil
        clusterNode *n = clusterLookupNode(c->argv[2]->ptr);
        // 没找到
        if (!n) {
            addReplyErrorFormat(c,"Unknown node %s", (char*)c->argv[2]->ptr);
            return;
        // 不能删除myself
        } else if (n == myself) {
            addReplyError(c,"I tried hard but I can't forget myself...");
            return;
        // 如果myself是从节点，且myself节点的主节点是被删除的目标键，回复错误信息
        } else if (nodeIsSlave(myself) && myself->slaveof == n) {
            addReplyError(c,"Can't forget my master!");
            return;
        }
        // 将n添加到黑名单中
        clusterBlacklistAddNode(n);
        // 从集群中删除该节点
        clusterDelNode(n);
        // 更新状态和保存配置
        clusterDoBeforeSleep(CLUSTER_TODO_UPDATE_STATE|
                             CLUSTER_TODO_SAVE_CONFIG);
        addReply(c,shared.ok);
    // 将当前节点设置为 <NODE ID> 指定的节点的从节点
    } else if (!strcasecmp(c->argv[1]->ptr,"replicate") && c->argc == 3) {
        /* CLUSTER REPLICATE <NODE ID> */
        // 获取指定的主节点
        clusterNode *n = clusterLookupNode(c->argv[2]->ptr);

        /* Lookup the specified node in our table. */
        // 没找到该节点
        if (!n) {
            addReplyErrorFormat(c,"Unknown node %s", (char*)c->argv[2]->ptr);
            return;
        }

        /* I can't replicate myself. */
        // 不能复制myself自己
        if (n == myself) {
            addReplyError(c,"Can't replicate myself");
            return;
        }

        /* Can't replicate a slave. */
        // 如果该节点是从节点，回复错误
        if (nodeIsSlave(n)) {
            addReplyError(c,"I can only replicate a master, not a slave.");
            return;
        }

        /* If the instance is currently a master, it should have no assigned
         * slots nor keys to accept to replicate some other node.
         * Slaves can switch to another master without issues. */
        // 如果myself是主节点，并且负责有一些槽或者数据库中没有键
        if (nodeIsMaster(myself) &&
            (myself->numslots != 0 || dictSize(server.db[0].dict) != 0)) {
            addReplyError(c,
                "To set a master the node must be empty and "
                "without assigned slots.");
            return;
        }

        /* Set the master. */
        // 将该节点设置为myself节点的主节点
        clusterSetMaster(n);
        // 更新状态和保存配置
        clusterDoBeforeSleep(CLUSTER_TODO_UPDATE_STATE|CLUSTER_TODO_SAVE_CONFIG);
        addReply(c,shared.ok);
    // 打印给定主节点的所有从节点信息
    } else if (!strcasecmp(c->argv[1]->ptr,"slaves") && c->argc == 3) {
        /* CLUSTER SLAVES <NODE ID> */
        // 查找该主节点
        clusterNode *n = clusterLookupNode(c->argv[2]->ptr);
        int j;

        /* Lookup the specified node in our table. */
        // 没找到
        if (!n) {
            addReplyErrorFormat(c,"Unknown node %s", (char*)c->argv[2]->ptr);
            return;
        }
        // 该节点不能是从节点
        if (nodeIsSlave(n)) {
            addReplyError(c,"The specified node is not a master");
            return;
        }
        // 添加回复从节点的个数
        addReplyMultiBulkLen(c,n->numslaves);
        // 添加所有从节点的信息
        for (j = 0; j < n->numslaves; j++) {
            sds ni = clusterGenNodeDescription(n->slaves[j]);
            addReplyBulkCString(c,ni);
            sdsfree(ni);
        }
    // 打印指定节点的故障报告的个数
    } else if (!strcasecmp(c->argv[1]->ptr,"count-failure-reports") &&
               c->argc == 3)
    {
        /* CLUSTER COUNT-FAILURE-REPORTS <NODE ID> */
        // 查找指定的节点
        clusterNode *n = clusterLookupNode(c->argv[2]->ptr);

        if (!n) {
            addReplyErrorFormat(c,"Unknown node %s", (char*)c->argv[2]->ptr);
            return;
        // 回复该节点的故障报告个数
        } else {
            addReplyLongLong(c,clusterNodeFailureReportsCount(n));
        }
    // 执行手动故障转移
    } else if (!strcasecmp(c->argv[1]->ptr,"failover") &&
               (c->argc == 2 || c->argc == 3))
    {
        /* CLUSTER FAILOVER [FORCE|TAKEOVER] */
        int force = 0, takeover = 0;

        if (c->argc == 3) {
            if (!strcasecmp(c->argv[2]->ptr,"force")) {
                // 设置当主节点下线执行手动故障转移的标识
                force = 1;
            } else if (!strcasecmp(c->argv[2]->ptr,"takeover")) {
                // 设置执行手动故障转移不需要集群一致的标识
                takeover = 1;
                force = 1; /* Takeover also implies force. */
            } else {
                addReply(c,shared.syntaxerr);
                return;
            }
        }

        /* Check preconditions. */
        // myself必须是从节点
        if (nodeIsMaster(myself)) {
            addReplyError(c,"You should send CLUSTER FAILOVER to a slave");
            return;
        // myself是从节点但必须存在从属的主节点
        } else if (myself->slaveof == NULL) {
            addReplyError(c,"I'm a slave but my master is unknown to me");
            return;
        // 主节点下线但是没有指定force标识
        } else if (!force &&
                   (nodeFailed(myself->slaveof) ||
                    myself->slaveof->link == NULL))
        {
            addReplyError(c,"Master is down or failed, "
                            "please use CLUSTER FAILOVER FORCE");
            return;
        }
        // 重置与手动故障转移的状态
        resetManualFailover();
        // 设置手动故障最大的执行时间
        server.cluster->mf_end = mstime() + CLUSTER_MF_TIMEOUT;
        // 如果设置执行手动故障转移不需要集群一致的标识
        if (takeover) {
            /* A takeover does not perform any initial check. It just
             * generates a new configuration epoch for this node without
             * consensus, claims the master's slots, and broadcast the new
             * configuration. */
            serverLog(LL_WARNING,"Taking over the master (user request).");
            // 处理节点纪元冲突
            clusterBumpConfigEpochWithoutConsensus();
            // 实现手动故障转移
            clusterFailoverReplaceYourMaster();
        // 如果设置当主节点下线执行手动故障转移的标识
        } else if (force) {
            /* If this is a forced failover, we don't need to talk with our
             * master to agree about the offset. We just failover taking over
             * it without coordination. */
            serverLog(LL_WARNING,"Forced failover user request accepted.");
            // 设置手动故障转移开始的标识
            server.cluster->mf_can_start = 1;
        } else {
            serverLog(LL_WARNING,"Manual failover user request accepted.");
            // 发送一个MFSTART消息个指定节点，和主节点比较复制偏移量是否相同
            clusterSendMFStart(myself->slaveof);
        }
        addReply(c,shared.ok);
    // 设置配置纪元
    } else if (!strcasecmp(c->argv[1]->ptr,"set-config-epoch") && c->argc == 3)
    {
        /* CLUSTER SET-CONFIG-EPOCH <epoch>
         *
         * The user is allowed to set the config epoch only when a node is
         * totally fresh: no config epoch, no other known node, and so forth.
         * This happens at cluster creation time to start with a cluster where
         * every node has a different node ID, without to rely on the conflicts
         * resolution system which is too slow when a big cluster is created. */
        long long epoch;
        // 获取指定的纪元
        if (getLongLongFromObjectOrReply(c,c->argv[2],&epoch,NULL) != C_OK)
            return;

        if (epoch < 0) {
            addReplyErrorFormat(c,"Invalid config epoch specified: %lld",epoch);
        // 指定配置纪元只能当集群中节点数小于1
        } else if (dictSize(server.cluster->nodes) > 1) {
            addReplyError(c,"The user can assign a config epoch only when the "
                            "node does not know any other node.");
        // myself节点的配置纪元已经不为0
        } else if (myself->configEpoch != 0) {
            addReplyError(c,"Node config epoch is already non-zero");
        } else {
            // 设置myself节点的配置纪元
            myself->configEpoch = epoch;
            serverLog(LL_WARNING,
                "configEpoch set to %llu via CLUSTER SET-CONFIG-EPOCH",
                (unsigned long long) myself->configEpoch);
            // 更新集群的当前纪元
            if (server.cluster->currentEpoch < (uint64_t)epoch)
                server.cluster->currentEpoch = epoch;
            /* No need to fsync the config here since in the unlucky event
             * of a failure to persist the config, the conflict resolution code
             * will assign an unique config to this node. */
            clusterDoBeforeSleep(CLUSTER_TODO_UPDATE_STATE|
                                 CLUSTER_TODO_SAVE_CONFIG);
            addReply(c,shared.ok);
        }
    // 重置集群
    } else if (!strcasecmp(c->argv[1]->ptr,"reset") &&
               (c->argc == 2 || c->argc == 3))
    {
        /* CLUSTER RESET [SOFT|HARD] */
        int hard = 0;

        /* Parse soft/hard argument. Default is soft. */
        if (c->argc == 3) {
            if (!strcasecmp(c->argv[2]->ptr,"hard")) {
                // 硬重置标识
                hard = 1;
            } else if (!strcasecmp(c->argv[2]->ptr,"soft")) {
                // 软重置标识
                hard = 0;
            } else {
                addReply(c,shared.syntaxerr);
                return;
            }
        }

        /* Slaves can be reset while containing data, but not master nodes
         * that must be empty. */
        // 如果myself是主节点，且字典的大小部位0，不能进行重置
        if (nodeIsMaster(myself) && dictSize(c->db->dict) != 0) {
            addReplyError(c,"CLUSTER RESET can't be called with "
                            "master nodes containing keys");
            return;
        }
        // 重置集群
        clusterReset(hard);
        addReply(c,shared.ok);
    } else {
        addReplyError(c,"Wrong CLUSTER subcommand or number of arguments");
    }
}

/* -----------------------------------------------------------------------------
 * DUMP, RESTORE and MIGRATE commands
 * -------------------------------------------------------------------------- */

/* Generates a DUMP-format representation of the object 'o', adding it to the
 * io stream pointed by 'rio'. This function can't fail. */
// 创建对象o的一个序列化表示，并将它添加到rio指针指向的io流中
void createDumpPayload(rio *payload, robj *o) {
    unsigned char buf[2];
    uint64_t crc;

    /* Serialize the object in a RDB-like format. It consist of an object type
     * byte followed by the serialized object. This is understood by RESTORE. */
    // 初始化缓冲区对象payload并设置缓冲区的地址
    rioInitWithBuffer(payload,sdsempty());
    serverAssert(rdbSaveObjectType(payload,o));
    serverAssert(rdbSaveObject(payload,o));

    /* Write the footer, this is how it looks like:
     * ----------------+---------------------+---------------+
     * ... RDB payload | 2 bytes RDB version | 8 bytes CRC64 |
     * ----------------+---------------------+---------------+
     * RDB version and CRC are both in little endian.
     */

    /* RDB version */
    // 两字节的RDB版本
    buf[0] = RDB_VERSION & 0xff;
    buf[1] = (RDB_VERSION >> 8) & 0xff;
    // 写到rio对象的缓冲区中
    payload->io.buffer.ptr = sdscatlen(payload->io.buffer.ptr,buf,2);

    /* CRC64 */
    // 写入CRC64的校验和
    crc = crc64(0,(unsigned char*)payload->io.buffer.ptr,
                sdslen(payload->io.buffer.ptr));
    memrev64ifbe(&crc);
    payload->io.buffer.ptr = sdscatlen(payload->io.buffer.ptr,&crc,8);
}

/* Verify that the RDB version of the dump payload matches the one of this Redis
 * instance and that the checksum is ok.
 * If the DUMP payload looks valid C_OK is returned, otherwise C_ERR
 * is returned. */
// 检查输入的dump数据中，RDB版本是否和当前Redis实例所使用的RDB版本相同，并检查校验和是否正确
// 检查正常返回C_OK，否则返回C_ERR
int verifyDumpPayload(unsigned char *p, size_t len) {
    unsigned char *footer;
    uint16_t rdbver;
    uint64_t crc;

    /* At least 2 bytes of RDB version and 8 of CRC64 should be present. */
    // 最少包含2个字节的RDB版本，以及8个字节的CRC64校验和，共10个字节
    if (len < 10) return C_ERR;
    // 指向后10个字节的起始地址
    footer = p+(len-10);

    /* Verify RDB version */
    //获取RDB版本
    rdbver = (footer[1] << 8) | footer[0];
    // 检验版本
    if (rdbver > RDB_VERSION) return C_ERR;

    /* Verify CRC64 */
    // 验证CRC64校验和
    crc = crc64(0,p,len-8);
    memrev64ifbe(&crc);
    return (memcmp(&crc,footer+2,8) == 0) ? C_OK : C_ERR;
}

/* DUMP keyname
 * DUMP is actually not used by Redis Cluster but it is the obvious
 * complement of RESTORE and can be useful for different applications. */
// DUMP keyname 命令的实现，将一个键的值序列化，然后传送到目标实例，目标实例在使用RESTORE对数据记性反序列化
void dumpCommand(client *c) {
    robj *o, *dumpobj;
    rio payload;

    /* Check if the key is here. */
    // 以读操作取出key的值对象
    if ((o = lookupKeyRead(c->db,c->argv[1])) == NULL) {
        addReply(c,shared.nullbulk);
        return;
    }

    /* Create the DUMP encoded representation. */
    // 将值对象进行序列化，保存在payload的缓冲区中
    createDumpPayload(&payload,o);

    /* Transfer to the client */
    // 将payload的缓冲区的序列化数据创建对象，添加到回复中
    dumpobj = createObject(OBJ_STRING,payload.io.buffer.ptr);
    addReplyBulk(c,dumpobj);
    decrRefCount(dumpobj);
    return;
}

/* RESTORE key ttl serialized-value [REPLACE] */
// RESTORE 命令的实现，实现对一个序列进行反序列化
void restoreCommand(client *c) {
    long long ttl;
    rio payload;
    int j, type, replace = 0;
    robj *obj;

    /* Parse additional options */
    // 解析REPLACE选项，如果指定了该选项，设置replace标识
    for (j = 4; j < c->argc; j++) {
        if (!strcasecmp(c->argv[j]->ptr,"replace")) {
            replace = 1;
        } else {
            addReply(c,shared.syntaxerr);
            return;
        }
    }

    /* Make sure this key does not already exist here... */
    // 如果没有指定替换标识，但是键存在，回复一个错误
    if (!replace && lookupKeyWrite(c->db,c->argv[1]) != NULL) {
        addReply(c,shared.busykeyerr);
        return;
    }

    /* Check if the TTL value makes sense */
    // 获取生存时间
    if (getLongLongFromObjectOrReply(c,c->argv[2],&ttl,NULL) != C_OK) {
        return;
    } else if (ttl < 0) {
        addReplyError(c,"Invalid TTL value, must be >= 0");
        return;
    }

    /* Verify RDB version and data checksum. */
    // 验证RDB版本和数据校验和
    if (verifyDumpPayload(c->argv[3]->ptr,sdslen(c->argv[3]->ptr)) == C_ERR)
    {
        addReplyError(c,"DUMP payload version or checksum are wrong");
        return;
    }
    // 初始化缓冲区对象payload并设置缓冲区的地址，读出了序列化数据到缓冲区中
    rioInitWithBuffer(&payload,c->argv[3]->ptr);
    // 类型错误
    if (((type = rdbLoadObjectType(&payload)) == -1) ||
        ((obj = rdbLoadObject(type,&payload)) == NULL))
    {
        addReplyError(c,"Bad data format");
        return;
    }

    /* Remove the old key if needed. */
    // 如果指定了替代的标识，那么删除旧的键
    if (replace) dbDelete(c->db,c->argv[1]);

    /* Create the key and set the TTL if any */
    // 添加键值对到数据库中
    dbAdd(c->db,c->argv[1],obj);
    // 设置生存时间
    if (ttl) setExpire(c->db,c->argv[1],mstime()+ttl);
    signalModifiedKey(c->db,c->argv[1]);
    addReply(c,shared.ok);
    server.dirty++;
}

/* MIGRATE socket cache implementation.
 *  迁移套接字缓存实现
 * We take a map between host:ip and a TCP socket that we used to connect
 * to this instance in recent time.
 * This sockets are closed when the max number we cache is reached, and also
 * in serverCron() when they are around for more than a few seconds. */

// 我们做一个host:ip和TCP socket的映射，TCP socket用来连接实例
// 该套接字在缓存达到上线时，被释放。而且在serverCron()函数中，也会定期删除一个过期的套接字

// 最大的缓存数
#define MIGRATE_SOCKET_CACHE_ITEMS 64 /* max num of items in the cache. */
// 缓存连接的生存时间10s
#define MIGRATE_SOCKET_CACHE_TTL 10 /* close cached sockets after 10 sec. */

typedef struct migrateCachedSocket {
    // TCP套接字
    int fd;
    // 上一次还原键的数据库ID
    long last_dbid;
    // 上一次使用的时间
    time_t last_use_time;
} migrateCachedSocket;

/* Return a migrateCachedSocket containing a TCP socket connected with the
 * target instance, possibly returning a cached one.
 *
 * This function is responsible of sending errors to the client if a
 * connection can't be established. In this case -1 is returned.
 * Otherwise on success the socket is returned, and the caller should not
 * attempt to free it after usage.
 *
 * If the caller detects an error while using the socket, migrateCloseSocket()
 * should be called so that the connection will be created from scratch
 * the next time. */
// 返回一个包含连接目标实例的TCP套接字的migrateCachedSocket结构，有可能返回一个缓存套接字
// 如果一个连接无法建立，函数负责回复错误给client，返回-1。否则返回套接字结构，调用者不应该释放该结构
// 当调用者在使用套接字时发现一个错误，应该调用migrateCloseSocket()函数来关闭套接字以便下次能够创建相同地址的连接
migrateCachedSocket* migrateGetSocket(client *c, robj *host, robj *port, long timeout) {
    int fd;
    sds name = sdsempty();
    migrateCachedSocket *cs;

    /* Check if we have an already cached socket for this ip:port pair. */
    // 根据参数创建sds形式的ip:port
    name = sdscatlen(name,host->ptr,sdslen(host->ptr));
    name = sdscatlen(name,":",1);
    name = sdscatlen(name,port->ptr,sdslen(port->ptr));
    // 在迁移缓存套接字的字典查找是否缓存过相同地址的连接
    cs = dictFetchValue(server.migrate_cached_sockets,name);
    if (cs) {
        sdsfree(name);
        // 如果可以找到，那么设置最近一次使用的时间，并返回该套接字结构
        cs->last_use_time = server.unixtime;
        return cs;
    }

    /* No cached socket, create one. */
    // 没哟缓存的套接字，创建一个，如果缓存套接字个数已经达到10个上线
    if (dictSize(server.migrate_cached_sockets) == MIGRATE_SOCKET_CACHE_ITEMS) {
        /* Too many items, drop one at random. */
        // 随机删除一个
        dictEntry *de = dictGetRandomKey(server.migrate_cached_sockets);
        cs = dictGetVal(de);
        close(cs->fd);
        zfree(cs);
        dictDelete(server.migrate_cached_sockets,dictGetKey(de));
    }

    /* Create the socket */
    // 创建一个新的连接，获得一个套接字
    fd = anetTcpNonBlockConnect(server.neterr,c->argv[1]->ptr,
                                atoi(c->argv[2]->ptr));
    // 连接创建失败
    if (fd == -1) {
        sdsfree(name);
        addReplyErrorFormat(c,"Can't connect to target node: %s",
            server.neterr);
        return NULL;
    }
    // 关闭nagle延迟算法
    anetEnableTcpNoDelay(server.neterr,fd);

    /* Check if it connects within the specified timeout. */
    // 检查连接在指定的timeout时间内是否有可写时间发生
    if ((aeWait(fd,AE_WRITABLE,timeout) & AE_WRITABLE) == 0) {
        sdsfree(name);
        addReplySds(c,
            sdsnew("-IOERR error or timeout connecting to the client\r\n"));
        close(fd);
        return NULL;
    }

    /* Add to the cache and return it to the caller. */
    // 添加到缓存中，并且将套接字结构返回给调用者
    cs = zmalloc(sizeof(*cs));
    cs->fd = fd;
    cs->last_dbid = -1;
    cs->last_use_time = server.unixtime;
    dictAdd(server.migrate_cached_sockets,name,cs);
    return cs;
}

/* Free a migrate cached connection. */
// 释放一个迁移缓存连接
void migrateCloseSocket(robj *host, robj *port) {
    sds name = sdsempty();
    migrateCachedSocket *cs;
    // 根据参数创建sds形式的ip:port
    name = sdscatlen(name,host->ptr,sdslen(host->ptr));
    name = sdscatlen(name,":",1);
    name = sdscatlen(name,port->ptr,sdslen(port->ptr));
    // 在迁移缓存套接字的字典查找是否缓存过相同地址的连接
    cs = dictFetchValue(server.migrate_cached_sockets,name);
    if (!cs) {
        sdsfree(name);
        return;
    }
    // 关闭连接，删除缓存
    close(cs->fd);
    zfree(cs);
    dictDelete(server.migrate_cached_sockets,name);
    sdsfree(name);
}

// 删除过期的连接
void migrateCloseTimedoutSockets(void) {
    dictIterator *di = dictGetSafeIterator(server.migrate_cached_sockets);
    dictEntry *de;
    // 遍历迁移缓存套接字的字典中的所有连接
    while((de = dictNext(di)) != NULL) {
        migrateCachedSocket *cs = dictGetVal(de);
        // 如果距离最近一次使用套接字连接已经超过10s，那么释放连接并且删除缓存
        if ((server.unixtime - cs->last_use_time) > MIGRATE_SOCKET_CACHE_TTL) {
            close(cs->fd);
            zfree(cs);
            dictDelete(server.migrate_cached_sockets,dictGetKey(de));
        }
    }
    dictReleaseIterator(di);
}

/* MIGRATE host port key dbid timeout [COPY | REPLACE]
 *
 * On in the multiple keys form:
 *
 * MIGRATE host port "" dbid timeout [COPY | REPLACE] KEYS key1 key2 ... keyN */
// MIGRATE 命令实现
void migrateCommand(client *c) {
    migrateCachedSocket *cs;
    int copy, replace, j;
    long timeout;
    long dbid;
    robj **ov = NULL; /* Objects to migrate. */
    robj **kv = NULL; /* Key names. */
    robj **newargv = NULL; /* Used to rewrite the command as DEL ... keys ... */
    rio cmd, payload;
    int may_retry = 1;
    int write_error = 0;
    int argv_rewritten = 0;

    /* To support the KEYS option we need the following additional state. */
    int first_key = 3; /* Argument index of the first key. */
    int num_keys = 1;  /* By default only migrate the 'key' argument. */

    /* Initialization */
    copy = 0;
    replace = 0;

    /* Parse additional options */
    // 解析附加项
    for (j = 6; j < c->argc; j++) {
        // copy项：不删除源节点上的key
        if (!strcasecmp(c->argv[j]->ptr,"copy")) {
            copy = 1;
        // replace项：替换目标节点上已存在的key
        } else if (!strcasecmp(c->argv[j]->ptr,"replace")) {
            replace = 1;
        // keys项：指定多个迁移的键
        } else if (!strcasecmp(c->argv[j]->ptr,"keys")) {
            // 第三个参数必须是空字符串""
            if (sdslen(c->argv[3]->ptr) != 0) {
                addReplyError(c,
                    "When using MIGRATE KEYS option, the key argument"
                    " must be set to the empty string");
                return;
            }
            // 指定要迁移的键，第一个键的下标
            first_key = j+1;
            // 键的个数
            num_keys = c->argc - j - 1;
            break; /* All the remaining args are keys. */
        } else {
            addReply(c,shared.syntaxerr);
            return;
        }
    }

    /* Sanity check */
    // 参数有效性检查
    if (getLongFromObjectOrReply(c,c->argv[5],&timeout,NULL) != C_OK ||
        getLongFromObjectOrReply(c,c->argv[4],&dbid,NULL) != C_OK)
    {
        return;
    }
    if (timeout <= 0) timeout = 1000;

    /* Check if the keys are here. If at least one key is to migrate, do it
     * otherwise if all the keys are missing reply with "NOKEY" to signal
     * the caller there was nothing to migrate. We don't return an error in
     * this case, since often this is due to a normal condition like the key
     * expiring in the meantime. */
    // 检查key是否存在，至少有一个key要迁移，否则如果所有的key都不存在，回复一个"NOKEY"通知调用者，没有要迁移的键
    ov = zrealloc(ov,sizeof(robj*)*num_keys);
    kv = zrealloc(kv,sizeof(robj*)*num_keys);
    int oi = 0;
    // 遍历所有指定的键
    for (j = 0; j < num_keys; j++) {
        // 以读操作取出key的值对象，保存在ov中
        if ((ov[oi] = lookupKeyRead(c->db,c->argv[first_key+j])) != NULL) {
            // 将存在的key保存到kv中
            kv[oi] = c->argv[first_key+j];
            // 计数存在的键的个数
            oi++;
        }
    }
    num_keys = oi;
    // 没有键存在，迁移失败，返回"+NOKEY"
    if (num_keys == 0) {
        zfree(ov); zfree(kv);
        addReplySds(c,sdsnew("+NOKEY\r\n"));
        return;
    }

try_again:
    write_error = 0;

    /* Connect */
    // 返回一个包含连接目标实例的TCP套接字的migrateCachedSocket结构，有可能返回一个缓存套接字
    cs = migrateGetSocket(c,c->argv[1],c->argv[2],timeout);
    if (cs == NULL) {
        zfree(ov); zfree(kv);
        return; /* error sent to the client by migrateGetSocket() */
    }
    // 初始化缓冲区对象cmd，用来构建SELECT命令
    rioInitWithBuffer(&cmd,sdsempty());

    /* Send the SELECT command if the current DB is not already selected. */
    // 创建一个SELECT命令，如果上一次要还原到的数据库ID和这次的不相同
    int select = cs->last_dbid != dbid; /* Should we emit SELECT? */
    // 则需要创建一个SELECT命令
    if (select) {
        serverAssertWithInfo(c,NULL,rioWriteBulkCount(&cmd,'*',2));
        serverAssertWithInfo(c,NULL,rioWriteBulkString(&cmd,"SELECT",6));
        serverAssertWithInfo(c,NULL,rioWriteBulkLongLong(&cmd,dbid));
    }

    /* Create RESTORE payload and generate the protocol to call the command. */
    // 将所有的键值对进行加工
    for (j = 0; j < num_keys; j++) {
        long long ttl = 0;
        // 获取当前key的过期时间
        long long expireat = getExpire(c->db,kv[j]);

        if (expireat != -1) {
            // 计算key的生存时间
            ttl = expireat-mstime();
            if (ttl < 1) ttl = 1;
        }
        // 以"*<count>\r\n"格式为写如一个int整型的count
        // 如果指定了replace，则count值为5，否则为4
        // 写回复的个数
        serverAssertWithInfo(c,NULL,rioWriteBulkCount(&cmd,'*',replace ? 5 : 4));
        // 如果运行在进群模式下，写回复一个"RESTORE-ASKING"
        if (server.cluster_enabled)
            serverAssertWithInfo(c,NULL,
                rioWriteBulkString(&cmd,"RESTORE-ASKING",14));
        // 如果不是集群模式下，则写回复一个"RESTORE"
        else
            serverAssertWithInfo(c,NULL,rioWriteBulkString(&cmd,"RESTORE",7));
        // 检测键对象的编码
        serverAssertWithInfo(c,NULL,sdsEncodedObject(kv[j]));
        // 写回复一个键
        serverAssertWithInfo(c,NULL,rioWriteBulkString(&cmd,kv[j]->ptr,
                sdslen(kv[j]->ptr)));
        // 写回复一个键的生存时间
        serverAssertWithInfo(c,NULL,rioWriteBulkLongLong(&cmd,ttl));

        /* Emit the payload argument, that is the serialized object using
         * the DUMP format. */
        // 将值对象序列化
        createDumpPayload(&payload,ov[j]);
        // 将序列化的值对象写到回复中
        serverAssertWithInfo(c,NULL,
            rioWriteBulkString(&cmd,payload.io.buffer.ptr,
                               sdslen(payload.io.buffer.ptr)));
        sdsfree(payload.io.buffer.ptr);

        /* Add the REPLACE option to the RESTORE command if it was specified
         * as a MIGRATE option. */
        // 如果指定了replace，还要写回复一个REPLACE选项
        if (replace)
            serverAssertWithInfo(c,NULL,rioWriteBulkString(&cmd,"REPLACE",7));
    }

    /* Transfer the query to the other node in 64K chunks. */
    errno = 0;
    {
        sds buf = cmd.io.buffer.ptr;
        size_t pos = 0, towrite;
        int nwritten = 0;
        // 将rio缓冲区的数据写到TCP套接字中，同步写，如果超过timeout时间，则返回错误
        while ((towrite = sdslen(buf)-pos) > 0) {
            // 一次写64k大小的数据
            towrite = (towrite > (64*1024) ? (64*1024) : towrite);
            nwritten = syncWrite(cs->fd,buf+pos,towrite,timeout);
            if (nwritten != (signed)towrite) {
                write_error = 1;
                goto socket_err;
            }
            // 记录已写的大小
            pos += nwritten;
        }
    }
    // 读取命令的回复
    char buf1[1024]; /* Select reply. */
    char buf2[1024]; /* Restore reply. */

    /* Read the SELECT reply if needed. */
    // 如果指定了select，读取该命令的回复
    if (select && syncReadLine(cs->fd, buf1, sizeof(buf1), timeout) <= 0)
        goto socket_err;

    /* Read the RESTORE replies. */
    int error_from_target = 0;
    int socket_error = 0;
    int del_idx = 1; /* Index of the key argument for the replicated DEL op. */
    // 没有指定copy选项，分配一个新的参数列表空间
    if (!copy) newargv = zmalloc(sizeof(robj*)*(num_keys+1));
    // 读取每一个键的回复
    for (j = 0; j < num_keys; j++) {
        // 同步读取每一个键的回复，超时timeout
        if (syncReadLine(cs->fd, buf2, sizeof(buf2), timeout) <= 0) {
            socket_error = 1;
            break;
        }
        // 如果指定了select，检查select的回复
        if ((select && buf1[0] == '-') || buf2[0] == '-') {
            /* On error assume that last_dbid is no longer valid. */
            // 如果select回复错误，那么last_dbid就是无效的了
            if (!error_from_target) {
                cs->last_dbid = -1;
                addReplyErrorFormat(c,"Target instance replied with error: %s",
                    (select && buf1[0] == '-') ? buf1+1 : buf2+1);
                error_from_target = 1;
            }
        } else {
            // 没有指定copy选项，要删除源节点的键
            if (!copy) {
                /* No COPY option: remove the local key, signal the change. */
                // 删除源节点的键
                dbDelete(c->db,kv[j]);
                // 发送信息
                signalModifiedKey(c->db,kv[j]);
                // 更新脏键个数
                server.dirty++;

                /* Populate the argument vector to replace the old one. */
                // 设置删除键的列表
                newargv[del_idx++] = kv[j];
                incrRefCount(kv[j]);
            }
        }
    }

    /* On socket error, if we want to retry, do it now before rewriting the
     * command vector. We only retry if we are sure nothing was processed
     * and we failed to read the first reply (j == 0 test). */
    // 套接字错误，第一个键就错误，可以进行重试
    if (!error_from_target && socket_error && j == 0 && may_retry &&
        errno != ETIMEDOUT)
    {
        goto socket_err; /* A retry is guaranteed because of tested conditions.*/
    }

    /* On socket errors, close the migration socket now that we still have
     * the original host/port in the ARGV. Later the original command may be
     * rewritten to DEL and will be too later. */
    // 套接字错误，关闭迁移连接
    if (socket_error) migrateCloseSocket(c->argv[1],c->argv[2]);
    // 没有指定copy选项
    if (!copy) {
        /* Translate MIGRATE as DEL for replication/AOF. Note that we do
         * this only for the keys for which we received an acknowledgement
         * from the receiving Redis server, by using the del_idx index. */
        // 如果删除了键
        if (del_idx > 1) {
            // 创建一个DEL命令，用来发送到AOF和从节点中
            newargv[0] = createStringObject("DEL",3);
            /* Note that the following call takes ownership of newargv. */
            // 用指定的newargv参数列表替代client的参数列表
            replaceClientCommandVector(c,del_idx,newargv);
            argv_rewritten = 1;
        } else {
            /* No key transfer acknowledged, no need to rewrite as DEL. */
            zfree(newargv);
        }
        newargv = NULL; /* Make it safe to call zfree() on it in the future. */
    }

    /* If we are here and a socket error happened, we don't want to retry.
     * Just signal the problem to the client, but only do it if we did not
     * already queue a different error reported by the destination server. */
    // 执行到这里，如果还没有跳到socket_err，那么关闭重试的标志，跳转到socket_err
    if (!error_from_target && socket_error) {
        may_retry = 0;
        goto socket_err;
    }
    // 不是目标节点的回复错误
    if (!error_from_target) {
        /* Success! Update the last_dbid in migrateCachedSocket, so that we can
         * avoid SELECT the next time if the target DB is the same. Reply +OK.
         *
         * Note: If we reached this point, even if socket_error is true
         * still the SELECT command succeeded (otherwise the code jumps to
         * socket_err label. */
        // 更新最近一次使用的数据库ID
        cs->last_dbid = dbid;
        addReply(c,shared.ok);
    } else {
        /* On error we already sent it in the for loop above, and set
         * the curretly selected socket to -1 to force SELECT the next time. */
    }
    // 释放空间
    sdsfree(cmd.io.buffer.ptr);
    zfree(ov); zfree(kv); zfree(newargv);
    return;

/* On socket errors we try to close the cached socket and try again.
 * It is very common for the cached socket to get closed, if just reopening
 * it works it's a shame to notify the error to the caller. */
socket_err:
    /* Cleanup we want to perform in both the retry and no retry case.
     * Note: Closing the migrate socket will also force SELECT next time. */
    sdsfree(cmd.io.buffer.ptr);

    /* If the command was rewritten as DEL and there was a socket error,
     * we already closed the socket earlier. While migrateCloseSocket()
     * is idempotent, the host/port arguments are now gone, so don't do it
     * again. */
    // 如果没有重写client参数列表，关闭连接，因为要保持一致性
    if (!argv_rewritten) migrateCloseSocket(c->argv[1],c->argv[2]);
    zfree(newargv);
    newargv = NULL; /* This will get reallocated on retry. */

    /* Retry only if it's not a timeout and we never attempted a retry
     * (or the code jumping here did not set may_retry to zero). */
    // 如果可以重试，跳转到try_again
    if (errno != ETIMEDOUT && may_retry) {
        may_retry = 0;
        goto try_again;
    }

    /* Cleanup we want to do if no retry is attempted. */
    zfree(ov); zfree(kv);
    addReplySds(c,
        sdscatprintf(sdsempty(),
            "-IOERR error or timeout %s to target instance\r\n",
            write_error ? "writing" : "reading"));
    return;
}

/* -----------------------------------------------------------------------------
 * Cluster functions related to serving / redirecting clients
 * -------------------------------------------------------------------------- */

/* The ASKING command is required after a -ASK redirection.
 * The client should issue ASKING before to actually send the command to
 * the target instance. See the Redis Cluster specification for more
 * information. */
// 客户端接到-ASK命令后，需要发送ASKING命令
void askingCommand(client *c) {
    // 必须在集群模式下运行
    if (server.cluster_enabled == 0) {
        addReplyError(c,"This instance has cluster support disabled");
        return;
    }
    // 设置CLIENT_ASKING标识，表示转向到其他
    c->flags |= CLIENT_ASKING;
    addReply(c,shared.ok);
}

/* The READONLY command is used by clients to enter the read-only mode.
 * In this mode slaves will not redirect clients as long as clients access
 * with read-only commands to keys that are served by the slave's master. */
// READONLY命令实现，用在client进入只读模式
void readonlyCommand(client *c) {
    // 必须在集群模式下运行
    if (server.cluster_enabled == 0) {
        addReplyError(c,"This instance has cluster support disabled");
        return;
    }
    // 设置READONLY标识
    c->flags |= CLIENT_READONLY;
    addReply(c,shared.ok);
}

/* The READWRITE command just clears the READONLY command state. */
// READWRITE命令实现
void readwriteCommand(client *c) {
    // 清除只读标识
    c->flags &= ~CLIENT_READONLY;
    addReply(c,shared.ok);
}

/* Return the pointer to the cluster node that is able to serve the command.
 * For the function to succeed the command should only target either:
 *
 * 1) A single key (even multiple times like LPOPRPUSH mylist mylist).
 * 2) Multiple keys in the same hash slot, while the slot is stable (no
 *    resharding in progress).
 *
 * On success the function returns the node that is able to serve the request.
 * If the node is not 'myself' a redirection must be perfomed. The kind of
 * redirection is specified setting the integer passed by reference
 * 'error_code', which will be set to CLUSTER_REDIR_ASK or
 * CLUSTER_REDIR_MOVED.
 *
 * When the node is 'myself' 'error_code' is set to CLUSTER_REDIR_NONE.
 *
 * If the command fails NULL is returned, and the reason of the failure is
 * provided via 'error_code', which will be set to:
 *
 * CLUSTER_REDIR_CROSS_SLOT if the request contains multiple keys that
 * don't belong to the same hash slot.
 *
 * CLUSTER_REDIR_UNSTABLE if the request contains multiple keys
 * belonging to the same slot, but the slot is not stable (in migration or
 * importing state, likely because a resharding is in progress).
 *
 * CLUSTER_REDIR_DOWN_UNBOUND if the request addresses a slot which is
 * not bound to any node. In this case the cluster global state should be
 * already "down" but it is fragile to rely on the update of the global state,
 * so we also handle it here.
 *
 * CLUSTER_REDIR_DOWN_STATE if the cluster is down but the user attempts to
 * execute a command that addresses one or more keys. */
// 返回一个能够执行命令的集群节点，该函数能够成功执行命令的条件：
/*
    1. 一个单个键
    2. 如果槽没有被迁移或导入，那么多个键应该属于一个槽
*/
clusterNode *getNodeByQuery(client *c, struct redisCommand *cmd, robj **argv, int argc, int *hashslot, int *error_code) {
    clusterNode *n = NULL;
    robj *firstkey = NULL;
    int multiple_keys = 0;
    multiState *ms, _ms;
    multiCmd mc;
    int i, slot = 0, migrating_slot = 0, importing_slot = 0, missing_keys = 0;

    /* Set error code optimistically for the base case. */
    // 初始化错误码
    if (error_code) *error_code = CLUSTER_REDIR_NONE;

    /* We handle all the cases as if they were EXEC commands, so we have
     * a common code path for everything */
    // 如果是事务命令，需要进行一些判断
    if (cmd->proc == execCommand) {
        /* If CLIENT_MULTI flag is not set EXEC is just going to return an
         * error. */
        // 如果没有设置CLIENT_MULTI，那么client无法执行事务，返回错误
        if (!(c->flags & CLIENT_MULTI)) return myself;
        ms = &c->mstate;
    } else {
        /* In order to have a single codepath create a fake Multi State
         * structure if the client is not in MULTI/EXEC state, this way
         * we have a single codepath below. */
        ms = &_ms;
        _ms.commands = &mc;
        _ms.count = 1;
        mc.argv = argv;
        mc.argc = argc;
        mc.cmd = cmd;
    }

    /* Check that all the keys are in the same hash slot, and obtain this
     * slot and the node associated. */
    // 检查所有在相同槽的键，获取槽和关联的节点
    for (i = 0; i < ms->count; i++) {
        struct redisCommand *mcmd;
        robj **margv;
        int margc, *keyindex, numkeys, j;

        mcmd = ms->commands[i].cmd;
        margc = ms->commands[i].argc;
        margv = ms->commands[i].argv;
        // 从argv和argc指定的参数列表中返回所有的键
        keyindex = getKeysFromCommand(mcmd,margv,margc,&numkeys);
        for (j = 0; j < numkeys; j++) {
            robj *thiskey = margv[keyindex[j]];
            int thisslot = keyHashSlot((char*)thiskey->ptr,
                                       sdslen(thiskey->ptr));

            if (firstkey == NULL) {
                /* This is the first key we see. Check what is the slot
                 * and node. */
                firstkey = thiskey;
                slot = thisslot;
                n = server.cluster->slots[slot];

                /* Error: If a slot is not served, we are in "cluster down"
                 * state. However the state is yet to be updated, so this was
                 * not trapped earlier in processCommand(). Report the same
                 * error to the client. */
                if (n == NULL) {
                    getKeysFreeResult(keyindex);
                    if (error_code)
                        *error_code = CLUSTER_REDIR_DOWN_UNBOUND;
                    return NULL;
                }

                /* If we are migrating or importing this slot, we need to check
                 * if we have all the keys in the request (the only way we
                 * can safely serve the request, otherwise we return a TRYAGAIN
                 * error). To do so we set the importing/migrating state and
                 * increment a counter for every missing key. */
                if (n == myself &&
                    server.cluster->migrating_slots_to[slot] != NULL)
                {
                    migrating_slot = 1;
                } else if (server.cluster->importing_slots_from[slot] != NULL) {
                    importing_slot = 1;
                }
            } else {
                /* If it is not the first key, make sure it is exactly
                 * the same key as the first we saw. */
                if (!equalStringObjects(firstkey,thiskey)) {
                    if (slot != thisslot) {
                        /* Error: multiple keys from different slots. */
                        getKeysFreeResult(keyindex);
                        if (error_code)
                            *error_code = CLUSTER_REDIR_CROSS_SLOT;
                        return NULL;
                    } else {
                        /* Flag this request as one with multiple different
                         * keys. */
                        multiple_keys = 1;
                    }
                }
            }

            /* Migarting / Improrting slot? Count keys we don't have. */
            if ((migrating_slot || importing_slot) &&
                lookupKeyRead(&server.db[0],thiskey) == NULL)
            {
                missing_keys++;
            }
        }
        getKeysFreeResult(keyindex);
    }

    /* No key at all in command? then we can serve the request
     * without redirections or errors in all the cases. */
    if (n == NULL) return myself;

    /* Cluster is globally down but we got keys? We can't serve the request. */
    if (server.cluster->state != CLUSTER_OK) {
        if (error_code) *error_code = CLUSTER_REDIR_DOWN_STATE;
        return NULL;
    }

    /* Return the hashslot by reference. */
    if (hashslot) *hashslot = slot;

    /* MIGRATE always works in the context of the local node if the slot
     * is open (migrating or importing state). We need to be able to freely
     * move keys among instances in this case. */
    if ((migrating_slot || importing_slot) && cmd->proc == migrateCommand)
        return myself;

    /* If we don't have all the keys and we are migrating the slot, send
     * an ASK redirection. */
    if (migrating_slot && missing_keys) {
        if (error_code) *error_code = CLUSTER_REDIR_ASK;
        return server.cluster->migrating_slots_to[slot];
    }

    /* If we are receiving the slot, and the client correctly flagged the
     * request as "ASKING", we can serve the request. However if the request
     * involves multiple keys and we don't have them all, the only option is
     * to send a TRYAGAIN error. */
    if (importing_slot &&
        (c->flags & CLIENT_ASKING || cmd->flags & CMD_ASKING))
    {
        if (multiple_keys && missing_keys) {
            if (error_code) *error_code = CLUSTER_REDIR_UNSTABLE;
            return NULL;
        } else {
            return myself;
        }
    }

    /* Handle the read-only client case reading from a slave: if this
     * node is a slave and the request is about an hash slot our master
     * is serving, we can reply without redirection. */
    if (c->flags & CLIENT_READONLY &&
        cmd->flags & CMD_READONLY &&
        nodeIsSlave(myself) &&
        myself->slaveof == n)
    {
        return myself;
    }

    /* Base case: just return the right node. However if this node is not
     * myself, set error_code to MOVED since we need to issue a rediretion. */
    if (n != myself && error_code) *error_code = CLUSTER_REDIR_MOVED;
    return n;
}

/* Send the client the right redirection code, according to error_code
 * that should be set to one of CLUSTER_REDIR_* macros.
 *
 * If CLUSTER_REDIR_ASK or CLUSTER_REDIR_MOVED error codes
 * are used, then the node 'n' should not be NULL, but should be the
 * node we want to mention in the redirection. Moreover hashslot should
 * be set to the hash slot that caused the redirection. */
// 发送client一个正确的重定向标识
void clusterRedirectClient(client *c, clusterNode *n, int hashslot, int error_code) {
    if (error_code == CLUSTER_REDIR_CROSS_SLOT) {
        addReplySds(c,sdsnew("-CROSSSLOT Keys in request don't hash to the same slot\r\n"));
    } else if (error_code == CLUSTER_REDIR_UNSTABLE) {
        /* The request spawns mutliple keys in the same slot,
         * but the slot is not "stable" currently as there is
         * a migration or import in progress. */
        addReplySds(c,sdsnew("-TRYAGAIN Multiple keys request during rehashing of slot\r\n"));
    } else if (error_code == CLUSTER_REDIR_DOWN_STATE) {
        addReplySds(c,sdsnew("-CLUSTERDOWN The cluster is down\r\n"));
    } else if (error_code == CLUSTER_REDIR_DOWN_UNBOUND) {
        addReplySds(c,sdsnew("-CLUSTERDOWN Hash slot not served\r\n"));
    } else if (error_code == CLUSTER_REDIR_MOVED ||
               error_code == CLUSTER_REDIR_ASK)
    {
        addReplySds(c,sdscatprintf(sdsempty(),
            "-%s %d %s:%d\r\n",
            (error_code == CLUSTER_REDIR_ASK) ? "ASK" : "MOVED",
            hashslot,n->ip,n->port));
    } else {
        serverPanic("getNodeByQuery() unknown error.");
    }
}

/* This function is called by the function processing clients incrementally
 * to detect timeouts, in order to handle the following case:
 *
 * 1) A client blocks with BLPOP or similar blocking operation.
 * 2) The master migrates the hash slot elsewhere or turns into a slave.
 * 3) The client may remain blocked forever (or up to the max timeout time)
 *    waiting for a key change that will never happen.
 *
 * If the client is found to be blocked into an hash slot this node no
 * longer handles, the client is sent a redirection error, and the function
 * returns 1. Otherwise 0 is returned and no operation is performed. */
// 重定向client的阻塞到其他的服务器
int clusterRedirectBlockedClientIfNeeded(client *c) {
    if (c->flags & CLIENT_BLOCKED && c->btype == BLOCKED_LIST) {
        dictEntry *de;
        dictIterator *di;

        /* If the cluster is down, unblock the client with the right error. */
        // 集群处于下线状态，根据client的错误码，发送消息
        if (server.cluster->state == CLUSTER_FAIL) {
            clusterRedirectClient(c,NULL,0,CLUSTER_REDIR_DOWN_STATE);
            return 1;
        }

        di = dictGetIterator(c->bpop.keys);
        while((de = dictNext(di)) != NULL) {
            robj *key = dictGetKey(de);
            int slot = keyHashSlot((char*)key->ptr, sdslen(key->ptr));
            clusterNode *node = server.cluster->slots[slot];

            /* We send an error and unblock the client if:
             * 1) The slot is unassigned, emitting a cluster down error.
             * 2) The slot is not handled by this node, nor being imported. */
            if (node != myself &&
                server.cluster->importing_slots_from[slot] == NULL)
            {
                if (node == NULL) {
                    clusterRedirectClient(c,NULL,0,
                        CLUSTER_REDIR_DOWN_UNBOUND);
                } else {
                    clusterRedirectClient(c,node,slot,
                        CLUSTER_REDIR_MOVED);
                }
                return 1;
            }
        }
        dictReleaseIterator(di);
    }
    return 0;
}
