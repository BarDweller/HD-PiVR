#define _LARGEFILE_SOURCE
#define _LARGEFILE64_SOURCE
#define _FILE_OFFSET_BITS 64
#define _XOPEN_SOURCE
#define _GNU_SOURCE
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <stdio.h>
#include <netinet/in.h>
#include <resolv.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/poll.h>
#include <signal.h>
#include <assert.h>

//size to read/write from hd pvr at.. 4k seems ok.
#define MEMBUF_SIZE 4096

//how long to wait before giving up and declaring device needs a restart.
#define POLL_DELAY 2500

//port to listen on.
#define PORT 1101

//max buffers to use in buffered writer, each buffer is MEMBUF_SIZE bytes.
#define MAX_BUFFERS 8192 //4096 = 16mb, 8192 = 32mb, 12288 = 48mb

//max no of streaming clients.. pi can handle about 5
#define MAX_BUFFERED_OUTPUTS 4

//no of historical buffer counts to remember..
#define MAX_BUFFER_COUNT_HISTORY 300

//thread prototype for the connected children.
void *SocketHandler(void*); //handles accepted sockets
void *StreamHandler(void*); //handles reading from hdpvr and writing to outputs
void *DiskIOHandler(void*); //buffered writer, writes data to disk.
void *BufferCleanUp(void*); //cleans up buffers if they remain free for 5 seconds
void *EventHandling(void*); //launches events when they are due.


#define EVENT_TYPE_ECHO 0
#define EVENT_TYPE_STARTREC 1
#define EVENT_TYPE_STOPREC 2
#define EVENT_TYPE_CHANNEL 3
//event
struct event {
    time_t eventTime;
    int type;
    char *internalId;
    char *externalId;
    char *data;
    struct event *next;
};

//data we'll share between all the threads.
struct shared {
    pthread_mutex_t sharedlock; // used to protect the fd array, and recording count
    int outfds[MAX_BUFFERED_OUTPUTS];
    char *outfdIntIds[MAX_BUFFERED_OUTPUTS];
    char *outfdExtIds[MAX_BUFFERED_OUTPUTS];
    char buffer[80];
    int recording;//no of fd's we are writing to.

    pthread_mutex_t diskiolock; //used to protect the data buffers and freebuffer arrays, and counts.
    int buffercount;
    int freecount;
    char **data;
    char **freebuffers;

    int historyFree[MAX_BUFFER_COUNT_HISTORY];
    int historyUsed[MAX_BUFFER_COUNT_HISTORY];
    int historyNowIndex;

    pthread_mutex_t eventLock;
    struct event *events;
};
char *UIIntID = "UI.Int.Id";
char *UIExtID = "UI.Ext.Id";
char *SocketIntED = "Sock.Int.Id";
char *SocketExtED = "Sock.Ext.Id";

//data we'll give to each thread uniquely.
struct data {
    int csock;
    struct shared *data; //pointer to shared data.
};

//http post..
struct post_data {
    int noOfKeys;
    char **keys;
    char **values;
};
struct post_data *getPostData(char *buffer);
char *getValue(char *key, struct post_data *data);

int main(int argv, char** argc) {
    signal(SIGPIPE, SIG_IGN);

    int host_port=PORT;

    struct sockaddr_in my_addr;

    int hsock;
    int * p_int ;

    socklen_t addr_size = 0;
    struct sockaddr_in sadr;
    pthread_t thread_id=0;

    //init the global data.
    struct shared *global = (struct shared*)malloc(sizeof(struct shared));
    global->recording=0;

    global->freebuffers = (char **)calloc(MAX_BUFFERS, sizeof(char *));
    global->data = (char **)calloc(MAX_BUFFERS, sizeof(char *));
    global->buffercount=0;
    global->freecount=0;
    memset(global->historyFree,0,MAX_BUFFER_COUNT_HISTORY*sizeof(int));
    memset(global->historyUsed,0,MAX_BUFFER_COUNT_HISTORY*sizeof(int));
    global->historyNowIndex=0;

    pthread_mutex_init(&(global->sharedlock), NULL);
    pthread_mutex_init(&(global->diskiolock), NULL);
    pthread_mutex_init(&(global->eventLock), NULL);

    //start the disk writing thread, it will sleep until data is ready to write
    pthread_create(&thread_id,0,&DiskIOHandler, (void*)global );
    pthread_detach(thread_id);

    //start the device read/write thread, it will sleep until there are clients to write to.
    pthread_create(&thread_id,0,&StreamHandler, (void*)global );
    pthread_detach(thread_id);

    //start the buffer cleanup thread.. it'll only kick in when the buffers exceed > 1024
    pthread_create(&thread_id,0,&BufferCleanUp, (void*)global );
    pthread_detach(thread_id);

    //start the event handling thread..
    pthread_create(&thread_id,0,&EventHandling, (void*)global );
    pthread_detach(thread_id);

    struct data *datainst;

    hsock = socket(AF_INET, SOCK_STREAM, 0);
    if(hsock == -1) {
        printf("Error initializing socket %d\n", errno);
        goto FINISH;
    }

    p_int = (int*)malloc(sizeof(int));
    *p_int = 1;

    if( (setsockopt(hsock, SOL_SOCKET, SO_REUSEADDR, (char*)p_int, sizeof(int)) == -1 )||
            (setsockopt(hsock, SOL_SOCKET, SO_KEEPALIVE, (char*)p_int, sizeof(int)) == -1 ) ) {
        printf("Error setting options %d\n", errno);
        free(p_int);
        goto FINISH;
    }
    free(p_int);

    my_addr.sin_family = AF_INET ;
    my_addr.sin_port = htons(host_port);

    memset(&(my_addr.sin_zero), 0, 8);
    my_addr.sin_addr.s_addr = INADDR_ANY ;

    if( bind( hsock, (struct sockaddr*)&my_addr, sizeof(my_addr)) == -1 ) {
        fprintf(stderr,"Error binding to socket, make sure nothing else is listening on this port %d\n",errno);
        goto FINISH;
    }
    if(listen( hsock, 10) == -1 ) {
        fprintf(stderr, "Error listening %d\n",errno);
        goto FINISH;
    }

    //Now lets do the server stuff
    addr_size = sizeof(struct sockaddr_in);
    while(1) {
        char tbuf[80];
        time_t timer;
        struct tm* tm_info;
        time(&timer);
        tm_info = localtime(&timer);
        strftime(tbuf,80,"%Y-%m-%d %H:%M:%S", tm_info);
        //printf("%s Waiting for a connection on port %d \n", tbuf, PORT);
        //allocate the block we'll send to the thread
        datainst = (struct data *)malloc(sizeof(struct data));
        //hook up our global shared data struct..
        datainst->data = global;
        //get the socket, store into struct.
        if((datainst->csock = accept( hsock, (struct sockaddr*)&sadr, &addr_size))!= -1) {
            time(&timer);
            tm_info = localtime(&timer);
            strftime(tbuf,80,"%Y-%m-%d %H:%M:%S", tm_info);
            pthread_mutex_lock(&(global->sharedlock));
            //printf("%s Received connection from %s\n",tbuf,inet_ntoa(sadr.sin_addr));
            pthread_mutex_unlock(&(global->sharedlock));
            pthread_create(&thread_id,0,&SocketHandler, (void*)datainst );
            pthread_detach(thread_id);
        } else {
            fprintf(stderr, "Error accepting %d\n", errno);
        }
    }

FINISH:
    free(global);
    return 0;
}

