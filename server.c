#include <getopt.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>

#include "base.h"

uint16_t colors[] = { 
    180,    0,      0,      /* red           */
    0,      180,    0,      /* green         */
    0,      0,      180,    /* blue          */
    180,    0,      180,    /* pink          */
    0,      180,    180,    /* cyan          */
    180,    180,    0,      /* dirty yellow  */
    255,    127,    0,      /* orange        */
    0,      102,    0,      /* dark green    */
    77,     0,      153,    /* dark violet   */
    153,    153,    153,    /* gray          */
    255,    153,    153,    /* pastel red    */
    255,    255,    0,      /* yellow        */
    153,    153,    255,    /* pastel violet */
    102,    0,      0,      /* dark red      */
    0,      0,      102,    /* dark blue     */
    0,      64,     64      /* dark cyan   */
};

struct point_s
{
    int x;
    int y;
};
typedef struct point_s point_t;

struct buffer_s
{
    char* buf;
    size_t size;
    size_t available_size;
    size_t used;
    size_t flush_size;
};
typedef struct buffer_s buffer_t;

struct amalgamate_config_s
{
    int port;
    char datafile[256];
    char grpfile[256];
    char htmlfile[256];
    buffer_t* buf;
    int n_streams;
    FILE** streams;
};
typedef struct amalgamate_config_s amalgamate_config_t;

struct client_status_s
{
    point_t view;
    point_t frame_top;
    point_t frame_bottom;
    unsigned int mask;
    int resolution;
};
typedef struct client_status_s client_status_t;

void usage();

void status_print(client_status_t* status)
{
    fprintf(stderr, "View: %d, %d\n", status->view.x, status->view.y); 
    fprintf(stderr, "Top: %d, %d\n", status->frame_top.x, status->frame_top.y); 
    fprintf(stderr, "Bottom: %d, %d\n", status->frame_bottom.x, status->frame_bottom.y); 
    fprintf(stderr, "Res: %d\n", status->resolution); 
}

buffer_t* buffer_create(size_t size, size_t flush_size)
{
    buffer_t* b;

    b = malloc(sizeof(buffer_t));
    b->size = size;
    b->used = 0;
    b->buf = malloc(sizeof(char) * size);
    b->flush_size = flush_size;
    return b;
}

point_t point_parse(char* str)
{
    char* x;
    char* y;
    char* s;
    point_t p = { -1, -1 };

    if((s = strchr(str, ',')) != NULL)
    {
        *s = '\0';
        x = str; 
        y = s + 1;
        p.x = atoi(x);
        p.y = atoi(y);
    }
    return p;
}

int configure(amalgamate_config_t* config, int argc, char** argv)
{
    /* current option */
    int ch;

    static struct option longopts[] = {
        {"port",    required_argument,  NULL,   'p'},
        {"streams", required_argument,  NULL,   'n'},
        {"html",    required_argument,  NULL,   'h'},
        {"groups",  required_argument,  NULL,   'g'},
        {NULL,      0,                  NULL,   0 }
    };
    
    /* DEFAULTS */
    config->port = 3333;
    config->n_streams = 5;
    config->datafile[0] = '\0';
    config->htmlfile[0] = '\0';
    
    while((ch = getopt_long(argc, argv, "p:n:h:g:", longopts, NULL)) != -1)
    {
        switch(ch)
        {
            case 'p':
                config->port = atoi(optarg);
                break;
            case 'n':
                config->n_streams = atoi(optarg);
                break;
            case 'h':
                strncpy(config->htmlfile, optarg, sizeof(config->htmlfile));
                break;
            case 'g':
                strncpy(config->grpfile, optarg, sizeof(config->grpfile));
                break;
            default:
                usage();
        }
    }
    argc -= optind;
    argv += optind;
    
    if(argc == 0)
    {
        printf("Input file name missing.\n");
        usage();
    }
    strncpy(config->datafile, argv[0], sizeof(config->datafile) - 1);
    if(config->n_streams > 0)
    {
        config->streams = malloc(sizeof(FILE*) * config->n_streams);
        memset(config->streams, 0, sizeof(FILE*) * config->n_streams);
    }
    else
        usage();
    return 0;
}

