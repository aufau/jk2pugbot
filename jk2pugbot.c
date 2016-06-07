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

#include "jk2pugbot.h"

/*
 * Configuration
 */

const char * const	botVersion	= "beta";
const char * const	botHost		= "irc.quakenet.org";
const char * const	botPort		= "6667";
const char * const	botNick		= "JK2PUGBOT";
const char * const	botChannel	= "#jk2pugbot";
const char * const	botTopic	= "Welcome to #jk2pugbot";
const char * const	botQpassword	= NULL;	// Password to auth with Q or NULL
const long	botDelay	= 50000;	// Between messages. In microseconds
const int	botTimeout	= 300;		// Try to reconnect after this number of seconds
const bool	botSilentWho	= true;		// Don't announce players in the main channel

pickup_t pickupsArray[] = {
	{ .name = "MB", .max = 6 },
	{ .name = "CTF", .max = 6 },
	{ .name = "4v4", .max = 8 },
	{ .name = "2v2", .max = 4 },
	{ .name = "duel", .max = 0 },
};

// These servers will be recommended when announcing a pickup game.
// If your game doesn't use quake 3 engine then don't set the .type variable.
server_t serversArray[] = {
	{ .name = "zedi", .address = "185.44.107.108", .port = "28051", .games = "2v2 4v4", .type = SV_Q3 },
	{ .name = "FC League", .address = "force-crusaders.org", .port = "28071", .games = "2v2 4v4", .type = SV_Q3 },
	{ .name = "[united] Coruscant", .address = "185.44.107.108", .port = "28070", .games = "CTF", .type = SV_Q3 },
	{ .name = "jk2.ouned.de", .address = "185.44.107.108", .port = "28071", .games = "CTF", .type = SV_Q3 },
};

/*
 * Code
 */

struct {
	int conn;		// irc server socket file descriptor
	char sbuf[MAX_MSG_LEN];	// send buffer
	bool statusChanged;

	pickupNode_t *allPickups;
	playerNode_t *nickList;
} bot;

void sbufRaw(void)
{
	time_t epochTime;
	struct tm *locTime;
	struct timeval timeout = {
		.tv_sec = 0,
		.tv_usec = botDelay
	};

	time(&epochTime);
	locTime = localtime(&epochTime);
	printf("%02d:%02d << %s", locTime->tm_hour, locTime->tm_min, bot.sbuf);
#ifndef DEBUG_INTERCEPT
	if (write(bot.conn, bot.sbuf, strlen(bot.sbuf)) == -1)
		perror("sbufRaw write: ");
#endif
	select(0, NULL, NULL, NULL, &timeout);
}

void raw(char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(bot.sbuf, sizeof(bot.sbuf), fmt, ap);
	va_end(ap);
	sbufRaw();
}

void *smalloc(size_t size)
{
	void *retval = malloc(size);
	assert(retval);
	return retval;
}

void sigHandler(int signum)
{
	raw("QUIT :SIGINT\r\n");
	close(bot.conn);
	exit(EXIT_SUCCESS);
}

