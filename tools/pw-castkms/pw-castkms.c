// SPDX-License-Identifier: GPL-2.0-only

#include "pw-castkms.h"

#include <errno.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../virtualscreen-edid.h"

#define REFERENCE_EDID_MAX_SIZE (4 * CASTKMS_EDID_BLOCK)

struct options {
	const char *device_path;
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
		"Usage: %s [-d /dev/dri/cardN] [OPTIONS]\n"
		"\n"
		"  -d, --device PATH     CastKMS primary node\n"
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
		{ "device", required_argument, NULL, 'd' },
		{ "crtc", required_argument, NULL, 'c' },
		{ "edid", required_argument, NULL, 'e' },
		{ "name", required_argument, NULL, 'n' },
		{ "help", no_argument, NULL, 'h' },
		{},
	};
	int option;

	*options = (struct options) {};

	while ((option = getopt_long(argc, argv, "d:c:e:n:h",
				     long_options, NULL)) != -1) {
		switch (option) {
		case 'd':
			options->device_path = optarg;
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

	if (!size || size > REFERENCE_EDID_MAX_SIZE ||
	    size % CASTKMS_EDID_BLOCK) {
		fprintf(stderr,
			"%s: EDID must be a 128-byte multiple up to %u bytes\n",
			path, REFERENCE_EDID_MAX_SIZE);
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

	generated = malloc(CASTKMS_EDID_BLOCK);
	if (!generated)
		return -ENOMEM;
	result = castkms_fill_named_edid(generated, options->monitor_name);
	if (result < 0) {
		fprintf(stderr, "%s\n", options->monitor_name ?
			"monitor name must be at most 13 characters" :
			"failed to build default output EDID");
		free(generated);
		return result;
	}

	*data = generated;
	*size = CASTKMS_EDID_BLOCK;
	return 0;
}
