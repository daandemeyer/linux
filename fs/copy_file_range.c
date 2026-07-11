// SPDX-License-Identifier: GPL-2.0

#include <linux/compat.h>
#include <linux/backing-file.h>
#include <linux/cred.h>
#include <linux/export.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/fsnotify.h>
#include <linux/sched/xacct.h>
#include <linux/splice.h>

#include <kunit/static_stub.h>
#include <kunit/visibility.h>

#include "internal.h"

#define COPY_FILE_RANGE_MAX_CHAIN_FILES	(FILESYSTEM_MAX_STACK_DEPTH + 1)

struct copy_file_range_chain {
	struct file *files[COPY_FILE_RANGE_MAX_CHAIN_FILES];
	unsigned int nr_files;
};

struct copy_file_range_context {
	struct copy_file_range_chain source;
	struct copy_file_range_chain destination;
	loff_t pos_in;
	loff_t pos_out;
	bool use_splice;
	bool splice_attempted;
	bool uses_terminal_opt_in;
};

static void copy_file_range_chain_reset(struct copy_file_range_chain *chain)
{
	while (chain->nr_files > 1)
		fput(chain->files[--chain->nr_files]);
}

static void copy_file_range_context_init(struct copy_file_range_context *ctx,
					 struct file *file_in, loff_t pos_in,
				 struct file *file_out, loff_t pos_out,
				 unsigned int flags)
{
	ctx->source.files[0] = file_in;
	ctx->source.nr_files = 1;
	ctx->destination.files[0] = file_out;
	ctx->destination.nr_files = 1;
	ctx->pos_in = pos_in;
	ctx->pos_out = pos_out;
	ctx->use_splice = flags & COPY_FILE_SPLICE;
}

static void copy_file_range_context_cleanup(struct copy_file_range_context *ctx)
{
	copy_file_range_chain_reset(&ctx->source);
	copy_file_range_chain_reset(&ctx->destination);
}

DEFINE_FREE(copy_file_range_context, struct copy_file_range_context,
	    copy_file_range_context_cleanup(&_T))

VISIBLE_IF_KUNIT
int copy_file_range_verify_backing_area(int read_write, struct file *file,
					const loff_t *ppos, size_t count)
{
	KUNIT_STATIC_STUB_REDIRECT(copy_file_range_verify_backing_area, read_write,
				   file, ppos, count);

	return rw_verify_area(read_write, file, ppos, count);
}

static struct file *
copy_file_range_resolve_one(const struct copy_file_range_layer_operations *ops,
			    struct file *file, enum copy_file_range_role role,
			    enum copy_file_range_resolve_mode mode, bool nested)
{
	struct file *next;

	if (nested) {
		scoped_with_creds(file->f_cred)
			next = ops->resolve(file, role, mode);
	} else {
		next = ops->resolve(file, role, mode);
	}
	return next;
}

static int
copy_file_range_resolve_next(struct copy_file_range_chain *chain,
			     enum copy_file_range_role role,
			     enum copy_file_range_resolve_mode mode)
{
	struct file *file = chain->files[chain->nr_files - 1];
	const struct copy_file_range_layer_operations *ops;

	ops = file->f_op->copy_file_range_layer_ops;
	if (!ops)
		return 0;
	if (chain->nr_files == COPY_FILE_RANGE_MAX_CHAIN_FILES)
		return -ELOOP;
	if (WARN_ON_ONCE(!ops->resolve))
		return -EIO;
	if (role == COPY_FILE_RANGE_DESTINATION &&
	    WARN_ON_ONCE(!ops->prepare_write || !ops->finish_write))
		return -EIO;

	struct file *next __free(fput) =
		copy_file_range_resolve_one(ops, file, role, mode,
					    chain->nr_files > 1);

