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
#include "bio.h"
#include "rio.h"

#include <signal.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <sys/param.h>

void aofUpdateCurrentSize(void);
void aofClosePipes(void);

/* ----------------------------------------------------------------------------
 * AOF rewrite buffer implementation.
 *
 * The following code implement a simple buffer used in order to accumulate
 * changes while the background process is rewriting the AOF file.
 *
 * We only need to append, but can't just use realloc with a large block
 * because 'huge' reallocs are not always handled as one could expect
 * (via remapping of pages at OS level) but may involve copying data.
 *
 * For this reason we use a list of blocks, every block is
 * AOF_RW_BUF_BLOCK_SIZE bytes.
 * ------------------------------------------------------------------------- */
// AOF 重写缓冲区实现
// 以下代码实现了一个简单的缓冲区，这个缓冲区用来累计一些当后台执行 BGREWRITEAOF 时所发生的数据改变
// 我们只是进行追加append操作，使用realloc分配一块较大的空间不总是我们所预期的那样，而且还可能包含大量的复制操作，基于这个原因，我们使用一个一些块的链表，每个块大小为10MB
// AOF缓冲区大小
#define AOF_RW_BUF_BLOCK_SIZE (1024*1024*10)    /* 10 MB per block */

// AOF块缓冲区结构
typedef struct aofrwblock {
    // 当前已经使用的和可用的字节数
    unsigned long used, free;
    // 缓冲区
    char buf[AOF_RW_BUF_BLOCK_SIZE];
} aofrwblock;

/* This function free the old AOF rewrite buffer if needed, and initialize
 * a fresh new one. It tests for server.aof_rewrite_buf_blocks equal to NULL
 * so can be used for the first initialization as well. */
// 按需释放旧的AOF缓冲块的，并且初始化一个新的。
void aofRewriteBufferReset(void) {
    // 如果当期有缓冲块的链表，则释放旧的缓冲区
    if (server.aof_rewrite_buf_blocks)
        listRelease(server.aof_rewrite_buf_blocks);

    // 初始化一个新的缓冲块链表
    server.aof_rewrite_buf_blocks = listCreate();
    // 设置链表的释放方法
    listSetFreeMethod(server.aof_rewrite_buf_blocks,zfree);
}

/* Return the current size of the AOF rewrite buffer. */
// 返回当前AOF重写缓冲区的大小
unsigned long aofRewriteBufferSize(void) {
    listNode *ln;
    listIter li;
    unsigned long size = 0;

    // 设置链表迭代器指向头节点，并设置迭代方向为从头到位
    listRewind(server.aof_rewrite_buf_blocks,&li);
    // 遍历所有链表节点
    while((ln = listNext(&li))) {
        // 取出当前节点的缓冲块
        aofrwblock *block = listNodeValue(ln);
        // 总缓冲区的大小 = 节点数量 × AOF_RW_BUF_BLOCK_SIZE - 最后一个节点的free字节数
        size += block->used;
    }
    return size;
}

/* Event handler used to send data to the child process doing the AOF
 * rewrite. We send pieces of our AOF differences buffer so that the final
 * write when the child finishes the rewrite will be small. */
// 事件处理程序发送一些数据给正在做AOF重写的子进程，我们发送AOF缓冲区一部分不同的数据给子进程，当子进程完成重写时，重写的文件会比较小
void aofChildWriteDiffData(aeEventLoop *el, int fd, void *privdata, int mask) {
    listNode *ln;
    aofrwblock *block;
    ssize_t nwritten;
    UNUSED(el);
    UNUSED(fd);
    UNUSED(privdata);
    UNUSED(mask);

    while(1) {
        // 获取缓冲块链表的头节点地址
        ln = listFirst(server.aof_rewrite_buf_blocks);
        // 获取缓冲块地址
        block = ln ? ln->value : NULL;
        // 如果aof_stop_sending_diff为真，则停止发送累计的不同数据给子进程，或者缓冲块为空
        // 则将管道的写端从服务器的监听队列中删除
        if (server.aof_stop_sending_diff || !block) {
            aeDeleteFileEvent(server.el,server.aof_pipe_write_data_to_child,
                              AE_WRITABLE);
            return;
        }
        // 如果已经有缓存的数据
        if (block->used > 0) {
            // 则将缓存的数据写到管道中
            nwritten = write(server.aof_pipe_write_data_to_child,
                             block->buf,block->used);
            if (nwritten <= 0) return;
            // 更新缓冲区的数据，覆盖掉已经写到管道的数据
            memmove(block->buf,block->buf+nwritten,block->used-nwritten);
            block->used -= nwritten;
        }
        // 如果当前节点的所缓冲的数据全部写完，则删除该节点
        if (block->used == 0) listDelNode(server.aof_rewrite_buf_blocks,ln);
    }
}

/* Append data to the AOF rewrite buffer, allocating new blocks if needed. */
// 将s指向的数据追加到AOF重写缓冲区中，如果需要可以新分配一个缓冲块
void aofRewriteBufferAppend(unsigned char *s, unsigned long len) {
    // 获取缓冲块链表的尾节点地址
    listNode *ln = listLast(server.aof_rewrite_buf_blocks);
    aofrwblock *block = ln ? ln->value : NULL;

    while(len) {
        /* If we already got at least an allocated block, try appending
         * at least some piece into it. */
        // 如果已经有一个缓存快，那么执行追加append操作
        if (block) {
            // 判断缓存块的可用空间最多能写的空间
            unsigned long thislen = (block->free < len) ? block->free : len;
            // 当前缓存块有一定空间
            if (thislen) {  /* The current block is not already full. */
                // 将s指向的数据拷贝thislen长度到当前缓存块
                memcpy(block->buf+block->used, s, thislen);
                // 更新可用和已用的信息
                block->used += thislen;
                block->free -= thislen;
                // 更新要拷贝的数据地址
                s += thislen;
                // 更新拷贝的长度
                len -= thislen;
            }
        }

        // 需要新创建一个缓存块
        if (len) { /* First block to allocate, or need another block. */
            int numblocks;

            // 分配缓存块空间和初始化
            block = zmalloc(sizeof(*block));
            block->free = AOF_RW_BUF_BLOCK_SIZE;
            block->used = 0;
            // 将缓存块加如到链表缓存区末尾
            listAddNodeTail(server.aof_rewrite_buf_blocks,block);

            /* Log every time we cross more 10 or 100 blocks, respectively
             * as a notice or warning. */
            // 每次创建10个或100个缓存块，就更新日志，当做标记

            // 获取当前缓存块个数
            numblocks = listLength(server.aof_rewrite_buf_blocks);
            // 更新日志
            if (((numblocks+1) % 10) == 0) {
                int level = ((numblocks+1) % 100) == 0 ? LL_WARNING :
                                                         LL_NOTICE;
                serverLog(level,"Background AOF buffer size: %lu MB",
                    aofRewriteBufferSize()/(1024*1024));
            }
        }
    }

    /* Install a file event to send data to the rewrite child if there is
     * not one already. */
    // 获取当前事件正在监听的类型，如果等于0，未设置，则设置管道aof_pipe_write_data_to_child为可写状态
    // 当然aof_pipe_write_data_to_child可以用的时候，调用aofChildWriteDiffDatah()函数写数据
    if (aeGetFileEvents(server.el,server.aof_pipe_write_data_to_child) == 0) {
        aeCreateFileEvent(server.el, server.aof_pipe_write_data_to_child,
            AE_WRITABLE, aofChildWriteDiffData, NULL);
    }
}

/* Write the buffer (possibly composed of multiple blocks) into the specified
 * fd. If a short write or any other error happens -1 is returned,
 * otherwise the number of bytes written is returned. */
// 将缓冲区的数据写到制定的文件描述符fd中，成功返回写入的字节数，否则返回-1
ssize_t aofRewriteBufferWrite(int fd) {
    listNode *ln;
    listIter li;
    ssize_t count = 0;

    // 设置链表迭代器指向头节点，并设置迭代方向为从头到位
    listRewind(server.aof_rewrite_buf_blocks,&li);
    // 遍历所有链表节点
    while((ln = listNext(&li))) {
        // 获取缓存块地址
        aofrwblock *block = listNodeValue(ln);
        ssize_t nwritten;

        // 如果有数据，则将已有的数据写到fd中
        if (block->used) {
            nwritten = write(fd,block->buf,block->used);
            // 如果发生短写（short write），设置errno返回-1
            if (nwritten != (ssize_t)block->used) {
                if (nwritten == 0) errno = EIO;
                return -1;
            }
            // 更新成功写的字节数
            count += nwritten;
        }
    }
    return count;
}

/* ----------------------------------------------------------------------------
 * AOF file implementation
 * ------------------------------------------------------------------------- */
// AOF文件的实现
/* Starts a background task that performs fsync() against the specified
 * file descriptor (the one of the AOF file) in another thread. */
// 在另一个线程中，创建一个后台任务，对给定的fd，执行一个fsync()函数
void aof_background_fsync(int fd) {
    bioCreateBackgroundJob(BIO_AOF_FSYNC,(void*)(long)fd,NULL,NULL);
}

/* Called when the user switches from "appendonly yes" to "appendonly no"
 * at runtime using the CONFIG command. */
// 用户通过CONFIG命令设置appendonly为no时，调用stopAppendOnly()
void stopAppendOnly(void) {
    // 保证AOF的状态为正在进行AOF
    serverAssert(server.aof_state != AOF_OFF);
    // 强制将AOF缓冲区内容冲洗到AOF文件中
    flushAppendOnlyFile(1);
    // 对AOF文件执行同步操作
    aof_fsync(server.aof_fd);
    // 关闭AOF文件
    close(server.aof_fd);

    // 清空AOF状态
    server.aof_fd = -1;
    server.aof_selected_db = -1;
    server.aof_state = AOF_OFF;
    /* rewrite operation in progress? kill it, wait child exit */
    // 如果正在进行AOF
    if (server.aof_child_pid != -1) {
        int statloc;

        serverLog(LL_NOTICE,"Killing running AOF rewrite child: %ld",
            (long) server.aof_child_pid);
        // 杀死当前正在进行AOF的子进程
        if (kill(server.aof_child_pid,SIGUSR1) != -1) {
            // 等待子进程退出
            while(wait3(&statloc,0,NULL) != server.aof_child_pid);
        }
        /* reset the buffer accumulating changes while the child saves */
        // 释放旧的AOF缓冲块的，并且初始化一个新的。
        aofRewriteBufferReset();
        // 删除临时文件
        aofRemoveTempFile(server.aof_child_pid);
        // 清除执行AOF进程的id和重写的时间
        server.aof_child_pid = -1;
        server.aof_rewrite_time_start = -1;
        /* close pipes used for IPC between the two processes. */
        // 关闭两个进程之间的通信管道
        aofClosePipes();
    }
}

