#ifndef _COLLECT_H_
#define _COLLECT_H_

#include <stdio.h>
#include <pthread.h>
#include <sys/time.h>

#define timersub(a, b, result)                 \
	do {                                        \
		(result)->tv_sec = (a)->tv_sec - (b)->tv_sec;                 \
		(result)->tv_usec = (a)->tv_usec - (b)->tv_usec;                  \
		if ((result)->tv_usec < 0) {                       \
			--(result)->tv_sec;                            \
			(result)->tv_usec += 1000000;                  \
		}                                         \
	} while (0)

#define NUM_GROUPS	8


typedef struct time_log{
	
	long total[NUM_GROUPS];
	unsigned long send_count[NUM_GROUPS];		// during 1 sec

	pthread_mutex_t mutex;

} time_log_t, *time_log_p;

typedef struct collector_tag {
	pthread_t	id;
	pthread_mutex_t mutex;
	struct time_log time;

} collector_t, *collector_p;


int create_collector(struct collector_tag *this, void* (*threadFunc)(void*));

long diffTime(struct timeval* , struct timeval*);

#endif
