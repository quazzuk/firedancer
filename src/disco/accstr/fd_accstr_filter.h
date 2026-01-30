#ifndef HEADER_fd_src_disco_accstr_fd_accstr_filter_h
#define HEADER_fd_src_disco_accstr_fd_accstr_filter_h

/* fd_accstr_filter.h provides a fast owner-based filter for account
   updates. It uses a two-stage approach:

   1. Bloom filter for fast rejection (~50ns, O(1))
   2. Exact pubkey match for confirmation

   This achieves sub-100ns filtering in the common case (reject) while
   maintaining perfect accuracy (no false negatives).

   Usage:
     fd_accstr_filter_t filter[1];
     fd_accstr_filter_init( filter );
     fd_accstr_filter_add_owner( filter, owner_pubkey );
     ...
     if( fd_accstr_filter_check( filter, account_owner ) ) {
       // Account passes filter
     } */

#include "../../util/fd_util_base.h"

/* Maximum number of owner pubkeys that can be filtered */

#define FD_ACCSTR_FILTER_MAX_OWNERS (256UL)

/* Bloom filter size in bits (must be power of 2) */

#define FD_ACCSTR_FILTER_BLOOM_BITS (4096UL)

/* Number of hash functions for bloom filter */

#define FD_ACCSTR_FILTER_BLOOM_K    (3UL)

struct fd_accstr_filter {
  ulong  owner_cnt;                                     /* Number of configured owners */
  uchar  owners[FD_ACCSTR_FILTER_MAX_OWNERS][32];       /* Owner pubkeys */
  ulong  bloom[FD_ACCSTR_FILTER_BLOOM_BITS / 64];       /* Bloom filter bits */
  int    pass_all;                                      /* If 1, pass all accounts */
};
typedef struct fd_accstr_filter fd_accstr_filter_t;

FD_PROTOTYPES_BEGIN

/* fd_accstr_filter_init initializes a filter to pass no accounts.
   Call fd_accstr_filter_add_owner to add owner pubkeys to pass.
   Call fd_accstr_filter_set_pass_all to pass all accounts. */

static inline void
fd_accstr_filter_init( fd_accstr_filter_t * filter ) {
  filter->owner_cnt = 0;
  filter->pass_all = 0;
  fd_memset( filter->bloom, 0, sizeof(filter->bloom) );
}

/* fd_accstr_filter_set_pass_all configures the filter to pass all
   accounts regardless of owner. Useful for debugging or when no
   filtering is needed. */

static inline void
fd_accstr_filter_set_pass_all( fd_accstr_filter_t * filter ) {
  filter->pass_all = 1;
}

/* Simple hash function for bloom filter.
   Uses FNV-1a variant with different seeds for each hash function. */

static inline ulong
fd_accstr_filter_hash( uchar const owner[32],
                       ulong       seed ) {
  ulong h = 14695981039346656037UL ^ seed; /* FNV offset basis */
  for( ulong i = 0; i < 32; i++ ) {
    h ^= (ulong)owner[i];
    h *= 1099511628211UL; /* FNV prime */
  }
  return h;
}

/* fd_accstr_filter_add_owner adds an owner pubkey to the filter.
   Returns 0 on success, -1 if filter is full. */

static inline int
fd_accstr_filter_add_owner( fd_accstr_filter_t * filter,
                            uchar const          owner[32] ) {
  if( FD_UNLIKELY( filter->owner_cnt >= FD_ACCSTR_FILTER_MAX_OWNERS ) ) {
    return -1;
  }

  /* Add to owner list */
  fd_memcpy( filter->owners[filter->owner_cnt], owner, 32 );
  filter->owner_cnt++;

  /* Add to bloom filter with multiple hashes */
  for( ulong k = 0; k < FD_ACCSTR_FILTER_BLOOM_K; k++ ) {
    ulong h = fd_accstr_filter_hash( owner, k );
    ulong idx = h & (FD_ACCSTR_FILTER_BLOOM_BITS - 1);
    filter->bloom[idx / 64] |= (1UL << (idx % 64));
  }

  return 0;
}

/* fd_accstr_filter_check returns 1 if the account with the given
   owner should be published, 0 if it should be filtered out.

   Fast path (~50ns): Bloom filter rejects most non-matching owners.
   Slow path (~200ns): Exact match confirms passing accounts. */

static inline int
fd_accstr_filter_check( fd_accstr_filter_t const * filter,
                        uchar const                owner[32] ) {
  /* Fast path: pass all mode */
  if( FD_UNLIKELY( filter->pass_all ) ) {
    return 1;
  }

  /* Fast path: no owners configured */
  if( FD_UNLIKELY( filter->owner_cnt == 0 ) ) {
    return 0;
  }

  /* Bloom filter check - all k bits must be set */
  for( ulong k = 0; k < FD_ACCSTR_FILTER_BLOOM_K; k++ ) {
    ulong h = fd_accstr_filter_hash( owner, k );
    ulong idx = h & (FD_ACCSTR_FILTER_BLOOM_BITS - 1);
    if( !(filter->bloom[idx / 64] & (1UL << (idx % 64))) ) {
      /* Definitely not in set */
      return 0;
    }
  }

  /* Bloom filter indicates possible match - do exact check */
  for( ulong i = 0; i < filter->owner_cnt; i++ ) {
    if( FD_UNLIKELY( fd_memeq( owner, filter->owners[i], 32 ) ) ) {
      return 1;
    }
  }

  /* False positive from bloom filter */
  return 0;
}

/* fd_accstr_filter_owner_cnt returns the number of configured owners */

static inline ulong
fd_accstr_filter_owner_cnt( fd_accstr_filter_t const * filter ) {
  return filter->owner_cnt;
}

/* fd_accstr_filter_is_pass_all returns 1 if filter passes all accounts */

static inline int
fd_accstr_filter_is_pass_all( fd_accstr_filter_t const * filter ) {
  return filter->pass_all;
}

FD_PROTOTYPES_END

#endif /* HEADER_fd_src_disco_accstr_fd_accstr_filter_h */