int initiateRecording(struct shared *global, int outfd, char* intid, char* extid);
int stopRecording(struct shared *global, char* intid, char* extid);

void *EventHandling(void* lp) {
    struct shared *global = (struct shared *)lp;
    struct event *current;
    double done=0;

    while(1){
        sleep(1);
        //obtain the event to process.. if any.. with lock!.
        pthread_mutex_lock(&(global->eventLock));
        if(global->events!=NULL){
            time_t now;
            time(&now);
            current = global->events;

            done=difftime(current->eventTime,now);

            while(done<0){
                //process the event.
                if(current->type==EVENT_TYPE_ECHO){
                    printf("EVENT: %s\n",current->data);
          		if(current->externalId!=UIExtID && current->externalId!=SocketExtED){
        				free(current->externalId);
        			}
        			if(current->internalId!=UIIntID && current->internalId!=SocketIntED){
        				free(current->internalId);
        			}
                }
                else if(current->type==EVENT_TYPE_STARTREC){
					int outfd;
                	char *filename = NULL;

                	if(current->data!=NULL){
                		int filenamelen = strlen(current->data) - (strstr(current->data,"?") - current->data);
                		filename = (char*)calloc(filenamelen+1, sizeof(char));
                		strncpy(filename,strstr(current->data,"?")+1,filenamelen);
                	}
                	if(filename==NULL){
						char *tbuf = (char *)calloc(80,sizeof(char));
						time_t timer;
						struct tm* tm_info;
						time(&timer);
						tm_info = localtime(&timer);
						strftime(tbuf,80,"%Y%m%d%H%M%S.ts", tm_info);
						filename = tbuf;
                	}
                	printf("Scheduled recording request...");
                    /** open the output file **/
                    if(-1 == (outfd = open(filename, O_CREAT | O_RDWR, S_IRWXU | S_IRWXG))) {
                        perror("Unable to open output file");
                    } else {
                        int started=initiateRecording(global,outfd,current->internalId,current->externalId);
                        if(!started) {
                            printf("Already recording, new request ignored\n");
                        } else {
                            printf("Recording now in progress\n");
                        }
                    }

                    free(filename);
                }
                else if(current->type==EVENT_TYPE_STOPREC){
                	printf("Scheduled stop recording request...");
                	int stopped = stopRecording(global,current->internalId,current->externalId);
                    if(!stopped) {
                        printf("Recording not in progress, stop request ignored\n");
                    } else {
                        printf("Recording stopped.\n");
                    }
                }
                else if(current->type==EVENT_TYPE_CHANNEL){
                	//Not implemented yet!
        			if(current->externalId!=UIExtID && current->externalId!=SocketExtED){
        				free(current->externalId);
        			}
        			if(current->internalId!=UIIntID && current->internalId!=SocketIntED){
        				free(current->internalId);
        			}
                }

                //event has been processed, remove event from queue.
                if(current->next!=NULL){
                    //update the global list.
                    global->events = current->next;
                    //free up the event.
                    free(current->data);
                    //free up the ids
        			if(current->externalId!=UIExtID && current->externalId!=SocketExtED){
        				free(current->externalId);
        			}
        			if(current->internalId!=UIIntID && current->internalId!=SocketIntED){
        				free(current->internalId);
        			}
                    //free the event struct itself.
                    free(current);
                    //move to the next event.
                    current=global->events;
                    done=difftime(current->eventTime,now);
                }else{
                    //no next event.. exit while loop.. sleep for a second.
                    done=0;
                    global->events = NULL;
                    free(current->data);
                    free(current);
                }
            }
        }
        pthread_mutex_unlock(&(global->eventLock));
    }
    return 0;
}

void *BufferCleanUp(void* lp) {
    struct shared *global = (struct shared *)lp;

    int freeloop=0;
    int freecounts[8] = {0,0,0,0,
    		             0,0,0,0};
    int currentFreeCount=0;

    //obtain the initial counts via the appropriate locks.
    pthread_mutex_lock(&(global->diskiolock));
    freecounts[0] = global->freecount;
    pthread_mutex_unlock(&(global->diskiolock));

    while(1){
        usleep(50000);
        currentFreeCount++;
        if(currentFreeCount>7){
            currentFreeCount=0;
        }
        global->historyNowIndex++;
        if(global->historyNowIndex >= MAX_BUFFER_COUNT_HISTORY){
            global->historyNowIndex = 0;
        }

        //obtain the initial counts via the appropriate locks.
        pthread_mutex_lock(&(global->diskiolock));
        freecounts[currentFreeCount] = global->freecount;
        global->historyFree[global->historyNowIndex] = global->freecount;
        global->historyUsed[global->historyNowIndex] = global->buffercount;

        //cheap way of checking all counts are over 1024.
        if(  (freecounts[0] >> 10)
           &&(freecounts[1] >> 10)
           &&(freecounts[2] >> 10)
           &&(freecounts[3] >> 10)
           &&(freecounts[4] >> 10)
           &&(freecounts[5] >> 10)
           &&(freecounts[6] >> 10)
           &&(freecounts[7] >> 10) ){
                //if we've been over 1024 for the last 8 loops.. we remove 512 buffers.
                //we'll then keep dropping 512 every loop while the count remains above 1024
                for(freeloop=0; freeloop<512; freeloop++){
                    char *buf = global->freebuffers[global->freecount-1];
                    free(buf);
                    global->freecount--;
                }
        }
        pthread_mutex_unlock(&(global->diskiolock));
    }
    return 0;
}

//ONLY CALL WHILE HOLDING global->sharedLock
void collapseFdArrays(struct shared *global){
	int outfdcount;
    int writepos=0;
    int currentmax=global->recording;
    for(outfdcount=0; outfdcount<currentmax; outfdcount++) {
        if(global->outfds[outfdcount] != -1) {
            if(writepos!=outfdcount) {
                //move the data back to the writepos slot, and set self to -1..
                global->outfds[writepos] = global->outfds[outfdcount];
                global->outfdIntIds[writepos] = global->outfdIntIds[outfdcount];
                global->outfdExtIds[writepos] = global->outfdExtIds[outfdcount];
                global->outfds[outfdcount] = -1;
            }
            writepos++;
        } else {
            global->recording--;
        }
    }
}

