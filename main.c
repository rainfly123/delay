#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h> 
#include <stdint.h> 
#include <ctype.h> 
#include <fcntl.h>
#include <errno.h>
#include <err.h>
#include <stddef.h>
#include "ev.h"
#include <time.h>
#include <pthread.h>
#include <stdint.h>
#include "ts.h"
#include "flv.h"
#include "cfg.h"

#define SERVER_PORT 8080
#define DEBUG 0

enum {
    NOTINIT = 0,
    CREATED = 1,
    SEEK = 2,
    RUNNING = 3
};

enum {
    FLV = 1,
    TS = 2
};
typedef struct {
    int fd;
    int file_fd;
    ev_io ev_write;
    ev_io ev_read;
    ev_timer ev_time;
    char name[32];
    char g_path[256];
    int  state ;
    time_t start_time;
    time_t cfg_start_time;
    uint64_t last_time;
    uint64_t last_pcr;
    uint32_t inited_time;
    uint64_t play_duration;
    uint64_t send_duration;
    uint32_t media;
    uint32_t in_used;
}client;

//only use in work_loop
#define MAX_CON 1024
static client sessions[MAX_CON];

//the three variables used to communication
pthread_mutex_t lock;
client swap;
ev_async async_watcher;

struct ev_loop *work_loop; 


char * flv_head="HTTP/1.1 200 OK\r\nConnection: keep-alive\r\nContent-Type: video/x-flv\r\nServer: xiecc/0.1\r\n\r\n";
char *ts_head="HTTP/1.1 200 OK\r\nConnection: keep-alive\r\nContent-Type: video/MP2T\r\nServer: xiecc/0.1\r\n\r\n";
char *error_head = "HTTP/1.1 404 NOT FOUND\r\nContent-Type:text/html\r\nContent-Length:56\r\nConnection:Keep-Alive\r\n\r\n<html><body><center>404 Not Found</center></body></html>";

uint64_t now_time(void)
{
    struct timeval t;
    gettimeofday(&t, NULL);

    uint64_t curTime;
    curTime = t.tv_sec;
    curTime *= 1000;                // sec -> msec
    curTime += t.tv_usec / 1000;    
    return curTime;
}

int setnonblock(int fd)
{
    int flags;
    int one = 1;
    int bufSize = 128 * 1024;
    int KeepAliveProbes = 1;
    int KeepAliveIntvl = 2;
    int KeepAliveTime = 120;
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bufSize, sizeof(int));
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(int));
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(int));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &KeepAliveProbes, sizeof(int));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &KeepAliveTime, sizeof(int));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &KeepAliveIntvl, sizeof(int));
    flags = fcntl(fd, F_GETFL);
    if (flags < 0)
        return flags;
    flags |= O_NONBLOCK;
    if (fcntl(fd, F_SETFL, flags) < 0) 
        return -1;
    return 0;
}

void timeout_cb (struct ev_loop *loop, ev_timer *w, int revents)
{
    client * session  = (client *)(((char *)w) - offsetof (client, ev_time));
    if (session->fd == 0 ) {
        return;
    }
    //ev_timer_stop (work_loop, &session->ev_time);
    ev_io_start(work_loop, &session->ev_write);
}

ssize_t safe_send(int fd, void *buffer, ssize_t size)
{
    int err;
    do {
       if (fd > 0)
       err = send(fd, buffer, size, 0);//flags??
    } while((err == -1) && (errno == EINTR));
   return (ssize_t)0;
}