	if (IS_ERR(next))
		return PTR_ERR(next);
	if (WARN_ON_ONCE(!next))
		return -EIO;
	if (!(next->f_mode & FMODE_BACKING))
		return -EXDEV;
	if (!S_ISREG(file_inode(next)->i_mode))
		return -EINVAL;
	if (role == COPY_FILE_RANGE_SOURCE) {
		if (!(next->f_mode & FMODE_READ))
			return -EBADF;
	} else if (!(next->f_mode & FMODE_WRITE)) {
		return -EBADF;
	}
	/* Stack depth must decrease, which also prevents cycles. */
	if (file_inode(file)->i_sb->s_stack_depth <=
	    file_inode(next)->i_sb->s_stack_depth)
		return -EXDEV;

	chain->files[chain->nr_files] = no_free_ptr(next);
	chain->nr_files++;
	return 1;
}

static int
copy_file_range_resolve_chain(struct copy_file_range_chain *chain,
			      enum copy_file_range_role role,
			      enum copy_file_range_resolve_mode mode)
{
	int ret;

	while ((ret = copy_file_range_resolve_next(chain, role, mode)) > 0)
		;
	return ret;
}

static struct file *
copy_file_range_chain_terminal(const struct copy_file_range_chain *chain)
{
	return chain->files[chain->nr_files - 1];
}

static bool copy_file_range_has_matching_terminal_method(const struct copy_file_range_context *ctx)
{
	struct file *file_in = copy_file_range_chain_terminal(&ctx->source);
	struct file *file_out = copy_file_range_chain_terminal(&ctx->destination);

	return file_out->f_op->copy_file_range &&
	       file_in->f_op->copy_file_range == file_out->f_op->copy_file_range;
}

static bool copy_file_range_paired_layers(struct file *file_in,
					  struct file *file_out)
{
	const struct copy_file_range_layer_operations *ops;

	ops = file_in->f_op->copy_file_range_layer_ops;
	return ops && ops == file_out->f_op->copy_file_range_layer_ops &&
	       !file_in->f_op->copy_file_range &&
	       !file_out->f_op->copy_file_range;
}

/* Do not resolve below a copy method reached through paired layers. */
static int
copy_file_range_resolve_paired_prefix(struct copy_file_range_context *ctx,
				      enum copy_file_range_resolve_mode mode)
{
	for (;;) {
		struct file *file_in = copy_file_range_chain_terminal(&ctx->source);
		struct file *file_out = copy_file_range_chain_terminal(&ctx->destination);
		int ret;

		if (copy_file_range_has_matching_terminal_method(ctx))
			return 1;
		if (!copy_file_range_paired_layers(file_in, file_out))
			return 0;

		ret = copy_file_range_resolve_next(&ctx->source,
						   COPY_FILE_RANGE_SOURCE, mode);
		if (ret <= 0)
			return ret ?: -EIO;
		ret = copy_file_range_resolve_next(&ctx->destination,
						   COPY_FILE_RANGE_DESTINATION,
						   mode);
		if (ret <= 0)
			return ret ?: -EIO;
	}
}

static int copy_file_range_resolve(struct copy_file_range_context *ctx,
				   enum copy_file_range_resolve_mode mode)
{
	int ret;

	copy_file_range_chain_reset(&ctx->source);
	copy_file_range_chain_reset(&ctx->destination);

	ret = copy_file_range_resolve_paired_prefix(ctx, mode);
	if (ret < 0)
		goto err;
	if (ret)
		return 0;

	ret = copy_file_range_resolve_chain(&ctx->source,
					    COPY_FILE_RANGE_SOURCE, mode);
	if (ret)
		goto err;
	ret = copy_file_range_resolve_chain(&ctx->destination,
					    COPY_FILE_RANGE_DESTINATION,
					       mode);
	if (!ret)
		return 0;
err:
	copy_file_range_chain_reset(&ctx->source);
	copy_file_range_chain_reset(&ctx->destination);
	return ret;
}

static bool copy_file_range_has_backing_files(const struct copy_file_range_context *ctx)
{
	return ctx->source.nr_files > 1 || ctx->destination.nr_files > 1;
}

