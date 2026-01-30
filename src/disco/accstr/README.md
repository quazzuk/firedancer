# Account Stream (accstr)

Stream account updates from Firedancer to external processes via shared memory with sub-microsecond latency.

## Overview

The accstr module provides a high-performance channel for streaming account state changes to external consumers. It uses Firedancer's Tango IPC infrastructure (mcache/dcache/fseq) over shared memory, avoiding network overhead and syscalls in the hot path.

**Key features:**
- ~300ns publish latency (no syscalls)
- Zero-copy reads for consumers
- Owner-based filtering with bloom filter (~50ns filter check)
- Ring buffer with configurable depth (drop-oldest on overflow)
- Multiple consumer support via shared memory

## Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                        Firedancer                                │
│                                                                  │
│  ┌──────────┐    fd_runtime_save_account()    ┌──────────────┐  │
│  │ Exec Tile │ ─────────────────────────────► │ accstr_ctx   │  │
│  └──────────┘                                  │              │  │
│                                                │ ┌──────────┐ │  │
│                                                │ │  Filter  │ │  │
│                                                │ └────┬─────┘ │  │
│                                                │      │       │  │
│                                                │ ┌────▼─────┐ │  │
│                                                │ │ mcache/  │ │  │
│                                                │ │ dcache   │ │  │
│                                                └─┴──────────┴─┘  │
└───────────────────────────────┬─────────────────────────────────┘
                                │ shared memory
                                │ (fd_wksp)
                    ┌───────────┴───────────┐
                    │                       │
              ┌─────▼─────┐           ┌─────▼─────┐
              │ Consumer  │           │ Consumer  │
              │ (C/Rust)  │           │ (C/Rust)  │
              └───────────┘           └───────────┘
```

## Building

```bash
# Build everything
make -j

# Build just the accstr components
make -j fd_accstr_example test_accstr

# The following are built:
#   - libfd_disco.a (includes accstr objects)
#   - build/native/gcc/bin/fd_accstr_example
#   - build/native/gcc/unit-test/test_accstr
```

## Testing

### Unit Test

Run the unit test to verify accstr functionality:

```bash
# Build and run
make run-unit-test-test_accstr

# Or manually
./build/native/gcc/unit-test/test_accstr
```

Expected output:
```
NOTICE  Testing accstr functionality
NOTICE  PASS: Created anonymous accstr workspace
NOTICE  PASS: Configured owner filter
NOTICE  PASS: Published account update
NOTICE  PASS: Correctly filtered account with wrong owner
NOTICE  PASS: mcache shows 1 message published
NOTICE  PASS: Message type is ACCOUNT_UPDATE
NOTICE  PASS: Pubkey matches
NOTICE  PASS: Owner matches
NOTICE  PASS: Lamports match
NOTICE  PASS: Slot matches
NOTICE  PASS: Data length matches
NOTICE  PASS: Account data matches
NOTICE  PASS: Statistics correct (published=1, filtered=1)
NOTICE  PASS: Published slot boundary
NOTICE  PASS: Cleaned up anonymous workspace
NOTICE  All tests passed!
```

### Integration Test

To test with a live Firedancer instance:

1. Start Firedancer with accstr enabled (see Integration section)
2. In another terminal, run the example consumer:

```bash
./build/native/gcc/bin/fd_accstr_example fd1_accstr
```

## Integration with Firedancer

### Step 1: Create Workspace at Startup

In your main process before forking tiles, create the accstr workspace:

```c
#include "disco/accstr/fd_accstr_setup.h"

/* At startup */
int err = fd_accstr_wksp_create(
    "fd1_accstr",              /* workspace name */
    FD_SHMEM_HUGE_PAGE_SZ,     /* 2MB huge pages */
    FD_ACCSTR_DEFAULT_DEPTH,   /* 32768 entries */
    FD_ACCSTR_DEFAULT_MTU      /* 10MB + header */
);
if( err ) {
    FD_LOG_ERR(( "Failed to create accstr workspace" ));
}
```

### Step 2: Attach in Exec Tile

In `fd_exec_tile.c`, add to the tile context and initialization:

```c
#include "disco/accstr/fd_accstr_ctx.h"

/* Add to fd_exec_tile_ctx_t struct */
typedef struct fd_exec_tile_ctx {
    /* ... existing fields ... */
    fd_accstr_ctx_t accstr_ctx[1];
} fd_exec_tile_ctx_t;

