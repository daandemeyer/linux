// SPDX-License-Identifier: GPL-2.0
/*
 * FUSE passthrough file range tests.
 *
 * The raw FUSE server keeps all backing files on one tmpfs.  This avoids an
 * external filesystem image while still exercising passthrough resolution,
 * the VFS-owned terminal splice path and the ordinary FUSE fallback.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <linux/fs.h>
#include <linux/fuse.h>
#include <linux/limits.h>
#include <linux/magic.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "kselftest_harness.h"

#define TEST_SIZE (128 * 1024)
#define SERVER_MAX_WRITE (1024 * 1024)
#define SERVER_BUF_SIZE (SERVER_MAX_WRITE + 4096)

enum node_index {
	NODE_SRC,
	NODE_DST,
	NODE_SERVER,
	NODE_DIRECT,
	NODE_MIXED,
	NODE_COUNT,
};

struct test_node {
	uint64_t ino;
	const char *name;
	const char *path;
	int fd;
	int backing_id;
	uint32_t open_flags;
	bool read_only;
	bool direct_writes;
};

static int make_path(char *path, size_t size, const char *directory,
		     const char *name)
{
	int len = snprintf(path, size, "%s/%s", directory, name);

	return len < 0 || (size_t)len >= size ? -ENAMETOOLONG : 0;
}

static int reply_data(int fd, const struct fuse_in_header *in, int error,
		      const void *data, size_t len)
{
	struct fuse_out_header out = {
		.len = sizeof(out) + (error ? 0 : len),
		.error = error ? -error : 0,
		.unique = in->unique,
	};
	struct iovec iov[] = {
		{ .iov_base = &out, .iov_len = sizeof(out) },
		{ .iov_base = (void *)data, .iov_len = error ? 0 : len },
	};
	ssize_t ret;

	do {
		ret = writev(fd, iov, error || !len ? 1 : 2);
	} while (ret < 0 && errno == EINTR);
	if (ret < 0)
		return -errno;
	return ret == (ssize_t)out.len ? 0 : -EIO;
}

static struct test_node *find_node(struct test_node *nodes, uint64_t ino)
{
	size_t i;

	for (i = 0; i < NODE_COUNT; i++)
		if (nodes[i].ino == ino)
			return &nodes[i];
	return NULL;
}

static struct test_node *find_name(struct test_node *nodes, const char *name)
{
	size_t i;

	for (i = 0; i < NODE_COUNT; i++)
		if (!strcmp(nodes[i].name, name))
			return &nodes[i];
	return NULL;
}

static int fill_attr(struct fuse_attr *attr, struct test_node *node)
{
	struct stat st;

	memset(attr, 0, sizeof(*attr));
	if (!node) {
		attr->ino = FUSE_ROOT_ID;
		attr->mode = S_IFDIR | 0755;
		attr->nlink = 2;
		attr->blksize = 4096;
		return 0;
	}

	if (fstat(node->fd, &st))
		return errno;
	attr->ino = node->ino;
	attr->size = st.st_size;
	attr->blocks = st.st_blocks;
	attr->atime = st.st_atim.tv_sec;
	attr->mtime = st.st_mtim.tv_sec;
	attr->ctime = st.st_ctim.tv_sec;
	attr->atimensec = st.st_atim.tv_nsec;
	attr->mtimensec = st.st_mtim.tv_nsec;
	attr->ctimensec = st.st_ctim.tv_nsec;
	attr->mode = st.st_mode;
	attr->nlink = st.st_nlink;
	attr->uid = st.st_uid;
	attr->gid = st.st_gid;
	attr->rdev = st.st_rdev;
	attr->blksize = st.st_blksize;
	return 0;
}

static int reply_attr(int fd, const struct fuse_in_header *in,
		      struct test_node *node)
{
	struct fuse_attr_out out = { .attr_valid = 60 };
	int error;

	error = fill_attr(&out.attr, node);
	return error ? reply_data(fd, in, error, NULL, 0) :
		       reply_data(fd, in, 0, &out, sizeof(out));
}

static int register_backing(int fuse_fd, struct test_node *node)
{
	struct fuse_backing_map map = { .fd = node->fd };
	int id;

	if (node->backing_id > 0)
		return node->backing_id;
	id = ioctl(fuse_fd, FUSE_DEV_IOC_BACKING_OPEN, &map);
	if (id < 0)
		return -errno;
	node->backing_id = id;
	return id;
}

static int setattr_node(struct test_node *node,
			const struct fuse_setattr_in *arg)
{
	struct timespec times[2] = {
		{ .tv_nsec = UTIME_OMIT },
		{ .tv_nsec = UTIME_OMIT },
	};
	struct stat st;
	mode_t mode;

	if (fstat(node->fd, &st))
		return errno;
	if ((arg->valid & FATTR_SIZE) && ftruncate(node->fd, arg->size))
		return errno;
	if (arg->valid & FATTR_MODE) {
		if (fchmod(node->fd, arg->mode))
			return errno;
	}
	if (arg->valid & FATTR_KILL_SUIDGID) {
		mode = st.st_mode & ~(S_ISUID | S_ISGID);
		if (fchmod(node->fd, mode))
			return errno;
	}
	if ((arg->valid & (FATTR_UID | FATTR_GID)) &&
	    fchown(node->fd,
		   arg->valid & FATTR_UID ? (uid_t)arg->uid : (uid_t)-1,
		   arg->valid & FATTR_GID ? (gid_t)arg->gid : (gid_t)-1))
		return errno;
	if (arg->valid & FATTR_ATIME_NOW) {
		times[0].tv_nsec = UTIME_NOW;
	} else if (arg->valid & FATTR_ATIME) {
		times[0].tv_sec = arg->atime;
		times[0].tv_nsec = arg->atimensec;
	}
	if (arg->valid & FATTR_MTIME_NOW) {
		times[1].tv_nsec = UTIME_NOW;
	} else if (arg->valid & FATTR_MTIME) {
		times[1].tv_sec = arg->mtime;
		times[1].tv_nsec = arg->mtimensec;
	}
	if (futimens(node->fd, times))
		return errno;
	return 0;
}

static int handle_request(int fd, struct test_node *nodes, void *buffer,
			  ssize_t size, bool *initialized)
{
	struct fuse_in_header *in = buffer;
	struct test_node *node;
	void *payload;

	if (size < (ssize_t)sizeof(*in) || in->len < sizeof(*in) ||
	    in->len > (uint32_t)size)
		return -EINVAL;
	payload = (char *)buffer + sizeof(*in);
	node = find_node(nodes, in->nodeid);

	switch (in->opcode) {
	case FUSE_INIT: {
		const struct fuse_init_in *arg = payload;
		struct fuse_init_out out = {
			.major = FUSE_KERNEL_VERSION,
			.minor = arg->minor < FUSE_KERNEL_MINOR_VERSION ?
				 arg->minor : FUSE_KERNEL_MINOR_VERSION,
			.max_readahead = arg->max_readahead,
			.max_write = SERVER_MAX_WRITE,
			.time_gran = 1,
			.max_pages = 256,
			.max_stack_depth = 1,
		};
		uint64_t flags = FUSE_INIT_EXT | FUSE_PASSTHROUGH;

		out.flags = flags;
		out.flags2 = flags >> 32;
		*initialized = true;
		return reply_data(fd, in, 0, &out, sizeof(out));
	}
	case FUSE_LOOKUP: {
		struct fuse_entry_out out = {
			.entry_valid = 60,
			.attr_valid = 60,
		};
		int error;

		if (in->nodeid != FUSE_ROOT_ID)
			return reply_data(fd, in, ENOENT, NULL, 0);
		node = find_name(nodes, payload);
		if (!node)
			return reply_data(fd, in, ENOENT, NULL, 0);
		out.nodeid = node->ino;
		out.generation = 1;
		error = fill_attr(&out.attr, node);
		return error ? reply_data(fd, in, error, NULL, 0) :
			       reply_data(fd, in, 0, &out, sizeof(out));
	}
	case FUSE_FORGET:
	case FUSE_BATCH_FORGET:
		return 0;
	case FUSE_GETATTR:
		if (in->nodeid == FUSE_ROOT_ID)
			return reply_attr(fd, in, NULL);
		return node ? reply_attr(fd, in, node) :
			      reply_data(fd, in, ENOENT, NULL, 0);
	case FUSE_SETATTR: {
		int error;

		if (!node)
			return reply_data(fd, in, ENOENT, NULL, 0);
		error = setattr_node(node, payload);
		return error ? reply_data(fd, in, error, NULL, 0) :
			       reply_attr(fd, in, node);
	}
	case FUSE_OPEN: {
		const struct fuse_open_in *arg = payload;
		struct fuse_open_out out = {};
		uint32_t open_flags;
		int id = 0;

		if (!node)
			return reply_data(fd, in, ENOENT, NULL, 0);
		if (!*initialized)
			return reply_data(fd, in, EIO, NULL, 0);
		open_flags = node->open_flags;
		if (node->direct_writes &&
		    (arg->flags & O_ACCMODE) != O_RDONLY)
			open_flags |= FOPEN_DIRECT_IO;
		if (open_flags & FOPEN_PASSTHROUGH) {
			id = register_backing(fd, node);
			if (id < 0)
				return reply_data(fd, in, -id, NULL, 0);
		}
		out.fh = node->ino;
		out.open_flags = open_flags;
		out.backing_id = id;
		return reply_data(fd, in, 0, &out, sizeof(out));
	}
	case FUSE_READ: {
		const struct fuse_read_in *arg = payload;
		void *data;
		ssize_t ret;
		int error;

		if (!node)
			return reply_data(fd, in, ENOENT, NULL, 0);
		data = malloc(arg->size);
		if (!data)
			return reply_data(fd, in, ENOMEM, NULL, 0);
		ret = pread(node->fd, data, arg->size, arg->offset);
		if (ret < 0)
			error = reply_data(fd, in, errno, NULL, 0);
		else
			error = reply_data(fd, in, 0, data, ret);
		free(data);
		return error;
	}
	case FUSE_WRITE: {
		const struct fuse_write_in *arg = payload;
		struct fuse_write_out out = {};
		void *data = (char *)payload + sizeof(*arg);
		ssize_t ret;

		if (!node)
			return reply_data(fd, in, ENOENT, NULL, 0);
		ret = pwrite(node->fd, data, arg->size, arg->offset);
		if (ret < 0)
			return reply_data(fd, in, errno, NULL, 0);
		out.size = ret;
		return reply_data(fd, in, 0, &out, sizeof(out));
	}
	case FUSE_FLUSH:
	case FUSE_RELEASE:
	case FUSE_FSYNC:
	case FUSE_ACCESS:
		return reply_data(fd, in, 0, NULL, 0);
	case FUSE_GETXATTR:
	case FUSE_LISTXATTR:
	case FUSE_REMOVEXATTR:
		return reply_data(fd, in, ENODATA, NULL, 0);
	case FUSE_COPY_FILE_RANGE:
	case FUSE_COPY_FILE_RANGE_64:
		return reply_data(fd, in, EOPNOTSUPP, NULL, 0);
	case FUSE_DESTROY:
		return 1;
	default:
		return reply_data(fd, in, ENOSYS, NULL, 0);
	}
}

static int open_backing_nodes(struct test_node *nodes)
{
	size_t i;

	for (i = 0; i < NODE_COUNT; i++) {
		int flags = nodes[i].read_only ? O_RDONLY : O_RDWR;

		nodes[i].fd = open(nodes[i].path, flags | O_CLOEXEC);
		if (nodes[i].fd < 0)
			return -errno;
	}
	return 0;
}

static void close_backing_nodes(struct test_node *nodes)
{
	size_t i;

	for (i = 0; i < NODE_COUNT; i++)
		if (nodes[i].fd >= 0)
			close(nodes[i].fd);
}

static bool fuse_unavailable_error(int error)
{
	return error == ENODEV || error == EOPNOTSUPP ||
	       error == EPERM || error == EACCES;
}

static int server_main(const char *mountpoint, const char *const *paths)
{
	struct test_node nodes[NODE_COUNT] = {
		{ 2, "src", paths[NODE_SRC], -1, 0, FOPEN_PASSTHROUGH, true, false },
		{ 3, "dst", paths[NODE_DST], -1, 0, FOPEN_PASSTHROUGH, false, false },
		{ 4, "server", paths[NODE_SERVER], -1, 0, 0, false, false },
		{ 5, "direct", paths[NODE_DIRECT], -1, 0,
		  FOPEN_PASSTHROUGH | FOPEN_DIRECT_IO, false, false },
		{ 6, "mixed", paths[NODE_MIXED], -1, 0,
		  FOPEN_PASSTHROUGH, false, true },
	};
	char options[256];
	char *buffer = NULL;
	bool initialized = false;
	pid_t mounter;
	int fuse_fd = -1;
	int status = KSFT_FAIL;

	if (open_backing_nodes(nodes))
		goto out;
	fuse_fd = open("/dev/fuse", O_RDWR | O_CLOEXEC);
	if (fuse_fd < 0) {
		if (fuse_unavailable_error(errno))
			status = KSFT_SKIP;
		goto out;
	}
	buffer = malloc(SERVER_BUF_SIZE);
	if (!buffer)
		goto out;

	snprintf(options, sizeof(options),
		 "fd=%d,rootmode=40000,user_id=%u,group_id=%u,default_permissions,allow_other",
		 fuse_fd, getuid(), getgid());
	mounter = fork();
	if (mounter < 0)
		goto out;
	if (!mounter) {
		if (mount("fuse-passthrough-test", mountpoint, "fuse",
			  MS_NOSUID | MS_NODEV, options)) {
			int error = errno;

			_exit(fuse_unavailable_error(error) ? KSFT_SKIP :
			      KSFT_FAIL);
		}
		_exit(KSFT_PASS);
	}

	for (;;) {
		ssize_t size;
		int ret;

		size = read(fuse_fd, buffer, SERVER_BUF_SIZE);
		if (size < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EPERM) {
				int mounter_status;
				pid_t waited;

				waited = mounter > 0 ?
					 waitpid(mounter, &mounter_status, WNOHANG) : 0;
				if (waited == mounter) {
					mounter = -1;
					if (!WIFEXITED(mounter_status))
						goto out;
					if (WEXITSTATUS(mounter_status) == KSFT_SKIP) {
						status = KSFT_SKIP;
						goto out;
					}
					if (WEXITSTATUS(mounter_status) != KSFT_PASS)
						goto out;
				}
				usleep(1000);
				continue;
			}
			if (errno == ENODEV) {
				int mounter_status;
				pid_t waited;

				if (initialized) {
					status = KSFT_PASS;
					break;
				}
				do {
					waited = mounter > 0 ?
						 waitpid(mounter, &mounter_status, 0) : 0;
				} while (waited < 0 && errno == EINTR);
				if (waited == mounter) {
					mounter = -1;
					if (WIFEXITED(mounter_status) &&
					    WEXITSTATUS(mounter_status) == KSFT_SKIP)
						status = KSFT_SKIP;
				}
				break;
			}
			goto out;
		}
		ret = handle_request(fuse_fd, nodes, buffer, size, &initialized);
		if (ret == 1) {
			status = KSFT_PASS;
			break;
		}
		if (ret < 0)
			goto out;
	}

	if (mounter > 0)
		waitpid(mounter, NULL, 0);
out:
	free(buffer);
	if (fuse_fd >= 0)
		close(fuse_fd);
	close_backing_nodes(nodes);
	return status;
}

static int write_pattern(const char *path, unsigned char pattern, size_t len)
{
	unsigned char buffer[4096];
	size_t written = 0;
	int fd;

	memset(buffer, pattern, sizeof(buffer));
	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0)
		return -errno;
	while (written < len) {
		size_t count = len - written;
		ssize_t ret;

		if (count > sizeof(buffer))
			count = sizeof(buffer);
		ret = write(fd, buffer, count);
		if (ret <= 0) {
			int error = ret < 0 ? errno : EIO;

			close(fd);
			return -error;
		}
		written += ret;
	}
	return close(fd) ? -errno : 0;
}

static ssize_t copy_once(const char *source, const char *destination,
			 size_t len, loff_t pos_in, loff_t pos_out)
{
	int source_fd, destination_fd;
	ssize_t ret;

	source_fd = open(source, O_RDONLY | O_CLOEXEC);
	if (source_fd < 0)
		return -errno;
	destination_fd = open(destination, O_WRONLY | O_CLOEXEC);
	if (destination_fd < 0) {
		ret = -errno;
		close(source_fd);
		return ret;
	}
	ret = copy_file_range(source_fd, &pos_in, destination_fd, &pos_out,
			      len, 0);
	if (ret < 0)
		ret = -errno;
	close(destination_fd);
	close(source_fd);
	return ret;
}

static int clone_once(const char *source, const char *destination, size_t len)
{
	struct file_clone_range range = { .src_length = len };
	int source_fd, destination_fd;
	int ret;

	source_fd = open(source, O_RDONLY | O_CLOEXEC);
	if (source_fd < 0)
		return -errno;
	destination_fd = open(destination, O_WRONLY | O_CLOEXEC);
	if (destination_fd < 0) {
		ret = -errno;
		close(source_fd);
		return ret;
	}
	range.src_fd = source_fd;
	ret = ioctl(destination_fd, FICLONERANGE, &range) ? -errno : 0;
	close(destination_fd);
	close(source_fd);
	return ret;
}

static int compare_range(const char *source, const char *destination,
			 loff_t pos_in, loff_t pos_out, size_t len)
{
	unsigned char source_buffer[4096], destination_buffer[4096];
	int source_fd, destination_fd;
	int ret = -1;

	source_fd = open(source, O_RDONLY | O_CLOEXEC);
	if (source_fd < 0)
		return -1;
	destination_fd = open(destination, O_RDONLY | O_CLOEXEC);
	if (destination_fd < 0)
		goto out_source;
	while (len) {
		size_t count = len;
		ssize_t source_read, destination_read;

		if (count > sizeof(source_buffer))
			count = sizeof(source_buffer);
		source_read = pread(source_fd, source_buffer, count, pos_in);
		destination_read = pread(destination_fd, destination_buffer,
					 count, pos_out);
		if (source_read != (ssize_t)count ||
		    destination_read != (ssize_t)count ||
		    memcmp(source_buffer, destination_buffer, count))
			goto out;
		pos_in += count;
		pos_out += count;
		len -= count;
	}
	ret = 0;
out:
	close(destination_fd);
out_source:
	close(source_fd);
	return ret;
}

static int read_range(const char *path, void *buffer, size_t len, loff_t pos)
{
	int fd;
	ssize_t ret;

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -errno;
	ret = pread(fd, buffer, len, pos);
	if (ret != (ssize_t)len)
		ret = ret < 0 ? -errno : -EIO;
	else
		ret = 0;
	close(fd);
	return ret;
}

static int copy_as_nobody(const char *source, const char *destination,
			  size_t len, loff_t pos_in, loff_t pos_out)
{
	int source_fd, destination_fd;
	int status;
	pid_t child;

	source_fd = open(source, O_RDONLY | O_CLOEXEC);
	if (source_fd < 0)
		return -errno;
	destination_fd = open(destination, O_WRONLY | O_CLOEXEC);
	if (destination_fd < 0) {
		status = -errno;
		close(source_fd);
		return status;
	}
	child = fork();
	if (!child) {
		ssize_t ret;

		if (setgroups(0, NULL) || setgid(65534) || setuid(65534))
			_exit(2);
		ret = copy_file_range(source_fd, &pos_in, destination_fd, &pos_out,
				      len, 0);
		_exit(ret == (ssize_t)len ? 0 : 1);
	}
	if (child < 0)
		status = -errno;
	else if (waitpid(child, &status, 0) != child)
		status = -errno;
	else
		status = WIFEXITED(status) ? WEXITSTATUS(status) : 3;
	close(destination_fd);
	close(source_fd);
	return status;
}

static long long read_proc_io(const char *key)
{
	char name[32];
	long long value;
	FILE *file;

	file = fopen("/proc/self/io", "re");
	if (!file)
		return -1;
	while (fscanf(file, "%31[^:]: %lld\n", name, &value) == 2) {
		if (!strcmp(name, key)) {
			fclose(file);
			return value;
		}
	}
	fclose(file);
	return -1;
}

FIXTURE(fuse_passthrough) {
	char root[PATH_MAX];
	char backing[PATH_MAX];
	char mountpoint[PATH_MAX];
	char paths[NODE_COUNT + 2][PATH_MAX];
	pid_t server;
	bool tmpfs_mounted;
	bool fuse_mounted;
};

static void cleanup_fixture(struct _test_data_fuse_passthrough *self)
{
	size_t i;

	if (self->mountpoint[0])
		umount2(self->mountpoint, MNT_DETACH);
	if (self->server > 0) {
		kill(self->server, SIGTERM);
		waitpid(self->server, NULL, 0);
	}
	if (self->tmpfs_mounted)
		umount2(self->backing, MNT_DETACH);
	for (i = 0; i < ARRAY_SIZE(self->paths); i++)
		if (self->paths[i][0])
			unlink(self->paths[i]);
	if (self->mountpoint[0])
		rmdir(self->mountpoint);
	if (self->backing[0])
		rmdir(self->backing);
	if (self->root[0])
		rmdir(self->root);
}

FIXTURE_SETUP(fuse_passthrough)
{
	const char *names[NODE_COUNT + 2] = {
		"source", "destination", "server", "direct", "mixed",
		"plain-source", "plain-output",
	};
	const char *server_paths[NODE_COUNT];
	struct stat before, after;
	int status;
	size_t i;

	memset(self, 0, sizeof(*self));
	self->server = -1;
	if (geteuid())
		SKIP(return, "FUSE passthrough requires root");
	if (access("/dev/fuse", R_OK | W_OK))
		SKIP(return, "/dev/fuse is unavailable");
	if (unshare(CLONE_NEWNS))
		SKIP(return, "cannot create mount namespace: %s", strerror(errno));
	ASSERT_EQ(0, mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL));

	strcpy(self->root, "/tmp/fuse-passthrough-cfr-XXXXXX");
	ASSERT_NE(NULL, mkdtemp(self->root));
	ASSERT_EQ(0, make_path(self->backing, sizeof(self->backing),
			       self->root, "backing"));
	ASSERT_EQ(0, make_path(self->mountpoint, sizeof(self->mountpoint),
			       self->root, "fuse"));
	ASSERT_EQ(0, mkdir(self->backing, 0755));
	ASSERT_EQ(0, mkdir(self->mountpoint, 0755));
	ASSERT_EQ(0, mount("tmpfs", self->backing, "tmpfs", 0,
			   "mode=0755,size=16m"));
	self->tmpfs_mounted = true;

	for (i = 0; i < ARRAY_SIZE(self->paths); i++) {
		ASSERT_EQ(0, make_path(self->paths[i], sizeof(self->paths[i]),
				       self->backing, names[i]));
		ASSERT_EQ(0, write_pattern(self->paths[i], 0x61 + i,
					   i == NODE_DST ? 0 : TEST_SIZE));
	}
	for (i = 0; i < NODE_COUNT; i++)
		server_paths[i] = self->paths[i];
	ASSERT_EQ(0, stat(self->mountpoint, &before));

	self->server = fork();
	ASSERT_GE(self->server, 0);
	if (!self->server)
		_exit(server_main(self->mountpoint, server_paths));

	for (i = 0; i < 200; i++) {
		pid_t waited = waitpid(self->server, &status, WNOHANG);

		if (waited == self->server) {
			bool unavailable = WIFEXITED(status) &&
					   WEXITSTATUS(status) == KSFT_SKIP;

			self->server = -1;
			cleanup_fixture(self);
			if (unavailable)
				SKIP(return, "FUSE filesystem is unavailable");
			ASSERT_TRUE(false)
				TH_LOG("FUSE server exited before mount: status=%#x",
				       status);
		}
		if (!stat(self->mountpoint, &after) && after.st_dev != before.st_dev) {
			self->fuse_mounted = true;
			break;
		}
		usleep(10000);
	}
	if (!self->fuse_mounted) {
		cleanup_fixture(self);
		ASSERT_TRUE(false) TH_LOG("timed out mounting FUSE filesystem");
	}

	{
		char path[PATH_MAX];
		int fd;

		ASSERT_EQ(0, make_path(path, sizeof(path), self->mountpoint,
				       "src"));
		fd = open(path, O_RDONLY | O_CLOEXEC);
		if (fd < 0 && (errno == ENOTTY || errno == EOPNOTSUPP ||
			       errno == EPERM)) {
			int error = errno;

			cleanup_fixture(self);
			SKIP(return, "FUSE passthrough is unavailable: %s",
			     strerror(error));
		}
		if (fd < 0)
			cleanup_fixture(self);
		ASSERT_GE(fd, 0);
		close(fd);
	}
}

FIXTURE_TEARDOWN(fuse_passthrough)
{
	cleanup_fixture(self);
}

static int logical_path(const char *mountpoint, const char *name, char *path,
			size_t size)
{
	return make_path(path, size, mountpoint, name);
}

TEST_F(fuse_passthrough, per_open_method_selection)
{
	char src[PATH_MAX], dst[PATH_MAX], server[PATH_MAX];
	char direct[PATH_MAX], mixed[PATH_MAX];
	const char *plain_source = self->paths[NODE_COUNT];
	const char *plain_output = self->paths[NODE_COUNT + 1];
	ssize_t ret;

	ASSERT_EQ(0, logical_path(self->mountpoint, "src", src, sizeof(src)));
	ASSERT_EQ(0, logical_path(self->mountpoint, "dst", dst, sizeof(dst)));
	ASSERT_EQ(0, logical_path(self->mountpoint, "server", server,
				  sizeof(server)));
	ASSERT_EQ(0, logical_path(self->mountpoint, "direct", direct,
				  sizeof(direct)));
	ASSERT_EQ(0, logical_path(self->mountpoint, "mixed", mixed,
				  sizeof(mixed)));

	/* A passthrough read open can resolve to its backing tmpfs file. */
	ASSERT_EQ(0, truncate(plain_output, 0));
	ASSERT_EQ((ssize_t)TEST_SIZE,
		  copy_once(src, plain_output, TEST_SIZE, 0, 0));
	ASSERT_EQ(0, compare_range(self->paths[NODE_SRC], plain_output,
				   0, 0, TEST_SIZE));

	/* Server and direct-I/O opens remain opaque to asymmetric copies. */
	ASSERT_EQ(-EXDEV, copy_once(server, plain_output, TEST_SIZE, 0, 0));
	ASSERT_EQ(-EXDEV, copy_once(direct, plain_output, TEST_SIZE, 0, 0));
	ASSERT_EQ(-EXDEV, copy_once(plain_source, server, TEST_SIZE, 0, 0));
	ASSERT_EQ(-EXDEV, copy_once(plain_source, direct, TEST_SIZE, 0, 0));

	/* Mixed opens are passthrough for reads and direct-I/O for writes. */
	ASSERT_EQ(0, truncate(plain_output, 0));
	ASSERT_EQ((ssize_t)TEST_SIZE,
		  copy_once(mixed, plain_output, TEST_SIZE, 0, 0));
	ASSERT_EQ(-EXDEV, copy_once(plain_source, mixed, TEST_SIZE, 0, 0));

	/* Exact FUSE pairs retain the FUSE method and its splice fallback. */
	ASSERT_EQ(0, truncate(self->paths[NODE_SERVER], 0));
	ASSERT_EQ((ssize_t)TEST_SIZE,
		  copy_once(src, server, TEST_SIZE, 0, 0));
	ASSERT_EQ(0, compare_range(self->paths[NODE_SRC],
				   self->paths[NODE_SERVER], 0, 0, TEST_SIZE));
	ASSERT_EQ(0, truncate(self->paths[NODE_DIRECT], 0));
	ASSERT_EQ((ssize_t)TEST_SIZE,
		  copy_once(src, direct, TEST_SIZE, 0, 0));
	ASSERT_EQ(0, compare_range(self->paths[NODE_SRC],
				   self->paths[NODE_DIRECT], 0, 0, TEST_SIZE));
	ASSERT_EQ(0, truncate(self->paths[NODE_DST], 0));
	ASSERT_EQ((ssize_t)TEST_SIZE,
		  copy_once(server, dst, TEST_SIZE, 0, 0));

	/* One inode may have a passthrough read fd and a direct write fd. */
	ASSERT_EQ(0, write_pattern(self->paths[NODE_MIXED], 0x69,
				   2 * TEST_SIZE));
	ret = copy_once(mixed, mixed, 4096, 0, TEST_SIZE);
	ASSERT_EQ((ssize_t)4096, ret);
	ASSERT_EQ(0, compare_range(self->paths[NODE_MIXED],
				   self->paths[NODE_MIXED], 0, TEST_SIZE, 4096));
}

