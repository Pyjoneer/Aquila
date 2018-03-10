#include <core/system.h>

#include <ds/ring.h>
#include <bits/fcntl.h>

#include <fs/pipe.h>
#include <fs/posix.h>

static ssize_t pipefs_read(struct inode *node, off_t offset __unused, size_t size, void *buf)
{
    struct pipe *pipe = node->p;
	return ring_read(pipe->ring, size, buf);
}

static ssize_t pipefs_write(struct inode *node, off_t offset __unused, size_t size, void *buf)
{
    struct pipe *pipe = node->p;
	return ring_write(pipe->ring, size, buf);
}

static int pipefs_can_read(struct file *file, size_t size)
{
    struct inode *node = file->node;
    struct pipe *pipe = node->p;
	return size <= ring_available(pipe->ring);
}

static int pipefs_can_write(struct file *file, size_t size)
{
    struct inode *node = file->node;
    struct pipe *pipe = node->p;
	return size >= pipe->ring->size - ring_available(pipe->ring);
}

static struct pipe *pipefs_mkpipe()
{
    struct pipe *p = kmalloc(sizeof(struct pipe));
    memset(p, 0, sizeof(struct pipe));
    p->ring = new_ring(PIPE_BUF_LEN);
    return p;
}

int pipefs_pipe(struct file *read, struct file *write)
{
    struct pipe *pipe = pipefs_mkpipe();
    read->node = kmalloc(sizeof(struct inode));
    write->node = kmalloc(sizeof(struct inode));

    read->node->read_queue = new_queue();
    write->node->write_queue = read->node->read_queue;

    read->node->fs  = &pipefs;
    write->node->fs = &pipefs;
    read->node->p  = pipe;
    write->node->p = pipe;

    write->flags = O_WRONLY;
    return 0;
}

struct fs pipefs = {
    .name = "pipefs",

    .iops = {
        .read  = pipefs_read,
        .write = pipefs_write,
    },

	.fops = {
		.read      = posix_file_read,
        .write     = posix_file_write,
        .can_read  = pipefs_can_read,
        .can_write = pipefs_can_write,
        .eof       = __vfs_never
	},
};
