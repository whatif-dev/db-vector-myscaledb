#include <Processors/QueryPlan/Optimizations/Optimizations.h>
#include <Processors/QueryPlan/Optimizations/projectionsCommon.h>
#include <Processors/QueryPlan/ExpressionStep.h>
#include <Processors/QueryPlan/FilterStep.h>
#include <Processors/QueryPlan/ReadFromMergeTree.h>
#include <Processors/QueryPlan/UnionStep.h>
#include <Processors/QueryPlan/ReadFromPreparedSource.h>
#include <Processors/Sources/NullSource.h>
#include <Common/logger_useful.h>
#include <Storages/MergeTree/MergeTreeDataSelectExecutor.h>
#include <stack>

namespace DB::QueryPlanOptimizations
{

/// Normal projection analysis result in case it can be applied.
/// For now, it is empty.
/// Normal projection can be used only if it contains all required source columns.
/// It would not be hard to support pre-computed expressions and filtration.
struct NormalProjectionCandidate : public ProjectionCandidate
{
};

static ActionsDAGPtr makeMaterializingDAG(const Block & proj_header, const Block main_header)
{
    /// Materialize constants in case we don't have it in output header.
    /// This may happen e.g. if we have PREWHERE.

    size_t num_columns = main_header.columns();
    /// This is a error; will have block structure mismatch later.
    if (proj_header.columns() != num_columns)
        return nullptr;

    std::vector<size_t> const_positions;
    for (size_t i = 0; i < num_columns; ++i)
    {
        auto col_proj = proj_header.getByPosition(i).column;
        auto col_main = main_header.getByPosition(i).column;
        bool is_proj_const = col_proj && isColumnConst(*col_proj);
        bool is_main_proj = col_main && isColumnConst(*col_main);
        if (is_proj_const && !is_main_proj)
            const_positions.push_back(i);
    }

    if (const_positions.empty())
        return nullptr;

    ActionsDAGPtr dag = std::make_unique<ActionsDAG>();
    auto & outputs = dag->getOutputs();
    for (const auto & col : proj_header.getColumnsWithTypeAndName())
        outputs.push_back(&dag->addInput(col));

    for (auto pos : const_positions)
    {
        auto & output = outputs[pos];
        output = &dag->materializeNode(*output);
    }

    return dag;
}

static bool hasAllRequiredColumns(const ProjectionDescription * projection, const Names & required_columns)
{
    for (const auto & col : required_columns)
    {
        if (!projection->sample_block.has(col))
            return false;
    }

    return true;
}


bool optimizeUseNormalProjections(Stack & stack, QueryPlan::Nodes & nodes)
{
    const auto & frame = stack.back();

    auto * reading = typeid_cast<ReadFromMergeTree *>(frame.node->step.get());
    if (!reading)
        return false;

    if (!canUseProjectionForReadingStep(reading))
        return false;

    auto iter = stack.rbegin();
    while (std::next(iter) != stack.rend())
    {
        iter = std::next(iter);

        if (!typeid_cast<FilterStep *>(iter->node->step.get()) &&
            !typeid_cast<ExpressionStep *>(iter->node->step.get()))
            break;
    }

    const auto metadata = reading->getStorageMetadata();
    const auto & projections = metadata->projections;

    std::vector<const ProjectionDescription *> normal_projections;
    for (const auto & projection : projections)
        if (projection.type == ProjectionDescription::Type::Normal)
            normal_projections.push_back(&projection);

    if (normal_projections.empty())
        return false;

    QueryDAG query;
    {
        auto & clild = iter->node->children[iter->next_child - 1];
        if (!query.build(*clild))
            return false;

        if (query.dag)
        {
            query.dag->removeUnusedActions();
            // LOG_TRACE(&Poco::Logger::get("optimizeUseProjections"), "Query DAG: {}", query.dag->dumpDAG());
        }
    }

    std::list<NormalProjectionCandidate> candidates;
    NormalProjectionCandidate * best_candidate = nullptr;

    const Names & required_columns = reading->getRealColumnNames();
    const auto & parts = reading->getParts();
    const auto & query_info = reading->getQueryInfo();
    ContextPtr context = reading->getContext();
    MergeTreeDataSelectExecutor reader(reading->getMergeTreeData());

    auto ordinary_reading_select_result = reading->selectRangesToRead(parts, /* alter_conversions = */ {});
    size_t ordinary_reading_marks = ordinary_reading_select_result->marks();

    // LOG_TRACE(&Poco::Logger::get("optimizeUseProjections"),
    //           "Marks for ordinary reading {}", ordinary_reading_marks);

    std::shared_ptr<PartitionIdToMaxBlock> max_added_blocks = getMaxAddedBlocks(reading);

    for (const auto * projection : normal_projections)
    {
        if (!hasAllRequiredColumns(projection, required_columns))
            continue;

        auto & candidate = candidates.emplace_back();
        candidate.projection = projection;

        ActionDAGNodes added_filter_nodes;
        if (query.filter_node)
            added_filter_nodes.nodes.push_back(query.filter_node);

        bool analyzed = analyzeProjectionCandidate(
            candidate, *reading, reader, required_columns, parts,
            metadata, query_info, context, max_added_blocks, added_filter_nodes);

        if (!analyzed)
            continue;

        // LOG_TRACE(&Poco::Logger::get("optimizeUseProjections"),
        //           "Marks for projection {} {}", projection->name ,candidate.sum_marks);

        if (candidate.sum_marks >= ordinary_reading_marks)
            continue;

        if (best_candidate == nullptr || candidate.sum_marks < best_candidate->sum_marks)
            best_candidate = &candidate;
    }

    if (!best_candidate)
    {
        reading->setAnalyzedResult(std::move(ordinary_reading_select_result));
        return false;
    }

    auto storage_snapshot = reading->getStorageSnapshot();
    auto proj_snapshot = std::make_shared<StorageSnapshot>(
        storage_snapshot->storage, storage_snapshot->metadata, storage_snapshot->object_columns); //, storage_snapshot->data);
    proj_snapshot->addProjection(best_candidate->projection);

    // LOG_TRACE(&Poco::Logger::get("optimizeUseProjections"), "Proj snapshot {}",
    //           proj_snapshot->getColumns(GetColumnsOptions::Kind::All).toString());

    auto query_info_copy = query_info;
    query_info_copy.prewhere_info = nullptr;

    auto projection_reading = reader.readFromParts(
        /*parts=*/ {},
        /*alter_conversions=*/ {},
        required_columns,
        proj_snapshot,
        query_info_copy,
        context,
        reading->getMaxBlockSize(),
        reading->getNumStreams(),
        max_added_blocks,
        best_candidate->merge_tree_projection_select_result_ptr,
        reading->isParallelReadingEnabled());

    if (!projection_reading)
    {
        Pipe pipe(std::make_shared<NullSource>(proj_snapshot->getSampleBlockForColumns(required_columns)));
        projection_reading = std::make_unique<ReadFromPreparedSource>(std::move(pipe));
    }

    bool has_ordinary_parts = best_candidate->merge_tree_ordinary_select_result_ptr != nullptr;
    if (has_ordinary_parts)
        reading->setAnalyzedResult(std::move(best_candidate->merge_tree_ordinary_select_result_ptr));

    // LOG_TRACE(&Poco::Logger::get("optimizeUseProjections"), "Projection reading header {}",
    //           projection_reading->getOutputStream().header.dumpStructure());

    projection_reading->setStepDescription(best_candidate->projection->name);

    auto & projection_reading_node = nodes.emplace_back(QueryPlan::Node{.step = std::move(projection_reading)});
    auto * next_node = &projection_reading_node;

    if (query.dag)
    {
        auto & expr_or_filter_node = nodes.emplace_back();

        if (query.filter_node)
        {
            expr_or_filter_node.step = std::make_unique<FilterStep>(
                projection_reading_node.step->getOutputStream(),
                query.dag,
                query.filter_node->result_name,
                true);
        }
        else
            expr_or_filter_node.step = std::make_unique<ExpressionStep>(
                projection_reading_node.step->getOutputStream(),
                query.dag);

        expr_or_filter_node.children.push_back(&projection_reading_node);
        next_node = &expr_or_filter_node;
    }

    if (!has_ordinary_parts)
    {
        /// All parts are taken from projection
        iter->node->children[iter->next_child - 1] = next_node;
    }
    else
    {
        const auto & main_stream = iter->node->children.front()->step->getOutputStream();
        const auto * proj_stream = &next_node->step->getOutputStream();

        if (auto materializing = makeMaterializingDAG(proj_stream->header, main_stream.header))
        {
            auto converting = std::make_unique<ExpressionStep>(*proj_stream, materializing);
            proj_stream = &converting->getOutputStream();
            auto & expr_node = nodes.emplace_back();
            expr_node.step = std::move(converting);
            expr_node.children.push_back(next_node);
            next_node = &expr_node;
        }

        auto & union_node = nodes.emplace_back();
        DataStreams input_streams = {main_stream, *proj_stream};
        union_node.step = std::make_unique<UnionStep>(std::move(input_streams));
        union_node.children = {iter->node->children.front(), next_node};
        iter->node->children[iter->next_child - 1] = &union_node;
    }

    /// Here we remove last steps from stack to be able to optimize again.
    /// In theory, read-in-order can be applied to projection.
    stack.resize(iter.base() - stack.begin());

    return true;
}

}
