#ifndef WEBSERVER_H
#define WEBSERVER_H

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <cassert>
#include <sys/epoll.h>

#include "./threadpool/threadpool.h"
#include "./http/http_conn.h"
#include "./timer/lst_timer.h"

const int MAX_FD = 65535;           //最大文件描述符
const int MAX_EVENT_NUMBER = 10000; //最大事件数
const int TIMESLOT = 5;             //超时时间

class WebServer{
public:
    WebServer();
    ~WebServer();

    void init(int port, int thread_num);
    
    void thread_pool();
    void startListen();
    void startLoop();
    bool dealclientdata();
    void timer(int clifd, const struct sockaddr_in& client_addr);
    void deal_timer(util_timer* timer, int sockfd);
    bool dealwithsignal(bool& timeout, bool& stop_server);
    void dealwithread(int sockfd);
    void adjust_timer(util_timer* timer);
    void dealwithwrite(int sockfd);

public:
    //线程池相关
    threadpool<http_conn> *m_pool;
    int m_thread_num;

    //epoll相关
    epoll_event events[MAX_EVENT_NUMBER];
    int m_epollfd;

    int m_pipefd[2];    //管道

    int m_port;         //端口号
    int m_listenfd;     //服务端套接字
    char* m_root_doc;   //根目录
    http_conn *users;   //解析用户信息

    //定时器
    client_data *users_timer;
    Utils utils;

};





#endif