/**
 * Print program usage and exit with code -1.
 */
void usage()
{
    printf(
        "Usage:\n\n"\
        "amalgamate [-v|--view w,h] [-t|--frame-top x,y] [-b|--frame-bottom x,y]\n"\
        "           [-r|--resolution N] inputfile.dat\n\n"\
        "Options:\n"\
        " -v, --view w,h\t\twidth and height of the view window (default 400,400)\n");
    exit(-1);
}

void buffer_send(FILE* stream, buffer_t* buf)
{
    char* header = "Content-type: text/plain; charset=x-user-defined\n\n";
    char* footer = "--loldongz101\r\n";
    fprintf(stream, "%x\r\n", buf->used + strlen(header) + strlen(footer));
    fputs(header, stream);
    fwrite(buf->buf, buf->used, 1, stream);
    fputs(footer, stream);
    fputs("\r\n", stream);
    fflush(stream);
    buf->used = 0;
}

void buffer_append(FILE* stream, buffer_t* buf, char* s, int len)
{
    if(buf->used >= buf->flush_size || buf->used + len >= buf->size)
        buffer_send(stream, buf);
    memcpy(buf->buf + buf->used, s, sizeof(char) * len);
    buf->used += len;
}

void dataset_amalgamate(client_status_t* status, dataset_t* dataset, buffer_t* buf, FILE* stream)
{
    point_t last_p;
    point_t new_p;
    double x_factor;
    double y_factor;
    int in = 0;
    int t;
    int s;
    trajectory_t* tr;
    char str[81920];
    int l = 0;

    x_factor = ((double)(status->frame_bottom.x - status->frame_top.x) / (double)status->view.x);
    y_factor = ((double)(status->frame_bottom.y - status->frame_top.y) / (double)status->view.y);
    
    for(t = 0; t < dataset->n_trajectories && t < 10; t++)
    {
        str[0] = '\0';
        l = 0;
        last_p.x = INT_MIN;
        last_p.y = INT_MIN;
        in = 0;
        tr = &dataset->trajectories[t];
        for(s = 1; s < tr->n_samples; s++)
        {
            new_p.x = (int)((double)(tr->samples[s].x - status->frame_top.x) / x_factor) / status->resolution * status->resolution;
            new_p.y = (int)((double)(tr->samples[s].y - status->frame_top.y) / y_factor) / status->resolution * status->resolution;
            if(new_p.x >= 0 && new_p.x < status->view.x && new_p.y >= 0 && new_p.y < status->view.y)
            {
                if(last_p.x != INT_MIN && in == 0)
                    l += sprintf(str + l, "%d,%d,", last_p.x, last_p.y);
                if(new_p.x != last_p.x || new_p.y != last_p.y)
                    l += sprintf(str + l, "%d,%d,", new_p.x, new_p.y);
                in = 1;
            }
            else
            {
                if(in == 1)
                    l += sprintf(str + l, "%d,%d,", last_p.x, last_p.y);
                in = 0;  
            }
            last_p = new_p;
        }
        l += sprintf(str + l, "\n");
        buffer_append(stream, buf, str, l);
    }
    buffer_send(stream, buf);
}

