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
#include "lzf.h"    /* LZF compression library */
#include "zipmap.h"
#include "endianconv.h"

#include <math.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <sys/param.h>

#define RDB_LOAD_NONE   0           //不编码，从rio读出
#define RDB_LOAD_ENC    (1<<0)
#define RDB_LOAD_PLAIN  (1<<1)

#define rdbExitReportCorruptRDB(...) rdbCheckThenExit(__LINE__,__VA_ARGS__)

extern int rdbCheckMode;    //在redis-check-rdb文件的一个标志，初始化为0
void rdbCheckError(const char *fmt, ...);
void rdbCheckSetError(const char *fmt, ...);

//检查rdb错误发送信息且退出
void rdbCheckThenExit(int linenum, char *reason, ...) {
    va_list ap;         // 可变参的宏va_list
    char msg[1024];
    int len;

    // 将错误信息写到msg缓冲区中
    len = snprintf(msg,sizeof(msg),
        "Internal error in RDB reading function at rdb.c:%d -> ", linenum);
    // 用reason初始化ap
    va_start(ap,reason);
    // 将ap指向的参数写到len长度的后面
    vsnprintf(msg+len,sizeof(msg)-len,reason,ap);
    // 结束关闭
    va_end(ap);

    //发送错误信息
    if (!rdbCheckMode) {
        serverLog(LL_WARNING, "%s", msg);
        char *argv[2] = {"",server.rdb_filename};
        redis_check_rdb_main(2,argv);
    } else {
        rdbCheckError("%s",msg);
    }
    exit(1);
}

// 将长度为len的数组p写到rdb中，返回写的长度
static int rdbWriteRaw(rio *rdb, void *p, size_t len) {
    if (rdb && rioWrite(rdb,p,len) == 0)
        return -1;
    return len;
}

// 将长度为1的type字符写到rdb中
int rdbSaveType(rio *rdb, unsigned char type) {
    return rdbWriteRaw(rdb,&type,1);
}

/* Load a "type" in RDB format, that is a one byte unsigned integer.
 * This function is not only used to load object types, but also special
 * "types" like the end-of-file type, the EXPIRE type, and so forth. */
// 从rdb中载入1字节的数据保存在type中，并返回其type
int rdbLoadType(rio *rdb) {
    unsigned char type;
    if (rioRead(rdb,&type,1) == 0) return -1;
    return type;
}

// 从rio读出一个时间，单位为秒，长度为4字节
time_t rdbLoadTime(rio *rdb) {
    int32_t t32;
    if (rioRead(rdb,&t32,4) == 0) return -1;
    return (time_t)t32;
}

// 写一个longlong类型的时间，单位为毫秒
int rdbSaveMillisecondTime(rio *rdb, long long t) {
    int64_t t64 = (int64_t) t;
    return rdbWriteRaw(rdb,&t64,8);
}

// 从rio中读出一个毫秒时间返回
long long rdbLoadMillisecondTime(rio *rdb) {
    int64_t t64;
    if (rioRead(rdb,&t64,8) == 0) return -1;
    return (long long)t64;
}

/* Saves an encoded length. The first two bits in the first byte are used to
 * hold the encoding type. See the RDB_* definitions for more information
 * on the types of encoding. */
// 将一个被编码的长度写入到rio中，返回保存编码后的len需要的字节数
int rdbSaveLen(rio *rdb, uint32_t len) {
    unsigned char buf[2];
    size_t nwritten;

    // 长度小于2^6
    if (len < (1<<6)) {
        /* Save a 6 bit len */
        // 高两位是00表示6位长，第六位表示len的值
        buf[0] = (len&0xFF)|(RDB_6BITLEN<<6);
        // 将buf[0]写到rio中
        if (rdbWriteRaw(rdb,buf,1) == -1) return -1;
        nwritten = 1;   //1字节

    // 长度小于2^14
    } else if (len < (1<<14)) {
        /* Save a 14 bit len */
        // 高两位是01表示14位长，剩下的14位表示len的值
        buf[0] = ((len>>8)&0xFF)|(RDB_14BITLEN<<6);
        buf[1] = len&0xFF;
        // 讲buf[0..1]写到rio中
        if (rdbWriteRaw(rdb,buf,2) == -1) return -1;
        nwritten = 2;   //2字节
    // 长度大于2^14
    } else {
        /* Save a 32 bit len */
        // 高两位为10表示32位长，剩下的6位不使用
        buf[0] = (RDB_32BITLEN<<6);
        // 将buf[0]写入
        if (rdbWriteRaw(rdb,buf,1) == -1) return -1;
        len = htonl(len);   //将len转换为网络序，写入rdb中
        if (rdbWriteRaw(rdb,&len,4) == -1) return -1;
        nwritten = 1+4; //5个字节
    }
    return nwritten;
}

/* Load an encoded length. The "isencoded" argument is set to 1 if the length
 * is not actually a length but an "encoding type". See the RDB_ENC_*
 * definitions in rdb.h for more information. */
// 返回一个从rio读出的len值，如果该len值不是整数，而是被编码后的值，那么将isencoded设置为1
uint32_t rdbLoadLen(rio *rdb, int *isencoded) {
    unsigned char buf[2];
    uint32_t len;
    int type;

    // 默认为没有编码
    if (isencoded) *isencoded = 0;
    // 将rio中的值读到buf中
    if (rioRead(rdb,buf,1) == 0) return RDB_LENERR;

    // (buf[0]&0xC0)>>6 = (1100 000 & buf[0]) >> 6 = buf[0]的最高两位
    type = (buf[0]&0xC0)>>6;

    // 一个编码过的值，返回解码值，设置编码标志
    if (type == RDB_ENCVAL) {
        /* Read a 6 bit encoding type. */
        if (isencoded) *isencoded = 1;
        return buf[0]&0x3F; //取出剩下六位表示的长度值

    // 一个6位长的值
    } else if (type == RDB_6BITLEN) {
        /* Read a 6 bit len. */
        return buf[0]&0x3F; //取出剩下六位表示的长度值

    // 一个14位长的值
    } else if (type == RDB_14BITLEN) {
        /* Read a 14 bit len. */
        // 从buf+1读出1个字节的值
        if (rioRead(rdb,buf+1,1) == 0) return RDB_LENERR;
        return ((buf[0]&0x3F)<<8)|buf[1];   //取出除最高两位的长度值

    // 一个32位长的值
    } else if (type == RDB_32BITLEN) {
        /* Read a 32 bit len. */
        // 读出4个字节的值
        if (rioRead(rdb,&len,4) == 0) return RDB_LENERR;
        return ntohl(len);  //转换为主机序的值
    } else {
        rdbExitReportCorruptRDB(
            "Unknown length encoding %d in rdbLoadLen()",type);
        return -1; /* Never reached. */
    }
}

/* Encodes the "value" argument as integer when it fits in the supported ranges
 * for encoded types. If the function successfully encodes the integer, the
 * representation is stored in the buffer pointer to by "enc" and the string
 * length is returned. Otherwise 0 is returned. */
// 将longlong类型的value编码成一个整数编码，如果可以编码，将编码后的值保存在enc中，返回编码后的字节数
int rdbEncodeInteger(long long value, unsigned char *enc) {
    // -2^8 <= value <= 2^8-1
    // 最高两位设置为 11，表示是一个编码过的值，低6位为 000000 ，表示是 RDB_ENC_INT8 编码格式
    // 剩下的1个字节保存value，返回2字节
    if (value >= -(1<<7) && value <= (1<<7)-1) {
        enc[0] = (RDB_ENCVAL<<6)|RDB_ENC_INT8;
        enc[1] = value&0xFF;
        return 2;

    // -2^16 <= value <= 2^16-1
    // 最高两位设置为 11，表示是一个编码过的值，低6位为 000001 ，表示是 RDB_ENC_INT16 编码格式
    // 剩下的2个字节保存value，返回3字节
    } else if (value >= -(1<<15) && value <= (1<<15)-1) {
        enc[0] = (RDB_ENCVAL<<6)|RDB_ENC_INT16;
        enc[1] = value&0xFF;
        enc[2] = (value>>8)&0xFF;
        return 3;
    // -2^32 <= value <= 2^32-1
    // 最高两位设置为 11，表示是一个编码过的值，低6位为 000010 ，表示是 RDB_ENC_INT32 编码格式
    // 剩下的4个字节保存value，返回5字节
    } else if (value >= -((long long)1<<31) && value <= ((long long)1<<31)-1) {
        enc[0] = (RDB_ENCVAL<<6)|RDB_ENC_INT32;
        enc[1] = value&0xFF;
        enc[2] = (value>>8)&0xFF;
        enc[3] = (value>>16)&0xFF;
        enc[4] = (value>>24)&0xFF;
        return 5;
    } else {
        return 0;
    }
}

/* Loads an integer-encoded object with the specified encoding type "enctype".
 * The returned value changes according to the flags, see
 * rdbGenerincLoadStringObject() for more info. */
// 将rio中的整数值根据不同的编码读出来，并根据flags构建成一个不同类型的值并返回
void *rdbLoadIntegerObject(rio *rdb, int enctype, int flags) {
    int plain = flags & RDB_LOAD_PLAIN; //无格式
    int encode = flags & RDB_LOAD_ENC;  //字符串对象
    unsigned char enc[4];
    long long val;

    // 根据不同的整数编码类型，从rio中读出整数值到enc中
    if (enctype == RDB_ENC_INT8) {
        if (rioRead(rdb,enc,1) == 0) return NULL;
        val = (signed char)enc[0];
    } else if (enctype == RDB_ENC_INT16) {
        uint16_t v;
        if (rioRead(rdb,enc,2) == 0) return NULL;
        v = enc[0]|(enc[1]<<8);
        val = (int16_t)v;
    } else if (enctype == RDB_ENC_INT32) {
        uint32_t v;
        if (rioRead(rdb,enc,4) == 0) return NULL;
        v = enc[0]|(enc[1]<<8)|(enc[2]<<16)|(enc[3]<<24);
        val = (int32_t)v;
    } else {
        val = 0; /* anti-warning */
        rdbExitReportCorruptRDB("Unknown RDB integer encoding type %d",enctype);
    }

    // 如果是整数，转换为字符串类型返回
    if (plain) {
        char buf[LONG_STR_SIZE], *p;
        int len = ll2string(buf,sizeof(buf),val);
        p = zmalloc(len);
        memcpy(p,buf,len);
        return p;
    // 如果是编码过的整数值，则转换为字符串对象，返回
    } else if (encode) {
        return createStringObjectFromLongLong(val);
    } else {
    // 返回一个字符串对象
        return createObject(OBJ_STRING,sdsfromlonglong(val));
    }
}

/* String objects in the form "2391" "-100" without any space and with a
 * range of values that can fit in an 8, 16 or 32 bit signed value can be
 * encoded as integers to save space */
// 将一些纯数字的字符串尝试转换为可以编码的整数，以节省内存
int rdbTryIntegerEncoding(char *s, size_t len, unsigned char *enc) {
    long long value;
    char *endptr, buf[32];

    /* Check if it's possible to encode this value as a number */
    // 尝试将字符串值转换为整数
    value = strtoll(s, &endptr, 10);
    // 字符串不是纯数字的返回0
    if (endptr[0] != '\0') return 0;
    // 将value转回字符串类型
    ll2string(buf,32,value);

    /* If the number converted back into a string is not identical
     * then it's not possible to encode the string as integer */
    // 比较转换前后的字符串，如果不相等，则返回0
    if (strlen(buf) != len || memcmp(buf,s,len)) return 0;

    // 可以编码则转换成整数，将编码类型保存enc中
    return rdbEncodeInteger(value,enc);
}

