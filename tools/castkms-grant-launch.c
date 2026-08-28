// SPDX-License-Identifier: GPL-2.0-only

#include <drm/castkms_drm.h>

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include "castkms-test-drm.h"

static_assert(sizeof(struct drm_castkms_create_grant) == 32,
	      "create-grant ABI size changed");
static_assert(offsetof(struct drm_castkms_create_grant, control_fd) == 24,
	      "create-grant control-fd offset changed");
static_assert(offsetof(struct drm_castkms_create_grant, reserved) == 28,
	      "create-grant reserved offset changed");

static volatile sig_atomic_t child_pid = -1;

static void forward_signal(int signal_number)
{
	pid_t pid = child_pid;

	if (pid > 0)
		kill(pid, signal_number);
}

static int parse_connector_id(const char *value, uint32_t *connector_id)
{
	char *end = NULL;
	unsigned long parsed;

	errno = 0;
	parsed = strtoul(value, &end, 0);
	if (errno || !*value || !end || *end || !parsed ||
	    parsed > UINT32_MAX)
		return -1;
	*connector_id = (uint32_t)parsed;
	return 0;
}

static int drop_implicit_master(int fd)
{
	if (ioctl(fd, DRM_IOCTL_DROP_MASTER, 0) < 0 && errno != EINVAL) {
		perror("DRM_IOCTL_DROP_MASTER");
		return -1;
	}
	return 0;
}

static void usage(const char *program)
{
	fprintf(stderr,
		"usage: %s [--capture-only] DRM-DEVICE CONNECTOR-ID -- COMMAND [ARG...]\n",
		program);
}

int main(int argc, char **argv)
{
	uint32_t rights = DRM_CASTKMS_GRANT_CAPTURE_PIXELS |
		DRM_CASTKMS_GRANT_MANAGE_ATTACHMENT |
		DRM_CASTKMS_GRANT_UPDATE_EDID |
		DRM_CASTKMS_GRANT_READ_CURSOR |
		DRM_CASTKMS_GRANT_MANAGE_CEC;
	struct drm_castkms_create_grant create = {
		.flags = DRM_CASTKMS_GRANT_CREATE_ADMIN,
		.fd = -1,
		.fd_flags = O_NONBLOCK,
		.control_fd = -1,
	};
	struct sigaction action = {
		.sa_handler = forward_signal,
	};
	char fd_string[32];
	pid_t waited;
	int issuer_fd;
	int status;
	int argument = 1;

	if (argc == 2 && (!strcmp(argv[1], "-h") ||
			  !strcmp(argv[1], "--help"))) {
		usage(argv[0]);
		return EXIT_SUCCESS;
	}
	if (argument < argc && !strcmp(argv[argument], "--capture-only")) {
		rights = DRM_CASTKMS_GRANT_CAPTURE_PIXELS |
			 DRM_CASTKMS_GRANT_READ_CURSOR;
		argument++;
	}
	if (argc - argument < 4 || strcmp(argv[argument + 2], "--") ||
	    parse_connector_id(argv[argument + 1], &create.connector_id)) {
		usage(argv[0]);
		return EXIT_FAILURE;
	}
	create.rights = rights;

	issuer_fd = open(argv[argument], O_RDWR | O_CLOEXEC | O_NONBLOCK);
	if (issuer_fd < 0) {
		perror("open DRM device");
		return EXIT_FAILURE;
	}
	if (castkms_test_check_driver_name(issuer_fd))
		goto fail;

	if (ioctl(issuer_fd, DRM_IOCTL_CASTKMS_CREATE_GRANT, &create) < 0) {
		perror("DRM_IOCTL_CASTKMS_CREATE_GRANT (admin)");
		goto fail;
	}
	if (create.fd < 0 || create.control_fd < 0 ||
	    create.fd == create.control_fd || !create.grant_id ||
	    !(fcntl(create.fd, F_GETFD) & FD_CLOEXEC) ||
	    !(fcntl(create.control_fd, F_GETFD) & FD_CLOEXEC)) {
		fprintf(stderr, "CREATE_GRANT returned invalid outputs\n");
		goto fail_grant;
	}
	/* A primary-node open is implicitly master when no compositor is active. */
	if (drop_implicit_master(issuer_fd))
		goto fail_grant;

	sigemptyset(&action.sa_mask);
	action.sa_flags = SA_RESTART;
	if (sigaction(SIGINT, &action, NULL) ||
	    sigaction(SIGTERM, &action, NULL) ||
	    sigaction(SIGHUP, &action, NULL)) {
		perror("sigaction");
		goto fail_grant;
	}

	child_pid = fork();
	if (child_pid < 0) {
		perror("fork");
		goto fail_grant;
	}
	if (child_pid == 0) {
		close(issuer_fd);
		close(create.control_fd);
		if (fcntl(create.fd, F_SETFD, 0) < 0) {
			perror("make grant fd inheritable");
			_exit(EXIT_FAILURE);
		}
		snprintf(fd_string, sizeof(fd_string), "%d", create.fd);
		if (setenv("CASTKMS_GRANT_FD", fd_string, 1) < 0) {
			perror("setenv CASTKMS_GRANT_FD");
			_exit(EXIT_FAILURE);
		}
		execvp(argv[argument + 3], &argv[argument + 3]);
		perror("execvp");
		_exit(errno == ENOENT ? 127 : 126);
	}

	close(create.fd);
	create.fd = -1;
	do {
		waited = waitpid(child_pid, &status, 0);
	} while (waited < 0 && errno == EINTR);
	child_pid = -1;

	close(create.control_fd);
	create.control_fd = -1;
	close(issuer_fd);

	if (waited < 0) {
		perror("waitpid");
		return EXIT_FAILURE;
	}
	if (WIFEXITED(status))
		return WEXITSTATUS(status);
	if (WIFSIGNALED(status))
		return 128 + WTERMSIG(status);
	return EXIT_FAILURE;

fail_grant:
	if (create.fd >= 0)
		close(create.fd);
	if (create.control_fd >= 0)
		close(create.control_fd);
fail:
	close(issuer_fd);
	return EXIT_FAILURE;
}
