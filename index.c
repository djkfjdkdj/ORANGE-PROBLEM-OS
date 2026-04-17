// index.c — Staging area implementation
//
// Text format of .pes/index (one entry per line, sorted by path):
//
//   <mode-octal> <64-char-hex-hash> <mtime-seconds> <size> <path>
//
// Example:
//   100644 a1b2c3d4e5f6...  1699900000 42 README.md
//   100644 f7e8d9c0b1a2...  1699900100 128 src/main.c
//
// This is intentionally a simple text format. No magic numbers, no
// binary parsing. The focus is on the staging area CONCEPT (tracking
// what will go into the next commit) and ATOMIC WRITES (temp+rename).
//
// PROVIDED functions: index_find, index_remove, index_status
// TODO functions:     index_load, index_save, index_add

#include "index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>

int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);

// ─── PROVIDED ────────────────────────────────────────────────────────────────

// Find an index entry by path (linear scan).
IndexEntry* index_find(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0)
            return &index->entries[i];
    }
    return NULL;
}

// Remove a file from the index.
// Returns 0 on success, -1 if path not in index.
int index_remove(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0) {
            int remaining = index->count - i - 1;
            if (remaining > 0)
                memmove(&index->entries[i], &index->entries[i + 1],
                        remaining * sizeof(IndexEntry));
            index->count--;
            return index_save(index);
        }
    }
    fprintf(stderr, "error: '%s' is not in the index\n", path);
    return -1;
}

