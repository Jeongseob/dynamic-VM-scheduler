#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <pthread.h>
#include <errno.h>

#include "crew.h"

/*
 *	Create worker thread
 */
int create_crew(struct crew_tag *crew, int size, void* (*threadFunc)(void*))
{
	int worker_index;
	int status;

	crew->worker_size = size;
	crew->worker = (worker_p)malloc(sizeof(worker_t)*size);
	crew->work_count = 0;
	crew->first = NULL;
	crew->last = NULL;

	// initialize synchronization object 
	status = pthread_barrier_init(&crew->barrier, NULL, size);
	if (status != 0)
		return status;

	status = pthread_mutex_init(&crew->mutex, NULL);
	if (status != 0)
		return status;

	status = pthread_cond_init(&crew->done, NULL);
	if (status != 0)
		return status;

	status = pthread_cond_init(&crew->go, NULL);
	if (status != 0)
		return status;

	// create worker thread
	for (worker_index = 0; worker_index < crew->worker_size; worker_index++) {
		crew->worker[worker_index].index = worker_index;
		crew->worker[worker_index].crew = crew;

		status = pthread_create (&crew->worker[worker_index].thread, NULL, threadFunc, (void*)&crew->worker[worker_index]);
		
		if (status != 0) {
			perror("phtread_create() error");
			return status;
		}
	}
	
	return 0;
}

int wait_crew(struct crew_tag *crew)
{
	int worker_index;
	int status;

	for (worker_index = 0; worker_index < crew->worker_size; worker_index++) {
		pthread_join(crew->worker[worker_index].thread, (void **)&status);
	}

	return 0;
}


/*
 *	Put item to work_queue
 */
int enque_item(struct crew_tag* crew, struct req_tag item, int dest_sock)
{
	work_p request;

	//work_item processed.
	request = (work_p)malloc(sizeof(work_t));
	memcpy(&request->data, &item, sizeof(req_t));
	request->next = NULL;

	
	// Adjust queue pointer
	if (crew->first == NULL) {
		crew->first = request;
		crew->last = request;
	} else {
		crew->last->next = request;
		crew->last = request;
	}

	crew->work_count++;

	return 0;
}

/*
 *	Get item from work_queue
 */
struct req_tag dequeue_item(struct crew_tag *_this)
{
	req_t item;
	work_p work;

	work = _this->first;
	_this->first = work->next;

	if (_this->first == NULL)
		_this->last = NULL;

	memcpy(&item, &work->data, sizeof(item));
	free(work);

	return item;
}
