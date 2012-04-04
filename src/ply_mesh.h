/**
 * @file
 *
 * Utilities for writing the output of @ref Marching to a PLY file.
 */

#ifndef PLY_MESH_H
#define PLY_MESH_H

#if HAVE_CONFIG_H
# include <config.h>
#endif
#include <CL/cl.hpp>
#include <string>
#include <boost/array.hpp>
#include "ply.h"

struct Splat;

namespace PLY
{

/**
 * Utility class to write an input splat back to a PLY file.
 * @see @ref PLY::Fetcher.
 */
class SplatFetcher
{
public:
    typedef Splat Element;

    std::string getName() const { return "vertex"; }

    PropertyTypeSet getProperties() const;
    void writeElement(const Element &e, Writer &writer) const;
};

/**
 * Utility class to write vertices generated by @ref Marching to a PLY file.
 * @see @ref PLY::Fetcher.
 */
class VertexFetcher
{
public:
    typedef cl_float3 Element;

    std::string getName() const { return "vertex"; }

    PropertyTypeSet getProperties() const;
    void writeElement(const Element &e, Writer &writer) const;
};

/**
 * Utility class to write indices generated by @ref Marching to a PLY file.
 * @see @ref PLY::Fetcher.
 */
class TriangleFetcher
{
public:
    typedef boost::array<cl_uint, 3> Element;

    std::string getName() const { return "face"; }

    PropertyTypeSet getProperties() const;
    void writeElement(const Element &e, Writer &writer) const;
};

} // namespace PLY

#endif /* !PLY_MESH_H */
