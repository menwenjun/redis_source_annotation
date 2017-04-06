/* SDSLib, A C dynamic strings library
 *
 * Copyright (c) 2006-2012, Salvatore Sanfilippo <antirez at gmail dot com>
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <assert.h>
#include "sds.h"
#include "zmalloc.h"

/* Create a new sds string with the content specified by the 'init' pointer
 * and 'initlen'.
 * If NULL is used for 'init' the string is initialized with zero bytes.
 *
 * The string is always null-termined (all the sds strings are, always) so
 * even if you create an sds string with:
 *
 * mystring = sdsnewlen("abc",3);
 *
 * You can print the string with printf() as there is an implicit \0 at the
 * end of the string. However the string is binary safe and can contain
 * \0 characters in the middle, as the length is stored in the sds header. */
sds sdsnewlen(const void *init, size_t initlen) { //创建一个长度为initlen的字符串,并保存init字符串中的值
    struct sdshdr *sh;

    if (init) {
        sh = zmalloc(sizeof(struct sdshdr)+initlen+1);  //申请空间：表头+initlen长度+'\0'
    } else {
        sh = zcalloc(sizeof(struct sdshdr)+initlen+1);  //如果init为空，则将申请的空间初始化为0
    }
    if (sh == NULL) return NULL;
    sh->len = initlen;      //设置表头的len成员
    sh->free = 0;           //设置free，新的sds不预留任何空间
    if (initlen && init)
        memcpy(sh->buf, init, initlen); //将指定的字符串init拷贝到表头的buf中
    sh->buf[initlen] = '\0';    //以'\0'结尾
    return (char*)sh->buf;
}

/* Create an empty (zero length) sds string. Even in this case the string
 * always has an implicit null term. */
sds sdsempty(void) {        //建立一个只有表头，字符串为空"\0"的sds
    return sdsnewlen("",0);
}

/* Create a new sds string starting from a null terminated C string. */
sds sdsnew(const char *init) {  //根据字符串init，创建一个与init一样长度字符串的sds（表头+buf）
    size_t initlen = (init == NULL) ? 0 : strlen(init);
    return sdsnewlen(init, initlen);
}

/* Duplicate an sds string. */
sds sdsdup(const sds s) {       //拷贝一份s的副本
    return sdsnewlen(s, sdslen(s));
}

/* Free an sds string. No operation is performed if 's' is NULL. */
void sdsfree(sds s) {       //释放s字符串和表头
    if (s == NULL) return;
    zfree(s-sizeof(struct sdshdr));
}

/* Set the sds string length to the length as obtained with strlen(), so
 * considering as content only up to the first null term character.
 *
 * This function is useful when the sds string is hacked manually in some
 * way, like in the following example:
 *
 * s = sdsnew("foobar");
 * s[2] = '\0';
 * sdsupdatelen(s);
 * printf("%d\n", sdslen(s));
 *
 * The output will be "2", but if we comment out the call to sdsupdatelen()
 * the output will be "6" as the string was modified but the logical length
 * remains 6 bytes. */
void sdsupdatelen(sds s) {      //更新字符串s的长度
    struct sdshdr *sh = (void*) (s-(sizeof(struct sdshdr)));
    int reallen = strlen(s);
    sh->free += (sh->len-reallen);
    sh->len = reallen;
}

/* Modify an sds string in-place to make it empty (zero length).
 * However all the existing buffer is not discarded but set as free space
 * so that next append operations will not require allocations up to the
 * number of bytes previously available. */
void sdsclear(sds s) {  //将字符串重置保存空间，懒惰释放
    struct sdshdr *sh = (void*) (s-(sizeof(struct sdshdr)));
    sh->free += sh->len;    //表头free成员+已使用空间的长度len = 新的free
    sh->len = 0;            //已使用空间变为0
    sh->buf[0] = '\0';         //字符串置空
}

/* Enlarge the free space at the end of the sds string so that the caller
 * is sure that after calling this function can overwrite up to addlen
 * bytes after the end of the string, plus one more byte for nul term.
 *
 * Note: this does not change the *length* of the sds string as returned
 * by sdslen(), but only the free buffer space we have. */
sds sdsMakeRoomFor(sds s, size_t addlen) {      //对 sds 中 buf 的长度进行扩展
    struct sdshdr *sh, *newsh;
    size_t free = sdsavail(s);  //获得s的未使用空间长度
    size_t len, newlen;

    if (free >= addlen) return s;   //free的长度够用不用扩展直接返回

    //free长度不够用，需要扩展
    len = sdslen(s);    //获得s字符串的长度
    sh = (void*) (s-(sizeof(struct sdshdr)));
    newlen = (len+addlen);  //扩展后的新长度
    if (newlen < SDS_MAX_PREALLOC)  //新长度小于“最大预分配长度”，就直接将扩展的新长度乘2
        newlen *= 2;
    else
        newlen += SDS_MAX_PREALLOC; //新长度大于“最大预分配长度”，就在加上一个“最大预分配长度”
    newsh = zrealloc(sh, sizeof(struct sdshdr)+newlen+1);   //获得新的扩展空间的地址
    if (newsh == NULL) return NULL;

    newsh->free = newlen - len; //更新新空间的未使用的空间free
    return newsh->buf;
}