int getQ3ServerInfo(server_t *server, q3serverInfo_t *info)
{
	char svbuf[MAX_Q3_INFO_LEN + 1];
	struct addrinfo hints;
	struct addrinfo *res;
	struct timeval timeout;
	const char *getinfo = "\xFF\xFF\xFF\xFF\x02getinfo\x0a\x00";
	char	*ptr;
	fd_set	set;
	int	readlen;
	int	sv_sock;

	memset(&hints, 0, sizeof(struct addrinfo));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_DGRAM;
	if (getaddrinfo(server->address, server->port, &hints, &res))
		return 0;
	sv_sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
	if (sv_sock == -1)
		return -1;

	sendto(sv_sock, getinfo, strlen(getinfo) + 1, 0, res->ai_addr, res->ai_addrlen);
	freeaddrinfo(res);

	FD_ZERO(&set);
	FD_SET(sv_sock, &set);
	timeout.tv_sec = 1;
	timeout.tv_usec = 0;
	if (select(sv_sock + 1, &set, NULL, NULL, &timeout) != 1) {
		close(sv_sock);
		return 0;
	}

	readlen = read(sv_sock, svbuf, MAX_Q3_INFO_LEN);
	close(sv_sock);
	if (readlen == -1)
		return -1;

	svbuf[readlen] = '\0';
	ptr = strtok(svbuf, "\\");
	info->maxclients = 0;
	info->clients = 0;
	do {
		if (!strcasecmp(ptr, "sv_maxclients"))
			info->maxclients += atoi(strtok(NULL, "\\"));
		else if (!strcasecmp(ptr, "sv_privateclients"))
			info->maxclients -= atoi(strtok(NULL, "\\"));
		else if (!strcasecmp(ptr, "clients"))
			info->clients = atoi(strtok(NULL, "\\"));

		ptr = strtok(NULL, "\\");
	} while (ptr);

	if (info->maxclients)
		return 1;
	else
		return 0;
}


/* List manipulation
 * functions
 */

pickupNode_t *pushPickup(pickupNode_t *node, pickup_t *pickup)
{
	pickupNode_t *pickupNode = smalloc(sizeof(pickupNode_t));
	pickupNode->pickup = pickup;
	pickupNode->next = node;
	return pickupNode;
}

pickupNode_t *popPickup(pickupNode_t *node)
{
	if (node) {
		pickupNode_t *retval = node->next;
		free(node);
		return retval;
	}
	return NULL;
}

pickupNode_t *cleanPickupList(pickupNode_t *node)
{
	if (!node)
		return NULL;
	else
		return cleanPickupList(popPickup(node));
}

playerNode_t *popPlayer(playerNode_t *node)
{
	if (node) {
		playerNode_t *retval = node->next;
		free(node);
		return retval;
	}
	return NULL;
}

player_t *pushNick(const char *nick)
{
	playerNode_t *playerNode = smalloc(sizeof(playerNode_t));
	player_t *player = smalloc(sizeof(player_t));
	player->nick = smalloc(strlen(nick) + 1);
	strcpy(player->nick, nick);
	playerNode->player = player;
	playerNode->next = bot.nickList;
	bot.nickList = playerNode;
	return player;
}

playerNode_t *pushPlayer(playerNode_t *node, player_t *player)
{
	playerNode_t *playerNode = smalloc(sizeof(playerNode_t));
	playerNode->player = player;
	playerNode->next = node;
	return playerNode;
}

serverNode_t *pushServer(serverNode_t *node, server_t *server)
{
	serverNode_t *serverNode = smalloc(sizeof(serverNode_t));
	serverNode->server = server;
	serverNode->next = node;
	return serverNode;
}

void addServer(pickupNode_t *node, server_t *server)
{
	if (node) {
		node->pickup->serverList = pushServer(node->pickup->serverList, server);
		addServer(node->next, server);
	}
}

/* cutPlayer
 * move player to the top of a list or return null
 */

playerNode_t *cutPlayerH(playerNode_t *node, player_t *player)
{
	if (!node->next) {
		return NULL;
	} else {
		if (node->next->player == player) {
			playerNode_t *playerNode = node->next;
			node->next = node->next->next;
			return playerNode;
		} else {
			return cutPlayerH(node->next, player);
		}
	}
}

playerNode_t *cutPlayer(playerNode_t *node, player_t *player)
{
	if (node) {
		if (node->player == player)
			return node;

		playerNode_t *playerNode = cutPlayerH(node, player);
		if (playerNode)
			playerNode->next = node;
		return playerNode;
	}
	return NULL;
}

player_t *findNickH(playerNode_t *node, const char *nick)
{
	if (!node)
		return NULL;
	else if (!strcmp(node->player->nick, nick))
		return node->player;
	else
		return findNickH(node->next, nick);
}

