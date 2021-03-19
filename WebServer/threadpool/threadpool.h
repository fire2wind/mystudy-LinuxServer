#ifndef THREADPOOL_H
#define THREADPOOL_H

#include "../lock/locker.h" 
#include <list>
#include "../http/http_conn.h"


template<typename T>
class threadpool
{
private:
    int m_thread_num;           //线程数量
    int m_max_request_num;      //最大的请求数量
    pthread_t *m_threads;       //存储线程池的数组
    bool m_stop;                //标识线程是否结束, true表示结束
    std::list<T*> m_workqueue;  //请求队列
    locker m_lock;              //互斥锁
    sem m_sem;                  //是否有任务要处理

public:
    threadpool(int thread_num = 8, int max_request_num = 10000);
    ~threadpool();
    bool append(T* request);

private:
    static void* worker(void* arg);
    void run();
};

template<typename T>
threadpool<T>::threadpool(int thread_num, int request_num):
    m_thread_num(thread_num),m_max_request_num(request_num),
    m_stop(false), m_threads(NULL)
{
    if(thread_num <= 0 || request_num <= 0)
        throw std::exception();

    m_threads = new pthread_t[m_thread_num];
    if(!m_threads)
        throw std::exception();
    
    for(int i = 0; i < m_thread_num; i++){
        printf("create %dth thread\n", i);
        if(pthread_create(m_threads + i, NULL, worker, this) != 0){
            delete [] m_threads;
            throw std::exception();
        }
        if(pthread_detach(m_threads[i]) != 0){
            delete [] m_threads;
            throw std::exception();
        }
    }
}

template<typename T>
threadpool<T>::~threadpool()
{
    delete [] m_threads;
    m_stop = true;
}

template<typename T>
bool threadpool<T>::append(T* request)
{
    m_lock.lock();
    if(m_workqueue.size() >= m_max_request_num){
        m_lock.unlock();
        return false;
    }
    m_workqueue.push_back(request);
    m_lock.unlock();
    m_sem.post();       //添加了一个请求，信号量+1
    return true;
}

template<typename T>
void* threadpool<T>::worker(void* arg)
{
    threadpool* pool = (threadpool*)arg;
    pool->run();
    return pool;
}

template<typename T>
void threadpool<T>::run()
{
    while(!m_stop){
        m_sem.wait();               //处理请求，信号量-1，信号量为0会阻塞
        m_lock.lock();
        if(m_workqueue.empty()){    
            m_lock.unlock();
            continue;
        }
        T* request = m_workqueue.front();
        m_workqueue.pop_front();
        m_lock.unlock();
        if(!request){               //请求为空不需要处理
            continue;
        }
        request->process();
    }
}
#endif