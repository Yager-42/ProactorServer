#ifndef QUESTQUEUE
#define QUESTQUEUE

#include <list>
using namespace std;
#include "locker.h"

template <typename T>
class questqueue
{
private:
    list<T*> m_questqueue;
    int m_max_queue;//最大请求长度
    mutex m_queue_mutex;//请求队列互斥锁
    sem m_queue_sem;//当前有无任务需要处理的信号量
public:
    questqueue(int max_queue);
    ~questqueue();
    T* pop();
    bool push(T* quest);
};


template<typename T>
questqueue<T>::questqueue(int max_queue)
{
    m_max_queue = max_queue;
    if(max_queue<=0)
    {
        throw "队列的大小错误";
    }
}

template<typename T>
questqueue<T>::~questqueue()
{
    for(auto it = m_questqueue.begin();it!=m_questqueue.end();++it)
    {
        delete *it;
    }
}

template<typename T>
bool questqueue<T>::push(T* quest)
{
    m_queue_mutex.lock();
    if(m_questqueue.size()>=m_max_queue)
    {
        return false;
    }
    m_questqueue.push_back(quest);
    m_queue_sem.post();

    m_queue_mutex.unlock();
    return 1;
}

template<typename T>
T* questqueue<T>::pop()
{
    m_queue_sem.wait();
    m_queue_mutex.lock();
    if(m_questqueue.empty())
    {
        m_queue_mutex.unlock();
        return NULL;
    }
    T* quest = m_questqueue.front();
    m_questqueue.pop_front();

    m_queue_mutex.unlock();
    return quest;
}

#endif