player_t *findNick(const char *nick)
{
	return findNickH(bot.nickList, nick);
}

int countPlayers(playerNode_t *node)
{
	if (!node)
		return 0;
	else
		return countPlayers(node->next) + 1;
}

void removePlayer(pickupNode_t *node, player_t *player)
{
	if (node) {
		playerNode_t *playerNode = cutPlayer(node->pickup->playerList, player);
		if (playerNode) {
			node->pickup->playerList = popPlayer(playerNode);
			node->pickup->count--;
			bot.statusChanged = true;
		}
		removePlayer(node->next, player);
	}
}

void removeNick(pickupNode_t *node, const char *nick)
{
	player_t *player = findNick(nick);
	if (player) {
		removePlayer(node, player);
	}
}

void purgePlayer(player_t *player)
{
	removePlayer(bot.allPickups, player);
	bot.nickList = popPlayer(cutPlayer(bot.nickList, player));
	free(player->nick);
	free(player);
}

void purgePlayers(playerNode_t *node)
{
	if (node) {
		playerNode_t *playerNode = node->next;
		purgePlayer(node->player);
		purgePlayers(playerNode);
	}
}

void purgeNick(const char *nick)
{
	player_t *player = findNick(nick);
	if (player)
		purgePlayer(player);
}

void addPlayer(pickupNode_t *node, player_t *player)
{
	if (node) {
		playerNode_t *playerNode = cutPlayer(node->pickup->playerList, player);
		if (playerNode) {
			node->pickup->playerList = playerNode;
		} else {
			node->pickup->playerList = pushPlayer(node->pickup->playerList, player);
			node->pickup->count++;
			bot.statusChanged = true;
			if (node->pickup->max && node->pickup->count == node->pickup->max) {
				announcePickup(node->pickup);
				purgePlayers(node->pickup->playerList);
				return;
			}
		}
		addPlayer(node->next, player);
	}
}

void addNick(pickupNode_t *node, const char *nick)
{
	player_t *player = findNick(nick);
	if (!player && node)
		player = pushNick(nick);

	addPlayer(node, player);
}

void changeNick(const char *nick, const char *newnick)
{
	player_t *player = findNick(nick);
	if (player) {
		free(player->nick);
		player->nick = smalloc(strlen(newnick) + 1);
		strcpy(player->nick, newnick);
	}
}

int printPlayers(playerNode_t *node, int pos, const char *sep)
{
	if (!node) {
		return pos;
	} else {
		if (!node->next)
			sep = "";
		pos += snprintf(bot.sbuf + pos, sizeof(bot.sbuf) - pos, "%s%s", node->player->nick, sep);
		return printPlayers(node->next, pos, sep);
	}
}

int printPickups(pickupNode_t *node, int pos)
{
	if (!node) {
		return pos;
	} else {
		const char *formatString;

		if (node->pickup->max) {
			formatString = node->pickup->count ?
				"\x02(\x02 %s %d/%d \x02)\x02" :
				"( %s %d/%d )";
			pos += snprintf(bot.sbuf + pos, sizeof(bot.sbuf) - pos, formatString,
					node->pickup->name,
					node->pickup->count,
					node->pickup->max);
		} else {
			formatString = node->pickup->count ?
				"\x02(\x02 %s %d \x02)\x02" :
				"( %s %d )";
			pos += snprintf(bot.sbuf + pos, sizeof(bot.sbuf) - pos, formatString,
					node->pickup->name,
					node->pickup->count);
		}
		return printPickups(node->next, pos);
	}
}

