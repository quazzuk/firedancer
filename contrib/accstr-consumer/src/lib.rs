//! Rust consumer for firedancer account update stream.
//!
//! This crate provides a safe Rust API for consuming account updates
//! from firedancer via shared memory.
//!
//! # Example
//!
//! ```rust,no_run
//! use fd_accstr_consumer::AccstrConsumer;
//!
//! fn main() -> Result<(), Box<dyn std::error::Error>> {
//!     let mut consumer = AccstrConsumer::attach("fd1_accstr")?;
//!
//!     loop {
//!         match consumer.poll()? {
//!             Some(update) => {
//!                 println!("Account updated in slot {}", update.slot);
//!                 consumer.ack();
//!             }
//!             None => {
//!                 std::thread::sleep(std::time::Duration::from_micros(10));
//!             }
//!         }
//!     }
//! }
//! ```

use std::ffi::CString;
use thiserror::Error;

// Include generated bindings
#[allow(non_upper_case_globals)]
#[allow(non_camel_case_types)]
#[allow(non_snake_case)]
#[allow(dead_code)]
#[allow(improper_ctypes)]
mod bindings {
    include!(concat!(env!("OUT_DIR"), "/bindings.rs"));
}

/// Message type constants
const FD_ACCSTR_MSG_ACCOUNT_UPDATE: u64 = 0;
const FD_ACCSTR_MSG_SLOT_BOUNDARY: u64 = 1;
const FD_ACCSTR_MSG_TXN_BOUNDARY: u64 = 2;
const FD_ACCSTR_MSG_TYPE_MASK: u64 = 0xF;

/// Size of account update header
const FD_ACCSTR_ACCOUNT_UPDATE_FIXED_SZ: usize = 100;

/// Accstr shmem magic value (must match C header)
const FD_ACCSTR_SHMEM_MAGIC: u64 = 0xACC5780000000001;

/// Errors that can occur when using the accstr consumer.
#[derive(Error, Debug)]
pub enum AccstrError {
    /// Failed to attach to the workspace.
    #[error("failed to attach to workspace: {0}")]
    AttachFailed(String),

    /// Consumer overrun - some updates were dropped.
    #[error("consumer overrun - some updates were dropped")]
    Overrun,

    /// Invalid workspace name (contains null byte).
    #[error("invalid workspace name")]
    InvalidName,
}

/// Account update received from firedancer.
#[derive(Debug, Clone)]
pub struct AccountUpdate {
    /// Account public key (32 bytes).
    pub pubkey: [u8; 32],

    /// Owner program public key (32 bytes).
    pub owner: [u8; 32],

    /// Lamports balance.
    pub lamports: u64,

    /// Slot when update occurred.
    pub slot: u64,

    /// Whether the account is executable.
    pub executable: bool,

    /// Rent epoch.
    pub rent_epoch: u64,

    /// Account data bytes.
    pub data: Vec<u8>,

    /// Monotonic write version for ordering.
    pub write_version: u64,
}

/// Slot boundary marker.
#[derive(Debug, Clone)]
pub struct SlotBoundary {
    /// Slot that just completed.
    pub slot: u64,

    /// Number of accounts updated in this slot.
    pub account_count: u64,

    /// Nanosecond timestamp.
    pub timestamp_ns: u64,
}

/// Transaction boundary marker.
#[derive(Debug, Clone)]
pub struct TxnBoundary {
    /// Slot containing this transaction.
    pub slot: u64,

    /// Index of transaction within the slot.
    pub txn_idx: u64,

    /// Transaction signature (64 bytes).
    pub signature: [u8; 64],

    /// Number of accounts modified by this transaction.
    pub account_count: u64,

    /// Whether the transaction succeeded.
    pub success: bool,

    /// Nanosecond timestamp.
    pub timestamp_ns: u64,
}

/// Shared memory header structure (matches C layout)
#[repr(C)]
struct AccstrShmemHdr {
    magic: u64,
    version: u64,
    mcache_gaddr: u64,
    dcache_gaddr: u64,
    fseq_gaddr: u64,
    depth: u64,
    mtu: u64,
    seq0: u64,
}

