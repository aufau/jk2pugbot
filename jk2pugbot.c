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
const char * const	botRealName	= "";
const char * const	botChannel	= "#jk2pugbot";
const char * const	botTopic	= "Welcome to #jk2pugbot";
const char * const	botQpassword	= NULL;	// Password to auth with Q or NULL
const int	botTimeout	= 300;		// Try to reconnect after this number of seconds
const bool	botSilentWho	= true;		// Don't announce players in the main channel
const bool	botPrintEmpty	= false;	// Print empty pickups in channel topic
const bool	botStrict1459	= false;	// If your server runs in strict RFC 1459 mode

pickup_t pickupsArray[] = {
	{ .name = "CTF", .max = 16 },
	{ .name = "4v4", .max = 8 },
	{ .name = "2v2", .max = 4 },
	{ .name = "duel", .max = 2 },
	{ .name = "ffa", .max = 0 },
};

// These servers will be recommended when announcing a pickup game.
// If your game doesn't use quake 3 engine then don't set the .type variable.
const server_t serversArray[] = {
	{ .name = "[united] Coruscant", .address = "185.44.107.108", .port = "28070", .games = "CTF", .type = SV_Q3 },
	{ .name = "jk2.ouned.de", .address = "185.44.107.108", .port = "28071", .games = "CTF", .type = SV_Q3 },
	{ .name = "SoL", .address = "31.186.250.121", .port = "28070", .games = "ffa duel", .type = SV_Q3 },
};

/*
 * Code
 */

struct {
	int conn;			// irc server socket file descriptor
	char sbuf[SEND_BUF_SIZE + 1];	// send buffer; +1 for closing \0 when printing
	char *cursor;
	char *topic;
	bool statusChanged;

	pickupNode_t *pickupList;
	playerNode_t *playerList;
	player_t *self;
} bot;

void announcePickup(pickup_t *pickup);

void __attribute__ ((noreturn)) com_error(const char *format, ...)
{
	va_list ap;

	va_start(ap, format);
	vfprintf(stderr, format, ap);
	va_end(ap);
	putc('\n', stderr);

	assert(0);
	close(bot.conn);
	exit(EXIT_FAILURE);
}

void __attribute__ ((noreturn)) com_perror(const char *s)
{
	perror(s);
	assert(0);
	close(bot.conn);
	exit(EXIT_FAILURE);
}

void com_warning(const char *format, ...)
{
	va_list ap;

	va_start(ap, format);
	vfprintf(stderr, format, ap);
	va_end(ap);
	putc('\n', stderr);
}

void *com_malloc(size_t size)
{
	void *retval = malloc(size);
	if (!retval)
		com_perror("malloc");
	return retval;
}

char *com_strdup(const char *s)
{
	char *dup;
#if _POSIX_C_SOURCE >= 200809L
	dup = strdup(s);
	if (!dup)
		com_perror("com_strdup");
#else
	int len = strlen(s) + 1;

	dup = com_malloc(len);
	memcpy(dup, s, len);
#endif
	return dup;
}

ssize_t com_write(int fildes, const void *buf, size_t nbyte)
{
	ssize_t len;
	size_t written = 0;

	while (written < nbyte) {
		len = write(fildes, (char *)buf + written, nbyte - written);
		written += len;

		if (len == -1) {
			perror("com_write");
			break;
		}
		if (len == 0)
			break;
	}
	return written;
}

/* IRC protocol-specific
 * functions
 */
int irc_islower(int c)
{
	return (c >= 'a' && c <= 'z');
}

int irc_isupper(int c)
{
	return (c >= 'A' && c <= 'Z');
}

int irc_isalpha(int c)
{
	return irc_islower(c) || irc_isupper(c);
}

int irc_isdigit(int c)
{
	return (c >= '0' && c <= '9');
}

int irc_isspecial(int c)
{
	// "[", "]", "\", "`", "_", "^", "{", "|", "}"
	return ((c >= 0x5b && c <= 0x60) ||
		(c >= 0x7b && c <= 0x7d));
}

int irc_tolower(int c)
{
	if (irc_isupper(c))
		return c + 'a' - 'A';
	if (botStrict1459) {
		if (c >= '[' && c <= '\\')
			return c + '{' - '[';
	} else {
		if (c >= '[' && c <= '^')
			return c + '{' - '[';
	}

	return c;
}

