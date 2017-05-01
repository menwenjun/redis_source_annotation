/*
 * Copyright (c) 2009-2012, Salvatore Sanfilippo <antirez at gmail dot com>
 * Copyright (c) 2009-2012, Pieter Noordhuis <pcnoordhuis at gmail dot com>
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

/*-----------------------------------------------------------------------------
 * Sorted set API
 *----------------------------------------------------------------------------*/

/* ZSETs are ordered sets using two data structures to hold the same elements
 * in order to get O(log(N)) INSERT and REMOVE operations into a sorted
 * data structure.
 *
 * The elements are added to a hash table mapping Redis objects to scores.
 * At the same time the elements are added to a skip list mapping scores
 * to Redis objects (so objects are sorted by scores in this "view"). */

/* This skiplist implementation is almost a C translation of the original
 * algorithm described by William Pugh in "Skip Lists: A Probabilistic
 * Alternative to Balanced Trees", modified in three ways:
 * a) this implementation allows for repeated scores.
 * b) the comparison is not just by key (our 'score') but by satellite data.
 * c) there is a back pointer, so it's a doubly linked list with the back
 * pointers being only at "level 1". This allows to traverse the list
 * from tail to head, useful for ZREVRANGE. */

#include "server.h"
#include <math.h>

static int zslLexValueGteMin(robj *value, zlexrangespec *spec);
static int zslLexValueLteMax(robj *value, zlexrangespec *spec);

//创建一个层数level，分数为score，对象为obj的跳跃表节点
zskiplistNode *zslCreateNode(int level, double score, robj *obj) {
    zskiplistNode *zn = zmalloc(sizeof(*zn)+level*sizeof(struct zskiplistLevel));   //分配空间
    zn->score = score;//设置分数
    zn->obj = obj; //设置对象
    return zn; //返回节点地址
}

//创建返回一个跳跃表 表头zskiplist
zskiplist *zslCreate(void) {
    int j;
    zskiplist *zsl;

    zsl = zmalloc(sizeof(*zsl));//分配空间
    zsl->level = 1;//设置默认层数
    zsl->length = 0;//设置跳跃表长度
    //创建一个层数为32，分数为0，没有obj的跳跃表头节点
    zsl->header = zslCreateNode(ZSKIPLIST_MAXLEVEL,0,NULL);
    //跳跃表头节点初始化
    for (j = 0; j < ZSKIPLIST_MAXLEVEL; j++) {
        zsl->header->level[j].forward = NULL;//将跳跃表头节点的所有前进指针forward设置为NULL
        zsl->header->level[j].span = 0;//将跳跃表头节点的所有跨度span设置为0
    }
    zsl->header->backward = NULL;//跳跃表头节点的后退指针backward置为NULL
    zsl->tail = NULL;//表头指向跳跃表尾节点的指针置为NULL
    return zsl;
}

void zslFreeNode(zskiplistNode *node) {//释放一个跳跃表节点
    decrRefCount(node->obj);//该节点对象的引用计数减1
    zfree(node);//释放该该节点空间
}

void zslFree(zskiplist *zsl) {//释放跳跃表表头zsl，以及跳跃表节点
    zskiplistNode *node = zsl->header->level[0].forward, *next;

    zfree(zsl->header);//释放跳跃表的头节点
    while(node) {//释放其他节点
        next = node->level[0].forward;//备份下一个节点地址
        zslFreeNode(node);//释放节点空间
        node = next;//指向下一个节点
    }
    zfree(zsl);//释放表头
}

/* Returns a random level for the new skiplist node we are going to create.
 * The return value of this function is between 1 and ZSKIPLIST_MAXLEVEL
 * (both inclusive), with a powerlaw-alike distribution where higher
 * levels are less likely to be returned. */
int zslRandomLevel(void) {//返回一个随机层数值
    int level = 1;
    while ((random()&0xFFFF) < (ZSKIPLIST_P * 0xFFFF))//ZSKIPLIST_P（0.25）
        level += 1;//返回一个1到ZSKIPLIST_MAXLEVEL（32）之间的值
    return (level<ZSKIPLIST_MAXLEVEL) ? level : ZSKIPLIST_MAXLEVEL;
}

//创建一个节点，分数为score，对象为obj，插入到zsl表头管理的跳跃表中，并返回新节点的地址
zskiplistNode *zslInsert(zskiplist *zsl, double score, robj *obj) {
    zskiplistNode *update[ZSKIPLIST_MAXLEVEL], *x;
    unsigned int rank[ZSKIPLIST_MAXLEVEL];
    int i, level;

    serverAssert(!isnan(score));
    x = zsl->header;//获取跳跃表头结点地址，从头节点开始一层一层遍历
    for (i = zsl->level-1; i >= 0; i--) {//遍历头节点的每个level，从下标最大层减1到0
        /* store rank that is crossed to reach the insert position */
        rank[i] = i == (zsl->level-1) ? 0 : rank[i+1];//更新rank[i]为i+1所跨越的节点数，但是最外一层为0
        //这个while循环是查找的过程，沿着x指针遍历跳跃表，满足以下条件则要继续在当层往前走
        while (x->level[i].forward &&   //当前层的前进指针不为空且
            (x->level[i].forward->score < score ||//当前的要插入的score大于当前层的score或
                (x->level[i].forward->score == score &&//当前score等于要插入的score且
                compareStringObjects(x->level[i].forward->obj,obj) < 0))) {//当前层的对象与要插入的obj不等
            rank[i] += x->level[i].span;//记录该层一共跨越了多少节点 加上 上一层遍历所跨越的节点数
            x = x->level[i].forward;//指向下一个节点
        }
        //while循环跳出时，用update[i]记录第i层所遍历到的最后一个节点，遍历到i=0时，就要在该节点后要插入节点
        update[i] = x;
    }
    /* we assume the key is not already inside, since we allow duplicated
     * scores, and the re-insertion of score and redis object should never
     * happen since the caller of zslInsert() should test in the hash table
     * if the element is already inside or not.
     * zslInsert() 的调用者会确保同分值且同成员的元素不会出现，
     * 所以这里不需要进一步进行检查，可以直接创建新元素。
     */
    level = zslRandomLevel();//获得一个随机的层数
    if (level > zsl->level) {//如果大于当前所有节点最大的层数时
        for (i = zsl->level; i < level; i++) {
            rank[i] = 0;//将大于等于原来zsl->level层以上的rank[]设置为0
            update[i] = zsl->header;//将大于等于原来zsl->level层以上update[i]指向头结点
            update[i]->level[i].span = zsl->length;//update[i]已经指向头结点，将第i层的跨度设置为length
                                                    //length代表跳跃表的节点数量
        }
        zsl->level = level;//更新表中的最大成数值
    }
    x = zslCreateNode(level,score,obj); //创建一个节点
    for (i = 0; i < level; i++) {   //遍历每一层
        x->level[i].forward = update[i]->level[i].forward;//设置新节点的前进指针为查找时（while循环）每一层最后一个节点的的前进指针
        update[i]->level[i].forward = x;//再把查找时每层的最后一个节点的前进指针设置为新创建的节点地址

        /* update span covered by update[i] as x is inserted here */
        x->level[i].span = update[i]->level[i].span - (rank[0] - rank[i]);//更新插入节点的跨度值
        update[i]->level[i].span = (rank[0] - rank[i]) + 1; //更新插入节点前一个节点的跨度值
    }

    /* increment span for untouched levels */
    for (i = level; i < zsl->level; i++) {//如果插入节点的level小于原来的zsl->level才会执行
        update[i]->level[i].span++;//因为高度没有达到这些层，所以只需将查找时每层最后一个节点的值的跨度加1
    }

    //设置插入节点的后退指针，就是查找时最下层的最后一个节点，该节点的地址记录在update[0]中
    //如果插入在第二个节点，也就是头结点后的位置就将后退指针设置为NULL
    x->backward = (update[0] == zsl->header) ? NULL : update[0];
    if (x->level[0].forward)//如果x节点不是最尾部的节点
        x->level[0].forward->backward = x;//就将x节点后面的节点的后退节点设置成为x地址
    else
        zsl->tail = x;//否则更新表头的tail指针，指向最尾部的节点x
    zsl->length++;//跳跃表节点计数器加1
    return x; //返回x地址
}

/* Internal function used by zslDelete, zslDeleteByScore and zslDeleteByRank */
//被zslDelete, zslDeleteByScore and zslDeleteByRank使用的内部函数
void zslDeleteNode(zskiplist *zsl, zskiplistNode *x, zskiplistNode **update) {//删除节点
    int i;
    //设置前进指针和跨度
    for (i = 0; i < zsl->level; i++) {//遍历下标为0到跳跃表最大层数-1的层
        if (update[i]->level[i].forward == x) {//如果找到该节点
            update[i]->level[i].span += x->level[i].span - 1;//将前一个节点的跨度减1
            update[i]->level[i].forward = x->level[i].forward;
            //前一个节点的前进指针指向被删除的节点的后一个节点，跳过该节点
        } else {
            update[i]->level[i].span -= 1;//在第i层没找到，只将该层的最后一个节点的跨度减1
        }
    }
     //设置后退指针
    if (x->level[0].forward) {  //如果被删除的前进节点不为空，后面还有节点
        x->level[0].forward->backward = x->backward;//就将后面节点的后退指针指向被删除节点x的回退指针
    } else {
        zsl->tail = x->backward;//否则直接将被删除的x节点的后退节点设置为表头的tail指针
    }
    //更新跳跃表最大层数
    while(zsl->level > 1 && zsl->header->level[zsl->level-1].forward == NULL)
        zsl->level--;
    zsl->length--;//节点计数器减1
}

/* Delete an element with matching score/object from the skiplist. */
int zslDelete(zskiplist *zsl, double score, robj *obj) {//删除score和obj的节点
    zskiplistNode *update[ZSKIPLIST_MAXLEVEL], *x;
    int i;

    //下面的循环类似于zslInsert的查找部分
    x = zsl->header;    //获取跳跃表头结点地址，从头节点开始一层一层遍历
    for (i = zsl->level-1; i >= 0; i--) {   //遍历头节点的每个level，从下标最大层减1到0
        //这个while循环是查找的过程，沿着x指针遍历跳跃表，满足以下条件则要继续在当层往前走
        while (x->level[i].forward &&   //当前层的前进指针不为空且
            (x->level[i].forward->score < score ||//当前的要插入的score大于当前层的score或
                (x->level[i].forward->score == score &&//当前score等于要插入的score且
                compareStringObjects(x->level[i].forward->obj,obj) < 0)))//当前层的对象与要插入的obj不等
            x = x->level[i].forward; //指向下一个节点
        //while循环跳出时，用update[i]记录第i层所遍历到的最后一个节点，遍历到i=0时，就要在该节点后要插入节点
        update[i] = x;
    }
    /* We may have multiple elements with the same score, what we need
     * is to find the element with both the right score and object. */
    x = x->level[0].forward;//获取x的后面一个节点
    if (x && score == x->score && equalStringObjects(x->obj,obj)) {//如果是要被删除的节点
        zslDeleteNode(zsl, x, update);//删除该节点
        zslFreeNode(x);//释放空间
        return 1;//删除成功
    }
    return 0; /* not found */
}
//value 是否大于（或大于等于）范围 spec 中的 min 项
//如果返回1表示value大于等于min，否则返回0
static int zslValueGteMin(double value, zrangespec *spec) {
    return spec->minex ? (value > spec->min) : (value >= spec->min);
}

//value是否小于或小于等于范围 spec 中的max
//返回1表示value小于等于max，否则返回0
int zslValueLteMax(double value, zrangespec *spec) {
    return spec->maxex ? (value < spec->max) : (value <= spec->max);
}

/* Returns if there is a part of the zset is in range. */
int zslIsInRange(zskiplist *zsl, zrangespec *range) {//如果range范围包含在跳跃表的范围返回1，否则返回0
    zskiplistNode *x;

    /* Test for ranges that will always be empty. */
    if (range->min > range->max ||
            (range->min == range->max && (range->minex || range->maxex)))//排除错误范围，或0范围
        return 0;
    //最高分
    x = zsl->tail;
    if (x == NULL || !zslValueGteMin(x->score,range))//最大分值比range的min还小，返回0，取非，则return 0
        return 0;
    //最低分
    x = zsl->header->level[0].forward;
    if (x == NULL || !zslValueLteMax(x->score,range))//最小分值比range的最大分值还要大，return 0
        return 0;
    return 1;   //最大值大于min，最小值小于max且min小于max返回1
}

/* Find the first node that is contained in the specified range.
 * Returns NULL when no element is contained in the range. */
zskiplistNode *zslFirstInRange(zskiplist *zsl, zrangespec *range) {//返回第一个分数在range范围内的节点
    zskiplistNode *x;
    int i;

    /* If everything is out of range, return early. */
    if (!zslIsInRange(zsl,range)) return NULL; //如果不在范围内，则返回NULL，确保至少有一个节点符号range

    //判断下限
    x = zsl->header;//遍历跳跃表
    for (i = zsl->level-1; i >= 0; i--) {//遍历每一层
        /* Go forward while *OUT* of range. */
        while (x->level[i].forward &&   //如果该层有下一个节点且
            !zslValueGteMin(x->level[i].forward->score,range))//当前节点的score还小于(小于等于)range的min
                x = x->level[i].forward;//继续指向下一个节点
    }

    /* This is an inner range, so the next node cannot be NULL. */
    x = x->level[0].forward;//找到目标节点
    serverAssert(x != NULL);//保证能找到

    /* Check if score <= max. */
    //判断上限
    if (!zslValueLteMax(x->score,range)) return NULL;//该节点的分值如果比max还要大，就返回NULL
    return x;
}

/* Find the last node that is contained in the specified range.
 * Returns NULL when no element is contained in the range. */
zskiplistNode *zslLastInRange(zskiplist *zsl, zrangespec *range) {//返回最后一个分数在range范围内的节点
    zskiplistNode *x;
    int i;

    /* If everything is out of range, return early. */
    if (!zslIsInRange(zsl,range)) return NULL;//如果不在范围内，则返回NULL，确保至少有一个节点符号range

    //判断上限
    x = zsl->header;//遍历跳跃表
    for (i = zsl->level-1; i >= 0; i--) {//遍历每一层
        /* Go forward while *IN* range. */
        while (x->level[i].forward &&//如果该层有下一个节点且
            zslValueLteMax(x->level[i].forward->score,range))//当前节点的score小于(小于等于)max
                x = x->level[i].forward;//继续指向下一个节点
    }

    /* This is an inner range, so this node cannot be NULL. */
    serverAssert(x != NULL);//保证能找到

    /* Check if score >= min. */
    //判断下限
    if (!zslValueGteMin(x->score,range)) return NULL;//如果找到的节点的分值比range的min还要小
    return x;
}

/* Delete all the elements with score between min and max from the skiplist.
 * Min and max are inclusive, so a score >= min || score <= max is deleted.
 * Note that this function takes the reference to the hash table view of the
 * sorted set, in order to remove the elements from the hash table too. */
//删除所有分值介于min和max之间（包括min和max）的节点，并且将字典中的对象也删除(字典和跳跃表同时表示有序集合)
unsigned long zslDeleteRangeByScore(zskiplist *zsl, zrangespec *range, dict *dict) {
    zskiplistNode *update[ZSKIPLIST_MAXLEVEL], *x;
    unsigned long removed = 0;
    int i;

    //找到删除节点中的最小值的节点，大于等于min的
    x = zsl->header;//遍历头结点的每一层
    for (i = zsl->level-1; i >= 0; i--) {
        while (x->level[i].forward && (range->minex ?   //当前层的前进指针不为空
            x->level[i].forward->score <= range->min :  //分数小于（小于等于）min，继续前进
            x->level[i].forward->score < range->min))
                x = x->level[i].forward;                //指向下一个节点
        update[i] = x;//记录要删除的节点的前一个节点的地址
    }

    /* Current node is the last with score < or <= min. */
    x = x->level[0].forward;//获得被删除节点的地址

    /* Delete nodes while in range. */
    //删除一直删到最大值的节点，该值小于等于max的
    while (x &&
           (range->maxex ? x->score < range->max : x->score <= range->max))//只要分值小于等于max就删除
    {
        zskiplistNode *next = x->level[0].forward;//备份被删除节点的下一个节点
        zslDeleteNode(zsl,x,update);//删除当前的节点
        dictDelete(dict,x->obj);//将字典中的对象也删除了
        zslFreeNode(x);//释放空间
        removed++; //计数器加1
        x = next;//指向下一个节点
    }
    return removed;
}
//删除字典序的范围range之间的节点，并且将字典中的对象也删除
unsigned long zslDeleteRangeByLex(zskiplist *zsl, zlexrangespec *range, dict *dict) {
    zskiplistNode *update[ZSKIPLIST_MAXLEVEL], *x;
    unsigned long removed = 0;//计数器
    int i;


    x = zsl->header;//遍历头结点的每一层
    for (i = zsl->level-1; i >= 0; i--) {
        while (x->level[i].forward &&//当前层的前进指针不为空且
            !zslLexValueGteMin(x->level[i].forward->obj,range))//obj对象字典序小于range的min
                x = x->level[i].forward;//前进指向下一个节点
        update[i] = x;//记录要删除的节点的前一个节点的地址
    }

    /* Current node is the last with score < or <= min. */
    x = x->level[0].forward;//获得被删除节点的地址

    /* Delete nodes while in range. */
    while (x && zslLexValueLteMax(x->obj,range)) {//只要obj字典序还小于max就一直删除
        zskiplistNode *next = x->level[0].forward;//备份被删除节点的下一个节点
        zslDeleteNode(zsl,x,update);//删除当前的节点
        dictDelete(dict,x->obj);//将字典中的对象也删除了
        zslFreeNode(x);//释放空间
        removed++;//计数器加1
        x = next;//指向下一个节点
    }
    return removed;//返回被删除的节点个数
}