// 将讲一个LZF压缩过的字符串的信息写入rio，返回写入的字节数
ssize_t rdbSaveLzfBlob(rio *rdb, void *data, size_t compress_len,
                       size_t original_len) {
    unsigned char byte;
    ssize_t n, nwritten = 0;

    /* Data compressed! Let's save it on disk */
    // 将1100 0011保存在byte中，表示编码过，是一个LZF压缩的字符串
    byte = (RDB_ENCVAL<<6)|RDB_ENC_LZF;
    // 将byte写入rio中
    if ((n = rdbWriteRaw(rdb,&byte,1)) == -1) goto writeerr;
    nwritten += n;

    // 将压缩后的长度值compress_len写入rio
    if ((n = rdbSaveLen(rdb,compress_len)) == -1) goto writeerr;
    nwritten += n;

    // 将压缩前的长度值original_len写入rio
    if ((n = rdbSaveLen(rdb,original_len)) == -1) goto writeerr;
    nwritten += n;

    // 将压缩的字符串值data写入rio
    if ((n = rdbWriteRaw(rdb,data,compress_len)) == -1) goto writeerr;
    nwritten += n;

    return nwritten;    //返回写入的字节数

writeerr:
    return -1;
}

ssize_t rdbSaveLzfStringObject(rio *rdb, unsigned char *s, size_t len) {
    size_t comprlen, outlen;
    void *out;

    /* We require at least four bytes compression for this to be worth it */
    if (len <= 4) return 0;
    outlen = len-4;
    if ((out = zmalloc(outlen+1)) == NULL) return 0;
    comprlen = lzf_compress(s, len, out, outlen);
    if (comprlen == 0) {
        zfree(out);
        return 0;
    }
    ssize_t nwritten = rdbSaveLzfBlob(rdb, out, comprlen, len);
    zfree(out);
    return nwritten;
}

/* Load an LZF compressed string in RDB format. The returned value
 * changes according to 'flags'. For more info check the
 * rdbGenericLoadStringObject() function. */
// 从rio中读出一个压缩过的字符串，将其解压并返回构建成的字符串对象
void *rdbLoadLzfStringObject(rio *rdb, int flags) {
    int plain = flags & RDB_LOAD_PLAIN; //无格式的，没有编码过
    unsigned int len, clen;
    unsigned char *c = NULL;
    sds val = NULL;

    // 读出clen值，表示压缩的长度
    if ((clen = rdbLoadLen(rdb,NULL)) == RDB_LENERR) return NULL;
    // 读出len值，表示未压缩的长度
    if ((len = rdbLoadLen(rdb,NULL)) == RDB_LENERR) return NULL;
    // 分配一个clen长度的空间
    if ((c = zmalloc(clen)) == NULL) goto err;

    /* Allocate our target according to the uncompressed size. */
    // 如果是一个未编码过的值
    if (plain) {
        val = zmalloc(len); //分配一个未编码值的空间
    } else {
        // 否则分配一个字符串空间
        if ((val = sdsnewlen(NULL,len)) == NULL) goto err;
    }

    /* Load the compressed representation and uncompress it to target. */
    // 从rio中读出压缩过的值
    if (rioRead(rdb,c,clen) == 0) goto err;
    // 将压缩过的值解压缩，保存在val中，解压后的长度为len
    if (lzf_decompress(c,clen,val,len) == 0) {
        if (rdbCheckMode) rdbCheckSetError("Invalid LZF compressed string");
        goto err;
    }
    zfree(c);   //释放空间

    if (plain)
        return val; //返回原值
    else
        return createObject(OBJ_STRING,val);    //返回字一个字符串对象
err:
    zfree(c);
    if (plain)
        zfree(val);
    else
        sdsfree(val);
    return NULL;
}

/* Save a string object as [len][data] on disk. If the object is a string
 * representation of an integer value we try to save it in a special form */
// 将一个原生的字符串值写入到rio
ssize_t rdbSaveRawString(rio *rdb, unsigned char *s, size_t len) {
    int enclen;
    ssize_t n, nwritten = 0;

    /* Try integer encoding */
    // 如果字符串可以进行整数编码
    if (len <= 11) {
        unsigned char buf[5];
        if ((enclen = rdbTryIntegerEncoding((char*)s,len,buf)) > 0) {
            // 将编码后的字符串写入rio，返回编码所需的字节数
            if (rdbWriteRaw(rdb,buf,enclen) == -1) return -1;
            return enclen;
        }
    }

    /* Try LZF compression - under 20 bytes it's unable to compress even
     * aaaaaaaaaaaaaaaaaa so skip it */
    // 如果开启了rdb压缩的设置，且字符串长度大于20，进行LZF压缩字符串
    if (server.rdb_compression && len > 20) {
        n = rdbSaveLzfStringObject(rdb,s,len);
        if (n == -1) return -1;
        if (n > 0) return n;
        /* Return value of 0 means data can't be compressed, save the old way */
    }

    /* Store verbatim */
    // 字符串既不能被压缩，也不能编码成整数
    // 因此直接写入rio中
    // 写入长度
    if ((n = rdbSaveLen(rdb,len)) == -1) return -1;
    nwritten += n;
    if (len > 0) {
        // 写入字符串
        if (rdbWriteRaw(rdb,s,len) == -1) return -1;
        nwritten += len;
    }
    return nwritten;    //返回写入的字节数
}

/* Save a long long value as either an encoded string or a string. */
// 将 longlong类型的value转换为字符串对象，并且进行编码，然后写到rio中
ssize_t rdbSaveLongLongAsStringObject(rio *rdb, long long value) {
    unsigned char buf[32];
    ssize_t n, nwritten = 0;
    // 将longlong类型value进行整数编码并将值写到buf中，节约内存
    int enclen = rdbEncodeInteger(value,buf);
    // 如果可以进行整数编码
    if (enclen > 0) {
        // 将整数编码后的longlong值写到rio中
        return rdbWriteRaw(rdb,buf,enclen);

    // 不能进行整数编码
    } else {
        /* Encode as string */
        // 转换为字符串
        enclen = ll2string((char*)buf,32,value);
        serverAssert(enclen < 32);
        // 将字符串长度写入rio中
        if ((n = rdbSaveLen(rdb,enclen)) == -1) return -1;
        nwritten += n;
        // 将字符串写入rio中
        if ((n = rdbWriteRaw(rdb,buf,enclen)) == -1) return -1;
        nwritten += n;
    }
    return nwritten;    //发送写入的长度
}

/* Like rdbSaveStringObjectRaw() but handle encoded objects */
// 将字符串对象obj写到rio中
int rdbSaveStringObject(rio *rdb, robj *obj) {
    /* Avoid to decode the object, then encode it again, if the
     * object is already integer encoded. */
    // 如果是int编码的是字符串对象
    if (obj->encoding == OBJ_ENCODING_INT) {
        // 讲对象值进行编码后发送给rio
        return rdbSaveLongLongAsStringObject(rdb,(long)obj->ptr);

    // RAW或EMBSTR编码类型的字符串对象
    } else {
        serverAssertWithInfo(NULL,obj,sdsEncodedObject(obj));
        // 将字符串类型的对象写到rio
        return rdbSaveRawString(rdb,obj->ptr,sdslen(obj->ptr));
    }
}

/* Load a string object from an RDB file according to flags:
 *
 * RDB_LOAD_NONE (no flags): load an RDB object, unencoded.
 * RDB_LOAD_ENC: If the returned type is a Redis object, try to
 *               encode it in a special way to be more memory
 *               efficient. When this flag is passed the function
 *               no longer guarantees that obj->ptr is an SDS string.
 * RDB_LOAD_PLAIN: Return a plain string allocated with zmalloc()
 *                 instead of a Redis object with an sds in it.
 * RDB_LOAD_SDS: Return an SDS string instead of a Redis object.
 */
//RDB_LOAD_NONE：读一个rio，不编码
//
// 根据flags，将从rio读出一个字符串对象进行编码
void *rdbGenericLoadStringObject(rio *rdb, int flags) {
    int encode = flags & RDB_LOAD_ENC;  //编码
    int plain = flags & RDB_LOAD_PLAIN; //原生的值
    int isencoded;
    uint32_t len;

    // 从rio中读出一个字符串对象，编码类型保存在isencoded中，所需的字节为len
    len = rdbLoadLen(rdb,&isencoded);
    // 如果读出的对象被编码(isencoded被设置为1)，则根据不同的长度值len映射到不同的整数编码
    if (isencoded) {
        switch(len) {
        case RDB_ENC_INT8:
        case RDB_ENC_INT16:
        case RDB_ENC_INT32:
            // 以上三种类型的整数编码，根据flags返回不同类型值
            return rdbLoadIntegerObject(rdb,len,flags);
        case RDB_ENC_LZF:
            // 如果是压缩后的字符串，进行构建压缩字符串编码对象
            return rdbLoadLzfStringObject(rdb,flags);
        default:
            rdbExitReportCorruptRDB("Unknown RDB string encoding type %d",len);
        }
    }

    // 如果len值错误，则返回NULL
    if (len == RDB_LENERR) return NULL;

    // 如果不是原生值
    if (!plain) {
        // 根据encode编码类型创建不同的字符串对象
        robj *o = encode ? createStringObject(NULL,len) :
                           createRawStringObject(NULL,len);
        // 设置o对象的值，从rio中读出来，如果失败，释放对象返回NULL
        if (len && rioRead(rdb,o->ptr,len) == 0) {
            decrRefCount(o);
            return NULL;
        }
        return o;
    // 如果设置了原生值
    } else {
        // 分配空间
        void *buf = zmalloc(len);
        // 从rio中读出来
        if (len && rioRead(rdb,buf,len) == 0) {
            zfree(buf);
            return NULL;
        }
        return buf; //返回
    }
}

// 从rio中读出一个字符串编码的对象
robj *rdbLoadStringObject(rio *rdb) {
    return rdbGenericLoadStringObject(rdb,RDB_LOAD_NONE);
}

// 从rio中读出一个字符串编码的对象，对象使用不同类型的编码
robj *rdbLoadEncodedStringObject(rio *rdb) {
    return rdbGenericLoadStringObject(rdb,RDB_LOAD_ENC);
}

/* Save a double value. Doubles are saved as strings prefixed by an unsigned
 * 8 bit integer specifying the length of the representation.
 * This 8 bit integer has special values in order to specify the following
 * conditions:
 * 253: not a number
 * 254: + inf
 * 255: - inf
 */
