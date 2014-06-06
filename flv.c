#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include "flv.h"

//data's len must equal 11 + 4 = 15
static int flv_tag_parse(Tag_s *tag, uint8_t *data)
{
   uint8_t * pos = data;
   uint32_t time;

   if (!tag || !data) {
       return -1; 
   }
   
   tag->pre_tag_size = *pos << 24 | *(pos + 1) << 16 | *(pos + 2) << 8 |*(pos + 3);
   pos += 4;
   tag->tagType = *pos++ &0x1f;
   tag->data_size = *pos << 16 | *(pos + 1) << 8 | *(pos + 2);
   pos += 3;
   time = *pos << 16 | *(pos + 1) << 8 | *(pos + 2);
   pos += 3;
   tag->time_stamp = *pos++ << 24 | time;
   tag->streamID = *pos << 16 | *(pos + 1) << 8 | *(pos + 2);
   tag->data = malloc(tag->data_size + 15);
   return 0;
 }

int flv_read_tag(Tag_s *tag, int fd)
{
    if (tag == NULL)
        return (ssize_t)-1;

    uint8_t data[15];
    int val;
   
    val = read(fd, data, 15); //flv tag
    if (val != 15) {
        printf("not 15\n");
        return (ssize_t)-1;
    }
    memset(tag, 0, sizeof(Tag_s));
    flv_tag_parse(tag, data);
    memcpy(tag->data, data, sizeof(data));
    return read(fd, (void *)(tag->data + 15), tag->data_size);
}

