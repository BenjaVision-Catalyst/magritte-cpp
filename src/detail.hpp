#pragma once
#include <magritte/magritte.hpp>
namespace magritte::detail {
bool portableLabelKey(const Value&);
std::string valueKey(const Value&, Limits, std::size_t&, std::size_t depth = 0);
}
