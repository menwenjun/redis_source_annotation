/*
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
#include "intset.h"
#include "zmalloc.h"
#include "endianconv.h"

/* Note that these encodings are ordered, so:
 * INTSET_ENC_INT16 < INTSET_ENC_INT32 < INTSET_ENC_INT64. */
#define INTSET_ENC_INT16 (sizeof(int16_t))   //16位，2个字节，表示范围-32,768~32,767
#define INTSET_ENC_INT32 (sizeof(int32_t))   //32位，4个字节，表示范围-2,147,483,648~2,147,483,647
#define INTSET_ENC_INT64 (sizeof(int64_t))   //64位，8个字节，表示范围-9,223,372,036,854,775,808~9,223,372,036,854,775,807

/* Return the required encoding for the provided value. */
static uint8_t _intsetValueEncoding(int64_t v) {    //返回合适v的编码方式
    if (v < INT32_MIN || v > INT32_MAX)         //如果超出32位所能表示数值的范围则返回INTSET_ENC_INT64
        return INTSET_ENC_INT64;
    else if (v < INT16_MIN || v > INT16_MAX)    //如果超出16位所能表示数值的范围则返回INTSET_ENC_INT32
        return INTSET_ENC_INT32;
    else
        return INTSET_ENC_INT16;                //返回用16位表示的INTSET_ENC_INT16
}

/* Return the value at pos, given an encoding. */
static int64_t _intsetGetEncoded(intset *is, int pos, uint8_t enc) {    //根据编码方式enc，返回在集合is中下标为pos的元素
    int64_t v64;
    int32_t v32;
    int16_t v16;

    if (enc == INTSET_ENC_INT64) {  //64位编码
        memcpy(&v64,((int64_t*)is->contents)+pos,sizeof(v64));  //从下标pos开始的内存空间拷贝64bit的数据到v64
        memrev64ifbe(&v64); //如果是大端序，就会转换成小端序
        return v64;
    } else if (enc == INTSET_ENC_INT32) {//32位编码
        memcpy(&v32,((int32_t*)is->contents)+pos,sizeof(v32));//从下标pos开始的内存空间拷贝32bit的数据到v32
        memrev32ifbe(&v32); //32位大小端转换
        return v32;
    } else {//16位编码
        memcpy(&v16,((int16_t*)is->contents)+pos,sizeof(v16));//从下标pos开始的内存空间拷贝16bit的数据到v16
        memrev16ifbe(&v16); //16位大小端转换
        return v16;
    }
}

/* Return the value at pos, using the configured encoding. */
static int64_t _intsetGet(intset *is, int pos) {    //根据集合is设置的编码方式，返回下标为pos的值
    return _intsetGetEncoded(is,pos,intrev32ifbe(is->encoding));
    //intrev32ifbe()函数返回参数的编码格式并且根据需求转换大小端
}

/* Set the value at pos, using the configured encoding. */
static void _intsetSet(intset *is, int pos, int64_t value) {    //根据集合is设置的编码方式，设置下标为pos的值为value
    uint32_t encoding = intrev32ifbe(is->encoding); //获取集合设置的编码方式

    if (encoding == INTSET_ENC_INT64) {             //如果是64位
        ((int64_t*)is->contents)[pos] = value;      //设置下标pos的值为value
        memrev64ifbe(((int64_t*)is->contents)+pos); //如果需要转换大小端
    } else if (encoding == INTSET_ENC_INT32) {      //如果是32位
        ((int32_t*)is->contents)[pos] = value;      //设置下标pos的值为value
        memrev32ifbe(((int32_t*)is->contents)+pos); //如果需要转换大小端
    } else {
        ((int16_t*)is->contents)[pos] = value;      //设置下标pos的值为value
        memrev16ifbe(((int16_t*)is->contents)+pos); //如果需要转换大小端
    }
}

/* Create an empty intset. */
intset *intsetNew(void) {           //创建一个空集合
    intset *is = zmalloc(sizeof(intset));   //分配空间
    is->encoding = intrev32ifbe(INTSET_ENC_INT16);  //设置编码方式
    is->length = 0; //集合为空
    return is;
}

