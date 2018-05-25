/*
 * Copyright © 2011 Intel Corporation
 * Copyright © 2016 Giulio Camuffo
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial
 * portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "config.h"

#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>
#include <pthread.h>

#include "compositor.h"
#include "compositor/weston.h"
#include "xwayland/xwayland-api.h"
#include "shared/helpers.h"

struct wet_xwayland {
	struct weston_compositor *compositor;
	const struct weston_xwayland_api *api;
	struct weston_xwayland *xwayland;
	struct wl_event_source *sigusr1_source;
	struct wl_client *client;
	int wm_fd;
	struct weston_process process;
};

static int
handle_sigusr1(int fd, unsigned int mask, void *data)
{
	struct wet_xwayland *wxw = data;
	pid_t pid;

	if ( read(fd, &(pid), sizeof(pid_t)) == sizeof(pid_t)
             && wxw->process.pid == pid ) {
		wxw->api->xserver_loaded(wxw->xwayland, wxw->client, wxw->wm_fd);
		wl_event_source_remove( wxw->sigusr1_source );
		close( fd );
	}

	return 1;
}

static void*
recieve_sigusr1(void *data) {
	struct wet_xwayland *wxw = *((struct wet_xwayland **)data + 1);
	int fd = *((int *)data);
	sigset_t ss;
	siginfo_t si;

	free(data);
	data = NULL;

	sigemptyset( &ss );
	sigaddset( &ss, SIGUSR1 );

	do {
		sigwaitinfo( &ss, &si );
	} while( wxw->process.pid != si.si_pid );

	while ( write( fd, &(si.si_pid), sizeof(pid_t) ) != sizeof(pid_t) );

	close( fd );

	pthread_exit(NULL);
}

static pid_t
spawn_xserver(void *user_data, const char *display, int abstract_fd, int unix_fd)
{
	struct wet_xwayland *wxw = user_data;
	pid_t pid;
	char s[8], abstract_fd_str[8], unix_fd_str[8], wm_fd_str[8];
	int sv[2], wm[2], fd;
	char *xserver = NULL;
	struct weston_config *config = wet_get_config(wxw->compositor);
	struct weston_config_section *section;

	if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) < 0) {
		weston_log("wl connection socketpair failed\n");
		return 1;
	}

	if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, wm) < 0) {
		weston_log("X wm connection socketpair failed\n");
		return 1;
	}

	pid = fork();
	switch (pid) {
	case 0:
		/* SOCK_CLOEXEC closes both ends, so we need to unset
		 * the flag on the client fd. */
		fd = dup(sv[1]);
		if (fd < 0)
			goto fail;
		snprintf(s, sizeof s, "%d", fd);
		setenv("WAYLAND_SOCKET", s, 1);

		fd = dup(abstract_fd);
		if (fd < 0)
			goto fail;
		snprintf(abstract_fd_str, sizeof abstract_fd_str, "%d", fd);
		fd = dup(unix_fd);
		if (fd < 0)
			goto fail;
		snprintf(unix_fd_str, sizeof unix_fd_str, "%d", fd);
		fd = dup(wm[1]);
		if (fd < 0)
			goto fail;
		snprintf(wm_fd_str, sizeof wm_fd_str, "%d", fd);

		section = weston_config_get_section(config,
						    "xwayland", NULL, NULL);
		weston_config_section_get_string(section, "path",
						 &xserver, XSERVER_PATH);

		/* Ignore SIGUSR1 in the child, which will make the X
		 * server send SIGUSR1 to the parent (weston) when
		 * it's done with initialization.  During
		 * initialization the X server will round trip and
		 * block on the wayland compositor, so avoid making
		 * blocking requests (like xcb_connect_to_fd) until
		 * it's done with that. */
		signal(SIGUSR1, SIG_IGN);

		if (execl(xserver,
			  xserver,
			  display,
			  "-rootless",
			  "-listen", abstract_fd_str,
			  "-listen", unix_fd_str,
			  "-wm", wm_fd_str,
			  "-terminate",
			  NULL) < 0)
			weston_log("exec of '%s %s -rootless "
				   "-listen %s -listen %s -wm %s "
				   "-terminate' failed: %m\n",
				   xserver, display,
				   abstract_fd_str, unix_fd_str, wm_fd_str);
	fail:
		_exit(EXIT_FAILURE);

	default:
		close(sv[1]);
		wxw->client = wl_client_create(wxw->compositor->wl_display, sv[0]);

		close(wm[1]);
		wxw->wm_fd = wm[0];

		wxw->process.pid = pid;
		weston_watch_process(&wxw->process);
		break;

	case -1:
		weston_log("Failed to fork to spawn xserver process\n");
		break;
	}

	return pid;
}

static void
xserver_cleanup(struct weston_process *process, int status)
{
	struct wet_xwayland *wxw =
		container_of(process, struct wet_xwayland, process);
	struct wl_event_loop *loop =
		wl_display_get_event_loop(wxw->compositor->wl_display);

	wxw->api->xserver_exited(wxw->xwayland, status);
	wxw->sigusr1_source = wl_event_loop_add_signal(loop, SIGUSR1,
                                                       handle_sigusr1, wxw);
	wxw->client = NULL;
}

int
wet_load_xwayland(struct weston_compositor *comp)
{
	const struct weston_xwayland_api *api;
	struct weston_xwayland *xwayland;
	struct wet_xwayland *wxw;
	struct wl_event_loop *loop;
	pthread_t sig_reciever;
	int sigusr1_fifo[2];
	void *sigusr1_data;

	if (weston_compositor_load_xwayland(comp) < 0)
		return -1;

	api = weston_xwayland_get_api(comp);
	if (!api) {
		weston_log("Failed to get the xwayland module API.\n");
		return -1;
	}

	xwayland = api->get(comp);
	if (!xwayland) {
		weston_log("Failed to get the xwayland object.\n");
		return -1;
	}

	wxw = zalloc(sizeof *wxw);
	if (!wxw)
		return -1;

	wxw->compositor = comp;
	wxw->api = api;
	wxw->xwayland = xwayland;
	wxw->process.cleanup = xserver_cleanup;
	if (api->listen(xwayland, wxw, spawn_xserver) < 0)
		return -1;

	if ( (sigusr1_data = malloc( 2 * sizeof(void *) )) == NULL )
		return -1;

	loop = wl_display_get_event_loop(wxw->compositor->wl_display);

	if ( pipe(sigusr1_fifo) != 0 ) {
		free(sigusr1_data);
		return -1;
	}

	wxw->sigusr1_source = wl_event_loop_add_fd(loop, sigusr1_fifo[0],
                                                   WL_EVENT_READABLE,
                                                   handle_sigusr1, wxw);

	*((int *) sigusr1_data) = sigusr1_fifo[1];
	*((struct wet_xwayland **) sigusr1_data + 1) = wxw;
	if ( pthread_create(&sig_reciever, NULL, recieve_sigusr1, sigusr1_data) != 0 )
	{
		weston_log("Failed to spawn xwayland signal reciever.\n");
		free( sigusr1_data );
		sigusr1_data = NULL;
		close(sigusr1_fifo[0]);
		close(sigusr1_fifo[1]);
		wl_event_source_remove( wxw->sigusr1_source );
	}
	else
		pthread_detach( sig_reciever );

	return 0;
}
