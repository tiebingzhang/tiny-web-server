#include <arpa/inet.h>          /* inet_ntoa */
#include <signal.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define SORT_DIR 
//to enable debug, define dprintf to printf
#define dprintf 

#ifdef __APPLE__
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE
#include "tunprintf.h"
#else
#define tunprintf printf
#endif
#pragma clang diagnostic ignored "-Wunused-value"
#else
#define tunprintf printf
#include <sys/sendfile.h>
DIR *fdopendir(int fd);
int openat(int dirfd, const char *pathname, int flags);
#endif

#define LISTENQ  1024  /* second argument to listen() */
#define MAXLINE 1024   /* max length of a line */
#define RIO_BUFSIZE 1024

typedef struct {
    int rio_fd;                 /* descriptor for this buf */
    int rio_cnt;                /* unread byte in this buf */
    char *rio_bufptr;           /* next unread byte in this buf */
    char rio_buf[RIO_BUFSIZE];  /* internal buffer */
} rio_t;

/* Simplifies calls to bind(), connect(), and accept() */
typedef struct sockaddr SA;

typedef struct {
    char filename[512];
    uint32_t offset;              /* for support Range */
    uint32_t end;
} http_request;

typedef struct {
    const char *extension;
    const char *mime_type;
} mime_map;

mime_map meme_types [] = {
    {".css", "text/css"},
    {".gif", "image/gif"},
    {".htm", "text/html"},
    {".html", "text/html"},
    {".jpeg", "image/jpeg"},
    {".jpg", "image/jpeg"},
    {".ico", "image/x-icon"},
    {".js", "application/javascript"},
    {".pdf", "application/pdf"},
    {".mp4", "video/mp4"},
    {".png", "image/png"},
    {".svg", "image/svg+xml"},
    {".xml", "text/xml"},
    {NULL, NULL},
};

char *default_mime_type = "text/plain";

void rio_readinitb(rio_t *rp, int fd){
    rp->rio_fd = fd;
    rp->rio_cnt = 0;
    rp->rio_bufptr = rp->rio_buf;
}

ssize_t writen(int fd, void *usrbuf, size_t n){
    size_t nleft = n;
    ssize_t nwritten;
    char *bufp = usrbuf;

    while (nleft > 0){
        if ((nwritten = write(fd, bufp, nleft)) <= 0){
            if (errno == EINTR)  /* interrupted by sig handler return */
                nwritten = 0;    /* and call write() again */
            else
                return -1;       /* errorno set by write() */
        }
        nleft -= nwritten;
        bufp += nwritten;
    }
    return n;
}


/*
 * rio_read - This is a wrapper for the Unix read() function that
 *    transfers min(n, rio_cnt) bytes from an internal buffer to a user
 *    buffer, where n is the number of bytes requested by the user and
 *    rio_cnt is the number of unread bytes in the internal buffer. On
 *    entry, rio_read() refills the internal buffer via a call to
 *    read() if the internal buffer is empty.
 */
/* $begin rio_read */
static ssize_t rio_read(rio_t *rp, char *usrbuf, size_t n){
    int cnt;
    while (rp->rio_cnt <= 0){  /* refill if buf is empty */

        rp->rio_cnt = read(rp->rio_fd, rp->rio_buf,
                           sizeof(rp->rio_buf));
        if (rp->rio_cnt < 0){
            if (errno != EINTR) /* interrupted by sig handler return */
                return -1;
        }
        else if (rp->rio_cnt == 0)  /* EOF */
            return 0;
        else
            rp->rio_bufptr = rp->rio_buf; /* reset buffer ptr */
    }

    /* Copy min(n, rp->rio_cnt) bytes from internal buf to user buf */
    cnt = n;
    if (rp->rio_cnt < n)
        cnt = rp->rio_cnt;
    memcpy(usrbuf, rp->rio_bufptr, cnt);
    rp->rio_bufptr += cnt;
    rp->rio_cnt -= cnt;
    return cnt;
}

/*
 * rio_readlineb - robustly read a text line (buffered)
 */
