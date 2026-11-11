/*
 * Diagnostic launcher for backboardd.
 *
 * The stock plist starts /usr/libexec/backboardd directly; launchd only says
 * "Service only ran for 0 seconds" when it exits immediately.  Run it through
 * this tiny static program so dyld library loading and the exec error are on
 * the console.
 */
#include "mini.h"

int
main(int argc, char **argv, char **envp)
{
	(void)argc;
	(void)argv;
	(void)envp;
	char *bb_argv[] = { "/usr/libexec/backboardd", NULL };
	char *bb_envp[] = {
		"DYLD_PRINT_LIBRARIES=1",
		"DYLD_PRINT_TO_FILE=/dev/console",
		"PATH=/usr/bin:/bin:/usr/sbin:/sbin",
		"HOME=/",
		"OS_ACTIVITY_DT_MODE=YES",
		NULL
	};

	printf("\n=== backboardd wrapper: starting /usr/libexec/backboardd ===\n");
	execve(bb_argv[0], bb_argv, bb_envp);
	printf("backboardd wrapper: execve failed: %s\n", strerror(errno));
	return 1;
}
