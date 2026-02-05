#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <sys/stat.h>
#include <unistd.h>

#define MAX_INODES 1024
#define NAME_LEN 32

/* Stores whether an inode is in use and whether it is a file or directory */
typedef struct {
    int used;
    char type;
} InodeInfo;

/* Represents a directory entry: inode number + fixed-length name */
typedef struct {
    uint32_t inode;
    char name[NAME_LEN];
} DirEnt;

/* Table holding metadata for all possible inodes */
static InodeInfo inode_table[MAX_INODES];

/* Print an error message and exit */
static void die(const char *msg)
{
    fprintf(stderr, "%s\n", msg);
    exit(1);
}

/* Check whether a path refers to a directory on the real file system */
static int is_directory(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        return 0;
    }
    return S_ISDIR(st.st_mode);
}

/* Copy a name into a fixed 32-byte buffer, truncating if necessary */
static void make_name32(char dst[NAME_LEN], const char *src)
{
    memset(dst, 0, NAME_LEN);
    strncpy(dst, src, NAME_LEN);
}

/* Load inode usage information from the binary inodes_list file */
static void load_inodes_list(void)
{
    FILE *f = fopen("inodes_list", "rb");
    if (!f) {
        perror("inodes_list");
        exit(1);
    }

    uint32_t index;
    char type;

    while (fread(&index, sizeof(uint32_t), 1, f) == 1 &&
           fread(&type,  sizeof(char),     1, f) == 1) {

        if (index >= MAX_INODES) {
            fprintf(stderr, "Invalid inode (out of range): %u\n", (unsigned)index);
            continue;
        }

        if (type != 'd' && type != 'f') {
            fprintf(stderr, "Invalid inode type for inode %u\n", (unsigned)index);
            continue;
        }

        inode_table[index].used = 1;
        inode_table[index].type = type;
    }

    fclose(f);
}

/* Write the current inode table back to inodes_list */
static void save_inodes_list(void)
{
    FILE *f = fopen("inodes_list", "wb");
    if (!f) {
        perror("inodes_list");
        return;
    }

    for (uint32_t i = 0; i < MAX_INODES; i++) {
        if (inode_table[i].used) {
            fwrite(&i, sizeof(uint32_t), 1, f);
            fwrite(&inode_table[i].type, sizeof(char), 1, f);
        }
    }

    fclose(f);
}

/* Search a directory for an entry with the given name */
static int dir_find(uint32_t dir_inode, const char *name, DirEnt *out)
{
    char fname[16];
    snprintf(fname, sizeof(fname), "%u", (unsigned)dir_inode);

    FILE *f = fopen(fname, "rb");
    if (!f) {
        return 0;
    }

    DirEnt ent;
    char key[NAME_LEN];
    make_name32(key, name);

    while (fread(&ent.inode, sizeof(uint32_t), 1, f) == 1 &&
           fread(ent.name, 1, NAME_LEN, f) == NAME_LEN) {

        if (memcmp(ent.name, key, NAME_LEN) == 0) {
            if (out) {
                *out = ent;
            }
            fclose(f);
            return 1;
        }
    }

    fclose(f);
    return 0;
}

/* Append a new entry to a directory file */
static int dir_append(uint32_t dir_inode, uint32_t child_inode, const char *name)
{
    char fname[16];
    snprintf(fname, sizeof(fname), "%u", (unsigned)dir_inode);

    FILE *f = fopen(fname, "ab");
    if (!f) {
        return 0;
    }

    char namebuf[NAME_LEN];
    make_name32(namebuf, name);

    fwrite(&child_inode, sizeof(uint32_t), 1, f);
    fwrite(namebuf, 1, NAME_LEN, f);

    fclose(f);
    return 1;
}

/* Find the first unused inode number */
static int find_free_inode(void)
{
    for (int i = 0; i < MAX_INODES; i++) {
        if (!inode_table[i].used) {
            return i;
        }
    }
    return -1;
}

/* Create a directory inode file containing . and .. */
static int create_dir_inode(uint32_t new_inode, uint32_t parent_inode)
{
    char fname[16];
    snprintf(fname, sizeof(fname), "%u", (unsigned)new_inode);

    FILE *f = fopen(fname, "wb");
    if (!f) {
        return 0;
    }

    char dot[NAME_LEN], dotdot[NAME_LEN];
    make_name32(dot, ".");
    make_name32(dotdot, "..");

    fwrite(&new_inode, sizeof(uint32_t), 1, f);
    fwrite(dot, 1, NAME_LEN, f);

    fwrite(&parent_inode, sizeof(uint32_t), 1, f);
    fwrite(dotdot, 1, NAME_LEN, f);

    fclose(f);
    return 1;
}

