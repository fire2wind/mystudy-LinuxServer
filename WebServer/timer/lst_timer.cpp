#include "lst_timer.h"

sort_timer_lst::~sort_timer_lst(){
    util_timer* tmp = head;
    while(tmp){
        head = tmp->next;
        delete tmp;
        tmp = head;
    }
}

void sort_timer_lst::add_timer(util_timer* timer){
    if(!timer)
        return;
    if(!head)
        head = tail = timer;

    if(timer->expire < head->expire){
        timer->next = head;
        head->prev = timer;
        head = timer;
        return;
    }
    add_timer(timer, head);
}

void sort_timer_lst::add_timer(util_timer* timer, util_timer* lst_head){
    //这里传进来时就已经确认timer小于head了
    util_timer* cur = lst_head;
    util_timer* tmp = cur->next;

    while(tmp){
        if(timer->expire < tmp->expire){
            cur->next = timer;
            timer->next = tmp;
            timer->prev = cur;
            tmp->prev = timer;
            break;
        }
        cur = tmp;
        tmp = tmp->next;
    }
    //这里说明timer是最大的
    if(!tmp){
        cur->next = timer;
        timer->prev = cur;
        timer->next = NULL;
        //timer成为尾节点后，要更新尾节点
        tail = timer;   
    }
}

void sort_timer_lst::adjust_timer(util_timer* timer){
    if(!timer)
        return;
    util_timer* tmp = timer->next;
    if(!tmp || (timer->expire < tmp->expire))
        return;
    
    if(timer == head){
        head = head->next;
        head->prev = NULL;
        timer->next = NULL;
        add_timer(timer, head);
    }
    else{
        timer->prev->next = timer->next;
        timer->next->prev = timer->prev;
        add_timer(timer, timer->next);
    }

}

void sort_timer_lst::del_timer(util_timer* timer){
    if(!timer)
        return;
    if(timer == head && timer == tail){
        delete timer;
        head = NULL;
        tail = NULL;
        return;
    }

    if(timer == head){
        head = head->next;
        head->prev = NULL;
        delete timer;
        return;
    }

    if(timer == tail){
        tail = tail->prev;
        tail->next = NULL;
        delete timer;
        return;
    }

    timer->next->prev = timer->prev;
    timer->prev->next = timer->next;
    delete timer;
}

void sort_timer_lst::tick(){
    if(!head)
        return;

    //printf("timer tick\n");
    time_t cur = time(NULL);    //获取当前系统时间
    util_timer* tmp = head;

    while(tmp){
        if(cur < tmp->expire)
            break;
        
        tmp->cb_func(tmp->user_data);
        head = tmp->next;
        if(head){
            head->prev = NULL;
        }
        delete tmp;
        tmp = head;
    }
}

int *Utils::u_pipefd = 0;
int Utils::u_epollfd = 0;

void cb_func(client_data *user_data){
    epoll_ctl(Utils::u_epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0);
    assert(user_data);
    close(user_data->sockfd);
    http_conn::m_user_num--;
}

void Utils::init(int timeslot){
    m_TIMESLOT = timeslot;
}

int Utils::setnonblocking(int fd){
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

void Utils::addfd(int epfd, int fd, bool one_shot){
    epoll_event event;
    event.data.fd = fd;
    if(one_shot)
        event.events |= EPOLLONESHOT;
    epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

void Utils::sig_handler(int sig){
    //为保证函数的可重入性，保留原来的errno
    int save_errno = errno;
    int msg = sig;
    send(u_pipefd[1], (char*)&msg, 1, 0);
    errno = save_errno;
}

void Utils::addsig(int sig, void(handler)(int), bool restart){
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = handler;
    //一旦给信号设置了SA_RESTART标记，那么当执行某个阻塞系统调用，收到该信号时，进程不会返回，而是重新执行该系统调用。
    //即防止进程异常退出
    if(restart)
        sa.sa_flags |= SA_RESTART;
    sigfillset(&sa.sa_mask);
    assert(sigaction(sig, &sa, NULL) != -1);

}

void Utils::timer_handler(){
    m_timer_lst.tick();
    //因为一次 alarm 调用只会引起一次SIGALARM 信号，所以我们要重新定时，以不断触发 SIGALARM信号。
    alarm(m_TIMESLOT);
}

void Utils::show_error(int clifd, const char* info)
{
    send(clifd, info, strlen(info), 0);
    close(clifd);
}