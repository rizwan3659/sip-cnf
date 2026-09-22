/* SPDX-License-Identifier: MIT */
/* Starts ./sip-cnf and walks it through the Kubernetes pod lifecycle:
 * ready -> SIGTERM -> not ready but still serving SIP -> clean exit. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", \
	__FILE__, __LINE__, #c); fails++; } } while (0)

static int SIP = 25060, HTTP = 28080;

static void msleep(int ms)
{
	struct timespec t = { ms / 1000, (ms % 1000) * 1000000L };
	nanosleep(&t, NULL);
}

static int http_get(const char *path, char *body, size_t cap)
{
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)HTTP),
				 .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
	if (connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
		close(fd);
		return -1;
	}
	char req[128];
	int n = snprintf(req, sizeof(req), "GET %s HTTP/1.1\r\nHost: x\r\n\r\n", path);
	send(fd, req, (size_t)n, 0);
	size_t got = 0;
	ssize_t r;
	while (got < cap - 1 && (r = recv(fd, body + got, cap - 1 - got, 0)) > 0)
		got += (size_t)r;
	body[got] = 0;
	close(fd);
	int code = -1;
	sscanf(body, "HTTP/1.1 %d", &code);
	return code;
}

static int sip_options(void)
{
	int fd = socket(AF_INET, SOCK_DGRAM, 0);
	struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)SIP),
				 .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
	const char m[] = "OPTIONS sip:cnf SIP/2.0\r\nVia: SIP/2.0/UDP t;branch=z9hG4bK1\r\n"
			 "From: <sip:t>;tag=1\r\nTo: <sip:cnf>\r\nCall-ID: lc\r\nCSeq: 1 OPTIONS\r\n\r\n";
	sendto(fd, m, sizeof(m) - 1, 0, (struct sockaddr *)&a, sizeof(a));
	struct pollfd p = { .fd = fd, .events = POLLIN };
	char buf[1024];
	int code = -1;
	if (poll(&p, 1, 1000) == 1) {
		ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
		if (n > 0) {
			buf[n] = 0;
			sscanf(buf, "SIP/2.0 %d", &code);
		}
	}
	close(fd);
	return code;
}

int main(void)
{
	char body[4096], env[3][32];
	int base = 20000 + (int)(getpid() % 5000);
	SIP = base;
	HTTP = base + 1;
	snprintf(env[0], 32, "SIP_PORT=%d", SIP);
	snprintf(env[1], 32, "HTTP_PORT=%d", HTTP);
	snprintf(env[2], 32, "DRAIN_SECONDS=2");

	pid_t pid = fork();
	if (pid == 0) {
		char *envp[] = { env[0], env[1], env[2], "POD_NAME=test-pod", NULL };
		char *argv[] = { "./sip-cnf", NULL };
		execve("./sip-cnf", argv, envp);
		_exit(127);
	}
	for (int i = 0; i < 50 && http_get("/healthz", body, sizeof(body)) != 200; i++)
		msleep(50);

	CHECK(http_get("/healthz", body, sizeof(body)) == 200);
	CHECK(http_get("/readyz", body, sizeof(body)) == 200);
	CHECK(sip_options() == 200);
	CHECK(http_get("/nope", body, sizeof(body)) == 404);

	kill(pid, SIGTERM);
	msleep(200);
	CHECK(http_get("/readyz", body, sizeof(body)) == 503);  /* out of the Service */
	CHECK(http_get("/healthz", body, sizeof(body)) == 200); /* but not restarted */
	CHECK(sip_options() == 200);                             /* still serving */
	CHECK(http_get("/metrics", body, sizeof(body)) == 200);
	CHECK(strstr(body, "sip_requests_total{method=\"OPTIONS\"} 2") != NULL);
	CHECK(strstr(body, "draining 1") != NULL);

	struct timespec t0, t1;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	int status = 0;
	waitpid(pid, &status, 0);
	clock_gettime(CLOCK_MONOTONIC, &t1);
	double waited = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
	CHECK(waited > 1.0 && waited < 3.0); /* ~DRAIN_SECONDS after SIGTERM */

	if (fails) {
		fprintf(stderr, "%d check(s) failed\n", fails);
		return 1;
	}
	printf("lifecycle test passed (drained and exited %.2f s after SIGTERM + 0.2 s)\n", waited);
	return 0;
}
