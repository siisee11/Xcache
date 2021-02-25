#ifndef TIMEPERF_H
#define TIMEPERF_H
#include <linux/jiffies.h>
#include <linux/string.h>
#include <linux/delay.h> 
#include <linux/ktime.h>
#include <linux/module.h> 
#include <linux/kernel.h> 
#include <linux/hrtimer.h> 

#define MAX_FUNC 20
struct fperf {
	const char *str;
	ktime_t elapsed;
	ktime_t start;
	uint64_t count;
};

static struct fperf fperf_arr[MAX_FUNC];

int fperf_num = 0;
inline void fperf_start(const char * str) {
	int i;
	struct fperf *fpp = NULL;
	for (i = 0; i < fperf_num; i++) {
		if(strcmp(fperf_arr[i].str, str) == 0) {
			fpp = &fperf_arr[i];
			break;
		}
	}
	if(fpp == NULL) {
		fperf_num++;
		fpp = &fperf_arr[fperf_num-1];
		fpp->str = str;
	}
	fpp->start = ktime_get();
	fpp->count++;
}

inline void fperf_end(const char * str) {
	int i;
	ktime_t now;
	struct fperf *fpp = NULL;
	for (i = 0; i < fperf_num; ++i) {
		if(strcmp(fperf_arr[i].str, str) == 0) {
			fpp = &fperf_arr[i];
			break;
		}
	}
	now = ktime_get();
	if(fpp != NULL) {
		fpp->elapsed += ktime_to_us(ktime_sub(now, fpp->start));
	}
}

inline void fperf_save(const char * str, ktime_t t) {
	int i;
	ktime_t now;
	struct fperf *fpp = NULL;
	for (i = 0; i < fperf_num; ++i) {
		if(strcmp(fperf_arr[i].str, str) == 0) {
			fpp = &fperf_arr[i];
			break;
		}
	}
	if(fpp == NULL) {
		fperf_num++;
		fpp = &fperf_arr[fperf_num-1];
		fpp->str = str;
	}
	if(fpp != NULL) {
		fpp->elapsed += t;
		fpp->count++;
	}
}

inline void fperf_print(const char *str) {
	int i;
	struct fperf *fpp = NULL;
	for (i = 0; i < fperf_num; ++i) {
		if(strcmp(fperf_arr[i].str, str) == 0) {
			fpp = &fperf_arr[i];
			break;
		}
	}
	if(fpp != NULL) {
		pr_info("[ %s ]: average %lld nsec \n", str, fpp->elapsed/fpp->count);
	}
}
#endif