TEST_F(fuse_passthrough, clone_method_selection)
{
	char src[PATH_MAX], dst[PATH_MAX], server[PATH_MAX];
	char direct[PATH_MAX], mixed[PATH_MAX];
	const char *plain_source = self->paths[NODE_COUNT];
	const char *plain_output = self->paths[NODE_COUNT + 1];
	struct stat st;

	ASSERT_EQ(0, logical_path(self->mountpoint, "src", src, sizeof(src)));
	ASSERT_EQ(0, logical_path(self->mountpoint, "dst", dst, sizeof(dst)));
	ASSERT_EQ(0, logical_path(self->mountpoint, "server", server,
				  sizeof(server)));
	ASSERT_EQ(0, logical_path(self->mountpoint, "direct", direct,
				  sizeof(direct)));
	ASSERT_EQ(0, logical_path(self->mountpoint, "mixed", mixed,
				  sizeof(mixed)));

	ASSERT_EQ(0, truncate(self->paths[NODE_DST], 0));
	ASSERT_EQ(-EOPNOTSUPP, clone_once(src, dst, TEST_SIZE));
	ASSERT_EQ(0, stat(self->paths[NODE_DST], &st));
	ASSERT_EQ(0, st.st_size);

	ASSERT_EQ(0, truncate(plain_output, 0));
	ASSERT_EQ(-EOPNOTSUPP, clone_once(src, plain_output, TEST_SIZE));
	ASSERT_EQ(-EOPNOTSUPP, clone_once(plain_source, dst, TEST_SIZE));
	ASSERT_EQ(-EXDEV, clone_once(server, dst, TEST_SIZE));
	ASSERT_EQ(-EXDEV, clone_once(direct, dst, TEST_SIZE));
	ASSERT_EQ(-EOPNOTSUPP, clone_once(mixed, dst, TEST_SIZE));
	ASSERT_EQ(-EXDEV, clone_once(src, server, TEST_SIZE));
	ASSERT_EQ(-EXDEV, clone_once(src, direct, TEST_SIZE));
	ASSERT_EQ(-EXDEV, clone_once(src, mixed, TEST_SIZE));
	ASSERT_EQ(0, stat(self->paths[NODE_DST], &st));
	ASSERT_EQ(0, st.st_size);
	ASSERT_EQ(0, stat(plain_output, &st));
	ASSERT_EQ(0, st.st_size);
}

