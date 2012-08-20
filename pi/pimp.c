/*	Pimp application
	by Rob Voisey of Fen Consultants, UK

	Copyright (c) 2012, Fen Consultants
	All rights reserved.

	Redistribution and use in source and binary forms, with or without
	modification, are permitted provided that the following conditions are met:
	* Redistributions of source code must retain the above copyright
	  notice, this list of conditions and the following disclaimer.
	* Redistributions in binary form must reproduce the above copyright
	  notice, this list of conditions and the following disclaimer in the
	  documentation and/or other materials provided with the distribution.
	* Neither the name of Fen Consultants nor the names of its contributors
	  may be used to endorse or promote products derived from this software
	  without specific prior written permission.

	THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
	ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
	WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
	DISCLAIMED. IN NO EVENT SHALL FEN CONSULTANTS BE LIABLE FOR ANY
	DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
	(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
	LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
	ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
	(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
	SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

	Project repository at https://github.com/fenconsultants/pimp
	More information at http://fenconsultants.com/blog
*/

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <termios.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <time.h>
#include <linux/serial.h>
#include <signal.h>

#define TTYNAME "/dev/ttyAMA0"
#define MAXPKTSIZE 16
#define MAXPORTS 8

/* Input and output ports (from / to the cloud) */

typedef struct
{
	unsigned int	id;
	char			*local;
	char			*type;
	char			*name;
} port;

port inputPorts[MAXPORTS];
port outputPorts[MAXPORTS];

/* Imp serial fd */

int impFd;

/* RX FSM state */

unsigned int rxState = 0;
unsigned int rxOffset = 0;
unsigned int rxCommand = 0;
unsigned int rxLength = 0;
unsigned char rxData[MAXPKTSIZE];

/*
 *	Send port list to imp
 */
void sendPortList()
{
	int i;
	char s[128];

	for(i=0; i<MAXPORTS; i++)
	{
		if(inputPorts[i].id != -1)
		{
			// Add input port <0xAA><0x80><Length><ID 0><ID 1><Type><0x09><Name>
			sprintf(s, "\xAA\x80%c%02d%s\x09%s", strlen(inputPorts[i].type) + strlen(inputPorts[i].name) + 2, inputPorts[i].id, inputPorts[i].type, inputPorts[i].name);
			write(impFd, s, strlen(s));
			fsync(impFd);
		}
		if(outputPorts[i].id != -1)
		{
			// Add output port <0xAA><0x81><Length><ID 0><ID 1><Type><0x09><Name>
			sprintf(s, "\xAA\x81%c%02d%s\x09%s", strlen(outputPorts[i].type) + strlen(outputPorts[i].name) + 2, outputPorts[i].id, outputPorts[i].type, outputPorts[i].name);
			write(impFd, s, strlen(s));
			fsync(impFd);
		}
	}
}

/*
 *	Process a command received from imp
 */
void command()
{
	char	ids[3];
	int	id;

	switch(rxCommand)
	{
		case 0 : /* Probe */
			write(impFd, "\x55", 1);
			fsync(impFd);
			sendPortList();
			break;

		case 1 : /* Process received data */
			/* Get port ID */
			memcpy(ids, rxData, 2);
			ids[2] = '\0';
			id = atoi(ids);

			/* Validate ID */
			if(id >= MAXPORTS) break;
			if(inputPorts[id].id == -1) break;

			/* Process data */
			if(strcmp(inputPorts[id].local, "console") == 0)
			{
				/* Echo to stdout */
				printf("Received: [%02d] %s\n", id, rxData + 2);
			}
			else
			{
				/* TODO tty and tcp ports */
			}
			break;

		default : /* Anything else is invalid */
			break;
	}
}

/*
 *	Process a byte received from imp
 */
void rxFsm(char c)
{
	switch(rxState)
	{
		case 0 : /* Waiting for header */
			if(c == 0xAA) rxState = 1;
			break;

		case 1 : /* Waiting for command */
			switch(c)
			{
				case 0x00 : /* Probe */
					rxCommand = 0x00;
					command();
					rxState = 0;
					break;

				case 0x01 : /* Received data from cloud */
					rxCommand = 0x01;
					rxState = 2;
					break;

				default: // Anything else is invalid
					fprintf(stderr, "pimp: Invalid command 0x%02X received from imp.\n", c);
					rxState = 0;
					break;
			}
			break;

		case 2 : // Waiting for data length
			rxLength = c;
			rxOffset = 0;
			rxState = 3;
			break;

		case 3 : // Receiving data
			rxData[rxOffset] = c;
			if(rxOffset++ == rxLength-1)
			{
				// Data complete
				command();
				memset(rxData, '\0', MAXPKTSIZE);
				rxState = 0;
			}
			break;
	}
}

/*
 *	Return an array of argument pointers from the given tab separated string
 */
