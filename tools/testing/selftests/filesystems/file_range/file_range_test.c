// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE

/* Needed by the tools copy of linux/fanotify.h. */
typedef struct {
	int val[2];
} __kernel_fsid_t;
#define __kernel_fsid_t __kernel_fsid_t

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <linux/fs.h>
#include <limits.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/fanotify.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "kselftest_harness.h"

#define FILE_SIZE	(64 * 1024)
#define EVENT_BUF_SIZE	8192

#define __cleanup(func) __attribute__((__cleanup__(func)))
#define DEFINE_FREE(_name, _type, _free) \
	static inline void __free_##_name(void *p) \
	{ _type _T = *(typeof(_T) *)p; _free; }
#define __free(_name) __cleanup(__free_##_name)

struct copy_result {
	ssize_t ret;
	int error;
	int done;
};

struct io_counters {
	unsigned long long rchar;
	unsigned long long wchar;
	unsigned long long syscr;
	unsigned long long syscw;
};

static void close_fd(int fd)
{
	if (fd >= 0)
		close(fd);
}

static void unlink_path(const char *path)
{
	if (path)
		unlink(path);
}

static void unmap_copy_result(struct copy_result *result)
{
	if (result != MAP_FAILED)
		munmap(result, sizeof(*result));
}

DEFINE_FREE(close_fd, int, close_fd(_T))
DEFINE_FREE(free_line, char *, free(_T))
DEFINE_FREE(unlink_path, const char *, unlink_path(_T))
DEFINE_FREE(unmap_copy_result, struct copy_result *, unmap_copy_result(_T))

static long long monotonic_msec(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

FIXTURE(file_range) {
	char root[PATH_MAX];
	bool tmpfs_mounted;
	bool overlay_mounted;
};

static int make_path(char *path, size_t size, const char *root,
		     const char *name)
{
	int len = snprintf(path, size, "%s/%s", root, name);

	return len < 0 || (size_t)len >= size ? -ENAMETOOLONG : 0;
}

static int create_pattern_file(const char *path, unsigned char value,
			       size_t size)
{
	unsigned char buffer[4096];
	size_t written = 0;

	memset(buffer, value, sizeof(buffer));
	int fd __free(close_fd) =
		open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);

	if (fd < 0)
		return -errno;
	while (written < size) {
		size_t count = size - written;
		ssize_t ret;

		if (count > sizeof(buffer))
			count = sizeof(buffer);
		do {
			ret = write(fd, buffer, count);
		} while (ret < 0 && errno == EINTR);
		if (ret <= 0) {
			int error = ret < 0 ? errno : EIO;

			return -error;
		}
		written += ret;
	}
	if (close(fd)) {
		int error = errno;

		fd = -1;
		return -error;
	}
	fd = -1;
	return 0;
}

static int create_empty_file(const char *path)
{
	int fd __free(close_fd) =
		open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);

	if (fd < 0)
		return -errno;
	if (close(fd)) {
		int error = errno;

		fd = -1;
		return -error;
	}
	fd = -1;
	return 0;
}

static int compare_files(const char *left, const char *right)
{
	unsigned char a[4096], b[4096];

	int left_fd __free(close_fd) = open(left, O_RDONLY | O_CLOEXEC);

	if (left_fd < 0)
		return -errno;

	int right_fd __free(close_fd) = open(right, O_RDONLY | O_CLOEXEC);

	if (right_fd < 0)
		return -errno;
	for (;;) {
		ssize_t na, nb;

		do {
			na = read(left_fd, a, sizeof(a));
		} while (na < 0 && errno == EINTR);
		if (na < 0)
			return -errno;
		do {
			nb = read(right_fd, b, sizeof(b));
		} while (nb < 0 && errno == EINTR);
		if (nb < 0)
			return -errno;
		if (na != nb || (na && memcmp(a, b, na)))
			return 1;
		if (!na)
			return 0;
	}
}

static void cleanup_fixture(struct _test_data_file_range *self)
{
	char merged[PATH_MAX];

	if (self->overlay_mounted) {
		if (!make_path(merged, sizeof(merged), self->root, "merged"))
			umount2(merged, MNT_DETACH);
		self->overlay_mounted = false;
	}
	if (self->tmpfs_mounted) {
		umount2(self->root, MNT_DETACH);
		self->tmpfs_mounted = false;
	}
	if (self->root[0]) {
		rmdir(self->root);
		self->root[0] = '\0';
	}
}

static void cleanup_fixture_owner(struct _test_data_file_range *self)
{
	if (self)
		cleanup_fixture(self);
}

DEFINE_FREE(file_range_fixture, struct _test_data_file_range *,
	    cleanup_fixture_owner(_T))