/* Delete all the elements with rank between start and end from the skiplist.
 * Start and end are inclusive. Note that start and end need to be 1-based */
//删除跳跃表中所有给定排位内的节点
unsigned long zslDeleteRangeByRank(zskiplist *zsl, unsigned int start, unsigned int end, dict *dict) {
    zskiplistNode *update[ZSKIPLIST_MAXLEVEL], *x;
    unsigned long traversed = 0, removed = 0;
    int i;

    x = zsl->header;//遍历头结点的每一层
    for (i = zsl->level-1; i >= 0; i--) {
        while (x->level[i].forward && (traversed + x->level[i].span) < start) {//还没到start起始的位置，则向前走
            traversed += x->level[i].span;//更新走的跨度
            x = x->level[i].forward;//指向下一个节点
        }
        update[i] = x;//记录要删除的节点的前一个节点的地址
    }

    traversed++;//因为找到的是要删除节点的前一个节点，所以traversed还要加1，此时的位置就是start的位置
    x = x->level[0].forward;//前进一个节点
    while (x && traversed <= end) {//从start开始删除到end
        zskiplistNode *next = x->level[0].forward;//备份下一节点的地址
        zslDeleteNode(zsl,x,update);//从跳跃表中删除节点
        dictDelete(dict,x->obj);//从字典中删除obj对象
        zslFreeNode(x);//释放空间
        removed++;//被删除节点计数器加1
        traversed++;//更新traversed
        x = next;//指向下一个节点
    }
    return removed;//返回被删除的节点个数
}

/* Find the rank for an element by both score and key.
 * Returns 0 when the element cannot be found, rank otherwise.
 * Note that the rank is 1-based due to the span of zsl->header to the
 * first element. */
unsigned long zslGetRank(zskiplist *zsl, double score, robj *o) {//查找score和o对象在跳跃表中的排位
    zskiplistNode *x;
    unsigned long rank = 0;
    int i;

    x = zsl->header;//遍历头结点的每一层
    for (i = zsl->level-1; i >= 0; i--) {
        while (x->level[i].forward &&
            (x->level[i].forward->score < score ||//只要分值还小于给定的score或者
                (x->level[i].forward->score == score &&//分值相等但是对象小于给定对象o
                compareStringObjects(x->level[i].forward->obj,o) <= 0))) {
            rank += x->level[i].span;//更新排位值
            x = x->level[i].forward;//指向下一个节点
        }

        /* x might be equal to zsl->header, so test if obj is non-NULL */
        //确保在第i层找到分值相同，且对象相同时才会返回排位值
        if (x->obj && equalStringObjects(x->obj,o)) {
            return rank;
        }
    }
    return 0;//没找到
}

/* Finds an element by its rank. The rank argument needs to be 1-based. */
zskiplistNode* zslGetElementByRank(zskiplist *zsl, unsigned long rank) {//返回排位为rank的节点地址
    zskiplistNode *x;
    unsigned long traversed = 0;//排位值，跨越过的节点数
    int i;

    x = zsl->header;
    for (i = zsl->level-1; i >= 0; i--) {//遍历头结点的每一层
        while (x->level[i].forward && (traversed + x->level[i].span) <= rank)//知道traversed还没到到达rank
        {
            traversed += x->level[i].span;//每次跟新排位值
            x = x->level[i].forward;//指向下一个节点
        }
        if (traversed == rank) { //跨越的节点数等于排位值，返回节点地址
            return x;
        }
    }
    return NULL;
}

/* Populate the rangespec according to the objects min and max. */
static int zslParseRange(robj *min, robj *max, zrangespec *spec) {//设置spec中的min和max值
    char *eptr;
    spec->minex = spec->maxex = 0;

    /* Parse the min-max interval. If one of the values is prefixed
     * by the "(" character, it's considered "open". For instance
     * ZRANGEBYSCORE zset (1.5 (2.5 will match min < x < max
     * ZRANGEBYSCORE zset 1.5 2.5 will instead match min <= x <= max */
    //处理最小值min
    if (min->encoding == OBJ_ENCODING_INT) {//如果该对象是用整数实现的
        spec->min = (long)min->ptr;//设置min值
    } else {//如果对象是用字符串实现的
        if (((char*)min->ptr)[0] == '(') { //左开
            spec->min = strtod((char*)min->ptr+1,&eptr);//将字符串转换为double类型的值，eptr用来存储不和条件的字符串地址
            if (eptr[0] != '\0' || isnan(spec->min)) return C_ERR;//isnan()函数用来判断min是否是一个数字 Not A Number
            spec->minex = 1;//1表示开区间，不包含
        } else {
            spec->min = strtod((char*)min->ptr,&eptr);
            if (eptr[0] != '\0' || isnan(spec->min)) return C_ERR;//ptr中有非数字字符或min不是数字，返回REDIS_ERR(-1)
        }
    }
    //处理最大值max
    if (max->encoding == OBJ_ENCODING_INT) {//如果该对象是用整数实现的
        spec->max = (long)max->ptr; //设置max值
    } else {
        if (((char*)max->ptr)[0] == '(') {
            spec->max = strtod((char*)max->ptr+1,&eptr);//将字符串转换为double类型的值，eptr用来存储不和条件的字符串地址
            if (eptr[0] != '\0' || isnan(spec->max)) return C_ERR;//ptr中有非数字字符或min不是数字，返回REDIS_ERR(-1)
            spec->maxex = 1;//1表示开区间，不包含
        } else {
            spec->max = strtod((char*)max->ptr,&eptr);
            if (eptr[0] != '\0' || isnan(spec->max)) return C_ERR;//ptr中有非数字字符或min不是数字，返回REDIS_ERR(-1)
        }
    }

    return C_OK; //处理成功，返回0
}

/* ------------------------ Lexicographic ranges ---------------------------- */

/* Parse max or min argument of ZRANGEBYLEX.
  * (foo means foo (open interval)
  * [foo means foo (closed interval)
  * - means the min string possible
  * + means the max string possible
  *
  * If the string is valid the *dest pointer is set to the redis object
  * that will be used for the comparision, and ex will be set to 0 or 1
  * respectively if the item is exclusive or inclusive. C_OK will be
  * returned.
  *
  * If the string is not a valid range C_ERR is returned, and the value
  * of *dest and *ex is undefined. */
// 解析ZRANGEBYLEX命令的最大值和最小值参数
int zslParseLexRangeItem(robj *item, robj **dest, int *ex) {//解析字典序的对象
    char *c = item->ptr;

    switch(c[0]) {
    case '+':               //+ 意味这值最大的字符串
        if (c[1] != '\0') return C_ERR;
        *ex = 0;
        *dest = shared.maxstring;
        incrRefCount(shared.maxstring);
        return C_OK;
    case '-':              //- 意味这值最小的字符串
        if (c[1] != '\0') return C_ERR;
        *ex = 0;
        *dest = shared.minstring;
        incrRefCount(shared.minstring);
        return C_OK;
    case '(':               //( 意味这是左开区间
        *ex = 1;
        *dest = createStringObject(c+1,sdslen(c)-1);
        return C_OK;
    case '[':               //[ 意味这左闭区间
        *ex = 0;
        *dest = createStringObject(c+1,sdslen(c)-1);
        return C_OK;
    default:
        return C_ERR;
    }
}

/* Populate the rangespec according to the objects min and max.
 *
 * Return C_OK on success. On error C_ERR is returned.
 * When OK is returned the structure must be freed with zslFreeLexRange(),
 * otherwise no release is needed. */
//根据最大值对象和最小值对象设置
static int zslParseLexRange(robj *min, robj *max, zlexrangespec *spec) {
    /* The range can't be valid if objects are integer encoded.
     * Every item must start with ( or [. */
    // 最大值对象和最小值对象不能是整数编码，因为第一个字符是( or [. */
    if (min->encoding == OBJ_ENCODING_INT ||
        max->encoding == OBJ_ENCODING_INT) return C_ERR;

    spec->min = spec->max = NULL;   //初始化最大值和最小值

    //解析最小值和最大值，如果任意出错则释放最小值和最大值空间，返回-1
    //解析成功，则设置spec的成员
    if (zslParseLexRangeItem(min, &spec->min, &spec->minex) == C_ERR ||
        zslParseLexRangeItem(max, &spec->max, &spec->maxex) == C_ERR) {
        if (spec->min) decrRefCount(spec->min);
        if (spec->max) decrRefCount(spec->max);
        return C_ERR;
    } else {
        return C_OK;
    }
}

/* Free a lex range structure, must be called only after zelParseLexRange()
 * populated the structure with success (C_OK returned). */
// 释放一个zlexrangespec结构中的对象空间
void zslFreeLexRange(zlexrangespec *spec) {
    decrRefCount(spec->min);
    decrRefCount(spec->max);
}

/* This is just a wrapper to compareStringObjects() that is able to
 * handle shared.minstring and shared.maxstring as the equivalent of
 * -inf and +inf for strings */
// 比较两个对象，能够处理shared.minstring and shared.maxstring的情况，相等返回0
int compareStringObjectsForLexRange(robj *a, robj *b) {
    if (a == b) return 0; /* This makes sure that we handle inf,inf and
                             -inf,-inf ASAP. One special case less. */
    if (a == shared.minstring || b == shared.maxstring) return -1;
    if (a == shared.maxstring || b == shared.minstring) return 1;
    return compareStringObjects(a,b);
}

// 比较value对象和规定的分数范围的最小值，返回1表示大于spec最小值
static int zslLexValueGteMin(robj *value, zlexrangespec *spec) {
    return spec->minex ?    //minex为1，则不包含
        (compareStringObjectsForLexRange(value,spec->min) > 0) :
        (compareStringObjectsForLexRange(value,spec->min) >= 0);
}

// 比较value对象和规定的分数范围的最大值，返回1表示小于spec最大值
static int zslLexValueLteMax(robj *value, zlexrangespec *spec) {
    return spec->maxex ?
        (compareStringObjectsForLexRange(value,spec->max) < 0) :
        (compareStringObjectsForLexRange(value,spec->max) <= 0);
}

/* Returns if there is a part of the zset is in the lex range. */
// 返回一个zset是否有一部分在range的范围内
int zslIsInLexRange(zskiplist *zsl, zlexrangespec *range) {
    zskiplistNode *x;

    /* Test for ranges that will always be empty. */
    // 测试range的范围是否合法
    if (compareStringObjectsForLexRange(range->min,range->max) > 1 ||
            (compareStringObjects(range->min,range->max) == 0 &&
            (range->minex || range->maxex)))
        return 0;
    x = zsl->tail;
    // ！查看跳跃表的尾节点的对象是否比range的最小值还大，
    if (x == NULL || !zslLexValueGteMin(x->obj,range))
        return 0;   //表示zset没有一部分在range范围
    x = zsl->header->level[0].forward;
    // ！查看跳跃表的头节点的对象是否比range的最大值还小，
    if (x == NULL || !zslLexValueLteMax(x->obj,range))
        return 0;   //表示zset没有一部分在range范围
    return 1;
}

/* Find the first node that is contained in the specified lex range.
 * Returns NULL when no element is contained in the range. */
// 返回第一个包含在range范围内的节点的地址
zskiplistNode *zslFirstInLexRange(zskiplist *zsl, zlexrangespec *range) {
    zskiplistNode *x;
    int i;

    /* If everything is out of range, return early. */
    // zsl是否有一部分包含在range内，没有则返回NULL
    if (!zslIsInLexRange(zsl,range)) return NULL;

    x = zsl->header;//遍历每一层
    for (i = zsl->level-1; i >= 0; i--) {
        /* Go forward while *OUT* of range. */
        // 一直前进寻找，当某一个节点比range的最小值大时，停止寻找
        while (x->level[i].forward &&
            !zslLexValueGteMin(x->level[i].forward->obj,range))
                x = x->level[i].forward;
    }

    /* This is an inner range, so the next node cannot be NULL. */
    // 保存当前节点地址
    x = x->level[0].forward;
    serverAssert(x != NULL);

    /* Check if score <= max. */
    // 检查当前节点是否超过最大值，超过则返回NULL
    if (!zslLexValueLteMax(x->obj,range)) return NULL;
    return x;
}

/* Find the last node that is contained in the specified range.
 * Returns NULL when no element is contained in the range. */
// 返回最后一个包含在range范围内的节点的地址
zskiplistNode *zslLastInLexRange(zskiplist *zsl, zlexrangespec *range) {
    zskiplistNode *x;
    int i;

    /* If everything is out of range, return early. */
    // zsl是否有一部分包含在range内，没有则返回NULL
    if (!zslIsInLexRange(zsl,range)) return NULL;

    x = zsl->header;//遍历每一层
    for (i = zsl->level-1; i >= 0; i--) {
        /* Go forward while *IN* range. */
        // 一直前进寻找，当某一个节点比range的最大值大时，停止寻找
        while (x->level[i].forward &&
            zslLexValueLteMax(x->level[i].forward->obj,range))
                x = x->level[i].forward;
    }

    /* This is an inner range, so this node cannot be NULL. */
    serverAssert(x != NULL);

    /* Check if score >= min. */
    // 检查当前节点是否小于最小值，小于则返回NULL
    if (!zslLexValueGteMin(x->obj,range)) return NULL;
    return x;
}

/*-----------------------------------------------------------------------------
 * Ziplist-backed sorted set API
 * 压缩列表实现的有序列表API
 *----------------------------------------------------------------------------*/
// 从sptr指向的entry中取出有序集合的分值
double zzlGetScore(unsigned char *sptr) {
    unsigned char *vstr;
    unsigned int vlen;
    long long vlong;
    char buf[128];
    double score;

    serverAssert(sptr != NULL);
    // 将sptr指向的entry保存到参数中
    serverAssert(ziplistGet(sptr,&vstr,&vlen,&vlong));

    // 如果是字符串类型的值
    if (vstr) {
        // 拷贝到buf中
        memcpy(buf,vstr,vlen);
        buf[vlen] = '\0';
        // 转换成double类型
        score = strtod(buf,NULL);
    // 整数值
    } else {
        score = vlong;
    }

    return score; //返回分值
}

/* Return a ziplist element as a Redis string object.
 * This simple abstraction can be used to simplifies some code at the
 * cost of some performance. */
// 返回一个sptr指向entry所构建的字符串类型的对象地址
robj *ziplistGetObject(unsigned char *sptr) {
    unsigned char *vstr;
    unsigned int vlen;
    long long vlong;

    serverAssert(sptr != NULL);
    // 将sptr指向的entry保存到参数中
    serverAssert(ziplistGet(sptr,&vstr,&vlen,&vlong));

    // 根据不同类型的值，创建返回字符串对象
    if (vstr) {
        return createStringObject((char*)vstr,vlen);
    } else {
        return createStringObjectFromLongLong(vlong);
    }
}

/* Compare element in sorted set with given element. */
// 比较eptr和cstr指向的元素，返回0表示相等，正整数表示eptr比cstr大
int zzlCompareElements(unsigned char *eptr, unsigned char *cstr, unsigned int clen) {
    unsigned char *vstr;
    unsigned int vlen;
    long long vlong;
    unsigned char vbuf[32];
    int minlen, cmp;

    // 将eptr指向的entry保存到参数中
    serverAssert(ziplistGet(eptr,&vstr,&vlen,&vlong));
    // 如果是整数值
    if (vstr == NULL) {
        /* Store string representation of long long in buf. */
        // 将整数值转换为字符串，保存在vbuf中
        vlen = ll2string((char*)vbuf,sizeof(vbuf),vlong);
        vstr = vbuf;
    }

    // 比较最小长度
    minlen = (vlen < clen) ? vlen : clen;
    // 比较
    cmp = memcmp(vstr,cstr,minlen);
    if (cmp == 0) return vlen-clen; //minlen长度的子串相等，返回长度差值
    return cmp; //返回结果
}

// 返回有序集合的元素个数
unsigned int zzlLength(unsigned char *zl) {
    return ziplistLen(zl)/2;
}

