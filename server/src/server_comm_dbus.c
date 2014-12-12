/**
 * \file netopeer_dbus.h
 * \author Michal Vaško <mvasko@cesnet.cz>
 * \brief Netopeer's DBus communication macros.
 *
 * Copyright (C) 2014 CESNET, z.s.p.o.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name of the Company nor the names of its contributors
 *    may be used to endorse or promote products derived from this
 *    software without specific prior written permission.
 *
 * ALTERNATIVELY, provided that this notice is retained in full, this
 * product may be distributed under the terms of the GNU General Public
 * License (GPL) version 2 or later, in which case the provisions
 * of the GPL apply INSTEAD OF those given above.
 *
 * This software is provided ``as is, and any express or implied
 * warranties, including, but not limited to, the implied warranties of
 * merchantability and fitness for a particular purpose are disclaimed.
 * In no event shall the company or contributors be liable for any
 * direct, indirect, incidental, special, exemplary, or consequential
 * damages (including, but not limited to, procurement of substitute
 * goods or services; loss of use, data, or profits; or business
 * interruption) however caused and on any theory of liability, whether
 * in contract, strict liability, or tort (including negligence or
 * otherwise) arising in any way out of the use of this software, even
 * if advised of the possibility of such damage.
 *
 */

#define _GNU_SOURCE
#include <libnetconf_xml.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/poll.h>
#include <sys/epoll.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/wait.h>
#include <unistd.h>

#include <libssh/libssh.h>
#include <libssh/callbacks.h>
#include <libssh/server.h>

#include "comm.h"

#define KEYS_DIR "/etc/ssh/"
#define USER "myuser"
#define PASS "mypass"
/* SSH_AUTH_METHOD_UNKNOWN SSH_AUTH_METHOD_NONE SSH_AUTH_METHOD_PASSWORD SSH_AUTH_METHOD_PUBLICKEY SSH_AUTH_METHOD_HOSTBASED SSH_AUTH_METHOD_INTERACTIVE SSH_AUTH_METHOD_GSSAPI_MIC */
#define SSH_AUTH_METHODS (SSH_AUTH_METHOD_PASSWORD | SSH_AUTH_METHOD_PUBLICKEY)

#define CLIENT_MAX_AUTH_ATTEMPTS 3
/* time for the users to authenticate themselves, in seconds */
#define CLIENT_AUTH_TIMEOUT 10

/* time for the client to close the SSH channel */
#define CLIENT_CHANNEL_CLOSE_TIMEOUT 2

#define CLIENT_POLL_TIMEOUT 200

#define BASE_READ_BUFFER_SIZE 2048

#define NC_V10_END_MSG "]]>]]>"
#define NC_V11_END_MSG "\n##\n"
#define NC_MAX_END_MSG_LEN 6

struct ntf_thread_config {
	struct nc_session *session;
	nc_rpc *subscribe_rpc;
};

struct bind_addr {
	char* addr;
	unsigned int* ports;
	unsigned int port_count;
	struct bind_addr* next;
};

struct client_struct {
	pthread_t thread_id;
	int sock;
	struct sockaddr_storage saddr;
	struct session_data_struct* ssh_sdata;
	struct channel_data_struct* ssh_cdata;
	struct client_struct* next;
};

/* A userdata struct for channel. */
struct channel_data_struct {
	int netconf_subsystem;			// was netconf subsystem requested?
	struct nc_session* ncsession;	// the netconf session
	char* username;					// the SSH username
	int server_in[2];				// pipe - server read, libnc write
	int server_out[2];				// pipe - server write, libnc read
	volatile int done;				// local done flag for a client
};

/* A userdata struct for session. */
struct session_data_struct {
	ssh_session sshsession;	// the SSH session
    ssh_channel sshchannel;	// the SSH channel
    int auth_attempts;		// number of failed auth attempts
    int authenticated;		// is the user authenticated?
	char* username;			// the SSH username
};

struct client_struct* ssh_clients;
extern struct bind_addr* ssh_binds;
extern int done, restart_soft;

