#define _GNU_SOURCE

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <assert.h>
#include <inttypes.h>

// compile: gcc replay.c -pthread

//Note: all sizes are in block (1 block = BLOCK_SIZE bytes)

// CONFIGURATION PART
static int const BLOCK_SIZE = 512; //1 block = n bytes
static int const LARGEST_REQUEST_SIZE = 65536; //blocks
static int const MEM_ALIGN = 512; //bytes
int numworkers = 200; // =number of threads
char tracefile[] = "T7-disk0-cut_smrmultires.txt"; //trace file to read as input
int printlatency = 1; //print every io latency
int maxio = 1000000; //halt if number of IO > maxio, to prevent printing too many to metrics file
int respecttime = 1;

// ANOTHER GLOBAL VARIABLES
int fd;
int totalio;
int jobtracker = 0;
int latecount = 0;
void *buff;
uint64_t starttime;

long *blkno; //TODO: devise better way to save blkno,size,flag
int *reqsize;
int *reqflag;
float *timestamp;

FILE *metrics; //current format: offset,size,type,latency(ms)

pthread_mutex_t lock;

/*=============================================================*/

void prepareMetrics(){
    if(printlatency == 1 && totalio > maxio){
        fprintf(stderr, "Too many IO in the trace file!\n");
        exit(1);
    }
    if(printlatency == 1){
        metrics = fopen("replay_metrics.txt", "w+");
        if(!metrics){
            fprintf(stderr,"Error creating metrics file!\n");
            exit(1);
        }
    }
}

int readTrace(char ***req){
    //first, read the number of lines
    FILE *trace = fopen(tracefile,"r");
    int ch;
    int numlines = 0;
    
    while(!feof(trace)){
        ch = fgetc(trace);
        if(ch == '\n'){
            numlines++;
        }
    }
    
    rewind(trace);

    //then, start parsing
    if((*req = malloc(numlines * sizeof(char*))) == NULL){
        fprintf(stderr,"Error in memory allocation\n");
        exit(1);
    }
    
    char line[100]; //assume it will not exceed 100 chars
    int i = 0;
    while(fgets(line, sizeof(line), trace) != NULL){
        line[strlen(line) - 1] = '\0';
        if(((*req)[i] = malloc((strlen(line) + 1) * sizeof(char))) == NULL){
            fprintf(stderr,"Error in memory allocation\n");
            exit(1);
        } //error here
        strcpy((*req)[i],line);
        i++;
    }
    fclose(trace);
    
    return numlines;
}

void arrangeIO(char **requestarray){
    blkno = malloc(totalio * sizeof(long));
    reqsize = malloc(totalio * sizeof(int));
    reqflag = malloc(totalio * sizeof(int));
    timestamp = malloc(totalio * sizeof(float));
    
    if(blkno == NULL || reqsize == NULL || reqflag == NULL || timestamp == NULL){
        fprintf(stderr,"Error malloc in arrangeIO!\n");
        exit(1);
    }
    
    int i = 0;
    for(i = 0; i < totalio; i++){
        char *io = malloc((strlen(requestarray[i]) + 1) * sizeof(char));
        if(io == NULL){
            fprintf(stderr,"Error malloc io!\n");
            exit(1);
        }
        strcpy(io,requestarray[i]);
        
        timestamp[i] = atof(strtok(io," ")); //1. request arrival time
        strtok(NULL," "); //2. device number
        blkno[i] = (long)atoi(strtok(NULL," ")) * BLOCK_SIZE; //3. block number
        reqsize[i] = atoi(strtok(NULL," ")) * BLOCK_SIZE; //4. request size
        reqflag[i] = atoi(strtok(NULL," ")); //5. request flags
        free(io);
    }
}

void atomicAdd(int *val, int add){
    assert(pthread_mutex_lock(&lock) == 0);
    (*val) += add;
    assert(pthread_mutex_unlock(&lock) == 0);
}