/* Move to next entry based on the values in eptr and sptr. Both are set to
 * NULL when there is no next entry. */
// 将当前的元素指针eptr和当前元素分值的指针sptr都指向下一个元素和元素的分值
void zzlNext(unsigned char *zl, unsigned char **eptr, unsigned char **sptr) {
    unsigned char *_eptr, *_sptr;
    serverAssert(*eptr != NULL && *sptr != NULL);

    // 下一个元素的地址为当前元素分值的下一个entry
    _eptr = ziplistNext(zl,*sptr);
    // 下一个entry不为空
    if (_eptr != NULL) {
        // 下一个元素分值的地址为下一个元素的下一个entry
        _sptr = ziplistNext(zl,_eptr);
        serverAssert(_sptr != NULL);
    } else {
        /* No next entry. */
        _sptr = NULL;
    }

    //保存到参数中返回
    *eptr = _eptr;
    *sptr = _sptr;
}

/* Move to the previous entry based on the values in eptr and sptr. Both are
 * set to NULL when there is no next entry. */
// 将当前的元素指针eptr和当前元素分值的指针sptr都指向上一个元素和元素的分值
void zzlPrev(unsigned char *zl, unsigned char **eptr, unsigned char **sptr) {
    unsigned char *_eptr, *_sptr;
    serverAssert(*eptr != NULL && *sptr != NULL);

    // 上一个元素分值的地址为当前一个元素的上一个entry
    _sptr = ziplistPrev(zl,*eptr);
    // 上一个entry不为空
    if (_sptr != NULL) {
        // 上一个元素的地址为上一个元素分值的上一个entry
        _eptr = ziplistPrev(zl,_sptr);
        serverAssert(_eptr != NULL);
    } else {
        /* No previous entry. */
        _eptr = NULL;
    }

    // 保存到参数中返回
    *eptr = _eptr;
    *sptr = _sptr;
}

/* Returns if there is a part of the zset is in range. Should only be used
 * internally by zzlFirstInRange and zzlLastInRange. */
// 判断ziplist中是否有entry节点在range范围之内，有一个则返回1，否则返回0
int zzlIsInRange(unsigned char *zl, zrangespec *range) {
    unsigned char *p;
    double score;

    /* Test for ranges that will always be empty. */
    // 测试range是否合法
    if (range->min > range->max ||
            (range->min == range->max && (range->minex || range->maxex)))
        return 0;

    // 保存ziplist总最后一个entry节点的地址，保存着有序集合的最大分值
    p = ziplistIndex(zl,-1); /* Last score. */
    if (p == NULL) return 0; /* Empty sorted set */
    // 取出有序集合的最大分值
    score = zzlGetScore(p);
    // 如果最大分值比range的最小最小，返回0
    if (!zslValueGteMin(score,range))
        return 0;

    // 保存ziplist总第一个entry节点的地址，保存着有序集合的最小分值
    p = ziplistIndex(zl,1); /* First score. */
    serverAssert(p != NULL);
    // 取出有序集合的最小分值
    score = zzlGetScore(p);
    // 如果最小分值比range的最大最大，返回0
    if (!zslValueLteMax(score,range))
        return 0;

    // ziplist中是至少有一个entry节点在range范围之内
    return 1;
}

/* Find pointer to the first element contained in the specified range.
 * Returns NULL when no element is contained in the range. */
// 返回第一个满足range范围的entry节点的地址
unsigned char *zzlFirstInRange(unsigned char *zl, zrangespec *range) {
    unsigned char *eptr = ziplistIndex(zl,0), *sptr;    //保存首元素的地址，从前往后遍历
    double score;

    /* If everything is out of range, return early. */
    // 先判断是否至少有一个entry符合range，没有返回NULL
    if (!zzlIsInRange(zl,range)) return NULL;

    // 遍历ziplist的所有entry
    while (eptr != NULL) {
        // 保存当前元素分值的entry地址
        sptr = ziplistNext(zl,eptr);
        serverAssert(sptr != NULL);

        // 取出当前元素的分值
        score = zzlGetScore(sptr);
        // 如果分值大于range的最小值
        if (zslValueGteMin(score,range)) {
            /* Check if score <= max. */
            // 分值还小于range的最大值
            if (zslValueLteMax(score,range))
                // 返回当前节点的地址
                return eptr;
            return NULL;
        }

        /* Move to next element. */
        // 指向下一个节点
        eptr = ziplistNext(zl,sptr);
    }

    return NULL;
}

/* Find pointer to the last element contained in the specified range.
 * Returns NULL when no element is contained in the range. */
// 返回最后一个满足range范围的entry节点的地址
unsigned char *zzlLastInRange(unsigned char *zl, zrangespec *range) {
    unsigned char *eptr = ziplistIndex(zl,-2), *sptr;//保存尾元素的地址，从后往前遍历
    double score;

    /* If everything is out of range, return early. */
    // 先判断是否至少有一个entry符合range，没有返回NULL
    if (!zzlIsInRange(zl,range)) return NULL;

    // 遍历ziplist的所有entry
    while (eptr != NULL) {
        // 保存当前元素分值的entry地址
        sptr = ziplistNext(zl,eptr);
        serverAssert(sptr != NULL);
        // 取出当前元素的分值
        score = zzlGetScore(sptr);
        // 如果分值小于range的最大值
        if (zslValueLteMax(score,range)) {
            /* Check if score >= min. */
            // 分值还大于range的最小值
            if (zslValueGteMin(score,range))
                // 返回当前节点的地址
                return eptr;
            return NULL;
        }

        /* Move to previous element by moving to the score of previous element.
         * When this returns NULL, we know there also is no element. */
        // 指向前一个元素和元素的分值
        sptr = ziplistPrev(zl,eptr);
        if (sptr != NULL)
            serverAssert((eptr = ziplistPrev(zl,sptr)) != NULL);
        else
            eptr = NULL;
    }

    return NULL;
}

// 比较p指向的entry的值和规定范围spec，返回1表示大于spec的最大值
static int zzlLexValueGteMin(unsigned char *p, zlexrangespec *spec) {
    robj *value = ziplistGetObject(p);  //将entry节点构建成一个ziplist对象
    int res = zslLexValueGteMin(value,spec);    //比较
    decrRefCount(value);
    return res;
}

// 比较p指向的entry的值和规定范围spec，返回1表示小于spec的最小值
static int zzlLexValueLteMax(unsigned char *p, zlexrangespec *spec) {
    robj *value = ziplistGetObject(p);  //将entry节点构建成一个ziplist对象
    int res = zslLexValueLteMax(value,spec);    //比较
    decrRefCount(value);
    return res;
}

/* Returns if there is a part of the zset is in range. Should only be used
 * internally by zzlFirstInRange and zzlLastInRange. */
// 判断ziplist中是否有entry节点在range范围之内，有一个则返回1，否则返回0
int zzlIsInLexRange(unsigned char *zl, zlexrangespec *range) {
    unsigned char *p;

    /* Test for ranges that will always be empty. */
    // 测试字典序范围是否合法
    if (compareStringObjectsForLexRange(range->min,range->max) > 1 ||
            (compareStringObjects(range->min,range->max) == 0 &&
            (range->minex || range->maxex)))
        return 0;

    // 最后一个元素的地址
    p = ziplistIndex(zl,-2); /* Last element. */
    if (p == NULL) return 0;
    // 小于range的最小值，返回0
    if (!zzlLexValueGteMin(p,range))
        return 0;

    // 第一个元素地址
    p = ziplistIndex(zl,0); /* First element. */
    serverAssert(p != NULL);
    // 大于range的最大值，返回0
    if (!zzlLexValueLteMax(p,range))
        return 0;

    return 1;
}

/* Find pointer to the first element contained in the specified lex range.
 * Returns NULL when no element is contained in the range. */
// 返回第一个满足range范围的元素的地址
unsigned char *zzlFirstInLexRange(unsigned char *zl, zlexrangespec *range) {
    unsigned char *eptr = ziplistIndex(zl,0), *sptr;    //第一个元素节点地址，从前往后遍历

    /* If everything is out of range, return early. */
    // 判断ziplist中是否有entry节点在range范围之内，有一个则返回1，否则返回0
    if (!zzlIsInLexRange(zl,range)) return NULL;

    // 遍历所有元素节点
    while (eptr != NULL) {
        // 大于最小值且小于最大值，返回当前节点地址
        if (zzlLexValueGteMin(eptr,range)) {
            /* Check if score <= max. */
            if (zzlLexValueLteMax(eptr,range))
                return eptr;
            return NULL;
        }

        /* Move to next element. */
        //跳过分值节点
        sptr = ziplistNext(zl,eptr); /* This element score. Skip it. */
        serverAssert(sptr != NULL);
        // 指向下一个元素节点
        eptr = ziplistNext(zl,sptr); /* Next element. */
    }

    return NULL;
}

/* Find pointer to the last element contained in the specified lex range.
 * Returns NULL when no element is contained in the range. */
// 返回最后一个满足range范围的元素的地址
unsigned char *zzlLastInLexRange(unsigned char *zl, zlexrangespec *range) {
    unsigned char *eptr = ziplistIndex(zl,-2), *sptr;   //最后一个元素节点地址，从后往前遍历

    /* If everything is out of range, return early. */
    // 判断ziplist中是否有entry节点在range范围之内，有一个则返回1，否则返回0
    if (!zzlIsInLexRange(zl,range)) return NULL;

    // 遍历所有元素节点
    while (eptr != NULL) {
        // 小于于最大值且大于最小值，返回当前节点地址
        if (zzlLexValueLteMax(eptr,range)) {
            /* Check if score >= min. */
            if (zzlLexValueGteMin(eptr,range))
                return eptr;
            return NULL;
        }

        /* Move to previous element by moving to the score of previous element.
         * When this returns NULL, we know there also is no element. */
        // eptr指向前一个元素节点
        sptr = ziplistPrev(zl,eptr);
        if (sptr != NULL)
            serverAssert((eptr = ziplistPrev(zl,sptr)) != NULL);
        else
            eptr = NULL;
    }

    return NULL;
}

// 从ziplist中查找ele，将分值保存在score中
unsigned char *zzlFind(unsigned char *zl, robj *ele, double *score) {
    unsigned char *eptr = ziplistIndex(zl,0), *sptr;    //第一个元素节点地址，从前往后遍历

    //得到字符串类型的ele对象
    ele = getDecodedObject(ele);
    while (eptr != NULL) {
        // 指向元素分值节点的地址
        sptr = ziplistNext(zl,eptr);
        serverAssertWithInfo(NULL,ele,sptr != NULL);

        // 比较，如果找到
        if (ziplistCompare(eptr,ele->ptr,sdslen(ele->ptr))) {
            /* Matching element, pull out score. */
            if (score != NULL) *score = zzlGetScore(sptr);  //保存元素的分值
            decrRefCount(ele);
            return eptr;    //返回元素节点的地址
        }

        /* Move to next element. */
        eptr = ziplistNext(zl,sptr);    //没找到则指向下一个元素节点的地址
    }

    decrRefCount(ele);
    return NULL;
}

/* Delete (element,score) pair from ziplist. Use local copy of eptr because we
 * don't want to modify the one given as argument. */
// 删除eptr制定的元素和分值从ziplist中
unsigned char *zzlDelete(unsigned char *zl, unsigned char *eptr) {
    unsigned char *p = eptr;

    /* TODO: add function to ziplist API to delete N elements from offset. */
    // 删除元素和分值
    zl = ziplistDelete(zl,&p);
    zl = ziplistDelete(zl,&p);
    return zl;
}

//将ele元素和分值score插入到eptr指向节点的前面
unsigned char *zzlInsertAt(unsigned char *zl, unsigned char *eptr, robj *ele, double score) {
    unsigned char *sptr;
    char scorebuf[128];
    int scorelen;
    size_t offset;

    serverAssertWithInfo(NULL,ele,sdsEncodedObject(ele));
    // 讲score转换为字符串，保存到scorebuf中，并计算score的字节长度
    scorelen = d2string(scorebuf,sizeof(scorebuf),score);

    // 如果eptr为空，则加到ziplist的尾部
    if (eptr == NULL) {
        zl = ziplistPush(zl,ele->ptr,sdslen(ele->ptr),ZIPLIST_TAIL);    //添加元素节点
        zl = ziplistPush(zl,(unsigned char*)scorebuf,scorelen,ZIPLIST_TAIL);    //添加分值

    // 插入在eptr节点的前面
    } else {
        /* Keep offset relative to zl, as it might be re-allocated. */
        // 备份当eptr指向节点的偏移量，防止内存被重新分配
        offset = eptr-zl;
        // 将元素插入在eptr前面
        zl = ziplistInsert(zl,eptr,ele->ptr,sdslen(ele->ptr));
        eptr = zl+offset;

        /* Insert score after the element. */
        // 找到分值节点的地址
        serverAssertWithInfo(NULL,ele,(sptr = ziplistNext(zl,eptr)) != NULL);
        // 插入分值
        zl = ziplistInsert(zl,sptr,(unsigned char*)scorebuf,scorelen);
    }

    return zl;
}

/* Insert (element,score) pair in ziplist. This function assumes the element is
 * not yet present in the list. */
//将ele元素和分值score插入在ziplist中，从小到大排序
unsigned char *zzlInsert(unsigned char *zl, robj *ele, double score) {
    unsigned char *eptr = ziplistIndex(zl,0), *sptr;    //第一个元素节点地址，从前往后遍历
    double s;

    ele = getDecodedObject(ele);    //得到字符串类型的ele对象
    // 遍历节点
    while (eptr != NULL) {
        // 元素分值节点的地址
        sptr = ziplistNext(zl,eptr);
        serverAssertWithInfo(NULL,ele,sptr != NULL);
        // 得到分值
        s = zzlGetScore(sptr);

        // 最小分值大于score，前插入
        if (s > score) {
            /* First element with score larger than score for element to be
             * inserted. This means we should take its spot in the list to
             * maintain ordering. */
            zl = zzlInsertAt(zl,eptr,ele,score);
            break;
        // 分值相等
        } else if (s == score) {
            /* Ensure lexicographical ordering for elements. */
            // 比较字典序，如果要插入的元素小于eptr指向的元素，前插入
            if (zzlCompareElements(eptr,ele->ptr,sdslen(ele->ptr)) > 0) {
                zl = zzlInsertAt(zl,eptr,ele,score);
                break;
            }
        }

        /* Move to next element. */
        // 指向下一个元素节点
        eptr = ziplistNext(zl,sptr);
    }

    /* Push on tail of list when it was not yet inserted. */
    // 如果所有节点都不能前插，那么将其插入在ziplist的尾部
    if (eptr == NULL)
        zl = zzlInsertAt(zl,NULL,ele,score);

    decrRefCount(ele);
    return zl;
}

// 删除ziplist中分值在制定范围内的元素，并保存删除的数量在deleted中
unsigned char *zzlDeleteRangeByScore(unsigned char *zl, zrangespec *range, unsigned long *deleted) {
    unsigned char *eptr, *sptr;
    double score;
    unsigned long num = 0;

    if (deleted != NULL) *deleted = 0;  //初始化

    // 指向第一个符合range范围的节点
    eptr = zzlFirstInRange(zl,range);
    if (eptr == NULL) return zl;

    /* When the tail of the ziplist is deleted, eptr will point to the sentinel
     * byte and ziplistNext will return NULL. */
    // 循环删除节点
    while ((sptr = ziplistNext(zl,eptr)) != NULL) {
        score = zzlGetScore(sptr);  //得到当前元素的分值
        // 如果元素分值在range范围内，则删除元素和分值
        if (zslValueLteMax(score,range)) {
            /* Delete both the element and the score. */
            zl = ziplistDelete(zl,&eptr);
            zl = ziplistDelete(zl,&eptr);
            //计数删除的节点个数
            num++;
        } else {
            /* No longer in range. */
            break;
        }
    }

    if (deleted != NULL) *deleted = num;
    return zl;
}

// 删除ziplist中分值在制定字典序范围内的元素，并保存删除的数量在deleted中
unsigned char *zzlDeleteRangeByLex(unsigned char *zl, zlexrangespec *range, unsigned long *deleted) {
    unsigned char *eptr, *sptr;
    unsigned long num = 0;

    if (deleted != NULL) *deleted = 0;  //初始化

    // 指向第一个符合range范围的元素节点
    eptr = zzlFirstInLexRange(zl,range);
    if (eptr == NULL) return zl;

    /* When the tail of the ziplist is deleted, eptr will point to the sentinel
     * byte and ziplistNext will return NULL. */
    // 循环删除节点
    while ((sptr = ziplistNext(zl,eptr)) != NULL) {
        // 如果元素在range范围内，则删除元素和分值
        if (zzlLexValueLteMax(eptr,range)) {
            /* Delete both the element and the score. */
            zl = ziplistDelete(zl,&eptr);
            zl = ziplistDelete(zl,&eptr);
            num++; //计数删除的节点个数
        } else {
            /* No longer in range. */
            break;
        }
    }

    if (deleted != NULL) *deleted = num;
    return zl;
}