// 写入一个double类型的字符串值，字符串前是一个8位长的无符号整数，它表示浮点数的长度
// 八位整数中的值表示一些特殊情况，
// 253：表示不是数字
// 254：表示正无穷
// 255：表示负无穷
int rdbSaveDoubleValue(rio *rdb, double val) {
    unsigned char buf[128];
    int len;

    // 如果val不是一个数字，则写入253
    if (isnan(val)) {
        buf[0] = 253;
        len = 1;
    // 如果val不是一个有限的值，根据正负性，写入255或254
    } else if (!isfinite(val)) {
        len = 1;
        buf[0] = (val < 0) ? 255 : 254;

    // 如果不是上面的两种情况，则表示val是一个double类型的数
    } else {

    // 64位系统中
    // DBL_MANT_DIG：表示尾数中的位数为53
    // LLONG_MAX：longlong表示的最大数为0x7fffffffffffffffLL
#if (DBL_MANT_DIG >= 52) && (LLONG_MAX == 0x7fffffffffffffffLL)
        /* Check if the float is in a safe range to be casted into a
         * long long. We are assuming that long long is 64 bit here.
         * Also we are assuming that there are no implementations around where
         * double has precision < 52 bit.
         *
         * Under this assumptions we test if a double is inside an interval
         * where casting to long long is safe. Then using two castings we
         * make sure the decimal part is zero. If all this is true we use
         * integer printing function that is much faster. */
        // double类型的最大值和最小值
        double min = -4503599627370495; /* (2^52)-1 */
        double max = 4503599627370496; /* -(2^52) */
        // 如果val在double表示的范围内，且val值是安全的，没有小数
        if (val > min && val < max && val == ((double)((long long)val)))
            // 将val转换为字符串，添加到8位长度值的后面
            ll2string((char*)buf+1,sizeof(buf)-1,(long long)val);
        else
#endif
            // 以宽度为17位的方式写到8位长度值的后面，17位的double双精度浮点数的长度最短且无损
            snprintf((char*)buf+1,sizeof(buf)-1,"%.17g",val);
        buf[0] = strlen((char*)buf+1);  //将刚才加入buf+1的字符串值的长度写到前8位的长度值中
        len = buf[0]+1; //总字节数
    }
    // 将buf中的字符串原生的写到rio中
    return rdbWriteRaw(rdb,buf,len);
}

/* For information about double serialization check rdbSaveDoubleValue() */
// 读出字符串表示的double值
int rdbLoadDoubleValue(rio *rdb, double *val) {
    char buf[256];
    unsigned char len;

    // 从rio中读出一个字节的长度，保存在len中
    if (rioRead(rdb,&len,1) == 0) return -1;

    // Redis中，0，负无穷，正无穷，非数字分别如下表示，在redis.c中
    // static double R_Zero, R_PosInf, R_NegInf, R_Nan;
    // R_Zero = 0.0;
    // R_PosInf = 1.0/R_Zero;
    // R_NegInf = -1.0/R_Zero;
    // R_Nan = R_Zero/R_Zero;
    // 如果长度值为这三个特殊值，返回0
    switch(len) {
    case 255: *val = R_NegInf; return 0;
    case 254: *val = R_PosInf; return 0;
    case 253: *val = R_Nan; return 0;

    // 否则从rio读出len长的字符串
    default:
        if (rioRead(rdb,buf,len) == 0) return -1;
        buf[len] = '\0';
        sscanf(buf, "%lg", val);    //将buf值写到val中
        return 0;
    }
}

/* Save the object type of object "o". */
// 将对象o的类型写到rio中
int rdbSaveObjectType(rio *rdb, robj *o) {
    // 根据不同数据类型，写入不同编码类型
    switch (o->type) {
    case OBJ_STRING:    //字符串类型
        return rdbSaveType(rdb,RDB_TYPE_STRING);
    case OBJ_LIST:      //列表类型
        if (o->encoding == OBJ_ENCODING_QUICKLIST)
            return rdbSaveType(rdb,RDB_TYPE_LIST_QUICKLIST);
        else
            serverPanic("Unknown list encoding");
    case OBJ_SET:       //集合类型
        if (o->encoding == OBJ_ENCODING_INTSET)
            return rdbSaveType(rdb,RDB_TYPE_SET_INTSET);
        else if (o->encoding == OBJ_ENCODING_HT)
            return rdbSaveType(rdb,RDB_TYPE_SET);
        else
            serverPanic("Unknown set encoding");
    case OBJ_ZSET:      //有序集合类型
        if (o->encoding == OBJ_ENCODING_ZIPLIST)
            return rdbSaveType(rdb,RDB_TYPE_ZSET_ZIPLIST);
        else if (o->encoding == OBJ_ENCODING_SKIPLIST)
            return rdbSaveType(rdb,RDB_TYPE_ZSET);
        else
            serverPanic("Unknown sorted set encoding");
    case OBJ_HASH:      //哈希类型
        if (o->encoding == OBJ_ENCODING_ZIPLIST)
            return rdbSaveType(rdb,RDB_TYPE_HASH_ZIPLIST);
        else if (o->encoding == OBJ_ENCODING_HT)
            return rdbSaveType(rdb,RDB_TYPE_HASH);
        else
            serverPanic("Unknown hash encoding");
    default:
        serverPanic("Unknown object type");
    }
    return -1; /* avoid warning */
}

/* Use rdbLoadType() to load a TYPE in RDB format, but returns -1 if the
 * type is not specifically a valid Object Type. */
// 从rio中读出一个类型并返回
int rdbLoadObjectType(rio *rdb) {
    int type;
    // 从rio中读出类型保存在type中
    if ((type = rdbLoadType(rdb)) == -1) return -1;
    // 判断是否是一个rio规定的类型
    if (!rdbIsObjectType(type)) return -1;
    return type;
}

/* Save a Redis object. Returns -1 on error, number of bytes written on success. */
// 将一个对象写到rio中，出错返回-1，成功返回写的字节数
ssize_t rdbSaveObject(rio *rdb, robj *o) {
    ssize_t n = 0, nwritten = 0;

    // 保存字符串对象
    if (o->type == OBJ_STRING) {
        /* Save a string value */
        if ((n = rdbSaveStringObject(rdb,o)) == -1) return -1;
        nwritten += n;
    // 保存一个列表对象
    } else if (o->type == OBJ_LIST) {
        /* Save a list value */
        // 列表对象编码为quicklist
        if (o->encoding == OBJ_ENCODING_QUICKLIST) {
            quicklist *ql = o->ptr;         //表头地址
            quicklistNode *node = ql->head; //头节点地址
            // 将quicklist的节点个数写入rio中
            if ((n = rdbSaveLen(rdb,ql->len)) == -1) return -1;
            nwritten += n;  //更新写入的字节数

            do {
                // 如果当前节点可以被压缩
                if (quicklistNodeIsCompressed(node)) {
                    void *data;         //将节点的数据压缩到data中
                    size_t compress_len = quicklistGetLzf(node, &data);
                    // 将一个压缩过的字符串写到rio中
                    if ((n = rdbSaveLzfBlob(rdb,data,compress_len,node->sz)) == -1) return -1;
                    nwritten += n;
                // 如果不能压缩
                } else {
                    // 将一个原生的字符串写入到rio中
                    if ((n = rdbSaveRawString(rdb,node->zl,node->sz)) == -1) return -1;
                    nwritten += n;
                }//遍历所有的quicklist节点
            } while ((node = node->next));
        } else {
            serverPanic("Unknown list encoding");
        }
    // 保存一个集合对象
    } else if (o->type == OBJ_SET) {
        /* Save a set value */
        // 集合对象是字典类型
        if (o->encoding == OBJ_ENCODING_HT) {
            dict *set = o->ptr;
            dictIterator *di = dictGetIterator(set);    //创建一个字典迭代器
            dictEntry *de;
            // 将成员的个数写入rio中
            if ((n = rdbSaveLen(rdb,dictSize(set))) == -1) return -1;
            nwritten += n;

            // 遍历集合成员
            while((de = dictNext(di)) != NULL) {
                robj *eleobj = dictGetKey(de);
                // 将当前节点保存的键对象写入rio中
                if ((n = rdbSaveStringObject(rdb,eleobj)) == -1) return -1;
                nwritten += n;
            }
            dictReleaseIterator(di);                    //释放字典迭代器
        // 集合对象是字intset类型
        } else if (o->encoding == OBJ_ENCODING_INTSET) {
            size_t l = intsetBlobLen((intset*)o->ptr);  //获取intset所占的字节数
            // 以一个原生字符串对象的方式将intset集合写到rio中
            if ((n = rdbSaveRawString(rdb,o->ptr,l)) == -1) return -1;
            nwritten += n;
        } else {
            serverPanic("Unknown set encoding");
        }
    // 保存一个有序集合对象
    } else if (o->type == OBJ_ZSET) {
        /* Save a sorted set value */
        // 有序集合对象是ziplist类型
        if (o->encoding == OBJ_ENCODING_ZIPLIST) {
            size_t l = ziplistBlobLen((unsigned char*)o->ptr);  //ziplist所占的字节数
            // 以一个原生字符串对象保存ziplist类型的有序集合
            if ((n = rdbSaveRawString(rdb,o->ptr,l)) == -1) return -1;
            nwritten += n;
        // 有序集合对象是skiplist类型
        } else if (o->encoding == OBJ_ENCODING_SKIPLIST) {
            zset *zs = o->ptr;
            dictIterator *di = dictGetIterator(zs->dict);   //创建字典迭代器
            dictEntry *de;
            // 将有序集合的节点个数写入rio中
            if ((n = rdbSaveLen(rdb,dictSize(zs->dict))) == -1) return -1;
            nwritten += n;
            // 遍历所有节点
            while((de = dictNext(di)) != NULL) {
                robj *eleobj = dictGetKey(de);  //获取当前节点保存的键
                double *score = dictGetVal(de); //键对应的值
                // 以字符串对象的形式将键对象写到rio中
                if ((n = rdbSaveStringObject(rdb,eleobj)) == -1) return -1;
                nwritten += n;
                // 将double值转换为字符串对象，写到rio中
                if ((n = rdbSaveDoubleValue(rdb,*score)) == -1) return -1;
                nwritten += n;
            }
            dictReleaseIterator(di);    //释放字典迭代器
        } else {
            serverPanic("Unknown sorted set encoding");
        }
    // 保存一个哈希对象
    } else if (o->type == OBJ_HASH) {
        /* Save a hash value */
        // 哈希对象是ziplist类型的
        if (o->encoding == OBJ_ENCODING_ZIPLIST) {
            size_t l = ziplistBlobLen((unsigned char*)o->ptr);  //ziplist所占的字节数

            // 以一个原生字符串对象保存ziplist类型的有序集合
            if ((n = rdbSaveRawString(rdb,o->ptr,l)) == -1) return -1;
            nwritten += n;
        // 哈希对象是字典类型的
        } else if (o->encoding == OBJ_ENCODING_HT) {
            dictIterator *di = dictGetIterator(o->ptr); //创建字典迭代器
            dictEntry *de;

            // 将哈希表的节点个数写入rio中
            if ((n = rdbSaveLen(rdb,dictSize((dict*)o->ptr))) == -1) return -1;
            nwritten += n;

            // 遍历整个哈希表
            while((de = dictNext(di)) != NULL) {
                robj *key = dictGetKey(de); // 获取当前节点保存的键
                robj *val = dictGetVal(de); // 键对应的值

                // 以字符串对象的形式将键对象和值对象写到rio中
                if ((n = rdbSaveStringObject(rdb,key)) == -1) return -1;
                nwritten += n;
                if ((n = rdbSaveStringObject(rdb,val)) == -1) return -1;
                nwritten += n;
            }
            dictReleaseIterator(di);

        } else {
            serverPanic("Unknown hash encoding");
        }

    } else {
        serverPanic("Unknown object type");
    }
    return nwritten;    //返回写入的字节数
}

/* Return the length the object will have on disk if saved with
 * the rdbSaveObject() function. Currently we use a trick to get
 * this length with very little changes to the code. In the future
 * we could switch to a faster solution. */
// 返回一个对象的长度，通过写入的方式
// 但是已经被放弃使用
size_t rdbSavedObjectLen(robj *o) {
    ssize_t len = rdbSaveObject(NULL,o);
    serverAssertWithInfo(NULL,o,len != -1);
    return len;
}

