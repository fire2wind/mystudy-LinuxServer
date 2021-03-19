#include "webserver.h"

WebServer::WebServer()
{
    //最大可接受http_conn数量
    users = new http_conn[MAX_FD];

    //根目录
    char server_path[200];
    //将当前工作目录的绝对路径复制到server_path中
    getcwd(server_path, 200);
    const char *root = "/resources";
    m_root_doc = (char*)malloc(strlen(server_path) + strlen(root));
    strcpy(m_root_doc, server_path);
    strcat(m_root_doc, root);

    //定时器
    users_timer = new client_data[MAX_FD];
}

WebServer::~WebServer()
{
    close(m_epollfd);
    close(m_listenfd);
    close(m_pipefd[1]);
    close(m_pipefd[0]);
    delete[] users;
    delete[] users_timer;
    delete m_pool;
}

void WebServer::init(int port, int thread_num)
{
    m_port = port;
    m_thread_num = thread_num;
}

void WebServer::thread_pool()
{
    m_pool = new threadpool<http_conn>(m_thread_num);
}

void WebServer::startListen()
{
    m_listenfd = socket(AF_INET, SOCK_STREAM, 0);
    assert(m_listenfd >= 0);

    int ret = 0;
    struct sockaddr_in address;
    bzero(&address, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(m_port);

    int flag = 1;
    setsockopt(m_listenfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag));
    ret = bind(m_listenfd, (struct sockaddr*)&address, sizeof(address));
    assert(ret >= 0);
    ret = listen(m_listenfd,10);
    assert(ret >= 0);

    utils.init(TIMESLOT);

    epoll_event events[MAX_EVENT_NUMBER];
    m_epollfd = epoll_create(10);
    assert(m_epollfd != -1);

    utils.addfd(m_epollfd, m_listenfd, false);
    http_conn::m_epollfd = m_epollfd;

    //创建管道
    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, m_pipefd);
    assert(ret != -1);
    utils.setnonblocking(m_pipefd[1]);
    utils.addfd(m_epollfd, m_pipefd[0], false);

    utils.addsig(SIGPIPE, SIG_IGN, false); //忽略SIGPIPE信号，防止主线程退出
    utils.addsig(SIGALRM, utils.sig_handler, false); //定时信号
    utils.addsig(SIGTERM, utils.sig_handler, false); //异常信号

    alarm(TIMESLOT);

    Utils::u_epollfd = m_epollfd;
    Utils::u_pipefd = m_pipefd;
}

void WebServer::startLoop()
{
    bool timeout = false;
    bool stop_server = false;

    while(!stop_server){
        int num = epoll_wait(m_epollfd, events, MAX_EVENT_NUMBER, -1);
        if(num < 0 && errno != EINTR){
            perror("epoll_wait");
            break;
        }

        for(int i=0; i<num; i++){
            int sockfd = events[i].data.fd;

            if(sockfd == m_listenfd){
                bool flag = dealclientdata();
                if(flag == false)
                    continue;
            }
            else if(events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)){
                //服务器关闭该连接，并移除对应的定时器
                util_timer* timer = users_timer[sockfd].timer;
                deal_timer(timer, sockfd);
            }
            //处理信号
            else if((sockfd == m_pipefd[0]) && (events[i].events & EPOLLIN)){
                bool flag = dealwithsignal(timeout, stop_server);
                if(flag == false)
                    printf("dealwithsignal error\n");
            }
            //处理可读事件
            else if(events[i].events & EPOLLIN){
                dealwithread(sockfd);
            }
            //处理可读事件
            else if(events[i].events & EPOLLOUT){
                dealwithwrite(sockfd);
            }
        }
        if(timeout){
            utils.timer_handler();
            printf("timer tick\n");
            timeout = false;
        }
    }
}

bool WebServer::dealclientdata()
{
    struct sockaddr_in client_addr;
    socklen_t len = sizeof(client_addr);
    int clifd = accept(m_listenfd, (struct sockaddr*)&client_addr, &len);
    if(clifd < 0){
        return false;
    }
    if(http_conn::m_user_num >= MAX_FD){
        utils.show_error(clifd, "Server busy");
        return false;
    }
    timer(clifd, client_addr);
    return true;
}

void WebServer::timer(int clifd, const struct sockaddr_in& client_addr)
{
    users[clifd].init(clifd, client_addr, m_root_doc);

    //初始化client_data数据
    users_timer[clifd].address = client_addr;
    users_timer[clifd].sockfd = clifd;
    //创建定时器
    util_timer* timer = new util_timer;
    //绑定用户数据
    timer->user_data = &users_timer[clifd];
    //设置回调函数
    timer->cb_func = cb_func;
    //设置超时时间
    time_t cur = time(NULL);
    timer->expire = cur + 3 * TIMESLOT;
    users_timer[clifd].timer = timer;
    //将定时器添加到链表中
    utils.m_timer_lst.add_timer(timer);
}

void WebServer::deal_timer(util_timer* timer, int sockfd)
{
    timer->cb_func(&users_timer[sockfd]);
    if(timer){
        utils.m_timer_lst.del_timer(timer);
    }
    printf("close fd: %d\n", users_timer[sockfd].sockfd);
}

bool WebServer::dealwithsignal(bool& timeout, bool& stop_server)
{
    int ret = 0;
    int sig;
    char signals[1024];
    ret = recv(m_pipefd[0], signals, sizeof(signals), 0);
    //出错
    if(ret == -1){
        return false;
    }
    //没读到数据
    else if(ret == 0){
        return false;
    }
    else{
        for(int i=0; i<ret; i++){
            switch (signals[i])
            {
            //发生SIGALRM，表示超时
            case SIGALRM:
            {
                //用timeout变量标记有定时任务需要处理，但不立即处理定时任务
                //这是因为定时任务的优先级不是很高，我们优先处理其他更重要的任务。
                timeout = true;
                break;
            }
            //产生了终端信号
            case SIGTERM:
            {
                stop_server = true;
                break;
            }
            
            default:
                break;
            }
        }
    }
    return true;
}

void WebServer::dealwithread(int sockfd)
{
    util_timer* timer = users_timer[sockfd].timer;

    //这里只采用模拟proactor模式
    if(users[sockfd].read()){
        printf("client IP:%s\n", inet_ntoa(users[sockfd].get_address()->sin_addr));
        //读取到了数据，将该事件加入请求队列中
        m_pool->append(users + sockfd);
    
        if(timer){
            adjust_timer(timer);
        }
    }
    else{
        deal_timer(timer, sockfd);
    }

}

//重新设置定时器
void WebServer::adjust_timer(util_timer* timer)
{
    time_t cur = time(NULL);
    timer->expire = cur + 3 * TIMESLOT;
    utils.m_timer_lst.adjust_timer(timer);

    printf("adjust time once\n");
}

void WebServer::dealwithwrite(int sockfd)
{
    util_timer* timer = users_timer[sockfd].timer;

    //模拟proactor模式
    if(users[sockfd].write()){
        //printf("send data to IP:%s\n", inet_ntoa(users[sockfd].get_address()->sin_addr));
        if(timer){
            adjust_timer(timer);
        }
    }
    else{
        deal_timer(timer, sockfd);
    }

}