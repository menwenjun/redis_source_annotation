/* The ziplist is a specially encoded dually linked list that is designed
 * to be very memory efficient. It stores both strings and integer values,
 * where integers are encoded as actual integers instead of a series of
 * characters. It allows push and pop operations on either side of the list
 * in O(1) time. However, because every operation requires a reallocation of
 * the memory used by the ziplist, the actual complexity is related to the
 * amount of memory used by the ziplist.
 *
 * ----------------------------------------------------------------------------
 *
 * ZIPLIST OVERALL LAYOUT:
 * The general layout of the ziplist is as follows:
 * <zlbytes><zltail><zllen><entry><entry><zlend>
 *
 * <zlbytes> is an unsigned integer to hold the number of bytes that the
 * ziplist occupies. This value needs to be stored to be able to resize the
 * entire structure without the need to traverse it first.
 *
 * <zltail> is the offset to the last entry in the list. This allows a pop
 * operation on the far side of the list without the need for full traversal.
 *
 * <zllen> is the number of entries.When this value is larger than 2**16-2,
 * we need to traverse the entire list to know how many items it holds.
 *
 * <zlend> is a single byte special value, equal to 255, which indicates the
 * end of the list.
 *
 * ZIPLIST ENTRIES:
 * Every entry in the ziplist is prefixed by a header that contains two pieces
 * of information. First, the length of the previous entry is stored to be
 * able to traverse the list from back to front. Second, the encoding with an
 * optional string length of the entry itself is stored.
 *
 * The length of the previous entry is encoded in the following way:
 * If this length is smaller than 254 bytes, it will only consume a single
 * byte that takes the length as value. When the length is greater than or
 * equal to 254, it will consume 5 bytes. The first byte is set to 254 to
 * indicate a larger value is following. The remaining 4 bytes take the
 * length of the previous entry as value.
 *
 * The other header field of the entry itself depends on the contents of the
 * entry. When the entry is a string, the first 2 bits of this header will hold
 * the type of encoding used to store the length of the string, followed by the
 * actual length of the string. When the entry is an integer the first 2 bits
 * are both set to 1. The following 2 bits are used to specify what kind of
 * integer will be stored after this header. An overview of the different
 * types and encodings is as follows:
 *
 * |00pppppp| - 1 byte
 *      String value with length less than or equal to 63 bytes (6 bits).
 * |01pppppp|qqqqqqqq| - 2 bytes
 *      String value with length less than or equal to 16383 bytes (14 bits).
 * |10______|qqqqqqqq|rrrrrrrr|ssssssss|tttttttt| - 5 bytes
 *      String value with length greater than or equal to 16384 bytes.
 * |11000000| - 1 byte
 *      Integer encoded as int16_t (2 bytes).
 * |11010000| - 1 byte
 *      Integer encoded as int32_t (4 bytes).
 * |11100000| - 1 byte
 *      Integer encoded as int64_t (8 bytes).
 * |11110000| - 1 byte
 *      Integer encoded as 24 bit signed (3 bytes).
 * |11111110| - 1 byte
 *      Integer encoded as 8 bit signed (1 byte).
 * |1111xxxx| - (with xxxx between 0000 and 1101) immediate 4 bit integer.
 *      Unsigned integer from 0 to 12. The encoded value is actually from
 *      1 to 13 because 0000 and 1111 can not be used, so 1 should be
 *      subtracted from the encoded 4 bit value to obtain the right value.
 * |11111111| - End of ziplist.
 *
 * All the integers are represented in little endian byte order.
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>
#include "zmalloc.h"
#include "util.h"
#include "ziplist.h"
#include "endianconv.h"
#include "redisassert.h"

#define ZIP_END 255                     //压缩列表的末尾end成员的值
#define ZIP_BIGLEN 254                  //压缩列表的最多节点数

/* Different encoding/length possibilities */
#define ZIP_STR_MASK 0xc0               //1100 0000
#define ZIP_STR_06B (0 << 6)            //0000 0000
#define ZIP_STR_14B (1 << 6)            //0100 0000
#define ZIP_STR_32B (2 << 6)            //1000 0000

#define ZIP_INT_MASK 0x30               //0011 0000
#define ZIP_INT_16B (0xc0 | 0<<4)       //1100 0000
#define ZIP_INT_32B (0xc0 | 1<<4)       //1101 0000
#define ZIP_INT_64B (0xc0 | 2<<4)       //1110 0000
#define ZIP_INT_24B (0xc0 | 3<<4)       //1111 0000
#define ZIP_INT_8B 0xfe                 //1111 1110

/* 4 bit integer immediate encoding */
#define ZIP_INT_IMM_MASK 0x0f           //0000 1111
#define ZIP_INT_IMM_MIN 0xf1            //1111 0001
#define ZIP_INT_IMM_MAX 0xfd            //1111 1101
#define ZIP_INT_IMM_VAL(v) (v & ZIP_INT_IMM_MASK)

//24位所能表示的最大值max 和 最小值min
#define INT24_MAX 0x7fffff              //0111 1111 1111 1111 1111 1111
#define INT24_MIN (-INT24_MAX - 1)      //1000 0000 0000 0000 0000 0001 (补码)

/*
*   正数的原码等于补码，负数的补码等于原码按位取反，末尾加1
*   计算机中所有数字的表示形式是补码，对于正数直接将二进制补码转换为十进制读数，而对于负数需要将补码转换成原码。
*/

/* Macro to determine type */
#define ZIP_IS_STR(enc) (((enc) & ZIP_STR_MASK) < ZIP_STR_MASK) //enc编码格式是否是字符串编码格式

/* Utility macros */
//  ziplist的成员宏定义
//  (*((uint32_t*)(zl))) 先对char *类型的zl进行强制类型转换成uint32_t *类型，
//  然后在用*运算符进行取内容运算，此时zl能访问的内存大小为4个字节。

#define
ZIPLIST_BYTES(zl)       (*((uint32_t*)(zl)))
//将zl定位到前4个字节的bytes成员，记录这整个压缩列表的内存字节数

#define ZIPLIST_TAIL_OFFSET(zl) (*((uint32_t*)((zl)+sizeof(uint32_t))))
//将zl定位到4字节到8字节的offset成员，记录着压缩列表尾节点距离列表的起始地址的偏移字节量

#define ZIPLIST_LENGTH(zl)      (*((uint16_t*)((zl)+sizeof(uint32_t)*2)))
//将zl定位到8字节到10字节的length成员，记录着压缩列表的节点数量

#define ZIPLIST_HEADER_SIZE     (sizeof(uint32_t)*2+sizeof(uint16_t))
//压缩列表表头（以上三个属性）的大小10个字节

#define ZIPLIST_ENTRY_HEAD(zl)  ((zl)+ZIPLIST_HEADER_SIZE)
//返回压缩列表首节点的地址

#define ZIPLIST_ENTRY_TAIL(zl)  ((zl)+intrev32ifbe(ZIPLIST_TAIL_OFFSET(zl)))
//返回压缩列表尾节点的地址

#define ZIPLIST_ENTRY_END(zl)   ((zl)+intrev32ifbe(ZIPLIST_BYTES(zl))-1)
//返回end成员的地址，一个字节。

/* We know a positive increment can only be 1 because entries can only be
 * pushed one at a time. */

#define ZIPLIST_INCR_LENGTH(zl,incr) { \        //增加节点数 \
    if (ZIPLIST_LENGTH(zl) < UINT16_MAX) \      //如果当前节点数小于65535，那么给length成员加incr个节点  \
        ZIPLIST_LENGTH(zl) = intrev16ifbe(intrev16ifbe(ZIPLIST_LENGTH(zl))+incr); \
}

typedef struct zlentry {
    //prevrawlen 前驱节点的长度
    //prevrawlensize 编码前驱节点的长度prevrawlen所需要的字节大小
    unsigned int prevrawlensize, prevrawlen;

    //len 当前节点值长度
    //lensize 编码当前节点长度len所需的字节数
    unsigned int lensize, len;

    //当前节点header的大小 = lensize + prevrawlensize
    unsigned int headersize;

    //当前节点的编码格式
    unsigned char encoding;

    //指向当前节点的指针，以char *类型保存
    unsigned char *p;
} zlentry;                  //压缩列表节点信息的结构

/* Extract the encoding from the byte pointed by 'ptr' and set it into
 * 'encoding'. */
#define ZIP_ENTRY_ENCODING(ptr, encoding) do {  \   //从ptr数组中取出节点的编码格式并将其赋值给encoding \
    (encoding) = (ptr[0]); \
    if ((encoding) < ZIP_STR_MASK) (encoding) &= ZIP_STR_MASK; \
} while(0)

