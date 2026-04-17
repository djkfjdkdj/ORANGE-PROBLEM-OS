// object.c — Content-addressable object store
//
// Every piece of data (file contents, directory listings, commits) is stored
// as an "object" named by its SHA-256 hash. Objects are stored under
// .pes/objects/XX/YYYYYY... where XX is the first two hex characters of the
// hash (directory sharding).
//
// PROVIDED functions: compute_hash, object_path, object_exists, hash_to_hex, hex_to_hash
// TODO functions:     object_write, object_read

#include "pes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <openssl/evp.h>

// ─── PROVIDED ────────────────────────────────────────────────────────────────

void hash_to_hex(const ObjectID *id, char *hex_out) {
    for (int i = 0; i < HASH_SIZE; i++) {
        sprintf(hex_out + i * 2, "%02x", id->hash[i]);
    }
    hex_out[HASH_HEX_SIZE] = '\0';
}

int hex_to_hash(const char *hex, ObjectID *id_out) {
    if (strlen(hex) < HASH_HEX_SIZE) return -1;
    for (int i = 0; i < HASH_SIZE; i++) {
        unsigned int byte;
        if (sscanf(hex + i * 2, "%2x", &byte) != 1) return -1;
        id_out->hash[i] = (uint8_t)byte;
    }
    return 0;
}

void compute_hash(const void *data, size_t len, ObjectID *id_out) {
    unsigned int hash_len;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
    EVP_DigestUpdate(ctx, data, len);
    EVP_DigestFinal_ex(ctx, id_out->hash, &hash_len);
    EVP_MD_CTX_free(ctx);
}

// Get the filesystem path where an object should be stored.
// Format: .pes/objects/XX/YYYYYYYY...
// The first 2 hex chars form the shard directory; the rest is the filename.
void object_path(const ObjectID *id, char *path_out, size_t path_size) {
    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(id, hex);
    snprintf(path_out, path_size, "%s/%.2s/%s", OBJECTS_DIR, hex, hex + 2);
}

int object_exists(const ObjectID *id) {
    char path[512];
    object_path(id, path, sizeof(path));
    return access(path, F_OK) == 0;
}

// ─── TODO: Implement these ──────────────────────────────────────────────────

// Write an object to the store.
//
// Object format on disk:
//   "<type> <size>\0<data>"
//   where <type> is "blob", "tree", or "commit"
//   and <size> is the decimal string of the data length
//
// Steps:
//   1. Build the full object: header ("blob 16\0") + data
//   2. Compute SHA-256 hash of the FULL object (header + data)
//   3. Check if object already exists (deduplication) — if so, just return success
//   4. Create shard directory (.pes/objects/XX/) if it doesn't exist
//   5. Write to a temporary file in the same shard directory
//   6. fsync() the temporary file to ensure data reaches disk
//   7. rename() the temp file to the final path (atomic on POSIX)
//   8. Open and fsync() the shard directory to persist the rename
//   9. Store the computed hash in *id_out

// HINTS - Useful syscalls and functions for this phase:
//   - sprintf / snprintf : formatting the header string
//   - compute_hash       : hashing the combined header + data
//   - object_exists      : checking for deduplication
//   - mkdir              : creating the shard directory (use mode 0755)
//   - open, write, close : creating and writing to the temp file
//                          (Use O_CREAT | O_WRONLY | O_TRUNC, mode 0644)
//   - fsync              : flushing the file descriptor to disk
//   - rename             : atomically moving the temp file to the final path
//