/* Create a file inode and write the name into it */
static int create_file_inode(uint32_t new_inode, const char *name)
{
    char fname[16];
    snprintf(fname, sizeof(fname), "%u", (unsigned)new_inode);

    FILE *f = fopen(fname, "wb");
    if (!f) {
        return 0;
    }

    char namebuf[NAME_LEN];
    make_name32(namebuf, name);

    size_t n = 0;
    while (n < NAME_LEN && namebuf[n] != '\0') {
        n++;
    }

    fwrite(namebuf, 1, n, f);
    fclose(f);
    return 1;
}

/* Print the contents of the current directory */
static void cmd_ls(uint32_t cwd)
{
    char fname[16];
    snprintf(fname, sizeof(fname), "%u", (unsigned)cwd);

    FILE *f = fopen(fname, "rb");
    if (!f) {
        perror("ls");
        return;
    }

    DirEnt ent;
    char namebuf[NAME_LEN + 1];

    while (fread(&ent.inode, sizeof(uint32_t), 1, f) == 1 &&
           fread(ent.name, 1, NAME_LEN, f) == NAME_LEN) {

        memcpy(namebuf, ent.name, NAME_LEN);
        namebuf[NAME_LEN] = '\0';
        printf("%u %s\n", (unsigned)ent.inode, namebuf);
    }

    fclose(f);
}

/* Change the current working directory */
static void cmd_cd(uint32_t *cwd, const char *name)
{
    DirEnt ent;

    if (!dir_find(*cwd, name, &ent)) {
        fprintf(stderr, "cd: no such directory\n");
        return;
    }

    if (ent.inode >= MAX_INODES || !inode_table[ent.inode].used ||
        inode_table[ent.inode].type != 'd') {
        fprintf(stderr, "cd: not a directory\n");
        return;
    }

    *cwd = ent.inode;
}

/* Create a new directory in the current directory */
static void cmd_mkdir(uint32_t cwd, const char *name)
{
    if (dir_find(cwd, name, NULL)) {
        fprintf(stderr, "mkdir: already exists\n");
        return;
    }

    int free_i = find_free_inode();
    if (free_i < 0) {
        fprintf(stderr, "mkdir: no free inodes\n");
        return;
    }

    inode_table[free_i].used = 1;
    inode_table[free_i].type = 'd';

    if (!create_dir_inode((uint32_t)free_i, cwd) ||
        !dir_append(cwd, (uint32_t)free_i, name)) {
        inode_table[free_i].used = 0;
    }
}

/* Create a new file in the current directory */
static void cmd_touch(uint32_t cwd, const char *name)
{
    if (dir_find(cwd, name, NULL)) {
        return;
    }

    int free_i = find_free_inode();
    if (free_i < 0) {
        fprintf(stderr, "touch: no free inodes\n");
        return;
    }

    inode_table[free_i].used = 1;
    inode_table[free_i].type = 'f';

    if (!create_file_inode((uint32_t)free_i, name) ||
        !dir_append(cwd, (uint32_t)free_i, name)) {
        inode_table[free_i].used = 0;
    }
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <fs_directory>\n", argv[0]);
        return 1;
    }

    if (!is_directory(argv[1])) {
        fprintf(stderr, "Not a directory: %s\n", argv[1]);
        return 1;
    }

    if (chdir(argv[1]) != 0) {
        perror("chdir");
        return 1;
    }

    memset(inode_table, 0, sizeof(inode_table));
    load_inodes_list();

    if (!inode_table[0].used || inode_table[0].type != 'd') {
        die("inode 0 is not a directory");
    }

    uint32_t cwd = 0;
    char line[256];

    while (1) {
        if (!fgets(line, sizeof(line), stdin)) {
            save_inodes_list();
            break;
        }

        line[strcspn(line, "\n")] = '\0';

        char *cmd = strtok(line, " \t");
        if (!cmd) continue;

        if (strcmp(cmd, "ls") == 0) {
            char *extra = strtok(NULL, " \t");
            if (extra) fprintf(stderr, "Invalid command\n");
            else cmd_ls(cwd);

        } else if (strcmp(cmd, "cd") == 0) {
            char *arg = strtok(NULL, " \t");
            char *extra = strtok(NULL, " \t");
            if (!arg || extra) fprintf(stderr, "Invalid command\n");
            else cmd_cd(&cwd, arg);

        } else if (strcmp(cmd, "mkdir") == 0) {
            char *arg = strtok(NULL, " \t");
            char *extra = strtok(NULL, " \t");
            if (!arg || extra) fprintf(stderr, "Invalid command\n");
            else cmd_mkdir(cwd, arg);

        } else if (strcmp(cmd, "touch") == 0) {
            char *arg = strtok(NULL, " \t");
            char *extra = strtok(NULL, " \t");
            if (!arg || extra) fprintf(stderr, "Invalid command\n");
            else cmd_touch(cwd, arg);

        } else if (strcmp(cmd, "exit") == 0) {
            char *extra = strtok(NULL, " \t");
            if (extra) fprintf(stderr, "Invalid command\n");
            else { save_inodes_list(); break; }

        } else {
            fprintf(stderr, "Invalid command\n");
        }
    }

    return 0;
}