int printServers(serverNode_t *node, int pos)
{
	q3serverInfo_t q3serverInfo;

	if (!node) {
		return pos;
	} else {
		int retVal = 0;

		if (node->server->type == SV_Q3)
			retVal = getQ3ServerInfo(node->server, &q3serverInfo);

		if (retVal == 1) {
			pos += snprintf(bot.sbuf + pos, sizeof(bot.sbuf) - pos,
					"\x02(\x02 %s %d/%d %s:%s \x02)\x02",
					node->server->name, q3serverInfo.clients,
					q3serverInfo.maxclients, node->server->address,
					node->server->port);
		} else if (retVal == -1) {
			pos += snprintf(bot.sbuf + pos, sizeof(bot.sbuf) - pos,
					"\x02(\x02 %s %s:%s \x02)\x02",
					node->server->name, node->server->address,
					node->server->port);
		}
		return printServers(node->next, pos);
	}
}


void updateStatus()
{
	int pos;

	pos = snprintf(bot.sbuf, sizeof(bot.sbuf), "TOPIC %s :", botChannel);
	pos = printPickups(bot.allPickups, pos);
	snprintf(bot.sbuf + pos, sizeof(bot.sbuf) - pos,
		 "\x02(\x02 %s \x02)(\x02 Type !help for more info \x02)\x02\r\n", botTopic);
	sbufRaw();
	bot.statusChanged = false;
}

void announceServers(pickupNode_t *node, const char *to)
{
	int pos;

	if (node) {
		if (node->pickup->serverList) {
			pos = snprintf(bot.sbuf, sizeof(bot.sbuf),
				       "PRIVMSG %s :Recommended %s servers: ",
				       to, node->pickup->name);
			pos = printServers(node->pickup->serverList, pos);
			snprintf(bot.sbuf + pos, sizeof(bot.sbuf) - pos, "\r\n");
			sbufRaw();
		}

		announceServers(node->next, to);
	}
}

void announcePickup(pickup_t *pickup)
{
	int pos;
	pickupNode_t *node;

	pos = snprintf(bot.sbuf, sizeof(bot.sbuf), "PRIVMSG ");
	pos = printPlayers(pickup->playerList, pos, ",");
	pos += snprintf(bot.sbuf + pos, sizeof(bot.sbuf) - pos,
		 ",%s :\x02%s pickup is ready to start!\x02 Players are: ",
		 botChannel, pickup->name);
	pos = printPlayers(pickup->playerList, pos, ", ");
	snprintf(bot.sbuf + pos, sizeof(bot.sbuf) - pos, "\r\n");
	sbufRaw();

	node = pushPickup(NULL, pickup);
	announceServers(node, botChannel);
	popPickup(node);
}

void announcePlayers(pickupNode_t *node, const char *to)
{
	if (node) {
		if (node->pickup->count) {
			int pos;

			if (node->pickup->max) {
				pos = snprintf(bot.sbuf, sizeof(bot.sbuf),
					       "PRIVMSG %s :\x02(\x02 %s %d/%d \x02)\x02 Players are: ",
					       to, node->pickup->name,
					       node->pickup->count, node->pickup->max);
			} else {
				pos = snprintf(bot.sbuf, sizeof(bot.sbuf),
					       "PRIVMSG %s :\x02(\x02 %s %d \x02)\x02 Players are: ",
					       to, node->pickup->name,
					       node->pickup->count);
			}

			pos = printPlayers(node->pickup->playerList, pos, ", ");
			snprintf(bot.sbuf + pos, sizeof(bot.sbuf) - pos, "\r\n");
			sbufRaw();
		}
		announcePlayers(node->next, to);
	}
}

