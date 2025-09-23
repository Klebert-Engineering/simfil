#pragma once

#include <tl/expected.hpp>

// Helper macro for bubbling-up tl::expected errors.
#define TRY_EXPECTED(res)                                                      \
  do {                                                                         \
    if (!(res).has_value()) [[unlikely]] {                                     \
      return tl::unexpected(std::move((res).error()));                         \
    }                                                                          \
  } while (false)

namespace simfil
{

template <class T, class E>
using expected = ::tl::expected<T, E>;

template <class E>
using unexpected = ::tl::unexpected<E>;

}
