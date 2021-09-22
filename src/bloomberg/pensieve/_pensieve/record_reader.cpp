#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <unordered_map>

#include "Python.h"

#include "exceptions.h"
#include "hooks.h"
#include "interval_tree.h"
#include "logging.h"
#include "record_reader.h"
#include "records.h"
#include "source.h"

namespace pensieve::api {

using namespace tracking_api;
using namespace io;
using namespace exception;

void
RecordReader::readHeader(HeaderRecord& header)
{
    d_input->read(header.magic, sizeof(MAGIC));
    if (memcmp(header.magic, MAGIC, sizeof(MAGIC)) != 0) {
        throw std::ios_base::failure(
                "The provided input file does not look like a binary generated by pensieve.");
    }
    d_input->read(reinterpret_cast<char*>(&header.version), sizeof(header.version));
    if (header.version != CURRENT_HEADER_VERSION) {
        throw std::ios_base::failure(
                "The provided input file is incompatible with this version of pensieve.");
    }
    d_input->read(reinterpret_cast<char*>(&header.native_traces), sizeof(header.native_traces));
    d_input->read(reinterpret_cast<char*>(&header.stats), sizeof(header.stats));
    header.command_line.reserve(4096);
    d_input->getline(header.command_line, '\0');
}

RecordReader::RecordReader(std::unique_ptr<Source> source)
: d_input(std::move(source))
{
    readHeader(d_header);
}

void
RecordReader::close() noexcept
{
    d_input->close();
}

bool
RecordReader::isOpen() const noexcept
{
    return d_input->is_open();
}

void
RecordReader::parseFrame()
{
    FrameSeqEntry frame_seq_entry{};
    d_input->read(reinterpret_cast<char*>(&frame_seq_entry), sizeof(FrameSeqEntry));
    thread_id_t tid = frame_seq_entry.tid;

    switch (frame_seq_entry.action) {
        case PUSH:
            d_stack_traces[tid].push_back(frame_seq_entry.frame_id);
            break;
        case POP:
            assert(!d_stack_traces[tid].empty());
            d_stack_traces[tid].pop_back();
            break;
    }
}

void
RecordReader::parseFrameIndex()
{
    tracking_api::pyframe_map_val_t pyframe_val;
    d_input->read(reinterpret_cast<char*>(&pyframe_val.first), sizeof(pyframe_val.first));
    d_input->getline(pyframe_val.second.function_name, '\0');
    d_input->getline(pyframe_val.second.filename, '\0');
    d_input->read(
            reinterpret_cast<char*>(&pyframe_val.second.parent_lineno),
            sizeof(pyframe_val.second.parent_lineno));
    auto iterator = d_frame_map.insert(pyframe_val);
    if (!iterator.second) {
        throw std::runtime_error("Two entries with the same ID found!");
    }
}

void
RecordReader::parseNativeFrameIndex()
{
    UnresolvedNativeFrame frame{};
    d_input->read(reinterpret_cast<char*>(&frame), sizeof(UnresolvedNativeFrame));
    d_native_frames.emplace_back(frame);
}

AllocationRecord
RecordReader::parseAllocationRecord()
{
    AllocationRecord record{};
    d_input->read(reinterpret_cast<char*>(&record), sizeof(AllocationRecord));
    return record;
}

void
RecordReader::parseSegmentHeader()
{
    std::string filename;
    uintptr_t addr;
    size_t num_segments;
    d_input->getline(filename, '\0');
    d_input->read(reinterpret_cast<char*>(&num_segments), sizeof(num_segments));
    d_input->read(reinterpret_cast<char*>(&addr), sizeof(addr));

    std::vector<Segment> segments(num_segments);
    for (size_t i = 0; i < num_segments; i++) {
        segments.emplace_back(parseSegment());
    }
    d_symbol_resolver.addSegments(filename, addr, segments);
}

Segment
RecordReader::parseSegment()
{
    RecordType record_type;
    d_input->read(reinterpret_cast<char*>(&record_type), sizeof(record_type));
    assert(record_type == RecordType::SEGMENT);
    Segment segment{};
    d_input->read(reinterpret_cast<char*>(&segment), sizeof(Segment));
    return segment;
}

size_t
RecordReader::getAllocationFrameIndex(const AllocationRecord& record)
{
    auto stack = d_stack_traces.find(record.tid);
    if (stack == d_stack_traces.end()) {
        return 0;
    }
    correctAllocationFrame(stack->second, record.py_lineno);
    return d_tree.getTraceIndex(stack->second);
}

void
RecordReader::correctAllocationFrame(stack_t& stack, int lineno)
{
    if (stack.empty()) {
        return;
    }
    const Frame& partial_frame = d_frame_map.at(stack.back());
    Frame allocation_frame{
            partial_frame.function_name,
            partial_frame.filename,
            partial_frame.parent_lineno,
            lineno};
    auto [allocation_index, is_new_frame] = d_allocation_frames.getIndex(allocation_frame);
    if (is_new_frame) {
        d_frame_map.emplace(allocation_index, allocation_frame);
    }
    stack.back() = allocation_index;
}

// Python public APIs

bool
RecordReader::nextAllocationRecord(Allocation* allocation)
{
    try {
        while (true) {
            RecordType record_type;
            d_input->read(reinterpret_cast<char*>(&record_type), sizeof(RecordType));

            switch (record_type) {
                case RecordType::ALLOCATION: {
                    AllocationRecord record = parseAllocationRecord();
                    size_t f_index = getAllocationFrameIndex(record);
                    *allocation = Allocation{
                            .record = record,
                            .frame_index = f_index,
                            .native_segment_generation = d_symbol_resolver.currentSegmentGeneration()};
                    return true;
                }
                case RecordType::FRAME:
                    parseFrame();
                    break;
                case RecordType::FRAME_INDEX:
                    parseFrameIndex();
                    break;
                case RecordType::NATIVE_TRACE_INDEX:
                    parseNativeFrameIndex();
                    break;
                case RecordType::MEMORY_MAP_START:
                    d_symbol_resolver.clearSegments();
                    break;
                case RecordType::SEGMENT_HEADER:
                    parseSegmentHeader();
                    break;
                default:
                    throw std::runtime_error("Invalid record type");
            }
        }
    } catch (const IoError&) {
        return false;
    }
}

PyObject*
RecordReader::Py_GetStackFrame(unsigned int index, size_t max_stacks)
{
    size_t stacks_obtained = 0;
    FrameTree::index_t current_index = index;
    PyObject* list = PyList_New(0);
    if (list == nullptr) {
        return nullptr;
    }

    int current_lineno = -1;
    while (current_index != 0 && stacks_obtained++ != max_stacks) {
        auto node = d_tree.nextNode(current_index);
        const auto& frame = d_frame_map.at(node.frame_id);
        PyObject* pyframe = frame.toPythonObject(d_pystring_cache, current_lineno);
        if (pyframe == nullptr) {
            goto error;
        }
        int ret = PyList_Append(list, pyframe);
        Py_DECREF(pyframe);
        if (ret != 0) {
            goto error;
        }
        current_index = node.parent_index;
        current_lineno = frame.parent_lineno;
    }
    return list;
error:
    Py_XDECREF(list);
    return nullptr;
}

PyObject*
RecordReader::Py_GetNativeStackFrame(FrameTree::index_t index, size_t generation, size_t max_stacks)
{
    size_t stacks_obtained = 0;
    FrameTree::index_t current_index = index;
    PyObject* list = PyList_New(0);
    if (list == nullptr) {
        return nullptr;
    }

    while (current_index != 0 && stacks_obtained++ != max_stacks) {
        auto frame = d_native_frames[current_index - 1];
        current_index = frame.index;
        auto resolved_frames = d_symbol_resolver.resolve(frame.ip, generation);
        if (!resolved_frames) {
            continue;
        }
        for (auto& native_frame : resolved_frames->frames()) {
            PyObject* pyframe = native_frame.toPythonObject(d_pystring_cache);
            if (pyframe == nullptr) {
                return nullptr;
            }
            int ret = PyList_Append(list, pyframe);
            Py_DECREF(pyframe);
            if (ret != 0) {
                goto error;
            }
        }
    }
    return list;
error:
    Py_XDECREF(list);
    return nullptr;
}

HeaderRecord
RecordReader::getHeader() const noexcept
{
    return d_header;
}

}  // namespace pensieve::api
