/* rio.c is a simple stream-oriented I/O abstraction that provides an interface
 * to write code that can consume/produce data using different concrete input
 * and output devices. For instance the same rdb.c code using the rio
 * abstraction can be used to read and write the RDB format using in-memory
 * buffers or files.
 *
 * A rio object provides the following methods:
 *  read: read from stream.
 *  write: write to stream.
 *  tell: get the current offset.
 *
 * It is also possible to set a 'checksum' method that is used by rio.c in order
 * to compute a checksum of the data written or read, or to query the rio object
 * for the current checksum.
 *
 * ----------------------------------------------------------------------------
 *
 * Copyright (c) 2009-2012, Pieter Noordhuis <pcnoordhuis at gmail dot com>
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
// rio.c是一个抽象的IO对象，可以面向不同的输入输出设备，例如一个缓冲区IO、文件IO和socket IO

#include "fmacros.h"
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include "rio.h"
#include "util.h"
#include "crc64.h"
#include "config.h"
#include "server.h"

/* ------------------------- Buffer I/O implementation ----------------------- */
// 缓冲区IO实现

/* Returns 1 or 0 for success/failure. */
// 将len长的buf写到一个缓冲区对象r中
static size_t rioBufferWrite(rio *r, const void *buf, size_t len) {
    r->io.buffer.ptr = sdscatlen(r->io.buffer.ptr,(char*)buf,len);  //追加操作
    r->io.buffer.pos += len;    //更新偏移量
    return 1;
}

/* Returns 1 or 0 for success/failure. */
// 讲缓冲区对象r读到buf中，读len长
static size_t rioBufferRead(rio *r, void *buf, size_t len) {
    // 缓冲区对象的长度小于len，不够读，返回0
    if (sdslen(r->io.buffer.ptr)-r->io.buffer.pos < len)
        return 0; /* not enough buffer to return len bytes. */
    // 读到buf中
    memcpy(buf,r->io.buffer.ptr+r->io.buffer.pos,len);
    // 更新偏移量
    r->io.buffer.pos += len;
    return 1;
}

/* Returns read/write position in buffer. */
// 返回缓冲区对象r当前的偏移量
static off_t rioBufferTell(rio *r) {
    return r->io.buffer.pos;
}

/* Flushes any buffer to target device if applicable. Returns 1 on success
 * and 0 on failures. */
// 清洗缓冲区
static int rioBufferFlush(rio *r) {
    UNUSED(r);  //void r，强转成void类型对象，缓冲区就相当于释放
    return 1; /* Nothing to do, our write just appends to the buffer. */
}

// 定义一个缓冲区对象并初始化方法和成员
static const rio rioBufferIO = {
    rioBufferRead,
    rioBufferWrite,
    rioBufferTell,
    rioBufferFlush,
    NULL,           /* update_checksum */
    0,              /* current checksum */
    0,              /* bytes read or written */
    0,              /* read/write chunk size */
    { { NULL, 0 } } /* union for io-specific vars */
};

// 初始化缓冲区对象r并设置缓冲区的地址
void rioInitWithBuffer(rio *r, sds s) {
    *r = rioBufferIO;
    r->io.buffer.ptr = s;
    r->io.buffer.pos = 0;
}

/* --------------------- Stdio file pointer implementation ------------------- */
// 标准文件IO实现
/* Returns 1 or 0 for success/failure. */
// 将len长的buf写入一个文件流对象
static size_t rioFileWrite(rio *r, const void *buf, size_t len) {
    size_t retval;

    // 调用底层库函数
    retval = fwrite(buf,len,1,r->io.file.fp);
    // 更新写入的长度
    r->io.file.buffered += len;

    // 如果已经达到自动的同步autosync所设置的字节数
    if (r->io.file.autosync &&
        r->io.file.buffered >= r->io.file.autosync)
    {
        // 冲洗键盘缓冲区中的数据到文件中
        fflush(r->io.file.fp);
        // 同步操作
        aof_fsync(fileno(r->io.file.fp));
        // 长度初始化为0
        r->io.file.buffered = 0;
    }
    return retval;
}

/* Returns 1 or 0 for success/failure. */
// 从文件流对象r中读出len长度的字节到buf中
static size_t rioFileRead(rio *r, void *buf, size_t len) {
    return fread(buf,len,1,r->io.file.fp);
}

/* Returns read/write position in file. */
// 返回文件流对象的偏移量
static off_t rioFileTell(rio *r) {
    return ftello(r->io.file.fp);
}

/* Flushes any buffer to target device if applicable. Returns 1 on success
 * and 0 on failures. */
// 清洗文件流
static int rioFileFlush(rio *r) {
    return (fflush(r->io.file.fp) == 0) ? 1 : 0;
}

