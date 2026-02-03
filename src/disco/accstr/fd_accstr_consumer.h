#ifndef HEADER_fd_src_disco_accstr_fd_accstr_consumer_h
#define HEADER_fd_src_disco_accstr_fd_accstr_consumer_h

/* fd_accstr_consumer.h provides an API for external processes to
   consume account updates from firedancer via shared memory.

   Usage:
     fd_accstr_consumer_t consumer[1];
     if( fd_accstr_consumer_attach( consumer, "fd1_accstr" ) ) {
       for(;;) {
         fd_accstr_account_update_t const * update;
         uchar const * data;
         int rc = fd_accstr_consumer_poll( consumer, &update, &data );
         if( rc == 1 ) {
           // Process update
           fd_accstr_consumer_ack( consumer );
         } else if( rc == -1 ) {
           // Consumer overrun - some updates dropped
         }
       }
       fd_accstr_consumer_detach( consumer );
     }

   The consumer is designed for a single-threaded polling model with
   minimal latency. No syscalls occur in the poll path. */

#include "fd_accstr_proto.h"
#include "../../tango/mcache/fd_mcache.h"
#include "../../tango/dcache/fd_dcache.h"
#include "../../tango/fseq/fd_fseq.h"
#include "../../util/wksp/fd_wksp.h"

/* fd_accstr_consumer_t is the consumer-side context for receiving
   account updates. Can be allocated on stack or heap. */

struct fd_accstr_consumer {
  fd_wksp_t *              wksp;     /* Attached workspace */
  fd_accstr_shmem_hdr_t *  hdr;      /* Shared memory header */
  fd_frag_meta_t const *   mcache;   /* Message cache (read-only) */
  uchar const *            dcache;   /* Data cache base (read-only) */
  ulong *                  fseq;     /* Flow control sequence (read-write) */

  ulong  depth;                      /* mcache depth */
  ulong  seq;                        /* Next expected sequence number */

  /* Cached current message (valid after successful poll) */
  fd_frag_meta_t           meta[1];  /* Current fragment metadata */

  /* Statistics */
  ulong  received_cnt;               /* Messages received */
  ulong  overrun_cnt;                /* Overruns detected */
};
typedef struct fd_accstr_consumer fd_accstr_consumer_t;

FD_PROTOTYPES_BEGIN

/* fd_accstr_consumer_attach attaches a consumer to an accstr workspace.
   consumer is a pointer to an fd_accstr_consumer_t (can be on stack).
   wksp_name is the name of the workspace to attach to.

   Returns consumer on success, NULL on failure. */

fd_accstr_consumer_t *
fd_accstr_consumer_attach( fd_accstr_consumer_t * consumer,
                           char const *           wksp_name );

/* fd_accstr_consumer_detach detaches a consumer from the workspace.
   Returns consumer on success, NULL on failure. */

void *
fd_accstr_consumer_detach( fd_accstr_consumer_t * consumer );

/* fd_accstr_consumer_poll polls for the next account update.

   On success:
     - Returns 1
     - *update_out points to the account update header
     - *data_out points to the account data bytes (update->data_len bytes)
     - Caller must call fd_accstr_consumer_ack() after processing

   If no data available:
     - Returns 0
     - *update_out and *data_out are undefined

   On consumer overrun:
     - Returns -1
     - Some messages were lost
     - Consumer will resync to latest available sequence
     - *update_out and *data_out are undefined

   This function is non-blocking and does not make syscalls. */

static inline int
fd_accstr_consumer_poll( fd_accstr_consumer_t *               consumer,
                         fd_accstr_account_update_t const * * update_out,
                         uchar const * *                      data_out ) {

  /* Read producer sequence */
  ulong const * seq = fd_mcache_seq_laddr_const( consumer->mcache );
  ulong tx_seq = fd_mcache_seq_query( seq );

  /* Check if producer has new data */
  long diff = fd_seq_diff( tx_seq, consumer->seq );

  if( FD_UNLIKELY( diff <= 0L ) ) {
    /* No new data available */
    return 0;
  }

  /* Check for overrun */
  if( FD_UNLIKELY( diff > (long)consumer->depth ) ) {
    /* Consumer overrun - we lost messages */
    consumer->overrun_cnt++;
    /* Resync to latest available */
    consumer->seq = fd_seq_dec( tx_seq, consumer->depth - 1 );
    return -1;
  }

  /* Read fragment metadata */
  ulong idx = fd_mcache_line_idx( consumer->seq, consumer->depth );
  fd_frag_meta_t const * mline = consumer->mcache + idx;

  /* Copy metadata with fences */
  FD_COMPILER_MFENCE();
  consumer->meta[0] = *mline;
  FD_COMPILER_MFENCE();

  /* Verify sequence matches (handles concurrent overwrite) */
  if( FD_UNLIKELY( consumer->meta->seq != consumer->seq ) ) {
    /* Metadata was overwritten during read - retry */
    return 0;
  }

  /* Get payload pointer */
  uchar const * payload = (uchar const *)fd_chunk_to_laddr_const( consumer->wksp, consumer->meta->chunk );

  /* Decode based on message type */
  ulong msg_type = FD_ACCSTR_SIG_TYPE( consumer->meta->sig );

  if( FD_LIKELY( msg_type == FD_ACCSTR_MSG_ACCOUNT_UPDATE ) ) {
    *update_out = (fd_accstr_account_update_t const *)payload;
    *data_out   = payload + FD_ACCSTR_ACCOUNT_UPDATE_FIXED_SZ;
    consumer->received_cnt++;
    return 1;
  } else if( msg_type == FD_ACCSTR_MSG_SLOT_BOUNDARY ||
             msg_type == FD_ACCSTR_MSG_TXN_BOUNDARY ) {
    /* Slot or txn boundary - skip and continue */
    consumer->seq = fd_seq_inc( consumer->seq, 1 );
    fd_fseq_update( consumer->fseq, consumer->seq );
    return 0;
  }

  /* Unknown message type - skip */
  consumer->seq = fd_seq_inc( consumer->seq, 1 );
  fd_fseq_update( consumer->fseq, consumer->seq );
  return 0;
}

