//
// Ethernet Header
// eth {
//   dst_addr (48),
//   src_addr (48),
//   eth_type (16),
// }

FD_TEMPL_DEF_STRUCT_BEGIN(eth)
  FD_TEMPL_MBR_ELEM_FIXED( dst_addr, uchar, 6 )
  FD_TEMPL_MBR_ELEM_FIXED( src_addr, uchar, 6 )
  FD_TEMPL_MBR_ELEM      ( eth_type, ushort   )
FD_TEMPL_DEF_STRUCT_END(eth)


// VLAN Header
// vlan {
//   pcp_dei (4),
//   vlan_id (12),
//   eth_type (16),
// }

FD_TEMPL_DEF_STRUCT_BEGIN(vlan)
  FD_TEMPL_MBR_ELEM_BITS( pcp_dei,  uchar,   4 )
  FD_TEMPL_MBR_ELEM_BITS( vlan_id,  ushort, 12 )
  FD_TEMPL_MBR_ELEM     ( eth_type, ushort     )
FD_TEMPL_DEF_STRUCT_END(vlan)

