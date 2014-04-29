/*
 * Copyright 2014 Con Kolivas
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 3 of the License, or (at your option)
 * any later version.  See COPYING for more details.
 */

#include "config.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <string.h>
#include <unistd.h>

#include "ckpool.h"
#include "libckpool.h"
#include "uthash.h"

#define MAX_MSGSIZE 1024

struct connector_instance {
	cklock_t lock;
	proc_instance_t *pi;
	int serverfd;
	int nfds;
};

typedef struct connector_instance conn_instance_t;

struct client_instance {
	UT_hash_handle hh;
	struct sockaddr address;
	socklen_t address_len;
	char buf[PAGESIZE];
	int bufofs;
	int fd;
	int id;
};

typedef struct client_instance client_instance_t;

/* For the hashtable of all clients */
static client_instance_t *clients;
static int client_id;

/* Accepts incoming connections to the server socket and generates client
 * instances */
void *acceptor(void *arg)
{
	conn_instance_t *ci = (conn_instance_t *)arg;
	client_instance_t *client;
	int fd;

	rename_proc("acceptor");

retry:
	client = ckzalloc(sizeof(client_instance_t));
	client->address_len = sizeof(client->address);
reaccept:
	fd = accept(ci->serverfd, &client->address, &client->address_len);
	if (unlikely(fd < 0)) {
		if (interrupted())
			goto reaccept;
		LOGERR("Failed to accept on socket %d in acceptor", ci->serverfd);
		dealloc(client);
		goto out;
	}
	keep_sockalive(fd);

	LOGINFO("Connected new client %d on socket %d", ci->nfds, fd);

	client->fd = fd;

	ck_wlock(&ci->lock);
	client->id = client_id++;
	HASH_ADD_INT(clients, id, client);
	ci->nfds++;
	ck_wunlock(&ci->lock);

	goto retry;
out:
	return NULL;
}

/* Invalidate this instance */
static void invalidate_client(ckpool_t *ckp, client_instance_t *client)
{
	char buf[256];

	close(client->fd);
	client->fd = -1;
	sprintf(buf, "dropclient=%d", client->id);
	send_proc(&ckp->stratifier, buf);
}

static void send_client(ckpool_t *ckp, conn_instance_t *ci, int id, const char *buf);

static void parse_client_msg(conn_instance_t *ci, client_instance_t *client)
{
	ckpool_t *ckp = ci->pi->ckp;
	char msg[PAGESIZE], *eol;
	int buflen, ret;
	bool moredata;
	json_t *val;

retry:
	buflen = PAGESIZE - client->bufofs;
	ret = recv(client->fd, client->buf + client->bufofs, buflen, MSG_DONTWAIT);
	if (ret < 1) {
		/* Nothing else ready to be read */
		if (!ret)
			return;

		LOGINFO("Client fd %d disconnected", client->fd);
		invalidate_client(ckp, client);
		return;
	}
	client->bufofs += ret;
	if (client->bufofs == PAGESIZE)
		moredata = true;
	else
		moredata = false;
reparse:
	eol = memchr(client->buf, '\n', client->bufofs);
	if (!eol) {
		if (unlikely(client->bufofs > MAX_MSGSIZE)) {
			LOGWARNING("Client fd %d overloaded buffer without EOL, disconnecting", client->fd);
			invalidate_client(ckp, client);
			return;
		}
		if (moredata)
			goto retry;
		return;
	}

	/* Do something useful with this message now */
	buflen = eol - client->buf + 1;
	if (unlikely(buflen > MAX_MSGSIZE)) {
		LOGWARNING("Client fd %d message oversize, disconnecting", client->fd);
		invalidate_client(ckp, client);
		return;
	}
	memcpy(msg, client->buf, buflen);
	msg[buflen] = 0;
	client->bufofs -= buflen;
	memmove(client->buf, client->buf + buflen, client->bufofs);
	val = json_loads(msg, 0, NULL);
	if (!val) {
		LOGINFO("Client id %d sent invalid json message %s", client->id, msg);
		send_client(ckp, ci, client->id, "Invalid JSON, disconnecting\n");
		invalidate_client(ckp, client);
		return;
	} else {
		char *s;

		json_object_set_new_nocheck(val, "client_id", json_integer(client->id));
		s = json_dumps(val, 0);
		send_proc(&ckp->stratifier, s);
		free(s);
		json_decref(val);
	}

	if (client->bufofs)
		goto reparse;
}

/* Waits on fds ready to read on from the list stored in conn_instance and
 * handles the incoming messages */
void *receiver(void *arg)
{
	conn_instance_t *ci = (conn_instance_t *)arg;
	client_instance_t *client;
	struct pollfd fds[65536];
	int ret, nfds, i;

	rename_proc("receiver");

retry:
	nfds = 0;

	ck_rlock(&ci->lock);
	for (client = clients; client != NULL; client = client->hh.next) {
		/* Invalid client */
		if (client->fd == -1)
			continue;
		fds[nfds].fd = client->fd;
		fds[nfds].events = POLLIN;
		nfds++;
	}
	ck_runlock(&ci->lock);

	if (!nfds) {
		cksleep_ms(100);
		goto retry;
	}
repoll:
	ret = poll(fds, nfds, 1000);
	if (ret < 0) {
		if (interrupted())
			goto repoll;
		LOGERR("Failed to poll in receiver");
		goto out;
	}
	if (!ret)
		goto retry;
	for (i = 0; i < nfds; i++) {

		if (!(fds[i].revents & POLLIN))
			continue;

		client = NULL;

		ck_rlock(&ci->lock);
		for (client = clients; client != NULL; client = client->hh.next) {
			if (client->fd == fds[i].fd)
				break;
		}
		ck_runlock(&ci->lock);

		if (!client)
			LOGWARNING("Failed to find client with fd %d in hashtable!", fds[i].fd);
		else
			parse_client_msg(ci, client);

		if (--ret < 1)
			break;
	}
	goto retry;

out:
	return NULL;
}