static bool
copy_file_range_terminal_route_compatible(struct file *file_in,
					  struct file *file_out)
{
	if (file_out->f_op->copy_file_range)
		return file_in->f_op->copy_file_range ==
		       file_out->f_op->copy_file_range;
	return file_inode(file_in)->i_sb == file_inode(file_out)->i_sb;
}

static bool
copy_file_range_logical_route_compatible(const struct copy_file_range_context *ctx,
					 bool paired_layers)
{
	struct file *file_in = ctx->source.files[0];
	struct file *file_out = ctx->destination.files[0];

	return ctx->use_splice || paired_layers ||
	       copy_file_range_terminal_route_compatible(file_in, file_out);
}

static bool
copy_file_range_paired_route(const struct copy_file_range_context *ctx)
{
	const struct copy_file_range_chain *source = &ctx->source;
	const struct copy_file_range_chain *destination = &ctx->destination;
	unsigned int i;

	if (!copy_file_range_has_backing_files(ctx))
		return true;
	if (source->nr_files != destination->nr_files)
		return false;

	for (i = 0; i + 1 < source->nr_files; i++) {
		if (!copy_file_range_paired_layers(source->files[i],
						   destination->files[i]))
			return false;
	}

	return true;
}

static int copy_file_range_check_aliases(const struct copy_file_range_context *ctx)
{
	const struct copy_file_range_chain *source = &ctx->source;
	const struct copy_file_range_chain *destination = &ctx->destination;
	unsigned int i, j;

	if (file_inode(source->files[0]) == file_inode(destination->files[0])) {
		if (source->nr_files != destination->nr_files)
			return -EXDEV;
		for (i = 0; i < source->nr_files; i++)
			if (file_inode(source->files[i]) !=
			    file_inode(destination->files[i]))
				return -EXDEV;
		return 0;
	}

	for (i = 0; i < source->nr_files; i++)
		for (j = 0; j < destination->nr_files; j++)
			if (file_inode(source->files[i]) ==
			    file_inode(destination->files[j]))
				return -EXDEV;

	return 0;
}

static int copy_file_range_check_terminal_route(struct copy_file_range_context *ctx)
{
	struct file *file_in = copy_file_range_chain_terminal(&ctx->source);
	struct file *file_out = copy_file_range_chain_terminal(&ctx->destination);

	ctx->uses_terminal_opt_in = false;
	if (ctx->use_splice)
		return 0;
	if (!copy_file_range_terminal_route_compatible(file_in, file_out))
		return -EXDEV;
	if (!copy_file_range_paired_route(ctx)) {
		if (file_out->f_op->copy_file_range ||
		    file_in->f_op != file_out->f_op ||
		    !(file_in->f_op->fop_flags & FOP_COPY_FILE_RANGE_BACKING))
			return -EXDEV;
		ctx->uses_terminal_opt_in = true;
	}

	return 0;
}

static int
copy_file_range_resolve_route(struct copy_file_range_context *ctx,
			      enum copy_file_range_resolve_mode mode)
{
	int ret;

	ret = copy_file_range_resolve(ctx, mode);
	if (!ret)
		ret = copy_file_range_check_terminal_route(ctx);
	if (!ret)
		ret = copy_file_range_check_aliases(ctx);
	return ret;
}

static int copy_file_range_checks(struct file *file_in, loff_t pos_in,
				  struct file *file_out, loff_t pos_out,
				  size_t *req_count)
{
	struct inode *inode_in = file_inode(file_in);
	struct inode *inode_out = file_inode(file_out);
	u64 count = *req_count;
	loff_t size_in;
	int ret;

	if (IS_IMMUTABLE(inode_out))
		return -EPERM;
	if (IS_SWAPFILE(inode_in) || IS_SWAPFILE(inode_out))
		return -ETXTBSY;
	if (pos_in + count < pos_in || pos_out + count < pos_out)
		return -EOVERFLOW;

	size_in = i_size_read(inode_in);
	if (pos_in >= size_in)
		count = 0;
	else
		count = min(count, size_in - (u64)pos_in);

	ret = generic_write_check_limits(file_out, pos_out, &count);
	if (ret)
		return ret;

	if (inode_in == inode_out && pos_out + count > pos_in &&
	    pos_out < pos_in + count)
		return -EINVAL;

	*req_count = count;
	return 0;
}

