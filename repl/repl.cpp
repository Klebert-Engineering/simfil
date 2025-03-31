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
    std::getline(std::cin, r);
#endif
    return r;
}

static auto eval_mt(simfil::Environment& env, const simfil::Expr& expr, const std::shared_ptr<simfil::ModelPool>& model)
{
    std::vector<std::vector<simfil::Value>> result;
    result.resize(std::max<size_t>(1, model->numRoots()));

    if (!model->numRoots()) {
        model->addRoot(simfil::ModelNode::Ptr());
        result[0] = simfil::eval(env, expr, *model->root(0));
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
        threads.emplace_back(std::thread([&, i]() {
            size_t next;
            while ((next = idx++) < model->numRoots()) {
                try {
                    result[next] = simfil::eval(env, expr, *model->root(next));
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

static void display_error(std::string_view expression, const simfil::ParserError& e)
{
    static const auto indent = "  ";
    auto [begin, end] = e.range();

    std::string underline;
    if (end >= begin) {
        if (begin > 0)
            std::generate_n(std::back_inserter(underline), begin, []() { return ' '; });
        underline.push_back('^');
        if (end > begin)
            std::generate_n(std::back_inserter(underline), end - begin - 1, []() { return '~'; });
    }

    std::cout << "Error:\n"
        << indent << e.what() << ".\n\n"
        << indent << expression << "\n"
        << indent << underline << "\n";
}

int main(int argc, char *argv[])
{
#if WITH_READLINE
    rl_attempted_completion_function = command_completion;
#endif

    auto model = std::make_shared<simfil::ModelPool>();
    std::map<std::string, simfil::Value, simfil::CaseInsensitiveCompare> constants;

    auto load_json = [&model](const std::string_view& filename) {
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
                if (auto pos = arg.find("="); (pos != std::string::npos) && (pos > 0)) {
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
        auto cmd = input("> ");
        if (cmd.empty())
            continue;
        if (cmd[0] == '/') {
            set_option("any", options.auto_any, cmd);
            set_option("wildcard", options.auto_wildcard, cmd);
            set_option("verbose", options.verbose, cmd);
            set_option("mt", options.multi_threaded, cmd);
            continue;
        }

        simfil::Environment env(model->strings());
        env.constants = constants;

        simfil::ExprPtr expr;
        try {
            expr = simfil::compile(env, cmd, options.auto_any, options.auto_wildcard);
        } catch (const simfil::ParserError& e) {
            display_error(cmd, e);
        } catch (const std::exception& e) {
            std::cout << "Compile:\n  " << e.what() << "\n";
        }

        if (!expr)
            continue;

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
        for (const auto& v : flatres) {
            std::cout << "  " << v.toString() << "\n";
        }
    }

    return 0;
}