void write_cb(struct ev_loop *loop, struct ev_io *w, int revents)
{ 
    if (EV_ERROR & revents)
    {
        perror(" w got invalid event");
        return;
    }
	client *session = (client*) (((char*)w) - offsetof(client, ev_write));
        if (session->fd == 0 )
        { 
            return;
        }
        switch (session->state) {
        
            case CREATED: {
                const char *head;
		char meta[255];
		int meta_fd;
		ssize_t size;
		char meta_data[2048];

		sprintf(meta, "%s/%s/flv", session->g_path, session->name);
		if (access(meta, R_OK) == 0) {
		    head = flv_head;
                    session->media= FLV;
                }
		else {
		    sprintf(meta, "%s/%s/ts", session->g_path, session->name);
		    head = ts_head;
                    session->media= TS ;
		}
                //printf("%s\n", meta);
		meta_fd = open(meta, O_RDONLY);
                if (meta_fd) {
		    size = read(meta_fd, meta_data, sizeof(meta_data));
		    close(meta_fd);
                } else
                {
                    head = error_head;
                }
		if (revents & EV_WRITE){
		    //write(w->fd, head, strlen(head));
                    ssize_t len = safe_send(w->fd, (void*)head, strlen(head));
                    if (len < 0)  {
                       if (errno == EAGAIN)
                           break;
                       else {
			       ev_io_stop(work_loop, &session->ev_write);
			       ev_io_stop(work_loop, &session->ev_read);
                           if (w->fd) {
                               close(w->fd);
                               session->fd = 0;
                           }
                           break;
                       }
                    }
                    //channel not found
                    if (head == error_head) {
			ev_io_stop(work_loop, &session->ev_write);
			ev_io_stop(work_loop, &session->ev_read);
                        if (w->fd) {
                            close(w->fd);
                            session->fd = 0;
                        }
                        break;
                    }
                    len = safe_send(w->fd, meta_data, size);
                    if (len < 0)  {
                       if (errno == EAGAIN)
                           break;
                       else {
			       ev_io_stop(work_loop, &session->ev_write);
			       ev_io_stop(work_loop, &session->ev_read);
                               if (w->fd) {
                                   close(w->fd);
                                   session->fd = 0;
                               }
                           break;
                       }
                    }
		}
                session->state = SEEK;
                break;
            }
            case SEEK: {
                char timestr [64];
                char indexstr [64];
                char fullpath[255];
                int sec;
                if (session->media == TS) {
                    struct tm start;
                    localtime_r(&session->start_time, &start);
                    sec = start.tm_sec % 10;
                    start.tm_sec = start.tm_sec - sec;
                    session->start_time -= sec;
                    strftime(timestr, sizeof(timestr), "%Y%m%d%H%M%S.ts", &start);
                    strftime(indexstr, sizeof(timestr), "%Y%m%d%H%M%S.index", &start);
                }else {
                    struct tm start;
                    localtime_r(&session->start_time, &start);
                    sec = start.tm_sec;
                    strftime(timestr, sizeof(timestr), "%Y%m%d%H%M.flv", &start);
                    strftime(indexstr, sizeof(timestr), "%Y%m%d%H%M.index", &start);
                }
                //printf("seek %s\n", timestr);
                off_t pos = 0;
                int fd;
                sprintf(fullpath, "%s/%s/%s", session->g_path, session->name, indexstr);
                fd = open(fullpath, O_RDONLY);
                if (fd > 0) {
                    lseek(fd, 4 * sec, SEEK_SET);
                    read(fd, &pos, sizeof(pos));
                    close(fd);
                }
                sprintf(fullpath, "%s/%s/%s", session->g_path, session->name, timestr);
                //printf("%s\n", fullpath);
                session->file_fd = open(fullpath, O_RDONLY);
                if (session->file_fd < 0) {
                    printf("no channel %s\n", fullpath);
		    ev_io_stop(work_loop, &session->ev_write);
		    ev_io_stop(work_loop, &session->ev_read);
		    close(w->fd);
                    session->fd = 0;
                    break;
                }
                if (pos > 0)
                    lseek(session->file_fd, pos, SEEK_SET);
                session->state = RUNNING;
                break;
            }
            case RUNNING: {
                int interval = 0;
                if (session->media == TS) { //ts
                    unsigned char buffer[TSPACKET_SIZE * 7];
                    unsigned char *p ;
                    uint32_t has_pcr = 0;
                    uint64_t pcr = 0;
                    int64_t diff_time = 0;
                    int64_t diff_pcr = 0;
                    ssize_t len = read(session->file_fd, buffer, sizeof(buffer));
                    if (len <= 0) {
                        ///////////////////////////
                        close(session->file_fd);
                        struct tm start;
                        char timestr[64];
                        char fullpath[255];
                        session->start_time += 10;
                        localtime_r(&session->start_time, &start);
                        strftime(timestr, sizeof(timestr), "%Y%m%d%H%M%S.ts", &start);
                        sprintf(fullpath, "%s/%s/%s", session->g_path, session->name, timestr);

                        session->file_fd = open(fullpath, O_RDONLY);
                        if (session->file_fd < 0)
                        {
                            if (session->cfg_start_time != 0) {
                                 session->start_time = session->cfg_start_time - 10;
                            }
                            else
                                 session->start_time += 10;
                        }
                        session->inited_time = 0;
                        break;
                    }
                    int i;
                    for (i = 0; i < 7; i++)
                    {
                       if ((i * TSPACKET_SIZE) >= len)
                           break;
                       p = buffer + i * TSPACKET_SIZE;
                       if (TSPACKET_HAS_ADAPTATION(p))
                       {
                           if (TSPACKET_GET_ADAPTATION_LEN(p) > 0)
                           {
                               if (TSPACKET_IS_PCRFLAG_SET(p)) {
                                  has_pcr  = 1;
                                   pcr = (TSPACKET_GET_PCRBASE(p)) / 45 ;
                                   if (session->inited_time > 0) {
                                       diff_pcr = pcr - session->last_pcr;
                                       session->last_pcr = pcr;
                                   } else {
                                       session->play_duration = 0;
                                       session->send_duration = 0;
                                       session->inited_time = 1;
                                       session->last_pcr = pcr;
                                       session->last_time = now_time();
                                   }
                               }
                           } 
                       }
                    }

                    len = safe_send(w->fd, buffer, len);
                    if (len < 0)  {
                        if (errno == EAGAIN)
                            break;
                        else {
		                 ev_io_stop(work_loop, &session->ev_write);
		                 ev_io_stop(work_loop, &session->ev_read);
		                 ev_timer_stop (work_loop, &session->ev_time);
                            if (w->fd) {
                               close(w->fd);
                               session->fd = 0;
                            }
                            if (session->file_fd) {
                               close(session->file_fd);
                               session->file_fd = 0;
                            }
                            break;
                        }
                   }
                   if (has_pcr == 0){
                       break;
                   }
                   uint64_t now = now_time();
                   diff_time = now - session->last_time;
                   session->last_time = now;
                   if (diff_pcr < 0) {
                       session->play_duration += 40;
                   }
                   if (diff_time < 0) {
                       session->send_duration += 40;
                   }
                   session->play_duration += diff_pcr;
                   session->send_duration += diff_time;
                   interval = session->play_duration - session->send_duration;
                   if (interval <= 0) {
                       break;
                   }
                   if (interval >= 2) {
                       ev_io_stop(work_loop, w);
                       float c = interval / 1000.0;
                       ev_timer_set(&session->ev_time, c, 0.);
                       ev_timer_start (work_loop, &session->ev_time);
                   }
                  
                }
                else { //flv
                ////////////////////////////////////
                    Tag_s tag;
                    int64_t diff_time = 0;
                    int64_t diff_pcr = 0;
                    uint32_t has_pcr = 0;
                    ssize_t len = flv_read_tag(&tag, session->file_fd);
                    if (len <= 0) {
                        ///////////////////////////
                        close(session->file_fd);
                        struct tm start;
                        char timestr[64];
                        char fullpath[255];
                        session->start_time += 60;
                        localtime_r(&session->start_time, &start);
                        strftime(timestr, sizeof(timestr), "%Y%m%d%H%M.flv", &start);
                        sprintf(fullpath, "%s/%s/%s", session->g_path, session->name, timestr);
#ifdef DEBUG
                        //debug
                        sprintf(fullpath, "%s/%s/same.flv", session->g_path, session->name);
                        //debug
#endif
                        session->file_fd = open(fullpath, O_RDONLY);
                        printf("change file\n");
                        session->inited_time = 0;
                        break;
                    }
                    if (IS_VIDEO_TAG(tag)) {
                        has_pcr = 1;
                    if (session->inited_time > 0) {
                        diff_pcr =  tag.time_stamp - session->last_pcr;
                        session->last_pcr = tag.time_stamp;
                    } else {
                        //reinit
                        session->play_duration = 0;
                        session->send_duration = 0;
                        session->inited_time = 1;
                        session->last_time = now_time();
                        session->last_pcr = tag.time_stamp;
                    }
                    }

                    len = safe_send(w->fd, tag.data, 15 + len);
                    if (len < 0)  {
                        free(tag.data);
                        if (errno == EAGAIN)
                            break; 
                        else {
                            ev_io_stop(work_loop, w);
		            ev_io_stop(work_loop, &session->ev_read);
		            ev_timer_stop (work_loop, &session->ev_time);
                            if (session->file_fd) {
                               close(session->file_fd);
                               session->file_fd = 0;
                            }
                            if (w->fd) {
		                close(w->fd);
                                session->fd = 0;
                            }
                            break;
                        }
                   }
                   free(tag.data);
                   if (has_pcr == 0){
                       break;
                   }
                   uint64_t now = now_time();
                   diff_time = now - session->last_time;
                   session->last_time = now;
                   if (diff_pcr < 0) {
                       session->play_duration += 40;
                   }
                   if (diff_time < 0) {
                       session->send_duration += 40;
                   }

                   session->play_duration += diff_pcr;
                   session->send_duration += diff_time;
                   interval = session->play_duration - session->send_duration;
                   if (interval <= 0) {
                       break;
                   }
                   if (interval >= 2) {
                       ev_io_stop(work_loop, w);
                       float c = interval / 1000.0;
                       ev_timer_set(&session->ev_time, c, 0.);
                       ev_timer_start (work_loop, &session->ev_time);
                   }
                }
                break;
            }
            default:
                printf("state error\n");
        } 
         	//close(w->fd);
//	此处可以安装一个ev_timer ev_timer的回调中，再次安装ev_io write
}