/* Save a key-value pair, with expire time, type, key, value.
 * On error -1 is returned.
 * On success if the key was actually saved 1 is returned, otherwise 0
 * is returned (the key was already expired). */
// 将一个键对象，值对象，过期时间，和类型写入到rio中，出错返回-1，成功返回1，键过期返回0
int rdbSaveKeyValuePair(rio *rdb, robj *key, robj *val,
                        long long expiretime, long long now)
{
    /* Save the expire time */
    // 保存过期时间
    if (expiretime != -1) {
        /* If this key is already expired skip it */
        // 判断键是否过期，过期则返回0
        if (expiretime < now) return 0;
        // 将一个毫秒的过期时间类型写入rio
        if (rdbSaveType(rdb,RDB_OPCODE_EXPIRETIME_MS) == -1) return -1;
        // 将过期时间写入rio
        if (rdbSaveMillisecondTime(rdb,expiretime) == -1) return -1;
    }

    /* Save type, key, value */
    // 将值对象的类型，键对象和值对象到rio中
    if (rdbSaveObjectType(rdb,val) == -1) return -1;
    if (rdbSaveStringObject(rdb,key) == -1) return -1;
    if (rdbSaveObject(rdb,val) == -1) return -1;
    return 1;
}

/* Save an AUX field. */
// 写入一个特殊的辅助操作码字段
int rdbSaveAuxField(rio *rdb, void *key, size_t keylen, void *val, size_t vallen) {
    // RDB_OPCODE_AUX 对应的操作码是250
    if (rdbSaveType(rdb,RDB_OPCODE_AUX) == -1) return -1;
    // 写入键对象和值对象
    if (rdbSaveRawString(rdb,key,keylen) == -1) return -1;
    if (rdbSaveRawString(rdb,val,vallen) == -1) return -1;
    return 1;
}

/* Wrapper for rdbSaveAuxField() used when key/val length can be obtained
 * with strlen(). */
// rdbSaveAuxField()的封装，适用于key和val是c语言字符串类型
int rdbSaveAuxFieldStrStr(rio *rdb, char *key, char *val) {
    return rdbSaveAuxField(rdb,key,strlen(key),val,strlen(val));
}

/* Wrapper for strlen(key) + integer type (up to long long range). */
// rdbSaveAuxField()的封装，适用于key是c语言类型字符串，val是一个longlong类型的整数
int rdbSaveAuxFieldStrInt(rio *rdb, char *key, long long val) {
    char buf[LONG_STR_SIZE];
    int vlen = ll2string(buf,sizeof(buf),val);
    return rdbSaveAuxField(rdb,key,strlen(key),buf,vlen);
}

/* Save a few default AUX fields with information about the RDB generated. */
// 将一个rdb文件的默认信息写入到rio中
int rdbSaveInfoAuxFields(rio *rdb) {
    // 判断主机的总线宽度，是64位还是32位
    int redis_bits = (sizeof(void*) == 8) ? 64 : 32;

    /* Add a few fields about the state when the RDB was created. */
    // 添加rdb文件的状态信息：Redis版本，redis位数，当前时间和Redis当前使用的内存数
    if (rdbSaveAuxFieldStrStr(rdb,"redis-ver",REDIS_VERSION) == -1) return -1;
    if (rdbSaveAuxFieldStrInt(rdb,"redis-bits",redis_bits) == -1) return -1;
    if (rdbSaveAuxFieldStrInt(rdb,"ctime",time(NULL)) == -1) return -1;
    if (rdbSaveAuxFieldStrInt(rdb,"used-mem",zmalloc_used_memory()) == -1) return -1;
    return 1;
}

/* Produces a dump of the database in RDB format sending it to the specified
 * Redis I/O channel. On success C_OK is returned, otherwise C_ERR
 * is returned and part of the output, or all the output, can be
 * missing because of I/O errors.
 *
 * When the function returns C_ERR and if 'error' is not NULL, the
 * integer pointed by 'error' is set to the value of errno just after the I/O
 * error. */
// 将一个RDB格式文件内容写入到rio中，成功返回C_OK，否则C_ERR和一部分或所有的出错信息
// 当函数返回C_ERR，并且error不是NULL，那么error被设置为一个错误码errno
int rdbSaveRio(rio *rdb, int *error) {
    dictIterator *di = NULL;
    dictEntry *de;
    char magic[10];
    int j;
    long long now = mstime();
    uint64_t cksum;

    // 开启了校验和选项
    if (server.rdb_checksum)
        // 设置校验和的函数
        rdb->update_cksum = rioGenericUpdateChecksum;
    // 将Redis版本信息保存到magic中
    snprintf(magic,sizeof(magic),"REDIS%04d",RDB_VERSION);
    // 将magic写到rio中
    if (rdbWriteRaw(rdb,magic,9) == -1) goto werr;
    // 将rdb文件的默认信息写到rio中
    if (rdbSaveInfoAuxFields(rdb) == -1) goto werr;

    // 遍历所有服务器内的数据库
    for (j = 0; j < server.dbnum; j++) {
        redisDb *db = server.db+j;      //当前的数据库指针
        dict *d = db->dict;             //当数据库的键值对字典
        // 跳过为空的数据库
        if (dictSize(d) == 0) continue;
        // 创建一个字典类型的迭代器
        di = dictGetSafeIterator(d);
        if (!di) return C_ERR;

        /* Write the SELECT DB opcode */
        // 写入数据库的选择标识码 RDB_OPCODE_SELECTDB为254
        if (rdbSaveType(rdb,RDB_OPCODE_SELECTDB) == -1) goto werr;
        // 写入数据库的id，占了一个字节的长度
        if (rdbSaveLen(rdb,j) == -1) goto werr;

        /* Write the RESIZE DB opcode. We trim the size to UINT32_MAX, which
         * is currently the largest type we are able to represent in RDB sizes.
         * However this does not limit the actual size of the DB to load since
         * these sizes are just hints to resize the hash tables. */
        // 写入调整数据库的操作码，我们将大小限制在UINT32_MAX以内，这并不代表数据库的实际大小，只是提示去重新调整哈希表的大小
        uint32_t db_size, expires_size;
        // 如果字典的大小大于UINT32_MAX，则设置db_size为最大的UINT32_MAX
        db_size = (dictSize(db->dict) <= UINT32_MAX) ?
                                dictSize(db->dict) :
                                UINT32_MAX;
        // 设置有过期时间键的大小超过UINT32_MAX，则设置expires_size为最大的UINT32_MAX
        expires_size = (dictSize(db->expires) <= UINT32_MAX) ?
                                dictSize(db->expires) :
                                UINT32_MAX;
        // 写入调整哈希表大小的操作码，RDB_OPCODE_RESIZEDB = 251
        if (rdbSaveType(rdb,RDB_OPCODE_RESIZEDB) == -1) goto werr;
        // 写入提示调整哈希表大小的两个值，如果
        if (rdbSaveLen(rdb,db_size) == -1) goto werr;
        if (rdbSaveLen(rdb,expires_size) == -1) goto werr;

        /* Iterate this DB writing every entry */
        // 遍历数据库所有的键值对
        while((de = dictNext(di)) != NULL) {
            sds keystr = dictGetKey(de);        //当前键
            robj key, *o = dictGetVal(de);      //当前键的值
            long long expire;

            // 在栈中创建一个键对象并初始化
            initStaticStringObject(key,keystr);
            // 当前键的过期时间
            expire = getExpire(db,&key);
            // 将键的键对象，值对象，过期时间写到rio中
            if (rdbSaveKeyValuePair(rdb,&key,o,expire,now) == -1) goto werr;
        }
        dictReleaseIterator(di);    //释放迭代器
    }
    di = NULL; /* So that we don't release it again on error. */

    /* EOF opcode */
    // 写入一个EOF码，RDB_OPCODE_EOF = 255
    if (rdbSaveType(rdb,RDB_OPCODE_EOF) == -1) goto werr;

    /* CRC64 checksum. It will be zero if checksum computation is disabled, the
     * loading code skips the check in this case. */
    // CRC64检验和，当校验和计算为0，没有开启是，在载入rdb文件时会跳过
    cksum = rdb->cksum;
    memrev64ifbe(&cksum);
    if (rioWrite(rdb,&cksum,8) == 0) goto werr;
    return C_OK;

// 写入错误
werr:
    if (error) *error = errno;  //保存错误码
    if (di) dictReleaseIterator(di);    //如果没有释放迭代器，则释放
    return C_ERR;
}

/* This is just a wrapper to rdbSaveRio() that additionally adds a prefix
 * and a suffix to the generated RDB dump. The prefix is:
 *
 * $EOF:<40 bytes unguessable hex string>\r\n
 *
 * While the suffix is the 40 bytes hex string we announced in the prefix.
 * This way processes receiving the payload can understand when it ends
 * without doing any processing of the content. */
// 在rdbSaveRio()基础上，继续添加一个前缀和一个后缀，格式如下：
// $EOF:<40 bytes unguessable hex string>\r\n
int rdbSaveRioWithEOFMark(rio *rdb, int *error) {
    char eofmark[RDB_EOF_MARK_SIZE];

    //随机生成一个40位长的十六进制的字符串作为当前Redis的运行ID
    getRandomHexChars(eofmark,RDB_EOF_MARK_SIZE);
    if (error) *error = 0;  //初始化错误码
    // 写"$EOF:"
    if (rioWrite(rdb,"$EOF:",5) == 0) goto werr;
    // 将Redis运行ID写入
    if (rioWrite(rdb,eofmark,RDB_EOF_MARK_SIZE) == 0) goto werr;
    // 写入后缀"\r\n"
    if (rioWrite(rdb,"\r\n",2) == 0) goto werr;
    // 写入rdb文件内容
    if (rdbSaveRio(rdb,error) == C_ERR) goto werr;
    // 将Redis运行ID写入
    if (rioWrite(rdb,eofmark,RDB_EOF_MARK_SIZE) == 0) goto werr;
    return C_OK;

// 写错误，设置错误码，返回C_ERR
werr: /* Write error. */
    /* Set 'error' only if not already set by rdbSaveRio() call. */
    if (error && *error == 0) *error = errno;
    return C_ERR;
}