void sendBufferToFds(char *buffer, struct shared *global){
    pthread_mutex_lock(&(global->sharedlock));
    int outfdcount;
    //iterate over the output fd's.. set them to -1 if they fail to write.
    for(outfdcount=0; outfdcount<(global->recording); outfdcount++) {
        if(global->outfds[outfdcount]!=-1) {
            ssize_t written = write(global->outfds[outfdcount], buffer, MEMBUF_SIZE);
            if(written==-1) {
                global->outfds[outfdcount]=-1;
            } else {
                fsync(global->outfds[outfdcount]);
            }
        }
    }

    //we're still holding the global lock.. so we can manipulate the recording count,
    //and move the contents of the outfds array around without fear of a new client corrupting us.

    //iterate over the outputfd's.. collapsing the array to move the valids to the front.
    collapseFdArrays(global);

    pthread_mutex_unlock(&(global->sharedlock));
}

void sendBuffersToFds(int buffersToWrite, struct shared *global){
    pthread_mutex_lock(&(global->sharedlock));
    int outfdcount;
    int buffercount;
    //iterate over the output fd's.. set them to -1 if they fail to write.
    for(outfdcount=0; outfdcount<(global->recording); outfdcount++) {
        if(global->outfds[outfdcount]!=-1) {
        	for(buffercount=0; buffercount<buffersToWrite; buffercount++){
				ssize_t written = write(global->outfds[outfdcount], global->data[buffercount], MEMBUF_SIZE);
				if(written==-1) {
					global->outfds[outfdcount]=-1;
				}
        	}
        }
    }
    //now issue the flushes.
    for(outfdcount=0; outfdcount<(global->recording); outfdcount++) {
        if(global->outfds[outfdcount]!=-1) {
			fsync(global->outfds[outfdcount]);
        }
    }

    //we're still holding the global lock.. so we can manipulate the recording count,
    //and move the contents of the outfds array around without fear of a new client corrupting us.

    //iterate over the outputfd's.. collapsing the array to move the valids to the front.
    collapseFdArrays(global);

    pthread_mutex_unlock(&(global->sharedlock));
}

void *DiskIOHandler(void* lp) {
    struct shared *global = (struct shared *)lp;

    int bufferstowrite=0;
    int enabled=0;

    //obtain the initial counts via the appropriate locks.
    pthread_mutex_lock(&(global->diskiolock));
    bufferstowrite = global->buffercount;
    pthread_mutex_unlock(&(global->diskiolock));

    pthread_mutex_lock(&(global->sharedlock));
    enabled = global->recording;
    pthread_mutex_unlock(&(global->sharedlock));

    while(1){
        //if we're not enabled yet, sleep a while, then reget the flag inside the appropriate lock.
        while(!enabled) {
            usleep(100);

            pthread_mutex_lock(&(global->diskiolock));
            bufferstowrite = global->buffercount;
            pthread_mutex_unlock(&(global->diskiolock));

            pthread_mutex_lock(&(global->sharedlock));
            enabled = global->recording;
            pthread_mutex_unlock(&(global->sharedlock));
        }

        //we're now enabled.. until we're not ;p
        while(enabled || bufferstowrite>0){
            //update the enabled flag from inside the lock
            pthread_mutex_lock(&(global->sharedlock));
            enabled = global->recording;
            pthread_mutex_unlock(&(global->sharedlock));

            //grab the current buffers to write value.
            //we release the lock, as only we ever remove buffers,
            //so we can allow new buffers to be added while we write the ones we have.
            pthread_mutex_lock(&(global->diskiolock));
            bufferstowrite = global->buffercount;
            pthread_mutex_unlock(&(global->diskiolock));

            //write thread might get ahead of read thread..
            if(bufferstowrite==0){
                if(enabled){
                    //just means we're still enabled, but no buffers have been added yet..
                    //wait a bit.. maybe more buffers will come =)
                    sleep(1);
                }
            }else{
                //we have buffers to write.. write out as many as we know there are
                //(there may be more to write by now.. if so we deal with them next loop)
            	sendBuffersToFds(bufferstowrite,global);

                //we've written buffers..
                // so now we move the buffers still to write to the front of the write array
                // and move the buffers we've written across to the end of the free array

                //lock to protect the arrays while we nobble the data.
                pthread_mutex_lock(&(global->diskiolock));

                //reduce by no of buffers written
                global->buffercount -= bufferstowrite;

                //move the current buffers onto the end of the free array where they can be reused.
                memcpy(&(global->freebuffers[global->freecount]), global->data, (bufferstowrite * sizeof(char *)));
                global->freecount += bufferstowrite;

                //if buffers were incremented while we were writing.. move the pointers along a bit
                //so the reads can process from the start again..
                if(global->buffercount >0 ){
                    memcpy(global->data, &(global->data[bufferstowrite]), global->buffercount * sizeof(char *) );
                }

                //all done =) free array is added to, write array removed from, counts adjusted, release lock.
                pthread_mutex_unlock(&(global->diskiolock));
            }
        }
    }
    return 0;
}