void read_cb(struct ev_loop *loop, struct ev_io *w, int revents)
{
    char buffer[1024];
    ssize_t read;

    if (EV_ERROR & revents)
    {
        perror("got invalid event");
        return;
    }
    client * session  = (client *)(((char *)w) - offsetof (client, ev_read));

    if (session->fd == 0) {
         return;
    }


    read = recv(w->fd, buffer, sizeof(buffer), 0);
    if (read <= 0)
    {
        ev_io_stop(work_loop, &session->ev_read);
        if (session->state != NOTINIT) {
            ev_io_stop(work_loop, &session->ev_write);
            ev_timer_stop (work_loop, &session->ev_time);
        }
        if (w->fd) {
            close(w->fd);
            session->fd = 0;
        }
        if (session->file_fd) {
            close(session->file_fd);
            session->file_fd = 0;
        }
        perror("peer closing");
        return;
   }
   else
   {
       if (session->state != NOTINIT){
           return;
       }

       char *p = session->name;
       char *q ;
       int len = sizeof(session->name);
       q = strchr(buffer, '/');
       if (q == NULL) {
           ev_io_stop(work_loop, w);
           if (session->file_fd) {
               close(session->file_fd);
               session->file_fd = 0;
           }
           if (w->fd) {
              close(w->fd);
              w->fd = 0;
           }
           printf("no channel id\n");
           return;
       }
       q++;
       while ((isalnum(*q) != 0) && (--len))
          *p++ = *q++;
       *p = '\0';
       char *p_path = session->g_path;
       time_t temp =  cfg_channel_search(session->name, &p_path);
       if (temp != 0) {
           session->start_time = temp;
           printf("g_path:%s channel:%s start:%ld\n", session->g_path, \
                      session->name, session->start_time);
           session->cfg_start_time = temp;
       }else {
           q = strstr(buffer, "timeshift=");
           if (q != NULL) {
               q += strlen("timeshift=");
               session->start_time = time(NULL) -  atoi(q);
               session->cfg_start_time = 0;
               printf("g_path:%s channel:%s start:%ld timeshift\n",session->g_path,\
                        session->name,session->start_time);
           }
       }

       session->state = CREATED;

       ev_io_init(&session->ev_write, write_cb, w->fd, EV_WRITE);
       ev_io_start(work_loop, &session->ev_write);
       ev_timer_init(&session->ev_time, timeout_cb, 0., 0.);
       return ;
   }
}