/* fd_accstr_consumer_poll_slot_boundary polls for a slot boundary.
   Similar to fd_accstr_consumer_poll but returns slot boundaries.

   Returns 1 if a slot boundary was found, 0 otherwise.
   *boundary_out points to the slot boundary on success. */

static inline int
fd_accstr_consumer_poll_slot_boundary( fd_accstr_consumer_t *           consumer,
                                       fd_accstr_slot_boundary_t const * * boundary_out ) {

  ulong const * seq = fd_mcache_seq_laddr_const( consumer->mcache );
  ulong tx_seq = fd_mcache_seq_query( seq );
  long diff = fd_seq_diff( tx_seq, consumer->seq );

  if( diff <= 0L ) return 0;

  if( diff > (long)consumer->depth ) {
    consumer->overrun_cnt++;
    consumer->seq = fd_seq_dec( tx_seq, consumer->depth - 1 );
    return -1;
  }

  ulong idx = fd_mcache_line_idx( consumer->seq, consumer->depth );
  fd_frag_meta_t const * mline = consumer->mcache + idx;

  FD_COMPILER_MFENCE();
  consumer->meta[0] = *mline;
  FD_COMPILER_MFENCE();

  if( consumer->meta->seq != consumer->seq ) return 0;

  ulong msg_type = FD_ACCSTR_SIG_TYPE( consumer->meta->sig );

  if( msg_type == FD_ACCSTR_MSG_SLOT_BOUNDARY ) {
    uchar const * payload = (uchar const *)fd_chunk_to_laddr_const( consumer->wksp, consumer->meta->chunk );
    *boundary_out = (fd_accstr_slot_boundary_t const *)payload;
    return 1;
  }

  return 0;
}

/* fd_accstr_consumer_poll_txn_boundary polls for a transaction boundary.
   Similar to fd_accstr_consumer_poll but returns transaction boundaries.

   Returns 1 if a transaction boundary was found, 0 otherwise, -1 on overrun.
   *boundary_out points to the transaction boundary on success.

   This allows consumers to batch account updates per transaction and
   apply them atomically. */

static inline int
fd_accstr_consumer_poll_txn_boundary( fd_accstr_consumer_t *          consumer,
                                      fd_accstr_txn_boundary_t const * * boundary_out ) {

  ulong const * seq = fd_mcache_seq_laddr_const( consumer->mcache );
  ulong tx_seq = fd_mcache_seq_query( seq );
  long diff = fd_seq_diff( tx_seq, consumer->seq );

  if( diff <= 0L ) return 0;

  if( diff > (long)consumer->depth ) {
    consumer->overrun_cnt++;
    consumer->seq = fd_seq_dec( tx_seq, consumer->depth - 1 );
    return -1;
  }

  ulong idx = fd_mcache_line_idx( consumer->seq, consumer->depth );
  fd_frag_meta_t const * mline = consumer->mcache + idx;

  FD_COMPILER_MFENCE();
  consumer->meta[0] = *mline;
  FD_COMPILER_MFENCE();

  if( consumer->meta->seq != consumer->seq ) return 0;

  ulong msg_type = FD_ACCSTR_SIG_TYPE( consumer->meta->sig );

  if( msg_type == FD_ACCSTR_MSG_TXN_BOUNDARY ) {
    uchar const * payload = (uchar const *)fd_chunk_to_laddr_const( consumer->wksp, consumer->meta->chunk );
    *boundary_out = (fd_accstr_txn_boundary_t const *)payload;
    return 1;
  }

  return 0;
}

/* fd_accstr_consumer_ack acknowledges processing of the current message.
   Must be called after successfully processing a poll result.
   This advances the consumer sequence and updates flow control. */

static inline void
fd_accstr_consumer_ack( fd_accstr_consumer_t * consumer ) {
  consumer->seq = fd_seq_inc( consumer->seq, 1 );
  fd_fseq_update( consumer->fseq, consumer->seq );
}

/* fd_accstr_consumer_get_stats returns consumer statistics.
   received is the number of messages successfully received.
   overruns is the number of overrun events (lost messages). */

static inline void
fd_accstr_consumer_get_stats( fd_accstr_consumer_t const * consumer,
                              ulong *                      received,
                              ulong *                      overruns ) {
  if( received ) *received = consumer->received_cnt;
  if( overruns ) *overruns = consumer->overrun_cnt;
}

FD_PROTOTYPES_END

#endif /* HEADER_fd_src_disco_accstr_fd_accstr_consumer_h */