/* Resize the intset */
static intset *intsetResize(intset *is, uint32_t len) { //调整集合的内存空间大小
    uint32_t size = len*intrev32ifbe(is->encoding); //计算数组的大小
    is = zrealloc(is,sizeof(intset)+size);  //分配空间，如果新空间的大小比原来的空间大，那么数组的元素会被保留
    return is;
}

/* Search for the position of "value". Return 1 when the value was found and
 * sets "pos" to the position of the value within the intset. Return 0 when
 * the value is not present in the intset and sets "pos" to the position
 * where "value" can be inserted. */
//找到is集合中值为value的下标，返回1，并保存在pos中，没有找到返回0，并将pos设置为value可以插入到数组的位置
static uint8_t intsetSearch(intset *is, int64_t value, uint32_t *pos) {
    int min = 0, max = intrev32ifbe(is->length)-1, mid = -1;
    int64_t cur = -1;

    /* The value can never be found when the set is empty */
    if (intrev32ifbe(is->length) == 0) {    //如果为空集合
        if (pos) *pos = 0;                  //pos设置为0
        return 0;
    } else {
        /* Check for the case where we know we cannot find the value,
         * but do know the insert position. */
        if (value > _intsetGet(is,intrev32ifbe(is->length)-1)) {    //因为数组是有序的，如果value大于数组最大值
            if (pos) *pos = intrev32ifbe(is->length);       //可以将pos设置为数组末尾
            return 0;
        } else if (value < _intsetGet(is,0)) {  //如果小于数组的最小值
            if (pos) *pos = 0;          //pos可以是下标为0的位置
            return 0;
        }
    }

    while(max >= min) { //有序集合中进行二分查找
        mid = ((unsigned int)min + (unsigned int)max) >> 1; //(min+max)/2，找到中间数的下标
        cur = _intsetGet(is,mid);   //等到下标为mid的值cur
        if (value > cur) {  //value大于当前值cur
            min = mid+1;    //后一半找
        } else if (value < cur) {   //value小于当前值cur
            max = mid-1;    //前一半找
        } else {
            break;  //找到退出循环
        }
    }

    if (value == cur) { //确认找到
        if (pos) *pos = mid;    //设置pos为找到的位置，返回1
        return 1;
    } else {
        if (pos) *pos = min;    //此时min和max相等，所以pos可以设置为min或max，返回0
        return 0;
    }
}

/* Upgrades the intset to a larger encoding and inserts the given integer. */
static intset *intsetUpgradeAndAdd(intset *is, int64_t value) { //根据value的编码方式，对整数集合is的编码格式升级
    uint8_t curenc = intrev32ifbe(is->encoding);    //当前集合的编码方式
    uint8_t newenc = _intsetValueEncoding(value);   //得到value合适的编码方式
    int length = intrev32ifbe(is->length);          //集合元素数量
    int prepend = value < 0 ? 1 : 0;                //如果value小于0，则要将value添加到数组最前端，因此为移动1个编码长度
    //集合的编码格式要升级，也就是内存增大
    //因为 value 的编码比集合原有的其他元素的编码都要大，所以value如果是负数，就是最小值，如果是正数则是最大值
    //索引value要么放在数组集合的最前端，要么最后端，根据prepend判断

    /* First set new encoding and resize */
    is->encoding = intrev32ifbe(newenc);    //更新集合is的编码方式
    is = intsetResize(is,intrev32ifbe(is->length)+1);   //根据新的编码方式重新设置内存空间大小

    /* Upgrade back-to-front so we don't overwrite values.
     * Note that the "prepend" variable is used to make sure we have an empty
     * space at either the beginning or the end of the intset. */
    //_intsetGetEncoded()得到下标为length的值
    //_intsetSet设置下标为prepend+length的值为_intsetGetEncoded返回的值
    //但是，编码格式已经发生改变，数组元素没变但是内存大小改变
    while(length--)
        _intsetSet(is,length+prepend,_intsetGetEncoded(is,length,curenc));

    /* Set the value at the beginning or the end. */
    if (prepend)    //value是负数，要放在最前端
        _intsetSet(is,0,value); //设置下标为0的值为value
    else
        _intsetSet(is,intrev32ifbe(is->length),value);  //value为正数，设置最末尾+1的值为value
    is->length = intrev32ifbe(intrev32ifbe(is->length)+1);  //数组元素加1
    return is;
}

