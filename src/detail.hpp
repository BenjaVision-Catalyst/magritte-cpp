#pragma once
#include <magritte/magritte.hpp>
namespace magritte::detail {
std::shared_ptr<Description> referenceTarget(const Description &);
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
Value::Fraction fractionOf(const Value &);
std::optional<Value::Fraction> floatFraction(double);
int compareFractions(const Value &, const Value &);
Value flattenedPriority(const Value &parent, const Value &child);
void checkLocalValue(const Value &, Limits = {});
std::string validationMessage(const Description &, const std::string &code, Limits = {});
std::string displayString(const Value&);
bool portableLabelKey(const Value&);
std::string valueKey(const Value&, Limits, std::size_t&, std::size_t depth = 0);
}
