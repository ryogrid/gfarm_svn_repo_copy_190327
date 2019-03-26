/*
 * $Id: gfmkdir.c 2054 2005-08-23 04:38:55Z soda $
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libgen.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <gfarm/gfarm.h>
#include "gfs_client.h"

char *program_name = "gfmkdir";

static void
usage()
{
	fprintf(stderr, "Usage: %s directory...\n", program_name);
	exit(1);
}

int
main(int argc, char **argv)
{
	char *e;
	int i, c, err = 0;
	extern int optind;

	if (argc <= 1)
		usage();
	program_name = basename(argv[0]);

	while ((c = getopt(argc, argv, "h?")) != EOF) {
		switch (c) {
		case 'h':
		case '?':
		default:
			usage();
		}
	}
	argc -= optind;
	argv += optind;

	e = gfarm_initialize(&argc, &argv);
	if (e != NULL) {
		fprintf(stderr, "%s: %s\n", program_name, e);
		exit(1);
	}
	for (i = 0; i < argc; i++) {
		e = gfs_mkdir(argv[i], 0755);
		if (e != NULL) {
			fprintf(stderr, "%s: %s: %s\n",
				program_name, argv[i], e);
			err++;
		}
	}
	e = gfarm_terminate();
	if (e != NULL) {
		fprintf(stderr, "%s: %s\n", program_name, e);
		err++;
	}
	return (err > 0);
}