/* Reallocate the sds string so that it has no free space at the end. The
 * contained string remains not altered, but next concatenation operations
 * will require a reallocation.
 *
 * After the call, the passed sds string is no longer valid and all the
 * references must be substituted with the new pointer returned by the call. */
sds sdsRemoveFreeSpace(sds s) { //回收sds中的未使用空间
    struct sdshdr *sh;

    sh = (void*) (s-(sizeof(struct sdshdr)));   //获得s表头的地址
    sh = zrealloc(sh, sizeof(struct sdshdr)+sh->len+1); //只分配表头len成员大小的空间
    sh->free = 0;       //更新free成员
    return sh->buf;
}

/* Return the total size of the allocation of the specifed sds string,
 * including:
 * 1) The sds header before the pointer.
 * 2) The string.
 * 3) The free buffer at the end if any.
 * 4) The implicit null term.
 */
size_t sdsAllocSize(sds s) {    //获得sds所分配的空间
    struct sdshdr *sh = (void*) (s-(sizeof(struct sdshdr)));

    return sizeof(*sh)+sh->len+sh->free+1;  //表头空间+len+free+'\0'
}

/* Increment the sds length and decrements the left free space at the
 * end of the string according to 'incr'. Also set the null term
 * in the new end of the string.
 *
 * This function is used in order to fix the string length after the
 * user calls sdsMakeRoomFor(), writes something after the end of
 * the current string, and finally needs to set the new length.
 *
 * Note: it is possible to use a negative increment in order to
 * right-trim the string.
 *
 * Usage example:
 *
 * Using sdsIncrLen() and sdsMakeRoomFor() it is possible to mount the
 * following schema, to cat bytes coming from the kernel to the end of an
 * sds string without copying into an intermediate buffer:
 *
 * oldlen = sdslen(s);
 * s = sdsMakeRoomFor(s, BUFFER_SIZE);
 * nread = read(fd, s+oldlen, BUFFER_SIZE);
 * ... check for nread <= 0 and handle it ...
 * sdsIncrLen(s, nread);
 */
void sdsIncrLen(sds s, int incr) {  //根据incr的正负，移动字符串末尾的'\0'标志
    struct sdshdr *sh = (void*) (s-(sizeof(struct sdshdr)));

    if (incr >= 0)
        assert(sh->free >= (unsigned int)incr); //保证free的空间大于等于要扩展的空间，否则直接终止程序
    else
        assert(sh->len >= (unsigned int)(-incr));   //保证len的空间长度大于incr绝对值的长度
    sh->len += incr;    //更新len、free和'\0'的位置
    sh->free -= incr;
    s[sh->len] = '\0';
}

/* Grow the sds to have the specified length. Bytes that were not part of
 * the original length of the sds will be set to zero.
 *
 * if the specified length is smaller than the current length, no operation
 * is performed. */
sds sdsgrowzero(sds s, size_t len) {    //将sds扩展制定长度并赋值为0
    struct sdshdr *sh = (void*)(s-(sizeof(struct sdshdr)));
    size_t totlen, curlen = sh->len;

    if (len <= curlen) return s;
    s = sdsMakeRoomFor(s,len-curlen);   //扩展字符串sds
    if (s == NULL) return NULL;

    /* Make sure added region doesn't contain garbage */
    sh = (void*)(s-(sizeof(struct sdshdr)));    //获得表头地址
    memset(s+curlen,0,(len-curlen+1)); /* also set trailing \0 byte */  //用0来填充扩展的空间

    //更新表头
    totlen = sh->len+sh->free;  //总长度
    sh->len = len;  //使用空间的长度
    sh->free = totlen-sh->len;  //总长度-使用空间的长度 = 为使用空间的长度
    return s;
}

/* Append the specified binary-safe string pointed by 't' of 'len' bytes to the
 * end of the specified sds string 's'.
 *
 * After the call, the passed sds string is no longer valid and all the
 * references must be substituted with the new pointer returned by the call. */
sds sdscatlen(sds s, const void *t, size_t len) {   //将字符串t追加到s表头的buf末尾，追加len个字节
    struct sdshdr *sh;
    size_t curlen = sdslen(s);  //原有的长度

    s = sdsMakeRoomFor(s,len);  //扩展空间
    if (s == NULL) return NULL;
    sh = (void*) (s-(sizeof(struct sdshdr)));
    memcpy(s+curlen, t, len);   //字符串拼接

    //更新属性
    sh->len = curlen+len;
    sh->free = sh->free-len;
    s[curlen+len] = '\0';
    return s;
}

/* Append the specified null termianted C string to the sds string 's'.
 *
 * After the call, the passed sds string is no longer valid and all the
 * references must be substituted with the new pointer returned by the call. */
sds sdscat(sds s, const char *t) {  //将t字符串拼接到s的末尾
    return sdscatlen(s, t, strlen(t));
}

