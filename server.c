#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/select.h>                 //for select()
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>

#define IPADDR      "127.0.0.1"         //回路Ip
#define PORT        6565
#define MAXLINE     1024                //最大描述符数目
#define LISTENQ     5                   //服务端监听队列长度
#define SIZE        10

typedef struct server_context_st
{
    int cli_cnt;        /*客户端个数*/
    int clifds[SIZE];   /*客户端的套接字描述符存储数组*/
    fd_set allfds;      /*句柄集合*/
    int maxfd;          /*句柄最大值*/
} server_context_st;

static server_context_st *s_srv_ctx = NULL;               //静态全局


static int create_server_proc(const char* ip,int port)
{
    int  fd;
    struct sockaddr_in servaddr;
    /* 建立socket套接字 */
    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1)
    {
        fprintf(stderr, "create socket fail,erron:%d,reason:%s\n", errno, strerror(errno));
        return -1;
    }

    /* closesocket(一般不会立即关闭而经历TIME_WAIT的过程)后想继续重用该socket */
    int reuse = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) == -1)
    {
        return -1;
    }

    /* 设置服务端地址 */
    bzero(&servaddr,sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    inet_pton(AF_INET,ip,&servaddr.sin_addr);
    servaddr.sin_port = htons(port);                        //主机字节序转换为网络字节序

    /* 绑定和监听 */
    if (bind(fd,(struct sockaddr*)&servaddr,sizeof(servaddr)) == -1)
    {
        perror("bind error: ");
        return -1;
    }

    listen(fd,LISTENQ);

    return fd;
}

static int accept_client_proc(int srvfd)
{
    struct sockaddr_in cliaddr;
    socklen_t cliaddrlen;
    cliaddrlen = sizeof(cliaddr);
    int clifd = -1;

    printf("accpet client proc is called.\n");

    ACCEPT:
    clifd = accept(srvfd,(struct sockaddr*)&cliaddr,&cliaddrlen);

    if (clifd == -1)
    {
        if (errno == EINTR)                                 //信号中断，EINTR interrupted system call
        {
            goto ACCEPT;                                    //继续阻塞接收
        }
        else
        {
            fprintf(stderr, "accept fail,error:%s\n", strerror(errno));
            return -1;
        }
    }

    fprintf(stdout, "accept a new client: %s:%d\n", inet_ntoa(cliaddr.sin_addr),cliaddr.sin_port);

    //将新的连接描述符添加到数组中
    int i = 0;
    for (i = 0; i < SIZE; i++)
    {
        if (s_srv_ctx->clifds[i] < 0)                       //server_init()已经初始化为-1
        {
            s_srv_ctx->clifds[i] = clifd;
            s_srv_ctx->cli_cnt++;
            break;
        }
    }

    if (i == SIZE)
    {
        fprintf(stderr,"too many clients.\n");
        return -1;
    }
//101 }
}

static int handle_client_msg(int fd, char *buf)
{
    assert(buf);                        //有内容，继续执行
    printf("recv buf is : %s\n", buf);
    write(fd, buf, strlen(buf) + 1);    //如果buf还有内容，继续写入
    return 0;
}

static void recv_client_msg(fd_set *readfds)
{
    int i = 0, n = 0;
    int clifd;
    char buf[MAXLINE] = {0};
    for (i = 0; i <= s_srv_ctx->cli_cnt; i++)               //遍历客户端套接字
    {
        clifd = s_srv_ctx->clifds[i];
        if (clifd < 0)
        {
            continue;
        }
        /*判断客户端套接字是否有数据*/
        if (FD_ISSET(clifd, readfds))                       //检查集合中指定描述符是否可读
        {
            //接收客户端发送的信息
            n = read(clifd, buf, MAXLINE);
            if (n <= 0)
            {
                /* n==0表示读取完成，客户端关闭套接字 */
                FD_CLR(clifd, &s_srv_ctx->allfds);          //将给定文件描述符从集合中删除
                close(clifd);
                s_srv_ctx->clifds[i] = -1;
                continue;
            }
            handle_client_msg(clifd, buf);                  //往描述符所指文件写内容
        }
    }
}
static void handle_client_proc(int srvfd)
{
    int  clifd = -1;
    int  retval = 0;
    fd_set *readfds = &s_srv_ctx->allfds;
    struct timeval tv;
    int i = 0;

    while (1)
    {
        /*每次调用select前都要重新设置文件描述符和时间，因为事件发生后，文件描述符和时间都被内核修改了*/
        FD_ZERO(readfds);           //初始化
        /*添加监听套接字*/
        FD_SET(srvfd, readfds);
        s_srv_ctx->maxfd = srvfd;

        tv.tv_sec = 30;             //超时设置：30s
        tv.tv_usec = 0;
        /*添加客户端套接字*/
        for (i = 0; i < s_srv_ctx->cli_cnt; i++)
        {
            clifd = s_srv_ctx->clifds[i];
            /*去除无效的客户端句柄*/
            if (clifd != -1)
            {
                FD_SET(clifd, readfds);
            }
            s_srv_ctx->maxfd = (clifd > s_srv_ctx->maxfd ? clifd : s_srv_ctx->maxfd);
        }

        /*开始轮询接收处理服务端和客户端套接字*/
        retval = select(s_srv_ctx->maxfd + 1, readfds, NULL, NULL, &tv);
        if (retval == -1)
        {
            fprintf(stderr, "select error:%s.\n", strerror(errno));
            return;
        }
        if (retval == 0)
        {
            fprintf(stdout, "select is timeout.\n");
            continue;
        }
        if (FD_ISSET(srvfd, readfds))
        {
            /*监听客户端请求*/
            accept_client_proc(srvfd);
        }
        else
        {
            /*接受处理客户端消息*/
            recv_client_msg(readfds);           //???
        }
    }
}

static void server_uninit()
{
    if (s_srv_ctx)
    {
        free(s_srv_ctx);
        s_srv_ctx = NULL;
    }
}

static int server_init()
{
    s_srv_ctx = (server_context_st *)malloc(sizeof(server_context_st));
    if (s_srv_ctx == NULL)
    {
        return -1;
    }

    memset(s_srv_ctx, 0, sizeof(server_context_st));

    int i = 0;
    for (;i < SIZE; i++)
    {
        s_srv_ctx->clifds[i] = -1;
    }

    return 0;
}

int main(int argc,char *argv[])
{
    int  srvfd;
    /* 初始化服务端context */
    if (server_init() < 0)
    {
        return -1;
    }
    /*创建服务,开始监听客户端请求 */
    srvfd = create_server_proc(IPADDR, PORT);               //任意IP，指定端口
    if (srvfd < 0)
    {
        fprintf(stderr, "socket create or bind fail.\n");
        goto err;
    }
    /*开始接收并处理客户端请求 */
    handle_client_proc(srvfd);
    server_uninit();
    return 0;
    err:
    server_uninit();
    return -1;
}