/* Delete all the elements with rank between start and end from the skiplist.
 * Start and end are inclusive. Note that start and end need to be 1-based */
// 删除给定下标范围内的所有元素
unsigned char *zzlDeleteRangeByRank(unsigned char *zl, unsigned int start, unsigned int end, unsigned long *deleted) {
    unsigned int num = (end-start)+1;   //计算删除的个数
    if (deleted) *deleted = num;
    // 有序集合的下标从1开始，所以start要减1
    zl = ziplistDeleteRange(zl,2*(start-1),2*num);
    return zl;
}

/*-----------------------------------------------------------------------------
 * Common sorted set API
 * 有序集合API
 *----------------------------------------------------------------------------*/
// 返回有序集合的元素个数
unsigned int zsetLength(robj *zobj) {
    int length = -1;
    // 如果是压缩列表
    if (zobj->encoding == OBJ_ENCODING_ZIPLIST) {
        length = zzlLength(zobj->ptr);
    // 如果是跳跃表
    } else if (zobj->encoding == OBJ_ENCODING_SKIPLIST) {
        length = ((zset*)zobj->ptr)->zsl->length;
    } else {
        serverPanic("Unknown sorted set encoding");
    }
    return length;
}

// 将有序集合对象的编码转换为encoding制定的编码类型
void zsetConvert(robj *zobj, int encoding) {
    zset *zs;
    zskiplistNode *node, *next;
    robj *ele;
    double score;

    if (zobj->encoding == encoding) return;
    // 从ziplist转换到skiplist
    if (zobj->encoding == OBJ_ENCODING_ZIPLIST) {
        unsigned char *zl = zobj->ptr;
        unsigned char *eptr, *sptr;
        unsigned char *vstr;
        unsigned int vlen;
        long long vlong;

        if (encoding != OBJ_ENCODING_SKIPLIST)
            serverPanic("Unknown target encoding");

        // 分配有序集合空间
        zs = zmalloc(sizeof(*zs));
        // 初始化字典和跳跃表成员
        zs->dict = dictCreate(&zsetDictType,NULL);
        zs->zsl = zslCreate();

        // 压缩列表的首元素指针
        eptr = ziplistIndex(zl,0);
        serverAssertWithInfo(NULL,zobj,eptr != NULL);
        // 压缩列表的首元素分值指针
        sptr = ziplistNext(zl,eptr);
        serverAssertWithInfo(NULL,zobj,sptr != NULL);

        // 遍历压缩列表的entry
        while (eptr != NULL) {
            score = zzlGetScore(sptr);  //取出分值
            serverAssertWithInfo(NULL,zobj,ziplistGet(eptr,&vstr,&vlen,&vlong));    //取出元素
            // 根据字符串或整数创建字符串类型对象
            if (vstr == NULL)
                ele = createStringObjectFromLongLong(vlong);
            else
                ele = createStringObject((char*)vstr,vlen);

            /* Has incremented refcount since it was just created. */
            // 将分值和成员插入到跳跃表中
            node = zslInsert(zs->zsl,score,ele);
            // 将分值和成员添加到字典中
            serverAssertWithInfo(NULL,zobj,dictAdd(zs->dict,ele,&node->score) == DICT_OK);
            incrRefCount(ele); /* Added to dictionary. */
            zzlNext(zl,&eptr,&sptr);    //更新元素和分值的指针
        }

        // 转换到skiplist编码后，将原来ziplist的空间释放
        zfree(zobj->ptr);
        // 设置编码和有序集合的值
        zobj->ptr = zs;
        zobj->encoding = OBJ_ENCODING_SKIPLIST;

    // 从OBJ_ENCODING_SKIPLIST转换到OBJ_ENCODING_ZIPLIST
    } else if (zobj->encoding == OBJ_ENCODING_SKIPLIST) {
        unsigned char *zl = ziplistNew();   //创建新的压缩列表

        if (encoding != OBJ_ENCODING_ZIPLIST)
            serverPanic("Unknown target encoding");

        /* Approach similar to zslFree(), since we want to free the skiplist at
         * the same time as creating the ziplist. */
        // 指向有序集合的地址
        zs = zobj->ptr;
        dictRelease(zs->dict);  //释放有序集合的字典，根据跳跃表遍历即可
        // 指向跳跃表的首节点
        node = zs->zsl->header->level[0].forward;
        // 释放表头节点和表头空间
        zfree(zs->zsl->header);
        zfree(zs->zsl);

        //遍历跳跃表的节点
        while (node) {
            // 将节点对象解码成字符串对象
            ele = getDecodedObject(node->obj);
            // 将当前元素和分值尾插到列表中
            zl = zzlInsertAt(zl,NULL,ele,node->score);
            decrRefCount(ele);

            // 备份下一个节点地址
            next = node->level[0].forward;
            // 释放当前节点空间
            zslFreeNode(node);
            // 指向下一个节点
            node = next;
        }

        zfree(zs);  //释放原来skiplist编码的有序集合
        // 设置新的有序集合和编码
        zobj->ptr = zl;
        zobj->encoding = OBJ_ENCODING_ZIPLIST;
    } else {
        serverPanic("Unknown sorted set encoding");
    }
}

/* Convert the sorted set object into a ziplist if it is not already a ziplist
 * and if the number of elements and the maximum element size is within the
 * expected ranges. */
// 按需转换编码成OBJ_ENCODING_ZIPLIST
void zsetConvertToZiplistIfNeeded(robj *zobj, size_t maxelelen) {
    // 如果已经是OBJ_ENCODING_ZIPLIST编码直接返回
    if (zobj->encoding == OBJ_ENCODING_ZIPLIST) return;
    zset *zset = zobj->ptr;

    //zset_max_ziplist_entries默认为128，表示最ziplist中最多entry的数量
    // zset_max_ziplist_value默认为64，表示ziplist一个entry的value最多为64字节
    if (zset->zsl->length <= server.zset_max_ziplist_entries &&
        maxelelen <= server.zset_max_ziplist_value)
            zsetConvert(zobj,OBJ_ENCODING_ZIPLIST);
}

/* Return (by reference) the score of the specified member of the sorted set
 * storing it into *score. If the element does not exist C_ERR is returned
 * otherwise C_OK is returned and *score is correctly populated.
 * If 'zobj' or 'member' is NULL, C_ERR is returned. */
// 将有序集合的member成员的分值保存到score中
int zsetScore(robj *zobj, robj *member, double *score) {
    if (!zobj || !member) return C_ERR;

    // ziplist中
    if (zobj->encoding == OBJ_ENCODING_ZIPLIST) {
        // 找到保存
        if (zzlFind(zobj->ptr, member, score) == NULL) return C_ERR;
    // 跳跃表中
    } else if (zobj->encoding == OBJ_ENCODING_SKIPLIST) {
        zset *zs = zobj->ptr;
        dictEntry *de = dictFind(zs->dict, member);//找保存member对象的节点
        if (de == NULL) return C_ERR;
        *score = *(double*)dictGetVal(de);  //取出分数保存在score中
    } else {
        serverPanic("Unknown sorted set encoding");
    }
    return C_OK;
}

/*-----------------------------------------------------------------------------
 * Sorted set commands
 * 有序集合命令实现
 *----------------------------------------------------------------------------*/

/* This generic command implements both ZADD and ZINCRBY. */
#define ZADD_NONE 0         /* 不设置标志*/
#define ZADD_INCR (1<<0)    /* 设置incr标志，Increment the score instead of setting it. */
#define ZADD_NX (1<<1)      /* 只有当前元素不存在，才会设置，Don't touch elements not already existing. */
#define ZADD_XX (1<<2)      /* 只有当前元素存在，才会设置，Only touch elements already exisitng. */
#define ZADD_CH (1<<3)      /* 返回条件或更新元素的个数，Return num of elements added or updated. */

// ZADD key [NX|XX] [CH] [INCR] score member [[score member] [score member] ...]
// ZINCRBY key increment member
// ZADD、ZINCRBY命令的底层实现
void zaddGenericCommand(client *c, int flags) {
    static char *nanerr = "resulting score is not a number (NaN)";
    robj *key = c->argv[1];
    robj *ele;
    robj *zobj;
    robj *curobj;
    double score = 0, *scores = NULL, curscore = 0.0;
    int j, elements;
    int scoreidx = 0;
    /* The following vars are used in order to track what the command actually
     * did during the execution, to reply to the client and to trigger the
     * notification of keyspace change. */
    int added = 0;      /* Number of new elements added. */
    int updated = 0;    /* Number of elements with updated score. */
    int processed = 0;  /* Number of elements processed, may remain zero with
                           options like XX. */

    /* Parse options. At the end 'scoreidx' is set to the argument position
     * of the score of the first score-element pair. */
    scoreidx = 2;   //从下标为2的参数开始解析，分别设置相对应的参数选项
    while(scoreidx < c->argc) {
        char *opt = c->argv[scoreidx]->ptr;
        if (!strcasecmp(opt,"nx")) flags |= ZADD_NX;
        else if (!strcasecmp(opt,"xx")) flags |= ZADD_XX;
        else if (!strcasecmp(opt,"ch")) flags |= ZADD_CH;
        else if (!strcasecmp(opt,"incr")) flags |= ZADD_INCR;
        else break;
        scoreidx++; //获得第一个score的参数下标
    }

    /* Turn options into simple to check vars. */
    // 设置对应的变量
    int incr = (flags & ZADD_INCR) != 0;
    int nx = (flags & ZADD_NX) != 0;
    int xx = (flags & ZADD_XX) != 0;
    int ch = (flags & ZADD_CH) != 0;

    /* After the options, we expect to have an even number of args, since
     * we expect any number of score-element pairs. */
    // 计算元素个数
    elements = c->argc-scoreidx;
    // 元素个数必须能整除2，且不能为0，否则发送语法错误信息给client
    if (elements % 2 || !elements) {
        addReply(c,shared.syntaxerr);
        return;
    }
    // 计算score-element对的个数
    elements /= 2; /* Now this holds the number of score-element pairs. */

    /* Check for incompatible options. */
    // NX和XX不能同时设置，否则发送错误信息
    if (nx && xx) {
        addReplyError(c,
            "XX and NX options at the same time are not compatible");
        return;
    }

    // INCR之支持一对score-element
    if (incr && elements > 1) {
        addReplyError(c,
            "INCR option supports a single increment-element pair");
        return;
    }

    /* Start parsing all the scores, we need to emit any syntax error
     * before executing additions to the sorted set, as the command should
     * either execute fully or nothing at all. */
    // 分配所有分数的空间，分数数组
    scores = zmalloc(sizeof(double)*elements);
    // 遍历所有的分数，将所有字符串类型的分数值转换double类型，保存在分数数组中
    for (j = 0; j < elements; j++) {
        if (getDoubleFromObjectOrReply(c,c->argv[scoreidx+j*2],&scores[j],NULL)
            != C_OK) goto cleanup;  //如果上述操作不成功，跳转到cleanup代码
    }

    /* Lookup the key and create the sorted set if does not exist. */
    // 以写操作取出有序集合对象
    zobj = lookupKeyWrite(c->db,key);

    // 如果当前有序集合对象不存在
    if (zobj == NULL) {
        // 如果设置了XX标志，跳转到reply_to_client，回复client
        if (xx) goto reply_to_client; /* No key + XX option: nothing to do. */
        // 根据不同的配置，创建不同编码的有序集合对象
        if (server.zset_max_ziplist_entries == 0 ||
            server.zset_max_ziplist_value < sdslen(c->argv[scoreidx+1]->ptr))
        {
            zobj = createZsetObject();
        } else {
            zobj = createZsetZiplistObject();
        }
        // 将key和zobj(value)添加到数据库中
        dbAdd(c->db,key,zobj);

    //有序集合对象存在
    } else {
        // 检查有序集合对象的数据类型
        if (zobj->type != OBJ_ZSET) {
            addReply(c,shared.wrongtypeerr);    //发送类型错误信息，跳转清理空间代码
            goto cleanup;
        }
    }

    // 遍历所有的元素
    for (j = 0; j < elements; j++) {
        score = scores[j];  //当前元素的分值

        // 如果为ziplist
        if (zobj->encoding == OBJ_ENCODING_ZIPLIST) {
            unsigned char *eptr;

            /* Prefer non-encoded element when dealing with ziplists. */
            // 元素的下标
            ele = c->argv[scoreidx+1+j*2];
            // 查找ele元素，保存分值在curscore，则要更新元素分值
            // 1. 执行更新update操作
            if ((eptr = zzlFind(zobj->ptr,ele,&curscore)) != NULL) {
                if (nx) continue;   //NX标志，跳过循环
                if (incr) {         //INCR标志，更新分值
                    score += curscore;
                    // 检查分值是否为number
                    if (isnan(score)) {
                        addReplyError(c,nanerr);
                        goto cleanup;
                    }
                }

                /* Remove and re-insert when score changed. */
                // 如果分值发生变化，则现将原来的元素和分值删除，然后在插入元素和新的分值
                if (score != curscore) {
                    zobj->ptr = zzlDelete(zobj->ptr,eptr);
                    zobj->ptr = zzlInsert(zobj->ptr,ele,score);
                    server.dirty++;     //更新脏键
                    updated++;          //计数更新分值的次数
                }
                processed++;    //计数加工的次数

            // 2. 没找到的情况，则执行添加add操作
            // 如果没有设置XX标志，意味着，要将ele和分值插入到ziplist中，如果entry数会发生变化，要转换编码
            } else if (!xx) {
                /* Optimize: check if the element is too large or the list
                 * becomes too long *before* executing zzlInsert. */
                //将ele元素和分值score插入在ziplist中，从小到大排序
                zobj->ptr = zzlInsert(zobj->ptr,ele,score);
                //查看ziplist元素数量，是否达到转换编码的阈值条件，达到条件要转换成skiplist的编码
                if (zzlLength(zobj->ptr) > server.zset_max_ziplist_entries)
                    zsetConvert(zobj,OBJ_ENCODING_SKIPLIST);
                // 查看新加入元素的数量，是否达到转换编码的阈值条件，达到条件要转换成skiplist的编码
                if (sdslen(ele->ptr) > server.zset_max_ziplist_value)
                    zsetConvert(zobj,OBJ_ENCODING_SKIPLIST);
                server.dirty++; //更新脏键
                added++;        //计算加入的节点数量
                processed++;    //计数加工的次数
            }

        // 如果是skiplist
        } else if (zobj->encoding == OBJ_ENCODING_SKIPLIST) {
            zset *zs = zobj->ptr;
            zskiplistNode *znode;
            dictEntry *de;

            // 将当前元素优化编码
            ele = c->argv[scoreidx+1+j*2] =
                tryObjectEncoding(c->argv[scoreidx+1+j*2]);
            // 在字典中查找保存当前元素的节点
            de = dictFind(zs->dict,ele);

            // 1. 在字典中能找到保存元素节点
            // 进行更新update操作
            if (de != NULL) {
                if (nx) continue;   //NX标志，跳过循环
                curobj = dictGetKey(de);    //当前的元素
                curscore = *(double*)dictGetVal(de);    //当前元素的分数

                if (incr) {         //INCR标志，更新更新分值
                    score += curscore;
                    // 检查分值是否为number
                    if (isnan(score)) {
                        addReplyError(c,nanerr);
                        /* Don't need to check if the sorted set is empty
                         * because we know it has at least one element. */
                        goto cleanup;
                    }
                }

                /* Remove and re-insert when score changed. We can safely
                 * delete the key object from the skiplist, since the
                 * dictionary still has a reference to it. */
                // 如果分值发生变化，则现将原来的元素和分值删除，然后在插入元素和新的分值
                if (score != curscore) {
                    // 删除跳跃表中分值和元素
                    serverAssertWithInfo(c,curobj,zslDelete(zs->zsl,curscore,curobj));
                    // 插入新的元素和分值到跳跃表中
                    znode = zslInsert(zs->zsl,score,curobj);
                    incrRefCount(curobj); /* Re-inserted in skiplist. */
                    //更新字典中节点的分值
                    dictGetVal(de) = &znode->score; /* Update score ptr. */
                    server.dirty++; //更新脏键
                    updated++;      //计算更新分值的次数
                }
                processed++;        //计算加工的次数

            // 2. 在字典中没找到元素，进行添加add操作
            // 如果没有设置XX标志，意味着，要将ele和score分值插入到skiplist中
            } else if (!xx) {
                //将score和ele插入到跳跃表中
                znode = zslInsert(zs->zsl,score,ele);
                incrRefCount(ele); /* Inserted in skiplist. */
                // 同时也要加入到字典中
                serverAssertWithInfo(c,NULL,dictAdd(zs->dict,ele,&znode->score) == DICT_OK);
                incrRefCount(ele); /* Added to dictionary. */
                server.dirty++; //更新脏键
                added++;        //计算加入的个数
                processed++;    //计算加工的次数
            }
        } else {
            serverPanic("Unknown sorted set encoding");
        }
    }

//回复client的代码
reply_to_client:
    if (incr) { /* ZINCRBY or INCR option. */
        if (processed)
            addReplyDouble(c,score);        //发送更新后的分值
        else
            addReply(c,shared.nullbulk);
    } else { /* ZADD. */
        addReplyLongLong(c,ch ? added+updated : added); //发送添加或更新的个数
    }

// 清理操作的代码
cleanup:
    zfree(scores);
    if (added || updated) {
        // 发送信号当键被修改
        signalModifiedKey(c->db,key);
        // 发送对应的事件通知
        notifyKeyspaceEvent(NOTIFY_ZSET,
            incr ? "zincr" : "zadd", key, c->db->id);
    }
}

