// main.c : 此文件包含 "main" 函数。程序执行将在此处开始并结束。
//

#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <limits.h>
#include <string.h>
#include <errno.h>

#define _USE_MATH_DEFINES   1

#ifndef __USE_MISC
#define __USE_MISC          1
#endif // !__USE_MISC

#include <math.h>

#if _WIN32
#include <Windows.h>
#include <process.h>

typedef HANDLE      zc_thread_object;
typedef HANDLE      zc_semaphore_object;
typedef unsigned    zc_thread_handler_rettype;

enum
{
    ZC_SEMAPHORE_ERRCODE_TIMEOUT = (int)WAIT_TIMEOUT
};

static inline void* zc_virtual_mem_alloc(void* startingAddress, size_t allocSize)
{
    return VirtualAlloc(startingAddress, allocSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
}

static inline void zc_virtual_mem_free(void* pMem, size_t allocSize)
{
    VirtualFree(pMem, 0U, MEM_RELEASE);
}

static inline int zc_thread_create(zc_thread_object *pThreadObject, zc_thread_handler_rettype(*pThreadHandler)(void*), void* arg)
{
    HANDLE threadObject = (HANDLE)_beginthreadex(NULL, 0, pThreadHandler, arg, 0, NULL);
    if (threadObject == NULL) return errno;

    if (pThreadObject != NULL) {
        *pThreadObject = threadObject;
    }

    return 0;
}

static inline int zc_thread_destroy(zc_thread_object threadObject)
{
    return CloseHandle(threadObject) ? 0 : GetLastError();
}

static inline void zc_thread_exit(zc_thread_handler_rettype retval)
{
    _endthreadex(retval);
}

static inline int zc_thread_join(zc_thread_object threadObject, zc_thread_handler_rettype* pRetVal)
{
    const int status = (int)WaitForSingleObject(threadObject, INFINITE);
    if (pRetVal != NULL)
    {
        DWORD exitCode = 0U;
        if (GetExitCodeThread(threadObject, &exitCode)) {
            *pRetVal = (zc_thread_handler_rettype)exitCode;
        }
    }

    return status;
}

static inline int zc_semaphore_create(zc_semaphore_object* pSemObj, int initialValue)
{
    HANDLE semObj = CreateSemaphoreA(NULL, initialValue, INT_MAX, NULL);
    if (semObj == NULL) return GetLastError();

    *pSemObj = semObj;
    return 0;
}

static inline int zc_semaphore_destroy(zc_semaphore_object* pSemObj)
{
    return CloseHandle(*pSemObj) ? 0 : GetLastError();
}

static inline int zc_semaphore_wait(zc_semaphore_object* pSemObj)
{
    return (int)WaitForSingleObject(*pSemObj, INFINITE);
}

static inline int zc_semaphore_wait_with_timeout(zc_semaphore_object* pSemObj, unsigned seconds)
{
    return (int)WaitForSingleObject(*pSemObj, seconds * 1000UL);
}

static inline int zc_semaphore_post(zc_semaphore_object* pSemObj)
{
    return ReleaseSemaphore(*pSemObj, 1L, NULL) ? 0 : GetLastError();
}

#else
#include <unistd.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <pthread.h>
#include <semaphore.h>

typedef pthread_t   zc_thread_object;
typedef sem_t       zc_semaphore_object;
typedef void*       zc_thread_handler_rettype;

enum
{
    ZC_SEMAPHORE_ERRCODE_TIMEOUT = ETIMEDOUT
};

static inline void* zc_virtual_mem_alloc(void* startingAddress, size_t allocSize)
{
    void* pMem = mmap(startingAddress, allocSize, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (pMem == NULL) return NULL;

    mprotect(pMem, allocSize, PROT_READ | PROT_WRITE);

    return pMem;
}

static inline void zc_virtual_mem_free(void* pMem, size_t allocSize)
{
    munmap(pMem, allocSize);
}

static inline int zc_thread_create(zc_thread_object* pThreadObject, zc_thread_handler_rettype(*pThreadHandler)(void*), void* arg)
{
    return pthread_create(pThreadObject, NULL, pThreadHandler, arg);
}

static inline int zc_thread_destroy(zc_thread_object threadObject)
{
    return 0;
}

static inline void zc_thread_exit(zc_thread_handler_rettype retval)
{
    pthread_exit(retval);
}

static inline int zc_thread_join(zc_thread_object threadObject, zc_thread_handler_rettype* pRetVal)
{
    return pthread_join(threadObject, pRetVal);
}

static inline int zc_semaphore_create(zc_semaphore_object* pSemObj, int initialValue)
{
    return sem_init(pSemObj, 0, initialValue) == 0 ? 0 : errno;
}

static inline int zc_semaphore_destroy(zc_semaphore_object* pSemObj)
{
    return sem_destroy(pSemObj) == 0 ? 0 : errno;
}

static inline int zc_semaphore_wait(zc_semaphore_object* pSemObj)
{
    return sem_wait(pSemObj) == 0 ? 0 : errno;
}

static inline int zc_semaphore_wait_with_timeout(zc_semaphore_object* pSemObj, unsigned seconds)
{
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    ts.tv_sec += seconds;
    return sem_timedwait(pSemObj, &ts) == 0 ? 0 : errno;
}

static inline int zc_semaphore_post(zc_semaphore_object* pSemObj)
{
    return sem_post(pSemObj) == 0 ? 0 : errno;
}

#endif

struct ThreadArgList
{
    uint8_t* contentBuffer;
    uint8_t* readBuffer;
    size_t packetSize;
    size_t totalDataSize;
};

static zc_semaphore_object s_syncSemaphore = { 0 };

static zc_thread_handler_rettype ThreadProcHandler(void* arg)
{
    struct ThreadArgList* argList = (struct ThreadArgList*)arg;
    uint8_t* const contentBuffer = argList->contentBuffer;
    const uint8_t* const readBuffer = argList->readBuffer;
    const size_t packetSize = argList->packetSize;
    const size_t totalDataSize = argList->totalDataSize;

    zc_thread_handler_rettype retVal = (zc_thread_handler_rettype)100;
    size_t offset = 0U;
    while (offset < totalDataSize)
    {
        const int status = zc_semaphore_wait_with_timeout(&s_syncSemaphore, 5U);
        if (status != 0)
        {
            if (status == ZC_SEMAPHORE_ERRCODE_TIMEOUT)
            {
                retVal = (zc_thread_handler_rettype)123;
                fprintf(stderr, "semaphore wait timeout!\n");
                break;
            }
            else
            {
                retVal = (zc_thread_handler_rettype)321;
                fprintf(stderr, "semaphore wait failed with error: %d\n", status);
                break;
            }
        }

        memcpy(&contentBuffer[offset], &readBuffer[offset], packetSize);

        offset += packetSize;
    }

    zc_thread_exit(retVal);
    return (zc_thread_handler_rettype)retVal;
}

int main(int argc, const char* argv[])
{
#if _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif

    printf("π + e = %f\n", M_PI + M_E);

    enum {
        DATA_PACKET_SIZE = 1024 * 1024,
        TOTAL_DATA_SIZE = DATA_PACKET_SIZE * 64,
        TOTAL_ELEM_COUNT = TOTAL_DATA_SIZE / (int)sizeof(int)
    };

    void* contentBuffer = calloc(TOTAL_DATA_SIZE, 1U);
    if (contentBuffer == NULL)
    {
        fprintf(stderr, "contentBuffer allocation failed!\n");
        return 1;
    }

    int result = 0;
    void* readBuffer = NULL;
    zc_thread_object threadObject = { 0 };

    do
    {
        readBuffer = zc_virtual_mem_alloc(NULL, TOTAL_DATA_SIZE);
        if (readBuffer == NULL)
        {
            result = 1;
            fprintf(stderr, "readBuffer allocation failed!\n");
            break;
        }

        result = zc_semaphore_create(&s_syncSemaphore, 0);
        if (result != 0)
        {
            fprintf(stderr, "Semaphore creation failed with error: %d\n", result);
            break;
        }

        struct ThreadArgList argList = {
            .contentBuffer = contentBuffer,
            .readBuffer = readBuffer,
            .packetSize = DATA_PACKET_SIZE,
            .totalDataSize = TOTAL_DATA_SIZE
        };

        result = zc_thread_create(&threadObject, &ThreadProcHandler, &argList);
        if (result != 0)
        {
            fprintf(stderr, "Thread creation failed with error: %d\n", result);
            break;
        }

        // Filling readBuffer
        int* pBuf = readBuffer;
        for (int i = 0, packetIndex = 0; i < TOTAL_ELEM_COUNT; ++i)
        {
            pBuf[i] = i;
            
            packetIndex += (int)sizeof(int);
            if (packetIndex == DATA_PACKET_SIZE)
            {
                packetIndex = 0;
                result = zc_semaphore_post(&s_syncSemaphore);
                if (result != 0) {
                    fprintf(stderr, "semaphore post failed with error: %d\n", result);
                }
            }
        }

        zc_thread_handler_rettype threadExitCode = 0;
        int result = zc_thread_join(threadObject, &threadExitCode);
        if (result != 0)
        {
            fprintf(stderr, "thread join failed with error: %d\n", result);
            break;
        }

        // Verify the result
        pBuf = contentBuffer;
        for (int i = 0; i < TOTAL_ELEM_COUNT; ++i)
        {
            if (pBuf[i] != i)
            {
                result = 1;
                fprintf(stderr, "contentBuffer not correct @%d, value is: %d\n", i, pBuf[i]);
                break;
            }
        }
        if (result == 0) {
            puts("Verification completed!");
        }

        printf("Thread exit code is: %lld\n", (long long)threadExitCode);
    }
    while (false);

    free(contentBuffer);

    if (readBuffer != NULL) {
        zc_virtual_mem_free(readBuffer, TOTAL_DATA_SIZE);
    }
    if (threadObject != (zc_thread_object)0) {
        zc_thread_destroy(threadObject);
    }

    result = zc_semaphore_destroy(&s_syncSemaphore);
    if (result != 0) {
        fprintf(stderr, "semaphore destruction failed with error: %d\n", result);
    }

    return result;
}

// 运行程序: Ctrl + F5 或调试 >“开始执行(不调试)”菜单
// 调试程序: F5 或调试 >“开始调试”菜单

// 入门使用技巧: 
//   1. 使用解决方案资源管理器窗口添加/管理文件
//   2. 使用团队资源管理器窗口连接到源代码管理
//   3. 使用输出窗口查看生成输出和其他消息
//   4. 使用错误列表窗口查看错误
//   5. 转到“项目”>“添加新项”以创建新的代码文件，或转到“项目”>“添加现有项”以将现有代码文件添加到项目
//   6. 将来，若要再次打开此项目，请转到“文件”>“打开”>“项目”并选择 .sln 文件