void promotePickup(pickupNode_t *node)
{
	const char *pluralSuffix;
	const char *beForm;
	pickupNode_t *pickupNode;

	if (node) {
		if (node->pickup->count == 1) {
			pluralSuffix = "";
			beForm = "is";
		} else {
			pluralSuffix = "s";
			beForm = "are";
		}

		if (node->pickup->max && node->pickup->count) {
			raw("PRIVMSG %s :\x02Only %d player%s needed for %s game!\x02 Type !add %s to sign up.\r\n",
			    botChannel, node->pickup->max - node->pickup->count,
			    pluralSuffix, node->pickup->name, node->pickup->name);
		} else if (node->pickup->count == 1 && !botSilentWho) {
			raw("PRIVMSG %s :\x02Wanna play %s? %s is waiting!\x02 Type !add %s\r\n",
			    botChannel, node->pickup->name,
			    node->pickup->playerList->player->nick, node->pickup->name);
		} else if (node->pickup->count) {
			raw("PRIVMSG %s :\x02Wanna play %s? There %s %d player%s waiting!\x02 Type !add %s\r\n",
			    botChannel, node->pickup->name, beForm, node->pickup->count,
			    pluralSuffix, node->pickup->name);

			if (!botSilentWho) {
				pickupNode = pushPickup(NULL, node->pickup);
				announcePlayers(pickupNode, botChannel);
				popPickup(pickupNode);
			}
		}
		promotePickup(node->next);
	}
}

void printHelp(const char *to)
{
	raw("PRIVMSG %s :You can type commands in the main channel or query the bot.\r\n", to);
	raw("PRIVMSG %s :!help, !version - print info messages\r\n", to);
	raw("PRIVMSG %s :!add - Add up to a pickup game.\r\n", to);
	raw("PRIVMSG %s :!remove - Remove yourself from all pickups.\r\n", to);
	raw("PRIVMSG %s :!who - List players added to pickups\r\n", to);
	raw("PRIVMSG %s :!promote - Promote a pickup game\r\n", to);
	raw("PRIVMSG %s :!servers - List recommended servers\r\n", to);
}

void printVersion(const char *to)
{
	raw("PRIVMSG %s :jk2pugbot %s by fau <faltec@gmail.com>\r\n", to, botVersion);
	raw("PRIVMSG %s :Visit https://github.com/aufau/jk2pugbot for more\r\n", to);
}

int printGamesH(pickupNode_t *node, int pos)
{
	if (!node) {
		return pos;
	} else {
		pos += snprintf(bot.sbuf + pos, sizeof(bot.sbuf) - pos, " %s", node->pickup->name);
		return printGamesH(node->next, pos);
	}
}

void printGames(const char *msg)
{
	int pos;

	pos = snprintf(bot.sbuf, sizeof(bot.sbuf), "PRIVMSG %s :Avaible pickup games are:", botChannel);
	pos = printGamesH(bot.allPickups, pos);
	pos += snprintf(bot.sbuf + pos, sizeof(bot.sbuf) - pos, ". ");
	pos += snprintf(bot.sbuf + pos, sizeof(bot.sbuf) - pos, msg);
	snprintf(bot.sbuf + pos, sizeof(bot.sbuf) - pos, "\r\n");
	sbufRaw();
}

/* Message Parsing
 * functions
 */

pickup_t *parsePickupListH(pickupNode_t *node, const char *list)
{
	if (!node)
		return NULL;
	if (!strcasecmp(list, node->pickup->name))
		return node->pickup;
	else
		return parsePickupListH(node->next, list);
}

pickupNode_t *parsePickupList(const char *list)
{
	if (!list) {
		return NULL;
	} else {
		pickup_t *pickup = parsePickupListH(bot.allPickups, list);
		list = strtok(NULL, " ");
		if (pickup)
			return pushPickup(parsePickupList(list), pickup);
		else
			return parsePickupList(list);
	}
}

