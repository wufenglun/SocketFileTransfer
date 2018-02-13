#include <stdio.h>
#include <string.h>
#include "ftree.h"
#include "hash.h"

#include <dirent.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <libgen.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <unistd.h>
#include <arpa/inet.h>

static struct client *addclient(struct client *top, int fd, struct in_addr addr);
static struct client *removeclient(struct client *top, int fd);
//===========================server===============================//
void rcopy_server(unsigned short port) {
    int listenfd;
    int fd;
    struct sockaddr_in peer;
    socklen_t socklen;
    listenfd = setup(port);
    struct client *p;
    struct client *head = NULL;
    struct timeval tv;
    int maxfd, nready;
    fd_set allset;
    fd_set rset;
    int i;
    
    FD_ZERO(&allset);
    FD_SET(listenfd, &allset);
    // maxfd identifies how far into the set to search
    maxfd = listenfd;
    
    while(1) {
        socklen = sizeof(peer);
        rset = allset;
        // timeout in seconds
        tv.tv_sec = 10;
        tv.tv_usec = 0;
        
        nready = select(maxfd + 1, &rset, NULL, NULL, &tv);
        if (nready == 0) {
            printf("No response from clients in %ld seconds\n", tv.tv_sec);
            continue;
        }
        
        if (nready == -1) {
            perror("select");
            continue;
        }
        
        if (FD_ISSET(listenfd, &rset)) {
            
            if ((fd = accept(listenfd, (struct sockaddr *)&peer, &socklen)) < 0) {
                perror("accept");
                exit(1);
            }
            FD_SET(fd, &allset);
            // keeps maxfd always be the max
            if (fd > maxfd) {
                maxfd = fd;
            }
            head = addclient(head, fd, peer.sin_addr);
            printf("New connection on port %d\n", ntohs(peer.sin_port));
        }
        
        // loop over the set and handle each client in the set.
        for(i = 0; i <= maxfd; i++) {
            if (FD_ISSET(i, &rset)) {
                for (p = head; p != NULL; p = p->next) {
                    if (p->fd == i) {
                        int result = handleclient(p);
                        if (result == 1) {
                            int tmp_fd = p->fd;
                            head = removeclient(head, p->fd);
                            FD_CLR(tmp_fd, &allset);
                            close(tmp_fd);
                        }
                        break;
                    }
                }
            }
        }
    }
}

int setup(unsigned short port) {
    int on = 1, status;
    struct sockaddr_in self;
    int listenfd;
    if ((listenfd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
        perror("socket");
        exit(1);
    }
    
    // Make sure we can reuse the port immediately after the
    // server terminates.
    status = setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR,
                        (const char *) &on, sizeof(on));
    if(status == -1) {
        perror("setsockopt -- REUSEADDR");
        exit(1);
    }
    
    self.sin_family = AF_INET;
    self.sin_addr.s_addr = INADDR_ANY;
    self.sin_port = htons(port);
    memset(&self.sin_zero, 0, sizeof(self.sin_zero));  // Initialize sin_zero to 0
    
    printf("Listening on %d\n", port);
    
    if (bind(listenfd, (struct sockaddr *)&self, sizeof(self)) == -1) {
        perror("bind"); // probably means port is in use
        exit(1);
    }
    
    if (listen(listenfd, 5) == -1) {
        perror("listen");
        exit(1);
    }
    return listenfd;
}

