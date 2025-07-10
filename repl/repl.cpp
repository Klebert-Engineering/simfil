#include "simfil/environment.h"
#include "simfil/parser.h"
#include "simfil/simfil.h"
#include "simfil/expression.h"
#include "simfil/value.h"
#include "simfil/model/model.h"
#include <algorithm>
#include <iterator>

#if defined(SIMFIL_WITH_MODEL_JSON)
#  include "simfil/model/json.h"
#endif

#include <string>
#include <string_view>
#include <thread>
#include <atomic>
#include <fstream>
#include <chrono>
#include <optional>
#include <iostream>

#if defined(WITH_READLINE)
#  include <readline/readline.h>
#  include <readline/history.h>
#endif

using namespace std::string_literals;

auto model = std::make_shared<simfil::ModelPool>();
simfil::Environment* current_env = nullptr;

struct
{
    bool auto_any = false;
    bool auto_wildcard = false;
    bool verbose = true;
    bool multi_threaded = true;
} options;

static void set_option(const std::string& option, bool& flag, std::string_view cmd)
{
    while (isspace(cmd.back()))
        cmd.remove_suffix(1);

    if (cmd == "/"s + option) {
        flag = !flag;
        std::cout << option << " is " << (flag ? "on" : "off") << "\n";
    }
}

static auto make_env()
{
    auto env = std::make_unique<simfil::Environment>(simfil::Environment::WithNewStringCache);
    return env;
}

#ifdef WITH_READLINE
char* command_generator(const char* text, int state)
{
    static std::vector<std::string> matches;
    static size_t match_index = 0;

    if (state == 0) {
        std::string query = text;
        matches.clear();
        match_index = 0;

        std::vector<std::string> cmds;
        auto tmp_env = make_env();
        for (const auto& [name, _] : tmp_env->functions) {
            cmds.push_back(name + "("s);
        }

        if (query[0] == '/') {
            return nullptr;
        }

        if (current_env) {
            simfil::CompletionOptions opts;
            opts.limit = 25;
            opts.autoWildcard = options.auto_wildcard;

            auto root = model->root(0);
            if (!root)
                return nullptr;

            auto comp = simfil::complete(*current_env, query, rl_point, *root, opts);
            if (!comp)
                return nullptr;

            for (const auto& candidate : *comp) {
                matches.push_back(query.substr(0, candidate.location.offset) + candidate.text);
            }
        }
    }

    if (match_index >= matches.size()) {
        return nullptr;
    } else {
        return strdup(matches[match_index++].c_str());
    }
}

char** command_completion(const char *text, int start, int end)
{
    rl_attempted_completion_over = 1;
    rl_completion_suppress_append = 1;
    return rl_completion_matches(text, command_generator);
}
#endif

static std::string input(simfil::Environment& env, const char* prompt = "> ")
{
    current_env = &env;

    std::string r;
#ifdef WITH_READLINE
    auto buf = readline(prompt);
    if (buf && buf[0]) {
        r = buf;
        add_history(buf);
    }
    free(buf);
#else
    std::cout << prompt << std::flush;
    std::getline(std::cin, r);
#endif

    current_env = nullptr;
    return r;
}

static auto eval_mt(simfil::Environment& env, const simfil::AST& ast, const std::shared_ptr<simfil::ModelPool>& model, simfil::Diagnostics& diag)
{
    std::vector<std::vector<simfil::Value>> result;
    result.resize(std::max<size_t>(1, model->numRoots()));

    if (!model->numRoots()) {
        model->addRoot(simfil::ModelNode::Ptr());
        auto res = simfil::eval(env, ast, *model->root(0), &diag);
        if (res)
            result[0] = std::move(*res);
        // TODO: Output eval errors
        return result;
    }

    auto n_threads = std::max<size_t>(std::min<size_t>(std::thread::hardware_concurrency(), model->numRoots()), 1);
    if (env.debug)
        n_threads = 1;
    if (!options.multi_threaded)
        n_threads = 1;

    auto idx = std::atomic_size_t(0);
    auto threads = std::vector<std::thread>();
    threads.reserve(n_threads);
    for (auto i = 0; i < n_threads; ++i) {
        threads.emplace_back([&, i]() {
            size_t next;
            while ((next = idx++) < model->numRoots()) {
                try {
                    auto res = simfil::eval(env, ast, *model->root(next), &diag);
                    if (res)
                        result[next] = std::move(*res);
                    // TODO: Output eval errors
                } catch (...) {
                    return;
                }
            }
        });
    }

    for (auto& thread : threads)
        if (thread.joinable())
            thread.join();

    return result;
}

