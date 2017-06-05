# redis-3.0.7源码注释的文件:

|   链表结构    |  adlist.c 和 adlist.h  |
| :-------: | :-------------------: |
| 简单动态字符串结构 |     sds.c 和 sds.h     |
|   字典结构    |    dict.c 和 dict.h    |
|   跳跃表结构   |   t_zset.c和 redis.h   |
|   整数集合    |  intset.c 和 intset.h  |
|   压缩列表    | ziplist.c 和 ziplist.h |

# redis-3.2.8源码注释的文件:

|               quicklist结构                |     quicklist.c 和 quicklist.h      |
| :--------------------------------------: | :--------------------------------: |
|                redis对象系统                 |        object.c 和 server.h         |
|              redis字符串类型键的实现              |             t_string.c             |
|              redis列表类型键的实现               |        t_list.c 和 server.h         |
|              redis哈希类型键的实现               |        t_hash.c 和 server.h         |
|              redis集合类型键的实现               |         t_set.c 和 server.h         |
|             redis 有序集合类型键的实现             |        t_zset.c 和 server.h         |
|               redis 数据库实现                |          db.c 和 server.h           |
|             redis IO层的抽象rio              |           rio.c 和 rio.h            |
|             redis RDB持久化的实现              |           rdb.c 和 rdb.h            |
|             redis AOF持久化的实现              |               aof.c                |
|              redis 事件处理的实现               |   ae.c 和 ae.h （多路复用库：ae_epoll.c）   |
| redis 网络链接库，负责发送/接收命令、创建/销毁redis客户端、通信协议分析、CLIENT命令实现等工作 | networking.c 和 server.c（client结构等） |
| redis 单机服务器实现，包括命令的执行，周期性任务serverCron()，maxmemory的策略、服务器main()函数 |        server.c 和 server.h         |
|         redis 复制(replication)功能          |           replication.c            |
|           redis 哨兵(Sentinel)实现           |             sentinel.c             |
|           redis 集群(Cluster)实现            |       cluster.c 和 cluster.h        |

 