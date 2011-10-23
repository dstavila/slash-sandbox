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

#include <nih/basic/functors.h>
#include <nih/basic/algorithms.h>
#include <nih/basic/cuda/scan.h>

namespace nih {
namespace cuda {
namespace bintree {

typedef Bintree_gen_context::Split_task Split_task;

// find the most significant bit smaller than start by which code0 and code1 differ
FORCE_INLINE NIH_HOST_DEVICE uint32 find_leading_bit_difference(
    const  int32  start_level,
    const uint32  code0,
    const uint32  code1)
{
    int32 level = start_level;

    while (level >= 0)
    {
        const uint32 mask = 1u << level;

        if ((code0 & mask) !=
            (code1 & mask))
            break;

        --level;
    }
    return level;
}

// do a single kd-split for all nodes in the input task queue, and generate
// a corresponding list of output tasks
template <typename Tree, uint32 BLOCK_SIZE>
__global__ void split_kernel(
    Tree                tree,
    const uint32        max_leaf_size,
    const bool          keep_singletons,
    const uint32        grid_size,
    const uint32*       codes,
    const uint32        in_tasks_count,
    const Split_task*   in_tasks,
    uint32*             out_tasks_count,
    Split_task*         out_tasks,
    const uint32        out_nodes_count,
    uint32*             out_leaf_count)
{
    const uint32 LOG_WARP_SIZE = 5;
    const uint32 WARP_SIZE = 1u << LOG_WARP_SIZE;

    volatile __shared__ uint32 warp_offset[ BLOCK_SIZE >> LOG_WARP_SIZE ];

    const uint32 warp_tid = threadIdx.x & (WARP_SIZE-1);
    const uint32 warp_id  = threadIdx.x >> LOG_WARP_SIZE;

    volatile __shared__ uint32 sm_red[ BLOCK_SIZE * 2 ];
    volatile uint32* warp_red = sm_red + WARP_SIZE * 2 * warp_id;

    // loop through all logical blocks associated to this physical one
    for (uint32 base_idx = blockIdx.x * BLOCK_SIZE;
        base_idx < in_tasks_count;
        base_idx += grid_size)
    {
        uint32 output_count = 0;
        uint32 split_index;

        const uint32 task_id = threadIdx.x + base_idx;

        uint32 node;
        uint32 begin;
        uint32 end;
        uint32 level;

        // check if the task id is in range, and if so try to find its split plane
        if (task_id < in_tasks_count)
        {
            const Split_task in_task = in_tasks[ task_id ];

            node  = in_task.m_node;
            begin = in_task.m_begin;
            end   = in_task.m_end;
            level = in_task.m_input;

            if (!keep_singletons)
            {
                level = find_leading_bit_difference(
                    level,
                    codes[begin],
                    codes[end-1] );
            }

            // check whether the input node really needs to be split
            if (end - begin > max_leaf_size)
            {
                // find the "partitioning pivot" using a binary search
                split_index = find_pivot(
                    codes + begin,
                    end - begin,
                    mask_and<uint32>( 1u << level ) ) - codes;

                output_count = (split_index == begin || split_index == end) ? 1u : 2u;
            }
        }

        uint32 offset = cuda::alloc( output_count, out_tasks_count, warp_tid, warp_red, warp_offset + warp_id );
        if (output_count >= 1)
            out_tasks[ offset + 0 ] = Split_task( out_nodes_count + offset + 0, begin, output_count == 1 ? end : split_index, level-1 );
        if (output_count == 2)
            out_tasks[ offset + 1 ] = Split_task( out_nodes_count + offset + 1, split_index, end, level-1 );

        const bool generate_leaf = (output_count == 0 && task_id < in_tasks_count);

        // count how many leaves we need to generate
        uint32 leaf_index = cuda::alloc<1>( generate_leaf, out_leaf_count, warp_tid, warp_offset + warp_id );

        // write the parent node
        if (task_id < in_tasks_count)
        {
            tree.write_node(
                node,
                output_count ? split_index != begin     : false,
                output_count ? split_index != end       : false,
                output_count ? out_nodes_count + offset : leaf_index );

            // make a leaf if necessary
            if (output_count == 0)
                tree.write_leaf( leaf_index, begin, end );
        }
    }
}
// generate a leaf for each task
template <typename Tree, uint32 BLOCK_SIZE>
__global__ void gen_leaves_kernel(
    Tree                tree,
    const uint32        grid_size,
    const uint32        in_tasks_count,
    const Split_task*   in_tasks,
    uint32*             out_leaf_count)
{
    const uint32 LOG_WARP_SIZE = 5;
    const uint32 WARP_SIZE = 1u << LOG_WARP_SIZE;

    __shared__ uint32 warp_offset[ BLOCK_SIZE >> LOG_WARP_SIZE ];

    const uint32 warp_tid = threadIdx.x & (WARP_SIZE-1);
    const uint32 warp_id  = threadIdx.x >> LOG_WARP_SIZE;

    // loop through all logical blocks associated to this physical one
    for (uint32 base_idx = blockIdx.x * BLOCK_SIZE;
        base_idx < in_tasks_count;
        base_idx += grid_size)
    {
        const uint32 task_id = threadIdx.x + base_idx;

        uint32 node;
        uint32 begin;
        uint32 end;

        // check if the task id is in range, and if so try to find its split plane
        if (task_id < in_tasks_count)
        {
            const Split_task in_task = in_tasks[ task_id ];

            node  = in_task.m_node;
            begin = in_task.m_begin;
            end   = in_task.m_end;
        }

        // alloc output slots
        uint32 leaf_index = cuda::alloc<1>( task_id < in_tasks_count, out_leaf_count, warp_tid, warp_offset + warp_id );

        // write the parent node
        if (task_id < in_tasks_count)
        {
            tree.write_node( node, false, false, leaf_index );
            tree.write_leaf( leaf_index, begin, end );
        }
    }
}

// do a single kd-split for all nodes in the input task queue, and generate
// a corresponding list of output tasks
template <typename Tree>
void split(
    Tree                tree,
    const uint32        max_leaf_size,
    const bool          keep_singletons,
    const uint32*       codes,
    const uint32        in_tasks_count,
    const Split_task*   in_tasks,
    uint32*             out_tasks_count,
    Split_task*         out_tasks,
    const uint32        out_nodes_count,
    uint32*             out_leaf_count)
{
    const uint32 BLOCK_SIZE = 128;
    const size_t max_blocks = thrust::detail::device::cuda::arch::max_active_blocks(split_kernel<Tree,BLOCK_SIZE>, BLOCK_SIZE, 0);
    const size_t n_blocks   = nih::min( max_blocks, (in_tasks_count + BLOCK_SIZE-1) / BLOCK_SIZE );
    const size_t grid_size  = n_blocks * BLOCK_SIZE;

    split_kernel<Tree, BLOCK_SIZE> <<<n_blocks,BLOCK_SIZE>>> (
        tree,
        max_leaf_size,
        keep_singletons,
        grid_size,
        codes,
        in_tasks_count,
        in_tasks,
        out_tasks_count,
        out_tasks,
        out_nodes_count,
        out_leaf_count );

    cudaThreadSynchronize();
}

// generate a leaf for each task
template <typename Tree>
void gen_leaves(
    Tree                tree,
    const uint32        in_tasks_count,
    const Split_task*   in_tasks,
    uint32*             out_leaf_count)
{
    const uint32 BLOCK_SIZE = 128;
    const size_t max_blocks = thrust::detail::device::cuda::arch::max_active_blocks(gen_leaves_kernel<Tree,BLOCK_SIZE>, BLOCK_SIZE, 0);
    const size_t n_blocks   = nih::min( max_blocks, (in_tasks_count + BLOCK_SIZE-1) / BLOCK_SIZE );
    const size_t grid_size  = n_blocks * BLOCK_SIZE;

    gen_leaves_kernel<Tree,BLOCK_SIZE> <<<n_blocks,BLOCK_SIZE>>> (
        tree,
        grid_size,
        in_tasks_count,
        in_tasks,
        out_leaf_count );

    cudaThreadSynchronize();
}

template <typename vector_type>
void resize_if_needed(vector_type& vec, const uint32 size)
{
    if (vec.size() < size)
        vec.resize( size );
}

} // namespace bintree

template <typename Tree>
void generate(
    Bintree_gen_context& context,
    const uint32  n_codes,
    const uint32* codes,
    const uint32  bits,
    const uint32  max_leaf_size,
    const bool    keep_singletons,
    Tree&         tree)
{
    tree.reserve_nodes( (n_codes / max_leaf_size) * 2 );
    tree.reserve_leaves( n_codes );

    // start building the octree
    resize_if_needed( context.m_task_queues[0], n_codes );
    resize_if_needed( context.m_task_queues[1], n_codes );

    Bintree_gen_context::Split_task* task_queues[2] = {
        thrust::raw_pointer_cast( &(context.m_task_queues[0]).front() ),
        thrust::raw_pointer_cast( &(context.m_task_queues[1]).front() )
    };

    uint32 in_queue  = 0;
    uint32 out_queue = 1;

    context.m_counters.resize( 4 );
    context.m_counters[ in_queue ]  = 1;
    context.m_counters[ out_queue ] = 0;
    context.m_counters[ 2 ]         = 0; // leaf counter

    context.m_task_queues[ in_queue ][0] = Bintree_gen_context::Split_task( 0, 0, n_codes, bits-1 );

    uint32 n_nodes = 1;

    // start splitting from the most significant bit
    int32 level = bits-1;

    // loop until there's tasks left in the input queue
    while (context.m_counters[ in_queue ] && level >= 0)
    {
        tree.reserve_nodes( n_nodes + context.m_counters[ in_queue ]*2 );

        // clear the output queue
        context.m_counters[ out_queue ] = 0;

        bintree::split(
            tree.get_cuda_context(),
            max_leaf_size,
            keep_singletons,
            codes,
            context.m_counters[ in_queue ],
            task_queues[ in_queue ],
            thrust::raw_pointer_cast( &context.m_counters.front() ) + out_queue,
            task_queues[ out_queue ],
            n_nodes,
            thrust::raw_pointer_cast( &context.m_counters.front() ) + 2 );

        uint32 out_count = context.m_counters[ out_queue ];

        // update the number of nodes
        n_nodes += context.m_counters[ out_queue ];

        // swap the input and output queues
        std::swap( in_queue, out_queue );

        // decrease the level
        --level;
    }

    // do level 0 separately, as we want to specialize it based on
    // the fact it doesn't need to generate any additional tasks.
    if (context.m_counters[ in_queue ])
    {
        bintree::gen_leaves(
            tree.get_cuda_context(),
            context.m_counters[ in_queue ],
            task_queues[ in_queue ],
            thrust::raw_pointer_cast( &context.m_counters.front() ) + 2 );
    }
    context.m_nodes  = n_nodes;
    context.m_leaves = context.m_counters[2];
}

} // namespace cuda
} // namespace nih