static void _client_free(struct client_struct* client) {
	if (client->sock != -1) {
		close(client->sock);
	}

	/* free session data */
	if (client->ssh_sdata != NULL) {
		if (client->ssh_sdata->sshchannel != NULL) {
			ssh_channel_free(client->ssh_sdata->sshchannel);
		}
		if (client->ssh_sdata->sshsession != NULL) {
			/* !! frees all the associated channels as well !! */
			ssh_free(client->ssh_sdata->sshsession);
		}
		free(client->ssh_sdata->username);

		free(client->ssh_sdata);
	}

	/* free channel data */
	if (client->ssh_cdata != NULL) {
		if (client->ssh_cdata->ncsession != NULL) {
			nc_session_free(client->ssh_cdata->ncsession);
		}
		free(client->ssh_cdata->username);
		if (client->ssh_cdata->server_in[0] != -1) {
			close(client->ssh_cdata->server_in[0]);
		}
		if (client->ssh_cdata->server_in[1] != -1) {
			close(client->ssh_cdata->server_in[1]);
		}
		if (client->ssh_cdata->server_out[0] != -1) {
			close(client->ssh_cdata->server_out[0]);
		}
		if (client->ssh_cdata->server_out[1] != -1) {
			close(client->ssh_cdata->server_out[1]);
		}

		free(client->ssh_cdata);
	}
}

static struct client_struct* client_find(struct client_struct* root, pthread_t tid) {
	struct client_struct* client;

	for (client = root; client != NULL; client = client->next) {
		if (client->thread_id == tid) {
			break;
		}
	}

	return client;
}

static void client_cleanup(struct client_struct** root) {
	struct client_struct* cur, *prev;

	if (root || *root == NULL) {
		return;
	}

	for (cur = *root; cur != NULL;) {
		_client_free(cur);
		prev = cur;
		cur = cur->next;
		free(prev);
	}

	*root = NULL;
}

static void client_append(struct client_struct** root, struct client_struct* clients) {
	struct client_struct* cur;

	if (root == NULL) {
		return;
	}

	if (*root == NULL) {
		*root = clients;
		return;
	}

	for (cur = *root; cur->next != NULL; cur = cur->next);

	cur->next = clients;
}

static void client_free(struct client_struct** root, pthread_t tid) {
	struct client_struct* client, *prev_client = NULL;

	for (client = *root; client != NULL; client = client->next) {
		if (client->thread_id == tid) {
			break;
		}
		prev_client = client;
	}

	if (client == NULL) {
		nc_verb_error("%s: internal error: client not found (%s:%d)", __func__, __FILE__, __LINE__);
		return;
	}

	_client_free(client);

	/* free the whole structure */
	if (prev_client == NULL) {
		*root = (*root)->next;
	} else {
		prev_client->next = client->next;
	}
	free(client);
}

static void* client_notification_thread(void* arg) {
	struct ntf_thread_config *config = (struct ntf_thread_config*)arg;

	ncntf_dispatch_send(config->session, config->subscribe_rpc);
	nc_rpc_free(config->subscribe_rpc);
	free(config);

	return NULL;
}

static void sshcb_channel_eof(ssh_session session, ssh_channel channel, void *userdata) {
	(void)session;
	(void)channel;
	(void)userdata;
	nc_verb_verbose("%s call", __func__);
}

static void sshcb_channel_close(ssh_session session, ssh_channel channel, void *userdata) {
	(void)session;
	(void)channel;
	(void)userdata;
	nc_verb_verbose("%s call", __func__);
}

static void sshcb_channel_signal(ssh_session session, ssh_channel channel, const char *signal, void *userdata) {
	(void)session;
	(void)channel;
	(void)signal;
	(void)userdata;
	nc_verb_verbose("%s call", __func__);
}

static void sshcb_channel_exit_status(ssh_session session, ssh_channel channel, int exit_status, void *userdata) {
	(void)session;
	(void)channel;
	(void)exit_status;
	(void)userdata;
	nc_verb_verbose("%s call", __func__);
}

