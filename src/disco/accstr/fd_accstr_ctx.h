#ifndef HEADER_fd_src_disco_accstr_fd_accstr_ctx_h
#define HEADER_fd_src_disco_accstr_fd_accstr_ctx_h

/* fd_accstr_ctx.h provides a context for streaming account updates
   to external processes via shared memory. The context is designed
   to be embedded in the exec tile and publish account updates inline
   during transaction commit (similar to fd_capture_ctx pattern).

   Usage:
     1. Create accstr workspace with fd_accstr_new()
     2. Join from exec tile with fd_accstr_join()
     3. Configure owner filter with fd_accstr_filter_add_owner()
     4. Call fd_accstr_publish() on each account update in commit path
     5. External consumers attach via fd_accstr_consumer API */

#include "fd_accstr_proto.h"
#include "fd_accstr_filter.h"
#include "../../tango/mcache/fd_mcache.h"
#include "../../tango/dcache/fd_dcache.h"
#include "../../tango/fseq/fd_fseq.h"
#include "../../util/wksp/fd_wksp.h"

#define FD_ACCSTR_CTX_MAGIC (0xACC5C7C000000001UL)

/* fd_accstr_ctx_t is the producer-side context for streaming account
   updates. It holds references to the mcache/dcache and the filter. */

struct fd_accstr_ctx {
  ulong magic;                       /* == FD_ACCSTR_CTX_MAGIC */

  /* Workspace and shared memory structures */
  fd_wksp_t *            wksp;       /* Workspace containing all structures */
  fd_accstr_shmem_hdr_t * hdr;       /* Shared memory header */
  fd_frag_meta_t *       mcache;     /* Message cache (ring buffer metadata) */
  uchar *                dcache;     /* Data cache (ring buffer payload) */
  ulong *                fseq;       /* Consumer flow control sequence */

  /* Producer state */
  ulong  depth;                      /* mcache depth (power of 2) */
  ulong  seq;                        /* Next sequence number to publish */
  ulong * mseq;                      /* mcache sequence pointer for updates */
  ulong  chunk0;                     /* First chunk in dcache */
  ulong  wmark;                      /* Watermark for chunk wraparound */
  ulong  chunk;                      /* Current chunk for next write */
  ulong  mtu;                        /* Max transmission unit */

  /* Write version for ordering */
  ulong  write_version;              /* Monotonically increasing counter */

  /* Owner filter */
  fd_accstr_filter_t filter[1];      /* Inline filter structure */

  /* Statistics */
  ulong  published_cnt;              /* Accounts published */
  ulong  filtered_cnt;               /* Accounts filtered out */
  ulong  overrun_cnt;                /* Consumer overruns detected */
};
typedef struct fd_accstr_ctx fd_accstr_ctx_t;

FD_PROTOTYPES_BEGIN

/* fd_accstr_ctx_align returns the alignment required for accstr context */

FD_FN_CONST static inline ulong
fd_accstr_ctx_align( void ) {
  return 128UL;
}

/* fd_accstr_ctx_footprint returns the footprint for accstr context */

FD_FN_CONST static inline ulong
fd_accstr_ctx_footprint( void ) {
  return sizeof(fd_accstr_ctx_t);
}

/* fd_accstr_wksp_footprint returns the total workspace footprint needed
   for the shared memory channel with the given configuration. */

FD_FN_PURE static inline ulong
fd_accstr_wksp_footprint( ulong depth,
                          ulong mtu ) {
  ulong l = FD_LAYOUT_INIT;
  l = FD_LAYOUT_APPEND( l, 128UL,                  sizeof(fd_accstr_shmem_hdr_t) );
  l = FD_LAYOUT_APPEND( l, fd_mcache_align(),      fd_mcache_footprint( depth, 0 ) );
  l = FD_LAYOUT_APPEND( l, fd_dcache_align(),      fd_dcache_req_data_sz( mtu, depth, 1, 1 ) );
  l = FD_LAYOUT_APPEND( l, fd_fseq_align(),        fd_fseq_footprint() );
  return FD_LAYOUT_FINI( l, 128UL );
}