static int copy_file_range_backing_checks(struct copy_file_range_context *ctx,
					  size_t len)
{
	struct copy_file_range_chain *source = &ctx->source;
	struct copy_file_range_chain *destination = &ctx->destination;
	unsigned int i, nr_files;
	int ret;

	if (destination->files[0]->f_flags & O_APPEND)
		return -EBADF;

	nr_files = max(source->nr_files, destination->nr_files);
	for (i = 1; i < nr_files; i++) {
		struct file *file_in = i < source->nr_files ?
				       source->files[i] : NULL;
		struct file *file_out = i < destination->nr_files ?
					destination->files[i] : NULL;

		if (file_out && IS_IMMUTABLE(file_inode(file_out)))
			return -EPERM;
		if ((file_in && IS_SWAPFILE(file_inode(file_in))) ||
		    (file_out && IS_SWAPFILE(file_inode(file_out))))
			return ctx->uses_terminal_opt_in ? -EXDEV : -ETXTBSY;
		if (file_in) {
			loff_t size_in = i_size_read(file_inode(file_in));

			if (ctx->pos_in >= size_in ||
			    len > size_in - (u64)ctx->pos_in)
				return -EXDEV;
		}
		if (file_out) {
			loff_t count = len;

			if (file_out->f_flags & O_APPEND)
				return -EBADF;
			ret = generic_write_check_limits(file_out, ctx->pos_out,
							 &count);
			if (ret)
				return ret;
			if (count != len)
				return -EXDEV;
		}
	}

	return 0;
}

static int copy_file_range_verify_backing_areas(struct copy_file_range_context *ctx,
						size_t len)
{
	struct copy_file_range_chain *source = &ctx->source;
	struct copy_file_range_chain *destination = &ctx->destination;
	loff_t pos_in = ctx->pos_in;
	loff_t pos_out = ctx->pos_out;
	unsigned int i, nr_files;
	int ret;

	nr_files = max(source->nr_files, destination->nr_files);
	for (i = 1; i < nr_files; i++) {
		if (i < source->nr_files) {
			struct file *file = source->files[i];

			scoped_with_creds(file->f_cred)
				ret = copy_file_range_verify_backing_area(READ, file,
									  &pos_in, len);
			if (ret)
				return ret;
		}
		if (i < destination->nr_files) {
			struct file *file = destination->files[i];

			scoped_with_creds(file->f_cred)
				ret = copy_file_range_verify_backing_area(WRITE, file,
									  &pos_out, len);
			if (ret)
				return ret;
		}
	}

	return 0;
}

static int
copy_file_range_revalidate_chain(const struct copy_file_range_chain *chain,
				 enum copy_file_range_role role)
{
	unsigned int i;

	for (i = 0; i + 1 < chain->nr_files; i++) {
		const struct copy_file_range_layer_operations *ops;

		ops = chain->files[i]->f_op->copy_file_range_layer_ops;
		if (!ops)
			return -EXDEV;

		struct file *next __free(fput) =
			copy_file_range_resolve_one(ops, chain->files[i], role,
						    COPY_FILE_RANGE_RESOLVE_CACHED,
						    i > 0);

		if (IS_ERR(next))
			return PTR_ERR(next) == -EAGAIN ? -EXDEV : PTR_ERR(next);
		if (WARN_ON_ONCE(!next))
			return -EIO;
		if (next != chain->files[i + 1])
			return -EXDEV;
	}

	return 0;
}

static int
copy_file_range_revalidate_destination(const struct copy_file_range_context *ctx)
{
	return copy_file_range_revalidate_chain(&ctx->destination,
						   COPY_FILE_RANGE_DESTINATION);
}