/* Append the specified sds 't' to the existing sds 's'.
 *
 * After the call, the modified sds string is no longer valid and all the
 * references must be substituted with the new pointer returned by the call. */
sds sdscatsds(sds s, const sds t) {     //将sds追加到s末尾
    return sdscatlen(s, t, sdslen(t));
}

/* Destructively modify the sds string 's' to hold the specified binary
 * safe string pointed by 't' of length 'len' bytes. */
sds sdscpylen(sds s, const char *t, size_t len) {   //将字符串t覆盖到s表头的buf中，拷贝len个字节
    struct sdshdr *sh = (void*) (s-(sizeof(struct sdshdr)));
    size_t totlen = sh->free+sh->len;   //获得总长度

    if (totlen < len) {         //总长度小于len
        s = sdsMakeRoomFor(s,len-sh->len);  //扩展l空间
        if (s == NULL) return NULL;
        sh = (void*) (s-(sizeof(struct sdshdr)));
        totlen = sh->free+sh->len;  //更新总长度
    }
    memcpy(s, t, len);  //拷贝字符串t覆盖s原有的字符串

    //更新表头
    s[len] = '\0';
    sh->len = len;
    sh->free = totlen-len;
    return s;
}

/* Like sdscpylen() but 't' must be a null-termined string so that the length
 * of the string is obtained with strlen(). */
sds sdscpy(sds s, const char *t) {      //将字符串覆盖到s表头的buf中
    return sdscpylen(s, t, strlen(t));
}

/* Helper for sdscatlonglong() doing the actual number -> string
 * conversion. 's' must point to a string with room for at least
 * SDS_LLSTR_SIZE bytes.
 *
 * The function returns the length of the null-terminated string
 * representation stored at 's'. */
#define SDS_LLSTR_SIZE 21
int sdsll2str(char *s, long long value) {   //将一个long long类型的value转化为字符串，返回字符串长度
    char *p, aux;
    unsigned long long v;
    size_t l;

    /* Generate the string representation, this method produces
     * an reversed string. */
    v = (value < 0) ? -value : value;
    p = s;
    do {
        *p++ = '0'+(v%10);
        v /= 10;
    } while(v);
    if (value < 0) *p++ = '-';

    /* Compute length and add null term. */
    l = p-s;
    *p = '\0';

    /* Reverse the string. */
    p--;
    while(s < p) {
        aux = *s;
        *s = *p;
        *p = aux;
        s++;
        p--;
    }
    return l;
}

/* Identical sdsll2str(), but for unsigned long long type. */
int sdsull2str(char *s, unsigned long long v) {//将一个unsigned long long类型的value转化为字符串，返回字符串长度
    char *p, aux;
    size_t l;

    /* Generate the string representation, this method produces
     * an reversed string. */
    p = s;
    do {
        *p++ = '0'+(v%10);
        v /= 10;
    } while(v);

    /* Compute length and add null term. */
    l = p-s;
    *p = '\0';

    /* Reverse the string. */
    p--;
    while(s < p) {
        aux = *s;
        *s = *p;
        *p = aux;
        s++;
        p--;
    }
    return l;
}

/* Create an sds string from a long long value. It is much faster than:
 *
 * sdscatprintf(sdsempty(),"%lld\n", value);
 */
sds sdsfromlonglong(long long value) {  //根据long long value创建一个SDS
    char buf[SDS_LLSTR_SIZE];
    int len = sdsll2str(buf,value); //返回装换成字符串的长度

    return sdsnewlen(buf,len);      //创建SDS
}

/* Like sdscatprintf() but gets va_list instead of being variadic. */
sds sdscatvprintf(sds s, const char *fmt, va_list ap) {     //打印函数，被 sdscatprintf 所调用
    va_list cpy;
    char staticbuf[1024], *buf = staticbuf, *t;
    size_t buflen = strlen(fmt)*2;

    /* We try to start using a static buffer for speed.
     * If not possible we revert to heap allocation. */
    if (buflen > sizeof(staticbuf)) {
        buf = zmalloc(buflen);
        if (buf == NULL) return NULL;
    } else {
        buflen = sizeof(staticbuf);
    }

    /* Try with buffers two times bigger every time we fail to
     * fit the string in the current buffer size. */
    while(1) {
        buf[buflen-2] = '\0';
        va_copy(cpy,ap);
        vsnprintf(buf, buflen, fmt, cpy);
        va_end(cpy);
        if (buf[buflen-2] != '\0') {
            if (buf != staticbuf) zfree(buf);
            buflen *= 2;
            buf = zmalloc(buflen);
            if (buf == NULL) return NULL;
            continue;
        }
        break;
    }

    /* Finally concat the obtained string to the SDS string and return it. */
    t = sdscat(s, buf);
    if (buf != staticbuf) zfree(buf);
    return t;
}

/* Append to the sds string 's' a string obtained using printf-alike format
 * specifier.
 *
 * After the call, the modified sds string is no longer valid and all the
 * references must be substituted with the new pointer returned by the call.
 *
 * Example:
 *
 * s = sdsnew("Sum is: ");
 * s = sdscatprintf(s,"%d+%d = %d",a,b,a+b).
 *
 * Often you need to create a string from scratch with the printf-alike
 * format. When this is the need, just use sdsempty() as the target string:
 *
 * s = sdscatprintf(sdsempty(), "... your format ...", args);
 */
