/* This software was written by Dirk Engling <erdgeist@erdgeist.org>
   It is considered beerware. Prost. Skol. Cheers or whatever.

   $id$ */

/* System */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>

/* Libowfat */
#include "byte.h"
#include "io.h"

/* Opentracker */
#include "trackerlogic.h"
#include "ot_mutex.h"
#include "ot_stats.h"
#include "ot_clean.h"
#include "ot_accesslist.h"
#include "ot_fullscrape.h"
#include "ot_livesync.h"

/* Forward declaration */
size_t return_peers_for_torrent( ot_torrent *torrent, size_t amount, char *reply, PROTO_FLAG proto );

void free_peerlist( ot_peerlist *peer_list ) {
  if( peer_list->peers.data ) {
    if( OT_PEERLIST_HASBUCKETS( peer_list ) ) {
      ot_vector *bucket_list = (ot_vector*)(peer_list->peers.data);

      while( peer_list->peers.size-- )
        free( bucket_list++->data );
    }
    free( peer_list->peers.data );
  }
  free( peer_list );
}

size_t add_peer_to_torrent_and_return_peers( ot_hash hash, ot_peer *peer, PROTO_FLAG proto, size_t amount, char * reply ) {
  int         exactmatch, delta_torrentcount = 0;
  size_t      reply_size;
  ot_torrent *torrent;
  ot_peer    *peer_dest;
  ot_vector  *torrents_list = mutex_bucket_lock_by_hash( hash );

  if( !accesslist_hashisvalid( hash ) ) {
    mutex_bucket_unlock_by_hash( hash, 0 );
    return 0;
  }

  torrent = vector_find_or_insert( torrents_list, (void*)hash, sizeof( ot_torrent ), OT_HASH_COMPARE_SIZE, &exactmatch );
  if( !torrent ) {
    mutex_bucket_unlock_by_hash( hash, 0 );
    return 0;
  }

  if( !exactmatch ) {
    /* Create a new torrent entry, then */
    memcpy( torrent->hash, hash, sizeof(ot_hash) );

    if( !( torrent->peer_list = malloc( sizeof (ot_peerlist) ) ) ) {
      vector_remove_torrent( torrents_list, torrent );
      mutex_bucket_unlock_by_hash( hash, 0 );
      return 0;
    }

    byte_zero( torrent->peer_list, sizeof( ot_peerlist ) );
    delta_torrentcount = 1;
  } else
    clean_single_torrent( torrent );

  torrent->peer_list->base = g_now_minutes;

  /* Check for peer in torrent */
  peer_dest = vector_find_or_insert_peer( &(torrent->peer_list->peers), peer, &exactmatch );
  if( !peer_dest ) {
    mutex_bucket_unlock_by_hash( hash, delta_torrentcount );
    return 0;
  }

  /* Tell peer that it's fresh */
  OT_PEERTIME( peer ) = 0;

  /* Sanitize flags: Whoever claims to have completed download, must be a seeder */
  if( ( OT_PEERFLAG( peer ) & ( PEER_FLAG_COMPLETED | PEER_FLAG_SEEDING ) ) == PEER_FLAG_COMPLETED )
    OT_PEERFLAG( peer ) ^= PEER_FLAG_COMPLETED;

  /* If we hadn't had a match create peer there */
  if( !exactmatch ) {

#ifdef WANT_SYNC_LIVE
    if( proto == FLAG_MCA )
      OT_PEERFLAG( peer ) |= PEER_FLAG_FROM_SYNC;
    else
      livesync_tell( hash, peer );
#endif

    torrent->peer_list->peer_count++;
    if( OT_PEERFLAG(peer) & PEER_FLAG_COMPLETED )
      torrent->peer_list->down_count++;
    if( OT_PEERFLAG(peer) & PEER_FLAG_SEEDING )
      torrent->peer_list->seed_count++;

  } else {
    stats_issue_event( EVENT_RENEW, 0, OT_PEERTIME( peer_dest ) );

#ifdef WANT_SYNC_LIVE
    /* Won't live sync peers that come back too fast. Only exception:
       fresh "completed" reports */
    if( proto != FLAG_MCA ) {
      if( OT_PEERTIME( peer_dest ) > OT_CLIENT_SYNC_RENEW_BOUNDARY ||
         ( !(OT_PEERFLAG(peer_dest) & PEER_FLAG_COMPLETED ) && (OT_PEERFLAG(peer) & PEER_FLAG_COMPLETED ) ) )
        livesync_tell( hash, peer );
    }
#endif

    if(  (OT_PEERFLAG(peer_dest) & PEER_FLAG_SEEDING )   && !(OT_PEERFLAG(peer) & PEER_FLAG_SEEDING ) )
      torrent->peer_list->seed_count--;
    if( !(OT_PEERFLAG(peer_dest) & PEER_FLAG_SEEDING )   &&  (OT_PEERFLAG(peer) & PEER_FLAG_SEEDING ) )
      torrent->peer_list->seed_count++;
    if( !(OT_PEERFLAG(peer_dest) & PEER_FLAG_COMPLETED ) &&  (OT_PEERFLAG(peer) & PEER_FLAG_COMPLETED ) )
      torrent->peer_list->down_count++;
    if(   OT_PEERFLAG(peer_dest) & PEER_FLAG_COMPLETED )
      OT_PEERFLAG( peer ) |= PEER_FLAG_COMPLETED;
  }

  memcpy( peer_dest, peer, sizeof(ot_peer) );
#ifdef WANT_SYNC
  if( proto == FLAG_MCA ) {
    mutex_bucket_unlock_by_hash( hash, delta_torrentcount );
    return 0;
  }
#endif

  reply_size = return_peers_for_torrent( torrent, amount, reply, proto );
  mutex_bucket_unlock_by_hash( torrent->hash, delta_torrentcount );
  return reply_size;
}

