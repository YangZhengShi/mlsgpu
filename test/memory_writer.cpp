/*
 * mlsgpu: surface reconstruction from point clouds
 * Copyright (C) 2013  University of Cape Town
 *
 * This file is part of mlsgpu.
 *
 * mlsgpu is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * @file
 *
 * Writer class that stores results in memory for easy testing.
 */

#if HAVE_CONFIG_H
# include <config.h>
#endif
#include <string>
#include <algorithm>
#include <locale>
#include <sstream>
#include <boost/lexical_cast.hpp>
#include "../src/binary_io.h"
#include "memory_reader.h"
#include "memory_writer.h"

MemoryWriter::MemoryWriter(
    std::tr1::unordered_map<std::string, std::string> &outputs)
: curOutput(NULL), outputs(outputs)
{
}

void MemoryWriter::openImpl(const boost::filesystem::path &filename)
{
    curOutput = &outputs[filename.string()];
    // Clear any previous data that might have been written
    curOutput->clear();
}

void MemoryWriter::closeImpl()
{
    curOutput = NULL;
}

std::size_t MemoryWriter::writeImpl(const void *buffer, std::size_t count, offset_type offset) const
{
    if (curOutput->size() < count + offset)
        curOutput->resize(count + offset);

    const char *p = (const char *) buffer;
    std::copy(p, p + count, curOutput->begin() + offset);
    return count;
}

void MemoryWriter::resizeImpl(offset_type size) const
{
    curOutput->resize(size);
}


const std::string &MemoryWriterPlyBase::getOutput(const std::string &filename) const
{
    std::tr1::unordered_map<std::string, std::string>::const_iterator pos = outputs.find(filename);
    if (pos == outputs.end())
        throw std::invalid_argument("No such output file `" + filename + "'");
    return pos->second;
}

void MemoryWriterPlyBase::parse(
    const std::string &content,
    std::vector<boost::array<float, 3> > &vertices,
    std::vector<boost::array<std::tr1::uint32_t, 3> > &triangles)
{
    std::istringstream in(content);
    in.imbue(std::locale::classic());
    std::string line;
    const std::string vertexPrefix = "element vertex ";
    const std::string trianglePrefix = "element face ";

    std::size_t numVertices = 0, numTriangles = 0;
    std::size_t headerSize = 0;
    while (getline(in, line))
    {
        if (line.substr(0, vertexPrefix.size()) == vertexPrefix)
            numVertices = boost::lexical_cast<std::size_t>(line.substr(vertexPrefix.size()));
        else if (line.substr(0, trianglePrefix.size()) == trianglePrefix)
            numTriangles = boost::lexical_cast<std::size_t>(line.substr(trianglePrefix.size()));
        else if (line == "end_header")
        {
            headerSize = in.tellg();
            break;
        }
    }

    MemoryReader handle(content.data(), content.size());
    handle.open("memory"); // filename is irrelevant
    vertices.resize(numVertices);
    handle.read(&vertices[0][0], numVertices * sizeof(vertices[0]), headerSize);

    triangles.resize(numTriangles);
    BinaryReader::offset_type pos = headerSize + numVertices * sizeof(vertices[0]);
    for (std::size_t i = 0; i < numTriangles; i++)
    {
        handle.read(&triangles[i][0], sizeof(triangles[i]), pos + 1);
        pos += 1 + sizeof(triangles[i]);
    }
}

namespace
{

class MemoryWriterFactory
{
private:
    std::tr1::unordered_map<std::string, std::string> &outputs;
public:
    typedef boost::shared_ptr<BinaryWriter> result_type;

    explicit MemoryWriterFactory(std::tr1::unordered_map<std::string, std::string> &outputs)
        : outputs(outputs) {}

    result_type operator()()
    {
        return boost::make_shared<MemoryWriter>(boost::ref(outputs));
    }
};

} // anonymous namespace

MemoryWriterPly::MemoryWriterPly()
: FastPly::Writer(MemoryWriterFactory(outputs))
{
}
