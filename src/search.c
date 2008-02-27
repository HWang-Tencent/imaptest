/* Copyright (C) 2008 Timo Sirainen */

#include "lib.h"
#include "array.h"
#include "str.h"
#include "strescape.h"
#include "utc-mktime.h"
#include "imap-date.h"
#include "imap-parser.h"
#include "commands.h"
#include "mailbox.h"
#include "client.h"
#include "search.h"

#include <time.h>
#include <stdlib.h>

enum search_arg_type {
	SEARCH_OR,
	SEARCH_SUB,

	/* sequence sets */
	SEARCH_SEQSET,

	/* size */
	SEARCH_SMALLER,
	SEARCH_LARGER,

	/* date */
	SEARCH_BEFORE,
	SEARCH_ON,
	SEARCH_SINCE,
	SEARCH_SENTBEFORE,
	SEARCH_SENTON,
	SEARCH_SENTSINCE,

	/* string */
	SEARCH_SUBJECT,

	SEARCH_TYPE_COUNT
};

struct search_node {
	enum search_arg_type type;

	/* for AND/ORs */
	struct search_node *first_child, *next_sibling;
	ARRAY_TYPE(seq_range) seqset;
	const char *str;
	uoff_t size;
	time_t date;
};

struct search_context {
	pool_t pool;
	struct client *client;
	struct search_node root;

	unsigned int build_msg_idx;
	ARRAY_TYPE(seq_range) result;
};

static int
search_node_verify_msg(struct client *client, struct search_node *node,
		       const struct message_metadata_static *ms)
{
	const char *str;
	time_t t;
	int tz;

	switch (node->type) {
	case SEARCH_SMALLER:
	case SEARCH_LARGER:
		if (ms->msg == NULL || ms->msg->full_size == 0)
			break;
		if (node->type == SEARCH_SMALLER)
			return ms->msg->full_size < node->size;
		else
			return ms->msg->full_size > node->size;
		break;
	case SEARCH_BEFORE:
	case SEARCH_ON:
	case SEARCH_SINCE:
		if (ms->internaldate == 0)
			break;
		t = ms->internaldate + ms->internaldate_tz*60;
		switch (node->type) {
		case SEARCH_BEFORE:
			return t < node->date;
		case SEARCH_ON:
			return t >= node->date && t < node->date + 3600*24;
		case SEARCH_SINCE:
			return t >= node->date;
		default:
			i_unreached();
		}
	case SEARCH_SENTBEFORE:
	case SEARCH_SENTON:
	case SEARCH_SENTSINCE:
		if (ms->msg == NULL)
			break;

		if (!mailbox_global_get_sent_date(ms->msg, &t, &tz))
			break;
		if (t == (time_t)-1)
			break;

		t += tz * 60;
		switch (node->type) {
		case SEARCH_SENTBEFORE:
			return t < node->date;
		case SEARCH_SENTON:
			return t >= node->date && t < node->date + 3600*24;
		case SEARCH_SENTSINCE:
			return t >= node->date;
		default:
			i_unreached();
		}
		break;
	case SEARCH_SUBJECT:
		if (ms->msg == NULL)
			break;
		if (!mailbox_global_get_subject_utf8(
				client->view->storage->source, ms->msg, &str))
			break;
		if (str == NULL) {
			/* Subject: header doesn't exist */
			return 0;
		}
		return strstr(str, node->str) != NULL;

	case SEARCH_OR:
	case SEARCH_SUB:
	case SEARCH_SEQSET:
	case SEARCH_TYPE_COUNT:
		i_unreached();
	}
	return -1;
}

static int
search_node_verify(struct client *client, struct search_node *node,
		   uint32_t seq, bool parent_or)
{
	const struct message_metadata_static *ms;
	int ret = -1, ret2;

	switch (node->type) {
	case SEARCH_OR:
	case SEARCH_SUB:
		ret = search_node_verify(client, node->first_child, seq,
					 node->type == SEARCH_OR);
		break;
	case SEARCH_SEQSET:
		ret = seq_range_exists(&node->seqset, seq);
		break;
	default:
		ms = message_metadata_static_lookup_seq(client->view, seq);
		if (ms != NULL)
			ret = search_node_verify_msg(client, node, ms);
		break;
	}

	if (ret == 0 && !parent_or)
		return 0;
	if (ret > 0 && parent_or)
		return 1;

	if (node->next_sibling == NULL)
		return ret;
	ret2 = search_node_verify(client, node->next_sibling, seq, parent_or);
	if (parent_or) {
		if (ret == -1 && ret2 == 0)
			ret2 = -1;
	} else {
		if (ret == -1 && ret2 > 0)
			ret2 = -1;
	}
	return ret2;
}

