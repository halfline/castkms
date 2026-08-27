// SPDX-License-Identifier: GPL-2.0-only

#include "pw-castkms.h"

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../virtualscreen-edid.h"

#define INPUT_EDID_MAX_SIZE (4 * CASTKMS_EDID_BLOCK)

struct options {
	int grant_fd;
	int pipewire_fd;
	bool allow_unrestricted_pipewire;
	const char *card_label;
	uint32_t preferred_crtc;
	const char *edid_path;
	const char *monitor_name;
};

void pw_castkms_fail(struct pw_castkms *bridge, const char *operation,
		     int status)
{
	if (!bridge->failed) {
		if (status) {
			fprintf(stderr, "%s: %s\n", operation,
				strerror(status < 0 ? -status : status));
		} else {
			fprintf(stderr, "%s\n", operation);
		}
	}

	bridge->failed = true;
	bridge->exit_status = EXIT_FAILURE;
	if (bridge->loop)
		pw_main_loop_quit(bridge->loop);
}

static int parse_fd(const char *value, int *fd)
{
	char *end = NULL;
	long parsed;

	if (!value || !*value)
		return -EINVAL;
	errno = 0;
	parsed = strtol(value, &end, 10);
	if (errno || end == value || *end || parsed < 0 || parsed > INT_MAX)
		return -EINVAL;

	*fd = (int)parsed;
	return 0;
}

static int parse_object_id(const char *value, uint32_t *id)
{
	char *end = NULL;
	unsigned long parsed;

	if (!value || !*value)
		return -EINVAL;
	errno = 0;
	parsed = strtoul(value, &end, 0);
	if (errno || end == value || *end || !parsed || parsed > UINT32_MAX)
		return -EINVAL;

	*id = (uint32_t)parsed;
	return 0;
}

static void usage(const char *program)
{
	fprintf(stderr,
		"Usage: %s -g GRANT-FD [-p PIPEWIRE-FD | -U] [OPTIONS]\n"
		"\n"
		"  -g, --grant-fd FD     inherited CastKMS 0.9 grant fd\n"
		"                         (or CASTKMS_GRANT_FD)\n"
		"  -p, --pipewire-fd FD  restricted PipeWire socket fd\n"
		"                         (or PIPEWIRE_REMOTE_FD)\n"
		"  -U, --allow-unrestricted-pipewire\n"
		"                         use the global development daemon\n"
		"  -d, --device PATH     issuer-provided primary-node label\n"
		"  -c, --crtc ID         require this compatible CRTC\n"
		"  -e, --edid FILE       attach with a binary EDID\n"
		"  -n, --name NAME       generate an EDID with this monitor name\n"
		"                         (at most 13 characters)\n",
		program);
}

/* Return 1 for --help, 0 for success, and a negative errno on failure. */
static int parse_options(int argc, char **argv, struct options *options)
{
	static const struct option long_options[] = {
		{ "grant-fd", required_argument, NULL, 'g' },
		{ "pipewire-fd", required_argument, NULL, 'p' },
		{ "allow-unrestricted-pipewire", no_argument, NULL, 'U' },
		{ "device", required_argument, NULL, 'd' },
		{ "crtc", required_argument, NULL, 'c' },
		{ "edid", required_argument, NULL, 'e' },
		{ "name", required_argument, NULL, 'n' },
		{ "help", no_argument, NULL, 'h' },
		{},
	};
	const char *environment_fd;
	int option;

	*options = (struct options) {
		.grant_fd = -1,
		.pipewire_fd = -1,
	};

	while ((option = getopt_long(argc, argv, "g:p:Ud:c:e:n:h",
				     long_options, NULL)) != -1) {
		switch (option) {
		case 'g':
			if (parse_fd(optarg, &options->grant_fd)) {
				fprintf(stderr, "invalid grant fd: %s\n", optarg);
				return -EINVAL;
			}
			break;
		case 'p':
			if (parse_fd(optarg, &options->pipewire_fd)) {
				fprintf(stderr, "invalid PipeWire fd: %s\n", optarg);
				return -EINVAL;
			}
			break;
		case 'U':
			options->allow_unrestricted_pipewire = true;
			break;
		case 'd':
			options->card_label = optarg;
			break;
		case 'c':
			if (parse_object_id(optarg, &options->preferred_crtc)) {
				fprintf(stderr, "invalid CRTC ID: %s\n", optarg);
				return -EINVAL;
			}
			break;
		case 'e':
			options->edid_path = optarg;
			break;
		case 'n':
			options->monitor_name = optarg;
			break;
		case 'h':
			usage(argv[0]);
			return 1;
		default:
			usage(argv[0]);
			return -EINVAL;
		}
	}

	if (optind != argc) {
		usage(argv[0]);
		return -EINVAL;
	}

	if (options->grant_fd < 0) {
		environment_fd = getenv("CASTKMS_GRANT_FD");
		if (environment_fd &&
		    parse_fd(environment_fd, &options->grant_fd)) {
			fprintf(stderr, "invalid CASTKMS_GRANT_FD\n");
			return -EINVAL;
		}
	}
	if (options->pipewire_fd < 0) {
		environment_fd = getenv("PIPEWIRE_REMOTE_FD");
		if (environment_fd &&
		    parse_fd(environment_fd, &options->pipewire_fd)) {
			fprintf(stderr, "invalid PIPEWIRE_REMOTE_FD\n");
			return -EINVAL;
		}
	}

	if (options->grant_fd < 0) {
		fprintf(stderr,
			"a CastKMS grant fd is required; ordinary card fds are not authorized\n");
		return -EACCES;
	}
	if (options->pipewire_fd < 0 &&
	    !options->allow_unrestricted_pipewire) {
		fprintf(stderr,
			"a restricted PipeWire fd is required (use -U only for isolated development)\n");
		return -EACCES;
	}
	if (options->edid_path && options->monitor_name) {
		fprintf(stderr, "-e and -n are mutually exclusive\n");
		return -EINVAL;
	}

	return 0;
}

