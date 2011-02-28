/*
 * syst.c - example of a simple system wide monitoring program
 *
 * Copyright (c) 2010 Google, Inc
 * Contributed by Stephane Eranian <eranian@google.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A
 * PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF
 * CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE
 * OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#include <sys/types.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <err.h>
#include <locale.h>
#include <sys/ioctl.h>
#include <time.h>

#include "perf_util.h"

#define MAX_GROUPS	16
#define MAX_PATH	1024

#ifndef STR
# define _STR(x) #x
# define STR(x) _STR(x)
#endif

typedef struct {
	const char *events[MAX_GROUPS];
	int nevents[MAX_GROUPS]; /* #events per group */
	int num_groups;
	int delay;
	int excl;
	int pin;
	int interval;
	int cpu;
	char *cgroup_name;
} options_t;

static options_t options;
static perf_event_desc_t **all_fds;

static const char
*cgroupfs_find_mountpoint(void)
{
	static char cgroup_mountpoint[MAX_PATH+1];
	FILE *fp;
	int found = 0;
	char type[64];

	fp = fopen("/proc/mounts", "r");
	if (!fp)
		return NULL;

	while (fscanf(fp, "%*s %"
				STR(MAX_PATH)
				"s %99s %*s %*d %*d\n",
				cgroup_mountpoint, type) == 2) {

		found = !strcmp(type, "cgroup");
		if (found)
			break;
	}
	fclose(fp);

	return found ? cgroup_mountpoint : NULL;
}

int
open_cgroup(char *name)
{
	char path[MAX_PATH+1];
	const char *mnt;
	int cfd;

	mnt = cgroupfs_find_mountpoint();
	if (!mnt)
		errx(1, "cannot find cgroup fs mount point");

	snprintf(path, MAX_PATH, "%s/%s", mnt, name);

	cfd = open(path, O_RDONLY);
	if (cfd == -1)
		warn("no access to cgroup %s\n", name);

	return cfd;
}

void
setup_cpu(int cpu, int cfd)
{
	perf_event_desc_t *fds = NULL;
	int old_total, total = 0, num;
	int i, j, n, ret, is_lead, group_fd;
	unsigned long flags;
	pid_t pid;

	for(i=0, j=0; i < options.num_groups; i++) {
		old_total = total;
		ret = perf_setup_list_events(options.events[i], &fds, &total);
		if (ret)
			errx(1, "cannot setup events\n");

		all_fds[cpu] = fds;

		num = total - old_total;

		options.nevents[i] = num;

		for(n=0; n < num; n++, j++) {

			is_lead = perf_is_group_leader(fds, j);
			if (is_lead) {
				fds[j].hw.disabled = 1;
				group_fd = -1;
			} else {
				fds[j].hw.disabled = 0;
				group_fd = fds[fds[j].group_leader].fd;
			}
			fds[j].hw.size = sizeof(struct perf_event_attr);

			if (options.cgroup_name) {
				flags = PERF_FLAG_PID_CGROUP;
				pid = cfd;
				//fds[j].hw.cgroup = 1;
				//fds[j].hw.cgroup_fd = cfd;
			} else {
				flags = 0;
				pid = -1;
			}

			if (options.pin && is_lead)
				fds[j].hw.pinned = 1;

			if (options.excl && is_lead)
				fds[j].hw.exclusive = 1;

			/* request timing information necessary for scaling counts */
			fds[j].hw.read_format = PERF_FORMAT_SCALE;
			fds[j].fd = perf_event_open(&fds[j].hw, pid, cpu, group_fd, flags);
			if (fds[j].fd == -1) {
				if (errno == EACCES)
					err(1, "you need to be root to run system-wide on this machine");

				warn("cannot attach event %s to CPU%ds, skipping it", fds[j].name, cpu);
				goto error;
			}
		}
	}
	return;
error:
	for (i=0; i < j; i++) {
		close(fds[i].fd);
		fds[i].fd = -1;
	}
}

void start_cpu(int c)
{
	perf_event_desc_t *fds = NULL;
	int j, ret, n = 0;

	fds = all_fds[c];

	if (fds[0].fd == -1)
		return;

	for(j=0; j < options.num_groups; j++) {
		/* group leader always first in each group */
		ret = ioctl(fds[n].fd, PERF_EVENT_IOC_ENABLE, 0);
		if (ret)
			err(1, "cannot enable event %s\n", fds[j].name);
		n += options.nevents[j];
	}
}

void stop_cpu(int c)
{
	perf_event_desc_t *fds = NULL;
	int j, ret, n = 0;

	fds = all_fds[c];

	if (fds[0].fd == -1)
		return;

	for(j=0; j < options.num_groups; j++) {
		/* group leader always first in each group */
		ret = ioctl(fds[n].fd, PERF_EVENT_IOC_DISABLE, 0);
		if (ret)
			err(1, "cannot disable event %s\n", fds[j].name);
		n += options.nevents[j];
	}
}

