/*
 * Make the cache directories the icon services expect, then copy the stderr
 * of iconservicesagent and backboardd to the console after launchd has had
 * time to try them.  Started by init before exec launchd; unlike a launchd
 * job, a static helper exec'd by init is allowed.
 */
#include "mini.h"

static void
check_path(const char *path)
{
	struct stat st;

	if (stat(path, &st) == 0) {
		printf("exists: %s mode=%o owner=%u:%u size=%lld\n", path, st.st_mode, st.st_uid, st.st_gid,
		    (long long)st.st_size);
	} else {
		printf("missing: %s: %s\n", path, strerror(errno));
	}
}

/*
 * The agents start within the first seconds of launchd, before bcatch's first
 * report.  Make the caches they expect now, so they exist no matter how far
 * init's var.skel walk got; the skeleton creates the same directories later
 * (and mkdir's EEXIST there is fine).
 */
static void
ensure_dir(const char *path, int mode, int uid, int gid)
{
	int made = mkdir(path, mode) == 0;

	if (!made && errno != 17 /* EEXIST */) {
		printf("bcatch: mkdir %s: %s\n", path, strerror(errno));
		return;
	}
	if (chmod(path, mode) != 0 || chown(path, uid, gid) != 0) {
		printf("bcatch: own %s: %s\n", path, strerror(errno));
		return;
	}
	if (made) {
		printf("bcatch: created %s mode=%o owner=%u:%u\n", path, mode, uid, gid);
	}
}

/* One synthetic swipe up from the bottom, to get past Setup's "Swipe up to open"
 * while there is no real touch driver for this panel. */
static void
swipe_once(void)
{
	struct stat st;
	pid_t pid;

	if (stat("/redwood/bin/rwtouch", &st) != 0) {
		return;
	}
	pid = fork();
	if (pid == 0) {
		char *argv[] = { "/redwood/bin/rwtouch", "wait", "5", "swipe", "0.5", "0.99", "0.5", "0.4", "250", NULL };

		execve(argv[0], argv, environ);
		_exit(127);
	}
}

/* The var_store restore can put back the Setup Assistant's own state; put
 * the skip-setup preferences back after it, before launchd starts. */
static void
copy_prefs(void)
{
	static const char *const names[] = {
		"com.apple.purplebuddy.plist",
		"com.apple.purplebuddy.budd.plist",
		"com.apple.SetupAssistant.plist",
		"com.apple.springboard.plist",
	};
	static const char prefix[] = "/redwood/var/mobile/Library/Preferences/";
	static const char target[] = "/private/var/mobile/Library/Preferences/";
	char from[256], to[256];
	char buf[1024];
	ssize_t n;
	int in, out;

	for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		strcpy(from, prefix);
		strcpy(from + sizeof(prefix) - 1, names[i]);
		strcpy(to, target);
		strcpy(to + sizeof(target) - 1, names[i]);
		in = open(from, O_RDONLY, 0);
		if (in < 0) {
			printf("bcatch: prefs open %s: %s\n", from, strerror(errno));
			continue;
		}
		out = open(to, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (out < 0) {
			printf("bcatch: prefs create %s: %s\n", to, strerror(errno));
			close(in);
			continue;
		}
		while ((n = read(in, buf, sizeof(buf))) > 0) {
			write(out, buf, (size_t)n);
		}
		close(in);
		close(out);
		chmod(to, 0644);
		chown(to, 501, 501);
	}
}

/* make sure a job's stderr file exists before launchd starts it */
static void
ensure_file(const char *path)
{
	int fd = open(path, O_WRONLY | O_CREAT, 0644);

	if (fd < 0) {
		printf("bcatch: create %s: %s\n", path, strerror(errno));
	} else {
		close(fd);
	}
}

static void
dump_tail(const char *path, off_t max)
{
	char buf[1024];
	struct stat st;
	off_t end, start;
	ssize_t n;
	int fd;

	if (stat(path, &st) != 0 || st.st_size == 0) {
		return;
	}
	fd = open(path, O_RDONLY, 0);
	if (fd < 0) {
		printf("tail %s: %s\n", path, strerror(errno));
		return;
	}
	end = lseek(fd, 0, SEEK_END);
	start = end > max ? end - max : 0;
	if (lseek(fd, start, SEEK_SET) < 0) {
		close(fd);
		return;
	}
	printf("\n=== %s (last %lld bytes) ===\n", path, (long long)(end - start));
	while ((n = read(fd, buf, sizeof(buf))) > 0) {
		write(1, buf, (size_t)n);
	}
	close(fd);
	printf("\n=== end %s ===\n", path);
}