static int read_edid_file(const char *path, uint8_t **data,
			  uint32_t *size_out)
{
	FILE *file;
	uint8_t *contents;
	long size;

	file = fopen(path, "rb");
	if (!file) {
		perror(path);
		return -errno;
	}
	if (fseek(file, 0, SEEK_END)) {
		perror(path);
		fclose(file);
		return -EIO;
	}
	size = ftell(file);
	if (size < 0) {
		perror(path);
		fclose(file);
		return -EIO;
	}
	rewind(file);

	if (!size || size > INPUT_EDID_MAX_SIZE ||
	    size % CASTKMS_EDID_BLOCK) {
		fprintf(stderr,
			"%s: EDID must be a 128-byte multiple up to %u bytes\n",
			path, INPUT_EDID_MAX_SIZE);
		fclose(file);
		return -EINVAL;
	}

	contents = malloc((size_t)size);
	if (!contents) {
		fclose(file);
		return -ENOMEM;
	}
	if (fread(contents, 1, (size_t)size, file) != (size_t)size) {
		fprintf(stderr, "%s: short EDID read\n", path);
		free(contents);
		fclose(file);
		return -EIO;
	}
	fclose(file);

	*data = contents;
	*size_out = (uint32_t)size;
	return 0;
}

static int build_output_edid(const struct options *options,
			     uint8_t **data, uint32_t *size)
{
	uint8_t *generated;
	int result;

	if (options->edid_path)
		return read_edid_file(options->edid_path, data, size);

	generated = malloc(CASTKMS_REFERENCE_EDID_MAX_SIZE);
	if (!generated)
		return -ENOMEM;
	result = castkms_fill_edid(generated, CASTKMS_REFERENCE_EDID_MAX_SIZE,
				  options->monitor_name,
				  CASTKMS_EDID_FLAG_AUDIO);
	if (result < 0) {
		fprintf(stderr, "%s\n", options->monitor_name ?
			"monitor name must be at most 13 characters" :
			"failed to build default output EDID");
		free(generated);
		return result;
	}

	*data = generated;
	*size = (uint32_t)result;
	return 0;
}

static int duplicate_pipewire_fd(struct pw_castkms *bridge, int inherited_fd)
{
	if (inherited_fd < 0) {
		fprintf(stderr,
			"warning: publishing to unrestricted PipeWire (development only)\n");
		return 0;
	}

	bridge->pipewire_fd = fcntl(inherited_fd, F_DUPFD_CLOEXEC, 3);
	if (bridge->pipewire_fd < 0) {
		perror("duplicate PipeWire fd");
		return -errno;
	}

	return 0;
}

int main(int argc, char **argv)
{
	struct pw_castkms bridge = {
		.grant_fd = -1,
		.pipewire_fd = -1,
		.exit_status = EXIT_FAILURE,
	};
	struct options options;
	uint8_t *edid = NULL;
	uint32_t edid_size = 0;
	bool pipewire_initialized = false;
	int status;

	status = parse_options(argc, argv, &options);
	if (status)
		return status > 0 ? EXIT_SUCCESS : EXIT_FAILURE;
	bridge.allow_unrestricted_pipewire =
		options.allow_unrestricted_pipewire;

	/* 1. Adopt and validate the issuer-provided holder fd. */
	status = castkms_open_grant(&bridge, options.grant_fd,
				    options.card_label);
	if (status)
		goto out;
	status = duplicate_pipewire_fd(&bridge, options.pipewire_fd);
	if (status)
		goto out;

	/* 2. Attach the granted output and wait for the compositor's mode. */
	status = build_output_edid(&options, &edid, &edid_size);
	if (status)
		goto out;
	status = castkms_configure_output(&bridge, options.preferred_crtc,
					  edid, edid_size);
	if (status)
		goto out;

	/* 3. Start capture before PipeWire asks us to allocate destinations. */
	status = castkms_start_capture(&bridge);
	if (status)
		goto out;

	/* 4. Publish holder-allocated DMA-BUFs and drive the event loop. */
	pw_init(&argc, &argv);
	pipewire_initialized = true;
	status = pipewire_open(&bridge);
	if (!status)
		(void)pipewire_run(&bridge);

out:
	/* Stop first so remove_buffer never unregisters a queued destination. */
	(void)castkms_stop_capture(&bridge);
	pipewire_close(&bridge);
	if (pipewire_initialized)
		pw_deinit();
	castkms_close(&bridge);
	free(edid);
	return bridge.exit_status;
}