/* Save the DB on disk. Return C_ERR on error, C_OK on success. */
// 将数据库保存在磁盘上，返回C_OK成功，否则返回C_ERR
int rdbSave(char *filename) {
    char tmpfile[256];
    char cwd[MAXPATHLEN]; /* Current working dir path for error messages. */
    FILE *fp;
    rio rdb;
    int error = 0;

    // 创建临时文件
    snprintf(tmpfile,256,"temp-%d.rdb", (int) getpid());
    // 以写方式打开该文件
    fp = fopen(tmpfile,"w");
    // 打开失败，获取文件目录，写入日志
    if (!fp) {
        char *cwdp = getcwd(cwd,MAXPATHLEN);
        // 写日志信息到logfile
        serverLog(LL_WARNING,
            "Failed opening the RDB file %s (in server root dir %s) "
            "for saving: %s",
            filename,
            cwdp ? cwdp : "unknown",
            strerror(errno));
        return C_ERR;
    }

    // 初始化一个rio对象，该对象是一个文件对象IO
    rioInitWithFile(&rdb,fp);
    // 将数据库的内容写到rio中
    if (rdbSaveRio(&rdb,&error) == C_ERR) {
        errno = error;
        goto werr;
    }

    /* Make sure data will not remain on the OS's output buffers */
    // 冲洗缓冲区，确保所有的数据都写入磁盘
    if (fflush(fp) == EOF) goto werr;
    // 将fp指向的文件同步到磁盘中
    if (fsync(fileno(fp)) == -1) goto werr;
    // 关闭文件
    if (fclose(fp) == EOF) goto werr;

    /* Use RENAME to make sure the DB file is changed atomically only
     * if the generate DB file is ok. */
    // 原子性改变rdb文件的名字
    if (rename(tmpfile,filename) == -1) {
        // 改变名字失败，则获得当前目录路径，发送日志信息，删除临时文件
        char *cwdp = getcwd(cwd,MAXPATHLEN);
        serverLog(LL_WARNING,
            "Error moving temp DB file %s on the final "
            "destination %s (in server root dir %s): %s",
            tmpfile,
            filename,
            cwdp ? cwdp : "unknown",
            strerror(errno));
        unlink(tmpfile);
        return C_ERR;
    }

    // 写日志文件
    serverLog(LL_NOTICE,"DB saved on disk");
    // 重置服务器的脏键
    server.dirty = 0;
    // 更新上一次SAVE操作的时间
    server.lastsave = time(NULL);
    // 更新SAVE操作的状态
    server.lastbgsave_status = C_OK;
    return C_OK;

// rdbSaveRio()函数的写错误处理，写日志，关闭文件，删除临时文件，发送C_ERR
werr:
    serverLog(LL_WARNING,"Write error saving DB on disk: %s", strerror(errno));
    fclose(fp);
    unlink(tmpfile);
    return C_ERR;
}

// 后台进行RDB持久化BGSAVE操作
int rdbSaveBackground(char *filename) {
    pid_t childpid;
    long long start;

    // 当前没有正在进行AOF和RDB操作，否则返回C_ERR
    if (server.aof_child_pid != -1 || server.rdb_child_pid != -1) return C_ERR;

    // 备份当前数据库的脏键值
    server.dirty_before_bgsave = server.dirty;
    // 最近一个执行BGSAVE的时间
    server.lastbgsave_try = time(NULL);
    // fork函数开始时间，记录fork函数的耗时
    start = ustime();
    // 创建子进程
    if ((childpid = fork()) == 0) {
        int retval;
        // 子进程执行的代码
        /* Child */

        // 关闭监听的套接字
        closeListeningSockets(0);
        // 设置进程标题，方便识别
        redisSetProcTitle("redis-rdb-bgsave");
        // 执行保存操作，将数据库的写到filename文件中
        retval = rdbSave(filename);

        if (retval == C_OK) {
            // 得到子进程进程的脏私有虚拟页面大小，如果做RDB的同时父进程正在写入的数据，那么子进程就会拷贝一个份父进程的内存，而不是和父进程共享一份内存。
            size_t private_dirty = zmalloc_get_private_dirty();
            // 将子进程分配的内容写日志
            if (private_dirty) {
                serverLog(LL_NOTICE,
                    "RDB: %zu MB of memory used by copy-on-write",
                    private_dirty/(1024*1024));
            }
        }
        // 子进程退出，发送信号给父进程，发送0表示BGSAVE成功，1表示失败
        exitFromChild((retval == C_OK) ? 0 : 1);
    } else {
        // 父进程执行的代码
        /* Parent */
        // 计算出fork的执行时间
        server.stat_fork_time = ustime()-start;
        // 计算fork的速率，GB/每秒
        server.stat_fork_rate = (double) zmalloc_used_memory() * 1000000 / server.stat_fork_time / (1024*1024*1024); /* GB per second. */
        //如果fork执行时长，超过设置的阀值，则要将其加入到一个字典中，与传入"fork"关联，以便进行延迟诊断
        latencyAddSampleIfNeeded("fork",server.stat_fork_time/1000);

        // 如果fork出错
        if (childpid == -1) {
            server.lastbgsave_status = C_ERR;   //设置BGSAVE错误
            // 更新日志信息
            serverLog(LL_WARNING,"Can't save in background: fork: %s",
                strerror(errno));
            return C_ERR;
        }
        // 更新日志信息
        serverLog(LL_NOTICE,"Background saving started by pid %d",childpid);
        server.rdb_save_time_start = time(NULL);    //设置BGSAVE开始的时间
        server.rdb_child_pid = childpid;            //设置负责执行BGSAVE操作的子进程id
        server.rdb_child_type = RDB_CHILD_TYPE_DISK;//设置BGSAVE的类型，往磁盘中写入
        //关闭哈希表的resize，因为resize过程中会有复制拷贝动作
        updateDictResizePolicy();
        return C_OK;
    }
    return C_OK; /* unreached */
}

// 删除临时文件，当BGSAVE执行被中断时使用
void rdbRemoveTempFile(pid_t childpid) {
    char tmpfile[256];

    snprintf(tmpfile,sizeof(tmpfile),"temp-%d.rdb", (int) childpid);
    unlink(tmpfile);
}

/* Load a Redis object of the specified type from the specified file.
 * On success a newly allocated object is returned, otherwise NULL. */