static void intsetMoveTail(intset *is, uint32_t from, uint32_t to) {    //向前或向后移动指定下标范围内的数组元素
    void *src, *dst;
    uint32_t bytes = intrev32ifbe(is->length)-from; //获得要移动的元素的个数
    uint32_t encoding = intrev32ifbe(is->encoding); //获得集合is的默认编码方式

    if (encoding == INTSET_ENC_INT64) {         //判断不同的编码格式
        src = (int64_t*)is->contents+from;      //获得要被移动范围的起始地址
        dst = (int64_t*)is->contents+to;        //获得要被移动到的目的地址
        bytes *= sizeof(int64_t);               //计算要移动多少个字节
    } else if (encoding == INTSET_ENC_INT32) {
        src = (int32_t*)is->contents+from;
        dst = (int32_t*)is->contents+to;
        bytes *= sizeof(int32_t);
    } else {
        src = (int16_t*)is->contents+from;
        dst = (int16_t*)is->contents+to;
        bytes *= sizeof(int16_t);
    }
    memmove(dst,src,bytes); //从src开始移动bytes个字节到dst
}

/* Insert an integer in the intset */
intset *intsetAdd(intset *is, int64_t value, uint8_t *success) {//将value添加到is集合中，如果成功success被设置为1否则为0
    uint8_t valenc = _intsetValueEncoding(value);   //获得value适合的编码类型
    uint32_t pos;
    if (success) *success = 1;  //设置success默认为1

    /* Upgrade encoding if necessary. If we need to upgrade, we know that
     * this value should be either appended (if > 0) or prepended (if < 0),
     * because it lies outside the range of existing values. */
    if (valenc > intrev32ifbe(is->encoding)) {  //如果value的编码类型大于集合的编码类型
        /* This always succeeds, so we don't need to curry *success. */
        return intsetUpgradeAndAdd(is,value);   //升级集合，并且将value加入集合，一定成功
    } else {
        /* Abort if the value is already present in the set.
         * This call will populate "pos" with the right position to insert
         * the value when it cannot be found. */
        if (intsetSearch(is,value,&pos)) {  //查找value，若果value已经存在，intsetSearch返回1,如果不存在，pos保存value可以插入的位置
            if (success) *success = 0;  //value存在，success设置为0
            return is;
        }

        //value在集合中不存在，且pos保存可以插入的位置
        is = intsetResize(is,intrev32ifbe(is->length)+1);   //调整集合大小
        if (pos < intrev32ifbe(is->length)) intsetMoveTail(is,pos,pos+1);   //如果pos不是在数组末尾则要移动调整集合
    }

    _intsetSet(is,pos,value);   //设置pos下标的值为value
    is->length = intrev32ifbe(intrev32ifbe(is->length)+1);  //集合节点数量加1
    return is;
}

/* Delete integer from intset */
intset *intsetRemove(intset *is, int64_t value, int *success) { //从集合中删除value，删除成功success设置为1，失败为0
    uint8_t valenc = _intsetValueEncoding(value);   //获得value适合的编码类型
    uint32_t pos;
    if (success) *success = 0;  //设置success默认为0

    //如果value的编码格式小于集合的编码格式且value在集合中已存在，pos保存着下标
    if (valenc <= intrev32ifbe(is->encoding) && intsetSearch(is,value,&pos)) {
        uint32_t len = intrev32ifbe(is->length);    //备份当前集合元素数量

        /* We know we can delete */
        if (success) *success = 1;  //删除成功，设置success为1

        /* Overwrite value with tail and update length */
        if (pos < (len-1)) intsetMoveTail(is,pos+1,pos);    //如果不是最后一个元素，则移动元素覆盖掉被删除的元素
        is = intsetResize(is,len-1);    //缩小大小
        is->length = intrev32ifbe(len-1);   //更新集合元素个数
    }
    return is;
}

/* Determine whether a value belongs to this set */
uint8_t intsetFind(intset *is, int64_t value) {     //返回1表示value是集合中的元素，否则返回0
    uint8_t valenc = _intsetValueEncoding(value);   //获得value适合的编码类型
    return valenc <= intrev32ifbe(is->encoding) && intsetSearch(is,value,NULL);
    //如果value的编码格式小于集合的编码格式且value在集合中已存在，返回1，其中任何一个不成立返回0
}