//Device read thread.. pulls data from device, writes it to sockets / buffered writer.
void *StreamHandler(void* lp) {
    struct shared *global = (struct shared *)lp;
    int devfd;
    char *ifname;
    struct pollfd fds[2];
    void *membuf;
    ssize_t nbytes;
    time_t timer;
    char buffer[80];
    struct tm* tm_info;
    int retval;

    ifname = "/dev/video0"; //TODO: make this an arg ;p
    if(NULL == (membuf = malloc(MEMBUF_SIZE))) {
        printf("Not enough memory to allocate buffer\n");
        fprintf(stderr, "Not enough memory\n");
        exit(EXIT_FAILURE);
    }
    while(1) {
        int enabled=0;
        pthread_mutex_lock(&(global->sharedlock));
        enabled = global->recording;
        pthread_mutex_unlock(&(global->sharedlock));
        while(!enabled) {
            sleep(1);
            pthread_mutex_lock(&(global->sharedlock));
            enabled = global->recording;
            pthread_mutex_unlock(&(global->sharedlock));
        }

        //someone enabled the datarelay, we'd better setup & start reading data.
        /** open the device **/
        if(-1 == (devfd = open(ifname, O_RDWR | O_NONBLOCK))) {
            perror("Unable to open device");
            exit(EXIT_FAILURE);
        }
        usleep(5000);

        /** setup descriptors for event polling **/
        fds[0].fd = STDIN_FILENO;
        fds[0].events = POLLIN;
        fds[1].fd = devfd;
        fds[1].events = POLLIN;

        /** start the recording loop **/
        int countdown=0;
        while(enabled) {
            pthread_mutex_lock(&(global->sharedlock));
            enabled = global->recording;
            pthread_mutex_unlock(&(global->sharedlock));

            while(countdown>0) {
                retval = poll(fds, 2, POLL_DELAY);
                if(0 == retval) {
                    time(&timer);
                    tm_info = localtime(&timer);
                    strftime(buffer,80,"%Y-%m-%d %H:%M:%S", tm_info);
                    fprintf(stderr, "%s  Waiting for ready (%d)...\n", buffer, countdown);
                    usleep(300);
                    countdown--;
                } else {
                    countdown=0;
                }
            }
            retval = poll(fds, 2, POLL_DELAY);
            if(0 == retval) {
                time(&timer);
                tm_info = localtime(&timer);
                strftime(buffer,80,"%Y-%m-%d %H:%M:%S", tm_info);
                fprintf(stderr, "%s  Lost signal, restarting device...\n",buffer);
                close(devfd);
                countdown=5;
                if(-1 == (devfd = open(ifname, O_RDWR | O_NONBLOCK))) {
                    perror("Unable to reopen the device");
                    exit(EXIT_FAILURE);
                } else {
                    fds[1].fd = devfd;
                    fds[1].events = POLLIN;
                    fprintf(stderr,"%s Device reaquired. Usleep for 5k for data\n",buffer);
                    usleep(5000);
                    continue;
                }
            } else if(-1 == retval) {
                printf("polling failed\n");
                perror("Polling failed");
                break;
            } else if(fds[0].revents & POLLIN) {
                //printf("user quit\n");
                //fprintf(stderr, "User quit\n");
                break;
            } else if(fds[1].revents & POLLIN) {
                nbytes = read(devfd, membuf, MEMBUF_SIZE);
                if(0 > nbytes) {
                    switch(errno) {
                    case EINTR:
                    case EAGAIN:{
                        usleep(2500);
                        continue;
                    }
                    default:
                        printf("Unknown errno response %d when reading device\n",errno);
                        perror("Unknown");
                        exit(EXIT_FAILURE);
                    }
                } else if(MEMBUF_SIZE == nbytes) {
                    int recording=0;
                    pthread_mutex_lock(&(global->sharedlock));
                    recording=global->recording;
                    pthread_mutex_unlock(&(global->sharedlock));

                    //if recording.. write out to outfd.
                    if(recording){
                        //we're about to alter the write/free buffer arrays, so we need this lock.
                        pthread_mutex_lock(&(global->diskiolock));

                        char *bufToUse = NULL;
                        //do we have space?
                        if(!(global->buffercount<MAX_BUFFERS)){
                            //this will be a file corruption scenario, as the buffer MAY already be being written from.
                            //plus the data in this last buffer is overwritten and lost forever.
                            //in an ideal world, the write thread will catch up, and start freeing buffers,
                            //and we'll just lose a chunk of the ts.
                            perror("Out of write buffers!! - reusing last buffer.. ");
                            bufToUse = global->data[(global->buffercount)-1];
                            //reduce by 1, as later we re-increment it.
                            global->buffercount--;
                        }

                        //any buffers we can reuse?
                        if(bufToUse==NULL && global->freecount>0){
                            global->data[global->buffercount] = global->freebuffers[global->freecount -1];
                            bufToUse = global->data[global->buffercount];
                            //this buffer will no longer be free..
                            global->freecount--;
                        }

                        //no buffer yet, but if we have space, we can make one..
                        if(bufToUse==NULL && (global->buffercount + global->freecount)<MAX_BUFFERS){
                            if(NULL == (bufToUse = malloc(MEMBUF_SIZE))) {
                                printf("Not enough memory to allocate buffer (%d)\n",(global->freecount+global->buffercount));
                                fprintf(stderr, "Not enough memory\n");
                                exit(EXIT_FAILURE);
                            }
                            global->data[global->buffercount] = bufToUse;
                        }

                        //at this stage, we pretty much have to have a buffer.. right?
                        assert(bufToUse!=NULL);

                        //copy the data from the read buffer to the selected write buffer.
                        memcpy(bufToUse, membuf, MEMBUF_SIZE);

                        //bump the write counter, to say theres a new buffer to write.
                        global->buffercount++;

                        //all done playing with the buffers.. release the lock.
                        pthread_mutex_unlock(&(global->diskiolock));
                    }
                    continue;
                } else {
                    printf("Short read\n");
                    perror("Short read");
                    exit(EXIT_FAILURE);
                }
            } else if(fds[1].revents & POLLERR) {
                printf("Pollerr\n");
                perror("pollerr");
                exit(EXIT_FAILURE);
                break;
            }
        }
        /** clean up **/
        close(devfd);
    }

    free(membuf);
    return 0;
}

struct post_data *getPostData(char *buffer){
    //find \n\n
    char *post=NULL;
    post=strstr(buffer,"\r\n\r\n") + 4;
    if(post==NULL){
        post = strstr(buffer,"\n\n") + 2;
    }
    struct post_data *data = (struct post_data *)calloc(1,sizeof(struct post_data));
    if(post==NULL){
        data->noOfKeys=0;
        return data;
    }
    int postidx = post-buffer;

    //how many ampersands are there..
    int ampcount=1;
    int idx=postidx;
    while(buffer[idx]!=0){
        if(buffer[idx]=='&'){
            ampcount++;
        }
        idx++;
    }
    data->keys = (char **)calloc(ampcount,sizeof(char *));
    data->values = (char **)calloc(ampcount,sizeof(char *));

    idx=0;
    //parse next string with &'s
    char *keyvalue=strtok(post,"&");
    int i=0;
    while(keyvalue!=NULL){
        data->keys[idx]=keyvalue;
        for(i=0;i<strlen(keyvalue);i++){
            if(keyvalue[i]=='='){
                keyvalue[i]=0;
                data->values[idx]=&keyvalue[i+1];
            }
        }
        keyvalue=strtok(NULL,"&");
        idx++;
    }
    data->noOfKeys = idx;
    return data;
}
char *getValue(char *key, struct post_data *data){
    int loop=0;
    for(loop=0; loop<data->noOfKeys;loop++){
        if(strcmp(key,data->keys[loop])==0){
            return data->values[loop];
        }
    }
    return NULL;
}
void dumpKeys(struct post_data *data){
    printf("No of Keys %d\n",data->noOfKeys);
    int i;
    for(i=0; i<data->noOfKeys; i++){
        printf(" key: %s\n",data->keys[i]);
        printf(" val: %s\n",data->values[i]);
        printf("getvalue : %s\n",getValue(data->keys[i],data));
    }
    printf("fish : %s\n",getValue("fish",data));
    printf("fred : %s\n",getValue("fred",data));
}

int getIndex(char *needle, char *haystack){
    char *found = strstr(haystack,needle);
    if(found!=NULL){
        return found-haystack;
    }else{
        return -1;
    }
}

void printTimestamp(){
    char tbuf[80];
    time_t timer;
    struct tm* tm_info;
    memset(tbuf,80,0);
    memset(&timer,sizeof(time_t),0);
    time(&timer);
    tm_info = localtime(&timer);
    strftime(tbuf,80,"%Y-%m-%d %H:%M:%S", tm_info);
    printf("%s ",tbuf);
}