TEST_F(fuse_passthrough, source_atime)
{
	const struct timespec old_times[2] = {
		{ .tv_sec = 946684800 },
		{ .tv_sec = 946684800 },
	};
	char src[PATH_MAX];
	struct stat before, after;
	const char *output = self->paths[NODE_COUNT + 1];

	ASSERT_EQ(0, logical_path(self->mountpoint, "src", src, sizeof(src)));
	ASSERT_EQ(0, utimensat(AT_FDCWD, src, old_times, 0));
	ASSERT_EQ(0, stat(src, &before));
	ASSERT_EQ(0, truncate(output, TEST_SIZE));
	ASSERT_EQ((ssize_t)8191, copy_once(src, output, 8191, 1, 1));
	ASSERT_EQ(0, stat(src, &after));
	ASSERT_GT(after.st_atim.tv_sec * 1000000000LL + after.st_atim.tv_nsec,
		  before.st_atim.tv_sec * 1000000000LL + before.st_atim.tv_nsec);
	ASSERT_EQ(0, compare_range(self->paths[NODE_SRC], output, 1, 1, 8191));
}

TEST_F(fuse_passthrough, backing_aliases)
{
	unsigned char before[4096], after[4096];
	const char *source_backing = self->paths[NODE_SRC];
	const char *destination_backing = self->paths[NODE_DST];
	char src[PATH_MAX], dst[PATH_MAX];

	ASSERT_EQ(0, logical_path(self->mountpoint, "src", src, sizeof(src)));
	ASSERT_EQ(0, logical_path(self->mountpoint, "dst", dst, sizeof(dst)));

	ASSERT_EQ(0, read_range(source_backing, before, sizeof(before), 64 * 1024));
	ASSERT_EQ(-EXDEV, copy_once(src, source_backing, sizeof(before),
				    0, 64 * 1024));
	ASSERT_EQ(0, read_range(source_backing, after, sizeof(after), 64 * 1024));
	ASSERT_EQ(0, memcmp(before, after, sizeof(before)));

	ASSERT_EQ(0, write_pattern(destination_backing, 0x6a, TEST_SIZE));
	ASSERT_EQ(0, read_range(destination_backing, before, sizeof(before),
				64 * 1024));
	ASSERT_EQ(-EXDEV, copy_once(destination_backing, dst, sizeof(before),
				    0, 64 * 1024));
	ASSERT_EQ(0, read_range(destination_backing, after, sizeof(after),
				64 * 1024));
	ASSERT_EQ(0, memcmp(before, after, sizeof(before)));
}

