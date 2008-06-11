/* Copyright (C) 2007 Timo Sirainen */

#include "lib.h"
#include "ioloop.h"
#include "array.h"
#include "istream.h"
#include "ostream.h"
#include "imap-parser.h"

#include "imap-args.h"
#include "imap-seqset.h"
#include "settings.h"
#include "mailbox.h"
#include "mailbox-state.h"
#include "commands.h"
#include "checkpoint.h"
#include "search.h"
#include "test-exec.h"
#include "client.h"

#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>

int clients_count = 0;
unsigned int total_disconnects = 0;
ARRAY_TYPE(client) clients;
ARRAY_DEFINE(stalled_clients, unsigned int);
bool stalled = FALSE, disconnect_clients = FALSE, no_new_clients = FALSE;

static unsigned int global_id_counter = 0;

static void
client_rawlog_input(struct client *client, const unsigned char *data,
		    size_t size);

int client_input_error(struct client *client, const char *fmt, ...)
{
	va_list va;

	va_start(va, fmt);
	i_error("%s[%u]: %s: %s", client->username, client->global_id,
		t_strdup_vprintf(fmt, va), imap_args_to_str(client->cur_args));
	va_end(va);

	client_disconnect(client);
	if (conf.error_quit)
		exit(2);
	return -1;
}

int client_state_error(struct client *client, const char *fmt, ...)
{
	va_list va;

	va_start(va, fmt);
	i_error("%s[%u]: %s: %s", client->username, client->global_id,
		t_strdup_vprintf(fmt, va), imap_args_to_str(client->cur_args));
	va_end(va);

	if (conf.error_quit)
		exit(2);
	return -1;
}

void client_exists(struct client *client, unsigned int msgs)
{
	unsigned int old_count = array_count(&client->view->uidmap);

	if (msgs < old_count) {
		client_input_error(client, "Message count dropped %u -> %u",
				   old_count, msgs);
		array_delete(&client->view->uidmap, msgs, old_count - msgs);
		return;
	}
	for (; old_count < msgs; old_count++)
		(void)array_append_space(&client->view->uidmap);
}

static int client_expunge(struct client *client, unsigned int seq)
{
	struct message_metadata_dynamic *metadata;

	metadata = array_idx_modifiable(&client->view->messages, seq - 1);
	if (metadata->fetch_refcount > 0) {
		client_input_error(client,
			"Referenced message expunged seq=%u uid=%u",
			seq, metadata->ms == NULL ? 0 : metadata->ms->uid);
		return -1;
	}
	mailbox_view_expunge(client->view, seq);
	return 0;
}

static int client_expunge_uid(struct client *client, uint32_t uid)
{
	const uint32_t *uidmap;
	unsigned int i, count;

	/* if there are unknown UIDs we don't really know which one of them
	   we should expunge, but it doesn't matter because they contain no
	   metadata at that point. */
	uidmap = array_get(&client->view->uidmap, &count);
	for (i = 0; i < count; i++) {
		if (uid <= uidmap[i]) {
			if (uid == uidmap[i]) {
				/* found it */
				client_expunge(client, i + 1);
				return 0;
			}
			break;
		}
	}

	/* there are one or more unknown messages. expunge the last one of them
	   (none of them should have any attached metadata) */
	if (i == 0 || uidmap[i-1] != 0) {
		client_input_error(client, "VANISHED UID=%u not found", uid);
		return -1;
	}

	client_expunge(client, i);
	return 0;
}

static void
client_expunge_uid_range(struct client *client,
			 const ARRAY_TYPE(seq_range) *expunged_uids)
{
	const struct seq_range *expunges;
	const uint32_t *uidmap;
	unsigned int seq, uid_count, expunge_count;

	expunges = array_get(expunged_uids, &expunge_count);
	uidmap = array_get(&client->view->uidmap, &uid_count);
	for (seq = uid_count; seq > 0; seq--) {
		i_assert(uidmap[seq-1] != 0);

		if (seq_range_exists(expunged_uids, uidmap[seq-1])) {
			client_expunge(client, seq);
			uidmap = array_get(&client->view->uidmap, &uid_count);
		}
	}
}