void return404(char *buffer, int csock, char *reason){
    char *eol = strstr(buffer,"\n");
    const int maxlen=1024;
    char output[maxlen];
    memset(output,maxlen,0);
    if(eol==NULL || ((eol-buffer)>(maxlen-1))){
        strncpy(output,buffer,maxlen-1);
    }else{
        strncpy(output,buffer,(eol-buffer));
    }
    char *notfound = "HTTP/1.0 404 Not Found\nContent-Type: text/plain\n\nError: cannot handle request ";
    send(csock,notfound,strlen(notfound),0);
    send(csock,output,strlen(output),0);
    if(reason!=NULL){
        send(csock,"\n",1,0);
        send(csock,reason,strlen(reason),0);
    }
    fsync(csock);
    close(csock);
}

void processFileRequest(char *buffer, int csock){
    int fd;
    int nbytes;
    const int fbufsize=4096;
    char fbuffer[4096];
    char outbuf[1024];

    //we know the request started with.. "GET /web/"
    //so trim that off, and find the ending and turn the space before HTTP/1.X into a 0
    char *file = buffer + strlen("GET /web/");
    char* http = strstr(buffer," HTTP/1.");

    if(http==NULL){
        return404(buffer,csock,"File request missing trailing HTTP marker");
        return;
    }else{
        http[0] = 0;

        if(strstr(file,"..")){
            return404(buffer,csock,"File request contained unsupported use of ..");
            return;
        }
        //trim off any ? data.
        if(strstr(file,"?")){
            char *q = strstr(file,"?");
            q[0]=0;
        }

        printTimestamp();
        printf("Serving filename of %s\n",file);

        fd = open(file,O_RDONLY);
        if(fd==-1){
            return404(buffer,csock,"Problem opening file for reading.");
            return;
        }

        char *found = "HTTP/1.0 200 OK\n";
        send(csock,found,strlen(found),0);

        off_t len = lseek(fd,0,SEEK_END);
        lseek(fd,0,SEEK_SET);
        memset(outbuf,0,1024);
        snprintf(outbuf,1023,"Content-Length: %d\n\n",(int)len);
        send(csock,outbuf,strlen(outbuf),0);

        nbytes = read(fd, fbuffer, fbufsize);
        while(nbytes!=0){
            if(nbytes==-1)
            {
                close(fd);
                return404(buffer,csock,"\n\n!!!Problem reading file");
                return;
            }
            send(csock,fbuffer,nbytes,0);
            fsync(csock);
            nbytes = read(fd, fbuffer, fbufsize);
        }
        close(fd);
        fsync(csock);
        close(csock);
    }

}

void processStatusRequest(char *buffer, int csock, struct shared *global){
        char *text="HTTP/1.0 200 OK\nContent-Type: text/plain\n\nStatus at : ";
        send(csock,text,strlen(text),0);
        int idx;
        char tbuf[80];
        time_t timer;
        struct tm* tm_info;
        time(&timer);
        tm_info = localtime(&timer);
        strftime(tbuf,80,"%Y-%m-%d %H:%M:%S", tm_info);
        send(csock,tbuf,strlen(tbuf),0);

        printf("%s Status request...\n",tbuf);

        int freebuffers = 0;
        int databuffers = 0;
        int noofconnections = 0;
        int recording = 0;
        pthread_mutex_lock(&(global->sharedlock));
        noofconnections = global->recording;
        recording = 0;
        for(idx=0; idx<global->recording; idx++){
        	if(strcmp(global->outfdIntIds[idx],SocketIntED)!=0){
        		recording = 1;
        		break;
        	}
        }
        pthread_mutex_unlock(&(global->sharedlock));
        pthread_mutex_lock(&(global->diskiolock));
        freebuffers = global->freecount;
        databuffers = global->buffercount;
        pthread_mutex_unlock(&(global->diskiolock));

        snprintf(tbuf,80,"\nNo of Connections : %d\n",noofconnections);
        send(csock,tbuf,strlen(tbuf),0);
        snprintf(tbuf,80,  "Recording?        : %d\n",recording);
        send(csock,tbuf,strlen(tbuf),0);
        snprintf(tbuf,80,  "Free Buffer Count : %d\n",freebuffers);
        send(csock,tbuf,strlen(tbuf),0);
        snprintf(tbuf,80,  "Data Buffer Count : %d\n",databuffers);
        send(csock,tbuf,strlen(tbuf),0);
        snprintf(tbuf,80,  "Total Buffer Usage: %d\n",databuffers+freebuffers);
        send(csock,tbuf,strlen(tbuf),0);

        pthread_mutex_lock(&(global->sharedlock));
        for(idx=0; idx<global->recording; idx++){
        	snprintf(tbuf,80,  "Client %d: %s / %s\n",idx,global->outfdIntIds[idx],global->outfdExtIds[idx]);
            send(csock,tbuf,strlen(tbuf),0);
        }
        pthread_mutex_unlock(&(global->sharedlock));

        fsync(csock);
        close(csock);
}

void processStatusJSONRequest(char *buffer, int csock, struct shared *global){
        char *text="HTTP/1.0 200 OK\nContent-Type: text/plain\n\n{\n  \"timestamp\":\"";
        send(csock,text,strlen(text),0);
        char tbuf[80];
        time_t timer;
        struct tm* tm_info;
        time(&timer);
        tm_info = localtime(&timer);
        strftime(tbuf,80,"%Y-%m-%d %H:%M:%S", tm_info);
        send(csock,tbuf,strlen(tbuf),0);

        //printf("%s Status request...\n",tbuf);
        int history=0;
        int freebuffers = 0;
        int databuffers = 0;
        int noofconnections = 0;
        int recording = 0;
        pthread_mutex_lock(&(global->sharedlock));
        noofconnections = global->recording;
        recording = global->recording>0;
        pthread_mutex_unlock(&(global->sharedlock));
        pthread_mutex_lock(&(global->diskiolock));
        freebuffers = global->freecount;
        databuffers = global->buffercount;
        pthread_mutex_unlock(&(global->diskiolock));

        snprintf(tbuf,80,"\",\n  \"connections\":\"%d\",",noofconnections);
        send(csock,tbuf,strlen(tbuf),0);
        snprintf(tbuf,80,  "\n  \"recording\":\"%d\",\n  ",recording);
        send(csock,tbuf,strlen(tbuf),0);
        snprintf(tbuf,80,  "\"currentFreeBuffers\":\"%d\",\n  ",freebuffers);
        send(csock,tbuf,strlen(tbuf),0);
        snprintf(tbuf,80,  "\"currentUsedBuffers\":\"%d\",\n  ",databuffers);
        send(csock,tbuf,strlen(tbuf),0);
        snprintf(tbuf,80,  "\"currentTotalBuffers\":\"%d\",\n  ",databuffers+freebuffers);
        send(csock,tbuf,strlen(tbuf),0);
        snprintf(tbuf,80,  "\"historyData\":[\n    {\"historyLength\":\"%d\",\"historyNowIndex\":\"%d\",\"counts\":[\n      ",MAX_BUFFER_COUNT_HISTORY,global->historyNowIndex);
        send(csock,tbuf,strlen(tbuf),0);
        for(history=0; history<MAX_BUFFER_COUNT_HISTORY; history++){
            snprintf(tbuf,80,  "{\"free\":\"%d\",\"used\":\"%d\"},",global->historyFree[history],global->historyUsed[history]);
            send(csock,tbuf,strlen(tbuf),0);
        }
        snprintf(tbuf,80,  "{\"free\":\"-1\",\"used\":\"-1\"}\n      ]\n    }\n  ]\n}");
        send(csock,tbuf,strlen(tbuf),0);
        fsync(csock);
        close(csock);
}

