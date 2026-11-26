# Inode disk image

All `JobStore` content is stored inside `DATA_DIRECTORY/disk.img`. The default image is fixed at 128 MiB. A second server cannot open the same image concurrently. Threads inside one server share a mutex-protected store.

| Region | Contents |
|---|---|
| 4096-byte superblock | Magic `OSINODE1`, format version, fixed capacity, block size, data block count, metadata bank size, inode count and data offset, checksum |
| Two metadata banks | Generation and checksum, free-data-block bitmap, 8192 inodes |
| Data area | 4096-byte blocks. Each block starts with a 4-byte next-block number, followed by up to 4092 bytes of file contents |

Each inode records its type, parent inode, name, first data block and byte length. Directory paths are resolved one component at a time by matching the parent inode and name. Root is inode 0. Each name is limited to 63 bytes. The supported API is `mkdir`, `exists`, `write`, `read`, `remove` and `statistics` in `src/inode_fs.hpp`. Directory creation requires its parent to exist. Removing a directory requires it to be empty. Files are limited to 16 MiB. All disk integers are encoded little-endian explicitly.

Writes allocate fresh blocks while the previous file remains valid. Only after the new data has been written and flushed does the filesystem commit the next metadata generation into the other bank. Overwrites and deletes return old blocks to the bitmap. An insufficient-space rejection leaves the old file and free-block count intact. Copy-on-write replacement needs enough free space for the entire new file. Completed job metadata is written after the final image, and downloads require that completion marker.

The image is reopened by validating the superblock and selecting the newest metadata bank with a valid checksum. The write and reopen guarantees cover normal application operation and orderly process shutdown. Power-loss durability, injected device-write faults and server-crash job resumption require further validation and implementation. The format uses checksums for metadata, and currently has no per-file data checksum.

The image file is created automatically on first use. Use a fresh data directory when upgrading from starter v1 or v2. Those versions stored jobs directly in ordinary directories, and importing their history requires a migration tool that is still pending.

`JobStore::abort(job_id)` reclaims every inode and data block belonging to an incomplete job in one metadata commit. Repeated calls are safe. Completed jobs are preserved. Creation failure uses the same recursive cleanup. The server should call `abort` after a terminal job failure while keeping any desired error event in its live scheduler history.

Each new job records its validated demo-account owner (`alice`, `bob` or `admin`) in `owner.txt` inside the image. `JobStore::owner` returns that persisted owner for server authorization and rejects missing or invalid owner records. Existing C++ callers that omit the owner parameter create an admin-owned job.

The standalone storage check covers nested directories, multi-block read/write, zero-byte files, overwrite and deletion block reclamation, insufficient-space rollback, path traversal rejection, image exclusivity, concurrent writes, reopening, and the completed-job marker. Build with the project CMake test target or directly with:

```bash
g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror -pthread -Isrc tests/storage_check.cpp src/storage.cpp src/inode_fs.cpp -o storage_check
./storage_check /tmp/os-storage-fresh-check
```

Use a fresh test directory on each run. The test creates a 4 MiB test image and a separate default-size job image.