static int setup_fixture(struct _test_data_file_range *self)
{
	char lower[PATH_MAX], upper[PATH_MAX], work[PATH_MAX];
	char merged[PATH_MAX], options[4 * PATH_MAX];
	char lower_data[PATH_MAX], lower_race[PATH_MAX];
	char plain[PATH_MAX];
	int ret;
	struct _test_data_file_range *fixture
		__free(file_range_fixture) = self;

	strcpy(self->root, "/tmp/file_range.XXXXXX");
	if (!mkdtemp(self->root))
		return -errno;
	if (mount("tmpfs", self->root, "tmpfs", MS_NODEV | MS_NOSUID,
		  "mode=0700"))
		return -errno;
	self->tmpfs_mounted = true;

	if (make_path(lower, sizeof(lower), self->root, "lower") ||
	    make_path(upper, sizeof(upper), self->root, "upper") ||
	    make_path(work, sizeof(work), self->root, "work") ||
	    make_path(merged, sizeof(merged), self->root, "merged") ||
	    make_path(lower_data, sizeof(lower_data), self->root,
		      "lower/data") ||
	    make_path(lower_race, sizeof(lower_race), self->root,
		      "lower/race") ||
	    make_path(plain, sizeof(plain), self->root, "plain"))
		return -ENAMETOOLONG;
	if (mkdir(lower, 0700) || mkdir(upper, 0700) || mkdir(work, 0700) ||
	    mkdir(merged, 0700))
		return -errno;
	ret = create_pattern_file(lower_data, 0x61, FILE_SIZE);
	if (ret)
		return ret;
	ret = create_pattern_file(lower_race, 0x72, FILE_SIZE);
	if (ret)
		return ret;
	ret = create_pattern_file(plain, 0x62, FILE_SIZE);
	if (ret)
		return ret;

	ret = snprintf(options, sizeof(options),
		       "lowerdir=%s,upperdir=%s,workdir=%s",
		       lower, upper, work);
	if (ret < 0 || (size_t)ret >= sizeof(options))
		return -ENAMETOOLONG;
	if (mount("overlay", merged, "overlay", 0, options))
		return -errno;
	self->overlay_mounted = true;
	fixture = NULL;
	return 0;
}

static int probe_backing_copy_file_range(struct _test_data_file_range *self)
{
	char logical_data[PATH_MAX], probe[PATH_MAX];
	loff_t pos_in = 0, pos_out = 0;
	int ret;

	ret = make_path(logical_data, sizeof(logical_data), self->root,
			"merged/data");
	if (ret)
		return ret;
	ret = make_path(probe, sizeof(probe), self->root, "probe");
	if (ret)
		return ret;
	const char *probe_path __free(unlink_path) = probe;

	int source_fd __free(close_fd) = open(logical_data, O_RDONLY | O_CLOEXEC);

	if (source_fd < 0)
		return -errno;

	int destination_fd __free(close_fd) =
		open(probe, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);

	if (destination_fd < 0)
		return -errno;

	ssize_t copied = copy_file_range(source_fd, &pos_in, destination_fd,
					 &pos_out, FILE_SIZE, 0);

	if (copied < 0)
		return -errno;
	return copied == FILE_SIZE ? 0 : -EIO;
}

FIXTURE_SETUP(file_range)
{
	int ret;

	if (geteuid())
		SKIP(return, "requires root");
	if (unshare(CLONE_NEWNS)) {
		if (errno == EPERM)
			SKIP(return, "requires CAP_SYS_ADMIN for a mount namespace");
		ASSERT_EQ(0, -errno) TH_LOG("unshare mount namespace: %m");
	}
	ASSERT_EQ(0, mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL))
		TH_LOG("make mount namespace private: %m");

	ret = setup_fixture(self);
	if (ret == -ENODEV || ret == -EOPNOTSUPP || ret == -EINVAL ||
	    ret == -EPERM || ret == -EACCES)
		SKIP(return, "tmpfs or overlayfs is unavailable: %s",
		     strerror(-ret));
	ASSERT_EQ(0, ret)
		TH_LOG("set up file range fixture: %s", strerror(-ret));

	/* Older kernels reject this overlay-to-backing route with EXDEV. */
	ret = probe_backing_copy_file_range(self);
	if (ret == -EXDEV) {
		cleanup_fixture(self);
		SKIP(return, "backing-file copy_file_range is not supported");
	}
	if (ret)
		cleanup_fixture(self);
	ASSERT_EQ(0, ret)
		TH_LOG("probe backing-file copy_file_range: %s", strerror(-ret));
}

FIXTURE_TEARDOWN(file_range)
{
	cleanup_fixture(self);
}

static int read_proc_io(pid_t pid, struct io_counters *counters)
{
	char path[64];
	size_t size = 0;
	unsigned int found = 0;
	FILE *file;

	char *line __free(free_line) = NULL;

	snprintf(path, sizeof(path), "/proc/%d/io", pid);
	file = fopen(path, "re");
	if (!file)
		return -errno;
	while (getline(&line, &size, file) >= 0) {
		unsigned long long value;

		if (sscanf(line, "rchar: %llu", &value) == 1) {
			counters->rchar = value;
			found |= 1U << 0;
		} else if (sscanf(line, "wchar: %llu", &value) == 1) {
			counters->wchar = value;
			found |= 1U << 1;
		} else if (sscanf(line, "syscr: %llu", &value) == 1) {
			counters->syscr = value;
			found |= 1U << 2;
		} else if (sscanf(line, "syscw: %llu", &value) == 1) {
			counters->syscw = value;
			found |= 1U << 3;
		}
	}
	if (fclose(file))
		return -errno;
	return found == 0xf ? 0 : -EIO;
}

