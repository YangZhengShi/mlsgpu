/**
 * @file
 *
 * Implementation of marching tetrahedra.
 */

/// Number of edges in a cell
#define NUM_EDGES 19

__constant sampler_t nearest = CLK_NORMALIZED_COORDS_FALSE | CLK_ADDRESS_CLAMP_TO_EDGE | CLK_FILTER_NEAREST;

/**
 * Computes a cell code from 8 isovalues. Non-negative (outside) values are
 * given 1 bits, negative (inside) and NaNs are given 0 bits.
 */
inline uint makeCode(const float iso[8])
{
    return (iso[0] >= 0.0f ? 0x01U : 0U)
        | (iso[1] >= 0.0f ? 0x02U : 0U)
        | (iso[2] >= 0.0f ? 0x04U : 0U)
        | (iso[3] >= 0.0f ? 0x08U : 0U)
        | (iso[4] >= 0.0f ? 0x10U : 0U)
        | (iso[5] >= 0.0f ? 0x20U : 0U)
        | (iso[6] >= 0.0f ? 0x40U : 0U)
        | (iso[7] >= 0.0f ? 0x80U : 0U);
}

inline bool isValid(const float iso[8])
{
    return isfinite(iso[0])
        && isfinite(iso[1])
        && isfinite(iso[2])
        && isfinite(iso[3])
        && isfinite(iso[4])
        && isfinite(iso[5])
        && isfinite(iso[6])
        && isfinite(iso[7]);
}

/**
 * Emits a boolean table indicating whether each cell may produce triangles.
 * It only needs to be conservative - nothing bad will happen if it emits
 * 1 for cells that end up producing nothing.
 *
 * Cell indices are linearized by treating them as y major.
 *
 * There is one work-item per cell in a slice, arranged in a 2D NDRange.
 *
 * @param[out] occupied      1 for occupied cells, 0 for unoccupied cells.
 * @param      isoA          Slice of samples for lower z.
 * @param      isoB          Slice of samples for higher z.
 */
__kernel void countOccupied(
    __global uint *occupied,
    __read_only image2d_t isoA,
    __read_only image2d_t isoB)
{
    uint2 gid = (uint2) (get_global_id(0), get_global_id(1));
    uint linearId = gid.y * get_global_size(0) + gid.x;

    float iso[8];
    iso[0] = read_imagef(isoA, nearest, convert_int2(gid + (uint2) (0, 0))).x;
    iso[1] = read_imagef(isoA, nearest, convert_int2(gid + (uint2) (1, 0))).x;
    iso[2] = read_imagef(isoA, nearest, convert_int2(gid + (uint2) (0, 1))).x;
    iso[3] = read_imagef(isoA, nearest, convert_int2(gid + (uint2) (1, 1))).x;
    iso[4] = read_imagef(isoB, nearest, convert_int2(gid + (uint2) (0, 0))).x;
    iso[5] = read_imagef(isoB, nearest, convert_int2(gid + (uint2) (1, 0))).x;
    iso[6] = read_imagef(isoB, nearest, convert_int2(gid + (uint2) (0, 1))).x;
    iso[7] = read_imagef(isoB, nearest, convert_int2(gid + (uint2) (1, 1))).x;

    uint code = makeCode(iso);
    bool valid = isValid(iso);
    occupied[linearId] = (valid && code != 0 && code != 255);
}

/**
 * Produce list of useful cells.
 *
 * There is one work-item per cell in a slice, arranged in a 2D NDRange.
 *
 * @param[out] cells         The coordinates of cells which may produce triangles.
 * @param      occupiedRemap Scan of output from @ref countOccupied.
 */
__kernel void compact(
    __global uint2 * restrict cells,
    __global const uint * restrict occupiedRemap)
{
    uint2 gid = (uint2) (get_global_id(0), get_global_id(1));
    uint linearId = gid.y * get_global_size(0) + gid.x;

    uint pos = occupiedRemap[linearId];
    uint next = occupiedRemap[linearId + 1];
    if (next != pos)
        cells[pos] = gid;
}

