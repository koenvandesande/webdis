#include "sha1/sha1.h"
#include <b64/cencode.h>
#include "websocket.h"
#include "client.h"
#include "cmd.h"
#include "worker.h"
#include "pool.h"
#include "http.h"
#include "slog.h"

/* message parsers */
#include "formats/json.h"
#include "formats/raw.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/param.h>

static void
ws_schedule_write(struct http_client *c);

/**
 * This code uses the WebSocket specification from RFC 6455.
 * A copy is available at http://www.rfc-editor.org/rfc/rfc6455.txt
 */

/* custom 64-bit encoding functions to avoid portability issues */
#define webdis_ntohl64(p) \
	((((uint64_t)((p)[0])) <<  0) + (((uint64_t)((p)[1])) <<  8) +\
	 (((uint64_t)((p)[2])) << 16) + (((uint64_t)((p)[3])) << 24) +\
	 (((uint64_t)((p)[4])) << 32) + (((uint64_t)((p)[5])) << 40) +\
	 (((uint64_t)((p)[6])) << 48) + (((uint64_t)((p)[7])) << 56))

#define webdis_htonl64(p) {\
	(char)(((p & ((uint64_t)0xff <<  0)) >>  0) & 0xff), (char)(((p & ((uint64_t)0xff <<  8)) >>  8) & 0xff), \
	(char)(((p & ((uint64_t)0xff << 16)) >> 16) & 0xff), (char)(((p & ((uint64_t)0xff << 24)) >> 24) & 0xff), \
	(char)(((p & ((uint64_t)0xff << 32)) >> 32) & 0xff), (char)(((p & ((uint64_t)0xff << 40)) >> 40) & 0xff), \
	(char)(((p & ((uint64_t)0xff << 48)) >> 48) & 0xff), (char)(((p & ((uint64_t)0xff << 56)) >> 56) & 0xff) }
