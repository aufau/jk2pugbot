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

const char	*botVersion	= "beta";
const char	*botHost	= "irc.quakenet.org";
const char	*botPort	= "6667";
const char	*botNick	= "JK2PUGBOT";
const char	*botChannel	= "#jk2pugbot";
const char	*botTopic	= "Welcome to #jk2pugbot";
const char	*botQpassword	= NULL;		// Password to auth with Q or NULL
int 		botDelay	= 50000;	// Between messages. In microseconds
int		botTimeout	= 300;		// Try to reconnect after this number of seconds

pickup_t pickupsArray[] = {
	{ .name = "MB", .max = 6 },
	{ .name = "CTF", .max = 6 },
	{ .name = "4v4", .max = 8 },
	{ .name = "2v2", .max = 4 },
	{ .name = "duel", .max = 0 },
};

/*
 * Code
 */

int	conn;
time_t	epochTime;
struct tm *locTime;
char	buf[BUFFER_SIZE];
char	sbuf[BUFFER_SIZE];

pickupNode_t	*allPickups;
playerNode_t	*nickList;

bool	statusChanged = false;

void sbufRaw(void)
{
	time(&epochTime);
	locTime = localtime(&epochTime);
	printf("%02d:%02d << %s", locTime->tm_hour, locTime->tm_min, sbuf);
	if (write(conn, sbuf, strlen(sbuf)) == -1)
		perror("sbufRaw write: ");
	usleep(botDelay);
}

void raw(char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(sbuf, 513, fmt, ap);
	va_end(ap);
	sbufRaw();
}

void *smalloc(size_t size)
{
	void *retval = malloc(size);
	assert(retval);
	return retval;
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
	playerNode->next = nickList;
	nickList = playerNode;
	return player;
}

playerNode_t *pushPlayer(playerNode_t *node, player_t *player)
{
	playerNode_t *playerNode = smalloc(sizeof(playerNode_t));
	playerNode->player = player;
	playerNode->next = node;
	return playerNode;
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
	return findNickH(nickList, nick);
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
			statusChanged = true;
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
	removePlayer(allPickups, player);
	nickList = popPlayer(cutPlayer(nickList, player));
	free(player->nick);
	free(player);
}

void purgePlayers(playerNode_t *node)
{
	if (node) {
		purgePlayer(node->player);
		purgePlayers(popPlayer(node));
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
			statusChanged = true;
			if (node->pickup->max && node->pickup->count == node->pickup->max) {
				announcePickup(node->pickup);
				purgePlayers(node->pickup->playerList);
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
		pos += snprintf(sbuf + pos, BUFFER_SIZE - pos, "%s%s", node->player->nick, sep);
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
			pos += snprintf(sbuf + pos, BUFFER_SIZE - pos, formatString,
					node->pickup->name,
					node->pickup->count,
					node->pickup->max);
		} else {
			formatString = node->pickup->count ?
				"\x02(\x02 %s %d \x02)\x02" :
				"( %s %d )";
			pos += snprintf(sbuf + pos, BUFFER_SIZE - pos, formatString,
					node->pickup->name,
					node->pickup->count);
		}
		return printPickups(node->next, pos);
	}
}

void updateStatus()
{
	int pos;

	pos = snprintf(sbuf, BUFFER_SIZE, "TOPIC %s :", botChannel);
	pos = printPickups(allPickups, pos);
	snprintf(sbuf + pos, BUFFER_SIZE - pos,
		 "\x02(\x02 %s \x02)(\x02 Type !help for more info \x02)\x02\r\n", botTopic);
	sbufRaw();
	statusChanged = false;
}

void announcePickup(pickup_t *pickup)
{
	int pos;

	pos = snprintf(sbuf, BUFFER_SIZE, "PRIVMSG ");
	pos = printPlayers(pickup->playerList, pos, ",");
	pos += snprintf(sbuf + pos, BUFFER_SIZE - pos,
		 ",%s :\x02%s pickup is ready to start!\x02 Players are: ",
		 botChannel, pickup->name);
	pos = printPlayers(pickup->playerList, pos, ", ");
	snprintf(sbuf + pos, BUFFER_SIZE - pos, "\r\n");
	sbufRaw();
}

void announcePlayers(pickupNode_t *node, const char *to)
{
	if (node) {
		if (node->pickup->count) {
			int pos;

			if (node->pickup->max) {
				pos = snprintf(sbuf, BUFFER_SIZE,
					       "PRIVMSG %s :\x02(\x02 %s %d/%d \x02)\x02 Players are: ",
					       to, node->pickup->name,
					       node->pickup->count, node->pickup->max);
			} else {
				pos = snprintf(sbuf, BUFFER_SIZE,
					       "PRIVMSG %s :\x02(\x02 %s %d \x02)\x02 Players are: ",
					       to, node->pickup->name,
					       node->pickup->count);
			}

			pos = printPlayers(node->pickup->playerList, pos, ", ");
			snprintf(sbuf + pos, BUFFER_SIZE - pos, "\r\n");
			sbufRaw();
		}
		announcePlayers(node->next, to);
	}
}