/* fd_accstr_new initializes a new accstr shared memory region.
   mem points to a workspace allocation of at least fd_accstr_wksp_footprint bytes.
   Returns mem on success, NULL on failure. */

void *
fd_accstr_new( void * mem,
               ulong  depth,
               ulong  mtu,
               ulong  seq0 );

/* fd_accstr_join joins an accstr context to the shared memory region.
   ctx points to an fd_accstr_ctx_t (can be on stack or in scratch).
   wksp_name is the name of the workspace to attach to.
   Returns ctx on success, NULL on failure. */

fd_accstr_ctx_t *
fd_accstr_join( fd_accstr_ctx_t * ctx,
                char const *      wksp_name );

/* fd_accstr_leave leaves the accstr context.
   Returns ctx on success, NULL on failure. */

void *
fd_accstr_leave( fd_accstr_ctx_t * ctx );

/* fd_accstr_publish publishes an account update to the shared memory
   channel. This should be called from the commit path for each
   modified account.

   Returns 1 if the account was published, 0 if filtered out. */

static inline int
fd_accstr_publish( fd_accstr_ctx_t *   ctx,
                   uchar const         pubkey[32],
                   uchar const         owner[32],
                   ulong               lamports,
                   uchar               executable,
                   ulong               rent_epoch,
                   ulong               slot,
                   uchar const *       data,
                   ulong               data_len ) {

  if( FD_UNLIKELY( !ctx || ctx->magic != FD_ACCSTR_CTX_MAGIC ) ) {
    return 0;
  }

  /* Check owner filter */
  if( !fd_accstr_filter_check( ctx->filter, owner ) ) {
    ctx->filtered_cnt++;
    return 0;
  }

  /* Calculate message size */
  ulong msg_sz = FD_ACCSTR_ACCOUNT_UPDATE_FIXED_SZ + data_len;
  if( FD_UNLIKELY( msg_sz > ctx->mtu ) ) {
    /* Account too large - skip it */
    return 0;
  }

  /* Wait for consumer to catch up if needed (simple flow control) */
  if( FD_LIKELY( ctx->fseq ) ) {
    ulong consumer_seq = fd_fseq_query( ctx->fseq );
    long diff = fd_seq_diff( ctx->seq, consumer_seq );
    if( FD_UNLIKELY( diff >= (long)ctx->depth ) ) {
      /* Consumer is too slow - overwrite oldest */
      ctx->overrun_cnt++;
    }
  }

  /* Write to dcache */
  uchar * dst = (uchar *)fd_chunk_to_laddr( ctx->wksp, ctx->chunk );

  /* Write header */
  fd_accstr_account_update_t * update = (fd_accstr_account_update_t *)dst;
  fd_memcpy( update->pubkey, pubkey, 32 );
  fd_memcpy( update->owner, owner, 32 );
  update->lamports      = lamports;
  update->slot          = slot;
  update->executable    = executable;
  update->rent_epoch_hi = (uchar)(rent_epoch >> 16);
  update->rent_epoch_lo = (ushort)(rent_epoch & 0xFFFF);
  update->data_len      = data_len;
  update->write_version = ctx->write_version++;

  /* Write account data */
  if( FD_LIKELY( data_len > 0 && data ) ) {
    fd_memcpy( dst + FD_ACCSTR_ACCOUNT_UPDATE_FIXED_SZ, data, data_len );
  }

  /* Publish to mcache */
  ulong sig = FD_ACCSTR_MSG_ACCOUNT_UPDATE;
  ulong ctl = fd_frag_meta_ctl( 0, 1, 1, 0 ); /* SOM | EOM */
  ulong tsnow = (ulong)fd_log_wallclock();
  fd_mcache_publish( ctx->mcache, ctx->depth, ctx->seq, sig, ctx->chunk, (ushort)msg_sz, ctl, tsnow, tsnow );

  /* Advance sequence and chunk */
  ctx->seq = fd_seq_inc( ctx->seq, 1 );
  ctx->chunk = fd_dcache_compact_next( ctx->chunk, msg_sz, ctx->chunk0, ctx->wmark );

  /* Update mcache sequence so consumers see the new message */
  fd_mcache_seq_update( ctx->mseq, ctx->seq );

  ctx->published_cnt++;
  return 1;
}

