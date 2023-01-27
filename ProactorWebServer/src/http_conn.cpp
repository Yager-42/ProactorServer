#include "http_conn.h"

int http_conn::st_m_epollfd = -1;
int http_conn::st_m_usercount = 0;

const char *root_directory = "resources";

// 初始化新接收的连接
void http_conn::init(int sockfd, const sockaddr_in &addr)
{
    m_sockfd = sockfd;
    m_address = addr;

    //设置端口复用
    int reuse = 1;
    setsockopt(m_sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    //添加描述符到epoll中
    addfd(st_m_epollfd, m_sockfd, 1);
    ++st_m_usercount;

    clear();
}

// 初始化连接
void http_conn::clear()
{
    unmap();

    m_check_state = CHECK_STATE_REQUESTLINE;
    m_checked_index = 0;
    m_start_line = 0;
    m_read_index = 0;
    m_write_index = 0;
    bytes_to_send = 0;

    m_iv_count = 0;
    m_iv[0].iov_len = m_iv[1].iov_len = 0;
    m_url = NULL;
    m_method = GET;
    m_version = 0;
    // HTTP 1.1 中默认启用Keep-Alive，如果加入"Connection: close "，才关闭
    m_keepalive = false;

    m_host = NULL;
    m_content_length = 0;
    m_address_mmap = NULL;

    bzero(m_read_buf, READ_BUFFER_SIZE);
    bzero(m_write_buf, WRITE_BUFFER_SIZE);
    bzero(&m_address, sizeof(m_address));
    bzero(m_iv, sizeof(m_iv));
    bzero(m_filename, FILENAME_MAXLEN);
}

//关闭连接
void http_conn::close_conn()
{
    if (m_sockfd != -1)
    {
        removefd(st_m_epollfd, m_sockfd);
        m_sockfd = -1;
        clear();
        --st_m_usercount;
    }
}

void http_conn::unmap()
{
    //如果还没映射
    if (m_address_mmap)
    {
        int ret = munmap(m_address_mmap, m_file_stat.st_size);
        if (ret == -1)
        {
            perror("mmap");
        }
        m_address_mmap = NULL;
    }
}

//处理业务逻辑
void http_conn::process()
{
    //解析http请求
    HTTP_CODE read_ret = process_read();
#ifdef process_read_result
    printf("process_read result : %d\n", read_ret);
#endif
    if (read_ret == NO_REQUEST)
    {
        //没读完，继续读
        modfd(st_m_epollfd, m_sockfd, EPOLLIN);
        return;
    }
    bool write_ret = process_write(read_ret);
    if (!write_ret)
    {
        close_conn();
    }

    modfd(st_m_epollfd, m_sockfd, EPOLLOUT);
}

//主状态机
//解析http请求
http_conn::HTTP_CODE http_conn::process_read()
{
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char *text = NULL;
    while ((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK) || ((line_status == parse_line()) == LINE_OK))
    {
        // 解析到了一行完整的数据  或者解析到了请求体,也是完整的数据
        // 获取一行数据
        text = get_line();
#ifdef patse_message
        printf("\n即将解析的数据: %s\n", text);
#endif
        m_start_line = m_checked_index;
        switch (m_check_state)
        {
        case CHECK_STATE_REQUESTLINE:
        {
            ret = parse_request_line(text);
            if (ret == BAD_REQUEST)
            {
                return BAD_REQUEST;
            }
            break;
        }
        case CHECK_STATE_HEADER:
        {
            ret = parse_headers(text);
            if (ret = BAD_REQUEST)
            {
                return BAD_REQUEST;
            }
            else if (ret = GET_REQUEST)
            {
                return do_request();
            }
            break;
        }
        case CHECK_STATE_CONTENT:
        {
            ret = parse_content_complete(text);
            if (ret = BAD_REQUEST)
            {
                return BAD_REQUEST;
            }
            else if (ret = GET_REQUEST)
            {
                return do_request();
            }
            line_status = LINE_OPEN;
            break;
        }
        default:
        {
            return INTERNAL_ERROR;
        }
        }
    }
    return NO_REQUEST;
}

// 解析HTTP请求行,获取请求方法 ,目标URL,HTTP版本
http_conn::HTTP_CODE http_conn::parse_request_line(char *text)
{
    // 1.解析请求行
    // GET /index.html HTTP/1.1
    // 1.1 请求方法 URL 协议版本
    // 初始化以及填入\0进行分隔
    char *index = text;
    char *method = text;
    index = strpbrk(index, "\t");
    if (!index)
        return BAD_REQUEST;
    *(index++) = '\0'; //空格变结束符,即让method可以在原来的空格处自动截断
    m_url = index;
    index = strpbrk(index, "\t");
    if (!index)
        return BAD_REQUEST;
    *(index++) = '\0';
    m_version = index;
    if (strcasecmp(method, "GET") == 0)
    {
        m_method = GET;
    }
    else
    {
        return BAD_REQUEST;
    }
    //http://192.168.110.129:10000/index.html
    if (strncasecmp(m_url, "http://", 7) == 0)
    {
        m_url += 7;                 //跳过http://
        m_url = strchr(m_url, '/'); //跳到第一个/
    }
    if (!m_url || m_url[0] != '/')
    {
        return BAD_REQUEST;
    }
    if (strcasecmp(m_version, "HTTP/1.1") != 0)
    {
        return BAD_REQUEST;
    }

    m_check_state = CHECK_STATE_HEADER;

#ifdef patse_message
    printf("请求头解析成功\n    url:%s,version:%s,method:%s\n", m_url, m_version, method);
#endif
    return NO_REQUEST;
}

//解析请求头
http_conn::HTTP_CODE http_conn::parse_headers(char *text)
{
#ifdef patse_message
    printf("分析请求头 : %s\n", text);
#endif
    /**
     * "Connection:"
     * "Content-Length:"
     * "Host:"
     */
    if (text[0] == '\0')
    {
        //没读完继续读，读完就润
        if (m_content_length != 0)
        {
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        return GET_REQUEST;
    }
    else if (strncasecmp(text, "Connection:", 11) == 0)
    {
        text += 11;
        //strcspn返回 str1 中第一个不在字符串 str2 中出现的字符下标。
        text += strcspn(text, " \t"); // 去除开头的空格和\t

        if (strcasecmp(text, "keep-alive") == 0)
        {
            m_keepalive = 1;
        }
        else if (strcasecmp(text, "close") == 0)
        {
            m_keepalive = 0;
        }
    }
    else if (strncasecmp(text, "Content-Length:", 15) == 0)
    {
        text += 15;
        text += strcspn(text, " \t");
        m_content_length = atol(text);
    }
    else if (strncasecmp(text, "Host:", 5) == 0)
    {
        text += 5;
        text += strcspn(text, " \t");
        m_host = text;
    }
    else
    {
#ifdef patse_message
        printf("解析失败,不知名的请求头: %s\n", text);
#endif
    }
    return NO_REQUEST;
}

//解析http请求内容
http_conn::HTTP_CODE http_conn::parse_content_complete(char *text)
{
    if (m_read_index >= (m_checked_index + m_content_length))
    {
        text[m_content_length] = '\0';
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

//分析完后的具体操作，拿去要求文件
http_conn::HTTP_CODE http_conn::do_request()
{
    int sumlen = strlen(m_url) + strlen(root_directory) + 1;
    snprintf(m_filename, std::min((int)(FILENAME_MAXLEN), sumlen), "%s%s", root_directory, m_url);
    printf("m_filename:%s\n", m_filename);
    // m_filename = "resources" + "/xxxxxxx"

    int ret = stat(m_filename, &m_file_stat);
    if (ret == -1)
    {
        perror("stat");
        return NO_RESOURCE;
    }

    // 判断访问权限
    if (!(m_file_stat.st_mode & S_IROTH))
    {
        return FORBIDDEN_REQUEST;
    }

    // 判断是否是目录
    if (S_ISDIR(m_file_stat.st_mode))
    {
        return BAD_REQUEST;
    }

    int fd = open(m_filename, O_RDONLY);
    //创建内存映射
    m_address_mmap = (char *)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
//MAP_PRIVATE ：不同步，内存映射区的数据改变了，对原来的文件不会修改，会重新创建一个新的文件。
#ifdef mmap_print
    printf("\nmmap :==================\n %s\n\n", m_address_mmap);
#endif

    close(fd);
    return FILE_REQUEST;
}

//从状态机
http_conn::LINE_STATUS http_conn::parse_line()
{
    char temp;
    for (; m_checked_index < m_read_index; ++m_checked_index)
    {
        temp = m_read_buf[m_checked_index];
        if (temp == '\r')
        {
            if ((m_checked_index + 1) == m_read_index)
            {
                return LINE_OPEN;
            }
            else if (m_read_buf[m_checked_index + 1] == '\n')
            {
                // 完整的一句,将 \r\n 变为 \0
                m_read_buf[m_checked_index++] = '\0';
                m_read_buf[m_checked_index++] = '\0';
                // 相当于 x x+1 = \0  then x+=2
                return LINE_OK;
            }
            return LINE_BAD;
        }
        else if (temp == '\n')
        {
            if ((m_checked_index > 1 && m_read_buf[m_checked_index - 1] == '\r'))
            { // 这次的第一个和上一次的最后一个是一个分隔
                m_read_buf[m_checked_index - 1] = '\0';
                m_read_buf[m_checked_index++] = '\0';
                return LINE_OK;
            }
        }
    }
    return LINE_OPEN;
}

//写入部分
//往写缓冲区中写数据
bool http_conn::add_response(const char *format, ...)
{
    if (m_write_index >= WRITE_BUFFER_SIZE)
    {
        return false;
    }
    va_list args;
    va_start(args, format);
    int len = vsnprintf(m_write_buf + m_write_index, WRITE_BUFFER_SIZE - m_write_index - 1, format, args);
    //该函数用于向一个字符串缓存区格式化打印字符串，且可以限定打印字符串的最大长度。
    // vsnprintf 用法类似snprintf,输入的最大长度为__maxlen-1
    // 调用args和format进行可变参数输入
    // 返回值为若空间足够则输入的长度
    if (len > WRITE_BUFFER_SIZE - m_write_index - 1)
    {
        // 说明输入的字符溢出
        return false;
    }
    m_write_index += len;
    va_end(args);
    return 1;
}
//添加状态行
bool http_conn::add_status_line(int status, const char *title)
{
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}

//添加响应头部
bool http_conn::add_headers(int content_len, time_t time)
{
    if (!add_content_length(content_len))
        return false;
    if (!add_content_type())
        return false;
    if (!add_connection())
        return false;
    if (!add_date(time))
        return false;
    if (!add_blank_line())
        return false;
    return true;
}

//响应头组件

//长度
bool http_conn::add_content_length(int content_len)
{
    return add_response("Content-Length: %d\r\n", content_len);
}

//类型
bool http_conn::add_content_type()
{
    // 虑区分是图片 / html/css
    char *format_file = strrchr(m_filename, '.');
    return add_response("Content-Type: %s\r\n", format_file == NULL ? "text/html" : (format_file + 1));
}

//始终保持连接
bool http_conn::add_connection()
{
    return add_response("Connection: %s\r\n", (m_keepalive == true) ? "keep-alive" : "close");
}

//时间
bool http_conn::add_date(time_t t)
{
    char timebuf[50];
    strftime(timebuf, 80, "%Y-%m-%d %H:%M:%S", localtime(&t));
    return add_response("Date: %s\r\n", timebuf);
}

//结束行
bool http_conn::add_blank_line()
{
    return add_response("\r\n");
}

bool http_conn::add_content(const char *content)
{
    return add_response("%s", content);
}

//生成返回报文
bool http_conn::process_write(HTTP_CODE ret)
{
    /*
        NO_REQUEST : 请求不完整，需要继续读取客户数据
        GET_REQUEST : 表示获得了一个完成的客户请求
        BAD_REQUEST : 表示客户请求语法错误
        NO_RESOURCE : 表示服务器没有资源
        FORBIDDEN_REQUEST : 表示客户对资源没有足够的访问权限
        FILE_REQUEST : 文件请求,获取文件成功
        INTERNAL_ERROR : 表示服务器内部错误
        CLOSED_CONNECTION : 表示客户端已经关闭连接了
    */
    int status = std::get<0>(response_info[ret]);
    const char *title = std::get<1>(response_info[ret]);
    const char *form = std::get<2>(response_info[ret]);
    if (ret == FILE_REQUEST)
    {
        if (!add_status_line(status, title))
        {
            return false;
        }
        if (!add_headers(m_file_stat.st_size, time(NULL)))
        {
            return false;
        }
#ifdef check_write_header
        if (check_write_header)
        {
            printf("OK 的 报文头:\n");
            write(STDOUT_FILENO, m_write_buf, m_write_index);
        }
#endif
        m_iv[0].iov_base = m_write_buf;
        m_iv[0].iov_len = m_write_index;
        m_iv[1].iov_base = m_address_mmap;
        m_iv[1].iov_len = m_file_stat.st_size;
        m_iv_count = 2;
        // 维护发送长度
        bytes_to_send = m_write_index + m_file_stat.st_size;

        return true;
    }
    else if (response_info.find(ret) != response_info.end())
    {
        //不请求文件，报个错
        if (!add_status_line(status, title))
            return false;
        if (!add_headers(strlen(form), time(NULL)))
            return false; // 发送本地时间
        m_iv[0].iov_base = m_write_buf;
        m_iv[0].iov_len = m_write_index;
        m_iv[1].iov_base = (char *)form;
        m_iv[1].iov_len = strlen(form) + 1;
        m_iv_count = 2;
        // 维护发送长度
        bytes_to_send = m_iv[0].iov_len + m_iv[1].iov_len;
        return 1;
    }
    else
    {
        return 0;
    }
}