int handleclient(struct client *p) {
    // get state
    uint32_t state;
    int len;
    if ((len = read(p->fd, &state, sizeof(uint32_t))) > 0) {
        // socket ok
        state = ntohl(state);
    } else if (len == 0) {
        // socket is closed
        return 1;
    } else { // shouldn't happen
        perror("read");
        return 1;
    }
    
    struct request *file = malloc(sizeof(struct request));
    uint32_t type;
    uint32_t mode;
    uint32_t size;
    int i = 0;
    while (i < 5) {
        if (state == AWAITING_TYPE) {
            // get type from the client
            if (read(p->fd, &type, sizeof(uint32_t)) <= 0) {
                perror("read");
                exit(1);
            }
            file->type = ntohl(type);
            printf("AWAITING_TYPE\n");
        } else if (state == AWAITING_PATH) {
            // get path from the client
            int nread = read(p->fd, file->path, sizeof(file->path));
            if (nread <= 0) {
                perror("read");
                exit(1);
            }
            file->path[nread] = '\0';
            printf("AWAITING_PATH\n");
        } else if (state == AWAITING_PERM) {
            // get mode
            if (read(p->fd, &mode, sizeof(uint32_t)) <= 0) {
                perror("read");
                exit(1);
            }
            file->mode = ntohl(mode);
            printf("AWAITING_PERM\n");
        } else if (state == AWAITING_HASH) {
            // get hash
            if (read(p->fd, file->hash, sizeof(file->hash)) <= 0 ) {
                perror("read");
                exit(1);
            }
            printf("AWAITING_HASH\n");
        } else if (state == AWAITING_SIZE) {
            // get file size
            if (read(p->fd, &size, sizeof(uint32_t)) <= 0) {
                perror("read");
                exit(1);
            }
            file->size = ntohl(size);
            printf("AWAITING_SIZE\n");
        } else {
            printf("ERROR.\n");
            return 1;
        }
        if (i == 4) {
            break;
        }
        if (read(p->fd, &state, sizeof(uint32_t)) <= 0) {
            perror("read");
            exit(1);
        }
        state = ntohl(state);
        i++;
    }
    
    if (file->type == REGFILE) {
        printf("We have a REGFILE: %s.\n", basename(file->path));
        // compare struct
        
        // call stat for comparing size
        int result = SENDFILE;
        struct stat destbuf;
        if (lstat(file->path, &destbuf) == 0) {
            // file exist on dest but is not a reg file, error.
            if (S_ISREG(destbuf.st_mode) == 0) {
                result = ERROR;
            } else {
                FILE *local_f = fopen(file->path, "r");
                // compare size
                if (destbuf.st_size == file->size && local_f != NULL) {
                    // compare hash
                    result = check_hash(hash(local_f), file->hash);
                    fclose(local_f);
                }
            }
        }
        // response
        if (result == SENDFILE) {
            uint32_t feedback = htonl(SENDFILE);
            if (write(p->fd, &feedback, sizeof(uint32_t)) == -1) {
                perror("write");
                exit(1);
            }
        } else if (result == OK) {
            chmod(file->path, file->mode);
            uint32_t feedback = htonl(OK);
            if (write(p->fd, &feedback, sizeof(uint32_t)) == -1) {
                perror("write");
                exit(1);
            }
        } else {
            uint32_t feedback = htonl(ERROR);
            if (write(p->fd, &feedback, sizeof(uint32_t)) == -1) {
                perror("write");
                exit(1);
            }
            fprintf(stderr, "Response REGFILE error.\n");
            exit(1);
        }
    } else if (file->type == REGDIR) {
        printf("We have a REGDIR: %s.\n", basename(file->path));
        struct stat destbuf;
        if (lstat(file->path, &destbuf) == 0) {
            // file exist on dest but is not a dir, error.
            if (S_ISDIR(destbuf.st_mode) == 0) {
                fprintf(stderr, "Response REGDIR error.\n");
                exit(1);
            }
        }
        mkdir(file->path, S_IRWXU | S_IRWXG | S_IRWXO);
        chmod(file->path, file->mode);
    } else if (file->type == TRANSFILE) {
        int result = OK;
        // create a reg file
        printf("We have a TRANSFILE: %s.\n", basename(file->path));
        FILE *f_to_w = fopen(file->path, "wb");
        if (f_to_w == NULL) {
            result = ERROR;
        } else {
            char buffer;
            int nbytes;
            int count = 0;
            // no need to read if size is zero(empty file)
            while (size != 0 && (nbytes = read(p->fd, &buffer, 1)) == 1) {
                fwrite(&buffer, sizeof(char), 1, f_to_w);
                count++;
                // end for loop when write enough btyes
                if (count == file->size) {
                    break;
                }
            } if (nbytes <= 0) {
                perror("read");
                exit(1);
            }
        }
        fclose(f_to_w);
        //response
        if (result == OK) {
            chmod(file->path, file->mode);
            uint32_t response = htonl(OK);
            if (write(p->fd, &response, sizeof(uint32_t)) == -1) {
                perror("write");
                exit(1);
            }
        } else {
            uint32_t response = htonl(ERROR);
            if (write(p->fd, &response, sizeof(uint32_t)) == -1) {
                perror("write");
                exit(1);
            }
            fprintf(stderr, "Response TRANSFILE error.\n");
            exit(1);
        }
    } else {
        printf("Should not get here!\n");
    }
    // free the space allocate for the struct request.
    free(file);
    return 0;
}