// 从rio中读出一个rdbtype类型的对象，成功返回新对象地址，否则返回NULL
robj *rdbLoadObject(int rdbtype, rio *rdb) {
    robj *o = NULL, *ele, *dec;
    size_t len;
    unsigned int i;

    // 读出一个字符串对象
    if (rdbtype == RDB_TYPE_STRING) {
        /* Read string value */
        // 从rio中读出一个已经编码过的字符串对象
        if ((o = rdbLoadEncodedStringObject(rdb)) == NULL) return NULL;
        // 优化编码
        o = tryObjectEncoding(o);
    // 读出一个列表对象
    } else if (rdbtype == RDB_TYPE_LIST) {
        /* Read list value */
        // 从rio中读出一个长度，表示列表的元素个数
        if ((len = rdbLoadLen(rdb,NULL)) == RDB_LENERR) return NULL;

        // 创建一个quicklist的列表对象
        o = createQuicklistObject();
        // 设置列表的压缩程度和ziplist最大的长度
        quicklistSetOptions(o->ptr, server.list_max_ziplist_size,
                            server.list_compress_depth);

        /* Load every single element of the list */
        // 读出len个元素
        while(len--) {
            // 从rio中读出已经编码过的字符串对象
            if ((ele = rdbLoadEncodedStringObject(rdb)) == NULL) return NULL;
            // 解码成字符串对象
            dec = getDecodedObject(ele);
            // 获得字符串的长度
            size_t len = sdslen(dec->ptr);
            // 将读出来的字符串对象push进quicklist
            quicklistPushTail(o->ptr, dec->ptr, len);
            decrRefCount(dec);
            decrRefCount(ele);
        }
    // 读出一个集合对象
    } else if (rdbtype == RDB_TYPE_SET) {
        /* Read list/set value */
        // 读出集合的成员个数
        if ((len = rdbLoadLen(rdb,NULL)) == RDB_LENERR) return NULL;

        /* Use a regular set when there are too many entries. */
        // 根据集合成员的数量，如果大于配置的最多intset节点数量，则创建一个字典编码的集合对象
        if (len > server.set_max_intset_entries) {
            o = createSetObject();
            /* It's faster to expand the dict to the right size asap in order
             * to avoid rehashing */
            if (len > DICT_HT_INITIAL_SIZE) //按需扩展集合大小
                dictExpand(o->ptr,len);
        // 小于则创建一个intset编码的集合对象
        } else {
            o = createIntsetObject();
        }

        /* Load every single element of the list/set */
        // 读入len个元素
        for (i = 0; i < len; i++) {
            long long llval;
            // 从rio中读出一个元素
            if ((ele = rdbLoadEncodedStringObject(rdb)) == NULL) return NULL;
            // 对元素对象进行优化编码
            ele = tryObjectEncoding(ele);
            // 将元素添加到intset中
            if (o->encoding == OBJ_ENCODING_INTSET) {
                /* Fetch integer value from element */
                // 尝试将元素对象转换为longlong类型，保存在llval中，如果可以则加入到intset中
                if (isObjectRepresentableAsLongLong(ele,&llval) == C_OK) {
                    o->ptr = intsetAdd(o->ptr,llval,NULL);
                // 如果不行，则进行编码转换，保存到字典类型的集合中
                } else {
                    setTypeConvert(o,OBJ_ENCODING_HT);
                    dictExpand(o->ptr,len);
                }
            }

            /* This will also be called when the set was just converted
             * to a regular hash table encoded set */
            // 如果是字典类型的集合对象，将元素对象加入到字典中
            if (o->encoding == OBJ_ENCODING_HT) {
                dictAdd((dict*)o->ptr,ele,NULL);
            } else {
                decrRefCount(ele);
            }
        }
    // 读出一个有序集合对象
    } else if (rdbtype == RDB_TYPE_ZSET) {
        /* Read list/set value */
        size_t zsetlen;
        size_t maxelelen = 0;
        zset *zs;
        // 读出一个有序集合的元素个数
        if ((zsetlen = rdbLoadLen(rdb,NULL)) == RDB_LENERR) return NULL;
        // 创建一个有序集合对象
        o = createZsetObject();
        zs = o->ptr;

        /* Load every single element of the list/set */
        // 读出每一个有序集合对象
        while(zsetlen--) {
            robj *ele;
            double score;
            zskiplistNode *znode;

            // 从rio中读出一个有序集合的元素对象
            if ((ele = rdbLoadEncodedStringObject(rdb)) == NULL) return NULL;
            // 优化编码
            ele = tryObjectEncoding(ele);
            // 从rio中读出一个分数值，保存在score中
            if (rdbLoadDoubleValue(rdb,&score) == -1) return NULL;

            /* Don't care about integer-encoded strings. */
            // 保存成员的最大长度
            if (sdsEncodedObject(ele) && sdslen(ele->ptr) > maxelelen)
                maxelelen = sdslen(ele->ptr);

            // 将当前元素对象插入到有序集合的跳跃表和字典中
            znode = zslInsert(zs->zsl,score,ele);
            dictAdd(zs->dict,ele,&znode->score);
            incrRefCount(ele); /* added to skiplist */
        }

        /* Convert *after* loading, since sorted sets are not stored ordered. */
        // 如果可以进行编码转换，那么转换成ziplist编码，节约空间
        if (zsetLength(o) <= server.zset_max_ziplist_entries &&
            maxelelen <= server.zset_max_ziplist_value)
                zsetConvert(o,OBJ_ENCODING_ZIPLIST);
    // 读出一个哈希对象
    } else if (rdbtype == RDB_TYPE_HASH) {
        size_t len;
        int ret;

        // 读出哈希节点数量
        len = rdbLoadLen(rdb, NULL);
        if (len == RDB_LENERR) return NULL;

        // 创建一个哈希对象，默认压缩列表编码类型
        o = createHashObject();

        /* Too many entries? Use a hash table. */
        // 如果节点数超过限制，则转换成字典编码类型
        if (len > server.hash_max_ziplist_entries)
            hashTypeConvert(o, OBJ_ENCODING_HT);

        /* Load every field and value into the ziplist */
        // 将field和value读到ziplist编码的哈希对象中将field和value读到ziplist编码的哈希对象中
        while (o->encoding == OBJ_ENCODING_ZIPLIST && len > 0) {
            robj *field, *value;

            len--;
            /* Load raw strings */
            // 读出field和value对象
            field = rdbLoadStringObject(rdb);
            if (field == NULL) return NULL;
            serverAssert(sdsEncodedObject(field));
            value = rdbLoadStringObject(rdb);
            if (value == NULL) return NULL;
            serverAssert(sdsEncodedObject(value));

            /* Add pair to ziplist */
            // 将field和value对象push到ziplist中
            o->ptr = ziplistPush(o->ptr, field->ptr, sdslen(field->ptr), ZIPLIST_TAIL);
            o->ptr = ziplistPush(o->ptr, value->ptr, sdslen(value->ptr), ZIPLIST_TAIL);
            /* Convert to hash table if size threshold is exceeded */
            // 如果超过了ziplist的限制，则要将编码转换字典类型
            if (sdslen(field->ptr) > server.hash_max_ziplist_value ||
                sdslen(value->ptr) > server.hash_max_ziplist_value)
            {
                decrRefCount(field);
                decrRefCount(value);
                hashTypeConvert(o, OBJ_ENCODING_HT);
                break;
            }
            decrRefCount(field);
            decrRefCount(value);
        }

        /* Load remaining fields and values into the hash table */
        // 将field和value读到ht编码的哈希对象中
        while (o->encoding == OBJ_ENCODING_HT && len > 0) {
            robj *field, *value;

            len--;
            /* Load encoded strings */
            // 从rio中读出一个编码过的字符串field对象
            field = rdbLoadEncodedStringObject(rdb);
            if (field == NULL) return NULL;
            // 从rio中读出一个编码过的字符串value对象
            value = rdbLoadEncodedStringObject(rdb);
            if (value == NULL) return NULL;
            // 尝试对field和value对象进行优化编码
            field = tryObjectEncoding(field);
            value = tryObjectEncoding(value);

            /* Add pair to hash table */
            // 将field和value对象将入哈希对象中
            ret = dictAdd((dict*)o->ptr, field, value);
            if (ret == DICT_ERR) {
                rdbExitReportCorruptRDB("Duplicate keys detected");
            }
        }

        /* All pairs should be read by now */
        serverAssert(len == 0);
    // 读出一个quicklist类型的列表
    } else if (rdbtype == RDB_TYPE_LIST_QUICKLIST) {
        // 读出quicklist的节点
        if ((len = rdbLoadLen(rdb,NULL)) == RDB_LENERR) return NULL;
        // 创建一个quicklist编码的列表对象
        o = createQuicklistObject();
        // 设置列表的压缩程度和ziplist最大的长度
        quicklistSetOptions(o->ptr, server.list_max_ziplist_size,
                            server.list_compress_depth);

        // 读出len个节点
        while (len--) {
            // 从rio中读出一个quicklistNode节点的ziplist地址
            unsigned char *zl = rdbGenericLoadStringObject(rdb,RDB_LOAD_PLAIN);
            if (zl == NULL) return NULL;
            // 将当前ziplist加入quicklistNode节点中，然后将该节点加到quicklist中
            quicklistAppendZiplist(o->ptr, zl);
        }

    } else if (rdbtype == RDB_TYPE_HASH_ZIPMAP  ||
               rdbtype == RDB_TYPE_LIST_ZIPLIST ||
               rdbtype == RDB_TYPE_SET_INTSET   ||
               rdbtype == RDB_TYPE_ZSET_ZIPLIST ||
               rdbtype == RDB_TYPE_HASH_ZIPLIST)
    {
        // 读出一个字符串值
        unsigned char *encoded = rdbGenericLoadStringObject(rdb,RDB_LOAD_PLAIN);
        if (encoded == NULL) return NULL;
        // 创建字符串
        o = createObject(OBJ_STRING,encoded); /* Obj type fixed below. */

        /* Fix the object encoding, and make sure to convert the encoded
         * data type into the base type if accordingly to the current
         * configuration there are too many elements in the encoded data
         * type. Note that we only check the length and not max element
         * size as this is an O(N) scan. Eventually everything will get
         * converted. */
        // 根据读取的编码类型，将值回复成原来的编码对象
        switch(rdbtype) {
            case RDB_TYPE_HASH_ZIPMAP:  //将zipmap转换为ziplist，zipmap已经不在使用
                /* Convert to ziplist encoded hash. This must be deprecated
                 * when loading dumps created by Redis 2.4 gets deprecated. */
                {
                    unsigned char *zl = ziplistNew();
                    unsigned char *zi = zipmapRewind(o->ptr);
                    unsigned char *fstr, *vstr;
                    unsigned int flen, vlen;
                    unsigned int maxlen = 0;

                    while ((zi = zipmapNext(zi, &fstr, &flen, &vstr, &vlen)) != NULL) {
                        if (flen > maxlen) maxlen = flen;
                        if (vlen > maxlen) maxlen = vlen;
                        zl = ziplistPush(zl, fstr, flen, ZIPLIST_TAIL);
                        zl = ziplistPush(zl, vstr, vlen, ZIPLIST_TAIL);
                    }

                    zfree(o->ptr);
                    o->ptr = zl;
                    o->type = OBJ_HASH;
                    o->encoding = OBJ_ENCODING_ZIPLIST;

                    if (hashTypeLength(o) > server.hash_max_ziplist_entries ||
                        maxlen > server.hash_max_ziplist_value)
                    {
                        hashTypeConvert(o, OBJ_ENCODING_HT);
                    }
                }
                break;
            case RDB_TYPE_LIST_ZIPLIST: //ziplist编码的列表，3.2版本不使用ziplist作为列表的编码
                o->type = OBJ_LIST;
                o->encoding = OBJ_ENCODING_ZIPLIST;
                listTypeConvert(o,OBJ_ENCODING_QUICKLIST);  //转换为quicklist
                break;
            case RDB_TYPE_SET_INTSET:   //整数集合编码的集合
                o->type = OBJ_SET;
                o->encoding = OBJ_ENCODING_INTSET;
                if (intsetLen(o->ptr) > server.set_max_intset_entries)  //按需转换为字典类型编码
                    setTypeConvert(o,OBJ_ENCODING_HT);
                break;
            case RDB_TYPE_ZSET_ZIPLIST: //压缩列表编码的有序集合对象
                o->type = OBJ_ZSET;
                o->encoding = OBJ_ENCODING_ZIPLIST;
                if (zsetLength(o) > server.zset_max_ziplist_entries)    //按需转换为跳跃表类型编码
                    zsetConvert(o,OBJ_ENCODING_SKIPLIST);
                break;
            case RDB_TYPE_HASH_ZIPLIST:  // 压缩列表编码的哈希对象
                o->type = OBJ_HASH;
                o->encoding = OBJ_ENCODING_ZIPLIST;
                if (hashTypeLength(o) > server.hash_max_ziplist_entries)    //按需转换为字典集合对象
                    hashTypeConvert(o, OBJ_ENCODING_HT);
                break;
            default:
                rdbExitReportCorruptRDB("Unknown RDB encoding type %d",rdbtype);
                break;
        }
    } else {
        rdbExitReportCorruptRDB("Unknown RDB encoding type %d",rdbtype);
    }
    return o;
}

/* Mark that we are loading in the global state and setup the fields
 * needed to provide loading stats. */
// 设置载入的状态信息
void startLoading(FILE *fp) {
    struct stat sb;

    /* Load the DB */
    server.loading = 1; //正在载入状态
    server.loading_start_time = time(NULL); //载入开始时间
    server.loading_loaded_bytes = 0;        //已载入的字节数

    // 读出文件的信息
    if (fstat(fileno(fp), &sb) == -1) {
        server.loading_total_bytes = 0;
    } else {
        server.loading_total_bytes = sb.st_size;    //设置载入的总字节
    }
}

/* Refresh the loading progress info */
// 设置载入时server的状态信息
void loadingProgress(off_t pos) {
    server.loading_loaded_bytes = pos;  //更新已载入的字节

    // 更新服务器内存使用的峰值
    if (server.stat_peak_memory < zmalloc_used_memory())
        server.stat_peak_memory = zmalloc_used_memory();
}

/* Loading finished */
// 载入完成
void stopLoading(void) {
    server.loading = 0;
}

/* Track loading progress in order to serve client's from time to time
   and if needed calculate rdb checksum  */
// 跟踪载入的信息，以便client进行查询，在rdb查询和时也需要
void rdbLoadProgressCallback(rio *r, const void *buf, size_t len) {
    // 如果设置了校验和，则进行校验和计算
    if (server.rdb_checksum)
        rioGenericUpdateChecksum(r, buf, len);
    // loading_process_events_interval_bytes 在server初始化是设置为2M
    // 在load时，用来设置读或写的最大字节数max_processing_chunk
    if (server.loading_process_events_interval_bytes &&
        (r->processed_bytes + len)/server.loading_process_events_interval_bytes > r->processed_bytes/server.loading_process_events_interval_bytes)
    {
        /* The DB can take some non trivial amount of time to load. Update
         * our cached time since it is used to create and update the last
         * interaction time with clients and for other important things. */
        // 数据库采用非琐碎的时间来加载，更新缓存时间因为它被用来创建和更新最后和client的互动时间
        updateCachedTime();
        // 如果环境为从节点，当前处于rdb文件的复制传输状态
        if (server.masterhost && server.repl_state == REPL_STATE_TRANSFER)
            // 发送空行给主节点，防止超时连接
            replicationSendNewlineToMaster();
        // 刷新载入进度信息
        loadingProgress(r->processed_bytes);
        //让服务器在被阻塞的情况下，仍然处理某些网络事件
        processEventsWhileBlocked();
    }
}