sds sdscatprintf(sds s, const char *fmt, ...) {     //打印任意数量个字符串，并将这些字符串追加到给定 sds 的末尾
    va_list ap;
    char *t;
    va_start(ap, fmt);
    t = sdscatvprintf(s,fmt,ap);
    va_end(ap);
    return t;
}

/* This function is similar to sdscatprintf, but much faster as it does
 * not rely on sprintf() family functions implemented by the libc that
 * are often very slow. Moreover directly handling the sds string as
 * new data is concatenated provides a performance improvement.
 *
 * However this function only handles an incompatible subset of printf-alike
 * format specifiers:
 *
 * %s - C String
 * %S - SDS string
 * %i - signed int
 * %I - 64 bit signed integer (long long, int64_t)
 * %u - unsigned int
 * %U - 64 bit unsigned integer (unsigned long long, uint64_t)
 * %% - Verbatim "%" character.
 */
sds sdscatfmt(sds s, char const *fmt, ...) {    //格式化打印多个字符串，并将这些字符串追加到给定 sds 的末尾
    struct sdshdr *sh = (void*) (s-(sizeof(struct sdshdr)));
    size_t initlen = sdslen(s);
    const char *f = fmt;
    int i;
    va_list ap;

    va_start(ap,fmt);
    f = fmt;    /* Next format specifier byte to process. */
    i = initlen; /* Position of the next byte to write to dest str. */
    while(*f) {
        char next, *str;
        unsigned int l;
        long long num;
        unsigned long long unum;

        /* Make sure there is always space for at least 1 char. */
        if (sh->free == 0) {
            s = sdsMakeRoomFor(s,1);
            sh = (void*) (s-(sizeof(struct sdshdr)));
        }

        switch(*f) {
        case '%':
            next = *(f+1);
            f++;
            switch(next) {
            case 's':
            case 'S':
                str = va_arg(ap,char*);
                l = (next == 's') ? strlen(str) : sdslen(str);
                if (sh->free < l) {
                    s = sdsMakeRoomFor(s,l);
                    sh = (void*) (s-(sizeof(struct sdshdr)));
                }
                memcpy(s+i,str,l);
                sh->len += l;
                sh->free -= l;
                i += l;
                break;
            case 'i':
            case 'I':
                if (next == 'i')
                    num = va_arg(ap,int);
                else
                    num = va_arg(ap,long long);
                {
                    char buf[SDS_LLSTR_SIZE];
                    l = sdsll2str(buf,num);
                    if (sh->free < l) {
                        s = sdsMakeRoomFor(s,l);
                        sh = (void*) (s-(sizeof(struct sdshdr)));
                    }
                    memcpy(s+i,buf,l);
                    sh->len += l;
                    sh->free -= l;
                    i += l;
                }
                break;
            case 'u':
            case 'U':
                if (next == 'u')
                    unum = va_arg(ap,unsigned int);
                else
                    unum = va_arg(ap,unsigned long long);
                {
                    char buf[SDS_LLSTR_SIZE];
                    l = sdsull2str(buf,unum);
                    if (sh->free < l) {
                        s = sdsMakeRoomFor(s,l);
                        sh = (void*) (s-(sizeof(struct sdshdr)));
                    }
                    memcpy(s+i,buf,l);
                    sh->len += l;
                    sh->free -= l;
                    i += l;
                }
                break;
            default: /* Handle %% and generally %<unknown>. */
                s[i++] = next;
                sh->len += 1;
                sh->free -= 1;
                break;
            }
            break;
        default:
            s[i++] = *f;
            sh->len += 1;
            sh->free -= 1;
            break;
        }
        f++;
    }
    va_end(ap);

    /* Add null-term */
    s[i] = '\0';
    return s;
}

/* Remove the part of the string from left and from right composed just of
 * contiguous characters found in 'cset', that is a null terminted C string.
 *
 * After the call, the modified sds string is no longer valid and all the
 * references must be substituted with the new pointer returned by the call.
 *
 * Example:
 *
 * s = sdsnew("AA...AA.a.aa.aHelloWorld     :::");
 * s = sdstrim(s,"Aa. :");
 * printf("%s\n", s);
 *
 * Output will be just "Hello World".
 */
sds sdstrim(sds s, const char *cset) {  //去除sds中包含有 cset字符串出现字符 的字符
    struct sdshdr *sh = (void*) (s-(sizeof(struct sdshdr)));
    char *start, *end, *sp, *ep;
    size_t len;

    //设置和备份指针位置
    sp = start = s;
    ep = end = s+sdslen(s)-1;

    //strchr()函数功能：查找cset中首次出现*sp字符的位置，成功返回第一次出现的位置
    while(sp <= end && strchr(cset, *sp)) sp++;         //从左开始修剪，sp为目标串的左边界
    while(ep > start && strchr(cset, *ep)) ep--;        //从右开始修剪，ep为目标串的右边界

    len = (sp > ep) ? 0 : ((ep-sp)+1);  //目标串的长度
    if (sh->buf != sp) memmove(sh->buf, sp, len);   //将字符串的位置前移到buf开头

    //更新表头
    sh->buf[len] = '\0';
    sh->free = sh->free+(sh->len-len);
    sh->len = len;
    return s;
}

