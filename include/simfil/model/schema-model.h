#pragma once

#include "simfil/model/model.h"

namespace simfil
{

/**
 * Lazy, read-only descriptor model over an existing schema graph.
 *
 * The lookup closure must retain its graph owner. Supply the same dedicated
 * schema/completion StringPool used by that lookup and by the query Environment;
 * this adapter retains, but never modifies, it. The graph must remain unchanged
 * for the view's lifetime. Views are request-local, not concurrently mutable.
 */
class SchemaModel : public Model
{
public:
    /** Bound expansion by schema depth and lazily allocated descriptor nodes. */
    SchemaModel(std::shared_ptr<StringPool> strings, Schema::Lookup lookup,
                std::size_t maxDepth = 32, std::size_t maxNodes = 10000);
    /** Release request-local views and the retained schema binding. */
    ~SchemaModel() override;

    /** Obtain a descriptor root; NoSchemaId describes unknown information. */
    auto root(SchemaId schema) const -> ModelNode::Ptr;

    /** Return the local namespace shared with the query environment. */
    auto strings() const -> std::shared_ptr<StringPool>;

    /** Number of allocated views, not the size of the underlying schema graph. */
    auto materializedNodeCount() const -> std::size_t;

    using Model::resolve;
    /** Resolve a descriptor address through the same node protocol as ordinary models. */
    auto resolve(const ModelNode& node, const ResolveFn& callback) const -> tl::expected<void, Error> override;
    /** Resolve metadata and represented field names without inserting into the pool. */
    auto lookupStringId(StringId id) const -> std::optional<std::string_view> override;

private:
    class Impl;
    class Node;
    std::unique_ptr<Impl> impl_;
};

}