/* Called when the user switches from "appendonly no" to "appendonly yes"
 * at runtime using the CONFIG command. */
// 用户通过CONFIG命令设置appendonly为yes时，调用startAppendOnly()
int startAppendOnly(void) {
    char cwd[MAXPATHLEN]; /* Current working dir path for error messages. */

    // 设置AOF最近一个同步的时间
    server.aof_last_fsync = server.unixtime;
    // 打开一个AOF文件
    server.aof_fd = open(server.aof_filename,O_WRONLY|O_APPEND|O_CREAT,0644);
    // 确保当前AOF状态为关闭状态
    serverAssert(server.aof_state == AOF_OFF);
    // 打开文件失败，获得文件路径更新日志
    if (server.aof_fd == -1) {
        char *cwdp = getcwd(cwd,MAXPATHLEN);

        serverLog(LL_WARNING,
            "Redis needs to enable the AOF but can't open the "
            "append only file %s (in server root dir %s): %s",
            server.aof_filename,
            cwdp ? cwdp : "unknown",
            strerror(errno));
        return C_ERR;
    }
    // 当前正在执行RDB持久化，那么将AOF提上日程
    if (server.rdb_child_pid != -1) {
        server.aof_rewrite_scheduled = 1;   //设置提上日程标记
        serverLog(LL_WARNING,"AOF was enabled but there is already a child process saving an RDB file on disk. An AOF background was scheduled to start when possible.");
    // 如果AOF后台重写失败
    } else if (rewriteAppendOnlyFileBackground() == C_ERR) {
        // 关闭AOF文件描述符，并更新日志
        close(server.aof_fd);
        serverLog(LL_WARNING,"Redis needs to enable the AOF but can't trigger a background AOF rewrite operation. Check the above logs for more info about the error.");
        return C_ERR;
    }
    /* We correctly switched on AOF, now wait for the rewrite to be complete
     * in order to append data on disk. */
    // 设置AOF状态为等待重写完成
    server.aof_state = AOF_WAIT_REWRITE;
    return C_OK;
}

/* Write the append only file buffer on disk.
 *
 * Since we are required to write the AOF before replying to the client,
 * and the only way the client socket can get a write is entering when the
 * the event loop, we accumulate all the AOF writes in a memory
 * buffer and write it on disk using this function just before entering
 * the event loop again.
 *
 * About the 'force' argument:
 *
 * When the fsync policy is set to 'everysec' we may delay the flush if there
 * is still an fsync() going on in the background thread, since for instance
 * on Linux write(2) will be blocked by the background fsync anyway.
 * When this happens we remember that there is some aof buffer to be
 * flushed ASAP, and will try to do that in the serverCron() function.
 *
 * However if force is set to 1 we'll write regardless of the background
 * fsync. */
// 将AOF缓存写到磁盘中
// 因为我们需要在回复client之前对AOF执行写操作，唯一的机会是在事件loop中，因此累计所有的AOF到缓存中，在下一次重新进入事件loop之前将缓存写到AOF文件中

// 关于force参数
// 当fsync被设置为每秒执行一次，如果后台仍有线程正在执行fsync操作，我们可能会延迟flush操作，因为write操作可能会被阻塞，当发生这种情况时，说明需要尽快的执行flush操作，会调用 serverCron() 函数。
// 然而如果force被设置为1，我们会无视后台的fsync，直接进行写入操作

#define AOF_WRITE_LOG_ERROR_RATE 30 /* Seconds between errors logging. */
// 将AOF缓存冲洗到磁盘中
void flushAppendOnlyFile(int force) {
    ssize_t nwritten;
    int sync_in_progress = 0;
    mstime_t latency;

    // 如果缓冲区中没有数据，直接返回
    if (sdslen(server.aof_buf) == 0) return;

    // 同步策略是每秒同步一次
    if (server.aof_fsync == AOF_FSYNC_EVERYSEC)
        // AOF同步操作是否在后台正在运行
        sync_in_progress = bioPendingJobsOfType(BIO_AOF_FSYNC) != 0;

    // 同步策略是每秒同步一次，且不是强制同步的
    if (server.aof_fsync == AOF_FSYNC_EVERYSEC && !force) {
        /* With this append fsync policy we do background fsyncing.
         * If the fsync is still in progress we can try to delay
         * the write for a couple of seconds. */
        // 根据这个同步策略，且没有设置强制执行，我们在后台执行同步
        // 如果同步已经在后台执行，那么可以延迟两秒，如果设置了force，那么服务器会阻塞在write操作上

        // 如果后台正在执行同步
        if (sync_in_progress) {
            // 延迟执行flush操作的开始时间为0，表示之前没有延迟过write
            if (server.aof_flush_postponed_start == 0) {
                /* No previous write postponing, remember that we are
                 * postponing the flush and return. */
                // 之前没有延迟过write操作，那么将延迟write操作的开始时间保存下来，然后就直接返回
                server.aof_flush_postponed_start = server.unixtime;
                return;
            // 如果之前延迟过write操作，如果没到2秒，直接返回，不执行write
            } else if (server.unixtime - server.aof_flush_postponed_start < 2) {
                /* We were already waiting for fsync to finish, but for less
                 * than two seconds this is still ok. Postpone again. */
                return;
            }
            /* Otherwise fall trough, and go write since we can't wait
             * over two seconds. */
            // 执行到这里，表示后台正在执行fsync，但是延迟时间已经超过2秒
            // 那么执行write操作，此时write会被阻塞
            server.aof_delayed_fsync++;
            serverLog(LL_NOTICE,"Asynchronous AOF fsync is taking too long (disk is busy?). Writing the AOF buffer without waiting for fsync to complete, this may slow down Redis.");
        }
    }
    /* We want to perform a single write. This should be guaranteed atomic
     * at least if the filesystem we are writing is a real physical one.
     * While this will save us against the server being killed I don't think
     * there is much to do about the whole server stopping for power problems
     * or alike */
    // 执行write操作，保证写操作是原子操作

    // 设置延迟检测开始的时间
    latencyStartMonitor(latency);
    // 将缓冲区的内容写到AOF文件中
    nwritten = write(server.aof_fd,server.aof_buf,sdslen(server.aof_buf));
    // 设置延迟的时间 = 当前的时间 - 开始的时间
    latencyEndMonitor(latency);
    /* We want to capture different events for delayed writes:
     * when the delay happens with a pending fsync, or with a saving child
     * active, and when the above two conditions are missing.
     * We also use an additional event name to save all samples which is
     * useful for graphing / monitoring purposes. */
    // 捕获不同造成延迟write的事件
    // 如果正在后台执行同步fsync
    if (sync_in_progress) {
        // 将latency和"aof-write-pending-fsync"关联到延迟诊断字典中
        latencyAddSampleIfNeeded("aof-write-pending-fsync",latency);
    // 如果正在执行AOF或正在执行RDB
    } else if (server.aof_child_pid != -1 || server.rdb_child_pid != -1) {
        // 将latency和"aof-write-active-child"关联到延迟诊断字典中
        latencyAddSampleIfNeeded("aof-write-active-child",latency);
    } else {
        // 将latency和"aof-write-alone"关联到延迟诊断字典中
        latencyAddSampleIfNeeded("aof-write-alone",latency);
    }
    // 将latency和"aof-write"关联到延迟诊断字典中
    latencyAddSampleIfNeeded("aof-write",latency);

    /* We performed the write so reset the postponed flush sentinel to zero. */
    // 执行了write，所以清零延迟flush的时间
    server.aof_flush_postponed_start = 0;

    // 如果写入的字节数不等于缓存的字节数，发生异常错误
    if (nwritten != (signed)sdslen(server.aof_buf)) {
        static time_t last_write_error_log = 0;
        int can_log = 0;

        /* Limit logging rate to 1 line per AOF_WRITE_LOG_ERROR_RATE seconds. */
        // 限制日志的频率每行30秒
        if ((server.unixtime - last_write_error_log) > AOF_WRITE_LOG_ERROR_RATE) {
            can_log = 1;
            last_write_error_log = server.unixtime;
        }

        /* Log the AOF write error and record the error code. */
        // 如果写入错误，写errno到日志
        if (nwritten == -1) {
            if (can_log) {
                serverLog(LL_WARNING,"Error writing to the AOF file: %s",
                    strerror(errno));
                server.aof_last_write_errno = errno;
            }
        // 如果是写了一部分，发生错误
        } else {
            if (can_log) {
                serverLog(LL_WARNING,"Short write while writing to "
                                       "the AOF file: (nwritten=%lld, "
                                       "expected=%lld)",
                                       (long long)nwritten,
                                       (long long)sdslen(server.aof_buf));
            }

            // 将追加的内容截断，删除了追加的内容，恢复成原来的文件
            if (ftruncate(server.aof_fd, server.aof_current_size) == -1) {
                if (can_log) {
                    serverLog(LL_WARNING, "Could not remove short write "
                             "from the append-only file.  Redis may refuse "
                             "to load the AOF the next time it starts.  "
                             "ftruncate: %s", strerror(errno));
                }
            } else {
                /* If the ftruncate() succeeded we can set nwritten to
                 * -1 since there is no longer partial data into the AOF. */
                nwritten = -1;
            }
            server.aof_last_write_errno = ENOSPC;
        }

        /* Handle the AOF write error. */
        // 如果是写入的策略为每次写入就同步，无法恢复这种策略的写，因为我们已经告知使用者，已经将写的数据同步到磁盘了，因此直接退出程序
        if (server.aof_fsync == AOF_FSYNC_ALWAYS) {
            /* We can't recover when the fsync policy is ALWAYS since the
             * reply for the client is already in the output buffers, and we
             * have the contract with the user that on acknowledged write data
             * is synced on disk. */
            serverLog(LL_WARNING,"Can't recover from AOF write error when the AOF fsync policy is 'always'. Exiting...");
            exit(1);
        } else {
            /* Recover from failed write leaving data into the buffer. However
             * set an error to stop accepting writes as long as the error
             * condition is not cleared. */
            //设置执行write操作的状态
            server.aof_last_write_status = C_ERR;

            /* Trim the sds buffer if there was a partial write, and there
             * was no way to undo it with ftruncate(2). */
            // 如果只写入了局部，没有办法用ftruncate()函数去恢复原来的AOF文件
            if (nwritten > 0) {
                // 只能更新当前的AOF文件的大小
                server.aof_current_size += nwritten;
                // 删除AOF缓冲区写入的字节数
                sdsrange(server.aof_buf,nwritten,-1);
            }
            return; /* We'll try again on the next call... */
        }

    // nwritten == (signed)sdslen(server.aof_buf
    // 执行write写入成功
    } else {
        /* Successful write(2). If AOF was in error state, restore the
         * OK state and log the event. */
        // 更新最近一次写的状态为 C_OK
        if (server.aof_last_write_status == C_ERR) {
            serverLog(LL_WARNING,
                "AOF write error looks solved, Redis can write again.");
            server.aof_last_write_status = C_OK;
        }
    }
    // 只能更新当前的AOF文件的大小
    server.aof_current_size += nwritten;

    /* Re-use AOF buffer when it is small enough. The maximum comes from the
     * arena size of 4k minus some overhead (but is otherwise arbitrary). */
    // 如果这个缓存足够小，小于4K，那么重用这个缓存，否则释放AOF缓存
    if ((sdslen(server.aof_buf)+sdsavail(server.aof_buf)) < 4000) {
        sdsclear(server.aof_buf);   //将缓存内容清空，重用
    } else {
        sdsfree(server.aof_buf);    //释放缓存空间
        server.aof_buf = sdsempty();//创建一个新缓存
    }

    /* Don't fsync if no-appendfsync-on-rewrite is set to yes and there are
     * children doing I/O in the background. */
    // 如果no-appendfsync-on-rewrite被设置为yes，表示正在执行重写，则不执行fsync
    // 或者正在执行 BGSAVE 或 BGWRITEAOF，也不执行
    if (server.aof_no_fsync_on_rewrite &&
        (server.aof_child_pid != -1 || server.rdb_child_pid != -1))
            return;

    /* Perform the fsync if needed. */

    // 执行fsync进行同步，每次写入都同步
    if (server.aof_fsync == AOF_FSYNC_ALWAYS) {
        /* aof_fsync is defined as fdatasync() for Linux in order to avoid
         * flushing metadata. */
        // 设置延迟检测开始的时间
        latencyStartMonitor(latency);
        // Linux下调用fdatasync()函数更高效的执行同步
        aof_fsync(server.aof_fd); /* Let's try to get this data on the disk */
        // 设置延迟的时间 = 当前的时间 - 开始的时间
        latencyEndMonitor(latency);
        // 将latency和"aof-fsync-always"关联到延迟诊断字典中
        latencyAddSampleIfNeeded("aof-fsync-always",latency);
        // 更新最近一次执行同步的时间
        server.aof_last_fsync = server.unixtime;

    // 每秒执行一次同步，当前时间大于上一次执行同步的时间
    } else if ((server.aof_fsync == AOF_FSYNC_EVERYSEC &&
                server.unixtime > server.aof_last_fsync)) {
        // 如果没有正在执行同步，那么在后台开一个线程执行同步
        if (!sync_in_progress) aof_background_fsync(server.aof_fd);
        // 更新最近一次执行同步的时间
        server.aof_last_fsync = server.unixtime;
    }
}