void dataset_amalgamate2(client_status_t* status, dataset_t* dataset, group_list_t* groups, buffer_t* buf, FILE* stream)
{
    point_t last_p;
    point_t new_p;
    double x_factor;
    double y_factor;
    int in = 0;
    int t;
    int s;
    trajectory_t* tr;
    uint16_t samples[20000];
    uint16_t record[25000];
    uint16_t len;
    int l = 0;
    int g = 0;

    x_factor = ((double)(status->frame_bottom.x - status->frame_top.x) / (double)status->view.x);
    y_factor = ((double)(status->frame_bottom.y - status->frame_top.y) / (double)status->view.y);
  
/*
    str[0] = '\0';
    l = 0;
    l += sprintf(str + l, "%c%c%c%c%c%c%c%c\n", (-1000 & 0xff), (-1000 >> 8), (-200 & 0xff), (-200 >> 8), (400 & 0xff), (400 >> 8), (200 & 0xff), (200 >> 8));
    buffer_append(stream, buf, str, l);
    buffer_send(stream, buf);
    return;
*/
    buffer_append(stream, buf, "CLRS", 4);
#ifdef DEBUG
#undef DEBUG
#endif
    for(g = 0; g < groups->n_groups; g++)
    {
        if(!(status->mask & (1 << g)))
            continue;
        if(groups->groups[g].n_trajectories != 1)
        {
            for(t = 0; t < groups->groups[g].n_trajectories && t < 4; t++)
            {
                len = 0;
                l = 0;
                last_p.x = INT_MIN;
                last_p.y = INT_MIN;
                in = 0;
                tr = &dataset->trajectories[groups->groups[g].trajectories[t]];
                for(s = 1; s < tr->n_samples; s++)
                {
                    new_p.x = (int)((double)(tr->samples[s].x - status->frame_top.x) / x_factor) / status->resolution * status->resolution;
                    new_p.y = (int)((double)(tr->samples[s].y - status->frame_top.y) / y_factor) / status->resolution * status->resolution;
                    if(new_p.x >= 0 && new_p.x < status->view.x && new_p.y >= 0 && new_p.y < status->view.y)
                    {
                        if(last_p.x != INT_MIN && in == 0)
                        {
        #ifdef DEBUG
                            fprintf(stderr, "%d %d, %d %d\n", ((unsigned char*)&last_p.x)[0], ((unsigned char*)&last_p.x)[1], ((unsigned char*)&last_p.y)[0], ((unsigned char*)&last_p.y)[1]);  
        #endif
                            samples[l++] = (uint16_t)last_p.x;
                            samples[l++] = (uint16_t)last_p.y;
                            len++;
                        }
                        if(new_p.x != last_p.x || new_p.y != last_p.y)
                        {
        #ifdef DEBUG
                            fprintf(stderr, "%d %d, %d %d\n", ((unsigned char*)&new_p.x)[0], ((unsigned char*)&new_p.x)[1], ((unsigned char*)&new_p.y)[0], ((unsigned char*)&new_p.y)[1]);  
        #endif
                            samples[l++] = (uint16_t)new_p.x;
                            samples[l++] = (uint16_t)new_p.y;
                            len++;
                        }
                        in = 1;
                    }
                    else
                    {
                        if(in == 1)
                        {
        #ifdef DEBUG
                            fprintf(stderr, "%d %d, %d %d\n", ((unsigned char*)&last_p.x)[0], ((unsigned char*)&last_p.x)[1], ((unsigned char*)&last_p.y)[0], ((unsigned char*)&last_p.y)[1]);
        #endif
                            samples[l++] = (uint16_t)new_p.x;
                            samples[l++] = (uint16_t)new_p.y;
                            len++;
                            memcpy(record, &colors[(g % (sizeof(colors) / 24)) * 3], sizeof(uint16_t) * 3);
                            memcpy(&record[3], &len, sizeof(uint16_t));
                            memcpy(&record[4], samples, len * sizeof(uint16_t) * 2);
                            buffer_append(stream, buf, (char*)record, (len * 2 + 4) * sizeof(uint16_t));
                            /*
                            *((uint16_t*)(lenstr)) = (uint16_t)len;
                            rgb = &colors[(g % (sizeof(colors) / 24)) * 3];
                            
                            buffer_append(stream, buf, (char*)rgb, 6);
                            buffer_append(stream, buf, lenstr, 2);
                            buffer_append(stream, buf, str, l);
                            */
                            len = 0;
                            l = 0;
                        }
                        in = 0;  
                    }
                    last_p = new_p;
                }
                if(in == 1)
                {
                    memcpy(record, &colors[(g % (sizeof(colors) / 24)) * 3], sizeof(uint16_t) * 3);
                    memcpy(&record[3], &len, sizeof(uint16_t));
                    memcpy(&record[4], samples, len * sizeof(uint16_t) * 2);
                    buffer_append(stream, buf, (char*)record, (len * 2 + 4) * sizeof(uint16_t));
                }
            }
        }
    }
    buffer_send(stream, buf);
}