static struct client *addclient(struct client *top, int fd, struct in_addr addr) {
    struct client *p = malloc(sizeof(struct client));
    if (!p) {
        perror("malloc");
        exit(1);
    }
    p->fd = fd;
    p->ipaddr = addr;
    p->next = top;
    top = p;
    return top;
}

static struct client *removeclient(struct client *top, int fd) {
    struct client **p;
    
    for (p = &top; *p && (*p)->fd != fd; p = &(*p)->next)
        ;
    // Now, p points to (1) top, or (2) a pointer to another client
    // This avoids a special case for removing the head of the list
    if (*p) {
        struct client *t = (*p)->next;
        free(*p);
        *p = t;
    } else {
        fprintf(stderr, "Trying to remove fd %d, but I don't know about it\n",
                fd);
    }
    return top;
}

//===========================client===============================//
// We find out it is unnecessary to create a struct in the client and then
// use its components to write to server. Instead, we send each piece directly
// to our server and then create a struct in the server and use this struct
// to finish our work.


// position is a global variable lives in ftree.c but its value
// comes from rcopy_client.c Before the position of the path is "useless"
// we need it to get the relative path of each file.
int position = 0;
int rcopy_client(char *source, char *host, unsigned short port) {
    static int soc;
    struct sockaddr_in peer;
    
    if ((soc = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
        perror("rcopy_client: socket");
        return 1;
    }
    peer.sin_family = AF_INET;
    peer.sin_port = htons(port);
    
    if (inet_pton(AF_INET, host, &peer.sin_addr) < 1) {
        perror("rcopy_client: inet_pton");
        close(soc);
        return 1;
    }
    
    if (connect(soc, (struct sockaddr *)&peer, sizeof(peer)) == -1) {
        perror("rcopy_client: connect");
        return 1;
    }
    
    // call stat
    struct stat srcbuf;
    if (lstat(source, &srcbuf)) {
        return 1;
    }
    
    uint32_t state;
    uint32_t type;
    // Source is a regular file
    if (S_ISREG(srcbuf.st_mode)) {
        // transmitting type
        state = htonl(AWAITING_TYPE);
        if (write(soc, &state, sizeof(uint32_t)) == -1) {
            perror("write");
            return 1;
        }
        
        type = htonl(REGFILE);
        if (write(soc, &type, sizeof(uint32_t)) == -1) {
            perror("write");
            return 1;
        }
        
        // transmitting path
        state = htonl(AWAITING_PATH);
        if (write(soc, &state, sizeof(uint32_t)) == -1) {
            perror("write");
            return 1;
        }
        
        char path[MAXPATH];
        strcpy(path, source + position);
        if (write(soc, path, sizeof(path)) == -1) {
            perror("write");
            return 1;
        }
        
        // transmitting mode
        state = htonl(AWAITING_PERM);
        if (write(soc, &state, sizeof(uint32_t)) == -1) {
            perror("write");
            return 1;
        }
        
        uint32_t mode = htonl(srcbuf.st_mode);
        if (write(soc, &mode, sizeof(uint32_t)) == -1) {
            perror("write");
            return 1;
        }
        
        // transmitting hash
        state = htonl(AWAITING_HASH);
        if (write(soc, &state, sizeof(uint32_t)) == -1) {
            perror("write");
            return 1;
        }
        
        FILE *f_hash = fopen(source, "r");
        if (write(soc, hash(f_hash), BLOCKSIZE) == -1) {
            perror("write");
            return 1;
        }
        fclose(f_hash);
        
        //transmitting file size
        state = htonl(AWAITING_SIZE);
        if (write(soc, &state, sizeof(uint32_t)) == -1) {
            perror("write");
            return 1;
        }
        
        uint32_t size;
        size = htonl(srcbuf.st_size);
        if (write(soc, &size, sizeof(uint32_t)) == -1) {
            perror("write");
            return 1;
        }
        
        // get feedback from server
        int feedback, nbytes;
        if ((nbytes = read(soc, &feedback, sizeof(uint32_t))) > 0 ) {
            feedback = ntohl(feedback);
        } else {
            perror("read");
            return 1;
        }
        
        if (feedback == SENDFILE) {
            int r, status;
            r = fork();
            // child process
            if (r == 0) {
                //create a new client
                int soc_send;
                struct sockaddr_in peer_send;
                
                if ((soc_send = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
                    perror("randclient: socket");
                    return 1;
                }
                peer_send.sin_family = AF_INET;
                peer_send.sin_port = htons(port);
                
                if (inet_pton(AF_INET, host, &peer_send.sin_addr) < 1) {
                    perror("randclient: inet_pton");
                    close(soc_send);
                    return 1;
                }
                
                if (connect(soc_send, (struct sockaddr *)&peer_send, sizeof(peer_send)) == -1) {
                    perror("randclient: connect");
                    return 1;
                }
                
                // transmitting type
                state = htonl(AWAITING_TYPE);
                if (write(soc_send, &state, sizeof(uint32_t)) == -1) {
                    perror("write");
                    return 1;
                }
                
                type = htonl(TRANSFILE);
                if (write(soc_send, &type, sizeof(uint32_t)) == -1) {
                    perror("write");
                    return 1;
                }
                
                // transmitting path
                state = htonl(AWAITING_PATH);
                if (write(soc_send, &state, sizeof(uint32_t)) == -1) {
                    perror("write");
                    return 1;
                }
                
                char path[MAXPATH];
                strcpy(path, source + position);
                if (write(soc_send, path, sizeof(path)) == -1) {
                    perror("write");
                    return 1;
                }
                
                // transmitting mode
                state = htonl(AWAITING_PERM);
                if (write(soc_send, &state, sizeof(uint32_t)) == -1) {
                    perror("write");
                    return 1;
                }
                
                uint32_t mode = htonl(srcbuf.st_mode);
                if (write(soc_send, &mode, sizeof(uint32_t)) == -1) {
                    perror("write");
                    return 1;
                }
                
                // transmitting hash
                state = htonl(AWAITING_HASH);
                if (write(soc_send, &state, sizeof(uint32_t)) == -1) {
                    perror("write");
                    return 1;
                }
                
                FILE *f_hash = fopen(source, "r");
                if (write(soc_send, hash(f_hash), BLOCKSIZE) == -1) {
                    perror("write");
                    return 1;
                }
                fclose(f_hash);
                
                // transmitting file size
                state = htonl(AWAITING_SIZE);
                if (write(soc_send, &state, sizeof(uint32_t)) == -1) {
                    perror("write");
                    return 1;
                }
                
                uint32_t size;
                size = htonl(srcbuf.st_size);
                if (write(soc_send, &size, sizeof(uint32_t)) == -1) {
                    perror("write");
                    return 1;
                }
                
                // transimitting file data
                FILE *f_to_r = fopen(source, "rb");
                char buffer;
                while (fread(&buffer, sizeof(char), 1, f_to_r) == 1) {
                    if (write(soc_send, &buffer, 1) == -1) {
                        perror("write");
                        return 1;
                    }
                }
                fclose(f_to_r);
                
                //get response from server to close socket
                uint32_t response;
                if (read(soc_send, &response, sizeof(uint32_t)) <= 0) {
                    perror("read");
                    return 1;
                }
                response = ntohl(response);
                
                if (response == ERROR) {
                    printf("Error when transmitting %s\n", basename(source));
                    close(soc_send);
                    exit(1);
                } else {
                    printf("Finished transmitting %s\n", basename(source));
                    close(soc_send);
                    exit(0);
                }
            // parent process
            } else if (r > 0) {
                if (wait(&status) != -1) {
                    if (WEXITSTATUS(status) == 1) {
                        return 1;
                    }
                } else {
                    perror("wait");
                    return 1;
                }
            } else {
                perror("fork");
                return 1;
            }
        } else if (feedback == OK) {
            printf("OK. No need to copy file\n");
        } else {
            fprintf(stderr, "Feedback: Server says ERROR.\n");
            return 1;
        }
    // Source is a dir
    } else if (S_ISDIR(srcbuf.st_mode)) {
        // transmitting type
        state = htonl(AWAITING_TYPE);
        if (write(soc, &state, sizeof(uint32_t)) == -1) {
            perror("write");
            return 1;
        }
        
        type = htonl(REGDIR);
        if (write(soc, &type, sizeof(uint32_t)) == -1) {
            perror("write");
            return 1;
        }
        
        // transmitting path
        state = htonl(AWAITING_PATH);
        if (write(soc, &state, sizeof(uint32_t)) == -1) {
            perror("write");
            return 1;
        }
        
        char path[MAXPATH];
        strcpy(path, source + position);
        if (write(soc, path, sizeof(path)) == -1) {
            perror("write");
            return 1;
        }
        
        // transmitting mode
        state = htonl(AWAITING_PERM);
        if (write(soc, &state, sizeof(uint32_t)) == -1) {
            perror("write");
            return 1;
        }
        
        uint32_t mode = htonl(srcbuf.st_mode);
        if (write(soc, &mode, sizeof(uint32_t)) == -1) {
            perror("write");
            return 1;
        }
        
        //transmitting hash
        state = htonl(AWAITING_HASH);
        if (write(soc, &state, sizeof(uint32_t)) == -1) {
            perror("write");
            return 1;
        }
        
        char hash[BLOCKSIZE] = {'\0'};
        if (write(soc, hash, sizeof(hash)) == -1) {
            perror("write");
            return 1;
        }
        
        //transmitting file size
        state = htonl(AWAITING_SIZE);
        if (write(soc, &state, sizeof(uint32_t)) == -1) {
            perror("write");
            return 1;
        }
        
        uint32_t size;
        size = htonl(srcbuf.st_size);
        if (write(soc, &size, sizeof(uint32_t)) == -1) {
            perror("write");
            return 1;
        }
        
        DIR *dirp = opendir(source);
        if (dirp == NULL) {
            perror("opendir");
            return 1;
        }
        struct dirent *dp;
        while ((dp = readdir(dirp)) != NULL) {
            if ((dp->d_name)[0] != '.') {
                char *sname = malloc(256);
                strcpy(sname, source);
                strcat(sname, "/");
                strcat(sname, dp->d_name);
                // recursion on those dir or file under this directory.
                rcopy_client(sname, host, port);
            }
        }
        closedir(dirp);
    }
    close(soc);
    return 0;
}
