#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <sys/socket.h>
#include <sys/wait.h>
#include <arpa/inet.h>

#define BUFFER_SIZE 4096

void echo_worker(void* arg)
{
    int clientfd = *(int*)arg;
 
	int recvbytes = 0;
	char* recvbuf = new char[BUFFER_SIZE];
	memset(recvbuf, 0, BUFFER_SIZE);
 
	while(1)
	{
		if ((recvbytes=recv(clientfd, recvbuf, BUFFER_SIZE,0)) <= 0) 
		{
            break;
		}
        send(clientfd, recvbuf, recvbytes, 0);
		recvbuf[recvbytes]='\0';
		printf("Client %d says:%s", clientfd, recvbuf);
	}
 
	close(clientfd);
	return;
}

void sig_child_handler(int signo)
{
    pid_t pid;
    int stat;

    while( (pid = waitpid(-1, &stat, WNOHANG) > 0))
        printf("Child %d terminated\n", pid);
    return;
}

int SERVERPORT= 8080;
int main(int argc, char* argv[])
{
    if(argc == 2)
    {
        SERVERPORT = atoi(argv[1]);
    }

    int sock_server = socket(AF_INET, SOCK_STREAM, 0);
    if(sock_server < 0)
    {
        perror("Socket cannot be created");
        return -1;
    }

    int on = 1;
	setsockopt(sock_server , SOL_SOCKET, SO_REUSEADDR, &on, sizeof(int)) ;
    sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(SERVERPORT);
    server_addr.sin_addr.s_addr = htons(INADDR_ANY);
    if(bind(sock_server, (sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Bind error");
        return -1;
    }

    if(listen(sock_server, 4096) < 0) {
        perror("Listen error");
        return -1;
    }

    signal(SIGCHLD, sig_child_handler);

    while(true)
    {
        sockaddr_in client_addr;
        socklen_t length = sizeof(client_addr); 
        int sock_client = accept(sock_server, (sockaddr*)&client_addr, &length);
        printf("%d\n", sock_client);
        if(sock_client < 0)
        {
            perror("Accept error");
            break;
        }

        pid_t pid = fork();
        if(pid == 0) //child process
        {
            close(sock_server);
            echo_worker((void*)&(sock_client));
            exit(0);
        }else{ //parent process
            close(sock_client);
        }
    }
}