static int wait_stopped(pid_t pid, int timeout_ms)
{
	long long deadline = monotonic_msec() + timeout_ms;
	int status;

	for (;;) {
		pid_t waited = waitpid(pid, &status, WNOHANG | WUNTRACED);

		if (waited == pid)
			return WIFSTOPPED(status) ? 0 : -ECHILD;
		if (waited < 0)
			return -errno;
		if (monotonic_msec() >= deadline)
			return -ETIMEDOUT;
		usleep(10000);
	}
}

static void kill_and_reap(pid_t pid)
{
	if (pid <= 0)
		return;
	kill(pid, SIGKILL);
	while (waitpid(pid, NULL, 0) < 0 && errno == EINTR)
		;
}

DEFINE_FREE(reap_child, pid_t, kill_and_reap(_T))

static int run_accounted_copy(const char *source, const char *destination,
			      struct io_counters *delta,
			      struct copy_result *result)
{
	struct io_counters before = {}, after = {};
	struct stat statbuf;

	struct copy_result *shared __free(unmap_copy_result) =
		mmap(NULL, sizeof(*shared), PROT_READ | PROT_WRITE,
		     MAP_SHARED | MAP_ANONYMOUS, -1, 0);

	if (shared == MAP_FAILED)
		return -errno;
	memset(shared, 0, sizeof(*shared));
	shared->ret = -1;

	int source_fd __free(close_fd) = open(source, O_RDONLY | O_CLOEXEC);

	if (source_fd < 0)
		return -errno;

	int destination_fd __free(close_fd) =
		open(destination, O_WRONLY | O_CLOEXEC);

	if (destination_fd < 0)
		return -errno;
	if (fstat(source_fd, &statbuf))
		return -errno;

	pid_t pid __free(reap_child) = fork();

	if (pid < 0)
		return -errno;
	if (!pid) {
		loff_t pos_in = 0, pos_out = 0;

		/* Let the parent snapshot counters around exactly one syscall. */
		raise(SIGSTOP);
		errno = 0;
		shared->ret = copy_file_range(source_fd, &pos_in, destination_fd,
					      &pos_out, statbuf.st_size, 0);
		shared->error = shared->ret < 0 ? errno : 0;
		__atomic_store_n(&shared->done, 1, __ATOMIC_RELEASE);
		raise(SIGSTOP);
		_exit(1);
	}

	int ret = wait_stopped(pid, 10000);

	if (ret)
		return ret;
	ret = read_proc_io(pid, &before);
	if (ret)
		return ret;
	if (kill(pid, SIGCONT))
		return -errno;
	ret = wait_stopped(pid, 10000);
	if (ret)
		return ret;
	if (!__atomic_load_n(&shared->done, __ATOMIC_ACQUIRE))
		return -EIO;
	ret = read_proc_io(pid, &after);
	if (ret)
		return ret;

	delta->rchar = after.rchar - before.rchar;
	delta->wchar = after.wchar - before.wchar;
	delta->syscr = after.syscr - before.syscr;
	delta->syscw = after.syscw - before.syscw;
	*result = *shared;
	return 0;
}

static void check_accounting(struct __test_metadata *_metadata,
			     const char *source, const char *destination)
{
	struct io_counters delta = {};
	struct copy_result result = {};
	int ret;

	if (access("/proc/self/io", R_OK))
		SKIP(return, "/proc/PID/io is unavailable");
	ret = run_accounted_copy(source, destination, &delta, &result);
	if (ret == -EACCES || ret == -EPERM || ret == -ENOENT)
		SKIP(return, "/proc/PID/io is unavailable: %s", strerror(-ret));
	ASSERT_EQ(0, ret) TH_LOG("accounted copy failed: %s", strerror(-ret));
	ASSERT_EQ(FILE_SIZE, result.ret)
		TH_LOG("copy_file_range: %s", strerror(result.error));
	EXPECT_EQ((unsigned long long)FILE_SIZE, delta.rchar);
	EXPECT_EQ((unsigned long long)FILE_SIZE, delta.wchar);
	EXPECT_EQ(1ULL, delta.syscr);
	EXPECT_EQ(1ULL, delta.syscw);
	EXPECT_EQ(0, compare_files(source, destination));
}

TEST_F(file_range, translated_source_accounting)
{
	char source[PATH_MAX], destination[PATH_MAX];

	ASSERT_EQ(0, make_path(source, sizeof(source), self->root,
			       "merged/data"));
	ASSERT_EQ(0, make_path(destination, sizeof(destination), self->root,
			       "source-accounting"));
	ASSERT_EQ(0, create_empty_file(destination));
	check_accounting(_metadata, source, destination);
}

TEST_F(file_range, translated_destination_accounting)
{
	char source[PATH_MAX], destination[PATH_MAX];

	ASSERT_EQ(0, make_path(source, sizeof(source), self->root, "plain"));
	ASSERT_EQ(0, make_path(destination, sizeof(destination), self->root,
			       "merged/destination-accounting"));
	ASSERT_EQ(0, create_empty_file(destination));
	check_accounting(_metadata, source, destination);
}

TEST_F(file_range, paired_overlay_accounting)
{
	char source[PATH_MAX], destination[PATH_MAX];

	ASSERT_EQ(0, make_path(source, sizeof(source), self->root,
			       "merged/data"));
	ASSERT_EQ(0, make_path(destination, sizeof(destination), self->root,
			       "merged/paired-accounting"));
	ASSERT_EQ(0, create_empty_file(destination));
	check_accounting(_metadata, source, destination);
}

