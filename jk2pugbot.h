#ifndef _MYIRCBOT_H_
#define _MYIRCBOT_H_

#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>

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