// 根据传入的命令和命令参数，将他们还原成协议格式
sds catAppendOnlyGenericCommand(sds dst, int argc, robj **argv) {
    char buf[32];
    int len, j;
    robj *o;

    // 格式："*<argc>\r\n"
    buf[0] = '*';
    len = 1+ll2string(buf+1,sizeof(buf)-1,argc);
    buf[len++] = '\r';
    buf[len++] = '\n';
    // 拼接到dst的后面
    dst = sdscatlen(dst,buf,len);

    // 遍历所有的参数，建立命令的格式：$<command_len>\r\n<command>\r\n
    for (j = 0; j < argc; j++) {
        o = getDecodedObject(argv[j]);  //解码成字符串对象
        buf[0] = '$';
        len = 1+ll2string(buf+1,sizeof(buf)-1,sdslen(o->ptr));
        buf[len++] = '\r';
        buf[len++] = '\n';
        dst = sdscatlen(dst,buf,len);
        dst = sdscatlen(dst,o->ptr,sdslen(o->ptr));
        dst = sdscatlen(dst,"\r\n",2);
        decrRefCount(o);
    }
    return dst; //返回还原后的协议内容
}

/* Create the sds representation of an PEXPIREAT command, using
 * 'seconds' as time to live and 'cmd' to understand what command
 * we are translating into a PEXPIREAT.
 *
 * This command is used in order to translate EXPIRE and PEXPIRE commands
 * into PEXPIREAT command so that we retain precision in the append only
 * file, and the time is always absolute and not relative. */
// 用sds表示一个 PEXPIREAT 命令，seconds为生存时间，cmd为指定转换的指令
// 这个函数用来转换 EXPIRE and PEXPIRE 命令成 PEXPIREAT ，以便在AOF时，时间总是一个绝对值
sds catAppendOnlyExpireAtCommand(sds buf, struct redisCommand *cmd, robj *key, robj *seconds) {
    long long when;
    robj *argv[3];

    /* Make sure we can use strtoll */
    // 解码成字符串对象，以便使用strtoll函数
    seconds = getDecodedObject(seconds);
    // 取出过期值，long long类型
    when = strtoll(seconds->ptr,NULL,10);
    /* Convert argument into milliseconds for EXPIRE, SETEX, EXPIREAT */
    // 将 EXPIRE, SETEX, EXPIREAT 参数的秒转换成毫秒
    if (cmd->proc == expireCommand || cmd->proc == setexCommand ||
        cmd->proc == expireatCommand)
    {
        when *= 1000;
    }
    /* Convert into absolute time for EXPIRE, PEXPIRE, SETEX, PSETEX */
    // 将 EXPIRE, PEXPIRE, SETEX, PSETEX 命令的参数，从相对时间设置为绝对时间
    if (cmd->proc == expireCommand || cmd->proc == pexpireCommand ||
        cmd->proc == setexCommand || cmd->proc == psetexCommand)
    {
        when += mstime();
    }
    decrRefCount(seconds);

    // 创建一个 PEXPIREAT 命令对象
    argv[0] = createStringObject("PEXPIREAT",9);
    argv[1] = key;
    argv[2] = createStringObjectFromLongLong(when);
    // 将命令还原成协议格式，追加到buf
    buf = catAppendOnlyGenericCommand(buf, 3, argv);
    decrRefCount(argv[0]);
    decrRefCount(argv[2]);
    // 返回buf
    return buf;
}

// 将命令追加到AOF文件中
void feedAppendOnlyFile(struct redisCommand *cmd, int dictid, robj **argv, int argc) {
    sds buf = sdsempty();   //设置一个空sds
    robj *tmpargv[3];

    /* The DB this command was targeting is not the same as the last command
     * we appended. To issue a SELECT command is needed. */
    // 使用SELECT命令，显式的设置当前数据库
    if (dictid != server.aof_selected_db) {
        char seldb[64];

        snprintf(seldb,sizeof(seldb),"%d",dictid);
        // 构造SELECT命令的协议格式
        buf = sdscatprintf(buf,"*2\r\n$6\r\nSELECT\r\n$%lu\r\n%s\r\n",
            (unsigned long)strlen(seldb),seldb);
        // 执行AOF时，当前的数据库ID
        server.aof_selected_db = dictid;
    }

    // 如果是 EXPIRE/PEXPIRE/EXPIREAT 三个命令，则要转换成 PEXPIREAT 命令
    if (cmd->proc == expireCommand || cmd->proc == pexpireCommand ||
        cmd->proc == expireatCommand) {
        /* Translate EXPIRE/PEXPIRE/EXPIREAT into PEXPIREAT */
        buf = catAppendOnlyExpireAtCommand(buf,cmd,argv[1],argv[2]);

    // 如果是 SETEX/PSETEX 命令，则转换成 SET and PEXPIREAT
    } else if (cmd->proc == setexCommand || cmd->proc == psetexCommand) {
        /* Translate SETEX/PSETEX to SET and PEXPIREAT */
        // SETEX key seconds value
        // 构建SET命令对象
        tmpargv[0] = createStringObject("SET",3);
        tmpargv[1] = argv[1];
        tmpargv[2] = argv[3];
        // 将SET命令按协议格式追加到buf中
        buf = catAppendOnlyGenericCommand(buf,3,tmpargv);
        decrRefCount(tmpargv[0]);
        // 将SETEX/PSETEX命令和键对象按协议格式追加到buf中
        buf = catAppendOnlyExpireAtCommand(buf,cmd,argv[1],argv[2]);

    // 其他命令直接按协议格式转换，然后追加到buf中
    } else {
        /* All the other commands don't need translation or need the
         * same translation already operated in the command vector
         * for the replication itself. */
        buf = catAppendOnlyGenericCommand(buf,argc,argv);
    }

    /* Append to the AOF buffer. This will be flushed on disk just before
     * of re-entering the event loop, so before the client will get a
     * positive reply about the operation performed. */
    // 如果正在进行AOF，则将命令追加到AOF的缓存中，在重新进入事件循环之前，这些命令会被冲洗到磁盘上，并向client回复
    if (server.aof_state == AOF_ON)
        server.aof_buf = sdscatlen(server.aof_buf,buf,sdslen(buf));

    /* If a background append only file rewriting is in progress we want to
     * accumulate the differences between the child DB and the current one
     * in a buffer, so that when the child process will do its work we
     * can append the differences to the new append only file. */
    // 如果后台正在进行重写，那么将命令追加到重写缓存区中，以便我们记录重写的AOF文件于当前数据库的差异
    if (server.aof_child_pid != -1)
        aofRewriteBufferAppend((unsigned char*)buf,sdslen(buf));

    sdsfree(buf);
}

/* ----------------------------------------------------------------------------
 * AOF loading
 * ------------------------------------------------------------------------- */
// AOF载入
/* In Redis commands are always executed in the context of a client, so in
 * order to load the append only file we need to create a fake client. */
// Redis命令总是在一个客户端中被执行，为了载入AOF文件，我们需要创建并返回一个伪client
struct client *createFakeClient(void) {
    struct client *c = zmalloc(sizeof(*c)); //分配空间

