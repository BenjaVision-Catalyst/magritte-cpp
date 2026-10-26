#pragma once
#include <magritte/magritte.hpp>
namespace magritte::detail {
bool knownClass(const std::string&);
bool shouldWrite(const Description&, const Value&, const Value&, Limits, std::size_t&);
std::string descriptionType(const Description&);
bool metadataSelector(const Description&, const std::string&, bool writing);
void checkDescription(const Description&);
void checkPortableDescription(const Description&, Limits, std::size_t&);
bool evaluatePredicate(const Predicate&, const Value&, Limits, std::size_t&);
bool isNumber(const Value&);
int compareNumbers(const Value&, const Value&);
std::string floatBits(double);
double floatFromBits(const std::string&);
std::string floatText(double);
void checkLocalValue(const Value &, Limits = {});
std::string displayString(const Value&);
bool portableLabelKey(const Value&);
std::string valueKey(const Value&, Limits, std::size_t&, std::size_t depth = 0);
}
