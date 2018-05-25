/*
 * Copyright © 2010-2011 Intel Corporation
 * Copyright © 2008-2011 Kristian Høgsberg
 * Copyright © 2012-2018 Collabora, Ltd.
 * Copyright © 2010-2011 Benjamin Franzke
 * Copyright © 2013 Jason Ekstrand
 * Copyright © 2017, 2018 General Electric Company
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

#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <dlfcn.h>
#include <stdint.h>
#include <string.h>
#include <sys/utsname.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <libinput.h>
#include <sys/time.h>
#include <linux/limits.h>

#include "weston.h"
#include "compositor.h"
#include "../shared/os-compatibility.h"
#include "../shared/helpers.h"
#include "../shared/string-helpers.h"
#include "git-version.h"
#include "version.h"
#include "weston.h"

#include "compositor-drm.h"
#include "compositor-headless.h"
#include "compositor-rdp.h"
#include "compositor-fbdev.h"
#include "compositor-x11.h"
#include "compositor-wayland.h"
#include "windowed-output-api.h"

#define WINDOW_TITLE "Weston Compositor"

struct wet_output_config {
	int width;
	int height;
	int32_t scale;
	uint32_t transform;
};

struct wet_compositor;

struct wet_head_tracker {
	struct wl_listener head_destroy_listener;
};

struct wet_compositor {
	struct weston_compositor *compositor;
	struct weston_config *config;
	struct wet_output_config *parsed_options;
	bool drm_use_current_mode;
	struct wl_listener heads_changed_listener;
	int (*simple_output_configure)(struct weston_output *output);
	bool init_failed;
};

static FILE *weston_logfile = NULL;

static int cached_tm_mday = -1;

static int weston_log_timestamp(void)
{
	struct timeval tv;
	struct tm *brokendown_time;
	char string[128];

	gettimeofday(&tv, NULL);

	brokendown_time = localtime(&tv.tv_sec);
	if (brokendown_time == NULL)
		return fprintf(weston_logfile, "[(NULL)localtime] ");

	if (brokendown_time->tm_mday != cached_tm_mday) {
		strftime(string, sizeof string, "%Y-%m-%d %Z", brokendown_time);
		fprintf(weston_logfile, "Date: %s\n", string);

		cached_tm_mday = brokendown_time->tm_mday;
	}

	strftime(string, sizeof string, "%H:%M:%S", brokendown_time);

	return fprintf(weston_logfile, "[%s.%03li] ", string, tv.tv_usec/1000);
}

static void
custom_handler(const char *fmt, va_list arg)
{
	weston_log_timestamp();
	fprintf(weston_logfile, "libwayland: ");
	vfprintf(weston_logfile, fmt, arg);
}

static void
weston_log_file_open(const char *filename)
{
	wl_log_set_handler_server(custom_handler);

	if (filename != NULL) {
		weston_logfile = fopen(filename, "a");
		if (weston_logfile)
			os_fd_set_cloexec(fileno(weston_logfile));
	}

	if (weston_logfile == NULL)
		weston_logfile = stderr;
	else
		setvbuf(weston_logfile, NULL, _IOLBF, 256);
}

static void
weston_log_file_close(void)
{
	if ((weston_logfile != stderr) && (weston_logfile != NULL))
		fclose(weston_logfile);
	weston_logfile = stderr;
}

static int
vlog(const char *fmt, va_list ap)
{
	int l;

	l = weston_log_timestamp();
	l += vfprintf(weston_logfile, fmt, ap);

	return l;
}

static int
vlog_continue(const char *fmt, va_list argp)
{
	return vfprintf(weston_logfile, fmt, argp);
}

static struct wl_list child_process_list;
static struct weston_compositor *segv_compositor;

static int
sigchld_handler(int signal_number, void *data)
{
	struct weston_process *p;
	int status;
	pid_t pid;

	while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
		wl_list_for_each(p, &child_process_list, link) {
			if (p->pid == pid)
				break;
		}

		if (&p->link == &child_process_list) {
			weston_log("unknown child process exited\n");
			continue;
		}

		wl_list_remove(&p->link);
		p->cleanup(p, status);
	}

	if (pid < 0 && errno != ECHILD)
		weston_log("waitpid error %m\n");

	return 1;
}

static void
child_client_exec(int sockfd, const char *path)
{
	int clientfd;
	char s[32];
	sigset_t allsigs;

	/* do not give our signal mask to the new process */
	sigfillset(&allsigs);
	sigprocmask(SIG_UNBLOCK, &allsigs, NULL);

	/* Launch clients as the user. Do not lauch clients with wrong euid.*/
	if (seteuid(getuid()) == -1) {
		weston_log("compositor: failed seteuid\n");
		return;
	}

	/* SOCK_CLOEXEC closes both ends, so we dup the fd to get a
	 * non-CLOEXEC fd to pass through exec. */
	clientfd = dup(sockfd);
	if (clientfd == -1) {
		weston_log("compositor: dup failed: %m\n");
		return;
	}

	snprintf(s, sizeof s, "%d", clientfd);
	setenv("WAYLAND_SOCKET", s, 1);

	if (execl(path, path, NULL) < 0)
		weston_log("compositor: executing '%s' failed: %m\n",
			path);
}

