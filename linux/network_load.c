#include <arpa/inet.h>
#include <ctype.h>
#include <curses.h>
#include <errno.h>
#include <fcntl.h>
#include <ncurses.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* Macros and symbolic constants that configure the program */
#define INPUT_BUFFER_SIZE 100000
#define OUTPUT_BUFFER_SIZE 10000
#define CMD_BUFFER_SIZE 2048

/* Well, yeah, these are purely functional macros now ... */
#define CMD_TYPE_START_UDP_IP 0
#define CMD_TYPE_CONNECT_TCP_IP 1

#define CTRL(c) (c & 0x1f)

#define log(f,...) { wprintw (log_win, f, ##__VA_ARGS__); wrefresh (log_win); }
#define log_perror(s) { wprintw (log_win, "%s: %s\n", s, strerror (errno)); wrefresh (log_win); }


/* Structures and other datatypes */
struct udp_sender {
	struct sockaddr_in saddr;
	char caddr[32];

	int fd;

	uint64_t data;
	double data_flow;
};

struct tcp_connection {
	struct sockaddr_in saddr;
	char caddr[32];

	int fd;
	uint64_t data_in, data_out;
	double data_flow_in, data_flow_out;
};


/* Global variables */
/* A list of TCP connections */
int epoll_fd = -1;

struct tcp_connection *tcp_connections = NULL;
size_t tcp_connections_cap = 0;
size_t tcp_connections_len = 0;


/* Helper functions */
int add_tcp_connection (const struct sockaddr_in *saddr, int fd)
{
	if (!tcp_connections)
	{
		tcp_connections_cap = 2;
		tcp_connections_len = 0;

		tcp_connections = calloc (tcp_connections_cap, sizeof (*tcp_connections));
		if (!tcp_connections)
		{
			perror ("calloc failed");
			return -1;
		}
	}

	if (tcp_connections_len >= tcp_connections_cap)
	{
		struct tcp_connection *newlist = calloc (
				tcp_connections_cap * 2,
				sizeof (*tcp_connections));

		if (!newlist)
		{
			perror ("calloc failed");
			return -1;
		}

		memcpy (newlist, tcp_connections, tcp_connections_len * sizeof (*tcp_connections));

		struct tcp_connection *oldlist = tcp_connections;

		tcp_connections = newlist;
		tcp_connections_cap *= 2;

		free (oldlist);
	}

	struct tcp_connection *s = tcp_connections + tcp_connections_len;

	s->saddr = *saddr;

	inet_ntop (AF_INET, saddr, s->caddr, sizeof (s->caddr));

	s->fd = fd;
	s->data_in = s->data_out = 0;
	s->data_flow_in = s->data_flow_out = 0.;


	struct epoll_event ep_event;

	ep_event.data.u64 = 200000 + tcp_connections_len;
	ep_event.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLHUP;
	epoll_ctl (epoll_fd, EPOLL_CTL_ADD, s->fd, &ep_event);

	tcp_connections_len++;

	return 0;
}

void remove_tcp_connection_by_id (int id)
{
	if (!tcp_connections || id >= tcp_connections_len)
		return;

	epoll_ctl (epoll_fd, EPOLL_CTL_DEL, tcp_connections[id].fd, NULL);

	close (tcp_connections[id].fd);

	tcp_connections_len--;

	for (size_t i = id; i < tcp_connections_len; i++)
	{
		tcp_connections[id] = tcp_connections[id + 1];
	}
}


