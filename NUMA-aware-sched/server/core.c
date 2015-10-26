#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>

#include "crew.h"
#include "sock.h"
#include "collect.h"

#ifdef sun
	#include <thread.h>
	#include <semaphore.h>
#endif


//#define DEBUG
#ifdef DEBUG
	#define DPRINTF(fmt, s...)  printf(fmt,##s)
#else
	#define DPRINTF(fmt, s...)
#endif

// Global variable
static time_log_t resSet;
static crew_t my_crew;
static collector_t my_collector;
static int groupid;

// Function prototype
void* collectThread(void *);
void* workerThread(void *);
void* recvThread(void *);

/*
 *	Server entry point
 */
int main(int argc, char *argv[])
{
	int status;

	// For socket
	int clnt_sock, serv_sock;

	struct sockaddr_in clnt_addr;
	int clnt_addr_size = sizeof(clnt_addr);

	// Default number of workers 1
	int nWorkers = CREW_SIZE;

	if (argc < 3) {
		fprintf(stderr, "usage: %s <groupid> <port> [number of threads]\n", argv[0]);
		exit(1);
	}
	
	nWorkers = atoi(argv[3]);
	groupid = atoi(argv[1]);

	printf("nWorkers : %d\n", nWorkers );

	// Initialize socket 
	serv_sock = init_sock(atoi(argv[2]));


#ifdef sun
	thr_setconcurrency(nWorkers + 1);
#endif

	// Make Collector
	
	status = create_collector(&my_collector, collectThread);
	if (status != 0) {
		fprintf(stderr, "Failed to create collector\n");
	}
	
	
	// Create crew thread
	status = create_crew(&my_crew, nWorkers, workerThread);
	if (status != 0) {
		fprintf(stderr, "Failed to create crew\n");
	}

	fprintf(stdout, "Waiting for llients... \n");
	
	
	while (1) {

		clnt_sock = accept(serv_sock, (struct sockaddr *)&clnt_addr, &clnt_addr_size);

		if (pthread_create(NULL, NULL, recvThread, (void*)clnt_sock) < 0) {
			perror("phtread_create() error");
			exit(1);
		}
	}

	printf("close...\n");
	return 0;
}
/*
 *	return Fibonacci sequence
 */
int fib(int n) 
{ 
	return n <= 2 ? 1 : fib(n-1) + fib(n-2); 
}

/*
 *	Crew's work thread
 */
void* workerThread(void *arg)
{
	int status, sock, nWrite;
	worker_p mine = (worker_t*)arg;
	crew_p crew = mine->crew;
	req_t item;
	work_p work;
	struct timeval begin, end;

	pthread_mutex_lock(&crew->mutex);
	
	/*
	 *	when crews are created, work queue is empty.
	 *	so, crew wait until anyone put job into queue
	 */
	while (crew->work_count == 0) {
		pthread_cond_wait(&crew->go, &crew->mutex);
	}
	pthread_mutex_unlock(&crew->mutex);

	printf("Crew %d starting\n", mine->index);

	while(1) {

		gettimeofday(&begin, NULL);

		status = pthread_mutex_lock (&crew->mutex);
		if (status != 0)
			fprintf(stderr, "Lock crew mutex");

		DPRINTF("Crew %d top: first is %#lx, count is %d\n", mine->index, crew->first, crew->work_count);
		
		/*
		 *	Until job come to queue, thread is sleeping
		 */
		while (crew->first == NULL) {
			DPRINTF("Crew %d first is NULL\n", mine->index);
			status = pthread_cond_wait (&crew->go, &crew->mutex);
			if (status != 0)
				fprintf(stderr, "wait for work\n");
		}
		
		/*
		 *	Once job is inserted to queue, crew wake up
		 */
		DPRINTF("Crew %d woke: %#lx, %d\n", mine->index, crew->first, crew->work_count);
		
		//sock = crew->first->sock;
		//item = dequeue_item(crew);

		work = crew->first;
		crew->first = work->next;
		sock = work->sock;

		if (crew->first == NULL)
			crew->last = NULL;
		
		memcpy(&item, &work->data, sizeof(work->data));

		status = pthread_mutex_unlock (&crew->mutex);
		if (status != 0)
			fprintf(stderr, "Lock crew mutex");
	
		/*
		 *	Here, job is handled
		 */

		// 1) Get fibonacci sequence
		item.result = fib(item.input);
		DPRINTF("Crew %d get data %d(fib:%d) from %d\n", mine->index, item.input, item.result, item.groupid);
		
		// 2) Send result_item to client
		nWrite = write(sock, &item, sizeof(item));

		// 3) Get end time
		gettimeofday(&end, NULL);
		//timersub(&end, &begin, &elapsed);

		// 4) Free
		free(work);

		status = pthread_mutex_lock(&resSet.mutex);
		if (status != 0)
			fprintf(stderr, "Lock result_set mutex");
		
		resSet.send_count[item.groupid]++;
		resSet.send_count[0] ++;

		resSet.total[item.groupid] += diffTime(&end, &begin);
		resSet.total[0] += diffTime(&end, &begin);

		status = pthread_mutex_unlock(&resSet.mutex);
		if (status != 0)
			fprintf(stderr, "Unlock result_set mutex");

		/*
		 *	after handled job, crew decrements work_count
		 */
		status = pthread_mutex_lock (&crew->mutex);
		if (status != 0)
			fprintf(stderr, "Lock result_set mutex");
		
		crew->work_count --;
		
		status = pthread_mutex_unlock (&crew->mutex);
		if (status != 0)
			fprintf(stderr, "Unlock result_set mutex");
	}

	return NULL;
}