// Parses IRC message string with \r\n removed. Returns true on sucess
// and false for malformed input.
bool parseMessage(char *ptr, char *msgEnd, message_t *message)
{
	time_t epochTime;
	struct tm *locTime;
	char *trailingptr;

	if (msgEnd - ptr > MAX_MSG_LEN - 2)
		return false;

	*msgEnd = '\0';

	time(&epochTime);
	locTime = localtime(&epochTime);
	printf("%02d:%02d >> %s\n", locTime->tm_hour, locTime->tm_min, ptr);

	// Find <trailing> sequence
	trailingptr = strstr(ptr, " :");
	if (trailingptr) {
		trailingptr[0] = '\0';
		trailingptr += 2;
	}
	message->trailing = trailingptr;

	// Parse prefix and command
	strtok(ptr, " ");
	if (ptr[0] == ':') {
		char *prefixptr;
		ptr++;
		message->prefix.nick = strtok_r(ptr, "!", &prefixptr);
		message->prefix.user = strtok_r(NULL, "@", &prefixptr);
		message->prefix.host = strtok_r(NULL, "", &prefixptr);

		ptr = strtok(NULL, " ");
		if (!ptr)
			return false;
	} else {
		memset(&message->prefix, 0, sizeof(message->prefix));
	}
	message->command = ptr;

	// Parse <middle> parameters
	int i = -1;
	do {
		i++;
		message->parameter[i] = strtok(NULL, " ");
	} while (i < 14 && message->parameter[i] != NULL);

	return true;
}

void privmsgReply(char *cmd, const char *replyTo, const char *from)
{
	pickupNode_t *pickupList;
	char *args;

	strtok(cmd, " ");
	args = strtok(NULL, " ");
	pickupList = parsePickupList(args);

	if (!strcmp(cmd, "add")) {
		if (pickupList)
			addNick(pickupList, from);
		else
			printGames("Type !add <game> to sign up.");
	} else if (!strcmp(cmd, "remove")) {
		if (!args)
			purgeNick(from);
		else if(pickupList)
			removeNick(pickupList, from);
		else
			printGames("Type !remove <game> to sign off.");
	} else if (!strcmp(cmd, "who")) {
		if (botSilentWho)
			replyTo = from;

		if (!args)
			announcePlayers(bot.allPickups, replyTo);
		else if(pickupList)
			announcePlayers(pickupList, replyTo);
		else
			printGames("Type !who <game> to see players who signed up already.");
	} else if (!strcmp(cmd, "servers")) {
		if (pickupList)
			announceServers(pickupList, replyTo);
		else
			printGames("Type !servers <game> to see recommended servers.");
	}else if (!strcmp(cmd, "promote")) {
		if (pickupList)
			promotePickup(pickupList);
		else
			printGames("Type !promote <game> to find more players.");
	} else if (!strcmp(cmd, "help")) {
		printHelp(replyTo);
	} else if (!strcmp(cmd, "version")) {
		printVersion(replyTo);
	} else if (!strcmp(cmd, "ping")) {
		raw("PRIVMSG %s :!pong\r\n", replyTo);
	}

	cleanPickupList(pickupList);
}

void messageReply(message_t *message)
{
	if (!strcmp(message->command, "PING")) {
		raw("PONG :%s\r\n", message->trailing);
	} else if (!strcmp(message->command, "PRIVMSG")) {
		if (message->trailing[0] == '!') {
			const char *replyTo;

			if (!strcmp(message->parameter[0], botNick))
				replyTo = message->prefix.nick;
			else
				replyTo = botChannel;

			privmsgReply(message->trailing + 1,
				     replyTo,
				     message->prefix.nick);
		}
	} else if (!strcmp(message->command, "PART") ||
		   !strcmp(message->command, "QUIT")) {
		purgeNick(message->prefix.nick);
	} else if (!strcmp(message->command, "KICK") &&
		   !strcmp(message->parameter[0], botChannel)) {
		purgeNick(message->parameter[1]);
	} else if (!strcmp(message->command, "NICK")) {
		changeNick(message->prefix.nick, message->trailing);
	} else if (!strcmp(message->command, "001")) {
		if (botQpassword) {
			raw("PRIVMSG Q@CServe.quakenet.org :AUTH %s %s\r\n",
			    botNick, botQpassword);
		}
		raw("JOIN %s\r\n", botChannel);
	}
}

