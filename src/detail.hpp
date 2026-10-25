#pragma once
#include <magritte/magritte.hpp>
namespace magritte::detail {
void checkDescription(const Description&);
void checkPortableDescription(const Description&, Limits, std::size_t&);
bool evaluatePredicate(const Predicate&, const Value&, Limits, std::size_t&);
bool isNumber(const Value&);
int compareNumbers(const Value&, const Value&);
std::string floatBits(double);
double floatFromBits(const std::string&);
std::string floatText(double);
std::string displayString(const Value&);
bool portableLabelKey(const Value&);
std::string valueKey(const Value&, Limits, std::size_t&, std::size_t depth = 0);
}