/*
 *	Receive work_item from client
 */
void* recvThread(void *arg)
{
	int csock = (int)arg, status, nRead=0;
	req_t work_item;
	work_p request;
	
	printf("Client connect... recv thread start (%d)\n", csock);

	while (1){
		memset(&work_item, 0x00, sizeof(req_t));
		nRead = read(csock, &work_item, sizeof(req_t));
				
		if (nRead <= 0) {
			break;
		}

		if (work_item.groupid > 7 || work_item.groupid < 0 ) {
			fprintf(stderr, "Invaild client groupid(%d)\n", work_item.groupid);
			break;
		}

		pthread_mutex_lock(&my_crew.mutex);

		//work_item processed.
		request = (work_p)malloc(sizeof(work_t));
		memcpy(&request->data, &work_item, sizeof(req_t));
		request->sock = csock;
		request->next = NULL;
		
		// Adjust queue pointer
		if (my_crew.first == NULL) {
			my_crew.first = request;
			my_crew.last = request;
		} else {
			my_crew.last->next = request;
			my_crew.last = request;
		}

		my_crew.work_count++;
		

		// Signal to employees
		status = pthread_cond_signal (&my_crew.go);

		if (status != 0) {
			free(my_crew.first);
			my_crew.first = NULL;
			my_crew.work_count = 0;
			pthread_mutex_unlock(&my_crew.mutex);
			exit(1);
		}
		pthread_mutex_unlock(&my_crew.mutex);
		
	}
	
	close(csock);
	printf("Recv thread done (%d)\n", csock);

	return NULL;
}

/*
 *	Collect thread
 *		1) Get total sending items
 *		2) 
 */
void* collectThread(void *arg)
{
	int rfd, nWrite, i;
	time_t t;
	struct tm tmptr;
	char message[1024];
	char filename[20];

	DPRINTF("collector thread start...\n");
	
	sprintf(filename, "./server.%d.result", groupid);
	rfd = open(filename, O_RDWR | O_CREAT | O_TRUNC, 0700);
	
	if (rfd == -1) {
		perror("open error : ");
		exit(1);
	}
	
	strcpy(message, ":: Start collecting data ::\n");
	nWrite = write(rfd, message, strlen(message));
	
	while (1) {

		memset(&resSet, 0x00, sizeof(time_log_t));
		sleep(1);
		
		time(&t);
		localtime_r(&t, &tmptr);

		pthread_mutex_lock(&resSet.mutex);
		
		if (resSet.send_count[0] == 0 || resSet.total[0] == 0) continue;

		sprintf(message, "<%02d:%02d:%02d> %5ld:%5ldus \t "
												, tmptr.tm_hour, tmptr.tm_min, tmptr.tm_sec
												, resSet.send_count[0]
												, resSet.total[0]/resSet.send_count[0]);
		
		
		for (i=1; i<NUM_GROUPS; i++) {
			
			if (resSet.total[i] != 0 && resSet.send_count[i] !=0) {
				sprintf(message+strlen(message), "%d:%5ldus, ", i, resSet.total[i]/resSet.send_count[i]);
			}
		}
		sprintf(message+strlen(message), "\n");
		nWrite = write(rfd, message, strlen(message));
		
		pthread_mutex_unlock(&resSet.mutex);

	}

	close(rfd);

	DPRINTF("collector thread done\n");
	return NULL;
}