/* Return bytes needed to store integer encoded by 'encoding' */
static unsigned int zipIntSize(unsigned char encoding) {    //返回保存编码格式encoding的字节大小
    switch(encoding) {
    case ZIP_INT_8B:  return 1;
    case ZIP_INT_16B: return 2;
    case ZIP_INT_24B: return 3;
    case ZIP_INT_32B: return 4;
    case ZIP_INT_64B: return 8;
    default: return 0; /* 4 bit immediate */
    }
    assert(NULL);
    return 0;
}

/* Encode the length 'rawlen' writing it in 'p'. If p is NULL it just returns
 * the amount of bytes required to encode such a length. */
//将节点的length成员编码，并写入p中，并返回编码的字节数，如果p为空只返回字节数。
static unsigned int zipEncodeLength(unsigned char *p, unsigned char encoding, unsigned int rawlen) {
    unsigned char len = 1, buf[5];

    if (ZIP_IS_STR(encoding)) { //如果是字符串编码格式
        /* Although encoding is given it may not be set for strings,
         * so we determine it here using the raw length. */
        if (rawlen <= 0x3f) {   //rawlen <= 0011 1111
            if (!p) return len; //返回1个字节
            buf[0] = ZIP_STR_06B | rawlen;   // 0000 0000 | rawlen = rawlen  将编码格式保存到buf[0]中
        } else if (rawlen <= 0x3fff) {       //0100 0000 <= rawlen <= 0011 1111 1111 1111
            len += 1;   //返回2个字节
            if (!p) return len;
            buf[0] = ZIP_STR_14B | ((rawlen >> 8) & 0x3f);  //将编码格式高八位写入buf[0]
            buf[1] = rawlen & 0xff;                         //将编码格式低八位写入buf[1]
        } else {
            len += 4;   //返回5个字节用来表示字符串
            if (!p) return len;
            buf[0] = ZIP_STR_32B;       //将编码格式的高八位写如buf[0]，剩下的四个字节全写为1
            buf[1] = (rawlen >> 24) & 0xff;
            buf[2] = (rawlen >> 16) & 0xff;
            buf[3] = (rawlen >> 8) & 0xff;
            buf[4] = rawlen & 0xff;
        }
    } else {    //如果是整型编码格式
        /* Implies integer encoding, so length is always 1. */
        if (!p) return len; //整型的长度只需要1个字节表示
        buf[0] = encoding;  //保存编码格式
    }

    /* Store this length at p */
    memcpy(p,buf,len);      //将编码格式写到p中
    return len;
}

/* Decode the length encoded in 'ptr'. The 'encoding' variable will hold the
 * entries encoding, the 'lensize' variable will hold the number of bytes
 * required to encode the entries length, and the 'len' variable will hold the
 * entries length. */
//从ptr中取出节点信息，并将其保存在encoding、lensize和len中
#define ZIP_DECODE_LENGTH(ptr, encoding, lensize, len) do {                             \
    /*从ptr数组中取出节点的编码格式并将其赋值给encoding*/                               \
    ZIP_ENTRY_ENCODING((ptr), (encoding));                                              \
    /*如果是字符串编码格式*/                                                            \
    if ((encoding) < ZIP_STR_MASK) {                                                    \
        if ((encoding) == ZIP_STR_06B) {   /*6位字符串编码格式*/                        \
            (lensize) = 1;                 /*编码长度需要1个字节*/                      \
            (len) = (ptr)[0] & 0x3f;       /*当前字节长度保存到len中*/                  \
        } else if ((encoding) == ZIP_STR_14B) {    /*14位字符串编码格式*/               \
            (lensize) = 2;                 /*编码长度需要2个字节*/                      \
            (len) = (((ptr)[0] & 0x3f) << 8) | (ptr)[1]; /*当前字节长度保存到len中*/    \
        } else if (encoding == ZIP_STR_32B) {   /*32串编码格式*/                        \
            (lensize) = 5;                   /*编码长度需要5节*/                        \
            (len) = ((ptr)[1] << 24) |         /*当前字节长度保存到len中*/              \
                    ((ptr)[2] << 16) |                                                  \
                    ((ptr)[3] <<  8) |                                                  \
                    ((ptr)[4]);                                                         \
        } else {                                                                        \
            assert(NULL);                                                               \
        }                                                                               \
    } else {    /*整数编码格式*/                                                        \
        (lensize) = 1;            /*需要1个字节*/                                       \
        (len) = zipIntSize(encoding);                                                   \
    }                                                                                   \
} while(0);

/* Encode the length of the previous entry and write it to "p". Return the
 * number of bytes needed to encode this length if "p" is NULL. */
//对p指向的当前节点的前驱节点的长度len成员进行编码，并写入p中，如果p为空，则仅仅返回编码len所需要的字节数
static unsigned int zipPrevEncodeLength(unsigned char *p, unsigned int len) {
    if (p == NULL) {
        return (len < ZIP_BIGLEN) ? 1 : sizeof(len)+1;  //如果前驱节点的长度len字节小于254则返回1个字节，否则返回5个
    } else {
        if (len < ZIP_BIGLEN) { //如果前驱节点的长度len字节小于254
            p[0] = len;         //将len保存在p[0]中
            return 1;           //返回所需的编码数1字节
        } else {                //前驱节点的长度len大于254字节
            p[0] = ZIP_BIGLEN;  //添加5字节的标示，0xfe
            memcpy(p+1,&len,sizeof(len));   //从p+1的起始地址开始拷贝len
            memrev32ifbe(p+1);
            return 1+sizeof(len);   //返回所需的编码数5字节
        }
    }
}

/* Encode the length of the previous entry and write it to "p". This only
 * uses the larger encoding (required in __ziplistCascadeUpdate). */
//将1个字节保存前置节点长度的编码成5字节格式
static void zipPrevEncodeLengthForceLarge(unsigned char *p, unsigned int len) {
    if (p == NULL) return;
    p[0] = ZIP_BIGLEN;  //5字节的标示 0xfe
    memcpy(p+1,&len,sizeof(len));   //拷贝len从地址p+1开始
    memrev32ifbe(p+1);
}

/* Decode the number of bytes required to store the length of the previous
 * element, from the perspective of the entry pointed to by 'ptr'. */
//取出编码前置节点所需要的节点数并保存到prevlensize中
#define ZIP_DECODE_PREVLENSIZE(ptr, prevlensize) do {                          \
    /*如果前驱节点小于254，则需要一个字节编码，返回1，否则返回5*/              \
    if ((ptr)[0] < ZIP_BIGLEN) {                                               \
        (prevlensize) = 1;                                                     \
    } else {                                                                   \
        (prevlensize) = 5;                                                     \
    }                                                                          \
} while(0);

/* Decode the length of the previous element, from the perspective of the entry
 * pointed to by 'ptr'. */
//取出编码前驱节点长度所需的字节数，并将其保存到prevlensize中，根据prevlensize，从ptr中取出节点长度值，并将其保存到prevlen
#define ZIP_DECODE_PREVLEN(ptr, prevlensize, prevlen) do {                     \
    /*先从ptr中取出编码前驱节点所需的字节数*/                                  \
    ZIP_DECODE_PREVLENSIZE(ptr, prevlensize);                                  \
    /*根据prevlensize字节数，找到节点长度值*/                                  \
    if ((prevlensize) == 1) {    /*如果是1字节的编码，那么就取ptr[0]*/         \
        (prevlen) = (ptr)[0];                                                  \
    } else if ((prevlensize) == 5) {/*如果是5字节的编码，那么就取ptr[1]到[4]*/ \
        assert(sizeof((prevlensize)) == 4); /*因为ptr[0]是标示0xfe*/           \
        memcpy(&(prevlen), ((char*)(ptr)) + 1, 4);                             \
        memrev32ifbe(&prevlen);                                                \
    }                                                                          \
} while(0);

/* Return the difference in number of bytes needed to store the length of the
 * previous element 'len', in the entry pointed to by 'p'. */
//计算 编码新的前驱节点长度所需的字节数减去原来所需字节数的差
static int zipPrevLenByteDiff(unsigned char *p, unsigned int len) {
    unsigned int prevlensize;
    ZIP_DECODE_PREVLENSIZE(p, prevlensize); //先从p中取出编码前驱节点所需的字节数
    return zipPrevEncodeLength(NULL, len) - prevlensize;    //计算编码len所需要的字节数减去刚才的prevlensize
}

/* Return the total number of bytes used by the entry pointed to by 'p'. */
//返回指针p所指向的节点所占用的字节数和
static unsigned int zipRawEntryLength(unsigned char *p) {
    unsigned int prevlensize, encoding, lensize, len;
    ZIP_DECODE_PREVLENSIZE(p, prevlensize);     //取出编码前驱节点长度所需的字节数
    ZIP_DECODE_LENGTH(p + prevlensize, encoding, lensize, len);//取出当前节点的编码格式,编码节点长度所需的字节数，节点长度
    return prevlensize + lensize + len;
}

