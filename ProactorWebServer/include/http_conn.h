#ifndef _HTTP_CONN_H_
#define _HTTP_CONN_H_

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <sys/epoll.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>
#include <time.h>
#include <algorithm>
#include <unordered_map>
#include <tuple>
#include "static_value.h"
#include "socket_control.h"

using std::tuple;
using std::unordered_map;

class http_conn : public base
{
public:
    static int st_m_epollfd;   //epoll文件标识符
    static int st_m_usercount; //用户数量

public:
    http_conn(){}
    void process(); //处理请求
    void init(int sockfd, const sockaddr_in &addr);
    void close_conn();
    bool Write();

    //与epoll交互
    void clear();

    int get_sockfd()
    {
        return m_sockfd; //获取socket文件描述符
    }
    int get_read_index()
    {
        return m_read_index;
    }
    char *get_read_buf()
    {
        return m_read_buf;
    }
    char *get_write_buf()
    {
        return m_write_buf;
    }
    int get_bytes_to_send()
    {
        return bytes_to_send;
    }
    iovec *get_iv()
    {
        return m_iv;
    }
    int get_iv_count()
    {
        return m_iv_count;
    }
    char *get_address_mmap()
    {
        return m_address_mmap;
    }
    bool is_keepalive()
    {
        return m_keepalive;
    }
    void set_read_index(int index)
    {
        m_read_index = index;
    }

private:
    //socket通信部份
    int m_sockfd;          //http连接的socket描述符
    sockaddr_in m_address; //socket地址

    //读缓冲区部分
    char m_read_buf[READ_BUFFER_SIZE]; //读缓冲区
    int m_read_index;                  //标识读缓冲区和读入的数据的最后一个字节的下标
    int m_checked_index;               //当前正在分析的字符在缓冲区的位置
    int m_start_line;                  //当前正在解析的行的起始位置
    char *get_line()
    {
        return m_read_buf + m_start_line; // 返回当前行的起始位置对应的指针
    }

    //写缓冲器部分
    char m_write_buf[WRITE_BUFFER_SIZE]; //写缓冲区
    int m_write_index;                   //写缓冲区待发送的字节数

    //writev
    struct iovec m_iv[2]; // 存储分散写的内容,0为报文头,1为报文内容
    int m_iv_count;       //writev数量
    //放送情况
    int bytes_to_send; //将要发送的数据的字节数

    //报文解析情况
    //请求行分析结果
    char *m_url;     //请求文件的地址
    char *m_version; //协议版本号
    METHOD m_method; //请求方法
    //请求头分析结果
    char *m_host;         //主机名
    int m_content_length; // 描述HTTP消息实体的传输长度
    bool m_keepalive;     // HTTP请求是否要保持连接

    //返回数据
    char m_filename[FILENAME_MAX]; // 客户请求的目标文件的完整目录,其内容为 root_directory+m_url
    struct stat m_file_stat;       //目标文件状态
    char* m_address_mmap;          // 客户请求的数据被mmap到的位置
    void unmap();                  // 释放内存映射的空间

    //状态机
    CHECK_STATE m_check_state; //主状态机所处的位置
    //主状态机
    HTTP_CODE process_read();                     //解析HTTP请求
    HTTP_CODE parse_request_line(char *text);     //解析HTTP首行
    HTTP_CODE parse_headers(char *text);          //解析HTTP请求头
    HTTP_CODE parse_content_complete(char *text); //解析HTTP请求内容
    HTTP_CODE do_request();                       //处理报文
    //次要状态机
    LINE_STATUS parse_line();

    //写响应报文
    bool process_write(HTTP_CODE ret);                   //写响应报文
    bool add_response(const char *format, ...);          // 往写缓冲区中写数据
    bool add_status_line(int status, const char *title); // 添加状态行
    bool add_headers(int content_len, time_t time);      // 添加响应头部
    // 响应头部组件
    bool add_content_length(int content_len); // content-length
    bool add_connection();                    // keep_alive
    bool add_content_type();                  // Content-Type
    bool add_date(time_t t);                  // 发送时间
    bool add_blank_line();                    // 空白结束行
    bool add_content(const char *content);
};


#endif