int initiateRecording(struct shared *global, int outfd, char* intid, char* extid){
	int started=0;
	int idx;

	//copy the intid/extid to storage we will free when the recording stops.
	char *localintid = calloc(strlen(intid)+1, sizeof(char));
	char *localextid = calloc(strlen(extid)+1, sizeof(char));
	memcpy(localintid, intid, strlen(intid));
	memcpy(localextid, extid, strlen(extid));

    pthread_mutex_lock(&(global->sharedlock));
    if(global->recording<MAX_BUFFERED_OUTPUTS) {
    	//check we don't already have a recording live with this id pair.
    	for(idx=0;idx<global->recording;idx++){
    		if(strcmp(global->outfdExtIds[idx],extid)==0 && strcmp(global->outfdIntIds[idx],intid)==0){
    			started = 1;
    			break;
    		}
    	}
    	if(!started){
			global->outfds[global->recording]=outfd;
			global->outfdIntIds[global->recording]=localintid;
			global->outfdExtIds[global->recording]=localextid;
			global->recording++;
			started=1;
    	}else{
    		started=0;
    	}
    }
    pthread_mutex_unlock(&(global->sharedlock));
    return started;
}

void processStartRecRequest(char *buffer, int csock, struct shared *global, struct post_data *data){
        char *text="HTTP/1.0 200 OK\nContent-Type: text/plain\n\nTold to Start Recording : Sent at : ";
        send(csock,text,strlen(text),0);
        int outfd;
        char tbuf[80];
        time_t timer;
        struct tm* tm_info;
        time(&timer);
        tm_info = localtime(&timer);
        strftime(tbuf,80,"%Y-%m-%d %H:%M:%S", tm_info);
        send(csock,tbuf,strlen(tbuf),0);

        printf("%s Start recording request...\n",tbuf);

        strftime(tbuf,80,"%Y%m%d%H%M%S.ts", tm_info);
        /** open the output file **/
        if(-1 == (outfd = open(tbuf, O_CREAT | O_RDWR, S_IRWXU | S_IRWXG))) {
            perror("Unable to open output file");
        } else {
        	char *intid = UIIntID;
        	char *extid = UIExtID;
        	if(data!=NULL){
        		char *dintid = getValue("intID",data);
        		char *dextid = getValue("extID",data);
        		if(dintid!=NULL && dextid!=NULL){
        			intid=dintid;
        			extid=dextid;
        		}
        	}
            int started=initiateRecording(global,outfd,intid,extid);
            if(!started) {
                char *alreadyrecording="\n\nAlready recording with current ID Pair, new request ignored\n";
                send(csock,alreadyrecording,strlen(alreadyrecording),0);
            } else {
                char *nowrecording="\n\nRecording now in progress\n";
                send(csock,nowrecording,strlen(nowrecording),0);
            }
        }

        fsync(csock);
        close(csock);
}

int stopRecording(struct shared *global, char* intid, char* extid){
	int stopped = 0;
    int idx=0;
    int stopEverything = strlen(intid)==0 && strlen(extid)==0;
    pthread_mutex_lock(&(global->sharedlock));
    if(global->recording>0) {
    	for(idx=0;idx<global->recording;idx++){
    		if(stopEverything || (strcmp(global->outfdExtIds[idx],extid)==0 && strcmp(global->outfdIntIds[idx],intid)==0)){
    			if(global->outfds[idx]!=-1){
					fsync(global->outfds[idx]);
					close(global->outfds[idx]);
    			}
    			//yes, identity checks, not equality checks.. that's what's needed here!
    			if(global->outfdExtIds[idx]!=UIExtID && global->outfdExtIds[idx]!=SocketExtED){
    				free(global->outfdExtIds[idx]);
    			}
    			if(global->outfdIntIds[idx]!=UIIntID && global->outfdIntIds[idx]!=SocketIntED){
    				free(global->outfdIntIds[idx]);
    			}
    			global->outfds[idx] = -1;
    			stopped = 1;
    		}
    	}
    	if(stopped){
    		collapseFdArrays(global);
    	}
    }
    pthread_mutex_unlock(&(global->sharedlock));
    return stopped;
}

void processStopRecRequest(char *buffer, int csock, struct shared *global, struct post_data *data){
        char *text="HTTP/1.0 200 OK\nContent-Type: text/plain\n\nTold to Stop Recording : Sent at : ";
        send(csock,text,strlen(text),0);
        char tbuf[80];
        time_t timer;
        struct tm* tm_info;
        time(&timer);
        tm_info = localtime(&timer);
        strftime(tbuf,80,"%Y-%m-%d %H:%M:%S", tm_info);
        send(csock,tbuf,strlen(tbuf),0);

        printf("%s Stop recording request...\n",tbuf);

    	char *intid = UIIntID;
    	char *extid = UIExtID;
    	if(data!=NULL){
    		char *dintid = getValue("intID",data);
    		char *dextid = getValue("extID",data);
    		if(dintid!=NULL && dextid!=NULL){
    			intid=dintid;
    			extid=dextid;
    		}
    	}

        int stopped=stopRecording(global,intid,extid);
        if(!stopped) {
            char *notrecording="\n\nCould not find matching recording to stop, Stop request ignored\n";
            send(csock,notrecording,strlen(notrecording),0);
        } else{
            char *stoppedok="\n\nRecording stopped.\n";
            send(csock,stoppedok,strlen(stoppedok),0);
        }
        fsync(csock);
        close(csock);
}

void processVideoRequest(char *buffer, int csock, struct shared *global){
    if(global->recording < MAX_BUFFERED_OUTPUTS) {
            char *text="HTTP/1.0 200 OK\nContent-Type: video/h264\nSync-Point: no\nPre-roll: no\nMedia-Start: invalid\nMedia-End: invalid\nStream-Start: invalid\nStream-End: invalid\n\n";
            send(csock,text,strlen(text),0);
            fsync(csock);

            char ofname[80];
            time_t curtime;
            struct tm *fmttime;

            /** set the output file name to time of creation **/
            time(&curtime);
            fmttime = localtime(&curtime);
            strftime(ofname, 80, "%Y-%m-%d %H:%M:%S", fmttime);

            printf("%s Start streaming request...\n",ofname);

            pthread_mutex_lock(&(global->sharedlock));
            global->outfds[global->recording]=csock;
            global->outfdIntIds[global->recording]=SocketIntED;
            global->outfdExtIds[global->recording]=SocketExtED;
            global->recording++;
            pthread_mutex_unlock(&(global->sharedlock));
        }
}

