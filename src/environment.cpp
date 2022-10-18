#include "simfil/environment.h"
#include "simfil/function.h"
#include "simfil/ext-geo.h"

namespace simfil
{

Environment::Environment()
    : warnMtx(std::make_unique<std::mutex>())
    , traceMtx(std::make_unique<std::mutex>())
{
    functions["any"]    = &AnyFn::Fn;
    functions["each"]   = &EachFn::Fn;
    functions["all"]    = &EachFn::Fn;
    functions["count"]  = &CountFn::Fn;
    functions["range"]  = &RangeFn::Fn;
    functions["arr"]    = &ArrFn::Fn;
    functions["split"]  = &SplitFn::Fn;
    functions["select"] = &SelectFn::Fn;
    functions["sum"]    = &SumFn::Fn;
    functions["keys"]   = &KeysFn::Fn;
    functions["trace"]  = &TraceFn::Fn;

    /* GeoJSON Extension */
    functions["geo"]        = &geo::GeoFn::Fn;
    functions["point"]      = &geo::PointFn::Fn;
    functions["bbox"]       = &geo::BBoxFn::Fn;
    functions["linestring"] = &geo::LineStringFn::Fn;
    //functions["polygon"]    = &geo::PolygonFn::Fn;
}

Environment::~Environment()
{}

auto Environment::warn(std::string message, std::string detail) -> void
{
    std::unique_lock<std::mutex> _(*warnMtx);
    warnings.emplace_back(std::move(message), std::move(detail));
}

auto Environment::trace(const std::string& name, std::function<void(Trace&)> fn) -> void
{
    std::unique_lock<std::mutex> _(*traceMtx);
    fn(traces[name]);
}

auto Environment::findFunction(std::string name) const -> const Function*
{
    if (auto iter = functions.find(name); iter != functions.end())
        return iter->second;
    return nullptr;
}

Context::Context(Environment* env, Context::Phase phase)
    : env(env)
    , phase(phase)
{}

}
