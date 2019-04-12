#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <unistd.h>

int main(int argc, char** argv)
{
	int exit_code = EXIT_FAILURE;

	int epoll_fd = -1;
	struct epoll_event ep_event;

	/* Acquire resources */
	epoll_fd = epoll_create (2);
	if (epoll_fd < 0)
	{
		perror ("Failed to create epoll instance");
		goto END;
	}

	/* About fds registered with epoll: u64 100 ist the first connection, u64 3
	 * is the listener, u64 0 is stdin. */
	ep_event.data.u64 = 0;
	ep_event.events = EPOLLIN;
	epoll_ctl (epoll_fd, EPOLL_CTL_ADD, 0, &ep_event);


	/* The polling loop, the program's central event handler */

	/* Everything worked so far, change error handling to optimistic. */
	exit_code = EXIT_SUCCESS;

	int running = 1;

	while (running)
	{
		int ret = epoll_wait (epoll_fd, &ep_event, 1, -1);

		if (ret < 0)
		{
			perror ("epoll_wait failed");
			exit_code = EXIT_FAILURE;
			break;
		}

		if (ret == 0)
			continue;

		/* Handle the event */
		if (ep_event.data.u64 == 0)
		{
			char c = tolower (getchar());

			switch (c)
			{
				case 'q':
					running = 0;
					break;
			}
		}
	}

END:
	/* Cleanup resources */
	if (epoll_fd >= 0)
	{
		close (epoll_fd);
	}

	return exit_code;
}