// 将指定的RDB文件读到数据库中
int rdbLoad(char *filename) {
    uint32_t dbid;
    int type, rdbver;
    redisDb *db = server.db+0;
    char buf[1024];
    long long expiretime, now = mstime();   //获取当前load操作的时间
    FILE *fp;
    rio rdb;

    // 只读打开文件
    if ((fp = fopen(filename,"r")) == NULL) return C_ERR;

    // 初始化一个文件流对象rio且设置对应文件指针
    rioInitWithFile(&rdb,fp);
    // 设置计算校验和的函数
    rdb.update_cksum = rdbLoadProgressCallback;
    // 设置载入读或写的最大字节数，2M
    rdb.max_processing_chunk = server.loading_process_events_interval_bytes;
    // 读出9个字节到buf，buf中保存着Redis版本"redis0007"
    if (rioRead(&rdb,buf,9) == 0) goto eoferr;
    buf[9] = '\0';  //"redis0007\0"
    //检查读出的版本号标识
    if (memcmp(buf,"REDIS",5) != 0) {
        fclose(fp);
        serverLog(LL_WARNING,"Wrong signature trying to load DB from file");
        errno = EINVAL; //读出的值非法
        return C_ERR;
    }
    // 转换成整数检查版本大小
    rdbver = atoi(buf+5);
    if (rdbver < 1 || rdbver > RDB_VERSION) {
        fclose(fp);
        serverLog(LL_WARNING,"Can't handle RDB format version %d",rdbver);
        errno = EINVAL;
        return C_ERR;
    }

    // 设置载入时server的状态信息
    startLoading(fp);
    // 开始读取RDB文件到数据库中
    while(1) {
        robj *key, *val;
        expiretime = -1;

        /* Read type. */
        // 首先读出类型
        if ((type = rdbLoadType(&rdb)) == -1) goto eoferr;

        /* Handle special types. */
        // 处理特殊情况
        // 如果首先是读出过期时间单位为秒
        if (type == RDB_OPCODE_EXPIRETIME) {
            /* EXPIRETIME: load an expire associated with the next key
             * to load. Note that after loading an expire we need to
             * load the actual type, and continue. */
            // 从rio中读出过期时间
            if ((expiretime = rdbLoadTime(&rdb)) == -1) goto eoferr;
            /* We read the time so we need to read the object type again. */
            // 从过期时间后读出一个键值对的类型
            if ((type = rdbLoadType(&rdb)) == -1) goto eoferr;
            /* the EXPIRETIME opcode specifies time in seconds, so convert
             * into milliseconds. */
            expiretime *= 1000; //转换成毫秒

        //读出过期时间单位为毫秒
        } else if (type == RDB_OPCODE_EXPIRETIME_MS) {
            /* EXPIRETIME_MS: milliseconds precision expire times introduced
             * with RDB v3. Like EXPIRETIME but no with more precision. */
            // 从rio中读出过期时间
            if ((expiretime = rdbLoadMillisecondTime(&rdb)) == -1) goto eoferr;
            /* We read the time so we need to read the object type again. */
            // 从过期时间后读出一个键值对的类型
            if ((type = rdbLoadType(&rdb)) == -1) goto eoferr;

        // 如果读到EOF，则直接跳出循环
        } else if (type == RDB_OPCODE_EOF) {
            /* EOF: End of file, exit the main loop. */
            break;

        // 读出的是切换数据库操作
        } else if (type == RDB_OPCODE_SELECTDB) {
            /* SELECTDB: Select the specified database. */
            // 读取出一个长度，保存的是数据库的ID
            if ((dbid = rdbLoadLen(&rdb,NULL)) == RDB_LENERR)
                goto eoferr;
            // 检查读出的ID是否合法
            if (dbid >= (unsigned)server.dbnum) {
                serverLog(LL_WARNING,
                    "FATAL: Data file was created with a Redis "
                    "server configured to handle more than %d "
                    "databases. Exiting\n", server.dbnum);
                exit(1);
            }
            // 切换数据库
            db = server.db+dbid;
            // 跳过本层循环，在读一个type
            continue; /* Read type again. */

        // 如果读出调整哈希表的操作
        } else if (type == RDB_OPCODE_RESIZEDB) {
            /* RESIZEDB: Hint about the size of the keys in the currently
             * selected data base, in order to avoid useless rehashing. */
            uint32_t db_size, expires_size;
            // 读出一个数据库键值对字典的大小
            if ((db_size = rdbLoadLen(&rdb,NULL)) == RDB_LENERR)
                goto eoferr;
            // 读出一个数据库过期字典的大小
            if ((expires_size = rdbLoadLen(&rdb,NULL)) == RDB_LENERR)
                goto eoferr;
            // 扩展两个字典
            dictExpand(db->dict,db_size);
            dictExpand(db->expires,expires_size);
            // 重新读出一个type
            continue; /* Read type again. */

        // 读出的是一个辅助字段
        } else if (type == RDB_OPCODE_AUX) {
            /* AUX: generic string-string fields. Use to add state to RDB
             * which is backward compatible. Implementations of RDB loading
             * are requierd to skip AUX fields they don't understand.
             *
             * An AUX field is composed of two strings: key and value. */
            robj *auxkey, *auxval;
            // 读出辅助字段的键对象和值对象
            if ((auxkey = rdbLoadStringObject(&rdb)) == NULL) goto eoferr;
            if ((auxval = rdbLoadStringObject(&rdb)) == NULL) goto eoferr;

            // 键对象的第一个字符是%
            if (((char*)auxkey->ptr)[0] == '%') {
                /* All the fields with a name staring with '%' are considered
                 * information fields and are logged at startup with a log
                 * level of NOTICE. */
                // 写日志信息
                serverLog(LL_NOTICE,"RDB '%s': %s",
                    (char*)auxkey->ptr,
                    (char*)auxval->ptr);
            } else {
                /* We ignore fields we don't understand, as by AUX field
                 * contract. */
                serverLog(LL_DEBUG,"Unrecognized RDB AUX field: '%s'",
                    (char*)auxkey->ptr);
            }

            decrRefCount(auxkey);
            decrRefCount(auxval);
            // 重新读出一个type
            continue; /* Read type again. */
        }

        /* Read key */
        // 读出一个key对象
        if ((key = rdbLoadStringObject(&rdb)) == NULL) goto eoferr;
        /* Read value */
        // 读出一个val对象
        if ((val = rdbLoadObject(type,&rdb)) == NULL) goto eoferr;
        /* Check if the key already expired. This function is used when loading
         * an RDB file from disk, either at startup, or when an RDB was
         * received from the master. In the latter case, the master is
         * responsible for key expiry. If we would expire keys here, the
         * snapshot taken by the master may not be reflected on the slave. */
        // 如果当前环境不是从节点，且该键设置了过期时间，已经过期
        if (server.masterhost == NULL && expiretime != -1 && expiretime < now) {
            // 释放键值对
            decrRefCount(key);
            decrRefCount(val);
            continue;
        }
        /* Add the new object in the hash table */
        // 将没有过期的键值对添加到数据库键值对字典中
        dbAdd(db,key,val);

        /* Set the expire time if needed */
        // 如果需要，设置过期时间
        if (expiretime != -1) setExpire(db,key,expiretime);

        decrRefCount(key);  //释放临时对象
    }

    // 此时已经读出完所有数据库的键值对，读到了EOF，但是EOF不是RDB文件的结束，还要进行校验和
    /* Verify the checksum if RDB version is >= 5 */
    // 当RDB版本大于5时，且开启了校验和的功能，那么进行校验和
    if (rdbver >= 5 && server.rdb_checksum) {
        uint64_t cksum, expected = rdb.cksum;

        // 读出一个8字节的校验和，然后比较
        if (rioRead(&rdb,&cksum,8) == 0) goto eoferr;
        memrev64ifbe(&cksum);
        if (cksum == 0) {
            serverLog(LL_WARNING,"RDB file was saved with checksum disabled: no check performed.");
        } else if (cksum != expected) {
            serverLog(LL_WARNING,"Wrong RDB checksum. Aborting now.");
            rdbExitReportCorruptRDB("RDB CRC error");
        }
    }

    fclose(fp); //关闭RDB文件
    stopLoading();  //设置载入完成的状态
    return C_OK;

// 错误退出
eoferr: /* unexpected end of file is handled here with a fatal exit */
    serverLog(LL_WARNING,"Short read or OOM loading DB. Unrecoverable error, aborting now.");
    // 检查rdb错误发送信息且退出
    rdbExitReportCorruptRDB("Unexpected EOF reading RDB file");
    return C_ERR; /* Just to avoid warning */
}

/* A background saving child (BGSAVE) terminated its work. Handle this.
 * This function covers the case of actual BGSAVEs. */
// 处理子进程进行BGSAVE完成时，要发送的实际信号
// BGSAVE的类型是写入磁盘的
// exitcode是子进程退出时的退出码，成功退出为0
// bysignal 子进程接受到信号
void backgroundSaveDoneHandlerDisk(int exitcode, int bysignal) {
    // 当子进程执行BGSAVE成功
    if (!bysignal && exitcode == 0) {
        serverLog(LL_NOTICE,
            "Background saving terminated with success");
        // 更新脏键
        server.dirty = server.dirty - server.dirty_before_bgsave;
        // 更新最近一个执行BGSAVE的时间
        server.lastsave = time(NULL);
        // 更新BGSAVE的执行状态为成功
        server.lastbgsave_status = C_OK;

    // exitcode不为0，则子进程执行BGSAVE出错
    } else if (!bysignal && exitcode != 0) {
        serverLog(LL_WARNING, "Background saving error");
        // 更新BGSAVE的执行状态为出错
        server.lastbgsave_status = C_ERR;

    // bysignal 不为0，表示子进程执行BGSAVE被信号中断
    } else {
        mstime_t latency;

        serverLog(LL_WARNING,
            "Background saving terminated by signal %d", bysignal);
        // 设置延迟检测开始的时间
        latencyStartMonitor(latency);
        // 删除RDB的临时文件
        rdbRemoveTempFile(server.rdb_child_pid);
        // 设置延迟的时间 = 当前的时间 - 开始的时间
        latencyEndMonitor(latency);
        // 将 "rdb-unlink-temp-file" 与 延迟的时间 关联到延迟检测的字典中
        latencyAddSampleIfNeeded("rdb-unlink-temp-file",latency);
        /* SIGUSR1 is whitelisted, so we have a way to kill a child without
         * tirggering an error conditon. */
        // 更新BGSAVE的执行状态为出错，除了SIGUSR1信号
        if (bysignal != SIGUSR1)
            server.lastbgsave_status = C_ERR;
    }

    // 初始化执行BGSAVE子进程的ID，-1表示没进行BGSAVE
    server.rdb_child_pid = -1;
    // rdb执行的类型设置为无类型(0)
    server.rdb_child_type = RDB_CHILD_TYPE_NONE;
    // 最近一次执行BGSAVE的时间
    server.rdb_save_time_last = time(NULL)-server.rdb_save_time_start;
    // 初始化BGSAVE开始的时间
    server.rdb_save_time_start = -1;
    /* Possibly there are slaves waiting for a BGSAVE in order to be served
     * (the first stage of SYNC is a bulk transfer of dump.rdb) */
    // 处理正在等待BGSAVE完成的从节点
    updateSlavesWaitingBgsave((!bysignal && exitcode == 0) ? C_OK : C_ERR, RDB_CHILD_TYPE_DISK);
}

/* A background saving child (BGSAVE) terminated its work. Handle this.
 * This function covers the case of RDB -> Salves socket transfers for
 * diskless replication. */