inline uint32_t  acquire_buffer(void)
{
    uint32_t i = 0;
    for (; i < MAX_CON; i++)
    {
        if (sessions[i].in_used == 0)
            return i;
    }
    return MAX_CON;
}

inline void release_buffer(void)
{
    uint32_t i = 0;
    
    for (; i < MAX_CON; i++)
    {
        if (sessions[i].in_used == 1) {
            if (sessions[i].fd == 0 ) {
                sessions[i].in_used = 0;
            }
        }
 
    }
}



static void async_cb (EV_P_ ev_async *w, int revents)

{
    client *session ;
    uint32_t i = acquire_buffer();
    if (i == MAX_CON) {
        return;
    }
    session = &sessions[i];

    pthread_mutex_lock(&lock);     //Don't forget locking
    memcpy(session, &swap, sizeof(client));
    pthread_mutex_unlock(&lock);   //Don't forget unlocking


    printf("new connection %d\n", session->fd);
    ev_io_init(&session->ev_read, read_cb, session->fd, EV_READ);
    ev_io_start(work_loop, &session->ev_read);
}

void idle_cb (struct ev_loop *loop, ev_idle *w, int revents)
{
    release_buffer();
    usleep(1000);
}


void * work(void *p)
{
    signal(SIGPIPE, SIG_IGN);
    ev_idle idle_watcher;
    ev_idle_init (&idle_watcher, idle_cb);
    ev_idle_start(work_loop, &idle_watcher);
    ev_async_init(&async_watcher, async_cb);
    ev_async_start(work_loop, &async_watcher);
    ev_loop(work_loop, 0);
    return (void *)0;
}

