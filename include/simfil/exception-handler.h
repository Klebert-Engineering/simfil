#pragma once

#include <functional>
#include <exception>
#include <string>
#include <typeinfo>

namespace simfil
{

/**
 * Define the error handler callback type. The first argument
 * is the name of the exception class, the second argument is
 * exception.what(), or an empty string if what() does not exist.
 */
using CustomThrowHandlerType = std::function<void(const std::string&, const std::string&)>;

/**
 * Singleton class to store a custom exception handling callback.
 */
class ThrowHandler
{
public:
    // Retrieves the singleton instance of the ThrowHandler
    static ThrowHandler& instance() {
        static ThrowHandler instance;
        return instance;
    }

    // Prevent copying and assignment
    ThrowHandler(const ThrowHandler&) = delete;
    ThrowHandler& operator=(const ThrowHandler&) = delete;

    void set(CustomThrowHandlerType handler) {
        customThrowHandler_ = std::move(handler);
    }

    [[nodiscard]] CustomThrowHandlerType const& get() const {
        return customThrowHandler_;
    }

private:
    CustomThrowHandlerType customThrowHandler_;
    ThrowHandler() = default; // Private constructor for singleton
};


/**
 * Template function that accepts any exception type
 * and constructor arguments. Runs the custom exception
 * handler callback if it is set, and then calls C++ throw.
 */
template<typename ExceptionType, typename... Args>
[[noreturn]] void raise(Args&&... args)
{
    ExceptionType exceptionInstance(std::forward<Args>(args)...);

    if (auto const& excHandler = ThrowHandler::instance().get()) {
        std::string typeName = typeid(ExceptionType).name();
        std::string errorMessage;
        if constexpr (requires {exceptionInstance.what();})
            errorMessage = exceptionInstance.what();
        excHandler(typeName, errorMessage);
    }

    throw std::move(exceptionInstance);
}

}
