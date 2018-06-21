#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>

void pty_main(char *command)
{
	signal(SIGINT, NULL);
	setsid();
	if (execl("/bin/sh", command, NULL) < 0)
	{
		puts("SH failed!");
		exit(-1);
	}
	exit(0);
}
