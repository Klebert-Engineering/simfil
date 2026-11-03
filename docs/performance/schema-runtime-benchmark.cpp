#include "simfil/simfil.h"
#include "simfil/model/model.h"
#include "simfil/model/schema.h"
#ifndef SIMFIL_PERF_REFERENCE
#include "simfil/model/schema-pool.h"
#endif

#include <nlohmann/json.hpp>
#include <algorithm>
#include <barrier>
#include <chrono>
#include <iostream>
#include <map>
#include <sstream>
#include <thread>
#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#endif

using namespace simfil;

/** One pinned steady-state measurement over independent models sharing a graph.
 * Compile the identical source against 77fe792 with SIMFIL_PERF_REFERENCE to
 * exercise its callback interface, and against the candidate without that flag. */
class RuntimeBenchmark
{
    /** Reproduce the repository's sparse-wide graph without benchmark-framework overhead. */
    class Graph
    {
    public:
        std::shared_ptr<StringPool> strings = std::make_shared<StringPool>();
        std::map<SchemaId, std::unique_ptr<Schema>> domains;
        std::vector<std::string> branches;
#ifndef SIMFIL_PERF_REFERENCE
        std::shared_ptr<SchemaPool> compact;
#endif
        /** Build declarations once, outside model construction and all timed regions. */
        explicit Graph(size_t width, bool useCompact)
        {
            auto target = strings->emplace("target").value();
            auto payload = strings->emplace("payload").value();
            auto noise = strings->emplace("noise").value();
            for (size_t i = 0; i < width; ++i)
            {
                branches.push_back("branch" + std::to_string(i));
                strings->emplace(branches.back()).value();
            }
#ifndef SIMFIL_PERF_REFERENCE
            if (useCompact)
            {
                compact = std::make_shared<SchemaPool>(strings);
                for (size_t i = 0; i < 4; ++i)
                    compact->addSchema(Schema::Kind::Object);
                compact->addSchema(Schema::Kind::Array);
                auto scalar = compact->addSchema(Schema::Kind::Value);
                compact->addFieldSchema(1, "payload", 2);
                compact->addFieldSchema(2, "target", scalar);
                compact->addFieldSchema(3, "noise", scalar);
                for (size_t i = 0; i < width; ++i)
                    compact->addFieldSchema(4, branches[i], i == 0 ? 1 : 3);
                compact->addElementSchema(5, 4);
                compact->finalize();
                return;
            }
#else
            if (useCompact)
                throw std::invalid_argument("The reference has no compact pool");
#endif
            for (SchemaId id = 1; id <= 4; ++id)
                domains[id] = std::make_unique<ObjectSchema>();
            auto object = [&](SchemaId id) -> ObjectSchema&
            { return static_cast<ObjectSchema&>(*domains.at(id)); };
            domains[65000] = std::make_unique<ValueSchema>();
            object(1).addField(payload, {2});
            object(2).addField(target, {65000});
            object(3).addField(noise, {65000});
            for (size_t i = 0; i < width; ++i)
                object(4).addField(strings->get(branches[i]), {SchemaId(i == 0 ? 1 : 3)});
            auto array = std::make_unique<ArraySchema>();
            array->addElementSchemas({4});
            domains[5] = std::move(array);
            for (auto& [id, domain] : domains)
                domain->finalize([&](SchemaId id) { return domains.at(id).get(); });
        }
        /** The reference and ordinary candidate retain the original map lookup shape. */
        Schema const* lookup(SchemaId id) const
        {
#ifndef SIMFIL_PERF_REFERENCE
            if (compact)
                return compact->domain(id);
#endif
            auto found = domains.find(id);
            return found == domains.end() ? nullptr : found->second.get();
        }
    };

    std::string representation_ = "callback", query_ = "basic", construction_ = "main";
    size_t objects_ = 2000, width_ = 32, queries_ = 200, warmups_ = 3;
    std::vector<int> cpus_;
    std::shared_ptr<Graph> graph_;
#ifndef SIMFIL_PERF_REFERENCE
    std::shared_ptr<const SchemaBinding> binding_;
#endif

