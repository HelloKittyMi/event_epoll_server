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

/*�����׽��ֱ�������*/
int setsockkeepalived(int sock)
{
	int keepAlive = 1;
	int keepIdle = 3;		//***��ʼ�״�̽��ǰ��TCP�ձ�ʱ��	/* The time (in seconds) the connection needs to remain idle before TCP starts sending keepalive probes. */
	int keepInterval = 3;	//***����keepaliveʱ,̽��ʱ����	/* The time (in seconds) between individual keepalive probes. */
	int keepCount = 2;		//***�ж϶Ͽ�ǰ��̽�����			/* The maximum number of keepalive probes TCP should send before dropping the connection. */

	if (setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, (void *)&keepAlive, sizeof(keepAlive)) == -1) {			//�����׽ӿڱ��ֻ
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

/*����дsocket��ʱʱ��*/
int setsockwtimeo(int sock, int timeo)
{
	struct timeval timeout = { 3, 0 };

	timeout.tv_sec = timeo;
	return setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
}

/*���ö�socket��ʱʱ��*/
int setsockrtimeo(int sock, int timeo)
{
	struct timeval timeout = { 3, 0 };

	timeout.tv_sec = timeo;
	return setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
}

/***************************************************************************
* Function: tcp_state
* Description: �������������״̬
* Input:  
* 		fd:�׽���������
* Return:�ɹ�0��ʧ��-1
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
* Description: ���÷�����ģʽ
* Input:  
* 		fd:�׽���������
* Return:�ɹ�0��ʧ��-1
***************************************************************************/
int set_fd_unblock(int fd)
{
	if (fcntl(fd, F_SETFL, O_NONBLOCK) != 0)			//* ������ģʽ */
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

	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));		//�򿪻�رյ�ַ���ù���
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


