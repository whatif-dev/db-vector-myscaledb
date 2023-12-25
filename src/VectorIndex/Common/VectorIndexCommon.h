/*
 * Copyright (2024) MOQI SINGAPORE PTE. LTD. and/or its affiliates
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once
#include <cmath>
#include <iostream>
#include <string>
#include <lib/lz4.h>
#include <Poco/JSON/JSON.h>
#include <Poco/JSON/Object.h>

#include <Compression/CompressedReadBuffer.h>
#include <Compression/CompressedWriteBuffer.h>
#include <Interpreters/OpenTelemetrySpanLog.h>
#include <Common/Exception.h>
#include <VectorIndex/Storages/VectorScanDescription.h>

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wshorten-64-to-32"
#pragma clang diagnostic ignored "-Wimplicit-fallthrough"
#pragma clang diagnostic ignored "-Wfloat-conversion"
#pragma clang diagnostic ignored "-Wimplicit-float-conversion"
#pragma clang diagnostic ignored "-Wcovered-switch-default"
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wunused-function"
#include <SearchIndex/VectorSearch.h>
#pragma clang diagnostic pop
#endif

#include <VectorIndex/Common/IndexException.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wzero-as-null-pointer-constant"
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#pragma GCC diagnostic pop

#include <SearchIndex/SearchIndexCommon.h>
#include <SearchIndex/VectorIndex.h>

#define VECTOR_INDEX_FILE_SUFFIX ".vidx3"
#define VECTOR_INDEX_FILE_OLD_SUFFIX ".vidx2"
#define MAX_BRUTE_FORCE_SEARCH_SIZE 50000
#define MIN_SEGMENT_SIZE 1000000
#define VECTOR_INDEX_DESCRIPTION "vector_index_description"
#define VECTOR_INDEX_CHECKSUMS "vector_index_checksums"
#define DECOUPLE_OWNER_PARTS_RESTORE_PREFIX "restore"
#define DISK_MODE_PARAM "disk_mode"

namespace DB
{
namespace ErrorCodes
{
    extern const int BAD_ARGUMENTS;
    extern const int STD_EXCEPTION;
    extern const int UNKNOWN_EXCEPTION;
}
}

/// Convert search index execption to DB::Exception
#define VECTOR_INDEX_EXCEPTION_ADAPT(callable, func_name)               \
    try                                                                 \
    {                                                                   \
        callable;                                                       \
    }                                                                   \
    catch (const DB::Exception & e)                                     \
    {                                                                   \
        throw VectorIndex::IndexException(                              \
            e.code(),                                                   \
            "Error in {}, {}", func_name, e.message());                 \
    }                                                                   \
    catch (const SearchIndexException & e)                              \
    {                                                                   \
        throw VectorIndex::IndexException(                              \
            e.getCode(),                                                \
            "Error in {}, {}", func_name, e.what());                    \
    }                                                                   \
    catch (const std::exception & e)                                    \
    {                                                                   \
        throw VectorIndex::IndexException(                              \
            DB::ErrorCodes::STD_EXCEPTION,                              \
            "Error in {}, {}",                                          \
            func_name, e.what());                                       \
    }                                                                   \
    catch (...)                                                         \
    {                                                                   \
        throw VectorIndex::IndexException(                              \
            DB::ErrorCodes::UNKNOWN_EXCEPTION,                          \
            "Unknown error in {}.", func_name);                         \
    }

namespace Search
{
class DiskIOManager;
enum class DataType;

}
namespace VectorIndex
{

using RowIds = std::vector<UInt64>;
using RowSource = std::vector<uint8_t>;

using VectorIndexIStream = Search::AbstractIStream;
using VectorIndexOStream = Search::AbstractOStream;

using VectorIndexBitmap = Search::DenseBitmap;
using VectorIndexBitmapPtr = std::shared_ptr<Search::DenseBitmap>;

using SearchResult = Search::SearchResult;
using SearchResultPtr = std::shared_ptr<SearchResult>;

using VectorIndexParameter = Search::Parameters;
using VectorIndexType = Search::IndexType;
using VectorIndexMetric = Search::Metric;
using VectorIndexDataType = Search::DataType;

using DiskIOManager = Search::DiskIOManager;

using SearchVectorIndex = Search::VectorIndex<VectorIndexIStream, VectorIndexOStream, VectorIndexBitmap, VectorIndexDataType::FloatVector>;
using VectorIndexPtr = std::shared_ptr<SearchVectorIndex>;

using SearchFloatVectorIndex = Search::VectorIndex<VectorIndexIStream, VectorIndexOStream, VectorIndexBitmap, VectorIndexDataType::FloatVector>;
using FloatVectorIndexPtr = std::shared_ptr<SearchFloatVectorIndex>;

using SearchBinaryVectorIndex = Search::VectorIndex<VectorIndexIStream, VectorIndexOStream, VectorIndexBitmap, VectorIndexDataType::BinaryVector>;
using BinaryVectorIndexPtr = std::shared_ptr<SearchBinaryVectorIndex>;

using VectorIndexVariantPtr = std::variant<FloatVectorIndexPtr, BinaryVectorIndexPtr>;

/// SearchIndexDataTypeMap maps Search::DataType enum values actual types
template <Search::DataType>
struct SearchIndexDataTypeMap;

template <Search::DataType T>
using VectorIndexSourceDataReader = Search::IndexSourceDataReader<typename SearchIndexDataTypeMap<T>::IndexDatasetType>;

template <>
struct SearchIndexDataTypeMap<Search::DataType::FloatVector>
{
    using VectorDatasetType = float;
    using IndexDatasetType = float;
    using VectorIndexPtr = FloatVectorIndexPtr;
};

template <>
struct SearchIndexDataTypeMap<Search::DataType::BinaryVector>
{
    using VectorDatasetType = uint8_t;
    using IndexDatasetType = bool;
    using VectorIndexPtr = BinaryVectorIndexPtr;
};

const int DEFAULT_TOPK = 30;


inline VectorIndexType fallbackToFlat(const Search::DataType &search_type)
{
    switch (search_type)
    {
        case Search::DataType::FloatVector:
            return VectorIndexType::FLAT;
        case Search::DataType::BinaryVector:
            return VectorIndexType::BinaryFLAT;
        default:
            throw DB::Exception(DB::ErrorCodes::LOGICAL_ERROR, "Unsupported vector search type");
    }
}

static inline std::string ParametersToString(const VectorIndexParameter & params)
{
    rapidjson::StringBuffer strBuf;
    rapidjson::Writer<rapidjson::StringBuffer> writer(strBuf);
    writer.StartObject();
    for (auto & param : params)
    {
        writer.Key(param.first.c_str());
        writer.String(param.second.c_str());
    }
    writer.EndObject();
    return strBuf.GetString();
}

static inline VectorIndexParameter convertPocoJsonToMap(Poco::JSON::Object::Ptr json)
{
    VectorIndexParameter params;
    if (json)
    {
        for (Poco::JSON::Object::ConstIterator it = json->begin(); it != json->end(); it++)
        {
            params.insert(std::make_pair(it->first, it->second.toString()));
        }
    }

    return params;
}

static inline std::string getVectorIndexChecksumsFileName(const std::string & index_name)
{
    return index_name + "-" + VECTOR_INDEX_CHECKSUMS + VECTOR_INDEX_FILE_SUFFIX;
}

static inline std::string getVectorIndexDescriptionFileName(const std::string & index_name)
{
    return index_name + "-" + VECTOR_INDEX_DESCRIPTION + VECTOR_INDEX_FILE_SUFFIX;
}

static inline std::string getDecoupledVectorIndexDescriptionFileName(const std::string & index_name, const int & old_part_id, const std::string & old_part_name)
{
    return "merged-" + std::to_string(old_part_id) + "-" + old_part_name + "-" + getVectorIndexDescriptionFileName(index_name);
}

}