ssize_t rio_readlineb(rio_t *rp, void *usrbuf, size_t maxlen){
    int n, rc;
    char c, *bufp = usrbuf;

    for (n = 1; n < maxlen; n++){
        if ((rc = rio_read(rp, &c, 1)) == 1){
            *bufp++ = c;
            if (c == '\n')
                break;
        } else if (rc == 0){
            if (n == 1)
                return 0; /* EOF, no data read */
            else
                break;    /* EOF, some data was read */
        } else
            return -1;    /* error */
    }
    *bufp = 0;
    return n;
}

void format_size(char* buf, struct stat *stat){
    if(S_ISDIR(stat->st_mode)){
        sprintf(buf, "%s", "[DIR]");
    } else {
        uint32_t size = stat->st_size;
        if(size < 1024){
            sprintf(buf, "%u", size);
        } else if (size < 1024 * 1024){
            sprintf(buf, "%.1fK", (double)size / 1024);
        } else if (size < 1024 * 1024 * 1024){
            sprintf(buf, "%.1fM", (double)size / 1024 / 1024);
        } else {
            sprintf(buf, "%.1fG", (double)size / 1024 / 1024 / 1024);
        }
    }
}

#ifdef SORT_DIR

#define FILELIST_MAX	128
#define NAMELEN_MAX		320
struct filelist_t{
    char name[NAMELEN_MAX];
    char timestamp[24];
    char size[24];
} filelist[FILELIST_MAX];
static int filelist_count=0;

int filelist_compare (const void * a, const void * b) {
  struct filelist_t *filelistA = (struct filelist_t *)a;
  struct filelist_t *filelistB = (struct filelist_t *)b;

  return (strcmp(filelistB->timestamp, filelistA->timestamp));
}

#endif

void handle_directory_request(int out_fd, int dir_fd, char *filename){
    char buf[MAXLINE], m_time[32], size[16];
    struct stat statbuf;
	dprintf("start handle dir\n");
    sprintf(buf, "HTTP/1.1 200 OK\r\n%s%s%s%s%s",
            "Content-Type: text/html\r\n\r\n",
            "<html><head><style>",
            "body{font-family: monospace; font-size: 13px;}",
            "td {padding: 1.5px 6px;}",
            "</style></head><body><table>\n");
    writen(out_fd, buf, strlen(buf));
	if (strcmp(filename,".")!=0){
		sprintf(buf, "<tr><td><a href='..'>[..]</a></td><td></td><td></td></tr>\n");
		writen(out_fd, buf, strlen(buf));
	}

    DIR *d = fdopendir(dir_fd);
    struct dirent *dp;
    int ffd;
	filelist_count=0;
    while ((dp = readdir(d)) != NULL){
        if(!strcmp(dp->d_name, ".") || !strcmp(dp->d_name, "..")){
            continue;
        }
        if ((ffd = openat(dir_fd, dp->d_name, O_RDONLY)) == -1){
            perror(dp->d_name);
            continue;
        }
        fstat(ffd, &statbuf);
        strftime(m_time, sizeof(m_time),
                 "%Y-%m-%d %H:%M", localtime(&statbuf.st_mtime));
        format_size(size, &statbuf);
        if(S_ISREG(statbuf.st_mode) || S_ISDIR(statbuf.st_mode)){
            char *d = S_ISDIR(statbuf.st_mode) ? "/" : "";
#ifdef SORT_DIR
			snprintf(filelist[filelist_count].name,sizeof(filelist[filelist_count].name),"%s%s",dp->d_name,d);
			snprintf(filelist[filelist_count].timestamp,sizeof(filelist[filelist_count].timestamp),"%s",m_time);
			snprintf(filelist[filelist_count].size,sizeof(filelist[filelist_count].size),"%s",size);
			filelist_count++;
			if (filelist_count>=FILELIST_MAX){
				break;
			}
#else
            sprintf(buf, "<tr><td><a href=\"%s%s\">%s%s</a></td><td>%s</td><td>%s</td></tr>\n",
                    dp->d_name, d, dp->d_name, d, m_time, size);
            writen(out_fd, buf, strlen(buf));
#endif
        }
        close(ffd);
    }
#ifdef SORT_DIR
	qsort (filelist, filelist_count, sizeof(struct filelist_t), filelist_compare);
	int i;
	for (i=0;i<filelist_count;i++){
		sprintf(buf, "<tr><td><a href=\"%s\">%s</a></td><td>%s</td><td>%s</td></tr>\n",
				filelist[i].name, filelist[i].name,filelist[i].timestamp,filelist[i].size);
		writen(out_fd, buf, strlen(buf));
	}
#endif

    sprintf(buf, "</table></body></html>");
    writen(out_fd, buf, strlen(buf));
    closedir(d);
	dprintf("done handle dir\n");
}