/* Check if string pointed to by 'entry' can be encoded as an integer.
 * Stores the integer value in 'v' and its encoding in 'encoding'. */
// 尝试更改entry指向的字符串的编码格式，如果可以将其保存在encoding中
static int zipTryEncoding(unsigned char *entry, unsigned int entrylen, long long *v, unsigned char *encoding) {
    long long value;

    if (entrylen >= 32 || entrylen == 0) return 0;  //字符串太长超过long long能表示的范围或太短返回0
    if (string2ll((char*)entry,entrylen,&value)) {  //将entry字符串转换为longlong类型保存在value中
        /* Great, the string can be encoded. Check what's the smallest
         * of our encoding types that can hold this value. */
        if (value >= 0 && value <= 12) {    //如果value介于0和12之间
            *encoding = ZIP_INT_IMM_MIN+value;  //设置编码格式介于ZIP_INT_IMM_MIN和ZIP_INT_IMM_MAX之间的值
        } else if (value >= INT8_MIN && value <= INT8_MAX) {//如果value介于八位能表示的范围
            *encoding = ZIP_INT_8B;             //设置编码格式为ZIP_INT_8B
        } else if (value >= INT16_MIN && value <= INT16_MAX) {
            *encoding = ZIP_INT_16B;
        } else if (value >= INT24_MIN && value <= INT24_MAX) {
            *encoding = ZIP_INT_24B;
        } else if (value >= INT32_MIN && value <= INT32_MAX) {
            *encoding = ZIP_INT_32B;
        } else {
            *encoding = ZIP_INT_64B;
        }
        *v = value; //保存value到v
        return 1;
    }
    return 0;
}

/* Store integer 'value' at 'p', encoded as 'encoding' */
//以encoding编码方式，将value写到p中
static void zipSaveInteger(unsigned char *p, int64_t value, unsigned char encoding) {
    int16_t i16;
    int32_t i32;
    int64_t i64;

    // 根据encoding的编码格式不同，将value写到p中
    if (encoding == ZIP_INT_8B) {
        ((int8_t*)p)[0] = (int8_t)value;
    } else if (encoding == ZIP_INT_16B) {
        i16 = value;
        memcpy(p,&i16,sizeof(i16));
        memrev16ifbe(p);
    } else if (encoding == ZIP_INT_24B) {
        i32 = value<<8;
        memrev32ifbe(&i32);
        memcpy(p,((uint8_t*)&i32)+1,sizeof(i32)-sizeof(uint8_t));
    } else if (encoding == ZIP_INT_32B) {
        i32 = value;
        memcpy(p,&i32,sizeof(i32));
        memrev32ifbe(p);
    } else if (encoding == ZIP_INT_64B) {
        i64 = value;
        memcpy(p,&i64,sizeof(i64));
        memrev64ifbe(p);
    } else if (encoding >= ZIP_INT_IMM_MIN && encoding <= ZIP_INT_IMM_MAX) {
        /* Nothing to do, the value is stored in the encoding itself. */
    } else {
        assert(NULL);
    }
}

/* Read integer encoded as 'encoding' from 'p' */
// 以encoding的编码格式从p中返回整数值
static int64_t zipLoadInteger(unsigned char *p, unsigned char encoding) {
    int16_t i16;
    int32_t i32;
    int64_t i64, ret = 0;

    //根据encoding不同的编码，将p中值返回
    if (encoding == ZIP_INT_8B) {
        ret = ((int8_t*)p)[0];
    } else if (encoding == ZIP_INT_16B) {
        memcpy(&i16,p,sizeof(i16));
        memrev16ifbe(&i16);
        ret = i16;
    } else if (encoding == ZIP_INT_32B) {
        memcpy(&i32,p,sizeof(i32));
        memrev32ifbe(&i32);
        ret = i32;
    } else if (encoding == ZIP_INT_24B) {
        i32 = 0;
        memcpy(((uint8_t*)&i32)+1,p,sizeof(i32)-sizeof(uint8_t));
        memrev32ifbe(&i32);
        ret = i32>>8;
    } else if (encoding == ZIP_INT_64B) {
        memcpy(&i64,p,sizeof(i64));
        memrev64ifbe(&i64);
        ret = i64;
    } else if (encoding >= ZIP_INT_IMM_MIN && encoding <= ZIP_INT_IMM_MAX) {
        ret = (encoding & ZIP_INT_IMM_MASK)-1;  //减1 是因为ZIP_INT_IMM_MIN值为1，编码时的加上了ZIP_INT_IMM_MIN的值
    } else {
        assert(NULL);
    }
    return ret;
}

/* Return a struct with all information about an entry. */
// 将p指向的列表节点信息全部保存到zlentry中，并返回该结构
static zlentry zipEntry(unsigned char *p) {
    zlentry e;

    // e.prevrawlensize 保存着编码前一个节点的长度所需的字节数
    // prevrawlen 保存着前一个节点的长度
    ZIP_DECODE_PREVLEN(p, e.prevrawlensize, e.prevrawlen);

    // p + e.prevrawlensize将指针移动到当前节点信息的起始地址
    // encoding保存当前节点的编码格式
    // lensize保存编码节点值长度所需的字节数
    // len保存这节点值的长度
    ZIP_DECODE_LENGTH(p + e.prevrawlensize, e.encoding, e.lensize, e.len);
    e.headersize = e.prevrawlensize + e.lensize;    //当前节点header的大小 = lensize + prevrawlensize
    e.p = p;    //保存指针
    return e;
}

/* Create a new empty ziplist. */
unsigned char *ziplistNew(void) {   //创建并返回一个新的压缩列表
    //ZIPLIST_HEADER_SIZE是压缩列表的表头大小，1字节是末端的end大小
    unsigned int bytes = ZIPLIST_HEADER_SIZE+1;

    unsigned char *zl = zmalloc(bytes); //为表头和表尾end成员分配空间
    ZIPLIST_BYTES(zl) = intrev32ifbe(bytes);    //将bytes成员初始化为bytes
    ZIPLIST_TAIL_OFFSET(zl) = intrev32ifbe(ZIPLIST_HEADER_SIZE);    //空列表的offset成员为表头大小
    ZIPLIST_LENGTH(zl) = 0;     //几点数量为0
    zl[bytes-1] = ZIP_END;      //将表尾end成员设置成默认的255
    return zl;
}

/* Resize the ziplist. */
static unsigned char *ziplistResize(unsigned char *zl, unsigned int len) {  //调整压缩列表为len大小
    zl = zrealloc(zl,len);  //给zl重新分配空间，如果len大于原来的大小则保留原有的元素
    ZIPLIST_BYTES(zl) = intrev32ifbe(len);  //更新总字节数bytes元素
    zl[len-1] = ZIP_END;                //重新设置列表的尾端成员
    return zl;
}

/* When an entry is inserted, we need to set the prevlen field of the next
 * entry to equal the length of the inserted entry. It can occur that this
 * length cannot be encoded in 1 byte and the next entry needs to be grow
 * a bit larger to hold the 5-byte encoded prevlen. This can be done for free,
 * because this only happens when an entry is already being inserted (which
 * causes a realloc and memmove). However, encoding the prevlen may require
 * that this entry is grown as well. This effect may cascade throughout
 * the ziplist when there are consecutive entries with a size close to
 * ZIP_BIGLEN, so we need to check that the prevlen can be encoded in every
 * consecutive entry.
 *
 * 当将一个新节点添加到某个节点之前的时候，
 * 如果原节点的 header 空间不足以保存新节点的长度，
 * 那么就需要对原节点的 header 空间进行扩展（从 1 字节扩展到 5 字节）。
 *
 * 但是，当对原节点进行扩展之后，原节点的下一个节点的 prevlen 可能出现空间不足，
 * 这种情况在多个连续节点的长度都接近 ZIP_BIGLEN 时可能发生。
 *
 * 这个函数就用于检查并修复后续节点的空间问题。
 *
 * Note that this effect can also happen in reverse, where the bytes required
 * to encode the prevlen field can shrink. This effect is deliberately ignored,
 * because it can cause a "flapping" effect where a chain prevlen fields is
 * first grown and then shrunk again after consecutive inserts. Rather, the
 * field is allowed to stay larger than necessary, because a large prevlen
 * field implies the ziplist is holding large entries anyway.
 *
 * 反过来说，
 * 因为节点的长度变小而引起的连续缩小也是可能出现的，
 * 不过，为了避免扩展-缩小-扩展-缩小这样的情况反复出现（flapping，抖动），
 * 我们不处理这种情况，而是任由 prevlen 比所需的长度更长。

 * The pointer "p" points to the first entry that does NOT need to be
 * updated, i.e. consecutive fields MAY need an update.
 *
 * 注意，程序的检查是针对 p 的后续节点，而不是 p 所指向的节点。
 * 因为节点 p 在传入之前已经完成了所需的空间扩展工作。
 */