/* In unprivileged_init(), after runtime setup */
static void
unprivileged_init( fd_topo_t * topo, fd_topo_tile_t * tile ) {
    /* ... existing init ... */

    /* Attach to accstr workspace */
    char const * accstr_wksp = "fd1_accstr";  /* from config */
    if( fd_accstr_wksp_attach( ctx->accstr_ctx, accstr_wksp ) ) {
        /* Configure owner filters */
        uchar token_program[32] = {
            0x06,0xdd,0xf6,0xe1,0xd7,0x65,0xa1,0x93,
            0xd9,0xcb,0xe1,0x46,0xce,0xeb,0x79,0xac,
            0x1c,0xb4,0x85,0xed,0x5f,0x5b,0x37,0x91,
            0x3a,0x8c,0xf5,0x85,0x7e,0xff,0x00,0xa9
        };
        fd_accstr_filter_add_owner( ctx->accstr_ctx->filter, token_program );

        /* Connect to runtime */
        ctx->runtime->log.accstr_ctx = ctx->accstr_ctx;

        FD_LOG_NOTICE(( "Account streaming enabled" ));
    } else {
        ctx->runtime->log.accstr_ctx = NULL;
        FD_LOG_WARNING(( "Account streaming disabled" ));
    }
}
```

### Step 3: Cleanup at Shutdown

```c
/* In tile cleanup */
fd_accstr_wksp_detach( ctx->accstr_ctx );

/* In main process cleanup */
fd_accstr_wksp_destroy( "fd1_accstr" );
```

## Consumer API (C)

### Basic Usage

```c
#include "disco/accstr/fd_accstr_consumer.h"

int main( void ) {
    fd_boot( &argc, &argv );

    /* Attach to workspace */
    fd_accstr_consumer_t consumer[1];
    if( !fd_accstr_consumer_attach( consumer, "fd1_accstr" ) ) {
        FD_LOG_ERR(( "Failed to attach" ));
        return 1;
    }

    /* Poll loop */
    for(;;) {
        fd_accstr_account_update_t const * update;
        uchar const * data;

        int rc = fd_accstr_consumer_poll( consumer, &update, &data );

        if( rc == 1 ) {
            /* Got an account update */
            printf( "Account updated in slot %lu\n", update->slot );
            printf( "  Lamports: %lu\n", update->lamports );
            printf( "  Data len: %lu\n", update->data_len );

            /* Acknowledge processing */
            fd_accstr_consumer_ack( consumer );

        } else if( rc == 0 ) {
            /* No data available */
            fd_log_sleep( 1000 ); /* 1ms */

        } else if( rc == -1 ) {
            /* Consumer overrun - some updates were dropped */
            ulong received, overruns;
            fd_accstr_consumer_get_stats( consumer, &received, &overruns );
            FD_LOG_WARNING(( "Overrun! Total: %lu", overruns ));
        }
    }

    fd_accstr_consumer_detach( consumer );
    fd_halt();
    return 0;
}
```

### Handling Slot Boundaries

```c
/* After checking for account updates */
if( rc == 0 ) {
    fd_accstr_slot_boundary_t const * boundary;
    rc = fd_accstr_consumer_poll_slot_boundary( consumer, &boundary );

    if( rc == 1 ) {
        printf( "Slot %lu completed: %lu accounts updated\n",
                boundary->slot, boundary->account_count );
        fd_accstr_consumer_ack( consumer );
    }
}
```

### Handling Transaction Boundaries

Transaction boundaries allow atomic batching of account updates per transaction:

```c
/* Buffer pending account updates */
account_update_t pending[MAX_UPDATES];
ulong pending_cnt = 0;

for(;;) {
    fd_accstr_account_update_t const * update;
    uchar const * data;

    int rc = fd_accstr_consumer_poll( consumer, &update, &data );
    if( rc == 1 ) {
        /* Buffer the update */
        pending[pending_cnt++] = copy_update( update, data );
        fd_accstr_consumer_ack( consumer );
        continue;
    }

    /* Check for transaction boundary */
    fd_accstr_txn_boundary_t const * txn_boundary;
    rc = fd_accstr_consumer_poll_txn_boundary( consumer, &txn_boundary );
    if( rc == 1 ) {
        /* Apply all buffered updates atomically */
        if( txn_boundary->success ) {
            apply_atomic_batch( pending, pending_cnt );
        } else {
            /* Transaction failed - discard updates */
        }
        pending_cnt = 0;
        fd_accstr_consumer_ack( consumer );
        continue;
    }

    /* Check for slot boundary */
    fd_accstr_slot_boundary_t const * slot_boundary;
    rc = fd_accstr_consumer_poll_slot_boundary( consumer, &slot_boundary );
    if( rc == 1 ) {
        printf( "Slot %lu completed\n", slot_boundary->slot );
        fd_accstr_consumer_ack( consumer );
        continue;
    }

    /* No data available */
    fd_log_sleep( 1000 );
}
```

## Consumer API (Rust)

A Rust consumer library is available at `contrib/accstr-consumer/`.

### Setup

```toml
# Cargo.toml
[dependencies]
fd-accstr-consumer = { path = "../firedancer/contrib/accstr-consumer" }
```

### Usage

```rust
use fd_accstr_consumer::{AccstrConsumer, AccountUpdate};

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let mut consumer = AccstrConsumer::attach("fd1_accstr")?;

    loop {
        match consumer.poll()? {
            Some(update) => {
                println!("Account {} updated in slot {}",
                    bs58::encode(&update.pubkey).into_string(),
                    update.slot);
                println!("  Lamports: {}", update.lamports);
                println!("  Data len: {}", update.data.len());
                consumer.ack();
            }
            None => {
                std::thread::sleep(std::time::Duration::from_micros(100));
            }
        }
    }
}
```

## Configuration

### Workspace Parameters

| Parameter | Default | Description |
|-----------|---------|-------------|
| `depth` | 32768 | Ring buffer entries (must be power of 2) |
| `mtu` | 10485860 | Max message size (10MB + 100 byte header) |
| `page_sz` | 2MB | Huge page size for workspace |

### Owner Filtering

Filter accounts by owner program to reduce noise:

```c
/* Add specific owners */
fd_accstr_filter_add_owner( ctx->filter, token_program_id );
fd_accstr_filter_add_owner( ctx->filter, my_program_id );