void processListEvents(char *buffer, int csock, struct shared *global, char *newId){
   char *event1="HTTP/1.0 200 OK\nContent-Type: text/plain\n\n{\n";
   char *event2=" \"events\":[\n";
   char *sep = ",\n";
   char *endtext="  ] \n} \n";
   char *endevent="\"}";
   char outbuf[80];
   char timebuf[80];
   struct tm *fmttime;

   send(csock,event1,strlen(event1),0);
   fsync(csock);

   if(newId!=NULL){
       snprintf(outbuf,80," \"addedId\":\"%s\",",newId);
       send(csock,outbuf,strlen(outbuf),0);
       fsync(csock);
   }

   send(csock,event2,strlen(event2),0);
   fsync(csock);

   //printf("Waiting on lock");

   pthread_mutex_lock(&(global->eventLock));
   //printf("processing event list.. events present? %d\n",(global->events==NULL?0:1));
   struct event *current = global->events;
   while(current!=NULL){
        fmttime = localtime(&(current->eventTime));
        strftime(timebuf, 80, "%Y-%m-%d %H:%M:%S", fmttime);
        snprintf(outbuf,80,"  { \"eventTime\":\"%s\",\"eventType\":\"%d\",\"eventData\":\"",timebuf,current->type);
        send(csock,outbuf,strlen(outbuf),0);
        fsync(csock);
        if(current->data!=NULL){
        	send(csock,current->data,strlen(current->data),0);
        	fsync(csock);
        }
        snprintf(outbuf,80,"\",\"intID\":\"%s\"",current->internalId);
        send(csock,outbuf,strlen(outbuf),0);
        fsync(csock);
        snprintf(outbuf,80,",\"extID\":\"%s",current->externalId);
        send(csock,outbuf,strlen(outbuf),0);
        fsync(csock);
        send(csock,endevent,strlen(endevent),0);
        fsync(csock);
        current = current->next;
        if(current!=NULL){
            send(csock,sep,strlen(sep),0);
            fsync(csock);
        }
   }
   pthread_mutex_unlock(&(global->eventLock));
   //printf("done\n");
   send(csock,endtext,strlen(endtext),0);
   fsync(csock);
   close(csock);
}

char *generateId(){
    char *obuf = calloc(81,sizeof(char));
    char tbuf[80]= {0};
    time_t timer;
    struct tm* tm_info;
    time(&timer);
    tm_info = localtime(&timer);
    strftime(tbuf,80,"%Y%m%d%H%M%S", tm_info);
    struct timeval tv;
    gettimeofday(&tv,NULL);
    long t = tv.tv_usec;
    snprintf(obuf,80,"%s%ld",tbuf,t);
    return obuf;
}

void processAddEvent(char *buffer, int csock, struct shared *global, struct post_data *post){
	char *genfilename=NULL;
	char *genshowid=NULL;
	char *genid=NULL;

	char *filename=NULL;
	char *showid=NULL;
	char *data=NULL;


    //Init the mandatory data for all events.. time, intid,extid.
    //(intid is generated if absent).

    char *timestring = "time";
    char *time = getValue(timestring,post);
    if(time==NULL){
       return404(buffer,csock,"No time in addEvent request");
       return;
    }
    char *myidstring = "intId";
    char *myid = getValue(myidstring,post);
    if(myid==NULL){
    	myid = generateId();
    	genid=myid;
    }
    char *theiridstring = "extId";
    char *theirid = getValue(theiridstring,post);
    if(theirid==NULL){
        return404(buffer,csock,"No externalId in addEvent request");
        if(genid!=NULL)
        	free(genid);
        return;
    }

    char *typestring = "type";
    char *typec = getValue(typestring,post);
    if(typec==NULL){
       return404(buffer,csock,"No type in addEvent request");
       if(genid!=NULL)
       	free(genid);
       return;
    }
    int type = atoi(typec);
    //ok, we have a type.. what options are valid for type?
    if(type==EVENT_TYPE_ECHO){
    	//ECHO
        char *datastring = "data";
        data = getValue(datastring,post);
        if(data==NULL){
            return404(buffer,csock,"Echo event with no data specified.");
            if(genid!=NULL)
            	free(genid);
            return;
        }
    }else if (type==EVENT_TYPE_STARTREC){
    	//START
        char *filenamestring = "filename";
        filename = getValue(filenamestring,post);
        if(filename==NULL){
        	//filename is optional..
        	filename = calloc(strlen(myid)+4,sizeof(char));
        	memcpy(filename,myid,strlen(myid));
        	memcpy(filename+strlen(myid), ".ts", 3);
        	genfilename=filename;
        }
        char *showidstring = "showid";
        showid = getValue(showidstring,post);
        if(showid==NULL){
           //show id is also optional..
           showid = calloc(5,sizeof(char));
           memcpy(showid, "auto", 3);
           genshowid=showid;
        }
    }else if (type==EVENT_TYPE_STOPREC){
    	//STOP
    	//stop has no data beyond the ids.
    }else if (type==EVENT_TYPE_CHANNEL){
    	//CHANNEL
        char *datastring = "data";
        data = getValue(datastring,post);
        if(data==NULL){
            return404(buffer,csock,"Channel change event with no data specified.");
            return;
        }
    }

    struct tm tm;
    time_t t;
    //format like this in html will get escaped to the below string..
    //messy.. but cheap.
    //"6 Dec 2001 12:33:45"
    if (strptime(time, "%d+%b+%Y+%H%%3A%M%%3A%S", &tm) == NULL){
       printf("Time string was %s\n",time);
       return404(buffer,csock,"Invalid time format in addEvent request eg '2 Jan 2010 12:45:20' ");
       if(genfilename!=NULL)
       	free(genfilename);
       if(genshowid!=NULL)
       	free(genshowid);
       if(genid!=NULL)
       	free(genid);
       return;
    }
    tm.tm_isdst = -1;      /* Not set by strptime(); tells mktime()
                          to determine whether daylight saving time
                          is in effect */
    t = mktime(&tm);
    if (t == -1){
       return404(buffer,csock,"Unable to mktime from time buffer");
       return;
    }

    //build the event structure.. all data in the structure MUST be copied
    //or built within this method. The data referenced by args from this
    //method are not valid once the request is complete.

    struct event *event = calloc(1, sizeof(struct event));
    event->eventTime = t;

    if(type==EVENT_TYPE_STARTREC){
		event->data = (char*)calloc(strlen(showid)+strlen(filename)+2,sizeof(char));
		if(showid!=NULL){
			memcpy(event->data,showid,strlen(showid));
		}
		memcpy(&(event->data[strlen(showid)]),"?",1);
		if(filename!=NULL){
			memcpy(&(event->data[strlen(showid)+1]),filename,strlen(filename));
		}
    }else{
    	if(data!=NULL){
			event->data = (char*)calloc(strlen(data)+1,sizeof(char));
			memcpy(event->data,data,strlen(data));
    	}else{
    		event->data=NULL;
    	}
    }

    event->type = type;

    event->internalId = (char*)calloc(strlen(myid)+1,sizeof(char));
    strcpy(event->internalId,myid);

    event->externalId = (char*)calloc(strlen(theirid)+1,sizeof(char));
    strcpy(event->externalId,theirid);

    event->next = NULL;

    //Event built.. insert into the list.

    pthread_mutex_lock(&(global->eventLock));
    struct event *current = global->events;
    struct event *last = NULL;
    int done=0;
    if(current==NULL){
        global->events = event;
    }else{
        while(current!=NULL){
            //difftime gives positive if the 1st arg is in the future past the 2nd arg.
            done=difftime(event->eventTime,current->eventTime);

            //if result is negative, our event comes before the current one..
            if(done<=0){
                //tag the current one onto our next.
                event->next = current;
                if(last==NULL){
                    global->events = event;
                }else{
                    last->next = event;
                }
                current=NULL;
            }else{
                //result was positive, our event comes after the current one.. so go looking for where.
                last = current;
                current = current->next;
                //if there is no next coming up.. then we just became the end of the list.
                if(current==NULL){
                    last->next=event;
                }
            }
        }
    }
    pthread_mutex_unlock(&(global->eventLock));

    //release any temp strings used while building the event.
    if(genfilename!=NULL)
    	free(genfilename);
    if(genshowid!=NULL)
    	free(genshowid);
    if(genid!=NULL)
    	free(genid);

    return processListEvents(buffer, csock, global, event->internalId);
}