void *performIO(){
    //double sum = 0;
    //int howmany = 0;
    int curtask;
    int mylatecount = 0;
    struct timeval t1,t2;

    while(jobtracker < totalio){
        //firstly save the task to avoid any possible contention later
        curtask = jobtracker;
        atomicAdd(&jobtracker,1); //I take this job
        
        //respect time part
        if(respecttime == 1){
            gettimeofday(&t1,NULL); //get current time
            uint64_t elapsedtime = t1.tv_sec * 1000000 + t1.tv_usec - starttime;
            if(elapsedtime <= timestamp[curtask] * 1000){
                usleep((uint64_t)(timestamp[curtask] * 1000) - elapsedtime);
            }else{ //I am late
                mylatecount++;
            }
        }
          
        //do the job      
        gettimeofday(&t1,NULL); //reset the start time to before start doing the job
        if(reqflag[curtask] == 0){
            if(pwrite(fd, buff, reqsize[curtask], blkno[curtask]) < 0){
                fprintf(stderr,"Cannot write size %d to offset %lu!\n",(reqsize[curtask] / 512), (blkno[curtask] / 512));
                exit(1);
            }
        }else{
            if(pread(fd, buff, reqsize[curtask], blkno[curtask]) < 0){
                fprintf(stderr,"Cannot read size %d to offset %lu!\n",(reqsize[curtask] / 512), (blkno[curtask] / 512));
                exit(1);
            }
        }
        gettimeofday(&t2,NULL);
        float iotime = (t2.tv_sec - t1.tv_sec) * 1000.0 + (t2.tv_usec - t1.tv_usec) / 1000.0;
        if(printlatency == 1){
            assert(pthread_mutex_lock(&lock) == 0);
            fprintf(metrics,"%.3f,%lu,%d,%d,%.3f\n",timestamp[curtask],blkno[curtask] / 512,reqsize[curtask] / 512,reqflag[curtask],iotime);
            assert(pthread_mutex_unlock(&lock) == 0);
        }
    }
    
    atomicAdd(&latecount, mylatecount);
    
    return NULL;
}

void *printProgress(){
    while(jobtracker <= totalio){
        printf("Progress: %.2f%%\r",(float)jobtracker / totalio * 100);
        fflush(stdout);
        
        if(jobtracker == totalio){
            break;
        }
        
        sleep(1);
    }
    printf("\n");
    
    return NULL;
}

void operateWorkers(){
    struct timeval t1,t2;
    float totaltime;
    
    printf("Start doing requests...\n");
    
    // thread creation
    pthread_t *tid = malloc(numworkers * sizeof(pthread_t));
    if(tid == NULL){
        fprintf(stderr,"Error malloc thread!\n");
        exit(1);
    }
    //pthread_t track_thread; //progress
    
    assert(pthread_mutex_init(&lock, NULL) == 0);
    
    int x;
    gettimeofday(&t1,NULL);
    starttime = t1.tv_sec * 1000000 + t1.tv_usec;
    for(x = 0; x < numworkers; x++){
        pthread_create(&tid[x], NULL, performIO, NULL);
    }
    //pthread_create(&track_thread, NULL, printProgress, NULL); //progress
    for(x = 0; x < numworkers; x++){
        pthread_join(tid[x], NULL);
    }
    //pthread_join(track_thread, NULL); //progress
    gettimeofday(&t2,NULL);
    totaltime = (t2.tv_sec - t1.tv_sec) * 1000.0 + (t2.tv_usec - t1.tv_usec) / 1000.0;
    printf("Total run time: %.3f ms\n",totaltime);
    if(respecttime == 1){
        printf("Late rate: %.2f%%\n",100 * (float)latecount / totalio);
    }
    
    fclose(metrics);
    assert(pthread_mutex_destroy(&lock) == 0);
    
    //run statistics
    system("python statistics.py");
}

int main(int argc, char *argv[]) {
    char device[64];
    char **request;
    
    if (argc <= 1){
        printf("Please specify device name\n");
        exit(1);
    }else{
        sprintf(device,"%s",argv[1]);
    }
    
    // read the trace before everything else
    totalio = readTrace(&request);
    arrangeIO(request);
    
    // start the disk part
    fd = open(device, O_DIRECT | O_SYNC | O_RDWR);
    if(fd < 0) {
        fprintf(stderr,"Cannot open %s\n", device);
        exit(1);
    }
    
    if (posix_memalign(&buff,MEM_ALIGN,LARGEST_REQUEST_SIZE * BLOCK_SIZE)){
        fprintf(stderr,"memory allocation failed\n");
        exit(1);
    }

    prepareMetrics();
    operateWorkers();

    return 0;
}