static void client_enabled(struct client *client, const struct imap_arg *args)
{
	const char *str;

	for (; args->type == IMAP_ARG_ATOM; args++) {
		str = IMAP_ARG_STR_NONULL(args);
		if (strcasecmp(str, "QRESYNC") == 0)
			client->qresync_enabled = TRUE;
	}
}

static int client_vanished(struct client *client, const struct imap_arg *args)
{
	struct mailbox_view *view = client->view;
	ARRAY_TYPE(seq_range) uids;
	const struct seq_range *range;
	unsigned int i, count;
	const char *uidset;
	uint32_t uid;

	if (!client->qresync_enabled) {
		client_input_error(client,
			"Server sent VANISHED but we hadn't enabled QRESYNC");
		return -1;
	}

	if (args->type == IMAP_ARG_LIST) {
		const ARRAY_TYPE(imap_arg_list) *list;
		const struct imap_arg *subargs;

		list = IMAP_ARG_LIST(args);
		subargs = array_idx(list, 0);
		if (subargs->type == IMAP_ARG_ATOM &&
		    subargs[1].type == IMAP_ARG_EOL &&
		    strcmp(IMAP_ARG_STR_NONULL(subargs), "EARLIER") == 0) {
			if (client->qresync_select_cache == NULL) {
				/* we don't care */
				return 0;
			}
			/* SELECTing with QRESYNC */
			args++;
		}
	}

	if (args->type != IMAP_ARG_ATOM || args[1].type != IMAP_ARG_EOL) {
		client_input_error(client, "Invalid VANISHED parameters");
		return -1;
	}
	uidset = IMAP_ARG_STR_NONULL(args);

	t_array_init(&uids, 16);
	if (imap_seq_set_parse(uidset, &uids) < 0) {
		client_input_error(client, "Invalid VANISHED sequence-set");
		return -1;
	}

	if (view->known_uid_count == array_count(&view->uidmap)) {
		/* all UIDs are known - we can handle UIDs that are already
		   expunged. this happens normally when doing a SELECT QRESYNC
		   and server couldn't keep track of only the new expunges. */
		client_expunge_uid_range(client, &uids);
		return 0;
	}

	/* we assume that there are no extra UIDs in the reply, even though
	   it's only a SHOULD in the spec. way too difficult to handle
	   otherwise. */
	range = array_get(&uids, &count);
	for (i = 0; i < count; i++) {
		for (uid = range[i].seq1; uid <= range[i].seq2; uid++)
			client_expunge_uid(client, uid);
	}
	return 0;
}

void client_capability_parse(struct client *client, const char *line)
{
	const char *const *tmp;
	unsigned int i;

	if (client->login_state != LSTATE_NONAUTH)
		client->postlogin_capability = TRUE;

	client->capabilities = 0;
	if (client->capabilities_list != NULL)
		p_strsplit_free(default_pool, client->capabilities_list);
	client->capabilities_list = p_strsplit(default_pool, line, " ");

	for (tmp = t_strsplit(line, " "); *tmp != NULL; tmp++) {
		for (i = 0; cap_names[i].name != NULL; i++) {
			if (strcasecmp(*tmp, cap_names[i].name) == 0) {
				client->capabilities |= cap_names[i].capability;
				break;
			}
		}
	}
}