static ssize_t copy_file_range_splice(struct copy_file_range_context *ctx,
				      struct file *file_in,
				      struct file *file_out, size_t len)
{
	loff_t pos_in = ctx->pos_in;
	loff_t pos_out = ctx->pos_out;

	/* Never hold the terminal freeze while splice reads the source. */
	ctx->splice_attempted = true;
	return do_splice_direct(file_in, &pos_in, file_out, &pos_out, len, 0);
}

/*
 * Each nonterminal frame freezes and prepares one destination layer, recurses
 * under the next edge's credentials, then finishes and thaws in reverse order.
 * Keeping that recursion here prevents a source wrapper from re-entering
 * vfs_copy_file_range() and acquiring the terminal destination freeze twice.
 */
static ssize_t copy_file_range_execute(struct copy_file_range_context *ctx,
				       unsigned int layer, size_t len)
{
	struct copy_file_range_chain *chain = &ctx->destination;
	const struct copy_file_range_layer_operations *ops;
	struct file *file;
	ssize_t ret;

	if (layer + 1 == chain->nr_files) {
		struct file *file_in = copy_file_range_chain_terminal(&ctx->source);
		struct file *file_out = copy_file_range_chain_terminal(chain);

		if (ctx->use_splice ||
		    (!file_out->f_op->copy_file_range &&
		     !file_in->f_op->remap_file_range))
			return copy_file_range_splice(ctx, file_in, file_out, len);

		if (ctx->uses_terminal_opt_in &&
		    file_in->f_op->remap_file_range) {
			unsigned long blocksize =
				file_inode(file_out)->i_sb->s_blocksize;

			if (!IS_ALIGNED(ctx->pos_in, blocksize) ||
			    !IS_ALIGNED(ctx->pos_out, blocksize)) {
				ctx->use_splice = true;
				return copy_file_range_splice(ctx, file_in, file_out,
							      len);
			}
		}

		scoped_guard(super_write, file_inode(file_out)->i_sb) {
			if (file_out->f_op->copy_file_range) {
				ret = file_out->f_op->copy_file_range(file_in,
						ctx->pos_in, file_out, ctx->pos_out,
						len, 0);
			} else {
				ret = file_in->f_op->remap_file_range(file_in,
						ctx->pos_in, file_out, ctx->pos_out,
						len, REMAP_FILE_CAN_SHORTEN);
				if (!ret ||
				    (ret < 0 && !ctx->uses_terminal_opt_in))
					ctx->use_splice = true;
			}
		}
		if (ctx->use_splice)
			return copy_file_range_splice(ctx, file_in, file_out, len);
		return ret;
	}

	file = chain->files[layer];
	ops = file->f_op->copy_file_range_layer_ops;
	scoped_guard(super_write, file_inode(file)->i_sb) {
		ret = ops->prepare_write(file, chain->files[layer + 1]);
		if (!ret) {
			scoped_with_creds(chain->files[layer + 1]->f_cred)
				ret = copy_file_range_execute(ctx, layer + 1, len);
			ops->finish_write(file, chain->files[layer + 1],
					  ctx->pos_out, ret);
		}
	}
	return ret;
}

static void copy_file_range_sync_source_access(const struct copy_file_range_context *ctx)
{
	unsigned int i = ctx->source.nr_files - 1;

	while (i--) {
		struct file *file = ctx->source.files[i];
		const struct copy_file_range_layer_operations *ops;

		ops = file->f_op->copy_file_range_layer_ops;
		if (ops->sync_source_access)
			ops->sync_source_access(file);
	}
}

static void copy_file_range_notify_backing(const struct copy_file_range_context *ctx)
{
	unsigned int depth, nr_files;

	nr_files = max(ctx->source.nr_files, ctx->destination.nr_files);
	for (depth = 1; depth < nr_files; depth++) {
		if (depth < ctx->source.nr_files) {
			unsigned int i = ctx->source.nr_files - depth;

			fsnotify_access(ctx->source.files[i]);
		}
		if (depth < ctx->destination.nr_files) {
			unsigned int i = ctx->destination.nr_files - depth;

			fsnotify_modify(ctx->destination.files[i]);
		}
	}
}

