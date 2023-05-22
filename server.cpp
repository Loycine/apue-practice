//练习reactor模式的多线程服务器
#include<iostream>
#include<cstring>
#include<sys/types.h>
#include<sys/socket.h>

//定义网卡与iP相关的定义文件
#include<netinet/in.h>
//定义一些格式转换函数，例如大端序，小端序函数便会使用这个库函数进行修改
#include<arpa/inet.h>

//定义了unix 下的标准库函数，例如getpid read write 等
#include<unistd.h>

//文件操作的头文件
#include<fcntl.h>

#include<sys/epoll.h>

//对应处理信号量的头文件
#include<signal.h>

//多线程库
#include<pthread.h>

//信号量提供库
#include<semaphore.h>
#include<list>
#include<cerrno>
#include<ctime>
#include<sstream>
#include<iomanip>
#include<cstdlib>
#include<cstdio>

const int k_WORKER_THREAD_NUM  = 5;
#define min(a, b) ((a<=b)? (a) : (b) )

//epoll的文件描述符
int g_epoll_fd = 0;

//不知道有什么作用
bool g_b_stop = false;
//监听的文件描述符
int g_listen_fd = 0;

pthread_t g_accept_thread_id = 0;

//全部初始化为0， 这里是work线程
pthread_t g_thread_ids[k_WORKER_THREAD_NUM] = {0};

//感觉有点类似于生产者与消费者的feel，主线程为生产者，work线程为消费者
pthread_cond_t g_accept_cond;

pthread_mutex_t g_accept_mutex;

//下面的条件变量还有互斥量不知道用来做什么好
pthread_cond_t g_cond;
pthread_mutex_t g_mutex;

//客户端互斥量，不知道是用来干哈的
pthread_mutex_t g_client_mutex;

//记录客户端，不知道干哈用，继续向下看
std::list<int> g_list_clients;



void programExit(int signo) {
    //::表示全局作用符的意思
    //注册信号还有信号处理函数
    //SIG_IGN 忽略该信号如 ^C 或者del. ^\发出SIG_QUIT信号
    //SIG_DFL 恢复系统默认处理
    //忽略中断信号
    ::signal(SIGINT, SIG_IGN);
    //忽略KILL信号
    ::signal(SIGKILL, SIG_IGN);
    //忽略终止信号
    ::signal(SIGTERM, SIG_IGN);

    std::cout << "Program recv signal " << signo << " to exit" << std::endl;
    g_b_stop = true;

    //注册epoll事件 从epollfd中删除一个fd
    ::epoll_ctl(g_epoll_fd, EPOLL_CTL_DEL, g_listen_fd, NULL);

    //shutdown是用来做什么用的啊, 意思是关闭TCP全双工连接，读和写半部全关闭
    ::shutdown(g_listen_fd, SHUT_RDWR);
    //将套接字从内存中移除
    ::close(g_listen_fd);
    ::close(g_epoll_fd);

    //销毁变量
    ::pthread_cond_destroy(&g_accept_cond);
    ::pthread_mutex_destroy(&g_accept_mutex);

    ::pthread_cond_destroy(&g_cond);
    ::pthread_mutex_destroy(&g_mutex);

    ::pthread_mutex_destroy(&g_client_mutex);
}

