#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <string.h>
#include <getopt.h>
#include <time.h>
#include <sys/epoll.h>
#include <sys/time.h>
#include <errno.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <execinfo.h>
#include <dirent.h>

#include <fcntl.h>
#include <stdint.h>
#include <netinet/tcp.h>
#include <sys/types.h>


#define MAX_EVENTS      10



#define BACKLOG		10

/*设置套接字保持连接*/
int setsockkeepalived(int sock)
{
	int keepAlive = 1;
	int keepIdle = 3;		//***开始首次探测前的TCP空闭时间	/* The time (in seconds) the connection needs to remain idle before TCP starts sending keepalive probes. */
	int keepInterval = 3;	//***两次keepalive时,探测时间间隔	/* The time (in seconds) between individual keepalive probes. */
	int keepCount = 2;		//***判断断开前的探测次数			/* The maximum number of keepalive probes TCP should send before dropping the connection. */

	if (setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, (void *)&keepAlive, sizeof(keepAlive)) == -1) {			//设置套接口保持活动
		return -errno;
	}
	if (setsockopt(sock, SOL_TCP, TCP_KEEPIDLE, (void *)&keepIdle, sizeof(keepIdle)) == -1) {
		return -errno;
	}
	if (setsockopt(sock, SOL_TCP, TCP_KEEPINTVL, (void *)&keepInterval, sizeof(keepInterval)) == -1) {
		return -errno;
	}
	if (setsockopt(sock, SOL_TCP, TCP_KEEPCNT, (void *)&keepCount, sizeof(keepCount)) == -1) {
		return -errno;
	}
	return 0;
}

/*设置写socket超时时间*/
int setsockwtimeo(int sock, int timeo)
{
	struct timeval timeout = { 3, 0 };

	timeout.tv_sec = timeo;
	return setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
}

/*设置读socket超时时间*/
int setsockrtimeo(int sock, int timeo)
{
	struct timeval timeout = { 3, 0 };

	timeout.tv_sec = timeo;
	return setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
}

/***************************************************************************
* Function: tcp_state
* Description: 检测描述符连接状态
* Input:  
* 		fd:套接字描述符
* Return:成功0，失败-1
***************************************************************************/
int tcp_state(int tcp_fd)
{
	struct tcp_info info;
	int optlen = sizeof(struct tcp_info);

	if (getsockopt(tcp_fd, IPPROTO_TCP, TCP_INFO, &info, (socklen_t *) & optlen) < 0)
	{
		return -errno;
	}
	
	return (info.tcpi_state == TCP_ESTABLISHED) ? 0 : -1;
}

/***************************************************************************
* Function: set_fd_unblock
* Description: 设置非阻塞模式
* Input:  
* 		fd:套接字描述符
* Return:成功0，失败-1
***************************************************************************/
int set_fd_unblock(int fd)
{
	if (fcntl(fd, F_SETFL, O_NONBLOCK) != 0)			//* 非阻塞模式 */
	{
		return -errno;
	}
	return 0;
}


int connect_to(int fd, uint32_t hip, uint16_t port)
{
	struct sockaddr_in server;
	int n = -1, ret = -1, err = 0;
	socklen_t len = -1;
	fd_set wset;
	struct timeval tv;
	int flag = fcntl(fd, F_GETFL, 0);

	memset(&server, 0, sizeof(server));
	server.sin_family = AF_INET;
	server.sin_port = htons(port);
	server.sin_addr.s_addr = htonl(hip);
	fcntl(fd, F_SETFL, flag | O_NONBLOCK);
	n = connect(fd, (struct sockaddr *)&server, sizeof(server));
	if (0 == n) {
		goto success;
	} else {
		if (errno != EINPROGRESS) {
			goto done;
		}
	}
	FD_ZERO(&wset);
	FD_SET(fd, &wset);
	tv.tv_sec = 6;
	tv.tv_usec = 0;
	n = select(fd + 1, NULL, &wset, NULL, &tv);
	if (n <= 0) {
		goto done;
	}
	if (FD_ISSET(fd, &wset)) {
		len = sizeof(err);
		n = getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
		if (n < 0 || err) {
			goto done;
		}
	} else {
		goto done;
	}
success:
	ret = 0;
done:
	fcntl(fd, F_SETFL, flag);
	return ret;
}


int listen_socket(int fd)
{
	if (listen(fd, BACKLOG) < 0) {
		return -errno;
	}
	return 0;
}

int bind_socket(int fd, int port)
{
	int on = 1;

	struct sockaddr_in addr;

	bzero(&addr, sizeof(struct sockaddr_in));
	addr.sin_family = AF_INET;
	addr.sin_port = htons((uint16_t) port);
	addr.sin_addr.s_addr = htonl(INADDR_ANY);

	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));		//打开或关闭地址复用功能
	if (bind(fd, (struct sockaddr *)&addr, sizeof(struct sockaddr)) < 0) {
		return -errno;
	}
	return 0;
}

int create_tcp_socket(void)
{
	int sock = -1;

	if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
		return -errno;
	}
	return sock;
}


