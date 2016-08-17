#include <iostream>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>

using namespace std;

#define MAXLINE 5
#define OPEN_MAX 100
#define LISTENQ 20
#define SERV_PORT 5000
#define INFTIM 1000


void setnonblocking(int sock)
{
    int opts;
    opts=fcntl(sock,F_GETFL);
    if(opts<0)
    {
        perror("fcntl(sock,GETFL)");
        return;
    }
    opts = opts|O_NONBLOCK;
    if(fcntl(sock,F_SETFL,opts)<0)
    {
        perror("fcntl(sock,SETFL,opts)");
        return;
    }
}

void CloseAndDisable(int sockid, epoll_event ee)
{
    close(sockid);
    ee.data.fd = -1;
}

int main()
{
    int i, maxi, listenfd, connfd, sockfd,epfd,nfds, portnumber;
    char line[MAXLINE];
    socklen_t clilen;

    portnumber = 5000;

    //����epoll_event�ṹ��ı���,ev����ע���¼�,�������ڻش�Ҫ������¼�

    struct epoll_event ev,events[20];
    //�������ڴ���accept��epollר�õ��ļ�������

    epfd=epoll_create(256);
    struct sockaddr_in clientaddr;
    struct sockaddr_in serveraddr;
    listenfd = socket(AF_INET, SOCK_STREAM, 0);

    memset(&serveraddr, 0, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;
    serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);
    serveraddr.sin_port=htons(portnumber);

    // bind and listen
    bind(listenfd,(sockaddr *)&serveraddr, sizeof(serveraddr));
    listen(listenfd, LISTENQ);

    //������Ҫ������¼���ص��ļ�������
    ev.data.fd=listenfd;
    //����Ҫ������¼�����
    ev.events=EPOLLIN|EPOLLET;
    //ev.events=EPOLLIN;

    //ע��epoll�¼�
    epoll_ctl(epfd,EPOLL_CTL_ADD,listenfd,&ev);

    maxi = 0;

    int bOut = 0;
    for ( ; ; )
    {
        if (bOut == 1)
            break;
        //�ȴ�epoll�¼��ķ���

        nfds=epoll_wait(epfd,events,20,-1);
        //�����������������¼�
        cout << "\nepoll_wait returns\n";

        for(i=0;i<nfds;++i)
        {
            if(events[i].data.fd==listenfd)//����¼�⵽һ��SOCKET�û����ӵ��˰󶨵�SOCKET�˿ڣ������µ����ӡ�
            {
                connfd = accept(listenfd,(sockaddr *)&clientaddr, &clilen);
                if(connfd<0){
                    perror("connfd<0");
                    return (1);
                }
                

                char *str = inet_ntoa(clientaddr.sin_addr);
                cout << "accapt a connection from " << str << endl;
                //�������ڶ��������ļ�������

                setnonblocking(connfd);
                ev.data.fd=connfd;
                //��������ע��Ķ������¼�

                ev.events=EPOLLIN | EPOLLET;
                //ev.events=EPOLLIN;

                //ע��ev
                epoll_ctl(epfd,EPOLL_CTL_ADD,connfd,&ev);
            }
            else if(events[i].events & EPOLLIN)//������Ѿ����ӵ��û��������յ����ݣ���ô���ж��롣
            {
                cout << "EPOLLIN" << endl;
                if ( (sockfd = events[i].data.fd) < 0)
                    continue;

                char * head = line;
                int recvNum = 0;
                int count = 0;
                bool bReadOk = false;
                while(1)
                {
                    // ȷ��sockfd��nonblocking��
                    recvNum = recv(sockfd, head + count, MAXLINE, 0);
                    if(recvNum < 0)
                    {
                        if(errno == EAGAIN)
                        {
                            // �����Ƿ�������ģʽ,���Ե�errnoΪEAGAINʱ,��ʾ��ǰ�������������ݿɶ�
                            // ������͵����Ǹô��¼��Ѵ���.
                            bReadOk = true;
                            break;
                        }
                        else if (errno == ECONNRESET)
                        {
                                // �Է�������RST
                                CloseAndDisable(sockfd, events[i]);
                                cout << "counterpart send out RST\n";
                                break;
                         }
                        else if (errno == EINTR)
                        {
                            // ���ź��ж�
                            continue;
                        }
                        else
                        {
                            //���������ֲ��Ĵ���
                            CloseAndDisable(sockfd, events[i]);
                            cout << "unrecovable error\n";
                            break;
                        }
                   }
                   else if( recvNum == 0)
                   {
                        // �����ʾ�Զ˵�socket�������ر�.���͹�FIN�ˡ�
                        CloseAndDisable(sockfd, events[i]);
                        cout << "counterpart has shut off\n";
                        break;
                   }

                   // recvNum > 0
                    count += recvNum;
                   if ( recvNum == MAXLINE)
                   {
                       continue;   // ��Ҫ�ٴζ�ȡ
                   }
                   else // 0 < recvNum < MAXLINE
                   {
                       // ��ȫ����
                       bReadOk = true;
                       break; // �˳�while(1),��ʾ�Ѿ�ȫ����������
                   }
                }

                if (bReadOk == true)
                {
                    // ��ȫ����������
                    line[count] = '\0';

                    cout << "we have read from the client : " << line;
                    //��������д�������ļ�������

                    ev.data.fd=sockfd;
                    //��������ע���д�����¼�

                    ev.events = EPOLLOUT | EPOLLET;
                    //�޸�sockfd��Ҫ������¼�ΪEPOLLOUT

                    epoll_ctl(epfd,EPOLL_CTL_MOD,sockfd,&ev);
                }
            }
            else if(events[i].events & EPOLLOUT) // ��������ݷ���
            {
                const char str[] = "hello from epoll : this is a long string which may be cut by the net\n";
                memcpy(line, str, sizeof(str));
                cout << "Write " << line << endl;
                sockfd = events[i].data.fd;

                bool bWritten = false;
                int writenLen = 0;
                int count = 0;
                char * head = line;
                while(1)
                {
                        // ȷ��sockfd�Ƿ�������
                        writenLen = send(sockfd, head + count, MAXLINE, 0);
                        if (writenLen == -1)
                        {
                            if (errno == EAGAIN)
                            {
                                // ����nonblocking ��socket���ԣ�����˵�����Ѿ�ȫ�����ͳɹ���
                                bWritten = true;
                                break;
                            }
                            else if(errno == ECONNRESET)
                            {
                                // �Զ�����,�Է�������RST
                                CloseAndDisable(sockfd, events[i]);
                                cout << "counterpart send out RST\n";
                                break;
                            }
                            else if (errno == EINTR)
                            {
                                // ���ź��ж�
                                continue;
                            }
                            else
                            {
                                // ��������
                            }
                        }

                        if (writenLen == 0)
                        {
                            // �����ʾ�Զ˵�socket�������ر�.
                            CloseAndDisable(sockfd, events[i]);
                            cout << "counterpart has shut off\n";
                            break;
                        }

                        // ���µ������writenLen > 0
                        count += writenLen;
                        if (writenLen == MAXLINE)
                        {
                            // ���ܻ�û��д��
                            continue;
                        }
                        else // 0 < writenLen < MAXLINE
                        {
                            // �Ѿ�д����
                            bWritten = true;
                            break; // �˳�while(1)
                        }
                }

                if (bWritten == true)
                {
                    //�������ڶ��������ļ�������
                    ev.data.fd=sockfd;

                    //��������ע��Ķ������¼�
                    ev.events=EPOLLIN | EPOLLET;

                    epoll_ctl(epfd,EPOLL_CTL_MOD,sockfd,&ev);
                }
            }
        }
    }
    return 0;
}

/*
ע�����¼��㣺

1. #14�趨Ϊ5�ǹ���ģ�Ϊ�˲��Ժ�������������

2. �����������Ĺ������ȶ�ȡ�ַ�����Ȼ����Է�д���ݡ�

3. #110������ͨ��socketΪ��������

4. ע��#130~#183�Ķ��ɾ���������read��

5. ע��#213~#264����ȫд������Ҫ�������ݵ�write��

6. ����EPOLLET��epoll_waitֻ����socket״̬�����仯��ʱ��Ż᷵�ء�����Ҫ��fd����ѭ��accept,read, write;ֱ֪��socket�Ļ������գ�read, accept)��������(write)Ϊֹ��
*/