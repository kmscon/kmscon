/*
 * Tests for the owner detection and the execute check of the pty, used to lock
 * terminals. Only /bin/sh is needed, so they also run in build sandboxes.
 */

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "../src/misc/pty.c"

static char tmpdir[] = "/tmp/kmscon-test-pty-XXXXXX";

static void fail(const char *what)
{
	perror(what);
	exit(EXIT_FAILURE);
}

/* Start /bin/sh as @name with @script. It blocks reading stdin until the
 * returned pipe is closed. */
static pid_t spawn(const char *name, const char *script, int *wr)
{
	int fds[2];
	pid_t pid;

	if (pipe(fds))
		fail("pipe");
	pid = fork();
	if (pid < 0)
		fail("fork");
	if (!pid) {
		dup2(fds[0], STDIN_FILENO);
		close(fds[0]);
		close(fds[1]);
		execl(name, "sh", "-c", script, (char *)NULL);
		_exit(127);
	}
	close(fds[0]);
	*wr = fds[1];
	return pid;
}

static void stop(pid_t pid, int wr)
{
	close(wr);
	kill(pid, SIGKILL);
	waitpid(pid, NULL, 0);
}

/* Wait until @pid runs @comm and, if @child is set, has a child */
static bool wait_for(pid_t pid, const char *comm, bool child)
{
	struct timespec ts = {0, 10 * 1000 * 1000};
	char path[64], buf[32];
	FILE *f;
	int i;

	for (i = 0; i < 200; ++i) {
		snprintf(path, sizeof(path), "/proc/%d/comm", pid);
		f = fopen(path, "re");
		if (f) {
			if (!fgets(buf, sizeof(buf), f))
				buf[0] = 0;
			fclose(f);
			buf[strcspn(buf, "\n")] = 0;
			if (!strcmp(buf, comm) && (!child || proc_find_child(pid)))
				return true;
		}
		nanosleep(&ts, NULL);
	}
	return false;
}

static bool has_proc_children(void)
{
	char path[64];

	snprintf(path, sizeof(path), "/proc/%d/task/%d/children", getpid(), getpid());
	return !access(path, R_OK);
}

static void test_get_owner(void)
{
	struct kmscon_pty pty = {.fd = -1};
	char login[sizeof(tmpdir) + 8];
	uid_t uid = 12345;
	bool ready;
	pid_t pid;
	int ret, wr;

	/* invalid arguments and closed pty */
	assert(kmscon_pty_get_owner(NULL, &uid) == -EINVAL);
	assert(kmscon_pty_get_owner(&pty, NULL) == -EINVAL);
	assert(kmscon_pty_get_owner(&pty, &uid) == -ENOENT);

	pty.fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
	if (pty.fd < 0)
		fail("open");
	assert(kmscon_pty_get_owner(&pty, &uid) == -ENOENT);

	/* a program other than login is logged in as soon as it runs */
	pid = spawn("/bin/sh", "read x", &wr);
	ready = wait_for(pid, "sh", false);
	assert(ready);
	pty.child = pid;
	uid = 12345;
	ret = kmscon_pty_get_owner(&pty, &uid);
	assert(ret == 0 && uid == getuid());
	stop(pid, wr);

	/* login without a child: the login prompt, nobody is logged in */
	snprintf(login, sizeof(login), "%s/login", tmpdir);
	if (symlink("/bin/sh", login))
		fail("symlink");
	pid = spawn(login, "read x", &wr);
	ready = wait_for(pid, "login", false);
	assert(ready);
	pty.child = pid;
	assert(kmscon_pty_get_owner(&pty, &uid) == -ENOENT);
	stop(pid, wr);

	/* login with a child: the child runs as the logged in user */
	if (has_proc_children()) {
		pid = spawn(login, "(read x); :", &wr);
		ready = wait_for(pid, "login", true);
		assert(ready);
		pty.child = pid;
		uid = 12345;
		ret = kmscon_pty_get_owner(&pty, &uid);
		assert(ret == 0 && uid == getuid());
		stop(pid, wr);
	} else {
		printf("skipped login with a child: no CONFIG_PROC_CHILDREN\n");
	}

	/* the child exited */
	pid = spawn("/bin/sh", ":", &wr);
	stop(pid, wr);
	pty.child = pid;
	assert(kmscon_pty_get_owner(&pty, &uid) < 0);

	unlink(login);
	close(pty.fd);
}

static void make_file(const char *path, mode_t mode)
{
	int fd;

	unlink(path);
	fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	if (fd < 0)
		fail("open");
	close(fd);
	if (chmod(path, mode))
		fail("chmod");
}

