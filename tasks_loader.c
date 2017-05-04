#include "tasks_loader.h"
#include "tasks_queue.h"
#include "tasks_loader.h"

#include <pthread.h>
#include <logger.h>

#include <stdlib.h>
#include <string.h>
#include <assert.h>

#ifdef _WIN32
	#include <windows.h>
#else
	#include <unistd.h>
	#include <errno.h>
#endif

#define THREAD_NUM 4

struct job {
	struct job* next;

	pthread_t id;
	int type;
	void* ud;

	char desc[32];
};

struct job_queue {
	struct job* head;
	struct job* tail;
	pthread_rwlock_t lock;
};

#define THREAD_MAX 16

enum JOB_TYPE {
	JOB_INVALID = 0,
	JOB_LOAD_FILE,
	JOB_PARSER_DATA	
};

struct load_params {
	int version;

	struct load_params* next;

	struct tasks_loader* loader;

	char filepath[512];
	char desc[32];

	void (*load_cb)(const char* filepath, void (*unpack)(const void* data, size_t size, void* ud), void* ud);

	void (*parser_cb)(const void* data, size_t size, void* ud);
	void (*release_cb)(void* ud);
	void* parser_ud;
};

struct load_params_queue {
	struct load_params* head;
	struct load_params* tail;
	pthread_rwlock_t lock;
};

struct parse_params {
	int version;

	struct parse_params* next;

	char* data;
	size_t size;

	void (*parser_cb)(const void* data, size_t size, void* ud);
	void (*release_cb)(void* ud);
	void* ud;
};

struct parse_params_queue {
	struct parse_params* head;
	struct parse_params* tail;
	pthread_rwlock_t lock;
};

struct tasks_loader {
	struct job_queue job_free_queue;
	struct job_queue job_load_queue;
	struct job_queue job_parse_queue;

	int version;
	pthread_mutex_t version_lock;
	pthread_mutex_t quit_lock;
	pthread_mutex_t dirty_lock;
	pthread_cond_t  dirty_cv;	

	pthread_t threads[THREAD_MAX];
	int thread_num;

	struct load_params_queue  params_load_queue;
	struct parse_params_queue params_parse_queue;
};

static inline int
_get_version(struct tasks_loader* loader) {
	int ret;
	pthread_mutex_lock(&loader->version_lock);
	ret = loader->version;
	pthread_mutex_unlock(&loader->version_lock);
	return ret;
}

static inline bool
_is_valid_version(struct tasks_loader* loader, int version) {
	bool ret;
	pthread_mutex_lock(&loader->version_lock);
	ret = version == loader->version;
	pthread_mutex_unlock(&loader->version_lock);
	return ret;
}

static inline void
_unpack_memory_to_job(const void* data, size_t size, void* ud) {
	struct load_params* prev_params = (struct load_params*)ud;
	if (!_is_valid_version(prev_params->loader, prev_params->version)) {
		return;
	}

	struct parse_params* params = NULL;
	TASKS_QUEUE_POP(prev_params->loader->params_parse_queue, params);
	if (!params) {
		params = (struct parse_params*)malloc(sizeof(*params));
	}

	params->version = _get_version(prev_params->loader);

	char* buf = (char*)malloc(size);
	memcpy(buf, data, size);

	params->size = size;
	params->data = buf;

	params->parser_cb         = prev_params->parser_cb;
	params->release_cb = prev_params->release_cb;
	params->ud         = prev_params->parser_ud;

	struct job* job = NULL;
	TASKS_QUEUE_POP(prev_params->loader->job_free_queue, job);
	if (!job) {
		job = (struct job*)malloc(sizeof(*job));
	}
	job->type = JOB_PARSER_DATA;
	job->ud = params;
	memcpy(job->desc, prev_params->desc, sizeof(prev_params->desc));

	TASKS_QUEUE_PUSH(prev_params->loader->job_parse_queue, job);

	//logger_printf("async_load push 3, job: %p", job);
}

static int 
_need_quit(struct tasks_loader* loader)
{
	switch (pthread_mutex_trylock(&loader->quit_lock)) {
	/* if we got the lock, unlock and return 1 (true) */	
	case 0: 
		pthread_mutex_unlock(&loader->quit_lock);
		return 1;

	/* return 0 (false) if the mutex was locked */
	case EBUSY: 
		return 0;
	}
	return 1;
}

static inline void
trigger_dirty_event(struct tasks_loader* loader) {
	pthread_mutex_lock(&loader->dirty_lock);
	pthread_cond_broadcast(&loader->dirty_cv);
	pthread_mutex_unlock(&loader->dirty_lock);
}

static void*
_load_file(void* arg) {
	struct tasks_loader* loader = (struct tasks_loader*)arg;
	while (1) {
		pthread_mutex_lock(&loader->dirty_lock);

		if (_need_quit(loader)) {
			pthread_mutex_unlock(&loader->dirty_lock);
			break;
		}

		struct job* job = NULL;
		TASKS_QUEUE_POP(loader->job_load_queue, job);
		if (!job) {
			pthread_cond_wait(&loader->dirty_cv, &loader->dirty_lock);
			pthread_mutex_unlock(&loader->dirty_lock);
			continue;
		}
		pthread_mutex_unlock(&loader->dirty_lock);

		//logger_printf("async_load pop 2, job: %p", job);

		struct load_params* params = (struct load_params*)job->ud;
		if (_is_valid_version(loader, params->version)) {
			memcpy(params->desc, job->desc, sizeof(job->desc));
			params->load_cb(params->filepath, &_unpack_memory_to_job, params);
		}

		TASKS_QUEUE_PUSH(loader->params_load_queue, params);
		TASKS_QUEUE_PUSH(loader->job_free_queue, job);
	}
	return NULL;
}