// 初始化一个文件流对象
static const rio rioFileIO = {
    rioFileRead,
    rioFileWrite,
    rioFileTell,
    rioFileFlush,
    NULL,           /* update_checksum */
    0,              /* current checksum */
    0,              /* bytes read or written */
    0,              /* read/write chunk size */
    { { NULL, 0 } } /* union for io-specific vars */
};

// 初始化一个文件流对象且设置对应文件
void rioInitWithFile(rio *r, FILE *fp) {
    *r = rioFileIO;
    r->io.file.fp = fp;
    r->io.file.buffered = 0;
    r->io.file.autosync = 0;
}

/* ------------------- File descriptors set implementation ------------------- */
// 文件描述符合集合实现
/* Returns 1 or 0 for success/failure.
 * The function returns success as long as we are able to correctly write
 * to at least one file descriptor.
 *
 * When buf is NULL and len is 0, the function performs a flush operation
 * if there is some pending buffer, so this function is also used in order
 * to implement rioFdsetFlush(). */
// 将buf写入文件描述符集合对象
static size_t rioFdsetWrite(rio *r, const void *buf, size_t len) {
    ssize_t retval;
    int j;
    unsigned char *p = (unsigned char*) buf;
    int doflush = (buf == NULL && len == 0);    //如果buf为空且len为0，相当于flush操作

    /* To start we always append to our buffer. If it gets larger than
     * a given size, we actually write to the sockets. */
    // 将buf中的内容写到文件描述符集合对象的缓冲区中
    if (len) {
        r->io.fdset.buf = sdscatlen(r->io.fdset.buf,buf,len);
        // 设置写完的标志
        len = 0; /* Prevent entering the while below if we don't flush. */
        if (sdslen(r->io.fdset.buf) > PROTO_IOBUF_LEN) doflush = 1; //如果缓冲区太大需要冲刷到socket中
    }

    // 冲洗文件描述符集合对象，设置集合缓冲区长度和集合缓冲区地址
    if (doflush) {
        p = (unsigned char*) r->io.fdset.buf;
        len = sdslen(r->io.fdset.buf);
    }

    /* Write in little chunchs so that when there are big writes we
     * parallelize while the kernel is sending data in background to
     * the TCP socket. */
    // 一次可能无法冲洗完，需要循环多次
    while(len) {
        // 一次最多冲洗1M字节
        size_t count = len < 1024 ? len : 1024;
        int broken = 0;
        for (j = 0; j < r->io.fdset.numfds; j++) {
            // errno为0表示ok，记录不为0的文件描述符个数
            if (r->io.fdset.state[j] != 0) {
                /* Skip FDs alraedy in error. */
                broken++;
                continue;
            }

            /* Make sure to write 'count' bytes to the socket regardless
             * of short writes. */
            size_t nwritten = 0;
            // 新写的数据一次或多次写够count个字节往第一个文件描述符fd
            while(nwritten != count) {
                retval = write(r->io.fdset.fds[j],p+nwritten,count-nwritten);
                // 写失败，判断是不是写阻塞，是则设置超时
                if (retval <= 0) {
                    /* With blocking sockets, which is the sole user of this
                     * rio target, EWOULDBLOCK is returned only because of
                     * the SO_SNDTIMEO socket option, so we translate the error
                     * into one more recognizable by the user. */
                    if (retval == -1 && errno == EWOULDBLOCK) errno = ETIMEDOUT;
                    break;
                }
                nwritten += retval; //每次加上写成功的字节数
            }

            // 如果刚才写失败的情况，则将当前的文件描述符状态设置为错误的标记码
            if (nwritten != count) {
                /* Mark this FD as broken. */
                r->io.fdset.state[j] = errno;
                if (r->io.fdset.state[j] == 0) r->io.fdset.state[j] = EIO;
            }
        }
        // 所有的文件描述符都出错返回0
        if (broken == r->io.fdset.numfds) return 0; /* All the FDs in error. */
        // 更新下次要写入的地址和长度
        p += count;
        len -= count;
        r->io.fdset.pos += count;   //已写入的偏移量
    }

    if (doflush) sdsclear(r->io.fdset.buf); //释放集合缓冲区
    return 1;
}

/* Returns 1 or 0 for success/failure. */
// 文件描述符集合对象不支持读，直接返回0
static size_t rioFdsetRead(rio *r, void *buf, size_t len) {
    UNUSED(r);
    UNUSED(buf);
    UNUSED(len);
    return 0; /* Error, this target does not support reading. */
}

/* Returns read/write position in file. */
// 获取偏移量
static off_t rioFdsetTell(rio *r) {
    return r->io.fdset.pos;
}

/* Flushes any buffer to target device if applicable. Returns 1 on success
 * and 0 on failures. */
// 清洗缓冲区的值
static int rioFdsetFlush(rio *r) {
    /* Our flush is implemented by the write method, that recognizes a
     * buffer set to NULL with a count of zero as a flush request. */
    return rioFdsetWrite(r,NULL,0);
}