void read_cpu(int c)
{
	perf_event_desc_t *fds;
	uint64_t values[3];
	double ratio;
	int i, j, n, ret;

	fds = all_fds[c];

	if (fds[0].fd == -1) {
		printf("CPU%d not monitored\n", c);
		return;
	}

	for(i=0, j = 0; i < options.num_groups; i++) {
		for(n = 0; n < options.nevents[i]; n++, j++) {
			memset(values, 0, sizeof(values));
			ret = read(fds[j].fd, values, sizeof(values));
			if (ret != sizeof(values)) {
				if (ret == -1)
					err(1, "cannot read event %s : %d", fds[j].name, ret);
				else {
					warnx("CPU%d G%-2d could not read event %s, read=%d", c, i, fds[j].name, ret);
					continue;
				}
			}
			/*
			 * scaling because we may be sharing the PMU and
			 * thus may be multiplexed
			 */
			fds[j].value = perf_scale(values);
			ratio = perf_scale_ratio(values);

			printf("CPU%-3d G%-2d %'-20"PRIu64" %s (scaling %.2f%%, ena=%'"PRIu64", run=%'"PRIu64") %s\n",
				c,
				i,
				fds[j].value,
				fds[j].name,
				(1.0-ratio)*100,
				values[1],
				values[2],
				options.cgroup_name ? options.cgroup_name : "");
	if (values[2] > values[1])
		errx(1, "WARNING: time_running > time_enabled %"PRIu64"\n", values[2] - values[1]);
		}
	}
}

void close_cpu(int c)
{
	perf_event_desc_t *fds = NULL;
	int i, j;

	fds = all_fds[c];

	if (fds[0].fd == -1)
		return;

	for(i=0; i < options.num_groups; i++) {
		for(j=0; j < options.nevents[i]; j++)
			close(fds[j].fd);
	}

	free(fds);
}

void
measure(void)
{
	int c, cmin, cmax, ncpus;
	int cfd = -1;

	cmin = 0;
	cmax = (int)sysconf(_SC_NPROCESSORS_ONLN);
	ncpus = cmax;

	if (options.cpu != -1) {
		cmin = options.cpu;
		cmax = cmin + 1;
	}

	all_fds = malloc(ncpus * sizeof(perf_event_desc_t *));
	if (!all_fds)
		err(1, "cannot allocate memory for all_fds");

	if (options.cgroup_name) {
		cfd = open_cgroup(options.cgroup_name);
		if (cfd == -1)
			exit(1);
	}

	for(c=cmin ; c < cmax; c++)
		setup_cpu(c, cfd);

	printf("<press CTRL-C to quit before %ds time limit>\n", options.delay);
	/*
	 * FIX this for hotplug CPU
	 */

	if (options.interval) {
		struct timespec tv;
		int delay;

		for (delay = 1 ; delay <= options.delay; delay++) {

		for(c=cmin ; c < cmax; c++)
			start_cpu(c);

			if (0) {
				tv.tv_sec = 0;
				tv.tv_nsec = 100000000;
				nanosleep(&tv, NULL);
			} else
				sleep(1);

		for(c=cmin ; c < cmax; c++)
			stop_cpu(c);

			for(c = cmin; c < cmax; c++) {
				printf("# %'ds -----\n", delay);
				read_cpu(c);
			}

		}

	} else {
		for(c=cmin ; c < cmax; c++)
			start_cpu(c);

		sleep(options.delay);

		if (0)
			for(c=cmin ; c < cmax; c++)
				stop_cpu(c);

		for(c = cmin; c < cmax; c++) {
			printf("# -----\n");
			read_cpu(c);
		}
	}

	for(c = cmin; c < cmax; c++)
		close_cpu(c);

	free(all_fds);
}

static void
usage(void)
{
	printf("usage: syst [-c cpu] [-x] [-h] [-p] [-d delay] [-P] [-G cgroup name] [-e event1,event2,...]\n");
}

int
main(int argc, char **argv)
{
	int c, ret;

	setlocale(LC_ALL, "");

	options.cpu = -1;

	while ((c=getopt(argc, argv,"hc:e:d:xPpG:")) != -1) {
		switch(c) {
			case 'x':
				options.excl = 1;
				break;
 			case 'p':
 				options.interval = 1;
 				break;
			case 'e':
				if (options.num_groups < MAX_GROUPS) {
					options.events[options.num_groups++] = optarg;
				} else {
					errx(1, "you cannot specify more than %d groups.\n",
						MAX_GROUPS);
				}
				break;
			case 'c':
				options.cpu = atoi(optarg);
				break;
			case 'd':
				options.delay = atoi(optarg);
				break;
			case 'P':
				options.pin = 1;
				break;
			case 'h':
				usage();
				exit(0);
			case 'G':
				options.cgroup_name = optarg;
				break;
			default:
				errx(1, "unknown error");
		}
	}
	if (!options.delay)
		options.delay = 20;

	if (!options.events[0]) {
		options.events[0] = "PERF_COUNT_HW_CPU_CYCLES,PERF_COUNT_HW_INSTRUCTIONS";
		options.num_groups = 1;
	}

	ret = pfm_initialize();
	if (ret != PFM_SUCCESS)
		errx(1, "libpfm initialization failed: %s\n", pfm_strerror(ret));
	
	measure();

	/* free libpfm resources cleanly */
	pfm_terminate();

	return 0;
}