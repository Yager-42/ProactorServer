#ifndef THREADPOOL
#define THREADPOOL

#include <list>
#include "locker.h"
#include "questqueue.h"

template <typename T>
class threadPool
{
public:
    threadPool(int poolSize = 8, int maxquest = 1000);
    ~threadPool();

    bool append(T *quest);

private:
    static void *worker(void *arg); //子线程调用的函数
    void run();

private:
    questqueue<T> m_questqueue; //请求队列
    int m_thread_poolsize;      //线程池中线程数量
    pthread_t *m_threads;       //线程池，数组实现

    bool m_stoppool;
};

template <typename T>
threadPool<T>::threadPool(int poolsize, int maxquest) : m_thread_poolsize(poolsize), m_stoppool(false), m_questqueue(maxquest)
{
    if (poolsize <= 0)
    {
        throw "线程池的大小错误";
    }

    m_threads = new pthread_t[m_thread_poolsize];
    for (int i = 0; i < m_thread_poolsize; ++i)
    {
        printf("create the %dth thread\n", i + 1);
        if (pthread_create(m_threads + i, NULL, worker, this) != 0)
        {
            //有一个创建错误就全部删除
            delete[] m_threads;
            throw "创建子线程时错误";
        }
    }
    for (int i = 0; i < m_thread_poolsize; ++i)
    {
        if (pthread_detach(m_threads[i]) != 0)
        {
            delete[] m_threads;
            throw "子线程分离时错误";
        }
    }
    printf("线程池初始化完成\n");
}

template <typename T>
threadPool<T>::~threadPool()
{
    m_stoppool = 1;
    delete[] m_threads;
}

template <typename T>
bool threadPool<T>::append(T *quest)
{
    return m_questqueue.push(quest);
}

template <typename T>
void *threadPool<T>::worker(void *arg)
{
    threadPool *pool = (threadPool *)arg;
    pool->run();
    return pool;
}

template <typename T>
void threadPool<T>::run()
{
    while (!m_stoppool)
    {
        T *quest = m_questqueue.pop();
        if (quest != NULL)
        {
            quest->process();
        }
    }
}

#endif