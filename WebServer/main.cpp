#include "http/http_conn.h"
#include "threadpool/threadpool.h"
#include <signal.h>
#include <assert.h>
#include "timer/lst_timer.h"

#define MAX_FD 2048
#define MAX_EVENT_NUM 4096
#define TIMESLOT 5
//添加文件描述符
extern void addfd(int epfd, int fd, bool one_shot);

//删除文件描述符
extern void removefd(int epfd, int fd);

//修改文件描述符，ev代表事件
extern void modfd(int epfd, int fd, int ev);

//信号处理函数，添加信号捕捉
void addsig(int sig, void(handler)(int))
{
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = handler;
    sigfillset(&sa.sa_mask);        //所有mask置1
    assert(sigaction(sig, &sa, NULL) != -1);      //捕捉信号

}

int main(int argc, char* argv[])
{
    if(argc <= 1){
        //basename: 从文件名中去掉目录和后缀
        printf("按照如下格式运行: %s 端口号\n", basename(argv[0]));
        return -1;
    }

    int port = atoi(argv[1]);

    //对SIGPIE信号进行处理，忽略SIGPIPE信号，防止主线程退出
    addsig(SIGPIPE, SIG_IGN);

    //创建线程池，并初始化
    threadpool<http_conn>* pool = NULL;
    try{
        pool = new threadpool<http_conn>;
    }
    catch(...){
        return -1;
    }

    

    //创建套接字
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    
    int ret = 0;
    struct sockaddr_in saddr;
    bzero(&saddr, sizeof(saddr));
    saddr.sin_family = AF_INET;
    saddr.sin_addr.s_addr = INADDR_ANY;
    saddr.sin_port = htons(port);

    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    ret = bind(fd, (struct sockaddr*)&saddr, sizeof(saddr));
    assert(ret != -1);
    
    ret = listen(fd, 10);
    assert(ret != -1);
    
    //创建epoll_event数组
    epoll_event events[MAX_EVENT_NUM];
    int epfd = epoll_create(10);
    addfd(epfd, fd, false);
    http_conn::m_epollfd = epfd;

    //创建管道
    int pipefd[2];
    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd);
    assert(ret != -1);
    //添加到工具类，并设置管道
    Utils utils;
    utils.init(5);
    utils.setnonblocking(pipefd[1]);
    addfd(epfd, pipefd[0], false);

    //设置信号
    utils.addsig(SIGPIPE, SIG_IGN, false);
    utils.addsig(SIGALRM, utils.sig_handler, false);
    utils.addsig(SIGTERM, utils.sig_handler, false);

    alarm(TIMESLOT);

    Utils::u_pipefd = pipefd;
    Utils::u_epollfd = epfd;

    //用于保存客户端信息的数组
    client_data* users = new client_data[MAX_FD];

    bool timeout = false;
    bool stop_server = false;

    while(!stop_server){
        int number = epoll_wait(epfd, events, MAX_EVENT_NUM, -1);
        if(number < 0 && errno != EINTR){
            perror("epoll_wait");
            break;
        }

        for(int i = 0; i < number; i++){
            int sockfd = events[i].data.fd;
            if(sockfd == fd){
                struct sockaddr_in cliaddr;
                socklen_t c_len = sizeof(cliaddr);
                int clifd;
                if((clifd = accept(fd, (struct sockaddr*)&cliaddr, &c_len)) < 0){
                    printf("error: %d\n", errno);
                    continue;
                }
                addfd(epfd, clifd, false);
                users[clifd].address = cliaddr;
                users[clifd].sockfd = clifd;

                // 创建定时器，设置其回调函数与超时时间，然后绑定定时器与用户数据，最后将定时器添加到链表timer_lst中
                util_timer* timer = new util_timer;
                timer->user_data = &users[clifd];
                timer->cb_func = cb_func;
                time_t cur = time(NULL);
                timer->expire = cur + 3 * TIMESLOT;
                users[clifd].timer = timer;
                utils.m_timer_lst.add_timer(timer);
                //不能超过最大连接数
                if(http_conn::m_user_num >= MAX_FD){
                    close(clifd);
                    continue;
                }
                //users[clifd].init(clifd, cliaddr);
            }
            /*
            else if(events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)){
                //对方异常断开或错误
                users[sockfd].close_conn();
            }
            
            else if(events[i].events & EPOLLIN){
                //一次性读完所有数据后，交给工作线程去处理
                if(users[sockfd].read())
                    pool->append(users + sockfd);
                else
                    users[sockfd].close_conn();
            }
            else if(events[i].events & EPOLLOUT){
                //一次性写完所有数据
                if(!users[sockfd].write())
                    users[sockfd].close_conn();
            }
            */
            else if((sockfd == pipefd[0]) && (events[i].events & EPOLLIN)){
                //管道里收到信号，处理信号
                int sig;
                char signals[1024];
                ret = recv(pipefd[0], signals, sizeof(signals), 0);
                if(ret == -1 || ret == 0){
                    continue;
                }
                else{
                        for(int i=0; i<ret; i++){
                            switch( signals[i] ){
                                case SIGALRM:
                                {
                                    //用timeout变量标记有定时任务需要处理，但不立即处理定时任务
                                    //这是因为定时任务的优先级不是很高，我们优先处理其他更重要的任务。
                                    timeout = true;
                                    break;
                                }
                                case SIGTERM:
                                {
                                    stop_server = true;
                                }
                            }
                    }
                }
            }
            else if(events[i].events & EPOLLIN){
                memset(users[sockfd].buf, '\0', BUFFER_SIZE);
                ret = recv(sockfd, users[sockfd].buf, BUFFER_SIZE, 0);
                printf( "get %d bytes of client data %s from %d\n", ret, users[sockfd].buf, sockfd );
                util_timer* timer = users[sockfd].timer;
                if(ret < 0){
                    //如果发生读错误，则关闭连接，并移除其对应的定时器
                    if(errno != EAGAIN){
                        cb_func(&users[sockfd]);
                        if(timer)
                            utils.m_timer_lst.del_timer(timer);
                    }
                }
                else if(ret == 0){
                    //如果对方已经关闭连接，则我们也关闭连接，并移除对应的定时器。
                    cb_func(&users[sockfd]);
                    if(timer)
                        utils.m_timer_lst.del_timer(timer);
                }
                else{
                    //如果某个客户端上有数据可读，则我们要调整该连接对应的定时器，以延迟该连接被关闭的时间。
                    if(timer){
                        time_t cur = time(NULL);
                        timer->expire = cur + 3 * TIMESLOT;
                        printf( "adjust timer once\n" );
                        utils.m_timer_lst.adjust_timer(timer);
                    }
                }
            }
        }
        //最后处理定时事件，因为I/O事件有更高的优先级。当然，这样做将导致定时任务不能精准的按照预定的时间执行。
        if(timeout){
            utils.timer_handler();
            timeout = false;
        }
    }

    close(epfd);
    close(fd);
    close(pipefd[1]);
    close(pipefd[0]);
    delete [] users;
    delete pool;
    return 0;
}