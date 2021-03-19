#include "http_conn.h"


// 定义HTTP响应的一些状态信息
const char* ok_200_title = "OK";
const char* error_400_title = "Bad Request";
const char* error_400_form = "Your request has bad syntax or is inherently impossible to satisfy.\n";
const char* error_403_title = "Forbidden";
const char* error_403_form = "You do not have permission to get file from this server.\n";
const char* error_404_title = "Not Found";
const char* error_404_form = "The requested file was not found on this server.\n";
const char* error_500_title = "Internal Error";
const char* error_500_form = "There was an unusual problem serving the requested file.\n";

//设置文件描述符非阻塞
int setnonblock(int fd)
{
    int old_flag = fcntl(fd, F_GETFL);
    old_flag |= O_NONBLOCK;
    fcntl(fd, F_SETFL, old_flag);
    return old_flag;
}

//向epoll事件中添加文件描述符
void addfd(int epfd, int fd, bool one_shot)
{
    epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN | EPOLLRDHUP | EPOLLET;

    if(one_shot)
        event.events |= EPOLLONESHOT;

    epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &event);
    setnonblock(fd);
}

//从epoll事件中删除文件描述符
void removefd(int epfd, int fd)
{
    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

//重置EPOLLONESHOT事件，ev代表事件
//确保下一次可读时，EPOLLIN能被触发
void modfd(int epfd, int fd, int ev)
{
    epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLET | EPOLLONESHOT | EPOLLRDHUP | ev;

    epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &event);
}

int http_conn::m_epollfd = -1;
int http_conn::m_user_num = 0;

void http_conn::close_conn()
{
    if(m_sockfd != -1){
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_num--;
    }
}