// 处理子进程进行BGSAVE完成时，要发送的实际信号
// BGSAVE的类型是写入从节点的socket的
// exitcode是子进程退出时的退出码，成功退出为0
// bysignal 子进程接受到信号
void backgroundSaveDoneHandlerSocket(int exitcode, int bysignal) {
    uint64_t *ok_slaves;

    // 当子进程执行BGSAVE成功
    if (!bysignal && exitcode == 0) {
        serverLog(LL_NOTICE,
            "Background RDB transfer terminated with success");
    // exitcode不为0，则子进程执行BGSAVE出错
    } else if (!bysignal && exitcode != 0) {
        serverLog(LL_WARNING, "Background transfer error");

    // bysignal 不为0，表示子进程执行BGSAVE被信号中断
    } else {
        serverLog(LL_WARNING,
            "Background transfer terminated by signal %d", bysignal);
    }
    // 初始化执行BGSAVE子进程的ID，-1表示没进行BGSAVE
    server.rdb_child_pid = -1;
    // rdb执行的类型设置为无类型(0)
    server.rdb_child_type = RDB_CHILD_TYPE_NONE;
    // 初始化BGSAVE开始的时间
    server.rdb_save_time_start = -1;

    /* If the child returns an OK exit code, read the set of slave client
     * IDs and the associated status code. We'll terminate all the slaves
     * in error state.
     *
     * If the process returned an error, consider the list of slaves that
     * can continue to be emtpy, so that it's just a special case of the
     * normal code path. */
    // 如果子进程返回一个成功的退出码，那么读出一组从节点的id和相关的状态码，我们将终止所有处于错误状态码的从节点
    // 如果进行返回一个错误 ，
    // 分配一个记录从节点列表空间大小的空间，保存一个count
    ok_slaves = zmalloc(sizeof(uint64_t)); /* Make space for the count. */
    ok_slaves[0] = 0;
    // 当子进程执行BGSAVE成功
    if (!bysignal && exitcode == 0) {
        int readlen = sizeof(uint64_t);

        // 从管道的读端，读取一个列表长度
        if (read(server.rdb_pipe_read_result_from_child, ok_slaves, readlen) ==
                 readlen)
        {
            // 更新整个节点列表空间的长度
            readlen = ok_slaves[0]*sizeof(uint64_t)*2;

            /* Make space for enough elements as specified by the first
             * uint64_t element in the array. */
            // 分配空间
            ok_slaves = zrealloc(ok_slaves,sizeof(uint64_t)+readlen);
            if (readlen &&
                read(server.rdb_pipe_read_result_from_child, ok_slaves+1,
                     readlen) != readlen)
            {
                ok_slaves[0] = 0;   //读出客户端ID和状态出错
            }
        }
    }

    // 关闭管道
    close(server.rdb_pipe_read_result_from_child);
    close(server.rdb_pipe_write_result_to_parent);

    /* We can continue the replication process with all the slaves that
     * correctly received the full payload. Others are terminated. */
    // 继续从从节点执行复制进程
    listNode *ln;
    listIter li;

    // 迭代器指向从节点链表表头
    listRewind(server.slaves,&li);
    // 遍历整个从节点链表
    while((ln = listNext(&li))) {
        client *slave = ln->value;  //从节点客户端

        // 如果当前的从节点客户端正在等待一个BGSAVE的完成
        if (slave->replstate == SLAVE_STATE_WAIT_BGSAVE_END) {
            uint64_t j;
            int errorcode = 0;

            /* Search for the slave ID in the reply. In order for a slave to
             * continue the replication process, we need to find it in the list,
             * and it must have an error code set to 0 (which means success). */
            // 我们必须从管道的回复中找到从节点的ID，为了从节点取继续执行复制过程，我们需要从链表中找到这个从节点，然后将error设置为0，这以为着成功

            // 遍历管道中的回复count，如果找到当前从节点id那么保存错误码errorcode
            for (j = 0; j < ok_slaves[0]; j++) {
                if (slave->id == ok_slaves[2*j+1]) {
                    errorcode = ok_slaves[2*j+2];
                    break; /* Found in slaves list. */
                }
            }
            // 遍历完了没有找到，则更新日志
            if (j == ok_slaves[0] || errorcode != 0) {
                serverLog(LL_WARNING,
                "Closing slave %s: child->slave RDB transfer failed: %s",
                    replicationGetSlaveName(slave),
                    (errorcode == 0) ? "RDB transfer child aborted"
                                     : strerror(errorcode));
                freeClient(slave);  //释放从节点空间

            // 找到了
            } else {
                serverLog(LL_WARNING,
                "Slave %s correctly received the streamed RDB file.",
                    replicationGetSlaveName(slave));
                /* Restore the socket as non-blocking. */
                // 将客户端的socket fd设置为非阻塞
                anetNonBlock(NULL,slave->fd);
                // 设置超时时间
                anetSendTimeout(NULL,slave->fd,0);
            }
        }
    }
    zfree(ok_slaves);

    // 处理正在等待BGSAVE完成的从节点
    updateSlavesWaitingBgsave((!bysignal && exitcode == 0) ? C_OK : C_ERR, RDB_CHILD_TYPE_SOCKET);
}

/* When a background RDB saving/transfer terminates, call the right handler. */
// 当BGSAVE 完成RDB文件，要么发送给从节点，要么保存到磁盘，调用正确的处理
void backgroundSaveDoneHandler(int exitcode, int bysignal) {
    // 根据RDB的类型，做正确的处理
    switch(server.rdb_child_type) {
    case RDB_CHILD_TYPE_DISK:
        backgroundSaveDoneHandlerDisk(exitcode,bysignal);
        break;
    case RDB_CHILD_TYPE_SOCKET:
        backgroundSaveDoneHandlerSocket(exitcode,bysignal);
        break;
    default:
        serverPanic("Unknown RDB child type.");
        break;
    }
}

/* Spawn an RDB child that writes the RDB to the sockets of the slaves
 * that are currently in SLAVE_STATE_WAIT_BGSAVE_START state. */
// fork一个子进程将rdb写到状态为等待BGSAVE开始的从节点的socket中
int rdbSaveToSlavesSockets(void) {
    int *fds;
    uint64_t *clientids;
    int numfds;
    listNode *ln;
    listIter li;
    pid_t childpid;
    long long start;
    int pipefds[2];

    if (server.aof_child_pid != -1 || server.rdb_child_pid != -1) return C_ERR;

    /* Before to fork, create a pipe that will be used in order to
     * send back to the parent the IDs of the slaves that successfully
     * received all the writes. */
    if (pipe(pipefds) == -1) return C_ERR;
    server.rdb_pipe_read_result_from_child = pipefds[0];
    server.rdb_pipe_write_result_to_parent = pipefds[1];

    /* Collect the file descriptors of the slaves we want to transfer
     * the RDB to, which are i WAIT_BGSAVE_START state. */
    fds = zmalloc(sizeof(int)*listLength(server.slaves));
    /* We also allocate an array of corresponding client IDs. This will
     * be useful for the child process in order to build the report
     * (sent via unix pipe) that will be sent to the parent. */
    clientids = zmalloc(sizeof(uint64_t)*listLength(server.slaves));
    numfds = 0;

    listRewind(server.slaves,&li);
    while((ln = listNext(&li))) {
        client *slave = ln->value;

        if (slave->replstate == SLAVE_STATE_WAIT_BGSAVE_START) {
            clientids[numfds] = slave->id;
            fds[numfds++] = slave->fd;
            replicationSetupSlaveForFullResync(slave,getPsyncInitialOffset());
            /* Put the socket in blocking mode to simplify RDB transfer.
             * We'll restore it when the children returns (since duped socket
             * will share the O_NONBLOCK attribute with the parent). */
            anetBlock(NULL,slave->fd);
            anetSendTimeout(NULL,slave->fd,server.repl_timeout*1000);
        }
    }

    /* Create the child process. */
    start = ustime();
    if ((childpid = fork()) == 0) {
        /* Child */
        int retval;
        rio slave_sockets;

        rioInitWithFdset(&slave_sockets,fds,numfds);
        zfree(fds);

        closeListeningSockets(0);
        redisSetProcTitle("redis-rdb-to-slaves");

        retval = rdbSaveRioWithEOFMark(&slave_sockets,NULL);
        if (retval == C_OK && rioFlush(&slave_sockets) == 0)
            retval = C_ERR;

        if (retval == C_OK) {
            size_t private_dirty = zmalloc_get_private_dirty();

            if (private_dirty) {
                serverLog(LL_NOTICE,
                    "RDB: %zu MB of memory used by copy-on-write",
                    private_dirty/(1024*1024));
            }

            /* If we are returning OK, at least one slave was served
             * with the RDB file as expected, so we need to send a report
             * to the parent via the pipe. The format of the message is:
             *
             * <len> <slave[0].id> <slave[0].error> ...
             *
             * len, slave IDs, and slave errors, are all uint64_t integers,
             * so basically the reply is composed of 64 bits for the len field
             * plus 2 additional 64 bit integers for each entry, for a total
             * of 'len' entries.
             *
             * The 'id' represents the slave's client ID, so that the master
             * can match the report with a specific slave, and 'error' is
             * set to 0 if the replication process terminated with a success
             * or the error code if an error occurred. */
            void *msg = zmalloc(sizeof(uint64_t)*(1+2*numfds));
            uint64_t *len = msg;
            uint64_t *ids = len+1;
            int j, msglen;

            *len = numfds;
            for (j = 0; j < numfds; j++) {
                *ids++ = clientids[j];
                *ids++ = slave_sockets.io.fdset.state[j];
            }

            /* Write the message to the parent. If we have no good slaves or
             * we are unable to transfer the message to the parent, we exit
             * with an error so that the parent will abort the replication
             * process with all the childre that were waiting. */
            msglen = sizeof(uint64_t)*(1+2*numfds);
            if (*len == 0 ||
                write(server.rdb_pipe_write_result_to_parent,msg,msglen)
                != msglen)
            {
                retval = C_ERR;
            }
            zfree(msg);
        }
        zfree(clientids);
        rioFreeFdset(&slave_sockets);
        exitFromChild((retval == C_OK) ? 0 : 1);
    } else {
        /* Parent */
        server.stat_fork_time = ustime()-start;
        server.stat_fork_rate = (double) zmalloc_used_memory() * 1000000 / server.stat_fork_time / (1024*1024*1024); /* GB per second. */
        latencyAddSampleIfNeeded("fork",server.stat_fork_time/1000);
        if (childpid == -1) {
            serverLog(LL_WARNING,"Can't save in background: fork: %s",
                strerror(errno));

            /* Undo the state change. The caller will perform cleanup on
             * all the slaves in BGSAVE_START state, but an early call to
             * replicationSetupSlaveForFullResync() turned it into BGSAVE_END */
            listRewind(server.slaves,&li);
            while((ln = listNext(&li))) {
                client *slave = ln->value;
                int j;

                for (j = 0; j < numfds; j++) {
                    if (slave->id == clientids[j]) {
                        slave->replstate = SLAVE_STATE_WAIT_BGSAVE_START;
                        break;
                    }
                }
            }
            close(pipefds[0]);
            close(pipefds[1]);
        } else {
            serverLog(LL_NOTICE,"Background RDB transfer started by pid %d",
                childpid);
            server.rdb_save_time_start = time(NULL);
            server.rdb_child_pid = childpid;
            server.rdb_child_type = RDB_CHILD_TYPE_SOCKET;
            updateDictResizePolicy();
        }
        zfree(clientids);
        zfree(fds);
        return (childpid == -1) ? C_ERR : C_OK;
    }
    return C_OK; /* Unreached. */
}

// SAVE 命令实现
void saveCommand(client *c) {
    // 如果正在执行持久化操作，则退出
    if (server.rdb_child_pid != -1) {
        addReplyError(c,"Background save already in progress");
        return;
    }
    // 执行SAVE
    if (rdbSave(server.rdb_filename) == C_OK) {
        addReply(c,shared.ok);
    } else {
        addReply(c,shared.err);
    }
}

/* BGSAVE [SCHEDULE] */
// BGSAVE 命令实现
void bgsaveCommand(client *c) {
    int schedule = 0;   //SCHEDULE控制BGSAVE的执行，避免和AOF重写进程冲突

    /* The SCHEDULE option changes the behavior of BGSAVE when an AOF rewrite
     * is in progress. Instead of returning an error a BGSAVE gets scheduled. */
    if (c->argc > 1) {
        // 设置schedule标志
        if (c->argc == 2 && !strcasecmp(c->argv[1]->ptr,"schedule")) {
            schedule = 1;
        } else {
            addReply(c,shared.syntaxerr);
            return;
        }
    }

    // 如果正在执行RDB持久化操作，则退出
    if (server.rdb_child_pid != -1) {
        addReplyError(c,"Background save already in progress");

    // 如果正在执行AOF持久化操作，需要将BGSAVE提上日程表
    } else if (server.aof_child_pid != -1) {
        // 如果schedule为真，设置rdb_bgsave_scheduled为1，表示将BGSAVE提上日程表
        if (schedule) {
            server.rdb_bgsave_scheduled = 1;
            addReplyStatus(c,"Background saving scheduled");
        } else {    //没有设置schedule，则不能立即执行BGSAVE
            addReplyError(c,
                "An AOF log rewriting in progress: can't BGSAVE right now. "
                "Use BGSAVE SCHEDULE in order to schedule a BGSAVE whenver "
                "possible.");
        }

    // 执行BGSAVE
    } else if (rdbSaveBackground(server.rdb_filename) == C_OK) {
        addReplyStatus(c,"Background saving started");
    } else {
        addReply(c,shared.err);
    }
}