//
// Returns 0 on success, -1 on error.
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out) {
    const char *type_str = NULL;
    char header[64];
    void *full_obj = NULL;
    size_t header_len, full_len;
    char final_path[512], shard_dir[512], temp_path[512];
    int fd = -1, dirfd = -1;
    ssize_t written_total = 0;

    if (!data || !id_out) return -1;

    switch (type) {
        case OBJ_BLOB:   type_str = "blob";   break;
        case OBJ_TREE:   type_str = "tree";   break;
        case OBJ_COMMIT: type_str = "commit"; break;
        default: return -1;
    }

    // Build header: "<type> <size>\0"
    int n = snprintf(header, sizeof(header), "%s %zu", type_str, len);
    if (n < 0 || (size_t)n >= sizeof(header) - 1) return -1;
    header_len = (size_t)n + 1;  // include '\0'

    // Build full object = header + data
    full_len = header_len + len;
    full_obj = malloc(full_len);
    if (!full_obj) return -1;

    memcpy(full_obj, header, header_len);
    memcpy((char *)full_obj + header_len, data, len);

    // Compute hash of full object
    compute_hash(full_obj, full_len, id_out);

    // Deduplication
    if (object_exists(id_out)) {
        free(full_obj);
        return 0;
    }

    // Final object path
    object_path(id_out, final_path, sizeof(final_path));

    // Extract shard directory from final path
    strncpy(shard_dir, final_path, sizeof(shard_dir) - 1);
    shard_dir[sizeof(shard_dir) - 1] = '\0';
    char *last_slash = strrchr(shard_dir, '/');
    if (!last_slash) {
        free(full_obj);
        return -1;
    }
    *last_slash = '\0';

    // Create shard directory if needed
    if (mkdir(shard_dir, 0755) != 0 && access(shard_dir, F_OK) != 0) {
        free(full_obj);
        return -1;
    }

    // Temp file in same shard directory
    if (snprintf(temp_path, sizeof(temp_path), "%s/.tmpXXXXXX", shard_dir) >= (int)sizeof(temp_path)) {
        free(full_obj);
        return -1;
    }

    fd = mkstemp(temp_path);
    if (fd < 0) {
        free(full_obj);
        return -1;
    }

    // Write full object
    while ((size_t)written_total < full_len) {
        ssize_t w = write(fd, (char *)full_obj + written_total, full_len - (size_t)written_total);
        if (w <= 0) {
            close(fd);
            unlink(temp_path);
            free(full_obj);
            return -1;
        }
        written_total += w;
    }

    // Flush file contents
    if (fsync(fd) != 0) {
        close(fd);
        unlink(temp_path);
        free(full_obj);
        return -1;
    }

    if (close(fd) != 0) {
        unlink(temp_path);
        free(full_obj);
        return -1;
    }
    fd = -1;

    // Atomic rename into place
    if (rename(temp_path, final_path) != 0) {
        unlink(temp_path);
        free(full_obj);
        return -1;
    }

    // fsync shard directory to persist rename
    dirfd = open(shard_dir, O_RDONLY | O_DIRECTORY);
    if (dirfd >= 0) {
        fsync(dirfd);
        close(dirfd);
    }

    free(full_obj);
    return 0;
}
// Read an object from the store.
//
// Steps:
//   1. Build the file path from the hash using object_path()
//   2. Open and read the entire file
//   3. Parse the header to extract the type string and size
//   4. Verify integrity: recompute the SHA-256 of the file contents
//      and compare to the expected hash (from *id). Return -1 if mismatch.
//   5. Set *type_out to the parsed ObjectType
//   6. Allocate a buffer, copy the data portion (after the \0), set *data_out and *len_out
//
// HINTS - Useful syscalls and functions for this phase:
//   - object_path        : getting the target file path
//   - fopen, fread, fseek: reading the file into memory
//   - memchr             : safely finding the '\0' separating header and data
//   - strncmp            : parsing the type string ("blob", "tree", "commit")
//   - compute_hash       : re-hashing the read data for integrity verification
//   - memcmp             : comparing the computed hash against the requested hash
//   - malloc, memcpy     : allocating and returning the extracted data
//
// The caller is responsible for calling free(*data_out).
// Returns 0 on success, -1 on error (file not found, corrupt, etc.).
int object_read(const ObjectID *id, ObjectType *type_out, void **data_out, size_t *len_out) {
    char path[512];
    FILE *fp = NULL;
    uint8_t *file_buf = NULL;
    uint8_t *nul_pos = NULL;
    long file_size_long;
    size_t file_size, header_len, data_len, parsed_len;
    char type_str[16];
    ObjectID computed;

    if (!id || !type_out || !data_out || !len_out) return -1;

    object_path(id, path, sizeof(path));

    fp = fopen(path, "rb");
    if (!fp) return -1;

    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return -1;
    }

    file_size_long = ftell(fp);
    if (file_size_long < 0) {
        fclose(fp);
        return -1;
    }
    file_size = (size_t)file_size_long;

    if (fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return -1;
    }

    file_buf = malloc(file_size);
    if (!file_buf) {
        fclose(fp);
        return -1;
    }

    if (fread(file_buf, 1, file_size, fp) != file_size) {
        free(file_buf);
        fclose(fp);
        return -1;
    }
    fclose(fp);

    compute_hash(file_buf, file_size, &computed);
    if (memcmp(computed.hash, id->hash, HASH_SIZE) != 0) {
        free(file_buf);
        return -1;
    }

    nul_pos = memchr(file_buf, '\0', file_size);
    if (!nul_pos) {
        free(file_buf);
        return -1;
    }

    header_len = (size_t)(nul_pos - file_buf);

    if (sscanf((char *)file_buf, "%15s %zu", type_str, &parsed_len) != 2) {
        free(file_buf);
        return -1;
    }

    if (strcmp(type_str, "blob") == 0) {
        *type_out = OBJ_BLOB;
    } else if (strcmp(type_str, "tree") == 0) {
        *type_out = OBJ_TREE;
    } else if (strcmp(type_str, "commit") == 0) {
        *type_out = OBJ_COMMIT;
    } else {
        free(file_buf);
        return -1;
    }

    if (header_len + 1 > file_size) {
        free(file_buf);
        return -1;
    }

    data_len = file_size - (header_len + 1);
    if (data_len != parsed_len) {
        free(file_buf);
        return -1;
    }

    *data_out = malloc(data_len);
    if (!*data_out) {
        free(file_buf);
        return -1;
    }

    memcpy(*data_out, nul_pos + 1, data_len);
    *len_out = data_len;

    free(file_buf);
    return 0;
}