/* Check @path as the user @uid, or as the test itself if @uid is negative */
static bool can_exec(const char *path, long uid)
{
	char *argv[] = {(char *)path, NULL};
	struct kmscon_pty pty = {.fd = -1, .argv = path ? argv : NULL};
	bool ret;

	if (uid >= 0 && kmscon_pty_set_user(&pty, uid))
		fail("kmscon_pty_set_user");
	ret = kmscon_pty_can_exec(&pty);

	free(pty.user);
	free(pty.home);
	free(pty.shell);
	free(pty.groups);
	return ret;
}

/* Check @path as the test's own user, with @home as its home directory */
static bool can_exec_from(const char *home, const char *path)
{
	char *argv[] = {(char *)path, NULL};
	struct kmscon_pty pty = {.fd = -1, .argv = argv};
	bool ret;

	if (kmscon_pty_set_user(&pty, getuid()))
		fail("kmscon_pty_set_user");
	free(pty.home);
	pty.home = strdup(home);
	if (!pty.home)
		fail("strdup");
	ret = kmscon_pty_can_exec(&pty);

	free(pty.user);
	free(pty.home);
	free(pty.shell);
	free(pty.groups);
	return ret;
}

static void test_can_exec(void)
{
	char dir[sizeof(tmpdir) + 8], file[sizeof(tmpdir) + 16];
	struct passwd *nobody;

	snprintf(dir, sizeof(dir), "%s/dir", tmpdir);
	snprintf(file, sizeof(file), "%s/cmd", dir);
	if (mkdir(dir, 0700))
		fail("mkdir");

	/* no program, missing file, directory */
	assert(!kmscon_pty_can_exec(NULL));
	assert(!can_exec(NULL, -1));
	assert(!can_exec(file, -1));
	assert(!can_exec(dir, -1));

	if (getuid() == 0) {
		/* root: any execute bit */
		make_file(file, 0644);
		assert(!can_exec(file, 0));
		make_file(file, 0601);
		assert(can_exec(file, 0));

		/* another user: checked as that user, directories included */
		nobody = getpwnam("nobody");
		if (nobody && !chmod(tmpdir, 0711) && !chmod(dir, 0711)) {
			make_file(file, 0755);
			if (can_exec(file, nobody->pw_uid)) {
				make_file(file, 0751);
				assert(!can_exec(file, nobody->pw_uid));
				chmod(dir, 0700);
				make_file(file, 0755);
				assert(!can_exec(file, nobody->pw_uid));
			} else {
				printf("skipped other users: cannot switch to \"nobody\"\n");
			}
		}
	} else {
		/* the test's own user: no switch, as with kmscon under libseat */
		make_file(file, 0500);
		assert(can_exec(file, getuid()));
		make_file(file, 0100);
		assert(!can_exec(file, getuid()));
		make_file(file, 0077);
		assert(!can_exec(file, getuid()));

		/* a directory on the way cannot be searched */
		make_file(file, 0700);
		if (chmod(dir, 0600))
			fail("chmod");
		assert(!can_exec(file, getuid()));
		if (chmod(dir, 0700))
			fail("chmod");
	}

	/* the program is looked up from the user's home, like when it runs */
	make_file(file, 0700);
	assert(can_exec_from(dir, "cmd"));
	assert(!can_exec_from(tmpdir, "cmd"));

	unlink(file);
	rmdir(dir);
}

static void test_set_user(void)
{
	struct kmscon_pty pty = {.fd = -1};
	struct passwd *pw;
	int ret;

	assert(kmscon_pty_set_user(NULL, 0) == -EINVAL);

	/* without root, only the test's own user is possible */
	if (getuid() != 0)
		assert(kmscon_pty_set_user(&pty, 0) == -EPERM);

	pw = getpwuid(getuid());
	if (!pw)
		fail("getpwuid");
	ret = kmscon_pty_set_user(&pty, getuid());
	assert(ret == 0);
	assert(!strcmp(pty.user, pw->pw_name));
	assert(pty.uid == pw->pw_uid);
	assert(pty.gid == pw->pw_gid);
	assert(pty.home && pty.shell);
	assert(pty.ngroups >= 1 && pty.groups[0] == pw->pw_gid);

	free(pty.user);
	free(pty.home);
	free(pty.shell);
	free(pty.groups);
}

int main(void)
{
	if (!mkdtemp(tmpdir))
		fail("mkdtemp");

	test_get_owner();
	test_can_exec();
	test_set_user();

	rmdir(tmpdir);
	return 0;
}