static bool fanotify_unavailable(int error)
{
	return error == ENOSYS || error == EINVAL || error == EPERM ||
	       error == EOPNOTSUPP || error == ENODEV;
}

static int fanotify_access_perm(const char *path)
{
	unsigned int flags = FAN_CLASS_CONTENT | FAN_CLOEXEC | FAN_NONBLOCK;

	int fan_fd __free(close_fd) =
		fanotify_init(flags, O_RDONLY | O_LARGEFILE | O_CLOEXEC);

	if (fan_fd < 0)
		return -errno;
	if (fanotify_mark(fan_fd, FAN_MARK_ADD | FAN_MARK_INODE,
			  FAN_ACCESS_PERM, AT_FDCWD, path))
		return -errno;

	int ret = fan_fd;

	fan_fd = -1;
	return ret;
}

static int respond_event(int fan_fd, int event_fd, uint32_t decision)
{
	int owned_event_fd __free(close_fd) = event_fd;
	struct fanotify_response response = {
		.fd = owned_event_fd,
		.response = decision,
	};
	ssize_t ret;

	do {
		ret = write(fan_fd, &response, sizeof(response));
	} while (ret < 0 && errno == EINTR);
	if (ret < 0)
		return -errno;
	return ret == sizeof(response) ? 0 : -EIO;
}

static int allow_event(int fan_fd, int event_fd)
{
	return respond_event(fan_fd, event_fd, FAN_ALLOW);
}

static int deny_event(int fan_fd, int event_fd)
{
	return respond_event(fan_fd, event_fd, FAN_DENY);
}

static int poll_readable(int fd, int timeout_ms)
{
	struct pollfd pfd = {
		.fd = fd,
		.events = POLLIN,
	};
	int ret;

	do {
		ret = poll(&pfd, 1, timeout_ms);
	} while (ret < 0 && errno == EINTR);
	if (ret <= 0)
		return ret;
	return pfd.revents & (POLLIN | POLLHUP) ? 1 : 0;
}

static int consume_permission_events(int fan_fd, pid_t hold_pid, int *held_fd,
				     pid_t count_pid, int *event_count,
				     int timeout_ms)
{
	union {
		char bytes[EVENT_BUF_SIZE];
		struct fanotify_event_metadata align;
	} buffer;
	struct fanotify_event_metadata *metadata;
	ssize_t len;

	if (poll_readable(fan_fd, timeout_ms) <= 0)
		return 0;
	do {
		len = read(fan_fd, buffer.bytes, sizeof(buffer.bytes));
	} while (len < 0 && errno == EINTR);
	if (len < 0)
		return errno == EAGAIN ? 0 : -errno;

	metadata = (struct fanotify_event_metadata *)buffer.bytes;
	while (FAN_EVENT_OK(metadata, len)) {
		if (metadata->vers != FANOTIFY_METADATA_VERSION)
			return -EPROTO;
		if (metadata->fd >= 0) {
			if ((metadata->mask & FAN_ACCESS_PERM) &&
			    metadata->pid == count_pid)
				(*event_count)++;
			if ((metadata->mask & FAN_ACCESS_PERM) &&
			    metadata->pid == hold_pid && *held_fd < 0) {
				*held_fd = metadata->fd;
			} else if (metadata->mask & FAN_ACCESS_PERM) {
				int ret = allow_event(fan_fd, metadata->fd);

				if (ret)
					return ret;
			} else {
				close(metadata->fd);
			}
		}
		metadata = FAN_EVENT_NEXT(metadata, len);
	}
	return 1;
}

static int wait_permission_event(int fan_fd, pid_t pid, int *held_fd,
				 int *event_count, int timeout_ms)
{
	long long deadline = monotonic_msec() + timeout_ms;

	while (*held_fd < 0) {
		int ret;

		if (monotonic_msec() >= deadline)
			return -ETIMEDOUT;
		ret = consume_permission_events(fan_fd, pid, held_fd, pid,
						event_count, 100);
		if (ret < 0)
			return ret;
	}
	return 0;
}

static int wait_child(pid_t pid, int *status, int timeout_ms)
{
	long long deadline = monotonic_msec() + timeout_ms;

	for (;;) {
		pid_t waited = waitpid(pid, status, WNOHANG);

		if (waited == pid)
			return 0;
		if (waited < 0)
			return -errno;
		if (monotonic_msec() >= deadline)
			return -ETIMEDOUT;
		usleep(10000);
	}
}

static int wait_child_with_permission_events(int fan_fd, pid_t pid,
					     int *held_fd, pid_t count_pid,
					     int *event_count, int *status,
					     int timeout_ms)
{
	long long deadline = monotonic_msec() + timeout_ms;

	for (;;) {
		pid_t waited = waitpid(pid, status, WNOHANG);
		int ret;

		if (waited == pid)
			return 0;
		if (waited < 0)
			return -errno;
		if (monotonic_msec() >= deadline)
			return -ETIMEDOUT;
		ret = consume_permission_events(fan_fd, -1, held_fd, count_pid,
						event_count, 100);
		if (ret < 0)
			return ret;
	}
}

struct permission_copy_options {
	const char *mark;
	const char *source;
	const char *destination;
	uint32_t response;
	bool unprivileged;
	int (*start_observer)(void *data, int fan_fd, int event_fd);
	int (*wait_observer)(void *data, int timeout_ms);
	void (*cleanup_observer)(void *data);
	void *observer_data;
};