// Print the status of the working directory.
//
// Identifies files that are staged, unstaged (modified/deleted in working dir),
// and untracked (present in working dir but not in index).
// Returns 0.
int index_status(const Index *index) {
    printf("Staged changes:\n");
    int staged_count = 0;
    // Note: A true Git implementation deeply diffs against the HEAD tree here. 
    // For this lab, displaying indexed files represents the staging intent.
    for (int i = 0; i < index->count; i++) {
        printf("  staged:     %s\n", index->entries[i].path);
        staged_count++;
    }
    if (staged_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    printf("Unstaged changes:\n");
    int unstaged_count = 0;
    for (int i = 0; i < index->count; i++) {
        struct stat st;
        if (stat(index->entries[i].path, &st) != 0) {
            printf("  deleted:    %s\n", index->entries[i].path);
            unstaged_count++;
        } else {
            // Fast diff: check metadata instead of re-hashing file content
            if (st.st_mtime != (time_t)index->entries[i].mtime_sec || st.st_size != (off_t)index->entries[i].size) {
                printf("  modified:   %s\n", index->entries[i].path);
                unstaged_count++;
            }
        }
    }
    if (unstaged_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    printf("Untracked files:\n");
    int untracked_count = 0;
    DIR *dir = opendir(".");
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            // Skip hidden directories, parent directories, and build artifacts
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
            if (strcmp(ent->d_name, ".pes") == 0) continue;
            if (strcmp(ent->d_name, "pes") == 0) continue; // compiled executable
            if (strstr(ent->d_name, ".o") != NULL) continue; // object files

            // Check if file is tracked in the index
            int is_tracked = 0;
            for (int i = 0; i < index->count; i++) {
                if (strcmp(index->entries[i].path, ent->d_name) == 0) {
                    is_tracked = 1; 
                    break;
                }
            }
            
            if (!is_tracked) {
                struct stat st;
                stat(ent->d_name, &st);
                if (S_ISREG(st.st_mode)) { // Only list regular files for simplicity
                    printf("  untracked:  %s\n", ent->d_name);
                    untracked_count++;
                }
            }
        }
        closedir(dir);
    }
    if (untracked_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    return 0;
}

// ─── HELPERS ────────────────────────────────────────────────────────────────

static int compare_index_entries(const void *a, const void *b) {
    const IndexEntry *ea = (const IndexEntry *)a;
    const IndexEntry *eb = (const IndexEntry *)b;
    return strcmp(ea->path, eb->path);
}

static uint32_t mode_from_stat(const struct stat *st) {
    if (S_ISDIR(st->st_mode))  return 0040000;
    if (st->st_mode & S_IXUSR) return 0100755;
    return 0100644;
}

// ─── TODO: Implement these ───────────────────────────────────────────────────

// Load the index from .pes/index.
//
// HINTS - Useful functions:
//   - fopen (with "r"), fscanf, fclose : reading the text file line by line
//   - hex_to_hash                      : converting the parsed string to ObjectID
//
// Returns 0 on success, -1 on error.
int index_load(Index *index) {
    FILE *fp;
    char line[2048];

    if (!index) return -1;
    index->count = 0;

    fp = fopen(INDEX_FILE, "r");
    if (!fp) {
        if (errno == ENOENT) return 0;   // no index yet
        return -1;
    }

    while (fgets(line, sizeof(line), fp)) {
        IndexEntry entry;
        char hex[HASH_HEX_SIZE + 1];
        char path[512];
        unsigned int mode;
        long long mtime_sec;
        long long size;

        if (line[0] == '\n' || line[0] == '\0') {
            continue;
        }

        if (sscanf(line, "%o %64s %lld %lld %511[^\n]",
                   &mode, hex, &mtime_sec, &size, path) != 5) {
            fclose(fp);
            return -1;
        }

        if (strlen(hex) != HASH_HEX_SIZE) {
            fclose(fp);
            return -1;
        }

        if (mtime_sec < 0 || size < 0) {
            fclose(fp);
            return -1;
        }

        if (index->count >= MAX_INDEX_ENTRIES) {
            fclose(fp);
            return -1;
        }

        entry.mode = (uint32_t)mode;
        entry.mtime_sec = (time_t)mtime_sec;
        entry.size = (size_t)size;

        if (hex_to_hash(hex, &entry.hash) != 0) {
            fclose(fp);
            return -1;
        }

        strncpy(entry.path, path, sizeof(entry.path) - 1);
        entry.path[sizeof(entry.path) - 1] = '\0';

        for (int i = 0; i < index->count; i++) {
            if (strcmp(index->entries[i].path, entry.path) == 0) {
                fclose(fp);
                return -1;
            }
        }

        index->entries[index->count++] = entry;
    }

    fclose(fp);

    qsort(index->entries, index->count, sizeof(IndexEntry), compare_index_entries);
    return 0;
}
// Save the index to .pes/index atomically.
//
// HINTS - Useful functions and syscalls:
//   - qsort                            : sorting the entries array by path
//   - fopen (with "w"), fprintf        : writing to the temporary file
//   - hash_to_hex                      : converting ObjectID for text output
//   - fflush, fileno, fsync, fclose    : flushing userspace buffers and syncing to disk
//   - rename                           : atomically moving the temp file over the old index
//
// Returns 0 on success, -1 on error.
int index_save(const Index *index) {
    FILE *fp = NULL;
    int fd = -1, dirfd = -1;
    char temp_path[512];
    char hex[HASH_HEX_SIZE + 1];
    IndexEntry *sorted_entries = NULL;

    if (!index) return -1;
    if (index->count > MAX_INDEX_ENTRIES) return -1;

    if (snprintf(temp_path, sizeof(temp_path), "%s.tmp", INDEX_FILE) >= (int)sizeof(temp_path)) {
        return -1;
    }

    if (index->count > 0) {
        sorted_entries = malloc(index->count * sizeof(IndexEntry));
        if (!sorted_entries) return -1;

        memcpy(sorted_entries, index->entries, index->count * sizeof(IndexEntry));
        qsort(sorted_entries, index->count, sizeof(IndexEntry), compare_index_entries);
    }

    fp = fopen(temp_path, "w");
    if (!fp) {
        free(sorted_entries);
        return -1;
    }

    for (size_t i = 0; i < index->count; i++) {
        hash_to_hex(&sorted_entries[i].hash, hex);

        if (fprintf(fp, "%o %s %llu %u %s\n",
                    sorted_entries[i].mode,
                    hex,
                    (unsigned long long)sorted_entries[i].mtime_sec,
                    sorted_entries[i].size,
                    sorted_entries[i].path) < 0) {
            fclose(fp);
            unlink(temp_path);
            free(sorted_entries);
            return -1;
        }
    }

    if (fflush(fp) != 0) {
        fclose(fp);
        unlink(temp_path);
        free(sorted_entries);
        return -1;
    }

    fd = fileno(fp);
    if (fd < 0 || fsync(fd) != 0) {
        fclose(fp);
        unlink(temp_path);
        free(sorted_entries);
        return -1;
    }

    if (fclose(fp) != 0) {
        unlink(temp_path);
        free(sorted_entries);
        return -1;
    }

    if (rename(temp_path, INDEX_FILE) != 0) {
        unlink(temp_path);
        free(sorted_entries);
        return -1;
    }

    dirfd = open(PES_DIR, O_RDONLY);
    if (dirfd >= 0) {
        fsync(dirfd);
        close(dirfd);
    }

    free(sorted_entries);
    return 0;
}
// Stage a file for the next commit.
//
// HINTS - Useful functions and syscalls:
//   - fopen, fread, fclose             : reading the target file's contents
//   - object_write                     : saving the contents as OBJ_BLOB
//   - stat / lstat                     : getting file metadata (size, mtime, mode)
//   - index_find                       : checking if the file is already staged
//
// Returns 0 on success, -1 on error.
int index_add(Index *index, const char *path) {
    struct stat st;
    FILE *fp = NULL;
    void *buf = NULL;
    size_t file_size, total_read;
    ObjectID blob_id;
    IndexEntry new_entry;
    IndexEntry *existing;

    if (!index || !path) return -1;

    if (path[0] == '\0') {
        fprintf(stderr, "error: empty path\n");
        return -1;
    }

    if (strlen(path) >= sizeof(new_entry.path)) {
        fprintf(stderr, "error: path too long: '%s'\n", path);
        return -1;
    }

    // Use lstat first to avoid accidentally accepting symlinks
    if (lstat(path, &st) != 0) {
        fprintf(stderr, "error: cannot stat '%s'\n", path);
        return -1;
    }

    if (!S_ISREG(st.st_mode)) {
        fprintf(stderr, "error: '%s' is not a regular file\n", path);
        return -1;
    }

    file_size = (size_t)st.st_size;

    fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "error: cannot open '%s'\n", path);
        return -1;
    }

    if (file_size > 0) {
        buf = malloc(file_size);
        if (!buf) {
            fclose(fp);
            return -1;
        }

        total_read = fread(buf, 1, file_size, fp);
        if (total_read != file_size) {
            free(buf);
            fclose(fp);
            return -1;
        }
    }

    if (fclose(fp) != 0) {
        free(buf);
        return -1;
    }
    fp = NULL;

    if (object_write(OBJ_BLOB, buf, file_size, &blob_id) != 0) {
        free(buf);
        return -1;
    }

    free(buf);
    buf = NULL;

    new_entry.mode = mode_from_stat(&st);
    new_entry.hash = blob_id;
    new_entry.mtime_sec = st.st_mtime;
    new_entry.size = file_size;
    strncpy(new_entry.path, path, sizeof(new_entry.path) - 1);
    new_entry.path[sizeof(new_entry.path) - 1] = '\0';

    existing = index_find(index, path);

    if (existing) {
        *existing = new_entry;
    } else {
        if (index->count >= MAX_INDEX_ENTRIES) {
            fprintf(stderr, "error: index is full\n");
            return -1;
        }
        index->entries[index->count++] = new_entry;
    }

    if (index_save(index) != 0) {
        return -1;
    }

    return 0;
}