/* Turn the string into a smaller (or equal) string containing only the
 * substring specified by the 'start' and 'end' indexes.
 *
 * start and end can be negative, where -1 means the last character of the
 * string, -2 the penultimate character, and so forth.
 *
 * The interval is inclusive, so the start and end characters will be part
 * of the resulting string.
 *
 * The string is modified in-place.
 *
 * Example:
 *
 * s = sdsnew("Hello World");
 * sdsrange(s,1,-1); => "ello World"
 */
void sdsrange(sds s, int start, int end) {  //根据start和end区间截取字符串
    struct sdshdr *sh = (void*) (s-(sizeof(struct sdshdr)));
    size_t newlen, len = sdslen(s);     //获得源串的长度

    //1，start或end为负数的情况，转化为非负数
    if (len == 0) return;
    if (start < 0) {    //如果start小于0，则从字符串尾部往前开始算起始位置
        start = len+start;
        if (start < 0) start = 0;
    }
    if (end < 0) {      //如果start小于0，则从字符串尾部往前开始算结束位置
        end = len+end;
        if (end < 0) end = 0;
    }

    //2，start和end为非负数的情况
    newlen = (start > end) ? 0 : (end-start)+1;     //截取后字符串的长度
    if (newlen != 0) {
        if (start >= (signed)len) {         //起始位置大于字符串的长度，被截取的字符串长度为0
            newlen = 0;
        } else if (end >= (signed)len) {    //结束位置大于字符串的长度，只截取到字符串的末尾
            end = len-1;
            newlen = (start > end) ? 0 : (end-start)+1;     //更新截取后字符串的长度
        }
    } else {
        start = 0;
    }

    //通过以上两种情况计算出start和end位置，用memmove函数截取
    if (start && newlen) memmove(sh->buf, sh->buf+start, newlen);   //截取后，移动到buf的前面

    //更新表头
    sh->buf[newlen] = 0;
    sh->free = sh->free+(sh->len-newlen);
    sh->len = newlen;
}

/* Apply tolower() to every character of the sds string 's'. */
void sdstolower(sds s) {        //将sds字符串所有字符转换为小写
    int len = sdslen(s), j;

    for (j = 0; j < len; j++) s[j] = tolower(s[j]); //tolower()函数将大写字符转换成小写
}

/* Apply toupper() to every character of the sds string 's'. */
void sdstoupper(sds s) {        //将sds字符串所有字符转换为大写
    int len = sdslen(s), j;

    for (j = 0; j < len; j++) s[j] = toupper(s[j]); //toupper()函数将小字符转换成大写
}

/* Compare two sds strings s1 and s2 with memcmp().
 *
 * Return value:
 *
 *     positive if s1 > s2.
 *     negative if s1 < s2.
 *     0 if s1 and s2 are exactly the same binary string.
 *
 * If two strings share exactly the same prefix, but one of the two has
 * additional characters, the longer string is considered to be greater than
 * the smaller one. */
int sdscmp(const sds s1, const sds s2) {    //比较两个sds的大小，相等返回0
    size_t l1, l2, minlen;
    int cmp;

    l1 = sdslen(s1);
    l2 = sdslen(s2);
    minlen = (l1 < l2) ? l1 : l2;   //
    cmp = memcmp(s1,s2,minlen);     //memcmp()函数比较s1和s2内存地址的前len个字节，返回s1减s2的差值
    if (cmp == 0) return l1-l2;     //s1长度大于s2返回正差值，否则返回负差值
    return cmp;
}

/* Split 's' with separator in 'sep'. An array
 * of sds strings is returned. *count will be set
 * by reference to the number of tokens returned.
 *
 * On out of memory, zero length string, zero length
 * separator, NULL is returned.
 *
 * Note that 'sep' is able to split a string using
 * a multi-character separator. For example
 * sdssplit("foo_-_bar","_-_"); will return two
 * elements "foo" and "bar".
 *
 * This version of the function is binary-safe but
 * requires length arguments. sdssplit() is just the
 * same function but for zero-terminated strings.
 */