/* fd_accstr_publish_slot_boundary publishes a slot boundary marker.
   Call this at the end of each slot after all account updates. */

static inline void
fd_accstr_publish_slot_boundary( fd_accstr_ctx_t * ctx,
                                 ulong             slot,
                                 ulong             account_count ) {
  if( FD_UNLIKELY( !ctx || ctx->magic != FD_ACCSTR_CTX_MAGIC ) ) {
    return;
  }

  /* Write to dcache */
  uchar * dst = (uchar *)fd_chunk_to_laddr( ctx->wksp, ctx->chunk );
  fd_accstr_slot_boundary_t * boundary = (fd_accstr_slot_boundary_t *)dst;
  boundary->slot          = slot;
  boundary->account_count = account_count;
  boundary->timestamp_ns  = (ulong)fd_log_wallclock();

  /* Publish to mcache */
  ulong sig = FD_ACCSTR_MSG_SLOT_BOUNDARY;
  ulong ctl = fd_frag_meta_ctl( 0, 1, 1, 0 ); /* SOM | EOM */
  ulong tsnow = boundary->timestamp_ns;
  fd_mcache_publish( ctx->mcache, ctx->depth, ctx->seq, sig, ctx->chunk, FD_ACCSTR_SLOT_BOUNDARY_SZ, ctl, tsnow, tsnow );

  /* Advance sequence and chunk */
  ctx->seq = fd_seq_inc( ctx->seq, 1 );
  ctx->chunk = fd_dcache_compact_next( ctx->chunk, FD_ACCSTR_SLOT_BOUNDARY_SZ, ctx->chunk0, ctx->wmark );

  /* Update mcache sequence so consumers see the new message */
  fd_mcache_seq_update( ctx->mseq, ctx->seq );
}

/* fd_accstr_publish_txn_boundary publishes a transaction boundary marker.
   Call this after all account updates for a single transaction have been
   published. This allows consumers to apply account updates atomically
   per transaction.

   slot - the slot containing this transaction
   txn_idx - the index of this transaction within the slot
   signature - the 64-byte transaction signature
   account_count - number of accounts modified by this transaction
   success - 1 if transaction succeeded, 0 if failed */

static inline void
fd_accstr_publish_txn_boundary( fd_accstr_ctx_t *   ctx,
                                ulong               slot,
                                ulong               txn_idx,
                                uchar const         signature[64],
                                ulong               account_count,
                                int                 success ) {
  if( FD_UNLIKELY( !ctx || ctx->magic != FD_ACCSTR_CTX_MAGIC ) ) {
    return;
  }

  /* Write to dcache */
  uchar * dst = (uchar *)fd_chunk_to_laddr( ctx->wksp, ctx->chunk );
  fd_accstr_txn_boundary_t * boundary = (fd_accstr_txn_boundary_t *)dst;
  boundary->slot          = slot;
  boundary->txn_idx       = txn_idx;
  fd_memcpy( boundary->signature, signature, 64 );
  boundary->account_count = account_count;
  boundary->success       = success;
  boundary->timestamp_ns  = (ulong)fd_log_wallclock();

  /* Publish to mcache */
  ulong sig = FD_ACCSTR_MSG_TXN_BOUNDARY;
  ulong ctl = fd_frag_meta_ctl( 0, 1, 1, 0 ); /* SOM | EOM */
  ulong tsnow = boundary->timestamp_ns;
  fd_mcache_publish( ctx->mcache, ctx->depth, ctx->seq, sig, ctx->chunk, FD_ACCSTR_TXN_BOUNDARY_SZ, ctl, tsnow, tsnow );

  /* Advance sequence and chunk */
  ctx->seq = fd_seq_inc( ctx->seq, 1 );
  ctx->chunk = fd_dcache_compact_next( ctx->chunk, FD_ACCSTR_TXN_BOUNDARY_SZ, ctx->chunk0, ctx->wmark );

  /* Update mcache sequence so consumers see the new message */
  fd_mcache_seq_update( ctx->mseq, ctx->seq );
}

FD_PROTOTYPES_END

#endif /* HEADER_fd_src_disco_accstr_fd_accstr_ctx_h */