int client_handle_untagged(struct client *client, const struct imap_arg *args)
{
	struct mailbox_view *view = client->view;
	const char *str;

	if (args->type != IMAP_ARG_ATOM)
		return -1;
	str = t_str_ucase(IMAP_ARG_STR(args));
	args++;

	if (is_numeric(str, '\0')) {
		unsigned int num = strtoul(str, NULL, 10);

		if (args->type != IMAP_ARG_ATOM)
			return -1;
		str = t_str_ucase(IMAP_ARG_STR(args));
		args++;

		if (strcmp(str, "EXISTS") == 0)
			client_exists(client, num);

                if (num > array_count(&view->uidmap) &&
		    client->last_cmd->state > STATE_SELECT) {
			client_input_error(client,
				"seq too high (%u > %u, state=%s)",
				num, array_count(&view->uidmap),
                                states[client->last_cmd->state].name);
		} else if (strcmp(str, "EXPUNGE") == 0) {
			if (client_expunge(client, num) < 0)
				return -1;
		} else if (strcmp(str, "RECENT") == 0) {
			view->recent_count = num;
			if (view->recent_count ==
			    array_count(&view->uidmap))
				view->storage->seen_all_recent = TRUE;
		} else if (!conf.no_tracking && strcmp(str, "FETCH") == 0)
			mailbox_state_handle_fetch(client, num, args);
	} else if (strcmp(str, "BYE") == 0) {
		if (client->last_cmd == NULL ||
		    client->last_cmd->state != STATE_LOGOUT) {
			str = args->type != IMAP_ARG_ATOM ? NULL :
				IMAP_ARG_STR(args);
			client_input_error(client, "Unexpected BYE");
		} else
			counters[client->last_cmd->state]++;
		client_mailbox_close(client);
		client->login_state = LSTATE_NONAUTH;
	} else if (strcmp(str, "FLAGS") == 0) {
		if (mailbox_state_set_flags(view, args) < 0)
			client_input_error(client, "Broken FLAGS");
	} else if (strcmp(str, "CAPABILITY") == 0)
		client_capability_parse(client, imap_args_to_str(args));
	else if (strcmp(str, "SEARCH") == 0)
		search_result(client, args);
	else if (strcmp(str, "ENABLED") == 0)
		client_enabled(client, args);
	else if (strcmp(str, "VANISHED") == 0) {
		if (client_vanished(client, args) < 0)
			return -1;
	} else if (strcmp(str, "THREAD") == 0) {
		i_free(view->last_thread_reply);
		view->last_thread_reply =
			i_strdup(imap_args_to_str(args + 1));
	} else if (strcmp(str, "OK") == 0) {
		client_handle_resp_text_code(client, args);
	} else if (strcmp(str, "NO") == 0) {
		/*i_info("%s: %s", client->username, line + 2);*/
	} else if (strcmp(str, "BAD") == 0) {
		client_input_error(client, "BAD received");
	}
	return 0;
}

static int
client_input_args(struct client *client, const struct imap_arg *args)
{
	const char *p, *tag, *tag_status;
	struct command *cmd;
	enum command_reply reply;

	if (args->type != IMAP_ARG_ATOM)
		return client_input_error(client, "Broken tag");
	tag = IMAP_ARG_STR(args);
	args++;

	if (strcmp(tag, "+") == 0) {
		if (client->last_cmd == NULL) {
			return client_input_error(client,
				"Unexpected command contination");
		}
		client->last_cmd->callback(client, client->last_cmd,
					   args, REPLY_CONT);
		return 0;
	}
	if (strcmp(tag, "*") == 0) {
		if (client->handle_untagged(client, args) < 0) {
			return client_input_error(client,
						  "Invalid untagged input");
		}
		return 0;
	}

	/* tagged reply */
	if (args->type != IMAP_ARG_ATOM) {
		return client_input_error(client, "Broken tagged reply");
	}
	tag_status = IMAP_ARG_STR(args);

	p = strchr(tag, '.');
	cmd = p != NULL &&
		atoi(t_strdup_until(tag, p)) == (int)client->global_id ?
		command_lookup(client, atoi(t_strcut(p+1, ' '))) : NULL;
	if (cmd == NULL) {
		return client_input_error(client, "Unexpected tagged reply: %s",
					  tag);
	}

	if (strcasecmp(tag_status, "OK") == 0)
		reply = REPLY_OK;
	else if (strcasecmp(tag_status, "NO") == 0)
		reply = REPLY_NO;
	else if (strcasecmp(tag_status, "BAD") == 0) {
		reply = REPLY_BAD;
		if (!cmd->expect_bad) {
			client_input_error(client, "BAD reply for command: %s",
					   cmd->cmdline);
		}
	} else {
		return client_input_error(client, "Broken tagged reply");
	}

	command_unlink(client, cmd);

	o_stream_cork(client->output);
	cmd->callback(client, cmd, args, reply);
	client_cmd_reply_finish(client);
	o_stream_uncork(client->output);
	command_free(cmd);
	return 0;
}

