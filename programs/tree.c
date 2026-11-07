
#include "libpieg.h"

#define TREE_MAX_DEPTH   24
#define TREE_MAX_ENTRIES 64
#define TREE_NAME_SIZE   96
#define TREE_PATH_SIZE   512
#define TREE_PREFIX_SIZE 512

typedef struct {
    char name[TREE_NAME_SIZE];
    uint8_t attributes;
} tree_entry_t;

typedef struct {
    tree_entry_t entries[TREE_MAX_ENTRIES];
    int count;
    int truncated;
} tree_level_t;

static tree_level_t levels[TREE_MAX_DEPTH];

static unsigned long total_files;
static unsigned long total_dirs;

static int max_depth = 2;
static int dirs_only = 0;

static void outc(char c) {
    pieg->putchar(c);
}

static void outs(const char *s) {
    while (*s)
        outc(*s++);
}

static void outnum(unsigned long n) {
    char buf[24];
    int i = 0;

    if (!n) {
        outc('0');
        return;
    }

    while (n && i < 23) {
        buf[i++] = '0' + (n % 10);
        n /= 10;
    }

    while (i > 0)
        outc(buf[--i]);
}

static int str_compare(const char *a, const char *b) {
    while (*a && *b && *a == *b) {
        a++;
        b++;
    }

    return (unsigned char)*a - (unsigned char)*b;
}

static int str_length(const char *s) {
    int n = 0;

    while (s[n])
        n++;

    return n;
}

static void str_copy(char *dst, const char *src, int max) {
    int i = 0;

    while (src[i] && i < max - 1) {
        dst[i] = src[i];
        i++;
    }

    dst[i] = '\0';
}

static void make_path(
    char *out,
    const char *parent,
    const char *name
) {
    int i = 0;
    int j = 0;

    while (parent[i] && i < TREE_PATH_SIZE - 1) {
        out[i] = parent[i];
        i++;
    }

    if (i > 0 && out[i - 1] != '/')
        out[i++] = '/';

    while (name[j] && i < TREE_PATH_SIZE - 1)
        out[i++] = name[j++];

    out[i] = '\0';
}

static void list_callback(
    const char *name,
    uint8_t attributes,
    uint32_t size,
    void *context
) {
    tree_level_t *level = (tree_level_t *)context;

    (void)size;

    if (str_compare(name, ".") == 0 ||
        str_compare(name, "..") == 0)
        return;

    int is_dir =
        (attributes & PIEG_ATTR_DIRECTORY) != 0;

    if (dirs_only && !is_dir)
        return;

    if (level->count >= TREE_MAX_ENTRIES) {
        level->truncated = 1;
        return;
    }

    tree_entry_t *entry = &level->entries[level->count++];

    str_copy(entry->name, name, TREE_NAME_SIZE);
    entry->attributes = attributes;
}

static void sort_entries(tree_level_t *level) {
    for (int i = 1; i < level->count; i++) {
        tree_entry_t current = level->entries[i];
        int j = i - 1;

        while (j >= 0 &&
               str_compare(
                   level->entries[j].name,
                   current.name
               ) > 0) {
            level->entries[j + 1] = level->entries[j];
            j--;
        }

        level->entries[j + 1] = current;
    }
}

static void walk_tree(
    const char *path,
    const char *prefix,
    int depth
) {
    if (depth >= max_depth || depth >= TREE_MAX_DEPTH)
        return;

    tree_level_t *level = &levels[depth];

    level->count = 0;
    level->truncated = 0;

    if (pieg->fs_list(path, list_callback, level) < 0) {
        outs(prefix);
        outs("[cannot read directory]\n");
        return;
    }

    sort_entries(level);

    for (int i = 0; i < level->count; i++) {
        tree_entry_t *entry = &level->entries[i];

        int is_last =
            i == level->count - 1 && !level->truncated;

        int is_dir =
            (entry->attributes & PIEG_ATTR_DIRECTORY) != 0;

        outs(prefix);
        outs(is_last ? "`-- " : "|-- ");
        outs(entry->name);

        if (is_dir) {
            outs("/");
            total_dirs++;
        } else {
            total_files++;
        }

        outs("\n");

        if (is_dir && depth + 1 < max_depth) {
            char child_path[TREE_PATH_SIZE];
            char child_prefix[TREE_PREFIX_SIZE];

            make_path(child_path, path, entry->name);
            str_copy(child_prefix, prefix, TREE_PREFIX_SIZE);

            int len = str_length(child_prefix);

            if (len < TREE_PREFIX_SIZE - 5) {
                if (is_last) {
                    child_prefix[len++] = ' ';
                    child_prefix[len++] = ' ';
                    child_prefix[len++] = ' ';
                    child_prefix[len++] = ' ';
                } else {
                    child_prefix[len++] = '|';
                    child_prefix[len++] = ' ';
                    child_prefix[len++] = ' ';
                    child_prefix[len++] = ' ';
                }

                child_prefix[len] = '\0';

                walk_tree(
                    child_path,
                    child_prefix,
                    depth + 1
                );
            }
        }
    }

    if (level->truncated) {
        outs(prefix);
        outs("`-- [more entries omitted]\n");
    }
}

static int parse_depth(const char *s) {
    int value = 0;

    if (!*s)
        return -1;

    while (*s) {
        if (*s < '0' || *s > '9')
            return -1;

        value = value * 10 + (*s - '0');

        if (value > TREE_MAX_DEPTH)
            return -1;

        s++;
    }

    return value < 1 ? -1 : value;
}

int main(int argc, char **argv) {
    char root[TREE_PATH_SIZE];

    if (!pieg)
        return 1;

    const char *input = ".";
    int have_path = 0;

    max_depth = 2;
    dirs_only = 0;

    for (int i = 1; i < argc; i++) {
        if (str_compare(argv[i], "-L") == 0) {
            if (++i >= argc) {
                outs("Usage: tree [-dirf] [-L depth] [directory]\n");
                return 1;
            }

            max_depth = parse_depth(argv[i]);

            if (max_depth < 1) {
                outs("tree: invalid depth (1-24)\n");
                return 1;
            }
        } else if (str_compare(argv[i], "-dirf") == 0) {
            dirs_only = 1;
        } else if (!have_path) {
            input = argv[i];
            have_path = 1;
        } else {
            outs("Usage: tree [-dirf] [-L depth] [directory]\n");
            return 1;
        }
    }

    pieg->resolve(input, root, sizeof(root));

    int is_directory = 0;
    uint32_t size = 0;

    if (pieg->fs_stat(root, &is_directory, &size) < 0 ||
        !is_directory) {
        outs("tree: cannot access directory\n");
        return 1;
    }

    total_files = 0;
    total_dirs = 0;

    outs(root);
    outs("\n");

    walk_tree(root, "", 0);

    outs("\n");

    outnum(total_dirs);
    outs(" directories, ");

    outnum(total_files);
    outs(" files\n");

    return 0;
}