/// Account update structure (matches C layout)
#[repr(C, packed)]
struct AccstrAccountUpdate {
    pubkey: [u8; 32],
    owner: [u8; 32],
    lamports: u64,
    slot: u64,
    executable: u8,
    rent_epoch_hi: u8,
    rent_epoch_lo: u16,
    data_len: u64,
    write_version: u64,
}

/// Slot boundary structure (matches C layout)
#[repr(C, packed)]
struct AccstrSlotBoundary {
    slot: u64,
    account_count: u64,
    timestamp_ns: u64,
}

/// Transaction boundary structure (matches C layout)
#[repr(C, packed)]
struct AccstrTxnBoundary {
    slot: u64,
    txn_idx: u64,
    signature: [u8; 64],
    account_count: u64,
    success: i32,
    timestamp_ns: u64,
}

/// Fragment metadata (matches fd_frag_meta_t)
#[repr(C)]
#[derive(Clone, Copy)]
struct FragMeta {
    seq: u64,
    sig: u64,
    chunk: u64,
    sz: u16,
    ctl: u16,
    tsorig: u32,
    tspub: u32,
}

/// Consumer for firedancer account update stream.
pub struct AccstrConsumer {
    /// Workspace base pointer
    wksp: *mut u8,
    /// mcache pointer (array of FragMeta)
    mcache: *const FragMeta,
    /// fseq pointer
    fseq: *mut u64,
    /// Ring buffer depth
    depth: u64,
    /// Current consumer sequence
    seq: u64,
    /// Messages received count
    received_cnt: u64,
    /// Overrun count
    overrun_cnt: u64,
}

impl AccstrConsumer {
    /// Convert gaddr to laddr (inline function reimplemented)
    #[inline]
    fn laddr(&self, gaddr: u64) -> *const u8 {
        (self.wksp as u64 + gaddr) as *const u8
    }

    /// Get mcache sequence pointer (at index depth)
    #[inline]
    fn mcache_seq_ptr(&self) -> *const u64 {
        unsafe { (self.mcache as *const u64).add(self.depth as usize * 4) }
    }

    /// Attach to an accstr workspace by name.
    pub fn attach(wksp_name: &str) -> Result<Self, AccstrError> {
        let c_name = CString::new(wksp_name).map_err(|_| AccstrError::InvalidName)?;

        unsafe {
            // Attach to workspace
            let wksp = bindings::fd_wksp_attach(c_name.as_ptr());
            if wksp.is_null() {
                return Err(AccstrError::AttachFailed(format!(
                    "fd_wksp_attach failed for {}",
                    wksp_name
                )));
            }

            // Find accstr header using tag query
            let tag: u64 = 1;
            let mut info: bindings::fd_wksp_tag_query_info_t = std::mem::zeroed();
            let cnt = bindings::fd_wksp_tag_query(wksp, &tag, 1, &mut info, 1);

            if cnt != 1 {
                bindings::fd_wksp_detach(wksp);
                return Err(AccstrError::AttachFailed(format!(
                    "fd_wksp_tag_query failed for {}",
                    wksp_name
                )));
            }

            // Convert wksp to raw pointer for address calculations
            let wksp_ptr = wksp as *mut u8;

            // Get header pointer: laddr_fast(wksp, gaddr) = wksp + gaddr
            let hdr = (wksp_ptr as u64 + info.gaddr_lo) as *const AccstrShmemHdr;

            if hdr.is_null() {
                bindings::fd_wksp_detach(wksp);
                return Err(AccstrError::AttachFailed("null header".to_string()));
            }

            let hdr_ref = &*hdr;

            // Validate magic
            if hdr_ref.magic != FD_ACCSTR_SHMEM_MAGIC {
                bindings::fd_wksp_detach(wksp);
                return Err(AccstrError::AttachFailed(format!(
                    "invalid magic: expected {:x}, got {:x}",
                    FD_ACCSTR_SHMEM_MAGIC, hdr_ref.magic
                )));
            }

            // Get mcache pointer
            let mcache = (wksp_ptr as u64 + hdr_ref.mcache_gaddr) as *const FragMeta;

            // Get fseq pointer
            let fseq = (wksp_ptr as u64 + hdr_ref.fseq_gaddr) as *mut u64;

            // Initialize fseq to starting sequence
            std::ptr::write_volatile(fseq, hdr_ref.seq0);

            Ok(Self {
                wksp: wksp_ptr,
                mcache,
                fseq,
                depth: hdr_ref.depth,
                seq: hdr_ref.seq0,
                received_cnt: 0,
                overrun_cnt: 0,
            })
        }
    }