static unsigned char *__ziplistCascadeUpdate(unsigned char *zl, unsigned char *p) {
    size_t curlen = intrev32ifbe(ZIPLIST_BYTES(zl)), rawlen, rawlensize;    //cur保存当前列表的总字节数
    size_t offset, noffset, extra;
    unsigned char *np;
    zlentry cur, next;

    //只要没有到压缩列表的end成员就继续循环
    while (p[0] != ZIP_END) {
        cur = zipEntry(p);      //将p指向的节点信息保存到cur结构中

        // headersize = lensize + prevrawlensize
        //前取节点长度编码所占字节数，和当前节点长度编码所占字节数，在加上当前节点的value长度
        //rawlen = prev_entry_len + encoding + value
        rawlen = cur.headersize + cur.len;      //当前节点的长度
        rawlensize = zipPrevEncodeLength(NULL,rawlen);  //计算编码当前节点的长度所需的字节数

        /* Abort if there is no next entry. */
        //如果没有下一个节点则跳出循环
        //这是连锁更新的第1个结束条件
        if (p[rawlen] == ZIP_END) break;

        //取出后继节点的信息保存到next中
        next = zipEntry(p+rawlen);

        /* Abort when "prevlen" has not changed. */
        //如果next节点的prevrawlen所保存的上一个节点长度等于rawlen
        //说明next节点的prevrawlen空间足够存放前驱节点的长度值
        //当前节点空间足够，那么这个节点以后的节点都不用更新，因此跳出循环
        //这是连锁更新的第2个结束条件
        if (next.prevrawlen == rawlen) break;

        //如果next节点对前一个节点长度的编码所需的字节数next.prevrawlensize 小于 对上一个节点长度进行编码所需要的节点rawlensize
        //因此要对next节点的header部分进行扩展，以便能够表示前一个节点的长度
        if (next.prevrawlensize < rawlensize) {
            /* The "prevlen" field of "next" needs more bytes to hold
             * the raw length of "cur". */

            offset = p-zl;          //记录当指针p的偏移量
            extra = rawlensize-next.prevrawlensize; //需要扩展的字节数
            zl = ziplistResize(zl,curlen+extra);    //调整压缩链表的空间大小
            p = zl+offset;  //还原p指向的位置

            /* Current pointer and offset for next element. */
            np = p+rawlen;      //next节点的新地址
            noffset = np-zl;    //记录next节点的偏移量

            /* Update tail offset when next element is not the tail element. */
            //更新压缩列表的表头tail_offset成员，如果next节点是尾部节点就不用更新
            if ((zl+intrev32ifbe(ZIPLIST_TAIL_OFFSET(zl))) != np) {
                ZIPLIST_TAIL_OFFSET(zl) =
                    intrev32ifbe(intrev32ifbe(ZIPLIST_TAIL_OFFSET(zl))+extra);
            }

            /* Move the tail to the back. */
            //移动next节点到新地址，为前驱节点cur腾出空间
            memmove(np+rawlensize,
                np+next.prevrawlensize,
                curlen-noffset-next.prevrawlensize-1);
            //将next节点的header以rawlen长度进行重新编码，更新prevrawlensize和prevrawlen
            zipPrevEncodeLength(np,rawlen);

            /* Advance the cursor */
            //更新p指针，移动到next节点，处理next的next节点
            p += rawlen;
            curlen += extra;    //更新压缩列表的总字节数
        } else {
            //如果next几点的prevrawlensize足够对前驱节点cur进行编码，但是不会进行缩小
            if (next.prevrawlensize > rawlensize) {
                /* This would result in shrinking, which we want to avoid.
                 * So, set "rawlen" in the available bytes. */
                //执行到这里说明， next 节点编码前置节点的 header 空间有 5 字节,而编码 rawlen 只需要 1 字节
                //因此，用5字节的空间将1字节的编码重新编码
                zipPrevEncodeLengthForceLarge(p+rawlen,rawlen);
            } else {
                //执行到这里说明，next.prevrawlensize = rawlensize
                //刚好足够空间进行编码，只需更新next节点的header
                zipPrevEncodeLength(p+rawlen,rawlen);
            }

            /* Stop here, as the raw length of "next" has not changed. */
            break;
        }
    }
    return zl;
}

/* Delete "num" entries, starting at "p". Returns pointer to the ziplist. */
//从p开始连续删除num个节点
static unsigned char *__ziplistDelete(unsigned char *zl, unsigned char *p, unsigned int num) {
    unsigned int i, totlen, deleted = 0;
    size_t offset;
    int nextdiff = 0;
    zlentry first, tail;

    first = zipEntry(p);    //保存p指向节点的信息
    for (i = 0; p[0] != ZIP_END && i < num; i++) {
        p += zipRawEntryLength(p);  //返回p指向节点所占的字节和，将p移动到下一个节点的起始地址
        deleted++;
    }

    totlen = p-first.p; //totle保存所有被删除节点的内存大小
    if (totlen > 0) {
        if (p[0] != ZIP_END) {  //此时p指向被删除节点后的第一个不删除的节点，如果被删除的节点后还有节点
            /* Storing `prevrawlen` in this entry may increase or decrease the
             * number of bytes required compare to the current `prevrawlen`.
             * There always is room to store this, because it was previously
             * stored by an entry that is now being deleted. */
            //因为被删除的节点后的第一个节点header部分可能无法表示前驱节点的长度
            //因此要计算出之前的节点长度和新的前驱节点长度的差值
            nextdiff = zipPrevLenByteDiff(p,first.prevrawlen);
            //将p向后移动差值的长度，减少删除的内存，用来扩展header空间
            p -= nextdiff;
            //将first的前驱节点长度编码扩展到p
            zipPrevEncodeLength(p,first.prevrawlen);

            /* Update offset for tail */
            //更新表头偏移量tail_offset成员，只是减去所有被删除的内存
            ZIPLIST_TAIL_OFFSET(zl) =
                intrev32ifbe(intrev32ifbe(ZIPLIST_TAIL_OFFSET(zl))-totlen);

            /* When the tail contains more than one entry, we need to take
             * "nextdiff" in account as well. Otherwise, a change in the
             * size of prevlen doesn't have an effect on the *tail* offset. */
            //记录当前p指向的节点信息
            tail = zipEntry(p);
            //如果被删除节点后有多于一个节点，那么程序需要将nextdiff记录的字节数也计算到表尾偏移量中。
            //再次更新tail_offset成员，因为当前p指向的节点不是尾节点，因此要加上nextdiff
            //这样才能让表尾偏移量正确对齐表尾节点。
            if (p[tail.headersize+tail.len] != ZIP_END) {
                ZIPLIST_TAIL_OFFSET(zl) =
                   intrev32ifbe(intrev32ifbe(ZIPLIST_TAIL_OFFSET(zl))+nextdiff);
            }

            /* Move tail to the front of the ziplist */
            //移动数据
            memmove(first.p,p,
                intrev32ifbe(ZIPLIST_BYTES(zl))-(p-zl)-1);
        } else {    //(p[0] == ZIP_END，已经没有节点了
            /* The entire tail was deleted. No need to move memory. */
            //更新tail_offset到前一个节点的地址，因为前一个节点是列表尾节点
            ZIPLIST_TAIL_OFFSET(zl) =
                intrev32ifbe((first.p-zl)-first.prevrawlen);
        }

        /* Resize and update length */
        //因为删除了节点，所以要删除多余的内存
        offset = first.p-zl;
        zl = ziplistResize(zl, intrev32ifbe(ZIPLIST_BYTES(zl))-totlen+nextdiff);    //调整内存大小
        ZIPLIST_INCR_LENGTH(zl,-deleted);   //更新节点计数器
        p = zl+offset;

        /* When nextdiff != 0, the raw length of the next entry has changed, so
         * we need to cascade the update throughout the ziplist */
        if (nextdiff != 0)
            zl = __ziplistCascadeUpdate(zl,p);  //检查是否需要连锁更新
    }
    return zl;
}