WL_EXPORT struct wl_client *
weston_client_launch(struct weston_compositor *compositor,
		     struct weston_process *proc,
		     const char *path,
		     weston_process_cleanup_func_t cleanup)
{
	int sv[2];
	pid_t pid;
	struct wl_client *client;

	weston_log("launching '%s'\n", path);

	if (os_socketpair_cloexec(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
		weston_log("weston_client_launch: "
			"socketpair failed while launching '%s': %m\n",
			path);
		return NULL;
	}

	pid = fork();
	if (pid == -1) {
		close(sv[0]);
		close(sv[1]);
		weston_log("weston_client_launch: "
			"fork failed while launching '%s': %m\n", path);
		return NULL;
	}

	if (pid == 0) {
		child_client_exec(sv[1], path);
		_exit(-1);
	}

	close(sv[1]);

	client = wl_client_create(compositor->wl_display, sv[0]);
	if (!client) {
		close(sv[0]);
		weston_log("weston_client_launch: "
			"wl_client_create failed while launching '%s'.\n",
			path);
		return NULL;
	}

	proc->pid = pid;
	proc->cleanup = cleanup;
	weston_watch_process(proc);

	return client;
}

WL_EXPORT void
weston_watch_process(struct weston_process *process)
{
	wl_list_insert(&child_process_list, &process->link);
}

struct process_info {
	struct weston_process proc;
	char *path;
};

static void
process_handle_sigchld(struct weston_process *process, int status)
{
	struct process_info *pinfo =
		container_of(process, struct process_info, proc);

	/*
	 * There are no guarantees whether this runs before or after
	 * the wl_client destructor.
	 */

	if (WIFEXITED(status)) {
		weston_log("%s exited with status %d\n", pinfo->path,
			   WEXITSTATUS(status));
	} else if (WIFSIGNALED(status)) {
		weston_log("%s died on signal %d\n", pinfo->path,
			   WTERMSIG(status));
	} else {
		weston_log("%s disappeared\n", pinfo->path);
	}

	free(pinfo->path);
	free(pinfo);
}

WL_EXPORT struct wl_client *
weston_client_start(struct weston_compositor *compositor, const char *path)
{
	struct process_info *pinfo;
	struct wl_client *client;

	pinfo = zalloc(sizeof *pinfo);
	if (!pinfo)
		return NULL;

	pinfo->path = strdup(path);
	if (!pinfo->path)
		goto out_free;

	client = weston_client_launch(compositor, &pinfo->proc, path,
				      process_handle_sigchld);
	if (!client)
		goto out_str;

	return client;

out_str:
	free(pinfo->path);

out_free:
	free(pinfo);

	return NULL;
}

static void
log_uname(void)
{
	struct utsname usys;

	uname(&usys);

	weston_log("OS: %s, %s, %s, %s\n", usys.sysname, usys.release,
						usys.version, usys.machine);
}

static struct wet_compositor *
to_wet_compositor(struct weston_compositor *compositor)
{
	return weston_compositor_get_user_data(compositor);
}

static struct wet_output_config *
wet_init_parsed_options(struct weston_compositor *ec)
{
	struct wet_compositor *compositor = to_wet_compositor(ec);
	struct wet_output_config *config;

	config = zalloc(sizeof *config);

	if (!config) {
		perror("out of memory");
		return NULL;
	}

	config->width = 0;
	config->height = 0;
	config->scale = 0;
	config->transform = UINT32_MAX;

	compositor->parsed_options = config;

	return config;
}

WL_EXPORT struct weston_config *
wet_get_config(struct weston_compositor *ec)
{
	struct wet_compositor *compositor = to_wet_compositor(ec);

	return compositor->config;
}

static const char xdg_error_message[] =
	"fatal: environment variable XDG_RUNTIME_DIR is not set.\n";

static const char xdg_wrong_message[] =
	"fatal: environment variable XDG_RUNTIME_DIR\n"
	"is set to \"%s\", which is not a directory.\n";

static const char xdg_wrong_mode_message[] =
	"warning: XDG_RUNTIME_DIR \"%s\" is not configured\n"
	"correctly.  Unix access mode must be 0700 (current mode is %o),\n"
	"and must be owned by the user (current owner is UID %d).\n";

static const char xdg_detail_message[] =
	"Refer to your distribution on how to get it, or\n"
	"http://www.freedesktop.org/wiki/Specifications/basedir-spec\n"
	"on how to implement it.\n";

static void
verify_xdg_runtime_dir(void)
{
	char *dir = getenv("XDG_RUNTIME_DIR");
	struct stat s;

	if (!dir) {
		weston_log(xdg_error_message);
		weston_log_continue(xdg_detail_message);
		exit(EXIT_FAILURE);
	}

	if (stat(dir, &s) || !S_ISDIR(s.st_mode)) {
		weston_log(xdg_wrong_message, dir);
		weston_log_continue(xdg_detail_message);
		exit(EXIT_FAILURE);
	}

	if ((s.st_mode & 0777) != 0700 || s.st_uid != getuid()) {
		weston_log(xdg_wrong_mode_message,
			   dir, s.st_mode & 0777, s.st_uid);
		weston_log_continue(xdg_detail_message);
	}
}

static int
usage(int error_code)
{
	fprintf(stderr,
		"Usage: weston [OPTIONS]\n\n"
		"This is weston version " VERSION ", the Wayland reference compositor.\n"
		"Weston supports multiple backends, and depending on which backend is in use\n"
		"different options will be accepted.\n\n"


		"Core options:\n\n"
		"  --version\t\tPrint weston version\n"
		"  -B, --backend=MODULE\tBackend module, one of\n"
#if defined(BUILD_DRM_COMPOSITOR)
			"\t\t\t\tdrm-backend.so\n"
#endif
#if defined(BUILD_FBDEV_COMPOSITOR)
			"\t\t\t\tfbdev-backend.so\n"
#endif
#if defined(BUILD_HEADLESS_COMPOSITOR)
			"\t\t\t\theadless-backend.so\n"
#endif
#if defined(BUILD_RDP_COMPOSITOR)
			"\t\t\t\trdp-backend.so\n"
#endif
#if defined(BUILD_WAYLAND_COMPOSITOR)
			"\t\t\t\twayland-backend.so\n"
#endif
#if defined(BUILD_X11_COMPOSITOR)
			"\t\t\t\tx11-backend.so\n"
#endif
		"  --shell=MODULE\tShell module, defaults to desktop-shell.so\n"
		"  -S, --socket=NAME\tName of socket to listen on\n"
		"  -i, --idle-time=SECS\tIdle time in seconds\n"
		"  --xwayland\t\tLoad the xwayland module\n"
		"  --modules\t\tLoad the comma-separated list of modules\n"
		"  --log=FILE\t\tLog to the given file\n"
		"  -c, --config=FILE\tConfig file to load, defaults to weston.ini\n"
		"  --no-config\t\tDo not read weston.ini\n"
		"  --wait-for-debugger\tRaise SIGSTOP on start-up\n"
		"  -h, --help\t\tThis help message\n\n");

#if defined(BUILD_DRM_COMPOSITOR)
	fprintf(stderr,
		"Options for drm-backend.so:\n\n"
		"  --seat=SEAT\t\tThe seat that weston should run on\n"
		"  --tty=TTY\t\tThe tty to use\n"
		"  --drm-device=CARD\tThe DRM device to use, e.g. \"card0\".\n"
		"  --use-pixman\t\tUse the pixman (CPU) renderer\n"
		"  --current-mode\tPrefer current KMS mode over EDID preferred mode\n\n");
#endif

#if defined(BUILD_FBDEV_COMPOSITOR)
	fprintf(stderr,
		"Options for fbdev-backend.so:\n\n"
		"  --tty=TTY\t\tThe tty to use\n"
		"  --device=DEVICE\tThe framebuffer device to use\n"
		"\n");
#endif

#if defined(BUILD_HEADLESS_COMPOSITOR)
	fprintf(stderr,
		"Options for headless-backend.so:\n\n"
		"  --width=WIDTH\t\tWidth of memory surface\n"
		"  --height=HEIGHT\tHeight of memory surface\n"
		"  --transform=TR\tThe output transformation, TR is one of:\n"
		"\tnormal 90 180 270 flipped flipped-90 flipped-180 flipped-270\n"
		"  --use-pixman\t\tUse the pixman (CPU) renderer (default: no rendering)\n"
		"  --no-outputs\t\tDo not create any virtual outputs\n"
		"\n");
#endif

#if defined(BUILD_RDP_COMPOSITOR)
	fprintf(stderr,
		"Options for rdp-backend.so:\n\n"
		"  --width=WIDTH\t\tWidth of desktop\n"
		"  --height=HEIGHT\tHeight of desktop\n"
		"  --env-socket\t\tUse socket defined in RDP_FD env variable as peer connection\n"
		"  --address=ADDR\tThe address to bind\n"
		"  --port=PORT\t\tThe port to listen on\n"
		"  --no-clients-resize\tThe RDP peers will be forced to the size of the desktop\n"
		"  --rdp4-key=FILE\tThe file containing the key for RDP4 encryption\n"
		"  --rdp-tls-cert=FILE\tThe file containing the certificate for TLS encryption\n"
		"  --rdp-tls-key=FILE\tThe file containing the private key for TLS encryption\n"
		"\n");
#endif

#if defined(BUILD_WAYLAND_COMPOSITOR)
	fprintf(stderr,
		"Options for wayland-backend.so:\n\n"
		"  --width=WIDTH\t\tWidth of Wayland surface\n"
		"  --height=HEIGHT\tHeight of Wayland surface\n"
		"  --scale=SCALE\t\tScale factor of output\n"
		"  --fullscreen\t\tRun in fullscreen mode\n"
		"  --use-pixman\t\tUse the pixman (CPU) renderer\n"
		"  --output-count=COUNT\tCreate multiple outputs\n"
		"  --sprawl\t\tCreate one fullscreen output for every parent output\n"
		"  --display=DISPLAY\tWayland display to connect to\n\n");
#endif

#if defined(BUILD_X11_COMPOSITOR)
	fprintf(stderr,
		"Options for x11-backend.so:\n\n"
		"  --width=WIDTH\t\tWidth of X window\n"
		"  --height=HEIGHT\tHeight of X window\n"
		"  --scale=SCALE\t\tScale factor of output\n"
		"  --fullscreen\t\tRun in fullscreen mode\n"
		"  --use-pixman\t\tUse the pixman (CPU) renderer\n"
		"  --output-count=COUNT\tCreate multiple outputs\n"
		"  --no-input\t\tDont create input devices\n\n");
#endif

	exit(error_code);
}

static int on_term_signal(int signal_number, void *data)
{
	struct wl_display *display = data;

	weston_log("caught signal %d\n", signal_number);
	wl_display_terminate(display);

	return 1;
}

static void
block_signal( int signal_number )
{
	sigset_t ss;

	sigemptyset( &ss );
	sigaddset( &ss, signal_number );
	pthread_sigmask( SIG_BLOCK, &ss, NULL );
}

static const char *
clock_name(clockid_t clk_id)
{
	static const char *names[] = {
		[CLOCK_REALTIME] =		"CLOCK_REALTIME",
		[CLOCK_MONOTONIC] =		"CLOCK_MONOTONIC",
		[CLOCK_MONOTONIC_RAW] =		"CLOCK_MONOTONIC_RAW",
		[CLOCK_REALTIME_COARSE] =	"CLOCK_REALTIME_COARSE",
		[CLOCK_MONOTONIC_COARSE] =	"CLOCK_MONOTONIC_COARSE",
#ifdef CLOCK_BOOTTIME
		[CLOCK_BOOTTIME] =		"CLOCK_BOOTTIME",
#endif
	};

	if (clk_id < 0 || (unsigned)clk_id >= ARRAY_LENGTH(names))
		return "unknown";

	return names[clk_id];
}

static const struct {
	uint32_t bit; /* enum weston_capability */
	const char *desc;
} capability_strings[] = {
	{ WESTON_CAP_ROTATION_ANY, "arbitrary surface rotation:" },
	{ WESTON_CAP_CAPTURE_YFLIP, "screen capture uses y-flip:" },
};

static void
weston_compositor_log_capabilities(struct weston_compositor *compositor)
{
	unsigned i;
	int yes;
	struct timespec res;

	weston_log("Compositor capabilities:\n");
	for (i = 0; i < ARRAY_LENGTH(capability_strings); i++) {
		yes = compositor->capabilities & capability_strings[i].bit;
		weston_log_continue(STAMP_SPACE "%s %s\n",
				    capability_strings[i].desc,
				    yes ? "yes" : "no");
	}

	weston_log_continue(STAMP_SPACE "presentation clock: %s, id %d\n",
			    clock_name(compositor->presentation_clock),
			    compositor->presentation_clock);

	if (clock_getres(compositor->presentation_clock, &res) == 0)
		weston_log_continue(STAMP_SPACE
				"presentation clock resolution: %d.%09ld s\n",
				(int)res.tv_sec, res.tv_nsec);
	else
		weston_log_continue(STAMP_SPACE
				"presentation clock resolution: N/A\n");
}

static void
handle_primary_client_destroyed(struct wl_listener *listener, void *data)
{
	struct wl_client *client = data;

	weston_log("Primary client died.  Closing...\n");

	wl_display_terminate(wl_client_get_display(client));
}

static int
weston_create_listening_socket(struct wl_display *display, const char *socket_name)
{
	if (socket_name) {
		if (wl_display_add_socket(display, socket_name)) {
			weston_log("fatal: failed to add socket: %m\n");
			return -1;
		}
	} else {
		socket_name = wl_display_add_socket_auto(display);
		if (!socket_name) {
			weston_log("fatal: failed to add socket: %m\n");
			return -1;
		}
	}

	setenv("WAYLAND_DISPLAY", socket_name, 1);

	return 0;
}

WL_EXPORT void *
wet_load_module_entrypoint(const char *name, const char *entrypoint)
{
	const char *builddir = getenv("WESTON_BUILD_DIR");
	char path[PATH_MAX];
	void *module, *init;
	size_t len;

	if (name == NULL)
		return NULL;

	if (name[0] != '/') {
		if (builddir)
			len = snprintf(path, sizeof path, "%s/.libs/%s", builddir,
				       name);
		else
			len = snprintf(path, sizeof path, "%s/%s", MODULEDIR,
				       name);
	} else {
		len = snprintf(path, sizeof path, "%s", name);
	}

	/* snprintf returns the length of the string it would've written,
	 * _excluding_ the NUL byte. So even being equal to the size of
	 * our buffer is an error here. */
	if (len >= sizeof path)
		return NULL;

	module = dlopen(path, RTLD_NOW | RTLD_NOLOAD);
	if (module) {
		weston_log("Module '%s' already loaded\n", path);
		dlclose(module);
		return NULL;
	}

	weston_log("Loading module '%s'\n", path);
	module = dlopen(path, RTLD_NOW);
	if (!module) {
		weston_log("Failed to load module: %s\n", dlerror());
		return NULL;
	}

	init = dlsym(module, entrypoint);
	if (!init) {
		weston_log("Failed to lookup init function: %s\n", dlerror());
		dlclose(module);
		return NULL;
	}

	return init;
}


WL_EXPORT int
wet_load_module(struct weston_compositor *compositor,
	        const char *name, int *argc, char *argv[])
{
	int (*module_init)(struct weston_compositor *ec,
			   int *argc, char *argv[]);

	module_init = wet_load_module_entrypoint(name, "wet_module_init");
	if (!module_init)
		return -1;
	if (module_init(compositor, argc, argv) < 0)
		return -1;
	return 0;
}

static int
wet_load_shell(struct weston_compositor *compositor,
	       const char *name, int *argc, char *argv[])
{
	int (*shell_init)(struct weston_compositor *ec,
			  int *argc, char *argv[]);

	shell_init = wet_load_module_entrypoint(name, "wet_shell_init");
	if (!shell_init)
		return -1;
	if (shell_init(compositor, argc, argv) < 0)
		return -1;
	return 0;
}

static int
load_modules(struct weston_compositor *ec, const char *modules,
	     int *argc, char *argv[], int32_t *xwayland)
{
	const char *p, *end;
	char buffer[256];

	if (modules == NULL)
		return 0;

	p = modules;
	while (*p) {
		end = strchrnul(p, ',');
		snprintf(buffer, sizeof buffer, "%.*s", (int) (end - p), p);

		if (strstr(buffer, "xwayland.so")) {
			weston_log("Old Xwayland module loading detected: "
				   "Please use --xwayland command line option "
				   "or set xwayland=true in the [core] section "
				   "in weston.ini\n");
			*xwayland = 1;
		} else {
			if (wet_load_module(ec, buffer, argc, argv) < 0)
				return -1;
		}

		p = end;
		while (*p == ',')
			p++;
	}

	return 0;
}

static int
weston_compositor_init_config(struct weston_compositor *ec,
			      struct weston_config *config)
{
	struct xkb_rule_names xkb_names;
	struct weston_config_section *s;
	int repaint_msec;
	int vt_switching;

	s = weston_config_get_section(config, "keyboard", NULL, NULL);
	weston_config_section_get_string(s, "keymap_rules",
					 (char **) &xkb_names.rules, NULL);
	weston_config_section_get_string(s, "keymap_model",
					 (char **) &xkb_names.model, NULL);
	weston_config_section_get_string(s, "keymap_layout",
					 (char **) &xkb_names.layout, NULL);
	weston_config_section_get_string(s, "keymap_variant",
					 (char **) &xkb_names.variant, NULL);
	weston_config_section_get_string(s, "keymap_options",
					 (char **) &xkb_names.options, NULL);

	if (weston_compositor_set_xkb_rule_names(ec, &xkb_names) < 0)
		return -1;

	weston_config_section_get_int(s, "repeat-rate",
				      &ec->kb_repeat_rate, 40);
	weston_config_section_get_int(s, "repeat-delay",
				      &ec->kb_repeat_delay, 400);

	weston_config_section_get_bool(s, "vt-switching",
				       &vt_switching, true);
	ec->vt_switching = vt_switching;

	s = weston_config_get_section(config, "core", NULL, NULL);
	weston_config_section_get_int(s, "repaint-window", &repaint_msec,
				      ec->repaint_msec);
	if (repaint_msec < -10 || repaint_msec > 1000) {
		weston_log("Invalid repaint_window value in config: %d\n",
			   repaint_msec);
	} else {
		ec->repaint_msec = repaint_msec;
	}
	weston_log("Output repaint window is %d ms maximum.\n",
		   ec->repaint_msec);

	return 0;
}

static char *
weston_choose_default_backend(void)
{
	char *backend = NULL;

	if (getenv("WAYLAND_DISPLAY") || getenv("WAYLAND_SOCKET"))
		backend = strdup("wayland-backend.so");
	else if (getenv("DISPLAY"))
		backend = strdup("x11-backend.so");
	else
		backend = strdup(WESTON_NATIVE_BACKEND);

	return backend;
}

static const struct { const char *name; uint32_t token; } transforms[] = {
	{ "normal",     WL_OUTPUT_TRANSFORM_NORMAL },
	{ "90",         WL_OUTPUT_TRANSFORM_90 },
	{ "180",        WL_OUTPUT_TRANSFORM_180 },
	{ "270",        WL_OUTPUT_TRANSFORM_270 },
	{ "flipped",    WL_OUTPUT_TRANSFORM_FLIPPED },
	{ "flipped-90", WL_OUTPUT_TRANSFORM_FLIPPED_90 },
	{ "flipped-180", WL_OUTPUT_TRANSFORM_FLIPPED_180 },
	{ "flipped-270", WL_OUTPUT_TRANSFORM_FLIPPED_270 },
};

WL_EXPORT int
weston_parse_transform(const char *transform, uint32_t *out)
{
	unsigned int i;

	for (i = 0; i < ARRAY_LENGTH(transforms); i++)
		if (strcmp(transforms[i].name, transform) == 0) {
			*out = transforms[i].token;
			return 0;
		}

	*out = WL_OUTPUT_TRANSFORM_NORMAL;
	return -1;
}

WL_EXPORT const char *
weston_transform_to_string(uint32_t output_transform)
{
	unsigned int i;

	for (i = 0; i < ARRAY_LENGTH(transforms); i++)
		if (transforms[i].token == output_transform)
			return transforms[i].name;

	return "<illegal value>";
}

static int
load_configuration(struct weston_config **config, int32_t noconfig,
		   const char *config_file)
{
	const char *file = "weston.ini";
	const char *full_path;

	*config = NULL;

	if (config_file)
		file = config_file;

	if (noconfig == 0)
		*config = weston_config_parse(file);

	if (*config) {
		full_path = weston_config_get_full_path(*config);

		weston_log("Using config file '%s'\n", full_path);
		setenv(WESTON_CONFIG_FILE_ENV_VAR, full_path, 1);

		return 0;
	}

	if (config_file && noconfig == 0) {
		weston_log("fatal: error opening or reading config file"
			   " '%s'.\n", config_file);

		return -1;
	}

	weston_log("Starting with no config file.\n");
	setenv(WESTON_CONFIG_FILE_ENV_VAR, "", 1);

	return 0;
}

static void
handle_exit(struct weston_compositor *c)
{
	wl_display_terminate(c->wl_display);
}

static void
wet_output_set_scale(struct weston_output *output,
		     struct weston_config_section *section,
		     int32_t default_scale,
		     int32_t parsed_scale)
{
	int32_t scale = default_scale;

	if (section)
		weston_config_section_get_int(section, "scale", &scale, default_scale);

	if (parsed_scale)
		scale = parsed_scale;

	weston_output_set_scale(output, scale);
}

/* UINT32_MAX is treated as invalid because 0 is a valid
 * enumeration value and the parameter is unsigned
 */
static void
wet_output_set_transform(struct weston_output *output,
			 struct weston_config_section *section,
			 uint32_t default_transform,
			 uint32_t parsed_transform)
{
	char *t;
	uint32_t transform = default_transform;

	if (section) {
		weston_config_section_get_string(section,
						 "transform", &t, "normal");

		if (weston_parse_transform(t, &transform) < 0) {
			weston_log("Invalid transform \"%s\" for output %s\n",
				   t, output->name);
			transform = default_transform;
		}
		free(t);
	}

	if (parsed_transform != UINT32_MAX)
		transform = parsed_transform;

	weston_output_set_transform(output, transform);
}

static int
wet_configure_windowed_output_from_config(struct weston_output *output,
					  struct wet_output_config *defaults)
{
	const struct weston_windowed_output_api *api =
		weston_windowed_output_get_api(output->compositor);

	struct weston_config *wc = wet_get_config(output->compositor);
	struct weston_config_section *section = NULL;
	struct wet_compositor *compositor = to_wet_compositor(output->compositor);
	struct wet_output_config *parsed_options = compositor->parsed_options;
	int width = defaults->width;
	int height = defaults->height;

	assert(parsed_options);

	if (!api) {
		weston_log("Cannot use weston_windowed_output_api.\n");
		return -1;
	}

	section = weston_config_get_section(wc, "output", "name", output->name);

	if (section) {
		char *mode;

		weston_config_section_get_string(section, "mode", &mode, NULL);
		if (!mode || sscanf(mode, "%dx%d", &width,
				    &height) != 2) {
			weston_log("Invalid mode for output %s. Using defaults.\n",
				   output->name);
			width = defaults->width;
			height = defaults->height;
		}
		free(mode);
	}

	if (parsed_options->width)
		width = parsed_options->width;

	if (parsed_options->height)
		height = parsed_options->height;

	wet_output_set_scale(output, section, defaults->scale, parsed_options->scale);
	wet_output_set_transform(output, section, defaults->transform, parsed_options->transform);

	if (api->output_set_size(output, width, height) < 0) {
		weston_log("Cannot configure output \"%s\" using weston_windowed_output_api.\n",
			   output->name);
		return -1;
	}

	return 0;
}

static int
count_remaining_heads(struct weston_output *output, struct weston_head *to_go)
{
	struct weston_head *iter = NULL;
	int n = 0;

	while ((iter = weston_output_iterate_heads(output, iter))) {
		if (iter != to_go)
			n++;
	}

	return n;
}

static void
wet_head_tracker_destroy(struct wet_head_tracker *track)
{
	wl_list_remove(&track->head_destroy_listener.link);
	free(track);
}

static void
handle_head_destroy(struct wl_listener *listener, void *data)
{
	struct weston_head *head = data;
	struct weston_output *output;
	struct wet_head_tracker *track =
		container_of(listener, struct wet_head_tracker,
			     head_destroy_listener);

	wet_head_tracker_destroy(track);

	output = weston_head_get_output(head);

	/* On shutdown path, the output might be already gone. */
	if (!output)
		return;

	if (count_remaining_heads(output, head) > 0)
		return;

	weston_output_destroy(output);
}

static struct wet_head_tracker *
wet_head_tracker_from_head(struct weston_head *head)
{
	struct wl_listener *lis;

	lis = weston_head_get_destroy_listener(head, handle_head_destroy);
	if (!lis)
		return NULL;

	return container_of(lis, struct wet_head_tracker,
			    head_destroy_listener);
}

/* Listen for head destroy signal.
 *
 * If a head is destroyed and it was the last head on the output, we
 * destroy the associated output.
 *
 * Do not bother destroying the head trackers on shutdown, the backend will
 * destroy the heads which calls our handler to destroy the trackers.
 */
static void
wet_head_tracker_create(struct wet_compositor *compositor,
			struct weston_head *head)
{
	struct wet_head_tracker *track;

	track = zalloc(sizeof *track);
	if (!track)
		return;

	track->head_destroy_listener.notify = handle_head_destroy;
	weston_head_add_destroy_listener(head, &track->head_destroy_listener);
}

static void
simple_head_enable(struct wet_compositor *wet, struct weston_head *head)
{
	struct weston_output *output;
	int ret = 0;

	/* Workaround for repeated DRM backend "off" setting.
	 * For any other case, we should not have an attached head that is not
	 * enabled. */
	if (weston_head_get_output(head))
		return;

	output = weston_compositor_create_output_with_head(wet->compositor,
							   head);
	if (!output) {
		weston_log("Could not create an output for head \"%s\".\n",
			   weston_head_get_name(head));
		wet->init_failed = true;

		return;
	}

	if (wet->simple_output_configure)
		ret = wet->simple_output_configure(output);
	if (ret < 0) {
		weston_log("Cannot configure output \"%s\".\n",
			   weston_head_get_name(head));
		weston_output_destroy(output);
		wet->init_failed = true;

		return;
	}

	/* Escape hatch for DRM backend "off" setting. */
	if (ret > 0)
		return;

	if (weston_output_enable(output) < 0) {
		weston_log("Enabling output \"%s\" failed.\n",
			   weston_head_get_name(head));
		weston_output_destroy(output);
		wet->init_failed = true;

		return;
	}

	wet_head_tracker_create(wet, head);

	/* The weston_compositor will track and destroy the output on exit. */
}

static void
simple_head_disable(struct weston_head *head)
{
	struct weston_output *output;
	struct wet_head_tracker *track;

	track = wet_head_tracker_from_head(head);
	if (track)
		wet_head_tracker_destroy(track);

	output = weston_head_get_output(head);
	assert(output);
	weston_output_destroy(output);
}

static void
simple_heads_changed(struct wl_listener *listener, void *arg)
{
	struct weston_compositor *compositor = arg;
	struct wet_compositor *wet = to_wet_compositor(compositor);
	struct weston_head *head = NULL;
	bool connected;
	bool enabled;
	bool changed;

	while ((head = weston_compositor_iterate_heads(wet->compositor, head))) {
		connected = weston_head_is_connected(head);
		enabled = weston_head_is_enabled(head);
		changed = weston_head_is_device_changed(head);

		if (connected && !enabled) {
			simple_head_enable(wet, head);
		} else if (!connected && enabled) {
			simple_head_disable(head);
		} else if (enabled && changed) {
			weston_log("Detected a monitor change on head '%s', "
				   "not bothering to do anything about it.\n",
				   weston_head_get_name(head));
		}
		weston_head_reset_device_changed(head);
	}
}

static void
wet_set_simple_head_configurator(struct weston_compositor *compositor,
				 int (*fn)(struct weston_output *))
{
	struct wet_compositor *wet = to_wet_compositor(compositor);

	wet->simple_output_configure = fn;

	wet->heads_changed_listener.notify = simple_heads_changed;
	weston_compositor_add_heads_changed_listener(compositor,
						&wet->heads_changed_listener);
}

static void
configure_input_device(struct weston_compositor *compositor,
		       struct libinput_device *device)
{
	struct weston_config_section *s;
	struct weston_config *config = wet_get_config(compositor);
	int enable_tap;
	int enable_tap_default;

	s = weston_config_get_section(config,
				      "libinput", NULL, NULL);

	if (libinput_device_config_tap_get_finger_count(device) > 0) {
		enable_tap_default =
			libinput_device_config_tap_get_default_enabled(
				device);
		weston_config_section_get_bool(s, "enable_tap",
					       &enable_tap,
					       enable_tap_default);
		libinput_device_config_tap_set_enabled(device,
						       enable_tap);
	}
}

static int
drm_backend_output_configure(struct weston_output *output)
{
	struct weston_config *wc = wet_get_config(output->compositor);
	struct wet_compositor *wet = to_wet_compositor(output->compositor);
	struct weston_config_section *section;
	const struct weston_drm_output_api *api = weston_drm_output_get_api(output->compositor);
	enum weston_drm_backend_output_mode mode =
		WESTON_DRM_BACKEND_OUTPUT_PREFERRED;

	char *s;
	char *modeline = NULL;
	char *gbm_format = NULL;
	char *seat = NULL;

	if (!api) {
		weston_log("Cannot use weston_drm_output_api.\n");
		return -1;
	}

	section = weston_config_get_section(wc, "output", "name", output->name);
	weston_config_section_get_string(section, "mode", &s, "preferred");

	if (strcmp(s, "off") == 0) {
		weston_output_disable(output);
		free(s);
		return 1;
	} else if (wet->drm_use_current_mode || strcmp(s, "current") == 0) {
		mode = WESTON_DRM_BACKEND_OUTPUT_CURRENT;
	} else if (strcmp(s, "preferred") != 0) {
		modeline = s;
		s = NULL;
	}
	free(s);

	if (api->set_mode(output, mode, modeline) < 0) {
		weston_log("Cannot configure an output using weston_drm_output_api.\n");
		free(modeline);
		return -1;
	}
	free(modeline);

	wet_output_set_scale(output, section, 1, 0);
	wet_output_set_transform(output, section, WL_OUTPUT_TRANSFORM_NORMAL, UINT32_MAX);

	weston_config_section_get_string(section,
					 "gbm-format", &gbm_format, NULL);

	api->set_gbm_format(output, gbm_format);
	free(gbm_format);

	weston_config_section_get_string(section, "seat", &seat, "");

	api->set_seat(output, seat);
	free(seat);

	return 0;
}

static int
load_drm_backend(struct weston_compositor *c,
		 int *argc, char **argv, struct weston_config *wc)
{
	struct weston_drm_backend_config config = {{ 0, }};
	struct weston_config_section *section;
	struct wet_compositor *wet = to_wet_compositor(c);
	int use_shadow;
	int ret = 0;

	wet->drm_use_current_mode = false;

	const struct weston_option options[] = {
		{ WESTON_OPTION_STRING, "seat", 0, &config.seat_id },
		{ WESTON_OPTION_INTEGER, "tty", 0, &config.tty },
		{ WESTON_OPTION_STRING, "drm-device", 0, &config.specific_device },
		{ WESTON_OPTION_BOOLEAN, "current-mode", 0, &wet->drm_use_current_mode },
		{ WESTON_OPTION_BOOLEAN, "use-pixman", 0, &config.use_pixman },
	};

	parse_options(options, ARRAY_LENGTH(options), argc, argv);

	section = weston_config_get_section(wc, "core", NULL, NULL);
	weston_config_section_get_string(section,
					 "gbm-format", &config.gbm_format,
					 NULL);
	weston_config_section_get_uint(section, "pageflip-timeout",
	                               &config.pageflip_timeout, 0);
	weston_config_section_get_bool(section, "pixman-shadow", &use_shadow, 1);
	config.use_pixman_shadow = use_shadow;

	config.base.struct_version = WESTON_DRM_BACKEND_CONFIG_VERSION;
	config.base.struct_size = sizeof(struct weston_drm_backend_config);
	config.configure_device = configure_input_device;

	wet_set_simple_head_configurator(c, drm_backend_output_configure);

	ret = weston_compositor_load_backend(c, WESTON_BACKEND_DRM,
					     &config.base);

	free(config.gbm_format);
	free(config.seat_id);

	return ret;
}

static int
headless_backend_output_configure(struct weston_output *output)
{
	struct wet_output_config defaults = {
		.width = 1024,
		.height = 640,
		.scale = 1,
		.transform = WL_OUTPUT_TRANSFORM_NORMAL
	};

	return wet_configure_windowed_output_from_config(output, &defaults);
}

static int
load_headless_backend(struct weston_compositor *c,
		      int *argc, char **argv, struct weston_config *wc)
{
	const struct weston_windowed_output_api *api;
	struct weston_headless_backend_config config = {{ 0, }};
	int no_outputs = 0;
	int ret = 0;
	char *transform = NULL;

	struct wet_output_config *parsed_options = wet_init_parsed_options(c);
	if (!parsed_options)
		return -1;

	const struct weston_option options[] = {
		{ WESTON_OPTION_INTEGER, "width", 0, &parsed_options->width },
		{ WESTON_OPTION_INTEGER, "height", 0, &parsed_options->height },
		{ WESTON_OPTION_BOOLEAN, "use-pixman", 0, &config.use_pixman },
		{ WESTON_OPTION_STRING, "transform", 0, &transform },
		{ WESTON_OPTION_BOOLEAN, "no-outputs", 0, &no_outputs },
	};

	parse_options(options, ARRAY_LENGTH(options), argc, argv);

	if (transform) {
		if (weston_parse_transform(transform, &parsed_options->transform) < 0) {
			weston_log("Invalid transform \"%s\"\n", transform);
			parsed_options->transform = UINT32_MAX;
		}
		free(transform);
	}

	config.base.struct_version = WESTON_HEADLESS_BACKEND_CONFIG_VERSION;
	config.base.struct_size = sizeof(struct weston_headless_backend_config);

	wet_set_simple_head_configurator(c, headless_backend_output_configure);

	/* load the actual wayland backend and configure it */
	ret = weston_compositor_load_backend(c, WESTON_BACKEND_HEADLESS,
					     &config.base);

	if (ret < 0)
		return ret;

	if (!no_outputs) {
		api = weston_windowed_output_get_api(c);

		if (!api) {
			weston_log("Cannot use weston_windowed_output_api.\n");
			return -1;
		}

		if (api->create_head(c, "headless") < 0)
			return -1;
	}

	return 0;
}

static int
rdp_backend_output_configure(struct weston_output *output)
{
	struct wet_compositor *compositor = to_wet_compositor(output->compositor);
	struct wet_output_config *parsed_options = compositor->parsed_options;
	const struct weston_rdp_output_api *api = weston_rdp_output_get_api(output->compositor);
	int width = 640;
	int height = 480;

	assert(parsed_options);

	if (!api) {
		weston_log("Cannot use weston_rdp_output_api.\n");
		return -1;
	}

	if (parsed_options->width)
		width = parsed_options->width;

	if (parsed_options->height)
		height = parsed_options->height;

	weston_output_set_scale(output, 1);
	weston_output_set_transform(output, WL_OUTPUT_TRANSFORM_NORMAL);

	if (api->output_set_size(output, width, height) < 0) {
		weston_log("Cannot configure output \"%s\" using weston_rdp_output_api.\n",
			   output->name);
		return -1;
	}

	return 0;
}

static void
weston_rdp_backend_config_init(struct weston_rdp_backend_config *config)
{
	config->base.struct_version = WESTON_RDP_BACKEND_CONFIG_VERSION;
	config->base.struct_size = sizeof(struct weston_rdp_backend_config);

	config->bind_address = NULL;
	config->port = 3389;
	config->rdp_key = NULL;
	config->server_cert = NULL;
	config->server_key = NULL;
	config->env_socket = 0;
	config->no_clients_resize = 0;
}

static int
load_rdp_backend(struct weston_compositor *c,
		int *argc, char *argv[], struct weston_config *wc)
{
	struct weston_rdp_backend_config config  = {{ 0, }};
	int ret = 0;

	struct wet_output_config *parsed_options = wet_init_parsed_options(c);
	if (!parsed_options)
		return -1;

	weston_rdp_backend_config_init(&config);

	const struct weston_option rdp_options[] = {
		{ WESTON_OPTION_BOOLEAN, "env-socket", 0, &config.env_socket },
		{ WESTON_OPTION_INTEGER, "width", 0, &parsed_options->width },
		{ WESTON_OPTION_INTEGER, "height", 0, &parsed_options->height },
		{ WESTON_OPTION_STRING,  "address", 0, &config.bind_address },
		{ WESTON_OPTION_INTEGER, "port", 0, &config.port },
		{ WESTON_OPTION_BOOLEAN, "no-clients-resize", 0, &config.no_clients_resize },
		{ WESTON_OPTION_STRING,  "rdp4-key", 0, &config.rdp_key },
		{ WESTON_OPTION_STRING,  "rdp-tls-cert", 0, &config.server_cert },
		{ WESTON_OPTION_STRING,  "rdp-tls-key", 0, &config.server_key }
	};

	parse_options(rdp_options, ARRAY_LENGTH(rdp_options), argc, argv);

	wet_set_simple_head_configurator(c, rdp_backend_output_configure);

	ret = weston_compositor_load_backend(c, WESTON_BACKEND_RDP,
					     &config.base);

	free(config.bind_address);
	free(config.rdp_key);
	free(config.server_cert);
	free(config.server_key);

	return ret;
}

static int
fbdev_backend_output_configure(struct weston_output *output)
{
	struct weston_config *wc = wet_get_config(output->compositor);
	struct weston_config_section *section;

	section = weston_config_get_section(wc, "output", "name", "fbdev");

	wet_output_set_transform(output, section, WL_OUTPUT_TRANSFORM_NORMAL, UINT32_MAX);
	weston_output_set_scale(output, 1);

	return 0;
}

static int
load_fbdev_backend(struct weston_compositor *c,
		      int *argc, char **argv, struct weston_config *wc)
{
	struct weston_fbdev_backend_config config = {{ 0, }};
	int ret = 0;

	const struct weston_option fbdev_options[] = {
		{ WESTON_OPTION_INTEGER, "tty", 0, &config.tty },
		{ WESTON_OPTION_STRING, "device", 0, &config.device },
	};

	parse_options(fbdev_options, ARRAY_LENGTH(fbdev_options), argc, argv);

	if (!config.device)
		config.device = strdup("/dev/fb0");

	config.base.struct_version = WESTON_FBDEV_BACKEND_CONFIG_VERSION;
	config.base.struct_size = sizeof(struct weston_fbdev_backend_config);
	config.configure_device = configure_input_device;

	wet_set_simple_head_configurator(c, fbdev_backend_output_configure);

	/* load the actual wayland backend and configure it */
	ret = weston_compositor_load_backend(c, WESTON_BACKEND_FBDEV,
					     &config.base);

	free(config.device);
	return ret;
}

static int
x11_backend_output_configure(struct weston_output *output)
{
	struct wet_output_config defaults = {
		.width = 1024,
		.height = 600,
		.scale = 1,
		.transform = WL_OUTPUT_TRANSFORM_NORMAL
	};

	return wet_configure_windowed_output_from_config(output, &defaults);
}

static int
load_x11_backend(struct weston_compositor *c,
		 int *argc, char **argv, struct weston_config *wc)
{
	char *default_output;
	const struct weston_windowed_output_api *api;
	struct weston_x11_backend_config config = {{ 0, }};
	struct weston_config_section *section;
	int ret = 0;
	int option_count = 1;
	int output_count = 0;
	char const *section_name;
	int i;

	struct wet_output_config *parsed_options = wet_init_parsed_options(c);
	if (!parsed_options)
		return -1;

	const struct weston_option options[] = {
	       { WESTON_OPTION_INTEGER, "width", 0, &parsed_options->width },
	       { WESTON_OPTION_INTEGER, "height", 0, &parsed_options->height },
	       { WESTON_OPTION_INTEGER, "scale", 0, &parsed_options->scale },
	       { WESTON_OPTION_BOOLEAN, "fullscreen", 'f', &config.fullscreen },
	       { WESTON_OPTION_INTEGER, "output-count", 0, &option_count },
	       { WESTON_OPTION_BOOLEAN, "no-input", 0, &config.no_input },
	       { WESTON_OPTION_BOOLEAN, "use-pixman", 0, &config.use_pixman },
	};

	parse_options(options, ARRAY_LENGTH(options), argc, argv);

	config.base.struct_version = WESTON_X11_BACKEND_CONFIG_VERSION;
	config.base.struct_size = sizeof(struct weston_x11_backend_config);

	wet_set_simple_head_configurator(c, x11_backend_output_configure);

	/* load the actual backend and configure it */
	ret = weston_compositor_load_backend(c, WESTON_BACKEND_X11,
					     &config.base);

	if (ret < 0)
		return ret;

	api = weston_windowed_output_get_api(c);

	if (!api) {
		weston_log("Cannot use weston_windowed_output_api.\n");
		return -1;
	}

	section = NULL;
	while (weston_config_next_section(wc, &section, &section_name)) {
		char *output_name;

		if (output_count >= option_count)
			break;

		if (strcmp(section_name, "output") != 0) {
			continue;
		}

		weston_config_section_get_string(section, "name", &output_name, NULL);
		if (output_name == NULL || output_name[0] != 'X') {
			free(output_name);
			continue;
		}

		if (api->create_head(c, output_name) < 0) {
			free(output_name);
			return -1;
		}
		free(output_name);

		output_count++;
	}

	default_output = NULL;

	for (i = output_count; i < option_count; i++) {
		if (asprintf(&default_output, "screen%d", i) < 0) {
			return -1;
		}

		if (api->create_head(c, default_output) < 0) {
			free(default_output);
			return -1;
		}
		free(default_output);
	}

	return 0;
}

static int
wayland_backend_output_configure(struct weston_output *output)
{
	struct wet_output_config defaults = {
		.width = 1024,
		.height = 640,
		.scale = 1,
		.transform = WL_OUTPUT_TRANSFORM_NORMAL
	};

	return wet_configure_windowed_output_from_config(output, &defaults);
}

static int
load_wayland_backend(struct weston_compositor *c,
		     int *argc, char **argv, struct weston_config *wc)
{
	struct weston_wayland_backend_config config = {{ 0, }};
	struct weston_config_section *section;
	const struct weston_windowed_output_api *api;
	const char *section_name;
	char *output_name = NULL;
	int count = 1;
	int ret = 0;
	int i;
	int32_t use_pixman_ = 0;
	int32_t sprawl_ = 0;
	int32_t fullscreen_ = 0;

	struct wet_output_config *parsed_options = wet_init_parsed_options(c);
	if (!parsed_options)
		return -1;

	config.cursor_size = 32;
	config.cursor_theme = NULL;
	config.display_name = NULL;

	const struct weston_option wayland_options[] = {
		{ WESTON_OPTION_INTEGER, "width", 0, &parsed_options->width },
		{ WESTON_OPTION_INTEGER, "height", 0, &parsed_options->height },
		{ WESTON_OPTION_INTEGER, "scale", 0, &parsed_options->scale },
		{ WESTON_OPTION_STRING, "display", 0, &config.display_name },
		{ WESTON_OPTION_BOOLEAN, "use-pixman", 0, &use_pixman_ },
		{ WESTON_OPTION_INTEGER, "output-count", 0, &count },
		{ WESTON_OPTION_BOOLEAN, "fullscreen", 0, &fullscreen_ },
		{ WESTON_OPTION_BOOLEAN, "sprawl", 0, &sprawl_ },
	};

	parse_options(wayland_options, ARRAY_LENGTH(wayland_options), argc, argv);
	config.sprawl = sprawl_;
	config.use_pixman = use_pixman_;
	config.fullscreen = fullscreen_;

	section = weston_config_get_section(wc, "shell", NULL, NULL);
	weston_config_section_get_string(section, "cursor-theme",
					 &config.cursor_theme, NULL);
	weston_config_section_get_int(section, "cursor-size",
				      &config.cursor_size, 32);

	config.base.struct_size = sizeof(struct weston_wayland_backend_config);
	config.base.struct_version = WESTON_WAYLAND_BACKEND_CONFIG_VERSION;

	/* load the actual wayland backend and configure it */
	ret = weston_compositor_load_backend(c, WESTON_BACKEND_WAYLAND,
					     &config.base);

	free(config.cursor_theme);
	free(config.display_name);

	if (ret < 0)
		return ret;

	api = weston_windowed_output_get_api(c);

	if (api == NULL) {
		/* We will just assume if load_backend() finished cleanly and
		 * windowed_output_api is not present that wayland backend is
		 * started with --sprawl or runs on fullscreen-shell.
		 * In this case, all values are hardcoded, so nothing can be
		 * configured; simply create and enable an output. */
		wet_set_simple_head_configurator(c, NULL);

		return 0;
	}

	wet_set_simple_head_configurator(c, wayland_backend_output_configure);

	section = NULL;
	while (weston_config_next_section(wc, &section, &section_name)) {
		if (count == 0)
			break;

		if (strcmp(section_name, "output") != 0) {
			continue;
		}

		weston_config_section_get_string(section, "name", &output_name, NULL);

		if (output_name == NULL)
			continue;

		if (output_name[0] != 'W' || output_name[1] != 'L') {
			free(output_name);
			continue;
		}

		if (api->create_head(c, output_name) < 0) {
			free(output_name);
			return -1;
		}
		free(output_name);

		--count;
	}

	for (i = 0; i < count; i++) {
		if (asprintf(&output_name, "wayland%d", i) < 0)
			return -1;

		if (api->create_head(c, output_name) < 0) {
			free(output_name);
			return -1;
		}
		free(output_name);
	}

	return 0;
}


static int
load_backend(struct weston_compositor *compositor, const char *backend,
	     int *argc, char **argv, struct weston_config *config)
{
	if (strstr(backend, "headless-backend.so"))
		return load_headless_backend(compositor, argc, argv, config);
	else if (strstr(backend, "rdp-backend.so"))
		return load_rdp_backend(compositor, argc, argv, config);
	else if (strstr(backend, "fbdev-backend.so"))
		return load_fbdev_backend(compositor, argc, argv, config);
	else if (strstr(backend, "drm-backend.so"))
		return load_drm_backend(compositor, argc, argv, config);
	else if (strstr(backend, "x11-backend.so"))
		return load_x11_backend(compositor, argc, argv, config);
	else if (strstr(backend, "wayland-backend.so"))
		return load_wayland_backend(compositor, argc, argv, config);

	weston_log("Error: unknown backend \"%s\"\n", backend);
	return -1;
}

static char *
copy_command_line(int argc, char * const argv[])
{
	FILE *fp;
	char *str = NULL;
	size_t size = 0;
	int i;

	fp = open_memstream(&str, &size);
	if (!fp)
		return NULL;

	fprintf(fp, "%s", argv[0]);
	for (i = 1; i < argc; i++)
		fprintf(fp, " %s", argv[i]);
	fclose(fp);

	return str;
}

int main(int argc, char *argv[])
{
	int ret = EXIT_FAILURE;
	char *cmdline;
	struct wl_display *display;
	struct wl_event_source *signals[4];
	struct wl_event_loop *loop;
	int i, fd;
	char *backend = NULL;
	char *shell = NULL;
	int32_t xwayland = 0;
	char *modules = NULL;
	char *option_modules = NULL;
	char *log = NULL;
	char *server_socket = NULL;
	int32_t idle_time = -1;
	int32_t help = 0;
	char *socket_name = NULL;
	int32_t version = 0;
	int32_t noconfig = 0;
	int32_t numlock_on;
	char *config_file = NULL;
	struct weston_config *config = NULL;
	struct weston_config_section *section;
	struct wl_client *primary_client;
	struct wl_listener primary_client_destroyed;
	struct weston_seat *seat;
	struct wet_compositor wet = { 0 };
	int require_input;
	int32_t wait_for_debugger = 0;

	const struct weston_option core_options[] = {
		{ WESTON_OPTION_STRING, "backend", 'B', &backend },
		{ WESTON_OPTION_STRING, "shell", 0, &shell },
		{ WESTON_OPTION_STRING, "socket", 'S', &socket_name },
		{ WESTON_OPTION_INTEGER, "idle-time", 'i', &idle_time },
		{ WESTON_OPTION_BOOLEAN, "xwayland", 0, &xwayland },
		{ WESTON_OPTION_STRING, "modules", 0, &option_modules },
		{ WESTON_OPTION_STRING, "log", 0, &log },
		{ WESTON_OPTION_BOOLEAN, "help", 'h', &help },
		{ WESTON_OPTION_BOOLEAN, "version", 0, &version },
		{ WESTON_OPTION_BOOLEAN, "no-config", 0, &noconfig },
		{ WESTON_OPTION_STRING, "config", 'c', &config_file },
		{ WESTON_OPTION_BOOLEAN, "wait-for-debugger", 0, &wait_for_debugger },
	};

	if (os_fd_set_cloexec(fileno(stdin))) {
		printf("Unable to set stdin as close on exec().\n");
		return EXIT_FAILURE;
	}

	cmdline = copy_command_line(argc, argv);
	parse_options(core_options, ARRAY_LENGTH(core_options), &argc, argv);

	if (help) {
		free(cmdline);
		usage(EXIT_SUCCESS);
	}

	if (version) {
		printf(PACKAGE_STRING "\n");
		free(cmdline);

		return EXIT_SUCCESS;
	}

	weston_log_set_handler(vlog, vlog_continue);
	weston_log_file_open(log);

	weston_log("%s\n"
		   STAMP_SPACE "%s\n"
		   STAMP_SPACE "Bug reports to: %s\n"
		   STAMP_SPACE "Build: %s\n",
		   PACKAGE_STRING, PACKAGE_URL, PACKAGE_BUGREPORT,
		   BUILD_ID);
	weston_log("Command line: %s\n", cmdline);
	free(cmdline);
	log_uname();

	verify_xdg_runtime_dir();

	display = wl_display_create();

	loop = wl_display_get_event_loop(display);
	signals[0] = wl_event_loop_add_signal(loop, SIGTERM, on_term_signal,
					      display);
	signals[1] = wl_event_loop_add_signal(loop, SIGINT, on_term_signal,
					      display);
	signals[2] = wl_event_loop_add_signal(loop, SIGQUIT, on_term_signal,
					      display);

	wl_list_init(&child_process_list);
	signals[3] = wl_event_loop_add_signal(loop, SIGCHLD, sigchld_handler,
					      NULL);

	if (!signals[0] || !signals[1] || !signals[2] || !signals[3])
		goto out_signals;

	if (load_configuration(&config, noconfig, config_file) < 0)
		goto out_signals;
	wet.config = config;
	wet.parsed_options = NULL;

	section = weston_config_get_section(config, "core", NULL, NULL);

	if (!wait_for_debugger)
		weston_config_section_get_bool(section, "wait-for-debugger",
					       &wait_for_debugger, 0);
	if (wait_for_debugger) {
		weston_log("Weston PID is %ld - "
			   "waiting for debugger, send SIGCONT to continue...\n",
			   (long)getpid());
		raise(SIGSTOP);
	}

	if (!backend) {
		weston_config_section_get_string(section, "backend", &backend,
						 NULL);
		if (!backend)
			backend = weston_choose_default_backend();
	}

	wet.compositor = weston_compositor_create(display, &wet);
	if (wet.compositor == NULL) {
		weston_log("fatal: failed to create compositor\n");
		goto out;
	}
	segv_compositor = wet.compositor;

	if (weston_compositor_init_config(wet.compositor, config) < 0)
		goto out;

	weston_config_section_get_bool(section, "require-input",
				       &require_input, true);
	wet.compositor->require_input = require_input;

	/*
	 * Block sigusr1 in case xwayland is to be loaded. That way,
	 * any thread called by an external library or within the
	 * compositor does not terminate the entire session.
	 */
	if (!xwayland)
		weston_config_section_get_bool(section, "xwayland", &xwayland,
					       false);
	if ( xwayland )
		block_signal( SIGUSR1 );

	if (load_backend(wet.compositor, backend, &argc, argv, config) < 0) {
		weston_log("fatal: failed to create compositor backend\n");
		goto out;
	}

	weston_compositor_flush_heads_changed(wet.compositor);
	if (wet.init_failed)
		goto out;

	if (idle_time < 0)
		weston_config_section_get_int(section, "idle-time", &idle_time, -1);
	if (idle_time < 0)
		idle_time = 300; /* default idle timeout, in seconds */

	wet.compositor->idle_time = idle_time;
	wet.compositor->default_pointer_grab = NULL;
	wet.compositor->exit = handle_exit;

	weston_compositor_log_capabilities(wet.compositor);

	server_socket = getenv("WAYLAND_SERVER_SOCKET");
	if (server_socket) {
		weston_log("Running with single client\n");
		if (!safe_strtoint(server_socket, &fd))
			fd = -1;
	} else {
		fd = -1;
	}

	if (fd != -1) {
		primary_client = wl_client_create(display, fd);
		if (!primary_client) {
			weston_log("fatal: failed to add client: %m\n");
			goto out;
		}
		primary_client_destroyed.notify =
			handle_primary_client_destroyed;
		wl_client_add_destroy_listener(primary_client,
					       &primary_client_destroyed);
	} else if (weston_create_listening_socket(display, socket_name)) {
		goto out;
	}

	if (!shell)
		weston_config_section_get_string(section, "shell", &shell,
						 "desktop-shell.so");

	if (wet_load_shell(wet.compositor, shell, &argc, argv) < 0)
		goto out;

	weston_config_section_get_string(section, "modules", &modules, "");
	if (load_modules(wet.compositor, modules, &argc, argv, &xwayland) < 0)
		goto out;

	if (load_modules(wet.compositor, option_modules, &argc, argv, &xwayland) < 0)
		goto out;

	if (!xwayland)
		weston_config_section_get_bool(section, "xwayland", &xwayland,
					       false);
	if (xwayland) {
		if (wet_load_xwayland(wet.compositor) < 0)
			goto out;
	}

	section = weston_config_get_section(config, "keyboard", NULL, NULL);
	weston_config_section_get_bool(section, "numlock-on", &numlock_on, 0);
	if (numlock_on) {
		wl_list_for_each(seat, &wet.compositor->seat_list, link) {
			struct weston_keyboard *keyboard =
				weston_seat_get_keyboard(seat);

			if (keyboard)
				weston_keyboard_set_locks(keyboard,
							  WESTON_NUM_LOCK,
							  WESTON_NUM_LOCK);
		}
	}

	for (i = 1; i < argc; i++)
		weston_log("fatal: unhandled option: %s\n", argv[i]);
	if (argc > 1)
		goto out;

	weston_compositor_wake(wet.compositor);

	wl_display_run(display);

	/* Allow for setting return exit code after
	* wl_display_run returns normally. This is
	* useful for devs/testers and automated tests
	* that want to indicate failure status to
	* testing infrastructure above
	*/
	ret = wet.compositor->exit_code;

out:
	/* free(NULL) is valid, and it won't be NULL if it's used */
	free(wet.parsed_options);

	weston_compositor_destroy(wet.compositor);

out_signals:
	for (i = ARRAY_LENGTH(signals) - 1; i >= 0; i--)
		if (signals[i])
			wl_event_source_remove(signals[i]);

	wl_display_destroy(display);

	weston_log_file_close();

	if (config)
		weston_config_destroy(config);
	free(config_file);
	free(backend);
	free(shell);
	free(socket_name);
	free(option_modules);
	free(log);
	free(modules);

	return ret;
}
