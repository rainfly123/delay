#include <time.h>
#ifndef _CFG_H_
#define _CFG_H_
int cfg_init(char * cfg_file);
time_t cfg_channel_search(char *channel, char **path) ;
int cfg_reload(char *cfg_file);
int cfg_dump(void);
#endif