static void sshcb_channel_exit_signal(ssh_session session, ssh_channel channel, const char *signal, int core, const char *errmsg, const char *lang, void *userdata) {
	(void)session;
	(void)channel;
	(void)signal;
	(void)core;
	(void)errmsg;
	(void)lang;
	(void)userdata;
	nc_verb_verbose("%s call", __func__);
}

/* returns how much of the data was processed */
static int sshcb_channel_data(ssh_session session, ssh_channel channel, void* data, uint32_t len, int is_stderr, void* userdata) {
	char* to_send;
	nc_rpc* rpc = NULL;
	nc_reply* rpc_reply = NULL;
	NC_MSG_TYPE rpc_type;
	struct channel_data_struct* cdata = (struct channel_data_struct*) userdata;
	struct nc_cpblts* capabilities = NULL;
	struct nc_err* err;
	int ret, to_send_size, to_send_len;

	(void) channel;
	(void) session;
	(void) is_stderr;

	if (!cdata->netconf_subsystem) {
		nc_verb_error("%s: some data received, but the netconf subsystem was not yet requested: raw data:\n%.*s", __func__, len, data);
		return len;
	}

	//nc_verb_verbose("%s: raw data received:\n%.*s", __func__, len, data);

	/* check if we received a whole NETCONF message */
	if (strncmp(data+len-strlen(NC_V10_END_MSG), NC_V10_END_MSG, strlen(NC_V10_END_MSG)) != 0 &&
			strncmp(data+len-strlen(NC_V11_END_MSG), NC_V11_END_MSG, strlen(NC_V11_END_MSG)) != 0) {
		return 0;
	}

	/* pass data from the client to the library */
	if ((ret = write(cdata->server_out[1], data, len)) != (signed)len) {
		if (ret == -1) {
			nc_verb_error("%s: failed to pass the client data to the library (%s)", __func__, strerror(errno));
		} else {
			nc_verb_error("%s: failed to pass the client data to the library", __func__);
		}

		cdata->done = 1;
		goto send_reply;
	}

	/* if there is no session, we expect a hello message */
	if (cdata->ncsession == NULL) {
		/* get server capabilities */
		capabilities = nc_session_get_cpblts_default();

		cdata->ncsession = nc_session_accept_inout(capabilities, cdata->username, cdata->server_out[0], cdata->server_in[1]);
		nc_cpblts_free(capabilities);
		if (cdata->ncsession == NULL) {
			nc_verb_error("%s: failed to create nc session", __func__);
			cdata->done = 1;
			goto send_reply;
		}

		// TODO show id, if still used
		nc_verb_verbose("New server session");

		nc_session_monitor(cdata->ncsession);

		/* hello message was processed, send our hello */
		goto send_reply;
	}

	/* receive a new RPC */
	rpc_type = nc_session_recv_rpc(cdata->ncsession, 0, &rpc);
	if (rpc_type != NC_MSG_RPC) {
		switch (rpc_type) {
		case NC_MSG_UNKNOWN:
			if (nc_session_get_status(cdata->ncsession) != NC_SESSION_STATUS_WORKING) {
				/* something really bad happened, and communication is not possible anymore */
				nc_verb_error("%s: failed to receive client's message (nc session not working)", __func__);
				cdata->done = 1;
				goto send_reply;
			}
			break;
		case NC_MSG_WOULDBLOCK:
			nc_verb_warning("%s: internal error: no full message received yet", __func__);
			return len;
		default:
			/* TODO something weird */
			break;
		}
	}

	/* process the new RPC */
	switch (nc_rpc_get_op(rpc)) {
	case NC_OP_CLOSESESSION:
		cdata->done = 1;
		rpc_reply = nc_reply_ok();
		break;

	case NC_OP_KILLSESSION:
		/*if ((op = ncxml_rpc_get_op_content(rpc)) == NULL || op->name == NULL ||
				xmlStrEqual(op->name, BAD_CAST "kill-session") == 0) {
			nc_verb_error("%s: corrupted RPC message", __func__);
			reply = nc_reply_error(nc_err_new(NC_ERR_OP_FAILED));
			xmlFreeNodeList(op);
			goto send_reply;
		}
		if (op->children == NULL || xmlStrEqual(op->children->name, BAD_CAST "session-id") == 0) {
			nc_verb_error("%s: no session id found");
			err = nc_err_new(NC_ERR_MISSING_ELEM);
			nc_err_set(err, NC_ERR_PARAM_INFO_BADELEM, "session-id");
			reply = nc_reply_error(err);
			xmlFreeNodeList(op);
			goto send_reply;
		}
		sid = (char *)xmlNodeGetContent(op->children);
		reply = comm_kill_session(conn, sid);
		xmlFreeNodeList(op);
		frre(sid);*/
		break;
	case NC_OP_CREATESUBSCRIPTION:
		/* create-subscription message */
		if (nc_cpblts_enabled(cdata->ncsession, "urn:ietf:params:netconf:capability:notification:1.0") == 0) {
			rpc_reply = nc_reply_error(nc_err_new(NC_ERR_OP_NOT_SUPPORTED));
			goto send_reply;
		}

		/* check if notifications are allowed on this session */
		if (nc_session_notif_allowed(cdata->ncsession) == 0) {
			nc_verb_error("%s: notification subscription is not allowed on this session", __func__);
			err = nc_err_new(NC_ERR_OP_FAILED);
			nc_err_set(err, NC_ERR_PARAM_TYPE, "protocol");
			nc_err_set(err, NC_ERR_PARAM_MSG, "Another notification subscription is currently active on this session.");
			rpc_reply = nc_reply_error(err);
			goto send_reply;
		}

		rpc_reply = ncntf_subscription_check(rpc);
		if (nc_reply_get_type(rpc_reply) != NC_REPLY_OK) {
			goto send_reply;
		}

		pthread_t thread;
		struct ntf_thread_config* ntf_config;

		if ((ntf_config = malloc(sizeof(struct ntf_thread_config))) == NULL) {
			nc_verb_error("%s: memory allocation failed", __func__);
			err = nc_err_new(NC_ERR_OP_FAILED);
			nc_err_set(err, NC_ERR_PARAM_MSG, "Memory allocation failed.");
			rpc_reply = nc_reply_error(err);
			err = NULL;
			goto send_reply;
		}
		ntf_config->session = cdata->ncsession;
		ntf_config->subscribe_rpc = nc_rpc_dup(rpc);

		/* perform notification sending TODO do we need another thread? */
		if ((pthread_create(&thread, NULL, client_notification_thread, ntf_config)) != 0) {
			nc_reply_free(rpc_reply);
			err = nc_err_new(NC_ERR_OP_FAILED);
			nc_err_set(err, NC_ERR_PARAM_MSG, "Creating thread for sending Notifications failed.");
			rpc_reply = nc_reply_error(err);
			err = NULL;
			goto send_reply;
		}
		pthread_detach(thread);
		break;

	default:
		if ((rpc_reply = ncds_apply_rpc2all(cdata->ncsession, rpc, NULL)) == NULL) {
			err = nc_err_new(NC_ERR_OP_FAILED);
			nc_err_set(err, NC_ERR_PARAM_MSG, "For unknown reason no reply was returned by the library.");
			rpc_reply = nc_reply_error(err);
		} else if (rpc_reply == NCDS_RPC_NOT_APPLICABLE) {
			err = nc_err_new(NC_ERR_OP_FAILED);
			nc_err_set(err, NC_ERR_PARAM_MSG, "There is no device/data that could be affected.");
			nc_reply_free(rpc_reply);
			rpc_reply = nc_reply_error(err);
		}

		goto send_reply;
	}

send_reply:
	if (rpc_reply != NULL && rpc != NULL) {
		nc_session_send_reply(cdata->ncsession, rpc, rpc_reply);
		nc_reply_free(rpc_reply);
	}
	if (rpc != NULL) {
		nc_rpc_free(rpc);
	}

	/* pass data from the library to the client */
	to_send_size = BASE_READ_BUFFER_SIZE;
	to_send_len = 0;
	to_send = malloc(to_send_size);
	while (1) {
		to_send_len += (ret = read(cdata->server_in[0], to_send+to_send_len, to_send_size-to_send_len));
		if (ret == -1) {
			break;
		}

		if (to_send_len == to_send_size) {
			to_send_size *= 2;
			to_send = realloc(to_send, to_send_size);
		} else {
			break;
		}
	}

	if (ret == -1) {
		nc_verb_error("%s: failed to pass the library data to the client (%s)", __func__, strerror(errno));
	} else {
		/* TODO libssh bug */
		ssh_set_fd_towrite(session);
		ssh_channel_write(channel, to_send, to_send_len);
	}
	//TODO always free the buffer, or reuse?
	free(to_send);

	return len;
}