/*epoll��linuxϵͳ���µĴ�������ӵĸ�Ч��ģ�ͣ� ���������ַ�ʽ�£� EPOLLLT��ʽ��EPOLLET��ʽ��

    EPOLLLT��ϵͳĬ�ϣ� ���������ַ�ʽ�£� ����Ա���׳����⣬ �ڽ�������ʱ��ֻҪsocket���뻺�������ݣ����ܹ����EPOLLIN�ĳ���֪ͨ�� ͬ���ڷ�������ʱ�� ֻҪ���ͻ��湻�ã� �����г�������ϵ�EPOLLOUT֪ͨ��

������EPOLLET������һ�ִ�����ʽ�� ��EPOLLLTҪ��Ч�ܶ࣬ �Գ���Ա��Ҫ��Ҳ��Щ�� ����Ա����С��ʹ�ã���Ϊ�����ڴ��ַ�ʽ��ʱ�� �ڽ�������ʱ�� ���������ֻ��֪ͨһ�Σ� ����readʱδ�������ݣ���ô��������EPOLLIN��֪ͨ�ˣ� ֱ���´����µ����ݵ���ʱΪֹ�� ����������ʱ�� ������ͻ���δ��Ҳֻ��һ��EPOLLOUT��֪ͨ�� ������ѷ��ͻ��������ˣ� �Ż��еڶ���EPOLLOUT֪ͨ�Ļ��ᣬ �����ڴ˷�ʽ��read��writeʱ��Ҫ����á� ��ʱд����� ���������� 
���ӣ� �����һ��socket��������ӵ�����epoll�У� ��ô��ʹ��EPOLLETģʽ�£� ֻҪǰһ��epoll_waitʱ��δ���꣬ ��ô��һ��epoll_wait�¼�ʱ�� Ҳ��õ�����֪ͨ�� ��ǰһ�����������£� ��һ��epoll�Ͳ���õ����¼���֪ͨ�ˡ���������*/
/*
EPOLLLT ��Ĭ����Ϊ�������Ͼ���˵��ֻҪһ���ļ����������ھ���״̬��epoll �ͻ᲻ͣ��֪ͨ�����¼���������ͳ�� select/poll ����������

EPOLLET ���µķ�ʽ��ֻ��һ���ļ��������´��ھ�����ʱ��֪ͨһ�Σ�֮�󲻹�������û�ж��꣬��������֪ͨ����Ȼ���������ݵ����ǻ�֪ͨ�ġ����ԣ��� EPOLLET ��ʱ��һ��Ҫ���ļ�����������Ϊ non-blocking�����������һֱ�����ݣ��������� EAGAIN ��ͣ��

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
    /*����socket*/
	if ((listen_fd = create_tcp_socket()) < 0) {
		printf("%s create tcp socket err\n", __FUNCTION__);
		return -1;
	}
    /*�����¼�ѭ��*/
	if ((epfd = epoll_create(MAX_EVENTS)) < 0) {
		close(listen_fd);
		printf("%s epoll_create err\n", __FUNCTION__);
		return -1;
	}
    /*��socket*/
	bind_socket(listen_fd, bindport);
    /*����socket*/
	listen_socket(listen_fd);
    /*��socket����Ϊ������ģʽ*/
	set_fd_unblock(listen_fd);
    /*������Ҫ������¼���ص��ļ�������*/
	ev.data.fd = listen_fd;
    /*����Ҫ������¼�����*/
	ev.events = EPOLLIN | EPOLLET;
    /*ע��epoll�¼�*/
	epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev);
	printf("socket bind port on %u\n", bindport);
	while (1) {
		nfds = epoll_wait(epfd, events, sizeof(events) / sizeof(events[0]), 3);
		if (nfds < 1) {
 //           printf("no connect\n");
			continue;
		}
		for (i = 0; i < nfds; i++) {				//���������¼�
			int evfd = events[i].data.fd;
			if (evfd < 0) {
				continue;
			}
			if (evfd == listen_fd) {
                /*��socket���ӣ���ʾ���µ����ӽ���*/             
				sock_size = sizeof(struct sockaddr_in);
				connectfd = accept(listen_fd, (struct sockaddr *)&client, (socklen_t *) & sock_size);  //accept�������

				if (connectfd < 0) {
					continue;
				}

				printf("accept new connect %d from %s:%d ok\n", connectfd, inet_ntoa(client.sin_addr), client.sin_port);
				/*������������Ϊ������ģʽ*/
                set_fd_unblock(connectfd);						
                /*��������*/
				setsockkeepalived(connectfd);                   
                /*���µ�������ӵ�epoll��������*/
				ev.data.fd = connectfd;
				ev.events = EPOLLIN | EPOLLET;					
				epoll_ctl(epfd, EPOLL_CTL_ADD, connectfd, &ev);
			} else if (events[i].events & EPOLLIN) {	
                /*�����ӵ��û�*/
                /*��ȡ����*/
                memset(readbuf, 0, sizeof(readbuf));
                ret = read(evfd, readbuf, readlen);
				if (ret < 0) {  
					if (tcp_state(evfd) != 0) {
                        /*���ӳ����Ӷ������Ƴ�������*/
						epoll_ctl(epfd, EPOLL_CTL_DEL, evfd, NULL);
						printf("connect exit0 :%d\n", evfd);
						shutdown(evfd, 2);
						close(evfd);
					} else {
						printf("read respond err %d\n", -ret);
					}
				} else {
                    /*��ȡ�ɹ�*/
                    printf("read len:%d\n",ret);
                    printf("readdata:%s\n", readbuf);
				}
			} 
            else if(events[i].events&EPOLLOUT)
            {
                /*�����ݴ����ͣ�дsocket*/
                /*ȡ����*/
                char *sendbuf="Hello, i am server";
                /*��������*/
                send(events[i].data.fd, sendbuf, strlen(sendbuf), 0);
                ev.data.fd = events[i].data.fd;
                ev.events = EPOLLIN|EPOLLET;
                /*�޸ı�ʶ�����ȴ���һ��ѭ��ʱ��������*/
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