static void send_client(ckpool_t *ckp, conn_instance_t *ci, int id, const char *buf)
{
	int fd = -1, ret, len, ofs = 0;
	client_instance_t *client;

	if (unlikely(!buf)) {
		LOGWARNING("Connector send_client sent a null buffer");
		return;
	}
	len = strlen(buf);
	if (unlikely(!len)) {
		LOGWARNING("Connector send_client sent a zero length buffer");
		return;
	}

	ck_rlock(&ci->lock);
	HASH_FIND_INT(clients, &id, client);
	if (likely(client))
		fd = client->fd;
	ck_runlock(&ci->lock);

	if (unlikely(fd == -1)) {
		if (client)
			LOGWARNING("Client id %d disconnected", id);
		else
			LOGWARNING("Connector failed to find client id %d", id);
		return;
	}

	while (len) {
		ret = send(fd, buf + ofs, len , 0);
		if (unlikely(ret < 0)) {
			if (interrupted())
				continue;
			LOGWARNING("Client id %d disconnected", id);
			invalidate_client(ckp, client);
			return;
		}
		ofs += ret;
		len -= ret;
	}
}

static int connector_loop(ckpool_t *ckp, proc_instance_t *pi, conn_instance_t *ci)
{
	int sockd, client_id, ret = 0;
	unixsock_t *us = &pi->us;
	char *buf = NULL;
	json_t *json_msg;

retry:
	dealloc(buf);
	sockd = accept(us->sockd, NULL, NULL);
	if (sockd < 0) {
		if (interrupted())
			goto retry;
		LOGERR("Failed to accept on connector socket, retrying in 5s");
		sleep(5);
		goto retry;
	}

	buf = recv_unix_msg(sockd);
	close(sockd);
	if (!buf) {
		LOGWARNING("Failed to get message in connector_loop");
		goto retry;
	}
	LOGDEBUG("Connector received message: %s", buf);
	if (!strncasecmp(buf, "shutdown", 8))
		goto out;
	json_msg = json_loads(buf, 0, NULL);
	if (unlikely(!json_msg)) {
		LOGWARNING("Invalid json message: %s", buf);
		goto retry;
	}

	/* Extract the client id from the json message and remove its entry */
	client_id = json_integer_value(json_object_get(json_msg, "client_id"));
	json_object_del(json_msg, "client_id");
	dealloc(buf);
	buf = json_dumps(json_msg, 0);
	realloc_strcat(&buf, "\n");
	send_client(ckp, ci, client_id, buf);
	json_decref(json_msg);

	goto retry;
out:
	dealloc(buf);
	return ret;
}

int connector(proc_instance_t *pi)
{
	pthread_t pth_acceptor, pth_receiver;
	char *url = NULL, *port = NULL;
	ckpool_t *ckp = pi->ckp;
	int sockd, ret = 0;
	conn_instance_t ci;
	const int on = 1;
	int tries = 0;

	if (ckp->serverurl) {
		if (!extract_sockaddr(ckp->serverurl, &url, &port)) {
			LOGWARNING("Failed to extract server address from %s", ckp->serverurl);
			ret = 1;
			goto out;
		}
		do {
			sockd = bind_socket(url, port);
			if (sockd > 0)
				break;
			LOGWARNING("Connector failed to bind to socket, retrying in 5s");
			sleep(5);
		} while (++tries < 25);

		dealloc(url);
		dealloc(port);
		if (sockd < 0) {
			LOGERR("Connector failed to bind to socket for 2 minutes");
			ret = 1;
			goto out;
		}
	} else {
		struct sockaddr_in serv_addr;

		sockd = socket(AF_INET, SOCK_STREAM, 0);
		if (sockd < 0) {
			LOGERR("Connector failed to open socket");
			ret = 1;
			goto out;
		}
		setsockopt(sockd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
		memset(&serv_addr, 0, sizeof(serv_addr));
		serv_addr.sin_family = AF_INET;
		serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
		serv_addr.sin_port = htons(3333);
		do {
			ret = bind(sockd, (struct sockaddr*)&serv_addr, sizeof(serv_addr));
			if (!ret)
				break;
			LOGWARNING("Connector failed to bind to socket, retrying in 5s");
			sleep(5);
		} while (++tries < 25);
		if (ret < 0) {
			LOGERR("Connector failed to bind to socket for 2 minutes");
			close(sockd);
			goto out;
		}
	}
	if (tries)
		LOGWARNING("Connector successfully bound to socket");

	ret = listen(sockd, 10);
	if (ret < 0) {
		LOGERR("Connector failed to listen on socket");
		close(sockd);
		goto out;
	}

	cklock_init(&ci.lock);
	memset(&ci, 0, sizeof(ci));
	ci.pi = pi;
	ci.serverfd = sockd;
	ci.nfds = 0;
	create_pthread(&pth_acceptor, acceptor, &ci);
	create_pthread(&pth_receiver, receiver, &ci);

	ret = connector_loop(ckp, pi, &ci);

	//join_pthread(pth_acceptor);
out:
	LOGINFO("%s connector exiting with return code %d", ckp->name, ret);
	if (ret) {
		send_proc(&ckp->main, "shutdown");
		sleep(1);
	}
	return ret;
}