static void
dump_hex(const char *path, off_t at, int len)
{
	unsigned char buf[32];
	int fd, n;

	if (len > (int)sizeof(buf)) {
		len = (int)sizeof(buf);
	}
	fd = open(path, O_RDONLY, 0);
	if (fd < 0) {
		printf("hex %s +0x%llx: %s\n", path, (long long)at, strerror(errno));
		return;
	}
	if (lseek(fd, at, SEEK_SET) < 0) {
		printf("hex %s +0x%llx: %s\n", path, (long long)at, strerror(errno));
		close(fd);
		return;
	}
	n = read(fd, buf, len);
	printf("hex %s +0x%llx:", path, (long long)at);
	for (int i = 0; i < n; i++) {
		printf(" %02x", buf[i]);
	}
	printf("\n");
	close(fd);
}

/* print a small stderr file whole (up to max bytes) */
static void
dump_raw(const char *path, int max)
{
	char buf[1024];
	struct stat st;
	ssize_t n;
	int in, total = 0;

	if (stat(path, &st) != 0 || st.st_size == 0) {
		return;
	}
	in = open(path, O_RDONLY, 0);
	if (in < 0) {
		return;
	}
	printf("\n=== %s (%lld bytes) ===\n", path, (long long)st.st_size);
	while (total < max && (n = read(in, buf, sizeof(buf))) > 0) {
		if (n > max - total) {
			n = max - total;
		}
		write(1, buf, (size_t)n);
		total += (int)n;
	}
	close(in);
	printf("\n=== end %s ===\n", path);
}

int
main(int argc, char **argv, char **envp)
{
	int out;

	(void)argc;
	(void)argv;
	(void)envp;

	/* before launchd starts the agents, not just before this report */
	ensure_dir("/private/var/mobile", 0711, 501, 501);
	ensure_dir("/private/var/mobile/Library", 0711, 501, 501);
	ensure_dir("/private/var/mobile/Library/Caches", 0700, 501, 501);
	ensure_dir("/private/var/mobile/Library/Caches/com.apple.iconservicesagent", 0700, 501, 501);
	ensure_dir("/private/var/mobile/Library/Caches/com.apple.metal", 0700, 501, 501);
	ensure_dir("/private/var/mobile/Library/Caches/com.apple.metalfe", 0700, 501, 501);
	ensure_dir("/private/var/mobile/Library/Preferences", 0755, 501, 501);
	copy_prefs();
	ensure_file("/private/var/backboardd.err");
	ensure_file("/private/var/iconservicesagent.err");
	ensure_file("/private/var/SpringBoard.err");

	/* launchd + backboardd + SpringBoard need a few seconds to try */
	usleep(30 * 1000 * 1000);
	/* swipe_once(); */

	out = open("/dev/console", O_WRONLY, 0);
	if (out < 0) {
		return 1;
	}
	dup2(out, 1);
	dup2(out, 2);
	if (out > 2) {
		close(out);
	}

	for (;;) {
		printf("\n=== icon cache paths ===\n");
		check_path("/private/var/containers/Shared/SystemGroup");
		check_path("/private/var/containers/Shared/SystemGroup/systemgroup.com.apple.lsd.iconscache");
		check_path("/private/var/mobile/Library/Caches");
		check_path("/private/var/mobile/Library/Caches/com.apple.iconservicesagent");
		check_path("/private/var/mobile/Library/Caches/com.apple.metal");
		check_path("/private/var/mobile/Library/Caches/com.apple.metalfe");
		check_path("/private/var/backboardd.err");
		check_path("/private/var/iconservicesagent.err");
		check_path("/private/var/SpringBoard.err");
		check_path("/usr/libexec/xpcproxy");
		dump_hex("/usr/libexec/xpcproxy", 0x4228, 8);
		dump_hex("/usr/libexec/xpcproxy", 0x4240, 8);
		dump_hex("/usr/libexec/xpcproxy", 0x6914, 12);
		dump_hex("/usr/libexec/xpcproxy", 0x6920, 12);

		dump_tail("/private/var/iconservicesagent.err", 2048);
		dump_tail("/private/var/backboardd.err", 2048);
		dump_tail("/private/var/SpringBoard.err", 4096);
		printf("\n=== bcatch: end (repeats in 15 s) ===\n");
		usleep(15 * 1000 * 1000);
	}
}
