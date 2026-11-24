#ifndef FS_PROC_H
#define FS_PROC_H

#include "fs/stat.h"
#include "misc.h"

struct proc_entry {
    struct proc_dir_entry *meta;
    unsigned long index;
    char **child_names;
    char *name;
    pid_t_ pid;
    sdword_t fd; // typedef might not have been read yet
};

struct proc_data {
    char *data;
    size_t size;
    size_t capacity;
    // What was to go in could not have the memory it needed: the content is
    // not whole, and proc_refresh_data gives ENOMEM, not the rest of it
    bool failed;
};

struct proc_dir_entry {
    const char *name;
    mode_t_ mode;
    
    // file with dynamic name
    void (*getname)(struct proc_entry *entry, char *buf);

    // file with custom show data function
    int (*show)(struct proc_entry *entry, struct proc_data *data);
    
    // file with a custom write function
    int (*update)(struct proc_entry *entry, struct proc_data *data);
    
    // file with custom pread functionality
    ssize_t (*pread)(struct proc_entry *entry, struct proc_data *data, off_t off);
    
    // file with custom pwrite functionality
    ssize_t (*pwrite)(struct proc_entry *entry, struct proc_data *data, off_t off);

    // symlink
    int (*readlink)(struct proc_entry *entry, char *buf);
    
    // remove
    int (*unlink)(struct proc_entry *entry);

    // directory with static list
    struct proc_children *children;

    // directory with dynamic contents
    bool (*readdir)(struct proc_entry *entry, unsigned long *index, struct proc_entry *next_entry);
    // and with entries found by name that readdir does not list (the root's
    // threads): true, with the entry and its index as readdir would say
    // them, if it finds name; else what readdir lists is looked through
    bool (*lookup)(struct proc_entry *entry, const char *name, unsigned long *index, struct proc_entry *next_entry);

    struct proc_dir_entry *parent;
    int inode;
};

struct proc_children {
    size_t count;
    struct proc_dir_entry entries[];
};

#define PROC_CHILDREN(...) { .count = sizeof((struct proc_dir_entry[])__VA_ARGS__) / sizeof(struct proc_dir_entry), .entries = __VA_ARGS__ }

extern struct proc_dir_entry proc_root;
extern struct proc_dir_entry proc_pid;
extern struct proc_children proc_ish_children;

mode_t_ proc_entry_mode(struct proc_entry *entry);
void proc_entry_getname(struct proc_entry *entry, char *buf);
int proc_entry_stat(struct proc_entry *entry, struct statbuf *stat);
void proc_entry_cleanup(struct proc_entry *entry);
// /proc/<pid>/fd/N's file, if it has no path to be followed to
// (fs/proc/pid.c); NULL if not
struct fd *proc_pid_fd_file(struct proc_entry *entry);
// The same for path, procfs's
struct fd *proc_fd_link_file(const char *path);

void free_string_array(char **array);

bool proc_dir_read(struct proc_entry *entry, unsigned long *index, struct proc_entry *next_entry);

void proc_buf_append(struct proc_data *buf, const void *data, size_t size);
void proc_printf(struct proc_data *buf, const char *format, ...);

#endif