/**
 * Count the number of triangles and indices produced by each cell.
 * There is one work-item per compacted cell.
 *
 * @param[out] viCount         Number of triangles+indices per cells.
 * @param      cells           Cell list written by @ref compact.
 * @param      isoA            Slice of samples for lower z.
 * @param      isoB            Slice of samples for higher z.
 * @param      countTable      Lookup table of counts per cube code.
 */
__kernel void countElements(
    __global uint2 * restrict viCount,
    __global const uint2 * restrict cells,
    __read_only image2d_t isoA,
    __read_only image2d_t isoB,
    __global const uchar2 * restrict countTable)
{
    uint gid = get_global_id(0);
    uint2 cell = cells[gid];

    float iso[8];
    iso[0] = read_imagef(isoA, nearest, convert_int2(cell + (uint2) (0, 0))).x;
    iso[1] = read_imagef(isoA, nearest, convert_int2(cell + (uint2) (1, 0))).x;
    iso[2] = read_imagef(isoA, nearest, convert_int2(cell + (uint2) (0, 1))).x;
    iso[3] = read_imagef(isoA, nearest, convert_int2(cell + (uint2) (1, 1))).x;
    iso[4] = read_imagef(isoB, nearest, convert_int2(cell + (uint2) (0, 0))).x;
    iso[5] = read_imagef(isoB, nearest, convert_int2(cell + (uint2) (1, 0))).x;
    iso[6] = read_imagef(isoB, nearest, convert_int2(cell + (uint2) (0, 1))).x;
    iso[7] = read_imagef(isoB, nearest, convert_int2(cell + (uint2) (1, 1))).x;

    uint code = makeCode(iso);
    viCount[gid] = convert_uint2(countTable[code]);
}

/**
 * Generate coordinates of a new vertex by interpolation along an edge.
 * @param iso0       Function sample at one corner.
 * @param iso1       Function sample at a second corner.
 * @param cell       Local coordinates of the lowest corner of the cell.
 * @param offset0    Local coordinate offset from @a cell to corner @a iso0.
 * @param offset1    Local coordinate offset from @a cell to corner @a iso1.
 * @param scale,bias Transformation from local to world coordinates.
 */
inline float3 interp(float iso0, float iso1, float3 cell, float3 offset0, float3 offset1, float3 scale, float3 bias)
{
    float inv = 1.0f / (iso1 - iso0);
    float3 lcoord = cell + (iso1 * offset0 - iso0 * offset1) * inv;
    return fma(lcoord, scale, bias);
}

#define INTERP(a, b) \
    interp(iso[a], iso[b], cellf, (float3) (a & 1, (a >> 1) & 1, (a >> 2) & 1), (float3) (b & 1, (b >> 1) & 1, (b >> 2) & 1), scale, bias)

/**
 * Generate vertices and indices for a slice.
 * There is one work-item per compacted cell.
 *
 * @param[out] vertices        Vertices in world coordinates.
 * @param[out] indices         Indices into @a vertices.
 * @param      viStart         Position to start writing vertices/indices for each cell.
 * @param      cells           List of compacted cells written by @ref compact.
 * @param      isoA            Slice of samples for lower z.
 * @param      isoB            Slice of samples for higher z.
 * @param      startTable      Lookup table indicating where to find vertices/indices in @a dataTable.
 * @param      dataTable       Lookup table of vertex and index indices.
 * @param      z               Z coordinate of the current slice.
 * @param      scale,bias      Transformation from local to world coordinates.
 * @param      lvertices       Scratch space of @ref NUM_EDGES elements per work item.
 */