int irc_strcasecmp(const char *s1, const char *s2)
{
	int diff;

	while (*s1) {
		diff = irc_tolower(*s1) - irc_tolower(*s2);
		if (diff)
			return diff;
		s1++;
		s2++;
	}

	return *s2;
}

bool irc_validateNick(const char *nick)
{
	int i;

	if (!nick)
		return false;

	// nickname   =  ( letter / special ) *8( letter / digit / special / "-" )
	if (!irc_isalpha(nick[0]) && !irc_isspecial(nick[0]))
		return false;

	for (i = 1; i <= 9 || !botStrict1459; i++) {
		int ch = nick[i];

		if (!ch)
			return true;
		if (!irc_isalpha(ch) && !irc_isspecial(ch) &&
		    !irc_isdigit(ch) && ch != '-')
			return false;
	}

	return false;
}

/* Buffered IRC server output
 * functions
 */
int bot_flush(void)
{
	time_t epochTime;
	struct tm *locTime;

	if (bot.cursor == bot.sbuf)
		return 0;

	time(&epochTime);
	locTime = localtime(&epochTime);
	*bot.cursor = '\0';
	printf("%02d:%02d << %s", locTime->tm_hour, locTime->tm_min, bot.sbuf);

#ifndef DEBUG_INTERCEPT
	size_t writeLen = bot.cursor - bot.sbuf;
	size_t len;

	len = com_write(bot.conn, bot.sbuf, writeLen);
	if (len < writeLen) {
		com_warning("bot_flush: Sent only %zd bytes out of %zd", len, writeLen);
		return EOF;
	}
#endif
	bot.cursor = bot.sbuf;
	return 0;
}

int bot_puts(const char *s)
{
	int len = strlen(s);

	if (len + 2 > SEND_BUF_SIZE)
		return EOF;

	if (bot.sbuf + SEND_BUF_SIZE <= bot.cursor + len + 2)
		if (bot_flush())
			return EOF;

	memcpy(bot.cursor, s, len);
	bot.cursor += len;
	*bot.cursor++ = '\r';
	*bot.cursor++ = '\n';
	return len + 2;
}

int bot_putchar(int c)
{
	unsigned char ch = c;

	if (bot.cursor >= bot.sbuf + SEND_BUF_SIZE)
		if (bot_flush())
			return EOF;

	*bot.cursor++ = ch;
	return ch;
}

int bot_printf(const char *format, ...)
{
	va_list ap;
	char buf[SEND_BUF_SIZE + 1];
	int len;

	va_start(ap, format);
	len = vsnprintf(buf, sizeof(buf), format, ap);
	va_end(ap);

	if (len < 0)
		return -1;
	if (len >= sizeof(buf))
		return -1;

	if (bot.cursor + len > bot.sbuf + SEND_BUF_SIZE)
		if (bot_flush())
			return -1;

	memcpy(bot.cursor, buf, len);
	bot.cursor += len;
	return len;
}

void sigHandler(int signum)
{
	bot_printf("QUIT :SIGINT\r\n");
	close(bot.conn);
	exit(EXIT_SUCCESS);
}

int getQ3ServerInfo(const server_t *server, q3serverInfo_t *info)
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

	bot_flush(); // Make sure we don't hold split messages

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
	pickupNode_t *pickupNode = com_malloc(sizeof(pickupNode_t));
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

pickupNode_t *freePickupList(pickupNode_t *node)
{
	if (!node)
		return NULL;
	else
		return freePickupList(popPickup(node));
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

player_t *registerPlayer(const char *nick, bool op)
{
	playerNode_t *playerNode;
	player_t *player;

	assert(irc_validateNick(nick));

	playerNode = com_malloc(sizeof(playerNode_t));
	player = com_malloc(sizeof(player_t));
	player->nick = com_malloc(strlen(nick) + 1);
	strcpy(player->nick, nick);
	player->op = op;
	playerNode->player = player;
	playerNode->next = bot.playerList;
	bot.playerList = playerNode;

	if (!bot.self && !strcmp(nick, botNick))
		bot.self = player;

	return player;
}

playerNode_t *pushPlayer(playerNode_t *node, player_t *player)
{
	playerNode_t *playerNode = com_malloc(sizeof(playerNode_t));
	playerNode->player = player;
	playerNode->next = node;
	return playerNode;
}

serverNode_t *pushServer(serverNode_t *node, const server_t *server)
{
	serverNode_t *serverNode = com_malloc(sizeof(serverNode_t));
	serverNode->server = server;
	serverNode->next = node;
	return serverNode;
}

void addServer(pickupNode_t *node, const server_t *server)
{
	if (node) {
		node->pickup->serverList = pushServer(node->pickup->serverList, server);
		addServer(node->next, server);
	}
}

/* cutPlayer
 * move player to the top of a list or return null
 */

playerNode_t *cutPlayerH(playerNode_t *node, const player_t *player)
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

playerNode_t *cutPlayer(playerNode_t *node, const player_t *player)
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
	assert(irc_validateNick(nick));

	return findNickH(bot.playerList, nick);
}

