/* SPDX-License-Identifier: MIT */
/* sip-cnf: a small SIP network function built for Kubernetes.
 *
 * One thread, one epoll loop:
 *   - UDP SIP on SIP_PORT (stateless responses, see sip_resp.c)
 *   - HTTP on HTTP_PORT: /healthz (liveness), /readyz (readiness), /metrics
 *   - signalfd for SIGTERM/SIGINT
 *
 * Shutdown follows the Kubernetes pod lifecycle: on SIGTERM the pod reports
 * not-ready at once, so the Service stops sending it new traffic, but it
 * keeps answering SIP for DRAIN_SECONDS while endpoints propagate, then
 * exits 0. A second signal exits immediately. */
#define _GNU_SOURCE
#include "sip_resp.h"

#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

enum { M_OPTIONS, M_REGISTER, M_OTHER, M_N };
static const char *mname[M_N] = { "OPTIONS", "REGISTER", "other" };

static struct {
	unsigned long req[M_N], resp_200, resp_501, malformed, http;
	int draining;
	double started;
} st;

static double now_s(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

static int env_int(const char *k, int def)
{
	const char *v = getenv(k);
	return v && *v ? atoi(v) : def;
}

static void logj(const char *event, const char *detail)
{
	/* one JSON object per line: what log collectors in a cluster expect */
	fprintf(stderr, "{\"ts\":%.3f,\"event\":\"%s\",\"detail\":\"%s\"}\n",
		now_s() - st.started, event, detail);
}

static int listen_on(int type, int port)
{
	int fd = socket(AF_INET, type | SOCK_NONBLOCK | SOCK_CLOEXEC, 0), one = 1;
	struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port),
				 .sin_addr.s_addr = htonl(INADDR_ANY) };
	if (fd < 0)
		return -1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0 ||
	    (type == SOCK_STREAM && listen(fd, 64) < 0)) {
		close(fd);
		return -1;
	}
	return fd;
}

static void handle_sip(int fd, const char *tag)
{
	char in[4096], out[4096];
	struct sockaddr_storage from;
	for (;;) {
		socklen_t fl = sizeof(from);
		ssize_t n = recvfrom(fd, in, sizeof(in), 0, (struct sockaddr *)&from, &fl);
		if (n <= 0)
			return;
		int code = 0;
		size_t len = sip_respond(in, (size_t)n, tag, out, sizeof(out), &code);
		if (!len) {
			st.malformed++;
			continue;
		}
		int m = strncmp(in, "OPTIONS ", 8) == 0 ? M_OPTIONS :
			strncmp(in, "REGISTER ", 9) == 0 ? M_REGISTER : M_OTHER;
		st.req[m]++;
		if (code == 200)
			st.resp_200++;
		else
			st.resp_501++;
		sendto(fd, out, len, 0, (struct sockaddr *)&from, fl);
	}
}

static void handle_http(int cfd)
{
	char req[1024], body[2048], resp[2560];
	ssize_t n = recv(cfd, req, sizeof(req) - 1, 0);
	if (n <= 0)
		return;
	req[n] = 0;
	st.http++;

	int code = 404, blen = 0;
	if (strncmp(req, "GET /healthz ", 13) == 0) {
		code = 200;
		blen = snprintf(body, sizeof(body), "ok\n");
	} else if (strncmp(req, "GET /readyz ", 12) == 0) {
		code = st.draining ? 503 : 200;
		blen = snprintf(body, sizeof(body), st.draining ? "draining\n" : "ready\n");
	} else if (strncmp(req, "GET /metrics ", 13) == 0) {
		code = 200;
		for (int i = 0; i < M_N; i++)
			blen += snprintf(body + blen, sizeof(body) - (size_t)blen,
					 "sip_requests_total{method=\"%s\"} %lu\n", mname[i], st.req[i]);
		blen += snprintf(body + blen, sizeof(body) - (size_t)blen,
				 "sip_responses_total{code=\"200\"} %lu\n"
				 "sip_responses_total{code=\"501\"} %lu\n"
				 "sip_malformed_total %lu\n"
				 "http_requests_total %lu\n"
				 "draining %d\n"
				 "uptime_seconds %.1f\n",
				 st.resp_200, st.resp_501, st.malformed, st.http,
				 st.draining, now_s() - st.started);
	} else {
		blen = snprintf(body, sizeof(body), "not found\n");
	}
	int rl = snprintf(resp, sizeof(resp),
			  "HTTP/1.1 %d %s\r\nContent-Type: text/plain\r\n"
			  "Content-Length: %d\r\nConnection: close\r\n\r\n%s",
			  code, code == 200 ? "OK" : code == 503 ? "Service Unavailable" : "Not Found",
			  blen, body);
	if (rl > 0)
		send(cfd, resp, (size_t)rl, MSG_NOSIGNAL);
}

