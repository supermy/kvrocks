#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <time.h>

#define MAX_OBJECT_NUMBER       (1024)
// #define MAX_MEMORY_SIZE         (1024*100)
#define MAX_MEMORY_SIZE         (1024*20)

struct BufferUnit{
   int   size;
   char* data;
};

struct BufferUnit   buffer_units[MAX_OBJECT_NUMBER];

void MallocBuffer(int buffer_size) {

for(int i=0; i<MAX_OBJECT_NUMBER; ++i)  {
    if (NULL != buffer_units[i].data)   continue;

    buffer_units[i].data = (char*)malloc(buffer_size);
    if (NULL == buffer_units[i].data)  continue;

    memset(buffer_units[i].data, 0x01, buffer_size);
    buffer_units[i].size = buffer_size;
    }
}

void FreeHalfBuffer(bool left_half_flag) {
    int half_index = MAX_OBJECT_NUMBER / 2;
    int min_index = 0;
    int max_index = MAX_OBJECT_NUMBER-1;
    if  (left_half_flag)
        max_index =  half_index;
    else
        min_index = half_index;

    for(int i=min_index; i<=max_index; ++i) {
        if (NULL == buffer_units[i].data) continue;

        free(buffer_units[i].data);
        buffer_units[i].data =  NULL;
        buffer_units[i].size = 0;
    }
}

// JeMalloc使用指南
// 1、JeMalloc库简介
// JeMalloc提供了静态库libjemalloc.a和动态库libjemalloc.so，默认安装在/usr/local/lib目录。

// 2、JeMalloc动态方式
// 通过-ljemalloc将JeMalloc链接到应用程序。
// 通过LD_PRELOAD预载入JeMalloc库可以不用重新编译应用程序即可使用JeMalloc。
// LD_PRELOAD="/usr/lib/libjemalloc.so"

// 3、JeMalloc静态方式
// 在编译选项的最后加入/usr/local/lib/libjemalloc.a链接静态库。

// 4、JeMalloc生效
// jemalloc利用malloc的hook来对代码中的malloc进行替换。

// JEMALLOC_EXPORT void (*__free_hook)(void *ptr) = je_free;
// JEMALLOC_EXPORT void *(*__malloc_hook)(size_t size) = je_malloc;
// JEMALLOC_EXPORT void *(*__realloc_hook)(void *ptr, size_t size) = je_realloc;


// 使用TCMalloc编译链接：
// g++ malloc.cpp -o test -ljemalloc
// 执行test，耗时558秒。
// 使用默认GLibc编译链接：
// g++ malloc.cpp -o test
// 执行test，耗时744秒。

int main() {
    memset(&buffer_units, 0x00, sizeof(buffer_units));
    int decrease_buffer_size = MAX_MEMORY_SIZE;
    bool left_half_flag   =   false;
    time_t  start_time = time(0);
    while(1)  {
        MallocBuffer(decrease_buffer_size);
        FreeHalfBuffer(left_half_flag);
        left_half_flag = !left_half_flag;
        --decrease_buffer_size;
        if (0 == decrease_buffer_size) break;
    }
    FreeHalfBuffer(left_half_flag);
    time_t end_time = time(0);
    long elapsed_time = difftime(end_time, start_time);

    printf("Used %ld seconds. \n", elapsed_time);
    return 1;
}