    selectDb(c,0);
    c->fd = -1;
    c->name = NULL;
    c->querybuf = sdsempty();
    c->querybuf_peak = 0;
    c->argc = 0;
    c->argv = NULL;
    c->bufpos = 0;
    c->flags = 0;
    c->btype = BLOCKED_NONE;
    /* We set the fake client as a slave waiting for the synchronization
     * so that Redis will not try to send replies to this client. */
    // 将客户端设置为一个等待同步的从节点，于是Redis不会发送回复给这个client
    c->replstate = SLAVE_STATE_WAIT_BGSAVE_START;
    c->reply = listCreate();
    c->reply_bytes = 0;
    c->obuf_soft_limit_reached_time = 0;
    c->watched_keys = listCreate();
    c->peerid = NULL;
    listSetFreeMethod(c->reply,decrRefCountVoid);
    listSetDupMethod(c->reply,dupClientReplyValue);
    initClientMultiState(c);
    return c;
}

// 释放伪client的参数列表空间
void freeFakeClientArgv(struct client *c) {
    int j;

    for (j = 0; j < c->argc; j++)
        decrRefCount(c->argv[j]);
    zfree(c->argv);
}

// 释放伪client空间
void freeFakeClient(struct client *c) {
    // 释放查询缓存
    sdsfree(c->querybuf);
    // 释放回复缓存
    listRelease(c->reply);
    // 释放监控列表
    listRelease(c->watched_keys);
    // 释放事物状态
    freeClientMultiState(c);
    // 释放client的空间
    zfree(c);
}

/* Replay the append log file. On success C_OK is returned. On non fatal
 * error (the append only file is zero-length) C_ERR is returned. On
 * fatal error an error message is logged and the program exists. */
// 执行AOF文件中的命令
// 成功返回C_OK，出现非致命错误返回C_ERR，例如AOF文件长度为0，出现致命错误打印日志退出
int loadAppendOnlyFile(char *filename) {
    struct client *fakeClient;
    FILE *fp = fopen(filename,"r"); //以读打开AOF文件
    struct redis_stat sb;
    int old_aof_state = server.aof_state;   //备份当前AOF的状态
    long loops = 0;
    off_t valid_up_to = 0; /* Offset of the latest well-formed command loaded. */

    // 如果文件打开，但是大小为0，则返回C_ERR
    if (fp && redis_fstat(fileno(fp),&sb) != -1 && sb.st_size == 0) {
        server.aof_current_size = 0;
        fclose(fp);
        return C_ERR;
    }

    // 如果文件打开失败，打印日志，退出
    if (fp == NULL) {
        serverLog(LL_WARNING,"Fatal error: can't open the append log file for reading: %s",strerror(errno));
        exit(1);
    }

    /* Temporarily disable AOF, to prevent EXEC from feeding a MULTI
     * to the same file we're about to read. */
    // 暂时关闭AOF，防止在执行MULTI时，EXEC命令被传播到AOF文件中
    server.aof_state = AOF_OFF;

    // 生成一个伪client
    fakeClient = createFakeClient();
    // 设置载入的状态信息
    startLoading(fp);

    while(1) {
        int argc, j;
        unsigned long len;
        robj **argv;
        char buf[128];
        sds argsds;
        struct redisCommand *cmd;

        /* Serve the clients from time to time */
        // 间隔性的处理client请求
        if (!(loops++ % 1000)) {
            // ftello(fp)返回当前文件载入的偏移量
            // 设置载入时server的状态信息，更新当前载入的进度
            loadingProgress(ftello(fp));
            // 在服务器被阻塞的状态下，仍然能处理请求
            // 因为当前处于载入状态，当client的请求到来时，总是返回loading的状态错误
            processEventsWhileBlocked();
        }

        // 将一行文件内容读到buf中，遇到"\r\n"停止
        if (fgets(buf,sizeof(buf),fp) == NULL) {
            if (feof(fp))   //如果文件已经读完了或数据库为空，则跳出while循环
                break;
            else
                goto readerr;
        }
        // 检查文件格式 "*<argc>\r\n"
        if (buf[0] != '*') goto fmterr;
        if (buf[1] == '\0') goto readerr;
        // 取出命令参数个数
        argc = atoi(buf+1);
        if (argc < 1) goto fmterr;  //至少一个参数，就是当前命令

        // 分配参数列表空间
        argv = zmalloc(sizeof(robj*)*argc);
        // 设置伪client的参数列表
        fakeClient->argc = argc;
        fakeClient->argv = argv;

        // 遍历参数列表
        // "$<command_len>\r\n<command>\r\n"
        for (j = 0; j < argc; j++) {
            // 读一行内容到buf中，遇到"\r\n"停止
            if (fgets(buf,sizeof(buf),fp) == NULL) {
                fakeClient->argc = j; /* Free up to j-1. */
                freeFakeClientArgv(fakeClient);
                goto readerr;
            }
            // 检查格式
            if (buf[0] != '$') goto fmterr;
            // 读出参数的长度len
            len = strtol(buf+1,NULL,10);
            // 初始化一个len长度的sds
            argsds = sdsnewlen(NULL,len);
            // 从文件中读出一个len字节长度，将值保存到argsds中
            if (len && fread(argsds,len,1,fp) == 0) {
                sdsfree(argsds);
                fakeClient->argc = j; /* Free up to j-1. */
                freeFakeClientArgv(fakeClient);
                goto readerr;
            }
            // 创建一个字符串对象保存读出的参数argsds
            argv[j] = createObject(OBJ_STRING,argsds);
            // 读两个字节，跳过"\r\n"
            if (fread(buf,2,1,fp) == 0) {
                fakeClient->argc = j+1; /* Free up to j. */
                freeFakeClientArgv(fakeClient);
                goto readerr; /* discard CRLF */
            }
        }

        /* Command lookup */
        // 查找命令
        cmd = lookupCommand(argv[0]->ptr);
        if (!cmd) {
            serverLog(LL_WARNING,"Unknown command '%s' reading the append only file", (char*)argv[0]->ptr);
            exit(1);
        }

        /* Run the command in the context of a fake client */
        // 调用伪client执行命令
        cmd->proc(fakeClient);

        /* The fake client should not have a reply */
        // 伪client不应该有回复
        serverAssert(fakeClient->bufpos == 0 && listLength(fakeClient->reply) == 0);
        /* The fake client should never get blocked */
        // 伪client不应该是阻塞的
        serverAssert((fakeClient->flags & CLIENT_BLOCKED) == 0);

        /* Clean up. Command code may have changed argv/argc so we use the
         * argv/argc of the client instead of the local variables. */
        // 释放伪client的参数列表
        freeFakeClientArgv(fakeClient);
        // 更新已载入且命令合法的当前文件的偏移量
        if (server.aof_load_truncated) valid_up_to = ftello(fp);
    }

    /* This point can only be reached when EOF is reached without errors.
     * If the client is in the middle of a MULTI/EXEC, log error and quit. */
    // 执行到这里，说明AOF文件的所有内容都被正确的读取
    // 如果伪client处于 MULTI/EXEC 的环境中，还有检测文件是否包含正确事物的结束，调到uxeof
    if (fakeClient->flags & CLIENT_MULTI) goto uxeof;

// 载入成功
loaded_ok: /* DB loaded, cleanup and return C_OK to the caller. */
    fclose(fp); //关闭文件
    freeFakeClient(fakeClient); //释放伪client
    server.aof_state = old_aof_state;   //还原AOF状态
    stopLoading();  //设置载入完成的状态
    aofUpdateCurrentSize(); //更新服务器状态，当前AOF文件的大小
    server.aof_rewrite_base_size = server.aof_current_size; //更新重写的大小
    return C_OK;

// 载入时读错误，如果feof(fp)为真，则直接执行 uxeof
readerr: /* Read error. If feof(fp) is true, fall through to unexpected EOF. */
    if (!feof(fp)) {
        // 退出前释放伪client的空间
        if (fakeClient) freeFakeClient(fakeClient); /* avoid valgrind warning */
        serverLog(LL_WARNING,"Unrecoverable error reading the append only file: %s", strerror(errno));
        exit(1);
    }

// 不被预期的AOF文件结束格式
uxeof: /* Unexpected AOF end of file. */
    // 如果发现末尾结束格式不完整则自动截掉,成功加载前面正确的数据。
    if (server.aof_load_truncated) {
        serverLog(LL_WARNING,"!!! Warning: short read while loading the AOF file !!!");
        serverLog(LL_WARNING,"!!! Truncating the AOF at offset %llu !!!",
            (unsigned long long) valid_up_to);
        // 截断文件到正确加载的位置
        if (valid_up_to == -1 || truncate(filename,valid_up_to) == -1) {
            if (valid_up_to == -1) {
                serverLog(LL_WARNING,"Last valid command offset is invalid");
            } else {
                serverLog(LL_WARNING,"Error truncating the AOF file: %s",
                    strerror(errno));
            }
        } else {
            /* Make sure the AOF file descriptor points to the end of the
             * file after the truncate call. */
            // 确保截断后的文件指针指向文件的末尾
            if (server.aof_fd != -1 && lseek(server.aof_fd,0,SEEK_END) == -1) {
                serverLog(LL_WARNING,"Can't seek the end of the AOF file: %s",
                    strerror(errno));
            } else {
                serverLog(LL_WARNING,
                    "AOF loaded anyway because aof-load-truncated is enabled");
                goto loaded_ok; //跳转到loaded_ok，表截断成功，成功加载前面正确的数据。
            }
        }
    }
    // 退出前释放伪client的空间
    if (fakeClient) freeFakeClient(fakeClient); /* avoid valgrind warning */
    serverLog(LL_WARNING,"Unexpected end of file reading the append only file. You can: 1) Make a backup of your AOF file, then use ./redis-check-aof --fix <filename>. 2) Alternatively you can set the 'aof-load-truncated' configuration option to yes and restart the server.");
    exit(1);

// 格式错误
fmterr: /* Format error. */
    // 退出前释放伪client的空间
    if (fakeClient) freeFakeClient(fakeClient); /* avoid valgrind warning */
    serverLog(LL_WARNING,"Bad file format reading the append only file: make a backup of your AOF file, then use ./redis-check-aof --fix <filename>");
    exit(1);
}

/* ----------------------------------------------------------------------------
 * AOF rewrite
 * ------------------------------------------------------------------------- */
// AOF 重写
/* Delegate writing an object to writing a bulk string or bulk long long.
 * This is not placed in rio.c since that adds the server.h dependency. */
