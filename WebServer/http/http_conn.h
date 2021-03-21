#ifndef HTTPCONN_H
#define HTTPCONN_H

#include <cstring>
#include <cstdio>
#include <arpa/inet.h>
#include <cstdlib>
#include <errno.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <sys/uio.h>


class http_conn{
public:
    //报文的请求方法
    enum METHOD {
        GET = 0, 
        POST, 
        HEAD, 
        PUT, 
        DELETE, 
        TRACE, 
        OPTIONS, 
        CONNECT
    };
    
    /*
        解析客户端请求时，主状态机的状态
        CHECK_STATE_REQUESTLINE:当前正在分析请求行
        CHECK_STATE_HEADER:当前正在分析头部字段
        CHECK_STATE_CONTENT:当前正在解析请求体
    */
    enum CHECK_STATE { 
        CHECK_STATE_REQUESTLINE = 0, 
        CHECK_STATE_HEADER, 
        CHECK_STATE_CONTENT 
    };
    
    /*
        服务器处理HTTP请求的可能结果，报文解析的结果
        NO_REQUEST          :   请求不完整，需要继续读取客户数据
        GET_REQUEST         :   表示获得了一个完成的客户请求
        BAD_REQUEST         :   表示客户请求语法错误
        NO_RESOURCE         :   表示服务器没有资源
        FORBIDDEN_REQUEST   :   表示客户对资源没有足够的访问权限
        FILE_REQUEST        :   文件请求,获取文件成功
        INTERNAL_ERROR      :   表示服务器内部错误
        CLOSED_CONNECTION   :   表示客户端已经关闭连接了
    */
    enum HTTP_CODE { 
        NO_REQUEST, 
        GET_REQUEST, 
        BAD_REQUEST, 
        NO_RESOURCE, 
        FORBIDDEN_REQUEST, 
        FILE_REQUEST, 
        INTERNAL_ERROR, 
        CLOSED_CONNECTION 
    };
    
    // 从状态机的三种可能状态，即行的读取状态，分别表示
    // 0.读取到一个完整的行 1.行出错 2.行数据尚且不完整
    enum LINE_STATUS { 
        LINE_OK = 0, 
        LINE_BAD, 
        LINE_OPEN 
    };

public:
    http_conn() {}
    ~http_conn() {}

    void process();     //处理请求并响应
    void init(int clifd, const sockaddr_in& cliaddr, char* root);   //初始化连接
    void close_conn();  //关闭连接

    bool read();        //非阻塞读
    bool write();       //非阻塞写
    sockaddr_in *get_address(){
        return &m_addr;
    }

private:
    void init();    //初始化成员变量
    HTTP_CODE process_read();   //解析HTTP请求
    bool process_write(HTTP_CODE ret);  //根据HTTP_CODE填充响应

    //这一组函数被process_read调用来分析HTTP请求
    HTTP_CODE parse_request_line(char* text);     //解析请求首行
    HTTP_CODE parse_header(char* text);     //解析请求头
    HTTP_CODE parse_content(char* text);  //解析请求体
    LINE_STATUS parse_line();   //解析每一行
    
    HTTP_CODE do_request(); //生成响应报文
    //m_start_line是已经解析的字符
    //这个函数的作用是将指针向后偏移，指向未处理的字符
    char* get_line() { return m_read_buff + m_start_line; }

    //这一组函数被process_write调用以填充HTTP应答
    void unmap();
    bool add_response(const char* format, ...);
    bool add_content(const char* content);
    bool add_content_type();
    bool add_status_line(int status, const char* title);
    bool add_headers(int content_length);
    bool add_content_length(int content_length);
    bool add_linger();
    bool add_blank_line();

public:
    static int m_epollfd;       //让所有socket事件注册到这里
    static int m_user_num;      //用户数量
    static const int READ_BUFF_SIZE = 2048; //读缓冲区大小
    static const int WRITE_BUFF_SIZE = 2048;//写缓冲区大小
    static const int FILENAME_LEN = 200;    // 文件名的最大长度

private:
    int m_sockfd;           //该HTTP的连接socket
    sockaddr_in m_addr;     //客户端的socket信息

    char m_read_buff[READ_BUFF_SIZE]; //读缓冲区
    int m_read_idx;         //标识读缓冲区中已经读入的客户端数据的最后一个字节的下一个位置
    int m_checked_idx;      //当前正在分析的字符在读缓冲区中的位置
    int m_start_line;       //已经解析的字符个数

    char m_write_buff[WRITE_BUFF_SIZE]; //写缓冲区
    int m_write_idx;            //写缓冲区中待发送的字节数

    CHECK_STATE m_check_state;  //主状态机当前所处状态
    METHOD m_method;    //请求方法

    
    
    char* doc_root;         //根目录
    

    char m_real_file[FILENAME_LEN]; //请求的目标文件的完整路径
    char* m_url;            //目标URL
    char* m_version;        //HTTP版本
    char* m_host;           //主机名
    bool m_connection;      //是否保持连接, Connection字段
    int m_content_length;   //消息体长度

    char* m_file_address;       //客户请求的目标文件被mmap到内存中的起始位置
    struct stat m_file_stat;    //目标文件的状态
    struct iovec m_iv[2];       //我们将采用writev来执行写操作，所以定义下面两个成员
    int m_iv_count;             //其中m_iv_count表示被写内存块的数量

    int bytes_have_send;    //已发送的字节数
    int bytes_to_send;      //剩余发送的字节数


};


#endif