static bool client_skip_literal(struct client *client)
{
	const unsigned char *data;
	size_t size;

	if (client->literal_left == 0)
		return TRUE;

	data = i_stream_get_data(client->input, &size);
	if (size < client->literal_left) {
		client->literal_left -= size;
		i_stream_skip(client->input, size);
		return FALSE;
	} else {
		i_stream_skip(client->input, client->literal_left);
		client->literal_left = 0;
		return TRUE;
	}
}

static void client_input(struct client *client)
{
	const struct imap_arg *imap_args;
	const char *line, *p;
	uoff_t literal_size;
	const unsigned char *data;
	size_t size;
	bool fatal;
	int ret;

        client->last_io = ioloop_time;

	switch (i_stream_read(client->input)) {
	case 0:
		return;
	case -1:
		/* disconnected */
		client_unref(client, TRUE);
		return;
	case -2:
		/* buffer full */
		i_error("line too long");
		client_unref(client, TRUE);
		return;
	}

	if (client->rawlog_output != NULL) {
		data = i_stream_get_data(client->input, &size);
		i_assert(client->prev_size <= size);
		if (client->prev_size != size) {
			client_rawlog_input(client,
					    data + client->prev_size,
					    size - client->prev_size);
		}
	}

	if (!client->seen_banner) {
		/* we haven't received the banner yet */
		line = i_stream_next_line(client->input);
		if (line == NULL)
			return;
		client->seen_banner = TRUE;

		p = strstr(line, "[CAPABILITY ");
		if (p == NULL)
			command_send(client, "CAPABILITY", state_callback);
		else {
			client_capability_parse(client, t_strcut(p + 12, ']'));
			(void)client_send_more_commands(client);
		}
	}

	while (client_skip_literal(client)) {
		ret = imap_parser_read_args(client->parser, 0,
					    IMAP_PARSE_FLAG_LITERAL_SIZE |
					    IMAP_PARSE_FLAG_ATOM_ALLCHARS,
					    &imap_args);
		if (ret == -2) {
			/* need more data */
			break;
		}
		if (ret < 0) {
			/* some error */
			client_input_error(client,
				"error parsing input: %s",
				imap_parser_get_error(client->parser, &fatal));
			return;
		}
		if (imap_args->type == IMAP_ARG_EOL) {
			/* FIXME: we get here, but we shouldn't.. */
			client->refcount++;
		} else {
			if (imap_parser_get_literal_size(client->parser,
							 &literal_size)) {
				if (literal_size <= MAX_INLINE_LITERAL_SIZE) {
					/* read the literal */
					imap_parser_read_last_literal(
						client->parser);
					continue;
				}
				/* literal too large. we still have to skip it
				   though. */
				client->literal_left = literal_size;
				continue;
			}

			/* FIXME: we should call this for large
			   literals too.. */
			client->refcount++;
			client->cur_args = imap_args;
			t_push();
			ret = client_input_args(client, imap_args);
			t_pop();
			client->cur_args = NULL;
		}

		if (client->literal_left == 0) {
			/* skip CRLF */
			imap_parser_reset(client->parser);

			data = i_stream_get_data(client->input, &size);
			if (size > 0 && data[0] == '\r') {
				i_stream_skip(client->input, 1);
				data = i_stream_get_data(client->input, &size);
			}
			if (size > 0 && data[0] == '\n')
				i_stream_skip(client->input, 1);
		}

		if (!client_unref(client, TRUE) || ret < 0)
			return;
	}

	if (do_rand(STATE_DISCONNECT)) {
		/* random disconnection */
		counters[STATE_DISCONNECT]++;
		client_unref(client, TRUE);
		return;
	}

	(void)i_stream_get_data(client->input, &client->prev_size);
	if (client->input->closed)
		client_unref(client, TRUE);
}

static void client_delay_timeout(void *context)
{
	struct client *client = context;

	i_assert(client->io == NULL);

	client->delayed = FALSE;
	client->last_io = ioloop_time;

	timeout_remove(&client->to);
	client->io = io_add(i_stream_get_fd(client->input),
			    IO_READ, client_input, client);
}