void promotePickup(pickupNode_t *node)
{
	if (node) {
		if (node->pickup->max) {
			const char *plural = node->pickup->count == 1 ? "" : "s";

			raw("PRIVMSG %s :\x02Only %d player%s needed for %s game!\x02 Type !add %s to sign up.\r\n",
			    botChannel, node->pickup->max - node->pickup->count,
			    plural, node->pickup->name, node->pickup->name);
		} else if (node->pickup->count) {
			pickupNode_t *pickupNode;

			if (node->pickup->count == 1) {
				raw("PRIVMSG %s :\x02Wanna play %s? %s is waiting!\x02 Type !add %s\r\n",
				    botChannel, node->pickup->name,
				    node->pickup->playerList->player->nick, node->pickup->name);
			} else {
				raw("PRIVMSG %s :\x02Wanna play %s? There are %d players waiting!\x02 Type !add %s\r\n",
				    botChannel, node->pickup->name,
				    node->pickup->count, node->pickup->name);

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
		pos += snprintf(sbuf + pos, BUFFER_SIZE - pos, " %s", node->pickup->name);
		return printGamesH(node->next, pos);
	}
}

void printGames()
{
	int pos;

	pos = snprintf(sbuf, BUFFER_SIZE, "PRIVMSG %s :Avaible pickup games are:", botChannel);
	pos = printGamesH(allPickups, pos);
	snprintf(sbuf + pos, BUFFER_SIZE - pos, ". Type !add <game> to sign up.\r\n");
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

pickupNode_t *parsePickupList(pickupNode_t *node, const char *list)
{
	if (list) {
		pickup_t *pickup = parsePickupListH(allPickups, list);
		if (pickup)
			node = pushPickup(node, pickup);
		else
			return NULL;

		list = strtok(NULL, " ");
		return parsePickupList(node, list);
	} else {
		return node;
	}
}

bool parseBuf(char *buf, message_t *message)
{
	static char *saveptr;
	char *ptr;
	char *trailingptr;

	ptr = buf ? buf : saveptr;
	saveptr = strstr(ptr, "\r\n");
	if (saveptr) {
		saveptr[0] = '\0';
		saveptr += 2;
	} else {
		return false;
	}

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
			return true; // Malformed message
	} else {
		// message->prefix = NULL;
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
	args = strtok(NULL , " ");
   	pickupList = parsePickupList(NULL, args);

	if (!strcmp(cmd, "add")) {
		if (pickupList)
			addNick(pickupList, from);
		else
			printGames();
	} else if (!strcmp(cmd, "remove")) {
		if (!args)
			purgeNick(from);
		else if(pickupList)
			removeNick(pickupList, from);
		else
			printGames();
	} else if (!strcmp(cmd, "who")) {
		if (!args)
			announcePlayers(allPickups, replyTo);
		else if(pickupList)
			announcePlayers(pickupList, replyTo);
		else
			printGames();
	} else if (!strcmp(cmd, "promote")) {
		if (pickupList)
			promotePickup(pickupList);
		else
			printGames();
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
	} else if (!strcmp(message->command, "NICK")) {
		changeNick(message->prefix.nick, message->trailing);
	} else if (!strcmp(message->command, "001")) {
		if (botQpassword)
			raw("PRIVMSG Q@CServe.quakenet.org :AUTH %s %s\r\n",
			    botNick, botQpassword);
		raw("JOIN %s\r\n", botChannel);
	}
}

void sigHandler(int signum)
{
	raw("QUIT :SIGINT\r\n");
	close(conn);
}

int main()
{
	message_t message;

	struct addrinfo hints;
	struct addrinfo *res;

	fd_set	set;
	struct timeval timeout;
	int	retVal;
	int	bufLen;

	struct sigaction act = {
		.sa_handler	= sigHandler,
		.sa_flags	= 0,
	};
	memset(&act, 0, sizeof(act));
	act.sa_handler = sigHandler;
	sigemptyset(&act.sa_mask);
	sigaction(SIGINT, &act, NULL);

	for (int i = 0; i < sizeof(pickupsArray) / sizeof(*pickupsArray); i++)
		allPickups = pushPickup(allPickups, &pickupsArray[i]);

	while (true) {
		purgePlayers(nickList);

		memset(&hints, 0, sizeof(struct addrinfo));
		hints.ai_family = AF_INET;
		hints.ai_socktype = SOCK_STREAM;
		retVal = getaddrinfo(botHost, botPort, &hints, &res);
		if (retVal) {
			fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(retVal));
			sleep(botTimeout);
			continue;
		}
		conn = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
		if (conn == -1) {
			perror("socket: ");
			sleep(botTimeout);
			continue;
		}
		retVal = connect(conn, res->ai_addr, res->ai_addrlen);
		if (retVal == -1) {
			perror("connect: ");
			sleep(botTimeout);
			continue;
		}

		raw("USER %s 0 0 :%s\r\n", botNick, botNick);
		raw("NICK %s\r\n", botNick);

		while (true) {
			FD_ZERO(&set);
			FD_SET(conn, &set);
			timeout.tv_sec = botTimeout;
			retVal = select(conn + 1, &set, NULL, NULL, &timeout);
			if (retVal == -1) {
				perror("select: ");
				sleep(botTimeout);
				continue;
			} else if (retVal == 0) {
				printf("\nPing timeout. Reconnecting...\n\n");
				break;
			}

			bufLen = read(conn, buf, 512);
			if (bufLen <= 0) {
				perror("read: ");
				sleep(botTimeout);
				continue;
			}
			buf[bufLen + 1] = '\0';
			parseBuf(buf, &message);
			do {
				messageReply(&message);
			} while (parseBuf(NULL, &message));
			if (statusChanged)
				updateStatus();
		}
	}
}