    /** Fail on invalid fixture operations rather than measuring a silently reduced workload. */
    template <class T> static void check(T const& result)
    {
        if (!result)
            throw std::runtime_error(result.error().message);
    }
    /** Pin each evaluator before worker-local allocation or warmup. */
    static void pin(int cpu)
    {
#ifdef __linux__
        if (cpu < 0 || cpu >= CPU_SETSIZE)
            throw std::invalid_argument("Invalid CPU index");
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(cpu, &set);
        if (pthread_setaffinity_np(pthread_self(), sizeof(set), &set))
            throw std::runtime_error("Cannot pin worker CPU");
#else
        throw std::runtime_error("Pinned measurements require Linux");
#endif
    }
    /** Build exactly the sparse-wide model used by test/schema.cpp, with no qualified leaves. */
    std::shared_ptr<ModelPool> model() const
    {
        auto strings = graph_->strings;
        if (representation_ == "separate" || representation_ == "pool-separate")
        {
            strings = std::make_shared<StringPool>();
            strings->emplace("runtime-padding").value();
        }
        auto pool = std::make_shared<ModelPool>(strings);
#ifndef SIMFIL_PERF_REFERENCE
        if (binding_)
            pool->setSchemaBinding(binding_);
#endif
        auto root = pool->newArray(objects_);
        for (size_t i = 0; i < objects_; ++i)
        {
            auto object = pool->newObject(width_, true);
            auto branch = pool->newObject(1, true), payload = pool->newObject(1, true);
            check(payload->addField("target", int64_t{1}));
            check(payload->setSchema(2));
            check(branch->addField("payload", payload));
            check(branch->setSchema(1));
            check(object->addField(graph_->branches.front(), branch));
            for (size_t b = 1; b < width_; ++b)
            {
                auto noise = pool->newObject(1, true);
                check(noise->addField("noise", int64_t(i + b)));
                check(noise->setSchema(3));
                check(object->addField(graph_->branches[b], noise));
            }
            check(object->setSchema(4));
            root->append(object);
        }
        check(root->setSchema(5));
        pool->addRoot(root);
        return pool;
    }