int countPlayers(const playerNode_t *node)
{
	if (!node)
		return 0;
	else
		return countPlayers(node->next) + 1;
}

void removePlayer(pickupNode_t *node, const player_t *player)
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

void removePickupPlayers(pickup_t *pickup)
{
	if (pickup->playerList) {
		removePlayer(bot.pickupList, pickup->playerList->player);
		removePickupPlayers(pickup);
	}
}

void removeNick(pickupNode_t *node, const char *nick)
{
	player_t *player = findNick(nick);
	if (player) {
		removePlayer(node, player);
	}
}

void forgetPlayer(player_t *player)
{
	removePlayer(bot.pickupList, player);
	bot.playerList = popPlayer(cutPlayer(bot.playerList, player));
	free(player->nick);
	free(player);
}

void forgetPlayers(playerNode_t *node)
{
	if (node) {
		playerNode_t *playerNode = node->next;
		forgetPlayer(node->player);
		forgetPlayers(playerNode);
	}
}

void forgetNick(const char *nick)
{
	player_t *player = findNick(nick);
	if (player)
		forgetPlayer(player);
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
				removePickupPlayers(node->pickup);
				return;
			}
		}
		addPlayer(node->next, player);
	}
}

void addNick(pickupNode_t *node, const char *nick)
{
	player_t *player = findNick(nick);
	if (!player && node) {
		player = registerPlayer(nick, false);
		com_warning("addNick: Player %s was not registered", nick);
	}

	addPlayer(node, player);
}

void changeNick(const char *nick, const char *newnick)
{
	player_t *player = findNick(nick);
	if (player) {
		free(player->nick);
		player->nick = com_malloc(strlen(newnick) + 1);
		strcpy(player->nick, newnick);
	}
}

void printPlayers(const playerNode_t *node, const char *sep, bool op)
{
	if (node) {
		const char *opMark = "";

		if (!node->next)
			sep = "";
		if (op && node->player->op)
			opMark = "@";
		bot_printf("%s%s%s", opMark, node->player->nick, sep);
		printPlayers(node->next, sep, op);
	}
}

void printPickups(const pickupNode_t *node)
{
	if (node) {
		const char *formatString;

		if (botPrintEmpty || node->pickup->count) {
			if (node->pickup->max) {
				formatString = node->pickup->count ?
					"\x02(\x02 %s %d/%d \x02)\x02" :
					"( %s %d/%d )";
				bot_printf(formatString, node->pickup->name,
					   node->pickup->count, node->pickup->max);
			} else {
				formatString = node->pickup->count ?
					"\x02(\x02 %s %d \x02)\x02" :
					"( %s %d )";
				bot_printf(formatString, node->pickup->name,
					   node->pickup->count);
			}
		}
		printPickups(node->next);
	}
}

void printServers(const serverNode_t *node)
{
	q3serverInfo_t q3serverInfo;

	if (node) {
		int retVal = 0;

		if (node->server->type == SV_Q3)
			retVal = getQ3ServerInfo(node->server, &q3serverInfo);

		if (retVal == 1) {
			bot_printf("\x02(\x02 %s %d/%d %s:%s \x02)\x02",
				   node->server->name, q3serverInfo.clients,
				   q3serverInfo.maxclients, node->server->address,
				   node->server->port);
		} else if (retVal == -1) {
			bot_printf("\x02(\x02 %s %s:%s \x02)\x02",
				   node->server->name, node->server->address,
				   node->server->port);
		}
		printServers(node->next);
	}
}

void setTopic(const char *newTopic)
{
	if (!newTopic)
		return;

	if (bot.topic)
		free(bot.topic);

	bot.topic = com_strdup(newTopic);
}

void updateStatus()
{
	bot_printf("TOPIC %s :", botChannel);
	printPickups(bot.pickupList);
	bot_printf("\x02(\x02 %s \x02)(\x02 Type !help \x02)\x02\r\n", bot.topic);
	bot.statusChanged = false;
}

