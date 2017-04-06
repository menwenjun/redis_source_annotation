/* SDSLib, A C dynamic strings library
 *
 * Copyright (c) 2006-2010, Salvatore Sanfilippo <antirez at gmail dot com>
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

#ifndef __SDS_H
#define __SDS_H

#define SDS_MAX_PREALLOC (1024*1024)    //预先分配内存的最大长度

#include <sys/types.h>
#include <stdarg.h>

typedef char *sds;  //sds兼容传统C风格字符串，所以起了个别名叫sds，并且可以存放sdshdr结构buf成员的地址

struct sdshdr {
    unsigned int len;   //buf中已占用空间的长度
    unsigned int free;  //buf中剩余可用空间的长度
    char buf[];         //初始化sds分配的数据空间，而且是柔性数组（Flexible array member）
};

static inline size_t sdslen(const sds s) {      //计算buf中字符串的长度
    struct sdshdr *sh = (void*)(s-(sizeof(struct sdshdr)));
    return sh->len;
}

static inline size_t sdsavail(const sds s) {    //计算buf中的未使用空间的长度
    struct sdshdr *sh = (void*)(s-(sizeof(struct sdshdr)));
    return sh->free;
}

sds sdsnewlen(const void *init, size_t initlen);    //创建一个长度为initlen的字符串,并保存init字符串中的值
sds sdsnew(const char *init);       //创建一个默认长度的字符串
sds sdsempty(void);     //建立一个只有表头，字符串为空"\0"的sds
size_t sdslen(const sds s); //计算buf中字符串的长度
sds sdsdup(const sds s);    //拷贝一份s的副本
void sdsfree(sds s);     //释放s字符串和表头
size_t sdsavail(const sds s);   //计算buf中的未使用空间的长度
sds sdsgrowzero(sds s, size_t len); //将sds扩展制定长度并赋值为0
sds sdscatlen(sds s, const void *t, size_t len);    //将字符串t追加到s表头的buf末尾，追加len个字节
sds sdscat(sds s, const char *t);       //将t字符串拼接到s的末尾
sds sdscatsds(sds s, const sds t);      //将sds追加到s末尾
sds sdscpylen(sds s, const char *t, size_t len);    //将字符串t覆盖到s表头的buf中，拷贝len个字节
sds sdscpy(sds s, const char *t);       //将字符串覆盖到s表头的buf中

sds sdscatvprintf(sds s, const char *fmt, va_list ap);  //打印函数，被 sdscatprintf 所调用
#ifdef __GNUC__
sds sdscatprintf(sds s, const char *fmt, ...)   //打印任意数量个字符串，并将这些字符串追加到给定 sds 的末尾
    __attribute__((format(printf, 2, 3)));
#else
sds sdscatprintf(sds s, const char *fmt, ...);  //打印任意数量个字符串，并将这些字符串追加到给定 sds 的末尾
#endif

sds sdscatfmt(sds s, char const *fmt, ...); //格式化打印多个字符串，并将这些字符串追加到给定 sds 的末尾
sds sdstrim(sds s, const char *cset);   //去除sds中包含有 cset字符串出现字符 的字符
void sdsrange(sds s, int start, int end);   //根据start和end区间截取字符串
void sdsupdatelen(sds s);           //更新字符串s的长度
void sdsclear(sds s);               //将字符串重置保存空间，懒惰释放
int sdscmp(const sds s1, const sds s2);     //比较两个sds的大小，相等返回0
sds *sdssplitlen(const char *s, int len, const char *sep, int seplen, int *count);  //使用长度为seplen的sep分隔符对长度为len的s进行分割，返回一个sds数组的地址，*count被设置为数组元素数量
void sdsfreesplitres(sds *tokens, int count); //释放tokens中的count个sds元素
void sdstolower(sds s);     //将sds字符串所有字符转换为小写
void sdstoupper(sds s);     //将sds字符串所有字符转换为大写
sds sdsfromlonglong(long long value);       //根据long long value创建一个SDS
sds sdscatrepr(sds s, const char *p, size_t len);   //将长度为len的字符串p以带引号""的格式追加到s末尾
sds *sdssplitargs(const char *line, int *argc); //参数拆分,主要用于 config.c 中对配置文件进行分析。
sds sdsmapchars(sds s, const char *from, const char *to, size_t setlen);    //将s中所有在 from 中的字符串，替换成 to 中的字符串
sds sdsjoin(char **argv, int argc, char *sep);  //以分隔符连接字符串子数组构成新的字符串

/* Low level functions exposed to the user API */
sds sdsMakeRoomFor(sds s, size_t addlen);   //对 sds 中 buf 的长度进行扩展
void sdsIncrLen(sds s, int incr);       //根据incr的正负，移动字符串末尾的'\0'标志
sds sdsRemoveFreeSpace(sds s);      //回收sds中的未使用空间
size_t sdsAllocSize(sds s);      //获得sds所有分配的空间

#endif
