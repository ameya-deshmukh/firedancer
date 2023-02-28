#if !defined(__linux__) || !FD_HAS_LIBBPF
#error "fd_xsk_aio requires Linux operating system with XDP support"
#endif

#include "../../util/fd_util.h"
#include "fd_xsk_private.h"
#include "fd_xsk_aio_private.h"

/* fd_xsk_aio_forward_cb implements fd_aio_cb_receive_t. */
static ulong
fd_xsk_aio_forward_cb( void *            context,
                       fd_aio_buffer_t * batch,
                       ulong             batch_cnt );

ulong
fd_xsk_aio_align( void ) {
  return FD_XSK_AIO_ALIGN;
}

ulong
fd_xsk_aio_footprint( ulong tx_depth,
                      ulong batch_cnt ) {
  ulong sz =       1UL*sizeof( fd_xsk_aio_t        )
           + batch_cnt*sizeof( fd_xsk_frame_meta_t )
           + batch_cnt*sizeof( fd_aio_buffer_t     )
           + tx_depth *sizeof( ulong               );

  sz = fd_ulong_align_up( sz, FD_XSK_AIO_ALIGN );
  /* assert( sz%FD_XSK_AIO_ALIGN==0UL ) */
  return sz;
}

void *
fd_xsk_aio_new( void * mem,
                ulong  tx_depth,
                ulong  batch_cnt ) {

  if( FD_UNLIKELY( !mem ) ) {
    FD_LOG_WARNING(( "NULL mem" ));
    return NULL;
  }

  if( FD_UNLIKELY( !fd_ulong_is_aligned( (ulong)mem, fd_xsk_aio_align() ) ) ) {
    FD_LOG_WARNING(( "misaligned mem" ));
    return NULL;
  }

  if( FD_UNLIKELY( tx_depth==0UL ) ) {
    FD_LOG_WARNING(( "zero tx_depth" ));
    return NULL;
  }

  if( FD_UNLIKELY( batch_cnt==0UL ) ) {
    FD_LOG_WARNING(( "zero batch_cnt" ));
    return NULL;
  }

  ulong footprint = fd_xsk_aio_footprint( tx_depth, batch_cnt );
  if( FD_UNLIKELY( !footprint ) ) {
    FD_LOG_WARNING(( "invalid footprint for tx_depth (%lu), batch_cnt (%lu)",
                      tx_depth, batch_cnt ));
    return NULL;
  }

  fd_memset( mem, 0, footprint );

  /* Allocate objects in fd_xsk_aio_t */

  fd_xsk_aio_t * xdp_aio = (fd_xsk_aio_t *)mem;

  /* Assumes alignment of `fd_xsk_aio_t` matches alignment of
     `fd_xsk_frame_meta_t` and `fd_aio_buffer_t`. */

  ulong meta_off     =                       sizeof(fd_xsk_aio_t       );
  ulong batch_off    = meta_off  + batch_cnt*sizeof(fd_xsk_frame_meta_t);
  ulong tx_stack_off = batch_off + batch_cnt*sizeof(fd_aio_buffer_t    );

  xdp_aio->batch_cnt    = batch_cnt;
  xdp_aio->tx_depth     = tx_depth;
  xdp_aio->meta_off     = meta_off;
  xdp_aio->batch_off    = batch_off;
  xdp_aio->tx_stack_off = tx_stack_off;

  return xdp_aio;
}