void initPickups()
{
	pickupNode_t *pickupList;
	char *games;
	int i;

	for (i = 0; i < sizeof(pickupsArray) / sizeof(*pickupsArray); i++)
		bot.allPickups = pushPickup(bot.allPickups, &pickupsArray[i]);

	for (i = 0; i < sizeof(serversArray) / sizeof(*serversArray); i++) {
		games = strdup(serversArray[i].games);
		assert(games);
		pickupList = parsePickupList(strtok(games, " "));
		addServer(pickupList, &serversArray[i]);
		free(games);
	}
}

void printLists()
{
#ifndef NDEBUG
	if(bot.nickList) {
		printf("bot.nickList = ");
		printPlayers(bot.nickList, 0, "->");
		printf(bot.sbuf);
		printf("\n");
	}
#endif
}

int main()
{
	char buf[2048];
	message_t message;

	struct addrinfo hints;
	struct addrinfo *res = NULL;

	fd_set	set;
	struct timeval timeout;
	int	retVal;
	int	msgLen;
	char	*bufEnd;
	char	*msgStart;
	char	*msgEnd;

	struct sigaction act = {
		.sa_handler	= sigHandler,
		.sa_flags	= 0,
	};

	memset(&act, 0, sizeof(act));
	act.sa_handler = sigHandler;
	sigemptyset(&act.sa_mask);
	sigaction(SIGINT, &act, NULL);

	initPickups();
connect:
	purgePlayers(bot.nickList);
#ifdef DEBUG_INTERCEPT
	bot.conn = STDIN_FILENO;
#else
	memset(&hints, 0, sizeof(struct addrinfo));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	if (res) {
		freeaddrinfo(res);
		res = NULL;
	}
	retVal = getaddrinfo(botHost, botPort, &hints, &res);
	if (retVal) {
		fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(retVal));
		sleep(botTimeout);
		goto connect;
	}
	bot.conn = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
	if (bot.conn == -1) {
		perror("socket: ");
		sleep(botTimeout);
		goto connect;
	}
	retVal = connect(bot.conn, res->ai_addr, res->ai_addrlen);
	if (retVal == -1) {
		perror("connect: ");
		sleep(botTimeout);
		goto connect;
	}
#endif // !DEBUG_INTERCEPT
	raw("USER %s 0 0 :%s\r\n", botNick, botNick);
	raw("NICK %s\r\n", botNick);

	msgLen = 0;
	while (true) {
		// Wait for a TCP packet
		FD_ZERO(&set);
		FD_SET(bot.conn, &set);
		timeout.tv_sec = botTimeout;
		timeout.tv_usec = 0;
		retVal = select(bot.conn + 1, &set, NULL, NULL, &timeout);
		if (retVal == -1) {
			perror("select: ");
			sleep(botTimeout);
			goto connect;
		} else if (retVal == 0) {
			printf("\nPing timeout. Reconnecting...\n\n");
			goto connect;
		}

		// Receive packet
		assert(sizeof(buf) - msgLen - 1 > 0);
		retVal = read(bot.conn, &buf[msgLen], sizeof(buf) - msgLen - 1);
		if (retVal == -1) {
			perror("read: ");
			goto connect;
		} else if (retVal == 0) { // FIN
			printf("\nConnection closed. Reconnecting...\n\n");
			goto connect;
		}
		bufEnd = &buf[msgLen + retVal];
		*bufEnd = '\0';

		// Parse mesages
		msgStart = buf;
		while ((msgEnd = strstr(msgStart, "\r\n"))) {
			if (parseMessage(msgStart, msgEnd, &message)) {
				messageReply(&message);
				printLists();
			}

			msgStart = msgEnd + 2;
		}

		// Save partial message for next read
		msgLen = 0;
		if (msgStart < bufEnd) {
			msgLen = bufEnd - msgStart;
			if (msgLen < MAX_MSG_LEN)
				memmove(buf, msgStart, msgLen);
			else
				msgLen = 0;
		}

		if (bot.statusChanged)
			updateStatus();
	}
}