static size_t return_peers_all( ot_peerlist *peer_list, char *reply ) {
  unsigned int bucket, num_buckets = 1;
  ot_vector * bucket_list = &peer_list->peers;
  char      * r = reply;

  if( OT_PEERLIST_HASBUCKETS(peer_list) ) {
    num_buckets = bucket_list->size;
    bucket_list = (ot_vector *)bucket_list->data;
  }

  for( bucket = 0; bucket<num_buckets; ++bucket ) {
    ot_peer * peers = (ot_peer*)bucket_list[bucket].data;
    size_t    peer_count = bucket_list[bucket].size;
    while( peer_count-- ) {
      memcpy(r,peers++,OT_PEER_COMPARE_SIZE);
      r+=OT_PEER_COMPARE_SIZE;
    }
  }

  return r - reply;
}

static size_t return_peers_selection( ot_peerlist *peer_list, size_t amount, char *reply ) {
  unsigned int bucket_offset, bucket_index = 0, num_buckets = 1;
  ot_vector  * bucket_list = &peer_list->peers;
  unsigned int shifted_pc = peer_list->peer_count;
  unsigned int shifted_step = 0;
  unsigned int shift = 0;
  char       * r = reply;

  if( OT_PEERLIST_HASBUCKETS(peer_list) ) {
    num_buckets = bucket_list->size;
    bucket_list = (ot_vector *)bucket_list->data;
  }

  /* Make fixpoint arithmetic as exact as possible */
#define MAXPRECBIT (1<<(8*sizeof(int)-3))
  while( !(shifted_pc & MAXPRECBIT ) ) { shifted_pc <<= 1; shift++; }
  shifted_step = shifted_pc/amount;
#undef MAXPRECBIT

  /* Initialize somewhere in the middle of peers so that
   fixpoint's aliasing doesn't alway miss the same peers */
  bucket_offset = random() % peer_list->peer_count;

  while( amount-- ) {
    ot_peer * peer;

    /* This is the aliased, non shifted range, next value may fall into */
    unsigned int diff = ( ( ( amount + 1 ) * shifted_step ) >> shift ) -
                        ( (   amount       * shifted_step ) >> shift );
    bucket_offset += 1 + random() % diff;

    while( bucket_offset >= bucket_list[bucket_index].size ) {
      bucket_offset -= bucket_list[bucket_index].size;
      bucket_index = ( bucket_index + 1 ) % num_buckets;
    }
    peer = ((ot_peer*)bucket_list[bucket_index].data) + bucket_offset;
    memcpy(r,peer,OT_PEER_COMPARE_SIZE);
    r+=OT_PEER_COMPARE_SIZE;
  }
  return r - reply;
}