static ssize_t copy_file_range_complete(const struct copy_file_range_context *ctx,
					struct file *file_in,
					struct file *file_out, ssize_t ret)
{
	if (ctx->splice_attempted)
		copy_file_range_sync_source_access(ctx);
	if (ret > 0) {
		copy_file_range_notify_backing(ctx);
		fsnotify_access(file_in);
		add_rchar(current, ret);
		fsnotify_modify(file_out);
		add_wchar(current, ret);
	}
	inc_syscr(current);
	inc_syscw(current);
	return ret;
}

ssize_t vfs_copy_file_range(struct file *file_in, loff_t pos_in,
			    struct file *file_out, loff_t pos_out,
			    size_t len, unsigned int flags)
{
	struct copy_file_range_context ctx __free(copy_file_range_context) = {};
	bool logical_route_compatible;
	bool paired_logical_layers;
	bool needs_may_open_resolution = false;
	ssize_t ret;

	if (flags & ~COPY_FILE_SPLICE)
		return -EINVAL;

	copy_file_range_context_init(&ctx, file_in, pos_in, file_out, pos_out, flags);
	ret = generic_file_rw_checks(file_in, file_out);
	if (ret)
		return ret;

	paired_logical_layers = copy_file_range_paired_layers(file_in, file_out);
	logical_route_compatible =
		copy_file_range_logical_route_compatible(&ctx, paired_logical_layers);
	if (!logical_route_compatible) {
		ret = copy_file_range_resolve_route(&ctx, COPY_FILE_RANGE_RESOLVE_CACHED);
		if (ret == -EAGAIN) {
			needs_may_open_resolution = true;
			if (!len || pos_in >= i_size_read(file_inode(file_in)))
				return -EXDEV;
		} else if (ret) {
			return ret;
		}
	}

	ret = copy_file_range_checks(file_in, pos_in, file_out, pos_out, &len);
	if (ret)
		return ret;
	if (needs_may_open_resolution && !len)
		return -EXDEV;
	ret = rw_verify_area(READ, file_in, &pos_in, len);
	if (ret)
		return ret;
	ret = rw_verify_area(WRITE, file_out, &pos_out, len);
	if (ret)
		return ret;
	if (!len)
		return 0;

	if (!ctx.use_splice &&
	    (!logical_route_compatible || paired_logical_layers)) {
		ret = copy_file_range_resolve_route(&ctx,
						    COPY_FILE_RANGE_RESOLVE_MAY_OPEN);
		if (ret)
			return copy_file_range_complete(&ctx, file_in, file_out, ret);
	}

	if (ctx.use_splice ||
	    !copy_file_range_chain_terminal(&ctx.destination)->f_op->copy_file_range ||
	    in_compat_syscall())
		len = min_t(size_t, MAX_RW_COUNT, len);

	/*
	 * Permission events may change a stacked destination.  Run all of them
	 * before the first write freeze, then revalidate before execution.
	 */
	if (copy_file_range_has_backing_files(&ctx)) {
		ret = copy_file_range_backing_checks(&ctx, len);
		if (ret)
			return copy_file_range_complete(&ctx, file_in, file_out, ret);
		ret = copy_file_range_verify_backing_areas(&ctx, len);
		if (ret)
			return copy_file_range_complete(&ctx, file_in, file_out, ret);
		ret = copy_file_range_revalidate_destination(&ctx);
		if (ret)
			return copy_file_range_complete(&ctx, file_in, file_out, ret);
		ret = copy_file_range_backing_checks(&ctx, len);
		if (ret)
			return copy_file_range_complete(&ctx, file_in, file_out, ret);
	}

	ret = copy_file_range_execute(&ctx, 0, len);
	return copy_file_range_complete(&ctx, file_in, file_out, ret);
}
EXPORT_SYMBOL(vfs_copy_file_range);