struct tasks_loader*
tasks_loader_create(int thread_num) {
	struct tasks_loader* loader = (struct tasks_loader*)malloc(sizeof(*loader));

	TASKS_QUEUE_INIT(loader->job_free_queue);
	TASKS_QUEUE_INIT(loader->job_load_queue);
	TASKS_QUEUE_INIT(loader->job_parse_queue);
	TASKS_QUEUE_INIT(loader->params_load_queue);
	TASKS_QUEUE_INIT(loader->params_parse_queue);

	loader->version = 0;
	pthread_mutex_init(&loader->version_lock, 0);

	pthread_mutex_init(&loader->quit_lock, NULL);
	pthread_mutex_lock(&loader->quit_lock);

	pthread_mutex_init(&loader->dirty_lock, NULL);
	pthread_cond_init(&loader->dirty_cv, NULL);

	loader->thread_num = thread_num;
	for (int i = 0; i < thread_num; ++i) {
		pthread_create(&loader->threads[i], NULL, _load_file, loader);
	}

	return loader;
}

static void
_release_parse_params(void* data) {
	struct parse_params* params = (struct parse_params*)data;
	free(params->data);
	params->data = NULL;
}

void
tasks_loader_release(struct tasks_loader* loader) {
	TASKS_QUEUE_CLEAR(loader->job_free_queue, struct job);
	TASKS_QUEUE_CLEAR(loader->job_load_queue, struct job);
	TASKS_QUEUE_CLEAR(loader->job_parse_queue, struct job);
	TASKS_QUEUE_CLEAR(loader->params_load_queue, struct load_params);
	TASKS_QUEUE_CLEAR2(loader->params_parse_queue, struct parse_params, _release_parse_params);

	pthread_mutex_unlock(&loader->quit_lock); 
	trigger_dirty_event(loader);
	for (int i = 0; i < loader->thread_num; ++i) {
		pthread_join(loader->threads[i], NULL);
	}

	pthread_mutex_destroy(&loader->version_lock);
	pthread_mutex_destroy(&loader->quit_lock);
	pthread_mutex_destroy(&loader->dirty_lock);
	pthread_cond_destroy(&loader->dirty_cv);
}

void
tasks_loader_clear(struct tasks_loader* loader) {
	pthread_mutex_lock(&loader->version_lock);
	++loader->version;

	struct job* job = NULL;

	do {
		TASKS_QUEUE_POP(loader->job_load_queue, job);
		if (job) {
			//logger_printf("async_load clear pop job_load_queue, job: %p", job);
			struct load_params* params = (struct load_params*)job->ud;
			if (params->release_cb) {
				params->release_cb(params->parser_ud);
			}

			TASKS_QUEUE_PUSH(loader->job_free_queue, job);
		}
	} while (job);

	do {
		TASKS_QUEUE_POP(loader->job_parse_queue, job);
		if (job) {
			//logger_printf("async_load clear pop job_parse_queue, job: %p", job);
			struct parse_params* params = (struct parse_params*)job->ud;
			if (params->release_cb) {
				params->release_cb(params->ud);
			}

			free(params->data), params->data = NULL;
			params->size = 0;
			TASKS_QUEUE_PUSH(loader->params_parse_queue, params);
			TASKS_QUEUE_PUSH(loader->job_free_queue, job);
		}
	} while (job);

	pthread_mutex_unlock(&loader->version_lock);
}

void 
tasks_load_file(struct tasks_loader* loader, const char* filepath, 
                struct tasks_load_cb* cb, const char* desc) {
	struct load_params* params = NULL;
	TASKS_QUEUE_POP(loader->params_load_queue, params);
	if (!params) {
		params = (struct load_params*)malloc(sizeof(*params));
	}

	params->version = _get_version(loader);

	params->loader = loader;

	strcpy(params->filepath, filepath);
	params->filepath[strlen(filepath)] = 0;

	params->load_cb = cb->load;

	params->parser_cb  = cb->parser;
	params->release_cb = cb->release;
	params->parser_ud  = cb->parser_ud;

	struct job* job = NULL;
	TASKS_QUEUE_POP(loader->job_free_queue, job);
	if (!job) {
		job = (struct job*)malloc(sizeof(*job));
	}

	job->type = JOB_LOAD_FILE;
	job->ud = params;	
	strcpy(job->desc, desc);
	job->desc[strlen(job->desc)] = 0;

	TASKS_QUEUE_PUSH(loader->job_load_queue, job);

	trigger_dirty_event(loader);

	//logger_printf("async_load push 1, job: %p", job);

	// pthread_create(&job->id, NULL, _load_file, NULL);
}

void 
tasks_loader_update(struct tasks_loader* loader) {
	struct job* job = NULL;
	TASKS_QUEUE_POP(loader->job_parse_queue, job);
	if (!job) {
		return;
	}

	//logger_printf("async_load pop 4, job: %p", job);

	struct parse_params* params = (struct parse_params*)job->ud;

	if (_is_valid_version(loader, params->version) && params->parser_cb) {
		params->parser_cb(params->data, params->size, params->ud);
	}
	if (_is_valid_version(loader, params->version) && params->release_cb) {
		params->release_cb(params->ud);
	}

	free(params->data), params->data = NULL;
	params->size = 0;
	TASKS_QUEUE_PUSH(loader->params_parse_queue, params);
	TASKS_QUEUE_PUSH(loader->job_free_queue, job);
}

bool 
tasks_loader_empty(struct tasks_loader* loader) {
	return TASKS_QUEUE_EMPTY(loader->job_load_queue)
		&& TASKS_QUEUE_EMPTY(loader->job_parse_queue);
}
