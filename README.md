# Enterprise Communication Platform with Distributed Backup (ECPB)

**C++ 17 | Linux | POSIX | Systems Programming**

An on-premises enterprise communication product featuring real-time messaging, file sharing, and a distributed backup architecture. The system implements chunk-based storage, content-addressable deduplication, AES-256 encryption, LZ4/ZSTD compression, multi-process orchestration via `fork()`/`exec()`, POSIX IPC, and a priority-based job scheduler with DAG dependency resolution — all built from scratch with zero external frameworks.

---

## Table of Contents

1. [Quick Start](#quick-start)
2. [System Requirements](#system-requirements)
3. [Dependencies](#dependencies)
4. [Installation](#installation)
5. [Building](#building)
6. [Usage](#usage)
7. [Architecture](#architecture)
8. [Module Reference](#module-reference)
9. [Data Structures](#data-structures)
10. [SQLite Synchronization Strategy](#sqlite-synchronization-strategy)
11. [Security Model](#security-model)
12. [Storage Layout](#storage-layout)
13. [Configuration Constants](#configuration-constants)
14. [Testing](#testing)
15. [Project Structure](#project-structure)
16. [Technical Details](#technical-details)

---

## Quick Start

```bash
# 1. Install all dependencies (Ubuntu/Debian)
sudo apt update
sudo apt install -y g++ make libssl-dev liblz4-dev libzstd-dev libsqlite3-dev

# 2. Build
make

# 3. Run the full test suite (backup -> list -> stats -> verify -> restore -> diff)
make test

# 4. Create a backup
./build/ecpb --backup /path/to/source --name my_backup

# 5. Restore it
./build/ecpb --restore 1 --dest /path/to/restore

# 6. Launch interactive UI
./build/ecpb
```

---

## System Requirements

| Requirement        | Minimum                        | Recommended                    |
|--------------------|--------------------------------|--------------------------------|
| **Operating System** | Linux (any distro with POSIX) | Ubuntu 22.04 LTS or later    |
| **Compiler**       | GCC 9+ or Clang 10+ (C++17)   | GCC 13+                       |
| **Architecture**   | x86_64, ARM64                  | x86_64                         |
| **RAM**            | 256 MB                         | 1 GB+                          |
| **Disk**           | Depends on backup data         | 2x source data size            |
| **Kernel**         | 4.x+                          | 5.x+ (for better POSIX shm)   |

---

## Dependencies

### Required Libraries

| Library           | Package Name (apt)    | Min Version  | Purpose                                    |
|-------------------|-----------------------|--------------|--------------------------------------------|
| **OpenSSL**       | `libssl-dev`          | 1.1.1+       | SHA-256 hashing (EVP API), AES-256-CBC encryption, CSPRNG (`RAND_bytes`) |
| **LZ4**           | `liblz4-dev`          | 1.9.0+       | Fast compression for backup chunks         |
| **Zstandard**     | `libzstd-dev`         | 1.4.0+       | High-ratio compression (alternative to LZ4)|
| **SQLite3**       | `libsqlite3-dev`      | 3.35.0+      | Metadata database (jobs, chunks, manifests, messaging) |
| **POSIX Threads** | Built-in (libpthread) | —            | Thread synchronization, mutexes            |
| **POSIX RT**      | Built-in (librt)      | —            | Shared memory (`shm_open`), semaphores     |

### Build Tools

| Tool     | Package Name (apt) | Min Version | Purpose         |
|----------|--------------------|-------------|-----------------|
| **GCC**  | `g++`              | 9.0+        | C++17 compiler  |
| **Make** | `make`             | 4.0+        | Build system    |

---

## Installation

### Ubuntu / Debian

```bash
sudo apt update
sudo apt install -y \
    g++ \
    make \
    libssl-dev \
    liblz4-dev \
    libzstd-dev \
    libsqlite3-dev
```

### Fedora / RHEL / CentOS

```bash
sudo dnf install -y \
    gcc-c++ \
    make \
    openssl-devel \
    lz4-devel \
    libzstd-devel \
    sqlite-devel
```

### Arch Linux

```bash
sudo pacman -S \
    gcc \
    make \
    openssl \
    lz4 \
    zstd \
    sqlite
```

### Alpine Linux

```bash
sudo apk add \
    g++ \
    make \
    openssl-dev \
    lz4-dev \
    zstd-dev \
    sqlite-dev
```

### Verifying Dependencies

After installing, confirm everything is available:

```bash
# Compiler
g++ --version          # Should show 9.x or higher

# Libraries (use any of these)
pkg-config --modversion openssl    # 1.1.1+
pkg-config --modversion liblz4     # 1.9.0+
pkg-config --modversion libzstd    # 1.4.0+
pkg-config --modversion sqlite3    # 3.35.0+

# Or check headers exist directly
ls /usr/include/openssl/evp.h      # OpenSSL
ls /usr/include/lz4.h              # LZ4
ls /usr/include/zstd.h             # Zstandard
ls /usr/include/sqlite3.h          # SQLite3
```

---

## Building

```bash
# Standard build (optimized, with warnings)
make

# The binary is placed at:
./build/ecpb

# Clean build artifacts
make clean

# Build and run all tests
make test
```

### Compiler Flags

The Makefile uses these flags:

| Flag          | Purpose                                      |
|---------------|----------------------------------------------|
| `-std=c++17`  | C++17 standard (required for `std::optional`, `std::filesystem`, structured bindings) |
| `-Wall -Wextra`| All warnings enabled                        |
| `-O2`         | Optimization level 2                         |
| `-I include`  | Header search path                           |

### Linker Libraries

| Flag        | Library            |
|-------------|--------------------|
| `-lssl`     | OpenSSL SSL        |
| `-lcrypto`  | OpenSSL Crypto     |
| `-llz4`     | LZ4 compression    |
| `-lzstd`    | Zstandard compression |
| `-lsqlite3` | SQLite3 database   |
| `-lpthread` | POSIX threads      |
| `-lrt`      | POSIX realtime (shm, semaphores) |

---

## Usage

### Command-Line Interface (Non-Interactive)

```bash
# Create a backup of a directory
./build/ecpb --data-dir ./my_data --backup /home/user/documents --name docs_backup

# List all backup jobs
./build/ecpb --data-dir ./my_data --list

# Show system statistics (chunk count, dedup savings, etc.)
./build/ecpb --data-dir ./my_data --stats

# Verify backup integrity (checks all chunk hashes + file existence)
./build/ecpb --data-dir ./my_data --verify 1

# Restore backup job #1 to a destination directory
./build/ecpb --data-dir ./my_data --restore 1 --dest /home/user/restored

# Set log verbosity (0=DEBUG, 1=INFO, 2=WARN, 3=ERROR)
./build/ecpb --data-dir ./my_data --log-level 0 --backup /path/to/source --name debug_backup
```

### CLI Options Reference

| Option                  | Description                                          |
|-------------------------|------------------------------------------------------|
| `--data-dir <path>`     | Data directory for DB, chunks, snapshots (default: `./ecpb_data`) |
| `--log-level <0-3>`     | Logging verbosity: 0=DEBUG, 1=INFO, 2=WARN, 3=ERROR |
| `--backup <source>`     | Source directory or file to back up                  |
| `--name <name>`         | Human-readable name for the backup job               |
| `--restore <job_id>`    | Restore backup by job ID                             |
| `--dest <path>`         | Destination directory for restore                    |
| `--verify <job_id>`     | Verify backup integrity without restoring            |
| `--list`                | List all backup jobs                                 |
| `--stats`               | Show system-wide statistics                          |
| `--help`                | Display usage information                            |

### Interactive Terminal UI

Launch without any action flags to enter the interactive menu:

```bash
./build/ecpb --data-dir ./my_data
```

```
========================================
  Enterprise Backup System (ECPB)
  C++ | POSIX | SQLite | AES-256
========================================

--- Main Menu ---
  1) Create Backup
  2) Restore Backup
  3) List Jobs
  4) Verify Backup
  5) System Stats
  6) Messaging
  7) Set Log Level
  0) Exit
```

The interactive UI provides the same capabilities as the CLI plus the messaging subsystem for channel-based communication and file sharing notifications.

---

## Architecture

```
+---------------------------------------------------------------------+
|                        Terminal UI / CLI                             |
+---------------------------------------------------------------------+
|                     Backup Orchestrator                              |
|  +---------------+  +---------------+  +--------------------------+ |
|  | Job Scheduler |  |  Snapshot     |  |   Multi-Process Engine   | |
|  | (PQ + DAG)    |  |  Manager     |  |   fork() / exec()        | |
|  +-------+-------+  +-------+------+  +----------+---------------+ |
|          |                   |                     |                 |
|          v                   v                     v                 |
|  +--------------------------------------------------------------------+
|  |                    Backup Worker                                   |
|  |  Snapshot -> Chunk -> Hash -> Compress -> Encrypt -> Store         |
|  +--------------------------------------------------------------------+
+---------------------------------------------------------------------+
|  +---------------+  +---------------+  +--------------------------+ |
|  | Chunk Store   |  |  Restore     |  |   Messaging Service      | |
|  | (CAS + Dedup) |  |  Engine      |  |   (Channels + Files)     | |
|  +-------+-------+  +-------+------+  +----------+---------------+ |
|          |                   |                     |                 |
+----------+-------------------+---------------------+-----------------+
|                       SQLite Database                                |
|  WAL Mode | Global Mutex | Busy Timeout | Auto-Retry                |
+---------------------------------------------------------------------+
|                      POSIX IPC Layer                                 |
|  Shared Memory | Message Queue (Pipe) | Named Semaphores            |
+---------------------------------------------------------------------+
|                     Data Structures                                  |
|  HashMap | PriorityQueue | DAG | CircularBuffer | B+ Tree           |
+---------------------------------------------------------------------+
```

### Backup Pipeline (per file)

```
Source File
    |
    v
+------------+
| Snapshot   |  Hardlink-based copy-on-write for consistency
| (CoW)      |  without blocking active transfers
+-----+------+
      |
      v
+------------+
| Chunking   |  Split into 64 KB fixed-size blocks
+-----+------+
      |
      v
+------------+
| SHA-256    |  Content-addressable hash per chunk
| Hashing    |  Full file hash for integrity verification
+-----+------+
      |
      v
+------------+
| Dedup      |  Check hash against existing chunks in DB
| Check      |  Skip storage if chunk already exists (ref_count++)
+-----+------+
      | (new chunk only)
      v
+------------+
| Compress   |  LZ4 (fast, default) or ZSTD (high ratio)
+-----+------+
      |
      v
+------------+
| Encrypt    |  AES-256-CBC with random IV per chunk
+-----+------+
      |
      v
+------------+
| Store      |  Content-addressable path: chunks/ab/cd/abcdef...
| (CAS)      |  Metadata stored in SQLite
+------------+
```

### Restore Pipeline (per file)

```
SQLite Manifest
    |
    v
+------------+
| Load       |  Read file manifest + chunk list from DB
| Metadata   |  Retrieve AES key from encryption_keys table
+-----+------+
      |
      v
+------------+
| Read Chunk |  Load from content-addressable store
+-----+------+
      |
      v
+------------+
| Decrypt    |  AES-256-CBC using stored key + embedded IV
+-----+------+
      |
      v
+------------+
| Decompress |  LZ4 or ZSTD based on stored compression type
+-----+------+
      |
      v
+------------+
| Verify     |  SHA-256 hash check per chunk
| Integrity  |  Full file hash verification after reassembly
+-----+------+
      |
      v
+------------+
| Write      |  Reassemble chunks into original file
| File       |  Recreate directory structure
+------------+
```

---

## Module Reference

### 1. Storage Engine (`include/storage/`)

#### `database.h` — SQLite Metadata Store (753 lines)

The central metadata store for all backup operations. Uses SQLite in WAL (Write-Ahead Logging) mode for concurrent read/write access.

**Tables:**

| Table             | Purpose                                     |
|-------------------|---------------------------------------------|
| `jobs`            | Backup job metadata (status, size, timestamps, compression, encryption flags) |
| `chunks`          | Content-addressable chunk registry (hash -> storage path, sizes, ref_count) |
| `file_manifests`  | Per-file metadata within a job (path, size, modification time, file hash) |
| `file_chunks`     | Chunk-to-manifest mapping (which chunks belong to which file, ordering) |
| `encryption_keys` | AES-256 keys per job (stored as hex strings) |
| `job_dependencies`| DAG edges for job scheduling                |
| `channels`        | Messaging channels                          |
| `messages`        | Channel messages (sender, content, timestamp)|

**Key classes:**
- `Database` — Full CRUD operations for all tables, with RAII connection management
- `Statement` — RAII prepared statement wrapper with automatic SQLITE_BUSY retry
- `DBLock` — RAII global mutex guard ensuring serialized DB access across modules

#### `chunk_store.h` — Content-Addressable Storage (268 lines)

Manages the physical storage of backup data chunks on disk.

- Splits files into fixed 64 KB chunks
- SHA-256 hash per chunk for content addressing
- Deduplication via database lookup before storage
- Compress -> Encrypt -> Write pipeline
- Read -> Decrypt -> Decompress -> Verify restore pipeline
- Content-addressable paths: `chunks/<first 2 hex>/<next 2 hex>/<full hash>`
- In-memory B+ tree index for fast chunk lookups
- In-memory HashMap for dedup checks

#### `rolling_checksum.h` — Adler32 Rolling Hash (62 lines)

rsync-style rolling checksum for incremental backup block matching.

- Adler32-based with modular arithmetic
- O(1) roll operation (remove old byte, add new byte)
- Bulk update for initial window computation

### 2. Cryptography (`include/crypto/`)

#### `sha256.h` — SHA-256 Hashing (123 lines)

Built on OpenSSL's EVP API (not deprecated `SHA256_*` functions).

- Single-shot hash for buffers, strings, vectors
- Streaming hash (`SHA256::Stream`) for large files without full memory load
- File hashing with 64 KB buffer reads
- Hex conversion utilities (`to_hex`, `from_hex`)

#### `aes256.h` — AES-256 Encryption (153 lines)

AES-256-CBC encryption with PKCS7 padding via OpenSSL EVP.

- CSPRNG key generation (`RAND_bytes`)
- Random IV per encryption (IV prepended to ciphertext)
- Encrypt/decrypt for buffers and vectors
- Key serialization (hex string <-> binary)

### 3. Compression (`include/compression/`)

#### `compressor.h` — LZ4/ZSTD Pipeline (109 lines)

Dual-algorithm compression with automatic fallback.

| Algorithm | Speed    | Ratio  | Use Case              |
|-----------|----------|--------|-----------------------|
| LZ4       | Very fast| Medium | Default, general use  |
| ZSTD      | Fast     | High   | Archival, cold storage|
| NONE      | N/A      | 1:1    | Pre-compressed data   |

### 4. IPC (`include/ipc/`)

#### `ipc.h` — POSIX Inter-Process Communication (256 lines)

Three IPC mechanisms for multi-process coordination:

- **SharedMemory** — POSIX `shm_open`/`mmap` segments (4 MB default) for worker progress tracking. Templated `read_at`/`write_at` for structured data.
- **MessageQueue** — Pipe-based message passing (`pipe()` + `select()` for timeouts). Workers send progress, completion, and failure messages to the orchestrator.
- **NamedSemaphore** — POSIX `sem_open` semaphores for worker count limiting (max 4 concurrent workers).
- **WorkerProgress** — Atomic struct in shared memory: job_id, bytes processed, files done, current file name.

### 5. Backup System (`include/backup/`)

#### `orchestrator.h` — Multi-Process Backup Engine (262 lines)

Central coordinator for backup operations.

- **Single-threaded mode** — Direct execution for simple backups (default CLI mode)
- **Multi-process mode** — `fork()` spawns isolated worker processes per job. Each child re-opens its own SQLite connection (required after fork). Parent monitors via `waitpid()` + IPC messages.
- Worker semaphore limits concurrent processes to `MAX_WORKER_PROCESSES` (4)
- Integrates: JobScheduler, ChunkStore, SnapshotManager, AES key management

#### `snapshot.h` — Copy-on-Write Snapshots (174 lines)

Creates consistent point-in-time views of source data.

- Attempts POSIX `link()` (hardlinks) first — true CoW semantics
- Falls back to file copy if hardlinks fail (cross-filesystem)
- Recursive directory traversal with symlink safety (`lstat`)
- Cleanup after backup completes

#### `worker.h` — Backup Worker Process (156 lines)

Executes a single backup job end-to-end.

Pipeline: Set RUNNING -> Create snapshot -> List files -> Process each file (chunk -> hash -> compress -> encrypt -> store) -> Store encryption key -> Update stats -> Mark COMPLETED -> Cleanup snapshot.

Sends IPC progress messages to orchestrator during execution.

### 6. Restore Engine (`include/restore/`)

#### `restore_engine.h` — Full Restore + Verification (153 lines)

- Restores all files from a completed backup job
- Retrieves AES key from database for decryption
- Rebuilds directory structure at destination
- Per-chunk SHA-256 integrity verification during restore
- Full file hash verification after reassembly
- `verify_backup()` — Non-destructive integrity check (verifies all chunk files exist and DB records are consistent)
- Continues restoring remaining files if one fails (partial restore)

### 7. Job Scheduler (`include/scheduler/`)

#### `job_scheduler.h` — Priority Queue + DAG Scheduler (145 lines)

Determines job execution order with dependency resolution.

- **Priority Queue** — Jobs sorted by priority (URGENT > HIGH > NORMAL > LOW), then by creation time (FIFO within same priority)
- **DAG (Directed Acyclic Graph)** — Dependency tracking with cycle detection. A job only becomes "ready" when all its dependencies are completed.
- Thread-safe with mutex protection
- Failed job cascading — dependents are automatically cancelled

### 8. Messaging (`include/messaging/`)

#### `messaging.h` — Channel-Based Communication (62 lines)

Simple enterprise messaging for backup notifications.

- Channel creation and text messaging
- File-sharing notifications linked to backup job IDs
- Event logging via CircularBuffer (last 256 events)
- Stored in SQLite for persistence

### 9. UI (`include/ui/`)

#### `terminal_ui.h` — Interactive Terminal Interface (315 lines)

Full-featured menu-driven interface for all operations: backup, restore, job listing, verification, statistics, messaging, and log level configuration.

---

## Data Structures

All data structures are implemented from scratch (no `std::map`, `std::priority_queue`, etc. for the core logic).

### HashMap (`hash_map.h`, 133 lines)

Open-addressing hash table with linear probing.

- Load factor threshold: 0.7 (auto-rehash at 2x capacity)
- Tombstone-based deletion (EMPTY / OCCUPIED / DELETED states)
- Power-of-2 capacity for fast modulo via bitmask
- Used for: Deduplication index (chunk hash -> exists)

### PriorityQueue (`priority_queue.h`, 105 lines)

Binary max-heap with custom comparator.

- O(log n) push/pop
- `remove_if()` — Remove arbitrary element by predicate
- `update()` — Re-prioritize an element in-place
- Used for: Job scheduling by priority

### DAG (`dag.h`, 144 lines)

Directed Acyclic Graph with topological ordering.

- Cycle detection via BFS path check before edge insertion
- Kahn's algorithm for topological sort
- `get_ready_nodes()` — Returns all nodes with in-degree 0
- Dependency and dependent queries
- Used for: Job dependency resolution

### CircularBuffer (`circular_buffer.h`, 95 lines)

Thread-safe ring buffer with mutex protection.

- Fixed capacity with optional overwrite mode (`push_overwrite`)
- `last_n()` — Retrieve N most recent items
- Used for: Event logging, IPC message buffering

### B+ Tree (`bplus_tree.h`, 226 lines)

Balanced search tree with linked leaf nodes.

- Order 64 (configurable via template parameter)
- O(log n) insert, find, erase
- Range queries via leaf-level linked list traversal
- In-order traversal via `for_each()`
- Used for: Chunk index (hash -> storage path)

---

## SQLite Synchronization Strategy

The project prevents `SQLITE_BUSY` errors through a multi-layer approach:

### Layer 1: WAL Mode

```sql
PRAGMA journal_mode=WAL;
PRAGMA synchronous=NORMAL;
```

WAL (Write-Ahead Logging) allows concurrent readers and a single writer without blocking. This is set at database open time.

### Layer 2: Busy Timeout

```cpp
sqlite3_busy_timeout(db_, 5000);  // 5 second timeout
```

SQLite will internally retry for up to 5 seconds before returning `SQLITE_BUSY`.

### Layer 3: Global Mutex (`DBLock`)

```cpp
inline std::mutex& db_global_mutex() {
    static std::mutex mtx;
    return mtx;
}
```

Every database operation acquires a process-wide global mutex via `DBLock`. This serializes all DB access within a single process, eliminating intra-process contention entirely.

### Layer 4: Statement-Level Retry

```cpp
int step_retry(int max_retries = 10) {
    for (int attempt = 0; attempt < max_retries; ++attempt) {
        int rc = sqlite3_step(stmt_);
        if (rc != SQLITE_BUSY) return rc;
        sleep_for(milliseconds(50 * (attempt + 1)));  // exponential backoff
    }
    return SQLITE_BUSY;
}
```

Each `sqlite3_step()` call retries up to 10 times with exponential backoff (50ms, 100ms, 150ms...) if busy.

### Layer 5: Fork Safety

After `fork()`, child processes open their own independent SQLite connection. SQLite connections cannot be shared across `fork()` boundaries — this is enforced in `orchestrator.h`:

```cpp
if (pid == 0) {
    // Child re-opens database
    Database child_db;
    child_db.open(data_dir_ + "/ecpb.db");
    // ... work with child_db ...
    child_db.close();
    _exit(result.success ? 0 : 1);
}
```

---

## Security Model

### Encryption

- **Algorithm:** AES-256-CBC (via OpenSSL EVP API)
- **Key Generation:** 256-bit CSPRNG key per backup session (`RAND_bytes`)
- **IV:** Random 128-bit IV per chunk (prepended to ciphertext)
- **Key Storage:** Hex-encoded in SQLite `encryption_keys` table, indexed by job_id
- **Padding:** PKCS7 (handled by OpenSSL)

### Integrity

- **Per-chunk:** SHA-256 hash computed before storage, verified on restore
- **Per-file:** Full file SHA-256 hash verified after chunk reassembly
- **Verification command:** `--verify <job_id>` checks all chunk files exist and DB records match

### Content Addressing

Chunks are stored at paths derived from their SHA-256 hash:
```
storage/chunks/ab/cd/abcdef0123456789...
```
This makes it impossible to modify chunk data without detection — any change would alter the hash and break the path mapping.

---

## Storage Layout

```
<data-dir>/
|-- ecpb.db                          # SQLite metadata database
|-- storage/
|   +-- chunks/
|       |-- ab/
|       |   +-- cd/
|       |       +-- abcdef01234...   # Chunk files (compressed + encrypted)
|       |-- ef/
|       |   +-- gh/
|       |       +-- efgh5678...
|       +-- ...
+-- snapshots/
    +-- snap_<job_id>_<timestamp>/    # Temporary CoW snapshots (cleaned up after backup)
```

---

## Configuration Constants

Defined in `include/common/types.h`:

| Constant                 | Value    | Description                                     |
|--------------------------|----------|-------------------------------------------------|
| `CHUNK_SIZE`             | 64 KB    | Fixed chunk size for file splitting              |
| `MAX_FILE_SIZE`          | 4 GB     | Maximum supported file size                      |
| `AES_KEY_LEN`            | 32 bytes | AES-256 key length                               |
| `AES_IV_LEN`             | 16 bytes | AES IV length                                    |
| `SQLITE_BUSY_TIMEOUT_MS` | 5000 ms | SQLite busy wait before retry                   |
| `SQLITE_MAX_RETRIES`     | 10      | Max statement retry attempts on SQLITE_BUSY     |
| `SHM_SEGMENT_SIZE`       | 4 MB    | POSIX shared memory segment size                |
| `MAX_WORKER_PROCESSES`   | 4       | Maximum concurrent fork'd backup workers        |
| `BPLUS_TREE_ORDER`       | 64      | B+ tree branching factor                        |
| `CIRCULAR_BUF_CAP`       | 1024    | Default circular buffer capacity                |
| `ROLLING_WINDOW`         | 48      | Rolling checksum window size (bytes)            |

---

## Testing

### Running Tests

```bash
# Full integration test suite (9 tests)
make test
```

### Test Coverage

| Test | Description                              | Validates                                    |
|------|------------------------------------------|----------------------------------------------|
| 1    | Backup a mixed directory                 | Text files, binary data, nested dirs, chunking|
| 2    | List all jobs                            | Job metadata persistence in SQLite           |
| 3    | System statistics                        | Chunk counting, dedup tracking, byte totals  |
| 4    | Verify backup integrity                  | All chunk files present, DB consistency       |
| 5    | Restore backup to new location           | Decrypt -> Decompress -> Reassemble pipeline |
| 6    | Byte-for-byte diff of restored files     | SHA-256 integrity, no data loss              |
| 7    | Cross-backup deduplication               | Same data backed up twice -> 0 new chunks    |
| 8    | Multi-chunk file (256 KB = 4 chunks)     | Chunk splitting and reassembly at boundaries |
| 9    | 50-file batch backup + restore           | Scalability, all 50 files restored correctly |

### Manual Testing

```bash
# Backup with debug logging
./build/ecpb --log-level 0 --backup /path/to/data --name test1

# Check deduplication (backup same data again)
./build/ecpb --backup /path/to/data --name test2
# "Stored: 0.00 B" means full deduplication worked

# Verify
./build/ecpb --verify 1

# Restore and diff
./build/ecpb --restore 1 --dest /tmp/restored
diff -r /path/to/data /tmp/restored
```

---

## Project Structure

```
enterprise-backup/
|-- Makefile                                    # Build system (66 lines)
|-- README.md                                   # This file
|-- src/
|   +-- main.cpp                                # Entry point, CLI/UI dispatch (182 lines)
+-- include/
    |-- common/
    |   |-- types.h                             # Type definitions, enums, constants (213 lines)
    |   +-- logger.h                            # Thread-safe logger with levels (56 lines)
    |-- datastructures/
    |   |-- hash_map.h                          # Open-addressing hash table (133 lines)
    |   |-- priority_queue.h                    # Binary max-heap (105 lines)
    |   |-- dag.h                               # Directed Acyclic Graph (144 lines)
    |   |-- circular_buffer.h                   # Thread-safe ring buffer (95 lines)
    |   +-- bplus_tree.h                        # B+ tree with range queries (226 lines)
    |-- storage/
    |   |-- database.h                          # SQLite metadata store (753 lines)
    |   |-- chunk_store.h                       # Content-addressable chunk storage (268 lines)
    |   +-- rolling_checksum.h                  # Adler32 rolling hash (62 lines)
    |-- crypto/
    |   |-- sha256.h                            # SHA-256 hashing via OpenSSL EVP (123 lines)
    |   +-- aes256.h                            # AES-256-CBC encryption (153 lines)
    |-- compression/
    |   +-- compressor.h                        # LZ4/ZSTD compression pipeline (109 lines)
    |-- ipc/
    |   +-- ipc.h                               # Shared memory, message queue, semaphores (256 lines)
    |-- backup/
    |   |-- orchestrator.h                      # Multi-process backup coordinator (262 lines)
    |   |-- snapshot.h                          # CoW snapshot manager (174 lines)
    |   +-- worker.h                            # Backup worker process (156 lines)
    |-- restore/
    |   +-- restore_engine.h                    # Full restore + verification (153 lines)
    |-- scheduler/
    |   +-- job_scheduler.h                     # Priority + DAG job scheduler (145 lines)
    |-- messaging/
    |   +-- messaging.h                         # Channel messaging service (62 lines)
    +-- ui/
        +-- terminal_ui.h                       # Interactive terminal interface (315 lines)

Total: 23 files, ~4,200 lines of C++17
```

---

## Technical Details

### Why Header-Only?

The project uses a header-only architecture for simplicity and fast iteration. All code lives in `.h` files under `include/`, compiled via a single translation unit (`src/main.cpp`). This eliminates link-time issues and makes the project easy to embed.

For a production deployment, the headers can be split into `.h` (declarations) + `.cpp` (implementations) and compiled separately for faster incremental builds.

### Why Not std::unordered_map / std::priority_queue?

The custom data structures serve two purposes:

1. **Educational** — Demonstrates mastery of fundamental algorithms (open-addressing, binary heaps, B+ trees, topological sort)
2. **Optimized** — The HashMap uses power-of-2 sizing with bitmask modulo (faster than `%`), the B+ tree provides range queries that `std::unordered_map` cannot, and the DAG integrates cycle detection directly into edge insertion

### Fork Safety

SQLite connections are process-local. After `fork()`, the child must open a new connection. The orchestrator enforces this:

```
Parent Process                  Child Process (after fork)
+-------------+                +-------------+
| DB conn #1  |                | DB conn #2  |  <- new connection
| Orchestrator|  --fork()-->   | Worker      |
| Scheduler   |                | ChunkStore  |
| IPC listen  |                | IPC send    |
+-------------+                +-------------+
```

### Deduplication

Content-addressable storage means identical data is only stored once, regardless of filename or location:

```
file_a.txt: "Hello World" -> SHA-256: abc123... -> stored once
file_b.txt: "Hello World" -> SHA-256: abc123... -> ref_count++ (not stored again)
```

The second backup of identical data stores 0 new bytes — only metadata references are created.

---

## License

This project is provided as-is for educational and enterprise use.