/*epoll是linux系统最新的处理多连接的高效率模型， 工作在两种方式下， EPOLLLT方式和EPOLLET方式。

    EPOLLLT是系统默认， 工作在这种方式下， 程序员不易出问题， 在接收数据时，只要socket输入缓存有数据，都能够获得EPOLLIN的持续通知， 同样在发送数据时， 只要发送缓存够用， 都会有持续不间断的EPOLLOUT通知。

而对于EPOLLET是另外一种触发方式， 比EPOLLLT要高效很多， 对程序员的要求也多些， 程序员必须小心使用，因为工作在此种方式下时， 在接收数据时， 如果有数据只会通知一次， 假如read时未读完数据，那么不会再有EPOLLIN的通知了， 直到下次有新的数据到达时为止； 当发送数据时， 如果发送缓存未满也只有一次EPOLLOUT的通知， 除非你把发送缓存塞满了， 才会有第二次EPOLLOUT通知的机会， 所以在此方式下read和write时都要处理好。 暂时写到这里， 留作备忘。 
附加： 如果将一个socket描述符添加到两个epoll中， 那么即使在EPOLLET模式下， 只要前一个epoll_wait时，未读完， 那么后一个epoll_wait事件时， 也会得到读的通知， 但前一个读完的情况下， 后一个epoll就不会得到读事件的通知了。。。。。*/
/*
EPOLLLT 是默认行为，基本上就是说，只要一个文件描述符处于就绪状态，epoll 就会不停的通知你有事件发生。传统的 select/poll 都是这样的

EPOLLET 是新的方式，只在一个文件描述符新处于就绪的时候通知一次，之后不管数据有没有读完，都不会再通知，当然，有新数据到还是会通知的。所以，用 EPOLLET 的时候，一定要把文件描述符设置为 non-blocking，而且最好是一直读数据，读到返回 EAGAIN 才停下

*/
int my_epoll(int bindport)
{
	int listen_fd = -1;
	int nfds = -1, epfd = -1, i = 0;
	socklen_t sock_size;
	int connectfd;
	struct sockaddr_in client;
	struct epoll_event ev, events[MAX_EVENTS];
    int ret = 0;
    
    char readbuf[1024] = {0};
    int readlen = 1024;
    /*创建socket*/
	if ((listen_fd = create_tcp_socket()) < 0) {
		printf("%s create tcp socket err\n", __FUNCTION__);
		return -1;
	}
    /*创建事件循环*/
	if ((epfd = epoll_create(MAX_EVENTS)) < 0) {
		close(listen_fd);
		printf("%s epoll_create err\n", __FUNCTION__);
		return -1;
	}
    /*绑定socket*/
	bind_socket(listen_fd, bindport);
    /*监听socket*/
	listen_socket(listen_fd);
    /*将socket设置为非阻塞模式*/
	set_fd_unblock(listen_fd);
    /*设置与要处理的事件相关的文件描述符*/
	ev.data.fd = listen_fd;
    /*设置要处理的事件类型*/
	ev.events = EPOLLIN | EPOLLET;
    /*注册epoll事件*/
	epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev);
	printf("socket bind port on %u\n", bindport);
	while (1) {
		nfds = epoll_wait(epfd, events, sizeof(events) / sizeof(events[0]), 3);
		if (nfds < 1) {
 //           printf("no connect\n");
			continue;
		}
		for (i = 0; i < nfds; i++) {				//遍历所有事件
			int evfd = events[i].data.fd;
			if (evfd < 0) {
				continue;
			}
			if (evfd == listen_fd) {
                /*主socket连接，表示有新的连接进入*/             
				sock_size = sizeof(struct sockaddr_in);
				connectfd = accept(listen_fd, (struct sockaddr *)&client, (socklen_t *) & sock_size);  //accept这个连接

				if (connectfd < 0) {
					continue;
				}

				printf("accept new connect %d from %s:%d ok\n", connectfd, inet_ntoa(client.sin_addr), client.sin_port);
				/*将新连接设置为非阻塞模式*/
                set_fd_unblock(connectfd);						
                /*保持连接*/
				setsockkeepalived(connectfd);                   
                /*将新的连接添加到epoll监听队列*/
				ev.data.fd = connectfd;
				ev.events = EPOLLIN | EPOLLET;					
				epoll_ctl(epfd, EPOLL_CTL_ADD, connectfd, &ev);
			} else if (events[i].events & EPOLLIN) {	
                /*已连接的用户*/
                /*读取数据*/
                memset(readbuf, 0, sizeof(readbuf));
                ret = read(evfd, readbuf, readlen);
				if (ret < 0) {  
					if (tcp_state(evfd) != 0) {
                        /*连接出错，从队列中移除改连接*/
						epoll_ctl(epfd, EPOLL_CTL_DEL, evfd, NULL);
						printf("connect exit0 :%d\n", evfd);
						shutdown(evfd, 2);
						close(evfd);
					} else {
						printf("read respond err %d\n", -ret);
					}
				} else {
                    /*读取成功*/
                    printf("read len:%d\n",ret);
                    printf("readdata:%s\n", readbuf);
				}
			} 
            else if(events[i].events&EPOLLOUT)
            {
                /*有数据待发送，写socket*/
                /*取数据*/
                char *sendbuf="Hello, i am server";
                /*发送数据*/
                send(events[i].data.fd, sendbuf, strlen(sendbuf), 0);
                ev.data.fd = events[i].data.fd;
                ev.events = EPOLLIN|EPOLLET;
                /*修改标识符，等待下一个循环时接收数据*/
                epoll_ctl(epfd,EPOLL_CTL_MOD,ev.data.fd,&ev); 
            }
            else {
				if (tcp_state(evfd) != 0) {
					epoll_ctl(epfd, EPOLL_CTL_DEL, evfd, NULL);
					printf("connect exit1 :%d\n", evfd);
					shutdown(evfd, 2);
					close(evfd);
				}
			}
		}
	}
}


int main(void)
{
    int port = 58891;
    my_epoll(port);
    return  0;
}