static int sshcb_channel_subsystem(ssh_session session, ssh_channel channel, const char* subsystem, void* userdata) {
	struct channel_data_struct *cdata = (struct channel_data_struct*) userdata;

	(void) cdata;
	(void) session;
	(void) channel;

	if (strcmp(subsystem, "netconf") == 0) {
		/* create pipes server <-> library here */
		if (pipe(cdata->server_in) == -1 || pipe(cdata->server_out) == -1) {
			nc_verb_error("%s: creating pipes failed (%s)", __func__, strerror(errno));
			return SSH_OK;
		}

		cdata->netconf_subsystem = 1;
	}

	return SSH_OK;
}

static int sshcb_auth_password(ssh_session session, const char* user, const char* pass, void* userdata) {
	struct session_data_struct* sdata = (struct session_data_struct*) userdata;

	(void) session;

	if (strcmp(user, USER) == 0 && strcmp(pass, PASS) == 0) {
		sdata->username = strdup(user);
		sdata->authenticated = 1;
		nc_verb_verbose("User '%s' authenticated", user);
		return SSH_AUTH_SUCCESS;
	}

	nc_verb_verbose("Failed user '%s' authentication attempt", user);
	sdata->auth_attempts++;
	return SSH_AUTH_DENIED;
}

static int sshcb_auth_pubkey(ssh_session session, const char* user, struct ssh_key_struct* pubkey, char signature_state, void* userdata) {
	struct session_data_struct* sdata = (struct session_data_struct*) userdata;

	(void)session;
	(void)pubkey;

	nc_verb_verbose("%s call", __func__);

	if (signature_state == SSH_PUBLICKEY_STATE_NONE) {
		/* just accepting the use of a particular (pubkey) key */
		return SSH_AUTH_SUCCESS;

	} else if (signature_state == SSH_PUBLICKEY_STATE_VALID) {
		sdata->username = strdup(user);
		sdata->authenticated = 1;
		nc_verb_verbose("User '%s' authenticated", user);
		return SSH_AUTH_SUCCESS;
	}

	nc_verb_verbose("Failed user '%s' authentication attempt", user);
	sdata->auth_attempts++;
	return SSH_AUTH_DENIED;
}

