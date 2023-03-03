#ifndef HEADER_fd_src_tango_xdp_fd_xsk_aio_private_h
#define HEADER_fd_src_tango_xdp_fd_xsk_aio_private_h

#include "fd_xsk_aio.h"
#include "../aio/fd_aio.h"

struct __attribute__((aligned(FD_XSK_AIO_ALIGN))) fd_xsk_aio_private {
  /* Data Layout Config ***********************************************/

  ulong batch_cnt;      /* the max size of any batch */
  ulong tx_depth;       /* depth of the fd_xsk_t tx_depth/cr_depth */
  ulong meta_off;       /* offset of fd_xsk_frame_meta_t[ batch_cnt ] */
  ulong batch_off;      /* offset of fd_aio_buffer_t    [ batch_cnt ] */
  ulong tx_stack_off;   /* offset of ulong              [ tx_depth  ] */

  /* Join Config ******************************************************/

  fd_xsk_t * xdp;
  fd_aio_t   rx;  /* from outside to user */
  fd_aio_t   tx;  /* from user to outside */
  void *     frame_mem; /* Address of start of frame memory */

  /* {rx,tx}_off: Offset from frame_mem to {rx,tx} frames.
     Unit is xdp->frame_sz. TODO consider using byte offset */
  ulong rx_off;
  ulong tx_off;

  ulong * tx_stack;     /* stack of unused tx frame indices
                           TODO consider using uint array */
  ulong   tx_stack_sz;  /* stack of unused tx frames */
  ulong   tx_top;       /* number of items on stack */

  /* Variable-length data *********************************************/

  /* ... fd_xsk_frame_meta_t[ batch_cnt ] follows ... */
  /* ... fd_aio_buffer_t    [ batch_cnt ] follows ... */
  /* ... ulong              [ tx_depth  ] follows ... */
};

FD_FN_PURE static inline fd_xsk_frame_meta_t *
fd_xsk_aio_meta( fd_xsk_aio_t * xdp_aio ) {
  return (fd_xsk_frame_meta_t *)( (ulong)xdp_aio + xdp_aio->meta_off );
}

FD_FN_PURE static inline fd_aio_buffer_t *
fd_xsk_aio_batch( fd_xsk_aio_t * xdp_aio ) {
  return (fd_aio_buffer_t *)( (ulong)xdp_aio + xdp_aio->batch_off );
}

#endif /* HEADER_fd_src_tango_xdp_fd_xsk_aio_private_h */