static void search_verify_result(struct client *client)
{
	struct search_context *ctx = client->search_ctx;
	const uint32_t *uids;
	uint32_t seq, msgs;
	int ret;
	bool found;

	uids = array_get(&client->view->uidmap, &msgs);
	for (seq = 1; seq <= msgs; seq++) {
		ret = search_node_verify(client, &ctx->root, seq, FALSE);
		found = seq_range_exists(&ctx->result, seq);
		if (ret > 0 && !found) {
			client_input_error(client,
				"SEARCH result missing seq %u (uid %u)",
				seq, uids[seq-1]);
		} else if (ret == 0 && found) {
			client_input_error(client,
				"SEARCH result has extra seq %u (uid %u)",
				seq, uids[seq-1]);
		}
	}
}

static void search_callback(struct client *client, struct command *cmd,
			    const struct imap_arg *args ATTR_UNUSED,
			    enum command_reply reply)
{
	i_assert(client->search_ctx != NULL);

	if (reply != REPLY_OK)
		client_input_error(client, "SEARCH failed");
	else if (!array_is_created(&client->search_ctx->result))
		client_input_error(client, "Missing untagged SEARCH");
	else {
		counters[cmd->state]++;
		search_verify_result(client);
	}
	pool_unref(&client->search_ctx->pool);
	client->search_ctx = NULL;
}

static time_t time_truncate_to_day(time_t t)
{
	const struct tm *tm;
	struct tm day_tm;

	tm = gmtime(&t);
	day_tm = *tm;
	day_tm.tm_hour = 0;
	day_tm.tm_min = 0;
	day_tm.tm_sec = 0;
	day_tm.tm_isdst = -1;
	return utc_mktime(&day_tm);
}

static bool node_children_has_conflict(struct search_node *parent,
				       enum search_arg_type type)
{
	struct search_node *node;

	node = parent->first_child;
	for (; node != NULL; node = node->next_sibling) {
		switch (node->type) {
		case SEARCH_SEQSET:
		case SEARCH_SMALLER:
		case SEARCH_LARGER:
			if (type == node->type)
				return TRUE;
			break;
		case SEARCH_ON:
			if (type == SEARCH_ON ||
			    type == SEARCH_BEFORE || type == SEARCH_SINCE)
				return TRUE;
			break;
		case SEARCH_BEFORE:
		case SEARCH_SINCE:
			if (type == SEARCH_ON || type == node->type)
				return TRUE;
			break;
		case SEARCH_SENTON:
			if (type == SEARCH_SENTON ||
			    type == SEARCH_SENTBEFORE ||
			    type == SEARCH_SENTSINCE)
				return TRUE;
			break;
		case SEARCH_SENTBEFORE:
		case SEARCH_SENTSINCE:
			if (type == SEARCH_SENTON || type == node->type)
				return TRUE;
			break;
		default:
			break;
		}
	}
	return FALSE;
}