static ssh_channel sshcb_channel_open(ssh_session session, void* userdata) {
	struct session_data_struct* sdata = (struct session_data_struct*) userdata;

	nc_verb_verbose("%s call", __func__);
	sdata->sshchannel = ssh_channel_new(session);
	return sdata->sshchannel;
}

void* ssh_client_thread(void* arg) {
	int n;
	ssh_event event;
	pthread_t my_tid;
	struct channel_data_struct* cdata = NULL;
	struct session_data_struct* sdata = NULL;
	struct client_struct* client;
	ssh_session session = (ssh_session)arg;

	if ((event = ssh_event_new()) == NULL) {
		nc_verb_error("%s: internal error: could not create SSH polling context (%s:%d)", __func__, __FILE__, __LINE__);
		goto finish;
	}

	cdata = calloc(1, sizeof(struct channel_data_struct));
	sdata = calloc(1, sizeof(struct session_data_struct));
	if (cdata == NULL || sdata == NULL) {
		nc_verb_error("%s: internal error: malloc failed (%s:%d)", __func__, __FILE__, __LINE__);
		goto finish;
	}
	memset(cdata->server_in, -1, 2*sizeof(int));
	memset(cdata->server_out, -1, 2*sizeof(int));
	sdata->sshsession = session;

	/* remember these structures for global access */
	my_tid = pthread_self();
	client = client_find(ssh_clients, my_tid);
	if (client == NULL) {
		nc_verb_error("%s: internal error: could not find our global client structure (%s:%d)", __func__, __FILE__, __LINE__);
		goto finish;
	}
	client->ssh_sdata = sdata;
	client->ssh_cdata = cdata;

	struct ssh_channel_callbacks_struct channel_cb = {
		.userdata = cdata,
		.channel_data_function = sshcb_channel_data,
		.channel_eof_function = sshcb_channel_eof,
		.channel_close_function = sshcb_channel_close,
		.channel_signal_function = sshcb_channel_signal,
		.channel_exit_status_function = sshcb_channel_exit_status,
		.channel_exit_signal_function = sshcb_channel_exit_signal,
		.channel_subsystem_request_function = sshcb_channel_subsystem
	};

	struct ssh_server_callbacks_struct server_cb = {
		.userdata = sdata,
		.auth_password_function = sshcb_auth_password,
		//.auth_none_function =
		.auth_pubkey_function = sshcb_auth_pubkey,
		.channel_open_request_session_function = sshcb_channel_open
	};

	ssh_callbacks_init(&server_cb);
	ssh_callbacks_init(&channel_cb);

	ssh_set_server_callbacks(session, &server_cb);

	if (ssh_handle_key_exchange(session) != SSH_OK) {
		nc_verb_error("%s: ssh error (%s:%d): %s", __func__, __FILE__, __LINE__, ssh_get_error(session));
		goto finish;
	}

	ssh_set_auth_methods(session, SSH_AUTH_METHODS);
	ssh_event_add_session(event, session);

	n = 0;
	while (sdata->authenticated == 0 || sdata->sshchannel == NULL) {
		if (done || cdata->done) {
			goto finish;
		}
		if (sdata->auth_attempts >= CLIENT_MAX_AUTH_ATTEMPTS) {
			nc_verb_error("Too many failed authentication attempts, dropping client");
			goto finish;
		}
		if (n >= CLIENT_AUTH_TIMEOUT*10) {
			nc_verb_error("Failed to authenticate for too long, dropping client");
			goto finish;
		}

		if (ssh_event_dopoll(event, 100) == SSH_ERROR) {
			nc_verb_error("%s: ssh error (%s:%d): %s", __func__, __FILE__, __LINE__, ssh_get_error(session));
			goto finish;
		}
		n++;
	}

	cdata->username = strdup(sdata->username);
	ssh_set_channel_callbacks(sdata->sshchannel, &channel_cb);

	do {
		/* poll the main event which takes care of the session and the channel */
		if (ssh_event_dopoll(event, CLIENT_POLL_TIMEOUT) == SSH_ERROR) {
			nc_verb_error("%s: ssh error (%s:%d): %s", __func__, __FILE__, __LINE__, ssh_get_error(session));
			ssh_channel_close(sdata->sshchannel);
		}
	} while (ssh_channel_is_open(sdata->sshchannel) && !done && !cdata->done);

	if (ssh_channel_is_open(sdata->sshchannel)) {
		ssh_channel_close(sdata->sshchannel);
	}

	/* wait for the client to close the channel */
	for (n = 0; n < CLIENT_CHANNEL_CLOSE_TIMEOUT*10 && (ssh_get_status(session) & (SSH_CLOSED | SSH_CLOSED_ERROR)) == 0; n++) {
		ssh_event_dopoll(event, 100);
	}
	if (n == CLIENT_CHANNEL_CLOSE_TIMEOUT*10) {
		nc_verb_verbose("%s: client did not close the channel in time", __func__);
	}

finish:
	if (!done && !restart_soft) {
		/* if we're done, we will free all the clients */
		client_free(&ssh_clients, my_tid);
	}
	if (event != NULL) {
		ssh_event_free(event);
	}
	return NULL;
}