fd_xsk_aio_t *
fd_xsk_aio_join( void *     shxdp_aio,
                 fd_xsk_t * xdp ) {

  if( FD_UNLIKELY( !shxdp_aio ) ) {
    FD_LOG_WARNING(( "NULL shxdp_aio" ));
    return NULL;
  }

  if( FD_UNLIKELY( !fd_ulong_is_aligned( (ulong)shxdp_aio, fd_xsk_aio_align() ) ) ) {
    FD_LOG_WARNING(( "misaligned shxdp_aio" ));
    return NULL;
  }

  /* Validate memory layout */

  fd_xsk_aio_t * xdp_aio = (fd_xsk_aio_t *)shxdp_aio;

  if( FD_UNLIKELY( xdp_aio->xdp
                || xdp_aio->rx.context
                || xdp_aio->tx.context ) ) {
    FD_LOG_WARNING(( "xdp_aio in an unclean state, resetting" ));
    xdp_aio->xdp = NULL;
    /* continue */
  }

  if( FD_UNLIKELY( xdp->tx_depth  != xdp_aio->tx_depth ) ) {
    FD_LOG_WARNING(( "incompatible xdp (tx_depth=%lu) and xdp_aio (tx_depth=%lu)",
                     xdp->tx_depth, xdp_aio->tx_depth ));
    return NULL;
  }

  /* Reset state */

  xdp_aio->xdp           = xdp;
  xdp_aio->rx.context    = NULL;
  xdp_aio->rx.cb_receive = NULL;
  xdp_aio->tx.context    = NULL;
  xdp_aio->tx.cb_receive = NULL;
  //xdp_aio->rx_off      = xdp->; TODO
  //xdp_aio->tx_off      = 0;
  xdp_aio->tx_stack_sz = xdp_aio->tx_depth;
  xdp_aio->tx_top      = 0;

  /* Set up TX callback (local address) */

  xdp_aio->tx.cb_receive = fd_xsk_aio_forward_cb;
  xdp_aio->tx.context    = (void *)xdp_aio;

  /* Enqueue frames to RX ring for receive (via fill ring) */

  ulong frame_off = xdp_aio->rx_off;
  ulong frame_sz  = xdp->frame_sz;
  for( ulong j=0; j<xdp->rx_depth; j++ ) {
    ulong enq_cnt =  fd_xsk_rx_enqueue( xdp, &frame_off, 1U );
    frame_off     += frame_sz;

    if( FD_UNLIKELY( !enq_cnt ) ) {
      FD_LOG_WARNING(( "fd_xsk_rx_enqueue() failed, was fd_xsk_t properly flushed?" ));
      return NULL;
    }
  }

  /* Add all TX frames to the free stack */

  frame_off = xdp_aio->tx_off*frame_sz;
  for( ulong j=0; j<xdp->tx_depth; j++ ) {
    xdp_aio->tx_stack[xdp_aio->tx_top] =  frame_off;
                      xdp_aio->tx_top++;
    frame_off                          += frame_sz;
  }

  return (fd_xsk_aio_t *)xdp_aio;
}


void *
fd_xsk_aio_leave( fd_xsk_aio_t * xdp_aio ) {

  if( FD_UNLIKELY( !xdp_aio ) ) {
    FD_LOG_WARNING(( "NULL xdp_aio" ));
    return NULL;
  }

  xdp_aio->xdp           = NULL;
  xdp_aio->rx.context    = NULL;
  xdp_aio->rx.cb_receive = NULL;
  xdp_aio->tx.context    = NULL;
  xdp_aio->tx.cb_receive = NULL;

  return (void *)xdp_aio;
}

void *
fd_xsk_aio_delete( void * xdp_aio ) {

  if( FD_UNLIKELY( !xdp_aio ) ) {
    FD_LOG_WARNING(( "NULL xdp_aio" ));
    return NULL;
  }

  if( FD_UNLIKELY( !fd_ulong_is_aligned( (ulong)xdp_aio, fd_xsk_aio_align() ) ) ) {
    FD_LOG_WARNING(( "misaligned xdp_aio" ));
    return NULL;
  }

  return (void *)xdp_aio;
}


fd_aio_t *
fd_xsk_aio_get_tx( fd_xsk_aio_t * xdp_aio ) {
  return &xdp_aio->tx;
}

void
fd_xsk_aio_set_rx( fd_xsk_aio_t * xdp_aio,
                   fd_aio_t *     aio ) {
  fd_memcpy( &xdp_aio->rx, aio, sizeof(fd_aio_t) );
}