// 将obj对象按照格式写入rio中，返回写入的字节数
int rioWriteBulkObject(rio *r, robj *obj) {
    /* Avoid using getDecodedObject to help copy-on-write (we are often
     * in a child process when this function is called). */
    // obj对象指向的是整数类型
    if (obj->encoding == OBJ_ENCODING_INT) {
        return rioWriteBulkLongLong(r,(long)obj->ptr);
    // obj对象指向的是字符串类型
    } else if (sdsEncodedObject(obj)) {
        return rioWriteBulkString(r,obj->ptr,sdslen(obj->ptr));
    } else {
        serverPanic("Unknown string encoding");
    }
}

/* Emit the commands needed to rebuild a list object.
 * The function returns 0 on error, 1 on success. */
// 重建一个列表对象，将所需要的命令写到rio中，成功返回1，出错返回0
int rewriteListObject(rio *r, robj *key, robj *o) {
    long long count = 0, items = listTypeLength(o);//列表的成员个数

    // 列表对象的编码是quicklist
    if (o->encoding == OBJ_ENCODING_QUICKLIST) {
        quicklist *list = o->ptr;
        // 创建一个quicklist的迭代器
        quicklistIter *li = quicklistGetIterator(list, AL_START_HEAD);
        quicklistEntry entry;

        // 遍历所有的entry节点，把当前指向的entry信息保存到quicklistEntry结构中
        while (quicklistNext(li,&entry)) {
            // 第一次先构建一个 RPUSH KEY
            if (count == 0) {
                // 列表命令的元素必须不能超过64个，以防止client输入缓冲区溢出
                int cmd_items = (items > AOF_REWRITE_ITEMS_PER_CMD) ?
                    AOF_REWRITE_ITEMS_PER_CMD : items;
                // "*<count>\r\n"
                if (rioWriteBulkCount(r,'*',2+cmd_items) == 0) return 0;
                // "$<5>\r\n<RPUSH>\r\n"
                if (rioWriteBulkString(r,"RPUSH",5) == 0) return 0;
                // "$<key_len>\r\n<key>\r\n"
                if (rioWriteBulkObject(r,key) == 0) return 0;
            }

            // 如果是字符串类型的值
            if (entry.value) {
                // "$<str_value_len>\r\n<str_value>\r\n"
                if (rioWriteBulkString(r,(char*)entry.value,entry.sz) == 0) return 0;
            // 如果是整数类型的值
            } else {
                // "$<longlong_value_len>\r\n<longlong_value>\r\n"
                if (rioWriteBulkLongLong(r,entry.longval) == 0) return 0;
            }
            // 如果当前构建的 RPUSH KEY 到达64个成员，那么重新构建一个RPUSH KEY
            if (++count == AOF_REWRITE_ITEMS_PER_CMD) count = 0;
            // 当剩余的成员个数小于64个，只需构建items+2个标识
            items--;
        }
        quicklistReleaseIterator(li);
    } else {
        serverPanic("Unknown list encoding");
    }
    return 1;
}

/* Emit the commands needed to rebuild a set object.
 * The function returns 0 on error, 1 on success. */
// 重建一个集合对象，将所需要的命令写到rio中，成功返回1，出错返回0
int rewriteSetObject(rio *r, robj *key, robj *o) {
    long long count = 0, items = setTypeSize(o);

    // 集合对象编码类型为intset
    if (o->encoding == OBJ_ENCODING_INTSET) {
        int ii = 0;
        int64_t llval;

        // 遍历intset，将下标ii的元素保存到llval中
        while(intsetGet(o->ptr,ii++,&llval)) {
            // 第一次先构建一个 SADD KEY
            if (count == 0) {
                int cmd_items = (items > AOF_REWRITE_ITEMS_PER_CMD) ?
                    AOF_REWRITE_ITEMS_PER_CMD : items;

                // 按格式构建一个 SADD KEY
                if (rioWriteBulkCount(r,'*',2+cmd_items) == 0) return 0;
                if (rioWriteBulkString(r,"SADD",4) == 0) return 0;
                if (rioWriteBulkObject(r,key) == 0) return 0;
            }
            // 写入当前整数值
            if (rioWriteBulkLongLong(r,llval) == 0) return 0;
            if (++count == AOF_REWRITE_ITEMS_PER_CMD) count = 0;
            items--;
        }
    // 集合对象编码类型为字典
    } else if (o->encoding == OBJ_ENCODING_HT) {
        dictIterator *di = dictGetIterator(o->ptr);
        dictEntry *de;

        // 遍历字典节点，返回当前的字典节点地址
        while((de = dictNext(di)) != NULL) {
            // 取出当前节点保存的值对象
            robj *eleobj = dictGetKey(de);
            // 第一次先构建一个 SADD KEY
            if (count == 0) {
                int cmd_items = (items > AOF_REWRITE_ITEMS_PER_CMD) ?
                    AOF_REWRITE_ITEMS_PER_CMD : items;
                // 按格式构建一个 SADD KEY
                if (rioWriteBulkCount(r,'*',2+cmd_items) == 0) return 0;
                if (rioWriteBulkString(r,"SADD",4) == 0) return 0;
                if (rioWriteBulkObject(r,key) == 0) return 0;
            }
            // 写入当前值对象
            if (rioWriteBulkObject(r,eleobj) == 0) return 0;
            if (++count == AOF_REWRITE_ITEMS_PER_CMD) count = 0;
            items--;
        }
        dictReleaseIterator(di);
    } else {
        serverPanic("Unknown set encoding");
    }
    return 1;
}

/* Emit the commands needed to rebuild a sorted set object.
 * The function returns 0 on error, 1 on success. */
// 重建一个有序集合对象，将所需要的命令写到rio中，成功返回1，出错返回0
int rewriteSortedSetObject(rio *r, robj *key, robj *o) {
    long long count = 0, items = zsetLength(o);

    // 有序集合对象编码类型为ziplist
    if (o->encoding == OBJ_ENCODING_ZIPLIST) {
        unsigned char *zl = o->ptr;
        unsigned char *eptr, *sptr;
        unsigned char *vstr;
        unsigned int vlen;
        long long vll;
        double score;
        // 元素节点
        eptr = ziplistIndex(zl,0);
        serverAssert(eptr != NULL);
        // 分数节点
        sptr = ziplistNext(zl,eptr);
        serverAssert(sptr != NULL);

        // 遍历所有的节点
        while (eptr != NULL) {
            // 获取元素节点值
            serverAssert(ziplistGet(eptr,&vstr,&vlen,&vll));
            // 获取分值
            score = zzlGetScore(sptr);

            // 第一次先构建一个 ZADD KEY
            if (count == 0) {
                int cmd_items = (items > AOF_REWRITE_ITEMS_PER_CMD) ?
                    AOF_REWRITE_ITEMS_PER_CMD : items;

                if (rioWriteBulkCount(r,'*',2+cmd_items*2) == 0) return 0;
                if (rioWriteBulkString(r,"ZADD",4) == 0) return 0;
                if (rioWriteBulkObject(r,key) == 0) return 0;
            }
            // 写入分值
            if (rioWriteBulkDouble(r,score) == 0) return 0;
            // 写入成员值
            if (vstr != NULL) {
                if (rioWriteBulkString(r,(char*)vstr,vlen) == 0) return 0;
            } else {
                if (rioWriteBulkLongLong(r,vll) == 0) return 0;
            }
            // 指向下一对元素和分值
            zzlNext(zl,&eptr,&sptr);
            if (++count == AOF_REWRITE_ITEMS_PER_CMD) count = 0;
            items--;
        }
    // 有序集合对象编码类型为跳跃表
    } else if (o->encoding == OBJ_ENCODING_SKIPLIST) {
        zset *zs = o->ptr;
        dictIterator *di = dictGetIterator(zs->dict);
        dictEntry *de;

        // 遍历所有节点
        while((de = dictNext(di)) != NULL) {
            // 获取成员对象和分值
            robj *eleobj = dictGetKey(de);
            double *score = dictGetVal(de);
            // 第一次先构建一个 ZADD KEY
            if (count == 0) {
                int cmd_items = (items > AOF_REWRITE_ITEMS_PER_CMD) ?
                    AOF_REWRITE_ITEMS_PER_CMD : items;

                if (rioWriteBulkCount(r,'*',2+cmd_items*2) == 0) return 0;
                if (rioWriteBulkString(r,"ZADD",4) == 0) return 0;
                if (rioWriteBulkObject(r,key) == 0) return 0;
            }
            // 写入分值和成员对象
            if (rioWriteBulkDouble(r,*score) == 0) return 0;
            if (rioWriteBulkObject(r,eleobj) == 0) return 0;
            if (++count == AOF_REWRITE_ITEMS_PER_CMD) count = 0;
            items--;
        }
        dictReleaseIterator(di);
    } else {
        serverPanic("Unknown sorted zset encoding");
    }
    return 1;
}

/* Write either the key or the value of the currently selected item of a hash.
 * The 'hi' argument passes a valid Redis hash iterator.
 * The 'what' filed specifies if to write a key or a value and can be
 * either OBJ_HASH_KEY or OBJ_HASH_VALUE.
 *
 * The function returns 0 on error, non-zero on success. */
// 选择哈希对象迭代器指向节点中的键或值对象写入到rio中，出错返回0，成功返回非0，what指定是field或value
static int rioWriteHashIteratorCursor(rio *r, hashTypeIterator *hi, int what) {
    // 哈希对象编码为ziplist
    if (hi->encoding == OBJ_ENCODING_ZIPLIST) {
        unsigned char *vstr = NULL;
        unsigned int vlen = UINT_MAX;
        long long vll = LLONG_MAX;

        // 从ziplist类型的哈希类型迭代器中获取对应的field或value，保存在参数中
        hashTypeCurrentFromZiplist(hi, what, &vstr, &vlen, &vll);
        // 将field或value写入rio
        if (vstr) {
            return rioWriteBulkString(r, (char*)vstr, vlen);
        } else {
            return rioWriteBulkLongLong(r, vll);
        }

    // 哈希对象编码为字典
    } else if (hi->encoding == OBJ_ENCODING_HT) {
        robj *value;

        // 从ht字典类型的哈希类型迭代器中获取对应的field或value，保存在参数中
        hashTypeCurrentFromHashTable(hi, what, &value);
        // 将field或value写入rio
        return rioWriteBulkObject(r, value);
    }

    serverPanic("Unknown hash encoding");
    return 0;
}

/* Emit the commands needed to rebuild a hash object.
 * The function returns 0 on error, 1 on success. */