// ZADD key score member [[score member] [score member] ...]
// ZADD 命令实现
void zaddCommand(client *c) {
    zaddGenericCommand(c,ZADD_NONE);
}

// ZINCRBY key increment member
// ZINCRBY命令实现
void zincrbyCommand(client *c) {
    zaddGenericCommand(c,ZADD_INCR);
}

// ZREM key member [member ...]
// ZREM命令实现
void zremCommand(client *c) {
    robj *key = c->argv[1];
    robj *zobj;
    int deleted = 0, keyremoved = 0, j;

    // 以写操作取出有序集合对象，并且检查对象的数据类型
    if ((zobj = lookupKeyWriteOrReply(c,key,shared.czero)) == NULL ||
        checkType(c,zobj,OBJ_ZSET)) return;

    // ziplist中删除
    if (zobj->encoding == OBJ_ENCODING_ZIPLIST) {
        unsigned char *eptr;

        // 遍历所有的元素
        for (j = 2; j < c->argc; j++) {
            // 如果在ziplist中找到
            if ((eptr = zzlFind(zobj->ptr,c->argv[j],NULL)) != NULL) {
                deleted++;  //计算删除的个数
                zobj->ptr = zzlDelete(zobj->ptr,eptr);  //删除元素和分值
                // 如果ziplist删除后为空，则要从数据库中删除该有序集合对象
                if (zzlLength(zobj->ptr) == 0) {
                    dbDelete(c->db,key);
                    keyremoved = 1;     //删除有序集合对象的标志
                    break;
                }
            }
        }
    // skiplist删除
    } else if (zobj->encoding == OBJ_ENCODING_SKIPLIST) {
        zset *zs = zobj->ptr;
        dictEntry *de;
        double score;

        // 遍历所有的元素
        for (j = 2; j < c->argc; j++) {
            // 在字典中查找保存该元素的节点地址
            de = dictFind(zs->dict,c->argv[j]);
            // 找到进行
            if (de != NULL) {
                deleted++;  //计算删除的个数

                /* Delete from the skiplist */
                score = *(double*)dictGetVal(de);   //被删除元素的分数
                // 从跳跃表中删除
                serverAssertWithInfo(c,c->argv[j],zslDelete(zs->zsl,score,c->argv[j]));

                /* Delete from the hash table */
                // 从字典中删除
                dictDelete(zs->dict,c->argv[j]);
                // 判断是否需要进行缩小字典的大小
                if (htNeedsResize(zs->dict)) dictResize(zs->dict);
                // 如果字典为空，则从数据库中删除该有序集合对象
                if (dictSize(zs->dict) == 0) {
                    dbDelete(c->db,key);
                    keyremoved = 1; //删除有序集合对象的标志
                    break;
                }
            }
        }
    } else {
        serverPanic("Unknown sorted set encoding");
    }

    // 如果至少有一个member被删除，则发送"zrem"事件通知
    if (deleted) {
        notifyKeyspaceEvent(NOTIFY_ZSET,"zrem",key,c->db->id);
        // 如果有序集合被删除
        if (keyremoved)
            // 发送"del"通知
            notifyKeyspaceEvent(NOTIFY_GENERIC,"del",key,c->db->id);
        // 键被修改，发送新号
        signalModifiedKey(c->db,key);
        server.dirty += deleted;
    }
    addReplyLongLong(c,deleted);    //发送删除的个数给client
}

/* Implements ZREMRANGEBYRANK, ZREMRANGEBYSCORE, ZREMRANGEBYLEX commands. */
#define ZRANGE_RANK 0       //下标排位的范围
#define ZRANGE_SCORE 1      //分数排位的范围
#define ZRANGE_LEX 2        //字典序排位的范围
//  ZREMRANGEBYRANK, ZREMRANGEBYSCORE, ZREMRANGEBYLEX命令的底层实现
void zremrangeGenericCommand(client *c, int rangetype) {
    robj *key = c->argv[1];
    robj *zobj;
    int keyremoved = 0;
    unsigned long deleted = 0;
    zrangespec range;
    zlexrangespec lexrange;
    long start, end, llen;

    /* Step 1: Parse the range. */
    // 1. 更加不同排位方式，解析范围
    if (rangetype == ZRANGE_RANK) {     //下标排位的范围，获取start起始和end结束
        if ((getLongFromObjectOrReply(c,c->argv[2],&start,NULL) != C_OK) ||
            (getLongFromObjectOrReply(c,c->argv[3],&end,NULL) != C_OK))
            return;
    } else if (rangetype == ZRANGE_SCORE) { //分数排位的范围，保存在zrangespec中
        if (zslParseRange(c->argv[2],c->argv[3],&range) != C_OK) {
            addReplyError(c,"min or max is not a float");
            return;
        }
    } else if (rangetype == ZRANGE_LEX) {   //字典序排位的范围，保存在zlexrangespec中
        if (zslParseLexRange(c->argv[2],c->argv[3],&lexrange) != C_OK) {
            addReplyError(c,"min or max not valid string range item");
            return;
        }
    }

    /* Step 2: Lookup & range sanity checks if needed. */
    // 2. 进行合法的范围检查
    // 以写操作读取有序集合对象并且进行对象的数据类型检查
    if ((zobj = lookupKeyWriteOrReply(c,key,shared.czero)) == NULL ||
        checkType(c,zobj,OBJ_ZSET)) goto cleanup;

    // 下标排位的方式
    if (rangetype == ZRANGE_RANK) {
        /* Sanitize indexes. */
        // 获取有序集合的元素个数
        llen = zsetLength(zobj);
        // 对起始start和结束end位置，进行调整和修剪
        if (start < 0) start = llen+start;
        if (end < 0) end = llen+end;
        if (start < 0) start = 0;

        /* Invariant: start >= 0, so this test will be true when end < 0.
         * The range is empty when start > end or start >= length. */
        // 非法的范围，发送0给client
        if (start > end || start >= llen) {
            addReply(c,shared.czero);
            goto cleanup;
        }
        // 结束位置不能超有序集合的长度
        if (end >= llen) end = llen-1;
    }

    /* Step 3: Perform the range deletion operation. */
    // 3. 执行范围删除的操作
    // 从ziplist删除范围内的元素
    if (zobj->encoding == OBJ_ENCODING_ZIPLIST) {
        switch(rangetype) {     //根据不同的排位方式，调用不同的函数进行删除
        case ZRANGE_RANK:
            zobj->ptr = zzlDeleteRangeByRank(zobj->ptr,start+1,end+1,&deleted);
            break;
        case ZRANGE_SCORE:
            zobj->ptr = zzlDeleteRangeByScore(zobj->ptr,&range,&deleted);
            break;
        case ZRANGE_LEX:
            zobj->ptr = zzlDeleteRangeByLex(zobj->ptr,&lexrange,&deleted);
            break;
        }
        // 如果删除后，ziplist为空，则要删除该有序集合
        if (zzlLength(zobj->ptr) == 0) {
            dbDelete(c->db,key);
            keyremoved = 1; //设置标志
        }
    // 从skiplist删除范围内的元素
    } else if (zobj->encoding == OBJ_ENCODING_SKIPLIST) {
        zset *zs = zobj->ptr;
        switch(rangetype) { // 根据不同的排位方式，调用不同的函数进行删除
        case ZRANGE_RANK:
            deleted = zslDeleteRangeByRank(zs->zsl,start+1,end+1,zs->dict);
            break;
        case ZRANGE_SCORE:
            deleted = zslDeleteRangeByScore(zs->zsl,&range,zs->dict);
            break;
        case ZRANGE_LEX:
            deleted = zslDeleteRangeByLex(zs->zsl,&lexrange,zs->dict);
            break;
        }

        // 判断是否需要进行缩小字典的大小
        if (htNeedsResize(zs->dict)) dictResize(zs->dict);
        // 如果字典为空，则从数据库中删除该有序集合对象
        if (dictSize(zs->dict) == 0) {
            dbDelete(c->db,key);
            keyremoved = 1; //删除有序集合对象的标志
        }
    } else {
        serverPanic("Unknown sorted set encoding");
    }

    /* Step 4: Notifications and reply. */
    // 4.发送通知和回复
    // 如果删除成功
    if (deleted) {
        // 这个代码666
        // 发送对应的时间通知和信号
        char *event[3] = {"zremrangebyrank","zremrangebyscore","zremrangebylex"};
        signalModifiedKey(c->db,key);
        notifyKeyspaceEvent(NOTIFY_ZSET,event[rangetype],key,c->db->id);
        if (keyremoved) //如果集合对象被删除，则发送"del"的事件通知
            notifyKeyspaceEvent(NOTIFY_GENERIC,"del",key,c->db->id);
    }
    server.dirty += deleted;    //更新脏键
    addReplyLongLong(c,deleted);    //发送删除的个数给client

// 清理空间代码
cleanup:
    if (rangetype == ZRANGE_LEX) zslFreeLexRange(&lexrange);    //字典序的max和min都是对象，所以要释放空间
}

// ZREMRANGEBYRANK key start stop
// ZREMRANGEBYRANK命令实现
void zremrangebyrankCommand(client *c) {
    zremrangeGenericCommand(c,ZRANGE_RANK);
}

// ZREMRANGEBYSCORE key min max
// ZREMRANGEBYSCORE命令实现
void zremrangebyscoreCommand(client *c) {
    zremrangeGenericCommand(c,ZRANGE_SCORE);
}

// ZREMRANGEBYLEX key min max
// ZREMRANGEBYLEX命令实现
void zremrangebylexCommand(client *c) {
    zremrangeGenericCommand(c,ZRANGE_LEX);
}

// 集合类型的迭代器
typedef struct {
    robj *subject;                  //所属的对象
    int type; /* Set, sorted set */ //对象的类型，可以是集合或有序集合
    int encoding;                   //编码
    double weight;                  //权重

    union {
        /* Set iterators. */        //集合迭代器
        union _iterset {
            struct {                //intset迭代器
                intset *is;         //所属的intset
                int ii;             //intset节点下标
            } is;
            struct {                //字典迭代器
                dict *dict;         //所属的字典
                dictIterator *di;   //字典的迭代器
                dictEntry *de;      //当前指向的字典节点
            } ht;
        } set;

        /* Sorted set iterators. */         //有序集合迭代器
        union _iterzset {                   //ziplist迭代器
            struct {
                unsigned char *zl;          //所属的ziplist
                unsigned char *eptr, *sptr; //当前指向的元素和分值
            } zl;
            struct {                        //zset迭代器
                zset *zs;                   //所属的zset
                zskiplistNode *node;        //当前指向的跳跃节点
            } sl;
        } zset;
    } iter;
} zsetopsrc;


/* Use dirty flags for pointers that need to be cleaned up in the next
 * iteration over the zsetopval. The dirty flag for the long long value is
 * special, since long long values don't need cleanup. Instead, it means that
 * we already checked that "ell" holds a long long, or tried to convert another
 * representation into a long long value. When this was successful,
 * OPVAL_VALID_LL is set as well. */
#define OPVAL_DIRTY_ROBJ 1      //脏对象标志，下次迭代之前要清理
#define OPVAL_DIRTY_LL 2        //脏整数标志，不需要被清理
#define OPVAL_VALID_LL 4        //有效整数标志，表示转换成功

/* Store value retrieved from the iterator. */
// 从集合类型迭代器中恢复存储的值
typedef struct {
    int flags;
    unsigned char _buf[32]; /* Private buffer. */
    // 保存元素的各种类型
    robj *ele;              //对象
    unsigned char *estr;    //ziplist
    unsigned int elen;
    long long ell;
    // 保存分值
    double score;
} zsetopval;

// 类型别名
typedef union _iterset iterset;
typedef union _iterzset iterzset;

// 初始化迭代器
void zuiInitIterator(zsetopsrc *op) {
    // 迭代对象为空，直接返回
    if (op->subject == NULL)
        return;

    // 集合类型迭代器
    if (op->type == OBJ_SET) {
        iterset *it = &op->iter.set;    //获得intset的地址
        // 迭代的是intset
        if (op->encoding == OBJ_ENCODING_INTSET) {
            it->is.is = op->subject->ptr;
            it->is.ii = 0;
        // 迭代的是字典
        } else if (op->encoding == OBJ_ENCODING_HT) {
            it->ht.dict = op->subject->ptr;
            it->ht.di = dictGetIterator(op->subject->ptr);
            it->ht.de = dictNext(it->ht.di);
        } else {
            serverPanic("Unknown set encoding");
        }
    // 有序集合迭代器
    } else if (op->type == OBJ_ZSET) {
        iterzset *it = &op->iter.zset;  //获得有序集合zset的地址
        // 迭代的是压缩表
        if (op->encoding == OBJ_ENCODING_ZIPLIST) {
            it->zl.zl = op->subject->ptr;
            it->zl.eptr = ziplistIndex(it->zl.zl,0);
            if (it->zl.eptr != NULL) {
                it->zl.sptr = ziplistNext(it->zl.zl,it->zl.eptr);
                serverAssert(it->zl.sptr != NULL);
            }
        // 迭代的是跳跃表
        } else if (op->encoding == OBJ_ENCODING_SKIPLIST) {
            it->sl.zs = op->subject->ptr;
            it->sl.node = it->sl.zs->zsl->header->level[0].forward;
        } else {
            serverPanic("Unknown sorted set encoding");
        }
    } else {
        serverPanic("Unsupported type");
    }
}

// 释放迭代器空间
void zuiClearIterator(zsetopsrc *op) {
    if (op->subject == NULL)
        return;

    // 释放集合类型迭代器
    if (op->type == OBJ_SET) {
        iterset *it = &op->iter.set;
        if (op->encoding == OBJ_ENCODING_INTSET) {
            UNUSED(it); /* skip */
        } else if (op->encoding == OBJ_ENCODING_HT) {
            dictReleaseIterator(it->ht.di);
        } else {
            serverPanic("Unknown set encoding");
        }
    // 释放有序集合迭代器
    } else if (op->type == OBJ_ZSET) {
        iterzset *it = &op->iter.zset;
        if (op->encoding == OBJ_ENCODING_ZIPLIST) {
            UNUSED(it); /* skip */
        } else if (op->encoding == OBJ_ENCODING_SKIPLIST) {
            UNUSED(it); /* skip */
        } else {
            serverPanic("Unknown sorted set encoding");
        }
    } else {
        serverPanic("Unsupported type");
    }
}

// 返回正在被迭代的元素的长度
int zuiLength(zsetopsrc *op) {
    if (op->subject == NULL)
        return 0;

    // 集合类型迭代器
    if (op->type == OBJ_SET) {
        if (op->encoding == OBJ_ENCODING_INTSET) {
            return intsetLen(op->subject->ptr);
        } else if (op->encoding == OBJ_ENCODING_HT) {
            dict *ht = op->subject->ptr;
            return dictSize(ht);
        } else {
            serverPanic("Unknown set encoding");
        }
    // 有序集合迭代器
    } else if (op->type == OBJ_ZSET) {
        if (op->encoding == OBJ_ENCODING_ZIPLIST) {
            return zzlLength(op->subject->ptr);
        } else if (op->encoding == OBJ_ENCODING_SKIPLIST) {
            zset *zs = op->subject->ptr;
            return zs->zsl->length;
        } else {
            serverPanic("Unknown sorted set encoding");
        }
    } else {
        serverPanic("Unsupported type");
    }
}

/* Check if the current value is valid. If so, store it in the passed structure
 * and move to the next element. If not valid, this means we have reached the
 * end of the structure and can abort. */
