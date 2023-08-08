/*
 * listening - check if a TCP server is listening
 *
 * To the extent possible under law, Leah Neukirchen <leah@vuxu.org>
 * has waived all copyright and related or neighboring rights to this work.
 * http://creativecommons.org/publicdomain/zero/1.0/
 */

#include <sys/socket.h>

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

uint64_t wait_timeout = 0;       // nanoseconds
uint64_t connect_timeout = 200;  // milliseconds

struct timespec now;
struct timespec deadline;
struct timespec delay;

int
scanfix(char *s, uint64_t *result, int scale)
{
	uint64_t r = 0;
	char *d = 0;

	if (!*s)
		return -EINVAL;

	while (*s) {
		if ((unsigned)(*s)-'0' < 10) {
			if (r <= UINT64_MAX/10 && 10*r <= UINT64_MAX-((*s)-'0'))
				r = r*10 + ((*s)-'0');
			else
				return -ERANGE;
		} else if (*s == '.') {
			if (d)
				return -EINVAL;
			d = s + 1;
		} else {
			return -EINVAL;
		}

		s++;
	}

	if (!d)
		d = s;

	int o = scale - (int)(s-d);
	for (; o > 0; o--) {
		if (r > UINT64_MAX/10)
			return -ERANGE;
		r *= 10;
	}
	for (; o < 0; o++) {
		if (r % 10 != 0)
			return -ERANGE;
		r /= 10;
	}

	*result = r;

	return 0;
}

int
syn_scan(const char *host, int port)
{
	printf("test %s:%d\n", host, port);

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof (struct sockaddr_in));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	// XXX use GAI
	inet_aton(host, &addr.sin_addr);

	int sock = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
	struct linger l = {1, 0};
	setsockopt(sock, SOL_SOCKET, SO_LINGER, (void *)&l, sizeof (struct linger));
	int zero = 0;
	setsockopt(sock, IPPROTO_TCP, TCP_QUICKACK, (void *)&zero, sizeof zero);
	int r;
	errno = 0;
	r = connect(sock, (struct sockaddr *)&addr, sizeof (struct sockaddr_in));
	if (r > 0 || errno != EINPROGRESS) {
		fprintf(stderr, "connect failed: %m\n");
		close(sock);
		return 99;
	}

	struct pollfd fds[1];
	fds[0] = (struct pollfd){ sock, POLLIN | POLLOUT | POLLERR, 0 };
	r = poll(fds, 1, connect_timeout);
	if (r == 0) {
		printf("timeout");
		return 2;
	} else if (r != 1) {
		printf("??? poll = %d %m\n", r);
		close(sock);
		return 99;
	}

	if (fds[0].revents & POLLERR) {
		int error = 0;
		socklen_t errlen = sizeof error;
		getsockopt(sock, SOL_SOCKET, SO_ERROR, (void *)&error, &errlen);
		printf("SO_ERROR = %d %s\n", error, strerror(error));
		close(sock);
		return 1;
	} else if (fds[0].revents & POLLOUT) {
		printf("socket is up\n");
		close(sock);
		return 0;
	}

	return 99;		// unreachable
}

int
main(int argc, char *argv[])
{
	int c, err;

	while ((c = getopt(argc, argv, "+t:w:")) != -1) {
		switch (c) {
                case 't':
			if ((err = scanfix(optarg, &connect_timeout, 3)) < 0) {
				fprintf(stderr, "failed to parse number '%s': %s\n",
				    optarg, strerror(-err));
				goto usage;
			}
			break;
                case 'w':
			if ((err = scanfix(optarg, &wait_timeout, 9)) < 0) {
				fprintf(stderr, "failed to parse number '%s': %s\n",
				    optarg, strerror(-err));
				goto usage;
			}
			break;
                default:
		usage:
                        fprintf(stderr,
                            "Usage: %s [-w WAIT_TIMEOUT] [-t CONNECT_TIMEOUT] [HOST:]PORT\n",
                            argv[0]);
                        exit(99);
                }
	}

	if (optind != argc - 1)
		goto usage;

	int port;
	const char *host = argv[argc-1];
	char *colon = strchr(host, ':');
	if (colon) {
		*colon = 0;
		port = atoi(colon+1);
	} else {
		port = atoi(host);
		host = "127.0.0.1";
	}

	if (wait_timeout == 0)
		return syn_scan(host, port);

	/* else we are waiting for the port to come up: */

	clock_gettime(CLOCK_MONOTONIC, &now);
	deadline.tv_sec = now.tv_sec;
	deadline.tv_nsec = now.tv_nsec + wait_timeout;
	if (deadline.tv_nsec >= 1000000000L) {
		deadline.tv_sec += deadline.tv_nsec / 1000000000L;
		deadline.tv_nsec = deadline.tv_nsec % 1000000000L;
	}
	delay.tv_sec = 0;
	delay.tv_nsec = connect_timeout * 100000L;
	if (delay.tv_nsec >= 1000000000L) {
		delay.tv_sec += delay.tv_nsec / 1000000000L;
		delay.tv_nsec = delay.tv_nsec % 1000000000L;
	}

	while (now.tv_sec < deadline.tv_sec ||
	    (now.tv_sec == deadline.tv_sec && now.tv_nsec <= deadline.tv_nsec)) {
		switch (syn_scan(host, port)) {
		case 0:
			return 0;
		case 99:
			return 99;
		case 1:
			nanosleep(&delay, 0);   // avoid busy wait
		}

		clock_gettime(CLOCK_MONOTONIC, &now);
	}

	return 2;
}