void http_conn::init(int clifd, const sockaddr_in& cliaddr, char* root)
{
    m_sockfd = clifd;
    m_addr = cliaddr;

    doc_root = root;
    //设置端口复用
    int reuse = 1;
    setsockopt(m_sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    //将该fd添加到epoll中，对其进行监听
    addfd(m_epollfd, m_sockfd, true);
    m_user_num++;
    
    init();
}

bool http_conn::read()
{
    if(m_read_idx >= READ_BUFF_SIZE)
        return false;
    
    int len = 0;
    while(true){
        len = recv(m_sockfd, m_read_buff + m_read_idx, READ_BUFF_SIZE - m_read_idx, 0);
        if(len == 0){
            //对方关闭连接
            return false;
        }
        else if(len == -1){
            if(errno == EAGAIN || errno == EWOULDBLOCK){
                //没有数据
                break;
            }
            return false;
        }
        m_read_idx += len;
    }
    return true;
}

void http_conn::init()
{
    m_check_state = CHECK_STATE_REQUESTLINE; //解析请求首行
    m_checked_idx = 0;
    m_start_line = 0;
    m_read_idx = 0;

    m_method = GET;
    m_url = 0;        
    m_version = 0;    
    m_host = 0;
    m_connection = false;      
    m_content_length = 0;
    m_write_idx = 0;            
    bytes_have_send = 0;
    bytes_to_send = 0;

    bzero(m_read_buff, READ_BUFF_SIZE);
    bzero(m_write_buff, WRITE_BUFF_SIZE);
    bzero(m_real_file, FILENAME_LEN);
}

//由线程池中的工作线程调用，处理HTTP请求的入口函数
void http_conn::process()
{
    //process_read函数开始处理HTTP请求
    HTTP_CODE read_ret = process_read();
    if(read_ret == NO_REQUEST){ //没有请求，继续监听可读事件
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        return;
    }

    //生成响应
    bool write_ret = process_write(read_ret);
    if(!write_ret)
        close_conn();
    //因为使用了EPOLLONESHOT，每次操作后都要修改监听事件
    modfd(m_epollfd, m_sockfd, EPOLLOUT);

}

http_conn::HTTP_CODE http_conn::process_read()
{
    //从状态机初始状态
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char *text = 0;

    //
    while((m_check_state == CHECK_STATE_CONTENT) && (line_status == LINE_OK) 
        || ((line_status = parse_line()) == LINE_OK)){
        //获取一行数据
        text = get_line();
        m_start_line = m_checked_idx;
        printf("正在解析: %s\n", text);
        //主状态机状态转换
        switch(m_check_state){
            case CHECK_STATE_REQUESTLINE:
            {
                ret = parse_request_line(text);
                if(ret == BAD_REQUEST){
                    return BAD_REQUEST;
                }
                break;
            }
            case CHECK_STATE_HEADER:
            {
                ret = parse_header(text);
                if(ret == BAD_REQUEST){
                    return BAD_REQUEST;
                }
                else if(ret == GET_REQUEST){
                    return do_request();
                }
                break;
            }
            case CHECK_STATE_CONTENT:
            {
                ret = parse_content(text);
                if(ret == GET_REQUEST){
                    return do_request();
                }
                line_status = LINE_OPEN;
                break;
            }
            default: return INTERNAL_ERROR;
            
        }
        
    }
    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::do_request()
{
    strcpy(m_real_file, doc_root);
    int len = strlen(doc_root);
    strncpy(m_real_file+len, m_url, FILENAME_LEN-len-1);

    //获取m_real_file文件的相关的属性信息，-1失败，0成功
    if(stat(m_real_file, &m_file_stat) < 0)
        return NO_RESOURCE;

    //判断访问权限
    if(!(m_file_stat.st_mode & S_IROTH))
        return FORBIDDEN_REQUEST;
    
    //判断是否是目录
    if(S_ISDIR(m_file_stat.st_mode))
        return BAD_REQUEST;
    
    //以只读方式打开文件
    int fd = open(m_real_file, O_RDONLY);
    //创建内存映射
    m_file_address = (char*)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    return FILE_REQUEST;
}

void http_conn::unmap()
{
    if(m_file_address){
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}

http_conn::HTTP_CODE http_conn::parse_request_line(char* text)
{
    //GET /index.html HTTP/1.1
    //判断第二个参数中的字符哪个在text中最先出现
    m_url = strpbrk(text, " \t");
    if(!m_url)
        return BAD_REQUEST;
    
    //置位空字符，字符串结束符，即: GET\0/index.html HTTP/1.1
    *m_url++ = '\0';
    char* method = text;
    //忽略大小写比较，先只支持GET方法
    if(strcasecmp(method, "GET") == 0)
        m_method = GET;
    else
        return BAD_REQUEST;

    m_version = strpbrk(m_url, " \t");
    if(!m_version)
        return BAD_REQUEST;
    *m_version++ = '\0';
    if((strcasecmp(m_version, "HTTP/1.1") != 0) && (strcasecmp(m_version, "HTTP/1.0") != 0))
        return BAD_REQUEST;

    //  http://IP:端口号/index.html
    if(strncasecmp(m_url, "http://", 7) == 0){
        m_url += 7;
        m_url = strchr(m_url, '/');
    }
    if(!m_url || m_url[0] != '/')
        return BAD_REQUEST;
    
    //请求首行解析完，检测状态变成解析头部
    m_check_state = CHECK_STATE_HEADER;

    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::parse_header(char* text)
{
    //空行，表示头部字段解析完毕
    if(text[0] == '\0'){
        //如果有消息体，获取其长度，主状态变为 CHECK_STATE_CONTENT
        if(m_content_length != 0){
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        return GET_REQUEST;
    }
    else if(strncasecmp(text, "Connection:", 11) == 0){
        //处理Connection头部字段
        text += 11;
        text += strspn(text, " \t");
        if(strcasecmp(text, "keep-alive") == 0)
            m_connection = true;
    }
    else if(strncasecmp(text, "Content-Length:", 15) == 0){
        // 处理Content-Length头部字段
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);
    }
    else if(strncasecmp(text, "Host:", 5) == 0){
        // 处理Host头部字段
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    }
    else{
        printf("!!! unknow header: %s\n", text);
    }
    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::parse_content(char* text)
{
    if(m_read_idx >= (m_content_length + m_checked_idx)){
        text[m_content_length] = '\0';
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

http_conn::LINE_STATUS http_conn::parse_line()
{
    char tmp;
    for(; m_checked_idx < m_read_idx; m_checked_idx++){
        tmp = m_read_buff[m_checked_idx];
        if(tmp == '\r'){
            if(m_checked_idx + 1 == m_read_idx)
                return LINE_OPEN;
            else if(m_read_buff[m_checked_idx+1] == '\n'){
                m_read_buff[m_checked_idx++] = '\0';
                m_read_buff[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
        else if(tmp == '\n'){
            if(m_checked_idx > 1 && m_read_buff[m_checked_idx-1] == '\r'){
                m_read_buff[m_checked_idx-1] = '\0';
                m_read_buff[m_read_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN;
}

bool http_conn::process_write(HTTP_CODE ret)
{
    switch(ret)
    {
        case FILE_REQUEST:
            add_status_line(200, ok_200_title);
            add_headers(m_file_stat.st_size);
            m_iv[0].iov_base = m_write_buff;
            m_iv[0].iov_len = m_write_idx;
            m_iv[1].iov_base = m_file_address;
            m_iv[1].iov_len = m_file_stat.st_size;
            m_iv_count = 2;
            //总的要发送的数据
            bytes_to_send = m_file_stat.st_size + m_write_idx; 
            return true;
        case INTERNAL_ERROR:
            add_status_line(500, error_500_title);
            add_headers(strlen(error_500_form));
            if(!add_content(error_500_form))
                return false;
            break;
        case BAD_REQUEST:
            add_status_line(400, error_400_title);
            add_headers(strlen(error_400_form));
            if(!add_content(error_400_form))
                return false;
            break;
        case NO_RESOURCE:
            add_status_line(404, error_404_title);
            add_headers(strlen(error_404_form));
            if(!add_content(error_404_form))
                return false;
            break;
        case FORBIDDEN_REQUEST:
            add_status_line(403, error_403_title);
            add_headers(strlen(error_403_form));
            if (!add_content(error_403_form ))
                return false;
            break;
        default:
            return false;
    }
    m_iv[0].iov_base = m_write_buff;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    return true;
}

bool http_conn::write()
{
    int tmp = 0;

    if(bytes_to_send == 0){
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        init();
        return true;
    }
    while(1){
        tmp = writev(m_sockfd, m_iv, m_iv_count);
        if(tmp < 0){
            // 如果TCP写缓冲没有空间，则等待下一轮EPOLLOUT事件，虽然在此期间，
            // 服务器无法立即接收到同一客户的下一个请求，但可以保证连接的完整性。
            if(errno == EAGAIN){
                modfd(m_epollfd, m_sockfd, EPOLLOUT);
                return true;
            }
            unmap();
            return false;
        }
        bytes_to_send -= tmp;
        bytes_have_send += tmp;
        
        if(bytes_have_send >= m_iv[0].iov_len){
            //响应头已发送完毕
            m_iv[0].iov_len = 0;
            m_iv[1].iov_base = m_file_address + (bytes_have_send-m_write_idx);
            m_iv[1].iov_len = bytes_to_send;
        }
        else{
            //响应头未发送完毕，修改下次写数据的位置
            m_iv[0].iov_base = m_write_buff + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - bytes_have_send;
        }

        //数据是否全部发送出去
        if(bytes_to_send <= 0){
            unmap();
            modfd(m_epollfd, m_sockfd, EPOLLIN);
            if(m_connection){
                init();
                return true;
            }
            else{
                return false;
            }
        }
    }
    //return true;
}

bool http_conn::add_response(const char* format, ...){
    if(m_write_idx >= WRITE_BUFF_SIZE)
        return false;
    //VA_LIST: 解决变参问题的一组宏，用于获取不确定个数的参数。
    va_list arg_list;
    va_start(arg_list, format);
    int len = vsnprintf(m_write_buff+m_write_idx, WRITE_BUFF_SIZE-1-m_write_idx, format, arg_list);
    if(len >= (WRITE_BUFF_SIZE-1-m_write_idx))
        return false;

    m_write_idx += len;
    va_end(arg_list);
    return true;

}

bool http_conn::add_status_line(int status, const char* title){
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}

bool http_conn::add_headers(int content_len){
    add_content_length(content_len);
    add_content_type();
    add_linger();
    add_blank_line();
}

bool http_conn::add_content_length(int content_len){
    return add_response("Content-Length: %d\r\n", content_len);
}

bool http_conn::add_content_type(){
    return add_response("Content-Type:%s\r\n", "text/html");
}

bool http_conn::add_linger(){
    return add_response("Connection: %s\r\n", 
        (m_connection == true) ? "keep-alive" : "close" );
}

bool http_conn::add_blank_line(){
    return add_response( "%s", "\r\n" );
}

bool http_conn::add_content(const char* content){
    return add_response("%s", content);
}