static struct search_node *
search_command_build(struct search_context *ctx, struct search_node *parent,
		     int probability)
{
	struct client *client = ctx->client;
	pool_t pool = client->search_ctx->pool;
	struct message_metadata_static *const *ms, *m1 = NULL, *m2 = NULL;
	struct search_node *node;
	unsigned int i, msgs, ms_count;

	if ((rand() % 100) >= probability)
		return NULL;

	ms = array_get(&client->view->storage->static_metadata, &ms_count);
	if (ctx->build_msg_idx >= ms_count)
		ctx->build_msg_idx = 0;

	node = p_new(pool, struct search_node, 1);
again:
	node->type = rand() % SEARCH_TYPE_COUNT;
	if (node_children_has_conflict(parent, node->type)) {
		/* can't add this type, try again */
		goto again;
	}
	switch (node->type) {
	case SEARCH_SUB:
		if (parent->type == SEARCH_SUB)
			node->type = SEARCH_OR;
	case SEARCH_OR:
		if (parent->type == SEARCH_OR && node->type == SEARCH_OR)
			goto again;
		probability -= I_MAX(probability/30, 1);
		node->first_child =
			search_command_build(ctx, node, probability);
		if (node->first_child == NULL)
			goto again;
		if (node->first_child->next_sibling == NULL) {
			/* just a single child - replace the sub node by it */
			node->next_sibling = node->first_child;
			node->first_child = NULL;
			node = node->next_sibling;
		}
		break;
	case SEARCH_SEQSET:
		p_array_init(&node->seqset, pool, 5);
		msgs = array_count(&client->view->uidmap);
		if (!client_get_random_seq_range(client, &node->seqset,
						 msgs / 2 + 1,
						 CLIENT_RANDOM_FLAG_TYPE_NONE))
			goto again;
		break;
	case SEARCH_SMALLER:
	case SEARCH_LARGER:
		/* find two messages with known sizes and use their average */
		for (i = ctx->build_msg_idx; i < ms_count; i++) {
			if (ms[i]->msg != NULL && ms[i]->msg->full_size != 0) {
				if (m1 == NULL)
					m1 = ms[i];
				else {
					m2 = ms[i];
					break;
				}
			}
		}
		if (m2 != NULL) {
			node->size = (m1->msg->full_size +
				      m2->msg->full_size) / 2;
		}
		if (node->size == 0)
			node->size = 2048 + (rand() % 2048);
		break;
	case SEARCH_BEFORE:
	case SEARCH_ON:
	case SEARCH_SINCE:
		/* find two messages with known internalsizes and use their
		   average */
		for (i = ctx->build_msg_idx; i < ms_count; i++) {
			if (ms[i]->internaldate != 0) {
				if (m1 == NULL)
					m1 = ms[i];
				else {
					m2 = ms[i];
					break;
				}
			}
		}
		if (m2 != NULL)
			node->date = ((long long)m1->internaldate +
				      (long long)m2->internaldate) / 2;
		if (node->date == 0)
			node->date = time(NULL) - (3600*24 * (rand() % 10));
		node->date = time_truncate_to_day(node->date);
		break;
	case SEARCH_SENTBEFORE:
	case SEARCH_SENTON:
	case SEARCH_SENTSINCE: {
		time_t t, t1 = 0, t2 = 0;
		int tz;

		/* find two messages with known dates and use their average */
		for (i = ctx->build_msg_idx; i < ms_count; i++) {
			if (ms[i]->msg != NULL &&
			    mailbox_global_get_sent_date(ms[i]->msg, &t, &tz) &&
			    t != 0 && t != (time_t)-1) {
				t += tz * 60;
				if (t1 == 0)
					t1 = t;
				else {
					t2 = t;
					break;
				}
			}
		}
		if (t2 != 0)
			node->date = ((long long)t1 + (long long)t2) / 2;
		if (t2 == 0)
			node->date = time(NULL) - (3600*24 * (rand() % 10));
		node->date = time_truncate_to_day(node->date);
		break;
	}
	case SEARCH_SUBJECT: {
		struct mailbox_source *source = client->view->storage->source;
		const char *str = NULL, *const *words;
		unsigned int len, count, start;

		/* find a random subject */
		for (i = ctx->build_msg_idx; i < ms_count; i++) {
			if (ms[i]->msg != NULL &&
			    mailbox_global_get_subject_utf8(source, ms[i]->msg,
							    &str) &&
			    str != NULL && *str != '\0')
				break;
		}
		if (str == NULL) {
			/* check for existence of subject header */
			str = "";
		} else if (rand() % 10 == 0) {
			/* search for the entire subject */
		} else {
			/* get a random word within the subject */
			words = t_strsplit_spaces(str, " ");
			count = str_array_length(words);
			str = count == 0 ? "" : words[rand() % count];

			/* get a random substring from the word */
			len = strlen(str);
			if (len > 1) {
				start = rand() % (len - 1);
				len = rand() % (len - 1 - start) + 1;
				str = t_strndup(str + start, len);
			}
		}
		node->str = p_strdup(pool, str);
		break;
	}
	case SEARCH_TYPE_COUNT:
		i_unreached();
	}

	probability /= 2;
	node->next_sibling = search_command_build(ctx, parent, probability);
	return node;
}

static bool str_need_escaping(const char *str)
{
	for (; *str != '\0'; str++) {
		if (*str < 32 || *str == '\\' || *str == '"' ||
		    (unsigned char)*str >= 128)
			return TRUE;
	}
	return FALSE;
}