/* Or pass all accounts (for debugging) */
fd_accstr_filter_set_pass_all( ctx->filter );

/* Clear all filters */
fd_accstr_filter_init( ctx->filter );
```

Common program IDs:

| Program | Base58 |
|---------|--------|
| Token Program | `TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA` |
| Token 2022 | `TokenzQdBNbLqP5VEhdkAS6EPFLC1PHnBqCXEpPxuEb` |
| System Program | `11111111111111111111111111111111` |
| Orca Whirlpool | `whirLbMiicVdio4qvUfM5KAg6Ct8VwpYzGff3uctyCc` |
| Raydium AMM | `675kPX9MHTjS2zt1qfr1NYHuzeLXfQM9H24wFSUt1Mp8` |

## Wire Protocol

### Account Update Message

```c
struct fd_accstr_account_update {
    uchar  pubkey[32];       /* Account public key */
    uchar  owner[32];        /* Owner program public key */
    ulong  lamports;         /* Lamports balance */
    ulong  slot;             /* Slot when update occurred */
    uchar  executable;       /* Executable flag */
    uchar  rent_epoch_hi;    /* Rent epoch (high byte) */
    ushort rent_epoch_lo;    /* Rent epoch (low 16 bits) */
    ulong  data_len;         /* Length of account data */
    ulong  write_version;    /* Monotonic ordering version */
    /* uchar data[data_len] follows */
};
```

Total fixed header: 100 bytes

### Slot Boundary Message

```c
struct fd_accstr_slot_boundary {
    ulong slot;              /* Completed slot */
    ulong account_count;     /* Accounts updated in slot */
    ulong timestamp_ns;      /* Wallclock timestamp */
};
```

### Transaction Boundary Message

```c
struct fd_accstr_txn_boundary {
    ulong slot;              /* Slot containing this transaction */
    ulong txn_idx;           /* Index within the slot */
    uchar signature[64];     /* Transaction signature */
    ulong account_count;     /* Accounts modified by this txn */
    int   success;           /* 1 if succeeded, 0 if failed */
    ulong timestamp_ns;      /* Wallclock timestamp */
};
```

Transaction boundaries allow consumers to apply account updates atomically
per transaction rather than per slot. This is useful for applications that
need to track which transaction caused which account changes.

### Message Types

| Type | Value | Description |
|------|-------|-------------|
| `FD_ACCSTR_MSG_ACCOUNT_UPDATE` | 0 | Account state change |
| `FD_ACCSTR_MSG_SLOT_BOUNDARY` | 1 | End of slot marker |
| `FD_ACCSTR_MSG_TXN_BOUNDARY` | 2 | End of transaction marker |

## Performance

| Operation | Latency |
|-----------|---------|
| Owner filter check (bloom) | ~20ns |
| Owner filter check (full) | ~50ns |
| Header write (100 bytes) | ~50ns |
| Data copy | ~1µs/MB |
| mcache publish | ~50ns |
| **Total (small account)** | **~200-300ns** |

- No syscalls in publish path
- Zero-copy reads for consumers
- Lock-free ring buffer

## Troubleshooting

### Consumer not receiving updates

1. Verify workspace exists: `fd_wksp_ctl query fd1_accstr`
2. Check owner filter configuration
3. Ensure `runtime->log.accstr_ctx` is set in exec tile

### Consumer overruns

Increase `depth` or optimize consumer processing:

```c
/* Use larger depth */
fd_accstr_wksp_create( "fd1_accstr", page_sz, 65536, mtu );
```

### Workspace creation fails

Check huge pages are available:

```bash
cat /proc/meminfo | grep Huge
# Ensure HugePages_Free > 0
```

## Files

| File | Description |
|------|-------------|
| `fd_accstr_proto.h` | Wire protocol definitions |
| `fd_accstr_filter.h` | Owner filtering (bloom + hash set) |
| `fd_accstr_ctx.h` | Producer context (inline publish) |
| `fd_accstr_ctx.c` | Producer implementation |
| `fd_accstr_consumer.h` | Consumer API (inline poll) |
| `fd_accstr_consumer.c` | Consumer implementation |
| `fd_accstr_setup.h` | Workspace setup helpers |
| `fd_accstr_setup.c` | Setup implementation |
| `fd_accstr_example.c` | Example consumer program |
| `test_accstr.c` | Unit tests |