//构建监听端口，同时构建epoll监听文件描述符，监听指定的套接字
bool createServerListen(const char* ip, short port) {
    //非阻塞的意思是，如果读取的数据不为空，直接返回读取数据大小，否则返回一个-EAGAIN， 提醒应用程序继续尝试读取
    //阻塞型的话，那么一旦读取为 0 则使用sk_wait_data将当前线程进行挂起
    //这行代码可以等价socket函数之后再使用fcntl()去修改为非阻塞
    g_listen_fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);

    if(g_listen_fd == -1) {
        //创建套接字失败
        return false;
    }

    //该处不明白作用何在,指示服务器立即回收端口使用
    int on = 1;
    ::setsockopt(g_listen_fd, SOL_SOCKET, SO_REUSEADDR, (char*)&on, sizeof(on));
    ::setsockopt(g_listen_fd, SOL_SOCKET, SO_REUSEPORT, (char*)&on, sizeof(on));

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = inet_addr(ip);
    server_addr.sin_port = htons(port);
    //绑定端口
    if(::bind(g_listen_fd, (sockaddr*)&server_addr, sizeof(server_addr)) == -1) {
        return false;
    }
   
    //使端口进入被动监听,50指定是请求队列大小
    if(::listen(g_listen_fd, 50) == -1) {
        return false;
    }

    //创建epoll文件描述符，参数为1仅仅是为了兼容以前的代码，实际上没有什么用
    g_epoll_fd = ::epoll_create(1);
    if(g_epoll_fd == -1) {
        return false;
    }

    struct epoll_event e;
    memset(&e, 0, sizeof(e));
    //对端连接断开触发的 epoll 事件会包含 EPOLLIN | EPOLLRDHUP，即 0x2001。
    e.events = EPOLLIN | EPOLLRDHUP;
    e.data.fd = g_listen_fd;
    
    //添加监听套接字
    if(::epoll_ctl(g_epoll_fd, EPOLL_CTL_ADD, g_listen_fd, &e) == -1) {
        return false;
    }
    return true;
}

//释放客户端连接过来的TCP套接字
void releaseClient(int client_fd) {
    int epoll_result = 0;
    epoll_result = ::epoll_ctl(g_epoll_fd, EPOLL_CTL_DEL, client_fd, NULL);
    if(epoll_result == -1) {
        std::cout << "release client socket failed as call epoll_ctl failed" << std::endl;
    }
    //从内存中将该文件描述符进行移除
    ::close(client_fd);
}

//接受线程处理函数，来一个请求便将其添加到epoll里面中去，每一个文件描述符设置为边缘触发的模式
void* acceptThreadFunc(void* arg) {
    while(!g_b_stop) {
        //有人会和它竞争吗？没有啊
        ::pthread_mutex_lock(&g_accept_mutex);
        
        //等待接受线程条件为真
        ::pthread_cond_wait(&g_accept_cond, &g_accept_mutex);
        //收到通知，线程苏醒进行处理
        struct sockaddr_in client_addr;
        socklen_t addrlen;
        int new_client_fd = ::accept(g_listen_fd, (struct sockaddr*)&client_addr, &addrlen);
        ::pthread_mutex_unlock(&g_accept_mutex);

        //等待下一个TCP连接
        if(new_client_fd == -1) {
            continue;
        }

        //客户端的ip, port
        std::cout << "new connection: " << ::inet_ntoa(client_addr.sin_addr) << ":" << ::ntohs(client_addr.sin_port) << std::endl;

        //将新socket设置为非block模式，因为边缘触发仅仅支持non-block socket
        int old_flag = ::fcntl(new_client_fd, F_GETFL, 0);
        int new_flag = old_flag | O_NONBLOCK;
        if(::fcntl(new_client_fd, F_SETFL, new_flag) == -1) {
            std::cout << "fcntl error, old_flag  = " << old_flag << ", new_flag =" << new_flag << std::endl;
            continue;
        }

        //添加进epoll中
        struct epoll_event e;
        memset(&e, 9, sizeof(e));
        //设置为边缘触发
        e.events = EPOLLIN | EPOLLRDHUP | EPOLLET;
        e.data.fd = new_client_fd;
        if(::epoll_ctl(g_epoll_fd, EPOLL_CTL_ADD, new_client_fd, &e) == -1) {
            std::cout << "epoll ctl error, fd is = " << new_client_fd << std::endl;
        }
    }
    return NULL;
}

