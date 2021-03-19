#ifndef LST_TIMER
#define LST_TIMER

#include <cstdio>
#include <ctime>
#include <arpa/inet.h>
#include "../http/http_conn.h"
#include <assert.h>
#include <signal.h>

#define BUFFER_SIZE 1024
class util_timer;

//用户数据结构
struct client_data{
    sockaddr_in address;        //客户端socket地址
    int sockfd;                 //socket文件描述符
    char buf[BUFFER_SIZE];      //读缓存
    util_timer* timer;          //定时器
};

//定时器类
class util_timer{
public:
    util_timer():prev(NULL), next(NULL){}

public:
    util_timer* prev;
    util_timer* next;
    client_data* user_data;
    time_t expire;
    void (*cb_func)(client_data*); //任务回调函数，回调函数处理的客户数据，由定时器的执行者传递给回调函数

};

class sort_timer_lst{
public:
    sort_timer_lst():head(NULL), tail(NULL){}
    ~sort_timer_lst();

    void add_timer(util_timer* timer);
    void adjust_timer(util_timer* timer);
    void del_timer(util_timer* timer);
    void tick();


private:
    void add_timer(util_timer* timer, util_timer* lst_head);
    util_timer* head;
    util_timer* tail;

};

//工具类，对定时器进行操作
class Utils{
public:
    Utils() {}
    ~Utils() {}

    //设置超时时间
    void init(int timeslot);

    //对文件描述符设置非阻塞
    int setnonblocking(int fd);

    //向内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
    void addfd(int epfd, int fd, bool one_shot);

    //信号处理函数
    static void sig_handler(int sig);

    //设置信号函数
    void addsig(int sig, void(handler)(int), bool restart);

    //定时处理任务，重新定时以不断触发SIGALRM信号
    void timer_handler();

    void show_error(int clifd, const char* info);

public:
    static int* u_pipefd;
    sort_timer_lst m_timer_lst;
    static int u_epollfd;
    int m_TIMESLOT;
    
};

void cb_func(client_data *user_data);
#endif