# fd-accstr-consumer

Rust consumer library for the firedancer account update stream.

## Overview

This crate provides a safe Rust API for consuming account updates from firedancer via shared memory. It uses bindgen to generate FFI bindings to the C implementation, providing sub-microsecond latency access to account state changes.

## Prerequisites

Before building this crate, you must build firedancer:

```bash
cd /path/to/firedancer
make -j
```

This creates the static libraries (`libfd_disco.a`, `libfd_tango.a`, `libfd_util.a`) that this crate links against.

## Building

```bash
cd contrib/accstr-consumer
cargo build --release
```

## Example

Run the example consumer (requires a running firedancer with accstr enabled):

```bash
cargo run --release --bin accstr-example -- fd1_accstr
```

## Library Usage

Add to your `Cargo.toml`:

```toml
[dependencies]
fd-accstr-consumer = { path = "/path/to/firedancer/contrib/accstr-consumer" }
```

Then use in your code:

```rust
use fd_accstr_consumer::{AccstrConsumer, AccountUpdate};

fn main() -> Result<(), Box<dyn std::error::Error>> {
    // Attach to workspace
    let mut consumer = AccstrConsumer::attach("fd1_accstr")?;

    loop {
        match consumer.poll()? {
            Some(update) => {
                println!("Account {} updated in slot {}",
                    bs58::encode(&update.pubkey).into_string(),
                    update.slot);
                println!("  Owner: {}", bs58::encode(&update.owner).into_string());
                println!("  Lamports: {}", update.lamports);
                println!("  Data len: {}", update.data.len());

                // Must acknowledge before next poll
                consumer.ack();
            }
            None => {
                // No data available
                std::thread::sleep(std::time::Duration::from_micros(10));
            }
        }
    }
}
```

## API

### `AccstrConsumer`

Main consumer struct.

- `attach(wksp_name: &str)` - Attach to a firedancer accstr workspace
- `poll()` - Poll for the next account update (non-blocking)
- `poll_slot_boundary()` - Poll for slot boundary markers
- `ack()` - Acknowledge processing of current update
- `get_stats()` - Get (received_count, overrun_count)

### `AccountUpdate`

Account state update.

```rust
pub struct AccountUpdate {
    pub pubkey: [u8; 32],
    pub owner: [u8; 32],
    pub lamports: u64,
    pub slot: u64,
    pub executable: bool,
    pub rent_epoch: u64,
    pub data: Vec<u8>,
    pub write_version: u64,
}
```

### `SlotBoundary`

End-of-slot marker.

```rust
pub struct SlotBoundary {
    pub slot: u64,
    pub account_count: u64,
    pub timestamp_ns: u64,
}
```

### `AccstrError`

Error type.

- `AttachFailed(String)` - Failed to attach to workspace
- `Overrun` - Consumer was too slow, some updates dropped
- `InvalidName` - Workspace name contains null bytes

## Thread Safety

`AccstrConsumer` implements `Send` but not `Sync`. It should be used from a single thread. For multi-threaded consumers, create separate `AccstrConsumer` instances per thread (each will receive all updates).

## Performance

- Poll latency: ~50-100ns (no syscalls)
- Zero-copy for account data reads
- Automatic overrun detection and recovery