//工作线程，负责处理客户端的请求，并且进行回复
void* workerThreadFunc(void* arg) {
    while(!g_b_stop) {
        //有没有可能一个线程处理了所有请求呢
        int client_fd = 0;
        //为什么要在这里加锁，一时间还没有明白为什么，再看看
        ::pthread_mutex_lock(&g_client_mutex);

        //这个地方就体现出了，signal函数的虚惊现象，为了避免这种现象，所以使用了while
        while(g_list_clients.empty()) {
            ::pthread_cond_wait(&g_cond, &g_client_mutex);
        }
        client_fd = g_list_clients.front();
        g_list_clients.pop_front();
        ::pthread_mutex_unlock(&g_client_mutex);

        //gdb 不能实时输出，使用这个函数实时刷新标准输出
        std::cout << std::endl;
        std::string str_client_msg;
        char buff[256];
        bool is_error = false;

        while(true) {
            //读取连接套接字的内容
            memset(buff, 0, sizeof(buff));
            int n_recv = ::recv(client_fd, buff, 256, 0);
            if(n_recv == -1) {
                if(errno == EWOULDBLOCK) { /*表明无数据可读了*/break; }
                else {
                    std::cout << "recv error, client disconnected, fd = " << client_fd  << std::endl;
                    releaseClient(client_fd);
                    is_error = true;
                    break;
                }
            }
            //如果客户端关闭了socket，服务器端也同时关闭
            else if(n_recv == 0) {
                std::cout << "peer closed, clienf disconnected, fd=" << client_fd << std::endl;
                releaseClient(client_fd);
                is_error = true;
                break;
            }
            //注意这部分内容
            str_client_msg += buff;
        }
        if(is_error) {
            //出现了错误，不需要继续向下执行
            continue;
        }
        std::cout << "Handle Thread is: "<< pthread_self()<<"， client msg: " << str_client_msg << std::endl;
        //将消息加上时间戳进行返回
        time_t now = time(NULL);
        struct tm* nowstr = localtime(&now);
        std::ostringstream ostimestr;
        ostimestr << "[" << nowstr->tm_year + 1900 << "-" << std::setw(2) << std::setfill('0') << nowstr->tm_mon + 1 << "-" << std::setw(2) << std::setfill('0') << nowstr->tm_mday << " " << std::setw(2) << std::setfill('0') << nowstr->tm_hour << ':' << std::setw(2)<<std::setfill('0') << nowstr->tm_min << ":" << std::setw(2) << std::setfill('0') << nowstr->tm_sec << "] server reply";
        str_client_msg.insert(0, ostimestr.str());
       
        //发送消息
        while(true) {
            int n_send = ::send(client_fd, str_client_msg.c_str(), str_client_msg.size(), 0);
            if(n_send == -1) {
                if(errno == EWOULDBLOCK) {
                    //没有缓冲区可以写入？
                    ::sleep(10);
                    continue;
                }
                else {
                    std::cout << "send error, fd=" << client_fd << std::endl;
                    releaseClient(client_fd);
                    break;
                }
            }
            //这里是否会分段输出呢？
             std::cout << "reply msg: " << str_client_msg << std::endl;
             str_client_msg.erase(0, n_send);
             if(str_client_msg.empty()) {
                 break;
             }
        }

    }
    return NULL;
}


void daemonRun() {
    //创建守护进程
    int pid = 0;
    //忽略子进程状态改变信号
    ::signal(SIGCHLD, SIG_IGN);

    //fork 父进程get 子进程id, 子进程get 0， 错误 为 0
    pid = fork();
    if(pid < 0) {
        std::cout << "fork error" << std::endl;
        //退出进程
        exit(-1);
    }
    else if(pid > 0 ) {
        //退出父进程
        exit(0);
    }

    //parent child 一开始在一个session里面， parent是进程组组长，如果父进程exit，那么子进程会成为孤儿进程，被init收养
    //setsid()child获得了一份新的回话id，父进程不再影响子进程
    ::setsid();
    int fd = 0;
    //这部分的作用是什么
    fd = open("/dev/null", O_RDWR, 0);
    if(fd != -1) {
        std::cout << "fd is: " << STDIN_FILENO << std::endl;
        std::cout << "STDIN is: " << STDIN_FILENO << std::endl;
        std::cout << "STDOUTis: " <<STDOUT_FILENO << std::endl;
        std::cout << "STDERR is: " << STDERR_FILENO<< std::endl;
        dup2(fd, STDIN_FILENO);
        std::cout << "new STDIN is: " << STDIN_FILENO << std::endl;
        dup2(fd, STDOUT_FILENO);
        std::cout << "new STDIN is: " << STDIN_FILENO << std::endl;
        dup2(fd, STDERR_FILENO);
    }
    if(fd > 2) {
        close(fd);
    }
}