sds *sdssplitlen(const char *s, int len, const char *sep, int seplen, int *count) { //使用长度为seplen的sep分隔符对长度为len的s进行分割，返回一个sds数组的地址，*count被设置为数组元素数量
    int elements = 0, slots = 5, start = 0, j;
    sds *tokens;

    if (seplen < 1 || len < 0) return NULL;

    tokens = zmalloc(sizeof(sds)*slots);    //默认sds长度为5个
    if (tokens == NULL) return NULL;

    if (len == 0) {     //len为0，count设为0，不分割
        *count = 0;
        return tokens;
    }
    for (j = 0; j < (len-(seplen-1)); j++) {    //sep和s对比(len-(seplen-1))次，
        /* make sure there is room for the next element and the final one */
        //如果当前字符串数组数量少于当前已存在数组+2个的时候，动态添加
        if (slots < elements+2) {
            sds *newtokens;

            slots *= 2;
            newtokens = zrealloc(tokens,sizeof(sds)*slots);
            //扩展内存失败，goto语句释放内存
            if (newtokens == NULL) goto cleanup;
            tokens = newtokens;
        }
        /* search the separator */
        //分成单字符比较或字符串比较匹配
        if ((seplen == 1 && *(s+j) == sep[0]) || (memcmp(s+j,sep,seplen) == 0)) {
            tokens[elements] = sdsnewlen(s+start,j-start);  //将s分割的一段生成为SDS然后放到预先的tokens数组里
            if (tokens[elements] == NULL) goto cleanup; //创建SDS失败要释放tokens数组
            elements++;
            start = j+seplen;
            j = j+seplen-1; /* skip the separator */
        }
    }
    /* Add the final element. We are sure there is room in the tokens array. */
    //最后一个字符串添加
    tokens[elements] = sdsnewlen(s+start,len-start);
    if (tokens[elements] == NULL) goto cleanup; //创建SDS失败要释放tokens数组
    elements++;
    *count = elements;
    return tokens;

cleanup:    //释放内存
    {
        int i;
        for (i = 0; i < elements; i++) sdsfree(tokens[i]);
        zfree(tokens);
        *count = 0;
        return NULL;
    }
}

/* Free the result returned by sdssplitlen(), or do nothing if 'tokens' is NULL. */
void sdsfreesplitres(sds *tokens, int count) {      //释放tokens中的count个sds元素
    if (!tokens) return;
    while(count--)
        sdsfree(tokens[count]);
    zfree(tokens);
}

/* Append to the sds string "s" an escaped string representation where
 * all the non-printable characters (tested with isprint()) are turned into
 * escapes in the form "\n\r\a...." or "\x<hex-number>".
 *
 * After the call, the modified sds string is no longer valid and all the
 * references must be substituted with the new pointer returned by the call. */
sds sdscatrepr(sds s, const char *p, size_t len) {  //将长度为len的字符串p以带引号""的格式追加到s末尾
    s = sdscatlen(s,"\"",1);    //追加左引号，\是转义字符
    while(len--) {
        switch(*p) {
        case '\\':
        case '"':
            s = sdscatprintf(s,"\\%c",*p);
            break;
        case '\n': s = sdscatlen(s,"\\n",2); break; // "\\n"是两个字符"\n"
        case '\r': s = sdscatlen(s,"\\r",2); break;
        case '\t': s = sdscatlen(s,"\\t",2); break;
        case '\a': s = sdscatlen(s,"\\a",2); break;
        case '\b': s = sdscatlen(s,"\\b",2); break;
        default:
            if (isprint(*p))
                s = sdscatprintf(s,"%c",*p);
            else
                s = sdscatprintf(s,"\\x%02x",(unsigned char)*p);
            break;
        }
        p++;
    }
    return sdscatlen(s,"\"",1); //追加右引号
}

/* Helper function for sdssplitargs() that returns non zero if 'c'
 * is a valid hex digit. */
int is_hex_digit(char c) {      //如果c是十六进制符号中的一个返回正数
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
           (c >= 'A' && c <= 'F');
}

/* Helper function for sdssplitargs() that converts a hex digit into an
 * integer from 0 to 15 */
int hex_digit_to_int(char c) {      //16进制字符转换为10进制整数
    switch(c) {
    case '0': return 0;
    case '1': return 1;
    case '2': return 2;
    case '3': return 3;
    case '4': return 4;
    case '5': return 5;
    case '6': return 6;
    case '7': return 7;
    case '8': return 8;
    case '9': return 9;
    case 'a': case 'A': return 10;
    case 'b': case 'B': return 11;
    case 'c': case 'C': return 12;
    case 'd': case 'D': return 13;
    case 'e': case 'E': return 14;
    case 'f': case 'F': return 15;
    default: return 0;
    }
}

/* Split a line into arguments, where every argument can be in the
 * following programming-language REPL-alike form:
 *
 * foo bar "newline are supported\n" and "\xff\x00otherstuff"
 *
 * The number of arguments is stored into *argc, and an array
 * of sds is returned.
 *
 * The caller should free the resulting array of sds strings with
 * sdsfreesplitres().
 *
 * Note that sdscatrepr() is able to convert back a string into
 * a quoted string in the same format sdssplitargs() is able to parse.
 *
 * The function returns the allocated tokens on success, even when the
 * input string is empty, or NULL if the input contains unbalanced
 * quotes or closed quotes followed by non space characters
 * as in: "foo"bar or "foo'
 * 这个函数主要用于 config.c 中对配置文件进行分析。
 * 例子：
 *  sds *arr = sdssplitargs("timeout 10086\r\nport 123321\r\n");
 * 会得出
 *  arr[0] = "timeout"
 *  arr[1] = "10086"
 *  arr[2] = "port"
 *  arr[3] = "123321"
 */
