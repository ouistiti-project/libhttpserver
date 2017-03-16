#ifndef VTHREAD_H
#define VTHREAD_H

typedef struct vthread_s* vthread_t;
#ifdef WIN32
typedef DWORD (__stdcall *vthread_routine)( LPVOID lpParam );
//#define vthread_routine LPTHREAD_START_ROUTINE
#else
typedef void *(*vthread_routine)(void *);
#endif

#ifdef WIN32
# define vthread_attr_t char
#else
# define vthread_attr_t pthread_attr_t
#endif

int vthread_create(vthread_t *thread, vthread_attr_t *attr,
	vthread_routine start_routine, void *arg, int argsize);

int vthread_join(vthread_t thread, void **value_ptr);

void vthread_wait(vthread_t threads[], int nbthreads);

int vthread_exist(vthread_t thread);

void vthread_yield(vthread_t thread);

#endif
