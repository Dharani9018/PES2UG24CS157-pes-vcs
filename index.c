#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <inttypes.h>
#include <time.h>

int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);


IndexEntry* index_find(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0)
            return &index->entries[i];
    }
    return NULL;
}

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

int index_status(const Index *index) {
    printf("Staged changes:\n");
    int staged_count = 0;
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
            if (st.st_mtime != (time_t)index->entries[i].mtime_sec ||
                st.st_size != (off_t)index->entries[i].size) {
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
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
            if (strcmp(ent->d_name, ".pes") == 0) continue;
            if (strcmp(ent->d_name, "pes") == 0) continue;
            if (strstr(ent->d_name, ".o") != NULL) continue;

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
                if (S_ISREG(st.st_mode)) {
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

static int compare_entries_by_path(const void *a, const void *b) {
    return strcmp(((const IndexEntry *)a)->path, ((const IndexEntry *)b)->path);
}

int index_load(Index *index) {
    index->count = 0;

    FILE *f = fopen(INDEX_FILE, "r");
    if (!f) return 0;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        if (index->count >= MAX_INDEX_ENTRIES) break;

        IndexEntry *e = &index->entries[index->count];

        char hex[HASH_HEX_SIZE + 2];
        unsigned int mode;
        uint64_t mtime;
        uint64_t size;
        char path[512];

        if (sscanf(line, "%o %64s %" SCNu64 " %" SCNu64 " %511s",
                   &mode, hex, &mtime, &size, path) != 5)
            continue;

        e->mode      = (uint32_t)mode;
        e->mtime_sec = mtime;
        e->size      = (uint32_t)size;
        snprintf(e->path, sizeof(e->path), "%s", path);

        if (hex_to_hash(hex, &e->hash) != 0) continue;

        index->count++;
    }

    fclose(f);
    return 0;
}

int index_save(const Index *index) {
    // Heap-allocate sorted copy to avoid stack overflow
    Index *sorted = malloc(sizeof(Index));
    if (!sorted) return -1;
    memcpy(sorted, index, sizeof(Index));
    qsort(sorted->entries, sorted->count, sizeof(IndexEntry), compare_entries_by_path);

    char tmp_path[] = INDEX_FILE ".tmp";
    FILE *f = fopen(tmp_path, "w");
    if (!f) { free(sorted); return -1; }

    for (int i = 0; i < sorted->count; i++) {
        char hex[HASH_HEX_SIZE + 1];
        hash_to_hex(&sorted->entries[i].hash, hex);
        fprintf(f, "%o %s %" PRIu64 " %" PRIu32 " %s\n",
                sorted->entries[i].mode,
                hex,
                (uint64_t)sorted->entries[i].mtime_sec,
                (uint32_t)sorted->entries[i].size,
                sorted->entries[i].path);
    }

    fflush(f);
    fsync(fileno(f));
    fclose(f);
    free(sorted);

    return rename(tmp_path, INDEX_FILE);
}

int index_add(Index *index, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "error: cannot open '%s'\n", path);
        return -1;
    }

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (file_size < 0) { fclose(f); return -1; }

    uint8_t *buf = malloc((size_t)file_size + 1);
    if (!buf) { fclose(f); return -1; }

    if (file_size > 0 && fread(buf, 1, (size_t)file_size, f) != (size_t)file_size) {
        free(buf); fclose(f); return -1;
    }
    fclose(f);

    // Write blob to object store
    ObjectID blob_id;
    if (object_write(OBJ_BLOB, buf, (size_t)file_size, &blob_id) != 0) {
        free(buf); return -1;
    }
    free(buf);

    // Get file metadata
    struct stat st;
    if (lstat(path, &st) != 0) return -1;

    uint32_t mode = (st.st_mode & S_IXUSR) ? 0100755 : 0100644;

    // Update existing entry or add new one
    IndexEntry *existing = index_find(index, path);
    if (existing) {
        existing->hash      = blob_id;
        existing->mode      = mode;
        existing->mtime_sec = (uint64_t)st.st_mtime;
        existing->size      = (uint32_t)st.st_size;
    } else {
        if (index->count >= MAX_INDEX_ENTRIES) return -1;
        IndexEntry *e = &index->entries[index->count];
        snprintf(e->path, sizeof(e->path), "%s", path);
        e->hash      = blob_id;
        e->mode      = mode;
        e->mtime_sec = (uint64_t)st.st_mtime;
        e->size      = (uint32_t)st.st_size;
        index->count++;
    }

    return index_save(index);
}