sds *sdssplitargs(const char *line, int *argc) {    //参数拆分,主要用于 config.c 中对配置文件进行分析。
    const char *p = line;
    char *current = NULL;
    char **vector = NULL;

    *argc = 0;
    while(1) {
        /* skip blanks */
        while(*p && isspace(*p)) p++;
        if (*p) {
            /* get a token */
            int inq=0;  /* set to 1 if we are in "quotes" */
            int insq=0; /* set to 1 if we are in 'single quotes' */
            int done=0;

            if (current == NULL) current = sdsempty();
            while(!done) {
                if (inq) {
                    if (*p == '\\' && *(p+1) == 'x' &&
                                             is_hex_digit(*(p+2)) &&
                                             is_hex_digit(*(p+3)))
                    {
                        unsigned char byte;

                        byte = (hex_digit_to_int(*(p+2))*16)+
                                hex_digit_to_int(*(p+3));
                        current = sdscatlen(current,(char*)&byte,1);
                        p += 3;
                    } else if (*p == '\\' && *(p+1)) {
                        char c;

                        p++;
                        switch(*p) {
                        case 'n': c = '\n'; break;
                        case 'r': c = '\r'; break;
                        case 't': c = '\t'; break;
                        case 'b': c = '\b'; break;
                        case 'a': c = '\a'; break;
                        default: c = *p; break;
                        }
                        current = sdscatlen(current,&c,1);
                    } else if (*p == '"') {
                        /* closing quote must be followed by a space or
                         * nothing at all. */
                        if (*(p+1) && !isspace(*(p+1))) goto err;
                        done=1;
                    } else if (!*p) {
                        /* unterminated quotes */
                        goto err;
                    } else {
                        current = sdscatlen(current,p,1);
                    }
                } else if (insq) {
                    if (*p == '\\' && *(p+1) == '\'') {
                        p++;
                        current = sdscatlen(current,"'",1);
                    } else if (*p == '\'') {
                        /* closing quote must be followed by a space or
                         * nothing at all. */
                        if (*(p+1) && !isspace(*(p+1))) goto err;
                        done=1;
                    } else if (!*p) {
                        /* unterminated quotes */
                        goto err;
                    } else {
                        current = sdscatlen(current,p,1);
                    }
                } else {
                    switch(*p) {
                    case ' ':
                    case '\n':
                    case '\r':
                    case '\t':
                    case '\0':
                        done=1;
                        break;
                    case '"':
                        inq=1;
                        break;
                    case '\'':
                        insq=1;
                        break;
                    default:
                        current = sdscatlen(current,p,1);
                        break;
                    }
                }
                if (*p) p++;
            }
            /* add the token to the vector */
            vector = zrealloc(vector,((*argc)+1)*sizeof(char*));
            vector[*argc] = current;
            (*argc)++;
            current = NULL;
        } else {
            /* Even on empty input string return something not NULL. */
            if (vector == NULL) vector = zmalloc(sizeof(void*));
            return vector;
        }
    }

err:
    while((*argc)--)
        sdsfree(vector[*argc]);
    zfree(vector);
    if (current) sdsfree(current);
    *argc = 0;
    return NULL;
}

/* Modify the string substituting all the occurrences of the set of
 * characters specified in the 'from' string to the corresponding character
 * in the 'to' array.
 *
 * For instance: sdsmapchars(mystring, "ho", "01", 2)
 * will have the effect of turning the string "hello" into "0ell1".
 *
 * The function returns the sds string pointer, that is always the same
 * as the input pointer since no resize is needed. */
sds sdsmapchars(sds s, const char *from, const char *to, size_t setlen) {   //将s中所有在 from 中的字符串，替换成 to 中的字符串
    size_t j, i, l = sdslen(s);

    for (j = 0; j < l; j++) {   //遍历s
        for (i = 0; i < setlen; i++) {  //遍历映射
            if (s[j] == from[i]) {  //替换字符串
                s[j] = to[i];
                break;
            }
        }
    }
    return s;
}

/* Join an array of C strings using the specified separator (also a C string).
 * Returns the result as an sds string. */
sds sdsjoin(char **argv, int argc, char *sep) { //以分隔符连接字符串子数组构成新的字符串
    sds join = sdsempty();  //创建一个空sds
    int j;

    for (j = 0; j < argc; j++) {
        join = sdscat(join, argv[j]);
        if (j != argc-1) join = sdscat(join,sep);   //以*sep链接
    }
    return join;
}

#ifdef SDS_TEST_MAIN
#include <stdio.h>
#include "testhelp.h"
#include "limits.h"

