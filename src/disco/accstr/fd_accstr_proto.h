#ifndef HEADER_fd_src_disco_accstr_fd_accstr_proto_h
#define HEADER_fd_src_disco_accstr_fd_accstr_proto_h

/* fd_accstr_proto.h defines the wire protocol for streaming account
   updates from firedancer to external processes via shared memory.

   The account stream uses the standard tango mcache/dcache pattern.
   Messages are published to an mcache with payload data in the dcache.
   External consumers attach via fd_wksp_attach() and poll the mcache.

   Message types:
   - FD_ACCSTR_MSG_ACCOUNT_UPDATE: Full account state after modification
   - FD_ACCSTR_MSG_SLOT_BOUNDARY: Marker at end of each slot
   - FD_ACCSTR_MSG_TXN_BOUNDARY: Marker at end of each transaction

   The signature field (fd_frag_meta_t.sig) encodes the message type
   in the low 4 bits. */

#include "../../util/fd_util_base.h"

/* Message type identifiers (stored in low 4 bits of sig) */

#define FD_ACCSTR_MSG_ACCOUNT_UPDATE  (0UL)
#define FD_ACCSTR_MSG_SLOT_BOUNDARY   (1UL)
#define FD_ACCSTR_MSG_TXN_BOUNDARY    (2UL)

#define FD_ACCSTR_MSG_TYPE_MASK       (0xFUL)

/* Extract message type from signature */

#define FD_ACCSTR_SIG_TYPE( sig ) ((sig) & FD_ACCSTR_MSG_TYPE_MASK)

/* fd_accstr_account_update_t is the wire format for account updates.
   The structure is followed immediately by the account data bytes.

   Total fixed header size: 32 + 32 + 8 + 8 + 1 + 1 + 2 + 8 + 8 = 100 bytes
   Followed by data[data_len] bytes. */

struct __attribute__((packed, aligned(8))) fd_accstr_account_update {
  uchar  pubkey[32];       /* Account public key */
  uchar  owner[32];        /* Owner program public key */
  ulong  lamports;         /* Lamports balance */
  ulong  slot;             /* Slot when update occurred */
  uchar  executable;       /* Executable flag (0 or 1) */
  uchar  rent_epoch_hi;    /* Rent epoch high byte */
  ushort rent_epoch_lo;    /* Rent epoch low 16 bits */
  ulong  data_len;         /* Length of account data following header */
  ulong  write_version;    /* Monotonically increasing write version for ordering */
  /* uchar data[data_len] follows immediately */
};
typedef struct fd_accstr_account_update fd_accstr_account_update_t;

#define FD_ACCSTR_ACCOUNT_UPDATE_FIXED_SZ (sizeof(fd_accstr_account_update_t))

/* Reconstruct full rent epoch from compressed representation */

static inline ulong
fd_accstr_rent_epoch( fd_accstr_account_update_t const * update ) {
  return ((ulong)update->rent_epoch_hi << 16) | (ulong)update->rent_epoch_lo;
}

/* fd_accstr_slot_boundary_t marks the end of a slot.
   Sent after all account updates for a slot have been published. */

struct __attribute__((packed, aligned(8))) fd_accstr_slot_boundary {
  ulong slot;              /* Slot that just completed */
  ulong account_count;     /* Number of accounts updated in this slot */
  ulong timestamp_ns;      /* Nanosecond wallclock timestamp */
};
typedef struct fd_accstr_slot_boundary fd_accstr_slot_boundary_t;

#define FD_ACCSTR_SLOT_BOUNDARY_SZ (sizeof(fd_accstr_slot_boundary_t))

/* fd_accstr_txn_boundary_t marks the end of a transaction.
   Sent after all account updates for a transaction have been published.
   This allows consumers to apply account updates atomically per transaction. */

struct __attribute__((packed, aligned(8))) fd_accstr_txn_boundary {
  ulong slot;              /* Slot containing this transaction */
  ulong txn_idx;           /* Index of transaction within the slot */
  uchar signature[64];     /* Transaction signature */
  ulong account_count;     /* Number of accounts modified by this txn */
  int   success;           /* 1 if txn succeeded, 0 if failed */
  ulong timestamp_ns;      /* Nanosecond wallclock timestamp */
};
typedef struct fd_accstr_txn_boundary fd_accstr_txn_boundary_t;

#define FD_ACCSTR_TXN_BOUNDARY_SZ (sizeof(fd_accstr_txn_boundary_t))

/* fd_accstr_shmem_hdr_t is the header at the start of the accstr
   workspace. External consumers read this to discover the mcache,
   dcache, and fseq locations. */

struct __attribute__((aligned(128))) fd_accstr_shmem_hdr {
  ulong magic;             /* == FD_ACCSTR_SHMEM_MAGIC */
  ulong version;           /* Protocol version (currently 1) */
  ulong mcache_gaddr;      /* Global address of mcache in workspace */
  ulong dcache_gaddr;      /* Global address of dcache in workspace */
  ulong fseq_gaddr;        /* Global address of consumer fseq */
  ulong depth;             /* mcache depth (power of 2) */
  ulong mtu;               /* Maximum message size (dcache slot size) */
  ulong seq0;              /* Initial sequence number */
  uchar _pad[64];          /* Pad to 128 bytes */
};
typedef struct fd_accstr_shmem_hdr fd_accstr_shmem_hdr_t;

#define FD_ACCSTR_SHMEM_MAGIC   (0xACC5780000000001UL)
#define FD_ACCSTR_SHMEM_VERSION (1UL)

/* Default configuration values */

#define FD_ACCSTR_DEFAULT_DEPTH     (32768UL)  /* mcache depth (32K entries) */
#define FD_ACCSTR_DEFAULT_MTU       (10485860UL) /* 10MB + 100 byte header */

/* Maximum account data size (10MB, matching Solana's limit) */

#define FD_ACCSTR_MAX_ACCOUNT_DATA_SZ (10UL * 1024UL * 1024UL)

#endif /* HEADER_fd_src_disco_accstr_fd_accstr_proto_h */