/* Compiles a list of random peers for a torrent
   * reply must have enough space to hold 92+6*amount bytes
   * does not yet check not to return self
*/
size_t return_peers_for_torrent( ot_torrent *torrent, size_t amount, char *reply, PROTO_FLAG proto ) {
  ot_peerlist *peer_list = torrent->peer_list;
  char        *r = reply;

  if( amount > peer_list->peer_count )
    amount = peer_list->peer_count;

  if( proto == FLAG_TCP ) {
    int erval = OT_CLIENT_REQUEST_INTERVAL_RANDOM;
    r += sprintf( r, "d8:completei%zde10:downloadedi%zde10:incompletei%zde8:intervali%ie12:min intervali%ie" PEERS_BENCODED "%zd:", peer_list->seed_count, peer_list->down_count, peer_list->peer_count-peer_list->seed_count, erval, erval/2, OT_PEER_COMPARE_SIZE*amount );
  } else {
    *(uint32_t*)(r+0) = htonl( OT_CLIENT_REQUEST_INTERVAL_RANDOM );
    *(uint32_t*)(r+4) = htonl( peer_list->peer_count );
    *(uint32_t*)(r+8) = htonl( peer_list->seed_count );
    r += 12;
  }

  if( amount ) {
    if( amount == peer_list->peer_count )
      r += return_peers_all( peer_list, r );
    else
      r += return_peers_selection( peer_list, amount, r );
  }

  if( proto == FLAG_TCP )
    *r++ = 'e';

  return r - reply;
}

/* Fetches scrape info for a specific torrent */
size_t return_udp_scrape_for_torrent( ot_hash hash, char *reply ) {
  int          exactmatch, delta_torrentcount = 0;
  ot_vector   *torrents_list = mutex_bucket_lock_by_hash( hash );
  ot_torrent  *torrent = binary_search( hash, torrents_list->data, torrents_list->size, sizeof( ot_torrent ), OT_HASH_COMPARE_SIZE, &exactmatch );

  if( !exactmatch ) {
    memset( reply, 0, 12);
  } else {
    uint32_t *r = (uint32_t*) reply;

    if( clean_single_torrent( torrent ) ) {
      vector_remove_torrent( torrents_list, torrent );
      memset( reply, 0, 12);
      delta_torrentcount = -1;
    } else {
      r[0] = htonl( torrent->peer_list->seed_count );
      r[1] = htonl( torrent->peer_list->down_count );
      r[2] = htonl( torrent->peer_list->peer_count-torrent->peer_list->seed_count );
    }
  }
  mutex_bucket_unlock_by_hash( hash, delta_torrentcount );
  return 12;
}

/* Fetches scrape info for a specific torrent */
size_t return_tcp_scrape_for_torrent( ot_hash *hash_list, int amount, char *reply ) {
  char *r = reply;
  int   exactmatch, i;

  r += sprintf( r, "d5:filesd" );

  for( i=0; i<amount; ++i ) {
    int          delta_torrentcount = 0;
    ot_hash     *hash = hash_list + i;
    ot_vector   *torrents_list = mutex_bucket_lock_by_hash( *hash );
    ot_torrent  *torrent = binary_search( hash, torrents_list->data, torrents_list->size, sizeof( ot_torrent ), OT_HASH_COMPARE_SIZE, &exactmatch );

    if( exactmatch ) {
      if( clean_single_torrent( torrent ) ) {
        vector_remove_torrent( torrents_list, torrent );
        delta_torrentcount = -1;
      } else {
        *r++='2';*r++='0';*r++=':';
        memcpy( r, hash, sizeof(ot_hash) ); r+=sizeof(ot_hash);
        r += sprintf( r, "d8:completei%zde10:downloadedi%zde10:incompletei%zdee",
          torrent->peer_list->seed_count, torrent->peer_list->down_count, torrent->peer_list->peer_count-torrent->peer_list->seed_count );
      }
    }
    mutex_bucket_unlock_by_hash( *hash, delta_torrentcount );
  }

  *r++ = 'e'; *r++ = 'e';
  return r - reply;
}

