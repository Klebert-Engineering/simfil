#pragma once

#include <algorithm>
#include <string_view>
#include <vector>

namespace simfil
{

inline auto levenshtein(std::string_view a, std::string_view b) -> int
{
    if (a.size() > b.size())
        return levenshtein(b, a);

    auto lo = a.size();
    auto hi = b.size();

    std::vector<int> dist;
    dist.resize(lo + 1);
    std::ranges::generate(dist.begin(), dist.end(), [i = 0]() mutable { return i++; });

    for (auto j = 1; j <= hi; ++j) {
        auto p = dist[0]++;

        for (auto i = 1; i <= lo; ++i) {
            auto s = dist[i];
            if (a[i - 1] == b[j - 1])
                dist[i] = p;
            else
                dist[i] = std::min<int>(std::min<int>(dist[i - 1], dist[i]), p) + 1;
            p = s;
        }
    }

    return dist[lo];
}

}
