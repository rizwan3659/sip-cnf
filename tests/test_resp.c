/* SPDX-License-Identifier: MIT */
#include "../src/sip_resp.h"

#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", \
	__FILE__, __LINE__, #c); fails++; } } while (0)

static const char OPTIONS[] =
	"OPTIONS sip:cnf@ims.example SIP/2.0\r\n"
	"Via: SIP/2.0/UDP 10.0.0.5:5060;branch=z9hG4bKa1\r\n"
	"Via: SIP/2.0/UDP 10.0.0.9:5060;branch=z9hG4bKb2\r\n"
	"From: <sip:pcscf@ims.example>;tag=77\r\n"
	"To: <sip:cnf@ims.example>\r\n"
	"Call-ID: opt-1\r\n"
	"CSeq: 12 OPTIONS\r\n"
	"Max-Forwards: 70\r\n\r\n";

int main(void)
{
	char out[2048];
	int code = 0;

	size_t n = sip_respond(OPTIONS, sizeof(OPTIONS) - 1, "pod-a", out, sizeof(out), &code);
	CHECK(n > 0 && code == 200);
	CHECK(strncmp(out, "SIP/2.0 200 OK\r\n", 16) == 0);
	/* both Vias, in order */
	char *v1 = strstr(out, "branch=z9hG4bKa1"), *v2 = strstr(out, "branch=z9hG4bKb2");
	CHECK(v1 && v2 && v1 < v2);
	CHECK(strstr(out, "To: <sip:cnf@ims.example>;tag=pod-a\r\n") != NULL);
	CHECK(strstr(out, "CSeq: 12 OPTIONS\r\n") && strstr(out, "Allow: OPTIONS, REGISTER\r\n"));
	CHECK(strcmp(out + n - 4, "\r\n\r\n") == 0);

	const char reg[] = "REGISTER sip:ims.example SIP/2.0\r\nv: SIP/2.0/UDP ue;branch=z9hG4bKc\r\n"
			   "f: <sip:ue@ims.example>;tag=1\r\nt: <sip:ue@ims.example>;tag=x\r\n"
			   "i: r1\r\nCSeq: 1 REGISTER\r\nm: <sip:ue@10.1.1.1>\r\nExpires: 600\r\n\r\n";
	n = sip_respond(reg, sizeof(reg) - 1, "pod-a", out, sizeof(out), &code);
	CHECK(n > 0 && code == 200);                                   /* compact forms */
	CHECK(strstr(out, "To: <sip:ue@ims.example>;tag=x\r\n") != NULL); /* keeps existing tag */
	CHECK(strstr(out, "Contact: <sip:ue@10.1.1.1>\r\nExpires: 600\r\n") != NULL);

	const char inv[] = "INVITE sip:x SIP/2.0\r\nVia: a\r\nFrom: b\r\nTo: c\r\nCall-ID: d\r\nCSeq: 1 INVITE\r\n\r\n";
	CHECK(sip_respond(inv, sizeof(inv) - 1, "t", out, sizeof(out), &code) > 0 && code == 501);

	/* dropped: responses, missing mandatory headers, bad framing, no room */
	const char resp[] = "SIP/2.0 200 OK\r\nVia: a\r\nFrom: b\r\nTo: c\r\nCall-ID: d\r\nCSeq: 1 X\r\n\r\n";
	CHECK(sip_respond(resp, sizeof(resp) - 1, "t", out, sizeof(out), &code) == 0);
	const char novia[] = "OPTIONS sip:x SIP/2.0\r\nFrom: b\r\nTo: c\r\nCall-ID: d\r\nCSeq: 1 OPTIONS\r\n\r\n";
	CHECK(sip_respond(novia, sizeof(novia) - 1, "t", out, sizeof(out), &code) == 0);
	CHECK(sip_respond(OPTIONS, 40, "t", out, sizeof(out), &code) == 0);
	const char nouri[] = "OPTIONS SIP/2.0\r\nVia: a\r\nFrom: b\r\nTo: c\r\nCall-ID: d\r\nCSeq: 1 OPTIONS\r\n\r\n";
	CHECK(sip_respond(nouri, sizeof(nouri) - 1, "t", out, sizeof(out), &code) == 0);
	CHECK(sip_respond(OPTIONS, sizeof(OPTIONS) - 1, "t", out, 64, &code) == 0);

	if (fails) {
		fprintf(stderr, "%d check(s) failed\n", fails);
		return 1;
	}
	printf("sip_respond tests passed\n");
	return 0;
}