int splitArgs(char *src, char **args)
{
	char	*split;
	int		i=1;

	while(*src == '\t') src++;
	if(*src == '\0' || *src == '#') return 0;

	args[0] = src;

	while((split = strchr(src, '\t')) != NULL && i < 5)
	{
		*split++ = '\0';
		while(*split == '\t') split++;
		src = args[i++] = split;
	}

	return i;
}

/*
 *	Load input & output port list configuration
 */
void loadPortLists()
{
	FILE	*fptr;
	char	s[128], *arg[5];
	int		i, nargs, inPort = 0, outPort = 0;

	if(fptr = fopen("portlist", "r"), fptr == NULL)
	{
		fprintf(stderr, "pimp: Missing portlist file, no ports configured\n");
		return;
	}

	/* Initialise all ports inactive */
	for(i=0; i<MAXPORTS; i++) inputPorts[i].id = outputPorts[i].id = -1;

	/* Parse portlist file */

	while(!feof(fptr))
	{
		if(fgets(s, 128, fptr))
		{
			/* Remove the end of line */
			strtok(s, "\n");

			/* Separate arguments */
			if(splitArgs(s, arg) == 5)
			{
				if(strcmp(arg[0], "input") == 0)
				{
					/* Add an input port */

					inputPorts[inPort].id = atoi(arg[1]);

					inputPorts[inPort].local = malloc(strlen(arg[2]) + 1);
					strcpy(inputPorts[inPort].local, arg[2]);

					inputPorts[inPort].type = malloc(strlen(arg[3]) + 1);
					strcpy(inputPorts[inPort].type, arg[3]);

					inputPorts[inPort].name = malloc(strlen(arg[4]) + 1);
					strcpy(inputPorts[inPort].name, arg[4]);

					printf("Input  %02d %-15s [ %-7s] %s\n", inputPorts[inPort].id, inputPorts[inPort].local, inputPorts[inPort].type, inputPorts[inPort].name);

					inPort++;
				}
				else if(strcmp(arg[0], "output") == 0)
				{
					/* Add an output port */

					outputPorts[outPort].id = atoi(arg[1]);

					outputPorts[outPort].local = malloc(strlen(arg[2]) + 1);
					strcpy(outputPorts[outPort].local, arg[2]);

					outputPorts[outPort].type = malloc(strlen(arg[3]) + 1);
					strcpy(outputPorts[outPort].type, arg[3]);

					outputPorts[outPort].name = malloc(strlen(arg[4]) + 1);
					strcpy(outputPorts[outPort].name, arg[4]);

					printf("Output %02d %-15s [ %-7s] %s\n", outputPorts[outPort].id, outputPorts[outPort].local, outputPorts[outPort].type, outputPorts[outPort].name);

					outPort++;
				}
				else
				{
					fprintf(stderr, "pimp: Invalid port specification '%s'.\n", arg[0]);
				}
			}
		}
	}

	fclose(fptr);
}

/*
 *  Entry point and main loop
 */
int main(int argc, char **argv)
{
	fd_set				fds;
	struct timespec		ts;
	int					retVal;
	int					b;
	unsigned char		st[2];
	int					fdMax = 0;
	struct termios		oldtio, newtio;

	printf("Pimp starting up...\n");

	/* Load config */

	loadPortLists();

	/* Open port */

	if(impFd = open(TTYNAME, O_RDWR | O_NOCTTY | O_NONBLOCK | O_NDELAY), impFd < 0)
	{
		fprintf(stderr, "pimp: Failed to open port %s\n", TTYNAME);
		exit(-1);
	}

	tcgetattr(impFd, &oldtio);
	bzero(&newtio, sizeof(newtio));

	newtio.c_cflag = B115200 | CS8 | CLOCAL | CREAD;
	newtio.c_iflag = IGNPAR;
	newtio.c_oflag = 0;
	newtio.c_lflag = 0;
	newtio.c_cc[VMIN]  = 0;
	newtio.c_cc[VTIME] = 0;

	tcflush(impFd, TCIFLUSH);
	tcsetattr(impFd,TCSANOW,&newtio);

	if(impFd > fdMax) fdMax = impFd;

	/* Initialise polling */

	ts.tv_sec = 0;
	ts.tv_nsec = 10000000;

	printf("Pimp ready.\n");

	/* Main loop */

	while(1)
	{
		/* Block on serial Rx with 10ms timeout */

		FD_ZERO(&fds);
		FD_SET(impFd, &fds);
		retVal = pselect(fdMax + 1, &fds, NULL, NULL, &ts, NULL);

		if(retVal == -1)
		{
			fprintf(stderr, "pimp: Error on select()\n");
			exit(-1);
		}
		else if(retVal && FD_ISSET(impFd, &fds))
		{
			/* Drain serial Rx buffer */
			do
			{
				b = read(impFd, st, 1);
				if(b > 0) rxFsm(st[0]);
			}
			while(b > 0);
		}
	}
}
