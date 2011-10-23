namespace nih {

/// leaf constructor
FORCE_INLINE Octree_node_base::Octree_node_base(const uint32 leaf_index) :
    m_packed_info( leaf_index << 8u )
{
}

/// full constructor
FORCE_INLINE Octree_node_base::Octree_node_base(const uint32 mask, const uint32 index) :
    m_packed_info( (index << 8u) & mask )
{
}

// is a leaf?
FORCE_INLINE bool Octree_node_base::is_leaf()
{
    return get_child_mask() ? false : true;
}

// set the 8-bit mask of active children
FORCE_INLINE void Octree_node_base::set_child_mask(const uint32 mask)
{
    m_packed_info = (m_packed_info & 0xFFFFFF00u) | mask;
}

// get the 8-bit mask of active children
FORCE_INLINE uint32 Octree_node_base::get_child_mask() const
{
    return m_packed_info & 0x000000FFu;
}

// set the offset to the first child
FORCE_INLINE void Octree_node_base::set_child_offset(const uint32 child)
{
    m_packed_info = (m_packed_info & 0x000000FFu) | (child << 8u);
}

// get the offset to the first child
FORCE_INLINE uint32 Octree_node_base::get_child_offset() const
{
    return m_packed_info >> 8u;
}

// check whether the i-th child exists
FORCE_INLINE bool Octree_node_base::is_active(const uint32 i) const
{
    return m_packed_info & (1u << i) ? true : false;
}

// get the index of the i-th child (among the active ones)
FORCE_INLINE uint32 Octree_node_base::get_child(const uint32 i) const
{
    return (m_packed_info >> 8u) + i;
}

// get the index of the i-th octant. returns kInvalid for non-active children.
FORCE_INLINE uint32 Octree_node<host_domain>::get_octant(const uint32 i) const
{
    const uint32 mask = get_child_mask();
    return mask & (1u << i) ? get_child_offset() + popc(uint8(mask << (8u - i))) : kInvalid;
}

// get the index of the i-th octant. returns kInvalid for non-active children.
FORCE_INLINE uint32 Octree_node<device_domain>::get_octant(const uint32 i) const
{
#ifdef __CUDACC__
    const uint32 mask = get_child_mask();
    return mask & (1u << i) ? get_child_offset() + __popc(mask << (8u - i)) : kInvalid;
#endif
}

} // namespace nih

