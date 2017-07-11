**redis 源码剖析和注释技术博客专栏：**[](http://blog.csdn.net/column/details/15428.html)

|                   文章地址                   |              文件名称              |
| :--------------------------------------: | :----------------------------: |
| [Redis源码剖析和注释（一）--- 链表结构](http://blog.csdn.net/men_wen/article/details/69215222) |      adlist.c 和 adlist.h       |
| [Redis源码剖析和注释（二）--- 简单动态字符串](http://blog.csdn.net/men_wen/article/details/69396550) |         sds.c 和 sds.h          |
| [Redis源码剖析和注释（三）--- Redis 字典结构](http://blog.csdn.net/men_wen/article/details/69787532) |        dict.c 和 dict.h         |
| [Redis源码剖析和注释（四）--- 跳跃表(skiplist)](http://blog.csdn.net/men_wen/article/details/70040026) |       t_zset.c和 redis.h        |
| [Redis源码剖析和注释（五）--- 整数集合(intset)](http://blog.csdn.net/men_wen/article/details/70145752) |      intset.c 和 intset.h       |
| [Redis源码剖析和注释（六）--- 压缩列表(ziplist)](http://blog.csdn.net/men_wen/article/details/70176753) |     ziplist.c 和 ziplist.h      |
| [Redis源码剖析和注释（七）---  快速列表(quicklist)](http://blog.csdn.net/men_wen/article/details/70229375) |   quicklist.c 和 quicklist.h    |
| [Redis源码剖析和注释（八）--- 对象系统(redisObject)](http://blog.csdn.net/men_wen/article/details/70257207) |      object.c 和 server.h       |
| [Redis源码剖析和注释（九）--- 字符串命令的实现(t_string)](http://blog.csdn.net/men_wen/article/details/70325566) |           t_string.c           |
| [Redis源码剖析和注释（十）--- 列表键命令实现(t_list)](http://blog.csdn.net/men_wen/article/details/70551119) |      t_list.c 和 server.h       |
| [Redis源码剖析和注释（十一）--- 哈希键命令的实现(t_hash)](http://blog.csdn.net/men_wen/article/details/70850618) |      t_hash.c 和 server.h       |
| [Redis源码剖析和注释（十二）--- 集合类型键实现(t_set)](http://blog.csdn.net/men_wen/article/details/70941408) |       t_set.c 和 server.h       |
| [Redis源码剖析和注释（十三）---  有序集合类型键实现(t_zset)](http://blog.csdn.net/men_wen/article/details/71043205) |      t_zset.c 和 server.h       |
| [Redis源码剖析和注释（十四）---- Redis 数据库及相关命令实现(db)](http://blog.csdn.net/men_wen/article/details/71088263) |        db.c 和 server.h         |
| [Redis源码剖析和注释（十五）---- 通知功能实现与实战 (notify)](http://blog.csdn.net/men_wen/article/details/71104369) |            notify.c            |
| [Redis源码剖析和注释（十六）---- Redis输入输出的抽象(rio)](http://blog.csdn.net/men_wen/article/details/71131550) |         rio.c 和 rio.h          |
| [Redis源码剖析和注释（十七）--- RDB持久化机制](http://blog.csdn.net/men_wen/article/details/71248449) |         rdb.c 和 rdb.h          |
| [Redis源码剖析和注释（十八）--- Redis AOF持久化机制](http://blog.csdn.net/men_wen/article/details/71375513) |             aof.c              |
| [Redis源码剖析和注释（十九）--- Redis 事件处理实现](http://blog.csdn.net/men_wen/article/details/71514524) | ae.c 和 ae.h （多路复用库：ae_epoll.c） |
| [Redis源码剖析和注释（二十）--- 网络连接库剖析(client的创建/释放、命令接收/回复、Redis通信协议分析等)](http://blog.csdn.net/men_wen/article/details/72084617) |                                |
| [Redis源码剖析和注释（二十一）--- 单机服务器实现(命令的执行、周期性任务、maxmemory策略实现、服务器主函数](http://blog.csdn.net/men_wen/article/details/72455944) |      server.c 和 server.h       |
| [Redis源码剖析和注释（二十二）--- Redis 复制(replicate)源码详细解析](http://blog.csdn.net/men_wen/article/details/72628439) |         replication.c          |
| [Redis源码剖析和注释（二十三）--- Redis Sentinel实现(哨兵的执行过程和执行的内容)](http://blog.csdn.net/men_wen/article/details/72805850) |           sentinel.c           |
| [Redis源码剖析和注释（二十四）--- Redis Sentinel实现(哨兵操作的深入剖析)](http://blog.csdn.net/men_wen/article/details/72805897) |           sentinel.c           |
| [Redis源码剖析和注释（二十五）--- Redis Cluster 的通信流程深入剖析（载入配置文件、节点握手、分配槽）](http://blog.csdn.net/men_wen/article/details/72871618) |     cluster.c 和 cluster.h      |
| [Redis源码剖析和注释（二十六）--- Redis 集群伸缩原理源码剖析](http://blog.csdn.net/men_wen/article/details/72961823) |     cluster.c 和 cluster.h      |
| [Redis源码剖析和注释（二十七）--- Redis 故障转移流程和原理剖析](http://blog.csdn.net/men_wen/article/details/73137338) |     cluster.c 和 cluster.h      |
| [Redis源码剖析和注释（二十八）--- Redis 事务实现和乐观锁](http://blog.csdn.net/men_wen/article/details/73351206) |            multi.c             |

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
|                redis 事务实现                |              multi.c               |