void announceServers(const pickupNode_t *node, const char *to)
{
	if (node) {
		if (node->pickup->serverList) {
			bot_printf("PRIVMSG %s :Recommended %s servers: ",
				   to, node->pickup->name);
			printServers(node->pickup->serverList);
			bot_printf("\r\n");
		}

		announceServers(node->next, to);
	}
}

void announcePickup(pickup_t *pickup)
{
	pickupNode_t *node;

	bot_printf("PRIVMSG ");
	printPlayers(pickup->playerList, ",", false);
	bot_printf(",%s :\x02%s pickup is ready to start!\x02 Players are: ",
		   botChannel, pickup->name);
	printPlayers(pickup->playerList, ", ", false);
	bot_printf("\r\n");

	node = pushPickup(NULL, pickup);
	announceServers(node, botChannel);
	popPickup(node);
}

void announcePlayers(const pickupNode_t *node, const char *to)
{
	if (node) {
		if (node->pickup->count) {
			if (node->pickup->max) {
				bot_printf("PRIVMSG %s :\x02(\x02 %s %d/%d \x02)\x02 Players are: ",
					   to, node->pickup->name,
					   node->pickup->count, node->pickup->max);
			} else {
				bot_printf("PRIVMSG %s :\x02(\x02 %s %d \x02)\x02 Players are: ",
					   to, node->pickup->name,
					   node->pickup->count);
			}

			printPlayers(node->pickup->playerList, ", ", false);
			bot_printf("\r\n");
		}
		announcePlayers(node->next, to);
	}
}