// 重建一个哈希对象，将所需要的命令写到rio中，成功返回1，出错返回0
int rewriteHashObject(rio *r, robj *key, robj *o) {
    hashTypeIterator *hi;
    long long count = 0, items = hashTypeLength(o); //哈希对象的节点个数

    // 创建哈希类型迭代器
    hi = hashTypeInitIterator(o);
    // 遍历哈希对象节点
    while (hashTypeNext(hi) != C_ERR) {
        if (count == 0) {
            int cmd_items = (items > AOF_REWRITE_ITEMS_PER_CMD) ?
                AOF_REWRITE_ITEMS_PER_CMD : items;
            // 第一次先构建一个 HMSET KEY
            if (rioWriteBulkCount(r,'*',2+cmd_items*2) == 0) return 0;
            if (rioWriteBulkString(r,"HMSET",5) == 0) return 0;
            if (rioWriteBulkObject(r,key) == 0) return 0;
        }
        // 将哈希对象的键值对对象写入到rio中
        if (rioWriteHashIteratorCursor(r, hi, OBJ_HASH_KEY) == 0) return 0;
        if (rioWriteHashIteratorCursor(r, hi, OBJ_HASH_VALUE) == 0) return 0;
        if (++count == AOF_REWRITE_ITEMS_PER_CMD) count = 0;
        items--;
    }

    hashTypeReleaseIterator(hi);

    return 1;
}

/* This function is called by the child rewriting the AOF file to read
 * the difference accumulated from the parent into a buffer, that is
 * concatenated at the end of the rewrite. */
// 该函数在子进程正在进行重写AOF文件时调用
// 用来读从父进程累计写入的缓冲区的差异，在重写结束时链接到文件的结尾
ssize_t aofReadDiffFromParent(void) {
    // 大多数Linux系统中默认的管道大小
    char buf[65536]; /* Default pipe buffer size on most Linux systems. */
    ssize_t nread, total = 0;

    // 从父进程读数据到buf中，读了nread个字节
    while ((nread =
            read(server.aof_pipe_read_data_from_parent,buf,sizeof(buf))) > 0) {
        // 将buf中的数据累计到子进程的差异累计的sds中
        server.aof_child_diff = sdscatlen(server.aof_child_diff,buf,nread);
        // 更新总的累计字节数
        total += nread;
    }
    return total;
}

/* Write a sequence of commands able to fully rebuild the dataset into
 * "filename". Used both by REWRITEAOF and BGREWRITEAOF.
 *
 * In order to minimize the number of commands needed in the rewritten
 * log Redis uses variadic commands when possible, such as RPUSH, SADD
 * and ZADD. However at max AOF_REWRITE_ITEMS_PER_CMD items per time
 * are inserted using a single command. */
// 写一系列的命令，用来完全重建数据集到filename文件中，被 REWRITEAOF and BGREWRITEAOF调用
// 为了使重建数据集的命令数量最小，Redis会使用 可变参的命令，例如RPUSH, SADD 和 ZADD。
// 然而每次单个命令的元素数量不能超过AOF_REWRITE_ITEMS_PER_CMD
int rewriteAppendOnlyFile(char *filename) {
    dictIterator *di = NULL;
    dictEntry *de;
    rio aof;
    FILE *fp;
    char tmpfile[256];
    int j;
    long long now = mstime();
    char byte;
    size_t processed = 0;

    /* Note that we have to use a different temp name here compared to the
     * one used by rewriteAppendOnlyFileBackground() function. */
    // 创建临时文件的名字保存到tmpfile中
    snprintf(tmpfile,256,"temp-rewriteaof-%d.aof", (int) getpid());
    // 打开文件
    fp = fopen(tmpfile,"w");
    if (!fp) {
        serverLog(LL_WARNING, "Opening the temp file for AOF rewrite in rewriteAppendOnlyFile(): %s", strerror(errno));
        return C_ERR;
    }
    // 设置一个空sds给 保存子进程AOF时差异累计数据的sds
    server.aof_child_diff = sdsempty();
    // 初始化rio为文件io对象
    rioInitWithFile(&aof,fp);
    // 如果开启了增量时同步，防止在缓存中累计太多命令，造成写入时IO阻塞时间过长
    if (server.aof_rewrite_incremental_fsync)
        // 设置自动同步的字节数限制为AOF_AUTOSYNC_BYTES = 32MB
        rioSetAutoSync(&aof,AOF_AUTOSYNC_BYTES);

    // 遍历所有的数据库
    for (j = 0; j < server.dbnum; j++) {
        // 按照格式构建 SELECT 命令内容
        char selectcmd[] = "*2\r\n$6\r\nSELECT\r\n";
        // 当前数据库指针
        redisDb *db = server.db+j;
        // 数据库的键值对字典
        dict *d = db->dict;
        // 如果数据库中没有键值对则跳过当前数据库
        if (dictSize(d) == 0) continue;
        // 创建一个安全的字典迭代器
        di = dictGetSafeIterator(d);
        if (!di) {
            // 创建失败返回C_ERR
            fclose(fp);
            return C_ERR;
        }

        /* SELECT the new DB */
        // 将SELECT 命令写入AOF文件，确保后面的命令能正确载入到数据库
        if (rioWrite(&aof,selectcmd,sizeof(selectcmd)-1) == 0) goto werr;
        // 将数据库的ID吸入AOF文件
        if (rioWriteBulkLongLong(&aof,j) == 0) goto werr;

        /* Iterate this DB writing every entry */
        // 遍历保存当前数据的键值对的字典
        while((de = dictNext(di)) != NULL) {
            sds keystr;
            robj key, *o;
            long long expiretime;

            // 当前节点保存的键值
            keystr = dictGetKey(de);
            // 当前节点保存的值对象
            o = dictGetVal(de);
            // 初始化一个在栈中分配的键对象
            initStaticStringObject(key,keystr);

            // 获取该键值对的过期时间
            expiretime = getExpire(db,&key);

            /* If this key is already expired skip it */
            // 如果当前键已经过期，则跳过该键
            if (expiretime != -1 && expiretime < now) continue;

            /* Save the key and associated value */
            // 根据值的对象类型，将键值对写到AOF文件中

            // 值为字符串类型对象
            if (o->type == OBJ_STRING) {
                /* Emit a SET command */
                char cmd[]="*3\r\n$3\r\nSET\r\n";
                // 按格式写入SET命令
                if (rioWrite(&aof,cmd,sizeof(cmd)-1) == 0) goto werr;
                /* Key and value */
                // 按格式写入键值对对象
                if (rioWriteBulkObject(&aof,&key) == 0) goto werr;
                if (rioWriteBulkObject(&aof,o) == 0) goto werr;
            // 值为列表类型对象
            } else if (o->type == OBJ_LIST) {
                // 重建一个列表对象命令，将键值对按格式写入
                if (rewriteListObject(&aof,&key,o) == 0) goto werr;
            // 值为集合类型对象
            } else if (o->type == OBJ_SET) {
                // 重建一个集合对象命令，将键值对按格式写入
                if (rewriteSetObject(&aof,&key,o) == 0) goto werr;
            // 值为有序集合类型对象
            } else if (o->type == OBJ_ZSET) {
                // 重建一个有序集合对象命令，将键值对按格式写入
                if (rewriteSortedSetObject(&aof,&key,o) == 0) goto werr;
            // 值为哈希类型对象
            } else if (o->type == OBJ_HASH) {
                // 重建一个哈希对象命令，将键值对按格式写入
                if (rewriteHashObject(&aof,&key,o) == 0) goto werr;
            } else {
                serverPanic("Unknown object type");
            }
            /* Save the expire time */
            // 如果该键有过期时间，且没过期，写入过期时间
            if (expiretime != -1) {
                char cmd[]="*3\r\n$9\r\nPEXPIREAT\r\n";
                // 将过期键时间全都以Unix时间写入
                if (rioWrite(&aof,cmd,sizeof(cmd)-1) == 0) goto werr;
                if (rioWriteBulkObject(&aof,&key) == 0) goto werr;
                if (rioWriteBulkLongLong(&aof,expiretime) == 0) goto werr;
            }
            /* Read some diff from the parent process from time to time. */
            // 在rio的缓存中每次写了10M，就从父进程读累计的差异，保存到子进程的aof_child_diff中
            if (aof.processed_bytes > processed+1024*10) {
                // 更新已写的字节数
                processed = aof.processed_bytes;
                // 从父进程累计写入的缓冲区的差异，在重写结束时链接到文件的结尾
                aofReadDiffFromParent();
            }
        }
        dictReleaseIterator(di);    //释放字典迭代器
        di = NULL;
    }

    /* Do an initial slow fsync here while the parent is still sending
     * data, in order to make the next final fsync faster. */
    // 当父进程仍然在发送数据时，先执行一个缓慢的同步，以便下一次最中的同步更快
    if (fflush(fp) == EOF) goto werr;
    if (fsync(fileno(fp)) == -1) goto werr;

    /* Read again a few times to get more data from the parent.
     * We can't read forever (the server may receive data from clients
     * faster than it is able to send data to the child), so we try to read
     * some more data in a loop as soon as there is a good chance more data
     * will come. If it looks like we are wasting time, we abort (this
     * happens after 20 ms without new data). */
    // 再次从父进程读取几次数据，以获得更多的数据，我们无法一直读取，因为服务器从client接受的数据总是比发送给子进程要快，所以当数据来临的时候，我们尝试从在循环中多次读取。
    // 如果在20ms之内没有新的数据到来，那么我们终止读取
    int nodata = 0;
    mstime_t start = mstime();  //读取的开始时间
    // 在20ms之内等待数据到来
    while(mstime()-start < 1000 && nodata < 20) {
        // 在1ms之内，查看从父进程读数据的fd是否变成可读的，若不可读则aeWait()函数返回0
        if (aeWait(server.aof_pipe_read_data_from_parent, AE_READABLE, 1) <= 0)
        {
            nodata++;   //更新新数据到来的时间，超过20ms则退出while循环
            continue;
        }
        // 当管道的读端可读时，清零nodata
        nodata = 0; /* Start counting from zero, we stop on N *contiguous*
                       timeouts. */
        // 从父进程累计写入的缓冲区的差异，在重写结束时链接到文件的结尾
        aofReadDiffFromParent();
    }

    /* Ask the master to stop sending diffs. */
    // 请求父进程停止发送累计差异数据
    if (write(server.aof_pipe_write_ack_to_parent,"!",1) != 1) goto werr;
    // 将从父进程读ack的fd设置为非阻塞模式
    if (anetNonBlock(NULL,server.aof_pipe_read_ack_from_parent) != ANET_OK)
        goto werr;
    /* We read the ACK from the server using a 10 seconds timeout. Normally
     * it should reply ASAP, but just in case we lose its reply, we are sure
     * the child will eventually get terminated. */
    // 在5000ms之内，从fd读1个字节的数据保存在byte中，查看byte是否是'!'
    if (syncRead(server.aof_pipe_read_ack_from_parent,&byte,1,5000) != 1 ||
        byte != '!') goto werr;
    // 如果收到的是父进程发来的'!'，则打印日志
    serverLog(LL_NOTICE,"Parent agreed to stop sending diffs. Finalizing AOF...");

    /* Read the final diff if any. */
    // 最后一次从父进程累计写入的缓冲区的差异
    aofReadDiffFromParent();

    /* Write the received diff to the file. */
    serverLog(LL_NOTICE,
        "Concatenating %.2f MB of AOF diff received from parent.",
        (double) sdslen(server.aof_child_diff) / (1024*1024));
    // 将子进程aof_child_diff中保存的差异数据写到AOF文件中
    if (rioWrite(&aof,server.aof_child_diff,sdslen(server.aof_child_diff)) == 0)
        goto werr;

    /* Make sure data will not remain on the OS's output buffers */
    // 再次冲洗文件缓冲区，执行同步操作
    if (fflush(fp) == EOF) goto werr;
    if (fsync(fileno(fp)) == -1) goto werr;
    if (fclose(fp) == EOF) goto werr;   //关闭文件

    /* Use RENAME to make sure the DB file is changed atomically only
     * if the generate DB file is ok. */
    // 原子性的将临时文件的名字，改成appendonly.aof
    if (rename(tmpfile,filename) == -1) {
        serverLog(LL_WARNING,"Error moving temp append only file on the final destination: %s", strerror(errno));
        unlink(tmpfile);
        return C_ERR;
    }
    // 打印日志
    serverLog(LL_NOTICE,"SYNC append only file rewrite performed");
    return C_OK;

// 写错误处理
werr:
    serverLog(LL_WARNING,"Write error writing append only file on disk: %s", strerror(errno));
    fclose(fp);
    unlink(tmpfile);
    if (di) dictReleaseIterator(di);
    return C_ERR;
}