__kernel void generateElements(
    __global float4 *vertices,
    __global ulong *vertexKeys,
    __global uint *indices,
    __global const uint2 * restrict viStart,
    __global const uint2 * restrict cells,
    __read_only image2d_t isoA,
    __read_only image2d_t isoB,
    __global const ushort2 * restrict startTable,
    __global const uchar * restrict dataTable,
    __global const ulong * restrict keyTable,
    uint z,
    float3 scale,
    float3 bias,
    __local float3 *lvertices)
{
    const uint gid = get_global_id(0);
    const uint lid = get_local_id(0);
    uint3 cell;
    cell.xy = cells[gid];
    cell.z = z;
    const float3 cellf = convert_float3(cell);
    __local float3 *lverts = lvertices + NUM_EDGES * lid;

    float iso[8];
    iso[0] = read_imagef(isoA, nearest, convert_int2(cell.xy + (uint2) (0, 0))).x;
    iso[1] = read_imagef(isoA, nearest, convert_int2(cell.xy + (uint2) (1, 0))).x;
    iso[2] = read_imagef(isoA, nearest, convert_int2(cell.xy + (uint2) (0, 1))).x;
    iso[3] = read_imagef(isoA, nearest, convert_int2(cell.xy + (uint2) (1, 1))).x;
    iso[4] = read_imagef(isoB, nearest, convert_int2(cell.xy + (uint2) (0, 0))).x;
    iso[5] = read_imagef(isoB, nearest, convert_int2(cell.xy + (uint2) (1, 0))).x;
    iso[6] = read_imagef(isoB, nearest, convert_int2(cell.xy + (uint2) (0, 1))).x;
    iso[7] = read_imagef(isoB, nearest, convert_int2(cell.xy + (uint2) (1, 1))).x;

    lverts[0] = INTERP(0, 1);
    lverts[1] = INTERP(0, 2);
    lverts[2] = INTERP(0, 3);
    lverts[3] = INTERP(1, 3);
    lverts[4] = INTERP(2, 3);
    lverts[5] = INTERP(0, 4);
    lverts[6] = INTERP(0, 5);
    lverts[7] = INTERP(1, 5);
    lverts[8] = INTERP(4, 5);
    lverts[9] = INTERP(0, 6);
    lverts[10] = INTERP(2, 6);
    lverts[11] = INTERP(4, 6);
    lverts[12] = INTERP(0, 7);
    lverts[13] = INTERP(1, 7);
    lverts[14] = INTERP(2, 7);
    lverts[15] = INTERP(3, 7);
    lverts[16] = INTERP(4, 7);
    lverts[17] = INTERP(5, 7);
    lverts[18] = INTERP(6, 7);

    uint code = makeCode(iso);
    uint2 viNext = viStart[gid];
    uint vNext = viNext.s0;
    uint iNext = viNext.s1;

    ushort2 start = startTable[code];
    ushort2 end = startTable[code + 1];

    ulong cellKey = ((ulong) cell.z << 43) | ((ulong) cell.y << 22) | ((ulong) cell.x << 1);

    for (uint i = 0; i < end.x - start.x; i++)
    {
        float4 vertex;
        vertex.xyz = lverts[dataTable[start.x + i]];
        vertex.w = as_float(vNext + i);
        vertices[vNext + i] = vertex;
        vertexKeys[vNext + i] = cellKey + keyTable[start.x + i];
    }
    for (uint i = 0; i < end.y - start.y; i++)
    {
        indices[iNext + i] = vNext + dataTable[start.y + i];
    }
}

/**
 * Determines which vertex keys are unique. For a range of equal keys, the @em last
 * one is given an indicator of 1, while the others get an indicator of 0.
 *
 * @param[out] vertexUnique         1 for exactly one instance of each key, 0 elsewhere.
 * @param      keys                 Vertex keys.
 *
 * @pre @a keys must be sorted such that equal keys are adjacent.
 */
__kernel void countUniqueVertices(__global uint * restrict vertexUnique,
                                  __global const ulong * restrict keys)
{
    const uint gid = get_global_id(0);
    bool last = (gid == get_global_size(0) - 1 || keys[gid] != keys[gid + 1]);
    vertexUnique[gid] = last ? 1 : 0;
}

__kernel void compactVertices(
    __global float3 * restrict outVertices,
    __global uint * restrict indexRemap,
    __global const uint * restrict vertexUnique,
    __global const float4 * restrict inVertices)
{
    const uint gid = get_global_id(0);
    const uint u = vertexUnique[gid];
    float4 v = inVertices[gid];
    if (u != vertexUnique[gid + 1])
    {
        outVertices[u] = v.xyz;
    }
    uint originalIndex = as_uint(v.w);
    indexRemap[originalIndex] = u;
}

__kernel void reindex(
    __global uint *indices,
    __global const uint * restrict indexRemap)
{
    const uint gid = get_global_id(0);
    indices[gid] = indexRemap[indices[gid]];
}