int socket_create(uint16_t port)
{
    int sock;
    struct sockaddr_in addr;

    sock = socket(PF_INET, SOCK_STREAM, 0);
    if(sock < 0)
    {
        perror("Cannot create socket");
        exit(EXIT_FAILURE);
    }
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if(bind(sock, (struct sockaddr *)&addr, sizeof(struct sockaddr_in)) < 0)
    {
        perror("Cannot bind socket");
        exit (EXIT_FAILURE);
    }
    return sock;
}

int client_handle_request(amalgamate_config_t* config, int fd, dataset_t* dataset, group_list_t* groups)
{
    char cmd[256];
    char buf[1024];
    client_status_t status;
    int data_fd = 0;
    FILE* stream = config->streams[fd];

    memset(buf, 0, sizeof(buf));
    fgets(buf, sizeof(buf), stream);
    if(feof(stream))
        return 0;
    while(buf[strlen(buf) - 1] == '\n' || buf[strlen(buf) - 1] == '\r')
        buf[strlen(buf) - 1] = '\0';
    strcpy(cmd, buf);
    while(strlen(buf))
    {
        fprintf(stderr, "Request line: %s\n", buf);
        fgets(buf, sizeof(buf), stream);
        if(feof(stream))
            return 0;
        while(buf[strlen(buf) - 1] == '\n' || buf[strlen(buf) - 1] == '\r')
            buf[strlen(buf) - 1] = '\0';
    }
    fprintf(stderr, "Command: %s\n", cmd);
    if(strncmp(cmd, "POST /data", 10) == 0 || strncmp(cmd, "GET /data", 9) == 0)
    {
        /* setup data channel */
        fprintf(stderr, "Setting up data channel.\n");
        fprintf(config->streams[fd], "HTTP/1.1 200 OK\r\nCache-Control: no-cache\r\nExpires: Thu, 01 Dec 1994 16:00:00 GMT\r\nConnection: Keep-Alive\r\nContent-Type: text/plain; charset=x-user-defined\r\nTransfer-Encoding: chunked\r\nContent-Type: multipart/x-mixed-replace;boundary=\"loldongz101\"\r\n\r\n");
        sprintf(buf, "--loldongz101\r\nContent-type: text/plain\n\n%d\n--loldongz101\r\n", fd);
        fprintf(config->streams[fd], "%x\r\n%s\r\n", strlen(buf), buf);
        /* sprintf(buf, "Content-type: text/plain\n\nReady\n--loldongz101\r\n");
        fprintf(config->streams[fd], "%x\r\n%s\r\n", strlen(buf), buf); */
        fflush(config->streams[fd]);
    }
    else if(strncmp(cmd, "GET /control/html", 17) == 0)
    {
        struct stat stats;
        FILE* in = fopen(config->htmlfile, "r");
        char linebuf[4096];
        if(in != NULL)
        {
            stat(config->htmlfile, &stats);
            fprintf(config->streams[fd], "HTTP/1.1 200 OK\r\nCache-Control: no-cache\r\nConnection: keep-alive\r\nContent-Length: %d\r\nExpires: Thu, 01 Dec 1994 16:00:00 GMT\r\nContent-Type: text/html\r\n\r\n", (int)stats.st_size);
            while(!feof(in))
            {
                fgets(linebuf, sizeof(linebuf), in);
                fwrite(linebuf, strlen(linebuf), 1, config->streams[fd]);
            }
            fclose(in);
            fflush(config->streams[fd]);
        }
        else
            fprintf(stderr, "Cannot open HTML file.\n");
    }
    else if(strncmp(cmd, "POST /control", 13) == 0 || strncmp(cmd, "GET /control", 12) == 0)
    {
        if(strncmp(cmd, "POST /control", 13) == 0)
        {
            sscanf(cmd, "POST /control/%d/%d/%d/%d/%d/%d/%d/%d", &data_fd, &status.view.x, &status.view.y,
                   &status.frame_top.x, &status.frame_top.y, 
                   &status.frame_bottom.x, &status.frame_bottom.y,
                   &status.resolution);
        }
        else
        {
            sscanf(cmd, "GET /control/%d/%d/%d/%d/%d/%d/%d/%d/%d", &data_fd, &status.view.x, &status.view.y,
                   &status.frame_top.x, &status.frame_top.y, 
                   &status.frame_bottom.x, &status.frame_bottom.y,
                   &status.resolution, &status.mask);
        }
        if(data_fd > 0 && data_fd < config->n_streams && config->streams[data_fd] != NULL)
        {
            /* sending ok to control channel */
            fprintf(config->streams[fd], "HTTP/1.1 200 OK\r\nCache-Control: no-cache\r\nConnection: keep-alive\r\nExpires: Thu, 01 Dec 1994 16:00:00 GMT\r\nContent-Length: 3\r\nContent-Type: text/plain\r\n\r\n");
            fprintf(config->streams[fd], "0\r\n");
            fflush(config->streams[fd]);
            /* send data to data channel */
            status_print(&status);
            dataset_amalgamate2(&status, dataset, groups, config->buf, config->streams[data_fd]);
            fflush(config->streams[data_fd]);
        }
        else
        {
            fprintf(stderr, "Data channel %d does not exist.\n", data_fd);
            fprintf(config->streams[fd], "HTTP/1.1 200 OK\r\nCache-Control: no-cache\r\nConnection: Keep-Alive\r\nExpires: Thu, 01 Dec 1994 16:00:00 GMT\r\nContent-Length: 3\r\nContent-Type: text/plain\r\n\r\n");
            fprintf(config->streams[fd], "0\r\n");
            fflush(config->streams[fd]);
        }
    }
    else
    {
        fprintf(config->streams[fd], "HTTP/1.1 404 Not Found\r\nCache-Control: no-cache\r\nConnection: close\r\nExpires: Thu, 01 Dec 1994 16:00:00 GMT\r\nContent-Length: 3\r\nContent-Type: text/plain\r\n\r\n");
        fprintf(config->streams[fd], "0\r\n");
        fflush(config->streams[fd]);
        return 0;
    }
    
    return 1;
}

