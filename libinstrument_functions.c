/* * libinstrument-functions * Copyright © 2018 yiifburj
 * 
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <sys/mman.h>
#include <fcntl.h>           /* For O_* constants */
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include "libinstrument_functions.h"


#define FILENAME_SIZE	32
#define PROCESSESNAME_SIZE  (FILENAME_SIZE - sizeof POSTFIX + 1)
#define CONTROLLER "instument-functions_controller"
#define PROCESSES_NUM  (sizeof processes / sizeof processes[0])
#define SHM_SIZE  PROCESSES_NUM


static FILE *output;
static const char *controller;
static const char * const processes[]={
	"controller",
	"vtysh",
};

static int __attribute__ ((no_instrument_function)) get_process_name(char *buf, int buflen)
{
	char cmdbuf[32] = {0};
	snprintf(cmdbuf, sizeof cmdbuf, "/proc/%d/comm", getpid());
	int fd = open(cmdbuf, O_RDONLY);
	if(fd < 0)
	{
		perror(cmdbuf);
		return -1;
	}
	int unused = read(fd, buf, buflen-1);
	(void)unused;
	close(fd);
	buf[buflen-1] = 0;
	
	char *p = strchr(buf, '\n');
	if(p == NULL) 
		return -1;

	*p = 0;
	return 0;
}


static __attribute__ ((no_instrument_function)) void output_maps(FILE *fp_output)
{
	char cmdbuf[32] = {0};
	snprintf(cmdbuf, sizeof cmdbuf, "/proc/%d/maps", getpid());

	FILE *fp = fopen(cmdbuf, "r");
	if(fp == NULL)
	{
		fprintf(fp_output, "open error:%s\n",cmdbuf);
		return;
	}

	int l;
	char buf[128];
	while((l=fread(buf,1,sizeof buf, fp)) > 0)
	{
		fwrite(buf, 1, l, fp_output);
	}
	fclose(fp);
}

static __attribute__ ((no_instrument_function)) void open_file(void)
{
#define PREFIX "./"
#define POSTFIX ".log"
	char filename[FILENAME_SIZE] = PREFIX;
	if (get_process_name(filename+sizeof PREFIX - 1, PROCESSESNAME_SIZE - (sizeof PREFIX - 1)- (sizeof POSTFIX)) < 0) {
		return;
	}
	strcat(filename, POSTFIX);

	/* 多线程可能重复打开,需要原子变量操作,或者使用__thread变量 */
	output = fopen(filename, "a");
	if(output == NULL)
	{
		fprintf(stderr, "fopen failed %s\n", strerror(errno));
	}
	else
	{
		output_maps(output);
	}
};

static __attribute__ ((no_instrument_function)) void close_file(void)
{
	fclose(output);
	output = NULL;
}

void __cyg_profile_func_enter (void *func,  void *caller)
{
	if(*controller == OFF)
		return;
	if(output == NULL) {
		open_file();
	}
	if(output != NULL){
		fprintf(output, "e %p %p\n", func, caller);
	}
}

void __cyg_profile_func_exit (void *func, void *caller)
{
	if(*controller == OFF)
		return;
	if(*controller == TO_OFF)
	{
		close_file();
		*(char *)controller = OFF;
		return;
	}
	if(output != NULL) {
		fprintf(output, "x %p %p\n", func, caller);
	}
}


int __attribute__ ((no_instrument_function)) get_process_idx(const char *name)
{
	int i;
	for (i = 0; i < PROCESSES_NUM; ++i) {
		if(strcmp(name, processes[i]) == 0)
			return i;
	}
	fprintf(stderr, "%s not registered\n", name);
	return -1;
}

char * __attribute__ ((no_instrument_function)) get_map_addr_size(int prot, int *size)
{
	int new = 1;
	int fd = shm_open(CONTROLLER, O_RDWR|O_CREAT|O_EXCL, S_IRUSR|S_IWUSR|S_IROTH|S_IRGRP);
	if(fd < 0)
	{
		if(errno == EEXIST)
		{
			fd = shm_open(CONTROLLER, O_RDWR, S_IRUSR|S_IWUSR);
			if (fd < 0) {
				perror("shm_open");
				return NULL;
			}
			new = 0;
		}
		else
		{
			perror("shm_open:");
			return NULL;
		}
	}

	if(new)
	{
		if (ftruncate(fd, SHM_SIZE) < 0)
		{
			perror("ftruncate");
			close(fd);
			shm_unlink(CONTROLLER);
			return NULL;
		}
	}

	char *controller_map = mmap(NULL, SHM_SIZE, prot, MAP_SHARED, fd, 0);
	close(fd);
	if (controller_map == MAP_FAILED)
	{
		perror("mmap:");
		if(new)
		{
			shm_unlink(CONTROLLER);
		}
		return NULL;
	}


	*size = SHM_SIZE;

	return controller_map;
}
static char * __attribute__ ((no_instrument_function)) get_map_addr(int prot)
{
	int size;
	return get_map_addr_size(prot, &size);
}

static __attribute__((constructor, no_instrument_function))void library_constructor(void)
{
	static char unused=OFF;
	char name[PROCESSESNAME_SIZE];
	if(get_process_name(name, sizeof name) < 0)
	{
		controller = &unused;
		return;
	}

	int idx = get_process_idx(name);
	if(idx < 0)
	{
		controller = &unused;
		return;
	}

	char *addr = get_map_addr(PROT_READ);
	if(addr == NULL)
	{
		controller = &unused;
		return;
	}
	controller = &addr[idx];
}