/* ----------------------------------------------------------------------------
 * AOF rewrite pipes for IPC
 * -------------------------------------------------------------------------- */
// AOF重写管道的进程间通信
/* This event handler is called when the AOF rewriting child sends us a
 * single '!' char to signal we should stop sending buffer diffs. The
 * parent sends a '!' as well to acknowledge. */
// 当AOF重写的子进程发送一个'!'字符信号，调用这个事件处理程序，告知我们应该停止发送缓冲区差异数据
// 父进程也发送给一个'!'字符信号去告知收到。
void aofChildPipeReadable(aeEventLoop *el, int fd, void *privdata, int mask) {
    char byte;
    UNUSED(el);
    UNUSED(privdata);
    UNUSED(mask);

    // 从fd中读一个字节的数据，且是'!'字符信号
    if (read(fd,&byte,1) == 1 && byte == '!') {
        //打印日志
        serverLog(LL_NOTICE,"AOF rewrite child asks to stop sending diffs.");
        // 停止发送累计的不同数据给子进程标志设置为真
        server.aof_stop_sending_diff = 1;
        // 应答给子进程一个'!'字符信号
        if (write(server.aof_pipe_write_ack_to_child,"!",1) != 1) {
            /* If we can't send the ack, inform the user, but don't try again
             * since in the other side the children will use a timeout if the
             * kernel can't buffer our write, or, the children was
             * terminated. */
            serverLog(LL_WARNING,"Can't send ACK to AOF child: %s",
                strerror(errno));
        }
    }
    /* Remove the handler since this can be called only one time during a
     * rewrite. */
    // 将从子进程读ack文件描述符fd从监听队列中删除
    aeDeleteFileEvent(server.el,server.aof_pipe_read_ack_from_child,AE_READABLE);
}

/* Create the pipes used for parent - child process IPC during rewrite.
 * We have a data pipe used to send AOF incremental diffs to the child,
 * and two other pipes used by the children to signal it finished with
 * the rewrite so no more data should be written, and another for the
 * parent to acknowledge it understood this new condition. */
// 在重写期间创建一个用来在父子进程通信的管道。有一个数据管道用于子进程发送AOF增量差异，另外两个由子进程使用完成重写操作，所以不应该写入更多的数据，另一个用于父进程发送ack应答信号取确认这个新条件。
int aofCreatePipes(void) {
    int fds[6] = {-1, -1, -1, -1, -1, -1};
    int j;

    // 创建管道
    if (pipe(fds) == -1) goto error; /* parent -> children data. */
    if (pipe(fds+2) == -1) goto error; /* children -> parent ack. */
    if (pipe(fds+4) == -1) goto error; /* children -> parent ack. */
    /* Parent -> children data is non blocking. */
    // 将两条发送数据管道设置为非阻塞模式
    if (anetNonBlock(NULL,fds[0]) != ANET_OK) goto error;
    if (anetNonBlock(NULL,fds[1]) != ANET_OK) goto error;
    // 监听aof_pipe_read_ack_from_child的状态，如果可读，则调用aofChildPipeReadable
    if (aeCreateFileEvent(server.el, fds[2], AE_READABLE, aofChildPipeReadable, NULL) == AE_ERR) goto error;

    server.aof_pipe_write_data_to_child = fds[1];
    server.aof_pipe_read_data_from_parent = fds[0];
    server.aof_pipe_write_ack_to_parent = fds[3];
    server.aof_pipe_read_ack_from_child = fds[2];
    server.aof_pipe_write_ack_to_child = fds[5];
    server.aof_pipe_read_ack_from_parent = fds[4];
    // 停止发送累计的不同数据给子进程标志设置为假
    server.aof_stop_sending_diff = 0;
    return C_OK;

// 错误处理，关闭fd
error:
    serverLog(LL_WARNING,"Error opening /setting AOF rewrite IPC pipes: %s",
        strerror(errno));
    for (j = 0; j < 6; j++) if(fds[j] != -1) close(fds[j]);
    return C_ERR;
}

// 关闭所有管道
void aofClosePipes(void) {
    // 将aof_pipe_read_ack_from_child和aof_pipe_write_data_to_child从监听的队列中删除
    aeDeleteFileEvent(server.el,server.aof_pipe_read_ack_from_child,AE_READABLE);
    aeDeleteFileEvent(server.el,server.aof_pipe_write_data_to_child,AE_WRITABLE);
    // 关闭所有fd
    close(server.aof_pipe_write_data_to_child);
    close(server.aof_pipe_read_data_from_parent);
    close(server.aof_pipe_write_ack_to_parent);
    close(server.aof_pipe_read_ack_from_child);
    close(server.aof_pipe_write_ack_to_child);
    close(server.aof_pipe_read_ack_from_parent);
}

/* ----------------------------------------------------------------------------
 * AOF background rewrite
 * ------------------------------------------------------------------------- */

/* This is how rewriting of the append only file in background works:
 *
 * 1) The user calls BGREWRITEAOF
 * 2) Redis calls this function, that forks():
 *    2a) the child rewrite the append only file in a temp file.
 *    2b) the parent accumulates differences in server.aof_rewrite_buf.
 * 3) When the child finished '2a' exists.
 * 4) The parent will trap the exit code, if it's OK, will append the
 *    data accumulated into server.aof_rewrite_buf into the temp file, and
 *    finally will rename(2) the temp file in the actual file name.
 *    The the new file is reopened as the new append only file. Profit!
 */
// 以下是BGREWRITEAOF的工作步骤
// 1. 用户调用BGREWRITEAOF
// 2. Redis调用这个函数，它执行fork()
//      2.1 子进程在临时文件中执行重写操作
//      2.2 父进程将累计的差异数据追加到server.aof_rewrite_buf中
// 3. 当子进程完成2.1
// 4. 父进程会捕捉到子进程的退出码，如果是OK，那么追加累计的差异数据到临时文件，并且对临时文件rename，用它代替旧的AOF文件，然后就完成AOF的重写。
int rewriteAppendOnlyFileBackground(void) {
    pid_t childpid;
    long long start;

    // 如果正在进行重写或正在进行RDB持久化操作，则返回C_ERR
    if (server.aof_child_pid != -1 || server.rdb_child_pid != -1) return C_ERR;
    // 创建父子进程间通信的管道
    if (aofCreatePipes() != C_OK) return C_ERR;
    // 记录fork()开始时间
    start = ustime();

    // 子进程
    if ((childpid = fork()) == 0) {
        char tmpfile[256];

        /* Child */
        // 关闭监听的套接字
        closeListeningSockets(0);
        // 设置进程名字
        redisSetProcTitle("redis-aof-rewrite");
        // 创建临时文件
        snprintf(tmpfile,256,"temp-rewriteaof-bg-%d.aof", (int) getpid());
        // 对临时文件进行AOF重写
        if (rewriteAppendOnlyFile(tmpfile) == C_OK) {
            // 获取子进程使用的内存空间大小
            size_t private_dirty = zmalloc_get_private_dirty();

            if (private_dirty) {
                serverLog(LL_NOTICE,
                    "AOF rewrite: %zu MB of memory used by copy-on-write",
                    private_dirty/(1024*1024));
            }
            // 成功退出子进程
            exitFromChild(0);
        } else {
            // 异常退出子进程
            exitFromChild(1);
        }

    // 父进程
    } else {
        /* Parent */
        // 设置fork()函数消耗的时间
        server.stat_fork_time = ustime()-start;
        // 计算fork的速率，GB/每秒
        server.stat_fork_rate = (double) zmalloc_used_memory() * 1000000 / server.stat_fork_time / (1024*1024*1024); /* GB per second. */
        // 将"fork"和fork消耗的时间关联到延迟诊断字典中
        latencyAddSampleIfNeeded("fork",server.stat_fork_time/1000);
        if (childpid == -1) {
            serverLog(LL_WARNING,
                "Can't rewrite append only file in background: fork: %s",
                strerror(errno));
            return C_ERR;
        }
        // 打印日志
        serverLog(LL_NOTICE,
            "Background append only file rewriting started by pid %d",childpid);
        // 将AOF日程标志清零
        server.aof_rewrite_scheduled = 0;
        // AOF开始的时间
        server.aof_rewrite_time_start = time(NULL);
        // 设置AOF重写的子进程pid
        server.aof_child_pid = childpid;
        // 在AOF或RDB期间，不能对哈希表进行resize操作
        updateDictResizePolicy();
        /* We set appendseldb to -1 in order to force the next call to the
         * feedAppendOnlyFile() to issue a SELECT command, so the differences
         * accumulated by the parent into server.aof_rewrite_buf will start
         * with a SELECT statement and it will be safe to merge. */
        // 将aof_selected_db设置为-1，强制让feedAppendOnlyFile函数执行时，执行一个select命令
        server.aof_selected_db = -1;
        // 清空脚本缓存
        replicationScriptCacheFlush();
        return C_OK;
    }
    return C_OK; /* unreached */
}