static struct pollfd* sock_listen(const struct bind_addr* addrs, unsigned int* count) {
	const int optVal = 1;
	const socklen_t optLen = sizeof(optVal);
	unsigned int i;
	char is_ipv4;
	struct pollfd* pollsock;
	struct sockaddr_storage saddr;

	struct sockaddr_in* saddr4;
	struct sockaddr_in6* saddr6;

	/*
	 * Always have the last pollfd structure ready -
	 * this way we can reuse it safely (continue;)
	 * every time an error occurs during its
	 * modification.
	 */
	*count = 1;
	pollsock = calloc(1, sizeof(struct pollfd));

	/* for every address... */
	for (;addrs != NULL; addrs = addrs->next) {
		if (strchr(addrs->addr, ':') == NULL) {
			is_ipv4 = 1;
		} else {
			is_ipv4 = 0;
		}

		/* ...and for every port a pollfd struct is created */
		for (i = 0; i < addrs->port_count; ++i) {
			pollsock[*count-1].fd = socket((is_ipv4 ? AF_INET : AF_INET6), SOCK_STREAM, 0);
			if (pollsock[*count-1].fd == -1) {
				nc_verb_error("%s: could not create socket (%s)", __func__, strerror(errno));
				continue;
			}

			if (setsockopt(pollsock[*count-1].fd, SOL_SOCKET, SO_REUSEADDR, (void*) &optVal, optLen) != 0) {
				nc_verb_error("%s: could not set socket SO_REUSEADDR option (%s)", __func__, strerror(errno));
				continue;
			}

			bzero(&saddr, sizeof(struct sockaddr_storage));
			if (is_ipv4) {
				saddr4 = (struct sockaddr_in*)&saddr;

				saddr4->sin_family = AF_INET;
				saddr4->sin_port = htons(addrs->ports[i]);

				if (inet_pton(AF_INET, addrs->addr, &saddr4->sin_addr) != 1) {
					nc_verb_error("%s: failed to convert IPv4 address \"%s\"", __func__, addrs->addr);
					continue;
				}

				if (bind(pollsock[*count-1].fd, (struct sockaddr*)saddr4, sizeof(struct sockaddr_in)) == -1) {
					nc_verb_error("%s: could not bind \"%s\" (%s)", __func__, addrs->addr, strerror(errno));
					continue;
				}

			} else {
				saddr6 = (struct sockaddr_in6*)&saddr;

				saddr6->sin6_family = AF_INET6;
				saddr6->sin6_port = htons(addrs->ports[i]);

				if (inet_pton(AF_INET6, addrs->addr, &saddr6->sin6_addr) != 1) {
					nc_verb_error("%s: failed to convert IPv6 address \"%s\"", __func__, addrs->addr);
					continue;
				}

				if (bind(pollsock[*count-1].fd, (struct sockaddr*)saddr6, sizeof(struct sockaddr_in6)) == -1) {
					nc_verb_error("%s: could not bind \"%s\" (%s)", __func__, addrs->addr, strerror(errno));
					continue;
				}
			}

			if (listen(pollsock[*count-1].fd, 5) == -1) {
				nc_verb_error("%s: unable to start listening on \"%s\" (%s)", __func__, addrs->addr, strerror(errno));
				continue;
			}

			pollsock[*count-1].events = POLLIN;

			pollsock = realloc(pollsock, (*count+1)*sizeof(struct pollfd));
			bzero(&pollsock[*count], sizeof(struct pollfd));
			++(*count);
		}
	}