int main(void) {
    {
        struct sdshdr *sh;
        sds x = sdsnew("foo"), y;

        test_cond("Create a string and obtain the length",
            sdslen(x) == 3 && memcmp(x,"foo\0",4) == 0)

        sdsfree(x);
        x = sdsnewlen("foo",2);
        test_cond("Create a string with specified length",
            sdslen(x) == 2 && memcmp(x,"fo\0",3) == 0)

        x = sdscat(x,"bar");
        test_cond("Strings concatenation",
            sdslen(x) == 5 && memcmp(x,"fobar\0",6) == 0);

        x = sdscpy(x,"a");
        test_cond("sdscpy() against an originally longer string",
            sdslen(x) == 1 && memcmp(x,"a\0",2) == 0)

        x = sdscpy(x,"xyzxxxxxxxxxxyyyyyyyyyykkkkkkkkkk");
        test_cond("sdscpy() against an originally shorter string",
            sdslen(x) == 33 &&
            memcmp(x,"xyzxxxxxxxxxxyyyyyyyyyykkkkkkkkkk\0",33) == 0)

        sdsfree(x);
        x = sdscatprintf(sdsempty(),"%d",123);
        test_cond("sdscatprintf() seems working in the base case",
            sdslen(x) == 3 && memcmp(x,"123\0",4) == 0)

        sdsfree(x);
        x = sdsnew("--");
        x = sdscatfmt(x, "Hello %s World %I,%I--", "Hi!", LLONG_MIN,LLONG_MAX);
        test_cond("sdscatfmt() seems working in the base case",
            sdslen(x) == 60 &&
            memcmp(x,"--Hello Hi! World -9223372036854775808,"
                     "9223372036854775807--",60) == 0)

        sdsfree(x);
        x = sdsnew("--");
        x = sdscatfmt(x, "%u,%U--", UINT_MAX, ULLONG_MAX);
        test_cond("sdscatfmt() seems working with unsigned numbers",
            sdslen(x) == 35 &&
            memcmp(x,"--4294967295,18446744073709551615--",35) == 0)

        sdsfree(x);
        x = sdsnew("xxciaoyyy");
        sdstrim(x,"xy");
        test_cond("sdstrim() correctly trims characters",
            sdslen(x) == 4 && memcmp(x,"ciao\0",5) == 0)

        y = sdsdup(x);
        sdsrange(y,1,1);
        test_cond("sdsrange(...,1,1)",
            sdslen(y) == 1 && memcmp(y,"i\0",2) == 0)

        sdsfree(y);
        y = sdsdup(x);
        sdsrange(y,1,-1);
        test_cond("sdsrange(...,1,-1)",
            sdslen(y) == 3 && memcmp(y,"iao\0",4) == 0)

        sdsfree(y);
        y = sdsdup(x);
        sdsrange(y,-2,-1);
        test_cond("sdsrange(...,-2,-1)",
            sdslen(y) == 2 && memcmp(y,"ao\0",3) == 0)

        sdsfree(y);
        y = sdsdup(x);
        sdsrange(y,2,1);
        test_cond("sdsrange(...,2,1)",
            sdslen(y) == 0 && memcmp(y,"\0",1) == 0)

        sdsfree(y);
        y = sdsdup(x);
        sdsrange(y,1,100);
        test_cond("sdsrange(...,1,100)",
            sdslen(y) == 3 && memcmp(y,"iao\0",4) == 0)

        sdsfree(y);
        y = sdsdup(x);
        sdsrange(y,100,100);
        test_cond("sdsrange(...,100,100)",
            sdslen(y) == 0 && memcmp(y,"\0",1) == 0)

        sdsfree(y);
        sdsfree(x);
        x = sdsnew("foo");
        y = sdsnew("foa");
        test_cond("sdscmp(foo,foa)", sdscmp(x,y) > 0)

        sdsfree(y);
        sdsfree(x);
        x = sdsnew("bar");
        y = sdsnew("bar");
        test_cond("sdscmp(bar,bar)", sdscmp(x,y) == 0)

        sdsfree(y);
        sdsfree(x);
        x = sdsnew("aar");
        y = sdsnew("bar");
        test_cond("sdscmp(bar,bar)", sdscmp(x,y) < 0)

        sdsfree(y);
        sdsfree(x);
        x = sdsnewlen("\a\n\0foo\r",7);
        y = sdscatrepr(sdsempty(),x,sdslen(x));
        test_cond("sdscatrepr(...data...)",
            memcmp(y,"\"\\a\\n\\x00foo\\r\"",15) == 0)

        {
            int oldfree;

            sdsfree(x);
            sdsfree(y);
            x = sdsnew("0");
            sh = (void*) (x-(sizeof(struct sdshdr)));
            test_cond("sdsnew() free/len buffers", sh->len == 1 && sh->free == 0);
            x = sdsMakeRoomFor(x,1);
            sh = (void*) (x-(sizeof(struct sdshdr)));
            test_cond("sdsMakeRoomFor()", sh->len == 1 && sh->free > 0);
            oldfree = sh->free;
            x[1] = '1';
            sdsIncrLen(x,1);
            test_cond("sdsIncrLen() -- content", x[0] == '0' && x[1] == '1');
            test_cond("sdsIncrLen() -- len", sh->len == 2);
            test_cond("sdsIncrLen() -- free", sh->free == oldfree-1);

            sdsfree(x);
        }
    }
    test_report()
    return 0;
}
#endif
