/*
 * ctrlaltdel.c - Set the function of the Ctrl-Alt-Del combination
 * Created 4-Jul-92 by Peter Orbaek <poe@daimi.aau.dk>
 * 1999-02-22 Arkadiusz Miśkiewicz <misiek@pld.ORG.PL>
 * - added Native Language Support
 */

#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "linux_reboot.h"
#include "nls.h"
#include "c.h"
#include "closestream.h"
#include "pathnames.h"

static void __attribute__ ((__noreturn__)) usage(FILE * out)
{
	fprintf(out, USAGE_HEADER);
	fprintf(out, _(" %s hard|soft\n"), program_invocation_short_name);

	fprintf(out, USAGE_SEPARATOR);
	fprintf(out, _("Set the function of the Ctrl-Alt-Del combination.\n"));

	fprintf(out, USAGE_OPTIONS);
	fprintf(out, USAGE_HELP);
	fprintf(out, USAGE_VERSION);
	fprintf(out, USAGE_MAN_TAIL("ctrlaltdel(8)"));
	exit(out == stderr ? EXIT_FAILURE : EXIT_SUCCESS);
}

static int get_cad(void)
{
	FILE *fp;
	int val;

	if (!(fp = fopen(_PATH_PROC_CTRL_ALT_DEL, "r"))) {
		warn("%s", _PATH_PROC_CTRL_ALT_DEL);
		return EXIT_FAILURE;
	}
	if (fscanf(fp, "%d", &val) != 1)
		val = -1;
	fclose(fp);
	switch (val) {
	case 0:
		fputs("soft\n", stdout);
		break;
	case 1:
		fputs("hard\n", stdout);
		break;
	default:
		printf("%s hard\n", _("implicit"));
		warnx(_("unexpected value in %s: %d"), _PATH_PROC_CTRL_ALT_DEL, val);
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}

static int set_cad(const char *arg)
{
	unsigned int cmd;

	if (geteuid()) {
		warnx(_("You must be root to set the Ctrl-Alt-Del behavior"));
		return EXIT_FAILURE;
	}
	if (!strcmp("hard", arg))
		cmd = LINUX_REBOOT_CMD_CAD_ON;
	else if (!strcmp("soft", arg))
		cmd = LINUX_REBOOT_CMD_CAD_OFF;
	else {
		warnx(_("unknown argument: %s"), arg);
		return EXIT_FAILURE;
	}
	if (my_reboot(cmd) < 0) {
		warnx("reboot");
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}

int main(int argc, char **argv)
{
	int ch, ret;
	static const struct option longopts[] = {
		{"version", no_argument, NULL, 'V'},
		{"help", no_argument, NULL, 'h'},
		{NULL, 0, NULL, 0}
	};

	setlocale(LC_ALL, "");
	bindtextdomain(PACKAGE, LOCALEDIR);
	textdomain(PACKAGE);
	atexit(close_stdout);

	while ((ch = getopt_long(argc, argv, "Vh", longopts, NULL)) != -1)
		switch (ch) {
		case 'V':
			printf(UTIL_LINUX_VERSION);
			return EXIT_SUCCESS;
		case 'h':
			usage(stdout);
		default:
			usage(stderr);
		}

	if (argc < 2)
		ret = get_cad();
	else
		ret = set_cad(argv[1]);
	return ret;
}