// 将当前迭代器指向的元素保存到val中，并更新指向，指向下一个元素
int zuiNext(zsetopsrc *op, zsetopval *val) {
    if (op->subject == NULL)
        return 0;

    // 对上一次的对象进行清理
    if (val->flags & OPVAL_DIRTY_ROBJ)
        decrRefCount(val->ele);

    // 讲val设置为0
    memset(val,0,sizeof(zsetopval));

    // 如果是集合
    if (op->type == OBJ_SET) {
        iterset *it = &op->iter.set;
        // intset编码类型
        if (op->encoding == OBJ_ENCODING_INTSET) {
            int64_t ell;

            // 取出当前元素
            if (!intsetGet(it->is.is,it->is.ii,&ell))
                return 0;
            val->ell = ell;
            val->score = 1.0;   //集合分值默认值为1.0

            /* Move to next element. */
            it->is.ii++;    //指向下一个元素
        // 字典类型
        } else if (op->encoding == OBJ_ENCODING_HT) {
            if (it->ht.de == NULL)
                return 0;
            val->ele = dictGetKey(it->ht.de);   //取出当前元素
            val->score = 1.0;   //集合分值默认值为1.0

            /* Move to next element. */
            it->ht.de = dictNext(it->ht.di);
        } else {
            serverPanic("Unknown set encoding");
        }
    // 有序集合
    } else if (op->type == OBJ_ZSET) {
        iterzset *it = &op->iter.zset;
        // ziplist
        if (op->encoding == OBJ_ENCODING_ZIPLIST) {
            /* No need to check both, but better be explicit. */
            if (it->zl.eptr == NULL || it->zl.sptr == NULL)
                return 0;
            // 取出元素
            serverAssert(ziplistGet(it->zl.eptr,&val->estr,&val->elen,&val->ell));
            // 取出分值
            val->score = zzlGetScore(it->zl.sptr);

            /* Move to next element. */
            zzlNext(it->zl.zl,&it->zl.eptr,&it->zl.sptr);   //更新指向
        //skiplist
        } else if (op->encoding == OBJ_ENCODING_SKIPLIST) {
            if (it->sl.node == NULL)
                return 0;
            // 保存元素和分值
            val->ele = it->sl.node->obj;
            val->score = it->sl.node->score;

            /* Move to next element. */
            it->sl.node = it->sl.node->level[0].forward;    //指向下一个节点
        } else {
            serverPanic("Unknown sorted set encoding");
        }
    } else {
        serverPanic("Unsupported type");
    }
    return 1;
}
// 根据val设置longlong值
int zuiLongLongFromValue(zsetopval *val) {
    if (!(val->flags & OPVAL_DIRTY_LL)) {
        // 设置标志
        val->flags |= OPVAL_DIRTY_LL;

        // 从对象中取出值
        if (val->ele != NULL) {
            // 从INT编码的对象中取出整数值
            if (val->ele->encoding == OBJ_ENCODING_INT) {
                val->ell = (long)val->ele->ptr;
                val->flags |= OPVAL_VALID_LL;   //设置成功标志
            // 字符串编码对象，RAW或EMBSTR
            } else if (sdsEncodedObject(val->ele)) {
                // 转换成整数，保存在ell中
                if (string2ll(val->ele->ptr,sdslen(val->ele->ptr),&val->ell))
                    // 转换成功，设置标志
                    val->flags |= OPVAL_VALID_LL;
            } else {
                serverPanic("Unsupported element encoding");
            }
        // 从ziplist中取出值
        } else if (val->estr != NULL) {
            // 转换成整数，保存在ell中
            if (string2ll((char*)val->estr,val->elen,&val->ell))
                // 转换成功，设置标志
                val->flags |= OPVAL_VALID_LL;
        } else {
            /* The long long was already set, flag as valid. */
            // 总是设置成功转换标志
            val->flags |= OPVAL_VALID_LL;
        }
    }
    return val->flags & OPVAL_VALID_LL; //返回1 ，表示转换成功
}

// 根据val中的值，创建对象
robj *zuiObjectFromValue(zsetopval *val) {
    if (val->ele == NULL) {
        // 根据ziplist中的值，创建对象
        if (val->estr != NULL) {
            val->ele = createStringObject((char*)val->estr,val->elen);
        } else {
            val->ele = createStringObjectFromLongLong(val->ell);
        }
        val->flags |= OPVAL_DIRTY_ROBJ; //设置脏对象标志，下次需要清理
    }
    return val->ele;
}

// 从val中取出字符串
int zuiBufferFromValue(zsetopval *val) {
    if (val->estr == NULL) {
        // 从对象中取
        if (val->ele != NULL) {
            // 如果是INT，则转换为字符串类型，保存在estr
            if (val->ele->encoding == OBJ_ENCODING_INT) {
                val->elen = ll2string((char*)val->_buf,sizeof(val->_buf),(long)val->ele->ptr);
                val->estr = val->_buf;
            // 如果是字符串类型，则直接保存
            } else if (sdsEncodedObject(val->ele)) {
                val->elen = sdslen(val->ele->ptr);
                val->estr = val->ele->ptr;
            } else {
                serverPanic("Unsupported element encoding");
            }
        // 从私有的buf中取
        } else {
            val->elen = ll2string((char*)val->_buf,sizeof(val->_buf),val->ell);
            val->estr = val->_buf;
        }
    }
    return 1;
}

/* Find value pointed to by val in the source pointer to by op. When found,
 * return 1 and store its score in target. Return 0 otherwise. */
// 在迭代器制定的对象中查找给定的元素
// 找到返回1，没找到返回0
int zuiFind(zsetopsrc *op, zsetopval *val, double *score) {
    if (op->subject == NULL)
        return 0;

    // 集合
    if (op->type == OBJ_SET) {
        // intset编码类型
        if (op->encoding == OBJ_ENCODING_INTSET) {
            // 找到则设置score为1，返回1，否则返回0
            if (zuiLongLongFromValue(val) &&            //设置整数ell成员
                intsetFind(op->subject->ptr,val->ell))
            {
                *score = 1.0;
                return 1;
            } else {
                return 0;
            }
        // 字典编码类型
        } else if (op->encoding == OBJ_ENCODING_HT) {
            dict *ht = op->subject->ptr;
            zuiObjectFromValue(val);    //设置对象成员
            // 找到则设置score为1，返回1，否则返回0
            if (dictFind(ht,val->ele) != NULL) {
                *score = 1.0;
                return 1;
            } else {
                return 0;
            }
        } else {
            serverPanic("Unknown set encoding");
        }
    // 有序集合
    } else if (op->type == OBJ_ZSET) {
        zuiObjectFromValue(val);    //设置对象成员

        // 压缩列表编码类型
        if (op->encoding == OBJ_ENCODING_ZIPLIST) {
            // 找到返回1
            if (zzlFind(op->subject->ptr,val->ele,score) != NULL) {
                /* Score is already set by zzlFind. */
                return 1;
            } else {
                return 0;
            }
        // 跳跃表编码类型
        } else if (op->encoding == OBJ_ENCODING_SKIPLIST) {
            zset *zs = op->subject->ptr;
            dictEntry *de;
            // 寻找元素，找到后设置分值，返回1
            if ((de = dictFind(zs->dict,val->ele)) != NULL) {
                *score = *(double*)dictGetVal(de);
                return 1;
            } else {
                return 0;
            }
        } else {
            serverPanic("Unknown sorted set encoding");
        }
    } else {
        serverPanic("Unsupported type");
    }
}

// 对比两个被迭代对象的元素的个数，返回差值
int zuiCompareByCardinality(const void *s1, const void *s2) {
    return zuiLength((zsetopsrc*)s1) - zuiLength((zsetopsrc*)s2);
}

#define REDIS_AGGR_SUM 1    //和
#define REDIS_AGGR_MIN 2    //最小数
#define REDIS_AGGR_MAX 3    //最大数
#define zunionInterDictValue(_e) (dictGetVal(_e) == NULL ? 1.0 : *(double*)dictGetVal(_e))
//默认使用的参数 SUM ，可以将所有集合中某个成员的 score 值之 和 作为结果集中该成员的 score 值；
//使用参数 MIN ，可以将所有集合中某个成员的 最小 score 值作为结果集中该成员的 score 值；
//而参数 MAX 则是将所有集合中某个成员的 最大 score 值作为结果集中该成员的 score 值。

//根据aggregate参数，将对应的操作值保存在target中
inline static void zunionInterAggregate(double *target, double val, int aggregate) {
    //求和
    if (aggregate == REDIS_AGGR_SUM) {
        *target = *target + val;
        /* The result of adding two doubles is NaN when one variable
         * is +inf and the other is -inf. When these numbers are added,
         * we maintain the convention of the result being 0.0. */
        // 检查是否溢出，否则置位0.0
        if (isnan(*target)) *target = 0.0;
    // 求这数的最小数
    } else if (aggregate == REDIS_AGGR_MIN) {
        *target = val < *target ? val : *target;
    // 求这数的最大数
    } else if (aggregate == REDIS_AGGR_MAX) {
        *target = val > *target ? val : *target;
    } else {
        /* safety net */
        serverPanic("Unknown ZUNION/INTER aggregate type");
    }
}

// ZUNIONSTORE destination numkeys key [key ...] [WEIGHTS weight [weight ...]] [AGGREGATE SUM|MIN|MAX]
// ZINTERSTORE destination numkeys key [key ...] [WEIGHTS weight [weight ...]] [AGGREGATE SUM|MIN|MAX]
// ZUNIONSTORE ZINTERSTORE命令的底层实现
void zunionInterGenericCommand(client *c, robj *dstkey, int op) {
    int i, j;
    long setnum;
    int aggregate = REDIS_AGGR_SUM;     //默认是求和
    zsetopsrc *src;
    zsetopval zval;
    robj *tmp;
    unsigned int maxelelen = 0;         //判断是否需要编码转换的条件之一
    robj *dstobj;
    zset *dstzset;
    zskiplistNode *znode;
    int touched = 0;

    /* expect setnum input keys to be given */
    // 取出numkeys参数值，保存在setnum中
    if ((getLongFromObjectOrReply(c, c->argv[2], &setnum, NULL) != C_OK))
        return;

    // 至少指定一个key
    if (setnum < 1) {
        addReplyError(c,
            "at least 1 input key is needed for ZUNIONSTORE/ZINTERSTORE");
        return;
    }

    /* test if the expected number of keys would overflow */
    // setnum与key数量不匹配，发送语法错误信息
    if (setnum > c->argc-3) {
        addReply(c,shared.syntaxerr);
        return;
    }

    /* read keys to be used for input */
    // 为每一个key分配一个迭代器空间
    src = zcalloc(sizeof(zsetopsrc) * setnum);
    // 遍历
    for (i = 0, j = 3; i < setnum; i++, j++) {
        // 以写方式读取出有序集合
        robj *obj = lookupKeyWrite(c->db,c->argv[j]);
        if (obj != NULL) {  //检查读取出的集合类型，否则发送信息
            if (obj->type != OBJ_ZSET && obj->type != OBJ_SET) {
                zfree(src);
                addReply(c,shared.wrongtypeerr);
                return;
            }

            // 初始化每个集合对象的迭代器
            src[i].subject = obj;
            src[i].type = obj->type;
            src[i].encoding = obj->encoding;
        } else {
            src[i].subject = NULL;  //集合对象不存在则设置为NULL
        }

        /* Default all weights to 1. */
        src[i].weight = 1.0;        //默认权重为1.0
    }

    /* parse optional extra arguments */
    // 解析其他的参数
    if (j < c->argc) {
        int remaining = c->argc - j;    //其他参数的数量

        while (remaining) {
            // 设置了权重WEIGHTS参数
            if (remaining >= (setnum + 1) && !strcasecmp(c->argv[j]->ptr,"weights")) {
                j++; remaining--;
                // 遍历设置的weight，保存到每个集合对象的迭代器中
                for (i = 0; i < setnum; i++, j++, remaining--) {
                    if (getDoubleFromObjectOrReply(c,c->argv[j],&src[i].weight,
                            "weight value is not a float") != C_OK)
                    {
                        zfree(src);
                        return;
                    }
                }
            // 设置了集合方式AGGREGATE参数
            } else if (remaining >= 2 && !strcasecmp(c->argv[j]->ptr,"aggregate")) {
                j++; remaining--;
                // 根据不同的参数，设置默认的方式
                if (!strcasecmp(c->argv[j]->ptr,"sum")) {
                    aggregate = REDIS_AGGR_SUM;
                } else if (!strcasecmp(c->argv[j]->ptr,"min")) {
                    aggregate = REDIS_AGGR_MIN;
                } else if (!strcasecmp(c->argv[j]->ptr,"max")) {
                    aggregate = REDIS_AGGR_MAX;
                } else {
                    zfree(src);
                    addReply(c,shared.syntaxerr);   //不是以上三种集合方式，发送语法错误信息
                    return;
                }
                j++; remaining--;
            } else {
                zfree(src);
                addReply(c,shared.syntaxerr);   //不是以上两种参数，发送语法错误信息
                return;
            }
        }
    }

    /* sort sets from the smallest to largest, this will improve our
     * algorithm's performance */
    // 对所有集合元素的多少，从小到大排序，提高算法性能
    qsort(src,setnum,sizeof(zsetopsrc),zuiCompareByCardinality);

    dstobj = createZsetObject();    //创建结果集对象
    dstzset = dstobj->ptr;
    memset(&zval, 0, sizeof(zval)); //初始化保存有序集合元素和分值的结构

    // ZINTERSTORE命令
    if (op == SET_OP_INTER) {
        /* Skip everything if the smallest input is empty. */
        // 如果当前集合没有元素，则不处理
        if (zuiLength(&src[0]) > 0) {
            /* Precondition: as src[0] is non-empty and the inputs are ordered
             * by size, all src[i > 0] are non-empty too. */
            // 初始化元素最少的有序集合
            zuiInitIterator(&src[0]);

            // 遍历元素最少的有序集合的每一个元素，将元素的分值和数值保存到zval中
            while (zuiNext(&src[0],&zval)) {
                double score, value;
                // 计算加权分值
                score = src[0].weight * zval.score;
                if (isnan(score)) score = 0;

                // 遍历其后的每一个有序集合，分别跟元素最少的有序集合做运算
                for (j = 1; j < setnum; j++) {
                    /* It is not safe to access the zset we are
                     * iterating, so explicitly check for equal object. */
                    // 如果输入的key一样
                    if (src[j].subject == src[0].subject) {
                        // 计算新的加权分值
                        value = zval.score*src[j].weight;
                        // 根据集合方式sum、min、max保存结果值到score
                        zunionInterAggregate(&score,value,aggregate);

                    // 输入的key与第一个key不一样，在当前key查找当前的元素，并将找到元素的分值保存在value中
                    } else if (zuiFind(&src[j],&zval,&value)) {
                        // 计算加权后的分值
                        value *= src[j].weight;
                        // 根据集合方式sum、min、max保存结果值到score
                        zunionInterAggregate(&score,value,aggregate);

                    // 如果当前元素在所有有序集合中没有找到，则跳过for循环，处理下一个元素
                    } else {
                        break;
                    }
                }

                /* Only continue when present in every input. */
                // 如果没有跳出for循环，那么该元素一定是每个key集合中都存在，因此进行ZINTER的STORE操作
                if (j == setnum) {
                    tmp = zuiObjectFromValue(&zval);    //创建当前元素的对象
                    znode = zslInsert(dstzset->zsl,score,tmp);  //插入到有序集合中
                    incrRefCount(tmp); /* added to skiplist */
                    dictAdd(dstzset->dict,tmp,&znode->score);   //加入到字典中
                    incrRefCount(tmp); /* added to dictionary */

                    if (sdsEncodedObject(tmp)) {    //如果是字符串编码的对象
                        if (sdslen(tmp->ptr) > maxelelen)   //更新最大长度
                            maxelelen = sdslen(tmp->ptr);
                    }
                }
            }
            zuiClearIterator(&src[0]);  //释放集合类型的迭代器
        }
    // ZUNIONSTORE命令
    } else if (op == SET_OP_UNION) {
        dict *accumulator = dictCreate(&setDictType,NULL);  //创建一个临时字典作为结果集字典
        dictIterator *di;
        dictEntry *de;
        double score;

        // 计算出最大有序集合元素个数，并集至少和最大的集合一样大，因此按需扩展结果集字典
        if (setnum) {
            /* Our union is at least as large as the largest set.
             * Resize the dictionary ASAP to avoid useless rehashing. */
            dictExpand(accumulator,zuiLength(&src[setnum-1]));
        }

        /* Step 1: Create a dictionary of elements -> aggregated-scores
         * by iterating one sorted set after the other. */
        // 1.遍历所有的集合
        for (i = 0; i < setnum; i++) {
            if (zuiLength(&src[i]) == 0) continue;  //跳过空集合

            zuiInitIterator(&src[i]);   //初始化集合迭代器
            // 遍历当前集合的所有元素，迭代器当前指向的元素保存在zval中
            while (zuiNext(&src[i],&zval)) {
                /* Initialize value */
                score = src[i].weight * zval.score;     //初始化分值
                if (isnan(score)) score = 0;

                /* Search for this element in the accumulating dictionary. */
                // 从结果集字典中找当前元素
                de = dictFind(accumulator,zuiObjectFromValue(&zval));
                /* If we don't have it, we need to create a new entry. */
                // 如果在结果集中找不到，则加入到结果集中
                if (de == NULL) {
                    tmp = zuiObjectFromValue(&zval);    //创建元素对象
                    /* Remember the longest single element encountered,
                     * to understand if it's possible to convert to ziplist
                     * at the end. */
                    if (sdsEncodedObject(tmp)) {    //字符串类型的话要更新maxelelen
                        if (sdslen(tmp->ptr) > maxelelen)
                            maxelelen = sdslen(tmp->ptr);
                    }
                    /* Add the element with its initial score. */
                    de = dictAddRaw(accumulator,tmp);   //加入到结果集字典中
                    incrRefCount(tmp);
                    dictSetDoubleVal(de,score);         //设置元素的分值

                // 如果在结果集中可以找到
                } else {
                    /* Update the score with the score of the new instance
                     * of the element found in the current sorted set.
                     *
                     * Here we access directly the dictEntry double
                     * value inside the union as it is a big speedup
                     * compared to using the getDouble/setDouble API. */
                    zunionInterAggregate(&de->v.d,score,aggregate); //更新加权分值
                }
            }
            zuiClearIterator(&src[i]);  //清理当前集合的迭代器
        }

        /* Step 2: convert the dictionary into the final sorted set. */
        // 2. 创建字典的迭代器，遍历结果集字典
        di = dictGetIterator(accumulator);

        /* We now are aware of the final size of the resulting sorted set,
         * let's resize the dictionary embedded inside the sorted set to the
         * right size, in order to save rehashing time. */
        // 按需扩展结果集合中的字典成员
        dictExpand(dstzset->dict,dictSize(accumulator));

        // 遍历结果集字典，
        while((de = dictNext(di)) != NULL) {
            // 获得元素对象和分值
            robj *ele = dictGetKey(de);
            score = dictGetDoubleVal(de);
            // 插入到结果集合的跳跃表中
            znode = zslInsert(dstzset->zsl,score,ele);
            incrRefCount(ele); /* added to skiplist */
            // 添加到结果集合的字典中
            dictAdd(dstzset->dict,ele,&znode->score);
            incrRefCount(ele); /* added to dictionary */
        }
        dictReleaseIterator(di);    //释放字典的迭代器

        /* We can free the accumulator dictionary now. */
        dictRelease(accumulator);   //释放临时保存结果的字典
    } else {
        serverPanic("Unknown operator");
    }

    // 删除已经存在的dstkey，并做标记
    if (dbDelete(c->db,dstkey))
        touched = 1;
    // 如果结果集元素不为0
    if (dstzset->zsl->length) {
        zsetConvertToZiplistIfNeeded(dstobj,maxelelen); //按需进行编码转换
        dbAdd(c->db,dstkey,dstobj);                     //将数据库中的dstkey和dstobj关联成对
        addReplyLongLong(c,zsetLength(dstobj));         //发送结果集合的元素数量给client
        signalModifiedKey(c->db,dstkey);                //发送键被修改的信号
        notifyKeyspaceEvent(NOTIFY_ZSET,                //发送对应的事件通知
            (op == SET_OP_UNION) ? "zunionstore" : "zinterstore",
            dstkey,c->db->id);
        server.dirty++;                                 //更新脏键
    // 结果集没有元素，释放对象，发送0给client
    } else {
        decrRefCount(dstobj);
        addReply(c,shared.czero);
        if (touched) {
            signalModifiedKey(c->db,dstkey);
            notifyKeyspaceEvent(NOTIFY_GENERIC,"del",dstkey,c->db->id);
            server.dirty++;
        }
    }
    zfree(src); //释放集合迭代器数组空间
}

