#include <stdio.h>
#include <signal.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <limits.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <sys/socket.h>

#include "zdtmtst.h"

const char *test_doc	= "Multi-process socket loop";
const char *test_author	= "Pavel Emelianov <xemul@parallels.com>";

#define PROCS_DEF	4
#define PROCS_MAX	64
unsigned int num_procs = PROCS_DEF;
TEST_OPTION(num_procs, uint, "# processes to create "
	    "(default " __stringify(PROCS_DEF)
	    ", max " __stringify(PROCS_MAX) ")", 0);

volatile sig_atomic_t num_exited = 0;
void inc_num_exited(int signo)
{
	num_exited++;
}

int main(int argc, char **argv)
{
	int ret = 0;
	pid_t pid;
	int i;
	uint8_t buf[0x100000];
	int socks[PROCS_MAX * 2];
	int in, out;

	test_init(argc, argv);

	if (num_procs > PROCS_MAX) {
		err("%d processes is too many: max = %d\n", num_procs, PROCS_MAX);
		exit(1);
	}

	for (i = 0; i < num_procs; i++)
		if (socketpair(AF_LOCAL, SOCK_STREAM, 0, socks + i * 2)) {
			err("Can't create socks: %m\n");
			exit(1);
		}

	if (signal(SIGCHLD, inc_num_exited) == SIG_ERR) {
		err("can't set SIGCHLD handler: %m\n");
		exit(1);
	}

	for (i = 1; i < num_procs; i++) {	/* i = 0 - parent */
		pid = test_fork();
		if (pid < 0) {
			err("Can't fork: %m\n");
			kill(0, SIGKILL);
			exit(1);
		}

		if (pid == 0) {
			int j;
			in = i * 2;
			out = in - 1;
			for (j = 0; j < num_procs * 2; j++)
				if (j != in && j != out)
					close(socks[j]);

			signal(SIGPIPE, SIG_IGN);
			if (pipe_in2out(socks[in], socks[out], buf, sizeof(buf)) < 0)
				/* pass errno as exit code to the parent */
				if (test_go() /* signal NOT delivered */ ||
					(errno != EINTR && errno != EPIPE
						&& errno != ECONNRESET))
					ret = errno;

			test_waitsig();	/* even if failed, wait for migration to complete */

			close(socks[in]);
			close(socks[out]);
			exit(ret);
		}
	}

	for (i = 1; i < num_procs * 2 - 1; i++)
		close(socks[i]);
	in = socks[0];
	out = socks[num_procs * 2 - 1];

	/* don't block on writing, _do_ block on reading */
	if (set_nonblock(out,1) < 0) {
		err("setting O_NONBLOCK failed: %m");
		exit(1);
	}

	if (num_exited) {
		err("Some children died unexpectedly\n");
		kill(0, SIGKILL);
		exit(1);
	}

	test_daemon();

	while (test_go()) {
		int len, rlen = 0, wlen;
		uint8_t rbuf[sizeof(buf)], *p;

		datagen(buf, sizeof(buf), NULL);
		wlen = write(out, buf, sizeof(buf));
		if (wlen < 0) {
			if (errno == EINTR)
				continue;
			else {
				fail("write failed: %m\n", i);
				ret = 1;
				break;
			}
		}

		for (p = rbuf, len = wlen; len > 0; p += rlen, len -= rlen) {
			rlen = read(in, p, len);
			if (rlen <= 0)
				break;
		}

		if (rlen < 0 && errno == EINTR)
			continue;

		if (len > 0) {
			fail("read failed: %m\n");
			ret = 1;
			break;
		}

		if (memcmp(buf, rbuf, wlen)) {
			fail("data mismatch\n");
			ret = 1;
			break;
		}
	}


	test_waitsig();	/* even if failed, wait for migration to complete */

	/* We expect that write(2) in child may return error only after signal
	 * has been received. Thus, send signal before closing parent fds.
	 */
	if (kill(0, SIGTERM)) {
		fail("failed to send SIGTERM to my process group: %m\n");
		goto out;	/* shouldn't wait() in this case */
	}
	if (close(out))
		fail("Failed to close parent fd 'out': %m\n");
	/* If last child in the chain (from whom we read data) receives signal
	 * after parent has finished reading but before calling write(2), this
	 * child can block forever.  To avoid this, close 'in' fd.
	 */
	if (close(in))
		fail("failed to close parent fd 'in': %m\n");

	for (i = 1; i < num_procs; i++) {	/* i = 0 - parent */
		int chret;
		if (wait(&chret) < 0) {
			fail("can't wait for a child: %m\n");
			ret = 1;
			continue;
		}

		chret = WEXITSTATUS(chret);
		if (chret) {
			fail("child %d exited with non-zero code %d (%s)\n",
			     i, chret, strerror(chret));
			ret = 1;
			continue;
		}
	}

	if (!ret)
		pass();

out:
	return 0;
}