	/* the last pollsock is not valid */
	--(*count);
	if (*count == 0) {
		free(pollsock);
		pollsock = NULL;
	}
	return pollsock;
}

static struct client_struct* sock_accept(struct pollfd* pollsock, unsigned int pollsock_count) {
	int r;
	unsigned int i;
	socklen_t client_saddr_len;
	struct client_struct* ret, *cur, *prev = NULL;

	/* poll for a new connection */
	errno = 0;
	do {
		r = poll(pollsock, pollsock_count, -1);
		if (r == -1 && errno == EINTR) {
			/* we are likely going to exit or restart */
			return NULL;
		}
		if (r == -1) {
			nc_verb_error("%s: poll failed (%s), trying again", __func__, strerror(errno));
			continue;
		}
	} while (r == 0);

	ret = calloc(1, sizeof(struct client_struct));
	cur = ret;

	/* accept every polled connection */
	for (i = 0; i < pollsock_count; ++i) {
		if (pollsock[i].revents & POLLIN) {
			client_saddr_len = sizeof(struct sockaddr_storage);

			cur->sock = accept(pollsock[i].fd, (struct sockaddr*)&cur->saddr, &client_saddr_len);
			if (cur->sock == -1) {
				nc_verb_error("%s: accept failed (%s), trying again", __func__, strerror(errno));
				continue;
			}

			cur->next = calloc(1, sizeof(struct client_struct));
			prev = cur;
			cur = cur->next;
		}

		pollsock[i].revents = 0;
	}