/* Insert item at "p". */
//将长度为slen的字节数组插入到p所指定的位置
static unsigned char *__ziplistInsert(unsigned char *zl, unsigned char *p, unsigned char *s, unsigned int slen) {
    size_t curlen = intrev32ifbe(ZIPLIST_BYTES(zl)), reqlen;    //curlen记录当前压缩列表的总字节数
    unsigned int prevlensize, prevlen = 0;
    size_t offset;
    int nextdiff = 0;
    unsigned char encoding = 0;
    long long value = 123456789; /* initialized to avoid warning. Using a value
                                    that is easy to see if for some reason
                                    we use it uninitialized. */
    zlentry tail;

    /* Find out prevlen for the entry that is inserted. */
    if (p[0] != ZIP_END) {  //如果p不是指向列表的尾部，说明要插入的位置在列表的中间
        //获取p指向节点的prevlensize和prevlen成员
        ZIP_DECODE_PREVLEN(p, prevlensize, prevlen);
    } else {    //p指向压缩列表的尾部
        unsigned char *ptail = ZIPLIST_ENTRY_TAIL(zl);  //获取压缩列表的尾节点地址
        if (ptail[0] != ZIP_END) {  //如果ptail指向最后一个节点，
            prevlen = zipRawEntryLength(ptail); //取出列表尾节点的长度，作为插入节点的prevlen
        }
    }

    /* See if the entry can be encoded */
    //尝试将字符串转换为整数，如果可以，value保存节点，encoding保存value的编码方式
    if (zipTryEncoding(s,slen,&value,&encoding)) {
        /* 'encoding' is set to the appropriate integer encoding */
        reqlen = zipIntSize(encoding);  //保存编码长度所需的字节数
    } else {
        /* 'encoding' is untouched, however zipEncodeLength will use the
         * string length to figure out how to encode it. */
        reqlen = slen;  //不能转换，保存字符串长度slen到reqlen
    }
    /* We need space for both the length of the previous entry and
     * the length of the payload. */
    reqlen += zipPrevEncodeLength(NULL,prevlen);    //计算前驱节点的长度所需字节数
    reqlen += zipEncodeLength(NULL,encoding,slen);  //计算当前节点长度所需字节数

    /* When the insert position is not equal to the tail, we need to
     * make sure that the next entry can hold this entry's length in
     * its prevlen field. */
    //检查p指向的节点的header能否编码新节点的长度
    //nextdiff保存新旧编码的插值，如果大于0，说明要对p指向节点的header进行扩展
    nextdiff = (p[0] != ZIP_END) ? zipPrevLenByteDiff(p,reqlen) : 0;

    /* Store offset because a realloc may change the address of zl. */
    //计算当前地址的偏移量
    offset = p-zl;
    zl = ziplistResize(zl,curlen+reqlen+nextdiff);  //重新调整内存大小
    p = zl+offset;  //新分配的地址zl加偏移量offset找到刚才操作的位置

    /* Apply memory move when necessary and update tail offset. */
    if (p[0] != ZIP_END) {  //新节点之后还有节点
        /* Subtract one because of the ZIP_END bytes */
        memmove(p+reqlen,p-nextdiff,curlen-offset-1+nextdiff);  //为新节点腾出空间

        /* Encode this entry's raw length in the next entry. */
        zipPrevEncodeLength(p+reqlen,reqlen);   //计算新节点的编码，保存到刚才移动的后继节点的header中

        /* Update offset for tail */
        //更新列表表头成员tail_offset
        ZIPLIST_TAIL_OFFSET(zl) =
            intrev32ifbe(intrev32ifbe(ZIPLIST_TAIL_OFFSET(zl))+reqlen);

        /* When the tail contains more than one entry, we need to take
         * "nextdiff" in account as well. Otherwise, a change in the
         * size of prevlen doesn't have an effect on the *tail* offset. */
        //如果新节点后有多于一个节点，需要将nextdiff值也计算到表尾偏移量中tail_offset
        tail = zipEntry(p+reqlen);
        if (p[reqlen+tail.headersize+tail.len] != ZIP_END) {
            ZIPLIST_TAIL_OFFSET(zl) =
                intrev32ifbe(intrev32ifbe(ZIPLIST_TAIL_OFFSET(zl))+nextdiff);
        }
    } else {
        /* This element will be the new tail. */
        // 新节点就是表尾节点
        ZIPLIST_TAIL_OFFSET(zl) = intrev32ifbe(p-zl);
    }

    /* When nextdiff != 0, the raw length of the next entry has changed, so
     * we need to cascade the update throughout the ziplist */
    //如果nextdiff不等于0的话，后继节点就有可能发生连锁更新
    if (nextdiff != 0) {
        offset = p-zl;
        zl = __ziplistCascadeUpdate(zl,p+reqlen);
        p = zl+offset;
    }

    /* Write the entry */
    p += zipPrevEncodeLength(p,prevlen);    //将前驱节点的长度写入到新节点的header
    p += zipEncodeLength(p,encoding,slen);  //将节点的长度写入新节点的header

    //根据编码，分别出是字节数组还是整数，将节点值写入节点的value
    if (ZIP_IS_STR(encoding)) {
        memcpy(p,s,slen);
    } else {
        zipSaveInteger(p,value,encoding);
    }
    ZIPLIST_INCR_LENGTH(zl,1);  //更新列表的节点计数器
    return zl;
}

//将长度为slen的字符串推入到zl的表头或表尾
unsigned char *ziplistPush(unsigned char *zl, unsigned char *s, unsigned int slen, int where) {
    unsigned char *p;

    //根据where计算出表头或表尾的地址
    p = (where == ZIPLIST_HEAD) ? ZIPLIST_ENTRY_HEAD(zl) : ZIPLIST_ENTRY_END(zl);
    return __ziplistInsert(zl,p,s,slen);    //将节点插入到p地址中
}

/* Returns an offset to use for iterating with ziplistNext. When the given
 * index is negative, the list is traversed back to front. When the list
 * doesn't contain an element at the provided index, NULL is returned. */
//根据下标index返回该下标的节点的地址
unsigned char *ziplistIndex(unsigned char *zl, int index) {
    unsigned char *p;
    unsigned int prevlensize, prevlen = 0;
    if (index < 0) {            //下标为负数
        index = (-index)-1;
        p = ZIPLIST_ENTRY_TAIL(zl); //计算出尾节点地址，从尾部开始遍历
        if (p[0] != ZIP_END) {      //列表不为空
            ZIP_DECODE_PREVLEN(p, prevlensize, prevlen);    //计算出表尾节点的prevlensize和prevlen值
            while (prevlen > 0 && index--) {    //根据prevlen值前前遍历
                p -= prevlen;       //更新p指针，指向前一个节点
                ZIP_DECODE_PREVLEN(p, prevlensize, prevlen);    //每次更新prevlensize和prevlen值
            }
        }
    } else {
        p = ZIPLIST_ENTRY_HEAD(zl); //计算出表头节点地址，从头开始遍历
        while (p[0] != ZIP_END && index--) {    //列表不为空
            p += zipRawEntryLength(p);  //更新p指针，指向后继节点
        }
    }
    return (p[0] == ZIP_END || index > 0) ? NULL : p;   //返回节点的地址
}

/* Return pointer to next entry in ziplist.
 *
 * zl is the pointer to the ziplist
 * p is the pointer to the current element
 *
 * The element after 'p' is returned, otherwise NULL if we are at the end. */
unsigned char *ziplistNext(unsigned char *zl, unsigned char *p) {   //返回p指向节点的后继节点的地址
    ((void) zl);

    /* "p" could be equal to ZIP_END, caused by ziplistDelete,
     * and we should return NULL. Otherwise, we should return NULL
     * when the *next* element is ZIP_END (there is no next entry). */
    if (p[0] == ZIP_END) {  //如果p已经指向列表尾部返回NULL，列表没有节点的情况
        return NULL;
    }

    p += zipRawEntryLength(p);  //更新p指向后继节点，只有一个节点的情况
    if (p[0] == ZIP_END) {  //p已经是表尾节点
        return NULL;
    }

    return p;
}

/* Return pointer to previous entry in ziplist. */
unsigned char *ziplistPrev(unsigned char *zl, unsigned char *p) {   //返回p指向节点的前驱节点地址
    unsigned int prevlensize, prevlen = 0;

    /* Iterating backwards from ZIP_END should return the tail. When "p" is
     * equal to the first element of the list, we're already at the head,
     * and should return NULL. */
    if (p[0] == ZIP_END) {          //p已经指向列表尾端，列表为空
        p = ZIPLIST_ENTRY_TAIL(zl); //找到尾节点地址
        return (p[0] == ZIP_END) ? NULL : p;    //尾端节点是end成员，那么返回NULL
    } else if (p == ZIPLIST_ENTRY_HEAD(zl)) {   //p指向头结点，无前驱节点，返回NULL
        return NULL;
    } else {
        ZIP_DECODE_PREVLEN(p, prevlensize, prevlen);    //p指向中间的节点，不是表头或表尾节点
        assert(prevlen > 0);    //保证中间节点的前驱节点长度大于0
        return p-prevlen;   //返回前驱节点地址
    }
}

/* Get entry pointed to by 'p' and store in either '*sstr' or 'sval' depending
 * on the encoding of the entry. '*sstr' is always set to NULL to be able
 * to find out whether the string pointer or the integer value was set.
 * Return 0 if 'p' points to the end of the ziplist, 1 otherwise. */
