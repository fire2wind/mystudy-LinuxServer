#include "http/http_conn.h"
#include "threadpool/threadpool.h"
#include <signal.h>
#include <assert.h>

#define MAX_FD 2048
#define MAX_EVENT_NUM 4096

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

    //用于保存客户端信息的数组
    http_conn* users = new http_conn[MAX_FD];

    //创建套接字
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if(fd == -1){
        perror("socket");
        return -1;
    }
    
    struct sockaddr_in saddr;
    saddr.sin_family = AF_INET;
    saddr.sin_addr.s_addr = INADDR_ANY;
    saddr.sin_port = htons(port);

    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    if(bind(fd, (struct sockaddr*)&saddr, sizeof(saddr)) == -1){
        perror("bind");
        exit(-1);
    }
    if(listen(fd, 10) == -1){
        perror("listen");
        exit(-1);
    }

    //创建epoll_event数组
    epoll_event events[MAX_EVENT_NUM];
    int epfd = epoll_create(10);
    addfd(epfd, fd, false);
    http_conn::m_epollfd = epfd;

    while(true){
        int ret = epoll_wait(epfd, events, MAX_EVENT_NUM, -1);
        if(ret < 0 && errno != EINTR){
            perror("epoll_wait");
            break;
        }

        for(int i = 0; i < ret; i++){
            int sockfd = events[i].data.fd;
            if(sockfd == fd){
                struct sockaddr_in cliaddr;
                socklen_t c_len = sizeof(cliaddr);
                int clifd;
                if((clifd = accept(fd, (struct sockaddr*)&cliaddr, &c_len)) < 0){
                    printf("error: %d\n", errno);
                    continue;
                }

                //不能超过最大连接数
                if(http_conn::m_user_num >= MAX_FD){
                    close(clifd);
                    continue;
                }
                users[clifd].init(clifd, cliaddr);
            }
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
        }
    }

    close(epfd);
    close(fd);
    delete [] users;
    delete pool;
    return 0;
}