// 初始化一个文件描述符集合对象
static const rio rioFdsetIO = {
    rioFdsetRead,
    rioFdsetWrite,
    rioFdsetTell,
    rioFdsetFlush,
    NULL,           /* update_checksum */
    0,              /* current checksum */
    0,              /* bytes read or written */
    0,              /* read/write chunk size */
    { { NULL, 0 } } /* union for io-specific vars */
};

// 初始化一个文件描述符集合对象并设置成员变量
void rioInitWithFdset(rio *r, int *fds, int numfds) {
    int j;

    *r = rioFdsetIO;
    r->io.fdset.fds = zmalloc(sizeof(int)*numfds);
    r->io.fdset.state = zmalloc(sizeof(int)*numfds);
    memcpy(r->io.fdset.fds,fds,sizeof(int)*numfds);
    for (j = 0; j < numfds; j++) r->io.fdset.state[j] = 0;
    r->io.fdset.numfds = numfds;
    r->io.fdset.pos = 0;
    r->io.fdset.buf = sdsempty();
}

/* release the rio stream. */
// 释放文件描述符集合流对象
void rioFreeFdset(rio *r) {
    zfree(r->io.fdset.fds);
    zfree(r->io.fdset.state);
    sdsfree(r->io.fdset.buf);
}

/* ---------------------------- Generic functions ---------------------------- */
// 通用函数
/* This function can be installed both in memory and file streams when checksum
 * computation is needed. */
// 根据CRC64算法进行校验和
void rioGenericUpdateChecksum(rio *r, const void *buf, size_t len) {
    r->cksum = crc64(r->cksum,buf,len);
}

/* Set the file-based rio object to auto-fsync every 'bytes' file written.
 * By default this is set to zero that means no automatic file sync is
 * performed.
 *
 * This feature is useful in a few contexts since when we rely on OS write
 * buffers sometimes the OS buffers way too much, resulting in too many
 * disk I/O concentrated in very little time. When we fsync in an explicit
 * way instead the I/O pressure is more distributed across time. */
// 设置自动同步的字节数限制，如果bytes为0，则意味着不执行
void rioSetAutoSync(rio *r, off_t bytes) {
    serverAssert(r->read == rioFileIO.read);    //限制为文件流对象，不对其他对象设置限制
    r->io.file.autosync = bytes;
}

/* --------------------------- Higher level interface --------------------------
 *
 * The following higher level functions use lower level rio.c functions to help
 * generating the Redis protocol for the Append Only File. */
// 下面的高级函数调用上面的低级函数来辅助生成AOF文件的协议

/* Write multi bulk count in the format: "*<count>\r\n". */
// 以"*<count>\r\n"格式为写如一个int整型的count
size_t rioWriteBulkCount(rio *r, char prefix, int count) {
    char cbuf[128];
    int clen;

    // 构建一个 "*<count>\r\n"
    cbuf[0] = prefix;
    clen = 1+ll2string(cbuf+1,sizeof(cbuf)-1,count);
    cbuf[clen++] = '\r';
    cbuf[clen++] = '\n';
    // 调用rio的接口，将cbuf写如r中
    if (rioWrite(r,cbuf,clen) == 0) return 0;
    return clen;
}

/* Write binary-safe string in the format: "$<count>\r\n<payload>\r\n". */
// 以"$<count>\r\n<payload>\r\n"为格式写入一个字符串
size_t rioWriteBulkString(rio *r, const char *buf, size_t len) {
    size_t nwritten;

    // 写入"$<len>\r\n"
    if ((nwritten = rioWriteBulkCount(r,'$',len)) == 0) return 0;
    // 追加写入一个buf，也就是<payload>部分
    if (len > 0 && rioWrite(r,buf,len) == 0) return 0;
    // 追加"\r\n"
    if (rioWrite(r,"\r\n",2) == 0) return 0;
    return nwritten+len+2;  //返回长度
}

/* Write a long long value in format: "$<count>\r\n<payload>\r\n". */
// 以"$<count>\r\n<payload>\r\n"为格式写入一个longlong 值
size_t rioWriteBulkLongLong(rio *r, long long l) {
    char lbuf[32];
    unsigned int llen;

    // 将longlong转为字符串，按字符串的格式写入
    llen = ll2string(lbuf,sizeof(lbuf),l);
    return rioWriteBulkString(r,lbuf,llen);
}

/* Write a double value in the format: "$<count>\r\n<payload>\r\n" */
// 以"$<count>\r\n<payload>\r\n"为格式写入一个 double 值
size_t rioWriteBulkDouble(rio *r, double d) {
    char dbuf[128];
    unsigned int dlen;

    //以宽度为17位的方式写到dbuf中，17位的double双精度浮点数的长度最短且无损
    dlen = snprintf(dbuf,sizeof(dbuf),"%.17g",d);
    return rioWriteBulkString(r,dbuf,dlen);
}
