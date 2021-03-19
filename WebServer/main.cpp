#include "http/http_conn.h"
#include "threadpool/threadpool.h"
#include <signal.h>
#include <assert.h>
#include "timer/lst_timer.h"
#include "webserver.h"

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

    WebServer server;

    //初始化
    server.init(port, 8);

    //线程池
    server.thread_pool();

    //监听
    server.startListen();

    //循环处理数据
    server.startLoop();
    return 0;
}