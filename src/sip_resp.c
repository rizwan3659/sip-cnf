/* SPDX-License-Identifier: MIT */
#include "sip_resp.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#define MAX_VIA 8

struct hv { const char *p; int n; };

static struct hv trim(const char *p, const char *e)
{
	while (p < e && (*p == ' ' || *p == '\t'))
		p++;
	while (e > p && (e[-1] == ' ' || e[-1] == '\t'))
		e--;
	return (struct hv){ p, (int)(e - p) };
}

static int name_is(struct hv n, const char *full, const char *compact)
{
	return (n.n == (int)strlen(full) && strncasecmp(n.p, full, (size_t)n.n) == 0) ||
	       (compact && n.n == 1 && strncasecmp(n.p, compact, 1) == 0);
}

size_t sip_respond(const char *req, size_t len, const char *to_tag,
		   char *out, size_t cap, int *status)
{
	const char *p = req, *end = req + len;
	struct hv method = { 0 }, from = { 0 }, to = { 0 }, callid = { 0 }, cseq = { 0 },
		  contact = { 0 }, expires = { 0 }, via[MAX_VIA];
	int nvia = 0;

	/* request line */
	const char *eol = memchr(p, '\n', len);
	if (!eol || eol == p || eol[-1] != '\r')
		return 0;
	const char *sp = memchr(p, ' ', (size_t)(eol - p));
	if (!sp || sp == p || (eol - p > 7 && strncmp(p, "SIP/2.0", 7) == 0))
		return 0;                               /* a response, not a request */
	/* "METHOD SP Request-URI SP SIP/2.0" */
	if (eol - 9 <= sp || eol[-9] != ' ' || strncmp(eol - 8, "SIP/2.0", 7) != 0)
		return 0;
	method = (struct hv){ p, (int)(sp - p) };
	p = eol + 1;

	for (;;) {
		eol = memchr(p, '\n', (size_t)(end - p));
		if (!eol || eol[-1] != '\r')
			return 0;
		if (eol == p + 1)
			break;
		const char *c = memchr(p, ':', (size_t)(eol - p));
		if (!c)
			return 0;
		struct hv n = trim(p, c), v = trim(c + 1, eol - 1);
		if (name_is(n, "Via", "v")) {
			if (nvia == MAX_VIA)
				return 0;
			via[nvia++] = v;
		} else if (name_is(n, "From", "f")) from = v;
		else if (name_is(n, "To", "t")) to = v;
		else if (name_is(n, "Call-ID", "i")) callid = v;
		else if (name_is(n, "CSeq", NULL)) cseq = v;
		else if (name_is(n, "Contact", "m")) contact = v;
		else if (name_is(n, "Expires", NULL)) expires = v;
		p = eol + 1;
	}
	if (!nvia || !from.n || !to.n || !callid.n || !cseq.n)
		return 0; /* RFC 3261 s.8.1.1 mandatory headers */

	int st = 501;
	const char *reason = "Not Implemented";
	if (method.n == 7 && memcmp(method.p, "OPTIONS", 7) == 0) {
		st = 200;
		reason = "OK";
	} else if (method.n == 8 && memcmp(method.p, "REGISTER", 8) == 0) {
		st = 200;
		reason = "OK";
	}

	int n = snprintf(out, cap, "SIP/2.0 %d %s\r\n", st, reason);
	for (int i = 0; i < nvia && n > 0 && (size_t)n < cap; i++)
		n += snprintf(out + n, cap - (size_t)n, "Via: %.*s\r\n", via[i].n, via[i].p);
	int has_tag = 0;
	for (int i = 0; i + 4 <= to.n; i++)
		if (strncasecmp(to.p + i, ";tag", 4) == 0)
			has_tag = 1;
	if (n > 0 && (size_t)n < cap)
		n += snprintf(out + n, cap - (size_t)n,
			      "From: %.*s\r\nTo: %.*s%s%s\r\nCall-ID: %.*s\r\nCSeq: %.*s\r\n",
			      from.n, from.p, to.n, to.p, has_tag ? "" : ";tag=",
			      has_tag ? "" : to_tag, callid.n, callid.p, cseq.n, cseq.p);
	if (st == 200 && method.n == 7 && n > 0 && (size_t)n < cap)
		n += snprintf(out + n, cap - (size_t)n, "Allow: OPTIONS, REGISTER\r\n");
	if (st == 200 && method.n == 8 && contact.n && n > 0 && (size_t)n < cap)
		n += snprintf(out + n, cap - (size_t)n, "Contact: %.*s\r\nExpires: %.*s\r\n",
			      contact.n, contact.p, expires.n ? expires.n : 4,
			      expires.n ? expires.p : "3600");
	if (n > 0 && (size_t)n < cap)
		n += snprintf(out + n, cap - (size_t)n, "Content-Length: 0\r\n\r\n");
	if (n <= 0 || (size_t)n >= cap)
		return 0;
	*status = st;
	return (size_t)n;
}