void client_delay(struct client *client, unsigned int msecs)
{
	i_assert(client->to == NULL);

	client->delayed = TRUE;
	io_remove(&client->io);
	client->to = timeout_add(msecs, client_delay_timeout, client);
}

static int client_output(void *context)
{
        struct client *client = context;
	int ret;

	o_stream_cork(client->output);
	ret = o_stream_flush(client->output);
	client->last_io = ioloop_time;

	if (client->append_vsize_left > 0) {
		if (client_append_continue(client) < 0)
			client_unref(client, TRUE);
	}
	o_stream_uncork(client->output);

        return ret;
}

static void client_wait_connect(void *context)
{
	struct client *client = context;
	int err, fd;

	fd = i_stream_get_fd(client->input);
	err = net_geterror(fd);
	if (err != 0) {
		i_error("connect() failed: %s", strerror(err));
		client_unref(client, TRUE);
		return;
	}

	io_remove(&client->io);
	client->io = io_add(fd, IO_READ, client_input, client);
}

struct client *client_new(unsigned int idx, struct mailbox_source *source)
{
	struct client *client;
	const char *mailbox;
	int fd;

	i_assert(idx >= array_count(&clients) ||
		 *array_idx(&clients, idx) == NULL);
	/*if (stalled) {
		array_append(&stalled_clients, &idx, 1);
		return NULL;
	}*/

	fd = net_connect_ip(&conf.ip, conf.port, NULL);
	if (fd < 0) {
		i_error("connect() failed: %m");
		return NULL;
	}

