#include <stdio.h>
#include <stdlib.h>

#include <gfarm/gfarm.h>

#define BUFSIZE 256

int
main(int argc, char **argv)
{
	gfarm_error_t e;
	GFS_File gf;
	char buf[BUFSIZE];
	int eof;

	e = gfarm_initialize(&argc, &argv);
	if (e != GFARM_ERR_NO_ERROR) {
		fprintf(stderr, "gfarm_initialize: %s\n",
		    gfarm_error_string(e));
		return (2);
	}

	if (argc != 3) {
		fprintf(stderr, "Usage: putline <gfarm_url> <string>\n");
		return (2);
	}
	e = gfs_pio_create(argv[1],
			   GFARM_FILE_RDWR|GFARM_FILE_TRUNC, 0666, &gf);
	if (e != GFARM_ERR_NO_ERROR) {
		fprintf(stderr, "gfs_pio_create: %s\n",
		    gfarm_error_string(e));
		return (2);
	}
	e = gfs_pio_putline(gf, argv[2]);
	if (e != GFARM_ERR_NO_ERROR) {
		fprintf(stderr, "%s\n", gfarm_error_string(e));
		return (1);
	}
	e = gfs_pio_close(gf);
	if (e != GFARM_ERR_NO_ERROR) {
		fprintf(stderr, "gfs_pio_close(1st): %s\n",
		    gfarm_error_string(e));
		return (2);
	}
	e = gfs_pio_open(argv[1], GFARM_FILE_RDONLY, &gf);
	if (e != GFARM_ERR_NO_ERROR) {
		fprintf(stderr, "gfs_pio_open: %s\n",
		    gfarm_error_string(e));
		return (2);
	}
	e = gfs_pio_getline(gf, buf, sizeof buf, &eof);
	if (e != GFARM_ERR_NO_ERROR) {
		fprintf(stderr, "gfs_pio_getline: %s\n",
			gfarm_error_string(e));
		return (2);
	}
	printf("%s", buf);
	e = gfs_pio_close(gf);
	if (e != GFARM_ERR_NO_ERROR) {
		fprintf(stderr, "gfs_pio_close(2nd): %s\n",
		    gfarm_error_string(e));
		return (2);
	}

	e = gfarm_terminate();
	if (e != GFARM_ERR_NO_ERROR) {
		fprintf(stderr, "gfarm_terminate: %s\n",
		    gfarm_error_string(e));
		return (2);
	}
	return (0);
}