	if (prev == NULL) {
		/* no client accepted */
		free(ret);
		return NULL;
	}

	prev->next = NULL;
	free(cur);

	return ret;
}

static void sock_cleanup(struct pollfd* pollsock, unsigned int pollsock_count) {
	unsigned int i;

	for (i = 0; i < pollsock_count; ++i) {
		close(pollsock[i].fd);
	}
	free(pollsock);
}

void ssh_listen_loop(int do_init) {
	ssh_bind sshbind;
	ssh_session sshsession;
	int ret;
	struct client_struct* new_clients, *cur_client;

	static struct pollfd* pollsock;
	static unsigned int pollsock_count;

	if (do_init) {
		ssh_threads_set_callbacks(ssh_threads_get_pthread());
		ssh_init();
		sshbind = ssh_bind_new();

		ssh_bind_options_set(sshbind, SSH_BIND_OPTIONS_RSAKEY, KEYS_DIR "ssh_host_rsa_key");
		//ssh_bind_options_set(sshbind, SSH_BIND_OPTIONS_DSAKEY, "ssh_host_dsa_key");
		//ssh_bind_options_set(sshbind, SSH_BIND_OPTIONS_ECDSAKEY, "ssh_host_ecdsa_key");

		ssh_bind_options_set(sshbind, SSH_BIND_OPTIONS_LOG_VERBOSITY_STR, "3");

		pollsock = sock_listen(ssh_binds, &pollsock_count);
		if (pollsock == NULL) {
			nc_verb_error("%s: failed to listen on any address", __func__);
			return;
		}
	}

	while (1) {
		new_clients = sock_accept(pollsock, pollsock_count);
		if (new_clients == NULL) {
			if (done) {
				/* signal received wanting us to exit or restart */
				break;
			} else {
				/* quite weird, but let it slide */
				nc_verb_verbose("%s: sock_accept returned NULL, should not happen", __func__);
				continue;
			}
		}

		for (cur_client = new_clients; cur_client != NULL; cur_client = cur_client->next) {
			sshsession = ssh_new();
			if (sshsession == NULL) {
				nc_verb_error("%s: ssh error: failed to allocate a new SSH session (%s:%d)", __func__, __FILE__, __LINE__);
				break;
			}

			if (ssh_bind_accept_fd(sshbind, sshsession, cur_client->sock) != SSH_ERROR) {
				/* add the client into the global ssh_clients structure */
				client_append(&ssh_clients, cur_client);

				if ((ret = pthread_create(&cur_client->thread_id, NULL, ssh_client_thread, (void*)sshsession)) != 0) {
					nc_verb_error("%s: failed to create a dedicated SSH client thread (%s)", strerror(ret));
					client_free(&ssh_clients, cur_client->thread_id);
					ssh_disconnect(sshsession);
					ssh_free(sshsession);
					break;
				}
				pthread_detach(cur_client->thread_id);
			} else {
				nc_verb_error("%s: SSH failed to accept a new connection: %s", __func__, ssh_get_error(sshbind));
				ssh_free(sshsession);
				break;
			}
		}

		if (cur_client != NULL) {
			/* propagate break */
			done = 1;
			break;
		}
	}

	if (!restart_soft) {
		sock_cleanup(pollsock, pollsock_count);
		client_cleanup(&ssh_clients);
		ssh_bind_free(sshbind);
		ssh_finalize();
	}
}