static int
ws_compute_handshake(struct http_client *c, char *out, size_t *out_sz) {

	unsigned char *buffer, sha1_output[20];
	char magic[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
	SHA1Context ctx;
	base64_encodestate b64_ctx;
	int pos, i;

	// websocket handshake
	const char *key = client_get_header(c, "Sec-WebSocket-Key");
	size_t key_sz = key?strlen(key):0, buffer_sz = key_sz + sizeof(magic) - 1;
	if(!key || key_sz < 16 || key_sz > 32) { /* supposed to be exactly 16 bytes that were b64 encoded */
		slog(c->s, WEBDIS_WARNING, "Invalid Sec-WebSocket-Key", 0);
		return -1;
	}
	buffer = calloc(buffer_sz, 1);
	if(!buffer) {
		slog(c->s, WEBDIS_ERROR, "Failed to allocate memory for WS header", 0);
		return -1;
	}

	// concatenate key and guid in buffer
	memcpy(buffer, key, key_sz);
	memcpy(buffer+key_sz, magic, sizeof(magic)-1);

	// compute sha-1
	SHA1Reset(&ctx);
	SHA1Input(&ctx, buffer, buffer_sz);
	SHA1Result(&ctx);
	for(i = 0; i < (int)(20/sizeof(int)); ++i) {	// put in correct byte order before memcpy.
		ctx.Message_Digest[i] = ntohl(ctx.Message_Digest[i]);
	}
	memcpy(sha1_output, ctx.Message_Digest, 20);

	// encode `sha1_output' in base 64, into `out'.
	base64_init_encodestate(&b64_ctx);
	pos = base64_encode_block((const char*)sha1_output, 20, out, &b64_ctx);
	base64_encode_blockend(out + pos, &b64_ctx);

	// compute length, without \n
	*out_sz = strlen(out);
	if(out[*out_sz-1] == '\n')
		(*out_sz)--;

	free(buffer);

	return 0;
}

int
ws_handshake_reply(struct http_client *c) {

	char sha1_handshake[40];
	char *buffer = NULL, *p;
	const char *origin = NULL, *host = NULL;
	size_t origin_sz = 0, host_sz = 0, handshake_sz = 0, sz;

	char template_start[] = "HTTP/1.1 101 Switching Protocols\r\n"
		"Upgrade: websocket\r\n"
		"Connection: Upgrade";
	char template_accept[] = "\r\n" /* just after the start */
		"Sec-WebSocket-Accept: "; /* %s */
	char template_sec_origin[] = "\r\n"
		"Sec-WebSocket-Origin: "; /* %s (optional header) */
	char template_loc[] = "\r\n"
		"Sec-WebSocket-Location: ws://"; /* %s%s */
	char template_end[] = "\r\n\r\n";

	if((origin = client_get_header(c, "Origin"))) {
		origin_sz = strlen(origin);
	} else if((origin = client_get_header(c, "Sec-WebSocket-Origin"))) {
		origin_sz = strlen(origin);
	}
	if((host = client_get_header(c, "Host"))) {
		host_sz = strlen(host);
	}

	/* need those headers */
	if(!host || !host_sz || !c->path || !c->path_sz) {
		slog(c->s, WEBDIS_WARNING, "Missing headers for WS handshake", 0);
		return -1;
	}

	memset(sha1_handshake, 0, sizeof(sha1_handshake));
	if(ws_compute_handshake(c, &sha1_handshake[0], &handshake_sz) != 0) {
		/* failed to compute handshake. */
		slog(c->s, WEBDIS_WARNING, "Failed to compute handshake", 0);
		return -1;
	}

	sz = sizeof(template_start)-1
		+ sizeof(template_accept)-1 + handshake_sz
		+ (origin && origin_sz ? (sizeof(template_sec_origin)-1 + origin_sz) : 0) /* optional origin */
		+ sizeof(template_loc)-1 + host_sz + c->path_sz
		+ sizeof(template_end)-1;

	p = buffer = malloc(sz);
	if(!p) {
		slog(c->s, WEBDIS_ERROR, "Failed to allocate buffer for WS handshake", 0);
		return -1;
	}

	/* Concat all */

	/* template_start */
	memcpy(p, template_start, sizeof(template_start)-1);
	p += sizeof(template_start)-1;

	/* template_accept */
	memcpy(p, template_accept, sizeof(template_accept)-1);
	p += sizeof(template_accept)-1;
	memcpy(p, &sha1_handshake[0], handshake_sz);
	p += handshake_sz;

	/* template_sec_origin */
	if(origin && origin_sz) {
		memcpy(p, template_sec_origin, sizeof(template_sec_origin)-1);
		p += sizeof(template_sec_origin)-1;
		memcpy(p, origin, origin_sz);
		p += origin_sz;
	}

	/* template_loc */
	memcpy(p, template_loc, sizeof(template_loc)-1);
	p += sizeof(template_loc)-1;
	memcpy(p, host, host_sz);
	p += host_sz;
	memcpy(p, c->path, c->path_sz);
	p += c->path_sz;

	/* template_end */
	memcpy(p, template_end, sizeof(template_end)-1);
	p += sizeof(template_end)-1;

	/* create buffer that will hold data to send out */
	c->ws_wbuf = evbuffer_new();
	if(!c->ws_wbuf) {
		slog(c->s, WEBDIS_ERROR, "Failed to allocate response for WS handshake", 0);
		free(buffer);
		return -1;
	}

	int add_ret = evbuffer_add(c->ws_wbuf, buffer, sz);
	if(add_ret < 0) {
		slog(c->s, WEBDIS_ERROR, "Failed to add response for WS handshake", 0);
		free(buffer);
		return -1;
	}

	ws_schedule_write(c); /* will free buffer and response once sent */

	return 0;
}


static int
ws_execute(struct http_client *c, const char *frame, size_t frame_len) {

	struct cmd*(*fun_extract)(struct http_client *, const char *, size_t) = NULL;
	formatting_fun fun_reply = NULL;

	if((c->path_sz == 1 && strncmp(c->path, "/", 1) == 0) ||
	   strncmp(c->path, "/.json", 6) == 0) {
		fun_extract = json_ws_extract;
		fun_reply = json_reply;
	} else if(strncmp(c->path, "/.raw", 5) == 0) {
		fun_extract = raw_ws_extract;
		fun_reply = raw_reply;
	}

	if(fun_extract) {

		/* Parse websocket frame into a cmd object. */
		struct cmd *cmd = fun_extract(c, frame, frame_len);

		if(cmd) {
			/* copy client info into cmd. */
			cmd_setup(cmd, c);
			cmd->is_websocket = 1;

			if (c->self_cmd != NULL) {
				/* This client already has its own connection
				 * to Redis from a previous command; use it from
				 * now on. */
				cmd->ac = c->self_cmd->ac;
			} else {
				/* First WS command; make new Redis context
				 * for this client */
				cmd->ac = pool_connect(c->w->pool, cmd->database, 0);
				c->self_cmd = cmd;
				cmd->pub_sub_client = c;
			}

			/* send it off */
			cmd_send(cmd, fun_reply);

			return 0;
		}
	}

	return -1;
}

static struct ws_msg *
ws_msg_new() {
	return calloc(1, sizeof(struct ws_msg));
}

static void
ws_msg_add(struct ws_msg *m, const char *p, size_t psz, const unsigned char *mask) {

	/* add data to frame */
	size_t i;
	m->payload = realloc(m->payload, m->payload_sz + psz);
	memcpy(m->payload + m->payload_sz, p, psz);

	/* apply mask */
	for(i = 0; i < psz && mask; ++i) {
		m->payload[m->payload_sz + i] = (unsigned char)p[i] ^ mask[i%4];
	}

	/* save new size */
	m->payload_sz += psz;
}

static void
ws_msg_free(struct ws_msg **m) {

	free((*m)->payload);
	free(*m);
	*m = NULL;
}

static enum ws_state
ws_parse_data(const char *frame, size_t sz, struct ws_msg **msg) {

	int has_mask;
	uint64_t len;
	const char *p;
	unsigned char mask[4];

	/* parse frame and extract contents */
	if(sz < 8) {
		return WS_READING;
	}

	has_mask = frame[1] & 0x80 ? 1:0;

	/* get payload length */
	len = frame[1] & 0x7f;	/* remove leftmost bit */
	if(len <= 125) { /* data starts right after the mask */
		p = frame + 2 + (has_mask ? 4 : 0);
		if(has_mask) memcpy(&mask, frame + 2, sizeof(mask));
	} else if(len == 126) {
		uint16_t sz16;
		memcpy(&sz16, frame + 2, sizeof(uint16_t));
		len = ntohs(sz16);
		p = frame + 4 + (has_mask ? 4 : 0);
		if(has_mask) memcpy(&mask, frame + 4, sizeof(mask));
	} else if(len == 127) {
		len = webdis_ntohl64(frame+2);
		p = frame + 10 + (has_mask ? 4 : 0);
		if(has_mask) memcpy(&mask, frame + 10, sizeof(mask));
	} else {
		return WS_ERROR;
	}

	/* we now have the (possibly masked) data starting in p, and its length.  */
	if(len > sz - (p - frame)) { /* not enough data */
		return WS_READING;
	}

	if(!*msg)
		*msg = ws_msg_new();
	ws_msg_add(*msg, p, len, has_mask ? mask : NULL);
	(*msg)->total_sz += len + (p - frame);

	if(frame[0] & 0x80) { /* FIN bit set */
		return WS_MSG_COMPLETE;
	} else {
		return WS_READING;	/* need more data */
	}
}


/**
 * Process some data just received on the socket.
 */
enum ws_state
ws_add_data(struct http_client *c) {

	enum ws_state state;

	state = ws_parse_data(c->buffer, c->sz, &c->frame);

	while(state == WS_MSG_COMPLETE) {
		int ret = ws_execute(c, c->frame->payload, c->frame->payload_sz);

		/* remove frame from client buffer */
		http_client_remove_data(c, c->frame->total_sz);

		/* free frame and set back to NULL */
		ws_msg_free(&c->frame);

		if(ret != 0) {
			/* can't process frame. */
			slog(c->s, WEBDIS_DEBUG, "ws_add_data: ws_execute failed", 0);
			return WS_ERROR;
		}
		state = ws_parse_data(c->buffer, c->sz, &c->frame);
	}
	return state;
}

int
ws_frame_and_send_response(struct cmd *cmd, const char *p, size_t sz) {

	char *frame = malloc(sz + 8); /* create frame by prepending header */
	size_t frame_sz = 0;
	if (frame == NULL)
		return -1;

	/*
      The length of the "Payload data", in bytes: if 0-125, that is the
      payload length.  If 126, the following 2 bytes interpreted as a
      16-bit unsigned integer are the payload length.  If 127, the
      following 8 bytes interpreted as a 64-bit unsigned integer (the
      most significant bit MUST be 0) are the payload length.
	  */
	frame[0] = '\x81';
	if(sz <= 125) {
		frame[1] = sz;
		memcpy(frame + 2, p, sz);
		frame_sz = sz + 2;
	} else if (sz <= 65536) {
		uint16_t sz16 = htons(sz);
		frame[1] = 126;
		memcpy(frame + 2, &sz16, 2);
		memcpy(frame + 4, p, sz);
		frame_sz = sz + 4;
	} else { /* sz > 65536 */
		char sz64[8] = webdis_htonl64(sz);
		frame[1] = 127;
		memcpy(frame + 2, sz64, 8);
		memcpy(frame + 10, p, sz);
		frame_sz = sz + 10;
	}

	/* mark as keep alive, otherwise we'll close the connection after the first reply */
	int add_ret = evbuffer_add(cmd->http_client->ws_wbuf, frame, frame_sz);
	if (add_ret < 0) {
		free(frame);
		slog(cmd->w->s, WEBDIS_ERROR, "Failed response allocation in ws_frame_and_send_response", 0);
		return -1;
	}

	/* send WS frame */
	ws_schedule_write(cmd->http_client);

	return 0;
}

static void
ws_can_write(int fd, short event, void *p) {

	int ret;
	struct http_client *c = p;
	(void)event;

	c->ws_scheduled_write = 0;

	/* send pending data */
	ret = evbuffer_write(c->ws_wbuf, fd);

	if(ret < 0) {
		close(fd);
	} else if(ret > 0 && evbuffer_get_length(c->ws_wbuf) > 0) { /* more data to send */
		ws_schedule_write(c);
	}
}

static void
ws_schedule_write(struct http_client *c) {

	if(c->ws_scheduled_write) {
		return;
	}
	event_set(&c->ws_wev, c->fd, EV_WRITE, ws_can_write, c);
	event_base_set(c->w->base, &c->ws_wev);
	int ret = event_add(&c->ws_wev, NULL);
	if(ret == 0) {
		c->ws_scheduled_write = 1;
	} else { /* could not schedule write */
		slog(c->w->s, WEBDIS_ERROR, "Could not schedule WS write", 0);
	}
}