pthread_t thread_create(void * (*Run)(void * inData) ,void *inParam)
{
    //int max_priority = 0;
    pthread_t tid = 0 ;
    int err = -1 ;
    //struct sched_param param;
    pthread_attr_t attr;
    pthread_attr_init(&attr); /*初始化线程属性变量*/
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setscope(&attr, PTHREAD_SCOPE_SYSTEM);
    //pthread_attr_setschedpolicy(&attr,SCHED_RR);
    // max_priority = sched_get_priority_max(SCHED_RR);
    //param.sched_priority=max_priority;
    //pthread_attr_setschedparam(&attr,&param);
    err = pthread_create(&tid,&attr,Run,inParam);
    if(err != 0 ) 
    {
        perror("Error:thread:");
        return -1 ;
    }
    pthread_attr_destroy(&attr);
    return tid ;
}



static void accept_cb(struct ev_loop *loop, struct ev_io *w, int revents)
{
    int client_fd;
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);

    if (EV_ERROR & revents)
    {
        perror(" w got invalid event");
        return;
    }

    client_fd = accept(w->fd, (struct sockaddr *)&client_addr, &client_len);
    if (client_fd == -1) {
        return;
    }
    setnonblock(client_fd) ;
    if (!ev_async_pending(&async_watcher)) {
        pthread_mutex_lock(&lock);
        memset(&swap, 0, sizeof(swap));
        swap.fd = client_fd;
        swap.state = NOTINIT;
        swap.start_time = 1384227300;
        swap.in_used = 1;
        pthread_mutex_unlock(&lock);
        ev_async_send(work_loop, &async_watcher);
    }
}
static char * cfg_path = NULL;
static void cfg_cb (struct ev_loop *loop, ev_stat *w, int revents)
{
    cfg_reload(cfg_path);
}
   
int main(int argc, char **argv)
{
    ev_stat cfg;
    if (argc == 1) {
        printf("no cfg\n");
        return 0;
    }
    if (argc == 2) {
        daemon(0, 0);
    }
    cfg_path = strdup(argv[1]);
    cfg_init(cfg_path);
    ev_stat_init (&cfg, cfg_cb, cfg_path, 2.);
    signal(SIGPIPE, SIG_IGN);
    struct ev_loop *loop = ev_default_loop (0);
    work_loop = ev_loop_new(0);
    int listen_fd;
    struct sockaddr_in listen_addr; 
    int reuseaddr_on = 1;
   
    pthread_mutex_init(&lock, NULL);
    thread_create(work, NULL);
    listen_fd = socket(AF_INET, SOCK_STREAM, 0); 
    if (listen_fd < 0) {
        perror("listen failed");
        return -1;
    }
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuseaddr_on,
            sizeof(reuseaddr_on)) == -1)
    {
        perror("setsockopt failed");
        return -1;
    }
    memset(&listen_addr, 0, sizeof(listen_addr));
    listen_addr.sin_family = AF_INET;
    listen_addr.sin_addr.s_addr = INADDR_ANY;
    listen_addr.sin_port = htons(SERVER_PORT);
    if (bind(listen_fd, (struct sockaddr *)&listen_addr,
            sizeof(listen_addr)) < 0)
    {
        perror("bind failed");
        return -1;
    }
    if (listen(listen_fd, 128) < 0)
    {
        perror("listen failed");
        return -1;
    }
    if (setnonblock(listen_fd) < 0)
    {
        perror("failed to set server socket to non-blocking");
        return -1;
    }
	 
    ev_io ev_accept;
    ev_io_init(&ev_accept, accept_cb, listen_fd, EV_READ);
    ev_io_start(loop, &ev_accept);
    if (cfg_path != NULL)
        ev_stat_start (loop, &cfg);
    ev_loop (loop, 0);
    return 0;
}
