#undef TRACE_SYSTEM
#define TRACE_SYSTEM android_fs

#ifndef MAX_TRACE_PATHBUF_LEN
#define MAX_TRACE_PATHBUF_LEN	256
#endif

#if !defined(_TRACE_ANDROID_FS_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_ANDROID_FS_H

#include <linux/tracepoint.h>
#if IS_ENABLED(CONFIG_ANDROID_FS_TRACE)
#include <trace/events/android_fs_template.h>

DEFINE_EVENT(android_fs_data_start_template, android_fs_dataread_start,
	TP_PROTO(struct inode *inode, loff_t offset, int bytes,
		 pid_t pid, char *pathname, char *command),
	TP_ARGS(inode, offset, bytes, pid, pathname, command));

DEFINE_EVENT(android_fs_data_end_template, android_fs_dataread_end,
	TP_PROTO(struct inode *inode, loff_t offset, int bytes),
	TP_ARGS(inode, offset, bytes));

DEFINE_EVENT(android_fs_data_start_template, android_fs_datawrite_start,
	TP_PROTO(struct inode *inode, loff_t offset, int bytes,
		 pid_t pid, char *pathname, char *command),
	TP_ARGS(inode, offset, bytes, pid, pathname, command));

DEFINE_EVENT(android_fs_data_end_template, android_fs_datawrite_end,
	TP_PROTO(struct inode *inode, loff_t offset, int bytes),
	     TP_ARGS(inode, offset, bytes));

DEFINE_EVENT(android_fs_fsync_start_template, android_fs_fsync_start,
	TP_PROTO(struct inode *inode,
		 pid_t pid, char *pathname, char *command),
	TP_ARGS(inode, pid, pathname, command));

DEFINE_EVENT(android_fs_data_end_template, android_fs_fsync_end,
	TP_PROTO(struct inode *inode, loff_t offset, int bytes),
	     TP_ARGS(inode, offset, bytes));

#else

#define trace_android_fs_dataread_start_enabled()	(false)
#define trace_android_fs_dataread_end_enabled()	(false)
#define trace_android_fs_datawrite_start_enabled()	(false)
#define trace_android_fs_datawrite_end_enabled()	(false)
#define trace_android_fs_fsync_start_enabled()	(false)
#define trace_android_fs_fsync_end_enabled()	(false)

#define trace_android_fs_dataread_start(...)		do { } while (0)
#define trace_android_fs_dataread_end(...)		do { } while (0)
#define trace_android_fs_datawrite_start(...)		do { } while (0)
#define trace_android_fs_datawrite_end(...)		do { } while (0)
#define trace_android_fs_fsync_start(...)		do { } while (0)
#define trace_android_fs_fsync_end(...)			do { } while (0)

#endif
#endif /* _TRACE_ANDROID_FS_H */

/* This part must be outside protection */
#if IS_ENABLED(CONFIG_ANDROID_FS_TRACE)
#include <trace/define_trace.h>
#endif

#if IS_ENABLED(CONFIG_ANDROID_FS_TRACE) && !defined(ANDROID_FSTRACE_GET_PATHNAME)
#define ANDROID_FSTRACE_GET_PATHNAME

static inline char *
android_fstrace_get_pathname(char *buf, int buflen, struct inode *inode)
{
	char *path;
	struct dentry *d;

	/*
	 * d_obtain_alias() will either iput() if it locates an existing
	 * dentry or transfer the reference to the new dentry created.
	 * So get an extra reference here.
	 */
	ihold(inode);
	d = d_obtain_alias(inode);
	if (likely(!IS_ERR(d))) {
		path = dentry_path_raw(d, buf, buflen);
		if (unlikely(IS_ERR(path))) {
			strcpy(buf, "ERROR");
			path = buf;
		}
		dput(d);
	} else {
		strcpy(buf, "ERROR");
		path = buf;
	}
	return path;
}
#elif !IS_ENABLED(CONFIG_ANDROID_FS_TRACE)
static inline char *
android_fstrace_get_pathname(char *buf, int buflen, struct inode *inode)
{
	(void)buf;
	(void)buflen;
	(void)inode;
	return NULL;
}
#endif