TEST_F(fuse_passthrough, destination_attributes)
{
	const struct timespec old_times[2] = {
		{ .tv_sec = 946684800 },
		{ .tv_sec = 946684800 },
	};
	const char *source = self->paths[NODE_COUNT];
	const char *backing = self->paths[NODE_DST];
	char dst[PATH_MAX];
	struct stat logical_stat, backing_stat;
	loff_t pos_in = 0, pos_out = 4096;
	int source_fd, destination_fd, logical_read_fd, probe_fd;
	ssize_t ret;

	ASSERT_EQ(0, logical_path(self->mountpoint, "dst", dst, sizeof(dst)));
	destination_fd = open(dst, O_WRONLY | O_TRUNC | O_CLOEXEC);
	ASSERT_GE(destination_fd, 0);
	source_fd = open(source, O_RDONLY | O_CLOEXEC);
	ASSERT_GE(source_fd, 0);
	ret = copy_file_range(source_fd, &pos_in, destination_fd, &pos_out,
			      TEST_SIZE, 0);
	ASSERT_EQ((ssize_t)TEST_SIZE, ret);
	ASSERT_EQ(0, fstat(destination_fd, &logical_stat));
	ASSERT_EQ((off_t)(TEST_SIZE + 4096), logical_stat.st_size);
	ASSERT_EQ(0, stat(backing, &backing_stat));
	ASSERT_EQ(logical_stat.st_size, backing_stat.st_size);
	close(source_fd);
	close(destination_fd);
	ASSERT_EQ(0, compare_range(source, backing, 0, 4096, TEST_SIZE));

	/* A short write must not import a larger externally changed backing size. */
	ASSERT_EQ(0, truncate(dst, 0));
	logical_read_fd = open(dst, O_RDONLY | O_CLOEXEC);
	ASSERT_GE(logical_read_fd, 0);
	destination_fd = open(dst, O_WRONLY | O_CLOEXEC);
	ASSERT_GE(destination_fd, 0);
	source_fd = open(source, O_RDONLY | O_CLOEXEC);
	ASSERT_GE(source_fd, 0);
	ASSERT_EQ(0, truncate(backing, 2 * TEST_SIZE));
	pos_in = 0;
	pos_out = 4096;
	ASSERT_EQ((ssize_t)4096,
		  copy_file_range(source_fd, &pos_in, destination_fd, &pos_out,
				  4096, 0));
	probe_fd = open(self->paths[NODE_COUNT + 1], O_WRONLY | O_TRUNC | O_CLOEXEC);
	ASSERT_GE(probe_fd, 0);
	pos_in = TEST_SIZE;
	pos_out = 0;
	ASSERT_EQ((ssize_t)0,
		  copy_file_range(logical_read_fd, &pos_in, probe_fd, &pos_out,
				  4096, 0));
	close(probe_fd);
	close(source_fd);
	close(destination_fd);
	close(logical_read_fd);

	/* A completed backing write updates the logical mtime and ctime. */
	ASSERT_EQ(0, truncate(dst, 0));
	ASSERT_EQ(0, utimensat(AT_FDCWD, dst, old_times, 0));
	ASSERT_EQ((ssize_t)TEST_SIZE,
		  copy_once(source, dst, TEST_SIZE, 0, 0));
	ASSERT_EQ(0, stat(dst, &logical_stat));
	ASSERT_EQ(0, stat(backing, &backing_stat));
	ASSERT_GT(logical_stat.st_mtim.tv_sec, old_times[1].tv_sec);
	ASSERT_EQ(logical_stat.st_mtim.tv_sec, backing_stat.st_mtim.tv_sec);
	ASSERT_EQ(logical_stat.st_mtim.tv_nsec, backing_stat.st_mtim.tv_nsec);
	ASSERT_EQ(logical_stat.st_ctim.tv_sec, backing_stat.st_ctim.tv_sec);
	ASSERT_EQ(logical_stat.st_ctim.tv_nsec, backing_stat.st_ctim.tv_nsec);
}