void
fd_xsk_aio_housekeep( fd_xsk_aio_t * xdp_aio ) {
  fd_xsk_t *            xdp         = xdp_aio->xdp;
  fd_aio_t *            ingress     = &xdp_aio->rx;
  fd_xsk_frame_meta_t * meta        = fd_xsk_aio_meta ( xdp_aio );
  fd_aio_buffer_t *     aio_batch   = fd_xsk_aio_batch( xdp_aio );
  ulong                 batch_sz    = xdp_aio->batch_cnt;
  ulong                 frame_laddr = (ulong)fd_xsk_umem_laddr( xdp_aio->xdp );

  /* try completing receives */
  ulong rx_avail = fd_xsk_rx_complete( xdp, meta, batch_sz );

  /* forward to aio */
  if( rx_avail ) {
    for( ulong j=0; j<rx_avail; j++ ) {
      aio_batch[j] = (fd_aio_buffer_t) {
        .data    = (void *)(frame_laddr + meta[j].off),
        .data_sz = meta[j].sz
      };
    }

    fd_aio_send( ingress, aio_batch, rx_avail );
    /* TODO frames may not all be processed at this point
       we should count them, and possibly buffer them */

    /* return frames to rx ring */
    ulong enq_rc = fd_xsk_rx_enqueue2( xdp, meta, rx_avail );
    if( FD_UNLIKELY( enq_rc < rx_avail ) ) {
      /* this should not be possible */
      FD_LOG_WARNING(( "frames lost trying to replenish rx ring" ));
    }
  }

  /* any tx to complete? */
  ulong tx_completed = fd_xsk_tx_complete( xdp,
                                           xdp_aio->tx_stack    + xdp_aio->tx_top,
                                           xdp_aio->tx_stack_sz - xdp_aio->tx_top );
  xdp_aio->tx_top += tx_completed;
}


void
fd_xsk_aio_tx_complete( fd_xsk_aio_t * xdp_aio ) {
  fd_xsk_t * xdp = xdp_aio->xdp;

  ulong tx_completed = fd_xsk_tx_complete( xdp,
                                           xdp_aio->tx_stack    + xdp_aio->tx_top,
                                           xdp_aio->tx_stack_sz - xdp_aio->tx_top );
  xdp_aio->tx_top += tx_completed;
}


ulong
fd_xsk_aio_forward_cb( void *            context,
                       fd_aio_buffer_t * batch,
                       ulong             batch_sz ) {
  fd_xsk_aio_t * xdp_aio = (fd_xsk_aio_t*)context;
  fd_xsk_t *     xdp     = xdp_aio->xdp;

  fd_xsk_aio_tx_complete( xdp_aio );

  ulong                 cap        = xdp_aio->batch_cnt; /* capacity of xdp_aio batch */
  uchar *               frame_mem  = xdp_aio->frame_mem; /* frame memory */
  ulong                 frame_size = xdp->frame_sz;
  fd_xsk_frame_meta_t * meta       = fd_xsk_aio_meta( xdp_aio );  /* frame metadata */

  ulong k=0;
  for( ulong j=0; j<batch_sz; ++j ) {
    /* find a buffer */
    if( FD_UNLIKELY( !xdp_aio->tx_top ) ) {
      /* none available */
      return j;
    }

    --xdp_aio->tx_top;
    ulong offset = xdp_aio->tx_stack[xdp_aio->tx_top];

    uchar const * data    = batch[j].data;
    ulong         data_sz = batch[j].data_sz;

    /* copy frame into tx memory */
    if( FD_UNLIKELY( batch[j].data_sz > frame_size ) ) {
      FD_LOG_ERR(( "%s : frame too large for xdp ring, dropping", __func__ ));
      /* fail */
    } else {
      fd_memcpy( frame_mem + offset, data, data_sz );
      if( k == cap ) {
        ulong tx_tot = k;
        ulong sent   = 0;
        while(1) {
          sent += fd_xsk_tx_enqueue( xdp, meta + sent, k - sent );
          if( sent == tx_tot ) break;

          /* we didn't send all
             complete, then try again */

          fd_xsk_aio_tx_complete( xdp_aio );
        }

        k = 0;
      }

      meta[k] = (fd_xsk_frame_meta_t){ offset, (unsigned)data_sz, 0 };
      k++;
    }
  }

  /* any left to send? */
  if( k ) {
    fd_xsk_tx_enqueue( xdp, meta, k );
  }

  return batch_sz;
}