/* The main program */
int main(int argc, char** argv)
{
	int exit_code = EXIT_FAILURE;

	int row, col, gen_win_height, conn_win_height, log_win_height;
	WINDOW *gen_win = NULL, *conn_win = NULL, *log_win = NULL, *help_win = NULL, *cmd_win = NULL;

	/* To handle user input */
	int cmd_type;
	char cmd_buffer[CMD_BUFFER_SIZE];
	int cmd_buffer_pos;

	struct epoll_event ep_event;

	int udp_infd = -1;
	int udp_inport = 11111;
	struct sockaddr_in udp_inaddr = {
		.sin_family = AF_INET,
		.sin_port = htons (udp_inport),
		.sin_addr = INADDR_ANY
	};

	int tcp_listenfd = -1;
	int tcp_listenport = 11112;
	struct sockaddr_in tcp_listenaddr = {
		.sin_family = AF_INET,
		.sin_port = htons (tcp_listenport),
		.sin_addr = INADDR_ANY
	};

	/* A list of UDP senders */
	struct udp_sender *udp_senders = NULL;
	size_t udp_senders_cap = 0;
	size_t udp_senders_len = 0;

	/* To calculate data flow */
	uint64_t udp_data = 0;
	double udp_data_flow = 0.;
	struct timespec t1, t2;

	uint8_t input_buffer[INPUT_BUFFER_SIZE];
	uint8_t output_buffer[OUTPUT_BUFFER_SIZE];

	/* Acquire resources */
	/* Start curses mode */
	initscr ();
	getmaxyx (stdscr, row, col);
	raw ();
	noecho ();
	keypad (stdscr, TRUE);
	curs_set (0);

	/* Create a window for general information, one to display all connections,
	 * and one to log to. */
	gen_win_height = 7;
	log_win_height = 4;
	conn_win_height = row - gen_win_height - log_win_height;

	gen_win = newwin (gen_win_height, col, 0, 0);
	conn_win = newwin (conn_win_height, col, gen_win_height, 0);
	log_win = newwin (log_win_height, col, gen_win_height + conn_win_height, 0);

	if (!gen_win | !conn_win | !log_win)
	{
		fprintf (stderr, "Failed to create ncurses window\n");
		goto END;
	}

	keypad (gen_win, TRUE);
	keypad (conn_win, TRUE);
	keypad (log_win, TRUE);

	/* Write initial content to the windows */
	wprintw (gen_win, "Network load generator and data flow meter");
	mvwprintw (gen_win, 1, 0, "Hit `q' to quit and F1 for help\n");
	wrefresh (gen_win);

	wprintw (conn_win, "TCP connections and UDP senders:\n");
	whline (conn_win, 0, col);
	wrefresh (conn_win);

	whline (log_win, 0, col);
	wmove (log_win, 1, 0);
	wrefresh (log_win);

	/* Initialize an epoll interface */
	epoll_fd = epoll_create (2);
	if (epoll_fd < 0)
	{
		perror ("Failed to create epoll instance");
		goto END;
	}

	/* About fds registered with epoll: u64 100000 ist the first udp sender,
	 * u64 200000 is the first tcp connection, u64 3 is the udp input,
	 * u64 4 is the tcp listener, and u64 0 is stdin. */
	ep_event.data.u64 = 0;
	ep_event.events = EPOLLIN;
	epoll_ctl (epoll_fd, EPOLL_CTL_ADD, 0, &ep_event);


	/* Create a socket to receive incoming udp data */
	udp_infd = socket (AF_INET, SOCK_DGRAM, 0);
	if (udp_infd < 0)
	{
		perror ("Failed to create an udp socket");
		goto END;
	}

	int flags = fcntl (udp_infd, F_GETFL, 0);
	if (flags < 0)
	{
		perror ("Failed to get udp input fd flags");
		goto END;
	}

	flags |= O_NONBLOCK;

	if (fcntl (udp_infd, F_SETFL, flags) < 0)
	{
		perror ("Failed to set udp input fd to nonblocking mode");
		goto END;
	}

	if (bind (udp_infd, (struct sockaddr*) &udp_inaddr, sizeof (udp_inaddr)) >= 0)
	{
		/* Add to epoll instance */
		ep_event.data.u64 = 3;
		ep_event.events = EPOLLIN;
		epoll_ctl (epoll_fd, EPOLL_CTL_ADD, udp_infd, &ep_event);

		wprintw (gen_win, "Incoming traffic is accepted on port %d\n", (int) udp_inport);
		wrefresh (gen_win);
	}
	else
	{
		close (udp_infd);
		udp_infd = -1;

		wprintw (gen_win, "No incoming traffic is accepted\n");
		wrefresh (gen_win);
	}

	/* Create a socket to listen for tcp connections */
	tcp_listenfd = socket (AF_INET, SOCK_STREAM, 0);
	if (tcp_listenfd < 0)
	{
		perror ("Failed to create a tcp socket");
		goto END;
	}

	flags = fcntl (tcp_listenfd, F_GETFL, 0);
	if (flags < 0)
	{
		perror ("Failed to get tcp listener fd's flags");
		goto END;
	}

	flags |= O_NONBLOCK;

	if (fcntl (tcp_listenfd, F_SETFL, flags) < 0)
	{
		perror ("Failed to set the tcp listener's fd to nonblocking mode");
		goto END;
	}

	if (bind (tcp_listenfd, (struct sockaddr*) &tcp_listenaddr, sizeof (tcp_listenaddr)) >= 0)
	{
		/* Start listening */
		if (listen (tcp_listenfd, 1000) < 0)
		{
			perror ("Failed to start listening on a tcp socket");
			goto END;
		}

		/* Add to epoll instance */
		ep_event.data.u64 = 4;
		ep_event.events = EPOLLIN;
		epoll_ctl (epoll_fd, EPOLL_CTL_ADD, tcp_listenfd, &ep_event);

		wprintw (gen_win, "Listening for TCP connections on port %d\n", (int) tcp_listenport);
		wrefresh (gen_win);
	}
	else
	{
		close (tcp_listenfd);
		tcp_listenfd = -1;

		wprintw (gen_win, "Not listening for incoming tcp connections");
		wrefresh (gen_win);
	}


	/* Fill the output buffer with some random data */
	log ("Generating %d bytes of random data to send ... ", (int) OUTPUT_BUFFER_SIZE);

	srand (time (NULL));
	
	for (size_t i = 0; i < OUTPUT_BUFFER_SIZE; i++)
	{
		output_buffer[i] = rand () % 256;
	}

	log ("done.\n");


	/* The polling loop, the program's central event handler */
	/* Everything worked so far, change error handling to optimistic. */
	exit_code = EXIT_SUCCESS;

	int running = 1;

	clock_gettime (CLOCK_MONOTONIC, &t1);

	while (running)
	{
		clock_gettime (CLOCK_MONOTONIC, &t2);

		long delay = t2.tv_nsec - t1.tv_nsec + (t2.tv_sec - t1.tv_sec) * 1000000000;

		if (delay > 900000000)
		{
			/* Print receiver */
			if (udp_infd >= 0)
			{
				udp_data_flow = (double) udp_data / ((double) delay / 1e9) * 8;

				/* Format a nice display */
				double val;
				char *unit;

				if (udp_data_flow > 1e9)
				{
					unit = "GBit";
					val = udp_data_flow / 1e9;
				}
				else if (udp_data_flow > 1e6)
				{
					unit = "MBit";
					val = udp_data_flow / 1e6;
				}
				else if (udp_data_flow > 1e3)
				{
					unit = "kBit";
					val = udp_data_flow / 1e3;
				}
				else
				{
					unit = "Bit";
					val = udp_data_flow;
				}

				mvwprintw (gen_win, 4, 0,
						"Current incoming data flow: %.3f %s/s",
						val,
						unit);
			}

			/* Print senders */
			if (udp_senders && udp_senders_len > 0)
			{
				wclear (conn_win);

				wprintw (conn_win, "Connections resp. senders:\n");
				whline (conn_win, 0, col);
				wmove (conn_win, 2, 0);

				double val;
				char *unit;

				for (size_t i = 0; i < udp_senders_len; i++)
				{
					struct udp_sender *s = udp_senders + i;

					s->data_flow = (double) s->data / ((double) delay / 1e9) * 8;

					/* Format a nice display */
					if (s->data_flow > 1e9)
					{
						unit = "GBit";
						val = s->data_flow / 1e9;
					}
					else if (s->data_flow > 1e6)
					{
						unit = "MBit";
						val = s->data_flow / 1e6;
					}
					else if (s->data_flow > 1e3)
					{
						unit = "kBit";
						val = s->data_flow / 1e3;
					}
					else
					{
						unit = "Bit";
						val = s->data_flow;
					}

					wprintw (conn_win, " %-32s   %.3f %s/s\n", s->caddr, val, unit);

					s->data = 0;
				}
			}

			if (!help_win)
			{
				wrefresh (gen_win);
				wrefresh (conn_win);

				if (cmd_win)
				{
					redrawwin (cmd_win);
					wrefresh (cmd_win);
				}
			}

			/* Reset counters */
			t1 = t2;
			delay = 0;
			udp_data = 0;
		}

		/* Wait until the timeout expires or a fd becomes ready */
		int ret = epoll_wait (epoll_fd, &ep_event, 1, 1000 - delay / 1000000);

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
			int c = wgetch (cmd_win ? cmd_win : gen_win);

			if (help_win)
			{
				wclear (help_win);
				wrefresh (help_win);
				delwin (help_win);
				help_win = NULL;

				redrawwin (gen_win);
				wrefresh (gen_win);

				redrawwin (conn_win);
				wrefresh (conn_win);

				redrawwin (log_win);
				wrefresh (log_win);
			}
			else if (c == CTRL('u'))
			{
				cmd_type = CMD_TYPE_START_UDP_IP;
				cmd_buffer_pos = 0;

				cmd_win = newwin (4, col - 2, (row - 4) / 2, 0);
				if (!cmd_win)
				{
					fprintf (stderr, "Failed to create ncurses window\n");
					exit_code = EXIT_FAILURE;
					break;
				}

				box (cmd_win, 0, 0);
				mvwprintw (cmd_win, 1, 1, "Start sending UDP packets to IPv4 address:");
				wmove (cmd_win, 2, 1);

				curs_set (1);
				wmove (cmd_win, 2, 1);

				wrefresh (cmd_win);
			}
			else if (c == CTRL('t'))
			{
				cmd_type = CMD_TYPE_CONNECT_TCP_IP;
				cmd_buffer_pos = 0;

				cmd_win = newwin (4, col - 2, (row - 4) / 2, 0);
				if (!cmd_win)
				{
					fprintf (stderr, "Failed to create ncurses window\n");
					exit_code = EXIT_FAILURE;
					break;
				}

				box (cmd_win, 0, 0);
				mvwprintw (cmd_win, 1, 1, "Connect using TCP to IPv4 address:");
				wmove (cmd_win, 2, 1);

				curs_set (1);
				wmove (cmd_win, 2, 1);

				wrefresh (cmd_win);
			}
			else if (c == 10 || c == KEY_ENTER)
			{
				if (cmd_win)
				{
					wclear (cmd_win);
					wrefresh (cmd_win);
					delwin (cmd_win);

					cmd_win = NULL;

					curs_set (0);

					redrawwin (gen_win);
					wrefresh (gen_win);

					redrawwin (conn_win);
					wrefresh (conn_win);

					redrawwin (log_win);
					wrefresh (log_win);

					/* Process user input */
					if (cmd_type == CMD_TYPE_START_UDP_IP)
					{
						struct in_addr addr;

						cmd_buffer[cmd_buffer_pos] = '\0';

						if (inet_aton (cmd_buffer, &addr))
						{
							if (!udp_senders)
							{
								udp_senders_cap = 2;
								udp_senders_len = 0;

								udp_senders = calloc (udp_senders_cap, sizeof (*udp_senders));
								if (!udp_senders)
								{
									perror ("calloc failed.");
									exit_code = EXIT_FAILURE;
									break;
								}
							}

							if (udp_senders_len >= udp_senders_cap)
							{
								struct udp_sender *newlist = calloc (
										udp_senders_cap * 2,
										sizeof (*udp_senders));

								if (!newlist)
								{
									perror ("calloc failed.");
									exit_code = EXIT_FAILURE;
									break;
								}

								memcpy (
										newlist,
										udp_senders,
										udp_senders_len * sizeof (*udp_senders));

								struct udp_sender *oldlist = udp_senders;
								udp_senders = newlist;
								free (oldlist);

								udp_senders_cap *= 2;
							}

							struct udp_sender *s = udp_senders + udp_senders_len;

							s->saddr.sin_family = AF_INET;
							s->saddr.sin_port = htons (udp_inport);
							s->saddr.sin_addr = addr;

							inet_ntop (AF_INET, &addr, s->caddr, 32);

							int ok = 1;

							s->fd = socket (AF_INET, SOCK_DGRAM, 0);
							if (s->fd < 0)
								ok = 0;

							if (ok)
							{
								if (connect (s->fd, (struct sockaddr *) &(s->saddr), sizeof (s->saddr)) < 0)
								{
									close (s->fd);
									ok = 0;
								}
							}

							int flags;

							if (ok)
							{
								flags = fcntl (s->fd, F_GETFL, 0);
								if (flags < 0)
								{
									close (s->fd);
									ok = 0;
								}

							}

							flags |= O_NONBLOCK;

							if (ok)
							{
								if (fcntl (s->fd, F_SETFL, flags) < 0)
								{
									close (s->fd);
									ok = 0;
								}
							}

							if (ok)
							{
								s->data = 0;
								s->data_flow = 0.;

								ep_event.data.u64 = 100000 + udp_senders_len;
								ep_event.events = EPOLLOUT;
								epoll_ctl (epoll_fd, EPOLL_CTL_ADD, s->fd, &ep_event);

								udp_senders_len++;
							}
						}
					}
					else if (cmd_type == CMD_TYPE_CONNECT_TCP_IP)
					{
						struct in_addr addr;

						cmd_buffer[cmd_buffer_pos] = '\0';

						if (inet_aton (cmd_buffer, &addr))
						{
							struct sockaddr_in saddr = {
								.sin_family = AF_INET,
								.sin_port = htons (tcp_listenport),
								.sin_addr = addr
							};

							int ok = 1;

							int fd = socket (AF_INET, SOCK_STREAM, 0);
							if (fd < 0)
								ok = 0;

							if (ok)
							{
								if (connect (fd, (struct sockaddr *) &(saddr), sizeof (saddr)) < 0)
								{
									close (fd);
									ok = 0;
								}
							}

							int flags;

							if (ok)
							{
								flags = fcntl (fd, F_GETFL, 0);
								if (flags < 0)
								{
									close (fd);
									ok = 0;
								}
							}

							flags |= O_NONBLOCK;

							if (ok)
							{
								if (fcntl (fd, F_SETFL, flags) < 0)
								{
									close (fd);
									ok = 0;
								}
							}

							if (ok)
							{
								if (add_tcp_connection (&saddr, fd) < 0)
									close (fd);
							}
						}
					}
				}
			}
			else if (tolower(c) == 'q')
			{
				if (cmd_win)
				{
					wclear (cmd_win);
					wrefresh (cmd_win);
					delwin (cmd_win);

					cmd_win = NULL;

					curs_set (0);

					redrawwin (gen_win);
					wrefresh (gen_win);

					redrawwin (conn_win);
					wrefresh (conn_win);

					redrawwin (log_win);
					wrefresh (log_win);
				}
				else
				{
					running = 0;
				}
			}
			else if (c == KEY_F(1) && !cmd_win)
			{
				help_win = newwin (row, col, 0, 0);
				if (!help_win)
				{
					fprintf (stderr, "Failed to create ncurses window\n");
					exit_code = EXIT_FAILURE;
					break;
				}

				wprintw (help_win, "Help:\n\n");
				wprintw (help_win,
"Ctrl+U        Begin sending UDP packets to a specified host\n"
"Ctrl+T        Connect to a specified host using TCP\n\n"
"If no incoming traffic is accepted, this is most likely because\n"
"I cannot bind an udp socket to port 11111 on any IPv4 address of the system.\n");

				wprintw (help_win, "\nPress any key to return to main screen ...");
				wrefresh (help_win);
			}
			else if (cmd_win && (cmd_type == CMD_TYPE_START_UDP_IP ||
						cmd_type == CMD_TYPE_CONNECT_TCP_IP))
			{
				if ((c >= '0' && c <= '9') || c == '.')
				{
					cmd_buffer[cmd_buffer_pos++] = c;
					waddch (cmd_win, c);
				}
			}
		}
		else if (ep_event.data.u64 == 3)
		{
			/* Incomming data on the udp socket */
			int ret;

			do {
				ret = read (udp_infd, input_buffer, INPUT_BUFFER_SIZE);

				if (ret < 0)
				{
					if (errno != EWOULDBLOCK && errno != EAGAIN)
					{
						perror ("read on udp input fd failed");
						running = 0;
						exit_code = EXIT_FAILURE;
					}
				}
				else
				{
					udp_data += ret;
				}
			}
			while (ret > INPUT_BUFFER_SIZE / 2);
		}
		else if (ep_event.data.u64 == 4)
		{
			/* Accept a new connection */
		}
		else if (ep_event.data.u64 >= 200000)
		{
			struct tcp_connection *c = tcp_connections + (ep_event.data.u64 - 200000);

			int ret;

			if (ep_event.events & (EPOLLHUP | EPOLLRDHUP))
			{
				int id = ep_event.data.u64 - 200000;

				log ("Connection %d closed by remote\n", id);

				remove_tcp_connection_by_id (id);
				c = NULL;
			}

			if (c && (ep_event.events & EPOLLIN))
			{
				do {
					ret = read (c->fd, input_buffer, INPUT_BUFFER_SIZE);

					if (ret < 0)
					{
						if (errno != EWOULDBLOCK && errno != EAGAIN)
						{
							int id = ep_event.data.u64 - 200000;

							log ("Removing connection %d since it seems broken\n", id);

							remove_tcp_connection_by_id (id);
							c = NULL;

							break;
						}
					}

					c->data_in += ret;
				}
				while (ret > INPUT_BUFFER_SIZE / 2);
			}

			if (c && (ep_event.events & EPOLLOUT))
			{
				do {
					ret = write (c->fd, output_buffer, OUTPUT_BUFFER_SIZE);

					if (ret < 0)
					{
						if (errno != EWOULDBLOCK && errno != EAGAIN)
						{
							int id = ep_event.data.u64 - 200000;

							log ("Removing connection %d since it seems broken\n", id);

							remove_tcp_connection_by_id (id);
							c = NULL;

							break;
						}
					}

					c->data_out += ret;
				}
				while (ret > OUTPUT_BUFFER_SIZE / 2);
			}
		}
		else if (ep_event.data.u64 >= 100000)
		{
			struct udp_sender *s = udp_senders + (ep_event.data.u64 - 100000);

			int ret;

			do {
				ret = write (s->fd, output_buffer, OUTPUT_BUFFER_SIZE);

				if (ret < 0)
				{
					if (errno != EWOULDBLOCK && errno != EAGAIN)
					{
						// perror ("write on udp output fd of sender failed");
						log_perror ("write on udp output fd of sender failed");

						// running = 0;
						// exit_code = EXIT_FAILURE;
					}
				}
				else
				{
					s->data += ret;
				}
			}
			while (ret > OUTPUT_BUFFER_SIZE / 2);
		}
	}

END:
	/* Cleanup resources */
	if (epoll_fd >= 0)
		close (epoll_fd);

	if (tcp_connections)
	{
		for (size_t i = 0; i < tcp_connections_len; i++)
		{
			close (tcp_connections[i].fd);
		}

		free (tcp_connections);
	}

	if (udp_senders)
	{
		for (size_t i = 0; i < udp_senders_len; i++)
		{
			close (udp_senders[i].fd);
		}

		free (udp_senders);
	}

	if (tcp_listenfd >= 0)
		close (tcp_listenfd);

	if (udp_infd >= 0)
		close (udp_infd);

	/* Exit curses mode */
	if (cmd_win)
		delwin (cmd_win);

	if (help_win)
		delwin (help_win);

	if (log_win)
		delwin (log_win);

	if (conn_win)
		delwin (conn_win);

	if (gen_win)
		delwin (gen_win);

	endwin ();

	return exit_code;
}