TEST_F(fuse_passthrough, destination_killpriv)
{
	const char *source = self->paths[NODE_COUNT];
	const char *backing = self->paths[NODE_DST];
	char dst[PATH_MAX];
	struct stat logical_stat, backing_stat;
	size_t len = TEST_SIZE - 1;

	ASSERT_EQ(0, logical_path(self->mountpoint, "dst", dst, sizeof(dst)));
	ASSERT_EQ(0, truncate(dst, TEST_SIZE));
	ASSERT_EQ(0, chmod(dst, 06777));
	ASSERT_EQ(0, stat(dst, &logical_stat));
	ASSERT_EQ(06000, logical_stat.st_mode & 06000);
	ASSERT_EQ(0, stat(backing, &backing_stat));
	ASSERT_EQ(06000, backing_stat.st_mode & 06000);

	ASSERT_EQ(0, copy_as_nobody(source, dst, len, 1, 1));
	ASSERT_EQ(0, stat(dst, &logical_stat));
	ASSERT_EQ(0, stat(backing, &backing_stat));
	ASSERT_EQ(0, logical_stat.st_mode & 06000);
	ASSERT_EQ(0, backing_stat.st_mode & 06000);
	ASSERT_EQ(0, compare_range(source, backing, 1, 1, len));
}