static void search_command_append(string_t *cmd, const struct search_node *node)
{
	const struct search_node *left, *right;
	unsigned int len;

	switch (node->type) {
	case SEARCH_OR:
		i_assert(node->first_child != NULL);
		left = node->first_child;
		right = left->next_sibling;

		while (right != NULL) {
			str_append(cmd, "OR ");
			search_command_append(cmd, left);
			str_append_c(cmd, ' ');
			left = right;
			right = right->next_sibling;
		}
		search_command_append(cmd, left);
		break;
	case SEARCH_SUB:
		i_assert(node->first_child != NULL);
		str_append_c(cmd, '(');
		node = node->first_child;
		while (node != NULL) {
			search_command_append(cmd, node);
			str_append_c(cmd, ' ');
			node = node->next_sibling;
		}
		str_truncate(cmd, str_len(cmd)-1);
		str_append_c(cmd, ')');
		break;
	case SEARCH_SEQSET: {
		const struct seq_range *range;
		unsigned int i, count;

		range = array_get(&node->seqset, &count);
		for (i = 0; i < count; i++) {
			if (i > 0)
				str_append_c(cmd, ',');
			if (range[i].seq1 == range[i].seq2)
				str_printfa(cmd, "%u", range[i].seq1);
			else {
				str_printfa(cmd, "%u:%u", range[i].seq1,
					    range[i].seq2);
			}
		}
		break;
	}
	case SEARCH_SMALLER:
		str_printfa(cmd, "SMALLER %"PRIuUOFF_T, node->size);
		break;
	case SEARCH_LARGER:
		str_printfa(cmd, "LARGER %"PRIuUOFF_T, node->size);
		break;
	case SEARCH_BEFORE:
	case SEARCH_ON:
	case SEARCH_SINCE:
	case SEARCH_SENTBEFORE:
	case SEARCH_SENTON:
	case SEARCH_SENTSINCE:
		switch (node->type) {
		case SEARCH_BEFORE:
			str_append(cmd, "BEFORE");
			break;
		case SEARCH_ON:
			str_append(cmd, "ON");
			break;
		case SEARCH_SINCE:
			str_append(cmd, "SINCE");
			break;
		case SEARCH_SENTBEFORE:
			str_append(cmd, "SENTBEFORE");
			break;
		case SEARCH_SENTON:
			str_append(cmd, "SENTON");
			break;
		case SEARCH_SENTSINCE:
			str_append(cmd, "SENTSINCE");
			break;
		default:
			i_unreached();
		}
		str_append_c(cmd, ' ');
		len = str_len(cmd);
		str_append(cmd, imap_to_datetime(node->date));
		/* truncate to contain only date */
		str_truncate(cmd, len + 11);
		break;
	case SEARCH_SUBJECT:
		if (!str_need_escaping(node->str))
			str_printfa(cmd, "SUBJECT \"%s\"", node->str);
		else {
			str_printfa(cmd, "SUBJECT {%u+}\r\n%s",
				    (unsigned int)strlen(node->str), node->str);
		}
		break;
	case SEARCH_TYPE_COUNT:
		i_unreached();
	}
}

void search_command_send(struct client *client)
{
	pool_t pool;
	string_t *cmd;

	i_assert(client->search_ctx == NULL);

	pool = pool_alloconly_create("search context", 16384);
	client->search_ctx = p_new(pool, struct search_context, 1);
	client->search_ctx->pool = pool;
	client->search_ctx->client = client;
	client->search_ctx->root.type = SEARCH_SUB;
	client->search_ctx->root.first_child =
		search_command_build(client->search_ctx,
				     &client->search_ctx->root, 100);

	cmd = t_str_new(256);
	str_append(cmd, "SEARCH ");
	search_command_append(cmd, &client->search_ctx->root);

	/* remove () around the search query */
	str_delete(cmd, 7, 1);
	str_truncate(cmd, str_len(cmd)-1);

	command_send(client, str_c(cmd), search_callback);
}

void search_result(struct client *client, const struct imap_arg *args)
{
	const char *str;
	unsigned long num;
	unsigned int msgs_count;

	if (client->search_ctx == NULL) {
		client_input_error(client, "unexpected SEARCH reply");
		return;
	}

	if (array_is_created(&client->search_ctx->result)) {
		client_input_error(client, "duplicate SEARCH reply");
		return;
	}

	msgs_count = array_count(&client->view->uidmap);
	p_array_init(&client->search_ctx->result, client->search_ctx->pool, 64);
	for (; args->type != IMAP_ARG_EOL; args++) {
		if (args->type != IMAP_ARG_ATOM) {
			client_input_error(client,
					   "SEARCH reply contains non-atoms");
			return;
		}
		str = IMAP_ARG_STR(args);
		num = strtoul(str, NULL, 10);
		if (!is_numeric(str, '\0') || num == 0 || num > (uint32_t)-1) {
			client_input_error(client,
				"SEARCH reply contains invalid numbers");
			return;
		}
		if (num > msgs_count) {
			client_input_error(client,
					   "SEARCH reply seq %lu > %u EXISTS",
					   num, msgs_count);
			break;
		}
		seq_range_array_add(&client->search_ctx->result, 0, num);
	}
}