#ifndef _CREW_H_
#define _CREW_H_

#include <string>

using namespace std;

typedef struct req_tag {
	unsigned int vmKey;
	unsigned int adversaryVmAffinity;
	unsigned int srcHostID;
	unsigned int destHostID;
	unsigned int localID;
} req_t, *req_p;

typedef struct work_tag {
	struct work_tag *next;
	req_t data;
	
} work_t, *work_p;

typedef struct worker_tag {
	int index;
	pthread_t thread;
	struct crew_tag *crew;
} worker_t, *worker_p;

typedef struct crew_tag {
	int worker_size;
	worker_t *worker;
	long work_count;
	work_t *first, *last;

	pthread_barrier_t barrier;
	pthread_mutex_t mutex;
	pthread_cond_t done;
	pthread_cond_t go;

} crew_t, *crew_p;

int create_crew(struct crew_tag *crew, int size, void* (*threadFunc)(void*));
int wait_crew(struct crew_tag *crew);
int enque_item(struct crew_tag* crew, struct req_tag item, int dest_sock);
struct req_tag dequeue_item(struct crew_tag*);

#endif