// ZUNIONSTORE destination numkeys key [key ...] [WEIGHTS weight [weight ...]] [AGGREGATE SUM|MIN|MAX]
// ZUNIONSTORE命令实现
void zunionstoreCommand(client *c) {
    zunionInterGenericCommand(c,c->argv[1], SET_OP_UNION);
}

// ZINTERSTORE destination numkeys key [key ...] [WEIGHTS weight [weight ...]] [AGGREGATE SUM|MIN|MAX]
// ZINTERSTORE 命令实现
void zinterstoreCommand(client *c) {
    zunionInterGenericCommand(c,c->argv[1], SET_OP_INTER);
}

// ZRANGE key start stop [WITHSCORES]
// ZREVRANGE key start stop [WITHSCORES]
// ZRANGE、ZREVRANGE命令底层实现
void zrangeGenericCommand(client *c, int reverse) {
    robj *key = c->argv[1];
    robj *zobj;
    int withscores = 0;
    long start;
    long end;
    int llen;
    int rangelen;

    // 取出起始位置start和结束位置end
    if ((getLongFromObjectOrReply(c, c->argv[2], &start, NULL) != C_OK) ||
        (getLongFromObjectOrReply(c, c->argv[3], &end, NULL) != C_OK)) return;

    // 如果有WITHSCORES参数，设置标志
    if (c->argc == 5 && !strcasecmp(c->argv[4]->ptr,"withscores")) {
        withscores = 1;
    // 参数太多发送语法错误信息
    } else if (c->argc >= 5) {
        addReply(c,shared.syntaxerr);
        return;
    }

    // 以读操作取出key有序集合并检查集合的数据类型
    if ((zobj = lookupKeyReadOrReply(c,key,shared.emptymultibulk)) == NULL
         || checkType(c,zobj,OBJ_ZSET)) return;

    /* Sanitize indexes. */
    // 获得有序集合的下标范围
    llen = zsetLength(zobj);
    // 处理负数下标
    if (start < 0) start = llen+start;
    if (end < 0) end = llen+end;
    if (start < 0) start = 0;

    /* Invariant: start >= 0, so this test will be true when end < 0.
     * The range is empty when start > end or start >= length. */
    // 修剪调整下标
    if (start > end || start >= llen) {
        addReply(c,shared.emptymultibulk);
        return;
    }
    if (end >= llen) end = llen-1;
    rangelen = (end-start)+1;       //范围长度

    /* Return the result in form of a multi-bulk reply */
    // 发送回复的行数信息给client
    addReplyMultiBulkLen(c, withscores ? (rangelen*2) : rangelen);

    // 如果是ziplist
    if (zobj->encoding == OBJ_ENCODING_ZIPLIST) {
        unsigned char *zl = zobj->ptr;
        unsigned char *eptr, *sptr;
        unsigned char *vstr;
        unsigned int vlen;
        long long vlong;

        // 根据reverse决定遍历的头或尾元素节点地址eptr
        if (reverse)
            eptr = ziplistIndex(zl,-2-(2*start));
        else
            eptr = ziplistIndex(zl,2*start);

        serverAssertWithInfo(c,zobj,eptr != NULL);
        // 获得分值节点地址
        sptr = ziplistNext(zl,eptr);

        //取出rangelen个元素
        while (rangelen--) {
            serverAssertWithInfo(c,zobj,eptr != NULL && sptr != NULL);
            serverAssertWithInfo(c,zobj,ziplistGet(eptr,&vstr,&vlen,&vlong));   //讲元素信息保存在参数中
            // 元素的类型不同，发送不同类型的回复信息
            if (vstr == NULL)
                addReplyBulkLongLong(c,vlong);
            else
                addReplyBulkCBuffer(c,vstr,vlen);
            // 设置了withscores，发送分值
            if (withscores)
                addReplyDouble(c,zzlGetScore(sptr));

            // 根据reverse，指向下一个元素和分值的节点
            if (reverse)
                zzlPrev(zl,&eptr,&sptr);
            else
                zzlNext(zl,&eptr,&sptr);
        }

    // 如果是跳跃表
    } else if (zobj->encoding == OBJ_ENCODING_SKIPLIST) {
        zset *zs = zobj->ptr;
        zskiplist *zsl = zs->zsl;
        zskiplistNode *ln;
        robj *ele;

        /* Check if starting point is trivial, before doing log(N) lookup. */
        // 根据reverse决定，遍历的头或尾元素节点地址
        if (reverse) {
            ln = zsl->tail;
            if (start > 0)
                ln = zslGetElementByRank(zsl,llen-start);
        } else {
            ln = zsl->header->level[0].forward;
            if (start > 0)
                ln = zslGetElementByRank(zsl,start+1);
        }

        // 取出rangelen个元素
        while(rangelen--) {
            serverAssertWithInfo(c,zobj,ln != NULL);
            ele = ln->obj;
            addReplyBulk(c,ele);    //发送元素
            if (withscores)
                addReplyDouble(c,ln->score);    //发送分值
            // 指向下一个节点
            ln = reverse ? ln->backward : ln->level[0].forward;
        }
    } else {
        serverPanic("Unknown sorted set encoding");
    }
}

// ZRANGE key start stop [WITHSCORES]
// ZRANGE命令实现
void zrangeCommand(client *c) {
    zrangeGenericCommand(c,0);
}

// ZREVRANGE key start stop [WITHSCORES]
// ZREVRANGE命令实现
void zrevrangeCommand(client *c) {
    zrangeGenericCommand(c,1);
}

/* This command implements ZRANGEBYSCORE, ZREVRANGEBYSCORE. */
// ZRANGEBYSCORE key min max [WITHSCORES] [LIMIT offset count]
// ZREVRANGEBYSCORE key max min [WITHSCORES] [LIMIT offset count]
// ZRANGEBYSCORE ZREVRANGEBYSCORE命令的底层实现
void genericZrangebyscoreCommand(client *c, int reverse) {
    zrangespec range;
    robj *key = c->argv[1];
    robj *zobj;
    long offset = 0, limit = -1;
    int withscores = 0;
    unsigned long rangelen = 0;
    void *replylen = NULL;
    int minidx, maxidx;

    /* Parse the range arguments. */
    // 解析min和max范围的参数的下标
    if (reverse) {
        /* Range is given as [max,min] */
        maxidx = 2; minidx = 3;
    } else {
        /* Range is given as [min,max] */
        minidx = 2; maxidx = 3;
    }

    // 解析min和max范围参数，保存到zrangespec中，默认是包含[]临界值
    if (zslParseRange(c->argv[minidx],c->argv[maxidx],&range) != C_OK) {
        addReplyError(c,"min or max is not a float");
        return;
    }

    /* Parse optional extra arguments. Note that ZCOUNT will exactly have
     * 4 arguments, so we'll never enter the following code path. */
    // 解析其他的参数
    if (c->argc > 4) {
        int remaining = c->argc - 4;    //其他参数个数
        int pos = 4;

        while (remaining) {
            // 如果有WITHSCORES，设置标志
            if (remaining >= 1 && !strcasecmp(c->argv[pos]->ptr,"withscores")) {
                pos++; remaining--;
                withscores = 1;
            // 如果有LIMIT，取出offset和count值 保存在offset和limit中
            } else if (remaining >= 3 && !strcasecmp(c->argv[pos]->ptr,"limit")) {
                if ((getLongFromObjectOrReply(c, c->argv[pos+1], &offset, NULL) != C_OK) ||
                    (getLongFromObjectOrReply(c, c->argv[pos+2], &limit, NULL) != C_OK)) return;
                pos += 3; remaining -= 3;
            } else {
                addReply(c,shared.syntaxerr);
                return;
            }
        }
    }

    /* Ok, lookup the key and get the range */
    // 以读操作取出有序集合对象
    if ((zobj = lookupKeyReadOrReply(c,key,shared.emptymultibulk)) == NULL ||
        checkType(c,zobj,OBJ_ZSET)) return;

    // ziplist
    if (zobj->encoding == OBJ_ENCODING_ZIPLIST) {
        unsigned char *zl = zobj->ptr;
        unsigned char *eptr, *sptr;
        unsigned char *vstr;
        unsigned int vlen;
        long long vlong;
        double score;

        /* If reversed, get the last node in range as starting point. */
        // 获得range范围内的起始元素节点地址
        if (reverse) {
            eptr = zzlLastInRange(zl,&range);
        } else {
            eptr = zzlFirstInRange(zl,&range);
        }

        /* No "first" element in the specified interval. */
        // 没有元素在range里，发送空回复
        if (eptr == NULL) {
            addReply(c, shared.emptymultibulk);
            return;
        }

        /* Get score pointer for the first element. */
        serverAssertWithInfo(c,zobj,eptr != NULL);
        // 分值节点地址
        sptr = ziplistNext(zl,eptr);

        /* We don't know in advance how many matching elements there are in the
         * list, so we push this object that will represent the multi-bulk
         * length in the output buffer, and will "fix" it later */
        // 回复长度
        replylen = addDeferredMultiBulkLength(c);

        /* If there is an offset, just traverse the number of elements without
         * checking the score because that is done in the next loop. */
        // 跳过offset设定的长度
        while (eptr && offset--) {
            if (reverse) {
                zzlPrev(zl,&eptr,&sptr);
            } else {
                zzlNext(zl,&eptr,&sptr);
            }
        }

        // 遍历所有符合范围的节点
        while (eptr && limit--) {
            score = zzlGetScore(sptr);  //获取分值

            /* Abort when the node is no longer in range. */
            // 检查分值是否符合范围内的分值
            if (reverse) {
                if (!zslValueGteMin(score,&range)) break;
            } else {
                if (!zslValueLteMax(score,&range)) break;
            }

            /* We know the element exists, so ziplistGet should always succeed */
            serverAssertWithInfo(c,zobj,ziplistGet(eptr,&vstr,&vlen,&vlong));   //保存当前元素的值到参数中

            rangelen++;
            // 不同元素类型，发送不同的类型的值给client
            if (vstr == NULL) {
                addReplyBulkLongLong(c,vlong);
            } else {
                addReplyBulkCBuffer(c,vstr,vlen);
            }

            if (withscores) {
                addReplyDouble(c,score);    //发送分值
            }

            /* Move to next node */
            // 指向下一个元素和分值节点
            if (reverse) {
                zzlPrev(zl,&eptr,&sptr);
            } else {
                zzlNext(zl,&eptr,&sptr);
            }
        }
    // 跳跃表
    } else if (zobj->encoding == OBJ_ENCODING_SKIPLIST) {
        zset *zs = zobj->ptr;
        zskiplist *zsl = zs->zsl;
        zskiplistNode *ln;

        /* If reversed, get the last node in range as starting point. */
        // 获得range范围内的起始节点地址
        if (reverse) {
            ln = zslLastInRange(zsl,&range);
        } else {
            ln = zslFirstInRange(zsl,&range);
        }

        /* No "first" element in the specified interval. */
        // 没有元素存在在制定的范围内
        if (ln == NULL) {
            addReply(c, shared.emptymultibulk);
            return;
        }

        /* We don't know in advance how many matching elements there are in the
         * list, so we push this object that will represent the multi-bulk
         * length in the output buffer, and will "fix" it later */
        // 回复长度
        replylen = addDeferredMultiBulkLength(c);

        /* If there is an offset, just traverse the number of elements without
         * checking the score because that is done in the next loop. */
        // 跳过offset个节点
        while (ln && offset--) {
            if (reverse) {
                ln = ln->backward;
            } else {
                ln = ln->level[0].forward;
            }
        }

        // 遍历所有符合范围的节点
        while (ln && limit--) {
            /* Abort when the node is no longer in range. */
            // 检查分值是否符合
            if (reverse) {
                if (!zslValueGteMin(ln->score,&range)) break;
            } else {
                if (!zslValueLteMax(ln->score,&range)) break;
            }

            rangelen++;
            addReplyBulk(c,ln->obj);    //发送元素

            if (withscores) {
                addReplyDouble(c,ln->score);    //发送分值
            }

            /* Move to next node */
            // 指向下一个节点
            if (reverse) {
                ln = ln->backward;
            } else {
                ln = ln->level[0].forward;
            }
        }
    } else {
        serverPanic("Unknown sorted set encoding");
    }

    if (withscores) {   //更新回复长度
        rangelen *= 2;
    }

    setDeferredMultiBulkLength(c, replylen, rangelen);  //发送范围的长度
}

// ZRANGEBYSCORE key max min [WITHSCORES] [LIMIT offset count]
// ZRANGEBYSCORE 命令实现
void zrangebyscoreCommand(client *c) {
    genericZrangebyscoreCommand(c,0);
}

// ZREVRANGEBYSCORE key max min [WITHSCORES] [LIMIT offset count]
// ZREVRANGEBYSCORE 命令实现
void zrevrangebyscoreCommand(client *c) {
    genericZrangebyscoreCommand(c,1);
}