//提取p指向的节点的值
unsigned int ziplistGet(unsigned char *p, unsigned char **sstr, unsigned int *slen, long long *sval) {
    zlentry entry;
    if (p == NULL || p[0] == ZIP_END) return 0; //p为空，或p指向end成员的地址，返回0代表失败
    if (sstr) *sstr = NULL; //sstr不为空，设置*sstr为空

    entry = zipEntry(p);    //保存p指向节点的信息
    if (ZIP_IS_STR(entry.encoding)) {   //如果是字节数组
        if (sstr) {
            *slen = entry.len;      //保存字符串长度
            *sstr = p+entry.headersize;     //保存字符串值
        }
    } else {        //如果是整数
        if (sval) {
            *sval = zipLoadInteger(p+entry.headersize,entry.encoding);  //将值保存到sval中
        }
    }
    return 1;   //成功
}

/* Insert an entry at "p". */
//在p指向的地址插入节点，如果p指向一个节点，意味着是前插
unsigned char *ziplistInsert(unsigned char *zl, unsigned char *p, unsigned char *s, unsigned int slen) {
    return __ziplistInsert(zl,p,s,slen);
}

/* Delete a single entry from the ziplist, pointed to by *p.
 * Also update *p in place, to be able to iterate over the
 * ziplist, while deleting entries. */
//从zl中删除*p指向的节点
unsigned char *ziplistDelete(unsigned char *zl, unsigned char **p) {
    size_t offset = *p-zl;          //保存*p的偏移量
    zl = __ziplistDelete(zl,*p,1);  //zl可能被改变，因为可能重新分配内存了

    /* Store pointer to current element in p, because ziplistDelete will
     * do a realloc which might result in a different "zl"-pointer.
     * When the delete direction is back to front, we might delete the last
     * entry and end up with "p" pointing to ZIP_END, so check this. */
    *p = zl+offset; //还原*p的位置
    return zl;
}

/* Delete a range of entries from the ziplist. */
//删除下标index开始的num个节点
unsigned char *ziplistDeleteRange(unsigned char *zl, unsigned int index, unsigned int num) {
    unsigned char *p = ziplistIndex(zl,index);  //定位到下标为index节点的地址
    return (p == NULL) ? zl : __ziplistDelete(zl,p,num);
}

/* Compare entry pointer to by 'p' with 'sstr' of length 'slen'. */
/* Return 1 if equal. */
// 将p指向的节点和sstr比较，相等返回1，不相等返回0
unsigned int ziplistCompare(unsigned char *p, unsigned char *sstr, unsigned int slen) {
    zlentry entry;
    unsigned char sencoding;
    long long zval, sval;
    if (p[0] == ZIP_END) return 0;

    entry = zipEntry(p);    //保存p指向的节点信息
    if (ZIP_IS_STR(entry.encoding)) {   //字节数组
        /* Raw compare */
        if (entry.len == slen) {    //如果长度相等，比较内存
            return memcmp(p+entry.headersize,sstr,slen) == 0;   //相等返回1
        } else {
            return 0;
        }
    } else {    //整数比较
        /* Try to compare encoded values. Don't compare encoding because
         * different implementations may encoded integers differently. */
        if (zipTryEncoding(sstr,slen,&sval,&sencoding)) {   //如果能够将strr转换为整数
          zval = zipLoadInteger(p+entry.headersize,entry.encoding); //得到整数值
          return zval == sval;      //比较，相等返回1
        }
    }
    return 0;       //不相等
}

/* Find pointer to the entry equal to the specified entry. Skip 'skip' entries
 * between every comparison. Returns NULL when the field could not be found. */
//寻找节点值和vstr相等的节点，并返回给节点地址，并跳过skip个节点
unsigned char *ziplistFind(unsigned char *p, unsigned char *vstr, unsigned int vlen, unsigned int skip) {
    int skipcnt = 0;
    unsigned char vencoding = 0;
    long long vll = 0;

    //只要没有遍历完节点就一直遍历
    while (p[0] != ZIP_END) {
        unsigned int prevlensize, encoding, lensize, len;
        unsigned char *q;

        ZIP_DECODE_PREVLENSIZE(p, prevlensize);     //获得p指向节点header中的prevlensize
        ZIP_DECODE_LENGTH(p + prevlensize, encoding, lensize, len); //获得当前节点的lensize和len
        q = p + prevlensize + lensize;  //后继节点的地址

        if (skipcnt == 0) { //执行第一次之后会跳过skip次
            /* Compare current entry with specified entry */
            if (ZIP_IS_STR(encoding)) { //字节数组
                if (len == vlen && memcmp(q, vstr, vlen) == 0) {    //比较长度和内存value
                    return p;   //相等返回地址
                }
            } else {    //整数
                /* Find out if the searched field can be encoded. Note that
                 * we do it only the first time, once done vencoding is set
                 * to non-zero and vll is set to the integer value. */
                if (vencoding == 0) {
                    //如果第一次将vencoding被重新编码，以后就不会在执行zipTryEncoding
                    if (!zipTryEncoding(vstr, vlen, &vll, &vencoding)) {
                        /* If the entry can't be encoded we set it to
                         * UCHAR_MAX so that we don't retry again the next
                         * time. */
                        vencoding = UCHAR_MAX;
                    }
                    /* Must be non-zero by now */
                    assert(vencoding);
                }

                /* Compare current entry with specified entry, do it only
                 * if vencoding != UCHAR_MAX because if there is no encoding
                 * possible for the field it can't be a valid integer. */
                if (vencoding != UCHAR_MAX) {
                    long long ll = zipLoadInteger(q, encoding); //比较整数值
                    if (ll == vll) {
                        return p;
                    }
                }
            }

            /* Reset skip count */
            skipcnt = skip;
        } else {
            /* Skip entry */
            skipcnt--;  //跳过skip个节点
        }

        /* Move to next entry */
        p = q + len;    //p指向下一个节点
    }

    return NULL;
}

/* Return length of ziplist. */
//返回压缩列表的节点数量
unsigned int ziplistLen(unsigned char *zl) {
    unsigned int len = 0;
    if (intrev16ifbe(ZIPLIST_LENGTH(zl)) < UINT16_MAX) {    //如果及节点数小于UINT16_MAX
        len = intrev16ifbe(ZIPLIST_LENGTH(zl)); //直接获取length成员值
    } else {
        unsigned char *p = zl+ZIPLIST_HEADER_SIZE;  //如果节点数大于UINT16_MAX
        while (*p != ZIP_END) { //遍历整个列表
            p += zipRawEntryLength(p);
            len++;
        }

        /* Re-store length if small enough */
        if (len < UINT16_MAX) ZIPLIST_LENGTH(zl) = intrev16ifbe(len);   //设置节点数量
    }
    return len;
}

/* Return ziplist blob size in bytes. */
size_t ziplistBlobLen(unsigned char *zl) {  //返回压缩列表的所占内存字节数
    return intrev32ifbe(ZIPLIST_BYTES(zl));
}

void ziplistRepr(unsigned char *zl) {   //格式化打印压缩列表的信息
    unsigned char *p;
    int index = 0;
    zlentry entry;

    printf(
        "{total bytes %d} "
        "{length %u}\n"
        "{tail offset %u}\n",
        intrev32ifbe(ZIPLIST_BYTES(zl)),
        intrev16ifbe(ZIPLIST_LENGTH(zl)),
        intrev32ifbe(ZIPLIST_TAIL_OFFSET(zl)));
    p = ZIPLIST_ENTRY_HEAD(zl);
    while(*p != ZIP_END) {
        entry = zipEntry(p);
        printf(
            "{"
                "addr 0x%08lx, "
                "index %2d, "
                "offset %5ld, "
                "rl: %5u, "
                "hs %2u, "
                "pl: %5u, "
                "pls: %2u, "
                "payload %5u"
            "} ",
            (long unsigned)p,
            index,
            (unsigned long) (p-zl),
            entry.headersize+entry.len,
            entry.headersize,
            entry.prevrawlen,
            entry.prevrawlensize,
            entry.len);
        p += entry.headersize;
        if (ZIP_IS_STR(entry.encoding)) {
            if (entry.len > 40) {
                if (fwrite(p,40,1,stdout) == 0) perror("fwrite");
                printf("...");
            } else {
                if (entry.len &&
                    fwrite(p,entry.len,1,stdout) == 0) perror("fwrite");
            }
        } else {
            printf("%lld", (long long) zipLoadInteger(p,entry.encoding));
        }
        printf("\n");
        p += entry.len;
        index++;
    }
    printf("{end}\n\n");
}
//测试代码
#ifdef ZIPLIST_TEST_MAIN
#include <sys/time.h>
#include "adlist.h"
#include "sds.h"