/* Return random member */
int64_t intsetRandom(intset *is) {  //随机返回一个元素
    return _intsetGet(is,rand()%intrev32ifbe(is->length));  //随机生成一个下标，返回该下标的值
}

/* Sets the value to the value at the given position. When this position is
 * out of range the function returns 0, when in range it returns 1. */
uint8_t intsetGet(intset *is, uint32_t pos, int64_t *value) {   //获得下标为pos的值并保存在value中
    if (pos < intrev32ifbe(is->length)) {   //如果pos小于数组长度
        *value = _intsetGet(is,pos);        //返回pos下标的值，保存在value中
        return 1;
    }
    return 0;
}

/* Return intset length */
uint32_t intsetLen(intset *is) {    //返回集合的元素个数
    return intrev32ifbe(is->length);    //返回length成员
}

/* Return intset blob size in bytes. */
size_t intsetBlobLen(intset *is) {  //返回集合所占用的字节总量
    return sizeof(intset)+intrev32ifbe(is->length)*intrev32ifbe(is->encoding);  //编码格式×元素个数+集合大小
}

#ifdef INTSET_TEST_MAIN
#include <sys/time.h>

void intsetRepr(intset *is) {
    int i;
    for (i = 0; i < intrev32ifbe(is->length); i++) {
        printf("%lld\n", (uint64_t)_intsetGet(is,i));
    }
    printf("\n");
}

void error(char *err) {
    printf("%s\n", err);
    exit(1);
}

void ok(void) {
    printf("OK\n");
}

long long usec(void) {
    struct timeval tv;
    gettimeofday(&tv,NULL);
    return (((long long)tv.tv_sec)*1000000)+tv.tv_usec;
}

#define assert(_e) ((_e)?(void)0:(_assert(#_e,__FILE__,__LINE__),exit(1)))
void _assert(char *estr, char *file, int line) {
    printf("\n\n=== ASSERTION FAILED ===\n");
    printf("==> %s:%d '%s' is not true\n",file,line,estr);
}

intset *createSet(int bits, int size) {
    uint64_t mask = (1<<bits)-1;
    uint64_t i, value;
    intset *is = intsetNew();

    for (i = 0; i < size; i++) {
        if (bits > 32) {
            value = (rand()*rand()) & mask;
        } else {
            value = rand() & mask;
        }
        is = intsetAdd(is,value,NULL);
    }
    return is;
}

void checkConsistency(intset *is) {
    int i;

    for (i = 0; i < (intrev32ifbe(is->length)-1); i++) {
        uint32_t encoding = intrev32ifbe(is->encoding);

        if (encoding == INTSET_ENC_INT16) {
            int16_t *i16 = (int16_t*)is->contents;
            assert(i16[i] < i16[i+1]);
        } else if (encoding == INTSET_ENC_INT32) {
            int32_t *i32 = (int32_t*)is->contents;
            assert(i32[i] < i32[i+1]);
        } else {
            int64_t *i64 = (int64_t*)is->contents;
            assert(i64[i] < i64[i+1]);
        }
    }
}

