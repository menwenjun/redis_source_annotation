/* Linux epoll(2) based ae.c module
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


#include <sys/epoll.h>

typedef struct aeApiState {
    // epoll事件的文件描述符
    int epfd;
    // 事件表
    struct epoll_event *events;
} aeApiState;   //事件的状态

// 创建一个epoll实例，保存到eventLoop中
static int aeApiCreate(aeEventLoop *eventLoop) {
    aeApiState *state = zmalloc(sizeof(aeApiState));

    if (!state) return -1;
    // 初始化事件表空间
    state->events = zmalloc(sizeof(struct epoll_event)*eventLoop->setsize);
    if (!state->events) {
        zfree(state);
        return -1;
    }
    // 创建一张事件表，返回一个文件描述符来标识这张时间表
    // 1024只是给内核的提示实际事件表需要的大小
    state->epfd = epoll_create(1024); /* 1024 is just a hint for the kernel */
    if (state->epfd == -1) {
        zfree(state->events);
        zfree(state);
        return -1;
    }
    // 保存到事件轮询状态结构中
    eventLoop->apidata = state;
    return 0;
}

// 调整事件表的大小
static int aeApiResize(aeEventLoop *eventLoop, int setsize) {
    aeApiState *state = eventLoop->apidata;
    // 重新分配空间大小
    state->events = zrealloc(state->events, sizeof(struct epoll_event)*setsize);
    return 0;
}

// 释放epoll实例和事件表空间
static void aeApiFree(aeEventLoop *eventLoop) {
    aeApiState *state = eventLoop->apidata;

    close(state->epfd);
    zfree(state->events);
    zfree(state);
}

// 在epfd标识的事件表上注册fd的事件
static int aeApiAddEvent(aeEventLoop *eventLoop, int fd, int mask) {
    aeApiState *state = eventLoop->apidata;
    struct epoll_event ee = {0}; /* avoid valgrind warning */
    /* If the fd was already monitored for some event, we need a MOD
     * operation. Otherwise we need an ADD operation. */
    // EPOLL_CTL_ADD，向epfd注册fd的上的event
    // EPOLL_CTL_MOD，修改fd已注册的event
    // #define AE_NONE 0           //未设置
    // #define AE_READABLE 1       //事件可读
    // #define AE_WRITABLE 2       //事件可写
    // 判断fd事件的操作，如果没有设置事件，则进行关联mask类型事件，否则进行修改
    int op = eventLoop->events[fd].mask == AE_NONE ?
            EPOLL_CTL_ADD : EPOLL_CTL_MOD;

    // struct epoll_event {
    //      uint32_t     events;      /* Epoll events */
    //      epoll_data_t data;        /* User data variable */
    // };
    ee.events = 0;
    // 如果是修改事件，合并之前的事件类型
    mask |= eventLoop->events[fd].mask; /* Merge old events */
    // 根据mask映射epoll的事件类型
    if (mask & AE_READABLE) ee.events |= EPOLLIN;   //读事件
    if (mask & AE_WRITABLE) ee.events |= EPOLLOUT;  //写事件
    ee.data.fd = fd;    //设置事件所从属的目标文件描述符
    // 将ee事件注册到epoll中
    if (epoll_ctl(state->epfd,op,fd,&ee) == -1) return -1;
    return 0;
}

// 在epfd标识的事件表上注删除fd的事件
static void aeApiDelEvent(aeEventLoop *eventLoop, int fd, int delmask) {
    aeApiState *state = eventLoop->apidata;
    struct epoll_event ee = {0}; /* avoid valgrind warning */
    // 删除后的事件类型
    int mask = eventLoop->events[fd].mask & (~delmask);

    ee.events = 0;
    // 根据mask映射epoll的事件类型
    if (mask & AE_READABLE) ee.events |= EPOLLIN;
    if (mask & AE_WRITABLE) ee.events |= EPOLLOUT;
    ee.data.fd = fd;    //设置事件所从属的目标文件描述符
    // 如果fd的事件不为空事件，则进行修改
    if (mask != AE_NONE) {
        epoll_ctl(state->epfd,EPOLL_CTL_MOD,fd,&ee);
    // 如果fd的事件为空了，就直接进行删除
    } else {
        /* Note, Kernel < 2.6.9 requires a non null event pointer even for
         * EPOLL_CTL_DEL. */
        epoll_ctl(state->epfd,EPOLL_CTL_DEL,fd,&ee);
    }
}

// 等待所监听文件描述符上有事件发生
static int aeApiPoll(aeEventLoop *eventLoop, struct timeval *tvp) {
    aeApiState *state = eventLoop->apidata;
    int retval, numevents = 0;

    // 监听事件表上是否有事件发生
    retval = epoll_wait(state->epfd,state->events,eventLoop->setsize,
            tvp ? (tvp->tv_sec*1000 + tvp->tv_usec/1000) : -1);
    // 至少有一个就绪的事件
    if (retval > 0) {
        int j;

        numevents = retval;
        // 遍历就绪的事件表，将其加入到eventLoop的就绪事件表中
        for (j = 0; j < numevents; j++) {
            int mask = 0;
            struct epoll_event *e = state->events+j;

            // 根据就绪的事件类型，设置mask
            if (e->events & EPOLLIN) mask |= AE_READABLE;
            if (e->events & EPOLLOUT) mask |= AE_WRITABLE;
            if (e->events & EPOLLERR) mask |= AE_WRITABLE;
            if (e->events & EPOLLHUP) mask |= AE_WRITABLE;
            // 添加到就绪事件表中
            eventLoop->fired[j].fd = e->data.fd;
            eventLoop->fired[j].mask = mask;
        }
    }
    // 返回就绪的事件个数
    return numevents;
}

// 返回正在使用的IO多路复用库的名字
static char *aeApiName(void) {
    return "epoll";
}
