#define _XOPEN_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <pthread.h>

enum {
    TS = 1,
    FLV = 2
};

static char g_path[256];
typedef struct chan
{
    char  channel[256];
    char  path[256];
    int type;
    time_t start;
    struct chan * next;
} Channel;

static Channel * head = NULL;
static Channel * tail = NULL;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

int cfg_init(char * cfg_file){
    if (cfg_file == NULL) {
        return -1;
    }
    if (access(cfg_file, R_OK) != 0) {
        return -1;
    }
    
    FILE *file = fopen(cfg_file, "r");
    if (file == NULL) {
        return -1;
    }
    char line[256];
    char start[256];
    char * val;
    struct tm tm_temp;
    
    do {
        bzero(line, sizeof(line));
        bzero(start, sizeof(start));

        val = fgets(line, sizeof(line), file);
        if (strchr(line, '#') != NULL)
            continue;
        if (strchr(line, '/') != NULL) {
            line[strlen(line) - 1] = '\0';
            if (line[strlen(line) - 1] == ' ')
                line[strlen(line) - 1] = '\0';
            strcpy(g_path, line);
            continue;
        }

        line[strlen(line)] = '\0';  //remove \n
        char * space = strchr(line, ' ');
        if (space == NULL) continue;
        if (line[0] == ' ') continue;

        Channel *temp = (Channel *) calloc(1, sizeof(Channel));
        if (temp == NULL) continue;

        if (head == NULL) {
            head = temp;
            tail = temp; 
        }else {
            tail->next = temp;
            tail = temp;
        }

        strcpy(temp->path, g_path);
        strncpy(temp->channel, line, space - line);
        while (*space == ' ')
            space++;
        char * space2 = strchr(space, ' ');
        if (space2 != NULL) {
            strncpy(start, space, space2 - space);
            char *ts = strstr(space2, "ts");
            if (ts != NULL) {
              temp->type = TS;
              memset(&tm_temp, 0, sizeof(tm_temp));
              strptime(start, "%Y%m%d%H%M%S", &tm_temp);
              temp->start = mktime(&tm_temp);
            }
            else {
              temp->type = FLV;
              memset(&tm_temp, 0, sizeof(tm_temp));
              strptime(start, "%Y%m%d%H%M", &tm_temp);
              temp->start = mktime(&tm_temp);
            }
        }
        temp->next = NULL;
    } while (val != NULL);
    fclose(file);
    return 0;
}

// return start time
time_t cfg_channel_search(char *channel, char **path) {
    Channel * temp = head;
    if (channel == NULL)
        return (time_t)0;
    if (path == NULL)
        return (time_t)0;
    if (*path == NULL)
        return (time_t)0;

    pthread_mutex_lock(&lock);
    strcpy(*path, g_path);
    for (; temp != NULL; temp = temp->next)
    {
        if (strcmp(temp->channel, channel) == 0) {
            pthread_mutex_unlock(&lock);
            return temp->start;
        }
    }
    pthread_mutex_unlock(&lock);
    return (time_t)0;
    
}

int cfg_reload(char *cfg_file)
{
    if (cfg_file == NULL) {
        return -1;
    }

    Channel * temp = head;
    Channel * backward ;
    pthread_mutex_lock(&lock);
    for (; temp != NULL; ) {
        backward = temp->next;
        free(temp);
        temp = backward;
    }
    head = NULL;
    tail = NULL;
    cfg_init(cfg_file);
    pthread_mutex_unlock(&lock);
    return 0;
}

int cfg_dump(void)
{
    pthread_mutex_lock(&lock);
    Channel * temp = head;
    for (; temp != NULL; temp = temp->next) {
         printf("Path:%s\n", temp->path);   
         printf("Channel:%s\n", temp->channel);   
         printf("Start:%ld\n", temp->start);   
         printf("Type:%d\n", temp->type);   
         printf("Next:%p\n", temp->next);   
    }
    pthread_mutex_unlock(&lock);
    return 0;
}
#if 0
int main(int argc, char **argv)
{
    char buf[244];
    char *p = buf;
    time_t tmp;
    cfg_reload("./live.cfg");
    cfg_dump();
    tmp = cfg_channel_search("cctv1", &p);
    printf("buf:%s\n", buf);   
    printf("start:%lld\n", (uint64_t)tmp);   
}
#endif