#define debug(f, ...) { if (DEBUG) printf(f, __VA_ARGS__); }

unsigned char *createList() {
    unsigned char *zl = ziplistNew();
    zl = ziplistPush(zl, (unsigned char*)"foo", 3, ZIPLIST_TAIL);
    zl = ziplistPush(zl, (unsigned char*)"quux", 4, ZIPLIST_TAIL);
    zl = ziplistPush(zl, (unsigned char*)"hello", 5, ZIPLIST_HEAD);
    zl = ziplistPush(zl, (unsigned char*)"1024", 4, ZIPLIST_TAIL);
    return zl;
}

unsigned char *createIntList() {
    unsigned char *zl = ziplistNew();
    char buf[32];

    sprintf(buf, "100");
    zl = ziplistPush(zl, (unsigned char*)buf, strlen(buf), ZIPLIST_TAIL);
    sprintf(buf, "128000");
    zl = ziplistPush(zl, (unsigned char*)buf, strlen(buf), ZIPLIST_TAIL);
    sprintf(buf, "-100");
    zl = ziplistPush(zl, (unsigned char*)buf, strlen(buf), ZIPLIST_HEAD);
    sprintf(buf, "4294967296");
    zl = ziplistPush(zl, (unsigned char*)buf, strlen(buf), ZIPLIST_HEAD);
    sprintf(buf, "non integer");
    zl = ziplistPush(zl, (unsigned char*)buf, strlen(buf), ZIPLIST_TAIL);
    sprintf(buf, "much much longer non integer");
    zl = ziplistPush(zl, (unsigned char*)buf, strlen(buf), ZIPLIST_TAIL);
    return zl;
}

long long usec(void) {
    struct timeval tv;
    gettimeofday(&tv,NULL);
    return (((long long)tv.tv_sec)*1000000)+tv.tv_usec;
}

void stress(int pos, int num, int maxsize, int dnum) {
    int i,j,k;
    unsigned char *zl;
    char posstr[2][5] = { "HEAD", "TAIL" };
    long long start;
    for (i = 0; i < maxsize; i+=dnum) {
        zl = ziplistNew();
        for (j = 0; j < i; j++) {
            zl = ziplistPush(zl,(unsigned char*)"quux",4,ZIPLIST_TAIL);
        }

        /* Do num times a push+pop from pos */
        start = usec();
        for (k = 0; k < num; k++) {
            zl = ziplistPush(zl,(unsigned char*)"quux",4,pos);
            zl = ziplistDeleteRange(zl,0,1);
        }
        printf("List size: %8d, bytes: %8d, %dx push+pop (%s): %6lld usec\n",
            i,intrev32ifbe(ZIPLIST_BYTES(zl)),num,posstr[pos],usec()-start);
        zfree(zl);
    }
}

void pop(unsigned char *zl, int where) {
    unsigned char *p, *vstr;
    unsigned int vlen;
    long long vlong;

    p = ziplistIndex(zl,where == ZIPLIST_HEAD ? 0 : -1);
    if (ziplistGet(p,&vstr,&vlen,&vlong)) {
        if (where == ZIPLIST_HEAD)
            printf("Pop head: ");
        else
            printf("Pop tail: ");

        if (vstr)
            if (vlen && fwrite(vstr,vlen,1,stdout) == 0) perror("fwrite");
        else
            printf("%lld", vlong);

        printf("\n");
        ziplistDeleteRange(zl,-1,1);
    } else {
        printf("ERROR: Could not pop\n");
        exit(1);
    }
}

int randstring(char *target, unsigned int min, unsigned int max) {
    int p = 0;
    int len = min+rand()%(max-min+1);
    int minval, maxval;
    switch(rand() % 3) {
    case 0:
        minval = 0;
        maxval = 255;
    break;
    case 1:
        minval = 48;
        maxval = 122;
    break;
    case 2:
        minval = 48;
        maxval = 52;
    break;
    default:
        assert(NULL);
    }

    while(p < len)
        target[p++] = minval+rand()%(maxval-minval+1);
    return len;
}

void verify(unsigned char *zl, zlentry *e) {
    int i;
    int len = ziplistLen(zl);
    zlentry _e;

    for (i = 0; i < len; i++) {
        memset(&e[i], 0, sizeof(zlentry));
        e[i] = zipEntry(ziplistIndex(zl, i));

        memset(&_e, 0, sizeof(zlentry));
        _e = zipEntry(ziplistIndex(zl, -len+i));

        assert(memcmp(&e[i], &_e, sizeof(zlentry)) == 0);
    }
}