    /// Poll for the next account update.
    ///
    /// Returns `Ok(Some(update))` if an update is available,
    /// `Ok(None)` if no data, or `Err(Overrun)` if updates were dropped.
    pub fn poll(&mut self) -> Result<Option<AccountUpdate>, AccstrError> {
        unsafe {
            // Read producer sequence from mcache seq location
            let seq_ptr = self.mcache_seq_ptr();
            let tx_seq = std::ptr::read_volatile(seq_ptr);

            // Check if producer has new data
            let diff = tx_seq.wrapping_sub(self.seq) as i64;

            if diff <= 0 {
                return Ok(None);
            }

            // Check for overrun
            if diff > self.depth as i64 {
                self.overrun_cnt += 1;
                self.seq = tx_seq.wrapping_sub(self.depth - 1);
                return Err(AccstrError::Overrun);
            }

            // Read fragment metadata
            let idx = self.seq & (self.depth - 1);
            let mline = self.mcache.add(idx as usize);

            // Read with memory fence
            std::sync::atomic::fence(std::sync::atomic::Ordering::SeqCst);
            let meta = std::ptr::read_volatile(mline);
            std::sync::atomic::fence(std::sync::atomic::Ordering::SeqCst);

            // Verify sequence matches
            if meta.seq != self.seq {
                return Ok(None);
            }

            // Get payload pointer
            let payload = self.laddr(meta.chunk);

            // Decode message type
            let msg_type = meta.sig & FD_ACCSTR_MSG_TYPE_MASK;

            if msg_type == FD_ACCSTR_MSG_ACCOUNT_UPDATE {
                let update_ptr = payload as *const AccstrAccountUpdate;
                let update = &*update_ptr;

                let mut pubkey = [0u8; 32];
                let mut owner = [0u8; 32];
                pubkey.copy_from_slice(&update.pubkey);
                owner.copy_from_slice(&update.owner);

                let rent_epoch =
                    ((update.rent_epoch_hi as u64) << 16) | (update.rent_epoch_lo as u64);

                let data = if update.data_len > 0 {
                    let data_ptr = payload.add(FD_ACCSTR_ACCOUNT_UPDATE_FIXED_SZ);
                    std::slice::from_raw_parts(data_ptr, update.data_len as usize).to_vec()
                } else {
                    Vec::new()
                };

                self.received_cnt += 1;

                Ok(Some(AccountUpdate {
                    pubkey,
                    owner,
                    lamports: update.lamports,
                    slot: update.slot,
                    executable: update.executable != 0,
                    rent_epoch,
                    data,
                    write_version: update.write_version,
                }))
            } else if msg_type == FD_ACCSTR_MSG_SLOT_BOUNDARY
                || msg_type == FD_ACCSTR_MSG_TXN_BOUNDARY
            {
                // Skip slot/txn boundary, advance sequence
                self.seq = self.seq.wrapping_add(1);
                std::ptr::write_volatile(self.fseq, self.seq);
                Ok(None)
            } else {
                // Unknown message type, skip
                self.seq = self.seq.wrapping_add(1);
                std::ptr::write_volatile(self.fseq, self.seq);
                Ok(None)
            }
        }
    }