int main(int argc, char** argv) {
    ::printf("Go to start server, Let's goooooooo\n");
    short port = 0;
    int ch = 0;
    bool is_daemon = false;
    //a:b:cd::e 冒号表示必须有一个参数，参数可以空格隔开也可以连着， ::标识参数可选，但一旦有参数，参数必须连接选项 单个字符表示没有参数
    while((ch = getopt(argc, argv, "p:d")) != -1) {
        switch(ch) {
            case 'd':
                is_daemon = true;
                break;
            case 'p':
                std::cout << "Now handle port arg" << std::endl;
                port = atol(optarg);
                break;
        }
    }
    if(is_daemon) {
        printf("Now we want to change ourselves to daemon process");
        daemonRun();
    }
    if(port == 0) {
        printf("port is %d\n", port);
        port = 12345;
    }

    if(!createServerListen("0.0.0.0", port)) {
        std::cout << "Create Server error ip: 0.0.0.0 and port is: " << port << std::endl;
        return -1; 
    }

    printf("server create success!!!\n");
    std::cout << std::endl;
    //设置信号处理
    signal(SIGCHLD, SIG_DFL);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, programExit);
    signal(SIGKILL, programExit);
    signal(SIGTERM, programExit);

    //线程互斥量与条件变量处理
    pthread_cond_init(&g_accept_cond, NULL);
    pthread_mutex_init(&g_accept_mutex, NULL);
    pthread_cond_init(&g_cond, NULL);
    pthread_mutex_init(&g_mutex, NULL);

    pthread_mutex_init(&g_client_mutex, NULL);
    //创建生产者线程
    pthread_create(&g_accept_thread_id, NULL, acceptThreadFunc, NULL);
    
    //创建消费者线程
    for(int i = 0; i < k_WORKER_THREAD_NUM; ++i) {
        pthread_create(&g_thread_ids[i], NULL, workerThreadFunc, NULL);
    }

    for(int i = 0; i < k_WORKER_THREAD_NUM; i++) {
        std::cout << g_thread_ids[i] << " ";
    }
    std::cout << std::endl;

    while(!g_b_stop) {
        struct epoll_event ev[1024];
        //该函数用于轮询I/O事件的发生；最后一个数字表示超时时间， 如果超过最大事件数有blog说是不会丢弃事件的
        int n = ::epoll_wait(g_epoll_fd, ev, 1024, 10);
        if(n == 0) {
            continue;
        }
        else if(n < 0) {
            std::cout << "epoll wait error" << std::endl;
            continue;
        }

        int m = min(n, 1024);
        //如果一次性超过1024个事件发生，是否会有消息被忽略
        for(int i = 0; i < m; ++i) {
            
            //通知接收线程接收新连接
            if(ev[i].data.fd == g_listen_fd) {
                pthread_cond_signal(&g_accept_cond);
            }


            //通知普通线程进行处理
            else {
                pthread_mutex_lock(&g_client_mutex);

                g_list_clients.push_back(ev[i].data.fd);
                //究竟会唤醒多少个线程去处理呢
                pthread_cond_signal(&g_cond);
                pthread_mutex_unlock(&g_client_mutex);

            }
        }
    }
    return 0;
}