int main(int argc, char **argv) {
    uint8_t success;
    int i;
    intset *is;
    sranddev();

    printf("Value encodings: "); {
        assert(_intsetValueEncoding(-32768) == INTSET_ENC_INT16);
        assert(_intsetValueEncoding(+32767) == INTSET_ENC_INT16);
        assert(_intsetValueEncoding(-32769) == INTSET_ENC_INT32);
        assert(_intsetValueEncoding(+32768) == INTSET_ENC_INT32);
        assert(_intsetValueEncoding(-2147483648) == INTSET_ENC_INT32);
        assert(_intsetValueEncoding(+2147483647) == INTSET_ENC_INT32);
        assert(_intsetValueEncoding(-2147483649) == INTSET_ENC_INT64);
        assert(_intsetValueEncoding(+2147483648) == INTSET_ENC_INT64);
        assert(_intsetValueEncoding(-9223372036854775808ull) == INTSET_ENC_INT64);
        assert(_intsetValueEncoding(+9223372036854775807ull) == INTSET_ENC_INT64);
        ok();
    }

    printf("Basic adding: "); {
        is = intsetNew();
        is = intsetAdd(is,5,&success); assert(success);
        is = intsetAdd(is,6,&success); assert(success);
        is = intsetAdd(is,4,&success); assert(success);
        is = intsetAdd(is,4,&success); assert(!success);
        ok();
    }

    printf("Large number of random adds: "); {
        int inserts = 0;
        is = intsetNew();
        for (i = 0; i < 1024; i++) {
            is = intsetAdd(is,rand()%0x800,&success);
            if (success) inserts++;
        }
        assert(intrev32ifbe(is->length) == inserts);
        checkConsistency(is);
        ok();
    }

    printf("Upgrade from int16 to int32: "); {
        is = intsetNew();
        is = intsetAdd(is,32,NULL);
        assert(intrev32ifbe(is->encoding) == INTSET_ENC_INT16);
        is = intsetAdd(is,65535,NULL);
        assert(intrev32ifbe(is->encoding) == INTSET_ENC_INT32);
        assert(intsetFind(is,32));
        assert(intsetFind(is,65535));
        checkConsistency(is);

        is = intsetNew();
        is = intsetAdd(is,32,NULL);
        assert(intrev32ifbe(is->encoding) == INTSET_ENC_INT16);
        is = intsetAdd(is,-65535,NULL);
        assert(intrev32ifbe(is->encoding) == INTSET_ENC_INT32);
        assert(intsetFind(is,32));
        assert(intsetFind(is,-65535));
        checkConsistency(is);
        ok();
    }

    printf("Upgrade from int16 to int64: "); {
        is = intsetNew();
        is = intsetAdd(is,32,NULL);
        assert(intrev32ifbe(is->encoding) == INTSET_ENC_INT16);
        is = intsetAdd(is,4294967295,NULL);
        assert(intrev32ifbe(is->encoding) == INTSET_ENC_INT64);
        assert(intsetFind(is,32));
        assert(intsetFind(is,4294967295));
        checkConsistency(is);

        is = intsetNew();
        is = intsetAdd(is,32,NULL);
        assert(intrev32ifbe(is->encoding) == INTSET_ENC_INT16);
        is = intsetAdd(is,-4294967295,NULL);
        assert(intrev32ifbe(is->encoding) == INTSET_ENC_INT64);
        assert(intsetFind(is,32));
        assert(intsetFind(is,-4294967295));
        checkConsistency(is);
        ok();
    }

    printf("Upgrade from int32 to int64: "); {
        is = intsetNew();
        is = intsetAdd(is,65535,NULL);
        assert(intrev32ifbe(is->encoding) == INTSET_ENC_INT32);
        is = intsetAdd(is,4294967295,NULL);
        assert(intrev32ifbe(is->encoding) == INTSET_ENC_INT64);
        assert(intsetFind(is,65535));
        assert(intsetFind(is,4294967295));
        checkConsistency(is);

        is = intsetNew();
        is = intsetAdd(is,65535,NULL);
        assert(intrev32ifbe(is->encoding) == INTSET_ENC_INT32);
        is = intsetAdd(is,-4294967295,NULL);
        assert(intrev32ifbe(is->encoding) == INTSET_ENC_INT64);
        assert(intsetFind(is,65535));
        assert(intsetFind(is,-4294967295));
        checkConsistency(is);
        ok();
    }

    printf("Stress lookups: "); {
        long num = 100000, size = 10000;
        int i, bits = 20;
        long long start;
        is = createSet(bits,size);
        checkConsistency(is);

        start = usec();
        for (i = 0; i < num; i++) intsetSearch(is,rand() % ((1<<bits)-1),NULL);
        printf("%ld lookups, %ld element set, %lldusec\n",num,size,usec()-start);
    }

    printf("Stress add+delete: "); {
        int i, v1, v2;
        is = intsetNew();
        for (i = 0; i < 0xffff; i++) {
            v1 = rand() % 0xfff;
            is = intsetAdd(is,v1,NULL);
            assert(intsetFind(is,v1));

            v2 = rand() % 0xfff;
            is = intsetRemove(is,v2,NULL);
            assert(!intsetFind(is,v2));
        }
        checkConsistency(is);
        ok();
    }
}
#endif