int main(int argc, char **argv) {
    unsigned char *zl, *p;
    unsigned char *entry;
    unsigned int elen;
    long long value;

    /* If an argument is given, use it as the random seed. */
    if (argc == 2)
        srand(atoi(argv[1]));

    zl = createIntList();
    ziplistRepr(zl);

    zl = createList();
    ziplistRepr(zl);

    pop(zl,ZIPLIST_TAIL);
    ziplistRepr(zl);

    pop(zl,ZIPLIST_HEAD);
    ziplistRepr(zl);

    pop(zl,ZIPLIST_TAIL);
    ziplistRepr(zl);

    pop(zl,ZIPLIST_TAIL);
    ziplistRepr(zl);

    printf("Get element at index 3:\n");
    {
        zl = createList();
        p = ziplistIndex(zl, 3);
        if (!ziplistGet(p, &entry, &elen, &value)) {
            printf("ERROR: Could not access index 3\n");
            return 1;
        }
        if (entry) {
            if (elen && fwrite(entry,elen,1,stdout) == 0) perror("fwrite");
            printf("\n");
        } else {
            printf("%lld\n", value);
        }
        printf("\n");
    }

    printf("Get element at index 4 (out of range):\n");
    {
        zl = createList();
        p = ziplistIndex(zl, 4);
        if (p == NULL) {
            printf("No entry\n");
        } else {
            printf("ERROR: Out of range index should return NULL, returned offset: %ld\n", p-zl);
            return 1;
        }
        printf("\n");
    }

    printf("Get element at index -1 (last element):\n");
    {
        zl = createList();
        p = ziplistIndex(zl, -1);
        if (!ziplistGet(p, &entry, &elen, &value)) {
            printf("ERROR: Could not access index -1\n");
            return 1;
        }
        if (entry) {
            if (elen && fwrite(entry,elen,1,stdout) == 0) perror("fwrite");
            printf("\n");
        } else {
            printf("%lld\n", value);
        }
        printf("\n");
    }

    printf("Get element at index -4 (first element):\n");
    {
        zl = createList();
        p = ziplistIndex(zl, -4);
        if (!ziplistGet(p, &entry, &elen, &value)) {
            printf("ERROR: Could not access index -4\n");
            return 1;
        }
        if (entry) {
            if (elen && fwrite(entry,elen,1,stdout) == 0) perror("fwrite");
            printf("\n");
        } else {
            printf("%lld\n", value);
        }
        printf("\n");
    }

    printf("Get element at index -5 (reverse out of range):\n");
    {
        zl = createList();
        p = ziplistIndex(zl, -5);
        if (p == NULL) {
            printf("No entry\n");
        } else {
            printf("ERROR: Out of range index should return NULL, returned offset: %ld\n", p-zl);
            return 1;
        }
        printf("\n");
    }

    printf("Iterate list from 0 to end:\n");
    {
        zl = createList();
        p = ziplistIndex(zl, 0);
        while (ziplistGet(p, &entry, &elen, &value)) {
            printf("Entry: ");
            if (entry) {
                if (elen && fwrite(entry,elen,1,stdout) == 0) perror("fwrite");
            } else {
                printf("%lld", value);
            }
            p = ziplistNext(zl,p);
            printf("\n");
        }
        printf("\n");
    }

    printf("Iterate list from 1 to end:\n");
    {
        zl = createList();
        p = ziplistIndex(zl, 1);
        while (ziplistGet(p, &entry, &elen, &value)) {
            printf("Entry: ");
            if (entry) {
                if (elen && fwrite(entry,elen,1,stdout) == 0) perror("fwrite");
            } else {
                printf("%lld", value);
            }
            p = ziplistNext(zl,p);
            printf("\n");
        }
        printf("\n");
    }

    printf("Iterate list from 2 to end:\n");
    {
        zl = createList();
        p = ziplistIndex(zl, 2);
        while (ziplistGet(p, &entry, &elen, &value)) {
            printf("Entry: ");
            if (entry) {
                if (elen && fwrite(entry,elen,1,stdout) == 0) perror("fwrite");
            } else {
                printf("%lld", value);
            }
            p = ziplistNext(zl,p);
            printf("\n");
        }
        printf("\n");
    }

    printf("Iterate starting out of range:\n");
    {
        zl = createList();
        p = ziplistIndex(zl, 4);
        if (!ziplistGet(p, &entry, &elen, &value)) {
            printf("No entry\n");
        } else {
            printf("ERROR\n");
        }
        printf("\n");
    }

    printf("Iterate from back to front:\n");
    {
        zl = createList();
        p = ziplistIndex(zl, -1);
        while (ziplistGet(p, &entry, &elen, &value)) {
            printf("Entry: ");
            if (entry) {
                if (elen && fwrite(entry,elen,1,stdout) == 0) perror("fwrite");
            } else {
                printf("%lld", value);
            }
            p = ziplistPrev(zl,p);
            printf("\n");
        }
        printf("\n");
    }

    printf("Iterate from back to front, deleting all items:\n");
    {
        zl = createList();
        p = ziplistIndex(zl, -1);
        while (ziplistGet(p, &entry, &elen, &value)) {
            printf("Entry: ");
            if (entry) {
                if (elen && fwrite(entry,elen,1,stdout) == 0) perror("fwrite");
            } else {
                printf("%lld", value);
            }
            zl = ziplistDelete(zl,&p);
            p = ziplistPrev(zl,p);
            printf("\n");
        }
        printf("\n");
    }

    printf("Delete inclusive range 0,0:\n");
    {
        zl = createList();
        zl = ziplistDeleteRange(zl, 0, 1);
        ziplistRepr(zl);
    }

    printf("Delete inclusive range 0,1:\n");
    {
        zl = createList();
        zl = ziplistDeleteRange(zl, 0, 2);
        ziplistRepr(zl);
    }

    printf("Delete inclusive range 1,2:\n");
    {
        zl = createList();
        zl = ziplistDeleteRange(zl, 1, 2);
        ziplistRepr(zl);
    }

    printf("Delete with start index out of range:\n");
    {
        zl = createList();
        zl = ziplistDeleteRange(zl, 5, 1);
        ziplistRepr(zl);
    }

    printf("Delete with num overflow:\n");
    {
        zl = createList();
        zl = ziplistDeleteRange(zl, 1, 5);
        ziplistRepr(zl);
    }

    printf("Delete foo while iterating:\n");
    {
        zl = createList();
        p = ziplistIndex(zl,0);
        while (ziplistGet(p,&entry,&elen,&value)) {
            if (entry && strncmp("foo",(char*)entry,elen) == 0) {
                printf("Delete foo\n");
                zl = ziplistDelete(zl,&p);
            } else {
                printf("Entry: ");
                if (entry) {
                    if (elen && fwrite(entry,elen,1,stdout) == 0)
                        perror("fwrite");
                } else {
                    printf("%lld",value);
                }
                p = ziplistNext(zl,p);
                printf("\n");
            }
        }
        printf("\n");
        ziplistRepr(zl);
    }

    printf("Regression test for >255 byte strings:\n");
    {
        char v1[257],v2[257];
        memset(v1,'x',256);
        memset(v2,'y',256);
        zl = ziplistNew();
        zl = ziplistPush(zl,(unsigned char*)v1,strlen(v1),ZIPLIST_TAIL);
        zl = ziplistPush(zl,(unsigned char*)v2,strlen(v2),ZIPLIST_TAIL);

        /* Pop values again and compare their value. */
        p = ziplistIndex(zl,0);
        assert(ziplistGet(p,&entry,&elen,&value));
        assert(strncmp(v1,(char*)entry,elen) == 0);
        p = ziplistIndex(zl,1);
        assert(ziplistGet(p,&entry,&elen,&value));
        assert(strncmp(v2,(char*)entry,elen) == 0);
        printf("SUCCESS\n\n");
    }

    printf("Regression test deleting next to last entries:\n");
    {
        char v[3][257];
        zlentry e[3];
        int i;

        for (i = 0; i < (sizeof(v)/sizeof(v[0])); i++) {
            memset(v[i], 'a' + i, sizeof(v[0]));
        }

        v[0][256] = '\0';
        v[1][  1] = '\0';
        v[2][256] = '\0';

        zl = ziplistNew();
        for (i = 0; i < (sizeof(v)/sizeof(v[0])); i++) {
            zl = ziplistPush(zl, (unsigned char *) v[i], strlen(v[i]), ZIPLIST_TAIL);
        }

        verify(zl, e);

        assert(e[0].prevrawlensize == 1);
        assert(e[1].prevrawlensize == 5);
        assert(e[2].prevrawlensize == 1);

        /* Deleting entry 1 will increase `prevrawlensize` for entry 2 */
        unsigned char *p = e[1].p;
        zl = ziplistDelete(zl, &p);

        verify(zl, e);

        assert(e[0].prevrawlensize == 1);
        assert(e[1].prevrawlensize == 5);

        printf("SUCCESS\n\n");
    }

    printf("Create long list and check indices:\n");
    {
        zl = ziplistNew();
        char buf[32];
        int i,len;
        for (i = 0; i < 1000; i++) {
            len = sprintf(buf,"%d",i);
            zl = ziplistPush(zl,(unsigned char*)buf,len,ZIPLIST_TAIL);
        }
        for (i = 0; i < 1000; i++) {
            p = ziplistIndex(zl,i);
            assert(ziplistGet(p,NULL,NULL,&value));
            assert(i == value);

            p = ziplistIndex(zl,-i-1);
            assert(ziplistGet(p,NULL,NULL,&value));
            assert(999-i == value);
        }
        printf("SUCCESS\n\n");
    }

    printf("Compare strings with ziplist entries:\n");
    {
        zl = createList();
        p = ziplistIndex(zl,0);
        if (!ziplistCompare(p,(unsigned char*)"hello",5)) {
            printf("ERROR: not \"hello\"\n");
            return 1;
        }
        if (ziplistCompare(p,(unsigned char*)"hella",5)) {
            printf("ERROR: \"hella\"\n");
            return 1;
        }

        p = ziplistIndex(zl,3);
        if (!ziplistCompare(p,(unsigned char*)"1024",4)) {
            printf("ERROR: not \"1024\"\n");
            return 1;
        }
        if (ziplistCompare(p,(unsigned char*)"1025",4)) {
            printf("ERROR: \"1025\"\n");
            return 1;
        }
        printf("SUCCESS\n\n");
    }

    printf("Stress with random payloads of different encoding:\n");
    {
        int i,j,len,where;
        unsigned char *p;
        char buf[1024];
        int buflen;
        list *ref;
        listNode *refnode;

        /* Hold temp vars from ziplist */
        unsigned char *sstr;
        unsigned int slen;
        long long sval;

        for (i = 0; i < 20000; i++) {
            zl = ziplistNew();
            ref = listCreate();
            listSetFreeMethod(ref,sdsfree);
            len = rand() % 256;

            /* Create lists */
            for (j = 0; j < len; j++) {
                where = (rand() & 1) ? ZIPLIST_HEAD : ZIPLIST_TAIL;
                if (rand() % 2) {
                    buflen = randstring(buf,1,sizeof(buf)-1);
                } else {
                    switch(rand() % 3) {
                    case 0:
                        buflen = sprintf(buf,"%lld",(0LL + rand()) >> 20);
                        break;
                    case 1:
                        buflen = sprintf(buf,"%lld",(0LL + rand()));
                        break;
                    case 2:
                        buflen = sprintf(buf,"%lld",(0LL + rand()) << 20);
                        break;
                    default:
                        assert(NULL);
                    }
                }

                /* Add to ziplist */
                zl = ziplistPush(zl, (unsigned char*)buf, buflen, where);

                /* Add to reference list */
                if (where == ZIPLIST_HEAD) {
                    listAddNodeHead(ref,sdsnewlen(buf, buflen));
                } else if (where == ZIPLIST_TAIL) {
                    listAddNodeTail(ref,sdsnewlen(buf, buflen));
                } else {
                    assert(NULL);
                }
            }

            assert(listLength(ref) == ziplistLen(zl));
            for (j = 0; j < len; j++) {
                /* Naive way to get elements, but similar to the stresser
                 * executed from the Tcl test suite. */
                p = ziplistIndex(zl,j);
                refnode = listIndex(ref,j);

                assert(ziplistGet(p,&sstr,&slen,&sval));
                if (sstr == NULL) {
                    buflen = sprintf(buf,"%lld",sval);
                } else {
                    buflen = slen;
                    memcpy(buf,sstr,buflen);
                    buf[buflen] = '\0';
                }
                assert(memcmp(buf,listNodeValue(refnode),buflen) == 0);
            }
            zfree(zl);
            listRelease(ref);
        }
        printf("SUCCESS\n\n");
    }

    printf("Stress with variable ziplist size:\n");
    {
        stress(ZIPLIST_HEAD,100000,16384,256);
        stress(ZIPLIST_TAIL,100000,16384,256);
    }

    return 0;
}

#endif
