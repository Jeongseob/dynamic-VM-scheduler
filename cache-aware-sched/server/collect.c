#include "collect.h"

int create_collector(struct collector_tag *this, void* (*threadFunc)(void*))
{
	int status;
	
	status = pthread_mutex_init(&this->mutex, NULL);
	if (status !=0 ) 
		return status;

	status = pthread_create(&this->id, NULL, threadFunc, NULL);
	if (status !=0 ) {
		perror("pthread create error");
		return status;
	}

	return 0;
}


long diffTime(struct timeval *end, struct timeval *begin)
{
	long timedif;

	timedif = 1000000L * (end->tv_sec - begin->tv_sec) + end->tv_usec - begin->tv_usec;

	return timedif;
}
