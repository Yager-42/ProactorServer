#ifndef _EPOLL_CLASS_
#define _EPOLL_CLASS_

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/epoll.h>
#include <signal.h>
#include <sys/epoll.h>
#include "http_conn.h"
#include "locker.h"
#include "threadPool.h"
#include "socket_control.h"

class epoll_class : public base
{
public:
    epoll_class(int port);
    ~epoll_class();
    void run();

private:
    static const int MAX_FD = 65535;
    static const int MAX_EVENT_NUMBER = 10000;
    //线程池
    threadPool<http_conn> *pool;
    //创建一个数组用于保存所有的客户端信息
    http_conn *users;
    //监听的套接字
    int listenfd;
    // 创建epoll对象和事件数组
    epoll_event events[MAX_EVENT_NUMBER];
    int epollfd;

    //添加信号捕捉
    void addsig(int sig, void(handler)(int));
    // 非阻塞的读,循环读取
    bool Read(http_conn &conn);
    // 非阻塞的读
    bool Write(http_conn &conn);
};

void epoll_class::addsig(int sig, void(handler)(int))
{
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = handler;
    sigfillset(&sa.sa_mask);
    sigaction(sig, &sa, NULL);
}

bool epoll_class::Read(http_conn &conn)
{
    int m_sockfd = conn.get_sockfd();
    int m_read_index = conn.get_read_index();
    char *m_read_buf = conn.get_read_buf();
    if (m_read_index >= READ_BUFFER_SIZE)
    {
        return 0;
    }
    while (1)
    {
        int bytes_read = recv(m_sockfd, m_read_buf + m_read_index, READ_BUFFER_SIZE - m_read_index, 0);
        if (bytes_read == -1)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                //阻塞了没执行完就等待
                break;
            }
            else
            {
                //报错退出
                return 0;
            }
        }
        else if (bytes_read == 0)
        {
            return 0; //什么都没读到报错
        }
        m_read_index += bytes_read;
    }
    conn.set_read_index(m_read_index);
    return 1;
}

bool epoll_class::Write(http_conn &conn)
{
    const int m_sockfd = conn.get_sockfd();
    const int bytes_to_send = conn.get_bytes_to_send();
    iovec *m_iv = conn.get_iv();
    const int m_iv_count = conn.get_iv_count();
    if (bytes_to_send == 0)
    {
        // 将要发送的字节为0
        modfd(epollfd, m_sockfd, EPOLLIN);
        conn.clear();
        return true;
    }
    while (1)
    {
        int ret = writev(m_sockfd, m_iv, m_iv_count);
        if (ret == -1)
        {
            if (errno == EAGAIN)
            {
                //重试
                modfd(epollfd, m_sockfd, EPOLLOUT);
                return true;
            }
            return false;
        }

        if (ret >= m_iv[0].iov_len)
        {
            //写完了头
            //更新现在发送的内容情况
            m_iv[1].iov_base = (char *)m_iv[1].iov_base + (ret - m_iv[0].iov_len);
            m_iv[1].iov_len -= (ret - m_iv[0].iov_len);
            m_iv[0].iov_len = 0;
        }
        else
        {
            //头没写完
            //更新情况
            m_iv[0].iov_base = (char *)(m_iv[0].iov_base) + ret;
            m_iv[0].iov_len -= ret;
        }
        if (m_iv[1].iov_len <= 0)
        {
            //发送完毕
            modfd(epollfd, m_sockfd, EPOLLIN);
            if (conn.is_keepalive())
            {
                conn.clear();
                return 1;
            }
            else
            {
                return 0;
            }
        }
    }
}

epoll_class::~epoll_class()
{
    // 结束后close
    close(epollfd);
    close(listenfd);
    delete[] users;
    delete pool;
}

epoll_class::epoll_class(int port)
{
    // 对SIGPIPE信号处理
    // 避免因为SIGPIPE退出
    addsig(SIGPIPE, SIG_IGN);

    //创建线程池
    pool = NULL;
    try
    {
        pool = new threadPool<http_conn>;
    }
    catch (const char *msg)
    {
        printf("error:%s\n", msg);
        exit(-1);
    }

    //创建保存客户端信息的数组
    users = new http_conn[MAX_FD];
    printf("创建保存客户端信息的数组\n");

    //创建监听套接字
    listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenfd == -1)
    {
        perror("socket");
        exit(-1);
    }
    printf("创建监听套接字\n");

    //端口
    int reuse = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    printf("端口复用设置\n");

    //绑定
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    int ret = bind(listenfd, (struct sockaddr*)&addr, sizeof(addr));
    if (ret == -1)
    {
        perror("bind");
        exit(-1);
    }
    printf("绑定地址\n");
    // 监听
    ret = listen(listenfd, 5);
    if (ret == -1)
    {
        perror("listen");
        exit(-1);
    }
    printf("监听地址\n");
    //创建epoll
    epollfd = epoll_create(100);

    //将监听的套接字fd添加到epoll
    addfd(epollfd, listenfd, false);
    http_conn::st_m_usercount = 0;
    http_conn::st_m_epollfd = epollfd;
}

void epoll_class::run()
{
    //进行监听
    while (1)
    {
        printf("开始等待\n");
        int num = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);
        if (num < 0 && errno != EINTR)
        {
            printf("epoll fail\n");
            break;
        }
        printf("等待到信息\n");

        for (int i = 0; i < num; ++i)
        {
            int sockfd = events[i].data.fd;

            if (events[i].data.fd == listenfd)
            {
                //监听到客户端连接
                sockaddr_in client_addr;
                socklen_t client_addrlen = sizeof(client_addr);
                int connfd = accept(listenfd, (sockaddr *)&client_addr, &client_addrlen);

                if (connfd < 0)
                {
                    printf("errno is: %d\n", errno);
                    continue;
                }

                if (http_conn::st_m_usercount >= MAX_FD)
                {
                    close(connfd);
                    continue;
                }
                users[connfd].init(connfd, client_addr);
            }
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))
            {
                //报错处理
                users[sockfd].close_conn();
            }
            else if (events[i].events & EPOLLIN)
            {
                //读事件
                if (Read(users[sockfd]))
                {
                    // 读完了
                    pool->append(users + sockfd);
                }
                else
                {
                    // 读失败了
                    users[sockfd].close_conn();
                }
            }
            else if (events[i].events & EPOLLOUT)
            {
                //写事件
                if (Write(users[sockfd]))
                {
                    // 写成功
                    ;
                }
                else
                {
                    // 写失败了 / 写完不保持连接
                    users[sockfd].close_conn();
                }
            }
        }
    }
}
#endif