void promotePickup(const pickupNode_t *node)
{
	const char *pluralSuffix;
	const char *pluralSuffixLeft;
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
		if (node->pickup->max - node->pickup->count == 1)
			pluralSuffixLeft = "";
		else
			pluralSuffixLeft = "s";

		if (node->pickup->max && node->pickup->count) {
			bot_printf("PRIVMSG %s :\x02Only %d player%s needed for %s game!\x02 Type !add %s to sign up.\r\n",
			    botChannel, node->pickup->max - node->pickup->count,
			    pluralSuffixLeft, node->pickup->name, node->pickup->name);
		} else if (node->pickup->count == 1 && !botSilentWho) {
			bot_printf("PRIVMSG %s :\x02Wanna play %s? %s is waiting!\x02 Type !add %s\r\n",
			    botChannel, node->pickup->name,
			    node->pickup->playerList->player->nick, node->pickup->name);
		} else if (node->pickup->count) {
			bot_printf("PRIVMSG %s :\x02Wanna play %s? There %s %d player%s waiting!\x02 Type !add %s\r\n",
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
	bot_printf("PRIVMSG %s :You can type commands in the main channel or query the bot.\r\n", to);
	bot_printf("PRIVMSG %s :!help, !version - print info messages\r\n", to);
	bot_printf("PRIVMSG %s :!add - Add up to a pickup game\r\n", to);
	bot_printf("PRIVMSG %s :!remove - Remove yourself from all pickups\r\n", to);
	bot_printf("PRIVMSG %s :!who - List players added to pickups\r\n", to);
	bot_printf("PRIVMSG %s :!promote - Promote a pickup game\r\n", to);
	bot_printf("PRIVMSG %s :!servers - List recommended servers\r\n", to);

	if (!irc_validateNick(to))
		return;

	player_t *player = findNick(to);

	if (!player || !player->op)
		return;

	bot_printf("PRIVMSG %s :!topic - Set channel topic\r\n");
}

void printVersion(const char *to)
{
	bot_printf("PRIVMSG %s :jk2pugbot %s by fau <faltec@gmail.com>\r\n", to, botVersion);
	bot_printf("PRIVMSG %s :Visit https://github.com/aufau/jk2pugbot for more\r\n", to);
}

void printGamesH(const pickupNode_t *node)
{
	if (node) {
		bot_printf(" %s", node->pickup->name);
		printGamesH(node->next);
	}
}

void printGames(const char *msg)
{
	bot_printf("PRIVMSG %s :Avaible pickup games are:", botChannel);
	printGamesH(bot.pickupList);
	bot_printf(". %s\r\n", msg);
}

/* Message Parsing
 * functions
 */

pickup_t *findPickup(const pickupNode_t *node, const char *list)
{
	if (!node)
		return NULL;
	if (!strcasecmp(list, node->pickup->name))
		return node->pickup;
	else
		return findPickup(node->next, list);
}

pickupNode_t *parsePickupList(char *list)
{
	char *item;
	pickup_t *pickup;

	item = strtok(list, " ");
	if (!item)
		return NULL;
	pickup = findPickup(bot.pickupList, item);

	if (pickup)
		return pushPickup(parsePickupList(NULL), pickup);
	else
		return parsePickupList(NULL);
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

	memset(message, 0, sizeof(*message));

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
	pickupNode_t *pickupList = NULL;
	char *args;

	args = strchr(cmd, ' ');
	if (args)
		*args++ = '\0';

	if (!strcmp(cmd, "add")) {
		if (args)
			pickupList = parsePickupList(args);
		if (pickupList)
			addNick(pickupList, from);
		else
			printGames("Type !add <game> to sign up.");
	} else if (!strcmp(cmd, "remove")) {
		if (args) {
			pickupList = parsePickupList(args);

			if (pickupList)
				removeNick(pickupList, from);
			else
				printGames("Type !remove <game> to sign off.");
		} else {
			removeNick(bot.pickupList, from);
		}
	} else if (!strcmp(cmd, "who")) {
		if (botSilentWho)
			replyTo = from;

		if (args) {
			pickupList = parsePickupList(args);

			if (pickupList)
				announcePlayers(pickupList, replyTo);
			else
				printGames("Type !who <game> to see players who signed up already.");
		} else {
			announcePlayers(bot.pickupList, replyTo);
		}
	} else if (!strcmp(cmd, "servers")) {
		if (args)
			pickupList = parsePickupList(args);
		if (pickupList)
			announceServers(pickupList, replyTo);
		else
			printGames("Type !servers <game> to see recommended servers.");
	}else if (!strcmp(cmd, "promote")) {
		if (args)
			pickupList = parsePickupList(args);
		if (pickupList)
			promotePickup(pickupList);
		else
			printGames("Type !promote <game> to find more players.");
	} else if (!strcmp(cmd, "help")) {
		printHelp(replyTo);
	} else if (!strcmp(cmd, "version")) {
		printVersion(replyTo);
	} else if (!strcmp(cmd, "ping")) {
		bot_printf("PRIVMSG %s :!pong\r\n", replyTo);
	} else if (!strcmp(cmd, "topic")) {
		player_t *player = findNick(from);

		if (args && player && player->op) {
			setTopic(args);
			bot.statusChanged = true;
		}
	}

	freePickupList(pickupList);
}

void numericReplyReply(int num, message_t *message)
{
	const char *nick;

	if (!message->parameter[0] || strcmp(message->parameter[0], botNick))
		return;

	switch (num) {
	case RPL_WELCOME:
		if (botQpassword) {
			bot_printf("PRIVMSG Q@CServe.quakenet.org :AUTH %s %s\r\n",
				   botNick, botQpassword);
			bot_printf("MODE %s +x\r\n", botNick);
		}
		bot_printf("JOIN %s\r\n", botChannel);
		break;
	case RPL_NAMREPLY:
		if (!message->parameter[2] ||
		    irc_strcasecmp(message->parameter[2], botChannel))
			break;

		nick = strtok(message->trailing, " ");
		while (nick) {
			player_t *player;
			bool op = false;

			if (nick[0] == '@') {
				op = true;
				nick++;
			} else if (nick[0] == '+') {
				nick++;
			}

			player = findNick(nick);
			if (player)
				player->op = op;
			else
				registerPlayer(nick, op);

			nick = strtok(NULL, " ");
		}
		break;
	}
}

void messageReply(message_t *message)
{
	if (!strcmp(message->command, "PING")) {
		if (message->trailing)
			bot_printf("PONG :%s\r\n", message->trailing);
		else
			bot_puts("PONG");
	} else if (!strcmp(message->command, "PRIVMSG")) {
		if (message->trailing && message->trailing[0] == '!' &&
			message->parameter[0] && message->prefix.nick) {
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
		if (message->prefix.nick)
			forgetNick(message->prefix.nick);
	} else if (!strcmp(message->command, "KICK")) {
		if (message->parameter[0] && message->parameter[1] &&
		    !irc_strcasecmp(message->parameter[0], botChannel))
			forgetNick(message->parameter[1]);
	} else if (!strcmp(message->command, "NICK")) {
		if (message->prefix.nick && message->trailing)
			changeNick(message->prefix.nick, message->trailing);
	} else if (!strcmp(message->command, "JOIN")) {
		if (message->prefix.nick && message->parameter[0] &&
		    !irc_strcasecmp(message->parameter[0], botChannel)) {
			if (findNick(message->prefix.nick))
				com_warning("JOIN: Player %s was already registered",
					    message->prefix.nick);
			else
				registerPlayer(message->prefix.nick, false);
		}
	} else if (!strcmp(message->command, "MODE")) {
		if (message->parameter[0] && message->parameter[1] &&
		    !irc_strcasecmp(message->parameter[0], botChannel)) {
			player_t *player;
			const char *nick;
			bool op = false;
			int i;

			for (i = 1; message->parameter[1][i]; i++) {
				if (message->parameter[1][i] == 'o')
					op = true;
			}
			if (!op)
				return;
			if (message->parameter[1][0] == '-')
				op = false;
			else if (message->parameter[1][0] != '+')
				return;

			// There is 'o' mode so 'l' and <limit> parameters are not
			nick = message->parameter[2];
			if (!nick)
				return;

			player = findNick(nick);
			if (player) {
				player->op = op;
			} else {
				player = registerPlayer(nick, op);
				com_warning("MODE: Player %s was not registered",
					    nick);
			}

			if (op && player == bot.self)
				bot.statusChanged = true;
		}
	} else {
		// Determine numeric reply number
		int num = 0;
		for (int i = 0; i < 3; i++) {
			if (message->command[i] >= '0' && message->command[i] <= '9') {
				num *= 10;
				num += message->command[i] - '0';
			} else {
				return;
			}
		}

		numericReplyReply(num, message);
	}
}

void initPickups()
{
	pickupNode_t *pickupList;
	char *games;
	int i;

	for (i = 0; i < sizeof(pickupsArray) / sizeof(*pickupsArray); i++)
		bot.pickupList = pushPickup(bot.pickupList, &pickupsArray[i]);

	for (i = 0; i < sizeof(serversArray) / sizeof(*serversArray); i++) {
		games = com_strdup(serversArray[i].games);
		pickupList = parsePickupList(games);
		addServer(pickupList, &serversArray[i]);
		freePickupList(pickupList);
		free(games);
	}
}

void printLists()
{
#ifndef NDEBUG
	if(bot.playerList) {
		bot_flush();
		printf("bot.playerList = ");
		printPlayers(bot.playerList, "->", true);
		*bot.cursor = '\0';
		puts(bot.sbuf);
		bot.cursor = bot.sbuf;
	}
#endif
}

int main()
{
	char buf[RECV_BUF_SIZE];
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
	setTopic(botTopic);
	assert(irc_validateNick(botNick));
connect:
	bot.cursor = bot.sbuf;
	forgetPlayers(bot.playerList);
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
		com_warning("getaddrinfo: %s", gai_strerror(retVal));
		sleep(botTimeout);
		goto connect;
	}
	bot.conn = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
	if (bot.conn == -1) {
		perror("socket");
		sleep(botTimeout);
		goto connect;
	}
	retVal = connect(bot.conn, res->ai_addr, res->ai_addrlen);
	if (retVal == -1) {
		perror("connect");
		sleep(botTimeout);
		goto connect;
	}
#endif // !DEBUG_INTERCEPT
	bot_printf("NICK %s\r\n", botNick);
	bot_printf("USER %s 0 * :%s\r\n", botNick, botRealName);

	msgLen = 0;
	while (true) {
		// Wait for a TCP packet
		FD_ZERO(&set);
		FD_SET(bot.conn, &set);
		timeout.tv_sec = botTimeout;
		timeout.tv_usec = 0;
		retVal = select(bot.conn + 1, &set, NULL, NULL, &timeout);
		if (retVal == -1) {
			perror("select");
			sleep(botTimeout);
			goto connect;
		} else if (retVal == 0) {
			com_warning("Ping timeout. Reconnecting...");
			goto connect;
		}

		// Receive packet
		assert(sizeof(buf) - msgLen - 1 > 0);
		retVal = read(bot.conn, &buf[msgLen], sizeof(buf) - msgLen - 1);
		if (retVal == -1) {
			perror("read");
			goto connect;
		} else if (retVal == 0) { // FIN
			com_warning("Connection closed. Reconnecting...");
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

		// Send messages
		bot_flush();
	}
}
