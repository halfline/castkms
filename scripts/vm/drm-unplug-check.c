// SPDX-License-Identifier: GPL-2.0-only

#include <drm/drm.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static int wait_for_release(void)
{
	char byte;
	ssize_t ret;

	do {
		ret = read(STDIN_FILENO, &byte, sizeof(byte));
	} while (ret < 0 && errno == EINTR);

	if (ret != sizeof(byte)) {
		fprintf(stderr, "release gate failed: ret=%zd errno=%d\n", ret,
			errno);
		return -1;
	}

	return 0;
}

int main(int argc, char **argv)
{
	struct drm_version version = {};
	char name[32] = {};
	char byte;
	ssize_t read_ret;
	int debugfs_fd;
	int drm_fd;
	int ioctl_ret;
	int saved_errno;

	if (argc != 3) {
		fprintf(stderr, "usage: %s DRM-DEVICE DEBUGFS-FILE\n", argv[0]);
		return EXIT_FAILURE;
	}

	drm_fd = open(argv[1], O_RDWR | O_CLOEXEC);
	if (drm_fd < 0) {
		perror("open DRM device");
		return EXIT_FAILURE;
	}

	debugfs_fd = open(argv[2], O_RDONLY | O_CLOEXEC);
	if (debugfs_fd < 0) {
		perror("open debugfs file");
		close(drm_fd);
		return EXIT_FAILURE;
	}

	version.name = name;
	version.name_len = sizeof(name) - 1;
	if (ioctl(drm_fd, DRM_IOCTL_VERSION, &version) < 0) {
		perror("initial DRM_IOCTL_VERSION");
		return EXIT_FAILURE;
	}

	if (version.name_len != strlen("castkms") ||
	    memcmp(name, "castkms", strlen("castkms"))) {
		fprintf(stderr, "unexpected DRM driver: %.*s\n",
			(int)version.name_len, name);
		return EXIT_FAILURE;
	}

	printf("ready=castkms\n");
	fflush(stdout);

	if (wait_for_release())
		return EXIT_FAILURE;

	memset(&version, 0, sizeof(version));
	errno = 0;
	ioctl_ret = ioctl(drm_fd, DRM_IOCTL_VERSION, &version);
	saved_errno = errno;
	if (ioctl_ret != -1 || saved_errno != ENODEV) {
		fprintf(stderr,
			"post-unplug DRM_IOCTL_VERSION: ret=%d errno=%d\n",
			ioctl_ret, saved_errno);
		return EXIT_FAILURE;
	}
	printf("drm_ioctl_after_unplug=ENODEV\n");

	errno = 0;
	read_ret = read(debugfs_fd, &byte, sizeof(byte));
	saved_errno = errno;
	/* debugfs rejects reads from removed entries before seq_file runs. */
	if (read_ret != -1 || saved_errno != EIO) {
		fprintf(stderr,
			"post-unplug debugfs read: ret=%zd errno=%d\n",
			read_ret, saved_errno);
		return EXIT_FAILURE;
	}
	printf("debugfs_read_after_unplug=EIO\n");

	close(debugfs_fd);
	close(drm_fd);

	return EXIT_SUCCESS;
}