int main(void)
{
	int sip_port = env_int("SIP_PORT", 5060), http_port = env_int("HTTP_PORT", 8080);
	int drain = env_int("DRAIN_SECONDS", 10);
	const char *pod = getenv("POD_NAME");
	char tag[64], msg[128];
	snprintf(tag, sizeof(tag), "%s", pod && *pod ? pod : "sipcnf");
	st.started = now_s();

	sigset_t mask;
	sigemptyset(&mask);
	sigaddset(&mask, SIGTERM);
	sigaddset(&mask, SIGINT);
	sigprocmask(SIG_BLOCK, &mask, NULL);
	int sfd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);

	int ufd = listen_on(SOCK_DGRAM, sip_port), hfd = listen_on(SOCK_STREAM, http_port);
	int ep = epoll_create1(EPOLL_CLOEXEC);
	if (sfd < 0 || ufd < 0 || hfd < 0 || ep < 0) {
		logj("fatal", strerror(errno));
		return 1;
	}
	struct epoll_event ev = { .events = EPOLLIN };
	ev.data.fd = sfd; epoll_ctl(ep, EPOLL_CTL_ADD, sfd, &ev);
	ev.data.fd = ufd; epoll_ctl(ep, EPOLL_CTL_ADD, ufd, &ev);
	ev.data.fd = hfd; epoll_ctl(ep, EPOLL_CTL_ADD, hfd, &ev);

	snprintf(msg, sizeof(msg), "sip udp/%d http/%d drain %ds", sip_port, http_port, drain);
	logj("started", msg);

	double deadline = 0;
	for (;;) {
		int timeout = -1;
		if (st.draining) {
			double left = deadline - now_s();
			if (left <= 0)
				break;
			timeout = (int)(left * 1000) + 1;
		}
		struct epoll_event evs[16];
		int n = epoll_wait(ep, evs, 16, timeout);
		if (n < 0 && errno != EINTR)
			break;
		for (int i = 0; i < n; i++) {
			int fd = evs[i].data.fd;
			if (fd == ufd) {
				handle_sip(ufd, tag);
			} else if (fd == hfd) {
				int c;
				while ((c = accept4(hfd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC)) >= 0) {
					struct epoll_event ce = { .events = EPOLLIN | EPOLLRDHUP,
								  .data.fd = c };
					epoll_ctl(ep, EPOLL_CTL_ADD, c, &ce);
				}
			} else if (fd == sfd) {
				struct signalfd_siginfo si;
				while (read(sfd, &si, sizeof(si)) == sizeof(si)) {
					if (st.draining) {
						logj("exit", "second signal");
						return 0;
					}
					st.draining = 1;
					deadline = now_s() + drain;
					logj("draining", si.ssi_signo == SIGTERM ? "SIGTERM" : "SIGINT");
				}
			} else { /* HTTP client */
				handle_http(fd);
				epoll_ctl(ep, EPOLL_CTL_DEL, fd, NULL);
				close(fd);
			}
		}
	}
	snprintf(msg, sizeof(msg), "served %lu SIP requests", st.req[0] + st.req[1] + st.req[2]);
	logj("stopped", msg);
	return 0;
}