void processRemoveEvent(char *buffer, int csock, struct shared *global, struct post_data *post){
    char *myidstring = "intId";
    char *myid = getValue(myidstring,post);
    if(myid==NULL){
        return404(buffer,csock,"No internalId in removeEvent request");
        return;
    }
    char *theiridstring = "extId";
    char *theirid = getValue(theiridstring,post);

    //allow theirid to be null.. to delete all matching intids.
	//    if(theirid==NULL){
	//        return404(buffer,csock,"No externalId in removeEvent request");
	//        return;
	//    }

    pthread_mutex_lock(&(global->eventLock));
    struct event *current = global->events;
    struct event *last = NULL;
    if(current==NULL){
        //no events to remove from.. event may have already fired.. c'est la vie.
    }else{
        while(current!=NULL){
			if((theirid!=NULL && strcmp(current->externalId,theirid)==0) && strcmp(current->internalId,myid)==0){
				//we have a match, so we will remove it.. so we'll free the data contained in it.
    			if(current->externalId!=UIExtID && current->externalId!=SocketExtED){
    				free(current->externalId);
    			}
    			if(current->internalId!=UIIntID && current->internalId!=SocketIntED){
    				free(current->internalId);
    			}
    			free(current->data);
				//found match... unhook it.
    			if(last!=NULL){
    				last->next=current->next;
    			}else{
    				//matched the 1st item.. rewrite the global..
    				global->events=current->next;
    			}
				free(current);
			}else{
				last = current;
			}
        	current = current->next;
        }
    }
    pthread_mutex_unlock(&(global->eventLock));

    return processListEvents(buffer, csock, global,NULL);
}

void processPostRequest(char *buffer, int csock, struct shared *global){
    struct post_data *data = getPostData(buffer);
    char *actionString = "action";
    char *action = getValue(actionString,data);
    if(action==NULL){
       return404(buffer,csock,"No action in post request");
       return;
    }else{
        if(strcmp(action,"status")==0){
            processStatusRequest(buffer,csock,global);
        }else if(strcmp(action,"jstatus")==0){
            processStatusJSONRequest(buffer,csock,global);
        }else if(strcmp(action,"startrec")==0){
            processStartRecRequest(buffer,csock,global,data);
        }else if(strcmp(action,"stoprec")==0){
            processStopRecRequest(buffer,csock,global,data);
        }else if(strcmp(action,"listevents")==0){
            processListEvents(buffer,csock,global,NULL);
        }else if(strcmp(action,"addevent")==0){
            processAddEvent(buffer,csock,global,data);
        }else if(strcmp(action,"removeevent")==0){
            processRemoveEvent(buffer,csock,global,data);
        }else{
            char *text="HTTP/1.0 200 OK\nContent-Type: text/plain\n\nUnknown action requested : ";
            send(csock,text,strlen(text),0);
            fsync(csock);
            send(csock,action,strlen(action),0);
            fsync(csock);
        }
    }
    if(data!=NULL){
    	free(data->keys);
    	free(data->values);
        free(data);
    }
}

void processHttpRequest(char *buffer, int csock, struct shared *global){
    if(getIndex("GET /web/",buffer)==0){
        processFileRequest(buffer,csock);
    }else if(getIndex("GET /status HTTP/1.1",buffer)==0){
        processStatusRequest(buffer,csock,global);
    }else if(getIndex("GET /startrec HTTP/1.1",buffer)==0){
        processStartRecRequest(buffer,csock,global,NULL);
    }else if(getIndex("GET /stoprec HTTP/1.1",buffer)==0){
        processStopRecRequest(buffer,csock,global,NULL);
    }else if(getIndex("GET /video HTTP/1.1",buffer)==0){
        processVideoRequest(buffer,csock,global);
    }else if(getIndex("GET /favicon.ico HTTP/1.",buffer)==0){
        return404(buffer,csock,"We don't do Favicons =)");
    }else if(getIndex("GET /jstatus HTTP/1.1",buffer)==0){
        processStatusJSONRequest(buffer,csock,global);
    }else if(getIndex("POST /action HTTP/1.1",buffer)==0){
        processPostRequest(buffer,csock,global);
    }else{
        return404(buffer,csock,"Unknown request");
    }

}

void *SocketHandler(void* lp) {
    struct data *datainst = (struct data *)lp;
    int csock = datainst->csock; //(int*)lp;
    struct shared *global = datainst->data;

    const int buffer_len = 4096;
    char buffer[buffer_len];

    int bytecount;

    memset(buffer, 0, buffer_len);
    if((bytecount = recv(csock, buffer, buffer_len-1, 0))== -1) {
        fprintf(stderr, "Error receiving data %d\n", errno);
        goto FINISH;
    }

    //request was read ok.. now figure out what it asked for.
    processHttpRequest(buffer,csock,global);

FINISH:
    free(lp);
    return 0;
}