static void permission_copy_child(int source_fd, int destination_fd, int fan_fd,
				  bool unprivileged, int ready_fd,
				  struct copy_result *result)
{
	loff_t pos_in = 0, pos_out = 0;
	char ready = 1;
	ssize_t ret;

	close(fan_fd);
	if (unprivileged &&
	    (setgroups(0, NULL) || setgid(65534) || setuid(65534)))
		goto child_error;
	if (ready_fd >= 0) {
		do {
			ret = write(ready_fd, &ready, sizeof(ready));
		} while (ret < 0 && errno == EINTR);
		close(ready_fd);
		ready_fd = -1;
		if (ret != sizeof(ready))
			goto child_error;
	}

	errno = 0;
	result->ret = copy_file_range(source_fd, &pos_in, destination_fd,
				      &pos_out, FILE_SIZE, 0);
	result->error = result->ret < 0 ? errno : 0;
	__atomic_store_n(&result->done, 1, __ATOMIC_RELEASE);
	_exit(0);

child_error:
	result->ret = -1;
	result->error = errno;
	__atomic_store_n(&result->done, 1, __ATOMIC_RELEASE);
	if (ready_fd >= 0)
		close(ready_fd);
	_exit(2);
}

static int run_permission_copy(const struct permission_copy_options *options,
			       struct copy_result *result, int *status,
			       int *event_count)
{
	struct copy_result *shared = MAP_FAILED;
	int ready_pipe[2] = { -1, -1 };
	int source_fd = -1, destination_fd = -1;
	int fan_fd = -1, held_fd = -1;
	pid_t pid = -1;
	char ready;
	ssize_t nread;
	int ret;

	fan_fd = fanotify_access_perm(options->mark);
	if (fan_fd < 0)
		return fan_fd;
	shared = mmap(NULL, sizeof(*shared), PROT_READ | PROT_WRITE,
		      MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (shared == MAP_FAILED) {
		ret = -errno;
		goto out;
	}
	memset(shared, 0, sizeof(*shared));
	shared->ret = -1;

	source_fd = open(options->source, O_RDONLY | O_CLOEXEC);
	destination_fd = open(options->destination, O_WRONLY | O_CLOEXEC);
	if (source_fd < 0 || destination_fd < 0) {
		ret = -errno;
		goto out;
	}
	if (options->unprivileged && pipe2(ready_pipe, O_CLOEXEC)) {
		ret = -errno;
		goto out;
	}

	pid = fork();
	if (pid < 0) {
		ret = -errno;
		goto out;
	}
	if (!pid) {
		close_fd(ready_pipe[0]);
		permission_copy_child(source_fd, destination_fd, fan_fd,
				      options->unprivileged, ready_pipe[1], shared);
	}
	close_fd(ready_pipe[1]);
	ready_pipe[1] = -1;
	if (ready_pipe[0] >= 0) {
		do {
			nread = read(ready_pipe[0], &ready, sizeof(ready));
		} while (nread < 0 && errno == EINTR);
		close_fd(ready_pipe[0]);
		ready_pipe[0] = -1;
		if (nread != sizeof(ready) || ready != 1) {
			ret = -ECHILD;
			goto out;
		}
	}

	ret = wait_permission_event(fan_fd, pid, &held_fd, event_count, 10000);
	if (ret)
		goto out;
	if (options->start_observer) {
		if (!options->wait_observer || !options->cleanup_observer) {
			ret = -EINVAL;
			goto out;
		}
		ret = options->start_observer(options->observer_data, fan_fd,
					      held_fd);
		if (ret)
			goto out;
		ret = options->wait_observer(options->observer_data, 10000);
		if (ret)
			goto out;
	}
	ret = respond_event(fan_fd, held_fd, options->response);
	held_fd = -1;
	if (ret)
		goto out;
	if (options->response == FAN_ALLOW)
		ret = wait_child_with_permission_events(fan_fd, pid, &held_fd,
							pid, event_count, status, 10000);
	else
		ret = wait_child(pid, status, 10000);
	if (ret)
		goto out;
	pid = -1;
	*result = *shared;
out:
	if (held_fd >= 0) {
		deny_event(fan_fd, held_fd);
		held_fd = -1;
	}
	close_fd(fan_fd);
	kill_and_reap(pid);
	if (options->cleanup_observer)
		options->cleanup_observer(options->observer_data);
	close_fd(ready_pipe[0]);
	close_fd(ready_pipe[1]);
	close_fd(source_fd);
	close_fd(destination_fd);
	unmap_copy_result(shared);
	return ret;
}

struct timestamp_observer {
	const char *path;
	const struct timespec *times;
	mode_t mode;
	pid_t pid;
};

static int start_timestamp_observer(void *data, int fan_fd, int event_fd)
{
	struct timestamp_observer *observer = data;
	pid_t pid;

	pid = fork();
	if (pid < 0)
		return -errno;
	if (!pid) {
		close(event_fd);
		close(fan_fd);
		_exit(utimensat(AT_FDCWD, observer->path, observer->times, 0) ?
		      1 : 0);
	}
	observer->pid = pid;
	return 0;
}

static int wait_timestamp_observer(void *data, int timeout_ms)
{
	struct timestamp_observer *observer = data;
	struct stat statbuf;
	int status;
	int ret;

	ret = wait_child(observer->pid, &status, timeout_ms);
	if (ret)
		return ret;
	observer->pid = -1;
	if (!WIFEXITED(status) || WEXITSTATUS(status))
		return -ECHILD;
	if (stat(observer->path, &statbuf))
		return -errno;
	if (statbuf.st_mtim.tv_sec != observer->times[1].tv_sec ||
	    statbuf.st_mtim.tv_nsec != observer->times[1].tv_nsec ||
	    (statbuf.st_mode & 07777) != observer->mode)
		return -EUCLEAN;
	return 0;
}

static void cleanup_timestamp_observer(void *data)
{
	struct timestamp_observer *observer = data;

	if (observer->pid <= 0)
		return;
	kill_and_reap(observer->pid);
	observer->pid = -1;
}

TEST_F_TIMEOUT(file_range, translated_destination_permission_once, 30)
{
	struct permission_copy_options options = {
		.response = FAN_ALLOW,
	};
	struct copy_result result = {};
	char source[PATH_MAX], destination[PATH_MAX];
	int status = 0, event_count = 0;
	int ret;

	ASSERT_EQ(0, make_path(source, sizeof(source), self->root, "plain"));
	ASSERT_EQ(0, make_path(destination, sizeof(destination), self->root,
			       "merged/permission-once"));
	ASSERT_EQ(0, create_empty_file(destination));

	options.mark = source;
	options.source = source;
	options.destination = destination;
	ret = run_permission_copy(&options, &result, &status, &event_count);
	if (ret < 0 && fanotify_unavailable(-ret))
		SKIP(return, "FAN_ACCESS_PERM is unavailable: %s",
		     strerror(-ret));
	ASSERT_EQ(0, ret) TH_LOG("permission copy setup: %s", strerror(-ret));
	ASSERT_TRUE(WIFEXITED(status));
	ASSERT_EQ(0, WEXITSTATUS(status));
	ASSERT_TRUE(result.done);
	ASSERT_EQ(FILE_SIZE, result.ret)
		TH_LOG("copy_file_range: %s", strerror(result.error));
	EXPECT_EQ(1, event_count);
	EXPECT_EQ(0, compare_files(source, destination));
}

TEST_F_TIMEOUT(file_range,
	       paired_overlay_backing_permission_before_destination_lock, 30)
{
	const struct timespec changed_times[2] = {
		{ .tv_nsec = UTIME_OMIT },
		{ .tv_sec = 946684801, .tv_nsec = 123456789 },
	};
	struct permission_copy_options options = {
		.response = FAN_ALLOW,
		.unprivileged = true,
		.start_observer = start_timestamp_observer,
		.wait_observer = wait_timestamp_observer,
		.cleanup_observer = cleanup_timestamp_observer,
	};
	struct timestamp_observer observer = {
		.times = changed_times,
		.pid = -1,
	};
	struct copy_result result = {};
	struct stat before, after;
	char source[PATH_MAX], lower_source[PATH_MAX], destination[PATH_MAX];
	int status = 0, event_count = 0;
	int ret;

	ASSERT_EQ(0, make_path(source, sizeof(source), self->root, "merged/data"));
	ASSERT_EQ(0, make_path(lower_source, sizeof(lower_source), self->root,
			       "lower/data"));
	ASSERT_EQ(0, make_path(destination, sizeof(destination), self->root,
			       "merged/permission-order"));
	ASSERT_EQ(0, create_pattern_file(destination, 0x5a, FILE_SIZE / 2));
	ASSERT_EQ(0, chmod(destination, 06777));
	ASSERT_EQ(0, stat(destination, &before));
	ASSERT_EQ((mode_t)06000, before.st_mode & 06000);

	observer.path = destination;
	observer.mode = before.st_mode & 07777;
	options.mark = lower_source;
	options.source = source;
	options.destination = destination;
	options.observer_data = &observer;
	ret = run_permission_copy(&options, &result, &status, &event_count);
	if (ret < 0 && fanotify_unavailable(-ret))
		SKIP(return, "FAN_ACCESS_PERM is unavailable: %s",
		     strerror(-ret));
	ASSERT_EQ(0, ret) TH_LOG("allowed permission copy: %s", strerror(-ret));
	ASSERT_TRUE(WIFEXITED(status));
	ASSERT_EQ(0, WEXITSTATUS(status));
	ASSERT_TRUE(result.done);
	ASSERT_EQ(FILE_SIZE, result.ret)
		TH_LOG("copy_file_range: %s", strerror(result.error));
	ASSERT_EQ(1, event_count);
	ASSERT_EQ(0, stat(destination, &after));
	ASSERT_EQ((mode_t)0, after.st_mode & 06000);
	EXPECT_EQ(0, compare_files(source, destination));
}

TEST_F_TIMEOUT(file_range,
	       paired_overlay_denied_backing_permission_preserves_privs, 30)
{
	struct permission_copy_options options = {
		.response = FAN_DENY,
		.unprivileged = true,
	};
	struct copy_result result = {};
	struct stat before, after;
	char source[PATH_MAX], lower_source[PATH_MAX], destination[PATH_MAX];
	char reference[PATH_MAX];
	int status = 0, event_count = 0;
	int ret;

	ASSERT_EQ(0, make_path(source, sizeof(source), self->root, "merged/data"));
	ASSERT_EQ(0, make_path(lower_source, sizeof(lower_source), self->root,
			       "lower/data"));
	ASSERT_EQ(0, make_path(destination, sizeof(destination), self->root,
			       "merged/permission-denial"));
	ASSERT_EQ(0, make_path(reference, sizeof(reference), self->root,
			       "permission-denial-reference"));
	ASSERT_EQ(0, create_pattern_file(destination, 0x5a, FILE_SIZE / 2));
	ASSERT_EQ(0, create_pattern_file(reference, 0x5a, FILE_SIZE / 2));
	ASSERT_EQ(0, chmod(destination, 06777));
	ASSERT_EQ(0, stat(destination, &before));
	ASSERT_EQ((mode_t)06000, before.st_mode & 06000);

	options.mark = lower_source;
	options.source = source;
	options.destination = destination;
	ret = run_permission_copy(&options, &result, &status, &event_count);
	if (ret < 0 && fanotify_unavailable(-ret))
		SKIP(return, "FAN_ACCESS_PERM is unavailable: %s",
		     strerror(-ret));
	ASSERT_EQ(0, ret) TH_LOG("denied permission copy: %s", strerror(-ret));
	ASSERT_TRUE(WIFEXITED(status));
	ASSERT_EQ(0, WEXITSTATUS(status));
	ASSERT_TRUE(result.done);
	ASSERT_EQ(-1, result.ret);
	ASSERT_EQ(EPERM, result.error);
	ASSERT_EQ(1, event_count);
	ASSERT_EQ(0, stat(destination, &after));
	EXPECT_EQ(before.st_mode & 07777, after.st_mode & 07777);
	EXPECT_EQ(before.st_size, after.st_size);
	EXPECT_EQ(0, compare_files(destination, reference));
}

struct copyup_race_state {
	struct copy_result *result;
	pid_t copy_pid;
	pid_t writer_pid;
	int source_fd;
	int destination_fd;
	int lower_fd;
	int held_fd;
	int fan_fd;
};

static void cleanup_copyup_race(struct copyup_race_state *state)
{
	if (state->held_fd >= 0) {
		if (state->fan_fd >= 0)
			allow_event(state->fan_fd, state->held_fd);
		else
			close_fd(state->held_fd);
	}
	close_fd(state->fan_fd);
	kill_and_reap(state->copy_pid);
	kill_and_reap(state->writer_pid);
	close_fd(state->source_fd);
	close_fd(state->destination_fd);
	close_fd(state->lower_fd);
	unmap_copy_result(state->result);
}

DEFINE_FREE(copyup_race, struct copyup_race_state, cleanup_copyup_race(&_T))

static int run_permission_copyup_race(struct __test_metadata *_metadata,
				      const char *logical_source,
				      const char *lower_source,
				      const char *destination,
				      int *skip_error)
{
	unsigned char original, replacement, logical_byte;
	int copy_events = 0, copy_status = 0, writer_status = 0;
	ssize_t bytes;
	int ret;
	struct copyup_race_state state __free(copyup_race) = {
		.result = MAP_FAILED,
		.copy_pid = -1,
		.writer_pid = -1,
		.source_fd = -1,
		.destination_fd = -1,
		.lower_fd = -1,
		.held_fd = -1,
		.fan_fd = -1,
	};

	*skip_error = 0;
	state.result = mmap(NULL, sizeof(*state.result), PROT_READ | PROT_WRITE,
			    MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (state.result == MAP_FAILED)
		return -errno;
	memset(state.result, 0, sizeof(*state.result));
	state.result->ret = -1;

	state.lower_fd = open(lower_source, O_RDONLY | O_CLOEXEC);
	if (state.lower_fd < 0) {
		ret = -errno;
		TH_LOG("open lower source: %s", strerror(-ret));
		return ret;
	}
	bytes = pread(state.lower_fd, &original, 1, 0);
	if (bytes != 1) {
		ret = bytes < 0 ? -errno : -EIO;
		TH_LOG("read lower source: %s", strerror(-ret));
		return ret;
	}
	close_fd(state.lower_fd);
	state.lower_fd = -1;
	replacement = original ^ 0xff;

	state.fan_fd = fanotify_init(FAN_CLASS_CONTENT | FAN_CLOEXEC |
				     FAN_NONBLOCK,
				     O_RDONLY | O_LARGEFILE | O_CLOEXEC);
	if (state.fan_fd < 0) {
		int error = errno;

		if (fanotify_unavailable(error))
			*skip_error = error;
		else
			TH_LOG("fanotify_init: %s", strerror(error));
		return -error;
	}
	if (fanotify_mark(state.fan_fd, FAN_MARK_ADD | FAN_MARK_INODE,
			  FAN_ACCESS_PERM, AT_FDCWD, lower_source)) {
		int error = errno;

		if (fanotify_unavailable(error))
			*skip_error = error;
		else
			TH_LOG("fanotify_mark: %s", strerror(error));
		return -error;
	}

	state.source_fd = open(logical_source, O_RDONLY | O_CLOEXEC);
	if (state.source_fd < 0) {
		ret = -errno;
		TH_LOG("open logical source: %s", strerror(-ret));
		return ret;
	}
	state.destination_fd = open(destination,
				    O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
				    0600);
	if (state.destination_fd < 0) {
		ret = -errno;
		TH_LOG("open destination: %s", strerror(-ret));
		return ret;
	}
	state.copy_pid = fork();
	if (state.copy_pid < 0) {
		ret = -errno;
		TH_LOG("fork copy: %s", strerror(-ret));
		return ret;
	}
	if (!state.copy_pid) {
		loff_t pos_in = 0, pos_out = 0;

		close_fd(state.fan_fd);
		errno = 0;
		state.result->ret = copy_file_range(state.source_fd, &pos_in,
						    state.destination_fd, &pos_out,
						    FILE_SIZE, 0);
		state.result->error = state.result->ret < 0 ? errno : 0;
		__atomic_store_n(&state.result->done, 1, __ATOMIC_RELEASE);
		_exit(0);
	}

	/* Hold the backing permission check before copy execution starts. */
	ret = wait_permission_event(state.fan_fd, state.copy_pid, &state.held_fd,
				    &copy_events, 10000);
	if (ret) {
		TH_LOG("wait for backing permission event: %s", strerror(-ret));
		return ret;
	}

	state.writer_pid = fork();
	if (state.writer_pid < 0) {
		ret = -errno;
		TH_LOG("fork copy-up writer: %s", strerror(-ret));
		return ret;
	}
	if (!state.writer_pid) {
		int fd;
		int child_ret = 1;

		close_fd(state.held_fd);
		close_fd(state.fan_fd);
		fd = open(logical_source, O_WRONLY | O_CLOEXEC);
		if (fd >= 0 && pwrite(fd, &replacement, 1, 0) == 1)
			child_ret = 0;
		close_fd(fd);
		_exit(child_ret);
	}

	/* Copy the logical source up while its selected lower file is pinned. */
	ret = wait_child_with_permission_events(state.fan_fd, state.writer_pid,
						&state.held_fd, state.copy_pid,
						&copy_events, &writer_status, 10000);
	if (ret) {
		TH_LOG("wait for copy-up writer: %s", strerror(-ret));
		return ret;
	}
	state.writer_pid = -1;
	if (!WIFEXITED(writer_status) || WEXITSTATUS(writer_status)) {
		TH_LOG("copy-up writer failed: status=%d", writer_status);
		return -ECHILD;
	}

	/* The copy must continue from the retained lower file after copy-up. */
	ret = allow_event(state.fan_fd, state.held_fd);
	state.held_fd = -1;
	if (ret) {
		TH_LOG("allow held copy event: %s", strerror(-ret));
		return ret;
	}

	ret = wait_child_with_permission_events(state.fan_fd, state.copy_pid,
						&state.held_fd, state.copy_pid,
						&copy_events, &copy_status, 10000);
	if (ret) {
		TH_LOG("wait for copy child: %s", strerror(-ret));
		return ret;
	}
	state.copy_pid = -1;
	if (!WIFEXITED(copy_status) || WEXITSTATUS(copy_status) ||
	    !__atomic_load_n(&state.result->done, __ATOMIC_ACQUIRE) ||
	    state.result->ret != FILE_SIZE) {
		TH_LOG("copy status=%d ret=%zd error=%s", copy_status,
		       state.result->ret, strerror(state.result->error));
		return state.result->ret < 0 && state.result->error ?
			-state.result->error : -EIO;
	}
	if (copy_events != 1) {
		TH_LOG("backing permission events=%d expected=1", copy_events);
		return -EIO;
	}

	close_fd(state.fan_fd);
	state.fan_fd = -1;
	ret = compare_files(lower_source, destination);
	if (ret) {
		TH_LOG("destination does not contain the selected lower file");
		return ret < 0 ? ret : -EUCLEAN;
	}
	state.lower_fd = open(logical_source, O_RDONLY | O_CLOEXEC);
	if (state.lower_fd < 0) {
		ret = -errno;
		TH_LOG("open logical source: %s", strerror(-ret));
		return ret;
	}
	bytes = pread(state.lower_fd, &logical_byte, 1, 0);
	if (bytes != 1) {
		ret = bytes < 0 ? -errno : -EIO;
		TH_LOG("read logical source: %s", strerror(-ret));
		return ret;
	}
	if (logical_byte != replacement) {
		TH_LOG("logical source was not copied up");
		return -EUCLEAN;
	}
	return 0;
}

TEST_F_TIMEOUT(file_range, permission_copyup_race, 60)
{
	char logical_source[PATH_MAX], lower_source[PATH_MAX];
	char destination[PATH_MAX];
	int skip_error;
	int ret;

	ASSERT_EQ(0, make_path(logical_source, sizeof(logical_source), self->root,
			       "merged/race"));
	ASSERT_EQ(0, make_path(lower_source, sizeof(lower_source), self->root,
			       "lower/race"));
	ASSERT_EQ(0, make_path(destination, sizeof(destination), self->root,
			       "race-out"));

	ret = run_permission_copyup_race(_metadata, logical_source, lower_source,
					 destination, &skip_error);
	if (skip_error)
		SKIP(return, "FAN_ACCESS_PERM is unavailable: %s",
		     strerror(skip_error));
	EXPECT_EQ(0, ret) TH_LOG("permission copy-up race: %s", strerror(-ret));
}

TEST_HARNESS_MAIN