// ZCOUNT key min max
// ZCOUNT命令实现
void zcountCommand(client *c) {
    robj *key = c->argv[1];
    robj *zobj;
    zrangespec range;
    int count = 0;

    /* Parse the range arguments */
    // 解析min和max范围参数，保存到range中
    if (zslParseRange(c->argv[2],c->argv[3],&range) != C_OK) {
        addReplyError(c,"min or max is not a float");
        return;
    }

    /* Lookup the sorted set */
    // 以读操作取出有序集合对象并检查数据类型
    if ((zobj = lookupKeyReadOrReply(c, key, shared.czero)) == NULL ||
        checkType(c, zobj, OBJ_ZSET)) return;

    // ziplist
    if (zobj->encoding == OBJ_ENCODING_ZIPLIST) {
        unsigned char *zl = zobj->ptr;
        unsigned char *eptr, *sptr;
        double score;

        /* Use the first element in range as the starting point */
        // 范围内的第一个元素节点地址
        eptr = zzlFirstInRange(zl,&range);

        /* No "first" element */
        // 范围内无元素，发送0
        if (eptr == NULL) {
            addReply(c, shared.czero);
            return;
        }

        /* First element is in range */
        // 分值节点地址
        sptr = ziplistNext(zl,eptr);
        // 取出分值
        score = zzlGetScore(sptr);
        // 保证分值符合范围
        serverAssertWithInfo(c,zobj,zslValueLteMax(score,&range));

        /* Iterate over elements in range */
        // 迭代符合范围的节点
        while (eptr) {
            // 取出分值
            score = zzlGetScore(sptr);

            /* Abort when the node is no longer in range. */
            // 保证分值符合范围
            if (!zslValueLteMax(score,&range)) {
                break;
            } else {
                count++;                    //更新计数器
                zzlNext(zl,&eptr,&sptr);    //指向下一个元素和分值节点
            }
        }
    // skiplist
    } else if (zobj->encoding == OBJ_ENCODING_SKIPLIST) {
        zset *zs = zobj->ptr;
        zskiplist *zsl = zs->zsl;
        zskiplistNode *zn;
        unsigned long rank;

        /* Find first element in range */
        // 范围内的第一个元素节点地址
        zn = zslFirstInRange(zsl, &range);

        /* Use rank of first element, if any, to determine preliminary count */
        // 有符合范围的节点
        if (zn != NULL) {
            // 范围内第一个节点的下标
            rank = zslGetRank(zsl, zn->score, zn->obj);
            // 计算范围内第一个节点之前有多少个节点
            count = (zsl->length - (rank - 1));

            /* Find last element in range */
            // 范围内最后一个节点的地址
            zn = zslLastInRange(zsl, &range);

            /* Use rank of last element, if any, to determine the actual count */
            if (zn != NULL) {
                // 范围内最后一个节点的下标
                rank = zslGetRank(zsl, zn->score, zn->obj);
                // 计算范围内第最后一个节点之前有多少个节点，和之前的做差，就是范围内的节点个数
                count -= (zsl->length - rank);
            }
        }
    } else {
        serverPanic("Unknown sorted set encoding");
    }

    addReplyLongLong(c, count); //回复个数
}

// ZLEXCOUNT key min max
// ZLEXCOUNT 命令实现
void zlexcountCommand(client *c) {
    robj *key = c->argv[1];
    robj *zobj;
    zlexrangespec range;
    int count = 0;

    /* Parse the range arguments */
    // 解析min和max范围参数，保存到range中
    if (zslParseLexRange(c->argv[2],c->argv[3],&range) != C_OK) {
        addReplyError(c,"min or max not valid string range item");
        return;
    }

    /* Lookup the sorted set */
    // 以读操作取出有序集合对象并检查数据类型
    if ((zobj = lookupKeyReadOrReply(c, key, shared.czero)) == NULL ||
        checkType(c, zobj, OBJ_ZSET))
    {
        zslFreeLexRange(&range);
        return;
    }

    // ziplist
    if (zobj->encoding == OBJ_ENCODING_ZIPLIST) {
        unsigned char *zl = zobj->ptr;
        unsigned char *eptr, *sptr;

        /* Use the first element in range as the starting point */
        // 范围内的第一个元素节点地址
        eptr = zzlFirstInLexRange(zl,&range);

        /* No "first" element */
        // 范围内无元素，发送0
        if (eptr == NULL) {
            zslFreeLexRange(&range);
            addReply(c, shared.czero);
            return;
        }

        /* First element is in range */
        // 分值节点地址
        sptr = ziplistNext(zl,eptr);
        // 保证分值符合范围
        serverAssertWithInfo(c,zobj,zzlLexValueLteMax(eptr,&range));

        /* Iterate over elements in range */
        // 迭代符合范围的节点
        while (eptr) {
            /* Abort when the node is no longer in range. */
            // 保证分值符合范围
            if (!zzlLexValueLteMax(eptr,&range)) {
                break;
            } else {
                count++;                //更新计数器
                zzlNext(zl,&eptr,&sptr);//指向下一个元素和分值
            }
        }
    // skiplist
    } else if (zobj->encoding == OBJ_ENCODING_SKIPLIST) {
        zset *zs = zobj->ptr;
        zskiplist *zsl = zs->zsl;
        zskiplistNode *zn;
        unsigned long rank;

        /* Find first element in range */
        // 范围内的第一个元素节点地址
        zn = zslFirstInLexRange(zsl, &range);

        /* Use rank of first element, if any, to determine preliminary count */
        if (zn != NULL) {
            // 范围内第一个节点的下标
            rank = zslGetRank(zsl, zn->score, zn->obj);
            // 计算范围内第一个节点之前有多少个节点
            count = (zsl->length - (rank - 1));

            /* Find last element in range */
            // 范围内最后一个节点的地址
            zn = zslLastInLexRange(zsl, &range);

            /* Use rank of last element, if any, to determine the actual count */
            if (zn != NULL) {
                // 范围内最后一个节点的下标
                rank = zslGetRank(zsl, zn->score, zn->obj);
                // 计算范围内第最后一个节点之前有多少个节点，和之前的做差，就是范围内的节点个数
                count -= (zsl->length - rank);
            }
        }
    } else {
        serverPanic("Unknown sorted set encoding");
    }

    zslFreeLexRange(&range);    //释放字典序范围
    addReplyLongLong(c, count); //发送范围内元素个数
}

/* This command implements ZRANGEBYLEX, ZREVRANGEBYLEX. */
// ZRANGEBYLEX key min max [LIMIT offset count]
// ZRANGEBYLEX 命令实现
void genericZrangebylexCommand(client *c, int reverse) {
    zlexrangespec range;
    robj *key = c->argv[1];
    robj *zobj;
    long offset = 0, limit = -1;
    unsigned long rangelen = 0;
    void *replylen = NULL;
    int minidx, maxidx;

    /* Parse the range arguments. */
    // 解析min和max范围的参数的下标
    if (reverse) {
        /* Range is given as [max,min] */
        maxidx = 2; minidx = 3;
    } else {
        /* Range is given as [min,max] */
        minidx = 2; maxidx = 3;
    }

    // 解析min和max范围的参数，保存到range
    if (zslParseLexRange(c->argv[minidx],c->argv[maxidx],&range) != C_OK) {
        addReplyError(c,"min or max not valid string range item");
        return;
    }

    /* Parse optional extra arguments. Note that ZCOUNT will exactly have
     * 4 arguments, so we'll never enter the following code path. */
    // 解析其他参数
    if (c->argc > 4) {
        int remaining = c->argc - 4;    //其他参数的个数
        int pos = 4;

        while (remaining) {
            // 如果有LIMIT，取出offset和count保存在offset和limit
            if (remaining >= 3 && !strcasecmp(c->argv[pos]->ptr,"limit")) {
                if ((getLongFromObjectOrReply(c, c->argv[pos+1], &offset, NULL) != C_OK) ||
                    (getLongFromObjectOrReply(c, c->argv[pos+2], &limit, NULL) != C_OK)) return;
                pos += 3; remaining -= 3;
            } else {
                zslFreeLexRange(&range);
                addReply(c,shared.syntaxerr);
                return;
            }
        }
    }

    /* Ok, lookup the key and get the range */
    // 以读操作取出有序集合对象
    if ((zobj = lookupKeyReadOrReply(c,key,shared.emptymultibulk)) == NULL ||
        checkType(c,zobj,OBJ_ZSET))
    {
        zslFreeLexRange(&range);
        return;
    }

    // ziplist
    if (zobj->encoding == OBJ_ENCODING_ZIPLIST) {
        unsigned char *zl = zobj->ptr;
        unsigned char *eptr, *sptr;
        unsigned char *vstr;
        unsigned int vlen;
        long long vlong;

        /* If reversed, get the last node in range as starting point. */
        // 获得range范围内的起始元素节点地址
        if (reverse) {
            eptr = zzlLastInLexRange(zl,&range);
        } else {
            eptr = zzlFirstInLexRange(zl,&range);
        }

        /* No "first" element in the specified interval. */
        // 没有元素在范围内
        if (eptr == NULL) {
            addReply(c, shared.emptymultibulk);
            zslFreeLexRange(&range);
            return;
        }

        /* Get score pointer for the first element. */
        serverAssertWithInfo(c,zobj,eptr != NULL);
        // 分数节点的地址
        sptr = ziplistNext(zl,eptr);

        /* We don't know in advance how many matching elements there are in the
         * list, so we push this object that will represent the multi-bulk
         * length in the output buffer, and will "fix" it later */
        // 回复长度
        replylen = addDeferredMultiBulkLength(c);

        /* If there is an offset, just traverse the number of elements without
         * checking the score because that is done in the next loop. */
        // 跳过offset设定的长度
        while (eptr && offset--) {
            if (reverse) {
                zzlPrev(zl,&eptr,&sptr);
            } else {
                zzlNext(zl,&eptr,&sptr);
            }
        }

        // 遍历所有符合范围的节点
        while (eptr && limit--) {
            /* Abort when the node is no longer in range. */
            // 检查分值是否符合范围内的分值
            if (reverse) {
                if (!zzlLexValueGteMin(eptr,&range)) break;
            } else {
                if (!zzlLexValueLteMax(eptr,&range)) break;
            }

            /* We know the element exists, so ziplistGet should always
             * succeed. */
            serverAssertWithInfo(c,zobj,ziplistGet(eptr,&vstr,&vlen,&vlong));   //保存当前元素的值到参数中

            rangelen++;
            // 不同元素值类型，发送不同类型的值client
            if (vstr == NULL) {
                addReplyBulkLongLong(c,vlong);
            } else {
                addReplyBulkCBuffer(c,vstr,vlen);
            }

            /* Move to next node */
            // 指向下一个元素和分值节点
            if (reverse) {
                zzlPrev(zl,&eptr,&sptr);
            } else {
                zzlNext(zl,&eptr,&sptr);
            }
        }
    // skiplist
    } else if (zobj->encoding == OBJ_ENCODING_SKIPLIST) {
        zset *zs = zobj->ptr;
        zskiplist *zsl = zs->zsl;
        zskiplistNode *ln;

        /* If reversed, get the last node in range as starting point. */
        // 获得range范围内的起始节点地址
        if (reverse) {
            ln = zslLastInLexRange(zsl,&range);
        } else {
            ln = zslFirstInLexRange(zsl,&range);
        }

        /* No "first" element in the specified interval. */
        // 没有节点符合范围
        if (ln == NULL) {
            addReply(c, shared.emptymultibulk);
            zslFreeLexRange(&range);
            return;
        }

        /* We don't know in advance how many matching elements there are in the
         * list, so we push this object that will represent the multi-bulk
         * length in the output buffer, and will "fix" it later */
        // 回复长度
        replylen = addDeferredMultiBulkLength(c);

        /* If there is an offset, just traverse the number of elements without
         * checking the score because that is done in the next loop. */
        // 跳过offset个节点
        while (ln && offset--) {
            if (reverse) {
                ln = ln->backward;
            } else {
                ln = ln->level[0].forward;
            }
        }

        // 遍历所有符合范围的节点
        while (ln && limit--) {
            /* Abort when the node is no longer in range. */
            if (reverse) {  //检查分值是否符合
                if (!zslLexValueGteMin(ln->obj,&range)) break;
            } else {
                if (!zslLexValueLteMax(ln->obj,&range)) break;
            }

            rangelen++;
            addReplyBulk(c,ln->obj);    //发送元素

            /* Move to next node */
            // 指向下一个节点
            if (reverse) {
                ln = ln->backward;
            } else {
                ln = ln->level[0].forward;
            }
        }
    } else {
        serverPanic("Unknown sorted set encoding");
    }

    zslFreeLexRange(&range);    //释放字典序范围
    setDeferredMultiBulkLength(c, replylen, rangelen);  //发送回复长度值
}

// ZRANGEBYLEX key min max [LIMIT offset count]
// ZRANGEBYLEX 命令实现
void zrangebylexCommand(client *c) {
    genericZrangebylexCommand(c,0);
}

// ZREVRANGEBYLEX key min max [LIMIT offset count]
// ZREVRANGEBYLEX命令实现
void zrevrangebylexCommand(client *c) {
    genericZrangebylexCommand(c,1);
}

// ZCARD key
// ZCARD命令实现
void zcardCommand(client *c) {
    robj *key = c->argv[1];
    robj *zobj;

    // 以读操作取出有序集合对象
    if ((zobj = lookupKeyReadOrReply(c,key,shared.czero)) == NULL ||
        checkType(c,zobj,OBJ_ZSET)) return;

    addReplyLongLong(c,zsetLength(zobj));   //发送集合元素个数
}

// ZSCORE key member
// ZSCORE 命令实现
void zscoreCommand(client *c) {
    robj *key = c->argv[1];
    robj *zobj;
    double score;

    // 以读操作取出有序集合对象
    if ((zobj = lookupKeyReadOrReply(c,key,shared.nullbulk)) == NULL ||
        checkType(c,zobj,OBJ_ZSET)) return;

    // 保存有序集合对象的分值到score
    if (zsetScore(zobj,c->argv[2],&score) == C_ERR) {
        addReply(c,shared.nullbulk);
    } else {
        addReplyDouble(c,score);    //发送分值
    }
}

// ZRANK key member
// ZREVRANK key member
// ZRANK ZREVRANK命令底层实现
void zrankGenericCommand(client *c, int reverse) {
    robj *key = c->argv[1];
    robj *ele = c->argv[2];
    robj *zobj;
    unsigned long llen;
    unsigned long rank;

    // 以读操作取出有序集合对象
    if ((zobj = lookupKeyReadOrReply(c,key,shared.nullbulk)) == NULL ||
        checkType(c,zobj,OBJ_ZSET)) return;
    llen = zsetLength(zobj);    //获得集合元素数量

    serverAssertWithInfo(c,ele,sdsEncodedObject(ele));  //member参数必须是字符串类型对象

    // ziplist
    if (zobj->encoding == OBJ_ENCODING_ZIPLIST) {
        unsigned char *zl = zobj->ptr;
        unsigned char *eptr, *sptr;

        eptr = ziplistIndex(zl,0);  //元素节点地址
        serverAssertWithInfo(c,zobj,eptr != NULL);
        sptr = ziplistNext(zl,eptr);//分值节点地址
        serverAssertWithInfo(c,zobj,sptr != NULL);

        rank = 1;//排位
        while(eptr != NULL) {
            // 比较当前元素和member参数的大小
            if (ziplistCompare(eptr,ele->ptr,sdslen(ele->ptr)))
                break;
            rank++; //如果大于，排位加1
            zzlNext(zl,&eptr,&sptr);    //指向下个元素和分值
        }

        // 发送排位值
        if (eptr != NULL) {
            if (reverse)
                addReplyLongLong(c,llen-rank);
            else
                addReplyLongLong(c,rank-1);
        } else {
            addReply(c,shared.nullbulk);
        }
    // skiplist
    } else if (zobj->encoding == OBJ_ENCODING_SKIPLIST) {
        zset *zs = zobj->ptr;
        zskiplist *zsl = zs->zsl;
        dictEntry *de;
        double score;

        ele = c->argv[2];   //member参数
        de = dictFind(zs->dict,ele);    //查找保存member的节点地址
        if (de != NULL) {
            score = *(double*)dictGetVal(de);   //获得分值
            rank = zslGetRank(zsl,score,ele);   //计算排位
            serverAssertWithInfo(c,ele,rank); /* Existing elements always have a rank. */
            // 发送排位
            if (reverse)
                addReplyLongLong(c,llen-rank);
            else
                addReplyLongLong(c,rank-1);
        } else {
            addReply(c,shared.nullbulk);
        }
    } else {
        serverPanic("Unknown sorted set encoding");
    }
}

// ZRANK key member
// ZRANK 命令实现
void zrankCommand(client *c) {
    zrankGenericCommand(c, 0);
}

// ZREVRANK key member
// ZREVRANK 命令实现
void zrevrankCommand(client *c) {
    zrankGenericCommand(c, 1);
}

// ZSCAN key cursor [MATCH pattern] [COUNT count]
// ZSCAN 命令实现
void zscanCommand(client *c) {
    robj *o;
    unsigned long cursor;
    // 获得游标值，保存在cursor
    if (parseScanCursorOrReply(c,c->argv[2],&cursor) == C_ERR) return;
    // 以读操作取出有序集合对象
    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.emptyscan)) == NULL ||
        checkType(c,o,OBJ_ZSET)) return;
    // 调用底层的SCAN函数
    scanGenericCommand(c,o,cursor);
}