static const char* get_mime_type(char *filename){
    char *dot = strrchr(filename, '.');
    if(dot){ // strrchar Locate last occurrence of character in string
        mime_map *map = meme_types;
        while(map->extension){
            if(strcmp(map->extension, dot) == 0){
                return map->mime_type;
            }
            map++;
        }
    }
    return default_mime_type;
}


int open_listenfd(int port){
    int listenfd, optval=1;
    struct sockaddr_in serveraddr;

    /* Create a socket descriptor */
    if ((listenfd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
        return -1;

    /* Eliminates "Address already in use" error from bind. */
    if (setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR,
                   (const void *)&optval , sizeof(int)) < 0)
        return -1;

#ifndef __APPLE__
    // 6 is TCP's protocol number
    // enable this, much faster : 4000 req/s -> 17000 req/s
    if (setsockopt(listenfd, 6, TCP_CORK,
                   (const void *)&optval , sizeof(int)) < 0)
        return -1;
#endif

    /* Listenfd will be an endpoint for all requests to port
       on any IP address for this host */
    memset(&serveraddr, 0, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;
    //serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);
    serveraddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    serveraddr.sin_port = htons((unsigned short)port);
    if (bind(listenfd, (SA *)&serveraddr, sizeof(serveraddr)) < 0)
        return -1;

    /* Make it a listening socket ready to accept connection requests */
    if (listen(listenfd, LISTENQ) < 0)
        return -1;
    return listenfd;
}

void url_decode(char* src, char* dest, int max) {
    char *p = src;
    char code[3] = { 0 };
    while(*p && --max) {
        if(*p == '%') {
            memcpy(code, ++p, 2);
            *dest++ = (char)strtoul(code, NULL, 16);
            p += 2;
        } else {
            *dest++ = *p++;
        }
    }
    *dest = '\0';
}

int parse_request(int fd, http_request *req){
	int ret;
    rio_t rio;
    char buf[MAXLINE], method[MAXLINE], uri[MAXLINE];
    req->offset = 0;
    req->end = 0;              /* default */

	dprintf("parse request\n");
    rio_readinitb(&rio, fd);
    rio_readlineb(&rio, buf, MAXLINE);
    sscanf(buf, "%s %s", method, uri); /* version is not cared */
    /* read all */
    while(buf[0] != '\n' && buf[1] != '\n') { /* \n || \r\n */
        ret=rio_readlineb(&rio, buf, MAXLINE);
		dprintf("ret=%d --buf=%s--\n",ret,buf);
		if (ret<=0){
			return -1;
		}
        if(buf[0] == 'R' && buf[1] == 'a' && buf[2] == 'n'){
            sscanf(buf, "Range: bytes=%u-%u", &req->offset, &req->end);
            // Range: [start, end]
            if( req->end != 0) req->end ++;
        }
    }
    char* filename = uri;
    if(uri[0] == '/'){
        int length;
        filename = uri + 1;
        length = strlen(filename);
        if (length == 0){
            filename = ".";
        } else {
            int i;
            for (i = 0; i < length; ++ i) {
                if (filename[i] == '?') {
                    filename[i] = '\0';
                    break;
                }
            }
        }
    }
    url_decode(filename, req->filename, MAXLINE);
	dprintf("done parse request: filename=%s\n",filename);
	return 0;
}


void log_access(int status, struct sockaddr_in *c_addr, http_request *req){
    printf("%s:%d %d - %s\n", inet_ntoa(c_addr->sin_addr),
           ntohs(c_addr->sin_port), status, req->filename);
}

void client_error(int fd, int status, char *msg, char *longmsg){
    char buf[MAXLINE];
    sprintf(buf, "HTTP/1.1 %d %s\r\n", status, msg);
    sprintf(buf + strlen(buf),
            "Content-length: %lu\r\n\r\n", strlen(longmsg));
    sprintf(buf + strlen(buf), "%s", longmsg);
    writen(fd, buf, strlen(buf));
}

int dosendfile(int out_fd,int in_fd,int sendlen){
	char buf[2048];
	int bytes_left=sendlen, bytes_read;
	while(bytes_left>0){
		bytes_read=read(in_fd,buf,sizeof(buf));
		if (bytes_read<=0){
			break;
		}
		writen(out_fd,buf,bytes_read);
		bytes_left-=bytes_read;
	}
	return 0;
}

void serve_static(int out_fd, int in_fd, http_request *req,
                  uint32_t total_size){
    char buf[256];
    if (req->offset > 0){
        sprintf(buf, "HTTP/1.1 206 Partial\r\n");
        sprintf(buf + strlen(buf), "Content-Range: bytes %u-%u/%u\r\n",
                req->offset, req->end, total_size);
    } else {
        sprintf(buf, "HTTP/1.1 200 OK\r\nAccept-Ranges: bytes\r\n");
    }
    sprintf(buf + strlen(buf), "Cache-Control: no-cache\r\n");
    // sprintf(buf + strlen(buf), "Cache-Control: public, max-age=315360000\r\nExpires: Thu, 31 Dec 2037 23:55:55 GMT\r\n");

    sprintf(buf + strlen(buf), "Content-length: %u\r\n",
            req->end - req->offset);
    sprintf(buf + strlen(buf), "Content-type: %s\r\n\r\n",
            get_mime_type(req->filename));

    writen(out_fd, buf, strlen(buf));
	printf("before sending file offset=%u end=%u\n",req->offset,req->end);
    off_t offset = req->offset; /* copy */
    while(offset < req->end){
		off_t sendlen=req->end - req->offset;
#ifdef __APPLE__
		if(dosendfile(out_fd,in_fd,sendlen)<0)
#else
        if(sendfile(out_fd, in_fd, &offset, sendlen) <= 0) 
#endif
		{
			perror("sendfile");
            break;
        }
        //printf("offset: %d \n\n", offset);
        close(out_fd);
        break;
    }
}

void process(int fd, struct sockaddr_in *clientaddr){
    int status = 200; 
    tunprintf("tiny: accept request, fd is %d, pid is %d\n", fd, getpid());
    http_request req;
    if (parse_request(fd, &req)<0){
		status = 400;
		char *msg = "Unknow Error";
		client_error(fd, status, "Error 1", msg);
		return;
	}

    struct stat sbuf;
	int ffd = open(req.filename, O_RDONLY, 0);
    if(ffd <= 0){
        status = 404;
        char *msg = "File not found";
        client_error(fd, status, "Not found", msg);
    } else {
        fstat(ffd, &sbuf);
        if(S_ISREG(sbuf.st_mode)){
            if (req.end == 0){
                req.end = sbuf.st_size;
            }
            if (req.offset > 0){
                status = 206;
            }
            serve_static(fd, ffd, &req, sbuf.st_size);
        } else if(S_ISDIR(sbuf.st_mode)){
            status = 200;
            handle_directory_request(fd, ffd, req.filename);
        } else {
            status = 400;
            char *msg = "Unknow Error";
            client_error(fd, status, "Error", msg);
        }
        close(ffd);
    }
    log_access(status, clientaddr, &req);
}

#if TARGET_OS_IPHONE
void *tinywebserver(void* arg)
#else
int main(int argc, char** argv)
#endif
{
#if TARGET_OS_IPHONE
	(void *)(arg);
#endif
    struct sockaddr_in clientaddr;
    int default_port = 9999,
        listenfd,
        connfd;
	tunprintf("tiny: starting.\n");
    socklen_t clientlen = sizeof clientaddr;
    listenfd = open_listenfd(default_port);
    if (listenfd > 0) {
        tunprintf("tiny: listen on port %d, fd is %d\n", default_port, listenfd);
    } else {
        perror("ERROR");
        exit(listenfd);
    }
    // Ignore SIGPIPE signal, so if browser cancels the request, it
    // won't kill the whole process.
    signal(SIGPIPE, SIG_IGN);

	while(1){
		connfd = accept(listenfd, (SA *)&clientaddr, &clientlen);
		process(connfd, &clientaddr);
		tunprintf("tiny:done processing\n");
		close(connfd);
	}
}
