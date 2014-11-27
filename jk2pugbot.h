/*
   Copyright 2014 Witold Piłat

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef _MYIRCBOT_H_
#define _MYIRCBOT_H_

#include <assert.h>
#include <netdb.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>


#define BUFFER_SIZE 513

struct prefix_s {
	union {
		char *servername;
		char *nick;
	};
	char *user;
	char *host;
};

typedef struct message_s {
	struct prefix_s prefix;
	char *command;
	char *parameter[14];
	char *trailing;
} message_t;

typedef struct player_s {
	char *nick;
} player_t;

typedef struct playerNode_s {
	player_t *player;
	struct playerNode_s *next;
} playerNode_t;

typedef struct pickup_s {
	const char *name;
	int count;
	int max;
	struct playerNode_s *playerList;
} pickup_t;

typedef struct pickupNode_s {
	pickup_t *pickup;
	struct pickupNode_s *next;
} pickupNode_t;

void announcePickup(pickup_t *pickup);

#endif // _MYIRCBOT_H_