	client = i_new(struct client, 1);
	client->refcount = 1;
	client->tag_counter = 1;
	client->idx = idx;
	client->global_id = ++global_id_counter;
	mailbox = t_strdup_printf(conf.mailbox, idx);
	client->storage = mailbox_storage_get(source, mailbox);
	client->view = mailbox_view_new(client->storage);
	if (strchr(conf.mailbox, '%') != NULL)
		client->try_create_mailbox = TRUE;
	client->fd = fd;
	client->input = i_stream_create_fd(fd, 1024*64, FALSE);
	client->output = o_stream_create_fd(fd, (size_t)-1, FALSE);
	if (conf.rawlog) {
		int log_fd;
		const char *rawlog_path;

		rawlog_path = t_strdup_printf("rawlog.%u", client->global_id);
		log_fd = open(rawlog_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
		if (log_fd == -1)
			i_fatal("creat(%s) failed: %m", rawlog_path);
		client->rawlog_output = o_stream_create_fd(log_fd, 0, TRUE);
	}
	client->parser = imap_parser_create(client->input, NULL, (size_t)-1);
	client->io = io_add(fd, IO_READ, client_wait_connect, client);
	client->username = i_strdup_printf(conf.username_template,
					   (int)(random() % USER_RAND + 1),
					   (int)(random() % DOMAIN_RAND + 1));
        client->last_io = ioloop_time;
	i_array_init(&client->commands, 16);
	o_stream_set_flush_callback(client->output, client_output, client);
	clients_count++;

	client->handle_untagged = client_handle_untagged;
	client->send_more_commands = client_plan_send_more_commands;

        array_idx_set(&clients, idx, &client);
        return client;
}

void client_disconnect(struct client *client)
{
	i_stream_close(client->input);
	o_stream_close(client->output);

	if (client->io != NULL)
		io_remove(&client->io);
	if (client->to != NULL)
		timeout_remove(&client->to);
	client->to = timeout_add(0, client_input, client);
}

bool client_unref(struct client *client, bool reconnect)
{
	struct mailbox_storage *storage = client->storage;
	unsigned int idx = client->idx;
	struct command *const *cmds;
	unsigned int i, count;
	bool checkpoint;

	i_assert(client->refcount > 0);
	if (--client->refcount > 0)
		return TRUE;

	total_disconnects++;
	if (conf.disconnect_quit)
		exit(1);

	if (--clients_count == 0)
		stalled = FALSE;
	array_idx_clear(&clients, idx);

	cmds = array_get(&client->commands, &count);
	checkpoint = client->checkpointing != NULL;
	for (i = 0; i < count; i++)
		command_free(cmds[i]);
	array_free(&client->commands);

	if (client->qresync_select_cache != NULL)
		mailbox_offline_cache_unref(&client->qresync_select_cache);

	client_mailbox_close(client);
	mailbox_view_free(&client->view);

	if (client->io != NULL)
		io_remove(&client->io);
	if (client->to != NULL)
		timeout_remove(&client->to);
	if (close(client->fd) < 0)
		i_error("close(client) failed: %m");
	if (client->rawlog_output != NULL)
		o_stream_destroy(&client->rawlog_output);
	imap_parser_destroy(&client->parser);

	if (client->test_exec_ctx != NULL) {
		/* storage must be fully unreferenced before new test can
		   begin. */
		mailbox_storage_unref(&client->storage);
		test_execute_cancel_by_client(client);
	}

	if (client->capabilities_list != NULL)
		p_strsplit_free(default_pool, client->capabilities_list);
	o_stream_unref(&client->output);
	i_stream_unref(&client->input);
	i_free(client->username);

	if (clients_count == 0 && disconnect_clients)
		io_loop_stop(current_ioloop);
	else if (io_loop_is_running(current_ioloop) && !no_new_clients &&
		 !disconnect_clients && reconnect) {
		client_new(idx, storage->source);
		if (!stalled) {
			const unsigned int *indexes;
			unsigned int i, count;

			indexes = array_get(&stalled_clients, &count);
			for (i = 0; i < count && i < 3; i++)
				client_new(indexes[i], storage->source);
			array_delete(&stalled_clients, 0, i);
		}
	}
	i_free(client);

	if (storage != NULL) {
		if (checkpoint)
			checkpoint_neg(storage);
		mailbox_storage_unref(&storage);
	}
	return FALSE;
}

void client_mailbox_close(struct client *client)
{
	if (client->login_state == LSTATE_SELECTED) {
		if (rand() % 3 == 0)
			mailbox_view_save_offline_cache(client->view);

		client->login_state = LSTATE_AUTH;
	}
	mailbox_view_free(&client->view);
	client->view = mailbox_view_new(client->storage);
}

int client_send_more_commands(struct client *client)
{
	int ret;

	o_stream_cork(client->output);
	ret = client->send_more_commands(client);
	o_stream_uncork(client->output);
	return ret;
}

static void
client_rawlog_line(struct client *client, const void *data, size_t size,
		   bool partial)
{
	struct const_iovec iov[3];
	char timestamp[256];
	struct timeval tv;

	if (gettimeofday(&tv, NULL) < 0)
		timestamp[0] = '\0';
	else {
		i_snprintf(timestamp, sizeof(timestamp), "%lu.%06u ",
			   (unsigned long)tv.tv_sec, (unsigned int)tv.tv_usec);
	}

	iov[0].iov_base = timestamp;
	iov[0].iov_len = strlen(timestamp);
	iov[1].iov_base = data;
	iov[1].iov_len = size;
	iov[2].iov_base = ">>\n";
	iov[2].iov_len = 3;
	o_stream_sendv(client->rawlog_output, iov, partial ? 3 : 2);
}

static void
client_rawlog_input(struct client *client, const unsigned char *data,
		    size_t size)
{
	size_t i, start = 0;

	for (i = 0; i < size; i++) {
		if (data[i] == '\n') {
			client_rawlog_line(client, data + start,
					   i - start + 1, FALSE);
			start = i + 1;
		}
	}
	if (start != size)
		client_rawlog_line(client, data + start, size - start, TRUE);
}

void client_rawlog_output(struct client *client, const char *line)
{
	client_rawlog_line(client, line, strlen(line), FALSE);
}

unsigned int clients_get_random_idx(void)
{
	struct client *const *c;
	unsigned int i, idx, count;

	/* first try randomly */
	c = array_get(&clients, &count);
	for (i = 0; i < 100; i++) {
		idx = rand() % count;
		if (c[idx] != NULL)
			return idx;
	}
	/* then just try anything */
	for (i = 0; i < count; i++) {
		if (c[i] != NULL)
			return i;
	}
	i_unreached();
	return 0;
}

void clients_init(void)
{
	i_array_init(&stalled_clients, CLIENTS_COUNT);
}

void clients_deinit(void)
{
	array_free(&stalled_clients);
}
