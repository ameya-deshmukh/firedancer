use std::{
    ffi::CString,
    hint::spin_loop,
    mem::transmute,
    ops::Not,
    ptr,
};
use anyhow::{
    anyhow,
    Result,
};
use chrono::Utc;
use firedancer_sys::{
    tango::{
        fd_chunk_to_laddr_const,
        fd_dcache_join,
        fd_frag_meta_seq_query,
        fd_mcache_depth,
        fd_mcache_join,
        fd_mcache_line_idx,
        fd_mcache_seq_laddr_const,
        fd_mcache_seq_query,
        fd_tempo_lazy_default,
    },
    util::{
        fd_pod_query_subpod,
        fd_wksp_containing,
        fd_wksp_pod_attach,
        fd_wksp_pod_map,
    },
};
use rand::prelude::*;

/// Tango exposes a simple API for consuming from a Tango mcache/dcache queue.
struct Tango {
    /// Configuration
    pod_gaddr: String,
    cfg_path: String,
    mcache_out_path: String,
    dcache_out_path: String,

    /// Output crossbeam channel to send what we have read from the Tango consumer on
    output: crossbeam_channel::Sender<Vec<u8>>,
}

impl Tango {
    pub unsafe fn run(&self) -> Result<()> {
        // Load configuration
        let pod = fd_wksp_pod_attach(CString::new(self.pod_gaddr.clone())?.as_ptr());
        let cfg_pod = fd_pod_query_subpod(pod, CString::new(self.cfg_path.clone())?.as_ptr());
        cfg_pod
            .is_null()
            .not()
            .then(|| ())
            .ok_or(anyhow!("pod config path not found"))?;

        // Join the mcache
        let mcache = fd_mcache_join(fd_wksp_pod_map(
            cfg_pod,
            CString::new(self.mcache_out_path.clone())?.as_ptr(),
        ));
        mcache
            .is_null()
            .not()
            .then(|| ())
            .ok_or(anyhow!("fd_mcache_join failed"))?;

        // Join the dcache
        let dcache = fd_dcache_join(fd_wksp_pod_map(
            cfg_pod,
            CString::new(self.dcache_out_path.clone())?.as_ptr(),
        ));
        dcache
            .is_null()
            .not()
            .then(|| ())
            .ok_or(anyhow!("fd_dcache_join failed"))?;

        // Look up the mline address
        let depth = fd_mcache_depth(mcache);
        let sync = fd_mcache_seq_laddr_const(mcache);
        let seq = fd_mcache_seq_query(sync);
        let mcache_line_idx = fd_mcache_line_idx(seq, depth);
        let mline = mcache.add(mcache_line_idx.try_into().unwrap());
        let workspace = fd_wksp_containing(transmute(mline));
        workspace
            .is_null()
            .not()
            .then(|| ())
            .ok_or(anyhow!("fd_wksp_containing failed"))?;

        // Set frequency of houskeeping operations
        let mut next_housekeeping = Utc::now().timestamp_nanos();
        let housekeeping_interval_ns = fd_tempo_lazy_default(depth);
        let mut rng = rand::thread_rng();

        // Continually consume data from the queue
        loop {
            // Do housekeeping at intervals
            let now = Utc::now().timestamp_nanos();
            if now >= next_housekeeping {
                // TODO: housekeeping; track/forward diagnostic counters
                next_housekeeping =
                    now + rng.gen_range(housekeeping_interval_ns..=2 * housekeeping_interval_ns)
            }

            // Overrun check
            let seq_found = fd_frag_meta_seq_query(mline);
            if seq_found != seq {
                // Consumer has either caught up or overrun
                if seq_found < seq {
                    spin_loop();
                    continue;
                }

                // TODO: support for proper flow control
            }

            // Speculatively copy data out of dcache
            let chunk = fd_chunk_to_laddr_const(
                transmute(workspace),
                (*mline).__bindgen_anon_1.as_ref().chunk.into(),
            ) as *const u8;
            let size = (*mline).__bindgen_anon_1.as_ref().sz;
            let mut bytes = Vec::with_capacity(size.into());
            ptr::copy_nonoverlapping(chunk, bytes.as_mut_ptr(), size.into());
            bytes.set_len(size.into());

            // Check the producer hasn't overran us while we were copying the data
            let seq_found = fd_frag_meta_seq_query(mline);
            if seq_found != seq {
                continue;
            }

            // Send the data on the channel
            self.output.send(bytes)?;
        }
    }
}
