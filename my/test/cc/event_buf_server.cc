#include<stdio.h>
#include<string.h>
#include<errno.h>

#include<event.h>
#include<event2/bufferevent.h>



void accept_cb(int fd, short events, void* arg);
void socket_read_cb(bufferevent* bev, void* arg);
void event_cb(struct bufferevent *bev, short event, void *arg);
int tcp_server_init(int port, int listen_num);

// 对于一个服务器而言，流程大致如下：
    // 获取待监听的内容的fd；
    // 创建一个event_base；
    // 创建一个event,指定待监听的fd，待监听事件的类型，以及事件放生时的回调函数及传给回调函数的参数；
    // 将event添加到event_base的事件管理器中；
    // 开启event_base的事件处理循环；
    // （异步）当事件发生的时候，调用前面设置的回调函数。

// 使用BufferEvent
    // 在上面的代码中，client的cmd中有信息输入时，client直接将数据写入到fd中，server中收到信息后，也是直接将信息写入到fd中，因为fd是非阻塞的，所以不能保证正确。那么需要一个自己管理的缓存来管理自己的数据。那么步骤将稍微有些变化，如下所示：

    // 设置scokfd为nonblocking；
    // 使用bufferevent_socket_new创建一个struct bufferevent* bev，关联上面的sockfd，并托管给event_base;
    // 使用bufferevent_setcb(bev, read_cb, write_cb, error_cb, (void*)arg)；
    // 使用buffevent_enable(bev, EV_READ|EV_WRITE|EV_PERSIST)来启动read/write事件
int main(int argc, char** argv)
{

    int listener = tcp_server_init(9999, 10);
    if( listener == -1 )
    {
        perror(" tcp_server_init error ");
        return -1;
    }

    // libevent默认情况下是单线程的，可以配置成多线程，每个线程有且只有一个event_base，对应一个struct event_base结构体以及附于其上的事件管理器，用来调度托管给它的一系列event，可以和操作系统的进程管理类比。当一个事件发生后，event_base会在合适的时间，不一定是立即去调用绑定在这个事件上的函数，直到这个函数执行完，再去调度其他的事件。
    struct event_base* base = event_base_new();

    // event_base内部有一个循环，循环阻塞在epoll等系统调用上，直到有一个/一些时间发生，然后去处理这些事件。当然，这些事件要被绑定在这个event_base上，每个事件对应一个struct event，可以是监听一个fd或者信号量之类的，struct event使用event_new来创建和绑定，使用event_add来将event绑定到event_base中
    //添加监听客户端请求连接事件
    //参数：event_base,监听的对象，需要监听的事件，事件发生后的回调函数，传给回调函数的参数
        //     注：libevent支持的事件及属性包括（使用bitfield实现）
        // EV_TIMEOUT:超时；
        // EV_READ:只要网络缓冲中还有数据，回调函数就会被触发；
        // EV_WRITE:只要塞给网络缓冲的数据被写完，回调函数就会被触发；
        // EV_SIGNAL:POSIX信号量；
        // EV_PERSIST:不指定这个属性，回调函数被触发后事件会被删除；
        // EV_ET:Edge-Trigger边缘触发（这个还不懂是什么意思）
    struct event* ev_listen = event_new(base, listener, EV_READ | EV_PERSIST,
                                        accept_cb, base);
    //参数：event，超时时间，NULL表示无超时设置
    event_add(ev_listen, NULL);

    // 然后启动event_base的循环，开始处理事件。循环地启动使用event_base_dispatch，循环将一直持续，找到不再有需要关注的事件，或者是遇到event_loopbreak()/event_loopexit()函数。
    event_base_dispatch(base);
    event_base_free(base);


    return 0;
}



void accept_cb(int fd, short events, void* arg)
{
    evutil_socket_t sockfd;

    struct sockaddr_in client;
    socklen_t len = sizeof(client);

    sockfd = ::accept(fd, (struct sockaddr*)&client, &len );
    evutil_make_socket_nonblocking(sockfd);

    printf("accept a client %d\n", sockfd);

    struct event_base* base = (event_base*)arg;

    bufferevent* bev = bufferevent_socket_new(base, sockfd, BEV_OPT_CLOSE_ON_FREE);
    bufferevent_setcb(bev, socket_read_cb, NULL, event_cb, arg);

    bufferevent_enable(bev, EV_READ | EV_PERSIST);
}



void socket_read_cb(bufferevent* bev, void* arg)
{
    char msg[4096];

    size_t len = bufferevent_read(bev, msg, sizeof(msg));

    msg[len] = '\0';
    printf("recv the client msg: %s", msg);


    char reply_msg[4096] = "I have recvieced the msg: ";

    strcat(reply_msg + strlen(reply_msg), msg);
    bufferevent_write(bev, reply_msg, strlen(reply_msg));
}



void event_cb(struct bufferevent *bev, short event, void *arg)
{

    if (event & BEV_EVENT_EOF)
        printf("connection closed\n");
    else if (event & BEV_EVENT_ERROR)
        printf("some other error\n");

    //这将自动close套接字和free读写缓冲区
    bufferevent_free(bev);
}


typedef struct sockaddr SA;
int tcp_server_init(int port, int listen_num)
{
    int errno_save;
    evutil_socket_t listener;

    // SOCK_STREAM提供面向连接的稳定数据传输，即TCP协议。SOCK_STREAM应用在C语言socket编程中，在进行网络连接前，需要用socket函数向系统申请一个通信端口。
    // 选择 AF_INET 的目的就是使用 IPv4 进行通信。因为 IPv4 使用 32 位地址，相比 IPv6 的 128 位来说，计算更快，便于用于局域网通信。
    // 而且 AF_INET 相比 AF_UNIX 更具通用性，因为 Windows 上有 AF_INET 而没有 AF_UNIX。
    // 注：AF_INET（又称 PF_INET）是 IPv4 网络协议的套接字类型，AF_INET6 则是 IPv6 的；而 AF_UNIX 则是 Unix 系统本地通信。
    listener = ::socket(AF_INET, SOCK_STREAM, 0);
    if( listener == -1 )
        return -1;

    //允许多次绑定同一个地址。要用在socket和bind之间
    evutil_make_listen_socket_reuseable(listener);

    struct sockaddr_in sin;
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = 0;
    sin.sin_port = htons(port);

    if( ::bind(listener, (SA*)&sin, sizeof(sin)) < 0 )
        goto error;

    if( ::listen(listener, listen_num) < 0)
        goto error;


    //跨平台统一接口，将套接字设置为非阻塞状态
    evutil_make_socket_nonblocking(listener);

    return listener;

    error:
        errno_save = errno;
        evutil_closesocket(listener);
        errno = errno_save;

        return -1;
}