int main(int argc, char** argv)
{
    amalgamate_config_t config;
    dataset_t* dataset;
    group_list_t* groups = NULL;
    int done = 0;
    int i;
    int client_fd;
    struct sockaddr_in client;
    size_t addr_len = sizeof(struct sockaddr_in);
    fd_set active_fd_set;
    fd_set read_fd_set;
    int server_fd;

    configure(&config, argc, argv);   
    config.buf = buffer_create(16384, 4096);
    dataset = dataset_load(config.datafile);
    groups = group_list_load(config.grpfile);
    if(dataset)
    {
        server_fd = socket_create(config.port);
        if(listen(server_fd, 1) < 0)
        {
            perror("Cannot listen on socket");
            exit(EXIT_FAILURE);
        }
        FD_ZERO(&active_fd_set);
        FD_SET(server_fd, &active_fd_set);
        
        while(!done)
        {
            read_fd_set = active_fd_set;
            if(select(FD_SETSIZE, &read_fd_set, NULL, NULL, NULL) < 0)
            {
                perror("Select failed");
                exit(EXIT_FAILURE);
            }
            
            for(i = 0; i < FD_SETSIZE; ++i)
            {
                if(FD_ISSET(i, &read_fd_set))
                {
                    if(i == server_fd)
                    {
                        client_fd = accept(server_fd, (struct sockaddr*)&client, &addr_len);
                        if(client_fd < 0)
                        {
                            perror("Cannot accept connection");
                            exit(EXIT_FAILURE);
                        }
                        if(client_fd < config.n_streams)
                        {
                            config.streams[client_fd] = fdopen(client_fd, "r+");
                            fprintf(stderr, "Connection from: %s:%hd.\n", inet_ntoa(client.sin_addr), ntohs(client.sin_port));
                            FD_SET(client_fd, &active_fd_set);
                        }
                        else
                        {
                            fprintf(stderr, "Rejecting connection.\n");
                            close(client_fd);
                        }
                    }
                    else
                    {
                        fprintf(stderr, "Handling client\n");
                        if(!client_handle_request(&config, i, dataset, groups))
                        {
                            fprintf(stderr, "Client closes connection.\n");
                            fclose(config.streams[i]);
                            config.streams[i] = NULL;
                            FD_CLR(i, &active_fd_set);
                        }
                    }
                }
            }
        }
        dataset_destroy(&dataset);
    }
    return EXIT_SUCCESS;
}