TEST_F(fuse_passthrough, task_accounting_once)
{
	const char *source = self->paths[NODE_COUNT];
	const char *output = self->paths[NODE_COUNT + 1];
	char src[PATH_MAX], dst[PATH_MAX];
	long long before, after;
	loff_t pos_in, pos_out;
	int source_fd, destination_fd;
	ssize_t ret;

	ASSERT_EQ(0, logical_path(self->mountpoint, "src", src, sizeof(src)));
	ASSERT_EQ(0, logical_path(self->mountpoint, "dst", dst, sizeof(dst)));

	ASSERT_EQ(0, truncate(output, 0));
	source_fd = open(src, O_RDONLY | O_CLOEXEC);
	ASSERT_GE(source_fd, 0);
	destination_fd = open(output, O_WRONLY | O_CLOEXEC);
	ASSERT_GE(destination_fd, 0);
	before = read_proc_io("syscw");
	ASSERT_GE(before, 0);
	pos_in = 0;
	pos_out = 0;
	ret = copy_file_range(source_fd, &pos_in, destination_fd, &pos_out,
			      TEST_SIZE, 0);
	after = read_proc_io("syscw");
	ASSERT_EQ((ssize_t)TEST_SIZE, ret);
	ASSERT_EQ(before + 1, after);
	close(destination_fd);
	close(source_fd);

	ASSERT_EQ(0, truncate(self->paths[NODE_DST], 0));
	source_fd = open(source, O_RDONLY | O_CLOEXEC);
	ASSERT_GE(source_fd, 0);
	destination_fd = open(dst, O_WRONLY | O_CLOEXEC);
	ASSERT_GE(destination_fd, 0);
	before = read_proc_io("syscw");
	ASSERT_GE(before, 0);
	pos_in = 0;
	pos_out = 0;
	ret = copy_file_range(source_fd, &pos_in, destination_fd, &pos_out,
			      TEST_SIZE, 0);
	after = read_proc_io("syscw");
	ASSERT_EQ((ssize_t)TEST_SIZE, ret);
	ASSERT_EQ(before + 1, after);
	close(destination_fd);
	close(source_fd);
}

TEST_HARNESS_MAIN