static ot_peerlist dummy_list;
size_t remove_peer_from_torrent( ot_hash hash, ot_peer *peer, char *reply, PROTO_FLAG proto ) {
  int          exactmatch;
  size_t       reply_size = 0;
  ot_vector   *torrents_list = mutex_bucket_lock_by_hash( hash );
  ot_torrent  *torrent = binary_search( hash, torrents_list->data, torrents_list->size, sizeof( ot_torrent ), OT_HASH_COMPARE_SIZE, &exactmatch );
  ot_peerlist *peer_list = &dummy_list;

#ifdef WANT_SYNC_LIVE
  if( proto != FLAG_MCA ) {
    OT_PEERFLAG( peer ) |= PEER_FLAG_STOPPED;
    livesync_tell( hash, peer );
  }
#endif

  if( exactmatch ) {
    peer_list = torrent->peer_list;
    switch( vector_remove_peer( &peer_list->peers, peer ) ) {
      case 2:  peer_list->seed_count--; /* Fall throughs intended */
      case 1:  peer_list->peer_count--; /* Fall throughs intended */
      default: break;
    }
  }

  if( proto == FLAG_TCP ) {
    int erval = OT_CLIENT_REQUEST_INTERVAL_RANDOM;
    reply_size = sprintf( reply, "d8:completei%zde10:incompletei%zde8:intervali%ie12:min intervali%ie" PEERS_BENCODED "0:e", peer_list->seed_count, peer_list->peer_count - peer_list->seed_count, erval, erval / 2 );
  }

  /* Handle UDP reply */
  if( proto == FLAG_UDP ) {
    ((uint32_t*)reply)[2] = htonl( OT_CLIENT_REQUEST_INTERVAL_RANDOM );
    ((uint32_t*)reply)[3] = htonl( peer_list->peer_count - peer_list->seed_count );
    ((uint32_t*)reply)[4] = htonl( peer_list->seed_count);
    reply_size = 20;
  }

  mutex_bucket_unlock_by_hash( hash, 0 );
  return reply_size;
}

void exerr( char * message ) {
  fprintf( stderr, "%s\n", message );
  exit( 111 );
}

void trackerlogic_init( ) {
  srandom( time(NULL) );
  g_tracker_id = random();

  /* Initialise background worker threads */
  mutex_init( );
  clean_init( );
  fullscrape_init( );
  accesslist_init( );
  livesync_init( );
  stats_init( );
}

void trackerlogic_deinit( void ) {
  int bucket, delta_torrentcount = 0;
  size_t j;

  /* Free all torrents... */
  for(bucket=0; bucket<OT_BUCKET_COUNT; ++bucket ) {
    ot_vector *torrents_list = mutex_bucket_lock( bucket );
    if( torrents_list->size ) {
      for( j=0; j<torrents_list->size; ++j ) {
        ot_torrent *torrent = ((ot_torrent*)(torrents_list->data)) + j;
        free_peerlist( torrent->peer_list );
        delta_torrentcount -= 1;
      }
      free( torrents_list->data );
    }
    mutex_bucket_unlock( bucket, delta_torrentcount );
  }

  /* Deinitialise background worker threads */
  stats_deinit( );
  livesync_deinit( );
  accesslist_deinit( );
  fullscrape_deinit( );
  clean_deinit( );
  /* Release mutexes */
  mutex_deinit( );
}

const char *g_version_trackerlogic_c = "$Source$: $Revision$\n";