static void show_help()
{
    std::cout << "Usage: simfil-repl [OPTIONS] [--] FILENAME...\n"
        << "\n"
        << "Options:\n"
        << "  -D <identifier=value>\n"
        << "          Define a constant variable, set to value\n"
        << "  -h\n"
        << "          Show this help"
        << "\n";
}

static void display_error(std::string_view expression, const simfil::Error& e)
{
    static const auto indent = "  ";
    auto [offset, size] = e.location;

    std::string underline;
    if (size >= 0) {
        if (offset > 0)
            std::generate_n(std::back_inserter(underline), offset, []() { return ' '; });
        underline.push_back('^');
        if (size > 0)
            std::generate_n(std::back_inserter(underline), size - 1, []() { return '~'; });
    }

    std::cout << "Error:\n"
        << indent << e.message << ".\n\n"
        << indent << expression << "\n"
        << indent << underline << "\n";
}

int main(int argc, char *argv[])
{
#if WITH_READLINE
    rl_completer_word_break_characters = "";
    rl_attempted_completion_function = command_completion;
#endif

    std::map<std::string, simfil::Value, simfil::CaseInsensitiveCompare> constants;

    auto load_json = [](const std::string_view& filename) {
#if defined(SIMFIL_WITH_MODEL_JSON)
        std::cout << "Parsing " << filename << "\n";
        auto f = std::ifstream(std::string(filename));
        simfil::json::parse(f, model);
#endif
    };

    auto tail_args = false;
    while (*++argv != nullptr) {
        std::string_view arg = *argv;
        if ((!tail_args) && (arg[0] == '-')) {
            switch (arg[1]) {
            case '-':
                tail_args = true;
                break;
            case 'h':
                show_help();
                return 0;
            case 'D':
                arg.remove_prefix(2);
                if (arg.empty()) {
                    arg = *++argv;
                }
                if (auto pos = arg.find('='); (pos != std::string::npos) && (pos > 0)) {
                    constants.try_emplace(std::string(arg.substr(0, pos)), simfil::Value::make(std::string(arg.substr(pos + 1))));
                } else {
                    std::cerr << "Invalid definition: " << arg << "\n";
                    return 1;
                }
                break;
            default:
                std::cerr << "Unknown option: " << arg << "\n";
                return 1;
            }
        } else {
            load_json(arg);
        }
    }

    for (;;) {
        simfil::Environment env(model->strings());
        env.constants = constants;

        auto cmd = input(env, "> ");
        if (cmd.empty())
            continue;
        if (cmd[0] == '/') {
            set_option("any", options.auto_any, cmd);
            set_option("wildcard", options.auto_wildcard, cmd);
            set_option("verbose", options.verbose, cmd);
            set_option("mt", options.multi_threaded, cmd);
            continue;
        }

        auto ast = simfil::compile(env, cmd, options.auto_any, options.auto_wildcard);
        if (!ast) {
            display_error(cmd, ast.error());
            continue;
        }

        if (options.verbose)
            std::cout << "Expression:\n  " << (*ast)->expr().toString() << "\n";

        simfil::Diagnostics diag;
        std::vector<std::vector<simfil::Value>> res;
        std::chrono::milliseconds msec;

        try {
            auto eval_begin = std::chrono::steady_clock::now();
            res = eval_mt(env, **ast, model, diag);
            msec = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - eval_begin);
        } catch (const std::exception& e) {
            std::cout << "Error:\n  " << e.what() << "\n";
            continue;
        }

        std::vector<simfil::Value> flatres;
        flatres.reserve(res.size());
        for (auto&& v : res) {
            flatres.insert(flatres.end(),
                           std::make_move_iterator(v.begin()),
                           std::make_move_iterator(v.end()));
        }

        std::cout << "Time:\n  " << msec.count() << " ms\n";
        std::cout << "Result:\n";
        for (const auto& v : flatres) {
            std::cout << "  " << v.toString() << "\n";
        }

        auto messages = simfil::diagnostics(env, **ast, diag);
        if (!messages) {
            std::cerr << "Error: " << messages.error().message << "\n";
            continue;
        }

        if (!messages->empty()) {
            std::cout << "Diagnostics:\n";

            auto underlineQuery = [&](simfil::SourceLocation loc) {
                std::string underline;
                std::fill_n(std::back_inserter(underline), loc.offset, ' ');
                underline.push_back('^');
                if (loc.size > 0)
                    std::fill_n(std::back_inserter(underline), loc.size - 1, '~');
                return underline;
            };

            for (const auto& m : *messages) {
                std::cout << "  " << m.message << "\n"
                          << "  Here: " << (*ast)->query() << "\n"
                          << "        " << underlineQuery(m.location) << "\n"
                          << "   Fix: " << m.fix.value_or("-") << "\n";
            }
        }
    }

    return 0;
}