    /** Prove binding/provenance and dictionary separation before measuring count-only queries. */
    void verifyModel(ModelPool const& pool, Environment& env, SharedAST const& proof) const
    {
        auto root = pool.root(0).value();
        BoundExpression witness(proof, env);
        auto values = witness.eval(*root);
        check(values);
        if (values->size() != objects_)
            throw std::runtime_error("Incorrect proof query cardinality");
#ifndef SIMFIL_PERF_REFERENCE
        auto rootDomain = root->schemaReference();
        if (binding_ && (rootDomain.id != 5 || rootDomain.binding.get() != binding_.get()))
            throw std::runtime_error("Model does not retain the intended exact graph");
        auto separate = representation_ == "separate" || representation_ == "pool-separate";
        if ((pool.strings().get() != graph_->strings.get()) != separate ||
            (separate && pool.strings()->get("target") == graph_->strings->get("target")))
            throw std::runtime_error("Incorrect dictionary namespace separation");
#endif
        for (auto const& value : *values)
        {
            if (value.as<ValueType::Int>() != 1)
                throw std::runtime_error("Incorrect proof query value");
#ifndef SIMFIL_PERF_REFERENCE
            auto domain = value.schemaReference();
            if (binding_ && (domain.id != (graph_->compact ? 6 : 65000) ||
                             domain.binding.get() != binding_.get()))
                throw std::runtime_error("Bound query lost scalar provenance");
#endif
        }
    }

public:
    /** Parse only explicit workload controls so subprocess output is self-describing. */
    RuntimeBenchmark(int argc, char** argv)
    {
        for (int i = 1; i < argc; i += 2)
        {
            if (i + 1 == argc)
                throw std::invalid_argument("Every option requires a value");
            std::string key = argv[i], value = argv[i + 1];
            if (key == "--representation")
                representation_ = value;
            else if (key == "--query")
                query_ = value;
            else if (key == "--construction")
                construction_ = value;
            else if (key == "--objects")
                objects_ = std::stoull(value);
            else if (key == "--branches")
                width_ = std::stoull(value);
            else if (key == "--queries")
                queries_ = std::stoull(value);
            else if (key == "--warmups")
                warmups_ = std::stoull(value);
            else if (key == "--cpus")
            {
                std::istringstream input(value);
                for (std::string cpu; std::getline(input, cpu, ',');)
                    cpus_.push_back(std::stoi(cpu));
            }
            else
                throw std::invalid_argument("Unknown option: " + key);
        }
        if (cpus_.empty() || cpus_.size() > 16 || !objects_ || !width_ || !queries_ ||
            (construction_ != "main" && construction_ != "worker") ||
            (query_ != "basic" && query_ != "plans" && query_ != "exact"))
            throw std::invalid_argument("Invalid workload parameters");
        if (representation_ != "callback" && representation_ != "bound" &&
            representation_ != "separate" && representation_ != "pool" &&
            representation_ != "pool-separate")
            throw std::invalid_argument("Unknown schema representation");
#ifdef SIMFIL_PERF_REFERENCE
        if (representation_ != "callback")
            throw std::invalid_argument("Reference must use callback mode");
#endif
    }
    /** Warm independently, synchronize only the start, and report every worker's latency. */
    nlohmann::json run()
    {
        // Deterministic first-touch placement also matters on NUMA machines.
        pin(cpus_.front());
        graph_ = std::make_shared<Graph>(width_, representation_.starts_with("pool"));
#ifndef SIMFIL_PERF_REFERENCE
        if (representation_ != "callback")
            binding_ = graph_->compact
                           ? graph_->compact->binding("runtime-benchmark-pool")
                           : std::make_shared<SchemaBinding>(
                                 "runtime-benchmark-graph", graph_,
                                 [graph = graph_](SchemaId id) { return graph->lookup(id); },
                                 graph_->strings);
#endif
        Environment compiler(graph_->strings);
        auto source =
            query_ == "exact" ? "count(*.branch0.payload.target == 1)" : "count(**.target == 1)";
        auto compiled = compile(compiler, source, false, false);
        check(compiled);
        SharedAST ast(std::move(*compiled));
        auto proofAst = compile(compiler, "*.branch0.payload.target", false, false);
        check(proofAst);
        SharedAST proof(std::move(*proofAst));
        std::vector<std::shared_ptr<ModelPool>> models(cpus_.size());
        if (construction_ == "main")
            for (auto& value : models)
                value = model();
        std::vector<double> elapsed(cpus_.size());
        std::vector<std::string> errors(cpus_.size());
        std::barrier start(ptrdiff_t(cpus_.size()));
        std::barrier finish(ptrdiff_t(cpus_.size()));
        std::vector<std::thread> workers;
        for (size_t worker = 0; worker < cpus_.size(); ++worker)
            workers.emplace_back(
                [&, worker]
                {
                    bool started = false;
                    try
                    {
                        pin(cpus_[worker]);
                        if (!models[worker])
                            models[worker] = model();
                        Environment env(models[worker]->strings());
                        if (representation_ == "callback" || representation_ == "bound")
                            env.querySchemaCallback = [graph = graph_](SchemaId id)
                            { return graph->lookup(id); };
                        verifyModel(*models[worker], env, proof);
                        env.enableWildcardFieldPlans = query_ == "plans";
                        BoundExpression query(ast, env);
                        auto root = models[worker]->root(0).value();
                        auto evaluate = [&]
                        {
                            auto result = query.eval(*root);
                            check(result);
                            if (result->size() != 1 ||
                                result->front().as<ValueType::Int>() != int64_t(objects_))
                                throw std::runtime_error("Incorrect query result");
                        };
                        for (size_t i = 0; i < warmups_; ++i)
                            evaluate();
                        start.arrive_and_wait();
                        started = true;
                        auto begin = std::chrono::steady_clock::now();
                        for (size_t i = 0; i < queries_; ++i)
                            evaluate();
                        elapsed[worker] = std::chrono::duration<double, std::micro>(
                                              std::chrono::steady_clock::now() - begin)
                                              .count() /
                                          queries_;
                        // Early workers must not tear down shared graph state during peers' timing.
                        finish.arrive_and_wait();
                    }
                    catch (std::exception const& error)
                    {
                        errors[worker] = error.what();
                        if (!started)
                            start.arrive_and_drop();
                        finish.arrive_and_drop();
                    }
                });
        for (auto& worker : workers)
            worker.join();
        for (auto const& error : errors)
            if (!error.empty())
                throw std::runtime_error(error);
        nlohmann::json result = {{"representation", representation_},
                                 {"query", query_},
                                 {"construction", construction_},
                                 {"objects", objects_},
                                 {"branches", width_},
                                 {"queries", queries_},
                                 {"warmups", warmups_},
                                 {"cpus", cpus_},
                                 {"main_cpu", cpus_.front()},
                                 {"worker_us", elapsed},
                                 {"max_worker_us", *std::ranges::max_element(elapsed)},
                                 {"sizeof_value", sizeof(Value)},
                                 {"sizeof_model_node", sizeof(ModelNode)},
                                 {"sizeof_scalar", sizeof(ScalarValueType)}};
        auto memory = [](MemoryUsage usage) -> nlohmann::json {
            return {{"logical_bytes", usage.logicalBytes},
                    {"allocated_bytes", usage.allocatedBytes}};
        };
        auto& usage = result["memory"];
        usage["basis"] = "retained-capacity lower bounds; not allocator/RSS totals";
        usage["schema_dictionary"] = memory(graph_->strings->memoryUsage());
        usage["models"] = nlohmann::json::array();
        for (auto const& model : models)
        {
            auto stats = model->memoryUsageStats();
            nlohmann::json entry = {{"columns", memory(stats.total())}};
            // Shared dictionaries are counted once above, not once per worker.
            entry["private_dictionary"] = model->strings().get() == graph_->strings.get()
                                              ? memory({})
                                              : memory(model->strings()->memoryUsage());
#ifndef SIMFIL_PERF_REFERENCE
            entry["schema_binding_storage"] = memory(stats.schemaBindings);
#endif
            usage["models"].push_back(std::move(entry));
        }
        // The independent ObjectSchema registry has no complete retained-memory API.
        // Null is an explicit evidence gap, never an invented zero-byte graph.
        usage["graph"] = nullptr;
#ifndef SIMFIL_PERF_REFERENCE
        if (graph_->compact)
            usage["graph"] = {
                {"domain_columns_and_indexes", memory(graph_->compact->schemaMemoryUsage())},
                {"primitive_columns", memory(graph_->compact->memoryUsageStats().total())}};
#endif
        return result;
    }
};

/** Emit one JSON record; timing failures never masquerade as fast successful runs. */
int main(int argc, char** argv)
{
    try
    {
        std::cout << RuntimeBenchmark(argc, argv).run().dump() << '\n';
    }
    catch (std::exception const& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
