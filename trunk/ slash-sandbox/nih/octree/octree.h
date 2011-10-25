/*
 * Copyright (c) 2010-2011, NVIDIA Corporation
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of NVIDIA Corporation nor the
 *     names of its contributors may be used to endorse or promote products
 *     derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL <COPYRIGHT HOLDER> BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include <nih/basic/types.h>
#include <nih/basic/cuda_domains.h>
#include <nih/bits/popcount.h>
#include <nih/tree/model.h>

namespace nih {

///
/// Implements a basic octree node class, which stores up to 8 children
/// per node, and uses a bit-mask to determine which of the 8 slots is
/// actualy in use. The active children are stored consecutively relative
/// to a base offset.
///
struct Octree_node_base
{
    static const uint32 kInvalid = uint32(-1);

    /// empty constructor
    NIH_HOST_DEVICE Octree_node_base() {}

    /// leaf constructor
    NIH_HOST_DEVICE Octree_node_base(const uint32 leaf_index);

    /// full constructor
    NIH_HOST_DEVICE Octree_node_base(const uint32 mask, const uint32 index);

    /// is a leaf?
    NIH_HOST_DEVICE bool is_leaf() const;

    /// set the 8-bit mask of active children
    NIH_HOST_DEVICE void set_child_mask(const uint32 mask);

    /// get the 8-bit mask of active children
    NIH_HOST_DEVICE uint32 get_child_mask() const;

    /// check whether the i-th child exists
    NIH_HOST_DEVICE bool has_child(const uint32 i) const;

    /// set the offset to the first child
    NIH_HOST_DEVICE void set_child_offset(const uint32 child);

    /// get the offset to the first child
    NIH_HOST_DEVICE uint32 get_child_offset() const;

    /// get leaf index
    NIH_HOST_DEVICE uint32 get_leaf_index() const;

    /// return the number of children
    NIH_HOST_DEVICE uint32 get_child_count() const { return popc( get_child_mask() ); }

    /// get the index of the i-th child (among the active ones)
    NIH_HOST_DEVICE uint32 get_child(const uint32 i) const;

    /// get the index of the i-th octant. returns kInvalid for non-active children.
    NIH_HOST_DEVICE uint32 get_octant(const uint32 i) const;

    uint32 m_packed_info;
};

/// get the index of the i-th octant. returns kInvalid for non-active children.
uint32 get_octant(const Octree_node_base& node, const uint32 i, host_domain tag);

/// get the index of the i-th octant. returns kInvalid for non-active children.
NIH_DEVICE uint32 get_octant(const Octree_node_base& node, const uint32 i, device_domain tag);


/// A simple Breadth-First Tree model implementation for Octrees
template <typename Tree_type, typename Domain_type>
struct Octree {};

template <typename Domain_type>
struct Octree< breadth_first_tree, Domain_type >
{
    typedef Domain_type         domain_type;
    typedef Octree_node_base    node_type;
    typedef breadth_first_tree  tree_type;

    /// constructor
    Octree(
        const node_type* nodes,
        const uint32     n_leaves,
        const uint2*     leaves,
        const uint32     n_levels,
        const uint32*    levels) : 
        m_nodes( nodes ),
        m_leaf_count( n_leaves ),
        m_leaves( leaves ),
        m_level_count( n_levels ),
        m_levels( levels )
    {}

    /// return the number of levels
    NIH_HOST_DEVICE uint32 get_level_count() const { return m_level_count; }

    /// return the i-th level
    NIH_HOST_DEVICE uint32 get_level(const uint32 i) const { return m_levels[i]; }

    /// retrieve a node
    NIH_HOST_DEVICE node_type get_node(const uint32 index) const
    {
        return m_nodes[ index ];
    }

    /// return the number of leaves
    NIH_HOST_DEVICE uint32 get_leaf_count() const { return m_leaf_count; }

    /// retrieve a leaf
    NIH_HOST_DEVICE uint2 get_leaf(const uint32 index) const
    {
        return m_leaves[ index ];
    }

    const node_type*    m_nodes;
    const uint32        m_leaf_count;
    const uint2*        m_leaves;
    const uint32*       m_levels;
    uint32              m_level_count;
};

} // namespace nih

#include <nih/octree/octree_inline.h>