// BGREWRITEAOF 命令实现
void bgrewriteaofCommand(client *c) {
    // 如果已经正在进行AOF重写，则不能重复执行BGREWRITEAOF
    if (server.aof_child_pid != -1) {
        addReplyError(c,"Background append only file rewriting already in progress");
    // 如果正在执行RDB，那么将AOF重写提上日程表，将aof_rewrite_scheduled设置为1
    } else if (server.rdb_child_pid != -1) {
        server.aof_rewrite_scheduled = 1;
        addReplyStatus(c,"Background append only file rewriting scheduled");
    // 执行重写操作
    } else if (rewriteAppendOnlyFileBackground() == C_OK) {
        addReplyStatus(c,"Background append only file rewriting started");
    } else {
        addReply(c,shared.err);
    }
}

// 删除临时文件
void aofRemoveTempFile(pid_t childpid) {
    char tmpfile[256];

    snprintf(tmpfile,256,"temp-rewriteaof-bg-%d.aof", (int) childpid);
    unlink(tmpfile);
}

/* Update the server.aof_current_size field explicitly using stat(2)
 * to check the size of the file. This is useful after a rewrite or after
 * a restart, normally the size is updated just adding the write length
 * to the current length, that is much faster. */
// 更新AOF文件的当前的大小，记录到服务器状态中，用于BGREWRITEAOF执行后，或服务器重启时
void aofUpdateCurrentSize(void) {
    struct redis_stat sb;
    mstime_t latency;

    // 设置延迟检测开始的时间
    latencyStartMonitor(latency);
    // 读取AOF文件的信息到sb中
    if (redis_fstat(server.aof_fd,&sb) == -1) {
        serverLog(LL_WARNING,"Unable to obtain the AOF file length. stat: %s",
            strerror(errno));
    } else {
        // 更新AOF文件的大小
        server.aof_current_size = sb.st_size;
    }
    // 设置延迟的时间 = 当前的时间 - 开始的时间
    latencyEndMonitor(latency);
    // latency超过设置的latency_monitor_threshold阀值，则将latency和"aof-fstat"关联到延迟诊断字典中
    latencyAddSampleIfNeeded("aof-fstat",latency);
}

/* A background append only file rewriting (BGREWRITEAOF) terminated its work.
 * Handle this. */
// 子进程AOF重写执行完成后，父进程调用该函数
void backgroundRewriteDoneHandler(int exitcode, int bysignal) {
    // 子进程正常退出
    if (!bysignal && exitcode == 0) {
        int newfd, oldfd;
        char tmpfile[256];
        long long now = ustime();
        mstime_t latency;

        serverLog(LL_NOTICE,
            "Background AOF rewrite terminated with success");

        /* Flush the differences accumulated by the parent to the
         * rewritten AOF. */
        // 设置延迟检测开始的时间
        latencyStartMonitor(latency);
        snprintf(tmpfile,256,"temp-rewriteaof-bg-%d.aof",
            (int)server.aof_child_pid);
        // 打开临时文件，追加写入
        newfd = open(tmpfile,O_WRONLY|O_APPEND);
        if (newfd == -1) {
            serverLog(LL_WARNING,
                "Unable to open the temporary AOF produced by the child: %s", strerror(errno));
            goto cleanup;
        }

        // 将缓冲区的数据追加写到制定的文件描述符fd中，成功返回写入的字节数，否则返回-1
        if (aofRewriteBufferWrite(newfd) == -1) {
            serverLog(LL_WARNING,
                "Error trying to flush the parent diff to the rewritten AOF: %s", strerror(errno));
            close(newfd);
            goto cleanup;
        }
        // 设置延迟的时间 = 当前的时间 - 开始的时间
        latencyEndMonitor(latency);
        // latency超过设置的latency_monitor_threshold阀值
        // 则将latency和"aof-rewrite-diff-write"关联到延迟诊断字典中
        latencyAddSampleIfNeeded("aof-rewrite-diff-write",latency);

        serverLog(LL_NOTICE,
            "Residual parent diff successfully flushed to the rewritten AOF (%.2f MB)", (double) aofRewriteBufferSize() / (1024*1024));

        /* The only remaining thing to do is to rename the temporary file to
         * the configured file and switch the file descriptor used to do AOF
         * writes. We don't want close(2) or rename(2) calls to block the
         * server on old file deletion.
         *
         * There are two possible scenarios:
         *
         * 1) AOF is DISABLED and this was a one time rewrite. The temporary
         * file will be renamed to the configured file. When this file already
         * exists, it will be unlinked, which may block the server.
         *
         * 2) AOF is ENABLED and the rewritten AOF will immediately start
         * receiving writes. After the temporary file is renamed to the
         * configured file, the original AOF file descriptor will be closed.
         * Since this will be the last reference to that file, closing it
         * causes the underlying file to be unlinked, which may block the
         * server.
         *
         * To mitigate the blocking effect of the unlink operation (either
         * caused by rename(2) in scenario 1, or by close(2) in scenario 2), we
         * use a background thread to take care of this. First, we
         * make scenario 1 identical to scenario 2 by opening the target file
         * when it exists. The unlink operation after the rename(2) will then
         * be executed upon calling close(2) for its descriptor. Everything to
         * guarantee atomicity for this switch has already happened by then, so
         * we don't care what the outcome or duration of that close operation
         * is, as long as the file descriptor is released again. */
        // 如果AOF已经完成，被设置为关闭状态
        if (server.aof_fd == -1) {
            /* AOF disabled */

             /* Don't care if this fails: oldfd will be -1 and we handle that.
              * One notable case of -1 return is if the old file does
              * not exist. */
            // 重新非阻塞打开AOF文件，防止rename是阻塞服务器，因为rename会unlink原来的临时文件
             oldfd = open(server.aof_filename,O_RDONLY|O_NONBLOCK);
        } else {
            /* AOF enabled */
            oldfd = -1; /* We'll set this to the current AOF filedes later. */
        }

        /* Rename the temporary file. This will not unlink the target file if
         * it exists, because we reference it with "oldfd". */
        // 设置延迟检测开始的时间
        latencyStartMonitor(latency);
        // 设置AOF文件的名字，替换现有的AOF文件
        if (rename(tmpfile,server.aof_filename) == -1) {
            serverLog(LL_WARNING,
                "Error trying to rename the temporary AOF file %s into %s: %s",
                tmpfile,
                server.aof_filename,
                strerror(errno));
            close(newfd);
            if (oldfd != -1) close(oldfd);
            goto cleanup;
        }
        // 设置延迟的时间 = 当前的时间 - 开始的时间
        latencyEndMonitor(latency);
        // latency超过设置的latency_monitor_threshold阀值
        // 则将latency和"aof-rename"关联到延迟诊断字典中
        latencyAddSampleIfNeeded("aof-rename",latency);

        // 如果AOF已经完成，被设置为关闭状态，直接关闭打开的临时文件
        if (server.aof_fd == -1) {
            /* AOF disabled, we don't need to set the AOF file descriptor
             * to this new file, so we can close it. */
            close(newfd);
        } else {
            // 如果AOF没有完成，用新的AOF文件fd替换原来的AOF文件fd
            /* AOF enabled, replace the old fd with the new one. */
            oldfd = server.aof_fd;
            server.aof_fd = newfd;
            // 根据同步策略，进行同步，因为之前做了一个追加新命令到重写缓冲区
            if (server.aof_fsync == AOF_FSYNC_ALWAYS)
                aof_fsync(newfd);
            else if (server.aof_fsync == AOF_FSYNC_EVERYSEC)
                aof_background_fsync(newfd);
            // 强制引发select
            server.aof_selected_db = -1; /* Make sure SELECT is re-issued */
            // 更新AOF文件的当前的大小，记录到服务器状态中，用于BGREWRITEAOF执行后，或服务器重启时
            aofUpdateCurrentSize();
            // 更新当前的重新的大小
            server.aof_rewrite_base_size = server.aof_current_size;

            /* Clear regular AOF buffer since its contents was just written to
             * the new AOF from the background rewrite buffer. */
            // 清空AOF缓存
            sdsfree(server.aof_buf);
            server.aof_buf = sdsempty();
        }

        // 更新最近一次执行BGREWRITEAOF的状态为C_OK
        server.aof_lastbgrewrite_status = C_OK;

        serverLog(LL_NOTICE, "Background AOF rewrite finished successfully");
        /* Change state from WAIT_REWRITE to ON if needed */
        // 更新AOF的状态
        if (server.aof_state == AOF_WAIT_REWRITE)
            server.aof_state = AOF_ON;

        /* Asynchronously close the overwritten AOF. */
        // 异步关闭旧的AOF文件
        if (oldfd != -1) bioCreateBackgroundJob(BIO_CLOSE_FILE,(void*)(long)oldfd,NULL,NULL);

        serverLog(LL_VERBOSE,
            "Background AOF rewrite signal handler took %lldus", ustime()-now);
    // 如果子进程执行BGREWRITEAOF错误退出
    } else if (!bysignal && exitcode != 0) {
        /* SIGUSR1 is whitelisted, so we have a way to kill a child without
         * tirggering an error conditon. */
        // 更新最近一次执行BGREWRITEAOF的状态为C_ERR
        if (bysignal != SIGUSR1)
            server.aof_lastbgrewrite_status = C_ERR;
        serverLog(LL_WARNING,
            "Background AOF rewrite terminated with error");
    // 其他错误
    } else {
        server.aof_lastbgrewrite_status = C_ERR;

        serverLog(LL_WARNING,
            "Background AOF rewrite terminated by signal %d", bysignal);
    }

// 清空代码
cleanup:
    aofClosePipes();
    aofRewriteBufferReset();
    aofRemoveTempFile(server.aof_child_pid);
    server.aof_child_pid = -1;
    server.aof_rewrite_time_last = time(NULL)-server.aof_rewrite_time_start;
    server.aof_rewrite_time_start = -1;
    /* Schedule a new rewrite if we are waiting for it to switch the AOF ON. */
    if (server.aof_state == AOF_WAIT_REWRITE)
        server.aof_rewrite_scheduled = 1;
}
