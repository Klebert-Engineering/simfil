#include "simfil/simfil.h"
#include "simfil/expression.h"
#include "simfil/value.h"
#include "simfil/model.h"

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

struct
{
    bool auto_any = false;
    bool sum_results = true;
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
    auto env = std::make_unique<simfil::Environment>();
    return env;
}

#ifdef WITH_READLINE
char* command_generator(const char *text, int state)
{
    static std::vector<std::string> matches;
    static size_t match_index = 0;

    if (state == 0) {
        matches.clear();
        match_index = 0;

        std::vector<std::string> cmds;
        auto tmp_env = make_env();
        for (const auto& [name, _] : tmp_env->functions) {
            cmds.push_back(name + "("s);
        }

        for (auto word : cmds) {
            if (word.substr(0, strlen(text)) == text)
                matches.push_back(std::string(word));
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

static std::string input(const char* prompt = "> ")
{
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
    std::cin >> r;
#endif
    return r;
}

static auto eval_mt(simfil::Environment& env, const simfil::Expr& expr, const std::vector<std::unique_ptr<simfil::Model>>& model)
{
    std::vector<std::vector<simfil::Value>> result;
    result.resize(std::max<size_t>(1, model.size()));

    if (model.empty()) {
        result[0] = simfil::eval(env, expr, nullptr);
        return result;
    }

    auto n_threads = std::max<size_t>(std::min<size_t>(std::thread::hardware_concurrency(), model.size()), 1);
    if (env.debug)
        n_threads = 1;
    if (!options.multi_threaded)
        n_threads = 1;

    auto idx = std::atomic_size_t(0);
    auto threads = std::vector<std::thread>();
    threads.reserve(n_threads);
    for (auto i = 0; i < n_threads; ++i) {
        threads.emplace_back(std::thread([&, i]() {
            size_t next;
            while ((next = idx++) < model.size()) {
                const auto& doc = model[next];
                try {
                    result[next] = simfil::eval(env, expr, doc->root());
                } catch (...) {
                    return;
                }
            }
        }));
    }

    for (auto& thread : threads)
        if (thread.joinable())
            thread.join();

    return result;
}


int main(int argc, char *argv[])
{
#if WITH_READLINE
    rl_attempted_completion_function = command_completion;
#endif

    std::vector<std::unique_ptr<simfil::Model>> model;
#if defined(SIMFIL_WITH_MODEL_JSON)
    for (auto arg = argv + 1; *arg; ++arg) {
        std::cout << "Parsing " << *arg << "\n";
        auto f = std::ifstream(*arg);
        model.push_back(simfil::json::parse(f));
    }
#endif

    for (;;) {
        auto cmd = input("> ");
        if (cmd.empty())
            continue;
        if (cmd[0] == '/') {
            set_option("any", options.auto_any, cmd);
            set_option("verbose", options.verbose, cmd);
            set_option("mt", options.multi_threaded, cmd);
            continue;
        }

        simfil::Environment env;
        simfil::ExprPtr expr;
        try {
            expr = simfil::compile(env, cmd, options.auto_any);
        } catch (const std::exception& e) {
            std::cout << "Compile:\n  " << e.what() << "\n";
            continue;
        }

        if (options.verbose)
            std::cout << "Expression:\n  " << expr->toString() << "\n";

        std::vector<std::vector<simfil::Value>> res;
        std::chrono::milliseconds msec;

        try {
            auto eval_begin = std::chrono::steady_clock::now();
            res = eval_mt(env, *expr, model);
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
        ///if (!options.sum_results) {
            for (const auto& v : flatres) {
                std::cout << "  " << v.toString() << "\n";
            }
        ///}
    }

    return 0;
}