    /// Poll for a slot boundary marker.
    pub fn poll_slot_boundary(&mut self) -> Result<Option<SlotBoundary>, AccstrError> {
        unsafe {
            let seq_ptr = self.mcache_seq_ptr();
            let tx_seq = std::ptr::read_volatile(seq_ptr);

            let diff = tx_seq.wrapping_sub(self.seq) as i64;

            if diff <= 0 {
                return Ok(None);
            }

            if diff > self.depth as i64 {
                self.overrun_cnt += 1;
                self.seq = tx_seq.wrapping_sub(self.depth - 1);
                return Err(AccstrError::Overrun);
            }

            let idx = self.seq & (self.depth - 1);
            let mline = self.mcache.add(idx as usize);

            std::sync::atomic::fence(std::sync::atomic::Ordering::SeqCst);
            let meta = std::ptr::read_volatile(mline);
            std::sync::atomic::fence(std::sync::atomic::Ordering::SeqCst);

            if meta.seq != self.seq {
                return Ok(None);
            }

            let msg_type = meta.sig & FD_ACCSTR_MSG_TYPE_MASK;

            if msg_type == FD_ACCSTR_MSG_SLOT_BOUNDARY {
                let payload = self.laddr(meta.chunk);
                let boundary_ptr = payload as *const AccstrSlotBoundary;
                let boundary = &*boundary_ptr;

                Ok(Some(SlotBoundary {
                    slot: boundary.slot,
                    account_count: boundary.account_count,
                    timestamp_ns: boundary.timestamp_ns,
                }))
            } else {
                Ok(None)
            }
        }
    }

    /// Poll for a transaction boundary marker.
    ///
    /// Transaction boundaries allow consumers to batch account updates
    /// per transaction and apply them atomically.
    pub fn poll_txn_boundary(&mut self) -> Result<Option<TxnBoundary>, AccstrError> {
        unsafe {
            let seq_ptr = self.mcache_seq_ptr();
            let tx_seq = std::ptr::read_volatile(seq_ptr);

            let diff = tx_seq.wrapping_sub(self.seq) as i64;

            if diff <= 0 {
                return Ok(None);
            }

            if diff > self.depth as i64 {
                self.overrun_cnt += 1;
                self.seq = tx_seq.wrapping_sub(self.depth - 1);
                return Err(AccstrError::Overrun);
            }

            let idx = self.seq & (self.depth - 1);
            let mline = self.mcache.add(idx as usize);

            std::sync::atomic::fence(std::sync::atomic::Ordering::SeqCst);
            let meta = std::ptr::read_volatile(mline);
            std::sync::atomic::fence(std::sync::atomic::Ordering::SeqCst);

            if meta.seq != self.seq {
                return Ok(None);
            }

            let msg_type = meta.sig & FD_ACCSTR_MSG_TYPE_MASK;

            if msg_type == FD_ACCSTR_MSG_TXN_BOUNDARY {
                let payload = self.laddr(meta.chunk);
                let boundary_ptr = payload as *const AccstrTxnBoundary;
                let boundary = &*boundary_ptr;

                let mut signature = [0u8; 64];
                signature.copy_from_slice(&boundary.signature);

                Ok(Some(TxnBoundary {
                    slot: boundary.slot,
                    txn_idx: boundary.txn_idx,
                    signature,
                    account_count: boundary.account_count,
                    success: boundary.success != 0,
                    timestamp_ns: boundary.timestamp_ns,
                }))
            } else {
                Ok(None)
            }
        }
    }

    /// Acknowledge processing of the current update.
    pub fn ack(&mut self) {
        self.seq = self.seq.wrapping_add(1);
        unsafe {
            std::ptr::write_volatile(self.fseq, self.seq);
        }
    }

    /// Get consumer statistics.
    pub fn get_stats(&self) -> (u64, u64) {
        (self.received_cnt, self.overrun_cnt)
    }
}

impl Drop for AccstrConsumer {
    fn drop(&mut self) {
        unsafe {
            if !self.wksp.is_null() {
                bindings::fd_wksp_detach(self.wksp as *mut bindings::fd_wksp_t);
            }
        }
    }
}

unsafe impl Send for AccstrConsumer {}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_invalid_name() {
        let result = AccstrConsumer::attach("name\0with\0nulls");
        assert!(matches!(result, Err(AccstrError::InvalidName)));
    }
}
