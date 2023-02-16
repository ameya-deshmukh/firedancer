#ifndef HEADER_fd_src_tango_aio_aio_h
#define HEADER_fd_src_tango_aio_aio_h

#include "../../util/fd_util_base.h"

/* fd_aio defines a simple abstraction for asynchronous I/O.

   Specifically, `fd_aio_t` acts as a sink for transmitting batches of
   buffers to an arbitrary receiver.  Decoupling is achieved via a call-
   back interface.

   ### Example: Setup

     fd_quic_t * quic = get_quic(); // get an initialized quic instance
     fd_xdp_t *  xdp  = get_xdp();  // get an initialized xdp instance

     fd_aio_t aio_xdp_to_quic;
     fd_aio_t aio_quic_to_xdp;

     fd_quic_set_aio( quic, aio_xdp_to_quic, aio_quic_to_xdp );
     fd_xdp_set_aio ( xdp, aio_quic_to_xdp,  aio_xdp_to_quic );

     // the two *set_aio calls have the following effect:
     //   aio_xdp_to_quic.cb_receive = fd_aio_cb_receive;
     //   aio_xdp_to_quic.context    = quic;

     //   aio_quic_to_xdp.cb_receive = fd_xdp_aio_cb_receive;
     //   aio_quic_to_xdp.context    = xdp;

     // now whenever fd_quic_process is called on quic, quic will
     // be able to send data to xdp via fd_aio_send(...)
     // and vice versa

   ### Example: Sending

     fd_aio_t aio;

     aio.cb_receive = my_cb_receive;
     aio.context    = quic;

     fd_aio_buffer_t batch[10] = {{ .data = data, .data_sz = data_sz }};

     fd_aio_buffer_t cur_batch    = batch;
     ulong           cur_batch_sz = 10;
     while( cur_batch_sz ) {
       int send_rc = fd_aio_send( aio, cur_batch, cur_batch_sz ); // send a batch of buffers to the peer
       if( send_rc < 0 ) {
         fprintf( stderr, "error occurred during send\n" );
         break;
       }
       cur_batch    += send_rc;
       cur_batch_sz -= send_rc;

       // possibly do some other process that might free up resources
       // to avoid deadlock
     } */


/* fd_aio_buffer_t is a contiguous range of I/O data in local memory
   passed from sender to receiver. The receiver may access up to
   `data_sz` bytes from the memory area beginning at `data`. */

#define FD_AIO_BUFFER_ALIGN (16UL)

struct __attribute__((aligned(FD_AIO_BUFFER_ALIGN))) fd_aio_buffer {
  void * data;
  ulong  data_sz;
};
typedef struct fd_aio_buffer fd_aio_buffer_t;

/* fd_aio_cb_receive_t is the callback provided by the receiver.

   The callback function should not block.  The `context` argument is
   taken from `fd_aio_t`.  The `batch` and `batch_cnt` arguments are
   provided by the sender in `fd_aio_send`.  See `fd_aio_send` for an
   explanation of these arguments and the return value.  The `batch`
   buffers must not be accessed after returning. */

typedef ulong (*fd_aio_cb_receive_t)( void *            context,
                                      fd_aio_buffer_t * batch,
                                      ulong             batch_cnt );

/* fd_aio_t is an asynchronous sink for I/O data. */

struct fd_aio {
  /* Implementation-defined context object.  Accessed by both sender
     and receiver. */
  void * context;

  /* Receiver callback called by sender. */
  fd_aio_cb_receive_t cb_receive;
};
typedef struct fd_aio fd_aio_t;

FD_PROTOTYPES_BEGIN

/* fd_aio_send: Sends a batch of buffers to the receiver.

   Arguments
     aio         Sink containing context and receiver callback
     batch       Array of `fd_aio_buffer_t` objects
     batch_cnt   Size of `batch` array

   Returns
     N == 0      No buffers were processed, you may try again
     N >  0      N buffers were processed
     N == ~0     A fatal error occurred

   The receiver must process the given buffers in batch in order.  If
   less than `batch_cnt` buffers were processed by the receiver, the
   sender should retry the `fd_aio_send` call with the remaining
   buffers.  The reciever is expected to not access the buffers after
   this function returns.

   Internally, calls the `fd_aio_cb_receive_t` callback registered in
   `aio` with the given `batch` and `batch_cnt`, and returns its return
   value.  U.B if `aio` was not initialized. */

/* TODO: This would ideally be extern inline but that causes issues
         with the build system. */
static inline ulong
fd_aio_send( fd_aio_t *        aio,
             fd_aio_buffer_t * batch,
             ulong             batch_sz ) {
  return aio->cb_receive( aio->context, batch, batch_sz );
}

FD_PROTOTYPES_END

#endif /* HEADER_fd_src_tango_aio_aio_h */
